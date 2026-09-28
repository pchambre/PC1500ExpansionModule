/* ble_link.c -- see ble_link.h and BLE_PROTOCOL.md.
 *
 * core0 (BTstack's context, IRQ-driven): scanning, connecting, finding the
 * Link service, and receiving. A notification is one frame; it lands in
 * one of two one-frame slots -- the answer to the frame this side has in
 * flight (ACK/ERR), or a frame the peer sends -- and core1 takes it from
 * there. With one frame in flight per direction (BLE_PROTOCOL.md sec.4),
 * that's all the buffering there is.
 *
 * core1 (the commands): sends a frame through async_context_execute_sync()
 * and waits for its answer, polling the slots. core0 publishes a slot's
 * contents before its flag (__dmb()), core1 reads the flag first. The MCU
 * log is written from core1 only (flash writes pause core0 -- main.c), and
 * its messages use only characters the PC-1500 can show (no apostrophe). */
#include "ble_link.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "flash_layout.h"
#include "hardware/sync.h"
#include "mcu_log.h"
#include "pc_exp.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

static_assert(PICO_FLASH_BANK_TOTAL_SIZE == FLASH_BTSTACK_SIZE, "BTstack store size");

/* The SDK's BTstack flash store goes where flash_layout.h says, not at its
 * default (CMakeLists.txt points pico_flash_bank_get_storage_offset_func
 * here). */
uint32_t ble_flash_bank_offset(void) { return FLASH_BTSTACK_OFFSET; }

/* ---- BLE_PROTOCOL.md ---- */

enum {
    T_HELLO = 0x01,
    T_BYE = 0x02,
    T_TEXT = 0x10,
    T_FILE_PUT = 0x20,
    T_FILE_DATA = 0x21,
    T_FILE_END = 0x22,
    T_FILE_GET = 0x23,
    T_FILE_ABORT = 0x24,
    T_ACK = 0x7E,
    T_ERR = 0x7F,
};
enum { E_BAD_FRAME = 1, E_UNSUPPORTED = 2, E_ABORTED = 7 };

#define PROTOCOL_VERSION 1
#define KIND_PC1500 1
#define MY_NAME "PC-1500"
#define TARGET_SERVER 0
#define HEADER 4
#define FRAME_MAX 252 /* ATT MTU 255 (btstack_config.h's HCI_ACL_PAYLOAD_SIZE) - 3 */
#define MIN_MTU 64

#define ANSWER_TIMEOUT_MS 5000
#define POWER_TIMEOUT_MS 8000
#define CONNECT_TIMEOUT_MS 10000
#define NAME_SCAN_MS 3000

static const uint8_t kLinkService[16] = {0xC3, 0x1F, 0x00, 0x01, 0x92, 0xA3, 0x40, 0xAB,
                                         0xB6, 0x3D, 0x7C, 0xDB, 0x0A, 0x37, 0xAE, 0xD0};
static const uint8_t kLinkRx[16] = {0xC3, 0x1F, 0x00, 0x02, 0x92, 0xA3, 0x40, 0xAB,
                                    0xB6, 0x3D, 0x7C, 0xDB, 0x0A, 0x37, 0xAE, 0xD0};
static const uint8_t kLinkTx[16] = {0xC3, 0x1F, 0x00, 0x03, 0x92, 0xA3, 0x40, 0xAB,
                                    0xB6, 0x3D, 0x7C, 0xDB, 0x0A, 0x37, 0xAE, 0xD0};

/* Results of a command, beside the protocol's own ERR codes (1-7). */
enum { R_OK = 0, R_NO_LINK = -1, R_TIMEOUT = -2 };

/* ---- state ---- */

typedef enum { L_OFF, L_IDLE, L_SCANNING, L_CONNECTING, L_DISCOVERING, L_READY } link_state_t;

static volatile bool g_wanted;  /* core1 wants the radio (a command, or a connection) */
static bool g_powered;          /* core0: BTstack powered on since the radio came up */
static bool g_stack_ready;      /* core0: l2cap/gatt/sm set up since the radio came up */
static volatile bool g_working; /* HCI_STATE_WORKING */
static volatile link_state_t g_state = L_OFF;
static btstack_packet_callback_registration_t g_hci_cb;

#define MAX_PEERS 8
#define PEER_NAME_MAX 16
typedef struct {
    bd_addr_t addr;
    uint8_t addr_type;
    int8_t rssi;
    bool link; /* advertises the Link service */
    char name[PEER_NAME_MAX + 1];
} peer_t;
static peer_t g_peers[MAX_PEERS];
static volatile uint8_t g_npeers;
static uint8_t g_listed[MAX_PEERS]; /* the last SCAN listing: indices of Link peers */
static uint8_t g_nlisted;

