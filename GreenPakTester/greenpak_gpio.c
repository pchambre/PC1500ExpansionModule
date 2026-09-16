/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_gpio.h"
#include "greenpak_pins.h"

#include "hardware/gpio.h"

void greenpak_gpio_init_all(void) {
    for (uint8_t i = 0; i < GREENPAK_IO_COUNT; i++) {
        uint32_t gpio = greenpak_io_map[i].gpio;
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
    }
}

void greenpak_gpio_drive(uint8_t io_index, bool level) {
    uint32_t gpio = greenpak_gpio_for_io(io_index);
    gpio_put(gpio, level);
    gpio_set_dir(gpio, GPIO_OUT);
}

void greenpak_gpio_release(uint8_t io_index) {
    gpio_set_dir(greenpak_gpio_for_io(io_index), GPIO_IN);
}

void greenpak_gpio_release_pulled_down(uint8_t io_index) {
    uint32_t gpio = greenpak_gpio_for_io(io_index);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_down(gpio);
}

bool greenpak_gpio_read(uint8_t io_index) {
    return gpio_get(greenpak_gpio_for_io(io_index));
}
