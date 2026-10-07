/* wifi_link.c -- see wifi_link.h.
 *
 * core0 (the CYW43's async context): the driver and lwIP themselves, and
 * the scan callback filling g_found. core1 (the commands): everything else,
 * calling into core0 with on_core0() and polling with short sleeps, as
 * ble_link.c does. The MCU log is written from core1 only, and never with
 * a password in it. */
#include "wifi_link.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "pc_exp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "sha256.h"
#include "wifi_store.h"

#define POWER_TIMEOUT_MS 8000
#define SCAN_TIMEOUT_MS 10000
#define JOIN_TIMEOUT_MS 20000 /* association, then DHCP */
/* Every wait in one command ends by then: monitor.c gives up on a command
 * at 30s (COMMAND_TIMEOUT_US) and answers ERROR itself. */
#define COMMAND_BUDGET_MS 25000
#define WPA3_RETRY_MIN_MS 4000 /* a WPA3 retry only with this much left */
#define MAX_FOUND 20

/* cyw43_ev_scan_result_t's auth_mode bits (cyw43_ll.c) */
#define SCAN_WEP 0x01
#define SCAN_WPA 0x02
#define SCAN_WPA2 0x04 /* any RSN: WPA2 or WPA3 */

typedef struct {
    char ssid[EXP_WIFI_SSID_MAX + 1];
    int16_t rssi;
    uint8_t auth; /* SCAN_* bits, 0 = open */
} found_t;

static found_t g_found[MAX_FOUND]; /* the last scan, strongest first once sorted */
static uint8_t g_nfound;
static volatile bool g_wanted; /* core1 wants the radio */
static volatile bool g_radio;  /* core0: the CYW43 is up */
static volatile bool g_sta;    /* station mode is on */
static bool g_store_ready;
static char g_hostname[MCU_CONFIG_HOSTNAME_MAX + 1]; /* lwIP keeps the pointer */
static char g_ssid[EXP_WIFI_SSID_MAX + 1];           /* the network joined */
static absolute_time_t g_deadline;                   /* this command's, COMMAND_BUDGET_MS */

/* ---- core0 ---- */

bool wifi_link_wanted(void) { return g_wanted; }

void wifi_link_poll(bool radio_up) {
    if (!radio_up) g_sta = false; /* cyw43_arch_deinit() took station mode down with it */
    g_radio = radio_up;
}

static uint32_t do_sta_on(void *param) {
    (void)param;
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], g_hostname); /* for DHCP */
    g_sta = true;
    return 0;
}

static uint32_t do_sta_off(void *param) {
    (void)param;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_disable_sta_mode();
    g_sta = false;
    return 0;
}

/* Several access points can share an SSID: one entry, the strongest. When
 * the list is full, a stronger network takes the weakest one's place. */
static int on_scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    found_t *f = NULL;
    uint8_t n = r->ssid_len > EXP_WIFI_SSID_MAX ? EXP_WIFI_SSID_MAX : r->ssid_len;
    if (n == 0) return 0; /* a hidden network: WFCON "name" reaches it */
    for (uint8_t i = 0; i < g_nfound && !f; i++)
        if (strlen(g_found[i].ssid) == n && memcmp(g_found[i].ssid, r->ssid, n) == 0) {
            if (r->rssi <= g_found[i].rssi) return 0;
            f = &g_found[i];
        }
    if (!f && g_nfound < MAX_FOUND) f = &g_found[g_nfound++];
    for (uint8_t i = 0; i < g_nfound && !f; i++)
        if (g_found[i].rssi < r->rssi && (!f || g_found[i].rssi < f->rssi)) f = &g_found[i];
    if (!f) return 0;
    memcpy(f->ssid, r->ssid, n);
    f->ssid[n] = 0;
    f->rssi = r->rssi;
    f->auth = r->auth_mode;
    return 0;
}

static uint32_t do_scan_start(void *param) {
    cyw43_wifi_scan_options_t opts;
    (void)param;
    memset(&opts, 0, sizeof opts);
    g_nfound = 0;
    return cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan_result) == 0;
}

static uint32_t do_scan_active(void *param) {
    (void)param;
    return cyw43_wifi_scan_active(&cyw43_state);
}

