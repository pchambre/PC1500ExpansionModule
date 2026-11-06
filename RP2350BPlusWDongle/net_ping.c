/* net_ping.c -- see net_ping.h. */
#include "net_ping.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "lwip/dns.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"
#include "mcu_log.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "wifi_link.h"

#define DNS_TIMEOUT_MS 8000
#define ROUND_MS 1000   /* a reply waited for this long, and rounds this far apart */
#define PAYLOAD_LEN 32
#define PING_ID 0x1500

static uint32_t on_core0(uint32_t (*fn)(void *), void *param) {
    return async_context_execute_sync(cyw43_arch_async_context(), fn, param);
}

/* ---- the name lookup ---- */

static volatile int8_t g_dns; /* 0 waiting, 1 found, -1 not */
static ip_addr_t g_dns_addr;

static void on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name, (void)arg;
    if (addr) g_dns_addr = *addr;
    g_dns = addr ? 1 : -1;
}

static uint32_t do_dns(void *param) {
    err_t err = dns_gethostbyname(param, &g_dns_addr, on_dns, NULL);
    if (err == ERR_OK) g_dns = 1;
    else if (err != ERR_INPROGRESS) g_dns = -1;
    return 0;
}

bool net_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    absolute_time_t until;
    if (ipaddr_aton(host, out)) return true;
    g_dns = 0;
    on_core0(do_dns, (void *)host);
    until = make_timeout_time_ms(timeout_ms);
    while (g_dns == 0 && !time_reached(until)) sleep_ms(10);
    if (g_dns != 1) return false;
    *out = g_dns_addr;
    return true;
}

/* ---- core0: the echo ---- */

static struct raw_pcb *g_raw;
static ip_addr_t g_target;
static volatile uint16_t g_seq;   /* the round's: its reply's seqno */
static volatile bool g_got;
static volatile uint8_t g_ttl;
static volatile uint32_t g_reply_us;

/* An echo reply to our request, from the target: noted, and eaten. */
static u8_t on_icmp(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    const struct ip_hdr *iph = p->payload;
    const struct icmp_echo_hdr *e;
    u16_t hl;
    (void)arg, (void)pcb;
    if (p->len < sizeof(struct ip_hdr)) return 0;
    hl = IPH_HL_BYTES(iph);
    if (p->len < hl + sizeof(struct icmp_echo_hdr)) return 0;
    e = (const struct icmp_echo_hdr *)((const u8_t *)p->payload + hl);
    if (ICMPH_TYPE(e) != ICMP_ER || e->id != lwip_htons(PING_ID) || e->seqno != lwip_htons(g_seq) ||
        !ip_addr_cmp(addr, &g_target))
        return 0;
    g_ttl = IPH_TTL(iph);
    g_reply_us = time_us_32();
    g_got = true;
    pbuf_free(p);
    __sev();
    return 1;
}

static uint32_t do_open(void *param) {
    (void)param;
    if (g_raw) raw_remove(g_raw);
    g_raw = raw_new(IP_PROTO_ICMP);
    if (!g_raw) return 1;
    raw_recv(g_raw, on_icmp, NULL);
    raw_bind(g_raw, IP_ADDR_ANY);
    return 0;
}

static uint32_t do_send(void *param) {
    struct pbuf *p;
    struct icmp_echo_hdr *e;
    uint16_t len = sizeof(struct icmp_echo_hdr) + PAYLOAD_LEN;
    err_t err;
    (void)param;
    if (!g_raw) return 1;
    p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (!p) return 1;
    e = p->payload;
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->id = lwip_htons(PING_ID);
    e->seqno = lwip_htons(g_seq);
    for (uint16_t i = 0; i < PAYLOAD_LEN; i++) ((uint8_t *)(e + 1))[i] = (uint8_t)('A' + i % 26);
    e->chksum = 0;
    e->chksum = inet_chksum(e, len);
    err = raw_sendto(g_raw, p, &g_target);
    pbuf_free(p);
    return err == ERR_OK ? 0 : 1;
}

/* ---- core1 ---- */

static uint8_t start(uint8_t *w) {
    char host[EXP_SSH_HOST_MAX + 1], ip[16];
    uint8_t n = w[0] > EXP_SSH_HOST_MAX ? EXP_SSH_HOST_MAX : w[0];
    memcpy(host, w + 1, n);
    host[n] = 0;
    if (!wifi_link_connected()) {
        w[0] = EXP_SSH_ERR_NO_WIFI;
        return EXP_STATUS_ERROR;
    }
    if (!net_resolve(host, &g_target, DNS_TIMEOUT_MS)) {
        mcu_log_warn("PING host not found");
        w[0] = EXP_SSH_ERR_NOT_FOUND;
        return EXP_STATUS_ERROR;
    }
    if (on_core0(do_open, NULL) != 0) {
        w[0] = EXP_SSH_ERR_REFUSED;
        return EXP_STATUS_ERROR;
    }
    ipaddr_ntoa_r(&g_target, ip, sizeof ip);
    w[0] = (uint8_t)strlen(ip);
    memcpy(w + 1, ip, w[0]);
    return EXP_STATUS_SUCCESS;
}

/* One round: the request, its reply (or none in ROUND_MS), and the rest of
 * the round waited out, so rounds are a second apart. */
static uint8_t round_trip(uint8_t *w) {
    absolute_time_t until = make_timeout_time_ms(ROUND_MS);
    uint32_t sent_us, ms;
    g_seq = w[0];
    g_got = false;
    sent_us = time_us_32();
    if (on_core0(do_send, NULL) != 0) {
        mcu_log_warn("PING send failed");
        return EXP_STATUS_ERROR;
    }
    while (!g_got && !time_reached(until)) best_effort_wfe_or_timeout(make_timeout_time_ms(5));
    if (!g_got) {
        w[0] = 0;
        return EXP_STATUS_SUCCESS;
    }
    ms = (g_reply_us - sent_us + 500) / 1000;
    if (ms > 0xFFFF) ms = 0xFFFF;
    w[0] = 1;
    w[1] = (uint8_t)(ms >> 8);
    w[2] = (uint8_t)ms;
    w[3] = g_ttl;
    while (!time_reached(until)) sleep_ms(10);
    return EXP_STATUS_SUCCESS;
}

uint8_t net_ping_command(uint8_t command, uint8_t *w) {
    switch (command) {
        case EXP_COMMAND_PING_START: return start(w);
        case EXP_COMMAND_PING_ROUND: return round_trip(w);
        default: return EXP_STATUS_NOT_IMPLEMENTED;
    }
}
