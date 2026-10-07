/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* kbd_host.c -- see kbd_host.h.
 *
 * Classic Bluetooth keyboards (2026-10-04) and BLE ones, HID over GATT
 * (2026-10-06): one keyboard at a time, of either kind -- PAIR looks for
 * both at once and takes the first it finds. Boot protocol, so there's no
 * report descriptor to fetch or parse: every keyboard sends the same
 * 8-byte report, and a combined keyboard and touchpad's mouse reports (a
 * different length) are simply ignored.
 *
 * Pairing: this side says it can display but not input (SSP Display Only)
 * and asks for MITM protection, so a keyboard pairs by Passkey Entry -- the
 * PC-1500 shows six digits, the user types them on the keyboard and presses
 * Enter. A Bluetooth 2.0 keyboard asks for a PIN instead (legacy pairing):
 * the same, with a PIN chosen here. The bond's link key goes to bt_store.c;
 * the keyboard's address and name to the same store, under KBD_TAG.
 *
 * BLE: PAIR scans for the HID service in advertisements, connects, and
 * pairs with the security manager as Display Only with MITM -- the same
 * six digits to type. That's only for the keyboard's pairing: the Link
 * needs No Input No Output (ble_link.c), so it's put back after. Then
 * BTstack's HID service host (hids_host), in report protocol: each report
 * is read through the keyboard's own report map into the boot report the
 * sequencer takes. (Boot protocol first, 2026-10-06: the KHB030 went on
 * sending on its report characteristic, which hids_host doesn't listen to
 * in boot mode -- not one key arrived.) A BLE keyboard
 * can't call us as a classic one does: once bonded, a background
 * connection waits for it to advertise, scanning at a low duty cycle
 * (board owner's call: the first key after it has slept may be lost). It
 * steps aside while the Link scans or connects -- BTstack makes one LE
 * connection at a time -- and starts again after (le_tick()). */
#include "kbd_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "bt_store.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "hci_dump.h"
#include "hardware/sync.h"
#include "kbd_seq.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "monitor.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"

#define KBD_TAG 0x4B424430u    /* 'KBD0': [address 6][name length][name] -- a classic keyboard */
#define KBD_TAG_LE 0x4B424431u /* 'KBD1': [kind][LE address type][address 6][name length][name] */
#define KIND_CLASSIC 0
#define KIND_LE 1
/* The background connection to a bonded BLE keyboard: 30 ms of scanning
 * every 1.28 s (units of 0.625 ms); BTstack's defaults (hci.c) otherwise. */
#define LE_BG_SCAN_INTERVAL 0x0800
#define LE_BG_SCAN_WINDOW 0x0030
#define LE_SCAN_INTERVAL 0x0060
#define LE_SCAN_WINDOW 0x0030
#define LE_CONN_INTERVAL_MIN 0x0008
#define LE_CONN_INTERVAL_MAX 0x0018
#define LE_CONN_LATENCY 4
#define LE_SUPERVISION_TIMEOUT 0x0048
#define LE_TICK_MS 1000
#define SEARCH_UNITS 10     /* inquiry length, x 1.28 s */
#define COD_MAJOR_PERIPHERAL 0x0500u
#define COD_MINOR_KEYBOARD 0x0040u

/* ---- state (core0, BTstack's context) ---- */

static volatile uint8_t g_state = EXP_KBD_NONE;
static bd_addr_t g_addr;   /* the paired keyboard, or the one being paired */
static bool g_have_addr;   /* g_addr is a bonded keyboard */
static char g_name[EXP_KBD_NAME_MAX + 1];
static char g_code[7];     /* what to type on the keyboard */
static uint16_t g_cid;     /* HID host connection, or 0 */
static bool g_pairing;     /* PAIR's search/connection, as opposed to a reconnection */
static uint16_t g_pair_cid; /* PAIR's own connection: only its events end a pairing */
static bool g_pair_retry;  /* PAIR found the keyboard while an older connection to it
                              (a reconnection under way) was still closing */
static btstack_packet_callback_registration_t g_hci_cb;
static uint8_t g_descriptors[64]; /* hid_host's; boot protocol fetches none */

/* BLE (2026-10-06) */
static bool g_le;                 /* g_addr is a BLE keyboard */
static bd_addr_type_t g_addr_type; /* ...and its address type */
static hci_con_handle_t g_le_con = HCI_CON_HANDLE_INVALID;
static uint16_t g_hids_cid;       /* hids_host's connection, or 0 */
static bool g_le_scanning;        /* PAIR's LE scan is on */
static bool g_le_rescan;          /* ...for a retry: PAIR's keyboard, found again */
static bool g_le_auto;            /* the background connection to the bonded keyboard is on */
static bool g_le_sm_display;      /* the security manager is set up for its pairing */
static btstack_packet_callback_registration_t g_sm_cb;
static btstack_timer_source_t g_le_timer;
static uint8_t g_hids_descriptors[512]; /* hids_host's: the keyboard's report map */

/* Reports, from BTstack's interrupt to the bus loop: one producer, one
 * consumer, on the same core. */
#define RING 16
static uint8_t g_ring[RING][8];
static volatile uint8_t g_ring_head, g_ring_tail;

static kbd_seq_t g_seq; /* the bus loop's */

/* For BLKBD ? (EXP_COMMAND_KBD_STATUS): what's actually arriving. */
static uint16_t g_diag_reports;
static uint8_t g_diag_len, g_diag_head[4];
static uint8_t g_diag_protocol = 0xFF; /* SET_PROTOCOL's answer: handshake << 4 | mode; FF none */
static uint8_t g_diag_fail_step, g_diag_fail_status; /* where PAIR last failed (1 search, 2 connect,
                                                        3 connection, 4 pairing), and BTstack's status */

static void pair_failed(uint8_t step, uint8_t status) {
    g_diag_fail_step = step;
    g_diag_fail_status = status;
    g_state = EXP_KBD_FAILED;
    g_pairing = false;
    g_pair_retry = false;
}

/* CONNECTING can't wait for ever (2026-10-06: on hardware it did). Waiting
 * on an older connection to the keyboard that BTstack drops without an
 * event (pair_connect()'s retry) fails as step 2, COMMAND_DISALLOWED; a
 * keyboard that never answers our connection, as step 3, CONNECTION_TIMEOUT.
 * Once it's asking for the code, the state isn't CONNECTING any more. */
#define CONNECT_TIMEOUT_MS 20000
static btstack_timer_source_t g_connect_timer;

static void connect_timed_out(btstack_timer_source_t *ts) {
    (void)ts;
    if (!g_pairing || g_state != EXP_KBD_CONNECTING) return;
    if (g_le) {
        if (g_le_scanning) gap_stop_scan(); /* a retry still looking for it */
        g_le_scanning = g_le_rescan = false;
        if (g_le_con != HCI_CON_HANDLE_INVALID) gap_disconnect(g_le_con);
        else gap_connect_cancel();
        pair_failed(3, ERROR_CODE_CONNECTION_TIMEOUT);
        return;
    }
    if (g_pair_retry) {
        pair_failed(2, ERROR_CODE_COMMAND_DISALLOWED);
    } else {
        if (g_pair_cid) hid_host_disconnect(g_pair_cid); /* its CLOSED comes later: ignored */
        pair_failed(3, ERROR_CODE_CONNECTION_TIMEOUT);
    }
}

static void connect_timer_start(void) {
    btstack_run_loop_remove_timer(&g_connect_timer);
    btstack_run_loop_set_timer_handler(&g_connect_timer, connect_timed_out);
    btstack_run_loop_set_timer(&g_connect_timer, CONNECT_TIMEOUT_MS);
    btstack_run_loop_add_timer(&g_connect_timer);
}

/* PAIR's connection to the keyboard just found (g_addr). */
static void pair_connect(void) {
    uint8_t status = hid_host_connect(g_addr, HID_PROTOCOL_MODE_BOOT, &g_pair_cid);
    if (status == ERROR_CODE_SUCCESS) {
        g_cid = g_pair_cid;
        g_pair_retry = false;
    } else if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        g_pair_retry = true; /* an older connection to it is still there: again once it's gone */
    } else {
        pair_failed(2, status);
    }
}