static hci_con_handle_t g_con = HCI_CON_HANDLE_INVALID;
static enum { D_SERVICE, D_CHARACTERISTICS, D_NOTIFY } g_step;
static gatt_client_service_t g_service;
static gatt_client_characteristic_t g_rx, g_tx;
static bool g_have_service, g_have_rx, g_have_tx;
static gatt_client_notification_t g_notification;
static bool g_listening;
static uint16_t g_frame_max = 20;

/* The two receive slots (see the top of this file). */
static volatile bool g_answer_ready;
static uint8_t g_answer_type, g_answer_seq, g_answer_code;
static volatile bool g_frame_ready;
static uint8_t g_frame[FRAME_MAX];
static uint16_t g_frame_len;
static volatile uint32_t g_dropped;
/* Diagnostics, logged only when a request goes unanswered: notifications
 * seen since connecting, and the first bytes of the last one. */
static volatile uint32_t g_notified;
static uint8_t g_last_notify[4];
/* ...and for a connection that fails: its HCI status, the ATT status of a
 * failed discovery step, and how far discovery got. */
static volatile uint8_t g_connect_status, g_att_status;
/* Security events seen on the connection -- BTstack drops notifications
 * while re-encryption is being set up, or on a bonded link that isn't
 * encrypted, but still takes ATT responses: which is what the first
 * HELLO looked like on hardware. */
enum { SEC_PAIRING = 0x01, SEC_PAIRED = 0x02, SEC_PAIR_FAILED = 0x04, SEC_JUST_WORKS = 0x08,
       SEC_ENCRYPTED = 0x10, SEC_REENCRYPT = 0x20 };
static volatile uint8_t g_security;
static btstack_packet_callback_registration_t g_sm_cb;

/* core1: this side's frame numbers; the transfer in progress. */
static uint8_t g_tx_seq;
static enum { X_NONE, X_PUT, X_GET } g_xfer;
static bool g_xfer_failed;
static bool g_get_end;
static uint8_t g_get_buf[FRAME_MAX];
static uint16_t g_get_len, g_get_pos;

/* ---- core0: BTstack events ---- */

static void on_notify(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    uint16_t len = gatt_event_notification_get_value_length(packet);
    const uint8_t *v = gatt_event_notification_get_value(packet);
    g_notified++;
    memcpy(g_last_notify, v, len < 4 ? len : 4);
    if (len < HEADER || len > FRAME_MAX || len - HEADER != little_endian_read_16(v, 2)) {
        g_dropped++;
        return;
    }
    if (v[0] == T_ACK || v[0] == T_ERR) {
        g_answer_type = v[0];
        g_answer_seq = v[1];
        g_answer_code = len > HEADER ? v[HEADER] : 0;
        __dmb();
        g_answer_ready = true;
    } else if (!g_frame_ready) {
        memcpy(g_frame, v, len);
        g_frame_len = len;
        __dmb();
        g_frame_ready = true;
    } else {
        g_dropped++; /* the peer broke one-frame-in-flight */
    }
}

static void link_failed(void) {
    g_att_status |= 0x80; /* marks "discovery gave up" even with ATT success */
    if (g_con != HCI_CON_HANDLE_INVALID) gap_disconnect(g_con);
    else g_state = L_IDLE;
}

static void on_gatt(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET || g_state != L_DISCOVERING) return;
    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &g_service);
            g_have_service = true;
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t c;
            gatt_event_characteristic_query_result_get_characteristic(packet, &c);
            if (memcmp(c.uuid128, kLinkRx, 16) == 0) {
                g_rx = c;
                g_have_rx = true;
            } else if (memcmp(c.uuid128, kLinkTx, 16) == 0) {
                g_tx = c;
                g_have_tx = true;
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE:
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                g_att_status = gatt_event_query_complete_get_att_status(packet);
                link_failed();
            } else if (g_step == D_SERVICE) {
                if (!g_have_service) {
                    link_failed();
                    break;
                }
                g_step = D_CHARACTERISTICS;
                gatt_client_discover_characteristics_for_service(on_gatt, g_con, &g_service);
            } else if (g_step == D_CHARACTERISTICS) {
                if (!g_have_rx || !g_have_tx) {
                    link_failed();
                    break;
                }
                g_step = D_NOTIFY;
                gatt_client_listen_for_characteristic_value_updates(&g_notification, on_notify, g_con, &g_tx);
                g_listening = true;
                gatt_client_write_client_characteristic_configuration(
                    on_gatt, g_con, &g_tx, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            } else {
                uint16_t mtu = ATT_DEFAULT_MTU;
                gatt_client_get_mtu(g_con, &mtu);
                if (mtu < MIN_MTU) {
                    link_failed();
                    break;
                }
                g_frame_max = (uint16_t)(mtu - 3 > FRAME_MAX ? FRAME_MAX : mtu - 3);
                g_state = L_READY;
            }
            break;
        default:
            break;
    }
}

