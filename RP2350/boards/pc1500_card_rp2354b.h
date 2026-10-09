/* pc1500_card_rp2354b.h -- the SDK board definition for the internal
 * expansion card, PC1500-RP2354B-BLE-more-RAM (2026-10-09; PC1500_TARGET=card
 * in CMakeLists.txt selects it).
 *
 * An RP2354B: the RP2350B package (48 GPIOs, QFN-80) with 2 MB of flash in
 * the package, a 12 MHz crystal (X1), USB-C for stdio and flashing, no LED.
 * Its GPIOs are board_pins_card.h's; nothing here claims a default UART,
 * I2C or SPI pin, since every low GPIO is the PC-1500 bus. */
#ifndef _BOARDS_PC1500_CARD_RP2354B_H
#define _BOARDS_PC1500_CARD_RP2354B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define PC1500_CARD_RP2354B
#define PICO_RP2350A 0 /* the B package: GPIO30-47 exist */

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
