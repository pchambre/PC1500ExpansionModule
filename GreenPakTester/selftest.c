/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "selftest.h"
#include "gp1_model.h"
#include "gp2_model.h"
#include "greenpak_gpio.h"
#include "greenpak_pins.h"
#include "greenpak_virtual_io.h"

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"

bool *get_bool_levels(uint32_t input_value, uint8_t input_count) {
    bool *levels = malloc((size_t)input_count * sizeof(bool));
    for (uint8_t j = 0; j < input_count; j++) {
        levels[j] = (input_value & (1u << (input_count - 1 - j))) != 0;
    }
    return levels;
}

/* Release every mapped pin. Pins the bench itself drove as an input to
 * the GreenPAK (address bus, DME0, PV, R/W-from-bus, OD, 1500A-sense)
 * get a pull-down so they settle to a firm, repeatable low instead of
 * floating at whatever level breadboard crosstalk or residual charge
 * leaves them at -- a bare release left DME0 "dimly" non-zero, enough to
 * make the very next selftest_run()'s Initial State Test misread the
 * decode as open. The design's own output pins are left plain-floating
 * instead, since a Pico-side pull-down would fight an open-drain design
 * relying on an external pull. */
static void selftest_release_all(const selftest_outputs_t *outputs) {
    for (uint8_t i = 0; i < GREENPAK_IO_COUNT; i++) {
        uint8_t pin = greenpak_io_map[i].greenpak_pin;
        bool is_dut_output = false;
        for (uint8_t k = 0; k < outputs->count; k++) {
            if (outputs->pins[k] == pin) {
                is_dut_output = true;
                break;
            }
        }
        if (is_dut_output) {
            greenpak_gpio_release(pin);
        } else {
            greenpak_gpio_release_pulled_down(pin);
        }
    }
}

const selftest_outputs_t gp1_outputs = {
    (const uint8_t[]){ 10, 12, 13, 15, 16, 18 }, 6 };  /* SRAM AD11-AD14, R/W to SRAM, CS */
const selftest_outputs_t gp1_card_outputs = {
    (const uint8_t[]){ 12, 13, 15, 16, 18 }, 5 };      /* SRAM AD14-AD16, R/W to SRAM, CS */
const selftest_outputs_t gp2_outputs = {
    (const uint8_t[]){ 10, 12 }, 2 };                  /* read/write triggers */
const selftest_outputs_t gp2_card_outputs = {
    (const uint8_t[]){ 10, 12, 16 }, 3 };              /* read/write triggers, INHIBIT */

selftest_result_t selftest_run(const selftest_vector_t *vectors, uint32_t count,
                               const selftest_outputs_t *outputs) {
    selftest_result_t result = { 0, 0, 0 };

    for (uint32_t v = 0; v < count; v++) {
        const selftest_vector_t *t = &vectors[v];
        bool *vi_levels = get_bool_levels(t->vi_levels, t->vi_count);
        bool *input_levels = get_bool_levels(t->input_levels, t->input_count);
        bool *expected_levels = get_bool_levels(t->expected_levels, t->output_count);

        printf ("Running vector: %s\n", t->name);

        bool vi_ok = true;
        for (uint8_t i = 0; i < t->vi_count; i++) {
            bool ok = greenpak_virtual_input_set(t->vi_ios[i], vi_levels[i]);
            vi_ok = vi_ok && ok;
            printf("  VI%u: writing=%d %s\n",
                   t->vi_ios[i], vi_levels[i], ok ? "OK" : "I2C FAIL");
        }

        for (uint8_t i = 0; i < t->output_count; i++) {
            greenpak_gpio_release(t->output_ios[i]);
        }

        for (uint8_t i = 0; i < t->input_count; i++) {
            printf("  IO%u: writing=%d\n",
                   t->input_ios[i], input_levels[i]);
        }        

        for (uint8_t i = 0; i < t->input_count; i++) {
            greenpak_gpio_drive(t->input_ios[i], input_levels[i]);
        }

        //sleep_us(5000000);
        sleep_us(t->settle_us);

        bool vector_ok = vi_ok;
        for (uint8_t i = 0; i < t->output_count; i++) {
            bool actual = greenpak_gpio_read(t->output_ios[i]);
            bool ok = (actual == expected_levels[i]);
            vector_ok = vector_ok && ok;
            printf("  IO%u: expected=%d actual=%d %s\n",
                   t->output_ios[i], expected_levels[i], actual, ok ? "OK" : "MISMATCH");
        }
        printf("[%s] %s\n\n", vector_ok ? "PASS" : "FAIL", t->name);

        free(vi_levels);
        free(input_levels);
        free(expected_levels);

        result.total++;
        if (vector_ok) result.passed++; else result.failed++;
    }

    printf("Self-test complete: %u/%u passed\n", result.passed, result.total);

    selftest_release_all(outputs);
    return result;
}

#define SWEEP_SHOW_MAX 20u

