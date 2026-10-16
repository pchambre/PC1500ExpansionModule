/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "gp2_model.h"

#include <stdbool.h>

#define BIT(v, n) (((v) >> (n)) & 1u)

uint32_t gp2_card_model(uint32_t in, uint32_t vi, uint32_t *care) {
    bool a15 = BIT(in, GP2M_IN_AD15), a14 = BIT(in, GP2M_IN_AD14), a13 = BIT(in, GP2M_IN_AD13);
    bool a12 = BIT(in, GP2M_IN_AD12), a11 = BIT(in, GP2M_IN_AD11);
    bool rw = BIT(in, GP2M_IN_RW), replace = BIT(vi, GP2M_VI_REPLACE) && BIT(in, GP2M_IN_READY);

    bool rom = a15 && !a14 && !a13;
    bool rom_term = rom && (!BIT(vi, GP2M_VI_REMAP) || (!a12 && !a11));
    bool base_rom = a15 && a14 && replace;
    bool cs = BIT(in, GP2M_IN_DME0) && ((rom_term && !BIT(in, GP2M_IN_PV)) || base_rom);

    uint32_t out = 0;
    out |= (uint32_t)(cs && rw && !BIT(in, GP2M_IN_OD)) << GP2M_OUT_TRIG_RD;
    out |= (uint32_t)(cs && !a12 && !a11 && !rw && !base_rom) << GP2M_OUT_TRIG_WR;
    out |= (uint32_t)!replace << GP2M_OUT_INHIBIT;
    *care = (1u << GP2M_OUT_COUNT) - 1u;
    return out;
}
