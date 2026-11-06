/* qmi_cs1_spi.c -- see qmi_cs1_spi.h for the full explanation.
 *
 * Register semantics used here (verified against the installed Pico SDK
 * 2.3.0's hardware/regs/qmi.h and hardware/structs/qmi.h, RP2350):
 *   - QMI_DIRECT_CSR.EN: enables direct (bit-banged-by-hardware) serial
 *     mode. "Direct serial mode allows the processor to send and receive
 *     raw serial frames... Only SPI mode 0 (CPOL=0 CPHA=0) is supported"
 *     -- exactly SD SPI mode's own requirement.
 *   - QMI_DIRECT_CSR.CLKDIV: SCK frequency = clk_sys / CLKDIV (1..255
 *     direct, 0 encodes 256). Documented as safe to change on-the-fly;
 *     "the serial interface will sample the latest clock divisor each
 *     time it begins the transmission of a new byte."
 *   - QMI_DIRECT_CSR.ASSERT_CS1N: "When 1, assert (i.e. drive low) the
 *     CS1n chip select line... applies even when DIRECT_CSR_EN is 0" --
 *     used here to select the SD card instead of the flash (CS0).
 *   - QMI_DIRECT_TX.OE: output-enable for this pushed word -- 1 to
 *     actually drive DATA onto the bus (used for both real writes and
 *     read phases, since SD SPI is full-duplex and the master must keep
 *     driving MOSI, conventionally 0xFF, while clocking in a response).
 *   - QMI_DIRECT_TX.DWIDTH: 0 = 8-bit transfer, 1 = 16-bit. Always 0
 *     here (byte-oriented).
 *   - QMI_DIRECT_TX.IWIDTH: transfer width -- QMI_DIRECT_TX_IWIDTH_VALUE_S
 *     (0) is standard single-wire (1-bit) SPI, matching SD SPI mode
 *     (as opposed to dual/quad, used for XIP flash reads, not here).
 *   - QMI_DIRECT_RX: one byte pushed here per byte clocked, read after
 *     confirming DIRECT_CSR.RXEMPTY is 0.
 */
#include "qmi_cs1_spi.h"

#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "pico.h" /* pulls in pico/platform.h correctly -- including it directly errors out */

void qmi_cs1_spi_init(uint8_t cs1_gpio) {
    /* GPIO_FUNC_XIP_CS1 is only valid on GPIO 0, 8, or 19 on RP2350A --
     * the caller (board_pins.h's PIN_SD_CS1) is responsible for that
     * constraint; this function does not re-validate it. */
    gpio_set_function(cs1_gpio, GPIO_FUNC_XIP_CS1);

    /* Start at a slow, SD-card-init-safe clock (roughly the standard
     * ~400kHz SD initialization rate) -- the caller switches to the
     * card's real operating speed via qmi_cs1_spi_set_baudrate() only
     * after CMD0/CMD8/ACMD41 init succeeds, per the SD SPI-mode spec. */
    uint32_t csr = qmi_hw->direct_csr;
    csr &= ~QMI_DIRECT_CSR_CLKDIV_BITS;
    csr |= (200u << QMI_DIRECT_CSR_CLKDIV_LSB) & QMI_DIRECT_CSR_CLKDIV_BITS;
    /* Never auto-assert CS1 on FIFO activity -- this driver always
     * asserts/deasserts it explicitly, so a transaction's boundaries are
     * exactly transaction_begin()/transaction_end(), never implicit. */
    csr &= ~(QMI_DIRECT_CSR_AUTO_CS1N_BITS | QMI_DIRECT_CSR_AUTO_CS0N_BITS);
    qmi_hw->direct_csr = csr;
}

void qmi_cs1_spi_set_baudrate(uint32_t sys_clk_hz, uint32_t baud_hz) {
    uint32_t clkdiv = sys_clk_hz / baud_hz;
    if (clkdiv < 1) clkdiv = 1;
    if (clkdiv > 256) clkdiv = 256;
    uint32_t encoded = (clkdiv == 256) ? 0 : clkdiv;

    uint32_t csr = qmi_hw->direct_csr;
    csr &= ~QMI_DIRECT_CSR_CLKDIV_BITS;
    csr |= (encoded << QMI_DIRECT_CSR_CLKDIV_LSB) & QMI_DIRECT_CSR_CLKDIV_BITS;
    qmi_hw->direct_csr = csr;
}

void __not_in_flash_func(qmi_cs1_spi_transaction_begin)(void) {
    /* Enable direct mode first, then assert CS1 -- both are simple
     * register writes; order matters only in that EN must be set before
     * any transfer_byte() call, and ASSERT_CS1N is documented to apply
     * even with EN=0, so asserting before or after EN is functionally
     * equivalent, but doing EN first keeps the "transaction" concept
     * (direct mode active) bracketing the "device selected" concept. */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

void __not_in_flash_func(qmi_cs1_spi_transaction_end)(void) {
    /* Deassert CS1 first (release the card), then leave direct mode so
     * XIP flash fetches can resume immediately. */
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
}

void __not_in_flash_func(qmi_cs1_spi_send_init_clocks)(int n_bytes) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    /* Deliberately do NOT assert CS0N or CS1N here -- both stay
     * deasserted, matching SD SPI's power-up requirement of clocking
     * with the card unselected. */
    for (int i = 0; i < n_bytes; i++) {
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                           | (QMI_DIRECT_TX_IWIDTH_VALUE_S << QMI_DIRECT_TX_IWIDTH_LSB)
                           | 0xFFu;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
        }
        (void)qmi_hw->direct_rx;
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
}

uint8_t __not_in_flash_func(qmi_cs1_spi_transfer_byte)(uint8_t tx) {
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                       | (QMI_DIRECT_TX_IWIDTH_VALUE_S << QMI_DIRECT_TX_IWIDTH_LSB)
                       | (uint32_t)tx;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
        /* spin -- one byte at typical SD SPI clock rates is a handful of
         * microseconds at most; not worth a wfe/timeout here. */
    }
    return (uint8_t)(qmi_hw->direct_rx & 0xFFu);
}