selftest_result_t selftest_sweep(const selftest_sweep_t *s, const selftest_outputs_t *outputs) {
    selftest_result_t result = { 0, 0, 0 };
    uint32_t vi_combos = 1u << s->vi_count;
    uint32_t in_combos = 1u << s->input_count;
    uint32_t shown = 0;

    printf("Sweep: %s -- %u combinations (virtual inputs, LSB first: %s)\n",
           s->name, vi_combos * in_combos, s->vi_legend);

    for (uint8_t o = 0; o < s->output_count; o++) {
        greenpak_gpio_release(s->output_pins[o]);
    }

    uint32_t written_vi = 0;
    for (uint32_t vi = 0; vi < vi_combos; vi++) {
        /* Only the bits that changed go over I2C. */
        bool vi_ok = true;
        for (uint8_t b = 0; b < s->vi_count; b++) {
            bool level = (vi >> b) & 1u;
            if (vi == 0 || level != ((written_vi >> b) & 1u)) {
                vi_ok = greenpak_virtual_input_set(s->vi_ios[b], level) && vi_ok;
            }
        }
        written_vi = vi;
        if (!vi_ok) {
            printf("  vi=0x%02X: I2C FAIL -- sweep stopped\n", (unsigned)vi);
            result.failed++;
            break;
        }

        for (uint32_t in = 0; in < in_combos; in++) {
            for (uint8_t b = 0; b < s->input_count; b++) {
                greenpak_gpio_drive(s->input_pins[b], (in >> b) & 1u);
            }
            sleep_us(s->settle_us);

            uint32_t actual = 0;
            for (uint8_t o = 0; o < s->output_count; o++) {
                if (greenpak_gpio_read(s->output_pins[o])) actual |= 1u << o;
            }
            uint32_t care;
            uint32_t expected = s->model(in, vi, &care);
            uint32_t diff = (actual ^ expected) & care;

            result.total++;
            if (diff == 0) {
                result.passed++;
                continue;
            }
            result.failed++;
            if (shown++ < SWEEP_SHOW_MAX) {
                /* Every model puts AD11-AD15 in bits 0-4: shown as the address. */
                printf("  MISMATCH %04XH", (unsigned)((in & 0x1Fu) << 11));
                for (uint8_t b = 5; b < s->input_count; b++)
                    printf(" %s=%u", s->input_names[b], (unsigned)((in >> b) & 1u));
                printf(" vi=0x%02X:", (unsigned)vi);
                for (uint8_t o = 0; o < s->output_count; o++) {
                    if ((diff >> o) & 1u) {
                        printf(" %s=%u (want %u)", s->output_names[o],
                               (unsigned)((actual >> o) & 1u), (unsigned)((expected >> o) & 1u));
                    }
                }
                printf("\n");
            }
        }
    }
    if (shown > SWEEP_SHOW_MAX) {
        printf("  ... %u more mismatches not shown\n", (unsigned)(shown - SWEEP_SHOW_MAX));
    }
    printf("[%s] Sweep %s: %u/%u combinations match\n\n",
           result.failed == 0 ? "PASS" : "FAIL", s->name,
           (unsigned)result.passed, (unsigned)result.total);

    selftest_release_all(outputs);
    return result;
}
 
/* EXAMPLE / PLACEHOLDER ONLY -- not derived from GP1's actual design.
 * Demonstrates the vector-table shape until real truth-table data is
 * available (see README.md). Drives IO0 high and IO1 low, then checks
 * IO6 reads high -- fabricated numbers, not real GP1 logic. Replace
 * before relying on selftest results for anything. */
static const uint8_t example_inputs[]    = { 0, 1 };
static const bool    example_levels[]    = { true, false };
static const uint8_t example_outputs[]   = { 6 };
static const bool    example_expected[]  = { true };
/*  Address inputs start with AD15-AD11 and DME0 (pin 20)
    ROM range adds PV (pin 17)
    1500A range adds pin 19 (low for PC-1500A)
*/
static const uint8_t address_inputs[]    = { 6, 5, 4, 3, 2, 20 };
static const uint8_t address_inputs_rom_range[]    = { 6, 5, 4, 3, 2, 20, 17 };
static const uint8_t address_inputs_1500A_range[]    = { 6, 5, 4, 3, 2, 20, 19 };
// address outputs is AD14-AD11 for the SRAM, plus CS (active low, pin 18)
static const uint8_t address_outputs[]   = { 15, 13, 12, 10, 18 };
static const uint8_t cs_output[]   = { 18 };
// R/W write-protect: R/W from bus (pin7) passes through to R/W to SRAM
// (pin16) unless AD15 is high and WE (virtual input 0) is low.
static const uint8_t rw_inputs[]   = { 6, 7 };
static const uint8_t rw_output[]   = { 16 };

