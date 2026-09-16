/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_virtual_io.h"
#include "greenpak_i2c.h"

#include <assert.h>

bool greenpak_virtual_input_set(uint8_t vi_index, bool level) {
    assert(vi_index < 8);
    uint8_t bit = 7 - vi_index;

    /* Mask bit = 1 protects that bit from the next Byte Write Command;
     * 0 leaves it writable. We want only `bit` writable. */
    uint8_t mask = (uint8_t)~(1u << bit);
    if (!greenpak_i2c_write_reg(greenpak_dut_addr, GREENPAK_REG_BYTE_WRITE_MASK, mask)) {
        return false;
    }

    uint8_t value = level ? (uint8_t)(1u << bit) : 0;
    return greenpak_i2c_write_reg(greenpak_dut_addr, GREENPAK_REG_VIRTUAL_INPUT, value);
}

bool greenpak_virtual_input_get(uint8_t vi_index, bool *level_out) {
    assert(vi_index < 8);
    uint8_t bit = 7 - vi_index;

    uint8_t value;
    if (!greenpak_i2c_read_reg(greenpak_dut_addr, GREENPAK_REG_VIRTUAL_INPUT, &value, 1)) {
        return false;
    }
    *level_out = (value & (1u << bit)) != 0;
    return true;
}
