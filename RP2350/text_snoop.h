/* text_snoop.h -- watching BASIC's text buffers on the bus (2026-10-17): a
 * trial of the internal card's text-display idea (the IWS-style 80x25
 * console in the host app) on a plain dongle, built only with
 * -DPC1500_TEXT_SNOOP=ON (its own build directory).
 *
 * Hardware for the trial: GreenPAK2 IO12 is a second write trigger, high
 * for ME0 writes to 7000H-7FFFH (2026-10-18: was 7800H-7FFFH; now the LCD's
 * dot memory too), jumpered to GPIO28 -- where the
 * SC18IS602B's INT bodge wire was, which must be disconnected (it pulls
 * the line low after every SPI transfer). MCONF BRIDGEINT is ignored in
 * this build.
 *
 * A PIO state machine (text_snoop.pio, on pio2) samples every such write's
 * address and data into a DMA ring with no CPU involved, like the
 * existing write path. text_snoop_poll(), on core1 between commands,
 * drains it and keeps a copy of the text buffers (7B60H-7BFFH) and the
 * cursor/mode bytes (787EH-7880H). Once a burst of writes settles it
 * reports, on the USB console for now:
 *   [line] a line entered or listed, tokens back to text (basic_detok.h)
 *   [out ] PRINT's output
 *   [type] / [edit] the line being typed or edited, @ its LCD column.
 * Build with -DTEXT_SNOOP_RAW for every captured write too. No sleep in
 * this build. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef PC1500_TEXT_SNOOP
/* PIO and DMA up; core0, after the bus PIO setup. */
void text_snoop_init(void);

/* Drains the ring to the USB console. True if there was anything. core1. */
bool text_snoop_poll(void);

/* BLCLS: whatever the LCD has shown so far, then a CLEAR record, to the
 * Link now. core1. */
void text_snoop_clear(void);

/* The LCD's indicators as read now ([764EH][764FH], BLCON): sent to the
 * Screen pane, and the base later changes are seen against. core1. */
void text_snoop_set_status(const uint8_t status[2]);

/* What the module ROM draws straight onto the LCD (ROM1's DISP_N_CHARS0),
 * which never passes through BASIC's buffers: a SHOW line, and a BROWSE
 * listing (SDLS, MLOG VIEW, MCONF, ...) -- all of it, not just the row on
 * the LCD. Queued as OUTPUT records after whatever the LCD showed before;
 * sent once the command is done. core1 (keywords.c). */
void text_snoop_screen_line(const uint8_t *chars, size_t len);
void text_snoop_listing(const uint8_t *window);

/* The SSH terminal (ssh_session.c, 2026-10-18): a finished line, or the
 * live one with its cursor (0-79, -1 none) -- the whole 80 columns, not
 * the LCD's 26. core1. */
void text_snoop_terminal(const char *text, size_t len, int cursor, bool finished);
#endif