typedef struct {
    const char *ssid, *pw;
    uint32_t auth;
} join_t;

static uint32_t do_join(void *param) {
    const join_t *j = param;
    return cyw43_arch_wifi_connect_async(j->ssid, j->pw[0] ? j->pw : NULL, j->auth) == 0;
}

/* The driver's own record of the join: how it ended (bits 0-3) and which
 * steps got done -- 802.11 authentication 0x200, association 0x400, the
 * WPA key exchange 0x800 (cyw43_ctrl.c WIFI_JOIN_STATE_*). */
static uint32_t do_join_state(void *param) {
    (void)param;
    return cyw43_state.wifi_join_state;
}

static uint32_t do_link_status(void *param) {
    (void)param;
    return (uint32_t)cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
}

static uint32_t do_ip_text(void *param) {
    ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), param, 16);
    return 0;
}

/* ---- core1 ---- */

static uint32_t on_core0(uint32_t (*fn)(void *), void *param) {
    return async_context_execute_sync(cyw43_arch_async_context(), fn, param);
}

static int link_status(void) { return (int)on_core0(do_link_status, NULL); }

/* `ms` from now, or the command's deadline if that's sooner. */
static absolute_time_t until_ms(uint32_t ms) {
    absolute_time_t t = make_timeout_time_ms(ms);
    return absolute_time_diff_us(t, g_deadline) < 0 ? g_deadline : t;
}

/* The radio up and station mode on, or false (logged) after a while. */
static bool power_up(void) {
    absolute_time_t until = until_ms(POWER_TIMEOUT_MS);
    g_wanted = true;
    while (!g_radio) {
        if (time_reached(until)) {
            mcu_log_warn("WIFI radio failed");
            return false;
        }
        sleep_ms(5);
    }
    if (!g_sta) {
        strncpy(g_hostname, mcu_config_get_hostname(), MCU_CONFIG_HOSTNAME_MAX);
        on_core0(do_sta_on, NULL);
    }
    return true;
}

/* Station mode off, and the radio let go (DORMANT sleep allowed again,
 * once BLE doesn't want it either). */
static void power_down(void) {
    if (g_sta) on_core0(do_sta_off, NULL);
    g_ssid[0] = 0;
    g_wanted = false;
}

static uint8_t failure(uint8_t *w, uint8_t code) {
    w[0] = code;
    return EXP_STATUS_ERROR;
}

/* Scans until the CYW43 says it's done. g_found sorted strongest first. */
static bool scan(void) {
    absolute_time_t until = until_ms(SCAN_TIMEOUT_MS);
    if (!on_core0(do_scan_start, NULL)) {
        mcu_log_warn("WIFI scan failed");
        return false;
    }
    do {
        sleep_ms(50);
        if (time_reached(until)) {
            mcu_log_warn("WIFI scan timed out");
            return false;
        }
    } while (on_core0(do_scan_active, NULL));
    for (uint8_t i = 1; i < g_nfound; i++) /* insertion sort, by signal */
        for (uint8_t j = i; j > 0 && g_found[j].rssi > g_found[j - 1].rssi; j--) {
            found_t t = g_found[j];
            g_found[j] = g_found[j - 1];
            g_found[j - 1] = t;
        }
    return true;
}

static const char *auth_text(uint8_t auth) {
    if (auth & SCAN_WPA2) return "WPA2";
    if (auth & SCAN_WPA) return "WPA";
    if (auth & SCAN_WEP) return "WEP";
    return "OPEN";
}

/* The last scan as a LIST_SD_DIR listing, for BROWSE: the SSID, then
 * "-52 WPA2" where the size would go, '*' after a remembered network. */
