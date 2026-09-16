/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "selftest.h"
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

selftest_result_t selftest_run(const selftest_vector_t *vectors, uint32_t count) {
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

        result.total++;
        if (vector_ok) result.passed++; else result.failed++;
    }

    printf("Self-test complete: %u/%u passed\n", result.passed, result.total);

    /* Release every mapped pin. Pins the bench itself drove as an
     * input to the GreenPAK (address bus, DME0, PV, R/W-from-bus, OD,
     * 1500A-sense) get a pull-down so they settle to a firm, repeatable
     * low instead of floating at whatever level breadboard crosstalk
     * or residual charge leaves them at -- a bare release left DME0
     * "dimly" non-zero, enough to make the very next selftest_run()'s
     * Initial State Test misread the decode as open. GreenPAK *output*
     * pins (CS, R/W to SRAM, GP2's read/write triggers) are left
     * plain-floating instead, since a Pico-side pull-down would fight
     * an open-drain design relying on an external pull. */
    static const uint8_t dut_output_pins[] = { 18, 16, 10, 12 };
    for (uint8_t i = 0; i < GREENPAK_IO_COUNT; i++) {
        uint8_t pin = greenpak_io_map[i].greenpak_pin;
        bool is_dut_output = false;
        for (uint8_t k = 0; k < sizeof(dut_output_pins); k++) {
            if (dut_output_pins[k] == pin) {
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
 * From the GreenPAK design doc's "GP2" section logic:
 *   100 = ROM                          (AD15,AD14,AD13 = 100, i.e. 8000H-9FFFH)
 *   CS = ROM & (!Remap | AD12,AD11==00) & !PV & DME0
 *   read_trigger  = CS & R/W & !OD
 *   write_trigger = CS & !R/W
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
 * serving ROM" behavior described in that section. */
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
        .name = "GP2 Test 8 - write trigger asserts on write",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x118, /* 8800H, DME0=1 (gate open), R/W=0, OD=0 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x1, /* read=0, write=1 */
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
        .name = "GP2 Test 10 - write trigger unaffected by OD",
        .vi_ios = (const uint8_t[]){ GP2_VI_SRAM_ROM_REMAP },
        .vi_levels = 0x0,
        .vi_count = 1,
        .input_ios = gp2_inputs,
        .input_levels = 0x119, /* 8800H, DME0=1 (gate open), R/W=0, OD=1 */
        .input_count = 9,
        .output_ios = gp2_trigger_outputs,
        .expected_levels = 0x1, /* still write=1 */
        .output_count = 2,
        .settle_us = 100,
    },
};

const uint32_t test_vectors_gp2_count = sizeof(test_vectors_gp2) / sizeof(test_vectors_gp2[0]);
