/* app.h -- what app.c (the Link) shares with kbd.c (the keyboard). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One scanner, two users: the Link's SCAN_START (it wins) and the
 * keyboard's pairing search or background reconnection. */
enum { SCAN_NONE = 0, SCAN_LINK = 1, SCAN_KBD = 2 };
extern uint8_t scanner_owner;
/* Starts scanning for `owner` with these parameters (0.625 ms units);
 * false if the Link has it and `owner` is the keyboard. */
bool scanner_take(uint8_t owner, bool active, uint16_t interval, uint16_t window);
/* Stops it, if `owner` has it. */
void scanner_release(uint8_t owner);

/* The Link is opening a connection: the keyboard's waits (one opening at a
 * time). */
bool app_link_connecting(void);
