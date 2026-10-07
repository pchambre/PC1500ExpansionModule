/* ssh_store.c -- see ssh_store.h.
 *
 * Flash format (one sector): a 16-byte header ["PSH1"][12 unused], the
 * device key's 32-byte seed (all 0xFF: none yet), 16 unused, then
 * SSH_STORE_HOSTS records of 80 bytes, the most recent first: [used 0xA5]
 * [host len][host 40][port hi][port lo][key 32][4 unused]. */
#include "ssh_store.h"

#include <ctype.h>
#include <string.h>

#include "mcu_store.h"
#include "pc_exp.h"
#include "pico/rand.h"
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"

#define MAGIC "PSH1"
#define HEADER 16
#define SEED_AT HEADER
#define HOSTS_AT (HEADER + 48)
#define RECORD 80
#define USED 0xA5
#define IMAGE_LEN (HOSTS_AT + SSH_STORE_HOSTS * RECORD)

typedef struct {
    bool used;
    char host[SSH_STORE_HOST_MAX + 1];
    uint16_t port;
    uint8_t key[32];
} host_t;

static bool g_loaded, g_dirty, g_have_seed;
static uint8_t g_seed[32];
static host_t g_hosts[SSH_STORE_HOSTS];

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static void load(void) {
    static uint8_t image[IMAGE_LEN];
    if (g_loaded) return;
    g_loaded = true;
    memset(g_hosts, 0, sizeof g_hosts);
    if (!mcu_store_read(EXP_STORE_SLOT_SSH, 0, image, sizeof image) || memcmp(image, MAGIC, 4) != 0) return;
    for (int i = 0; i < 32 && !g_have_seed; i++) g_have_seed = image[SEED_AT + i] != 0xFF;
    if (g_have_seed) memcpy(g_seed, image + SEED_AT, 32);
    for (int i = 0; i < SSH_STORE_HOSTS; i++) {
        const uint8_t *r = image + HOSTS_AT + i * RECORD;
        uint8_t n = r[1] > SSH_STORE_HOST_MAX ? SSH_STORE_HOST_MAX : r[1];
        if (r[0] != USED) continue;
        g_hosts[i].used = true;
        memcpy(g_hosts[i].host, r + 2, n);
        g_hosts[i].host[n] = 0;
        g_hosts[i].port = (uint16_t)(r[2 + SSH_STORE_HOST_MAX] << 8 | r[3 + SSH_STORE_HOST_MAX]);
        memcpy(g_hosts[i].key, r + 4 + SSH_STORE_HOST_MAX, 32);
    }
    crypto_wipe(image, sizeof image);
}

bool ssh_store_device_key(uint8_t secret[64], uint8_t public_key[32]) {
    uint8_t seed[32];
    load();
    if (!g_have_seed) {
        for (int i = 0; i < 32; i += 8) {
            uint64_t r = get_rand_64(); /* the RP2350's TRNG */
            memcpy(g_seed + i, &r, 8);
        }
        g_have_seed = g_dirty = true;
        if (!ssh_store_commit()) return false;
    }
    memcpy(seed, g_seed, sizeof seed);
    crypto_ed25519_key_pair(secret, public_key, seed); /* wipes `seed` */
    return true;
}

static host_t *find(const char *host, uint16_t port) {
    for (int i = 0; i < SSH_STORE_HOSTS; i++)
        if (g_hosts[i].used && g_hosts[i].port == port && same_name(g_hosts[i].host, host)) return &g_hosts[i];
    return NULL;
}

ssh_host_check_t ssh_store_check_host(const char *host, uint16_t port, const uint8_t key[32]) {
    const host_t *h;
    load();
    h = find(host, port);
    if (!h) return SSH_HOST_UNKNOWN;
    return memcmp(h->key, key, 32) == 0 ? SSH_HOST_KNOWN : SSH_HOST_CHANGED;
}

void ssh_store_add_host(const char *host, uint16_t port, const uint8_t key[32]) {
    const host_t *had;
    int at = SSH_STORE_HOSTS - 1; /* full: the oldest makes way */
    load();
    had = find(host, port);
    if (had) {
        at = (int)(had - g_hosts);
    } else {
        for (int i = 0; i < SSH_STORE_HOSTS; i++)
            if (!g_hosts[i].used) {
                at = i;
                break;
            }
    }
    memmove(&g_hosts[1], &g_hosts[0], (size_t)at * sizeof g_hosts[0]);
    memset(&g_hosts[0], 0, sizeof g_hosts[0]);
    g_hosts[0].used = true;
    strncpy(g_hosts[0].host, host, SSH_STORE_HOST_MAX);
    g_hosts[0].port = port;
    memcpy(g_hosts[0].key, key, 32);
    g_dirty = true;
}

int ssh_store_forget(const char *host) {
    int n = 0;
    load();
    for (int i = 0; i < SSH_STORE_HOSTS; i++) {
        if (!g_hosts[i].used || (host && !same_name(g_hosts[i].host, host))) continue;
        memset(&g_hosts[i], 0, sizeof g_hosts[i]);
        n++;
    }
    if (n) g_dirty = true;
    return n;
}

bool ssh_store_commit(void) {
    static uint8_t image[IMAGE_LEN];
    bool ok;
    int out = 0;
    if (!g_dirty) return true;
    g_dirty = false;
    memset(image, 0xFF, sizeof image);
    memcpy(image, MAGIC, 4);
    if (g_have_seed) memcpy(image + SEED_AT, g_seed, 32);
    for (int i = 0; i < SSH_STORE_HOSTS; i++) {
        uint8_t *r = image + HOSTS_AT + out * RECORD;
        if (!g_hosts[i].used) continue; /* forgotten ones close up */
        r[0] = USED;
        r[1] = (uint8_t)strlen(g_hosts[i].host);
        memset(r + 2, 0, SSH_STORE_HOST_MAX);
        memcpy(r + 2, g_hosts[i].host, r[1]);
        r[2 + SSH_STORE_HOST_MAX] = (uint8_t)(g_hosts[i].port >> 8);
        r[3 + SSH_STORE_HOST_MAX] = (uint8_t)g_hosts[i].port;
        memcpy(r + 4 + SSH_STORE_HOST_MAX, g_hosts[i].key, 32);
        out++;
    }
    ok = mcu_store_erase(EXP_STORE_SLOT_SSH) && mcu_store_write(EXP_STORE_SLOT_SSH, 0, image, sizeof image);
    crypto_wipe(image, sizeof image);
    if (!ok) g_dirty = true;
    return ok;
}
