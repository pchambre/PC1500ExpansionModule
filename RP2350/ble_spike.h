/* ble_spike.h -- BLE bring-up spike (2026-09-27, step 0 of the BLE plan):
 * with MCONF BLE=1, the CYW43 stays up and BTstack scans passively for LE
 * advertisers, so the bus, power draw and flash/RAM cost of running
 * Bluetooth next to the PC-1500 bus can be measured before the real link
 * (ble_link.h, later) is written. Not the final design. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* True while MCONF BLE=1: the radio must stay up (no DORMANT sleep). */
bool ble_spike_wanted(void);

/* core0, every monitor_run() loop pass, after the CYW43 is up
 * (`radio_up`). Starts or stops the scan to match MCONF BLE. */
void ble_spike_poll(bool radio_up);

/* core1: a one-line summary for the MCU log ("BLE 12d 340A NAME": devices,
 * advertising reports, the last advertised name)
 * into `msg`, if anything new was seen since the last call. */
bool ble_spike_take_summary(char *msg, size_t size);
