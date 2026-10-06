/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* kbd_host.c -- see kbd_host.h.
 *
 * Classic Bluetooth only, for now (2026-10-04: every keyboard to hand is
 * Bluetooth 3.0); BLE keyboards (HOGP) are to follow. Boot protocol, so
 * there's no report descriptor to fetch or parse: every keyboard sends the
 * same 8-byte report, and a combined keyboard and touchpad's mouse reports
 * (a different length) are simply ignored.
 *
 * Pairing: this side says it can display but not input (SSP Display Only)
 * and asks for MITM protection, so a keyboard pairs by Passkey Entry -- the
 * PC-1500 shows six digits, the user types them on the keyboard and presses
 * Enter. A Bluetooth 2.0 keyboard asks for a PIN instead (legacy pairing):
 * the same, with a PIN chosen here. The bond's link key goes to bt_store.c;
 * the keyboard's address and name to the same store, under KBD_TAG. */
#include "kbd_host.h"

#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "bt_store.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "hardware/sync.h"
#include "kbd_seq.h"
#include "mcu_config.h"
#include "monitor.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"

#define KBD_TAG 0x4B424430u /* 'KBD0': [address 6][name length][name] */
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

static void save_keyboard(void) {
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    uint8_t rec[6 + 1 + EXP_KBD_NAME_MAX];
    uint8_t n = (uint8_t)strlen(g_name);
    memcpy(rec, g_addr, 6);
    rec[6] = n;
    memcpy(rec + 7, g_name, n);
    tlv->store_tag(ctx, KBD_TAG, rec, (uint32_t)(7 + n));
}

static void load_keyboard(void) {
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    uint8_t rec[6 + 1 + EXP_KBD_NAME_MAX];
    int len = tlv->get_tag(ctx, KBD_TAG, rec, sizeof rec);
    g_have_addr = len >= 7;
    if (!g_have_addr) return;
    memcpy(g_addr, rec, 6);
    uint8_t n = rec[6] > EXP_KBD_NAME_MAX || 7 + rec[6] > len ? 0 : rec[6];
    memcpy(g_name, rec + 7, n);
    g_name[n] = 0;
    g_state = EXP_KBD_PAIRED;
}

static void forget_keyboard(void) {
    if (g_cid) hid_host_disconnect(g_cid); /* its events come later, under its own cid */
    g_cid = 0;
    if (g_have_addr) gap_drop_link_key_for_bd_addr(g_addr);
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    tlv->delete_tag(ctx, KBD_TAG);
    g_have_addr = false;
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
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE: /* up: try the bonded keyboard (it may be asleep -- then it calls us) */
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING || !kbd_host_wanted()) break;
            gap_connectable_control(1);
            if (g_have_addr && !g_cid && hid_host_connect(g_addr, HID_PROTOCOL_MODE_BOOT, &g_cid) != ERROR_CODE_SUCCESS)
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
            g_state = EXP_KBD_CONNECTING;
            connect_timer_start();
            pair_connect();
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (g_state == EXP_KBD_SEARCHING) {
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
    g_hci_cb.callback = &on_hci;
    hci_add_event_handler(&g_hci_cb);
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
    int status = gap_inquiry_start(SEARCH_UNITS);
    if (status != ERROR_CODE_SUCCESS) pair_failed(1, (uint8_t)status);
    return 1;
}

static uint32_t do_stop(void *param) {
    (void)param;
    if (g_state == EXP_KBD_SEARCHING) gap_inquiry_stop();
    if (g_pairing && g_cid) hid_host_disconnect(g_cid);
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
