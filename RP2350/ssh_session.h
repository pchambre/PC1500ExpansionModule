/* ssh_session.h -- the SSH keyword's session on the dongle (2026-10-07):
 * EXP_COMMAND_SSH_* (pc_exp.h). Runs ssh_client.h over an lwIP TCP
 * connection, its output into ssh_term.h, the PC-1500's keys through
 * ssh_keys.h; the device key and known hosts are ssh_store.h's.
 *
 * Threading, as wifi_link.h: lwIP runs in the CYW43's async context on
 * core0, which only copies what arrives into a ring and sends what it's
 * given. Everything else -- the protocol and its crypto, the terminal, the
 * keys -- runs on core1: in the SSH commands, and, while the TERM action
 * has the window, between commands (ssh_session_poll(), from monitor.c's
 * core1 loop), so the bus loop on core0 never waits on any of it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core1: an EXP_COMMAND_SSH_* command. `window` is the data window. */
uint8_t ssh_session_command(uint8_t command, uint8_t *window);

/* core1: true while the TERM action has the window: the core1 loop calls
 * ssh_session_poll() instead of sleeping until the next command. */
bool ssh_session_terminal(void);

/* core1: moves the session on, takes the keys the ROM reported, and puts
 * the line to show in the window. Returns quickly. */
void ssh_session_poll(uint8_t *window);
