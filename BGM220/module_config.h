/* module_config.h -- pins and sizes for the BGM220S firmware.
 * The contract with the RP2354B is ../RP2350/MODULE_PROTOCOL.md. */
#pragma once

#include "em_gpio.h"

#define MODULE_PROTOCOL_VERSION 0
#define MODULE_FW_VERSION "0.1"

/* HOST_WAKE: high while events are queued (sec.4). PA04 on the card; on the
 * Explorer Kit it's LED0, so the LED shows queued events. */
#define HOST_WAKE_PORT gpioPortA
#define HOST_WAKE_PIN 4
/* Driven alike: the Explorer Kit's mikroBUS PWM pin, where an RP2354B wired
 * to the kit takes HOST_WAKE (README.md). Unconnected on the card. */
#define HOST_WAKE2_PORT gpioPortB
#define HOST_WAKE2_PIN 4

/* MODULE_WAKE: the RP2354B holds it high for a session (sec.4). PB03, an
 * EM4 wake pin. Pulled up here, so on the Explorer Kit, where nothing drives
 * it, the module behaves as if a session were always open. On the card the
 * RP2354B drives it. */
#define MODULE_WAKE_PORT gpioPortB
#define MODULE_WAKE_PIN 3

#define FRAME_MAX_PAYLOAD 300 /* sec.5 */
#define LINK_FRAME_MAX 244    /* MTU 247 - 3 (BLE_PROTOCOL.md sec.2) */
#define EVENT_QUEUE_SLOTS 8   /* sec.7 */
#define NAME_MAX 15
