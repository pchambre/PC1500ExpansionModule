/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* kbd_host.h -- the external keyboard's Bluetooth side on the Pico 2 W
 * (2026-10-04, MCONF BLKBD): a classic Bluetooth HID host on the CYW43's
 * BTstack, alongside the Link (ble_link.c). The keyboard is paired once
 * (BLKBD), bonded (bt_store.c), and after that connects by itself when a
 * key is pressed. Its reports, in boot protocol, go to the key sequencer
 * (kbd_seq.h), whose key the ROM's driver reads from the data window.
 *
 * The radio side only: the mapping and the sequencer are portable
 * (kbd_seq.c) and shared with other builds and with pc1500emu -- an
 * internal-card build with the RN4871 would put its own transport here.
 *
 * Threading: BTstack's callbacks run in its async context on core0 (a
 * background interrupt); kbd_host_publish() runs in core0's bus loop and
 * takes the reports from them through a small ring; kbd_host_command()
 * runs on core1 and reaches BTstack with async_context_execute_sync(). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core0: MCONF BLKBD is on -- the radio and BTstack stay up for the
 * keyboard (ble_link_wanted()). */
bool kbd_host_wanted(void);

/* core0, once, where BTstack is set up (ble_link_poll()). */
void kbd_host_stack_init(void);

/* core0, every bus-loop pass: the keyboard's reports into the sequencer,
 * and its key and ON count into the window (EXP_KBD_KEY, EXP_KBD_BREAK;
 * `window` = offset 0 of the data window). */
void kbd_host_publish(uint8_t *window);

/* core1: EXP_COMMAND_KBD_PAIR/STATUS/STOP/FORGET (pc_exp.h). Returns the
 * status. */
uint8_t kbd_host_command(uint8_t command, uint8_t *window);
