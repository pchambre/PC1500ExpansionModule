/* kbd.h -- the BLE keyboard host (HID over GATT), MODULE_PROTOCOL.md's
 * KBD_* requests and events (2026-10-13). One keyboard: paired once
 * (KBD_PAIR -- the PC-1500 shows the passkey, it's typed on the keyboard),
 * bonded in the stack's database, its record in NVM3; then, with
 * KBD_ENABLE, reconnected in the background whenever it advertises. Its
 * keyboard reports go to the RP2354B as 8-byte boot reports (KBD_REPORT),
 * read through its Report Map (hidmap.h). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sl_bt_api.h"

void kbd_init(void); /* at the stack's boot event */
/* The keyboard's connection's events; true if it took the event. */
bool kbd_on_event(sl_bt_msg_t *evt);
/* Every advertising report while anyone scans. */
void kbd_on_adv(const sl_bt_evt_scanner_legacy_advertisement_report_t *e);
/* RQ_KBD_*: the status byte, answer in `a`. */
uint8_t kbd_request(uint8_t type, const uint8_t *p, size_t len, uint8_t *a, size_t *n);
/* Main loop: timers, and the background scan. */
void kbd_tick(void);
/* KBD_PAIR is searching or connecting. */
bool kbd_pairing(void);