static void scan_listing(uint8_t *w) {
    char summary[EXP_DIR_SUMMARY_LEN + 1];
    uint8_t *r;
    for (uint8_t i = 0; i < g_nfound; i++) {
        char text[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + 1];
        size_t n = strlen(g_found[i].ssid);
        r = w + 2 + (uint16_t)i * EXP_DIR_RECORD_SIZE;
        memset(r, 0, EXP_DIR_RECORD_SIZE);
        memset(text, ' ', sizeof text);
        memcpy(text, g_found[i].ssid, n > EXP_DIR_NAME_LEN ? EXP_DIR_NAME_LEN : n);
        snprintf(text + EXP_DIR_NAME_LEN, EXP_DIR_SIZE_TEXT_LEN + 1, "%d %s%s", g_found[i].rssi,
                 auth_text(g_found[i].auth), wifi_store_find(g_found[i].ssid) ? "*" : "");
        memcpy(r, text, EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN);
        for (uint8_t k = 0; k < EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN; k++)
            if (r[k] == 0) r[k] = ' '; /* snprintf's terminator */
    }
    w[0] = 0;
    w[1] = g_nfound;
    r = w + 2 + (uint16_t)g_nfound * EXP_DIR_RECORD_SIZE;
    memset(r, ' ', EXP_DIR_SUMMARY_LEN);
    memcpy(r, summary, (size_t)snprintf(summary, sizeof summary, "%u FOUND", g_nfound));
}

/* Joins, and waits for an IP address. A "no network" answer is retried,
 * as the SDK's own cyw43_arch_wifi_connect_timeout_ms() does: it can come
 * before the access point has answered at all. */
/* "WIFI try 2 pw12 h3F": the security asked for (1 WPA, 2 WPA2, 3 WPA3/2,
 * 0 open), and the password handed to the driver -- its length and the
 * first byte of its SHA-256, so a wrong or missing one shows without the
 * password itself being logged. */
static void log_try(const char *pw, uint32_t auth) {
    char msg[MCU_LOG_MSG_MAX + 1];
    uint8_t h[SHA256_LEN];
    int kind = auth == CYW43_AUTH_WPA_TKIP_PSK ? 1 : auth == CYW43_AUTH_WPA2_MIXED_PSK ? 2 : auth == CYW43_AUTH_WPA3_WPA2_AES_PSK ? 3 : 0;
    sha256(pw, strlen(pw), h);
    snprintf(msg, sizeof msg, "WIFI try %d pw%u h%02X", kind, (unsigned)strlen(pw), h[0]);
    mcu_log_warn(msg);
}

static int join_once(const char *ssid, const char *pw, uint32_t auth) {
    join_t j = {ssid, pw, auth};
    absolute_time_t start = get_absolute_time(), until = until_ms(JOIN_TIMEOUT_MS);
    int status = CYW43_LINK_JOIN;
    log_try(pw, auth);
    if (!on_core0(do_join, &j)) return CYW43_LINK_FAIL;
    while (status != CYW43_LINK_UP) {
        sleep_ms(100);
        status = link_status();
        if (status == CYW43_LINK_BADAUTH || status == CYW43_LINK_FAIL || time_reached(until)) {
            /* "WIFI st 604 1234ms": the driver's join state, and how long it took */
            char msg[MCU_LOG_MSG_MAX + 1];
            snprintf(msg, sizeof msg, "WIFI st %03lX %lums", (unsigned long)on_core0(do_join_state, NULL),
                     (unsigned long)(absolute_time_diff_us(start, get_absolute_time()) / 1000));
            mcu_log_warn(msg);
            if (status == CYW43_LINK_BADAUTH || status == CYW43_LINK_FAIL) return status;
            return status == CYW43_LINK_NONET ? status : CYW43_LINK_FAIL;
        }
        if (status == CYW43_LINK_NONET && !on_core0(do_join, &j)) return CYW43_LINK_FAIL;
    }
    return status;
}

/* Joins `ssid` (scanned security `auth`, or -1 if the scan didn't see it:
 * a hidden network). Out: [len][IP text]; remembered on success. */
