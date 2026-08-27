/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* board_pins.h
 *
 * GPIO assignment for the RP2350B expansion board redesign (bare RP2350B
 * "core dev board", not the Pico 2 W module -- see ../RP2350/ for that
 * earlier, superseded design). Matches the real KiCad schematic at
 * C:\Users\paulc\Documents\PC1500ExpansionBoard\PC1500-RP2350B-BLE
 * (U1's GPIO0-34 contiguous layout) -- if this file and that schematic
 * ever disagree, the schematic is ground truth, re-derive this from it.
 *
 * Unlike the Pico 2 W module (26 usable GPIOs, no spares, QMI CS1 needed
 * to make the SD card fit), the bare RP2350B exposes all 48 GPIOs and
 * this design only needs 35, leaving 13 genuinely spare (GPIO35-39,
 * GPIO44-47, plus GPIO28-31) -- so the SD card uses a normal, full 4-pin
 * hardware SPI1 peripheral instead of the QMI-CS1 trick.
 *
 * GPIO0-12 = AD0-AD12 (address bus) is a deliberate direct bit alignment:
 * GPIO*n* = AD*n*, so firmware reads the whole 13-bit address with zero
 * shifting (`gpio_in & ADDR_PIN_MASK`). GPIO13-20 = DATA0-7_MCU is also
 * fully contiguous (no split fields needed here, unlike the old Pico 2 W
 * layout where SD_CS1's fixed silicon pin forced a low/high split).
 *
 * Every SPI/UART pin below was checked against the real RP2350 GPIO
 * function-select table (the doc comment at the top of the Pico SDK's
 * hardware/gpio.h) to confirm it actually supports the hardware
 * peripheral this firmware uses -- not just that the schematic wiring is
 * electrically valid. The two GreenPAK I2C links deliberately do NOT use
 * RP2350's hardware I2C peripherals (their SDA/SCL pairs don't land on a
 * matched I2C instance) -- see greenpak_i2c.h; that's fine since these
 * are low-speed, non-timing-critical control links.
 */
#pragma once

/* ---- LH5801-facing bus: 13-bit flat address, 8K window (0x8000-0x9FFF).
 * GPIO*n* = AD*n* for n=0..12, contiguous, matching the schematic exactly. ---- */
#define PIN_A0  0
#define PIN_A1  1
#define PIN_A2  2
#define PIN_A3  3
#define PIN_A4  4
#define PIN_A5  5
#define PIN_A6  6
#define PIN_A7  7
#define PIN_A8  8
#define PIN_A9  9
#define PIN_A10 10
#define PIN_A11 11
#define PIN_A12 12
#define ADDR_PIN_BASE  PIN_A0   /* A0..A12 are contiguous -- read as one 13-bit field */
#define ADDR_PIN_COUNT 13
#define ADDR_PIN_MASK  ((1u << ADDR_PIN_COUNT) - 1u)

/* ---- Data bus D0-D7: fully contiguous (GPIO13-20), one 8-bit field --
 * unlike the Pico 2 W layout, nothing forces a low/high split here. ---- */
#define PIN_D0 13
#define PIN_D1 14
#define PIN_D2 15
#define PIN_D3 16
#define PIN_D4 17
#define PIN_D5 18
#define PIN_D6 19
#define PIN_D7 20
#define DATA_PIN_BASE  PIN_D0
#define DATA_PIN_COUNT 8
#define DATA_PIN_MASK  ((1u << DATA_PIN_COUNT) - 1u)

/* ---- GreenPAK trigger lines: combine CS+R/W+OE ("read requested, drive
 * data now") and CS+W ("write requested, latch data now") into two
 * unambiguous edges -- firmware doesn't compute CS&&RW&&OE itself. ---- */
#define PIN_TRIG_RD 21  /* CS && Read && OE asserted by the GreenPAK -- drive data bus now */
#define PIN_TRIG_WR 22  /* CS && Write asserted by the GreenPAK -- latch data bus now */

/* ---- I2C-style links to the two GreenPAKs (bit-banged, not hardware
 * I2C -- see greenpak_i2c.h for why). GreenPAK1 (U3) handles the SRAM
 * address/control mux and the PC-1500/1500A sense circuit; GreenPAK2
 * (U9) generates the two trigger lines above and DME0/OD decode. ---- */
#define PIN_GREENPAK1_SDA 23
#define PIN_GREENPAK1_SCL 24
#define PIN_GREENPAK2_SDA 25
#define PIN_GREENPAK2_SCL 26

/* ---- SRAM/ROM mux status line from GreenPAK1 (IO14) -- reflects
 * whether the 6K ROM window is currently answered by the SRAM directly
 * (ROM_FROM_SRAM) or routed through this MCU's buffer (ROM_FROM_MCU). ---- */
#define PIN_ROM_SRAM_STATUS 27

/* ---- SD card via a real hardware SPI1 peripheral. Checked against the
 * RP2350 GPIO funcsel table: GPIO40=SPI1 RX(MISO), 41=SPI1 CSn, 42=SPI1
 * SCK, 43=SPI1 TX(MOSI) -- a genuine, correctly-ordered SPI1 block (see
 * hw_config.c). CS itself is toggled as a plain GPIO by the FatFs SD
 * driver, not through the hardware CSn line, but GPIO41 is used for it
 * anyway since it was spare and keeps the block visually contiguous. ---- */
#define PIN_SD_MISO 40
#define PIN_SD_CS   41
#define PIN_SD_SCK  42
#define PIN_SD_MOSI 43

/* ---- RN4871 BLE module, UART transparent mode. GPIO32/33 checked
 * against the RP2350 GPIO funcsel table: GPIO32=UART0 TX, GPIO33=UART0
 * RX -- exactly matches the direction each net needs (BLE_UART_RX on the
 * schematic = what this MCU transmits, into the module's own RX pin;
 * BLE_UART_TX = what the module transmits, into this MCU's RX). ---- */
#define PIN_BLE_UART_TX 32  /* schematic net BLE_UART_RX -- MCU transmits here */
#define PIN_BLE_UART_RX 33  /* schematic net BLE_UART_TX -- MCU receives here */
#define PIN_BLE_WAKE    34

/* GPIO28-31, GPIO35-39, GPIO44-47 (13 pins) are spare. */
