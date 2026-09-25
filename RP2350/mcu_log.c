/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_log.c -- see mcu_log.h for the why.
 *
 * STORAGE DESIGN (2026-09-25; replaced a single sector erased and rewritten
 * on every entry): an append-only ring of flash sectors, sized by MCONF
 * LOGSIZE (KB, default 100 -- the planned RP2354B card has only 2MB of
 * flash), ending just below the STSAVE store (flash_layout.h).
 *
 * - Each 4K sector holds a 32-byte header (slot 0) and 127 32-byte records.
 * - A new record is programmed into the next erased slot: only the 256-byte
 *   page holding it is programmed, with 0xFF everywhere else in the page
 *   (NOR flash programming only clears bits, so the bytes already there are
 *   left as they are).
 * - When the newest sector is full, the next sector round the ring is
 *   erased and becomes the newest: the log's oldest 127 records go at once.
 *   So each sector is erased once per LOGSIZE's worth of entries, instead of
 *   once per entry as before.
 * - MLOG RESET appends a CLEAR record instead of erasing (erasing the whole
 *   log would take seconds at larger sizes); reading stops at the newest CLEAR.
 * - A sector belongs to the current log only if its header matches LOGSIZE
 *   and the log generation (MCU_CONFIG_LOGGEN): changing LOGSIZE moves the
 *   ring's start, so it bumps the generation and starts a fresh log.
 * - The ring never grows into the firmware image: its size is clamped at
 *   boot to what fits above the program's end plus a margin for growth.
 *
 * CROSS-CORE SAFETY: both cores run from flash (XIP), and an erase/program
 * stalls the whole QSPI bus, so every write goes through pico_flash's
 * flash_safe_execute(), which pauses the other core for the duration --
 * requires flash_safe_execute_core_init() to have been called once on core0
 * first (see main.c). Reads go straight through XIP. */
#include "mcu_log.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "mcu_config.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define SECTOR_MAGIC 0x4C4F4732u /* "LOG2" */
#define RECORD_SIZE 32
#define SLOTS_PER_SECTOR (FLASH_SECTOR_SIZE / RECORD_SIZE) /* 128; slot 0 is the header */

#define RECORD_EMPTY 0xFF /* erased flash */
#define RECORD_ENTRY 0x01
#define RECORD_CLEAR 0x02

/* The ring ends below the settings and the FNSAVE/STSAVE stores -- see
 * flash_layout.h. */
#define LOG_TOP FLASH_LOG_TOP
/* Room left above the firmware image for it to grow on a later update. */
#define FIRMWARE_GROWTH_MARGIN (256 * 1024)

typedef struct {
    uint32_t magic;
    uint32_t seq; /* increases by one per sector started */
    uint16_t gen; /* MCU_CONFIG_LOGGEN */
    uint16_t size_kb;
    uint8_t reserved[RECORD_SIZE - 12];
} sector_header_t;

typedef struct {
    uint8_t type;
    uint8_t level;
    uint8_t len;
    char msg[MCU_LOG_MSG_MAX];
    uint8_t reserved[RECORD_SIZE - 3 - MCU_LOG_MSG_MAX];
} record_t;

_Static_assert(sizeof(sector_header_t) == RECORD_SIZE, "sector header size");
_Static_assert(sizeof(record_t) == RECORD_SIZE, "record size");

extern char __flash_binary_end;

static uint32_t g_ring_start;   /* flash offset of sector 0 */
static uint16_t g_sectors;
static uint16_t g_size_kb;
static uint16_t g_gen;
static bool g_have_head;        /* false until the first record of this log */
static uint16_t g_head;         /* newest sector */
static uint32_t g_head_seq;
static uint16_t g_head_slot;    /* next free slot in the newest sector */

static const uint8_t *flash_at(uint32_t offset) { return (const uint8_t *)(XIP_BASE + offset); }
static uint32_t sector_offset(uint16_t i) { return g_ring_start + (uint32_t)i * FLASH_SECTOR_SIZE; }