static uint8_t join(uint8_t *w, const char *ssid, const char *given_pw, int auth) {
    int status;
    char ip[16], pw[EXP_WIFI_PW_MAX + 1];
    strncpy(pw, given_pw, EXP_WIFI_PW_MAX); /* may be the store's own copy, which wifi_store_add() moves */
    pw[EXP_WIFI_PW_MAX] = 0;
    if (link_status() != CYW43_LINK_DOWN) on_core0(do_sta_off, NULL); /* leave the old network first */
    if (!power_up()) return failure(w, EXP_WIFI_ERR_FAILED);
    if (!pw[0]) {
        status = join_once(ssid, "", CYW43_AUTH_OPEN);
    } else {
        status = join_once(ssid, pw, auth == SCAN_WPA ? CYW43_AUTH_WPA_TKIP_PSK : CYW43_AUTH_WPA2_MIXED_PSK);
        /* a WPA3-only network may refuse the WPA2 handshake */
        if ((status == CYW43_LINK_FAIL || status == CYW43_LINK_BADAUTH) && (auth < 0 || (auth & SCAN_WPA2)) &&
            absolute_time_diff_us(get_absolute_time(), g_deadline) > WPA3_RETRY_MIN_MS * 1000)
            status = join_once(ssid, pw, CYW43_AUTH_WPA3_WPA2_AES_PSK);
    }
    if (status != CYW43_LINK_UP) {
        power_down();
        if (status == CYW43_LINK_BADAUTH) {
            mcu_log_warn("WIFI wrong password");
            return failure(w, EXP_WIFI_ERR_BAD_PASSWORD);
        }
        mcu_log_warn(status == CYW43_LINK_NONET ? "WIFI network not found" : "WIFI connect failed");
        return failure(w, status == CYW43_LINK_NONET ? EXP_WIFI_ERR_NOT_FOUND : EXP_WIFI_ERR_FAILED);
    }
    strncpy(g_ssid, ssid, EXP_WIFI_SSID_MAX);
    wifi_store_add(ssid, pw);
    if (!wifi_store_commit()) mcu_log_warn("WIFI network not saved");
    on_core0(do_ip_text, ip);
    mcu_log_info("WIFI connected");
    w[0] = (uint8_t)strlen(ip);
    memcpy(w + 1, ip, w[0]);
    return EXP_STATUS_SUCCESS;
}

/* WEP only: the CYW43 driver can't join it. */
static bool wep(int auth) { return auth > 0 && (auth & SCAN_WEP) && !(auth & (SCAN_WPA | SCAN_WPA2)); }

/* The password to use: the one given (pw len != EXP_WIFI_PW_NONE), else
 * the remembered one, else none for an open (or hidden) network. NULL if
 * a secured network has none. */
static const char *password(const uint8_t *given, const char *ssid, int auth, char *buf) {
    const wifi_net_t *known = wifi_store_find(ssid);
    if (given[0] != EXP_WIFI_PW_NONE) {
        uint8_t n = given[0] > EXP_WIFI_PW_MAX ? EXP_WIFI_PW_MAX : given[0];
        memcpy(buf, given + 1, n);
        buf[n] = 0;
        return buf;
    }
    if (known) return known->pw;
    return auth > 0 ? NULL : "";
}

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

/* CONNECT_NAME's network in the last scan: exactly, else in any case (the
 * PC-1500 types capitals unless SML is on). -1 if it isn't there. */
static int find_found(const char *ssid) {
    for (int i = 0; i < g_nfound; i++)
        if (strcmp(g_found[i].ssid, ssid) == 0) return i;
    for (int i = 0; i < g_nfound; i++)
        if (same_name(g_found[i].ssid, ssid)) return i;
    return -1;
}

static uint8_t connect_name(uint8_t *w) {
    char ssid[EXP_WIFI_SSID_MAX + 1], pw[EXP_WIFI_PW_MAX + 1];
    const char *use;
    uint8_t n = w[0] > EXP_WIFI_SSID_MAX ? EXP_WIFI_SSID_MAX : w[0];
    int i, auth = -1;
    if (n == 0 && g_ssid[0] && link_status() == CYW43_LINK_UP) { /* already on one */
        on_core0(do_ip_text, pw);
        w[0] = (uint8_t)strlen(pw);
        memcpy(w + 1, pw, w[0]);
        return EXP_STATUS_SUCCESS;
    }
    if (!power_up() || !scan()) {
        power_down();
        return failure(w, EXP_WIFI_ERR_FAILED);
    }
    if (n == 0) { /* the strongest remembered network in range */
        for (i = 0; i < g_nfound && !wifi_store_find(g_found[i].ssid); i++) {}
        if (i == g_nfound) {
            power_down();
            return failure(w, EXP_WIFI_ERR_NONE_KNOWN);
        }
        return join(w, g_found[i].ssid, wifi_store_find(g_found[i].ssid)->pw, g_found[i].auth);
    }
    memcpy(ssid, w + 1, n);
    ssid[n] = 0;
    i = find_found(ssid);
    if (i >= 0) {
        strcpy(ssid, g_found[i].ssid);
        auth = g_found[i].auth;
    }
    if (wep(auth)) return failure(w, EXP_WIFI_ERR_WEP);
    use = password(w + 1 + EXP_WIFI_SSID_MAX, ssid, auth, pw);
    if (!use) return failure(w, EXP_WIFI_ERR_NEED_PASSWORD); /* the keyword asks, then sends it again */
    return join(w, ssid, use, auth);
}