const selftest_vector_t test_vectors[] = {
    {
        /* Power-up / bring-up sanity check: with every GreenPAK input
         * left floating (no vi_ios, no input_ios), CS must default
         * high/inactive rather than spuriously asserting from an
         * undefined floating combination. Must run first in this array
         * -- relies on greenpak_gpio_init_all() (main.c) / the previous
         * selftest_run()'s trailing greenpak_gpio_release() loop having
         * left every IO floating already, not on anything this vector
         * itself drives. */
        .name = "Initial State Test - all inputs floating, CS stays high",
        .output_ios = cs_output,
        .expected_levels = 0x1,
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Address Test 1 - 16K first page",
        .input_ios = address_inputs,
        .input_levels = 0x1, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x0,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 2 - 16K second page",
        .input_ios = address_inputs,
        .input_levels = 0x3, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x2,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 3 - 16K sixth page",
        .input_ios = address_inputs,
        .input_levels = 0xb, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0xa,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        /* 0000H-3FFFH is pure passthrough (AD15,AD14=00 forces
         * SRAM_AD14=0 and collapses the other three equations to
         * SRAM_AD13:11 = AD13:11 exactly), and CS1 asserts across the
         * whole range regardless of AD13:11 -- so for every one of
         * these 8 pages, input_levels == expected_levels | 0x1 == (page*2)|0x1
         * (AD13:11 occupy bits 3:1, DME0=bit0=1 since DME0 is active-high
         * and must be driven high to keep the decode gate open, and
         * address_outputs' AD13:11 occupy the same bit positions with
         * CS=bit0=0). */
        .name = "Address Test 3b - 16K third page (1000H)",
        .input_ios = address_inputs,
        .input_levels = 0x5, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x4,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 3c - 16K fourth page (1800H)",
        .input_ios = address_inputs,
        .input_levels = 0x7, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x6,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 3d - 16K fifth page (2000H)",
        .input_ios = address_inputs,
        .input_levels = 0x9, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x8,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 3e - 16K seventh page (3000H)",
        .input_ios = address_inputs,
        .input_levels = 0xD, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0xC,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 3f - 16K eighth page (3800H)",
        .input_ios = address_inputs,
        .input_levels = 0xF, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0xE,
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 4 - skipped page at 4000H",
        .input_ios = address_inputs,
        .input_levels = 0x11, /* DME0=1 (gate open) */
        .input_count = 6,
        .output_ios = cs_output,
        .expected_levels = 0x1,
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Address Test 5 - skipped first page if DME0 is low",
        .input_ios = address_inputs,
        .input_levels = 0x0, /* DME0=0 (gate closed -- DME0 is active-high) */
        .input_count = 6,
        .output_ios = cs_output,
        .expected_levels = 0x1,
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Virtual Input Test 1 - WE/Remap low, CS never asserts with AD15 high",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_WE, GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 2,
        .input_ios = address_inputs,
        .input_levels = 0x21, /* AD15=1, DME0=1 (gate open), rest 0 */
        .input_count = 6,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* CS stays high/inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Virtual Input Test 2 - Remap high, CS asserts for 10001 with PV low, DME0 high",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = address_inputs_rom_range,
        .input_levels = 0x46, /* AD15,AD14,AD13,AD12,AD11,DME0,PV = 1,0,0,0,1,1,0 */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x0, /* CS asserts/low */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Address Test 6 - 5800H 2K window (PDCS3)",
        .input_ios = address_inputs,
        .input_levels = 0x17, /* AD15-11,DME0 = 0,1,0,1,1,1 */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x14, /* AD14,13,12,11,CS = 1,0,1,0,0(asserted) */
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 7 - 6000H 2K window (CS4)",
        .input_ios = address_inputs,
        .input_levels = 0x19, /* 0,1,1,0,0,1 */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x16, /* 1,0,1,1,0(asserted) */
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "Address Test 8 - 6800H 2K window (CS4)",
        .input_ios = address_inputs,
        .input_levels = 0x1B, /* 0,1,1,0,1,1 */
        .input_count = 6,
        .output_ios = address_outputs,
        .expected_levels = 0x18, /* 1,1,0,0,0(asserted) */
        .output_count = 5,
        .settle_us = 100,
    },
    {
        /* CS2 = ExpRAM1(010) & (AD12,AD11 in {01,10}) & 1500A & DME0.
         * "1500A" here follows this file's existing pin-19 convention
         * (see address_inputs_1500A_range comment / CS-for-the-SRAM doc
         * note "1500A is low for a PC1500A"): pin19=1 means NOT a
         * PC-1500A, i.e. the condition that enables CS2. DME0 is
         * active-high, so the gate term is DME0 (not !DME0). */
        .name = "CS2 Test 1 - 4800H enabled on non-1500A",
        .input_ios = address_inputs_1500A_range,
        .input_levels = 0x27, /* AD15-11,DME0,1500A = 0,1,0,0,1,1,1 */
        .input_count = 7,
        .output_ios = address_outputs,
        .expected_levels = 0x10, /* AD14,13,12,11,CS = 1,0,0,0,0(asserted) */
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "CS2 Test 2 - 5000H enabled on non-1500A",
        .input_ios = address_inputs_1500A_range,
        .input_levels = 0x2B, /* 0,1,0,1,0,1,1 */
        .input_count = 7,
        .output_ios = address_outputs,
        .expected_levels = 0x12, /* AD14,13,12,11,CS = 1,0,0,1,0(asserted) */
        .output_count = 5,
        .settle_us = 100,
    },
    {
        .name = "CS2 Test 3 - 4800H disabled on PC-1500A",
        .input_ios = address_inputs_1500A_range,
        .input_levels = 0x26, /* 0,1,0,0,1,1,0 -- pin19=0, PC-1500A */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* stays inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "CS4 Test - AD12 high excludes 7000H range",
        .input_ios = address_inputs,
        .input_levels = 0x1D, /* 0,1,1,1,0,1 */
        .input_count = 6,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* stays inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "ROM Test 1 - 9000H enabled with Remap high",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = address_inputs_rom_range,
        .input_levels = 0x4A, /* AD15-11,DME0,PV = 1,0,0,1,0,1,0 */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x0, /* asserted */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "ROM Test 2 - 8000H data interchange window excluded",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = address_inputs_rom_range,
        .input_levels = 0x42, /* 1,0,0,0,0,1,0 -- AD12,AD11 = 00 */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* stays inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "ROM Test 3 - 8800H blocked by PV high",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = address_inputs_rom_range,
        .input_levels = 0x47, /* 1,0,0,0,1,1,1 -- PV=1 */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* stays inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "ROM Test 4 - 8800H blocked by Remap low",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = address_inputs_rom_range,
        .input_levels = 0x46, /* 1,0,0,0,1,1,0 */
        .input_count = 7,
        .output_ios = cs_output,
        .expected_levels = 0x1, /* stays inactive */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Write Protect Test 1 - AD15 high, WE high allows write",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_WE },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = rw_inputs,
        .input_levels = 0x2, /* AD15,R/W = 1,0 (write requested) */
        .input_count = 2,
        .output_ios = rw_output,
        .expected_levels = 0x0, /* R/W to SRAM passes through low */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Write Protect Test 2 - AD15 high, WE low blocks write",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_WE },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = rw_inputs,
        .input_levels = 0x2, /* AD15,R/W = 1,0 (write requested) */
        .input_count = 2,
        .output_ios = rw_output,
        .expected_levels = 0x1, /* forced high -- write inhibited */
        .output_count = 1,
        .settle_us = 100,
    },
    {
        .name = "Write Protect Test 3 - AD15 low, R/W passes through regardless of WE",
        .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_WE },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = rw_inputs,
        .input_levels = 0x0, /* AD15,R/W = 0,0 */
        .input_count = 2,
        .output_ios = rw_output,
        .expected_levels = 0x0,
        .output_count = 1,
        .settle_us = 100,
    },
};

const uint32_t test_vectors_count = sizeof(test_vectors) / sizeof(test_vectors[0]);

