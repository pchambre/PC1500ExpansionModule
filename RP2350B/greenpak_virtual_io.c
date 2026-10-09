/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_virtual_io.h"

#include <assert.h>

#include "pico/time.h"

/* Retry tolerance (2026-09-21) -- see RP2350/greenpak_virtual_io.c's own
 * comment (this file is kept in sync with it by hand, per this file's own
 * header comment) for the full rationale: these previously did a single,
 * unretried I2C attempt each, and STAGE's BEGIN command chains 9
 * back-to-back calls into this file with zero retry margin anywhere,
 * root-caused as the real explanation for STAGE's "intermittent I2C
 * flakiness." */
#define GREENPAK_VIRTUAL_IO_RETRY_COUNT 5
#define GREENPAK_VIRTUAL_IO_RETRY_DELAY_US 200u

bool greenpak_virtual_input_set(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t vi_index, bool level) {
    assert(vi_index < 8);
    uint8_t bit = 7 - vi_index;

    /* Mask bit = 1 protects that bit from the next Byte Write Command;
     * 0 leaves it writable. We want only `bit` writable. */
    uint8_t mask = (uint8_t)~(1u << bit);
    uint8_t value = level ? (uint8_t)(1u << bit) : 0;

    for (int attempt = 0; attempt < GREENPAK_VIRTUAL_IO_RETRY_COUNT; attempt++) {
        if (greenpak_i2c_write_reg(bus, addr7, GREENPAK_REG_BYTE_WRITE_MASK, mask)
            && greenpak_i2c_write_reg(bus, addr7, GREENPAK_REG_VIRTUAL_INPUT, value)) {
            return true;
        }
        sleep_us(GREENPAK_VIRTUAL_IO_RETRY_DELAY_US);
    }
    return false;
}

bool greenpak_virtual_input_get(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t vi_index, bool *level_out) {
    assert(vi_index < 8);
    uint8_t bit = 7 - vi_index;

    for (int attempt = 0; attempt < GREENPAK_VIRTUAL_IO_RETRY_COUNT; attempt++) {
        uint8_t value;
        if (greenpak_i2c_read_reg(bus, addr7, GREENPAK_REG_VIRTUAL_INPUT, &value, 1)) {
            *level_out = (value & (1u << bit)) != 0;
            return true;
        }
        sleep_us(GREENPAK_VIRTUAL_IO_RETRY_DELAY_US);
    }
    return false;
}
