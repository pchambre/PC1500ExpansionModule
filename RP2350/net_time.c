/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* net_time.c -- see net_time.h. */
#include "net_time.h"

#include <string.h>

#include "hardware/sync.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "mcu_log.h"
#include "net_ping.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "wifi_link.h"

#define NTP_SERVER "pool.ntp.org"
#define NTP_PORT 123
#define NTP_PACKET_LEN 48
#define NTP_TO_UNIX 2208988800u /* 1900-01-01 to 1970-01-01, in seconds */
#define DNS_TIMEOUT_MS 4000
#define ANSWER_MS 1500
#define TRIES 2

static uint32_t on_core0(uint32_t (*fn)(void *), void *param) {
    return async_context_execute_sync(cyw43_arch_async_context(), fn, param);
}

/* ---- core0 ---- */

static struct udp_pcb *g_pcb;
static ip_addr_t g_server;
static uint8_t g_sent_tx[8];       /* our transmit timestamp, which the answer echoes */
static volatile bool g_got;
static uint8_t g_answer[NTP_PACKET_LEN];
static volatile uint32_t g_answer_us;

static void on_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg, (void)pcb;
    if (!g_got && port == NTP_PORT && ip_addr_cmp(addr, &g_server) && p->tot_len >= NTP_PACKET_LEN &&
        pbuf_copy_partial(p, g_answer, NTP_PACKET_LEN, 0) == NTP_PACKET_LEN) {
        g_answer_us = time_us_32();
        g_got = true;
    }
    pbuf_free(p);
}

static uint32_t do_open(void *param) {
    (void)param;
    if (!g_pcb) {
        g_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (!g_pcb) return 1;
        udp_recv(g_pcb, on_udp, NULL);
    }
    return 0;
}

static uint32_t do_close(void *param) {
    (void)param;
    if (g_pcb) udp_remove(g_pcb);
    g_pcb = NULL;
    return 0;
}

static uint32_t do_send(void *param) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_PACKET_LEN, PBUF_RAM);
    uint8_t *q;
    err_t err;
    (void)param;
    if (!p) return 1;
    q = p->payload;
    memset(q, 0, NTP_PACKET_LEN);
    q[0] = 0x23; /* LI 0, version 4, mode 3 (client) */
    memcpy(q + 40, g_sent_tx, sizeof g_sent_tx);
    g_got = false;
    err = udp_sendto(g_pcb, p, &g_server, NTP_PORT);
    pbuf_free(p);
    return err == ERR_OK ? 0 : 1;
}

/* ---- core1 ---- */

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* One query: UTC at the answer's arrival, in Unix ms. */
static bool query(int64_t *utc_ms) {
    uint32_t sent_us = time_us_32(), stamp = sent_us ^ 0x50433135u; /* any value we'd know again */
    memset(g_sent_tx, 0, sizeof g_sent_tx);
    memcpy(g_sent_tx + 4, &stamp, 4);
    if (on_core0(do_send, NULL) != 0) return false;
    absolute_time_t until = make_timeout_time_ms(ANSWER_MS);
    while (!g_got && !time_reached(until)) sleep_ms(5);
    if (!g_got) return false;
    __dmb();
    uint8_t mode = g_answer[0] & 7, stratum = g_answer[1];
    if (mode != 4 || stratum == 0 || stratum > 15 || (g_answer[0] >> 6) == 3 || /* not synchronized */
        memcmp(g_answer + 24, g_sent_tx, sizeof g_sent_tx) != 0)                 /* not our query's answer */
        return false;
    /* the server's transmit time, plus half the round trip */
    uint32_t seconds = be32(g_answer + 40), fraction = be32(g_answer + 44);
    uint32_t rtt_us = g_answer_us - sent_us;
    *utc_ms = (int64_t)(seconds - NTP_TO_UNIX) * 1000 + (int64_t)(((uint64_t)fraction * 1000) >> 32) +
              rtt_us / 2000;
    return true;
}

bool net_time_get(int64_t *utc_ms) {
    bool ok = false;
    if (!wifi_link_connected()) return false;
    if (!net_resolve(NTP_SERVER, &g_server, DNS_TIMEOUT_MS)) {
        mcu_log_warn("SNTP: " NTP_SERVER " not found");
        return false;
    }
    if (on_core0(do_open, NULL) != 0) return false;
    for (int i = 0; i < TRIES && !ok; i++) ok = query(utc_ms);
    on_core0(do_close, NULL);
    if (!ok) mcu_log_warn("SNTP: no answer");
    return ok;
}
