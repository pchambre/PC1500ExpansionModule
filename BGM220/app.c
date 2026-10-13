/* app.c -- the PC-1500 internal card's BGM220S firmware (2026-10-12).
 *
 * The module does the radio work and passes Link frames through unchanged;
 * the Link protocol itself stays on the RP2354B (../RP2350/MODULE_PROTOCOL.md
 * sec.1). Both Link roles (BLE_PROTOCOL.md sec.1):
 *   advertiser -- the Link service under the name; a peer connects and
 *                 subscribes to TX; frames come as RX writes, go as TX
 *                 notifications;
 *   connector  -- SCAN_START finds Link advertisers (by the service UUID,
 *                 never the name), CONNECT connects, finds the service,
 *                 subscribes to the peer's TX; frames go as RX writes
 *                 (without response), come as TX notifications.
 * One Link connection at a time, either role. The keyboard comes next.
 *
 * Everything runs in the main loop: hostlink_poll() for the UART, the
 * Bluetooth stack's events through sl_bt_on_event(), and timer expiries as
 * flags set by sleeptimer callbacks. */
#include <stdbool.h>
#include <string.h>

#include "app_assert.h"
#include "em_gpio.h"
#include "events.h"
#include "gatt_db.h"
#include "gpiointerrupt.h"
#include "hostlink.h"
#include "module_config.h"
#include "sl_bt_api.h"
#include "sl_iostream_init_usart_instances.h"
#include "sl_iostream_uart.h"
#include "sl_main_init.h"
#include "sl_sleeptimer.h"
#include "app.h"
#include "kbd.h"

/* The Link service and characteristics (BLE_PROTOCOL.md sec.2), as the
 * stack wants 128-bit UUIDs: little-endian. */
static const uint8_t kLinkUuid[16] = {0xd0, 0xae, 0x37, 0x0a, 0xdb, 0x7c, 0x3d, 0xb6,
                                      0xab, 0x40, 0xa3, 0x92, 0x01, 0x00, 0x1f, 0xc3};
static const uint8_t kRxUuid[16] = {0xd0, 0xae, 0x37, 0x0a, 0xdb, 0x7c, 0x3d, 0xb6,
                                    0xab, 0x40, 0xa3, 0x92, 0x02, 0x00, 0x1f, 0xc3};
static const uint8_t kTxUuid[16] = {0xd0, 0xae, 0x37, 0x0a, 0xdb, 0x7c, 0x3d, 0xb6,
                                    0xab, 0x40, 0xa3, 0x92, 0x03, 0x00, 0x1f, 0xc3};

/* LINK_FAILED reasons of our own (MODULE_PROTOCOL.md sec.7); anything
 * else is the stack's connection-close reason. */
#define FAIL_TIMEOUT 0xF001    /* CONNECT's timeout ran out */
#define FAIL_NO_SERVICE 0xF002 /* no Link service, or not both characteristics */
#define FAIL_SMALL_MTU 0xF003  /* MTU under 64 (BLE_PROTOCOL.md sec.2) */
#define FAIL_CANCELLED 0xF004  /* CONNECT_CANCEL */
#define LINK_MIN_MTU 64

#define NO_CONN 0xFF

static uint8_t adv_set = 0xFF;
static bool adv_wanted;
static char name[NAME_MAX + 1] = "PC-1500";
static volatile bool wake_changed;

/* ---- the Link connection ---- */

static enum {
    L_IDLE,
    L_CONNECTING,  /* connector: connection_open sent */
    L_DISCOVERING, /* connector: finding the Link service */
    L_CHARS,       /* connector: finding its RX and TX */
    L_SUBSCRIBING, /* connector: enabling TX notifications */
    L_WAIT_MTU,    /* connector: subscribed, MTU not yet exchanged */
    L_PERIPHERAL,  /* advertiser: connected, waiting for the subscription */
    L_UP,          /* LINK_UP sent */
} link_state = L_IDLE;
static uint8_t link_conn = NO_CONN;
static bool link_central; /* we connected */
static uint16_t link_mtu = 23;
static uint32_t peer_service;
static uint16_t peer_rx, peer_tx; /* the peer's characteristics (connector) */
static uint16_t link_fail;        /* why the connector side is closing, for LINK_FAILED */

