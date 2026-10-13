/* events.c -- see events.h. Called from the main loop only (the Bluetooth
 * stack's events arrive there too), so no locking. */
#include "events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "module_config.h"

#define SLOT_DATA (LINK_FRAME_MAX + 16)

static struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[SLOT_DATA];
} q[EVENT_QUEUE_SLOTS];
static unsigned head, count;
static bool overflowed; /* a dropped event, reported once the queue drains a little */

static void host_wake(bool on) {
    if (on) {
        GPIO_PinOutSet(HOST_WAKE_PORT, HOST_WAKE_PIN);
        GPIO_PinOutSet(HOST_WAKE2_PORT, HOST_WAKE2_PIN);
    } else {
        GPIO_PinOutClear(HOST_WAKE_PORT, HOST_WAKE_PIN);
        GPIO_PinOutClear(HOST_WAKE2_PORT, HOST_WAKE2_PIN);
    }
}

void events_init(void) {
    head = count = 0;
    overflowed = false;
    GPIO_PinModeSet(HOST_WAKE_PORT, HOST_WAKE_PIN, gpioModePushPull, 0);
    GPIO_PinModeSet(HOST_WAKE2_PORT, HOST_WAKE2_PIN, gpioModePushPull, 0);
}

static bool put(uint8_t type, const uint8_t *data, size_t len) {
    if (count == EVENT_QUEUE_SLOTS || len > SLOT_DATA) {
        return false;
    }
    unsigned i = (head + count) % EVENT_QUEUE_SLOTS;
    q[i].type = type;
    q[i].len = (uint16_t)len;
    if (len) {
        memcpy(q[i].data, data, len);
    }
    count++;
    host_wake(true);
    return true;
}

bool events_push(uint8_t type, const uint8_t *data, size_t len) {
    /* the last slot is kept for the overflow LOG */
    if (count >= EVENT_QUEUE_SLOTS - 1) {
        overflowed = true;
        return false;
    }
    return put(type, data, len);
}

static void log_v(uint8_t level, const char *fmt, va_list ap) {
    uint8_t buf[2 + 100];
    int n = vsnprintf((char *)buf + 2, sizeof buf - 2, fmt, ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof buf - 2) {
        n = sizeof buf - 2;
    }
    buf[0] = level;
    buf[1] = (uint8_t)n;
    events_push(EV_LOG, buf, (size_t)n + 2);
}

void events_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(1, fmt, ap);
    va_end(ap);
}

void events_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(2, fmt, ap);
    va_end(ap);
}

size_t events_pop(uint8_t *out, size_t max) {
    if (overflowed && count < EVENT_QUEUE_SLOTS) {
        static const char msg[] = "MODULE EVENT QUEUE FULL";
        uint8_t buf[2 + sizeof msg];
        buf[0] = 2; /* level: error */
        buf[1] = sizeof msg - 1;
        memcpy(buf + 2, msg, sizeof msg - 1);
        overflowed = false;
        put(EV_LOG, buf, sizeof msg + 1);
    }
    if (count == 0) {
        host_wake(false);
        out[0] = EV_NONE;
        return 1;
    }
    size_t len = q[head].len;
    if (len + 1 > max) {
        len = max - 1;
    }
    out[0] = q[head].type;
    memcpy(out + 1, q[head].data, len);
    head = (head + 1) % EVENT_QUEUE_SLOTS;
    count--;
    if (count == 0) {
        host_wake(false); /* before the answer goes out (sec.4) */
    }
    return len + 1;
}
