/* ble_rn4871.h
 *
 * Minimal driver for the RN4871 BLE module in UART transparent (streaming)
 * mode -- hardware UART0 on board_pins.h's PIN_BLE_UART_TX/RX (GPIO32/33,
 * checked against the RP2350 GPIO funcsel table as a real UART0 TX/RX
 * pair) plus the wake pin (GPIO34, user's explicit choice: RN4871 P1_6,
 * a generic GPIO the module's own script must be configured to treat as
 * a wake source -- not a dedicated hardware wake pin).
 *
 * UNCONFIRMED against real hardware, same caveat as the rest of this
 * port: 115200 baud is the RN4871 factory-default UART rate per
 * Microchip's datasheet, not yet verified against this specific module/
 * firmware version. The module also needs to actually be configured
 * (via its own ASCII command mode, $$$ escape sequence, separate from
 * this driver) to run in transparent UART mode before this driver's
 * ble_rn4871_send/recv are meaningful -- that one-time provisioning step
 * isn't part of this driver.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BLE_RN4871_BAUD_RATE 115200

/* Configures the UART0 instance and the wake GPIO. Call once at startup,
 * before any send/recv/wake call. */
void ble_rn4871_init(void);

/* Drives the wake pin high (assert) or lets it float low (deassert) --
 * polarity/duration requirements depend on how the module's own onboard
 * script is configured; not yet determined against real hardware. */
void ble_rn4871_set_wake(bool asserted);

/* Blocking transparent-mode send -- writes len bytes of buf out over
 * the UART link to the module (which forwards them over the active BLE
 * connection, once one exists). */
void ble_rn4871_send(const uint8_t *buf, uint32_t len);

/* Returns true if at least one byte is available to read without
 * blocking. */
bool ble_rn4871_readable(void);

/* Blocking read of exactly one byte -- only call after ble_rn4871_readable()
 * returns true, or this will block until the module sends something. */
uint8_t ble_rn4871_read_byte(void);