/* ---- timers (sleeptimer callbacks only set flags) ---- */

static sl_sleeptimer_timer_handle_t scan_timer, connect_timer;
static volatile bool scan_expired, connect_expired;

static void scan_timer_cb(sl_sleeptimer_timer_handle_t *h, void *d) {
    (void)h, (void)d;
    scan_expired = true;
}

static void connect_timer_cb(sl_sleeptimer_timer_handle_t *h, void *d) {
    (void)h, (void)d;
    connect_expired = true;
}

/* ---- MODULE_WAKE: a session keeps the UART receiving (sec.4) ---- */

static void wake_isr(uint8_t pin) {
    (void)pin;
    wake_changed = true;
}

static void session_update(void) {
    bool on = GPIO_PinInGet(MODULE_WAKE_PORT, MODULE_WAKE_PIN);
    /* Outside a session the module may sleep in EM2, where the UART can't
     * receive; the MCU only sends inside one. */
    sl_iostream_uart_set_rx_energy_mode_restriction(sl_iostream_uart_host_handle, on);
}

/* ---- advertising ---- */

static void adv_apply(void) {
    sl_status_t sc;
    if (adv_set == 0xFF) {
        return; /* not booted yet: system_boot does it */
    }
    sl_bt_advertiser_stop(adv_set);
    if (!adv_wanted || link_conn != NO_CONN) {
        return;
    }
    uint8_t ad[3 + 18], sr[2 + NAME_MAX];
    size_t n = strlen(name);
    ad[0] = 2, ad[1] = 0x01, ad[2] = 0x06;                   /* flags: LE general, no BR/EDR */
    ad[3] = 17, ad[4] = 0x07, memcpy(ad + 5, kLinkUuid, 16); /* complete 128-bit UUIDs */
    sr[0] = (uint8_t)(n + 1), sr[1] = 0x09, memcpy(sr + 2, name, n); /* complete local name */
    sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_advertising_data_packet, sizeof ad, ad);
    app_assert_status(sc);
    sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_scan_response_packet, n + 2, sr);
    app_assert_status(sc);
    sc = sl_bt_advertiser_set_timing(adv_set, 160, 160, 0, 0); /* 100 ms */
    app_assert_status(sc);
    sc = sl_bt_legacy_advertiser_start(adv_set, sl_bt_legacy_advertiser_connectable);
    app_assert_status(sc);
}

/* ---- scanning: Link advertisers by UUID (BLE_PROTOCOL.md sec.3) ---- */

#define SCAN_SLOTS 16
static struct {
    bd_addr addr;
    uint8_t type;
    bool link;     /* lists the Link service UUID */
    bool reported; /* SCAN_REPORT sent */
    int8_t rssi;
    uint8_t name_len;
    char name[29];
} seen[SCAN_SLOTS];
static unsigned seen_count;
static bool scanning;
static uint8_t scan_what;

/* AD structures: does `data` list the Link UUID, and its local name. */
static void parse_ad(const uint8_t *d, size_t n, bool *link, char *nm, uint8_t *nm_len) {
    size_t i = 0;
    while (i + 1 < n) {
        uint8_t len = d[i], type = d[i + 1];
        if (len == 0 || i + 1 + len > n) {
            break;
        }
        const uint8_t *v = d + i + 2;
        size_t vlen = len - 1u;
        if (type == 0x06 || type == 0x07) { /* incomplete/complete 128-bit UUIDs */
            for (size_t k = 0; k + 16 <= vlen; k += 16) {
                if (memcmp(v + k, kLinkUuid, 16) == 0) {
                    *link = true;
                }
            }
        } else if ((type == 0x08 || type == 0x09) && vlen) { /* short/complete name */
            if (vlen > 28) {
                vlen = 28;
            }
            memcpy(nm, v, vlen);
            *nm_len = (uint8_t)vlen;
        }
        i += 1u + len;
    }
}