bool kbd_host_wanted(void) { return mcu_config_get(MCU_CONFIG_BLKBD) != 0; }

/* A connection to g_addr is ours: the bonded keyboard, or PAIR's. */
static bool g_have_addr_or_pairing(void) { return g_have_addr || g_pairing; }

/* Both kinds under KBD_TAG_LE; a classic keyboard paired before BLE ones
 * came (2026-10-06) is still read from KBD_TAG. */
static void save_keyboard(void) {
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    uint8_t rec[2 + 6 + 1 + EXP_KBD_NAME_MAX];
    uint8_t n = (uint8_t)strlen(g_name);
    rec[0] = g_le ? KIND_LE : KIND_CLASSIC;
    rec[1] = (uint8_t)g_addr_type;
    memcpy(rec + 2, g_addr, 6);
    rec[8] = n;
    memcpy(rec + 9, g_name, n);
    tlv->delete_tag(ctx, KBD_TAG);
    tlv->store_tag(ctx, KBD_TAG_LE, rec, (uint32_t)(9 + n));
}

static void load_keyboard(void) {
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    uint8_t rec[2 + 6 + 1 + EXP_KBD_NAME_MAX];
    uint8_t n;
    int len = tlv->get_tag(ctx, KBD_TAG_LE, rec, sizeof rec);
    if (len >= 9) {
        g_le = rec[0] == KIND_LE;
        g_addr_type = (bd_addr_type_t)rec[1];
        memcpy(g_addr, rec + 2, 6);
        n = rec[8] > EXP_KBD_NAME_MAX || 9 + rec[8] > len ? 0 : rec[8];
        memcpy(g_name, rec + 9, n);
    } else {
        len = tlv->get_tag(ctx, KBD_TAG, rec, sizeof rec);
        if (len < 7) {
            g_have_addr = false;
            return;
        }
        g_le = false;
        memcpy(g_addr, rec, 6);
        n = rec[6] > EXP_KBD_NAME_MAX || 7 + rec[6] > len ? 0 : rec[6];
        memcpy(g_name, rec + 7, n);
    }
    g_have_addr = true;
    g_name[n] = 0;
    g_state = EXP_KBD_PAIRED;
}

/* ---- BLE ---- */

/* What a BLE keyboard's connection went through, for the MCU log (MLOG
 * VERBOSE): core0 can't write the log (flash), so the events wait here and
 * core1 logs them on its next keyboard command -- BLKBD polls STATUS.
 * Each is a tag and two bytes: CON status, SMS (pairing started), PKY
 * (passkey shown), JW/NC (Just Works / numeric comparison asked), SMC
 * status reason (pairing complete), REN status (re-encryption), HID status
 * (HID service), DIS reason (disconnected). */
#define TRACE 24
static struct {
    char tag[4];
    uint8_t a, b;
    uint16_t ms; /* since the connection (CON), up to 65535 */
} g_trace[TRACE];
static volatile uint8_t g_trace_head, g_trace_tail;
static uint32_t g_trace_t0;

