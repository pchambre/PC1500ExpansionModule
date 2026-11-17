/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* module_swd.h -- the card's SWD lines to its BGM220S (GPIO38 SWCLK -> PA01,
 * GPIO39 SWDIO -> PA02), as a bit-banged ADIv5 host: MEM-AP word reads of
 * the RUNNING module, no reset, no halt, nothing written to its memory.
 * Taken from ../BGM220_Provision/swd.c, which also programs it. Card only
 * (PC1500_TARGET_CARD). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Takes the two pins, connects (line reset, JTAG-to-SWD, dormant wake-up),
 * powers the debug port up and checks the core's bus is reachable (MEM-AP
 * CSW DeviceEn -- 0 while the module sleeps below EM1). Retries for up to
 * `ms`. */
bool module_swd_attach(uint32_t ms);

/* For a module that doesn't answer while it runs: pulses RESETn and halts
 * its core in the window right after reset (as ../BGM220_Provision does),
 * up to `ms`. RAM survives a pin reset. module_swd_resume() lets it run. */
bool module_swd_reset_halt(uint32_t ms);
void module_swd_resume(void);

bool module_swd_read32(uint32_t addr, uint32_t *value);

/* `n` words from `addr` (word aligned), with the MEM-AP's auto-increment. */
bool module_swd_read_block(uint32_t addr, uint32_t *out, uint32_t n);

/* Drops the debug power-up request and lets the pins go (SWCLK pulled
 * down, SWDIO up, as the module's own pulls). */
void module_swd_detach(void);
