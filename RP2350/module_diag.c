/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* module_diag.c -- see module_diag.h. The ring's layout and entry types are
 * the diagnostic module build's diag.h (kept outside the repo):
 *
 *   +0  magic "MDG1"   +4 its own address   +8 cap | entry size << 16
 *   +12 seq (entries ever written)   +16 boots   +20..31 reserved
 *   +32 entries, 16 bytes each: t_ms, type | a << 8 | b << 16, c, d
 *
 * Each entry becomes one or more MCU log lines (23 characters at most),
 * each starting "M<seconds since the module booted>". */
#include "module_diag.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mcu_log.h"
#include "module_swd.h"

#define RING_MAGIC 0x3147444Du
#define RAM_BASE 0x20000000u
#define RAM_WORDS (0x8000u / 4u)
#define MAX_CAP 512u

enum {
    DG_BOOT = 1, DG_XO, DG_VOLT, DG_CNT, DG_OPEN, DG_PARAMS, DG_PHY, DG_CLOSE, DG_STATS1,
    DG_STATS2, DG_GATT, DG_SYSERR, DG_CONNECT, DG_ABORT, DG_SCAN, DG_ADV, DG_EXHAUST, DG_SECURITY,
};

/* What was copied last time, so a second MLOG MODULE adds only new ones. */
static uint32_t g_ring_addr, g_boots, g_next_seq;

/* ---- lines of at most MCU_LOG_MSG_MAX, wrapped at token boundaries ---- */

static char g_line[MCU_LOG_MSG_MAX + 1];
static size_t g_len, g_pre;

static void line_flush(void) {
    if (g_len > g_pre) mcu_log_user(g_line);
    g_len = g_pre;
    g_line[g_len] = 0;
}