static void trace(const char *tag, uint8_t a, uint8_t b) {
    uint8_t next = (uint8_t)((g_trace_head + 1) % TRACE);
    if (next == g_trace_tail) return; /* full: core1 hasn't come by */
    uint32_t now = btstack_run_loop_get_time_ms();
    if (strcmp(tag, "CON") == 0) g_trace_t0 = now;
    uint32_t ms = now - g_trace_t0;
    strncpy(g_trace[g_trace_head].tag, tag, 3);
    g_trace[g_trace_head].tag[3] = 0;
    g_trace[g_trace_head].a = a;
    g_trace[g_trace_head].b = b;
    g_trace[g_trace_head].ms = (uint16_t)(ms > 65535 ? 65535 : ms);
    __dmb();
    g_trace_head = next;
}

/* The security manager's own messages on the keyboard's connection, and
 * LE signalling, seen through BTstack's packet dump hook (2026-10-06,
 * pairing the KHB030): S> / S< an SMP PDU sent / received (its opcode and
 * first byte -- 05 is Pairing Failed, with its reason), L> / L< LE
 * signalling (12/13: connection parameter update request/response). And
 * BTstack's log_error() messages (ENABLE_LOG_ERROR), as text. */
static void dump_packet(uint8_t type, uint8_t in, uint8_t *packet, uint16_t len) {
    if (type != HCI_ACL_DATA_PACKET || len < 9 || g_le_con == HCI_CON_HANDLE_INVALID) return;
    if ((little_endian_read_16(packet, 0) & 0x0FFF) != g_le_con) return;
    uint16_t cid = little_endian_read_16(packet, 6);
    if (cid == 0x0006) trace(in ? "S<" : "S>", packet[8], len > 9 ? packet[9] : 0);
    else if (cid == 0x0005) trace(in ? "L<" : "L>", packet[8], 0);
}

#define ERRORS 4
static char g_errors[ERRORS][MCU_LOG_MSG_MAX + 1];
static volatile uint8_t g_errors_head, g_errors_tail;

static void dump_message(int level, const char *format, va_list args) {
    (void)level;
    uint8_t next = (uint8_t)((g_errors_head + 1) % ERRORS);
    if (next == g_errors_tail) return;
    vsnprintf(g_errors[g_errors_head], sizeof g_errors[0], format, args);
    __dmb();
    g_errors_head = next;
}

static const hci_dump_t kDump = {NULL, dump_packet, dump_message};

/* core1 */
static void trace_to_log(void) {
    while (g_errors_tail != g_errors_head) {
        __dmb();
        mcu_log_info(g_errors[g_errors_tail]);
        g_errors_tail = (uint8_t)((g_errors_tail + 1) % ERRORS);
    }
    static const char hex[] = "0123456789ABCDEF";
    while (g_trace_tail != g_trace_head) {
        __dmb();
        char msg[] = "KBD ... 00 00 T00000";
        uint16_t ms = g_trace[g_trace_tail].ms;
        memcpy(msg + 4, g_trace[g_trace_tail].tag, 3);
        for (int i = 4; i < 7; i++)
            if (msg[i] == 0) msg[i] = ' ';
        msg[8] = hex[g_trace[g_trace_tail].a >> 4];
        msg[9] = hex[g_trace[g_trace_tail].a & 15];
        msg[11] = hex[g_trace[g_trace_tail].b >> 4];
        msg[12] = hex[g_trace[g_trace_tail].b & 15];
        for (int i = 19; i >= 15; i--, ms /= 10) msg[i] = (char)('0' + ms % 10);
        g_trace_tail = (uint8_t)((g_trace_tail + 1) % TRACE);
        mcu_log_info(msg);
    }
}

/* The security manager as the Link wants it (ble_link.c, BTstack's
 * defaults), or as a keyboard's pairing does: MITM protection (the code to
 * type), unless g_le_just_works. */
static bool g_le_just_works;

/* Pairing tries (2026-10-06): the KHB030 sometimes sends its Pairing
 * Confirm before its Pairing Response, against the spec; BTstack, still
 * waiting for the response, gives up (Pairing Failed, unspecified reason).
 * When the confirm comes later it pairs fine -- so another try usually
 * does: on the same connection, or a new one if the keyboard hung up. */
#define LE_PAIR_TRIES 4
static uint8_t g_le_tries;