static void scan_report(unsigned k) {
    uint8_t b[1 + 1 + 6 + 1 + 1 + 28];
    size_t i = 0;
    b[i++] = scan_what;
    b[i++] = seen[k].type;
    for (int j = 0; j < 6; j++) {
        b[i++] = seen[k].addr.addr[5 - j];
    }
    b[i++] = (uint8_t)seen[k].rssi;
    b[i++] = seen[k].name_len;
    memcpy(b + i, seen[k].name, seen[k].name_len);
    i += seen[k].name_len;
    seen[k].reported = true;
    events_push(EV_SCAN_REPORT, b, i);
}

static void on_adv_report(sl_bt_evt_scanner_legacy_advertisement_report_t *e) {
    unsigned k;
    for (k = 0; k < seen_count; k++) {
        if (memcmp(&seen[k].addr, &e->address, sizeof(bd_addr)) == 0 && seen[k].type == e->address_type) {
            break;
        }
    }
    if (k == seen_count) {
        if (seen_count == SCAN_SLOTS) {
            return; /* full: plenty for a room's worth of peers */
        }
        memset(&seen[k], 0, sizeof seen[k]);
        seen[k].addr = e->address;
        seen[k].type = e->address_type;
        seen_count++;
    }
    seen[k].rssi = e->rssi;
    parse_ad(e->data.data, e->data.len, &seen[k].link, seen[k].name, &seen[k].name_len);
    /* Report once it's a Link peer and its name is in (the name usually
     * comes in the scan response); SCAN_DONE reports the nameless rest. */
    if (seen[k].link && !seen[k].reported && seen[k].name_len) {
        scan_report(k);
    }
}

/* ---- the scanner, shared with the keyboard (app.h) ---- */

uint8_t scanner_owner = SCAN_NONE;

bool scanner_take(uint8_t owner, bool active, uint16_t interval, uint16_t window) {
    if (owner == SCAN_KBD && scanner_owner == SCAN_LINK) {
        return false;
    }
    if (scanner_owner != SCAN_NONE) {
        sl_bt_scanner_stop();
    }
    sl_status_t sc = sl_bt_scanner_set_parameters(
        active ? sl_bt_scanner_scan_mode_active : sl_bt_scanner_scan_mode_passive, interval, window);
    if (sc == SL_STATUS_OK) {
        sc = sl_bt_scanner_start(sl_bt_scanner_scan_phy_1m, sl_bt_scanner_discover_observation);
    }
    if (sc != SL_STATUS_OK) {
        events_log_error("scanner %u: 0x%04X", owner, (unsigned)sc);
        scanner_owner = SCAN_NONE;
        return false;
    }
    scanner_owner = owner;
    return true;
}

void scanner_release(uint8_t owner) {
    if (scanner_owner == owner) {
        sl_bt_scanner_stop();
        scanner_owner = SCAN_NONE; /* the keyboard's background scan takes it back (kbd_tick()) */
    }
}

static void scan_finish(void) {
    if (!scanning) {
        return;
    }
    scanner_release(SCAN_LINK);
    sl_sleeptimer_stop_timer(&scan_timer);
    scanning = false;
    for (unsigned k = 0; k < seen_count; k++) {
        if (seen[k].link && !seen[k].reported) {
            scan_report(k);
        }
    }
    events_push(EV_SCAN_DONE, NULL, 0);
}

static uint8_t do_scan_start(const uint8_t *p, size_t len) {
    if (len != 3 || p[0] != 1) { /* 2 = keyboards: KBD_PAIR does that search */
        return ST_BAD_ARGS;
    }
    if (scanning) {
        scan_finish();
    }
    uint16_t ms = (uint16_t)(p[1] | p[2] << 8);
    seen_count = 0;
    scan_what = p[0];
    /* Active (BLE_PROTOCOL.md sec.3: the UUID is often only in the scan
     * response), 10 ms windows back to back. Takes the scanner from the
     * keyboard's background scan; a keyboard pairing search keeps it. */
    if (scanner_owner == SCAN_KBD && kbd_pairing()) {
        return ST_STATE;
    }
    if (!scanner_take(SCAN_LINK, true, 16, 16)) {
        return ST_FAILED;
    }
    scanning = true;
    sl_sleeptimer_start_timer_ms(&scan_timer, ms ? ms : 1, scan_timer_cb, NULL, 0, 0);
    return ST_OK;
}

