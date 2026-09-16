/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* selftest.h
 *
 * Test-vector-driven GPIO self-test for a GreenPAK design: drive a set
 * of input IO pins to specified levels, read back a set of output IO
 * pins after a settle delay, and compare against expected levels.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* GP1-SRAM.gp6's Connection Matrix Virtual Input assignments (indices
 * into greenpak_virtual_input_set/get, see greenpak_virtual_io.h). */
#define GP1_VI_SRAM_ROM_WE    0
#define GP1_VI_SRAM_ROM_REMAP 1

/* GP2's design uses the same virtual input index as GP1's Remap (per
 * user confirmation) -- GP2 has no WE-equivalent virtual input since it
 * doesn't control SRAM writes, only read/write trigger signals to the
 * MCU. */
#define GP2_VI_SRAM_ROM_REMAP 1

typedef struct {
    const char *name;              /* human-readable vector name for log output */
    const uint8_t *vi_ios;         /* virtual input indices (0..7, see greenpak_virtual_io.h)
                                     * to set over I2C before input_ios are driven */
    const uint32_t vi_levels;      /* levels to set on each, same length as vi_ios */
    uint8_t vi_count;
    const uint8_t *input_ios;      /* GreenPAK IOn indices to drive */
    const uint32_t input_levels;      /* levels to drive on each, same length as input_ios */
    uint8_t input_count;
    const uint8_t *output_ios;     /* GreenPAK IOn indices to read back */
    const uint32_t expected_levels;   /* expected levels, same length as output_ios */
    uint8_t output_count;
    uint32_t settle_us;            /* delay between drive and read, for propagation/debounce */
} selftest_vector_t;

typedef struct {
    uint32_t total;
    uint32_t passed;
    uint32_t failed;
} selftest_result_t;

/* Runs every vector in vectors[0..count), printf-reporting PASS/FAIL
 * per vector plus a final summary line. Restores all IOs to floating
 * input when done. */
selftest_result_t selftest_run(const selftest_vector_t *vectors, uint32_t count);

/* PLACEHOLDER test vectors -- NOT derived from GP1's actual design.
 * These exist only to exercise the harness end-to-end (see selftest.c);
 * do not treat their PASS/FAIL result as meaningful GP1 verification.
 * Replace with real vectors once GP1's truth table is decoded/supplied
 * (see GreenPakTester/README.md). */
extern const selftest_vector_t test_vectors[];
extern const uint32_t test_vectors_count;

/* GP2 -- read/write trigger logic per the GreenPAK design doc's "GP2"
 * section (100=ROM top bits, CS gating, read/write trigger formulas --
 * see selftest.c for the full derivation). GP2's own .gp6 project still
 * doesn't exist, so this is tested via direct pin I/O only, same as
 * GP1's suite. */
extern const selftest_vector_t test_vectors_gp2[];
extern const uint32_t test_vectors_gp2_count;

bool *get_bool_levels(uint32_t input_value, uint8_t input_count);
