/* link_secure.h -- the PC-1500 Link's authentication and encryption
 * (2026-10-03, BLE_PROTOCOL.md "Security"): the cryptography of pairing,
 * the proofs each side gives when a link starts, and the sealing of every
 * frame after that. The protocol around it -- who sends what, when -- is
 * the caller's (ble_link.c, pc1500emu's LinkCore); this is only the maths,
 * so all of them compute exactly the same bytes (the laptop app's Dart
 * port is ble_app/lib/secure.dart).
 *
 * Built on Monocypher (third_party/monocypher): X25519, SHA-512,
 * HMAC-SHA512, HKDF-SHA512, and ChaCha20-Poly1305 as RFC 8439 has it.
 *
 * Portable C, no Pico SDK: pc1500emu compiles it too. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_ID_LEN 8      /* a side's identity, random, kept for good */
#define LS_NONCE_LEN 16  /* HELLO's and pairing's fresh values */
#define LS_KEY_LEN 32    /* the long-term key a pairing makes */
#define LS_PUB_LEN 32    /* an X25519 public key */
#define LS_PROOF_LEN 16  /* a proof, a commitment, a confirmation */
#define LS_HEADER 4      /* a frame's type, seq, len */
#define LS_OVERHEAD 20   /* what sealing adds to a frame: counter 4, tag 16 */

/* ---- pairing (once per pair of devices) ---- */

/* A key pair from 32 random bytes (the secret key is those bytes). */
void ls_keypair(const uint8_t random32[32], uint8_t sk[32], uint8_t pk[LS_PUB_LEN]);

/* The advertiser's commitment to its pairing nonce, sent before it sees
 * the connector's: SHA-512("PC1500 commit" | pk_s | pk_c | n_s), 16 bytes. */
void ls_pair_commit(const uint8_t pk_s[LS_PUB_LEN], const uint8_t pk_c[LS_PUB_LEN], const uint8_t n_s[LS_NONCE_LEN],
                    uint8_t out[LS_PROOF_LEN]);

/* The six-digit code both sides show: the first 4 bytes of SHA-512("PC1500
 * code" | pk_c | pk_s | n_c | n_s), big-endian, mod 1000000. */
uint32_t ls_pair_code(const uint8_t pk_c[LS_PUB_LEN], const uint8_t pk_s[LS_PUB_LEN], const uint8_t n_c[LS_NONCE_LEN],
                      const uint8_t n_s[LS_NONCE_LEN]);

/* The long-term key: HKDF-SHA512(X25519(sk, pk_peer), salt n_c | n_s,
 * info "PC1500 pair" | pk_c | pk_s), 32 bytes. False if the peer's key
 * was a degenerate one (all-zero shared secret). */
bool ls_pair_ltk(const uint8_t sk[32], const uint8_t pk_peer[LS_PUB_LEN], const uint8_t pk_c[LS_PUB_LEN],
                 const uint8_t pk_s[LS_PUB_LEN], const uint8_t n_c[LS_NONCE_LEN], const uint8_t n_s[LS_NONCE_LEN],
                 uint8_t ltk[LS_KEY_LEN]);

/* A side's confirmation that it accepted, and holds the same key:
 * HMAC-SHA512(ltk, "PC1500 confirm C" or "...S" | id_c | id_s), 16 bytes.
 * role: 'C' the connector, 'S' the advertiser. */
void ls_pair_confirm(const uint8_t ltk[LS_KEY_LEN], char role, const uint8_t id_c[LS_ID_LEN],
                     const uint8_t id_s[LS_ID_LEN], uint8_t out[LS_PROOF_LEN]);

/* ---- every link ---- */

/* A side's proof that it holds the pair's key, for this link:
 * HMAC-SHA512(ltk, "PC1500 auth C" or "...S" | nonce_c | nonce_s | id_c
 * | id_s), 16 bytes. */
void ls_auth_proof(const uint8_t ltk[LS_KEY_LEN], char role, const uint8_t nonce_c[LS_NONCE_LEN],
                   const uint8_t nonce_s[LS_NONCE_LEN], const uint8_t id_c[LS_ID_LEN], const uint8_t id_s[LS_ID_LEN],
                   uint8_t out[LS_PROOF_LEN]);

/* Constant-time comparison of two proofs (or commitments). */
bool ls_equal16(const uint8_t a[LS_PROOF_LEN], const uint8_t b[LS_PROOF_LEN]);

/* A link's encryption, once both sides have proved themselves. Keys:
 * HKDF-SHA512(ltk, salt nonce_c | nonce_s, info "PC1500 session"), 64
 * bytes -- the first 32 the connector's sending key, the rest the
 * advertiser's. */
typedef struct {
    bool on;
    uint8_t tx_key[32], rx_key[32];
    uint32_t tx_counter;  /* the next frame's */
    bool rx_any;
    uint32_t rx_high;     /* the highest counter taken, */
    uint32_t rx_seen;     /* and which of the 32 below it were (bit n = rx_high - n) */
} ls_session_t;

void ls_session_start(ls_session_t *s, const uint8_t ltk[LS_KEY_LEN], const uint8_t nonce_c[LS_NONCE_LEN],
                      const uint8_t nonce_s[LS_NONCE_LEN], bool connector);
void ls_session_end(ls_session_t *s);

/* Seals a frame ([type][seq][len][payload], len = payload size) into
 * `out` (a separate buffer, room for len + LS_OVERHEAD): [type][seq][len'][counter u32]
 * [payload, encrypted][tag 16], len' = 4 + payload + 16. The header and
 * counter are authenticated too. Returns out's size, 0 if the counter has
 * run out (a link that long must be restarted). */
uint16_t ls_seal(ls_session_t *s, const uint8_t *frame, uint16_t len, uint8_t *out);

/* The reverse: `frame` as received, `len` its size; the plain frame into
 * `out` (a separate buffer). Returns its size, 0 if it isn't authentic, is
 * a replay, or is too old (more than 32 behind the newest). */
uint16_t ls_open(ls_session_t *s, const uint8_t *frame, uint16_t len, uint8_t *out);

void ls_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
