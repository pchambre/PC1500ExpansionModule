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

/* GP1-SRAM-InternalCard.gp6's virtual inputs, read from its wires
 * 2026-10-16 (VI index = matrix port id - 56). WE/Remap keep the
 * dongle's indices. */
#define GP1C_VI_SRAM_ROM_WE      0
#define GP1C_VI_SRAM_ROM_REMAP   1
#define GP1C_VI_V16              2
#define GP1C_VI_V15              3
#define GP1C_VI_V14              4
#define GP1C_VI_BASE_ROM_STAGED  5

/* GP2-MCU-InternalCard.gp6's virtual inputs (2026-10-16, from its wires):
 * the dongle GP2's Remap, plus ROM_REPLACEMENT (ROMRPLC). */
#define GP2C_VI_SRAM_ROM_REMAP   1
#define GP2C_VI_ROM_REPLACEMENT  2

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

/* The chip pins a design drives (CS, R/W to SRAM, SRAM address bits,
 * triggers). They differ per design -- GP2's 13/15 are inputs,
 * the GP1s' are outputs -- so each suite passes its own list, and the
 * cleanup leaves these floating and pulls every other pin down. */
typedef struct {
    const uint8_t *pins;
    uint8_t count;
} selftest_outputs_t;

extern const selftest_outputs_t gp1_outputs;
extern const selftest_outputs_t gp1_card_outputs;
extern const selftest_outputs_t gp2_outputs;
extern const selftest_outputs_t gp2_card_outputs;

/* Runs every vector in vectors[0..count), printf-reporting PASS/FAIL
 * per vector plus a final summary line. Restores all IOs to floating
 * input when done. */
selftest_result_t selftest_run(const selftest_vector_t *vectors, uint32_t count,
                               const selftest_outputs_t *outputs);

/* Exhaustive check against a reference model (gp1_model.h): every
 * combination of the model's virtual inputs and pin inputs is applied,
 * and each output pin is compared with the model's expected level
 * wherever the model says it's defined. Model bit n of the inputs,
 * virtual inputs and outputs lands on input_pins[n], vi_ios[n] and
 * output_pins[n]. */
typedef struct {
    const char *name;
    const char *vi_legend;         /* model virtual-input bits, LSB first, for the log */
    const uint8_t *input_pins;
    const char *const *input_names; /* for the log; bits 0-4 are always AD11-AD15 */
    uint8_t input_count;
    const uint8_t *vi_ios;
    uint8_t vi_count;
    const uint8_t *output_pins;
    const char *const *output_names;
    uint8_t output_count;
    uint32_t (*model)(uint32_t in, uint32_t vi, uint32_t *care);
    uint32_t settle_us;
} selftest_sweep_t;

/* Prints the first few mismatches and a summary; result counts
 * combinations, not pins. */
selftest_result_t selftest_sweep(const selftest_sweep_t *sweep, const selftest_outputs_t *outputs);

extern const selftest_sweep_t gp1_sweep;
extern const selftest_sweep_t gp1_card_sweep;
extern const selftest_sweep_t gp2_card_sweep;

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

/* Internal-card GP1 (GP1-SRAM-InternalCard.gp6) -- bring-up vectors for
 * the 128K SRAM decode: banks/staging window at 0000H-3FFFH, the
 * 4800H-6FFFH windows, ROM 8800H-9FFFH, base ROM C000H-FFFFH and write
 * protect. Every vector sets all six virtual inputs and all
 * nine pin inputs, so none depends on the one before it. */
extern const selftest_vector_t test_vectors_gp1_card[];
extern const uint32_t test_vectors_gp1_card_count;

/* Internal card's GP2 (GP2-MCU-InternalCard.gp6, on the prototype for
 * now) -- the dongle GP2's behaviour with ROM_REPLACEMENT off, then
 * C000H-FFFFH reads, INHIBIT, and no write trigger inside the base ROM
 * with it on. Every vector sets both virtual inputs and all nine pins. */
extern const selftest_vector_t test_vectors_gp2_card[];
extern const uint32_t test_vectors_gp2_card_count;

bool *get_bool_levels(uint32_t input_value, uint8_t input_count);
