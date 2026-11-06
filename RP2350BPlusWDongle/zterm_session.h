/* zterm_session.h -- ZIP/1500's console on the firmware (2026-10-29,
 * zmachine/DESIGN.md): zterm.c with its transcript in flash, and the game
 * kept across a POWMAN sleep.
 *
 * POWMAN reboots the MCU: its RAM and the SD channels are gone. So before
 * it powers down, zterm_session_suspend() writes the transcript's last
 * partial page and a session record (the console's state, the story's
 * path and channel); after the wake, zterm_session_resume() reads them
 * back and says which file to reopen on which channel, before the first
 * command is answered. The game itself is in the PC-1500's RAM, which
 * never noticed. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* EXP_COMMAND_ZTERM. story_path: for OPEN, the story's file as FatFs
 * opens it from anywhere (an absolute path), or "" if not known. */
uint8_t zterm_session_command(uint8_t *window, const char *story_path);

/* core1, before a POWMAN power-down. */
void zterm_session_suspend(void);

/* After a POWMAN wake: true if a game was open, with the story to reopen
 * (path, as given to OPEN) on *channel. */
bool zterm_session_resume(char *path, size_t path_size, uint8_t *channel);
