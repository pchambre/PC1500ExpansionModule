/* hostlink.h -- the UART side of MODULE_PROTOCOL.md: frames (sec.5),
 * requests in, answers out, a repeated seq answered from the cache. */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    ST_OK = 0,
    ST_BAD_FRAME = 1,
    ST_UNKNOWN_TYPE = 2,
    ST_BAD_ARGS = 3,
    ST_BUSY = 4,
    ST_NOT_CONNECTED = 5,
    ST_STATE = 6,
    ST_FAILED = 7,
};

enum {
    RQ_HELLO = 0x01,
    RQ_POLL = 0x02,
    RQ_SET_NAME = 0x03,
    RQ_SET_TX_POWER = 0x04,
    RQ_ADV_START = 0x10,
    RQ_ADV_STOP = 0x11,
    RQ_SCAN_START = 0x12,
    RQ_SCAN_STOP = 0x13,
    RQ_CONNECT = 0x14,
    RQ_CONNECT_CANCEL = 0x15,
    RQ_DISCONNECT = 0x16,
    RQ_LINK_SEND = 0x17,
    RQ_KBD_PAIR = 0x20,
    RQ_KBD_STOP = 0x21,
    RQ_KBD_FORGET = 0x22,
    RQ_KBD_ENABLE = 0x23,
    RQ_KBD_STATUS = 0x24,
};

/* The application's request handler (app.c): fills `answer` after the
 * status byte, which it returns; *answer_len is what it wrote after it. */
uint8_t hostlink_handle(uint8_t type, const uint8_t *payload, size_t len,
                        uint8_t *answer, size_t *answer_len);

void hostlink_init(void);
/* Read what has arrived, answer any complete request. Main loop. */
void hostlink_poll(void);