/* Remembers a Link peer (or any advertiser, until the table fills) from an
 * advertisement or scan response; the name and the service UUID may come
 * in either (BLE_PROTOCOL.md sec.3). */
static void note_advert(const uint8_t *packet) {
    bd_addr_t addr;
    peer_t *p = NULL;
    uint8_t len = gap_event_advertising_report_get_data_length(packet);
    const uint8_t *data = gap_event_advertising_report_get_data(packet);
    ad_context_t ad;
    gap_event_advertising_report_get_address(packet, addr);
    for (uint8_t i = 0; i < g_npeers; i++)
        if (bd_addr_cmp(g_peers[i].addr, addr) == 0) p = &g_peers[i];
    if (!p) {
        if (g_npeers == MAX_PEERS) return;
        p = &g_peers[g_npeers];
        memset(p, 0, sizeof *p);
        bd_addr_copy(p->addr, addr);
        p->addr_type = gap_event_advertising_report_get_address_type(packet);
        g_npeers++;
    }
    p->rssi = gap_event_advertising_report_get_rssi(packet);
    if (ad_data_contains_uuid128(len, data, kLinkService)) p->link = true;
    for (ad_iterator_init(&ad, len, data); ad_iterator_has_more(&ad); ad_iterator_next(&ad)) {
        uint8_t t = ad_iterator_get_data_type(&ad);
        if (t == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME || (t == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME && !p->name[0])) {
            uint8_t n = ad_iterator_get_data_len(&ad);
            if (n > PEER_NAME_MAX) n = PEER_NAME_MAX;
            memcpy(p->name, ad_iterator_get_data(&ad), n);
            p->name[n] = 0;
        }
    }
}

static void on_hci(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            g_working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
            g_state = g_working ? L_IDLE : L_OFF;
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            if (g_state == L_SCANNING) note_advert(packet);
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            if (g_state != L_CONNECTING) break;
            g_connect_status = gap_subevent_le_connection_complete_get_status(packet);
            if (g_connect_status != ERROR_CODE_SUCCESS) {
                g_state = L_IDLE;
                break;
            }
            g_con = gap_subevent_le_connection_complete_get_connection_handle(packet);
            g_have_service = g_have_rx = g_have_tx = false;
            g_step = D_SERVICE;
            g_state = L_DISCOVERING;
            gatt_client_discover_primary_services_by_uuid128(on_gatt, g_con, kLinkService);
            break;
        case SM_EVENT_PAIRING_STARTED:
            g_security |= SEC_PAIRING;
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            g_security |= sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS ? SEC_PAIRED : SEC_PAIR_FAILED;
            break;
        case SM_EVENT_JUST_WORKS_REQUEST:
            g_security |= SEC_JUST_WORKS;
            break;
        case SM_EVENT_REENCRYPTION_STARTED:
            g_security |= SEC_REENCRYPT;
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
            g_security |= SEC_ENCRYPTED;
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (hci_event_disconnection_complete_get_connection_handle(packet) != g_con) break;
            if (g_listening) gatt_client_stop_listening_for_characteristic_value_updates(&g_notification);
            g_listening = false;
            g_con = HCI_CON_HANDLE_INVALID;
            g_state = L_IDLE;
            break;
        default:
            break;
    }
}

/* ---- core0: power ---- */

bool ble_link_wanted(void) { return g_wanted; }