/* ---- connecting (the connector side) ---- */

static void link_reset(void) {
    link_state = L_IDLE;
    link_conn = NO_CONN;
    link_central = false;
    link_mtu = 23;
    peer_service = 0;
    peer_rx = peer_tx = 0;
    link_fail = 0;
    sl_sleeptimer_stop_timer(&connect_timer);
}

static void send_link_up(void) {
    bd_addr addr;
    uint8_t at = 0, b[1 + 1 + 6 + 2];
    memset(&addr, 0, sizeof addr);
    sl_bt_connection_get_remote_address(link_conn, &addr, &at);
    b[0] = link_central ? 0 : 1; /* 0 = we connected, 1 = they connected */
    b[1] = at;
    for (int k = 0; k < 6; k++) {
        b[2 + k] = addr.addr[5 - k];
    }
    b[8] = (uint8_t)link_mtu;
    b[9] = (uint8_t)(link_mtu >> 8);
    link_state = L_UP;
    sl_sleeptimer_stop_timer(&connect_timer);
    events_push(EV_LINK_UP, b, sizeof b);
}

/* Give up on a connection being made: close it; connection_closed sends
 * LINK_FAILED with `why`. */
static void connect_abort(uint16_t why) {
    link_fail = why;
    if (link_conn != NO_CONN) { /* also cancels an attempt not yet open */
        sl_status_t sc = sl_bt_connection_close(link_conn);
        if (sc != SL_STATUS_OK) {
            /* no "closed" event will come: without this the link state
             * stays busy for good (2026-10-13, a stuck CONNECT) */
            events_log_error("abort: close 0x%04X state %d", (unsigned)sc, (int)link_state);
            uint8_t b[2] = {(uint8_t)why, (uint8_t)(why >> 8)};
            link_reset();
            events_push(EV_LINK_FAILED, b, 2);
        }
    } else {
        uint8_t b[2] = {(uint8_t)why, (uint8_t)(why >> 8)};
        link_reset();
        events_push(EV_LINK_FAILED, b, 2);
    }
}

static uint8_t do_connect(const uint8_t *p, size_t len) {
    if (len != 9) {
        return ST_BAD_ARGS;
    }
    if (link_state != L_IDLE) {
        events_log_error("CONNECT: link state %d", (int)link_state);
        return ST_STATE;
    }
    if (scanning) {
        scan_finish();
    }
    bd_addr addr;
    for (int k = 0; k < 6; k++) {
        addr.addr[k] = p[1 + 5 - k]; /* sent most significant first */
    }
    uint16_t ms = (uint16_t)(p[7] | p[8] << 8);
    uint8_t conn;
    sl_status_t sc = sl_bt_connection_open(addr, p[0], sl_bt_gap_phy_1m, &conn);
    if (sc != SL_STATUS_OK) {
        events_log_error("CONNECT: open 0x%04X type %u", (unsigned)sc, p[0]);
        return ST_FAILED;
    }
    link_reset();
    link_conn = conn;
    link_central = true;
    link_state = L_CONNECTING;
    sl_sleeptimer_start_timer_ms(&connect_timer, ms ? ms : 10000, connect_timer_cb, NULL, 0, 0);
    return ST_OK;
}

/* The connector's steps after connecting, each started when the last one's
 * procedure completes. */
