/* kbd_host_module.c -- kbd_host.h on the internal card (2026-10-13): the
 * BLE keyboard host is the BGM220S's own (BGM220/kbd.c, MODULE_PROTOCOL.md
 * KBD_*); this side turns BLKBD's commands into module requests, and the
 * module's 8-byte boot reports (KBD_REPORT) into the same key sequencer and
 * window bytes the dongle's kbd_host.c feeds (kbd_seq.h). BLE keyboards
 * only: the BGM220S has no classic Bluetooth.
 *
 * Threading: the reports come from core1 (module_pump(), ble_link.c) into a
 * small ring; kbd_host_publish() on core0 takes them, as on the dongle. */
#include "kbd_host.h"
#include "kbd_host_module.h"

#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "hardware/sync.h"
#include "kbd_seq.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "module_link.h"
#include "pc_exp.h"
#include "pico/time.h"

enum { RQ_KBD_PAIR = 0x20, RQ_KBD_STOP = 0x21, RQ_KBD_FORGET = 0x22, RQ_KBD_ENABLE = 0x23, RQ_KBD_STATUS = 0x24 };
enum { EV_KBD_STATE = 0x10, EV_KBD_PASSKEY = 0x11, EV_KBD_REPORT = 0x12 };
#define PAIR_SEARCH_S 15 /* about the dongle's (10 x 1.28 s inquiry) */

static volatile uint8_t g_state = EXP_KBD_NONE;
static char g_code[7];
static char g_name[EXP_KBD_NAME_MAX + 1];
static uint16_t g_reports;
static uint8_t g_last[4];

#define RING 16
static uint8_t g_ring[RING][8];
static volatile uint8_t g_ring_head, g_ring_tail; /* core1 writes head, core0 tail */
static kbd_seq_t g_seq;                           /* core0's */
static volatile uint8_t g_key_now;                /* the key core0 last published */

static bool g_enable_sent, g_enable_val;

bool kbd_host_wanted(void) { return mcu_config_get(MCU_CONFIG_BLKBD) != 0; }
void kbd_host_stack_init(void) {}
void kbd_host_le_yield(void) {}
void kbd_host_trace_to_log(void) {}

/* ---- core1: the module's events ---- */

void kbd_module_event(uint8_t ev, const uint8_t *p, uint16_t n) {
    switch (ev) {
        case EV_KBD_STATE:
            if (n < 1) break;
            g_state = p[0];
            if (g_state != EXP_KBD_CODE) memset(g_code, 0, sizeof g_code);
            break;
        case EV_KBD_PASSKEY:
            if (n < 4) break;
            snprintf(g_code, sizeof g_code, "%06lu",
                     (unsigned long)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) %
                         1000000ul);
            g_state = EXP_KBD_CODE;
            break;
        case EV_KBD_REPORT: {
            if (n < 8) break;
            uint8_t next = (uint8_t)((g_ring_head + 1) % RING);
            g_reports++;
            memcpy(g_last, p, 4);
            if (next == g_ring_tail) break; /* core0 has fallen behind: drop it */
            memcpy(g_ring[g_ring_head], p, 8);
            __dmb();
            g_ring_head = next;
            break;
        }
        default:
            break;
    }
}

bool kbd_module_needs_sync(void) { return !g_enable_sent || g_enable_val != kbd_host_wanted(); }

void kbd_module_restarted(void) { g_enable_sent = false; }

void kbd_module_sync(void) {
    uint8_t want = kbd_host_wanted() ? 1 : 0, a[2];
    if (!kbd_module_needs_sync()) return;
    if (module_link_request(RQ_KBD_ENABLE, &want, 1, a, sizeof a) >= 1 && a[0] == 0) {
        g_enable_sent = true;
        g_enable_val = want;
    }
}

/* ---- core0 ---- */

