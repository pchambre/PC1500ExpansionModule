/* hidmap.c -- see hidmap.h. A HID report descriptor is a list of short
 * items: [prefix: tag(4) type(2) size(2)][0, 1, 2 or 4 data bytes]. Global
 * items (usage page, report size/count/ID, logical minimum) persist; local
 * ones (usages) end at each main item. Input main items lay out a report's
 * fields in order, so counting their bits gives each field's offset. */
#include "hidmap.h"

#include <string.h>

#define PAGE_KEYBOARD 0x07

static uint32_t item_value(const uint8_t *d, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) {
        v |= (uint32_t)d[i] << (8 * i);
    }
    return v;
}

bool hidmap_find_keyboard(const uint8_t *map, size_t len, hidmap_keyboard_t *kb) {
    uint16_t page = 0, umin = 0, umax = 0;
    uint32_t size = 0, count = 0;
    int32_t lmin = 0;
    uint8_t id = 0;
    bool have_umin = false;
    uint16_t usage0 = 0; /* the first plain usage, for a single-usage field */
    bool have_usage = false;
    uint16_t bit[256]; /* input bits so far, per report ID */
    memset(bit, 0, sizeof bit);
    memset(kb, 0, sizeof *kb);
    kb->mod_bit = 0xFFFF;
    kb->keys_bit = 0xFFFF;

    size_t i = 0;
    while (i < len) {
        uint8_t prefix = map[i++];
        if (prefix == 0xFE) { /* a long item: skip it */
            if (i + 2 > len) {
                break;
            }
            i += 2u + map[i];
            continue;
        }
        uint8_t n = (uint8_t)((prefix & 3) == 3 ? 4 : (prefix & 3));
        if (i + n > len) {
            break;
        }
        uint32_t v = item_value(map + i, n);
        i += n;
        uint8_t tag = prefix & 0xFC;
        switch (tag) {
            case 0x04: page = (uint16_t)v; break;                      /* usage page */
            case 0x14: /* logical minimum: signed */
                lmin = n == 1 ? (int8_t)v : n == 2 ? (int16_t)v : (int32_t)v;
                break;
            case 0x74: size = v; break;                                /* report size */
            case 0x84: id = (uint8_t)v; break;                         /* report ID */
            case 0x94: count = v; break;                               /* report count */
            case 0x08: if (!have_usage) { usage0 = (uint16_t)v; have_usage = true; } break; /* usage */
            case 0x18: umin = (uint16_t)v; have_umin = true; break;    /* usage minimum */
            case 0x28: umax = (uint16_t)v; break;                      /* usage maximum */
            case 0x80: { /* input */
                bool constant = (v & 0x01) != 0, variable = (v & 0x02) != 0;
                uint32_t bits = size * count;
                if (!constant && page == PAGE_KEYBOARD && !kb->found) {
                    uint16_t lo = have_umin ? umin : usage0;
                    if (variable && size == 1 && lo == 0xE0 && count >= 8) {
                        kb->mod_bit = bit[id];
                        kb->report_id = id;
                    } else if (variable && size == 1 && count > 8) { /* NKRO: a bit per key */
                        kb->keys_bit = bit[id];
                        kb->keys_count = (uint8_t)(count > 255 ? 255 : count);
                        kb->keys_size = 1;
                        kb->keys_min = (uint8_t)lo;
                        kb->keys_bitmap = true;
                        kb->report_id = id;
                    } else if (!variable && size >= 8) { /* the usual key array */
                        kb->keys_bit = bit[id];
                        kb->keys_count = (uint8_t)(count > 255 ? 255 : count);
                        kb->keys_size = (uint8_t)size;
                        kb->keys_min = (uint8_t)(have_umin ? umin : 0);
                        kb->keys_lmin = (int16_t)lmin;
                        kb->keys_bitmap = false;
                        kb->report_id = id;
                    }
                    if (kb->keys_bit != 0xFFFF) {
                        kb->found = true; /* the first keyboard report with keys */
                    }
                }
                bit[id] = (uint16_t)(bit[id] + bits);
                have_umin = have_usage = false;
                break;
            }
            case 0x90: case 0xB0: case 0xA0: case 0xC0: /* output, feature, collection, end */
                have_umin = have_usage = false;
                break;
            default:
                break;
        }
    }
    (void)umax;
    return kb->found;
}

static uint32_t get_bits(const uint8_t *r, size_t len, uint16_t at, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t k = 0; k < n; k++) {
        uint16_t b = (uint16_t)(at + k);
        if ((size_t)(b / 8) < len && (r[b / 8] >> (b % 8)) & 1) {
            v |= 1u << k;
        }
    }
    return v;
}

void hidmap_to_boot(const hidmap_keyboard_t *kb, const uint8_t *r, size_t len, uint8_t boot[8]) {
    uint8_t n = 0;
    memset(boot, 0, 8);
    if (kb->mod_bit != 0xFFFF) {
        boot[0] = (uint8_t)get_bits(r, len, kb->mod_bit, 8);
    }
    if (kb->keys_bitmap) {
        for (uint8_t k = 0; k < kb->keys_count && n < 6; k++) {
            if (get_bits(r, len, (uint16_t)(kb->keys_bit + k), 1)) {
                uint8_t usage = (uint8_t)(kb->keys_min + k);
                if (usage >= 0xE0 && usage <= 0xE7) {
                    boot[0] |= (uint8_t)(1u << (usage - 0xE0));
                } else if (usage) {
                    boot[2 + n++] = usage;
                }
            }
        }
    } else {
        for (uint8_t k = 0; k < kb->keys_count && n < 6; k++) {
            uint32_t v = get_bits(r, len, (uint16_t)(kb->keys_bit + k * kb->keys_size), kb->keys_size > 16 ? 16 : kb->keys_size);
            int32_t usage = (int32_t)v - kb->keys_lmin + kb->keys_min;
            if (usage <= 0) {
                continue; /* no key in this slot (or ErrorRollOver's 1-3: ignored below) */
            }
            if (usage <= 3) {
                continue;
            }
            if (usage >= 0xE0 && usage <= 0xE7) {
                boot[0] |= (uint8_t)(1u << (usage - 0xE0));
            } else if (usage < 0xE0) {
                boot[2 + n++] = (uint8_t)usage;
            }
        }
    }
}
