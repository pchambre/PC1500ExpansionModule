/* fl_store.c -- see fl_store.h. The wire format is pc_exp.h's
 * EXP_COMMAND_FL_*.
 *
 * A sector: ["FL1"][0][seq, 4 LE][used, 2 LE][0][0][crc, 4 LE] (FL_HEADER
 * bytes), then `used` bytes of records. crc is CRC-32 of the records. The
 * sector with the highest seq whose CRC checks is the store; neither:
 * empty. */
#include "fl_store.h"

#include <string.h>

#include "pc_exp.h"

static uint8_t g_img[FL_SECTOR];
static bool g_loaded;
static int g_at;     /* the sector the store is in, or -1: none written yet */
static uint32_t g_seq;
static uint16_t g_used;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Sector s, if it holds a store: its seq and used. */
static bool sector_valid(const fl_flash_t *f, int s, uint32_t *seq, uint16_t *used) {
    uint8_t h[FL_HEADER];
    if (!f->read(f->ctx, (uint32_t)s * FL_SECTOR, h, FL_HEADER)) return false;
    if (memcmp(h, "FL1", 3) != 0) return false;
    uint16_t u = (uint16_t)(h[8] | h[9] << 8);
    if (u > FL_CAPACITY) return false;
    if (!f->read(f->ctx, (uint32_t)s * FL_SECTOR + FL_HEADER, g_img + FL_HEADER, u)) return false;
    if (crc32(g_img + FL_HEADER, u) != get32(h + 12)) return false;
    *seq = get32(h + 4);
    *used = u;
    return true;
}

static void load(const fl_flash_t *f) {
    uint32_t seq[2];
    uint16_t used[2];
    bool ok[2];
    for (int s = 0; s < 2; s++) ok[s] = sector_valid(f, s, &seq[s], &used[s]);
    g_at = -1;
    g_seq = 0;
    g_used = 0;
    int best = ok[0] && (!ok[1] || seq[0] >= seq[1]) ? 0 : ok[1] ? 1 : -1;
    if (best >= 0) {
        sector_valid(f, best, &seq[best], &used[best]); /* its records into g_img */
        g_at = best;
        g_seq = seq[best];
        g_used = used[best];
    }
    g_loaded = true;
}

/* The store to the other sector. */
static bool save(const fl_flash_t *f) {
    int to = g_at == 0 ? 1 : 0;
    uint8_t *h = g_img;
    memset(h, 0, FL_HEADER);
    memcpy(h, "FL1", 3);
    put32(h + 4, g_seq + 1);
    h[8] = (uint8_t)g_used;
    h[9] = (uint8_t)(g_used >> 8);
    put32(h + 12, crc32(g_img + FL_HEADER, g_used));
    memset(g_img + FL_HEADER + g_used, 0xFF, FL_CAPACITY - g_used);
    if (!f->erase(f->ctx, (uint32_t)to * FL_SECTOR)) return false;
    if (!f->write(f->ctx, (uint32_t)to * FL_SECTOR, g_img, FL_SECTOR)) return false;
    g_at = to;
    g_seq++;
    return true;
}

/* A value chunk's length, or 0 if it isn't one. */
static uint8_t chunk_len(const uint8_t *c) {
    if (c[0] == 'N') return 9;
    if (c[0] == 'S' && c[1] <= FL_STRING_MAX) return (uint8_t)(2 + c[1]);
    return 0;
}

static bool key_ok(const uint8_t *k, uint8_t len) {
    if (len == 0 || len > FL_KEY_MAX) return false;
    for (uint8_t i = 0; i < len; i++)
        if (k[i] < 0x20 || k[i] > 0x7E || k[i] == '"' || k[i] == '=') return false;
    return true;
}

/* The record for this key: its offset in g_img, or 0. */
static uint16_t find(const uint8_t *k, uint8_t len) {
    uint16_t at = FL_HEADER, end = (uint16_t)(FL_HEADER + g_used);
    while (at < end) {
        uint8_t kl = g_img[at];
        uint16_t size = (uint16_t)(1 + kl + chunk_len(g_img + at + 1 + kl));
        if (kl == len && memcmp(g_img + at + 1, k, len) == 0) return at;
        at = (uint16_t)(at + size);
    }
    return 0;
}