static const sector_header_t *header_of(uint16_t i) { return (const sector_header_t *)flash_at(sector_offset(i)); }

static bool sector_is_ours(uint16_t i) {
    const sector_header_t *h = header_of(i);
    return h->magic == SECTOR_MAGIC && h->gen == g_gen && h->size_kb == g_size_kb;
}

static const record_t *record_at(uint16_t sector, uint16_t slot) {
    return (const record_t *)flash_at(sector_offset(sector) + (uint32_t)slot * RECORD_SIZE);
}

uint16_t mcu_log_max_size_kb(void) {
    uint32_t end = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE) + FIRMWARE_GROWTH_MARGIN;
    end = (end + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    uint32_t kb = end < LOG_TOP ? (LOG_TOP - end) / 1024 : 0;
    return kb > 0xFFFF ? 0xFFFF : (uint16_t)kb;
}

/* ---- flash writes (inside flash_safe_execute) ---- */

typedef struct {
    uint32_t erase_offset; /* a sector to erase first, or 0 */
    uint32_t page_offset;
    uint8_t page[FLASH_PAGE_SIZE];
} write_job_t;

static write_job_t g_job;

static void WriteCallback(void *param) {
    (void)param;
    if (g_job.erase_offset) flash_range_erase(g_job.erase_offset, FLASH_SECTOR_SIZE);
    flash_range_program(g_job.page_offset, g_job.page, FLASH_PAGE_SIZE);
}

/* Programs `bytes` (RECORD_SIZE) at flash `offset`, erasing `erase_offset`'s
 * sector first if non-zero. */
static void write_record(uint32_t offset, const void *bytes, uint32_t erase_offset) {
    uint32_t page = offset & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    memset(g_job.page, 0xFF, sizeof g_job.page);
    memcpy(g_job.page + (offset - page), bytes, RECORD_SIZE);
    g_job.erase_offset = erase_offset;
    g_job.page_offset = page;
    flash_safe_execute(WriteCallback, NULL, 1000);
}

/* Erases sector `i` and makes it the newest, with sequence number `seq`. */
static void start_sector(uint16_t i, uint32_t seq) {
    sector_header_t h;
    memset(&h, 0xFF, sizeof h);
    h.magic = SECTOR_MAGIC;
    h.seq = seq;
    h.gen = g_gen;
    h.size_kb = g_size_kb;
    write_record(sector_offset(i), &h, sector_offset(i));
    g_head = i;
    g_head_seq = seq;
    g_head_slot = 1;
    g_have_head = true;
}

static void append_record(uint8_t type, uint8_t level, const char *msg) {
    record_t r;
    uint8_t len = msg ? (uint8_t)strlen(msg) : 0;
    if (len > MCU_LOG_MSG_MAX) len = MCU_LOG_MSG_MAX;
    memset(&r, 0xFF, sizeof r);
    r.type = type;
    r.level = level;
    r.len = len;
    if (len) memcpy(r.msg, msg, len);
    if (!g_have_head) start_sector(0, 1);
    else if (g_head_slot == SLOTS_PER_SECTOR) start_sector((uint16_t)((g_head + 1) % g_sectors), g_head_seq + 1);
    write_record(sector_offset(g_head) + (uint32_t)g_head_slot * RECORD_SIZE, &r, 0);
    g_head_slot++;
}

/* ---- setup ---- */

/* Finds the newest sector of the current log and its next free slot. */
static void find_head(void) {
    g_have_head = false;
    for (uint16_t i = 0; i < g_sectors; i++) {
        if (!sector_is_ours(i)) continue;
        if (!g_have_head || header_of(i)->seq > g_head_seq) {
            g_have_head = true;
            g_head = i;
            g_head_seq = header_of(i)->seq;
        }
    }
    if (!g_have_head) return;
    g_head_slot = 1;
    while (g_head_slot < SLOTS_PER_SECTOR && record_at(g_head, g_head_slot)->type != RECORD_EMPTY) g_head_slot++;
}

