/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_pins.h
 *
 * Logical GreenPAK IOn index (0..14) <-> Pico GPIO number, plus the
 * physical GreenPAK package pin each corresponds to. This is the ONLY
 * place the breadboard wiring should be encoded -- greenpak_gpio.c,
 * selftest.c, and main.c refer to GreenPAK IOn indices, never raw Pico
 * GPIO numbers, so a rewiring only requires editing this table.
 *
 * Wiring (breadboard, all at 3.3V -- confirmed, no level shifters):
 *   Pico GPn -> GreenPAK physical pin n, for every n in
 *   {2,3,4,5,6,7,10,12,13,15,16,17,18,19,20}. This covers all 15 IO
 *   pins (IO0..IO14) on the SLG46826V (GreenPAK6, 20-pin package) with
 *   no gaps.
 *   GreenPAK pins 1 (VDD), 11 (GND), 14 (VDD2) go to Pico 3V3/GND
 *   directly, not GPIO -- not part of this table.
 *   GreenPAK pins 8 (SCL) and 9 (SDA) are wired via Pico I2C0 (GP1 =
 *   SCL0, GP0 = SDA0) -- see greenpak_i2c.h, not part of this table.
 *
 * Pin function labels (net names from GP1-SRAM.gp6) are informational
 * only, for log/debug readability -- they don't affect behavior.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t io_index;       /* GreenPAK IOn, 0..14 */
    uint8_t greenpak_pin;   /* physical package pin number */
    uint32_t gpio;          /* Pico GPIO number */
    const char *label;      /* net label from GP1-SRAM.gp6, or NULL if none */
} greenpak_io_map_entry_t;

#define GREENPAK_IO_COUNT 15

extern const greenpak_io_map_entry_t greenpak_io_map[GREENPAK_IO_COUNT];

/* Returns the Pico GPIO number for GreenPAK IOn `io_index` (0..14).
 * Asserts/traps on an out-of-range index -- this is a programming
 * error, not a runtime condition to recover from. */
uint32_t greenpak_gpio_for_io(uint8_t io_index);