static void connector_next(uint16_t result) {
    sl_status_t sc;
    switch (link_state) {
        case L_DISCOVERING:
            if (result != 0 || !peer_service) {
                connect_abort(result ? result : FAIL_NO_SERVICE);
                break;
            }
            sc = sl_bt_gatt_discover_characteristics(link_conn, peer_service);
            if (sc != SL_STATUS_OK) {
                connect_abort(FAIL_NO_SERVICE);
            } else {
                link_state = L_CHARS;
            }
            break;
        case L_CHARS:
            if (result != 0 || !peer_rx || !peer_tx) {
                connect_abort(result ? result : FAIL_NO_SERVICE);
                break;
            }
            sc = sl_bt_gatt_set_characteristic_notification(link_conn, peer_tx, sl_bt_gatt_notification);
            if (sc != SL_STATUS_OK) {
                connect_abort(FAIL_NO_SERVICE);
            } else {
                link_state = L_SUBSCRIBING;
            }
            break;
        case L_SUBSCRIBING:
            if (result != 0) {
                connect_abort(result);
            } else if (link_mtu >= LINK_MIN_MTU) {
                send_link_up();
            } else {
                link_state = L_WAIT_MTU; /* the exchange may still come */
            }
            break;
        default:
            break;
    }
}

/* ---- requests (MODULE_PROTOCOL.md sec.6) ---- */

static uint8_t do_hello(uint8_t *a, size_t *n) {
    bd_addr addr;
    uint8_t type;
    size_t i = 0;
    a[i++] = MODULE_PROTOCOL_VERSION;
    a[i++] = (uint8_t)strlen(MODULE_FW_VERSION);
    memcpy(a + i, MODULE_FW_VERSION, strlen(MODULE_FW_VERSION));
    i += strlen(MODULE_FW_VERSION);
    if (sl_bt_system_get_identity_address(&addr, &type) != SL_STATUS_OK) {
        memset(addr.addr, 0, 6);
    }
    for (int k = 5; k >= 0; k--) {
        a[i++] = addr.addr[k]; /* most significant byte first, as it's printed */
    }
    a[i++] = (uint8_t)FRAME_MAX_PAYLOAD;
    a[i++] = (uint8_t)(FRAME_MAX_PAYLOAD >> 8);
    *n = i;
    return ST_OK;
}

static uint8_t do_set_name(const uint8_t *p, size_t len) {
    if (len < 1 || p[0] > NAME_MAX || p[0] == 0 || len != (size_t)p[0] + 1) {
        return ST_BAD_ARGS;
    }
    memcpy(name, p + 1, p[0]);
    name[p[0]] = 0;
    sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, p[0], p + 1);
    adv_apply(); /* the scan response carries it */
    return ST_OK;
}

static uint8_t do_link_send(const uint8_t *p, size_t len) {
    if (link_state != L_UP) {
        return ST_NOT_CONNECTED;
    }
    if (len == 0 || len > (size_t)link_mtu - 3) {
        return ST_BAD_ARGS;
    }
    sl_status_t sc;
    if (link_central) {
        uint16_t sent = 0;
        sc = sl_bt_gatt_write_characteristic_value_without_response(link_conn, peer_rx, len, p, &sent);
        if (sc == SL_STATUS_OK && sent != len) {
            sc = SL_STATUS_FAIL;
        }
    } else {
        sc = sl_bt_gatt_server_send_notification(link_conn, gattdb_link_tx, len, p);
    }
    return sc == SL_STATUS_OK ? ST_OK : sc == SL_STATUS_NO_MORE_RESOURCE ? ST_BUSY : ST_FAILED;
}

