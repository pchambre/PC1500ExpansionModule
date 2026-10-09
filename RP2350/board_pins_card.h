/* board_pins_card.h (included through board_pins.h, PC1500_TARGET=card)
 *
 * GPIO assignment for the RP2354B internal expansion card,
 * PC1500-RP2354B-BLE-more-RAM (2026-10-09). The PIO bus serving
 * (read_serve.pio/write_serve.pio) needs the address bus, the data bus and
 * both trigger lines inside ONE PIO block's 32-GPIO window, so the card is
 * being rewired to put D0-D7 on GPIO16-23, in order. The schematic as of
 * 2026-10-09 still has D2/D1/D0 on GPIO16-18 and D3-D7 on GPIO35-39 --
 * this table is the rewired board, not that one.
 *
 * GreenPAK1 (SRAM decoding) and GreenPAK2 (TRIG_RD/TRIG_WR to the MCU) sit
 * behind a TCA9406 on one shared I2C bus, as on the dongle. */
#pragma once

/* ---- LH5801-facing bus. AD0-AD15 are all wired (GPIO0-15), but the MCU
 * serves the same 13-bit, 8K window (0x8000-0x9FFF) as the dongle; AD13-15
 * matter only to the GreenPAKs. ---- */
#define PIN_A0  0
#define PIN_A1  1
#define PIN_A2  2
#define PIN_A3  3
#define PIN_A4  4
#define PIN_A5  5
#define PIN_A6  6
#define PIN_A7  7
#define PIN_A8  8
#define PIN_A9  9
#define PIN_A10 10
#define PIN_A11 11
#define PIN_A12 12
#define ADDR_PIN_BASE  PIN_A0
#define ADDR_PIN_COUNT 13
#define ADDR_PIN_MASK  ((1u << ADDR_PIN_COUNT) - 1u)

/* ---- Data bus D0-D7: GPIO16-23, contiguous and in order (the rewire). ---- */
#define PIN_D0 16
#define PIN_D1 17
#define PIN_D2 18
#define PIN_D3 19
#define PIN_D4 20
#define PIN_D5 21
#define PIN_D6 22
#define PIN_D7 23
#define DATA_PIN_BASE  PIN_D0
#define DATA_PIN_COUNT 8

/* ---- GreenPAK2's trigger outputs (U9 IO6/IO7 -> GPIO30/31). ---- */
#define PIN_TRIG_RD 30  /* CS && Read && OE -- drive the data bus now */
#define PIN_TRIG_WR 31  /* CS && Write -- latch the data bus now; the sleep wake source */

/* ---- GreenPAK I2C, through U10 (TCA9406) to both GreenPAKs: GPIO28/29
 * are I2C0's SDA/SCL. ---- */
#define PIN_GREENPAK1_SDA 28
#define PIN_GREENPAK1_SCL 29
#define PIN_GREENPAK2_SDA PIN_GREENPAK1_SDA
#define PIN_GREENPAK2_SCL PIN_GREENPAK1_SCL
#define GREENPAK_I2C_INSTANCE i2c0

/* ---- microSD on SPI1 (J1): GPIO40-43 are SPI1's RX/CSn/SCK/TX in order.
 * CS is driven as a plain GPIO (sd_bus_spi.c). No SD bridge, so no
 * PIN_SD_BRIDGE_INT: the bridge's INT code compiles out. ---- */
#define PIN_SD_MISO 40
#define PIN_SD_CS   41
#define PIN_SD_SCK  42
#define PIN_SD_MOSI 43
#define SD_SPI_INSTANCE spi1

/* ---- BLE module (BGM220S, replacing the RN4871), UART0 with flow
 * control. Reserved: no BLE stack is linked yet (the stack choice is
 * pending), so nothing drives these pins. ---- */
#define PIN_BLE_UART_TX   32  /* MCU -> module */
#define PIN_BLE_UART_RX   33  /* module -> MCU */
#define PIN_BLE_UART_CTS  34
#define PIN_BLE_UART_RTS  35
#define PIN_BLE_RST       36
#define PIN_BLE_HOST_WAKE 37  /* module -> MCU: a second sleep wake source, later */

/* GreenPAK2 IO14 (U9 pin 20): wired to GPIO25, spare in firmware. */
#define PIN_GREENPAK2_IO14 25