void ble_link_poll(bool radio_up) {
    if (!radio_up) { /* cyw43_arch_deinit() took BTstack down with it */
        g_powered = g_stack_ready = false;
        g_working = false;
        g_state = L_OFF;
        g_con = HCI_CON_HANDLE_INVALID;
        g_listening = false;
        return;
    }
    bool want = g_wanted;
    if (want == g_powered) return;
    async_context_t *ctx = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(ctx);
    if (want) {
        if (!g_stack_ready) {
            l2cap_init();
            gatt_client_init();
            sm_init();
            sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
            /* A GATT server of our own, even with nothing but the standard
             * services in it (2026-09-27): without one BTstack drops the
             * peer's GATT requests unanswered, and Windows then held every
             * notification it sent us (Unreachable, 0 bytes, once the link
             * dropped) -- so no ACK ever arrived. A peer may query any
             * device it connects to. */
            att_db_util_init();
            att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
            att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME, ATT_PROPERTY_READ,
                                                  ATT_SECURITY_NONE, ATT_SECURITY_NONE, (uint8_t *)MY_NAME,
                                                  (uint16_t)strlen(MY_NAME));
            att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE);
            att_server_init(att_db_util_get_address(), NULL, NULL);
            g_hci_cb.callback = &on_hci;
            hci_add_event_handler(&g_hci_cb); /* a no-op if already added */
            g_sm_cb.callback = &on_hci;       /* security events come from SM */
            sm_add_event_handler(&g_sm_cb);
            g_stack_ready = true;
        }
        hci_power_control(HCI_POWER_ON);
    } else {
        hci_power_control(HCI_POWER_OFF);
        g_working = false;
        g_state = L_OFF;
    }
    async_context_release_lock(ctx);
    g_powered = want;
}

/* ---- core1: plumbing ---- */

static uint32_t on_core0(uint32_t (*fn)(void *), void *param) {
    return async_context_execute_sync(cyw43_arch_async_context(), fn, param);
}

/* The radio up and BTstack working, or false (logged) after a while. */
static bool power_up(void) {
    absolute_time_t until = make_timeout_time_ms(POWER_TIMEOUT_MS);
    g_wanted = true;
    while (!g_working) {
        if (time_reached(until)) {
            mcu_log_warn("BLE radio failed");
            return false;
        }
        sleep_ms(5);
    }
    return true;
}

/* Lets the radio go (and DORMANT sleep happen) when nothing needs it. */
static void release_if_idle(void) {
    if (g_state != L_READY && g_xfer == X_NONE) g_wanted = false;
}

typedef struct {
    const uint8_t *data;
    uint16_t len;
} write_t;

static uint32_t write_frame(void *param) {
    const write_t *w = param;
    if (g_state != L_READY) return 0xFF;
    return gatt_client_write_value_of_characteristic_without_response(g_con, g_rx.value_handle, w->len,
                                                                     (uint8_t *)w->data);
}

/* One frame out; retried while BTstack's buffers are full. */
static bool send_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len) {
    uint8_t frame[FRAME_MAX];
    write_t w = {frame, (uint16_t)(HEADER + len)};
    absolute_time_t until = make_timeout_time_ms(ANSWER_TIMEOUT_MS);
    if (w.len > g_frame_max) return false;
    frame[0] = type;
    frame[1] = seq;
    little_endian_store_16(frame, 2, len);
    if (len) memcpy(frame + HEADER, payload, len);
    for (;;) {
        uint32_t rc = on_core0(write_frame, &w);
        if (rc == ERROR_CODE_SUCCESS) return true;
        if (rc == 0xFF || time_reached(until)) return false;
        sleep_ms(2);
    }
}

static uint32_t do_disconnect(void *param) {
    (void)param;
    if (g_state == L_CONNECTING) {
        gap_connect_cancel();
        g_state = L_IDLE;
    } else if (g_con != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(g_con);
    }
    return 0;
}

static void drop_link(const char *why) {
    if (why) mcu_log_warn(why);
    on_core0(do_disconnect, NULL);
    for (int i = 0; i < 400 && g_state != L_IDLE && g_state != L_OFF; i++) sleep_ms(5);
}

/* "BLE rx3 7E000000 s00": what had arrived when an answer didn't -- the
 * count, the start of the last one, and the security events (SEC_*). */
static void log_no_answer(void) {
    char msg[MCU_LOG_MSG_MAX + 1];
    snprintf(msg, sizeof msg, "BLE rx%lu %02X%02X%02X%02X s%02X", (unsigned long)g_notified, g_last_notify[0],
             g_last_notify[1], g_last_notify[2], g_last_notify[3], g_security);
    mcu_log_warn(msg);
}

/* A frame out and its answer back: R_OK for ACK, the ERR code, R_NO_LINK,
 * or R_TIMEOUT (the link is then dropped -- sec.4). */
