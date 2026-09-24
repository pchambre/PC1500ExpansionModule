/* board_pins.h
 *
 * GPIO assignment for the RP2350 (Pico 2 W) PC1500-Pico2W-Dongle board --
 * NOT the RP2350B internal-card board, which has a genuinely different
 * layout and its own board_pins.h/firmware.
 *
 * Confirmed 2026-09-16 against PC1500-Pico2W-Dongle.kicad_sch's own
 * netlist (U1, the Pico 2 W module socket) -- read directly from the
 * schematic, not inferred -- after this table's earlier, unbuilt/
 * theoretical version turned out to disagree with the real board in
 * several places at once (see the git history around this date for the
 * full incident: those disagreements are what was actively breaking the
 * PC-1500's own boot cycle whenever the Pico was present). Every pin
 * below is schematic ground truth for this board specifically.
 *
 * GreenPAK glue logic (external to the RP2350) does SRAM chip-select,
 * MCU chip-select, and combines CS+R/W+OE into the two trigger lines
 * below -- none of that logic has RP2350 GPIOs of its own; only its two
 * trigger outputs and the I2C link are wired to this chip.
 */
#pragma once

/* ---- LH5801-facing bus: 13-bit flat address, 8K window (0x8000-0x9FFF) ---- */
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

/* ---- Data bus D0-D7: GP13-GP20, one contiguous 8-bit field (through
 * U6, a TXS0108E level shifter, to the LH5801-side bus) -- schematic
 * nets D0_LV..D7_LV. NOT split around GPIO19 -- an earlier, unbuilt
 * version of this table assumed GPIO19 was reserved for a QMI-CS1 SD
 * card chip-select and routed D6/D7 around it onto GP20/21 instead; on
 * the real board GPIO19 is simply D6, GPIO21 is TRIG_RD (see below), and
 * the SD card isn't wired to RP2350 GPIO/QMI at all (see PIN_SD note
 * below) -- that earlier assumption had the firmware hijacking a live
 * data-bus line for QMI/XIP duty on every boot, and driving an output
 * onto TRIG_RD (an input the GreenPAK itself drives) every time a read
 * looked like it needed bit 7. ---- */
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

/* ---- GreenPAK trigger lines: combine CS+R+OE ("read requested, drive
 * data now") and CS+W ("write requested, latch data now") into two
 * unambiguous edges -- firmware no longer computes CS&&RW&&OE itself.
 * These double as POWMAN wake sources (PWRUP0/PWRUP1); no separate wake
 * pin is used. Schematic nets TRIG_RD_PICO/TRIG_WR_PICO -- swapped from
 * this table's earlier (22/26) assumption; GP26 in particular is really
 * the GreenPAK I2C SDA line below, which idles high (pulled up), so the
 * old PIN_TRIG_WR=26 made the bus loop's write-trigger wait spin forever
 * against an idle I2C bus instead of a real write cycle. ---- */
#define PIN_TRIG_RD 21  /* CS && Read && OE asserted by the GreenPAK -- drive data bus now */
#define PIN_TRIG_WR 22  /* CS && Write asserted by the GreenPAK -- latch data bus now */

/* ---- I2C-style link to the GreenPAK(s): general comms plus the
 * Control_Mode_Control-equivalent ROM-source mux select (SRAM vs MCU for
 * the 6K ROM window) that the PSoC5 design drove as a dedicated pin.
 * GreenPAK1 and GreenPAK2 share this one physical bus -- there is no
 * separate PIN_GREENPAK2_SDA/SCL wiring, matching RP2350B/board_pins.h's
 * same convention; the two chips are distinguished by I2C slave address
 * (each chip's NVM-configured control code), not by pin.
 *
 * GP26/GP27, NOT GP27/GP28 -- overridden from this file's original
 * (unbuilt, theoretical) 27/28 assignment to match the actual fabricated
 * PC1500-Pico2W-Dongle board: confirmed 2026-09-14 via
 * PC1500-Pico2W-Dongle.kicad_sch's netlist, U1 (the Pico 2 W module
 * socket) physical pin 31 -> net "SDA" (=GP26) and pin 32 -> net "SCL"
 * (=GP27) through U8 (TCA9406DC level shifter) to the GreenPAKs. This
 * was the root cause of a full-range (0x08-0x77) I2C recovery scan
 * finding nothing on real hardware: firmware was bit-banging GP27/GP28,
 * where GP27 is really the board's SCL line and GP28 is unconnected --
 * the real SDA line (GP26) was never toggled at all. ---- */
#define PIN_GREENPAK1_SDA 26
#define PIN_GREENPAK1_SCL 27
#define PIN_GREENPAK2_SDA PIN_GREENPAK1_SDA
#define PIN_GREENPAK2_SCL PIN_GREENPAK1_SCL

/* ---- SD card: NOT wired to any RP2350 GPIO or the QMI-CS1 peripheral
 * on this board -- an earlier, unbuilt version of this table assumed a
 * QMI-CS1-driven SD card fixed to GPIO19 (see lib/qmi_cs1_sdspi/), which
 * doesn't match the real schematic at all. The microSD socket (J2) is
 * instead wired to U5 (an SC18IS602B I2C-to-SPI bridge chip), and U5
 * itself sits on the SAME I2C bus as the GreenPAKs above (its SDA/SCL
 * pins go straight to PIN_GREENPAK1_SDA/SCL, i.e. GP26/GP27) -- SD
 * access on this board means driving U5 over I2C, not a native RP2350
 * SPI/QMI peripheral. See sc18is602b.h (the bridge primitive) and
 * diskio_sd_bridge.c (the FatFs driver built on it) -- lib/qmi_cs1_sdspi/
 * remains unused on this board and must not be wired up here. ---- */

/* U5's INT pin (active LOW, open-drain -- SC18IS602B datasheet section
 * 6.2/7.1.6): asserts automatically whenever an SPI transmission
 * completes. Bodge-wired from U5 pin 9 to GP28 (Pico module pin 34,
 * spare header J3 pin 1) on 2026-09-20 -- confirmed physically connected.
 *
 * RE-ENABLED 2026-09-21, after a full re-investigation with the actual
 * NXP datasheet in hand (rev 7, 21 Oct 2019) and direct hardware
 * isolation -- the real mechanism was found: **reading the captured
 * buffer is what deasserts INT, not the Clear Interrupt command (0xF1)**.
 * Confirmed live: a write-only transaction left INT asserted even after
 * Clear Interrupt was sent; a bare I2C read of the buffer (no new SPI
 * write, no Clear Interrupt at all) flipped it to idle immediately.
 * Neither the datasheet's own text nor its worked example (section 8)
 * isolates a write-only-then-clear-with-no-read case, which is why every
 * earlier round of this investigation (working from the text, not this
 * direct isolation) missed it -- see sc18is602b.c's own INT section
 * comment for how the driver now accounts for this. Verified at both
 * 115 kHz (datasheet reference) and 1.8432 MHz (this board's real bulk
 * SPI clock, unchanged) with identical behavior, plus 5/5 clean full
 * real-SD-card round-trip runs (512B-32KB, raw sectors) across
 * independent power cycles with INT enabled throughout. I2C stays at its
 * existing 400 kHz Fast Mode config -- unrelated, separate clock domain
 * from this SPI-side pin. */
#define PIN_SD_BRIDGE_INT 28
