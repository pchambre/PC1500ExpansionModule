/* ssh_session.c -- see ssh_session.h. */
#include "ssh_session.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/sync.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "net_ping.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "ssh_client.h"
#include "ssh_keys.h"
#include "ssh_store.h"
#include "ssh_term.h"
#include "third_party/monocypher/monocypher.h"
#include "wifi_link.h"

#define DNS_TIMEOUT_MS 8000
#define CONNECT_TIMEOUT_MS 10000
#define STEP_MS 500
#define SEND_TIMEOUT_MS 10000
#define BLINK_MS 500
#define RING_LEN 16384 /* more than lwIP's TCP_WND: what's unread never overflows it */
#define CURSOR_CHAR '_'

/* ---- core0: the TCP connection ---- */

static struct tcp_pcb *g_pcb;
static volatile bool g_tcp_up, g_tcp_gone;
static volatile int8_t g_tcp_err; /* on_err's: why the connection went */
static uint8_t g_ring[RING_LEN];
static volatile uint32_t g_head, g_tail; /* core0 moves head, core1 tail */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg, (void)pcb, (void)err;
    if (!p) { /* the server closed it */
        g_tcp_gone = true;
        __sev();
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = q->payload;
        for (uint16_t i = 0; i < q->len; i++) {
            uint32_t next = (g_head + 1) % RING_LEN;
            if (next == g_tail) break; /* can't happen inside the TCP window */
            g_ring[g_head] = d[i];
            g_head = next;
        }
    }
    pbuf_free(p);
    __sev(); /* core1 may be waiting for it */
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    g_tcp_err = err;
    g_pcb = NULL; /* lwIP has freed it */
    g_tcp_gone = true;
    __sev();
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err == ERR_OK) {
        tcp_nagle_disable(pcb); /* a key at a time: no waiting to fill a segment */
        g_tcp_up = true;
    }
    return ERR_OK;
}

typedef struct {
    ip_addr_t addr;
    uint16_t port;
} connect_t;

static uint32_t do_connect(void *param) {
    const connect_t *c = param;
    g_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    err_t err;
    if (!g_pcb) return (uint32_t)-ERR_MEM;
    tcp_arg(g_pcb, NULL);
    tcp_recv(g_pcb, on_recv);
    tcp_err(g_pcb, on_err);
    if ((err = tcp_connect(g_pcb, &c->addr, c->port, on_connected)) != ERR_OK) {
        tcp_abort(g_pcb);
        g_pcb = NULL;
        return (uint32_t)-err;
    }
    return 0; /* lwIP's err_t, negated: 0 = under way */
}

static uint32_t do_close(void *param) {
    (void)param;
    if (!g_pcb) return 0;
    tcp_arg(g_pcb, NULL);
    tcp_recv(g_pcb, NULL);
    tcp_err(g_pcb, NULL);
    if (tcp_close(g_pcb) != ERR_OK) tcp_abort(g_pcb);
    g_pcb = NULL;
    return 0;
}

typedef struct {
    const uint8_t *data;
    size_t len;
} send_t;

