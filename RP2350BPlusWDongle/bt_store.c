/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* bt_store.c -- see bt_store.h.
 *
 * Flash and RAM format (one sector): "BTS1", then records back to back --
 * [tag 4, LE][length 2, LE][value] -- ended by a tag of 0 or 0xFFFFFFFF
 * (erased flash). Records are kept unique by tag: storing one replaces it. */
#include "bt_store.h"

#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "ble/le_device_db_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "flash_layout.h"
#include "mcu_store.h"
#include "monitor.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"

#define MAGIC "BTS1"
#define HEADER 4
#define RECORD_HEADER 6
#define STORE_SIZE 2048 /* ~25 bonds' worth; the sector holds more than needed */

static uint8_t g_store[STORE_SIZE];
static uint32_t g_used; /* bytes in use, header included */
static volatile bool g_dirty;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* The offset of `tag`'s record, or 0. */
static uint32_t find(uint32_t tag) {
    for (uint32_t at = HEADER; at + RECORD_HEADER <= g_used; at += RECORD_HEADER + rd16(g_store + at + 4))
        if (rd32(g_store + at) == tag) return at;
    return 0;
}

static int get_tag(void *context, uint32_t tag, uint8_t *buffer, uint32_t buffer_size) {
    (void)context;
    uint32_t at = find(tag);
    if (!at) return 0;
    uint16_t len = rd16(g_store + at + 4);
    if (len > buffer_size) len = (uint16_t)buffer_size;
    memcpy(buffer, g_store + at + RECORD_HEADER, len);
    return len;
}

static void delete_tag(void *context, uint32_t tag) {
    (void)context;
    uint32_t at = find(tag);
    if (!at) return;
    uint32_t len = RECORD_HEADER + rd16(g_store + at + 4);
    memmove(g_store + at, g_store + at + len, g_used - at - len);
    g_used -= len;
    g_dirty = true;
}

static int store_tag(void *context, uint32_t tag, const uint8_t *data, uint32_t data_size) {
    uint32_t at = find(tag);
    if (at && rd16(g_store + at + 4) == data_size && memcmp(g_store + at + RECORD_HEADER, data, data_size) == 0)
        return 0; /* unchanged: no flash write for it */
    delete_tag(context, tag);
    if (tag == 0 || tag == 0xFFFFFFFFu || data_size > 0xFFFF || g_used + RECORD_HEADER + data_size > STORE_SIZE)
        return 1;
    uint8_t *p = g_store + g_used;
    p[0] = (uint8_t)tag;
    p[1] = (uint8_t)(tag >> 8);
    p[2] = (uint8_t)(tag >> 16);
    p[3] = (uint8_t)(tag >> 24);
    p[4] = (uint8_t)data_size;
    p[5] = (uint8_t)(data_size >> 8);
    memcpy(p + RECORD_HEADER, data, data_size);
    g_used += RECORD_HEADER + data_size;
    g_dirty = true;
    return 0;
}

static const btstack_tlv_t kTlv = {get_tag, store_tag, delete_tag};

void bt_store_install(void) {
    static bool installed;
    if (!installed) { /* the saved records (a plain XIP read) */
        installed = true;
        g_used = HEADER;
        if (mcu_store_read(EXP_STORE_SLOT_BTBONDS, 0, g_store, STORE_SIZE) && memcmp(g_store, MAGIC, HEADER) == 0) {
            while (g_used + RECORD_HEADER <= STORE_SIZE) {
                uint32_t tag = rd32(g_store + g_used);
                uint32_t next = g_used + RECORD_HEADER + rd16(g_store + g_used + 4);
                if (tag == 0 || tag == 0xFFFFFFFFu || next > STORE_SIZE) break;
                g_used = next;
            }
        }
        memcpy(g_store, MAGIC, HEADER);
    }
    btstack_tlv_set_instance(&kTlv, NULL);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(&kTlv, NULL));
    le_device_db_tlv_configure(&kTlv, NULL);
}

/* core0 (BTstack's context): a consistent copy for core1 to save. */
static uint8_t g_snapshot[STORE_SIZE];
static uint32_t g_snapshot_len;
static uint32_t take_snapshot(void *param) {
    (void)param;
    memcpy(g_snapshot, g_store, g_used);
    g_snapshot_len = g_used;
    g_dirty = false;
    return 0;
}

bool bt_store_commit(void) {
    if (!g_dirty) return true;
    /* with the CYW43 down (a sleep since the bond changed) its async context
     * isn't running, and nothing on core0 changes the store: straight in */
    if (g_cyw43_up) async_context_execute_sync(cyw43_arch_async_context(), take_snapshot, NULL);
    else take_snapshot(NULL);
    /* mcu_store_write() wants whole pages: pad with the end marker */
    uint32_t len = (g_snapshot_len + RECORD_HEADER + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    if (len > STORE_SIZE) len = STORE_SIZE;
    memset(g_snapshot + g_snapshot_len, 0, len - g_snapshot_len);
    if (mcu_store_erase(EXP_STORE_SLOT_BTBONDS) && mcu_store_write(EXP_STORE_SLOT_BTBONDS, 0, g_snapshot, len))
        return true;
    g_dirty = true; /* try again next time */
    return false;
}