uint8_t hostlink_handle(uint8_t type, const uint8_t *p, size_t len, uint8_t *a, size_t *n) {
    *n = 0;
    switch (type) {
        case RQ_HELLO:
            return do_hello(a, n);
        case RQ_POLL:
            *n = events_pop(a, FRAME_MAX_PAYLOAD - 1);
            return ST_OK;
        case RQ_SET_NAME:
            return do_set_name(p, len);
        case RQ_ADV_START:
            adv_wanted = true;
            adv_apply();
            return ST_OK;
        case RQ_ADV_STOP:
            adv_wanted = false;
            adv_apply();
            return ST_OK;
        case RQ_SCAN_START:
            return do_scan_start(p, len);
        case RQ_SCAN_STOP:
            scan_finish(); /* SCAN_DONE follows */
            return ST_OK;
        case RQ_CONNECT:
            return do_connect(p, len);
        case RQ_CONNECT_CANCEL:
            if (!link_central || link_state == L_IDLE || link_state == L_UP) {
                return ST_STATE;
            }
            connect_abort(FAIL_CANCELLED);
            return ST_OK;
        case RQ_DISCONNECT: {
            if (link_conn == NO_CONN) {
                if (link_state != L_IDLE) { /* shouldn't be: say so, and recover */
                    events_log_error("DISCONNECT: no conn, state %d", (int)link_state);
                    link_reset();
                }
                return ST_NOT_CONNECTED;
            }
            sl_status_t sc = sl_bt_connection_close(link_conn);
            if (sc != SL_STATUS_OK) {
                bool was_up = link_state == L_UP;
                uint8_t b[2] = {0xFF, 0xF0}; /* 0xF0FF: closed here, not by the stack */
                events_log_error("DISCONNECT: close 0x%04X state %d", (unsigned)sc, (int)link_state);
                link_reset();
                events_push(was_up ? EV_LINK_DOWN : EV_LINK_FAILED, b, 2);
            }
            return ST_OK; /* LINK_DOWN follows */
        }
        case RQ_LINK_SEND:
            return do_link_send(p, len);
        case RQ_KBD_PAIR:
        case RQ_KBD_STOP:
        case RQ_KBD_FORGET:
        case RQ_KBD_ENABLE:
        case RQ_KBD_STATUS:
            return kbd_request(type, p, len, a, n); /* kbd.c */
        default:
            return ST_UNKNOWN_TYPE;
    }
}

bool app_link_connecting(void) {
    return link_central && link_state != L_IDLE && link_state != L_UP;
}

/* ---- the Bluetooth stack ---- */