static void le_sm_for_keyboard(bool on) {
    if (on == g_le_sm_display) return;
    g_le_sm_display = on;
    uint8_t auth = SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION;
    if (!g_le_just_works) auth |= SM_AUTHREQ_MITM_PROTECTION;
    sm_set_io_capabilities(on ? IO_CAPABILITY_DISPLAY_ONLY : IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(on ? auth : 0);
}

static void le_conn_params(bool background) {
    gap_set_connection_parameters(background ? LE_BG_SCAN_INTERVAL : LE_SCAN_INTERVAL,
                                  background ? LE_BG_SCAN_WINDOW : LE_SCAN_WINDOW, LE_CONN_INTERVAL_MIN,
                                  LE_CONN_INTERVAL_MAX, LE_CONN_LATENCY, LE_SUPERVISION_TIMEOUT, 0, 0);
}

static void le_auto_stop(void) {
    if (!g_le_auto) return;
    gap_auto_connection_stop_all();
    g_le_auto = false;
    le_conn_params(false);
}

/* The background connection to the bonded BLE keyboard, if it should be
 * on and isn't. */
static void le_auto_start(void) {
    if (g_le_auto || !kbd_host_wanted() || !g_have_addr || !g_le || g_pairing ||
        g_le_con != HCI_CON_HANDLE_INVALID || hci_get_state() != HCI_STATE_WORKING || ble_link_le_busy())
        return;
    le_conn_params(true);
    if (gap_auto_connection_start(g_addr_type, g_addr) == ERROR_CODE_SUCCESS) g_le_auto = true;
    else le_conn_params(false);
}

static void le_tick(btstack_timer_source_t *ts) {
    if (!kbd_host_wanted()) le_auto_stop();
    else le_auto_start();
    btstack_run_loop_set_timer(ts, LE_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

void kbd_host_le_yield(void) { le_auto_stop(); }

void kbd_host_trace_to_log(void) { trace_to_log(); }

/* A report into the ring, as for a classic keyboard's. */
static void push_report(const uint8_t keys[8]) {
    uint8_t next = (uint8_t)((g_ring_head + 1) % RING);
    if (next == g_ring_tail) return; /* the loop has fallen behind: drop it */
    memcpy(g_ring[g_ring_head], keys, 8);
    __dmb();
    g_ring_head = next;
}

/* `type` isn't checked: hids_host hands its reports over with type
 * HCI_EVENT_GATTSERVICE_META, its other events with HCI_EVENT_PACKET
 * (hids_host.c) -- checking for the latter dropped every key (2026-10-06). */
/* A report-protocol input report -- `report` with its report ID, if the
 * map uses them -- read through the keyboard's report map into an 8-byte
 * boot report: the modifier bits (usages E0H-E7H), then up to six keys.
 * Array fields (the usual six slots) and bitmap ones (NKRO) both: the
 * parser gives an array's key as the usage with value 1, a bitmap's as the
 * usage with its bit. False if the report has nothing of the keyboard's
 * (a media key's, a touchpad's). */
static bool boot_report_from(uint8_t service, const uint8_t *report, uint16_t len, uint8_t boot[8]) {
    btstack_hid_parser_t parser;
    bool keyboard = false;
    uint8_t keys = 0;
    memset(boot, 0, 8);
    btstack_hid_parser_init(&parser, hids_host_descriptor_storage_get_descriptor_data(g_hids_cid, service),
                            hids_host_descriptor_storage_get_descriptor_len(g_hids_cid, service), HID_REPORT_TYPE_INPUT,
                            report, len);
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t page, usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (page != HID_USAGE_PAGE_KEYBOARD) continue;
        keyboard = true;
        if (value == 0 || usage == 0) continue;
        if (usage >= 0xE0 && usage <= 0xE7) boot[0] |= (uint8_t)(1u << (usage - 0xE0));
        else if (keys < 6 && usage < 0xE0) boot[2 + keys++] = (uint8_t)usage;
    }
    return keyboard;
}

static void on_hids(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)type;
    (void)channel;
    (void)size;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            trace("HID", status, gattservice_subevent_hid_service_connected_get_protocol_mode(packet));
            if (status != ERROR_CODE_SUCCESS) {
                g_hids_cid = 0;
                if (g_pairing) pair_failed(5, status);
                if (g_le_con != HCI_CON_HANDLE_INVALID) gap_disconnect(g_le_con);
                break;
            }
            g_diag_protocol = HID_PROTOCOL_MODE_REPORT;
            g_diag_reports = 0;
            if (g_pairing) {
                g_have_addr = true;
                save_keyboard(); /* core1 writes it to flash (bt_store_commit) */
                btstack_run_loop_remove_timer(&g_connect_timer);
            }
            g_pairing = false;
            g_state = EXP_KBD_CONNECTED;
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            g_hids_cid = 0;
            break;
        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION:
            trace("NTF", gattservice_subevent_hid_service_reports_notification_get_configuration(packet), 0);
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            /* Report protocol: through the report map (boot_report_from()). */
            const uint8_t *r = gattservice_subevent_hid_report_get_report(packet);
            uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
            uint8_t boot[8];
            g_diag_reports++;
            g_diag_len = (uint8_t)(len > 255 ? 255 : len);
            memcpy(g_diag_head, r, len < 4 ? len : 4);
            if (g_diag_reports <= 4) trace("RPT", (uint8_t)len, len ? r[0] : 0); /* the first few, for the log */
            if (boot_report_from(gattservice_subevent_hid_report_get_service_index(packet), r, len, boot))
                push_report(boot);
            break;
        }
        default:
            break;
    }
}

/* Encrypted: the keyboard's HID service. */
static void le_hid_start(void) {
    uint8_t rc = hids_host_connect(g_le_con, on_hids, HID_PROTOCOL_MODE_REPORT, &g_hids_cid);
    trace("HIC", rc, 0);
    if (rc == ERROR_CODE_SUCCESS) return;
    g_hids_cid = 0;
    if (g_pairing) pair_failed(5, rc);
    gap_disconnect(g_le_con);
}

/* PAIR found a BLE keyboard (g_addr, g_addr_type): connect to it -- a
 * fresh pairing, so any bond left from an earlier try goes first (its key
 * would be used to re-encrypt instead). */
static void le_pair_connect(void) {
    le_conn_params(false);
    gap_delete_bonding(g_addr_type, g_addr);
    if (g_le_tries <= 1) g_le_just_works = false; /* a retry keeps what the last try learnt */
    le_sm_for_keyboard(true);
    uint8_t status = gap_connect(g_addr, g_addr_type);
    if (status != ERROR_CODE_SUCCESS) pair_failed(2, status);
}

/* Is this advertisement a HID device's -- the HID service, or a keyboard's
 * appearance? Its name, if it carries one, into `name`. */
