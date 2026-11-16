/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* debug_fwd.c -- see debug_fwd.h. */
#include "debug_fwd.h"

#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "hardware/sync.h"
#include "mcu_config.h"
#include "pico/stdio/driver.h"

/* A power of two. 16 KB (2026-11-16; 4 KB before): the text only goes out
 * between commands, so one command's output has to fit -- an SD card's
 * start-up trace alone filled 4 KB. */
#define RING_N 16384u

static uint8_t g_ring[RING_N];
static volatile uint32_t g_wr, g_rd; /* free-running; g_wr - g_rd bytes waiting */
static volatile uint32_t g_dropped;  /* bytes that didn't fit since the last report */
static volatile uint32_t g_gap_at;   /* where they'd have gone: g_wr when the first didn't fit */
static spin_lock_t *g_lock;
static bool g_was_on;                /* core1: BLDEBUG as the last poll saw it */
static bool g_line_open;             /* core1: the last byte sent wasn't a line end */

static bool on(void) { return mcu_config_get(MCU_CONFIG_BLDEBUG) != 0; }

/* Either core, any context (printf from an IRQ too). */
static void put(const char *p, int n) {
    if (!g_lock || !on()) return;
    uint32_t save = spin_lock_blocking(g_lock);
    for (int i = 0; i < n; i++) {
        if (g_wr - g_rd >= RING_N) {
            /* One notice per gap; drops at a later gap before the first is
             * reported are counted into it. */
            if (g_dropped == 0) g_gap_at = g_wr;
            g_dropped += (uint32_t)(n - i);
            break;
        }
        g_ring[g_wr++ & (RING_N - 1u)] = (uint8_t)p[i];
    }
    spin_unlock(g_lock, save);
}

static void out_chars(const char *buf, int len) { put(buf, len); }

static stdio_driver_t g_driver = {
    .out_chars = out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false,
#endif
};

void debug_fwd_init(void) {
    g_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
    stdio_set_driver_enabled(&g_driver, true);
}

void debug_fwd_mlog(char level, const char *msg) {
    char line[48];
    int n = snprintf(line, sizeof line, "MLOG %c: %s\n", level, msg ? msg : "");
    if (n > (int)sizeof line - 1) {
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    if (n > 0) put(line, n);
}

static void drop_all(void) {
    uint32_t save = spin_lock_blocking(g_lock);
    g_rd = g_wr;
    g_dropped = 0;
    spin_unlock(g_lock, save);
}

bool debug_fwd_pending(void) {
    return g_lock && on() && ble_link_session() && !ble_link_transfer_open() && g_wr != g_rd;
}

bool debug_fwd_poll(void) {
    uint8_t frame[256];
    uint32_t save, n, i, reported = 0;
    uint16_t max;
    int r;
    if (!g_lock) return false;
    if (!on()) {
        if (g_was_on) drop_all();
        g_was_on = false;
        return false;
    }
    if (!g_was_on) { /* turned on: nothing from before */
        drop_all();
        g_was_on = true;
    }
    if (!ble_link_session()) return false; /* kept for when a link comes, up to the ring's size */
    /* Not in the middle of a BLSAVE/BLLOAD: the server may be waiting on
     * our ACK of its FILE_DATA, and answers nothing else until it has it. */
    if (ble_link_transfer_open()) return false;
    max = ble_link_display_max();
    if (max > sizeof frame) max = sizeof frame;
    save = spin_lock_blocking(g_lock);
    n = 0;
    /* The text up to the gap, then the notice there, on a line of its own
     * (2026-11-16: it went first, as if the text after it were what was
     * lost). The rest goes in the next frame. */
    uint32_t upto = g_dropped ? g_gap_at - g_rd : g_wr - g_rd;
    for (i = 0; n < max && i < upto; i++) frame[n++] = g_ring[(g_rd + i) & (RING_N - 1u)];
    if (g_dropped && i == upto) {
        bool open = n ? frame[n - 1] != '\n' : g_line_open;
        char note[40];
        int k = snprintf(note, sizeof note, "%s[%lu bytes dropped]\n", open ? "\n" : "", (unsigned long)g_dropped);
        if (k > 0 && n + (uint32_t)k <= max) {
            memcpy(frame + n, note, (size_t)k);
            n += (uint32_t)k;
            reported = g_dropped;
        }
    }
    spin_unlock(g_lock, save);
    if (n == 0) {
        /* Nothing to say, but the peer is asked once per link whether it
         * takes any, so the first line goes out without a round trip. */
        ble_link_debug(NULL, 0);
        return false;
    }
    /* The send prints nothing itself; anything another core prints
     * meanwhile lands after what's being sent. */
    r = ble_link_debug(frame, (uint16_t)n);
    if (r < 0) return false; /* try again on the next poll */
    save = spin_lock_blocking(g_lock);
    g_rd += i;
    if (reported) {
        g_dropped -= reported;
        /* dropped meanwhile: still to be told, at the next gap or here */
        if (g_dropped && (uint32_t)(g_gap_at - g_rd) > g_wr - g_rd) g_gap_at = g_rd;
    }
    spin_unlock(g_lock, save);
    g_line_open = frame[n - 1] != '\n';
    return r > 0;
}
