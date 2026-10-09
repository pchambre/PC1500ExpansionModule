/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* hw_config.c
 *
 * Tells the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library which real
 * GPIOs/SPI instance the SD card is on -- see that library's own
 * examples/simple/hw_config.c for the template this follows, and
 * board_pins.h for why these specific GPIOs matter.
 *
 * KNOWN BUG (see board_pins.h): PIN_SD_MISO is GPIO27, which is NOT a
 * valid SPI1 RX funcsel pin -- only GPIO40 is, and it's wired to
 * nothing. CS/SCK/MOSI below are correctly on real SPI1 funcsel pins;
 * MISO alone is not, so the SD card cannot actually receive data over
 * hardware SPI1 as currently wired. This is a schematic bug, not
 * something fixable here -- do not "fix" it by switching MISO to
 * software/bit-banged SPI without discussing it first, since that
 * would paper over a real hardware defect instead of routing it
 * correctly.
 */
#include "hw_config.h"

#include "board_pins.h"

static spi_t spi = {
    .hw_inst = spi1,
    .sck_gpio = PIN_SD_SCK,
    .mosi_gpio = PIN_SD_MOSI,
    .miso_gpio = PIN_SD_MISO,
    .baud_rate = 125 * 1000 * 1000 / 4  /* 31.25 MHz -- same choice as the library's own example */
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = PIN_SD_CS
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if
};

size_t sd_get_num() { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}
