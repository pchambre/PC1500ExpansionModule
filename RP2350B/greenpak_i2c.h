/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.h
 *
 * Bit-banged (software) I2C master for the two GreenPAK links
 * (GreenPAK1/U3, GreenPAK2/U9). Deliberately NOT using RP2350's hardware
 * I2C peripherals: board_pins.h's GPIO23-26 don't land on a matched
 * SDA/SCL pair on the same hardware I2C instance (checked against the
 * RP2350 GPIO funcsel table -- GPIO23 is really I2C1 SCL, GPIO24 is I2C0
 * SDA, etc, not a usable pair). That's fine here since this link is
 * low-speed, non-timing-critical glue-logic comms, unlike the LH5801 bus
 * itself -- bit-banging on arbitrary GPIOs is the standard approach for
 * exactly this situation.
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
