/* zterm_session.c -- see zterm_session.h.
 *
 * Flash (mcu_store.c slot EXP_STORE_SLOT_ZTERM, flash_layout.h): the first
 * sector holds session records, a 256-byte page each, written in turn and
 * the sector erased when they've filled it; the newest (highest seq) is
 * the one that counts:
 *   ["ZT1"][seq, 4 LE][begin, 4 LE][end, 4 LE][zterm_t][path len][path]
 * The rest is the transcript ring. Position p of the transcript is at ring
 * offset p % RING; the page being filled stays in RAM until it's full, and
 * a page at a sector's start erases that sector first, which forgets the
 * ring's oldest sector (begin moves up). An OPEN starts again at 0 without
 * erasing anything: what's beyond the new end is never read. */
#include "zterm_session.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "mcu_log.h"
#include "mcu_store.h"
#include "pc_exp.h"
#include "pico/rand.h"
#include "zterm.h"

#define PAGE 256
#define RING (FLASH_ZTERM_SIZE - FLASH_SECTOR_SIZE)
#define RECORDS (FLASH_SECTOR_SIZE / PAGE)
#define REC_HEADER 15
#define PATH_MAX_LEN (PAGE - REC_HEADER - (int)sizeof(zterm_t) - 1)

_Static_assert(PATH_MAX_LEN >= 64, "zterm_t leaves too little of a record page for the story's path");

static zterm_t g_zt;
static uint32_t g_begin, g_end;
static uint8_t g_page[PAGE]; /* positions g_end & ~255 .. g_end */
static char g_story[PATH_MAX_LEN + 1];
static bool g_recorded_open; /* the newest record in flash says a game is open */
static bool g_dirty;         /* a command since the last record */
static uint32_t g_flushed;   /* the transcript is in flash up to here */

/* ---- the transcript ---- */

static void ring_reset(void *ctx) {
    (void)ctx;
    g_begin = g_end = g_flushed = 0;
    memset(g_page, 0xFF, sizeof g_page);
}

/* The page at position pos (a multiple of 256) to flash. */
static void ring_program(uint32_t pos) {
    uint32_t at = FLASH_SECTOR_SIZE + pos % RING;
    if (at % FLASH_SECTOR_SIZE == 0) {
        if (!mcu_store_erase_range(EXP_STORE_SLOT_ZTERM, at, FLASH_SECTOR_SIZE)) mcu_log_warn("ZTERM erase failed");
        if (pos >= RING && pos - RING + FLASH_SECTOR_SIZE > g_begin) g_begin = pos - RING + FLASH_SECTOR_SIZE;
    }
    if (!mcu_store_write(EXP_STORE_SLOT_ZTERM, at, g_page, PAGE)) mcu_log_warn("ZTERM write failed");
}

static void ring_append(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    for (uint16_t i = 0; i < len; i++) {
        g_page[g_end % PAGE] = data[i];
        g_end++;
        if (g_end % PAGE == 0) {
            ring_program(g_end - PAGE);
            memset(g_page, 0xFF, sizeof g_page);
            g_flushed = g_end;
        }
    }
}

static uint32_t ring_begin(void *ctx) {
    (void)ctx;
    return g_begin;
}

static uint32_t ring_end(void *ctx) {
    (void)ctx;
    return g_end;
}

static uint8_t ring_byte(void *ctx, uint32_t pos) {
    (void)ctx;
    if (pos >= g_end) return 0;
    if (pos >= g_end - g_end % PAGE) return g_page[pos % PAGE];
    return *(const uint8_t *)(XIP_BASE + FLASH_ZTERM_OFFSET + FLASH_SECTOR_SIZE + pos % RING);
}

static uint16_t ring_random16(void *ctx) {
    (void)ctx;
    return (uint16_t)get_rand_32();
}

static const zt_store_t kStore = {NULL, ring_reset, ring_append, ring_begin, ring_end, ring_byte, ring_random16};