static void setup(void) {
    uint16_t max_kb = mcu_log_max_size_kb();
    g_size_kb = mcu_config_get(MCU_CONFIG_LOGSIZE);
    if (g_size_kb > max_kb) g_size_kb = max_kb;
    if (g_size_kb < MCU_LOG_MIN_SIZE_KB) g_size_kb = MCU_LOG_MIN_SIZE_KB;
    g_size_kb &= (uint16_t)~3u; /* whole 4K sectors */
    g_sectors = (uint16_t)(g_size_kb / 4);
    g_ring_start = LOG_TOP - (uint32_t)g_sectors * FLASH_SECTOR_SIZE;
    g_gen = mcu_config_get(MCU_CONFIG_LOGGEN);
    find_head();
}

void mcu_log_init(void) { setup(); }

void mcu_log_resize(void) {
    /* A new ring start: earlier sectors of the old ring, or of an even
     * older one, mustn't be mistaken for this log's. */
    mcu_config_set(MCU_CONFIG_LOGGEN, (uint16_t)((mcu_config_get(MCU_CONFIG_LOGGEN) + 1) % 0xFFFF));
    setup();
}

/* ---- writing ---- */

void mcu_log_error(const char *msg) { append_record(RECORD_ENTRY, MCU_LOG_LEVEL_ERROR, msg); }
void mcu_log_warn(const char *msg) { append_record(RECORD_ENTRY, MCU_LOG_LEVEL_WARN, msg); }
void mcu_log_user(const char *msg) { append_record(RECORD_ENTRY, MCU_LOG_LEVEL_USER, msg); }

void mcu_log_info(const char *msg) {
    if (!mcu_log_get_info_enabled()) return;
    append_record(RECORD_ENTRY, MCU_LOG_LEVEL_INFO, msg);
}

void mcu_log_set_info_enabled(bool enabled) { mcu_config_set(MCU_CONFIG_LOGINFO, enabled ? 1 : 0); }
bool mcu_log_get_info_enabled(void) { return mcu_config_get(MCU_CONFIG_LOGINFO) != 0; }

void mcu_log_clear(void) {
    if (g_have_head) append_record(RECORD_CLEAR, 0, NULL);
}

/* ---- reading, newest first ---- */

/* Walks back from the newest record. Calls `visit` for each entry until it
 * returns false, a CLEAR record, or the oldest sector of the ring. */
static void walk(bool (*visit)(const record_t *r, void *ctx), void *ctx) {
    uint16_t sector = g_head, slot = g_head_slot;
    uint32_t seq = g_head_seq;
    if (!g_have_head) return;
    for (;;) {
        while (slot > 1) {
            const record_t *r = record_at(sector, --slot);
            if (r->type == RECORD_CLEAR) return;
            if (r->type == RECORD_ENTRY && !visit(r, ctx)) return;
        }
        sector = (uint16_t)((sector + g_sectors - 1) % g_sectors);
        if (sector == g_head || !sector_is_ours(sector) || header_of(sector)->seq != seq - 1) return;
        seq--;
        slot = SLOTS_PER_SECTOR;
    }
}

static bool count_visit(const record_t *r, void *ctx) {
    (void)r;
    (*(uint32_t *)ctx)++;
    return true;
}

uint32_t mcu_log_get_count(void) {
    uint32_t n = 0;
    walk(count_visit, &n);
    return n;
}

typedef struct {
    uint32_t skip;
    const record_t *found;
} find_ctx_t;

static bool find_visit(const record_t *r, void *ctx) {
    find_ctx_t *f = ctx;
    if (f->skip) {
        f->skip--;
        return true;
    }
    f->found = r;
    return false;
}

bool mcu_log_get_entry(uint32_t indexFromNewest, uint8_t *levelOut, char *msgOut) {
    find_ctx_t f = {indexFromNewest, NULL};
    walk(find_visit, &f);
    if (!f.found) return false;
    uint8_t len = f.found->len > MCU_LOG_MSG_MAX ? MCU_LOG_MSG_MAX : f.found->len;
    *levelOut = f.found->level;
    memcpy(msgOut, f.found->msg, len);
    msgOut[len] = 0;
    return true;
}
