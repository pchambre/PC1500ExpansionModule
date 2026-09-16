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
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GREENPAK_REG_VIRTUAL_INPUT   0x7A
#define GREENPAK_REG_BYTE_WRITE_MASK 0xC9

/* Sets virtual input `vi_index` (0..7) to `level` without disturbing
 * the other 7 virtual inputs. Returns false on I2C failure. */
bool greenpak_virtual_input_set(uint8_t vi_index, bool level);

/* Reads back virtual input `vi_index` (0..7) into *level_out: the last
 * value written via greenpak_virtual_input_set, or the NVM-loaded
 * default if nothing has been written since power-up/reset. Returns
 * false on I2C failure. */
bool greenpak_virtual_input_get(uint8_t vi_index, bool *level_out);
