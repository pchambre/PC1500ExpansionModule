/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_config.h -- firmware settings set from BASIC with MCONF (2026-09-25),
 * persisted in their own flash sector.
 *
 * Settings are numbered (MCU_CONFIG_*) and 16-bit; the names users type
 * live in keywords.c, which reaches these through EXP_COMMAND_CONFIG_GET/
 * EXP_COMMAND_CONFIG_SET. Reads come from a RAM copy, so they're cheap
 * enough for core0's bus loop. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    MCU_CONFIG_LED = 0,        /* 1 = flash the LED for SD/bridge activity (default 1) */
    MCU_CONFIG_SLEEPWAIT = 1,  /* STAGE RAM mode: ms to stay awake after a keyword
                                  before going DORMANT (default 0) */
    MCU_CONFIG_LOGSIZE = 2,    /* MCU log size in KB (default 100) -- see mcu_log.h */
    /* Not MCONF settings, just persisted with them: */
    MCU_CONFIG_LOGINFO = 3,    /* MLOG VERBOSE (1) / QUIET (0, default) */
    MCU_CONFIG_LOGGEN = 4,     /* the log's generation, bumped by a LOGSIZE change */
    MCU_CONFIG_COUNT
};

/* Loads the settings from flash (defaults if never saved). Call once at
 * boot, after flash_safe_execute_core_init(). */
void mcu_config_init(void);

/* 0xFFFF is never a stored value: it's erased flash, i.e. a setting added
 * after the settings were last saved, and reads as its default. */
uint16_t mcu_config_get(uint8_t id);

/* Stores and, if it changed, persists a setting. False for an unknown id. */
bool mcu_config_set(uint8_t id, uint16_t value);
