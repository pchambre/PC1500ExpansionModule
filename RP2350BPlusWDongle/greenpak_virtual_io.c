/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_virtual_io.h"

#include <assert.h>

#include "pico/time.h"

/* Retry tolerance (2026-09-21) -- these previously did a single, unretried
 * I2C attempt each, unlike sc18is602b.c's own driver for the SD bridge
 * chip, which retries up to 50 times specifically because that chip is
 * known to occasionally not ACK. STAGE's BEGIN command alone chains 9
 * back-to-back calls into this file (3x greenpak_virtual_input_set(), each
 * itself 2 I2C writes, plus 3x greenpak_virtual_input_get() to read every
 * one back and verify it took -- see monitor.c's EXP_COMMAND_ROM_COPY_BEGIN)
 * with zero retry margin anywhere in that chain -- a single missed ACK on
 * ANY of those 9 transactions failed the whole operation. Root-caused as
 * the real explanation for STAGE's previously-characterized "intermittent
 * I2C flakiness" (see this project's own memory notes), not a new theory --
 * a much smaller retry budget than the SD bridge's own 50x100us suffices
 * here (no known "busy while internally processing" behavior on this chip
 * family the way the SPI bridge chip has; this is just tolerating ordinary
 * transient bus noise). */
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