/* ================== GP2 ==================
 * read_trigger's formula, from the GreenPAK design doc's "GP2" section
 * (still believed current -- not touched by the 2026-09-23 change below):
 *   100 = ROM                          (AD15,AD14,AD13 = 100, i.e. 8000H-9FFFH)
 *   CS = ROM & (!Remap | AD12,AD11==00) & !PV & DME0
 *   read_trigger  = CS & R/W & !OD
 *
 * DME0 is active-high, so the gate term is DME0 (not !DME0) -- CS
 * requires DME0 driven high, and goes inactive when DME0 is low.
 *
 * CS itself isn't brought to a pin on GP2 (unlike GP1's IO12) -- it's
 * only observable through its effect on the two trigger outputs. The
 * "!Remap | AD12,AD11==00" term matters because AD12,AD11==00 is the
 * 8000H-87FFH data-interchange window (see the doc's "ROM/SRAM
 * toggles" section): that window must trigger the MCU unconditionally
 * (it's how the ROM-to-SRAM copy routine stages data), while the rest
 * of the ROM range (8800H-9FFFH) should only trigger the MCU while
 * Remap is off (ROM served by MCU bit-banging) and stop triggering once
 * Remap is on (ROM served directly by SRAM hardware) -- this is the
 * "GP2 will...suppress read triggers for the ROM range when SRAM is
 * serving ROM" behavior described in that section.
 *
 * write_trigger -- CHANGED 2026-09-23 (board owner's own GP2-MCU.gp6
 * update, made to stop the RP2350 from accepting writes into the
 * MCU-served 8800H-9FFFH ROM range, which should be read-only -- see
 * PC1500-PSOC5's own MEMORY_MAP_8000_9FFF.md and the write_serve.pio
 * write-protect gate added the same day for the software-side half of
 * this fix). The address term is no longer the whole ROM range with a
 * Remap-based OR -- it's now exactly 8000H-87FFH, unconditionally:
 *   write_trigger = (AD15,14,13,12,11 == 10000) & <rest unconfirmed>
 * Confirmed directly from the board owner's own real-hardware
 * observation: a write to 8800H, under the exact same DME0/R/W/OD
 * conditions Test 8/10 already used to test the OLD "write=1" behavior,
 * must now read as write=0 -- Tests 8/10 below were updated accordingly,
 * and Tests 11/12 added for the still-valid in-range case, mirroring the
 * SAME address/DME0/OD combinations so this is a direct A/B pair, not a
 * new set of assumptions. What's genuinely NOT confirmed yet: whether
 * DME0/PV/Remap still gate write_trigger the way they gate CS/read_trigger
 * above, or whether the new design dropped some of those too -- every
 * vector below keeps DME0=1/PV=0/Remap=0 (the conditions already known to
 * be favorable under the OLD formula) rather than assuming they still
 * matter one way or the other. Re-derive from GP2-MCU.gp6 properly (or
 * ask) before trusting a claim about those specific terms. */
static const uint8_t gp2_inputs[] = { 6, 5, 4, 3, 2, 13, 17, 7, 15 }; /* AD15,14,13,12,11,DME0,PV,R/W,OD */
static const uint8_t gp2_trigger_outputs[] = { 10, 12 };              /* read trigger, write trigger to MCU */

const selftest_vector_t test_vectors_gp2[] = {
    {
        .name = "GP2 Test 1 - 8800H (not interchange window), Remap off: read triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x11A, /* AD15-11,DME0,PV,R/W,OD = 1,0,0,0,1,1,0,1,0 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x2, /* read=1, write=0 */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 2 - 8000H (interchange window), Remap off: read triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x10A, /* 1,0,0,0,0,1,0,1,0 -- AD12,AD11=00 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x2,
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 3 - 8000H (interchange window), Remap ON: still triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x10A, /* same address as Test 2 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x2, /* unconditional -- data interchange window always triggers */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 4 - 8800H (not interchange window), Remap ON: trigger suppressed",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x1,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x11A, /* same address as Test 1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0, /* SRAM now serves this range directly -- no MCU trigger */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 5 - outside ROM range: never triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0xA, /* 0000H, DME0=1 (gate open), R/W=1, OD=0 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0,
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 6 - PV high blocks CS/triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x11E, /* 8800H + PV=1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0,
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 7 - DME0 low blocks CS/triggers",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x112, /* 8800H + DME0=0 (DME0 is active-high, so low blocks) */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0,
        .output_count = 2,
        .settle_us = 100,
    },
    {
        /* 2026-09-23: write_trigger's own address decode was narrowed in
         * GP2-MCU.gp6 (board owner's own change) from the whole ROM range
         * to exactly 8000H-87FFH (AD15=1, AD14:11=0000) -- 8800H no longer
         * asserts write_trigger at all, regardless of R/W/DME0/OD. See
         * Test 11/12 below for the still-valid in-range case. */
        .name = "GP2 Test 8 - write trigger does NOT assert for 8800H (outside 8000H-87FFH)",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x118, /* 8800H, DME0=1 (gate open), R/W=0, OD=0 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0, /* read=0, write=0 -- 8800H is outside the write window now */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 9 - OD high blocks read trigger",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x11B, /* 8800H, DME0=1 (gate open), R/W=1, OD=1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0,
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 10 - write trigger still suppressed for 8800H regardless of OD",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x119, /* 8800H, DME0=1 (gate open), R/W=0, OD=1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x0, /* still write=0 -- address alone decides this now, not OD */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 11 - write trigger asserts within 8000H-87FFH window",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x108, /* 8000H, DME0=1 (gate open), R/W=0, OD=0 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x1, /* read=0, write=1 -- inside 8000H-87FFH */
        .output_count = 2,
        .settle_us = 100,
    },
    {
        .name = "GP2 Test 12 - write trigger within 8000H-87FFH unaffected by OD",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x109, /* 8000H, DME0=1 (gate open), R/W=0, OD=1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x1, /* still write=1 -- OD only ever affected read */
        .output_count = 2,
        .settle_us = 100,
    },
};

