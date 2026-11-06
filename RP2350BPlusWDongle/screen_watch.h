/* screen_watch.h -- the RP2350's side of screen.h (2026-11-03): BASIC's
 * text buffers watched on the bus, and screen.h's backend (the Link,
 * MCONF BLDISP, the module ROM image).
 *
 * GreenPAK2 IO12 is a second write trigger, high for ME0 writes to
 * 7000H-7FFFH (GP2-MCU-DongleMultiLineDisplay.gp6 on the dongle), on
 * PIN_SCREEN_TRIG: the card's GPIO24 (TRIG_WR_SYS); the dongle's GPIO28,
 * shared with the SD bridge's INT -- J3 takes one or the other, a wire from
 * J5 pin 2 the trigger. With the trigger wired there, MCONF BRIDGEINT=0;
 * nothing checks it.
 *
 * A PIO state machine (screen_watch.pio, on pio2) samples every such
 * write's address and data into a DMA ring with no CPU involved, like the
 * write path; screen_watch_drain() takes them to screen_watch_write(). */
#pragma once

/* PIO, DMA and screen_init(); core0, after the bus PIO setup. */
void screen_watch_init(void);

/* The ring so far to screen_watch_write(). core1 (screen_poll() calls it,
 * and the Link's wait hook). */
void screen_watch_drain(void);