static int request(uint8_t type, const uint8_t *payload, uint16_t len) {
    uint8_t seq = g_tx_seq++;
    absolute_time_t until = make_timeout_time_ms(ANSWER_TIMEOUT_MS);
    g_answer_ready = false;
    if (g_state != L_READY || !send_frame(type, seq, payload, len)) return R_NO_LINK;
    while (!time_reached(until)) {
        if (g_answer_ready) {
            __dmb();
            bool mine = g_answer_seq == seq;
            uint8_t answer = g_answer_type, code = g_answer_code;
            g_answer_ready = false;
            if (mine) return answer == T_ACK ? R_OK : (code ? code : E_BAD_FRAME);
        }
        if (g_state != L_READY) return R_NO_LINK;
        sleep_us(500);
    }
    log_no_answer();
    drop_link("BLE peer no answer");
    return R_TIMEOUT;
}

/* The next frame from the peer, into *type/*seq/*payload/*len, or false
 * (dropping the link) after ANSWER_TIMEOUT_MS. */
static uint8_t g_in[FRAME_MAX];

static bool receive(uint8_t *type, uint8_t *seq, const uint8_t **payload, uint16_t *len) {
    absolute_time_t until = make_timeout_time_ms(ANSWER_TIMEOUT_MS);
    while (!g_frame_ready) {
        if (g_state != L_READY) return false;
        if (time_reached(until)) {
            drop_link("BLE peer went quiet");
            return false;
        }
        sleep_us(500);
    }
    __dmb();
    memcpy(g_in, g_frame, g_frame_len);
    *len = (uint16_t)(g_frame_len - HEADER);
    g_frame_ready = false;
    *type = g_in[0];
    *seq = g_in[1];
    *payload = g_in + HEADER;
    return true;
}

static void answer(uint8_t type, uint8_t seq, uint8_t code) { send_frame(type, seq, &code, type == T_ERR ? 1 : 0); }

/* [len hi][len lo][chars] at `slot` -> a str8 at `out`; returns its size. */
static uint16_t name_str8(const uint8_t *slot, uint8_t *out) {
    uint16_t n = (uint16_t)((slot[0] << 8) | slot[1]);
    if (n > EXP_PATH_ARG_LEN) n = EXP_PATH_ARG_LEN;
    out[0] = (uint8_t)n;
    memcpy(out + 1, slot + 2, n);
    return (uint16_t)(1 + n);
}

/* ---- core1: scanning ---- */

typedef struct {
    bool active;
} scan_t;

static uint32_t do_scan(void *param) {
    const scan_t *s = param;
    if (s->active) {
        g_npeers = 0;
        gap_set_scan_parameters(1 /* active: ask for scan responses */, 0x0030, 0x0030);
        gap_start_scan();
        g_state = L_SCANNING;
    } else {
        gap_stop_scan();
        if (g_state == L_SCANNING) g_state = L_IDLE;
    }
    return 0;
}

/* Scans for up to `ms`, or until a Link peer named `name` (any case) turns
 * up; fills g_listed with the Link peers found. Returns the index into
 * g_listed of the named one, or -1. */
static int scan(uint32_t ms, const char *name) {
    scan_t on = {true}, off = {false};
    absolute_time_t until = make_timeout_time_ms(ms);
    int found = -1;
    on_core0(do_scan, &on);
    while (!time_reached(until) && found < 0) {
        sleep_ms(20);
        for (uint8_t i = 0; name && i < g_npeers && found < 0; i++)
            if (g_peers[i].link && strcasecmp(g_peers[i].name, name) == 0) found = i;
    }
    on_core0(do_scan, &off);
    g_nlisted = 0;
    for (uint8_t i = 0; i < g_npeers; i++) {
        if (!g_peers[i].link) continue;
        if ((int)i == found) found = g_nlisted;
        g_listed[g_nlisted++] = i;
    }
    return found;
}

/* The last scan as a LIST_SD_DIR listing (name, then the signal strength
 * where the size would go), for BROWSE. */
