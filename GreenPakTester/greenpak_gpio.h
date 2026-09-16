/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_gpio.h
 *
 * Drive/release/read helpers over the 15 GreenPAK IO pins mapped in
 * greenpak_pins.h. Callers refer to pins by their physical GreenPAK
 * package pin number (2-7,10,12,13,15-20 for this wiring -- see
 * greenpak_pins.c's greenpak_io_map), not a 0..14 loop-friendly index
 * and not a raw Pico GPIO number, even though for this particular
 * wiring the Pico GPIO number happens to equal the package pin number.
 * Passing anything else (e.g. a bare 0..14 loop counter) hits
 * greenpak_gpio_for_io()'s assert/fallback instead of the intended pin
 * -- this has bitten this file once already (a stale cleanup loop that
 * silently skipped pins 15-20), so don't reintroduce it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Configures all 15 mapped GPIOs as floating inputs (safe default --
 * nothing is driven until a caller explicitly asks to). */
void greenpak_gpio_init_all(void);

/* Sets IO `io_index` as an output driving `level`. */
void greenpak_gpio_drive(uint8_t io_index, bool level);

/* Sets IO `io_index` back to a floating input, e.g. so the GreenPAK
 * itself can drive it as an output pin under test. No internal pull
 * is applied -- whether a pull is needed depends on whether GP1's
 * design configures that pin push-pull or open-drain, which isn't
 * decoded from the LUT; add one explicitly per-vector if a given test
 * needs it. Do NOT use this for a pin the bench itself drives as an
 * input to the GreenPAK (address bus, DME0, PV, R/W-from-bus, OD,
 * 1500A-sense) when you need it to settle to a firm, repeatable level
 * once released -- a bare floating CMOS input can sit at an
 * indeterminate level from breadboard crosstalk/residual charge
 * indefinitely; use greenpak_gpio_release_pulled_down() for those. */
void greenpak_gpio_release(uint8_t io_index);

/* Like greenpak_gpio_release(), but also engages the Pico's internal
 * weak pull-down, so a pin the bench itself drives as an input to the
 * GreenPAK settles to a firm, repeatable low once released instead of
 * floating at an indeterminate level. Only for such bench-driven-input
 * pins -- never for a GreenPAK *output* pin being read (CS, R/W to
 * SRAM, GP2's read/write triggers), since an open-drain output design
 * would rely on an external pull instead, which a Pico-side pull-down
 * would fight.
 *
 * KNOWN LIMITATION, confirmed on real hardware 2026-09-16: this alone
 * is not always enough. The RP2350 has a known erratum/behavior where
 * a pin that was actively driven and then switched to Hi-Z "bleeds"
 * residual/ghost voltage back onto the line, which the internal weak
 * pull (~50-80k ohm) doesn't fully suppress -- measured ~1.9V resting
 * between periodic 3.3V pulses on a pin this function was already
 * managing. A real discrete external pull-down resistor (much lower
 * value than the internal pull) on that net resolves it; confirmed by
 * testing. This function is still worth calling (belt-and-suspenders,
 * and it's the right thing for pins that DO get an external resistor
 * too), but don't assume it's sufficient on its own for a pin that's
 * repeatedly driven then released -- add the external resistor. */
void greenpak_gpio_release_pulled_down(uint8_t io_index);

/* Reads the current logic level of IO `io_index`. Meaningful only if
 * the pin is currently an input (i.e. after greenpak_gpio_release, or
 * before any greenpak_gpio_drive call). */
bool greenpak_gpio_read(uint8_t io_index);