static bool le_advert_is_keyboard(const uint8_t *adv, uint8_t len, char name[EXP_KBD_NAME_MAX + 1]) {
    bool hid = false;
    ad_context_t it;
    for (ad_iterator_init(&it, len, adv); ad_iterator_has_more(&it); ad_iterator_next(&it)) {
        uint8_t t = ad_iterator_get_data_type(&it), n = ad_iterator_get_data_len(&it);
        const uint8_t *d = ad_iterator_get_data(&it);
        if ((t == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME || t == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) && n) {
            if (n > EXP_KBD_NAME_MAX) n = EXP_KBD_NAME_MAX;
            memcpy(name, d, n);
            name[n] = 0;
        }
        if (t == BLUETOOTH_DATA_TYPE_APPEARANCE && n == 2 && little_endian_read_16(d, 0) == 0x03C1) hid = true;
    }
    return hid || ad_data_contains_uuid16(len, adv, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

/* The BLE keyboard's connection's events: connection, security, gone. */
static void le_on_event(uint8_t *packet) {
    hci_con_handle_t handle;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_ENCRYPTION_CHANGE:
            if (hci_event_encryption_change_get_connection_handle(packet) == g_le_con)
                trace("ENC", hci_event_encryption_change_get_status(packet),
                      hci_event_encryption_change_get_encryption_enabled(packet));
            break;
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE &&
                hci_subevent_le_connection_update_complete_get_connection_handle(packet) == g_le_con)
                trace("CUP", hci_subevent_le_connection_update_complete_get_status(packet),
                      (uint8_t)hci_subevent_le_connection_update_complete_get_conn_interval(packet));
            break;
        case SM_EVENT_SECURITY_REQUEST:
            if (sm_event_security_request_get_handle(packet) == g_le_con)
                trace("SRQ", sm_event_security_request_get_auth_req(packet), 0);
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            /* PAIR's search, or a retry's (CONNECTING, g_le_rescan) */
            if (!g_le_scanning || !(g_state == EXP_KBD_SEARCHING || (g_state == EXP_KBD_CONNECTING && g_le_rescan)))
                break;
            char name[EXP_KBD_NAME_MAX + 1] = "KEYBOARD";
            if (!le_advert_is_keyboard(gap_event_advertising_report_get_data(packet),
                                       gap_event_advertising_report_get_data_length(packet), name))
                break;
            gap_stop_scan();
            if (!g_le_rescan) gap_inquiry_stop();
            g_le_scanning = false;
            g_le_rescan = false;
            g_le = true;
            gap_event_advertising_report_get_address(packet, g_addr);
            g_addr_type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
            strcpy(g_name, name);
            if (g_state == EXP_KBD_SEARCHING) { /* a retry keeps its tries and its time */
                g_state = EXP_KBD_CONNECTING;
                connect_timer_start();
                g_le_tries = 1;
            }
            le_pair_connect();
            break;
        }
        case HCI_EVENT_META_GAP: {
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            if (gap_subevent_le_connection_complete_get_role(packet) != HCI_ROLE_MASTER || !g_le || !g_have_addr_or_pairing())
                break;
            bd_addr_t addr;
            gap_subevent_le_connection_complete_get_peer_address(packet, addr);
            if (bd_addr_cmp(addr, g_addr) != 0) break; /* the Link's, or someone else's */
            if (g_le_auto) { /* the background connection found it */
                gap_auto_connection_stop_all();
                g_le_auto = false;
                le_conn_params(false);
            }
            uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
            trace("CON", status, (uint8_t)gap_subevent_le_connection_complete_get_peer_address_type(packet));
            if (status != ERROR_CODE_SUCCESS) {
                if (g_pairing) pair_failed(3, status);
                break;
            }
            g_le_con = gap_subevent_le_connection_complete_get_connection_handle(packet);
            sm_request_pairing(g_le_con); /* pairs, or re-encrypts with the bond's key */
            break;
        }
        case SM_EVENT_PAIRING_STARTED:
            if (sm_event_pairing_started_get_handle(packet) == g_le_con) trace("SMS", 0, 0);
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER: /* type this on the keyboard */
            if (sm_event_passkey_display_number_get_handle(packet) != g_le_con) break;
            trace("PKY", 0, 0);
            snprintf(g_code, sizeof g_code, "%06lu",
                     (unsigned long)sm_event_passkey_display_number_get_passkey(packet));
            g_state = EXP_KBD_CODE;
            break;
        case SM_EVENT_JUST_WORKS_REQUEST: /* a keyboard that asks no code */
            handle = sm_event_just_works_request_get_handle(packet);
            if (handle != g_le_con) break;
            trace("JW", 0, 0);
            sm_just_works_confirm(handle);
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            handle = sm_event_numeric_comparison_request_get_handle(packet);
            if (handle != g_le_con) break;
            trace("NC", 0, 0);
            sm_numeric_comparison_confirm(handle);
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            if (sm_event_pairing_complete_get_handle(packet) != g_le_con) break;
            le_sm_for_keyboard(false);
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            uint8_t reason = sm_event_pairing_complete_get_reason(packet);
            trace("SMC", status, reason);
            /* A keyboard that can't do the code (2026-10-06, the KHB030
             * refused MITM with "authentication requirements"): once more
             * without it -- Just Works, nothing to type. */
            if (status != ERROR_CODE_SUCCESS && g_pairing && !g_le_just_works &&
                reason == SM_REASON_AUTHENTHICATION_REQUIREMENTS) {
                g_le_just_works = true;
                le_sm_for_keyboard(true);
                sm_request_pairing(g_le_con);
                break;
            }
            /* the keyboard hung up: the disconnection's handler tries again */
            if (status == ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION && g_pairing && g_le_tries < LE_PAIR_TRIES)
                break;
            if (status != ERROR_CODE_SUCCESS && g_pairing && g_le_tries < LE_PAIR_TRIES) {
                trace("TRY", ++g_le_tries, reason);
                le_sm_for_keyboard(true);
                sm_request_pairing(g_le_con);
                break;
            }
            if (status != ERROR_CODE_SUCCESS) {
                if (g_pairing) pair_failed(4, status);
                gap_disconnect(g_le_con);
                break;
            }
            le_hid_start();
            break;
        }
        case SM_EVENT_REENCRYPTION_COMPLETE: /* the bonded keyboard, back */
            if (sm_event_reencryption_complete_get_handle(packet) != g_le_con) break;
            trace("REN", sm_event_reencryption_complete_get_status(packet), 0);
            if (sm_event_reencryption_complete_get_status(packet) != ERROR_CODE_SUCCESS)
                gap_disconnect(g_le_con); /* it lost the bond: BLKBD to pair again */
            else
                le_hid_start();
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (hci_event_disconnection_complete_get_connection_handle(packet) != g_le_con) break;
            trace("DIS", hci_event_disconnection_complete_get_status(packet),
                  hci_event_disconnection_complete_get_reason(packet));
            static const uint8_t kNone[8] = {0}; /* its keys up: it can't say so any more */
            push_report(kNone);
            g_le_con = HCI_CON_HANDLE_INVALID;
            g_hids_cid = 0;
            le_sm_for_keyboard(false);
            if (g_pairing && (g_state == EXP_KBD_CONNECTING || g_state == EXP_KBD_CODE) && g_le_tries < LE_PAIR_TRIES) {
                /* The keyboard hung up. Find it again rather than connect to
                 * the address it had: after hanging up mid-pairing, the
                 * KHB030 couldn't be connected there (2026-10-07). */
                trace("TRY", ++g_le_tries, 0xDC); /* DC: after a disconnection */
                g_le_rescan = true;
                gap_set_scan_parameters(1, LE_SCAN_INTERVAL, LE_SCAN_WINDOW);
                gap_start_scan();
                g_le_scanning = true;
                break;
            }
            if (g_pairing) pair_failed(3, hci_event_disconnection_complete_get_reason(packet));
            else if (g_state == EXP_KBD_CONNECTED) g_state = EXP_KBD_PAIRED;
            le_auto_start(); /* wait for it to come back */
            break;
        }
        default:
            break;
    }
}