static void scan_listing(uint8_t *w) {
    static const char kHex[] = "0123456789ABCDEF";
    uint8_t *r;
    for (uint8_t i = 0; i < g_nlisted; i++) {
        const peer_t *p = &g_peers[g_listed[i]];
        char text[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + 1];
        int8_t rssi = p->rssi;
        uint8_t n = 0;
        r = w + 2 + (uint16_t)i * EXP_DIR_RECORD_SIZE;
        memset(r, 0, EXP_DIR_RECORD_SIZE);
        memset(text, ' ', sizeof text);
        if (p->name[0]) {
            memcpy(text, p->name, strlen(p->name));
        } else { /* no name advertised: the address */
            for (int b = 0; b < 6; b++) {
                text[2 * b] = kHex[p->addr[b] >> 4];
                text[2 * b + 1] = kHex[p->addr[b] & 0x0F];
            }
        }
        text[EXP_DIR_NAME_LEN + n++] = '-';
        rssi = (int8_t)(rssi < 0 ? -rssi : rssi);
        if (rssi >= 100) text[EXP_DIR_NAME_LEN + n++] = (char)('0' + rssi / 100);
        if (rssi >= 10) text[EXP_DIR_NAME_LEN + n++] = (char)('0' + rssi / 10 % 10);
        text[EXP_DIR_NAME_LEN + n++] = (char)('0' + rssi % 10);
        memcpy(text + EXP_DIR_NAME_LEN + n, "DBM", 3);
        memcpy(r, text, EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN);
    }
    w[0] = 0;
    w[1] = g_nlisted;
    r = w + 2 + (uint16_t)g_nlisted * EXP_DIR_RECORD_SIZE;
    memset(r, ' ', EXP_DIR_SUMMARY_LEN);
    r[0] = (uint8_t)('0' + g_nlisted);
    memcpy(r + 2, "FOUND", 5);
}

/* ---- core1: connecting ---- */

static uint32_t do_connect(void *param) {
    const peer_t *p = param;
    gap_stop_scan();
    g_con = HCI_CON_HANDLE_INVALID;
    g_state = L_CONNECTING;
    return gap_connect(p->addr, (bd_addr_type_t)p->addr_type);
}

/* Connects to `p` and exchanges HELLOs (sec.5); the peer's name goes to
 * the window as [len][chars]. */
static uint8_t connect(const peer_t *p, uint8_t *w) {
    absolute_time_t until = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    uint8_t hello[3 + sizeof MY_NAME], type, seq;
    const uint8_t *payload;
    uint16_t len;
    g_answer_ready = g_frame_ready = false;
    g_notified = 0;
    memset(g_last_notify, 0, sizeof g_last_notify);
    g_connect_status = 0xFF; /* no connection event yet */
    g_security = 0;
    g_att_status = 0;
    g_tx_seq = 0;
    g_xfer = X_NONE;
    if (on_core0(do_connect, (void *)p) != ERROR_CODE_SUCCESS) {
        mcu_log_warn("BLE connect refused");
        return EXP_STATUS_ERROR;
    }
    while (g_state == L_CONNECTING || g_state == L_DISCOVERING) {
        if (time_reached(until)) break;
        sleep_ms(10);
    }
    if (g_state != L_READY) {
        /* "BLE conn s4 d1 e00 a00": state and discovery step when it
         * stopped (see link_state_t, g_step), HCI and ATT status. */
        char msg[MCU_LOG_MSG_MAX + 1];
        snprintf(msg, sizeof msg, "BLE conn s%u d%u e%02X a%02X", (unsigned)g_state, (unsigned)g_step,
                 g_connect_status, g_att_status);
        mcu_log_warn(msg);
        drop_link(NULL);
        return EXP_STATUS_ERROR;
    }
    hello[0] = PROTOCOL_VERSION;
    hello[1] = KIND_PC1500;
    hello[2] = (uint8_t)strlen(MY_NAME);
    memcpy(hello + 3, MY_NAME, strlen(MY_NAME));
    if (request(T_HELLO, hello, (uint16_t)(3 + strlen(MY_NAME))) != R_OK || !receive(&type, &seq, &payload, &len)) {
        drop_link("BLE HELLO failed");
        return EXP_STATUS_ERROR;
    }
    if (type != T_HELLO || len < 3 || payload[0] != PROTOCOL_VERSION || 3 + payload[2] > len) {
        answer(T_ERR, seq, E_UNSUPPORTED);
        drop_link("BLE peer incompatible");
        return EXP_STATUS_ERROR;
    }
    answer(T_ACK, seq, 0);
    w[0] = payload[2];
    memcpy(w + 1, payload + 3, payload[2]);
    return EXP_STATUS_SUCCESS;
}

static void disconnect(void) {
    if (g_state == L_READY) request(T_BYE, NULL, 0);
    drop_link(NULL);
    g_xfer = X_NONE;
}

/* ---- core1: text and files ---- */

static uint8_t send_text(const uint8_t *w) {
    uint16_t total = (uint16_t)((w[0] << 8) | w[1]);
    const uint8_t *text = w + 2;
    uint8_t payload[FRAME_MAX];
    while (total) {
        uint16_t n = (uint16_t)(g_frame_max - HEADER - 1);
        if (n > total) n = total;
        payload[0] = 0; /* channel 0: the console */
        memcpy(payload + 1, text, n);
        if (request(T_TEXT, payload, (uint16_t)(n + 1)) != R_OK) {
            mcu_log_warn("BLE text failed");
            return EXP_STATUS_ERROR;
        }
        text += n;
        total = (uint16_t)(total - n);
    }
    return EXP_STATUS_SUCCESS;
}

