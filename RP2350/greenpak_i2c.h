/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.h
 *
 * Real RP2350 I2C1 hardware peripheral (2026-09-20) for the GreenPAK
 * link (GreenPAK1 and GreenPAK2, sharing one physical bus -- see
 * board_pins.h's PIN_GREENPAK1_SDA/SCL and PIN_GREENPAK2_SDA/SCL alias),
 * and for the SD card's SC18IS602B I2C-to-SPI bridge, which shares this
 * same bus (see sc18is602b.h's own top comment). GPIO26/27 (this board's
 * real pins, confirmed against the PC1500-Pico2W-Dongle schematic -- see
 * board_pins.h) land on a genuine I2C1 SDA/SCL pair -- see greenpak_i2c.c's
 * own top comment for why this replaced an earlier bit-banged
 * implementation (bit-banging's own per-bit software overhead turned out
 * to be the real ceiling on this bus's throughput, not any deliberate
 * delay tuning). NOT ported back to RP2350B's own copy of this file --
 * different board, different pins, not verified to land on a hardware
 * I2C peripheral the same way.
 *
 * This module provides the I2C primitives (plain read/write, plus
 * register-style read/write) that greenpak_virtual_io.h builds on.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sda_gpio;
    uint32_t scl_gpio;
} greenpak_i2c_bus_t;

/* GreenPAK1/GreenPAK2 I2C slave addresses (2026-09, STAGE keyword) --
 * confirmed already NVM-programmed on the real PC1500-Pico2W-Dongle
 * board with these distinct control codes, matching
 * GreenPakTester/greenpak_i2c.h's own real-hardware-validated values
 * (GP1-SRAM.gp6 compiled with control code 0x3, GP2-MCU.gp6 with 0x2).
 * NOT pending a provisioning step on this board -- supersedes the
 * earlier assumption in monitor.c's ROM_FROM_SRAM/ROM_FROM_MCU cases
 * that these addresses weren't assigned yet. */
#define GREENPAK_GP1_CONTROL_CODE 0x3
#define GREENPAK_GP2_CONTROL_CODE 0x2
#define GREENPAK_GP1_I2C_ADDR ((uint8_t)(GREENPAK_GP1_CONTROL_CODE << 3))
#define GREENPAK_GP2_I2C_ADDR ((uint8_t)(GREENPAK_GP2_CONTROL_CODE << 3))

/* GP1-SRAM.gp6's Connection Matrix Virtual Input assignments (indices
 * into greenpak_virtual_input_set/get, see greenpak_virtual_io.h) --
 * ported from GreenPakTester/selftest.h, independently cross-checked
 * against the real compiled .gp6 project files (named nets "SRAM/ROM
 * WE" and "SRAM/ROM Remap" wired to these exact virtual-input outputs).
 * GP2's design uses the same virtual input index as GP1's Remap (per
 * the board owner's own confirmation) -- GP2 has no WE-equivalent
 * virtual input since it doesn't control SRAM writes, only read/write
 * trigger signals to the MCU. */
#define GP1_VI_SRAM_ROM_WE    0
#define GP1_VI_SRAM_ROM_REMAP 1
#define GP2_VI_SRAM_ROM_REMAP 1

/* Configures the RP2350's real I2C1 hardware peripheral at 400kHz (the
 * SC18IS602B SD bridge's own ceiling -- see greenpak_i2c.c's own top
 * comment) and sets sda_gpio/scl_gpio to their I2C function. No internal
 * pull-ups are enabled -- real 1.5K external pull-ups already sit on this
 * bus (added 2026-09-20 specifically for Fast Mode), and the RP2350's own
 * weak internal pulls would only be redundant alongside them. */
void greenpak_i2c_init(const greenpak_i2c_bus_t *bus);

/* I2C bus recovery (2026-09-21) -- standard bit-banged clock-out sequence
 * (NXP UM10204 "Bus clear" procedure) for un-wedging a slave that's stuck
 * holding SDA low mid-byte, without power-cycling anything. Needed because
 * neither a power-rail blip nor U5's own RESET pin (SC18IS602B, tied to a
 * pull-up via R9, not wired to any RP2350 GPIO on this board) is reachable
 * in software -- U5 shares the same 3V3 rail as the Pico module itself
 * (confirmed via the real KiCad schematic), so the RP2350 can't cut power
 * to just U5 without also cutting its own. This is the only software-only
 * recovery option left. Temporarily takes SDA/SCL back from the I2C
 * peripheral to plain GPIO, clocks SCL up to 9 times (enough to walk any
 * slave through finishing whatever byte it's mid-transmission on -- the
 * spec's own upper bound, one full byte's worth of bit-clocks), checking
 * SDA after each pulse, then issues a manual STOP (SDA low-to-high while
 * SCL is high) before handing the pins back to greenpak_i2c_init(). Safe
 * to call any time the bus is suspected wedged, including as a precaution
 * after a deliberately out-of-spec experiment (e.g. an overclocked I2C
 * rate test) that got a hard failure. */
void greenpak_i2c_bus_recover(const greenpak_i2c_bus_t *bus);

/* Standard 7-bit-address I2C write: START, address+W, each byte in buf,
 * STOP. Returns true if every byte was ACKed. */
bool greenpak_i2c_write(const greenpak_i2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint32_t len);

/* Standard 7-bit-address I2C read: START, address+R, len bytes, STOP.
 * Returns true if the address was ACKed and every byte was read. */
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
