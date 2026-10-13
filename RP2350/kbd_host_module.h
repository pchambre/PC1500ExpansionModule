/* kbd_host_module.h -- the internal card's half of kbd_host.h
 * (kbd_host_module.c), for ble_link.c's module pump and monitor.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core1, from ble_link.c's module_pump(): KBD_STATE, KBD_PASSKEY and
 * KBD_REPORT events (MODULE_PROTOCOL.md sec.7). */
void kbd_module_event(uint8_t ev, const uint8_t *p, uint16_t n);

/* core1: tells the module MCONF BLKBD (KBD_ENABLE) when it changed, or the
 * module restarted. */
void kbd_module_sync(void);
bool kbd_module_needs_sync(void);
void kbd_module_restarted(void);

/* core0, before sleep: a key is held or the sequencer is mid-sequence --
 * the ROM reads the key from the window, which a sleeping MCU can't serve. */
bool kbd_module_busy(void);