/* A failed request's code for the window: the peer's ERR code, or 0. */
static uint8_t failure(uint8_t *w, int r) {
    w[0] = r > 0 ? (uint8_t)r : 0;
    return EXP_STATUS_ERROR;
}

static uint8_t file_put(uint8_t *w) {
    const uint8_t *args = w + EXP_BLE_FILE_ARGS;
    uint8_t payload[7 + 1 + EXP_PATH_ARG_LEN];
    uint32_t size = ((uint32_t)args[2] << 24) | ((uint32_t)args[3] << 16) | ((uint32_t)args[4] << 8) | args[5];
    int r;
    payload[0] = TARGET_SERVER;
    payload[1] = args[0]; /* kind */
    payload[2] = args[1]; /* flags */
    little_endian_store_32(payload, 3, size);
    r = request(T_FILE_PUT, payload, (uint16_t)(7 + name_str8(w, payload + 7)));
    if (r != R_OK) return failure(w, r);
    g_xfer = X_PUT;
    g_xfer_failed = false;
    return EXP_STATUS_SUCCESS;
}

static uint8_t file_get(uint8_t *w) {
    uint8_t payload[2 + EXP_PATH_ARG_LEN], type, seq;
    const uint8_t *in;
    uint16_t len;
    int r;
    payload[0] = TARGET_SERVER;
    r = request(T_FILE_GET, payload, (uint16_t)(1 + name_str8(w, payload + 1)));
    if (r != R_OK) return failure(w, r);
    if (!receive(&type, &seq, &in, &len)) return failure(w, R_NO_LINK);
    if (type != T_FILE_PUT || len < 7) {
        answer(T_ERR, seq, E_BAD_FRAME);
        return failure(w, E_BAD_FRAME);
    }
    answer(T_ACK, seq, 0);
    w[0] = in[1]; /* kind */
    g_xfer = X_GET;
    g_xfer_failed = g_get_end = false;
    g_get_len = g_get_pos = 0;
    return EXP_STATUS_SUCCESS;
}

/* WRITE_TO_SD_FILE while saving: FILE_DATA frames. */
static uint8_t xfer_write(const uint8_t *w, uint16_t len) {
    if (g_xfer != X_PUT || g_xfer_failed || len == 0) return EXP_STATUS_ERROR;
    while (len) {
        uint16_t n = (uint16_t)(g_frame_max - HEADER);
        if (n > len) n = len;
        if (request(T_FILE_DATA, w, n) != R_OK) {
            g_xfer_failed = true;
            mcu_log_warn("BLE save failed");
            return EXP_STATUS_ERROR;
        }
        w += n;
        len = (uint16_t)(len - n);
    }
    return EXP_STATUS_SUCCESS;
}

/* READ_FROM_SD_FILE while loading: up to `want` bytes from the FILE_DATA
 * frames; 0 at FILE_END. */
static int xfer_read(uint8_t *w, uint16_t want) {
    uint16_t n = 0;
    if (g_xfer != X_GET || g_xfer_failed) return -1;
    while (n < want) {
        uint8_t type, seq;
        const uint8_t *in;
        uint16_t len;
        if (g_get_pos < g_get_len) {
            uint16_t k = (uint16_t)(g_get_len - g_get_pos);
            if (k > want - n) k = (uint16_t)(want - n);
            memcpy(w + n, g_get_buf + g_get_pos, k);
            g_get_pos = (uint16_t)(g_get_pos + k);
            n = (uint16_t)(n + k);
            continue;
        }
        if (g_get_end) break;
        if (!receive(&type, &seq, &in, &len)) {
            g_xfer_failed = true;
            mcu_log_warn("BLE load failed");
            return -1;
        }
        if (type == T_FILE_DATA) {
            memcpy(g_get_buf, in, len);
            g_get_len = len;
            g_get_pos = 0;
            answer(T_ACK, seq, 0);
        } else if (type == T_FILE_END) {
            g_get_end = true;
            answer(T_ACK, seq, 0);
        } else {
            answer(type == T_FILE_ABORT ? T_ACK : T_ERR, seq, E_BAD_FRAME);
            g_xfer_failed = true;
            mcu_log_warn("BLE load aborted");
            return -1;
        }
    }
    return n;
}

