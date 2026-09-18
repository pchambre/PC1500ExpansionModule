/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.h
 *
 * Bit-banged (software) I2C master for the GreenPAK link (GreenPAK1 and
 * GreenPAK2, sharing one physical bus -- see board_pins.h's
 * PIN_GREENPAK1_SDA/SCL and PIN_GREENPAK2_SDA/SCL alias). Bit-banged
 * for consistency with RP2350B's own GreenPAK link (see that project's
 * own greenpak_i2c.h for the full reasoning), even though GPIO26/27
 * (this board's real pins, confirmed against the PC1500-Pico2W-Dongle
 * schematic -- see board_pins.h) do land on a usable I2C1 SDA/SCL pair
 * and a hardware-peripheral implementation is possible here.
 *
 * This module provides the I2C primitives (start/stop/byte transfer,
 * plus register-style read/write) that greenpak_virtual_io.h builds on.
 * Ported from RP2350B/greenpak_i2c.h -- keep the two in sync by hand.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sda_gpio;
    uint32_t scl_gpio;
} greenpak_i2c_bus_t;

/* Configures sda_gpio/scl_gpio as open-drain (input when idle-high,
 * driven low when asserting 0) with internal pull-ups enabled. External
 * pull-ups on the GreenPAK board are still assumed for real I2C timing
 * margins -- the internal pulls are a fallback, not a substitute;
 * unconfirmed against real hardware. */
void greenpak_i2c_init(const greenpak_i2c_bus_t *bus);

/* Standard 7-bit-address I2C write: START, address+W, each byte in buf,
 * STOP. Returns true if every byte was ACKed. */
bool greenpak_i2c_write(const greenpak_i2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint32_t len);

/* Standard 7-bit-address I2C read: START, address+R, len bytes (NAKing
 * only the last), STOP. Returns true if the address was ACKed. */
bool greenpak_i2c_read(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t *buf, uint32_t len);

/* Register-style write: one register-address byte followed by one data
 * byte, matching GreenPAK's register interface convention. */
bool greenpak_i2c_write_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value);

/* Same wire format as greenpak_i2c_write_reg(), but does not treat a NAK
 * on the final (value) byte as failure -- required specifically for the
 * NVM/EEPROM Erase Register (datasheet word address 0xE3). Per Renesas's
 * SLG46824/6/7-A errata ("Issue 2: Non-I2C Compliant ACK Behavior for
 * the NVM and EEPROM Page Erase Byte"), the chip deliberately NAKs the
 * erase data byte even though the erase command is accepted and
 * executes normally once STOP is sent -- a real, documented hardware
 * quirk, not a wiring fault. A NAK on the address or register byte is
 * still treated as failure (a genuine bus/wiring problem). Only use
 * this for the erase register write; every other GreenPAK register
 * write should use the strict greenpak_i2c_write_reg(). */
bool greenpak_i2c_write_reg_tolerate_nak(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value);

/* Register-style read: writes the register-address byte (no STOP), then
 * a repeated-start read of `len` bytes. */
bool greenpak_i2c_read_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len);
