/* history_session.h -- BASIC's command history on the dongle (2026-10-07,
 * MCONF HISTORY): EXP_COMMAND_HIST_* (pc_exp.h). cmd_history.h's history
 * and UI, the history kept in flash (mcu_store.c slot EXP_STORE_SLOT_HISTORY,
 * a ring of one 256-byte page per command), and the browsing run between
 * commands while the ROM's TERM_RUN has the window, as ssh_session.h's
 * terminal is. core1 only. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

uint8_t history_session_command(uint8_t command, uint8_t *window);
bool history_session_terminal(void);
void history_session_poll(uint8_t *window);
