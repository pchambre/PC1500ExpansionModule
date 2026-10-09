/* ble_link.h -- the PC-1500 Link over the CYW43's Bluetooth (2026-09-27):
 * BLE_PROTOCOL.md, as the connecting side (central). The BL* keywords reach
 * it through EXP_COMMAND_BLE_* (pc_exp.h), and a BLSAVE/BLLOAD's
 * READ/WRITE/CLOSE_SD_FILE come here instead of the card while a BLE
 * transfer is open.
 *
 * Threading: BTstack runs in the CYW43's async context on core0 (all CYW43
 * calls must stay there -- main.c). ble_link_command() runs on core1
 * (DoCommand) and blocks, with timeouts, while core0 does the work; it
 * reaches BTstack with async_context_execute_sync(). core0's loop keeps the
 * radio up and BTstack powered while the link is wanted
 * (ble_link_poll()). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core1: an EXP_COMMAND_BLE_* command, or READ/WRITE/CLOSE_SD_FILE while
 * ble_link_transfer_open(). `window` is the data window (offset 0 =
 * EXP_BUFFER_START_ABS). Returns the status. */
uint8_t ble_link_command(uint8_t command, uint8_t *window);

/* True while a BLSAVE/BLLOAD transfer owns READ/WRITE/CLOSE_SD_FILE. */
bool ble_link_transfer_open(void);

/* core0: true while the radio must stay up (a connection, or a command
 * using it) -- no DORMANT sleep then. */
bool ble_link_wanted(void);

/* core0, every monitor_run() pass: powers BTstack on or off to match
 * ble_link_wanted(), once the CYW43 is up (`radio_up`). */
void ble_link_poll(bool radio_up);

/* core1, for another module's command that needs BTstack running
 * (kbd_host.c's BLKBD FORGET with MCONF BLKBD=0): the radio up and BTstack
 * working, as for a BL* command -- false (logged) if it doesn't come up --
 * then let go again once nothing needs it. */
bool ble_link_stack_acquire(void);
void ble_link_stack_release(void);

/* core1, right after BLCON (time_sync.h): the host app's clock -- UTC in
 * Unix ms, and its UTC offset in seconds -- asked for with a TIME frame.
 * False (logged) with no link, a link to another PC-1500 (never asked), or
 * an app too old to answer. */
bool ble_link_host_time(int64_t *utc_ms, int32_t *offset_s);

/* core0, BTstack's context: the Link is scanning or making a connection --
 * BTstack allows one outgoing LE connection at a time, so a BLE keyboard's
 * background reconnection (kbd_host.c) waits until it's done. */
bool ble_link_le_busy(void);