/* 0 sent, 1 no room yet, 2 no connection */
static uint32_t do_send(void *param) {
    const send_t *s = param;
    err_t err;
    if (!g_pcb) return 2;
    if (tcp_sndbuf(g_pcb) < s->len) return 1;
    err = tcp_write(g_pcb, s->data, (u16_t)s->len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return 1;
    if (err != ERR_OK) return 2;
    tcp_output(g_pcb);
    return 0;
}

static uint32_t do_recved(void *param) {
    if (g_pcb) tcp_recved(g_pcb, (u16_t)(uintptr_t)param);
    return 0;
}

/* ---- core1 ---- */

static ssh_t g_ssh;
static ssh_term_t g_term;
static ssh_keys_t g_keys;
static bool g_session;     /* OPEN made a connection, not yet torn down */
static bool g_terminal;    /* the TERM action has the window */
static uint8_t g_error;    /* EXP_SSH_ERR_*, once CLOSED: our own reason, else from ssh_error() */
static uint32_t g_unacked; /* bytes taken from the ring, not yet given back to TCP's window */
static char g_host[EXP_SSH_HOST_MAX + 1];
static uint16_t g_port;
static uint8_t g_break_seen;
static uint32_t g_shown_version;
static bool g_blink;
static absolute_time_t g_next_blink;

static uint32_t on_core0(uint32_t (*fn)(void *), void *param) {
    return async_context_execute_sync(cyw43_arch_async_context(), fn, param);
}

static bool io_send(void *ctx, const uint8_t *data, size_t len) {
    send_t s = {data, len};
    absolute_time_t until = make_timeout_time_ms(SEND_TIMEOUT_MS);
    (void)ctx;
    for (;;) {
        uint32_t r = on_core0(do_send, &s);
        if (r == 0) return true;
        if (r == 2 || time_reached(until)) return false;
        sleep_ms(2);
    }
}

static void io_random(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    while (len) {
        uint64_t r = get_rand_64(); /* the RP2350's TRNG */
        size_t n = len < 8 ? len : 8;
        memcpy(out, &r, n);
        out += n;
        len -= n;
    }
}

static void io_data(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    ssh_term_output(&g_term, data, len);
}

/* What came in, to the protocol; and TCP's window opened again for it. */
static void pump(void) {
    while (g_tail != g_head && ssh_state(&g_ssh) != SSH_ST_CLOSED) {
        uint32_t head = g_head, tail = g_tail;
        uint32_t n = head > tail ? head - tail : RING_LEN - tail;
        size_t taken;
        __dmb();
        taken = ssh_feed(&g_ssh, g_ring + tail, n);
        g_tail = (tail + (uint32_t)taken) % RING_LEN;
        g_unacked += (uint32_t)taken;
        if (taken < n) break; /* paused, its buffer full */
    }
    if (g_unacked && (g_unacked >= 2048 || g_tail == g_head)) {
        on_core0(do_recved, (void *)(uintptr_t)g_unacked);
        g_unacked = 0;
    }
    if (g_tcp_gone && g_tail == g_head && ssh_state(&g_ssh) != SSH_ST_CLOSED) ssh_close(&g_ssh, true);
}

static uint8_t error_code(void) {
    switch (ssh_error(&g_ssh)) {
        case SSH_ERR_NONE: return EXP_SSH_ERR_NONE;
        case SSH_ERR_IO: return EXP_SSH_ERR_LOST;
        case SSH_ERR_HOSTKEY: return EXP_SSH_ERR_HOSTKEY;
        case SSH_ERR_REJECTED: return EXP_SSH_ERR_REJECTED;
        case SSH_ERR_AUTH: return EXP_SSH_ERR_AUTH;
        case SSH_ERR_CHANNEL: return EXP_SSH_ERR_CHANNEL;
        case SSH_ERR_DISCONNECTED: /* 14: no more auth methods -- a login that failed */
            return g_ssh.disconnect_reason == 14 ? EXP_SSH_ERR_AUTH : EXP_SSH_ERR_LOST;
        default: return EXP_SSH_ERR_PROTOCOL;
    }
}

/* The session's over: the connection closed, and why kept for STEP. */
static void teardown(void) {
    if (!g_session) return;
    if (ssh_state(&g_ssh) != SSH_ST_CLOSED) ssh_close(&g_ssh, false);
    if (g_error == EXP_SSH_ERR_NONE) g_error = error_code();
    on_core0(do_close, NULL);
    g_session = false;
    g_terminal = false;
    if (g_error != EXP_SSH_ERR_NONE) {
        char msg[MCU_LOG_MSG_MAX + 1];
        snprintf(msg, sizeof msg, "SSH closed: %u", g_error);
        mcu_log_warn(msg);
    }
}

static uint8_t failure(uint8_t *w, uint8_t code) {
    w[0] = code;
    return EXP_STATUS_ERROR;
}

static uint8_t open_session(uint8_t *w) {
    char user[EXP_SSH_USER_MAX + 1], pw[EXP_SSH_PW_MAX + 1];
    uint8_t secret[64], pub[32];
    const uint8_t *p = w;
    uint8_t n;
    connect_t c;
    absolute_time_t until;
    uint32_t err;
    char msg[MCU_LOG_MSG_MAX + 1];
    ssh_io_t io = {NULL, io_send, io_random, io_data};

    teardown();
    n = p[0] > EXP_SSH_USER_MAX ? EXP_SSH_USER_MAX : p[0];
    memcpy(user, p + 1, n);
    user[n] = 0;
    p += 1 + EXP_SSH_USER_MAX;
    n = p[0] > EXP_SSH_HOST_MAX ? EXP_SSH_HOST_MAX : p[0];
    memcpy(g_host, p + 1, n);
    g_host[n] = 0;
    p += 1 + EXP_SSH_HOST_MAX;
    g_port = (uint16_t)(p[0] << 8 | p[1]);
    p += 2;
    pw[0] = 0;
    if (p[0] != EXP_SSH_PW_NONE) {
        n = p[0] > EXP_SSH_PW_MAX ? EXP_SSH_PW_MAX : p[0];
        memcpy(pw, p + 1, n);
        pw[n] = 0;
        crypto_wipe((void *)(p + 1), n); /* not left in the window */
    }

    if (!wifi_link_connected()) return failure(w, EXP_SSH_ERR_NO_WIFI);
    if (!net_resolve(g_host, &c.addr, DNS_TIMEOUT_MS)) {
        mcu_log_warn("SSH host not found");
        return failure(w, EXP_SSH_ERR_NOT_FOUND);
    }
    c.port = g_port;
    g_head = g_tail = 0;
    g_unacked = 0;
    g_tcp_up = g_tcp_gone = false;
    g_tcp_err = ERR_OK;
    if ((err = on_core0(do_connect, &c)) != 0) {
        snprintf(msg, sizeof msg, "SSH tcp_connect %d", -(int)err);
        mcu_log_warn(msg);
        return failure(w, EXP_SSH_ERR_REFUSED);
    }
    until = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    while (!g_tcp_up && !g_tcp_gone && !time_reached(until)) sleep_ms(10);
    if (!g_tcp_up) {
        on_core0(do_close, NULL);
        /* lwIP's err_t: -13 ERR_ABRT, -14 ERR_RST (refused), -4 ERR_RTE (no route) */
        if (g_tcp_gone) snprintf(msg, sizeof msg, "SSH no connection %d", (int)g_tcp_err);
        else snprintf(msg, sizeof msg, "SSH connect timed out");
        mcu_log_warn(msg);
        return failure(w, EXP_SSH_ERR_REFUSED);
    }
    ssh_term_init(&g_term);
    ssh_keys_init(&g_keys);
    g_session = true;
    g_error = EXP_SSH_ERR_NONE;
    if (!ssh_store_device_key(secret, pub)) mcu_log_warn("SSH device key not saved");
    ssh_start(&g_ssh, &io, user, secret, pw[0] ? pw : NULL);
    crypto_wipe(secret, sizeof secret);
    crypto_wipe(pw, sizeof pw);
    mcu_log_info("SSH connected");
    return EXP_STATUS_SUCCESS;
}

/* The server's host key, against the known hosts: a known one goes on by
 * itself, a changed one ends it. True if the user has to be asked. */
static bool check_hostkey(void) {
    const uint8_t *key = ssh_hostkey_blob(&g_ssh) + 19; /* string "ssh-ed25519", string key */
    switch (ssh_store_check_host(g_host, g_port, key)) {
        case SSH_HOST_KNOWN: ssh_hostkey_answer(&g_ssh, true); return false;
        case SSH_HOST_CHANGED:
            mcu_log_warn("SSH host key changed");
            g_error = EXP_SSH_ERR_HOSTKEY;
            ssh_hostkey_answer(&g_ssh, false);
            return false;
        default: return true;
    }
}

static uint8_t step(uint8_t *w) {
    absolute_time_t until = make_timeout_time_ms(STEP_MS);
    if (!g_session) {
        w[0] = EXP_SSH_ST_CLOSED;
        w[1] = g_error;
        return EXP_STATUS_SUCCESS;
    }
    for (;;) {
        pump();
        switch (ssh_state(&g_ssh)) {
            case SSH_ST_HOSTKEY:
                if (!check_hostkey()) continue;
                w[0] = EXP_SSH_ST_HOSTKEY_NEW;
                w[1] = SSH_FINGERPRINT_LEN;
                ssh_fingerprint(ssh_hostkey_blob(&g_ssh), (char *)w + 2);
                return EXP_STATUS_SUCCESS;
            case SSH_ST_PASSWORD: w[0] = EXP_SSH_ST_PASSWORD; return EXP_STATUS_SUCCESS;
            case SSH_ST_OPEN: w[0] = EXP_SSH_ST_OPEN; return EXP_STATUS_SUCCESS;
            case SSH_ST_CLOSED:
                teardown();
                w[0] = EXP_SSH_ST_CLOSED;
                w[1] = g_error;
                return EXP_STATUS_SUCCESS;
            default: break;
        }
        if (time_reached(until)) {
            w[0] = EXP_SSH_ST_CONNECTING;
            return EXP_STATUS_SUCCESS;
        }
        best_effort_wfe_or_timeout(make_timeout_time_ms(5)); /* on_recv's SEV, or a moment */
    }
}

/* The public key's line in SSHKEY.PUB, and its fingerprint for the user. */
static uint8_t write_key(uint8_t *w) {
    uint8_t secret[64], pub[32], blob[SSH_HOSTKEY_BLOB_LEN];
    char line[81 + 8 + MCU_CONFIG_HOSTNAME_MAX + 2], fp[SSH_FINGERPRINT_LEN + 1];
    size_t n;
    FIL f;
    UINT written = 0;
    bool ok;
    if (!ssh_store_device_key(secret, pub)) return failure(w, EXP_SSH_ERR_CARD);
    crypto_wipe(secret, sizeof secret);
    n = ssh_public_key_line(pub, line);
    n += (size_t)snprintf(line + n, sizeof line - n, " pc1500@%s\n", mcu_config_get_hostname());
    ok = f_open(&f, "SSHKEY.PUB", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if (ok) {
        ok = f_write(&f, line, (UINT)n, &written) == FR_OK && written == n;
        ok = f_close(&f) == FR_OK && ok;
    }
    if (!ok) {
        mcu_log_warn("SSH SSHKEY.PUB not written");
        return failure(w, EXP_SSH_ERR_CARD);
    }
    memcpy(blob, "\0\0\0\x0bssh-ed25519\0\0\0\x20", 19);
    memcpy(blob + 19, pub, 32);
    ssh_fingerprint(blob, fp);
    w[0] = SSH_FINGERPRINT_LEN;
    memcpy(w + 1, fp, SSH_FINGERPRINT_LEN);
    return EXP_STATUS_SUCCESS;
}

static void indicators(uint8_t *w);

static void start_terminal(uint8_t *w) {
    g_terminal = true;
    g_break_seen = w[EXP_SSH_TERM_BREAK_COUNT];
    w[EXP_SSH_TERM_KEY] = 0;
    w[EXP_SSH_TERM_CLOSED] = 0;
    indicators(w);
    g_shown_version = g_term.version - 1; /* shown at the first poll */
    g_next_blink = make_timeout_time_ms(BLINK_MS);
    g_blink = true;
}

uint8_t ssh_session_command(uint8_t command, uint8_t *w) {
    switch (command) {
        case EXP_COMMAND_SSH_OPEN: return open_session(w);
        case EXP_COMMAND_SSH_STEP: return step(w);
        case EXP_COMMAND_SSH_ANSWER:
            if (!g_session || ssh_state(&g_ssh) != SSH_ST_HOSTKEY) return EXP_STATUS_ERROR;
            if (w[0]) {
                ssh_store_add_host(g_host, g_port, ssh_hostkey_blob(&g_ssh) + 19);
                if (!ssh_store_commit()) mcu_log_warn("SSH known host not saved");
            }
            ssh_hostkey_answer(&g_ssh, w[0] != 0);
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_SSH_PASSWORD: {
            char pw[EXP_SSH_PW_MAX + 1];
            uint8_t n = w[0] > EXP_SSH_PW_MAX ? EXP_SSH_PW_MAX : w[0];
            if (!g_session || ssh_state(&g_ssh) != SSH_ST_PASSWORD) return EXP_STATUS_ERROR;
            memcpy(pw, w + 1, n);
            pw[n] = 0;
            crypto_wipe(w + 1, n);
            ssh_password(&g_ssh, pw);
            crypto_wipe(pw, sizeof pw);
            return EXP_STATUS_SUCCESS;
        }
        case EXP_COMMAND_SSH_TERM:
            if (!g_session || ssh_state(&g_ssh) != SSH_ST_OPEN) return EXP_STATUS_ERROR;
            start_terminal(w);
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_SSH_CLOSE: teardown(); return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_SSH_KEY: return write_key(w);
        case EXP_COMMAND_SSH_FORGET: {
            char host[EXP_SSH_HOST_MAX + 1];
            uint8_t n = w[0] > EXP_SSH_HOST_MAX ? EXP_SSH_HOST_MAX : w[0];
            memcpy(host, w + 1, n);
            host[n] = 0;
            w[0] = (uint8_t)ssh_store_forget(n ? host : NULL);
            return ssh_store_commit() ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
        }
        default: return EXP_STATUS_NOT_IMPLEMENTED;
    }
}

bool ssh_session_terminal(void) { return g_terminal; }

/* The line to show, into the window, if it changed: the cursor blinks. */
static void publish(uint8_t *w) {
    char line[TERM_WIDTH];
    uint8_t cursor;
    if (time_reached(g_next_blink)) {
        g_blink = !g_blink;
        g_next_blink = make_timeout_time_ms(BLINK_MS);
        g_shown_version = g_term.version - 1;
    }
    if (g_term.version == g_shown_version) return;
    g_shown_version = g_term.version;
    cursor = ssh_term_render(&g_term, line);
    if (cursor != TERM_NO_CURSOR && g_blink) line[cursor] = CURSOR_CHAR;
    memcpy(w + EXP_SSH_TERM_LINE, line, TERM_WIDTH);
    __dmb(); /* the line before its count: the ROM reads the count first */
    w[EXP_SSH_TERM_LINE_COUNT]++;
}

/* SHIFT, DEF and SML as the LCD's indicators show them. */
static void indicators(uint8_t *w) {
    w[EXP_SSH_TERM_INDICATORS] = (uint8_t)((ssh_keys_def(&g_keys) || ssh_keys_scrolling(&g_keys) ? EXP_SSH_IND_DEF : 0) |
                                           (ssh_keys_caps(&g_keys) ? 0 : EXP_SSH_IND_SMALL) |
                                           (ssh_keys_shift(&g_keys) ? EXP_SSH_IND_SHIFT : 0));
}

void ssh_session_poll(uint8_t *w) {
    uint8_t out[SSH_KEYS_OUT_MAX], brk;
    ssh_view_t view;
    size_t n;
    if (!g_terminal) return;
    pump();
    n = ssh_keys_sample(&g_keys, w[EXP_SSH_TERM_KEY], to_ms_since_boot(get_absolute_time()), out, &view);
    if (n) {
        ssh_term_live(&g_term);
        ssh_write(&g_ssh, out, n);
    }
    switch (view) {
        case SSH_VIEW_UP: ssh_term_scroll(&g_term, 1); break;
        case SSH_VIEW_DOWN: ssh_term_scroll(&g_term, -1); break;
        case SSH_VIEW_LEFT: ssh_term_pan(&g_term, -1); break;
        case SSH_VIEW_RIGHT: ssh_term_pan(&g_term, 1); break;
        case SSH_VIEW_QUIT: ssh_close(&g_ssh, false); break;
        case SSH_VIEW_LIVE: ssh_term_live(&g_term); break;
        default: break;
    }
    brk = w[EXP_SSH_TERM_BREAK_COUNT];
    if (brk != g_break_seen) { /* ON: Ctrl-C */
        static const uint8_t ctrl_c = 0x03;
        g_break_seen = brk;
        ssh_term_live(&g_term);
        ssh_write(&g_ssh, &ctrl_c, 1);
    }
    publish(w);
    indicators(w);
    if (ssh_state(&g_ssh) == SSH_ST_CLOSED) {
        teardown();
        w[EXP_SSH_TERM_CLOSED] = 1; /* the TERM action ends */
    }
}
