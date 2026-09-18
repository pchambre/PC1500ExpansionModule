/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* sc18is602b.h -- thin driver for U5, the SC18IS602B I2C-to-SPI bridge
 * chip that carries this board's SD card (see board_pins.h's own SD
 * comment for why: the microSD socket isn't wired to any RP2350
 * GPIO/QMI peripheral at all on this board). U5 sits on the SAME I2C
 * bus as the GreenPAKs (PIN_GREENPAK1_SDA/SCL), distinguished by its own
 * I2C address -- confirmed 2026-09-17 against
 * PC1500-Pico2W-Dongle.kicad_sch: A0/A1/A2 (pins 14-16) are all tied to
 * GND, giving the fixed 7-bit address 0101000b = 0x28 (datasheet
 * section 7.1.1). Only SS0 is wired (to SD_CS) -- SS1-3 are
 * unconnected on this board.
 *
 * Protocol per NXP SC18IS602B datasheet (rev 7, 21 October 2019):
 *   - A "Configure SPI Interface" write (Function ID 0xF0 + one data
 *     byte) sets clock rate and mode; SPI mode/GPIO role otherwise
 *     default at reset (SS pins default to slave-select outputs, no
 *     GPIO Enable command needed for that).
 *   - A "SPI read and write" write (Function ID 0x01-0x0F, low 4 bits
 *     select SS0-SS3) clocks out the following data bytes on MOSI while
 *     simultaneously capturing MISO into the chip's own internal
 *     buffer (200 bytes deep) -- a real SPI transaction bounded by that
 *     one I2C write.
 *   - The captured bytes are retrieved by a plain I2C read (no Function
 *     ID) of the same length, in a separate I2C transaction.
 * sc18is602b_transfer() below wraps both halves and transparently
 * chunks anything longer than the 200-byte buffer (this driver uses a
 * conservative 128-byte chunk size) -- callers can just ask for a full
 * 512-byte SD block transfer directly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "greenpak_i2c.h"

#define SC18IS602B_I2C_ADDR 0x28

/* Configure SPI Interface (Function ID 0xF0) data-byte fields. */
#define SC18IS602B_MODE_CPOL0_CPHA0 0x00  /* SD cards want SPI Mode 0 */
#define SC18IS602B_CLK_1843KHZ 0x00       /* fastest this chip offers */
#define SC18IS602B_CLK_461KHZ  0x01
#define SC18IS602B_CLK_115KHZ  0x02
#define SC18IS602B_CLK_58KHZ   0x03        /* used during SD init (<=400kHz required) */

/* Sets SPI mode/clock rate. `bus` must already be greenpak_i2c_init()'d
 * (shared with the GreenPAK link -- see this file's own top comment). */
bool sc18is602b_configure(const greenpak_i2c_bus_t *bus, uint8_t mode, uint8_t clock_rate);

/* Full-duplex SPI transfer of `len` bytes through SS0 (the only SS wired
 * on this board), chunked internally to respect the chip's 200-byte
 * buffer. `tx` may be NULL (send 0xFF filler, e.g. for reads/dummy
 * clocks); `rx` may be NULL (discard the read-back entirely -- skips
 * the I2C read phase, not just the copy, saving real bus time when the
 * caller doesn't need the result). Returns false on any I2C failure. */
bool sc18is602b_transfer(const greenpak_i2c_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len);

/* Clocks `len` dummy (0xFF) bytes out on the shared SPICLK/MOSI lines
 * WITHOUT asserting SD_CS -- selects SS1 (unconnected on this board)
 * instead of SS0, since SPICLK/MOSI/MISO are one shared bus across all
 * four SSn outputs (datasheet Fig 1) and only the selected SSn pin
 * actually goes low. Needed for the SD power-up clock preamble: many
 * cards require CS to be sampled HIGH during those initial clocks to
 * select SPI mode at all, rather than staying in native SD mode and
 * never responding to any SPI-framed command afterward -- confirmed
 * live (2026-09-17) that clocking this preamble through SS0 (CS held
 * low the whole time) left a real card, known-good in other readers,
 * never responding to CMD0 at all. */
bool sc18is602b_clock_only(const greenpak_i2c_bus_t *bus, uint32_t len);
