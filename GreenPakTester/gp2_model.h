/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* gp2_model.h
 *
 * Reference model of the internal card's GP2 (GP2-MCU-InternalCard.gp6,
 * 2026-10-16; on the prototype for now): the dongle's GP2-MCU.gp6 plus
 * ROM_REPLACEMENT for ROMRPLC -- INHIBIT on IO10, and TRIG_RD for
 * C000H-FFFFH so the MCU serves a replacement base ROM. Written from the
 * .gp6 netlist:
 *   ROM      = AD15,AD14,AD13 == 100
 *   ROMTERM  = ROM & (!Remap | AD12,AD11 == 00)
 *   REPLACING = ROM_REPLACEMENT & MCU_READY   (2026-10-16: IO14 from the MCU,
 *               pulled down -- the PC-1500's own ROM until the MCU serves)
 *   BASEROM  = AD15 & AD14 & REPLACING
 *   CS       = DME0 & ((ROMTERM & !PV) | BASEROM)
 *   TRIG_RD  = CS & R/W & !OD
 *   TRIG_WR  = CS & AD12,AD11 == 00 & !R/W & !BASEROM   (8000H-87FFH only)
 *   INHIBIT  = !REPLACING                                (active-low)
 * Bit numbering as gp1_model.h: the low 5 input bits are AD11-AD15. */
#pragma once

#include <stdint.h>

enum {
    GP2M_IN_AD11 = 0,
    GP2M_IN_AD12,
    GP2M_IN_AD13,
    GP2M_IN_AD14,
    GP2M_IN_AD15,
    GP2M_IN_RW,     /* high = read */
    GP2M_IN_PV,
    GP2M_IN_OD,
    GP2M_IN_DME0,   /* active-high */
    GP2M_IN_READY,  /* MCU_READY, IO14 */
    GP2M_IN_COUNT
};

enum {
    GP2M_VI_REMAP = 0,
    GP2M_VI_REPLACE,  /* ROM_REPLACEMENT */
    GP2M_VI_COUNT
};

enum {
    GP2M_OUT_TRIG_RD = 0,
    GP2M_OUT_TRIG_WR,
    GP2M_OUT_INHIBIT,
    GP2M_OUT_COUNT
};

uint32_t gp2_card_model(uint32_t in, uint32_t vi, uint32_t *care);
