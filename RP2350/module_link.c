/* module_link.c -- see module_link.h and MODULE_PROTOCOL.md. */
#include "module_link.h"

#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "mcu_log.h"
#include "pico/stdlib.h"

#define MODULE_UART uart0 /* GPIO32-35: UART0 TX/RX/CTS/RTS */
#define MODULE_BAUD 115200
#define SOF 0xA5
#define MAX_PAYLOAD 300
#define ANSWER_TIMEOUT_US 50000u /* sec.5 */
#define WAKE_SETTLE_US 2000u     /* after raising MODULE_WAKE: see MODULE_PROTOCOL.md sec.9 */

static uint8_t g_seq;
static bool g_ready;

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF; /* CRC-16/CCITT-FALSE */
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* Nothing to do: taking the interrupt is what wakes core1's WFE. */
static void host_wake_irq(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
}

void module_link_init(void) {
    uart_init(MODULE_UART, MODULE_BAUD);
    gpio_set_function(PIN_BLE_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_BLE_UART_RX, GPIO_FUNC_UART);
    gpio_set_function(PIN_BLE_UART_CTS, GPIO_FUNC_UART);
    gpio_set_function(PIN_BLE_UART_RTS, GPIO_FUNC_UART);
    uart_set_hw_flow(MODULE_UART, true, true);
    uart_set_format(MODULE_UART, 8, 1, UART_PARITY_NONE);

    gpio_init(PIN_BLE_MODULE_WAKE); /* no session: low */
    gpio_set_dir(PIN_BLE_MODULE_WAKE, GPIO_OUT);
    gpio_put(PIN_BLE_MODULE_WAKE, 0);
    gpio_init(PIN_BLE_HOST_WAKE);   /* the module drives it */
    gpio_set_dir(PIN_BLE_HOST_WAKE, GPIO_IN);
    gpio_pull_down(PIN_BLE_HOST_WAKE);
    /* Its rising edge interrupts core1 (this is core1), which ends the WFE
     * core1 waits in between commands (monitor.c Core1Take()) -- no timed
     * polling. Asleep, it's a DORMANT wake source instead (monitor.c). */
    gpio_set_irq_enabled_with_callback(PIN_BLE_HOST_WAKE, GPIO_IRQ_EDGE_RISE, true, host_wake_irq);
    g_ready = true;
}

static void session(bool on) {
    if (on && !gpio_get_out_level(PIN_BLE_MODULE_WAKE)) {
        gpio_put(PIN_BLE_MODULE_WAKE, 1);
        sleep_us(WAKE_SETTLE_US);
    } else if (!on) {
        gpio_put(PIN_BLE_MODULE_WAKE, 0);
    }
}

/* One answer frame with this type and seq, or -1. */
static int read_answer(uint8_t type, uint8_t seq, uint8_t *ans, size_t max) {
    uint8_t f[5 + MAX_PAYLOAD + 2];
    size_t n = 0, want = 5;
    absolute_time_t end = make_timeout_time_us(ANSWER_TIMEOUT_US);
    while (!time_reached(end)) {
        if (!uart_is_readable(MODULE_UART)) continue;
        uint8_t b = (uint8_t)uart_getc(MODULE_UART);
        if (n == 0 && b != SOF) continue; /* resync */
        f[n++] = b;
        if (n == 5) {
            size_t len = f[3] | (size_t)f[4] << 8;
            if (len > MAX_PAYLOAD) {
                n = 0;
                continue;
            }
            want = 5 + len + 2;
        }
        if (n >= 5 && n == want) {
            size_t len = want - 7;
            uint16_t c = f[5 + len] | (uint16_t)(f[6 + len] << 8);
            if (c != crc16(f + 1, 4 + len) || f[1] != (type | 0x80) || f[2] != seq) return -1;
            if (len > max) len = max;
            memcpy(ans, f + 5, len);
            return (int)len;
        }
    }
    return -1;
}

int module_link_request(uint8_t type, const uint8_t *payload, size_t len, uint8_t *ans, size_t max) {
    if (!g_ready || len > MAX_PAYLOAD) return -1;
    uint8_t f[5 + MAX_PAYLOAD + 2];
    f[0] = SOF;
    f[1] = type;
    f[2] = g_seq;
    f[3] = (uint8_t)len;
    f[4] = (uint8_t)(len >> 8);
    if (len) memcpy(f + 5, payload, len);
    uint16_t c = crc16(f + 1, 4 + len);
    f[5 + len] = (uint8_t)c;
    f[6 + len] = (uint8_t)(c >> 8);
    session(true);
    for (int tries = 0; tries < 3; tries++) {
        while (uart_is_readable(MODULE_UART)) (void)uart_getc(MODULE_UART); /* stale bytes */
        /* Byte by byte with a deadline, not uart_write_blocking(): with
         * hardware flow control a module that never raises RTS (unpowered,
         * unwired, asleep) would hold core1 there for good. */
        absolute_time_t end = make_timeout_time_us(ANSWER_TIMEOUT_US);
        size_t sent = 0;
        while (sent < 7 + len && !time_reached(end)) {
            if (uart_is_writable(MODULE_UART)) uart_putc_raw(MODULE_UART, (char)f[sent++]);
        }
        if (sent < 7 + len) {
            printf("[module] CTS never let the request out (sent %u of %u)\n", (unsigned)sent, (unsigned)(7 + len));
            continue;
        }
        int got = read_answer(type, g_seq, ans, max);
        if (got >= 1) {
            g_seq++;
            return got;
        }
    }
    g_seq++;
    return -1;
}

bool module_link_host_wake(void) { return g_ready && gpio_get(PIN_BLE_HOST_WAKE); }

void module_link_session_end(void) {
    if (g_ready) session(false);
}

static void probe(void) {
    uint8_t a[MAX_PAYLOAD];
    printf("[module] HOST_WAKE=%d\n", gpio_get(PIN_BLE_HOST_WAKE));
    uint8_t v = 0;
    int n = module_link_request(0x01, &v, 1, a, sizeof a); /* HELLO */
    if (n < 4) {
        printf("[module] HELLO: no answer\n");
        mcu_log_error("MODULE NO ANSWER");
        session(false);
        return;
    }
    uint8_t vl = a[2];
    printf("[module] HELLO: status %u, protocol %u, firmware %.*s, address %02X:%02X:%02X:%02X:%02X:%02X\n",
           a[0], a[1], vl, (const char *)a + 3, a[3 + vl], a[4 + vl], a[5 + vl], a[6 + vl], a[7 + vl], a[8 + vl]);
    char msg[24];
    snprintf(msg, sizeof msg, "MODULE OK %.*s", vl > 6 ? 6 : vl, (const char *)a + 3);
    mcu_log_warn(msg);
    for (int i = 0; i < 16; i++) { /* POLL until empty */
        n = module_link_request(0x02, NULL, 0, a, sizeof a);
        if (n < 2) {
            printf("[module] POLL: no answer\n");
            break;
        }
        if (a[1] == 0) {
            printf("[module] POLL: no more events, HOST_WAKE=%d\n", gpio_get(PIN_BLE_HOST_WAKE));
            break;
        }
        printf("[module] event 0x%02X:", a[1]);
        for (int k = 2; k < n; k++) printf(" %02X", a[k]);
        printf("\n");
    }
    session(false);
}

void module_link_console_key(int c) {
    if (c == 'm' || c == 'M') probe();
}