const uint32_t test_vectors_gp2_count = sizeof(test_vectors_gp2) / sizeof(test_vectors_gp2[0]);

/* ================== GP1 sweeps ==================
 * Model bit n -> chip pin / virtual input n (see gp1_model.h). Both GP1
 * designs share the input pins: AD11-AD15, R/W, PV, IO13, DME0. */
static const uint8_t gp1_sweep_inputs[] = { 2, 3, 4, 5, 6, 7, 17, 19, 20 };
static const char *const gp1_sweep_input_names[] = { "AD11", "AD12", "AD13", "AD14", "AD15", "R/W", "PV", "IO13", "DME0" };

const selftest_sweep_t gp1_sweep = {
    .name = "GP1 (dongle)",
    .vi_legend = "WE, Remap",
    .input_pins = gp1_sweep_inputs,
    .input_names = gp1_sweep_input_names,
    .input_count = GP1M_IN_COUNT,
    .vi_ios = (const uint8_t[]){ GP1_VI_SRAM_ROM_WE, GP1_VI_SRAM_ROM_REMAP },
    .vi_count = GP1M_DVI_COUNT,
    .output_pins = (const uint8_t[]){ 18, 16, 10, 12, 13, 15 },
    .output_names = (const char *const[]){ "CS", "R/W", "SRAM_AD11", "SRAM_AD12", "SRAM_AD13", "SRAM_AD14" },
    .output_count = GP1M_DOUT_COUNT,
    .model = gp1_dongle_model,
    .settle_us = 10,
};

const selftest_sweep_t gp1_card_sweep = {
    .name = "GP1 (internal card)",
    .vi_legend = "WE, Remap, V14, V15, V16, BASE_ROM_STAGED",
    .input_pins = gp1_sweep_inputs,
    .input_names = gp1_sweep_input_names,
    .input_count = GP1M_IN_COUNT,
    .vi_ios = (const uint8_t[]){ GP1C_VI_SRAM_ROM_WE, GP1C_VI_SRAM_ROM_REMAP,
                                 GP1C_VI_V14, GP1C_VI_V15, GP1C_VI_V16, GP1C_VI_BASE_ROM_STAGED },
    .vi_count = GP1M_CVI_COUNT,
    .output_pins = (const uint8_t[]){ 18, 16, 12, 13, 15 },
    .output_names = (const char *const[]){ "CS", "R/W", "SRAM_AD14", "SRAM_AD15", "SRAM_AD16" },
    .output_count = GP1M_COUT_COUNT,
    .model = gp1_card_model,
    .settle_us = 10,
};

/* ================== GP1 (internal card) ==================
 * GP1-SRAM-InternalCard.gp6: 128K SRAM, GP1 drives SRAM AD14-AD16 and
 * AD0-AD13 are wired straight through. Bus 0000H-3FFFH goes wherever
 * V16-V14 say (base 000, bank n 1nn, staging window 011 onto the C000H
 * copy); 4000H-FFFFH map to the same SRAM address. CS:
 *   0000H-3FFFH, the 4800H-6FFFH windows (4800H/5000H only with IO13
 *   high), 8800H-9FFFH with Remap and PV low, C000H-FFFFH with
 *   BASE_ROM_STAGED (PV doesn't matter) -- all gated by DME0
 *   (active-high).
 * R/W to SRAM blocks writes at 8000H-FFFFH unless WE is set. INHIBIT
 * belongs to the card's GP2 (BASE_ROM_REPLACE), not this chip.
 *
 * Every vector sets every input, MSB first in these orders: */
static const uint8_t card_inputs[]  = { 6, 5, 4, 3, 2, 20, 17, 19, 7 }; /* AD15-AD11, DME0, PV, IO13, R/W */
static const uint8_t card_vis[]     = { GP1C_VI_SRAM_ROM_WE, GP1C_VI_SRAM_ROM_REMAP,
                                        GP1C_VI_V16, GP1C_VI_V15, GP1C_VI_V14, GP1C_VI_BASE_ROM_STAGED };
static const uint8_t card_outputs[] = { 15, 13, 12, 18, 16 };          /* SRAM AD16-AD14, CS, R/W */

/* SRAM AD16-AD14 values: V16-V14 encodings for bus 0000H-3FFFH, and
 * what the rest of the bus maps to. */
#define CARD_SRAM_BASE     0u
#define CARD_SRAM_BANK(n)  (4u | (n))
#define CARD_SRAM_STAGEWIN 3u
#define CARD_SRAM_4000     1u
#define CARD_SRAM_8000     2u
#define CARD_SRAM_C000     3u

#define CARD_IN(addr, dme0, pv, p, rw) \
    ((((uint32_t)(addr) >> 11) & 0x1Fu) << 4 | (dme0) << 3 | (pv) << 2 | (p) << 1 | (rw))
#define CARD_VI(we, remap, hi, staged) ((we) << 5 | (remap) << 4 | ((hi) & 7u) << 1 | (staged))
#define CARD_OUT(hi, cs, rw)           (((hi) & 7u) << 2 | (cs) << 1 | (rw))

/* Reads with DME0 high, PV low, IO13 high unless a vector says otherwise. */
#define CARD_READ(addr) CARD_IN(addr, 1, 0, 1, 1)

#define CARD_VECTOR(nm, vi, in, out) {                                    \
        .name = nm,                                                       \
        .vi_ios = card_vis, .vi_levels = (vi), .vi_count = 6,             \
        .input_ios = card_inputs, .input_levels = (in), .input_count = 9, \
        .output_ios = card_outputs, .expected_levels = (out), .output_count = 5, \
        .settle_us = 100,                                                 \
    }