static void tok(const char *fmt, ...) {
    char t[MCU_LOG_MSG_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    size_t n = strlen(t);
    if (g_len > g_pre && g_len + 1 + n > MCU_LOG_MSG_MAX) line_flush();
    if (g_len > g_pre && g_len < MCU_LOG_MSG_MAX) g_line[g_len++] = ' ';
    size_t room = MCU_LOG_MSG_MAX - g_len;
    if (n > room) n = room;
    memcpy(g_line + g_len, t, n);
    g_len += n;
    g_line[g_len] = 0;
}

static void line_start(uint32_t t_ms) {
    g_len = (size_t)snprintf(g_line, sizeof g_line, "M%lu.%lu ", (unsigned long)(t_ms / 1000u),
                             (unsigned long)(t_ms / 100u % 10u));
    g_pre = g_len;
}

static void addr_tok(uint32_t lo, uint32_t hi) {
    tok("%02X%02X%02X%02X%02X%02X", (unsigned)(hi >> 8 & 0xFF), (unsigned)(hi & 0xFF), (unsigned)(lo >> 24),
        (unsigned)(lo >> 16 & 0xFF), (unsigned)(lo >> 8 & 0xFF), (unsigned)(lo & 0xFF));
}

static void format(const uint32_t w[4]) {
    uint8_t type = (uint8_t)w[1], a = (uint8_t)(w[1] >> 8);
    uint16_t b = (uint16_t)(w[1] >> 16);
    uint32_t c = w[2], d = w[3];
    line_start(w[0]);
    switch (type) {
        case DG_BOOT:
            tok("BOOT%u", b);
            tok("rc%08lX", (unsigned long)c);
            if (a) tok("kept");
            break;
        case DG_XO:
            tok("XO%u/%lu", b, (unsigned long)c);
            tok("P%d", (int)(int16_t)d);
            break;
        case DG_VOLT:
            tok("A%u-%u", b, (unsigned)(c & 0xFFFF));
            tok("D%u-%u", (unsigned)(c >> 16), (unsigned)(d & 0xFFFF));
            tok("c%u", (unsigned)(d >> 16));
            tok("n%u", a * 16u);
            break;
        case DG_CNT:
            tok("t%u", b);
            tok("r%u", (unsigned)(c & 0xFFFF));
            tok("e%u", (unsigned)(c >> 16));
            tok("f%lu", (unsigned long)d);
            if (a != 127) tok("R%d", (int)(int8_t)a);
            break;
        case DG_OPEN:
            tok("OPEN%u", a);
            tok((b & 0xFF) ? "ctr" : "per");
            tok("at%u", b >> 8);
            addr_tok(c, d);
            break;
        case DG_PARAMS:
            tok("PRM%u", a);
            tok("i%u", b);
            tok("l%u", (unsigned)(c & 0xFFFF));
            tok("to%u", (unsigned)(c >> 16));
            tok("s%lu", (unsigned long)d);
            break;
        case DG_PHY:
            tok("PHY%u=%u", a, b);
            break;
        case DG_CLOSE:
            tok("CLOSE%u", a);
            tok("%04X", b);
            break;
        case DG_STATS1:
            tok("ST%u", a);
            tok("R%d/%d", (int)(int8_t)(b & 0xFF), (int)(int8_t)(b >> 8));
            tok("n%lu", (unsigned long)c);
            tok("m%lu", (unsigned long)d);
            break;
        case DG_STATS2:
            tok("ST%u", a);
            tok("ok%lu", (unsigned long)c);
            tok("crc%lu", (unsigned long)d);
            break;
        case DG_GATT:
            tok("GATT%u", a);
            tok("%04X", b);
            break;
        case DG_SYSERR:
            tok("SYSERR");
            tok("%04X", b);
            break;
        case DG_CONNECT:
            tok("CONN");
            tok("at%u", a);
            tok("%ums", b);
            addr_tok(c, d);
            break;
        case DG_ABORT:
            tok("ABORT");
            tok("%04X", b);
            tok("st%lu", (unsigned long)c);
            break;
        case DG_SCAN:
            if (a) tok("SCAN %ums", b);
            else tok("SCANEND n%u", b);
            break;
        case DG_ADV:
            tok("ADV");
            break;
        case DG_EXHAUST:
            tok("EXH");
            tok("b%u/%u", b & 0xFF, b >> 8);
            tok("h%u", (unsigned)(c & 0xFFFF));
            tok("m%u", (unsigned)(c >> 16));
            break;
        case DG_SECURITY:
            tok("BONDFAIL%u", a);
            tok("%04X", b);
            break;
        default:
            tok("T%u a%u", type, a);
            tok("%04X", b);
            tok("%08lX", (unsigned long)c);
            tok("%08lX", (unsigned long)d);
            break;
    }
    line_flush();
}

/* The ring: the magic followed by its own address, anywhere in RAM. */
static bool find_ring(uint32_t *addr) {
    static uint32_t buf[256];
    for (uint32_t base = 0; base < RAM_WORDS; base += 256u) {
        if (!module_swd_read_block(RAM_BASE + 4u * base, buf, 256u)) return false;
        for (uint32_t i = 0; i < 256u; i++) {
            if (buf[i] != RING_MAGIC) continue;
            uint32_t at = RAM_BASE + 4u * (base + i), self = 0;
            if (i + 1 < 256u ? (self = buf[i + 1], true) : module_swd_read32(at + 4u, &self)) {
                if (self == at) {
                    *addr = at;
                    return true;
                }
            }
        }
    }
    *addr = 0;
    return true;
}

static uint8_t copy(uint16_t *copied, uint16_t *lost) {
    uint32_t at, h[5];
    if (!find_ring(&at)) return MODULE_DIAG_READ_FAILED;
    if (!at) return MODULE_DIAG_NO_RING;
    if (!module_swd_read_block(at, h, 5)) return MODULE_DIAG_READ_FAILED;
    uint32_t cap = h[2] & 0xFFFF, esize = h[2] >> 16, seq = h[3], boots = h[4];
    if (cap == 0 || cap > MAX_CAP || esize != 16) return MODULE_DIAG_NO_RING;

    /* the same ring as last time, not reset since: only what's new */
    uint32_t from = 0;
    if (at == g_ring_addr && boots >= g_boots && seq >= g_next_seq) from = g_next_seq;
    uint32_t oldest = seq > cap ? seq - cap : 0;
    uint32_t lost_n = from < oldest ? oldest - from : 0;
    if (from < oldest) from = oldest;

    char hdr[MCU_LOG_MSG_MAX + 1];
    snprintf(hdr, sizeof hdr, "MOD boot%lu %lu-%lu", (unsigned long)boots, (unsigned long)from,
             (unsigned long)seq);
    mcu_log_user(hdr);
    uint32_t n = 0;
    for (uint32_t s = from; s < seq; s++) {
        uint32_t w[4];
        if (!module_swd_read_block(at + 32u + 16u * (s % cap), w, 4)) return MODULE_DIAG_READ_FAILED;
        format(w);
        n++;
    }
    /* entries the module wrote over while we read */
    uint32_t seq2 = seq;
    if (module_swd_read32(at + 12u, &seq2) && seq2 > cap && seq2 - cap > from) {
        snprintf(hdr, sizeof hdr, "MOD overrun %lu", (unsigned long)(seq2 - cap - from));
        mcu_log_user(hdr);
    }
    g_ring_addr = at;
    g_boots = boots;
    g_next_seq = seq;
    *copied = (uint16_t)(n > 0xFFFF ? 0xFFFF : n);
    *lost = (uint16_t)(lost_n > 0xFFFF ? 0xFFFF : lost_n);
    return MODULE_DIAG_OK;
}

uint8_t module_diag_copy(uint16_t *copied, uint16_t *lost) {
    *copied = *lost = 0;
    bool halted = false;
    if (!module_swd_attach(500)) {
        /* Not answering while it runs (2026-11-17, the first card): reset
         * it and halt it straight after -- the ring is kept over a pin
         * reset -- then let it run again. */
        mcu_log_user("MOD: reset to read");
        halted = module_swd_reset_halt(2000);
        if (!halted) {
            module_swd_detach();
            mcu_log_user("MOD: no SWD");
            return MODULE_DIAG_NO_SWD;
        }
    }
    uint8_t r = copy(copied, lost);
    if (halted) module_swd_resume();
    module_swd_detach();
    if (r == MODULE_DIAG_NO_RING) mcu_log_user("MOD: no ring");
    if (r == MODULE_DIAG_READ_FAILED) mcu_log_user("MOD: read failed");
    return r;
}
