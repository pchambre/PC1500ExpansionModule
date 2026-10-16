/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "gp1_model.h"

#include <stdbool.h>

#define BIT(v, n) (((v) >> (n)) & 1u)

/* The 4800H-6FFFH windows, as a function of AD13-AD11 and IO13 when
 * AD15,AD14 = 01 (the 4-bit LUT in both designs):
 *   4800H/5000H (001/010) only while IO13 is high, 5800H (011),
 *   6000H/6800H (10x). 4000H (000) and 7000H-7FFFH (11x) are not ours. */
static bool ram_window(uint32_t in) {
    bool a13 = BIT(in, GP1M_IN_AD13), a12 = BIT(in, GP1M_IN_AD12), a11 = BIT(in, GP1M_IN_AD11);
    bool p = BIT(in, GP1M_IN_P);
    return (!a13 && (a12 || a11) && (p || (a12 && a11))) || (a13 && !a12);
}

/* 0000H-3FFFH plus the 4800H-6FFFH windows. */
static bool low_ranges(uint32_t in) {
    return !BIT(in, GP1M_IN_AD15) && (!BIT(in, GP1M_IN_AD14) || ram_window(in));
}

/* 8800H-9FFFH: top bits 100, not the 8000H-87FFH interchange window. */
static bool rom_range(uint32_t in) {
    return BIT(in, GP1M_IN_AD15) && !BIT(in, GP1M_IN_AD14) && !BIT(in, GP1M_IN_AD13) &&
           (BIT(in, GP1M_IN_AD12) || BIT(in, GP1M_IN_AD11));
}

/* R/W to SRAM: writes above 8000H only while WE is set. */
static bool rw_to_sram(uint32_t in, bool we) {
    return BIT(in, GP1M_IN_RW) || (BIT(in, GP1M_IN_AD15) && !we);
}

uint32_t gp1_dongle_model(uint32_t in, uint32_t vi, uint32_t *care) {
    bool a15 = BIT(in, GP1M_IN_AD15), a14 = BIT(in, GP1M_IN_AD14), a13 = BIT(in, GP1M_IN_AD13);
    bool a12 = BIT(in, GP1M_IN_AD12), a11 = BIT(in, GP1M_IN_AD11);

    bool high = rom_range(in) && BIT(vi, GP1M_DVI_REMAP) && !BIT(in, GP1M_IN_PV);
    bool cs_active = BIT(in, GP1M_IN_DME0) && (low_ranges(in) || high);

    /* SRAM AD14-AD11, per the doc's address LUT equations. */
    bool s14 = a15 || a14;
    bool s13 = (a13 && a11) || (a13 && !a14) || a15;
    bool s12 = (a12 && a11) || (a12 && !a14) || (a14 && !a12 && !a11);
    bool s11 = a11 != a14;

    uint32_t out = 0;
    out |= (uint32_t)!cs_active << GP1M_DOUT_CS;
    out |= (uint32_t)rw_to_sram(in, BIT(vi, GP1M_DVI_WE)) << GP1M_DOUT_RW;
    out |= (uint32_t)s11 << GP1M_DOUT_SRAM_AD11;
    out |= (uint32_t)s12 << GP1M_DOUT_SRAM_AD12;
    out |= (uint32_t)s13 << GP1M_DOUT_SRAM_AD13;
    out |= (uint32_t)s14 << GP1M_DOUT_SRAM_AD14;

    /* The doc only defines the SRAM address where CS is asserted. */
    *care = (1u << GP1M_DOUT_CS) | (1u << GP1M_DOUT_RW);
    if (cs_active) {
        *care |= (1u << GP1M_DOUT_SRAM_AD11) | (1u << GP1M_DOUT_SRAM_AD12) |
                 (1u << GP1M_DOUT_SRAM_AD13) | (1u << GP1M_DOUT_SRAM_AD14);
    }
    return out;
}

uint32_t gp1_card_model(uint32_t in, uint32_t vi, uint32_t *care) {
    bool a15 = BIT(in, GP1M_IN_AD15), a14 = BIT(in, GP1M_IN_AD14);
    bool staged = BIT(vi, GP1M_CVI_BASE_ROM_STAGED);

    /* 8800H-9FFFH with Remap and PV low, or C000H-FFFFH while the base
     * ROM is staged (PV doesn't matter there). */
    bool high = (rom_range(in) && BIT(vi, GP1M_CVI_REMAP) && !BIT(in, GP1M_IN_PV)) ||
                (a15 && a14 && staged);
    bool cs_active = BIT(in, GP1M_IN_DME0) && (low_ranges(in) || high);

    /* Bus 0000H-3FFFH takes V16-V14; everything else maps straight
     * through (4000H -> 0x04000, 8000H -> 0x08000, C000H -> 0x0C000). */
    bool s14 = a14 || (!a15 && BIT(vi, GP1M_CVI_V14));
    bool s15 = a15 || (!a14 && BIT(vi, GP1M_CVI_V15));
    bool s16 = !a15 && !a14 && BIT(vi, GP1M_CVI_V16);

    uint32_t out = 0;
    out |= (uint32_t)!cs_active << GP1M_COUT_CS;
    out |= (uint32_t)rw_to_sram(in, BIT(vi, GP1M_CVI_WE)) << GP1M_COUT_RW;
    out |= (uint32_t)s14 << GP1M_COUT_SRAM_AD14;
    out |= (uint32_t)s15 << GP1M_COUT_SRAM_AD15;
    out |= (uint32_t)s16 << GP1M_COUT_SRAM_AD16;

    *care = (1u << GP1M_COUT_COUNT) - 1u;
    return out;
}
