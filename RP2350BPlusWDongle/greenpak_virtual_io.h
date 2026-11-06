/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_virtual_io.h
 *
 * SLG46826 Connection Matrix Virtual Inputs (datasheet rev 3.25,
 * section 6.3 "Connection Matrix Virtual Inputs" and 15.7.3 "I2C Byte
 * Write Bit Masking"). Eight of the connection matrix's inputs are
 * sourced from a register written over I2C instead of a macrocell
 * output, letting the host drive signals into the matrix without a
 * physical pin. All eight share one byte register (0x7A); bit 7 is
 * I2C_virtual_0, bit 0 is I2C_virtual_7 (datasheet Table 23).
 *
 * Writing 0x7A directly would clobber all 8 virtual inputs at once, so
 * every write here goes through the Byte Write Bit Masking register
 * (0xC9) first, to touch only the requested bit.
 *
 * `addr7` is an explicit parameter, not a fixed constant: GreenPAK1 and
 * GreenPAK2 share one physical I2C bus (see board_pins.h) and are
 * distinguished only by their I2C slave address (each chip's NVM-
 * configured control code -- see the GreenPakTester project's
 * greenpak_i2c.h for how that address is derived), so this driver
 * doesn't assume there's only one chip on the bus.
 *
 * Ported from RP2350B/greenpak_virtual_io.h -- keep the two in sync by
 * hand.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "greenpak_i2c.h"

#define GREENPAK_REG_VIRTUAL_INPUT   0x7A
#define GREENPAK_REG_BYTE_WRITE_MASK 0xC9

/* Sets virtual input `vi_index` (0..7) on the chip at `addr7` to
 * `level`, without disturbing that chip's other 7 virtual inputs.
 * Returns false on I2C failure. */
bool greenpak_virtual_input_set(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t vi_index, bool level);

/* Reads back virtual input `vi_index` (0..7) on the chip at `addr7`
 * into *level_out: the last value written via
 * greenpak_virtual_input_set, or the NVM-loaded default if nothing has
 * been written since power-up/reset. Returns false on I2C failure. */
bool greenpak_virtual_input_get(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t vi_index, bool *level_out);