/* ---- session records ---- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The newest record: its slot (or -1) and seq. */
static int newest_record(uint32_t *seq) {
    int best = -1;
    for (int i = 0; i < RECORDS; i++) {
        uint8_t h[REC_HEADER];
        if (!mcu_store_read(EXP_STORE_SLOT_ZTERM, (uint32_t)i * PAGE, h, sizeof h)) continue;
        if (h[0] != 'Z' || h[1] != 'T' || h[2] != '1') continue;
        uint32_t s = get32(h + 3);
        if (best < 0 || s > *seq) {
            best = i;
            *seq = s;
        }
    }
    return best;
}

static void write_record(void) {
    uint32_t seq = 0;
    int last = newest_record(&seq);
    int slot = last < 0 ? 0 : (last + 1) % RECORDS;
    if (slot == 0 && !mcu_store_erase_range(EXP_STORE_SLOT_ZTERM, 0, FLASH_SECTOR_SIZE)) {
        mcu_log_warn("ZTERM record erase failed");
        return;
    }
    uint8_t rec[PAGE];
    memset(rec, 0xFF, sizeof rec);
    memcpy(rec, "ZT1", 3);
    put32(rec + 3, seq + 1);
    put32(rec + 7, g_begin);
    put32(rec + 11, g_end);
    memcpy(rec + REC_HEADER, &g_zt, sizeof g_zt);
    uint8_t n = (uint8_t)strlen(g_story);
    rec[REC_HEADER + sizeof g_zt] = n;
    memcpy(rec + REC_HEADER + sizeof g_zt + 1, g_story, n);
    if (!mcu_store_write(EXP_STORE_SLOT_ZTERM, (uint32_t)slot * PAGE, rec, PAGE)) mcu_log_warn("ZTERM record failed");
    g_recorded_open = g_zt.open;
}

/* ---- the console ---- */

uint8_t zterm_session_command(uint8_t *window, const char *story_path) {
    if (window[0] == EXP_ZTERM_OPEN) {
        strncpy(g_story, story_path ? story_path : "", PATH_MAX_LEN);
        g_story[PATH_MAX_LEN] = 0;
        if (story_path && strlen(story_path) > PATH_MAX_LEN) {
            g_story[0] = 0;
            mcu_log_warn("ZTERM: story path too long for POWMAN");
        }
    }
    g_dirty = true;
    return zterm_command(&g_zt, &kStore, window);
}

/* Writes only what changed: with MCONF POWMANDELAY=0 this runs after
 * every key. */
void zterm_session_suspend(void) {
    if (!g_dirty || (!g_zt.open && !g_recorded_open)) return;
    if (g_zt.open && g_end > g_flushed) { /* the partial page: programmed again, whole, once it fills */
        ring_program(g_end - g_end % PAGE);
        g_flushed = g_end;
    }
    write_record();
    g_dirty = false;
}

bool zterm_session_resume(char *path, size_t path_size, uint8_t *channel) {
    uint32_t seq = 0;
    int slot = newest_record(&seq);
    uint8_t rec[PAGE];
    if (slot < 0 || !mcu_store_read(EXP_STORE_SLOT_ZTERM, (uint32_t)slot * PAGE, rec, PAGE)) return false;
    memcpy(&g_zt, rec + REC_HEADER, sizeof g_zt);
    g_recorded_open = g_zt.open;
    if (!g_zt.open) return false;
    g_begin = get32(rec + 7);
    g_end = get32(rec + 11);
    uint8_t n = rec[REC_HEADER + sizeof g_zt];
    if (n > PATH_MAX_LEN) n = 0;
    memcpy(g_story, rec + REC_HEADER + sizeof g_zt + 1, n);
    g_story[n] = 0;
    memset(g_page, 0xFF, sizeof g_page); /* the partial page, from its flash copy */
    uint32_t from = g_end - g_end % PAGE;
    if (g_end % PAGE)
        mcu_store_read(EXP_STORE_SLOT_ZTERM, FLASH_SECTOR_SIZE + from % RING, g_page, g_end % PAGE);
    g_flushed = g_end;
    g_dirty = false;
    *channel = g_zt.story_channel;
    strncpy(path, g_story, path_size - 1);
    path[path_size - 1] = 0;
    return path[0] != 0;
}
