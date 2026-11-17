/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* swd.h -- a bit-banged ARM SWD host (ADIv5), MEM-AP word access only.
 * Enough to halt a Cortex-M33 and drive its flash controller. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SWD_ACK_OK 1
#define SWD_ACK_WAIT 2
#define SWD_ACK_FAULT 4
#define SWD_ACK_PARITY 8 /* ours: a read's parity bit was wrong */

void swd_init(unsigned swclk_gpio, unsigned swdio_gpio);

/* Line reset, JTAG-to-SWD, and the dormant wake-up if that isn't enough;
 * then reads DP IDCODE. */
bool swd_connect(uint32_t *idcode);

/* Debug and system power-up (DP CTRL/STAT), then MEM-AP 0's IDR and CSW
 * (32-bit, secure, no increment). */
bool swd_power_up(uint32_t *ap_idr);

bool swd_read32(uint32_t addr, uint32_t *value);
bool swd_write32(uint32_t addr, uint32_t value);

/* The last transfer's ACK (SWD_ACK_*), for messages. */
int swd_last_ack(void);

/* Writes MEM-AP CSW and reads it back (bit 6, DeviceEn: the core's bus
 * is reachable). */
bool swd_set_csw(uint32_t csw, uint32_t *readback);
