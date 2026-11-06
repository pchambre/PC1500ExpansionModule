/* sd_bus.h -- the SPI transport under the SD card driver, diskio_sd.c
 * (2026-10-09).
 *
 * Two implementations, one per board (CMakeLists.txt):
 *   sd_bus_bridge.c -- the dongle: the SC18IS602B I2C-to-SPI bridge (U5),
 *                      sc18is602b.h, on the GreenPAKs' I2C bus.
 *   sd_bus_spi.c    -- the internal card: SPI1 straight to the microSD
 *                      socket, board_pins_card.h.
 * diskio_sd.c's SD protocol is the same on both. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The longest transfer that stays one CS-low burst: the bridge's real
 * ceiling (SC18IS602B_MAX_CHUNK). diskio_sd.c frames its commands, tokens
 * and payload chunks against it; SPI holds CS for any length, but keeps the
 * same number so the SD framing is identical on both boards. */
#define SD_BUS_MAX_CHUNK 199

/* Brings the transport up at the SD init clock (<=400 kHz), CS released.
 * False if the transport itself didn't answer (the bridge didn't ACK). */
bool sd_bus_open_slow(void);

/* Switches to the data clock once the card is initialized. */
bool sd_bus_set_fast(void);

#ifdef PC1500_SD_SPI
/* SPI only (2026-10-12): the data clock steps down -- 25, 12.5, 6.25, 3,
 * 1 MHz -- when diskio_sd.c finds the card misreading at the current one
 * (a hand-wired socket can't do 25 MHz). False at the slowest. */
bool sd_bus_slower(void);
uint32_t sd_bus_clock_khz(void);

/* SPI only (2026-10-12): while held, sd_bus_transfer() leaves CS low
 * between calls, so a whole block read or write is one CS-low transaction,
 * as the SD spec requires. Releasing raises CS and clocks one byte. The
 * bridge can't do this (199-byte bursts), and the dongle's cards tolerate
 * the CS toggling; the first prototype card's SDSC card didn't: a slow
 * card polled for its data token one CS-toggled byte at a time never sent
 * it, then answered nothing at all. */
void sd_bus_hold(bool on);
#endif

/* `len` dummy 0xFF bytes with CS HIGH -- the SD power-up preamble (>=74
 * clocks). */
bool sd_bus_clock_only(uint32_t len);

/* Full-duplex transfer of `len` bytes with CS asserted for the call and
 * released after it. `tx` NULL sends 0xFF; `rx` NULL discards what comes
 * back. False on a transport failure. */
bool sd_bus_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

/* The bridge's diagnostic counters (sc18is602b.h); zeros on SPI. */
void sd_bus_reset_stats(void);
void sd_bus_int_stats(uint32_t *outImmediate, uint32_t *outWaited, uint32_t *outTimeout);
