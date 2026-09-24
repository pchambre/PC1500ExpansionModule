/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_log.c -- see mcu_log.h for the why.
 *
 * STORAGE DESIGN: the whole log (header + all MCU_LOG_MAX_ENTRIES) lives
 * in one reserved flash sector, kept mirrored in a RAM struct for reads.
 * Every write (a new entry, a CLEAR, or toggling info-enabled) erases that
 * whole sector and reprograms it from the updated RAM image -- the
 * simplest possible correct implementation, deliberately not a real
 * circular buffer with in-place slot reuse (which would need careful
 * flash-erase-boundary bookkeeping to avoid rewriting more than necessary).
 * This is fine ONLY because this log is for rare events (internal MCU
 * failures a user can't otherwise see, not routine/high-frequency
 * logging) -- flash sectors on these boards are typically rated for
 * ~100,000 erase cycles; even several log writes a day for years stays
 * nowhere near that. Do not reuse this pattern for anything logged more
 * than occasionally.
 *
 * Reserved region: the LAST flash sector (PICO_FLASH_SIZE_BYTES -
 * FLASH_SECTOR_SIZE), a standard, safe placement -- this board's flash is
 * 4MB and the whole program image is under 400KB, so there's no risk of
 * this colliding with program code/data or the embedded ROM image.
 *
 * CROSS-CORE SAFETY: this board runs core0 (monitor_run()'s bus loop) and
 * core1 (DoCommand(), where a real log call -- e.g. a GreenPAK
 * verification failure -- would actually happen) concurrently, both
 * executing from flash (XIP). A flash erase/program stalls the whole QSPI
 * bus, which would corrupt whatever the OTHER core is doing mid-fetch if
 * not coordinated. Uses pico_flash's flash_safe_execute(), which pauses
 * the other core for the duration -- requires flash_safe_execute_core_init()
 * to have been called once on core0 first (see main.c). */
#include "mcu_log.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define MCU_LOG_MAGIC 0x4C4F4731u /* "LOG1" */

typedef struct {
    uint32_t magic;
    uint8_t infoEnabled;
    uint8_t entryCount; /* 0..MCU_LOG_MAX_ENTRIES, oldest-first in entries[] */
    uint8_t reserved[2];
} mcu_log_header_t;

typedef struct {
    uint8_t level; /* mcu_log_level_t */
    uint8_t msgLen;
    char msg[MCU_LOG_MSG_MAX];
} mcu_log_entry_t;

typedef struct {
    mcu_log_header_t header;
    mcu_log_entry_t entries[MCU_LOG_MAX_ENTRIES];
} mcu_log_image_t;

#define MCU_LOG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MCU_LOG_WRITE_SIZE \
    (((sizeof(mcu_log_image_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

static mcu_log_image_t g_image;

static void FlashWriteCallback(void *param) {
    (void)param;
    flash_range_erase(MCU_LOG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(MCU_LOG_FLASH_OFFSET, (const uint8_t *)&g_image, MCU_LOG_WRITE_SIZE);
}

/* Commits the current in-RAM g_image to flash. Safe to call from either
 * core (see this file's own top comment) -- flash_safe_execute() pairs
 * whichever core calls it against the OTHER one, so this works whether
 * the log call originated on core0 or core1. */
static void CommitToFlash(void) {
    flash_safe_execute(FlashWriteCallback, NULL, 1000);
}

void mcu_log_init(void) {
    const mcu_log_image_t *flashImage =
        (const mcu_log_image_t *)(XIP_BASE + MCU_LOG_FLASH_OFFSET);
    if (flashImage->header.magic == MCU_LOG_MAGIC &&
        flashImage->header.entryCount <= MCU_LOG_MAX_ENTRIES) {
        memcpy(&g_image, flashImage, sizeof(g_image));
        return;
    }
    /* Never written, or garbage -- fresh state. Default INFO logging OFF
     * (per the board owner's explicit instruction). Deliberately does NOT
     * write to flash here -- a boot that never logs anything shouldn't
     * cost a flash erase/program cycle just to persist an empty log. */
    memset(&g_image, 0, sizeof(g_image));
    g_image.header.magic = MCU_LOG_MAGIC;
    g_image.header.infoEnabled = 0;
    g_image.header.entryCount = 0;
}

static void AppendEntry(mcu_log_level_t level, const char *msg) {
    uint8_t len = (uint8_t)strlen(msg);
    if (len > MCU_LOG_MSG_MAX) len = MCU_LOG_MSG_MAX;

    if (g_image.header.entryCount == MCU_LOG_MAX_ENTRIES) {
        /* Full -- drop the oldest (index 0), shift the rest down. Cheap:
         * at most MCU_LOG_MAX_ENTRIES-1 small struct copies, and this
         * only ever runs on an already-rare log write. */
        memmove(&g_image.entries[0], &g_image.entries[1],
                (size_t)(MCU_LOG_MAX_ENTRIES - 1) * sizeof(mcu_log_entry_t));
        g_image.header.entryCount--;
    }

    mcu_log_entry_t *slot = &g_image.entries[g_image.header.entryCount];
    slot->level = (uint8_t)level;
    slot->msgLen = len;
    memcpy(slot->msg, msg, len);
    if (len < MCU_LOG_MSG_MAX) memset(slot->msg + len, 0, (size_t)(MCU_LOG_MSG_MAX - len));
    g_image.header.entryCount++;

    CommitToFlash();
}

void mcu_log_error(const char *msg) { AppendEntry(MCU_LOG_LEVEL_ERROR, msg); }
void mcu_log_warn(const char *msg) { AppendEntry(MCU_LOG_LEVEL_WARN, msg); }

void mcu_log_info(const char *msg) {
    if (!g_image.header.infoEnabled) return;
    AppendEntry(MCU_LOG_LEVEL_INFO, msg);
}

void mcu_log_set_info_enabled(bool enabled) {
    if ((bool)g_image.header.infoEnabled == enabled) return; /* no-op -- skip the flash write */
    g_image.header.infoEnabled = enabled ? 1 : 0;
    CommitToFlash();
}

bool mcu_log_get_info_enabled(void) { return g_image.header.infoEnabled != 0; }

void mcu_log_clear(void) {
    memset(g_image.entries, 0, sizeof(g_image.entries));
    g_image.header.entryCount = 0;
    CommitToFlash();
}

uint8_t mcu_log_get_count(void) { return g_image.header.entryCount; }

bool mcu_log_get_entry(uint8_t indexFromNewest, uint8_t *levelOut, char *msgOut) {
    if (indexFromNewest >= g_image.header.entryCount) return false;
    /* entries[] is oldest-first; newest is at entryCount-1. */
    const mcu_log_entry_t *e =
        &g_image.entries[g_image.header.entryCount - 1 - indexFromNewest];
    *levelOut = e->level;
    memcpy(msgOut, e->msg, e->msgLen);
    msgOut[e->msgLen] = 0;
    return true;
}
