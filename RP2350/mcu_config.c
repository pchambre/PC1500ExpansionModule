/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_config.c -- see mcu_config.h.
 *
 * Same storage pattern as mcu_log.c: a RAM image, written back by erasing
 * and reprogramming one whole reserved sector through flash_safe_execute()
 * (which pauses the other core -- both run from XIP flash). Settings change
 * rarely (only when someone types MCONF ...=...), so that's fine for flash
 * wear. See flash_layout.h for where the sector is. */
#include "mcu_config.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define MCU_CONFIG_MAGIC 0x43464731u /* "CFG1" */
#define MCU_CONFIG_FLASH_OFFSET FLASH_CONFIG_OFFSET

typedef struct {
    uint32_t magic;
    uint16_t value[MCU_CONFIG_COUNT];
} mcu_config_image_t;

static const uint16_t kDefaults[MCU_CONFIG_COUNT] = {
    [MCU_CONFIG_LED] = 1,
    [MCU_CONFIG_SLEEPWAIT] = 0,
    [MCU_CONFIG_LOGSIZE] = 100,
    [MCU_CONFIG_LOGINFO] = 0,
    [MCU_CONFIG_LOGGEN] = 0,
    [MCU_CONFIG_UNUSED_5] = 0,
};

/* Padded to a whole flash page for flash_range_program(). */
static union {
    mcu_config_image_t image;
    uint8_t page[FLASH_PAGE_SIZE];
} g_config;

/* The hostname sits at a fixed place in the page, clear of the settings
 * array however it grows: NUL-terminated; erased flash (a page saved before
 * 2026-09-28) means the default. */
#define HOSTNAME_OFFSET 128
static_assert(sizeof(mcu_config_image_t) <= HOSTNAME_OFFSET, "settings overrun the hostname");
static char *hostname_bytes(void) { return (char *)&g_config.page[HOSTNAME_OFFSET]; }

static void FlashWriteCallback(void *param) {
    (void)param;
    flash_range_erase(MCU_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(MCU_CONFIG_FLASH_OFFSET, g_config.page, FLASH_PAGE_SIZE);
}

void mcu_config_init(void) {
    const mcu_config_image_t *flashImage = (const mcu_config_image_t *)(XIP_BASE + MCU_CONFIG_FLASH_OFFSET);
    memset(&g_config, 0xFF, sizeof g_config);
    if (flashImage->magic == MCU_CONFIG_MAGIC) {
        memcpy(g_config.page, flashImage, FLASH_PAGE_SIZE); /* the settings and the hostname */
        /* settings added since the last save read as erased flash */
        for (int i = 0; i < MCU_CONFIG_COUNT; i++)
            if (g_config.image.value[i] == 0xFFFF) g_config.image.value[i] = kDefaults[i];
        return;
    }
    /* Never saved: defaults, without a flash write until something changes. */
    g_config.image.magic = MCU_CONFIG_MAGIC;
    memcpy(g_config.image.value, kDefaults, sizeof kDefaults);
}

uint16_t mcu_config_get(uint8_t id) {
    return id < MCU_CONFIG_COUNT ? g_config.image.value[id] : 0;
}

bool mcu_config_set(uint8_t id, uint16_t value) {
    if (id >= MCU_CONFIG_COUNT) return false;
    if (g_config.image.value[id] == value) return true;
    g_config.image.value[id] = value;
    flash_safe_execute(FlashWriteCallback, NULL, 1000);
    return true;
}

const char *mcu_config_get_hostname(void) {
    const char *h = hostname_bytes();
    return (h[0] == (char)0xFF || h[0] == 0) ? MCU_CONFIG_HOSTNAME_DEFAULT : h;
}

bool mcu_config_set_hostname(const char *name, uint8_t len) {
    if (len == 0 || len > MCU_CONFIG_HOSTNAME_MAX) return false;
    for (uint8_t i = 0; i < len; i++)
        if (name[i] < 0x20 || name[i] > 0x7E || name[i] == '"') return false;
    if (strlen(mcu_config_get_hostname()) == len && memcmp(mcu_config_get_hostname(), name, len) == 0) return true;
    memset(hostname_bytes(), 0xFF, MCU_CONFIG_HOSTNAME_MAX + 1);
    memcpy(hostname_bytes(), name, len);
    hostname_bytes()[len] = 0;
    flash_safe_execute(FlashWriteCallback, NULL, 1000);
    return true;
}
