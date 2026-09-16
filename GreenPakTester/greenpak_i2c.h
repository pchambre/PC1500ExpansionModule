/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.h
 *
 * Hardware I2C0 wrapper for the GreenPAK link. GP0 = I2C0 SDA and GP1 =
 * I2C0 SCL land on a real matched RP2350 hardware I2C0 funcsel pair
 * here, so this uses the pico-sdk's hardware_i2c peripheral directly --
 * unlike RP2350B's greenpak_i2c.c, which bit-bangs because its GreenPAK
 * link didn't land on a matched pair. See greenpak_pins.h for the rest
 * of the GreenPAK wiring (the 15 direct IO pins, handled separately by
 * greenpak_gpio.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GREENPAK_I2C_SDA_GPIO 0
#define GREENPAK_I2C_SCL_GPIO 1

/* GreenPAK control-register/NVM I2C slave addresses, for accessing
 * block address 000 (register range 0x00-0xFF, which covers the
 * Connection Matrix Virtual Input/Output registers at 0x74-0x7B/0xC9 --
 * see greenpak_virtual_io.h). Per the SLG46826 datasheet section 15.2,
 * the 7-bit address is [4-bit control code][3-bit block address]. The
 * control code is configurable (NVM bits, or externally by IO5-IO2)
 * and defaults to 0001 (addr 0x08), but this bench setup tests two
 * different chip designs, each compiled with its own distinct control
 * code so DUT identity can be told apart on the bus: GP1-SRAM.gp6 is
 * 0011 (0x18), GP2's design is 0010 (0x10). */
#define GREENPAK_GP1_CONTROL_CODE 0x3
#define GREENPAK_GP2_CONTROL_CODE 0x2
#define GREENPAK_GP1_I2C_ADDR (uint8_t)(GREENPAK_GP1_CONTROL_CODE << 3)
#define GREENPAK_GP2_I2C_ADDR (uint8_t)(GREENPAK_GP2_CONTROL_CODE << 3)

typedef enum {
    GREENPAK_DUT_NONE = 0,
    GREENPAK_DUT_GP1,
    GREENPAK_DUT_GP2,
} greenpak_dut_t;

/* The currently-detected DUT's I2C address, set by
 * greenpak_i2c_detect_dut(). greenpak_virtual_io.c's calls target
 * whichever chip this currently names -- there's only ever one DUT
 * plugged into this bench setup at a time, unlike RP2350B's dual-chip
 * shared bus. */
extern uint8_t greenpak_dut_addr;

/* Initializes I2C0 on GP0/GP1 at `baudrate_hz` and enables internal
 * pull-ups. External pull-ups on the breadboard are still assumed for
 * real bus timing margins -- the internal pulls are a fallback, not a
 * substitute; unconfirmed against real hardware. */
void greenpak_i2c_init(uint32_t baudrate_hz);

/* Writes `len` bytes from buf to addr7. Returns true if every byte was
 * ACKed (the underlying i2c_write_blocking call returned len). */
bool greenpak_i2c_write(uint8_t addr7, const uint8_t *buf, size_t len);

/* Reads `len` bytes from addr7 into buf. Returns true on success (the
 * underlying i2c_read_blocking call returned len). */
bool greenpak_i2c_read(uint8_t addr7, uint8_t *buf, size_t len);

/* Register-style write: one register-address byte followed by one data
 * byte, matching GreenPAK's register interface convention. */
bool greenpak_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t value);

/* Register-style read: writes the register-address byte (no STOP), then
 * a repeated-start read of `len` bytes. */
bool greenpak_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len);

/* Probes whether any device ACKs at `addr7` right now (zero-length
 * write -- Start, address byte, Stop, no data). */
bool greenpak_i2c_probe(uint8_t addr7);

/* Probes GREENPAK_GP1_I2C_ADDR then GREENPAK_GP2_I2C_ADDR. On a match,
 * sets greenpak_dut_addr and returns which chip was found; returns
 * GREENPAK_DUT_NONE (leaving greenpak_dut_addr unchanged) if neither
 * ACKs. */
greenpak_dut_t greenpak_i2c_detect_dut(void);
