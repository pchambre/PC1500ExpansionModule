/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* board_pins.h
 *
 * GPIO assignment for the RP2350B expansion board redesign (bare RP2350B
 * "core dev board", not the Pico 2 W module -- see ../RP2350/ for that
 * earlier, superseded design). Re-derived 2026-09-08 directly from the
 * KiCad PCB's own per-pad `pinfunction` fields (authoritative -- these
 * come straight from the RP2350B symbol's pin data, not from manual
 * pin-number bookkeeping) at
 * C:\Users\paulc\Documents\PSoC Creator\PC1500-PSOC5\PC1500-RP2350B-BLE.
 * The previous version of this file described an intended-but-never-
 * fully-realized layout (fully contiguous data bus, GPIO21/22 for the
 * trigger lines) that did not match the actual wiring -- this version
 * reflects verified, current reality, bugs included (see below).
 *
 * The bare RP2350B exposes all 48 GPIOs; this design uses 35 of them.
 *
 * GPIO0-12 = AD0-AD12 (address bus) is a direct bit alignment: GPIO*n* =
 * AD*n*, so firmware reads the whole 13-bit address with zero shifting
 * (`gpio_in & ADDR_PIN_MASK`), and it fits inside a single 32-bit
 * GPIO_IN register read. Confirmed pin-by-pin against the schematic
 * 2026-09-08, including catching and fixing a real bug where AD4_MCU/
 * AD6_MCU's net labels sat 2.54mm short of GPIO4/GPIO6's actual pin
 * position with no wire between them -- those two address bits were
 * silently unconnected until that fix.
 *
 * The data bus is NOT contiguous, unlike an earlier planned layout:
 * D0-D2 live at GPIO16-18 (in reverse order -- GPIO18=D0, GPIO17=D1,
 * GPIO16=D2), while D3-D7 are separately at GPIO35-39 (in-order, ascending).
 * Firmware must read/assemble the byte from two register groups.
 *
 * Re-derived again 2026-09-10: U7 (the FXMA108 level shifter between this
 * chip and the PC-1500's D0-D7) has each channel's PC-1500-side pin wired
 * to D_(7-channel), not D_channel -- a real, since-corrected schematic bug
 * (channel 0's A-side pin was floating outright; channels 1-7 were each
 * bridged to the WRONG PC-1500 bit). The fix keeps every already-routed
 * U1<->U7 physical connection untouched (that side was already non-crossing)
 * and instead renames which DATA_k_MCU net sits on which channel, so this
 * file's bit assignment flipped (old D0..D7 GPIOs are now this file's
 * D7..D0) relative to any version of this file from before 2026-09-10.
 *
 * KNOWN BUG, not yet fixed in hardware: SD_MISO is wired to GPIO27, a
 * plain GPIO with no SPI1 capability, while GPIO40 (the real SPI1 RX/
 * MISO funcsel pin, and the pin CS/SCK/MOSI below correctly assume) is
 * left unconnected. As wired, the SD card cannot use hardware SPI1 for
 * reads -- either bit-bang MISO on GPIO27, or fix the schematic to move
 * SD_MISO to GPIO40 (a real fix, not a relabel -- ask before assuming
 * which). PIN_SD_MISO below reflects actual wiring (27), not the
 * design intent (40).
 *
 * GreenPAK1 (U3) I2C runs through U10 (TCA9406, a 2-bit I2C level
 * translator) between U1's GPIO28/29 (3.3V side) and the shared VGG-side
 * I2C bus (SDA_GP/SCL_GP) that GreenPAK1's GREENPAK1_SDA/SCL are always
 * hard-wired to -- bit-banged on the RP2350B side, not a hardware I2C
 * peripheral (see greenpak_i2c.h). (Moved from GPIO23/24 to GPIO28/29 in
 * the 2026-09-08 pin-swap pass -- physically closer to U10, see the
 * spare-pin list at the bottom of this file for the full rationale.)
 *
 * GreenPAK2 (U9) shares that SAME bus -- there is no separate
 * PIN_GREENPAK2_SDA/SCL, GPIO28/29 talk to both chips -- but only when
 * two shunt jumpers are installed on J7 (pins 1-2 bridge SCL_GP to
 * GreenPAK2's own SCL_GP_U9; pins 3-4 do the same for SDA). Removing
 * those jumpers deliberately isolates GreenPAK2 from the bus so
 * GreenPAK1's slave address can be reprogrammed without GreenPAK2 also
 * responding/conflicting. Firmware talking to GreenPAK2 over I2C must
 * not assume it's reachable unconditionally -- whoever operates the
 * board controls that via the physical jumpers, not firmware.
 */
#pragma once

/* ---- LH5801-facing bus: 13-bit flat address, 8K window (0x8000-0x9FFF).
 * GPIO*n* = AD*n* for n=0..12, contiguous, verified pin-by-pin 2026-09-08. ---- */
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

/* ---- Data bus D0-D7: NOT contiguous. D0-D2 at GPIO16-18 (reverse order);
 * D3-D7 are separately at GPIO35-39 (ascending). Two register groups,
 * not one field. ---- */
#define PIN_D2 16
#define PIN_D1 17
#define PIN_D0 18
#define PIN_D3 35
#define PIN_D4 36
#define PIN_D5 37
#define PIN_D6 38
#define PIN_D7 39
/* Split OE masks for the two data-bus register groups. PIN_D3-D7 are
 * >= GPIO32, i.e. the RP2350B's separate "hi" SIO register bank
 * (sio_hw->gpio_hi_*, not sio_hw->gpio_*) -- a plain 32-bit mask
 * spanning both groups is invalid (shift >= type width for the hi
 * pins). Bit order within the assembled byte still needs explicit
 * per-pin assembly (see ReadDataIn/DriveData in monitor.c); these
 * masks are only for gpio_init()/OE set-clear, where per-bit order
 * doesn't matter. */
#define DATA_PINS_LO_MASK ((1u << PIN_D0) | (1u << PIN_D1) | (1u << PIN_D2))
#define DATA_PINS_HI_MASK ((1u << (PIN_D3 - 32)) | (1u << (PIN_D4 - 32)) | \
                            (1u << (PIN_D5 - 32)) | (1u << (PIN_D6 - 32)) | \
                            (1u << (PIN_D7 - 32)))

