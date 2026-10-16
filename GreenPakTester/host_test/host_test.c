/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* host_test.c -- runs selftest.c's GP1 suites on the PC against a
 * simulated GreenPAK whose outputs come from gp1_model.c, so a vector
 * table or sweep bug shows up before anything is flashed. The pin and
 * virtual-input wiring the simulation uses is the sweep descriptor's,
 * so a vector that disagrees with the descriptor fails here too.
 *
 * Build from GreenPakTester/, run from host_test/ (the per-vector log,
 * host_test.log, lands in the current directory):
 *   gcc -std=gnu11 -Wall -include stdbool.h -Ihost_test -I. host_test/host_test.c selftest.c \
 *       gp1_model.c gp2_model.c greenpak_pins.c -o host_test/host_test
 *   cd host_test && ./host_test
 * It also injects a fault (DME0 ignored in the CS term) to check the
 * sweep and vectors really catch one. Exit status is 0 only if the
 * clean runs pass and the faulty runs fail.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gp1_model.h"
#include "greenpak_gpio.h"
#include "greenpak_virtual_io.h"
#include "selftest.h"

static const selftest_sweep_t *sim;    /* design being simulated */
static bool sim_fault;                 /* ignore DME0 in CS */
static bool pin_driven[21], pin_level[21];
static uint8_t vi_reg;

static uint32_t sim_outputs(void) {
    uint32_t in = 0, vi = 0, care;
    for (uint8_t b = 0; b < sim->input_count; b++) {
        uint8_t pin = sim->input_pins[b];
        if (pin_driven[pin] && pin_level[pin]) in |= 1u << b;   /* released = pulled low */
    }
    for (uint8_t b = 0; b < sim->vi_count; b++) {
        if ((vi_reg >> sim->vi_ios[b]) & 1u) vi |= 1u << b;
    }
    if (sim_fault) {
        /* CS is output bit 0 in both models: evaluate it with DME0 forced high. */
        uint32_t forced = sim->model(in | (1u << GP1M_IN_DME0), vi, &care);
        uint32_t out = sim->model(in, vi, &care);
        return (out & ~1u) | (forced & 1u);
    }
    return sim->model(in, vi, &care);
}

void greenpak_gpio_drive(uint8_t pin, bool level) { pin_driven[pin] = true; pin_level[pin] = level; }
void greenpak_gpio_release(uint8_t pin) { pin_driven[pin] = false; }
void greenpak_gpio_release_pulled_down(uint8_t pin) { pin_driven[pin] = false; }

bool greenpak_gpio_read(uint8_t pin) {
    for (uint8_t o = 0; o < sim->output_count; o++) {
        if (sim->output_pins[o] == pin) return (sim_outputs() >> o) & 1u;
    }
    return pin_driven[pin] && pin_level[pin];
}

bool greenpak_virtual_input_set(uint8_t vi_index, bool level) {
    if (level) vi_reg |= (uint8_t)(1u << vi_index); else vi_reg &= (uint8_t)~(1u << vi_index);
    return true;
}

bool greenpak_virtual_input_get(uint8_t vi_index, bool *level_out) {
    *level_out = (vi_reg >> vi_index) & 1u;
    return true;
}

static void reset_sim(const selftest_sweep_t *design, bool fault) {
    sim = design;
    sim_fault = fault;
    memset(pin_driven, 0, sizeof(pin_driven));
    memset(pin_level, 0, sizeof(pin_level));
    vi_reg = 0;
}

int main(void) {
    int bad = 0;
    selftest_result_t r;

    /* Silence the per-vector chatter; keep the summaries on stderr. */
    FILE *log = freopen("host_test.log", "w", stdout);
    (void)log;

    reset_sim(&gp1_sweep, false);
    r = selftest_run(test_vectors, test_vectors_count, &gp1_outputs);
    fprintf(stderr, "dongle vectors:        %u/%u pass\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;
    r = selftest_sweep(&gp1_sweep, &gp1_outputs);
    fprintf(stderr, "dongle sweep:          %u/%u match\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;

    reset_sim(&gp1_card_sweep, false);
    r = selftest_run(test_vectors_gp1_card, test_vectors_gp1_card_count, &gp1_card_outputs);
    fprintf(stderr, "card vectors:          %u/%u pass\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;
    r = selftest_sweep(&gp1_card_sweep, &gp1_card_outputs);
    fprintf(stderr, "card sweep:            %u/%u match\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;

    reset_sim(&gp2_card_sweep, false);
    r = selftest_run(test_vectors_gp2_card, test_vectors_gp2_card_count, &gp2_card_outputs);
    fprintf(stderr, "card GP2 vectors:      %u/%u pass\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;
    r = selftest_sweep(&gp2_card_sweep, &gp2_card_outputs);
    fprintf(stderr, "card GP2 sweep:        %u/%u match\n", (unsigned)r.passed, (unsigned)r.total);
    bad |= r.failed != 0;

    reset_sim(&gp1_card_sweep, true);
    r = selftest_run(test_vectors_gp1_card, test_vectors_gp1_card_count, &gp1_card_outputs);
    fprintf(stderr, "card vectors, fault:   %u failing (want > 0)\n", (unsigned)r.failed);
    bad |= r.failed == 0;
    r = selftest_sweep(&gp1_card_sweep, &gp1_card_outputs);
    fprintf(stderr, "card sweep, fault:     %u mismatching (want > 0)\n", (unsigned)r.failed);
    bad |= r.failed == 0;

    fprintf(stderr, "%s (details in host_test.log)\n", bad ? "HOST TEST FAILED" : "HOST TEST OK");
    return bad;
}
