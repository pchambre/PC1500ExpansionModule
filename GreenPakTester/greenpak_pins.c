/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_pins.h"

#include <assert.h>
#include <stddef.h>

const greenpak_io_map_entry_t greenpak_io_map[GREENPAK_IO_COUNT] = {
    {  0,  2,  2, NULL },
    {  1,  3,  3, NULL },
    {  2,  4,  4, NULL },
    {  3,  5,  5, NULL },
    {  4,  6,  6, NULL },
    {  5,  7,  7, "R/W from bus" },
    {  6, 10, 10, NULL },
    {  7, 12, 12, NULL },
    {  8, 13, 13, NULL },
    {  9, 15, 15, NULL },
    { 10, 16, 16, "R/W to SRAM" },
    { 11, 17, 17, NULL },
    { 12, 18, 18, "SRAM CS" },
    { 13, 19, 19, NULL },
    { 14, 20, 20, NULL },
};

uint32_t greenpak_gpio_for_io(uint8_t greenpak_pin) {
    for (uint8_t i = 0; i < GREENPAK_IO_COUNT; i++) {
        if (greenpak_io_map[i].greenpak_pin == greenpak_pin) {
            return greenpak_io_map[i].gpio;
        }
    }
    assert(false); /* greenpak_pin isn't one of the 15 mapped IO pins */
    return 0;
}
