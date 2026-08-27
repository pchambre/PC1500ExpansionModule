/* ble_rn4871.c -- see ble_rn4871.h for the why and the open caveats. */
#include "ble_rn4871.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"

#include "board_pins.h"

#define BLE_UART uart0

void ble_rn4871_init(void) {
    uart_init(BLE_UART, BLE_RN4871_BAUD_RATE);
    gpio_set_function(PIN_BLE_UART_TX, UART_FUNCSEL_NUM(BLE_UART, PIN_BLE_UART_TX));
    gpio_set_function(PIN_BLE_UART_RX, UART_FUNCSEL_NUM(BLE_UART, PIN_BLE_UART_RX));
    uart_set_hw_flow(BLE_UART, false, false);
    uart_set_format(BLE_UART, 8, 1, UART_PARITY_NONE);

    gpio_init(PIN_BLE_WAKE);
    gpio_set_dir(PIN_BLE_WAKE, GPIO_OUT);
    gpio_put(PIN_BLE_WAKE, 0);
}

void ble_rn4871_set_wake(bool asserted) {
    gpio_put(PIN_BLE_WAKE, asserted);
}

void ble_rn4871_send(const uint8_t *buf, uint32_t len) {
    uart_write_blocking(BLE_UART, buf, len);
}

bool ble_rn4871_readable(void) {
    return uart_is_readable(BLE_UART);
}

uint8_t ble_rn4871_read_byte(void) {
    return uart_getc(BLE_UART);
}
