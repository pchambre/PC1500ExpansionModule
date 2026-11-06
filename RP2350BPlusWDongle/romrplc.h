/* romrplc.h -- ROMRPLC (2026-10-16): replacing the PC-1500's own ROM,
 * C000H-FFFFH, with a 16K image loaded from the SD card into MCU flash.
 *
 * One image slot (flash_layout.h FLASH_ROMRPLC_*, mcu_store slot
 * EXP_STORE_SLOT_ROMRPLC): a header sector -- the file name, its CRC-32,
 * and whether replacement is on and where it's served from -- then the
 * image. The header is rewritten alone when ENABLE/DISABLE/STAGEMCU/
 * STAGERAM change the state, so the image is only written by LOAD.
 *
 * Serving it (monitor.c): from the MCU, the image sits in the serve buffer
 * at bus C000H-FFFFH and the GreenPAK that drives INHIBIT gets
 * ROM_REPLACEMENT; from the SRAM (STAGERAM, a later board), the image is
 * staged there first. Every change of state ends in a reset of the
 * PC-1500 (EXP_KW_ACTION_REBOOT).
 *
 * Internal-card build only (PC1500_TARGET_CARD): the dongle can't see
 * AD13-AD15. Elsewhere romrplc_get() reports no image and the setters fail.
 * Called from core1 (keywords.c's commands) except romrplc_boot(), which
 * main.c's boot path runs on core0 before core1 exists. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROMRPLC_IMAGE_SIZE 16384u
#define ROMRPLC_NAME_MAX 40

typedef struct {
    bool loaded;    /* an image is in flash */
    bool enabled;   /* replacement is on */
    bool from_ram;  /* STAGERAM, else STAGEMCU */
    uint32_t crc;   /* the image's CRC-32 */
    char name[ROMRPLC_NAME_MAX + 1]; /* the file it came from, as typed or picked */
} romrplc_state_t;

/* The saved state. */
void romrplc_get(romrplc_state_t *out);

/* Replaces the image: erases the slot, writes `image` (16K, in RAM) and a
 * header naming `name`, replacement off, the source kept. */
bool romrplc_save_image(const uint8_t *image, const char *name);

/* Saves new flags with the existing image (header sector only). Refuses
 * to enable when no image is loaded. */
bool romrplc_set(bool enabled, bool from_ram);

/* Copies the image out of flash (to RAM). */
bool romrplc_read_image(uint8_t *dst);

/* Replacement on and served by the MCU -- what the boot path and the sleep
 * decisions ask. Cached: cheap enough for core0's loop. */
bool romrplc_mcu_serving(void);

/* Boot (core0, before the bus PIO starts): reads the header once and, when
 * the MCU serves the image, copies it to `serve` (the serve buffer's
 * C000H-FFFFH pages). True if it did. */
bool romrplc_boot(uint8_t *serve);
