/* link_store.c -- see link_store.h.
 *
 * Flash format (one sector): a 16-byte header ["PLK1"][id 8][4 unused],
 * then LINK_PAIRS_MAX records of 64 bytes: [used 0xA5][name len][name 16]
 * [id 8][ltk 32][6 unused]. Erased flash (0xFF) is "no identity yet". */
#include "link_store.h"

#include <ctype.h>
#include <string.h>

#include "mcu_store.h"
#include "pc_exp.h"
#include "pico/rand.h"

#define MAGIC "PLK1"
#define HEADER 16
#define RECORD 64
#define USED 0xA5

static uint8_t g_id[LS_ID_LEN];
static link_pair_t g_pairs[LINK_PAIRS_MAX];
static volatile bool g_dirty;

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

void link_store_init(void) {
    static uint8_t image[HEADER + LINK_PAIRS_MAX * RECORD];
    memset(g_pairs, 0, sizeof g_pairs);
    if (!mcu_store_read(EXP_STORE_SLOT_LINK, 0, image, sizeof image) || memcmp(image, MAGIC, 4) != 0) {
        uint64_t r = get_rand_64(); /* a new identity: the RP2350's TRNG */
        memcpy(g_id, &r, LS_ID_LEN);
        g_dirty = true;
        link_store_commit();
        return;
    }
    memcpy(g_id, image + 4, LS_ID_LEN);
    for (int i = 0; i < LINK_PAIRS_MAX; i++) {
        const uint8_t *r = image + HEADER + i * RECORD;
        uint8_t n = r[1] > LINK_NAME_MAX ? LINK_NAME_MAX : r[1];
        if (r[0] != USED) continue;
        g_pairs[i].used = true;
        memcpy(g_pairs[i].name, r + 2, n);
        g_pairs[i].name[n] = 0;
        memcpy(g_pairs[i].id, r + 2 + LINK_NAME_MAX, LS_ID_LEN);
        memcpy(g_pairs[i].ltk, r + 2 + LINK_NAME_MAX + LS_ID_LEN, LS_KEY_LEN);
    }
    ls_wipe(image, sizeof image);
}

const uint8_t *link_store_id(void) { return g_id; }

const link_pair_t *link_store_find(const uint8_t id[LS_ID_LEN]) {
    for (int i = 0; i < LINK_PAIRS_MAX; i++)
        if (g_pairs[i].used && memcmp(g_pairs[i].id, id, LS_ID_LEN) == 0) return &g_pairs[i];
    return NULL;
}

void link_store_add(const uint8_t id[LS_ID_LEN], const char *name, const uint8_t ltk[LS_KEY_LEN]) {
    link_pair_t *p = (link_pair_t *)link_store_find(id);
    for (int i = 0; !p && i < LINK_PAIRS_MAX; i++)
        if (!g_pairs[i].used) p = &g_pairs[i];
    if (!p) p = &g_pairs[0]; /* full: the oldest slot makes way */
    p->used = true;
    memcpy(p->id, id, LS_ID_LEN);
    strncpy(p->name, name, LINK_NAME_MAX);
    p->name[LINK_NAME_MAX] = 0;
    memcpy(p->ltk, ltk, LS_KEY_LEN);
    g_dirty = true;
}

int link_store_forget(const char *name) {
    int n = 0;
    for (int i = 0; i < LINK_PAIRS_MAX; i++) {
        if (!g_pairs[i].used || (name && !same_name(g_pairs[i].name, name))) continue;
        ls_wipe(&g_pairs[i], sizeof g_pairs[i]);
        n++;
    }
    if (n) g_dirty = true;
    return n;
}

bool link_store_commit(void) {
    static uint8_t image[HEADER + LINK_PAIRS_MAX * RECORD];
    bool ok;
    if (!g_dirty) return true;
    g_dirty = false;
    memset(image, 0xFF, sizeof image);
    memcpy(image, MAGIC, 4);
    memcpy(image + 4, g_id, LS_ID_LEN);
    for (int i = 0; i < LINK_PAIRS_MAX; i++) {
        uint8_t *r = image + HEADER + i * RECORD;
        if (!g_pairs[i].used) continue;
        r[0] = USED;
        r[1] = (uint8_t)strlen(g_pairs[i].name);
        memset(r + 2, 0, LINK_NAME_MAX);
        memcpy(r + 2, g_pairs[i].name, r[1]);
        memcpy(r + 2 + LINK_NAME_MAX, g_pairs[i].id, LS_ID_LEN);
        memcpy(r + 2 + LINK_NAME_MAX + LS_ID_LEN, g_pairs[i].ltk, LS_KEY_LEN);
    }
    ok = mcu_store_erase(EXP_STORE_SLOT_LINK) && mcu_store_write(EXP_STORE_SLOT_LINK, 0, image, sizeof image);
    ls_wipe(image, sizeof image);
    if (!ok) g_dirty = true;
    return ok;
}
