/* sl_iostream_usart_host_config.h -- the protocol UART ("host" iostream
 * instance) to the RP2354B (../../RP2350/MODULE_PROTOCOL.md sec.2-3).
 * Our own copy of the SDK's per-instance config (pc1500_module.slcp
 * config_file override): the SDK leaves the pins to its graphical pin tool,
 * so they're written here instead.
 *
 * Two pin sets:
 *   0 (default) -- the card: USART0 on PA05 TX, PA06 RX, PA08 CTS, PA07
 *                  RTS. On the Explorer Kit these are its USB serial port
 *                  (VCOM), which the PC bench tests (tools/) use.
 *   1           -- the Explorer Kit wired to an RP2354B through its
 *                  mikroBUS socket: PB01 TX, PB02 RX, PC00 CTS, PC01 RTS
 *                  (README.md). Generate with
 *                  --configuration PC1500_HOST_MIKROBUS:1 */
#ifndef SL_IOSTREAM_USART_HOST_CONFIG_H
#define SL_IOSTREAM_USART_HOST_CONFIG_H

#include "em_gpio.h"

// <<< Use Configuration Wizard in Context Menu >>>

// <h>USART settings

// <q PC1500_HOST_MIKROBUS> The Explorer Kit's mikroBUS pins instead of the card's
// <i> Default: 0
#define PC1500_HOST_MIKROBUS 0

// <o SL_IOSTREAM_USART_HOST_BAUDRATE> Baud rate
// <i> Default: 115200
#define SL_IOSTREAM_USART_HOST_BAUDRATE 115200

#define SL_IOSTREAM_USART_HOST_PARITY usartNoParity
#define SL_IOSTREAM_USART_HOST_STOP_BITS usartStopbits1
#define SL_IOSTREAM_USART_HOST_FLOW_CONTROL_TYPE usartHwFlowControlCtsAndRts
#define SL_IOSTREAM_USART_HOST_RX_BUFFER_SIZE 512
#define SL_IOSTREAM_USART_HOST_CONVERT_BY_DEFAULT_LF_TO_CRLF 0
/* The UART receives only above EM2; app.c turns this restriction on for a
 * session (MODULE_WAKE high) and off outside one. */
#define SL_IOSTREAM_USART_HOST_RESTRICT_ENERGY_MODE_TO_ALLOW_RECEPTION 1

// </h>

// <<< end of configuration section >>>

#define SL_IOSTREAM_USART_HOST_PERIPHERAL USART0
#define SL_IOSTREAM_USART_HOST_PERIPHERAL_NO 0

#if PC1500_HOST_MIKROBUS
#define SL_IOSTREAM_USART_HOST_TX_PORT gpioPortB
#define SL_IOSTREAM_USART_HOST_TX_PIN 1
#define SL_IOSTREAM_USART_HOST_RX_PORT gpioPortB
#define SL_IOSTREAM_USART_HOST_RX_PIN 2
#define SL_IOSTREAM_USART_HOST_CTS_PORT gpioPortC
#define SL_IOSTREAM_USART_HOST_CTS_PIN 0
#define SL_IOSTREAM_USART_HOST_RTS_PORT gpioPortC
#define SL_IOSTREAM_USART_HOST_RTS_PIN 1
#else
#define SL_IOSTREAM_USART_HOST_TX_PORT gpioPortA
#define SL_IOSTREAM_USART_HOST_TX_PIN 5
#define SL_IOSTREAM_USART_HOST_RX_PORT gpioPortA
#define SL_IOSTREAM_USART_HOST_RX_PIN 6
#define SL_IOSTREAM_USART_HOST_CTS_PORT gpioPortA
#define SL_IOSTREAM_USART_HOST_CTS_PIN 8
#define SL_IOSTREAM_USART_HOST_RTS_PORT gpioPortA
#define SL_IOSTREAM_USART_HOST_RTS_PIN 7
#endif

#endif
