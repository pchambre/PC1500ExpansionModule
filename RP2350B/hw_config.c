/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* hw_config.c
 *
 * Tells the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library which real
 * GPIOs/SPI instance the SD card is on -- see that library's own
 * examples/simple/hw_config.c for the template this follows, and
 * board_pins.h for why these specific GPIOs (a genuine hardware SPI1
 * block, checked against the RP2350 GPIO funcsel table).
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
