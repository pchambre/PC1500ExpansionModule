/* ssh_host_test.c -- runs the firmware's SSH client (ssh_client.c,
 * ssh_term.c, sha256.c) on a PC, over Winsock (2026-10-07).
 *
 *   ssh_host_test                       the offline checks only
 *   ssh_host_test HOST[:PORT] USER KEYFILE|- [PASSWORD] [COMMAND...]
 *
 * KEYFILE is an unencrypted OpenSSH ed25519 private key ("-" for none).
 * Logs in, accepts the host key (printing its fingerprint), sends each
 * COMMAND as a line, then "exit", and prints the shell's lines as the
 * 26-character display would show them, with the scrollback after.
 *
 * Build (MinGW, from RP2350/; add ssh_keys.c):
 *   gcc -std=c99 -O2 -I. tools/ssh_host_test.c ssh_client.c ssh_term.c sha256.c
 *       third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
 *       -lws2_32 -ladvapi32 -o build/ssh_host_test.exe */
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "ssh_client.h"
#include "ssh_term.h"
#include "ssh_keys.h"
#include "kbd_seq.h"

static int g_failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                              \
        }                                                              \
    } while (0)

/* ---- offline checks ---- */

static void hex(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
}

static void check_sha256(void) {
    uint8_t h[32];
    char x[65];
    sha256("", 0, h);
    hex(h, 32, x);
    CHECK(strcmp(x, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha256("abc", 3, h);
    hex(h, 32, x);
    CHECK(strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    hex(h, 32, x);
    CHECK(strcmp(x, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    {
        sha256_t s;
        static uint8_t a[1000];
        memset(a, 'a', sizeof a);
        sha256_init(&s);
        for (int i = 0; i < 1000; i++) sha256_update(&s, a, sizeof a);
        sha256_final(&s, h);
        hex(h, 32, x);
        CHECK(strcmp(x, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
}

static void check_base64(void) {
    char out[16];
    ssh_base64((const uint8_t *)"f", 1, out, true);
    CHECK(strcmp(out, "Zg==") == 0);
    ssh_base64((const uint8_t *)"fo", 2, out, true);
    CHECK(strcmp(out, "Zm8=") == 0);
    ssh_base64((const uint8_t *)"foob", 4, out, false);
    CHECK(strcmp(out, "Zm9vYg") == 0);
}

static void out_str(ssh_term_t *t, const char *s) { ssh_term_output(t, (const uint8_t *)s, strlen(s)); }

static int shows(const ssh_term_t *t, const char *want) {
    char line[TERM_WIDTH + 1], padded[TERM_WIDTH + 1];
    ssh_term_render(t, line);
    line[TERM_WIDTH] = 0;
    snprintf(padded, sizeof padded, "%-26s", want);
    if (strcmp(line, padded) != 0) printf("  shows [%s], wanted [%s]\n", line, padded);
    return strcmp(line, padded) == 0;
}

static void check_term(void) {
    static ssh_term_t t;
    static const char *long_line =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()_+-=[]{}"; /* 80 */
    ssh_term_init(&t);
    out_str(&t, "hello\r\n$ ");
    CHECK(shows(&t, "$ "));
    CHECK(ssh_term_render(&t, (char[TERM_WIDTH]){0}) == 2);
    ssh_term_scroll(&t, 1);
    CHECK(shows(&t, "hello"));
    ssh_term_scroll(&t, -1);
    CHECK(shows(&t, "$ "));

    /* an 80-character line: the live window follows the cursor */
    ssh_term_init(&t);
    out_str(&t, long_line);
    CHECK(shows(&t, "STUVWXYZ!@#$%^&*()_+-=[]{}"));
    out_str(&t, "\r\n");
    ssh_term_scroll(&t, 1);
    CHECK(shows(&t, "0123456789abcdefghijklmnop"));
    ssh_term_pan(&t, 1);
    CHECK(shows(&t, "qrstuvwxyzABCDEFGHIJKLMNOP"));
    ssh_term_pan(&t, 1);
    CHECK(shows(&t, "QRSTUVWXYZ!@#$%^&*()_+-=[]"));
    ssh_term_pan(&t, 1); /* the last step stops at the line's end */
    CHECK(shows(&t, "STUVWXYZ!@#$%^&*()_+-=[]{}"));
    ssh_term_pan(&t, 1);
    CHECK(shows(&t, "STUVWXYZ!@#$%^&*()_+-=[]{}"));
    ssh_term_pan(&t, -1);
    CHECK(shows(&t, "stuvwxyzABCDEFGHIJKLMNOPQR")); /* 54 - 26 */
    ssh_term_live(&t);
    CHECK(shows(&t, ""));

    /* wrap past 80, BS, CSI K, escapes dropped, UTF-8 as ? */
    ssh_term_init(&t);
    out_str(&t, long_line);
    out_str(&t, "Z");
    CHECK(shows(&t, "Z"));
    ssh_term_init(&t);
    out_str(&t, "abc\b\bX\x1b[Kq");
    CHECK(shows(&t, "aXq"));
    ssh_term_init(&t);
    out_str(&t, "\x1b[1;32mgreen\x1b[0m \x1b]0;title\x07ok \xc3\xa9t\xc3\xa9");
    CHECK(shows(&t, "green ok ?t?"));
    ssh_term_init(&t);
    out_str(&t, "a\tb");
    CHECK(shows(&t, "a       b"));

    /* scrolled back, new output keeps the same line in view */
    ssh_term_init(&t);
    out_str(&t, "one\r\ntwo\r\n");
    ssh_term_scroll(&t, 2);
    CHECK(shows(&t, "one"));
    out_str(&t, "three\r\n");
    CHECK(shows(&t, "one"));
}

/* One key pressed and let go: what it sent, and what it did to the view. */
static size_t press(ssh_keys_t *k, uint8_t key, uint8_t out[SSH_KEYS_OUT_MAX], ssh_view_t *view) {
    static uint32_t now = 1000;
    ssh_view_t up;
    size_t n = ssh_keys_sample(k, key, now += 100, out, view);
    ssh_keys_sample(k, 0, now += 100, out + n, &up);
    return n;
}

static void check_keys(void) {
    ssh_keys_t k;
    uint8_t out[2 * SSH_KEYS_OUT_MAX];
    ssh_view_t view;
    ssh_keys_init(&k);
    CHECK(press(&k, KBD_K_A, out, &view) == 1 && out[0] == 'a');
    press(&k, KBD_K_SHIFT, out, &view);
    CHECK(press(&k, KBD_K_A, out, &view) == 1 && out[0] == 'A');
    press(&k, KBD_K_DEF, out, &view);
    CHECK(press(&k, KBD_K_C, out, &view) == 1 && out[0] == 0x03);
    CHECK(press(&k, KBD_K_UP, out, &view) == 3 && out[2] == 'A' && view == SSH_VIEW_NONE);
    /* DEF+Up: scrollback, where the plain arrows move the view... */
    press(&k, KBD_K_DEF, out, &view);
    CHECK(press(&k, KBD_K_UP, out, &view) == 0 && view == SSH_VIEW_UP && k.scrolling);
    CHECK(press(&k, KBD_K_UP, out, &view) == 0 && view == SSH_VIEW_UP);
    CHECK(press(&k, KBD_K_RIGHT, out, &view) == 0 && view == SSH_VIEW_RIGHT);
    /* ...until DEF (or CL), which only ends it */
    CHECK(press(&k, KBD_K_DEF, out, &view) == 0 && view == SSH_VIEW_LIVE && !k.scrolling && !k.def);
    CHECK(press(&k, KBD_K_C, out, &view) == 1 && out[0] == 'c');
    press(&k, KBD_K_DEF, out, &view);
    press(&k, KBD_K_DOWN, out, &view);
    CHECK(press(&k, KBD_K_CL, out, &view) == 0 && view == SSH_VIEW_LIVE && !k.scrolling);
    /* any other key ends it and goes to the shell */
    press(&k, KBD_K_DEF, out, &view);
    press(&k, KBD_K_UP, out, &view);
    CHECK(press(&k, KBD_K_L, out, &view) == 1 && out[0] == 'l' && view == SSH_VIEW_LIVE && !k.scrolling);
    /* outside it, DEF+CL still ends the session */
    press(&k, KBD_K_DEF, out, &view);
    CHECK(press(&k, KBD_K_CL, out, &view) == 0 && view == SSH_VIEW_QUIT);
}

/* ---- live session ---- */

typedef struct {
    SOCKET sock;
    ssh_term_t term;
    uint32_t shown_version;
    HCRYPTPROV rng;
} host_t;

static bool host_send(void *ctx, const uint8_t *data, size_t len) {
    host_t *h = ctx;
    while (len) {
        int n = send(h->sock, (const char *)data, (int)len, 0);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static void host_random(void *ctx, uint8_t *out, size_t len) {
    host_t *h = ctx;
    if (!CryptGenRandom(h->rng, (DWORD)len, out)) {
        printf("no random numbers\n");
        exit(2);
    }
}

static void host_data(void *ctx, const uint8_t *data, size_t len) {
    host_t *h = ctx;
    ssh_term_output(&h->term, data, len);
}

static void show_display(host_t *h) {
    char line[TERM_WIDTH + 1];
    uint8_t cursor;
    if (h->term.version == h->shown_version) return;
    h->shown_version = h->term.version;
    cursor = ssh_term_render(&h->term, line);
    line[TERM_WIDTH] = 0;
    printf("  |%s|", line);
    if (cursor != TERM_NO_CURSOR) printf(" cursor %u", cursor);
    printf("\n");
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* An unencrypted OpenSSH ed25519 private key's 64-byte secret (PROTOCOL.key). */
static bool load_key(const char *path, uint8_t key[64]) {
    static char text[8192];
    static uint8_t bin[6144];
    size_t n = 0, bits = 0, len;
    uint32_t acc = 0, l;
    const uint8_t *p;
    FILE *f = fopen(path, "rb");
    char *body;
    if (!f) return false;
    len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    body = strstr(text, "-----BEGIN OPENSSH PRIVATE KEY-----");
    if (!body) return false;
    for (body += 35; *body && *body != '-'; body++) {
        int v = b64val(*body);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bin[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (n < 15 || memcmp(bin, "openssh-key-v1", 15) != 0) return false;
    p = bin + 15;
    for (int i = 0; i < 3; i++) { /* cipher, kdf, kdf options */
        l = be32(p);
        if (i < 2 && !(l == 4 && memcmp(p + 4, "none", 4) == 0)) return false; /* encrypted */
        p += 4 + l;
    }
    p += 4;           /* number of keys */
    p += 4 + be32(p); /* the public key */
    p += 4;           /* the private section's length */
    p += 8;           /* check ints */
    p += 4 + be32(p); /* key type */
    p += 4 + be32(p); /* public key */
    if (be32(p) != 64) return false;
    memcpy(key, p + 4, 64);
    return true;
}

static int run_session(int argc, char **argv) {
    static host_t h;
    static ssh_t s;
    char host[128], *colon, fp[SSH_FINGERPRINT_LEN + 1];
    const char *port = "22", *password = argc > 4 ? argv[4] : NULL;
    uint8_t key[64];
    bool have_key = strcmp(argv[3], "-") != 0;
    struct hostent *he;
    struct sockaddr_in addr;
    WSADATA wsa;
    ssh_io_t io = {&h, host_send, host_random, host_data};
    int next_command = 5;
    DWORD quiet_since = GetTickCount();
    bool exit_sent = false;

    if (have_key && !load_key(argv[3], key)) {
        printf("can't read the key %s (an unencrypted OpenSSH ed25519 key)\n", argv[3]);
        return 2;
    }
    snprintf(host, sizeof host, "%s", argv[1]);
    if ((colon = strchr(host, ':')) != NULL) {
        *colon = 0;
        port = colon + 1;
    }
    if (!CryptAcquireContextA(&h.rng, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return 2;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if ((he = gethostbyname(host)) == NULL) { /* old MinGW: no getaddrinfo */
        printf("can't find %s\n", host);
        return 2;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)atoi(port));
    memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
    h.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(h.sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        printf("can't connect to %s:%s\n", host, port);
        return 2;
    }
    ssh_term_init(&h.term);
    ssh_start(&s, &io, argv[2], have_key ? key : NULL, password);
    while (ssh_state(&s) != SSH_ST_CLOSED) {
        fd_set rd;
        struct timeval tv = {0, 200000};
        FD_ZERO(&rd);
        FD_SET(h.sock, &rd);
        if (select(0, &rd, NULL, NULL, &tv) > 0) {
            uint8_t buf[4096];
            int n = recv(h.sock, (char *)buf, sizeof buf, 0);
            if (n <= 0) {
                ssh_close(&s, true);
                break;
            }
            size_t taken = 0;
            while (taken < (size_t)n && ssh_state(&s) != SSH_ST_CLOSED) {
                taken += ssh_feed(&s, buf + taken, (size_t)n - taken);
                if (ssh_state(&s) == SSH_ST_HOSTKEY) {
                    ssh_fingerprint(ssh_hostkey_blob(&s), fp);
                    printf("host key %s: accepted\n", fp);
                    ssh_hostkey_answer(&s, true);
                } else if (ssh_state(&s) == SSH_ST_PASSWORD) {
                    printf("the server wants a password, and none was given (or it was wrong)\n");
                    ssh_close(&s, false);
                }
            }
            quiet_since = GetTickCount();
        }
        show_display(&h);
        /* after a quiet half second, the next command */
        if (ssh_state(&s) == SSH_ST_OPEN && GetTickCount() - quiet_since > 500) {
            const char *cmd = next_command < argc ? argv[next_command++] : exit_sent ? NULL : "exit";
            if (cmd) {
                char line[256];
                if (strcmp(cmd, "exit") == 0) exit_sent = true;
                printf("> %s\n", cmd);
                snprintf(line, sizeof line, "%s\r", cmd);
                ssh_term_live(&h.term);
                ssh_write(&s, (const uint8_t *)line, strlen(line));
            }
            quiet_since = GetTickCount();
        }
    }
    printf("closed: error %d%s\n", (int)ssh_error(&s),
           ssh_error(&s) == SSH_ERR_DISCONNECTED ? " (the server disconnected)" : "");
    printf("scrollback, oldest first:\n");
    for (int i = h.term.count; i >= 1; i--) {
        const term_line_t *l = &h.term.history[(h.term.newest + TERM_HISTORY - (i - 1)) % TERM_HISTORY];
        printf("  %.*s\n", l->len, l->text);
    }
    closesocket(h.sock);
    return ssh_error(&s) == SSH_ERR_NONE ? 0 : 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    check_sha256();
    check_base64();
    check_term();
    check_keys();
    printf("offline checks: %s\n", g_failures ? "FAILED" : "pass");
    if (g_failures) return 1;
    if (argc >= 4) return run_session(argc, argv);
    return 0;
}
