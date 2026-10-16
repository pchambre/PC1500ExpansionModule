/* board_pins_card.h (included through board_pins.h, PC1500_TARGET=card)
 *
 * GPIO assignment for the RP2354B internal expansion card,
 * PC1500-RP2354B-BLE-more-RAM (2026-10-09). The PIO bus serving
 * (read_serve.pio/write_serve.pio) needs the address bus, the data bus and
 * both trigger lines inside ONE PIO block's 32-GPIO window, so the card was
 * rewired (schematic, 2026-10-09) to put D0-D7 on GPIO16-23, in order --
 * before that D2/D1/D0 were on GPIO16-18 and D3-D7 on GPIO35-39. The same
 * change swapped the RN4871 for a BGM220S (below). Matches the schematic;
 * the board's routing still has to follow it.
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

/* The read path takes AD0-AD14 (2026-10-16, ROMRPLC): with AD15 dropped,
 * bus 8000H-9FFFH lands at serve-buffer offset 0000H-1FFFH as before and
 * bus C000H-FFFFH at 4000H-7FFFH, where a replacement ROM is served from
 * when GP2 raises TRIG_RD there. Writes stay 13 bits (8000H-87FFH only). */
#define READ_ADDR_PIN_COUNT 15

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
 * are I2C0's SDA/SCL. (The first prototype card had them on GPIO26/27, the
 * dongle's pins; it was rewired to 28/29 to match, 2026-10-12.) ---- */
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

/* ---- BLE module U8 (BGM220SC22WGA2, replacing the RN4871), UART0 with
 * flow control, on the module's PA05-PA08 -- the BGM220 Explorer Kit's
 * VCOM pins and the datasheet's NCP reference, so kit-tested module
 * firmware runs unchanged. Reserved: no BLE stack is linked yet (the stack
 * choice is pending), so nothing drives these pins. Schematic net names are
 * the module's view (BLE_UART_RX = the module's RX). ---- */
#define PIN_BLE_UART_TX   32  /* MCU -> module PA06 (RX), net BLE_UART_RX */
#define PIN_BLE_UART_RX   33  /* module PA05 (TX) -> MCU, net BLE_UART_TX */
#define PIN_BLE_UART_CTS  34  /* module PA07 (RTS) -> MCU, net BLE_UART_RTS */
#define PIN_BLE_UART_RTS  35  /* MCU -> module PA08 (CTS), net BLE_UART_CTS */
#define PIN_BLE_RST       36  /* module RESETn: internally pulled up, so drive low or release */
#define PIN_BLE_HOST_WAKE 37  /* module PA04 -> MCU: a second sleep wake source, later */
#define PIN_BLE_MODULE_WAKE 27 /* MCU -> module PB03 (an EM4 wake pin), net BLE_MODULE_WAKE:
                                  wakes the module before a command (2026-10-12) */
#define PIN_BLE_SWCLK     38  /* module PA01: the RP2354B can program the module over SWD */
#define PIN_BLE_SWDIO     39  /* module PA02 */

/* MCU_READY (ROMRPLC, 2026-10-16): high once the MCU serves a replacement
 * base ROM; GP2 only lets ROM_REPLACEMENT pull INHIBIT low (and raise
 * TRIG_RD for C000H-FFFFH) while it's high, so at power-on the PC-1500's
 * own ROM answers the reset vector until the MCU is ready. GPIO25 is wired
 * to GreenPAK2 IO14 (U9 pin 20) on the More-RAM card, and hand-wired to
 * the same GP2 pin on the prototype. GP2 pulls it down: low while the MCU
 * is off. */
#define PIN_MCU_READY 25
