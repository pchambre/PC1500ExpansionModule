/* wifi_store.c -- see wifi_store.h.
 *
 * Flash format (one sector): a 16-byte header ["PWF1"][12 unused], then
 * EXP_WIFI_REMEMBERED records of 128 bytes, the most recent first:
 * [used 0xA5][ssid len][ssid 32][pw len][pw 63][30 unused]. Erased flash
 * (0xFF) is "none remembered". */
#include "wifi_store.h"

#include <ctype.h>
#include <string.h>

#include "mcu_store.h"

#define MAGIC "PWF1"
#define HEADER 16
#define RECORD 128
#define USED 0xA5

static wifi_net_t g_nets[EXP_WIFI_REMEMBERED];
static bool g_dirty;

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

static void wipe(void *p, uint32_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

void wifi_store_init(void) {
    static uint8_t image[HEADER + EXP_WIFI_REMEMBERED * RECORD];
    memset(g_nets, 0, sizeof g_nets);
    if (!mcu_store_read(EXP_STORE_SLOT_WIFI, 0, image, sizeof image) || memcmp(image, MAGIC, 4) != 0) return;
    for (int i = 0; i < EXP_WIFI_REMEMBERED; i++) {
        const uint8_t *r = image + HEADER + i * RECORD;
        uint8_t n = r[1] > EXP_WIFI_SSID_MAX ? EXP_WIFI_SSID_MAX : r[1];
        uint8_t p = r[2 + EXP_WIFI_SSID_MAX];
        if (r[0] != USED) continue;
        if (p > EXP_WIFI_PW_MAX) p = EXP_WIFI_PW_MAX;
        g_nets[i].used = true;
        memcpy(g_nets[i].ssid, r + 2, n);
        g_nets[i].ssid[n] = 0;
        memcpy(g_nets[i].pw, r + 3 + EXP_WIFI_SSID_MAX, p);
        g_nets[i].pw[p] = 0;
    }
    wipe(image, sizeof image);
}

const wifi_net_t *wifi_store_find(const char *ssid) {
    for (int i = 0; i < EXP_WIFI_REMEMBERED; i++)
        if (g_nets[i].used && strcmp(g_nets[i].ssid, ssid) == 0) return &g_nets[i];
    return NULL;
}

void wifi_store_add(const char *ssid, const char *pw) {
    const wifi_net_t *had = wifi_store_find(ssid);
    int at = EXP_WIFI_REMEMBERED - 1; /* full: the oldest makes way */
    if (had) {
        at = (int)(had - g_nets);
        if (at == 0 && strcmp(had->pw, pw) == 0) return; /* nothing new */
    } else {
        for (int i = 0; i < EXP_WIFI_REMEMBERED; i++)
            if (!g_nets[i].used) {
                at = i;
                break;
            }
    }
    memmove(&g_nets[1], &g_nets[0], (size_t)at * sizeof g_nets[0]);
    memset(&g_nets[0], 0, sizeof g_nets[0]);
    g_nets[0].used = true;
    strncpy(g_nets[0].ssid, ssid, EXP_WIFI_SSID_MAX);
    strncpy(g_nets[0].pw, pw, EXP_WIFI_PW_MAX);
    g_dirty = true;
}

int wifi_store_forget(const char *ssid) {
    int n = 0;
    for (int i = 0; i < EXP_WIFI_REMEMBERED; i++) {
        if (!g_nets[i].used || (ssid && !same_name(g_nets[i].ssid, ssid))) continue;
        wipe(&g_nets[i], sizeof g_nets[i]);
        n++;
    }
    if (n) g_dirty = true;
    return n;
}

bool wifi_store_commit(void) {
    static uint8_t image[HEADER + EXP_WIFI_REMEMBERED * RECORD];
    bool ok;
    int out = 0;
    if (!g_dirty) return true;
    g_dirty = false;
    memset(image, 0xFF, sizeof image);
    memcpy(image, MAGIC, 4);
    for (int i = 0; i < EXP_WIFI_REMEMBERED; i++) {
        uint8_t *r = image + HEADER + out * RECORD;
        if (!g_nets[i].used) continue; /* forgotten ones close up */
        r[0] = USED;
        r[1] = (uint8_t)strlen(g_nets[i].ssid);
        memset(r + 2, 0, EXP_WIFI_SSID_MAX);
        memcpy(r + 2, g_nets[i].ssid, r[1]);
        r[2 + EXP_WIFI_SSID_MAX] = (uint8_t)strlen(g_nets[i].pw);
        memset(r + 3 + EXP_WIFI_SSID_MAX, 0, EXP_WIFI_PW_MAX);
        memcpy(r + 3 + EXP_WIFI_SSID_MAX, g_nets[i].pw, r[2 + EXP_WIFI_SSID_MAX]);
        out++;
    }
    ok = mcu_store_erase(EXP_STORE_SLOT_WIFI) && mcu_store_write(EXP_STORE_SLOT_WIFI, 0, image, sizeof image);
    wipe(image, sizeof image);
    if (!ok) g_dirty = true;
    return ok;
}
