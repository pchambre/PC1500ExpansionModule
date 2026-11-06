/* qmi_cs1_spi.h
 *
 * Generic byte-oriented SPI-over-QMI-direct-mode driver for RP2350,
 * using the QMI (QSPI Memory Interface) peripheral's second chip select,
 * CS1 -- the same shared SCLK/SD0-3 bus normally used for XIP flash (CS0),
 * repurposed for a second SPI-mode device. This is a real, documented
 * RP2350 feature (used elsewhere for XIP PSRAM); this module drives it in
 * "direct mode" instead, for ordinary command/data SPI transactions to a
 * non-memory-mapped device (an SD card, in this project -- see
 * qmi_cs1_sd.h/.c), not memory-mapped access.
 *
 * WHY THIS EXISTS: on RP2350A (Pico 2 / Pico 2 W), CS1 only costs one
 * GPIO (the CS1-capable pin itself: GPIO 0, 8, or 19 -- fixed by silicon,
 * not a free choice) instead of the 4 a normal SPI peripheral needs
 * (SCK/MOSI/MISO/CS), because SCLK/SD0/SD1 are shared with the onboard
 * flash's own CS0. See this directory's README.md for the full
 * reasoning and register references.
 *
 * CRITICAL CONSTRAINT: QMI is a single physical bus. While direct mode is
 * enabled (a transaction is in flight on CS1), the flash cannot service
 * XIP code/data fetches for EITHER core. Any code that must keep running
 * during a transfer -- including this driver's own caller, if it has a
 * deadline -- must be linked into RAM (see pico/platform.h's
 * __not_in_flash_func), not fetched from flash, or it will stall for the
 * transaction's duration. This is the same discipline the Pico SDK
 * already requires around flash erase/program operations -- a known
 * pattern, not something new invented here.
 *
 * UNVERIFIED AGAINST REAL HARDWARE: this was written from the RP2350
 * datasheet and the Pico SDK's own register headers
 * (hardware/structs/qmi.h, hardware/regs/qmi.h), reasoned through
 * carefully, but there was no RP2350 board, SD card, or oscilloscope
 * available to actually try it against silicon. Treat it as a strong
 * starting point for hardware bring-up, not as proven-working code.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init: configures `cs1_gpio` (must be 0, 8, or 19 on RP2350A)
 * for the GPIO_FUNC_XIP_CS1 alternate function, and sets direct mode's
 * clock divider to a slow, SD-card-init-safe rate (equivalent to the
 * standard ~400kHz SD initialization clock). Does not enable direct mode
 * -- that only happens around each transaction, in
 * qmi_cs1_spi_transaction_begin/end, so XIP flash fetches are unaffected
 * outside of an actual transfer. */
void qmi_cs1_spi_init(uint8_t cs1_gpio);

/* Change the direct-mode clock divider -- call after init, e.g. to speed
 * up from the SD card's slow init clock to its normal operating clock
 * once CMD0/ACMD41 init has completed. `sys_clk_hz` is the current
 * system clock (clk_sys); the resulting SPI clock is approximately
 * sys_clk_hz / (2 * clkdiv) -- see the .c file's own comment for the
 * derivation from QMI_DIRECT_CSR_CLKDIV. */
void qmi_cs1_spi_set_baudrate(uint32_t sys_clk_hz, uint32_t baud_hz);

/* Begin a transaction: enables direct mode and asserts CS1 (drives it
 * low). Flash XIP is stalled from this point until
 * qmi_cs1_spi_transaction_end() -- keep transactions as short as
 * correctness allows, and never call this from code that isn't itself
 * RAM-resident if something else needs the flash bus concurrently (see
 * this header's own top comment). */
void qmi_cs1_spi_transaction_begin(void);

/* End a transaction: deasserts CS1 and disables direct mode, restoring
 * normal XIP flash access. */
void qmi_cs1_spi_transaction_end(void);

/* Clock `n_bytes` of 0xFF over SD0/SD1 with CS1 (and CS0) left
 * deasserted -- for protocols like SD SPI mode that require a run of
 * idle clocks with the device NOT selected before the first real command
 * (SD: >=74 clocks with CS high after power-up). Self-contained: enables
 * and disables direct mode itself, no transaction_begin/end needed
 * around it. */
void qmi_cs1_spi_send_init_clocks(int n_bytes);

/* Full-duplex single-byte transfer: clocks `tx` out (driven, standard
 * 1-bit/single-wire SPI mode) while simultaneously clocking in and
 * returning whatever the device sends back. To read from the device with
 * nothing meaningful to send, pass 0xFF (SD SPI mode's own idle-high
 * convention). Must be called between transaction_begin/end. */
uint8_t qmi_cs1_spi_transfer_byte(uint8_t tx);

/* Convenience wrapper: transfer_byte(0xFF) n times, discarding the
 * transmitted-out value -- used for read phases (e.g. reading a block's
 * data or a command's response bytes). */
static inline uint8_t qmi_cs1_spi_read_byte(void) {
    return qmi_cs1_spi_transfer_byte(0xFF);
}

#ifdef __cplusplus
}
#endif