const selftest_vector_t test_vectors_gp1_card[] = {
    {
        /* Same bring-up check as the dongle suite: nothing driven, CS
         * must not assert. Must stay first. */
        .name = "Card Initial State - all inputs floating, CS stays high",
        .output_ios = cs_output,
        .expected_levels = 0x1,
        .output_count = 1,
        .settle_us = 100,
    },

    /* Bus 0000H-3FFFH: SRAM AD16-AD14 come from V16-V14. */
    CARD_VECTOR("Card 0000H base", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x0000), CARD_OUT(CARD_SRAM_BASE, 0, 1)),
    CARD_VECTOR("Card 3800H base", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x3800), CARD_OUT(CARD_SRAM_BASE, 0, 1)),
    CARD_VECTOR("Card 0000H bank 0", CARD_VI(0, 0, CARD_SRAM_BANK(0), 0), CARD_READ(0x0000), CARD_OUT(CARD_SRAM_BANK(0), 0, 1)),
    CARD_VECTOR("Card 1000H bank 1", CARD_VI(0, 0, CARD_SRAM_BANK(1), 0), CARD_READ(0x1000), CARD_OUT(CARD_SRAM_BANK(1), 0, 1)),
    CARD_VECTOR("Card 2000H bank 2", CARD_VI(0, 0, CARD_SRAM_BANK(2), 0), CARD_READ(0x2000), CARD_OUT(CARD_SRAM_BANK(2), 0, 1)),
    CARD_VECTOR("Card 3800H bank 3", CARD_VI(0, 0, CARD_SRAM_BANK(3), 0), CARD_READ(0x3800), CARD_OUT(CARD_SRAM_BANK(3), 0, 1)),
    CARD_VECTOR("Card 0000H staging window", CARD_VI(0, 0, CARD_SRAM_STAGEWIN, 0), CARD_READ(0x0000),
                CARD_OUT(CARD_SRAM_STAGEWIN, 0, 1)),
    CARD_VECTOR("Card staging write at 0000H passes with WE low", CARD_VI(0, 0, CARD_SRAM_STAGEWIN, 0),
                CARD_IN(0x0000, 1, 0, 1, 0), CARD_OUT(CARD_SRAM_STAGEWIN, 0, 0)),

    /* 4000H-7FFFH: straight through, CS only in the windows. */
    CARD_VECTOR("Card 4000H not ours", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x4000), CARD_OUT(CARD_SRAM_4000, 1, 1)),
    CARD_VECTOR("Card 4800H with IO13 high", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x4800, 1, 0, 1, 1),
                CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card 4800H with IO13 low", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x4800, 1, 0, 0, 1),
                CARD_OUT(CARD_SRAM_4000, 1, 1)),
    CARD_VECTOR("Card 5000H with IO13 high", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x5000, 1, 0, 1, 1),
                CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card 5000H with IO13 low", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x5000, 1, 0, 0, 1),
                CARD_OUT(CARD_SRAM_4000, 1, 1)),
    CARD_VECTOR("Card 5800H with IO13 low still asserts", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x5800, 1, 0, 0, 1),
                CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card 6000H", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x6000), CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card 6800H", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x6800), CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card 7000H not ours", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x7000), CARD_OUT(CARD_SRAM_4000, 1, 1)),
    CARD_VECTOR("Card 7800H not ours", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x7800), CARD_OUT(CARD_SRAM_4000, 1, 1)),

    /* 8000H-BFFFH: the 8800H-9FFFH ROM copy only. */
    CARD_VECTOR("Card 8000H interchange window never", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_READ(0x8000),
                CARD_OUT(CARD_SRAM_8000, 1, 1)),
    CARD_VECTOR("Card 8800H with Remap", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_READ(0x8800), CARD_OUT(CARD_SRAM_8000, 0, 1)),
    CARD_VECTOR("Card 9800H with Remap", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_READ(0x9800), CARD_OUT(CARD_SRAM_8000, 0, 1)),
    CARD_VECTOR("Card 8800H without Remap", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0x8800),
                CARD_OUT(CARD_SRAM_8000, 1, 1)),
    CARD_VECTOR("Card 8800H blocked by PV high", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_IN(0x8800, 1, 1, 1, 1),
                CARD_OUT(CARD_SRAM_8000, 1, 1)),
    CARD_VECTOR("Card A000H never, even with Remap and BASE_ROM_STAGED", CARD_VI(0, 1, CARD_SRAM_BASE, 1), CARD_READ(0xA000),
                CARD_OUT(CARD_SRAM_8000, 1, 1)),

    /* C000H-FFFFH: the base ROM copy while BASE_ROM_STAGED. */
    CARD_VECTOR("Card C000H staged", CARD_VI(0, 0, CARD_SRAM_BASE, 1), CARD_READ(0xC000), CARD_OUT(CARD_SRAM_C000, 0, 1)),
    CARD_VECTOR("Card C000H not staged", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_READ(0xC000), CARD_OUT(CARD_SRAM_C000, 1, 1)),
    CARD_VECTOR("Card E000H staged, PV high doesn't matter", CARD_VI(0, 0, CARD_SRAM_BASE, 1), CARD_IN(0xE000, 1, 1, 1, 1),
                CARD_OUT(CARD_SRAM_C000, 0, 1)),
    CARD_VECTOR("Card F800H staged", CARD_VI(0, 0, CARD_SRAM_BASE, 1), CARD_READ(0xF800), CARD_OUT(CARD_SRAM_C000, 0, 1)),

    /* DME0 (active-high) low: no CS anywhere. */
    CARD_VECTOR("Card DME0 low blocks 0000H", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x0000, 0, 0, 1, 1),
                CARD_OUT(CARD_SRAM_BASE, 1, 1)),
    CARD_VECTOR("Card DME0 low blocks 8800H", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_IN(0x8800, 0, 0, 1, 1),
                CARD_OUT(CARD_SRAM_8000, 1, 1)),
    CARD_VECTOR("Card DME0 low blocks C000H while staged", CARD_VI(0, 0, CARD_SRAM_BASE, 1),
                CARD_IN(0xC000, 0, 0, 1, 1), CARD_OUT(CARD_SRAM_C000, 1, 1)),

    /* Write protect above 8000H. */
    CARD_VECTOR("Card write 8800H, WE low blocks", CARD_VI(0, 1, CARD_SRAM_BASE, 0), CARD_IN(0x8800, 1, 0, 1, 0),
                CARD_OUT(CARD_SRAM_8000, 0, 1)),
    CARD_VECTOR("Card write 8800H, WE high passes", CARD_VI(1, 1, CARD_SRAM_BASE, 0), CARD_IN(0x8800, 1, 0, 1, 0),
                CARD_OUT(CARD_SRAM_8000, 0, 0)),
    CARD_VECTOR("Card write C000H, WE low blocks", CARD_VI(0, 0, CARD_SRAM_BASE, 1), CARD_IN(0xC000, 1, 0, 1, 0),
                CARD_OUT(CARD_SRAM_C000, 0, 1)),
    CARD_VECTOR("Card write C000H, WE high passes", CARD_VI(1, 0, CARD_SRAM_BASE, 1), CARD_IN(0xC000, 1, 0, 1, 0),
                CARD_OUT(CARD_SRAM_C000, 0, 0)),
    CARD_VECTOR("Card read 8800H with WE high stays a read", CARD_VI(1, 1, CARD_SRAM_BASE, 0), CARD_READ(0x8800),
                CARD_OUT(CARD_SRAM_8000, 0, 1)),
    CARD_VECTOR("Card read C000H with WE high stays a read", CARD_VI(1, 0, CARD_SRAM_BASE, 1), CARD_READ(0xC000),
                CARD_OUT(CARD_SRAM_C000, 0, 1)),
    CARD_VECTOR("Card write 4800H passes with WE low", CARD_VI(0, 0, CARD_SRAM_BASE, 0), CARD_IN(0x4800, 1, 0, 1, 0),
                CARD_OUT(CARD_SRAM_4000, 0, 0)),

    /* V16-V14 only steer bus 0000H-3FFFH. */
    CARD_VECTOR("Card V bits ignored at 6000H", CARD_VI(0, 0, CARD_SRAM_BANK(3), 0), CARD_READ(0x6000),
                CARD_OUT(CARD_SRAM_4000, 0, 1)),
    CARD_VECTOR("Card V bits ignored at 8800H", CARD_VI(0, 1, CARD_SRAM_BANK(3), 0), CARD_READ(0x8800),
                CARD_OUT(CARD_SRAM_8000, 0, 1)),
    CARD_VECTOR("Card V bits ignored at C000H", CARD_VI(0, 0, CARD_SRAM_BANK(3), 1), CARD_READ(0xC000),
                CARD_OUT(CARD_SRAM_C000, 0, 1)),
};