static uint8_t connect_index(uint8_t *w) {
    char pw[EXP_WIFI_PW_MAX + 1];
    const char *use;
    if (w[0] >= g_nfound) return failure(w, EXP_WIFI_ERR_NOT_FOUND);
    const found_t *f = &g_found[w[0]];
    if (wep(f->auth)) return failure(w, EXP_WIFI_ERR_WEP);
    use = password(w + 1, f->ssid, f->auth, pw);
    if (!use) return failure(w, EXP_WIFI_ERR_NEED_PASSWORD);
    return join(w, f->ssid, use, f->auth);
}

static uint8_t status(uint8_t *w) {
    char ip[16] = "";
    uint8_t state = EXP_WIFI_STATE_OFF, n = (uint8_t)strlen(g_ssid);
    if (g_sta && g_radio && g_ssid[0]) {
        state = link_status() == CYW43_LINK_UP ? EXP_WIFI_STATE_CONNECTED : EXP_WIFI_STATE_CONNECTING;
        if (state == EXP_WIFI_STATE_CONNECTED) on_core0(do_ip_text, ip);
    }
    w[0] = state;
    w[1] = n;
    memcpy(w + 2, g_ssid, n);
    w[2 + n] = (uint8_t)strlen(ip);
    memcpy(w + 3 + n, ip, w[2 + n]);
    return EXP_STATUS_SUCCESS;
}

bool wifi_link_connected(void) { return g_sta && g_radio && g_ssid[0] && link_status() == CYW43_LINK_UP; }

uint8_t wifi_link_command(uint8_t command, uint8_t *w) {
    uint8_t result;
    g_deadline = make_timeout_time_ms(COMMAND_BUDGET_MS);
    if (!g_store_ready) {
        wifi_store_init();
        g_store_ready = true;
    }
    switch (command) {
        case EXP_COMMAND_WIFI_SCAN:
            if (!power_up() || !scan()) {
                if (!g_ssid[0]) power_down();
                return EXP_STATUS_ERROR;
            }
            scan_listing(w);
            if (!g_ssid[0]) power_down(); /* station mode only stays on for a network */
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_WIFI_CONNECT:
            if (!power_up()) return failure(w, EXP_WIFI_ERR_FAILED);
            result = connect_index(w);
            if (result != EXP_STATUS_SUCCESS && !g_ssid[0]) power_down();
            return result;
        case EXP_COMMAND_WIFI_CONNECT_NAME:
            result = connect_name(w);
            if (result != EXP_STATUS_SUCCESS && !g_ssid[0]) power_down();
            return result;
        case EXP_COMMAND_WIFI_DISCONNECT:
            power_down();
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_WIFI_STATUS:
            return status(w);
        case EXP_COMMAND_WIFI_FORGET: {
            char ssid[EXP_WIFI_SSID_MAX + 1];
            uint8_t n = w[0] > EXP_WIFI_SSID_MAX ? EXP_WIFI_SSID_MAX : w[0];
            memcpy(ssid, w + 1, n);
            ssid[n] = 0;
            w[0] = (uint8_t)wifi_store_forget(n ? ssid : NULL);
            return wifi_store_commit() ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
        }
        default:
            return EXP_STATUS_NOT_IMPLEMENTED;
    }
}
