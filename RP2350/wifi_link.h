/* wifi_link.h -- Wi-Fi station mode on the CYW43 (2026-10-06): the WF*
 * keywords' scan, connect, disconnect and status (EXP_COMMAND_WIFI_*,
 * pc_exp.h), and the remembered networks (wifi_store.h). The base the
 * coming network keywords (PING, SSH) will run on.
 *
 * Threading, as ble_link.h: the CYW43 driver and lwIP run in the CYW43's
 * async context on core0 (pico_cyw43_arch_lwip_threadsafe_background).
 * wifi_link_command() runs on core1 (DoCommand) and blocks, with timeouts,
 * reaching them through async_context_execute_sync(). core0's loop keeps
 * the radio up while Wi-Fi is wanted -- from a command until DISCONNECT --
 * and the MCU doesn't sleep meanwhile (DORMANT takes the CYW43 down). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core1: an EXP_COMMAND_WIFI_* command. `window` is the data window
 * (offset 0 = EXP_BUFFER_START_ABS). Returns the status. */
uint8_t wifi_link_command(uint8_t command, uint8_t *window);

/* core0: true while the radio must stay up (station mode on, or a command
 * using it) -- no DORMANT sleep then. */
bool wifi_link_wanted(void);

/* core0, every monitor_run() pass: whether the CYW43 is up (`radio_up`). */
void wifi_link_poll(bool radio_up);
