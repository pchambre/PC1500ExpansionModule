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
    /* 5 was MCONF BLE, the 2026-09-27 BLE bring-up spike's switch: unused
     * now, and not to be reused -- an old value may still be in flash.
     * New settings go at the end. */
    MCU_CONFIG_UNUSED_5 = 5,
    MCU_CONFIG_AUTOSTAGE = 6,  /* 1 = STAGE RAM at power-on/reset (the ROM's boot
                                  hook, via ROM_GET_MODE); 0 = don't (default 0) */
    MCU_CONFIG_BLKBD = 7,      /* 1 = set up the external keyboard's driver at power-on/reset
                                  (the ROM's boot hook, EXP_COMMAND_KBD_INSTALL); 0 = don't
                                  (default 0) */
    MCU_CONFIG_POWMANDELAY = 8, /* STAGE RAM sleep, 2026-10-05: POWMAN power-down instead of
                                  only DORMANT. 0xFFFF (MCONF -1, default) = never; 0 = straight
                                  to POWMAN instead of DORMANT; n = DORMANT, then POWMAN if n
                                  seconds pass with no wake. monitor.c "STAGE RAM sleep" */
    MCU_CONFIG_KBDLAYOUT = 9,  /* the external keyboard's layout, 2026-10-07, as its HID
                                  country code: 33 US (default; 0 too), 8 French, 9 German,
                                  25 Spanish, 2 Belgian -- kbd_seq.h KBD_COUNTRY_* */
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

/* MCONF HOSTNAME (2026-09-28): this PC-1500's name on the BLE link -- sent
 * in HELLO (the other side shows "CONNECTED: name") and, with BLADV,
 * advertised. Up to MCU_CONFIG_HOSTNAME_MAX printable ASCII characters, no
 * quotes; kept with the settings. */
#define MCU_CONFIG_HOSTNAME_MAX 15
#define MCU_CONFIG_HOSTNAME_DEFAULT "PC-1500"
const char *mcu_config_get_hostname(void);
/* False, changing nothing, for an empty, too long or unprintable name. */
bool mcu_config_set_hostname(const char *name, uint8_t len);