static uint16_t record_size(uint16_t at) {
    uint8_t kl = g_img[at];
    return (uint16_t)(1 + kl + chunk_len(g_img + at + 1 + kl));
}

static void remove_at(uint16_t at) {
    uint16_t size = record_size(at), end = (uint16_t)(FL_HEADER + g_used);
    memmove(g_img + at, g_img + at + size, (size_t)(end - at - size));
    g_used = (uint16_t)(g_used - size);
}

uint8_t fl_command(const fl_flash_t *f, uint8_t command, uint8_t *w) {
    if (!g_loaded) load(f);
    switch (command) {
    case EXP_COMMAND_FL_GET: {
        if (!key_ok(w + 1, w[0])) return EXP_STATUS_ERROR;
        uint16_t at = find(w + 1, w[0]);
        if (!at) return EXP_STATUS_EOF;
        uint8_t kl = g_img[at];
        memcpy(w, g_img + at + 1 + kl, chunk_len(g_img + at + 1 + kl));
        return EXP_STATUS_SUCCESS;
    }
    case EXP_COMMAND_FL_SET: {
        uint8_t kl = w[0];
        if (!key_ok(w + 1, kl)) return EXP_STATUS_ERROR;
        uint8_t cl = chunk_len(w + 1 + kl);
        if (cl == 0) return EXP_STATUS_ERROR;
        uint16_t at = find(w + 1, kl);
        uint16_t old = at ? record_size(at) : 0;
        if (at && old == 1 + kl + cl && memcmp(g_img + at, w, old) == 0) return EXP_STATUS_SUCCESS; /* unchanged */
        if (g_used - old + 1 + kl + cl > FL_CAPACITY) return EXP_STATUS_ERROR; /* full */
        if (at) remove_at(at);
        memcpy(g_img + FL_HEADER + g_used, w, (size_t)(1 + kl + cl));
        g_used = (uint16_t)(g_used + 1 + kl + cl);
        return save(f) ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
    }
    case EXP_COMMAND_FL_RM: {
        if (!key_ok(w + 1, w[0])) return EXP_STATUS_ERROR;
        uint16_t at = find(w + 1, w[0]);
        if (!at) return EXP_STATUS_EOF;
        remove_at(at);
        return save(f) ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
    }
    case EXP_COMMAND_FL_LIST: { /* in [first, 2 BE]; out [count, 2 BE][returned][records] */
        uint16_t first = (uint16_t)(w[0] << 8 | w[1]);
        uint16_t at = FL_HEADER, end = (uint16_t)(FL_HEADER + g_used), n = 0, out = 3;
        uint8_t returned = 0;
        bool full = false; /* in order: the rest from the next FL_LIST */
        while (at < end) {
            uint16_t size = record_size(at);
            if (n >= first && !full) {
                if (returned < 255 && out + size <= EXP_FL_LIST_MAX) {
                    memcpy(w + out, g_img + at, size);
                    out = (uint16_t)(out + size);
                    returned++;
                } else {
                    full = true;
                }
            }
            n++;
            at = (uint16_t)(at + size);
        }
        w[0] = (uint8_t)(n >> 8);
        w[1] = (uint8_t)n;
        w[2] = returned;
        return EXP_STATUS_SUCCESS;
    }
    case EXP_COMMAND_FL_INFO: { /* [used, 2 BE][capacity, 2 BE] */
        w[0] = (uint8_t)(g_used >> 8);
        w[1] = (uint8_t)g_used;
        w[2] = (uint8_t)(FL_CAPACITY >> 8);
        w[3] = (uint8_t)FL_CAPACITY;
        return EXP_STATUS_SUCCESS;
    }
    default:
        return EXP_STATUS_ERROR;
    }
}

void fl_forget(void) { g_loaded = false; }
