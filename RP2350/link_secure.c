/* link_secure.c -- see link_secure.h. */
#include "link_secure.h"

#include <string.h>

#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"

void ls_wipe(void *p, size_t n) { crypto_wipe(p, n); }

bool ls_equal16(const uint8_t a[LS_PROOF_LEN], const uint8_t b[LS_PROOF_LEN]) { return crypto_verify16(a, b) == 0; }

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- pairing ---- */

void ls_keypair(const uint8_t random32[32], uint8_t sk[32], uint8_t pk[LS_PUB_LEN]) {
    memcpy(sk, random32, 32);
    crypto_x25519_public_key(pk, sk);
}

void ls_pair_commit(const uint8_t pk_s[LS_PUB_LEN], const uint8_t pk_c[LS_PUB_LEN], const uint8_t n_s[LS_NONCE_LEN],
                    uint8_t out[LS_PROOF_LEN]) {
    static const char kLabel[] = "PC1500 commit";
    crypto_sha512_ctx ctx;
    uint8_t h[64];
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, (const uint8_t *)kLabel, sizeof kLabel - 1);
    crypto_sha512_update(&ctx, pk_s, LS_PUB_LEN);
    crypto_sha512_update(&ctx, pk_c, LS_PUB_LEN);
    crypto_sha512_update(&ctx, n_s, LS_NONCE_LEN);
    crypto_sha512_final(&ctx, h);
    memcpy(out, h, LS_PROOF_LEN);
}

uint32_t ls_pair_code(const uint8_t pk_c[LS_PUB_LEN], const uint8_t pk_s[LS_PUB_LEN], const uint8_t n_c[LS_NONCE_LEN],
                      const uint8_t n_s[LS_NONCE_LEN]) {
    static const char kLabel[] = "PC1500 code";
    crypto_sha512_ctx ctx;
    uint8_t h[64];
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, (const uint8_t *)kLabel, sizeof kLabel - 1);
    crypto_sha512_update(&ctx, pk_c, LS_PUB_LEN);
    crypto_sha512_update(&ctx, pk_s, LS_PUB_LEN);
    crypto_sha512_update(&ctx, n_c, LS_NONCE_LEN);
    crypto_sha512_update(&ctx, n_s, LS_NONCE_LEN);
    crypto_sha512_final(&ctx, h);
    return (((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3]) % 1000000u;
}

bool ls_pair_ltk(const uint8_t sk[32], const uint8_t pk_peer[LS_PUB_LEN], const uint8_t pk_c[LS_PUB_LEN],
                 const uint8_t pk_s[LS_PUB_LEN], const uint8_t n_c[LS_NONCE_LEN], const uint8_t n_s[LS_NONCE_LEN],
                 uint8_t ltk[LS_KEY_LEN]) {
    static const char kLabel[] = "PC1500 pair";
    uint8_t shared[32], salt[2 * LS_NONCE_LEN], info[sizeof kLabel - 1 + 2 * LS_PUB_LEN], zero = 0;
    crypto_x25519(shared, sk, pk_peer);
    for (int i = 0; i < 32; i++) zero |= shared[i];
    if (!zero) return false;
    memcpy(salt, n_c, LS_NONCE_LEN);
    memcpy(salt + LS_NONCE_LEN, n_s, LS_NONCE_LEN);
    memcpy(info, kLabel, sizeof kLabel - 1);
    memcpy(info + sizeof kLabel - 1, pk_c, LS_PUB_LEN);
    memcpy(info + sizeof kLabel - 1 + LS_PUB_LEN, pk_s, LS_PUB_LEN);
    crypto_sha512_hkdf(ltk, LS_KEY_LEN, shared, sizeof shared, salt, sizeof salt, info, sizeof info);
    crypto_wipe(shared, sizeof shared);
    return true;
}

/* HMAC-SHA512(key, label | parts...), the first 16 bytes. */
static void mac16(const uint8_t key[LS_KEY_LEN], const char *label, char role, const uint8_t *a, size_t an,
                  const uint8_t *b, size_t bn, const uint8_t *c, size_t cn, const uint8_t *d, size_t dn,
                  uint8_t out[LS_PROOF_LEN]) {
    crypto_sha512_hmac_ctx ctx;
    uint8_t h[64];
    crypto_sha512_hmac_init(&ctx, key, LS_KEY_LEN);
    crypto_sha512_hmac_update(&ctx, (const uint8_t *)label, strlen(label));
    crypto_sha512_hmac_update(&ctx, (const uint8_t *)&role, 1);
    if (an) crypto_sha512_hmac_update(&ctx, a, an);
    if (bn) crypto_sha512_hmac_update(&ctx, b, bn);
    if (cn) crypto_sha512_hmac_update(&ctx, c, cn);
    if (dn) crypto_sha512_hmac_update(&ctx, d, dn);
    crypto_sha512_hmac_final(&ctx, h);
    memcpy(out, h, LS_PROOF_LEN);
    crypto_wipe(h, sizeof h);
}

void ls_pair_confirm(const uint8_t ltk[LS_KEY_LEN], char role, const uint8_t id_c[LS_ID_LEN],
                     const uint8_t id_s[LS_ID_LEN], uint8_t out[LS_PROOF_LEN]) {
    mac16(ltk, "PC1500 confirm ", role, id_c, LS_ID_LEN, id_s, LS_ID_LEN, NULL, 0, NULL, 0, out);
}