static void forget_keyboard(void) {
    if (g_cid) hid_host_disconnect(g_cid); /* its events come later, under its own cid */
    g_cid = 0;
    le_auto_stop();
    if (g_le_con != HCI_CON_HANDLE_INVALID) gap_disconnect(g_le_con);
    if (g_have_addr) {
        if (g_le) gap_delete_bonding(g_addr_type, g_addr);
        else gap_drop_link_key_for_bd_addr(g_addr);
    }
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    tlv->delete_tag(ctx, KBD_TAG);
    tlv->delete_tag(ctx, KBD_TAG_LE);
    g_have_addr = false;
    g_le = false;
    g_name[0] = 0;
    g_state = EXP_KBD_NONE;
}

static void on_hid(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) return;
    bd_addr_t addr;
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: /* the bonded keyboard, woken by a key */
            hid_subevent_incoming_connection_get_address(packet, addr);
            if (hid_subevent_incoming_connection_get_status(packet) == ERROR_CODE_SUCCESS && g_have_addr &&
                bd_addr_cmp(addr, g_addr) == 0 && !g_cid)
                hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet), HID_PROTOCOL_MODE_BOOT);
            else
                hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
            break;
        case HID_SUBEVENT_CONNECTION_OPENED: {
            /* Events are matched to their connection: an older one (a
             * reconnection PAIR dropped) failing mustn't fail the pairing. */
            uint16_t cid = hid_subevent_connection_opened_get_hid_cid(packet);
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                if (cid == g_cid) g_cid = 0;
                if (g_pairing && cid == g_pair_cid) pair_failed(3, status);
                else if (g_pairing && g_pair_retry) pair_connect();
                break;
            }
            if (g_pairing && cid != g_pair_cid) { /* not the one being paired */
                hid_host_disconnect(cid);
                break;
            }
            g_cid = cid;
            hid_subevent_connection_opened_get_bd_addr(packet, g_addr);
            if (g_pairing || !g_have_addr) {
                g_have_addr = true;
                save_keyboard(); /* core1 writes it to flash (bt_store_commit) */
            }
            g_pairing = false;
            g_state = EXP_KBD_CONNECTED;
            break;
        }
        case HID_SUBEVENT_CONNECTION_CLOSED: {
            uint16_t cid = hid_subevent_connection_closed_get_hid_cid(packet);
            /* A keyboard switched off (or out of range) with a key down
             * never sends that key's release: an empty report does. */
            uint8_t next = (uint8_t)((g_ring_head + 1) % RING);
            if (next != g_ring_tail) {
                memset(g_ring[g_ring_head], 0, 8);
                __dmb();
                g_ring_head = next;
            }
            if (cid == g_cid) {
                g_cid = 0;
                if (g_state == EXP_KBD_CONNECTED) g_state = g_have_addr ? EXP_KBD_PAIRED : EXP_KBD_NONE;
            }
            if (g_pairing && cid == g_pair_cid) pair_failed(3, 0);
            else if (g_pairing && g_pair_retry) pair_connect();
            break;
        }
        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            g_diag_protocol = (uint8_t)(hid_subevent_set_protocol_response_get_handshake_status(packet) << 4 |
                                        hid_subevent_set_protocol_response_get_protocol_mode(packet));
            break;
        case HID_SUBEVENT_REPORT: {
            /* Input: A1H (DATA, input), then the boot report's 8 bytes -- or,
             * from a keyboard that stayed in report protocol (refused or
             * ignored SET_PROTOCOL), its report ID first: keyboards put the
             * same 8 bytes under ID 1. */
            const uint8_t *r = hid_subevent_report_get_report(packet);
            uint16_t len = hid_subevent_report_get_report_len(packet);
            g_diag_reports++;
            g_diag_len = (uint8_t)(len > 255 ? 255 : len);
            memcpy(g_diag_head, r, len < 4 ? len : 4);
            const uint8_t *keys;
            if (len == 9 && r[0] == 0xA1) keys = r + 1;
            else if (len >= 10 && r[0] == 0xA1 && r[1] == 0x01) keys = r + 2;
            else break;
            uint8_t next = (uint8_t)((g_ring_head + 1) % RING);
            if (next == g_ring_tail) break; /* the loop has fallen behind: drop it */
            memcpy(g_ring[g_ring_head], keys, 8);
            __dmb();
            g_ring_head = next;
            break;
        }
        default:
            break;
    }
}