const uint32_t test_vectors_gp1_card_count = sizeof(test_vectors_gp1_card) / sizeof(test_vectors_gp1_card[0]);

/* ================== GP2 (internal card) ==================
 * GP2-MCU-InternalCard.gp6, on the prototype for now -- see gp2_model.h
 * for its logic. Same pins as the dongle's GP2, plus INHIBIT (active-low)
 * on IO10, pin 16. */
static const uint8_t gp2c_sweep_inputs[] = { 2, 3, 4, 5, 6, 7, 17, 15, 13, 20 }; /* model order, gp2_model.h */
static const char *const gp2c_sweep_input_names[] = { "AD11", "AD12", "AD13", "AD14", "AD15", "R/W", "PV", "OD", "DME0",
                                                      "READY" };

const selftest_sweep_t gp2_card_sweep = {
    .name = "GP2 (internal card)",
    .vi_legend = "Remap, ROM_REPLACEMENT",
    .input_pins = gp2c_sweep_inputs,
    .input_names = gp2c_sweep_input_names,
    .input_count = GP2M_IN_COUNT,
    .vi_ios = (const uint8_t[]){ GP2C_VI_SRAM_ROM_REMAP, GP2C_VI_ROM_REPLACEMENT },
    .vi_count = GP2M_VI_COUNT,
    .output_pins = (const uint8_t[]){ 10, 12, 16 },
    .output_names = (const char *const[]){ "TRIG_RD", "TRIG_WR", "INHIBIT" },
    .output_count = GP2M_OUT_COUNT,
    .model = gp2_card_model,
    .settle_us = 10,
};

/* Every vector sets every input, MSB first in these orders: */
static const uint8_t gp2c_inputs[]  = { 6, 5, 4, 3, 2, 13, 17, 7, 15, 20 }; /* AD15-AD11, DME0, PV, R/W, OD, MCU_READY */
static const uint8_t gp2c_vis[]     = { GP2C_VI_SRAM_ROM_REMAP, GP2C_VI_ROM_REPLACEMENT };
static const uint8_t gp2c_outputs[] = { 10, 12, 16 };                  /* TRIG_RD, TRIG_WR, INHIBIT */

#define GP2C_INR(addr, dme0, pv, rw, od, ready) \
    ((((uint32_t)(addr) >> 11) & 0x1Fu) << 5 | (dme0) << 4 | (pv) << 3 | (rw) << 2 | (od) << 1 | (ready))
/* MCU_READY high: the MCU is up and serving, as in normal running. */
#define GP2C_IN(addr, dme0, pv, rw, od) GP2C_INR(addr, dme0, pv, rw, od, 1)
#define GP2C_VI(remap, replace) ((remap) << 1 | (replace))
#define GP2C_OUT(rd, wr, inh)   ((rd) << 2 | (wr) << 1 | (inh))
/* DME0 high, PV low, OD low: a read, or a write. */
#define GP2C_READ(addr)  GP2C_IN(addr, 1, 0, 1, 0)
#define GP2C_WRITE(addr) GP2C_IN(addr, 1, 0, 0, 0)

#define GP2C_VECTOR(nm, vi, in, out) {                                     \
        .name = nm,                                                        \
        .vi_ios = gp2c_vis, .vi_levels = (vi), .vi_count = 2,              \
        .input_ios = gp2c_inputs, .input_levels = (in), .input_count = 10, \
        .output_ios = gp2c_outputs, .expected_levels = (out), .output_count = 3, \
        .settle_us = 100,                                                  \
    }

