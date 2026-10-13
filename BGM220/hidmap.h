/* hidmap.h -- just enough of a HID report descriptor (a BLE keyboard's
 * Report Map) to turn its keyboard input report into the 8-byte boot
 * report the RP2354B's key sequencer takes (kbd_seq.c): where the
 * modifier bits are, and the key array or key bitmap. The dongle does this
 * with BTstack's HID parser (kbd_host.c); the module has no BTstack. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool found;
    uint8_t report_id;   /* 0: the map uses no report IDs */
    uint16_t mod_bit;    /* bit offset of the 8 modifier bits (usages E0-E7); 0xFFFF none */
    uint16_t keys_bit;   /* bit offset of the key array (usage indexes) or bitmap */
    uint8_t keys_count;  /* array entries, or bitmap bits */
    uint8_t keys_size;   /* bits per array entry; 1 for a bitmap */
    uint8_t keys_min;    /* the first usage (usage minimum) */
    int16_t keys_lmin;   /* the array's logical minimum: value - lmin + min = usage */
    bool keys_bitmap;
} hidmap_keyboard_t;

/* Finds the keyboard input report in `map`. False if there isn't one. */
bool hidmap_find_keyboard(const uint8_t *map, size_t len, hidmap_keyboard_t *kb);

/* A keyboard input report (as notified: without its report ID) into a
 * boot report: [modifiers][0][up to 6 key usages]. */
void hidmap_to_boot(const hidmap_keyboard_t *kb, const uint8_t *report, size_t len, uint8_t boot[8]);