static void on_hci(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    le_on_event(packet); /* a BLE keyboard's (security events come here too) */
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE: /* up: try the bonded keyboard (it may be asleep -- then it calls us) */
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
                g_le_auto = false; /* the stack's own state went with it */
                g_le_con = HCI_CON_HANDLE_INVALID;
                g_hids_cid = 0;
                break;
            }
            if (!kbd_host_wanted()) break;
            gap_connectable_control(1);
            if (g_le) le_auto_start(); /* a BLE keyboard comes back when we connect to it */
            else if (g_have_addr && !g_cid && hid_host_connect(g_addr, HID_PROTOCOL_MODE_BOOT, &g_cid) != ERROR_CODE_SUCCESS)
                g_cid = 0;
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            if (g_state != EXP_KBD_SEARCHING) break;
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            if ((cod & 0x1F00u) != COD_MAJOR_PERIPHERAL || !(cod & COD_MINOR_KEYBOARD)) break;
            gap_event_inquiry_result_get_bd_addr(packet, g_addr);
            strcpy(g_name, "KEYBOARD");
            if (gap_event_inquiry_result_get_name_available(packet)) {
                uint8_t n = gap_event_inquiry_result_get_name_len(packet);
                if (n > EXP_KBD_NAME_MAX) n = EXP_KBD_NAME_MAX;
                memcpy(g_name, gap_event_inquiry_result_get_name(packet), n);
                g_name[n] = 0;
            }
            gap_inquiry_stop();
            if (g_le_scanning) gap_stop_scan();
            g_le_scanning = false;
            g_le = false;
            g_state = EXP_KBD_CONNECTING;
            connect_timer_start();
            pair_connect();
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE: /* the search's end, for both kinds */
            if (g_state == EXP_KBD_SEARCHING) {
                if (g_le_scanning) gap_stop_scan();
                g_le_scanning = false;
                g_state = EXP_KBD_NOT_FOUND;
                g_pairing = false;
            }
            break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION: /* SSP: type this on the keyboard */
            snprintf(g_code, sizeof g_code, "%06lu",
                     (unsigned long)hci_event_user_passkey_notification_get_numeric_value(packet));
            g_state = EXP_KBD_CODE;
            break;
        case HCI_EVENT_PIN_CODE_REQUEST: /* legacy pairing: a PIN of ours to type */
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            snprintf(g_code, sizeof g_code, "%06lu", (unsigned long)(get_rand_32() % 1000000u));
            gap_pin_code_response(addr, g_code);
            g_state = EXP_KBD_CODE;
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: /* Just Works, for a keyboard that asks no code */
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            gap_ssp_confirmation_response(addr);
            break;
        case GAP_EVENT_PAIRING_COMPLETE:
            if (gap_event_pairing_complete_get_status(packet) != ERROR_CODE_SUCCESS && g_pairing)
                pair_failed(4, gap_event_pairing_complete_get_status(packet));
            break;
        default:
            break;
    }
}

void kbd_host_stack_init(void) {
    hid_host_init(g_descriptors, sizeof g_descriptors);
    hid_host_register_packet_handler(on_hid);
    hids_host_init(g_hids_descriptors, sizeof g_hids_descriptors);
    g_hci_cb.callback = &on_hci;
    hci_add_event_handler(&g_hci_cb);
    g_sm_cb.callback = &on_hci; /* a BLE keyboard's security events */
    sm_add_event_handler(&g_sm_cb);
    g_le_sm_display = false; /* ble_link_poll()'s sm_init() set it up for the Link */
    /* With ENABLE_LE_SECURE_CONNECTIONS, sm_init() makes the security
     * manager Secure Connections only, and wants 16-byte keys: it refused a
     * keyboard without SC itself, reason 03 (2026-10-06, the KHB030). SC is
     * still asked for first. */
    sm_set_secure_connections_only_mode(false);
    sm_set_encryption_key_size_range(7, 16);
    hci_dump_init(&kDump); /* the keyboard's pairing messages, for the MCU log */
    btstack_run_loop_remove_timer(&g_le_timer);
    btstack_run_loop_set_timer_handler(&g_le_timer, le_tick);
    btstack_run_loop_set_timer(&g_le_timer, LE_TICK_MS);
    btstack_run_loop_add_timer(&g_le_timer);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_set_local_name(mcu_config_get_hostname());
    gap_set_class_of_device(0x000100u); /* computer, uncategorised */
    load_keyboard();
}