void sl_bt_on_event(sl_bt_msg_t *evt) {
    if (kbd_on_event(evt)) {
        return; /* the keyboard's connection's (kbd.c) */
    }
    switch (SL_BT_MSG_ID(evt->header)) {
        case sl_bt_evt_system_boot_id: {
            uint16_t got;
            sl_status_t sc = sl_bt_advertiser_create_set(&adv_set);
            app_assert_status(sc);
            sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, strlen(name), (const uint8_t *)name);
            sl_bt_gatt_set_max_mtu(LINK_FRAME_MAX + 3, &got); /* both roles: 247 */
            adv_apply();
            kbd_init();
            events_log("MODULE BOOT %s", MODULE_FW_VERSION);
            break;
        }
        case sl_bt_evt_connection_opened_id: {
            sl_bt_evt_connection_opened_t *e = &evt->data.evt_connection_opened;
            if (e->role == sl_bt_connection_role_central && e->connection == link_conn &&
                link_state == L_CONNECTING) {
                /* ours: find the Link service */
                link_state = L_DISCOVERING;
                if (sl_bt_gatt_discover_primary_services_by_uuid(link_conn, 16, kLinkUuid) != SL_STATUS_OK) {
                    connect_abort(FAIL_NO_SERVICE);
                }
            } else if (e->role == sl_bt_connection_role_peripheral && link_conn == NO_CONN) {
                link_reset(); /* a peer connected to our advertising */
                link_conn = e->connection;
                link_state = L_PERIPHERAL;
            } else if (e->connection != link_conn) {
                sl_bt_connection_close(e->connection); /* one Link at a time */
            }
            break;
        }
        case sl_bt_evt_gatt_mtu_exchanged_id:
            if (evt->data.evt_gatt_mtu_exchanged.connection == link_conn) {
                link_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
                if (link_state == L_WAIT_MTU) {
                    if (link_mtu >= LINK_MIN_MTU) {
                        send_link_up();
                    } else {
                        connect_abort(FAIL_SMALL_MTU);
                    }
                }
            }
            break;
        case sl_bt_evt_gatt_service_id: {
            sl_bt_evt_gatt_service_t *e = &evt->data.evt_gatt_service;
            if (e->connection == link_conn && e->uuid.len == 16 && memcmp(e->uuid.data, kLinkUuid, 16) == 0) {
                peer_service = e->service;
            }
            break;
        }
        case sl_bt_evt_gatt_characteristic_id: {
            sl_bt_evt_gatt_characteristic_t *e = &evt->data.evt_gatt_characteristic;
            if (e->connection == link_conn && e->uuid.len == 16) {
                if (memcmp(e->uuid.data, kRxUuid, 16) == 0) {
                    peer_rx = e->characteristic;
                } else if (memcmp(e->uuid.data, kTxUuid, 16) == 0) {
                    peer_tx = e->characteristic;
                }
            }
            break;
        }
        case sl_bt_evt_gatt_procedure_completed_id: {
            sl_bt_evt_gatt_procedure_completed_t *e = &evt->data.evt_gatt_procedure_completed;
            if (e->connection == link_conn && link_central) {
                connector_next(e->result);
            }
            break;
        }
        case sl_bt_evt_gatt_characteristic_value_id: {
            /* connector: the peer's TX notifications */
            sl_bt_evt_gatt_characteristic_value_t *e = &evt->data.evt_gatt_characteristic_value;
            if (e->connection == link_conn && e->characteristic == peer_tx &&
                e->att_opcode == sl_bt_gatt_handle_value_notification && e->value.len && link_state == L_UP) {
                events_push(EV_LINK_RX, e->value.data, e->value.len);
            }
            break;
        }
        case sl_bt_evt_gatt_server_characteristic_status_id: {
            /* advertiser: the peer subscribing to our TX brings the link up */
            sl_bt_evt_gatt_server_characteristic_status_t *e = &evt->data.evt_gatt_server_characteristic_status;
            if (e->connection == link_conn && !link_central && e->characteristic == gattdb_link_tx &&
                e->status_flags == sl_bt_gatt_server_client_config &&
                (e->client_config_flags & sl_bt_gatt_notification) && link_state == L_PERIPHERAL) {
                send_link_up();
            }
            break;
        }
        case sl_bt_evt_gatt_server_attribute_value_id: {
            /* advertiser: the peer writing our RX */
            sl_bt_evt_gatt_server_attribute_value_t *e = &evt->data.evt_gatt_server_attribute_value;
            if (e->connection == link_conn && e->attribute == gattdb_link_rx && e->value.len && link_state == L_UP) {
                events_push(EV_LINK_RX, e->value.data, e->value.len);
            }
            break;
        }
        case sl_bt_evt_scanner_legacy_advertisement_report_id:
            if (scanning) {
                on_adv_report(&evt->data.evt_scanner_legacy_advertisement_report);
            }
            kbd_on_adv(&evt->data.evt_scanner_legacy_advertisement_report);
            break;
        case sl_bt_evt_connection_closed_id: {
            sl_bt_evt_connection_closed_t *e = &evt->data.evt_connection_closed;
            if (e->connection == link_conn) {
                bool was_up = link_state == L_UP;
                uint16_t why = link_fail ? link_fail : e->reason;
                uint8_t b[2] = {(uint8_t)why, (uint8_t)(why >> 8)};
                link_reset();
                events_push(was_up ? EV_LINK_DOWN : EV_LINK_FAILED, b, 2);
                adv_apply(); /* advertise again if the MCU still wants it */
            }
            break;
        }
        default:
            break;
    }
}

/* ---- start-up and the main loop ---- */

void app_init(void) {
    events_init();
    hostlink_init();
    GPIO_PinModeSet(MODULE_WAKE_PORT, MODULE_WAKE_PIN, gpioModeInputPull, 1);
    GPIOINT_Init();
    GPIOINT_CallbackRegister(MODULE_WAKE_PIN, wake_isr);
    GPIO_ExtIntConfig(MODULE_WAKE_PORT, MODULE_WAKE_PIN, MODULE_WAKE_PIN, true, true, true);
    /* No RTOS: sl_iostream_read() doesn't block (its set_read_block()
     * exists only with a kernel -- don't call it). */
    session_update();
}

void app_process_action(void) {
    if (wake_changed) {
        wake_changed = false;
        session_update();
    }
    if (scan_expired) {
        scan_expired = false;
        scan_finish();
    }
    if (connect_expired) {
        connect_expired = false;
        if (link_central && link_state != L_UP && link_state != L_IDLE) {
            connect_abort(FAIL_TIMEOUT);
        }
    }
    kbd_tick();
    hostlink_poll();
}
