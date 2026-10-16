/* romrplc.c -- see romrplc.h. */
#include "romrplc.h"

#include <string.h>

#include "flash_layout.h"
#include "mcu_store.h"
#include "pc_exp.h"

#ifdef PC1500_TARGET_CARD

#define ROMRPLC_MAGIC 0x434C5052u /* "RPLC" */
#define ROMRPLC_VERSION 1
#define ROMRPLC_FLAG_ENABLED 0x01
#define ROMRPLC_FLAG_FROM_RAM 0x02
#define ROMRPLC_IMAGE_OFFSET FLASH_SECTOR_SIZE /* the header has the first sector */

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
    uint32_t length;
    uint32_t crc;
    char name[ROMRPLC_NAME_MAX + 1];
} header_t;

/* The flags as last read or written: core0's loop asks romrplc_mcu_serving(). */
static volatile bool g_mcu_serving;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool read_header(header_t *h) {
    return mcu_store_read(EXP_STORE_SLOT_ROMRPLC, 0, (uint8_t *)h, sizeof *h) && h->magic == ROMRPLC_MAGIC &&
           h->version == ROMRPLC_VERSION && h->length == ROMRPLC_IMAGE_SIZE;
}

static void cache(const header_t *h) {
    g_mcu_serving = (h->flags & ROMRPLC_FLAG_ENABLED) && !(h->flags & ROMRPLC_FLAG_FROM_RAM);
}

static bool write_header(const header_t *h) {
    if (!mcu_store_erase_range(EXP_STORE_SLOT_ROMRPLC, 0, FLASH_SECTOR_SIZE) ||
        !mcu_store_write(EXP_STORE_SLOT_ROMRPLC, 0, (const uint8_t *)h, sizeof *h))
        return false;
    cache(h);
    return true;
}

void romrplc_get(romrplc_state_t *out) {
    header_t h;
    memset(out, 0, sizeof *out);
    if (!read_header(&h)) return;
    out->loaded = true;
    out->enabled = (h.flags & ROMRPLC_FLAG_ENABLED) != 0;
    out->from_ram = (h.flags & ROMRPLC_FLAG_FROM_RAM) != 0;
    out->crc = h.crc;
    memcpy(out->name, h.name, ROMRPLC_NAME_MAX);
    out->name[ROMRPLC_NAME_MAX] = 0;
}

bool romrplc_save_image(const uint8_t *image, const char *name) {
    header_t old, h;
    memset(&h, 0, sizeof h);
    h.magic = ROMRPLC_MAGIC;
    h.version = ROMRPLC_VERSION;
    if (read_header(&old)) h.flags = old.flags & ROMRPLC_FLAG_FROM_RAM; /* keep the source, never enabled */
    h.length = ROMRPLC_IMAGE_SIZE;
    h.crc = crc32(image, ROMRPLC_IMAGE_SIZE);
    strncpy(h.name, name, ROMRPLC_NAME_MAX);
    g_mcu_serving = false;
    if (!mcu_store_erase(EXP_STORE_SLOT_ROMRPLC) ||
        !mcu_store_write(EXP_STORE_SLOT_ROMRPLC, ROMRPLC_IMAGE_OFFSET, image, ROMRPLC_IMAGE_SIZE) ||
        !mcu_store_write(EXP_STORE_SLOT_ROMRPLC, 0, (const uint8_t *)&h, sizeof h))
        return false;
    cache(&h);
    return true;
}

bool romrplc_set(bool enabled, bool from_ram) {
    header_t h;
    if (!read_header(&h)) return false;
    h.flags = (uint8_t)((enabled ? ROMRPLC_FLAG_ENABLED : 0) | (from_ram ? ROMRPLC_FLAG_FROM_RAM : 0));
    return write_header(&h);
}

bool romrplc_read_image(uint8_t *dst) {
    header_t h;
    return read_header(&h) && mcu_store_read(EXP_STORE_SLOT_ROMRPLC, ROMRPLC_IMAGE_OFFSET, dst, ROMRPLC_IMAGE_SIZE);
}

bool romrplc_mcu_serving(void) { return g_mcu_serving; }

bool romrplc_boot(uint8_t *serve) {
    header_t h;
    g_mcu_serving = false;
    if (!read_header(&h)) return false;
    cache(&h);
    return g_mcu_serving && mcu_store_read(EXP_STORE_SLOT_ROMRPLC, ROMRPLC_IMAGE_OFFSET, serve, ROMRPLC_IMAGE_SIZE);
}

#else /* the dongle: no ROM replacement */

void romrplc_get(romrplc_state_t *out) { memset(out, 0, sizeof *out); }
bool romrplc_save_image(const uint8_t *image, const char *name) { (void)image; (void)name; return false; }
bool romrplc_set(bool enabled, bool from_ram) { (void)enabled; (void)from_ram; return false; }
bool romrplc_read_image(uint8_t *dst) { (void)dst; return false; }
bool romrplc_mcu_serving(void) { return false; }
bool romrplc_boot(uint8_t *serve) { (void)serve; return false; }

#endif
