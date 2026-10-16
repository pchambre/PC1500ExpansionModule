/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* gp1_model.h
 *
 * Reference models of the two GP1 designs, written straight from the
 * equations in the "GreenPak design" doc and the 2026-10-15 LUT rework
 * (DME0 tested once, in the final CS LUT). selftest_sweep() drives every
 * input/virtual-input combination into the chip and compares the pins
 * against these. Pure C, no pico-sdk, so host_test/ can build them too.
 *
 * Bit numbering is the model's own, LSB first. Which chip pin or virtual
 * input each bit lands on is the sweep descriptor's job (selftest.c).
 */
#pragma once

#include <stdint.h>

/* Inputs (both designs). The low 5 bits are AD11-AD15, so
 * (in & 0x1F) << 11 is the bus address of the 2K page. */
enum {
    GP1M_IN_AD11 = 0,
    GP1M_IN_AD12,
    GP1M_IN_AD13,
    GP1M_IN_AD14,
    GP1M_IN_AD15,
    GP1M_IN_RW,     /* R/W from bus, high = read */
    GP1M_IN_PV,
    GP1M_IN_P,      /* IO13: high enables 4800H-57FFH (the CS2 vectors' convention, pin 19 = 1
                     * for "not a PC-1500A"; the doc says both ways) */
    GP1M_IN_DME0,   /* active-high */
    GP1M_IN_COUNT
};

/* Dongle GP1 (GP1-SRAM.gp6) virtual inputs. */
enum {
    GP1M_DVI_WE = 0,
    GP1M_DVI_REMAP,
    GP1M_DVI_COUNT
};

/* Dongle GP1 outputs. */
enum {
    GP1M_DOUT_CS = 0,   /* active-low */
    GP1M_DOUT_RW,       /* R/W to SRAM */
    GP1M_DOUT_SRAM_AD11,
    GP1M_DOUT_SRAM_AD12,
    GP1M_DOUT_SRAM_AD13,
    GP1M_DOUT_SRAM_AD14,
    GP1M_DOUT_COUNT
};

/* Internal-card GP1 (GP1-SRAM-InternalCard.gp6) virtual inputs. V16-V14
 * are the SRAM AD16-AD14 the firmware wants for bus 0000H-3FFFH: 000
 * base, 1bb bank bb, 011 staging window onto the C000H copy. */
enum {
    GP1M_CVI_WE = 0,
    GP1M_CVI_REMAP,
    GP1M_CVI_V14,
    GP1M_CVI_V15,
    GP1M_CVI_V16,
    GP1M_CVI_BASE_ROM_STAGED,   /* base ROM served from SRAM C000H-FFFFH */
    GP1M_CVI_COUNT
};

/* Internal-card GP1 outputs. INHIBIT is GP2's (BASE_ROM_REPLACE). */
enum {
    GP1M_COUT_CS = 0,   /* active-low */
    GP1M_COUT_RW,       /* R/W to SRAM */
    GP1M_COUT_SRAM_AD14,
    GP1M_COUT_SRAM_AD15,
    GP1M_COUT_SRAM_AD16,
    GP1M_COUT_COUNT
};

/* Each model returns the expected output bits for inputs `in` and
 * virtual inputs `vi`, and sets *care to the outputs that are defined
 * for that combination (a 0 bit is a don't-care). */
uint32_t gp1_dongle_model(uint32_t in, uint32_t vi, uint32_t *care);
uint32_t gp1_card_model(uint32_t in, uint32_t vi, uint32_t *care);