void kbd_host_publish(uint8_t *window) {
    static bool on;
    if (!kbd_host_wanted()) {
        if (on) { /* MCONF BLKBD=0: let go of everything -- not a key left held */
            on = false;
            g_ring_tail = g_ring_head;
            kbd_seq_clear(&g_seq);
            kbd_window_put(window, 0, g_seq.break_count);
            g_key_now = 0;
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
    g_key_now = kbd_seq_key(&g_seq, now);
    kbd_window_put(window, g_key_now, g_seq.break_count);
}

bool kbd_module_busy(void) {
    return g_key_now != 0 || kbd_seq_busy(&g_seq) || g_ring_tail != g_ring_head;
}

/* ---- core1: BLKBD's commands ---- */

static uint8_t request(uint8_t type, const uint8_t *p, size_t len, uint8_t *a, size_t max) {
    int n = module_link_request(type, p, len, a, max);
    if (n >= 1 && a[0] == 0) return EXP_STATUS_SUCCESS;
    char msg[32]; /* "BLKBD module 20 s06": the module's status, "n" for no answer */
    snprintf(msg, sizeof msg, "BLKBD module %02X %s%02X", type, n >= 1 ? "s" : "n", n >= 1 ? a[0] : (uint8_t)n);
    mcu_log_warn(msg);
    return EXP_STATUS_ERROR;
}

uint8_t kbd_host_command(uint8_t command, uint8_t *w) {
    uint8_t a[48], s = PAIR_SEARCH_S;
    uint8_t status;
    switch (command) {
        case EXP_COMMAND_KBD_PAIR:
            if (!kbd_host_wanted()) return EXP_STATUS_ERROR;
            if (!ble_link_stack_acquire()) { /* the module answering */
                mcu_log_warn("BLKBD pair: no module");
                return EXP_STATUS_ERROR;
            }
            kbd_module_sync();
            memset(g_code, 0, sizeof g_code);
            g_name[0] = 0;
            status = request(RQ_KBD_PAIR, &s, 1, a, sizeof a);
            if (status == EXP_STATUS_SUCCESS) g_state = EXP_KBD_SEARCHING;
            ble_link_stack_release();
            return status;
        case EXP_COMMAND_KBD_STATUS: {
            ble_link_module_idle(); /* the latest events first */
            int n = module_link_request(RQ_KBD_STATUS, NULL, 0, a, sizeof a);
            if (n >= 10 && a[0] == 0) { /* [status][state][type][address 6][len][name] */
                uint8_t k = a[9] > EXP_KBD_NAME_MAX ? EXP_KBD_NAME_MAX : a[9];
                g_state = a[1]; /* the module's is the real one */
                if (n >= 10 + k) {
                    memcpy(g_name, a + 10, k);
                    g_name[k] = 0;
                }
            }
            uint8_t len = (uint8_t)strlen(g_name);
            memset(w, 0, 36);
            w[0] = g_state;
            w[1] = g_state == EXP_KBD_CODE ? 6 : 0;
            memcpy(w + 2, g_code, 6);
            w[8] = len;
            memcpy(w + 9, g_name, len);
            w[25] = (uint8_t)(g_reports >> 8);
            w[26] = (uint8_t)g_reports;
            w[27] = 8;
            memcpy(w + 28, g_last, 4);
            w[32] = 0xFF; /* no SET_PROTOCOL on the module: report mode, through the map */
            w[35] = 1;    /* BLE */
            return EXP_STATUS_SUCCESS;
        }
        case EXP_COMMAND_KBD_STOP:
            return request(RQ_KBD_STOP, NULL, 0, a, sizeof a);
        case EXP_COMMAND_KBD_FORGET:
            if (!ble_link_stack_acquire()) return EXP_STATUS_ERROR;
            status = request(RQ_KBD_FORGET, NULL, 0, a, sizeof a);
            ble_link_stack_release();
            if (status == EXP_STATUS_SUCCESS) {
                g_state = EXP_KBD_NONE;
                g_name[0] = 0;
            }
            return status;
        default:
            return EXP_STATUS_ERROR;
    }
}