/* ---- GreenPAK trigger lines: combine CS+R/W+OE ("read requested, drive
 * data now") and CS+W ("write requested, latch data now") into two
 * unambiguous edges -- firmware doesn't compute CS&&RW&&OE itself. ---- */
#define PIN_TRIG_RD 30  /* CS && Read && OE asserted by the GreenPAK -- drive data bus now */
#define PIN_TRIG_WR 31  /* CS && Write asserted by the GreenPAK -- latch data bus now */

/* ---- GreenPAK1 (U3) I2C-style link, bit-banged (see greenpak_i2c.h),
 * routed through U10 (TCA9406 level translator) to VGG. GreenPAK1 owns
 * all SRAM control (address mux, /WE, CS) plus the PC-1500/1500A sense
 * circuit and a direct DME0 input; state is set/verified over this
 * link. GreenPAK2 has NO equivalent link -- see file header. ---- */
#define PIN_GREENPAK1_SDA 28
#define PIN_GREENPAK1_SCL 29

/* GreenPAK2 is NOT on separate GPIOs -- it shares GreenPAK1's exact
 * same SDA/SCL pins above, gated by the J7 shunt jumpers (see file
 * header). These aliases exist so main.c's two separate
 * greenpak_i2c_bus_t instances (g_greenpak1_bus/g_greenpak2_bus) still
 * compile; whether firmware SHOULD keep modeling this as two logical
 * buses on identical pins (vs. one shared bus + two I2C slave
 * addresses) is an open question, not resolved here -- flagged to the
 * user 2026-09-08, not changed unilaterally. */
#define PIN_GREENPAK2_SDA PIN_GREENPAK1_SDA
#define PIN_GREENPAK2_SCL PIN_GREENPAK1_SCL

/* GPIO25, GPIO26 are genuinely spare. */

/* ---- SD card. CS/SCK/MOSI correctly land on the real hardware SPI1
 * funcsel pins (GPIO41/42/43); MISO does NOT -- see KNOWN BUG above.
 * CS itself is toggled as a plain GPIO by the FatFs SD driver, not
 * through the hardware CSn line, but GPIO41 is used for it anyway
 * since it keeps the block visually contiguous. ---- */
#define PIN_SD_MISO 27  /* BUG: not on a SPI1 funcsel pin -- see file header */
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
#define PIN_BLE_WAKE    21

/* Spare: GPIO13-15, GPIO19-20, GPIO22-26, GPIO34, GPIO40 (unconnected,
 * see SD_MISO bug above), GPIO44-47. GPIO13/14/23/24/34 were TRIG_RD/
 * TRIG_WR/GREENPAK1_SDA/GREENPAK1_SCL/BLE_WAKE respectively until the
 * 2026-09-08 pin-swap pass moved each to a GPIO physically closer to
 * its real destination component (GreenPAK2 for the trigger lines,
 * U10 for the GreenPAK1 I2C link, the RN4871 BLE module for wake) --
 * GPIO15/19/20/44/45 were deliberately left untouched despite being
 * closer still, because they're the ones physically broken out to J5
 * as genuine spare-pin header access; consuming them for real signals
 * would defeat that header's purpose. */
