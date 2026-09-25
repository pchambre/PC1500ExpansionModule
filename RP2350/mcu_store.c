/* mcu_store.c -- see mcu_store.h. Writes go through flash_safe_execute(),
 * like mcu_log.c's and mcu_config.c's, since both cores run from flash. */
#include "mcu_store.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "pc_exp.h"
#include "pico/flash.h"
#include "pico/platform.h"

typedef struct {
    uint32_t offset;
    uint32_t size;
} slot_t;

static const slot_t kSlots[] = {
    [EXP_STORE_SLOT_FNKEYS] = {FLASH_FNKEYS_OFFSET, FLASH_FNKEYS_SIZE},
    [EXP_STORE_SLOT_STATE] = {FLASH_STATE_OFFSET, FLASH_STATE_SIZE},
};

static const slot_t *slot_of(uint8_t slot) {
    return slot < sizeof kSlots / sizeof kSlots[0] ? &kSlots[slot] : NULL;
}

static struct {
    uint32_t offset, len;
    const uint8_t *data;
    uint8_t page[FLASH_PAGE_SIZE];
} g_job;

static void EraseCallback(void *param) {
    (void)param;
    flash_range_erase(g_job.offset, g_job.len);
}

/* Whole pages, the last one padded with 0xFF (leaves those bytes erased). */
static void ProgramCallback(void *param) {
    (void)param;
    for (uint32_t done = 0; done < g_job.len; done += FLASH_PAGE_SIZE) {
        uint32_t n = g_job.len - done < FLASH_PAGE_SIZE ? g_job.len - done : FLASH_PAGE_SIZE;
        memset(g_job.page, 0xFF, sizeof g_job.page);
        memcpy(g_job.page, g_job.data + done, n);
        flash_range_program(g_job.offset + done, g_job.page, FLASH_PAGE_SIZE);
    }
}

bool mcu_store_erase(uint8_t slot) {
    const slot_t *s = slot_of(slot);
    if (!s) return false;
    g_job.offset = s->offset;
    g_job.len = s->size;
    return flash_safe_execute(EraseCallback, NULL, 2000) == PICO_OK;
}

bool mcu_store_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint32_t len) {
    const slot_t *s = slot_of(slot);
    if (!s || offset % FLASH_PAGE_SIZE || offset > s->size || len > s->size - offset) return false;
    g_job.offset = s->offset + offset;
    g_job.len = len;
    g_job.data = data;
    return flash_safe_execute(ProgramCallback, NULL, 1000) == PICO_OK;
}

bool mcu_store_read(uint8_t slot, uint32_t offset, uint8_t *data, uint32_t len) {
    const slot_t *s = slot_of(slot);
    if (!s || offset > s->size || len > s->size - offset) return false;
    memcpy(data, (const uint8_t *)(XIP_BASE + s->offset + offset), len);
    return true;
}
