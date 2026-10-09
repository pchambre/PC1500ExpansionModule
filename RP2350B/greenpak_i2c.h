/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.h
 *
 * Bit-banged (software) I2C master for the GreenPAK link (GreenPAK1/U3,
 * and GreenPAK2/U9 when its J7 isolation jumpers are populated -- see
 * board_pins.h). Originally NOT using RP2350's hardware I2C peripherals
 * because the old GPIO23/24 assignment didn't land on a matched SDA/SCL
 * pair on the same hardware instance. The 2026-09-08 pin-swap pass moved
 * this link to GPIO28/29, which DOES land on a real, matched hardware
 * I2C0 SDA/SCL pair (checked against the RP2350 GPIO funcsel table) --
 * so switching to the hardware peripheral is now a real option, not
 * previously possible. Left bit-banged for now since this link is
 * low-speed, non-timing-critical glue-logic comms and switching drivers
 * is a bigger change than a pin reassignment -- flagged as a real
 * opportunity, not acted on unilaterally.
 *
 * This module only provides the primitives (start/stop/byte transfer).
 * The actual GreenPAK command protocol isn't designed yet -- see
 * monitor.c's EXP_COMMAND_ROM_FROM_SRAM/ROM_FROM_MCU cases, still
 * placeholders pending that design (ported as-is from the Pico 2 W
 * version, which had the same open item).
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
 * NVM/EEPROM Erase Register (datasheet word address 0xE3), if/when NVM
 * provisioning is ported to this board (see RP2350/greenpak_nvm.c's
 * erase_page for the working reference implementation). Per Renesas's
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