const selftest_vector_t test_vectors_gp2_card[] = {
    {
        /* Nothing driven (pulled down): DME0 low, no trigger. INHIBIT is
         * left out -- ROM_REPLACEMENT is whatever the last run left. */
        .name = "GP2C Initial State - all inputs floating, no triggers",
        .output_ios = gp2c_outputs,
        .expected_levels = 0x0, /* read=0, write=0 */
        .output_count = 2,
        .settle_us = 100,
    },

    /* ROM_REPLACEMENT off: the dongle GP2's behaviour, INHIBIT high. */
    GP2C_VECTOR("GP2C 8800H read, Remap off: read triggers", GP2C_VI(0, 0), GP2C_READ(0x8800), GP2C_OUT(1, 0, 1)),
    GP2C_VECTOR("GP2C 8000H read, Remap off: read triggers", GP2C_VI(0, 0), GP2C_READ(0x8000), GP2C_OUT(1, 0, 1)),
    GP2C_VECTOR("GP2C 8000H read, Remap on: still triggers", GP2C_VI(1, 0), GP2C_READ(0x8000), GP2C_OUT(1, 0, 1)),
    GP2C_VECTOR("GP2C 8800H read, Remap on: suppressed", GP2C_VI(1, 0), GP2C_READ(0x8800), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 0000H read: never", GP2C_VI(0, 0), GP2C_READ(0x0000), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8800H read, PV high: blocked", GP2C_VI(0, 0), GP2C_IN(0x8800, 1, 1, 1, 0), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8800H read, DME0 low: blocked", GP2C_VI(0, 0), GP2C_IN(0x8800, 0, 0, 1, 0), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8800H read, OD high: blocked", GP2C_VI(0, 0), GP2C_IN(0x8800, 1, 0, 1, 1), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8800H write: outside the write window", GP2C_VI(0, 0), GP2C_WRITE(0x8800), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8000H write: write triggers", GP2C_VI(0, 0), GP2C_WRITE(0x8000), GP2C_OUT(0, 1, 1)),
    GP2C_VECTOR("GP2C 8000H write, OD high: still triggers", GP2C_VI(0, 0), GP2C_IN(0x8000, 1, 0, 0, 1),
                GP2C_OUT(0, 1, 1)),
    GP2C_VECTOR("GP2C C000H read, replacement off: never", GP2C_VI(0, 0), GP2C_READ(0xC000), GP2C_OUT(0, 0, 1)),

    /* ROM_REPLACEMENT on: INHIBIT low, C000H-FFFFH reads go to the MCU. */
    GP2C_VECTOR("GP2C C000H read, replacing: read triggers", GP2C_VI(0, 1), GP2C_READ(0xC000), GP2C_OUT(1, 0, 0)),
    GP2C_VECTOR("GP2C F800H read, replacing: read triggers", GP2C_VI(0, 1), GP2C_READ(0xF800), GP2C_OUT(1, 0, 0)),
    GP2C_VECTOR("GP2C E000H read, replacing, PV high: PV doesn't matter", GP2C_VI(0, 1),
                GP2C_IN(0xE000, 1, 1, 1, 0), GP2C_OUT(1, 0, 0)),
    GP2C_VECTOR("GP2C C000H read, replacing, DME0 low: blocked", GP2C_VI(0, 1), GP2C_IN(0xC000, 0, 0, 1, 0),
                GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C C000H read, replacing, OD high: blocked", GP2C_VI(0, 1), GP2C_IN(0xC000, 1, 0, 1, 1),
                GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C C000H write, replacing: no write trigger", GP2C_VI(0, 1), GP2C_WRITE(0xC000), GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C E000H write, replacing: no write trigger", GP2C_VI(0, 1), GP2C_WRITE(0xE000), GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C A000H read, replacing: never", GP2C_VI(0, 1), GP2C_READ(0xA000), GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C 4000H read, replacing: never", GP2C_VI(0, 1), GP2C_READ(0x4000), GP2C_OUT(0, 0, 0)),
    GP2C_VECTOR("GP2C 8000H write, replacing: window still works", GP2C_VI(0, 1), GP2C_WRITE(0x8000),
                GP2C_OUT(0, 1, 0)),
    GP2C_VECTOR("GP2C 8800H read, Remap off, replacing: read triggers", GP2C_VI(0, 1), GP2C_READ(0x8800),
                GP2C_OUT(1, 0, 0)),

    /* MCU_READY low (power-on, the MCU not serving yet): ROM_REPLACEMENT
     * does nothing -- INHIBIT stays high, C000H-FFFFH isn't the MCU's. */
    GP2C_VECTOR("GP2C replacing, MCU not ready: INHIBIT high", GP2C_VI(0, 1), GP2C_INR(0x0000, 1, 0, 1, 0, 0),
                GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C F800H read, replacing, MCU not ready: no trigger", GP2C_VI(0, 1),
                GP2C_INR(0xF800, 1, 0, 1, 0, 0), GP2C_OUT(0, 0, 1)),
    GP2C_VECTOR("GP2C 8800H read, Remap off, MCU not ready: still triggers", GP2C_VI(0, 1),
                GP2C_INR(0x8800, 1, 0, 1, 0, 0), GP2C_OUT(1, 0, 1)),
    GP2C_VECTOR("GP2C 8000H write, MCU not ready: window still works", GP2C_VI(0, 1),
                GP2C_INR(0x8000, 1, 0, 0, 0, 0), GP2C_OUT(0, 1, 1)),
    GP2C_VECTOR("GP2C MCU ready, replacement off: INHIBIT high", GP2C_VI(0, 0), GP2C_READ(0xC000),
                GP2C_OUT(0, 0, 1)),
};

const uint32_t test_vectors_gp2_card_count = sizeof(test_vectors_gp2_card) / sizeof(test_vectors_gp2_card[0]);
