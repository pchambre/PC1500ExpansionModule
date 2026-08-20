/* board_pins.h
 *
 * GPIO assignment for the RP2350 (Pico 2 W) expansion board redesign.
 *
 * This is the closed, budgeted pin table worked out against real Pico 2 W
 * constraints (see the session's design discussion / the repo's plan
 * history for the full reasoning): GP23/24/25/29 are wired on the Pico 2 W
 * module itself to the onboard CYW43439 and are not available for
 * anything else, leaving exactly 26 usable GPIOs -- and this table uses
 * all 26, with zero spare.
 *
 * The physical *routing* (which pad goes where on an actual PCB) is still
 * unconfirmed -- there is no RP2350 KiCad schematic for this board yet,
 * that's separate future work. But the pin *count*, *roles*, and (for
 * SD_CS1 specifically) the *silicon constraint* are settled, not
 * placeholders to be reshuffled later:
 *   - SD_CS1 MUST be GPIO 0, 8, or 19 on the RP2350A package (the only
 *     pins with the GPIO_FUNC_XIP_CS1 alternate function on this
 *     package -- RP2350B's GPIO47 doesn't exist here). This table uses
 *     GPIO 19.
 *   - Every other signal could in principle move to a different GPIO
 *     number without changing anything else, since they're all plain
 *     digital I/O with no silicon-level pin restriction.
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

/* ---- Data bus D0-D7 (not contiguous with the address bus -- SD_CS1's
 * fixed GPIO19 sits in the middle of what would otherwise be one run) ---- */
#define PIN_D0 13
#define PIN_D1 14
#define PIN_D2 15
#define PIN_D3 16
#define PIN_D4 17
#define PIN_D5 18
#define PIN_D6 20
#define PIN_D7 21
/* D0..D5 are contiguous (GPIO13-18); D6/D7 (GPIO20/21) are read/written
 * as a separate 2-bit field and combined in software -- see monitor.c. */
#define DATA_PIN_LOW_BASE  PIN_D0
#define DATA_PIN_LOW_COUNT 6
#define DATA_PIN_HIGH_BASE PIN_D6
#define DATA_PIN_HIGH_COUNT 2

/* ---- GreenPAK trigger lines: combine CS+R+OE ("read requested, drive
 * data now") and CS+W ("write requested, latch data now") into two
 * unambiguous edges -- firmware no longer computes CS&&RW&&OE itself.
 * These double as POWMAN wake sources (PWRUP0/PWRUP1); no separate wake
 * pin is used. ---- */
#define PIN_TRIG_RD 22  /* CS && Read && OE asserted by the GreenPAK -- drive data bus now */
#define PIN_TRIG_WR 26  /* CS && Write asserted by the GreenPAK -- latch data bus now */

/* ---- I2C-style link to the GreenPAK(s): general comms plus the
 * Control_Mode_Control-equivalent ROM-source mux select (SRAM vs MCU for
 * the 6K ROM window) that the PSoC5 design drove as a dedicated pin. ---- */
#define PIN_GREENPAK_SDA 27
#define PIN_GREENPAK_SCL 28

/* ---- SD card via QMI CS1 (shares SCLK/SD0/SD1 with the onboard flash's
 * own CS0 -- see RP2350/lib/qmi_cs1_sdspi/README.md). Fixed by silicon to
 * one of GPIO 0/8/19 on RP2350A; this board uses 19. ---- */
#define PIN_SD_CS1 19
