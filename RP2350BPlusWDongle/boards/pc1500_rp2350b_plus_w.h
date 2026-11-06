/* pc1500_rp2350b_plus_w.h -- Pico SDK board header for the Waveshare
 * RP2350B-Plus-W used as the dongle's MCU (RP2350BPlusWDongle).
 *
 * The SDK's own pico2_w.h with this board's differences, from Waveshare's
 * schematic (files.waveshare.com/wiki/RP2350B-Plus-W/RP2350B-Plus-W.pdf):
 * - RP2350B (80 pins, GPIO0-47), not RP2350A.
 * - W25Q128JV flash, 16 MB; PSRAM on QMI CS1 (GPIO47), unused here.
 * - The RM2 radio module (CYW43439, as on the Pico 2 W) on GPIO36-39:
 *   WL_ON 36, WL_D 37 (data in/out and host wake), WL_CS 38, WL_CLK 39.
 *   GPIO23 drives a red LED instead.
 * - VSYS sensed on GPIO46 (VSYS_SENSE, ADC6), not shared with WL_CLK.
 * - Header pins 31/32/34 are GPIO40/41/42 (ADC0-2); the chip's own
 *   GPIO26-28 only reach test points (see board_pins_dongle.h).
 */
#ifndef _BOARDS_PC1500_RP2350B_PLUS_W_H
#define _BOARDS_PC1500_RP2350B_PLUS_W_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)

#define PC1500_RP2350B_PLUS_W

#define PICO_RP2350A 0  /* RP2350B */

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

/* The radio's own GPIOs (on the CYW43439), as on the Pico 2 W */
#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif
#ifndef CYW43_WL_GPIO_SMPS_PIN
#define CYW43_WL_GPIO_SMPS_PIN 1
#endif
#ifndef CYW43_WL_GPIO_VBUS_PIN
#define CYW43_WL_GPIO_VBUS_PIN 2
#endif

/* VSYS has a pin of its own */
#ifndef CYW43_USES_VSYS_PIN
#define CYW43_USES_VSYS_PIN 0
#endif
#ifndef PICO_VSYS_PIN
#define PICO_VSYS_PIN 46
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#ifndef CYW43_PIN_WL_DYNAMIC
#define CYW43_PIN_WL_DYNAMIC 0
#endif
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 36u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 39u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 38u
#endif

#endif