/* CLOSE_SD_FILE: SUCCESS only if the whole file moved. */
static uint8_t xfer_close(void) {
    bool ok;
    if (g_xfer == X_PUT) {
        ok = !g_xfer_failed && request(T_FILE_END, NULL, 0) == R_OK;
        if (!ok && g_state == L_READY) request(T_FILE_ABORT, NULL, 0);
    } else {
        ok = !g_xfer_failed && g_get_end && g_get_pos == g_get_len;
        if (!ok && g_frame_ready) { /* stopped early: refuse what's waiting */
            uint8_t type, seq;
            const uint8_t *in;
            uint16_t len;
            if (receive(&type, &seq, &in, &len)) answer(T_ERR, seq, E_ABORTED);
        }
    }
    g_xfer = X_NONE;
    return ok ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
}

/* ---- core1: the commands ---- */

bool ble_link_transfer_open(void) { return g_xfer != X_NONE; }

uint8_t ble_link_command(uint8_t command, uint8_t *w) {
    uint8_t status = EXP_STATUS_ERROR;
    uint8_t *length_port = w + EXP_LENGTH_PORT_PAGE * 256 + EXP_LENGTH_PORT_ADDRESS;
    switch (command) {
        case EXP_COMMAND_WRITE_TO_SD_FILE:
            return xfer_write(w, (uint16_t)((length_port[0] << 8) | length_port[1]));
        case EXP_COMMAND_READ_FROM_SD_FILE: {
            uint16_t want = (uint16_t)((length_port[0] << 8) | length_port[1]);
            int n = want > EXP_MAX_TRANSFER_LEN ? -1 : xfer_read(w, want);
            if (n < 0) return EXP_STATUS_ERROR;
            length_port[0] = (uint8_t)(n >> 8);
            length_port[1] = (uint8_t)n;
            return EXP_STATUS_SUCCESS;
        }
        case EXP_COMMAND_CLOSE_SD_FILE:
            status = xfer_close();
            release_if_idle();
            return status;
        case EXP_COMMAND_BLE_DISCONNECT:
            if (g_working) disconnect();
            release_if_idle();
            return EXP_STATUS_SUCCESS;
        default:
            break;
    }
    if (!power_up()) {
        release_if_idle();
        w[0] = 0;
        return EXP_STATUS_ERROR;
    }
    switch (command) {
        case EXP_COMMAND_BLE_SCAN:
            if (g_state == L_READY) disconnect();
            scan((uint32_t)(w[0] ? w[0] : 3) * 1000u, NULL);
            scan_listing(w);
            status = EXP_STATUS_SUCCESS;
            break;
        case EXP_COMMAND_BLE_CONNECT:
            if (g_state == L_READY) disconnect();
            status = w[0] < g_nlisted ? connect(&g_peers[g_listed[w[0]]], w) : EXP_STATUS_ERROR;
            break;
        case EXP_COMMAND_BLE_CONNECT_NAME: {
            char name[EXP_PATH_ARG_LEN + 1];
            uint16_t n = (uint16_t)((w[0] << 8) | w[1]);
            int i;
            if (n > EXP_PATH_ARG_LEN) n = EXP_PATH_ARG_LEN;
            memcpy(name, w + 2, n);
            name[n] = 0;
            if (g_state == L_READY) disconnect();
            i = scan(NAME_SCAN_MS, name);
            if (i < 0) mcu_log_warn("BLE name not found");
            status = i >= 0 ? connect(&g_peers[g_listed[i]], w) : EXP_STATUS_ERROR;
            break;
        }
        case EXP_COMMAND_BLE_TEXT:
            status = g_state == L_READY ? send_text(w) : EXP_STATUS_ERROR;
            break;
        case EXP_COMMAND_BLE_FILE_PUT:
            status = g_state == L_READY ? file_put(w) : failure(w, R_NO_LINK);
            break;
        case EXP_COMMAND_BLE_FILE_GET:
            status = g_state == L_READY ? file_get(w) : failure(w, R_NO_LINK);
            break;
        default:
            status = EXP_STATUS_NOT_IMPLEMENTED;
            break;
    }
    if (status != EXP_STATUS_SUCCESS && (command == EXP_COMMAND_BLE_TEXT || command == EXP_COMMAND_BLE_FILE_PUT ||
                                         command == EXP_COMMAND_BLE_FILE_GET) && g_state != L_READY)
        mcu_log_warn("BLE: not connected");
    release_if_idle();
    return status;
}
