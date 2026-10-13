/* ble_link_compat.h -- the few BTstack names ble_link.c's Link protocol
 * uses, for the internal card (PC1500_BLE_MODULE, 2026-10-13), where the
 * radio is the BGM220S behind module_link.h and BTstack isn't linked.
 * Same meanings as BTstack's (btstack_util.h, bluetooth.h). */
#pragma once

#include <stdint.h>
#include <string.h>

typedef uint8_t bd_addr_t[6]; /* most significant byte first, as printed */
typedef uint16_t hci_con_handle_t;
#define HCI_CON_HANDLE_INVALID 0xFFFF
#define ERROR_CODE_SUCCESS 0

static inline uint16_t little_endian_read_16(const uint8_t *b, int p) {
    return (uint16_t)(b[p] | b[p + 1] << 8);
}

static inline uint32_t little_endian_read_32(const uint8_t *b, int p) {
    return (uint32_t)b[p] | (uint32_t)b[p + 1] << 8 | (uint32_t)b[p + 2] << 16 | (uint32_t)b[p + 3] << 24;
}

static inline void little_endian_store_16(uint8_t *b, uint16_t p, uint16_t v) {
    b[p] = (uint8_t)v;
    b[p + 1] = (uint8_t)(v >> 8);
}

static inline void little_endian_store_32(uint8_t *b, uint16_t p, uint32_t v) {
    b[p] = (uint8_t)v;
    b[p + 1] = (uint8_t)(v >> 8);
    b[p + 2] = (uint8_t)(v >> 16);
    b[p + 3] = (uint8_t)(v >> 24);
}

static inline int bd_addr_cmp(const bd_addr_t a, const bd_addr_t b) { return memcmp(a, b, 6); }
static inline void bd_addr_copy(bd_addr_t d, const bd_addr_t s) { memcpy(d, s, 6); }
