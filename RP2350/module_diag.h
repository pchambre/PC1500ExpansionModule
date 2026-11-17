/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* module_diag.h -- MLOG MODULE (2026-11-17): copies a diagnostic build of
 * the BGM220S firmware's RAM log ring into the MCU log, read over SWD while
 * the module runs. Card only (PC1500_TARGET_CARD). */
#pragma once

#include <stdint.h>

enum {
    MODULE_DIAG_OK = 0,
    MODULE_DIAG_NO_SWD = 1, /* the module's bus isn't reachable (asleep below EM1, or no answer) */
    MODULE_DIAG_NO_RING = 2, /* reachable, but no log ring: not a diagnostic build */
    MODULE_DIAG_READ_FAILED = 3,
};

/* Copies the entries not copied before (all of them after the module or the
 * card restarted). *copied: entries copied; *lost: entries overwritten in
 * the ring before they could be copied. */
uint8_t module_diag_copy(uint16_t *copied, uint16_t *lost);