void kbd_host_publish(uint8_t *window) {
    static bool on;
    if (!kbd_host_wanted()) {
        if (on) { /* MCONF BLKBD=0: let go of everything -- not a key left held */
            on = false;
            g_ring_tail = g_ring_head;
            kbd_seq_clear(&g_seq);
            window[EXP_KBD_KEY] = 0;
        }
        return;
    }
    on = true;
    g_seq.layout = kbd_seq_layout_for_country(mcu_config_get(MCU_CONFIG_KBDLAYOUT));
    uint32_t now = to_ms_since_boot(get_absolute_time());
    while (g_ring_tail != g_ring_head) {
        __dmb();
        kbd_seq_report(&g_seq, g_ring[g_ring_tail], now);
        g_ring_tail = (uint8_t)((g_ring_tail + 1) % RING);
    }
    window[EXP_KBD_KEY] = kbd_seq_key(&g_seq, now);
    window[EXP_KBD_BREAK] = g_seq.break_count;
}

/* ---- core1 ---- */

/* 1 once a search has started (or failed to); 0 while BTstack isn't up. */
static uint32_t do_pair(void *param) {
    (void)param;
    if (hci_get_state() != HCI_STATE_WORKING) return 0;
    forget_keyboard(); /* one keyboard at a time: a new one replaces it */
    g_pairing = true;
    g_pair_cid = 0;
    g_pair_retry = false;
    g_state = EXP_KBD_SEARCHING;
    int status = gap_inquiry_start(SEARCH_UNITS); /* its end ends the LE scan too */
    if (status != ERROR_CODE_SUCCESS) {
        pair_failed(1, (uint8_t)status);
        return 1;
    }
    gap_set_scan_parameters(1 /* active: names come in scan responses */, LE_SCAN_INTERVAL, LE_SCAN_WINDOW);
    gap_start_scan();
    g_le_scanning = true;
    return 1;
}

static uint32_t do_stop(void *param) {
    (void)param;
    if (g_state == EXP_KBD_SEARCHING) gap_inquiry_stop();
    if (g_le_scanning) gap_stop_scan();
    g_le_scanning = false;
    if (g_pairing && g_cid) hid_host_disconnect(g_cid);
    if (g_pairing && g_le) {
        if (g_le_con != HCI_CON_HANDLE_INVALID) gap_disconnect(g_le_con);
        else gap_connect_cancel();
        le_sm_for_keyboard(false);
    }
    if (g_pairing) g_state = EXP_KBD_NONE;
    g_pairing = false;
    return 0;
}

static uint32_t do_forget(void *param) {
    (void)param;
    forget_keyboard();
    return 0;
}

static uint32_t do_status(void *param) {
    uint8_t *w = param;
    uint8_t n = (uint8_t)strlen(g_name);
    w[0] = g_state;
    w[1] = g_state == EXP_KBD_CODE ? 6 : 0;
    memcpy(w + 2, g_code, 6);
    w[8] = n;
    memcpy(w + 9, g_name, n);
    w[25] = (uint8_t)(g_diag_reports >> 8);
    w[26] = (uint8_t)g_diag_reports;
    w[27] = g_diag_len;
    memcpy(w + 28, g_diag_head, 4);
    w[32] = g_diag_protocol;
    w[33] = g_diag_fail_step;
    w[34] = g_diag_fail_status;
    w[35] = g_le ? KIND_LE : KIND_CLASSIC;
    return 0;
}

/* For STATUS and STOP, which only read or reset this file's own state:
 * through BTstack's context while the CYW43 is up, else straight in -- with
 * the radio down (MCONF BLKBD=0, after a sleep) its async context isn't
 * running, and a call into it waited until monitor.c's 30s command
 * timeout (2026-10-06, BLKBD FORGET). Nothing on core0 touches this state
 * then. */
static uint32_t on_stack(uint32_t (*fn)(void *), void *param) {
    return g_cyw43_up ? async_context_execute_sync(cyw43_arch_async_context(), fn, param) : fn(param);
}

uint8_t kbd_host_command(uint8_t command, uint8_t *window) {
    async_context_t *ctx = cyw43_arch_async_context();
    trace_to_log();
    bt_store_commit(); /* a bond made since the last command (BLKBD polls this) */
    switch (command) {
        case EXP_COMMAND_KBD_PAIR:
            /* the radio comes up with MCONF BLKBD (ble_link_wanted()) */
            if (!kbd_host_wanted()) return EXP_STATUS_ERROR;
            for (int i = 0; i < 100; i++) { /* up to 5 s for it to come up */
                if (async_context_execute_sync(ctx, do_pair, NULL)) return EXP_STATUS_SUCCESS;
                sleep_ms(50);
            }
            return EXP_STATUS_ERROR;
        case EXP_COMMAND_KBD_STATUS:
            on_stack(do_status, window);
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_KBD_STOP:
            on_stack(do_stop, NULL);
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_KBD_FORGET: {
            /* the link key and the keyboard's record are BTstack's: it has
             * to be running, which with MCONF BLKBD=0 it may not be */
            bool up = ble_link_stack_acquire();
            if (up) {
                async_context_execute_sync(cyw43_arch_async_context(), do_forget, NULL);
                bt_store_commit(); /* the dropped bond, to flash now */
            }
            ble_link_stack_release();
            return up ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
        }
        default:
            return EXP_STATUS_ERROR;
    }
}