/* ---- every link ---- */

void ls_auth_proof(const uint8_t ltk[LS_KEY_LEN], char role, const uint8_t nonce_c[LS_NONCE_LEN],
                   const uint8_t nonce_s[LS_NONCE_LEN], const uint8_t id_c[LS_ID_LEN], const uint8_t id_s[LS_ID_LEN],
                   uint8_t out[LS_PROOF_LEN]) {
    mac16(ltk, "PC1500 auth ", role, nonce_c, LS_NONCE_LEN, nonce_s, LS_NONCE_LEN, id_c, LS_ID_LEN, id_s, LS_ID_LEN,
          out);
}

void ls_session_start(ls_session_t *s, const uint8_t ltk[LS_KEY_LEN], const uint8_t nonce_c[LS_NONCE_LEN],
                      const uint8_t nonce_s[LS_NONCE_LEN], bool connector) {
    static const char kInfo[] = "PC1500 session";
    uint8_t okm[64], salt[2 * LS_NONCE_LEN];
    memcpy(salt, nonce_c, LS_NONCE_LEN);
    memcpy(salt + LS_NONCE_LEN, nonce_s, LS_NONCE_LEN);
    crypto_sha512_hkdf(okm, sizeof okm, ltk, LS_KEY_LEN, salt, sizeof salt, (const uint8_t *)kInfo, sizeof kInfo - 1);
    memset(s, 0, sizeof *s);
    memcpy(s->tx_key, connector ? okm : okm + 32, 32);
    memcpy(s->rx_key, connector ? okm + 32 : okm, 32);
    crypto_wipe(okm, sizeof okm);
    s->on = true;
}

void ls_session_end(ls_session_t *s) { crypto_wipe(s, sizeof *s); }

/* The 12-byte nonce for a counter: 4 zero bytes, then the counter as 8
 * bytes, little-endian. */
static void nonce_of(uint32_t counter, uint8_t nonce[12]) {
    memset(nonce, 0, 12);
    put32(nonce + 4, counter);
}

uint16_t ls_seal(ls_session_t *s, const uint8_t *frame, uint16_t len, uint8_t *out) {
    const uint16_t n = (uint16_t)(len - LS_HEADER);
    uint8_t nonce[12], aad[6];
    crypto_aead_ctx ctx;
    if (len < LS_HEADER || s->tx_counter == 0xFFFFFFFFu) return 0;
    const uint32_t counter = s->tx_counter++;
    nonce_of(counter, nonce);
    aad[0] = frame[0];
    aad[1] = frame[1];
    put32(aad + 2, counter);
    out[0] = frame[0];
    out[1] = frame[1];
    out[2] = (uint8_t)(n + LS_OVERHEAD);
    out[3] = (uint8_t)((n + LS_OVERHEAD) >> 8);
    put32(out + 4, counter);
    /* a fresh context per frame: Monocypher's stream rekeys after each
     * message, and one message per nonce is plain RFC 8439 */
    crypto_aead_init_ietf(&ctx, s->tx_key, nonce);
    crypto_aead_write(&ctx, out + 8, out + 8 + n, aad, sizeof aad, frame + LS_HEADER, n);
    crypto_wipe(&ctx, sizeof ctx);
    return (uint16_t)(len + LS_OVERHEAD);
}

uint16_t ls_open(ls_session_t *s, const uint8_t *frame, uint16_t len, uint8_t *out) {
    uint8_t nonce[12], aad[6], tag[16], type, seq;
    crypto_aead_ctx ctx;
    uint32_t counter, diff = 0;
    uint16_t n;
    int bad;
    if (len < LS_HEADER + LS_OVERHEAD) return 0;
    n = (uint16_t)(len - LS_HEADER - LS_OVERHEAD);
    counter = get32(frame + 4);
    if (s->rx_any && counter <= s->rx_high) { /* a late one: new, and not too late? */
        diff = s->rx_high - counter;
        if (diff >= 32 || (s->rx_seen & (1u << diff))) return 0;
    }
    type = frame[0];
    seq = frame[1];
    nonce_of(counter, nonce);
    aad[0] = type;
    aad[1] = seq;
    put32(aad + 2, counter);
    memcpy(tag, frame + 8 + n, 16);
    crypto_aead_init_ietf(&ctx, s->rx_key, nonce);
    bad = crypto_aead_read(&ctx, out + LS_HEADER, tag, aad, sizeof aad, frame + 8, n);
    crypto_wipe(&ctx, sizeof ctx);
    if (bad) return 0;
    if (!s->rx_any || counter > s->rx_high) {
        const uint32_t shift = s->rx_any ? counter - s->rx_high : 32;
        s->rx_seen = shift >= 32 ? 1u : (s->rx_seen << shift) | 1u;
        s->rx_high = counter;
        s->rx_any = true;
    } else {
        s->rx_seen |= 1u << diff;
    }
    out[0] = type;
    out[1] = seq;
    out[2] = (uint8_t)n;
    out[3] = (uint8_t)(n >> 8);
    return (uint16_t)(LS_HEADER + n);
}
