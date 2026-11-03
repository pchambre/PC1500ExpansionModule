/* screen.c -- see screen.h. */
#include "screen.h"

#include <string.h>

#include "basic_detok.h"
#include "pc_exp.h"

static screen_backend_t B;
static bool g_on;      /* BLDISP as the last poll saw it */
static uint32_t g_gen; /* screen_generation() */

static void drop_all(void);
static void peer_reset(void);

static void lock(void) {
    if (B.lock) B.lock(B.ctx);
}
static void unlock(void) {
    if (B.unlock) B.unlock(B.ctx);
}

/* BLDISP: turned on or off, either way from nothing. */
static bool sync_on(void) {
    bool on = B.enabled && B.enabled(B.ctx);
    if (on != g_on) {
        g_on = on;
        drop_all();
        peer_reset();
    }
    return on;
}

/* ---- What there is to show ----
 * The history: finished records (LINE, OUTPUT) back to back, `kind len
 * bytes`, in a ring addressed by absolute positions -- the oldest whole
 * records give way to new ones. */
static uint8_t g_hist[SCREEN_HISTORY];
static uint32_t g_begin, g_end;
#define HIST(p) g_hist[(p) % SCREEN_HISTORY]

/* The latest states. Each change counts up its version, which is how the
 * peer's side knows it has to go again -- also when it changes while a
 * frame is on its way (the firmware's wait hook feeds the watch then). */
static uint8_t g_edit[1 + SCREEN_TEXT_MAX]; /* [cursor][text]; none when g_edit_len is 0 */
static uint16_t g_edit_len;
static uint32_t g_edit_ver;
static uint8_t g_status[2];
static bool g_status_valid;
static uint8_t g_game[255];
static uint8_t g_game_len;
static uint32_t g_game_ver;
static uint32_t g_clears; /* counts CLEARs */

/* SHOW's line, while it stands as the EDIT line (screen.h). */
static char g_show[64];
static size_t g_show_len;
static bool g_show_pending;

static void hist_add(uint8_t kind, const uint8_t *bytes, size_t len) {
    if (len > SCREEN_TEXT_MAX) len = SCREEN_TEXT_MAX;
    while (g_end - g_begin + 2u + len > SCREEN_HISTORY) g_begin += 2u + HIST(g_begin + 1);
    HIST(g_end) = kind;
    HIST(g_end + 1) = (uint8_t)len;
    for (size_t i = 0; i < len; i++) HIST(g_end + 2 + i) = bytes[i];
    g_end += 2u + (uint32_t)len;
}

static void set_edit(const uint8_t *text, size_t len, int cursor) {
    if (len > SCREEN_TEXT_MAX - 1) len = SCREEN_TEXT_MAX - 1;
    g_edit[0] = (uint8_t)(int8_t)cursor;
    memcpy(g_edit + 1, text, len);
    g_edit_len = (uint16_t)(1 + len);
    g_edit_ver++;
}

static void commit_show(void) {
    if (!g_show_pending) return;
    g_show_pending = false;
    g_edit_len = 0;
    hist_add(SCREEN_OUTPUT, (const uint8_t *)g_show, g_show_len);
}

static void put_line(uint8_t kind, const uint8_t *text, size_t len) {
    if (!sync_on()) return;
    commit_show();
    hist_add(kind, text, len);
    g_edit_len = 0; /* a line ends the peer's EDIT: it's what that became */
}

static void put_edit(const uint8_t *text, size_t len, int cursor) {
    if (!sync_on()) return;
    commit_show();
    set_edit(text, len, cursor);
}

static void put_clear(void) {
    if (!sync_on()) return;
    g_show_pending = false;
    g_begin = g_end;
    g_edit_len = 0;
    g_clears++;
}

static void put_game(const uint8_t *bytes, uint8_t len) {
    if (!sync_on()) return;
    memcpy(g_game, bytes, len);
    g_game_len = len;
    g_game_ver++;
}

static void put_status(const uint8_t status[2]) {
    if (!sync_on()) return;
    memcpy(g_status, status, 2);
    g_status_valid = true;
}

static void drop_all(void) {
    g_begin = g_end = 0;
    g_edit_len = 0;
    g_status_valid = false;
    g_game_len = 0;
    g_show_pending = false;
    g_gen++;
}

/* ---- BASIC's buffers (the 2026-10-17 trial's capture, on the user's
 * PC-1500):
 * - 7B60H-7BAFH, the output buffer: PRINT's text, ASCII, padded with 00H
 *   (a number right-aligned, " 1" at column 24).
 * - 7BB0H-7BFFH, the line: typed in ASCII a key at a time; tokenized in
 *   place on ENTER; LIST copies each program line here still tokenized; a
 *   prompt fills it with 0DH and writes '>'.
 * - 7880H, the mode: 00 prompt, 40 typing, 50 editing, 20 a RUN-mode
 *   result shown, 14 a PRO-mode line entered or listed, 01 a message.
 * - 787EH/787FH, the LCD cursor -- see lcd_column().
 * - 7A10H-, a message (BREAK): the mode goes to 01, then " BREAK IN " and
 *   the line number are written here, ending in 00H.
 * - 764EH/764FH, the indicators (ROM1's STATUS1/STATUS2).
 * Every change lands in a burst a few ms long at most, so a region is
 * reported once nothing has been written there for SETTLE_US. */
#define OUT_BASE 0x7B60u
#define LINE_BASE 0x7BB0u
#define BUF_LEN 80u
#define MSG_BASE 0x7A10u
#define MSG_LEN 32u
#define MODE_PROMPT 0x00u
#define MODE_TYPING 0x40u
#define MODE_EDITING 0x50u
#define MODE_MESSAGE 0x01u
#define SETTLE_US 20000u
/* The indicators alone go at most this often: BUSY flickers with every
 * statement a program runs. With anything else, at once. */
#define STATUS_MIN_US 100000u

static uint8_t g_out[BUF_LEN], g_line[BUF_LEN], g_msg[MSG_LEN];
static uint8_t g_cur_hi, g_cur_lo, g_mode;
static bool g_out_dirty, g_line_dirty, g_msg_dirty;
static bool g_msg_pending;  /* the mode went to 01: the next writes at 7A10H are a message */
static bool g_line_entered; /* reported as a LINE, and not written since */
static bool g_muted;
static uint32_t g_last_write_us;

/* Which region a burst wrote to first: reported in that order, so a line
 * entered comes before the output it starts, even when both settle at once. */
enum { R_OUT, R_LINE, R_MSG, R_COUNT };
static uint32_t g_dirty_order[R_COUNT], g_dirty_count;

/* ---- The module's own keywords (E1xx), out of the module ROM's keyword
 * table (rom.asm's format: 55H at 8800H, a first-letter index of 26 BE
 * pointers, each at its letter's first entry plus 2; entries `marker (low
 * nibble = name length) | name | code BE | address BE`, ending at a marker
 * with low nibble 0). The index's place moves with rom.asm, so it's found
 * by its pointers' letters. Read the first time a line is detokenized. */
#define MAX_MODULE_KW 160
#define MAX_NAME 15
static struct {
    uint16_t code;
    char name[MAX_NAME + 1];
} g_module_kw[MAX_MODULE_KW];
static unsigned g_module_kw_n;
static bool g_module_kw_loaded;

static uint8_t rom_byte(uint16_t addr) {
    int b = B.rom_byte ? B.rom_byte(B.ctx, addr) : -1;
    return b < 0 ? 0xFFu : (uint8_t)b;
}

static uint16_t rom_word(uint16_t addr) {
    return (uint16_t)(rom_byte(addr) << 8 | rom_byte((uint16_t)(addr + 1)));
}

static bool index_at(uint16_t at, uint16_t *table) {
    uint16_t lowest = 0xFFFF;
    unsigned used = 0;
    for (unsigned i = 0; i < 26; i++) {
        uint16_t p = rom_word((uint16_t)(at + 2 * i));
        if (!p) continue;
        if (p < 0x8802u || p > 0x9FFFu || rom_byte((uint16_t)(p - 1)) != 'A' + i) return false;
        if (p < lowest) lowest = p;
        used++;
    }
    if (!used) return false;
    *table = (uint16_t)(lowest - 2);
    return true;
}

static void load_module_keywords(void) {
    g_module_kw_loaded = true;
    uint16_t entry = 0;
    for (uint16_t at = 0x8801u; at < 0x8900u && !entry; at++)
        if (!index_at(at, &entry)) entry = 0;
    while (entry && g_module_kw_n < MAX_MODULE_KW && entry < 0x9FF0u) {
        uint8_t n = rom_byte(entry) & 0x0Fu;
        if (!n) break;
        uint16_t code = rom_word((uint16_t)(entry + 1 + n));
        if ((code >> 8) == 0xE1) {
            for (uint8_t k = 0; k < n; k++)
                g_module_kw[g_module_kw_n].name[k] = (char)rom_byte((uint16_t)(entry + 1 + k));
            g_module_kw[g_module_kw_n].name[n] = '\0';
            g_module_kw[g_module_kw_n].code = code;
            g_module_kw_n++;
        }
        entry = (uint16_t)(entry + 1 + n + 4);
    }
}

static const char *module_keyword(uint16_t code) {
    for (unsigned i = 0; i < g_module_kw_n; i++)
        if (g_module_kw[i].code == code) return g_module_kw[i].name;
    return NULL;
}

/* 787EH/787FH -> the LCD column (0-25) the cursor is on: 26 characters
 * across four 7-or-6 character groups, 12 apart (6 dot columns, two bytes
 * each): 7400H-7448H, 7506H-7542H, 7600H-7648H, 7706H-7742H. -1 if it's
 * anywhere else. */
static int lcd_column(uint8_t hi, uint8_t lo) {
    static const uint8_t first_col[4] = {0, 7, 13, 20}, start[4] = {0x00, 0x06, 0x00, 0x06},
                         count[4] = {7, 6, 7, 6};
    if (hi < 0x74 || hi > 0x77) return -1;
    unsigned g = hi - 0x74u;
    if (lo < start[g] || (lo - start[g]) % 12u || (lo - start[g]) / 12u >= count[g]) return -1;
    return first_col[g] + (lo - start[g]) / 12;
}

/* `len` LCD characters as text: blanks for anything not printable (00H is
 * padding), the trailing ones dropped, stopping at `stop`. Returns the
 * length. */
static size_t lcd_text(const uint8_t *chars, size_t len, int stop, char *text, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < len && (int)chars[i] != stop && n + 1 < size; i++)
        text[n++] = chars[i] >= 0x20 && chars[i] < 0x7F ? (char)chars[i] : ' ';
    while (n && text[n - 1] == ' ') n--;
    text[n] = '\0';
    return n;
}

static void report_out(void) {
    char text[BUF_LEN + 1];
    size_t n = lcd_text(g_out, BUF_LEN, 0x0D, text, sizeof text);
    if (n) put_line(SCREEN_OUTPUT, (const uint8_t *)text, n);
}

static void report_line(void) {
    char text[SCREEN_TEXT_MAX];
    if (g_mode == MODE_PROMPT) return; /* the prompt's own clear */
    if (!g_module_kw_loaded) load_module_keywords();
    size_t n = basic_detok_line(g_line, BUF_LEN, text, sizeof text, module_keyword);
    int col = lcd_column(g_cur_hi, g_cur_lo);
    /* CLS typed on its own: the ROM's CLS only resets the cursor (ROM1
     * LE865H/LECAEH), nothing to see -- clear the pane instead. Known by
     * its token (a line being typed is ASCII), since the cursor it resets
     * looks like typing again. In a program it leaves no trace; BLCLS does
     * it there. */
    if (g_line[0] == 0xF0 && g_line[1] == 0x88 && g_line[2] == 0x0D) {
        if (!g_line_entered) put_clear();
        g_line_entered = true;
        return;
    }
    /* ENTER on a statement that runs on (RUN): the line is tokenized and
     * the cursor leaves the LCD (787EH = 00), but the mode stays 40H
     * until it ends -- so a cursor off the LCD means entered too. */
    if ((g_mode == MODE_TYPING || g_mode == MODE_EDITING) && col >= 0) {
        put_edit((const uint8_t *)text, n, col);
    } else if (!g_line_entered) { /* not again when the mode changes after */
        g_line_entered = true;
        put_line(SCREEN_LINE, (const uint8_t *)text, n);
    }
}

static void report_msg(void) {
    char text[MSG_LEN + 1];
    g_msg_pending = false;
    size_t n = lcd_text(g_msg, MSG_LEN, 0x00, text, sizeof text);
    if (n) put_line(SCREEN_OUTPUT, (const uint8_t *)text, n);
}

/* Everything written so far, settled or not. */
static void report(void) {
    bool *flags[R_COUNT] = {&g_out_dirty, &g_line_dirty, &g_msg_dirty};
    for (;;) {
        int next = -1;
        for (int r = 0; r < R_COUNT; r++)
            if (*flags[r] && (next < 0 || g_dirty_order[r] < g_dirty_order[next])) next = r;
        if (next < 0) return;
        *flags[next] = false;
        if (next == R_OUT)
            report_out();
        else if (next == R_LINE)
            report_line();
        else
            report_msg();
    }
}

static void mark_dirty(bool *flag, unsigned region) {
    if (g_muted || !g_on) return;
    if (!*flag) g_dirty_order[region] = ++g_dirty_count;
    *flag = true;
}

void screen_watch_write(uint16_t addr, uint8_t data) {
    lock();
    bool text = true;
    if (addr >= OUT_BASE && addr < OUT_BASE + BUF_LEN) {
        g_out[addr - OUT_BASE] = data;
        mark_dirty(&g_out_dirty, R_OUT);
    } else if (addr >= LINE_BASE && addr < LINE_BASE + BUF_LEN) {
        /* The prompt's clear starting over a line not reported yet: that
         * line first. CLS comes straight back to the prompt, a few ms after
         * its ENTER -- before the line has settled. */
        if (addr == LINE_BASE && data == 0x0D && g_line[0] != 0x0D && g_line_dirty) report();
        g_line[addr - LINE_BASE] = data;
        mark_dirty(&g_line_dirty, R_LINE);
        g_line_entered = false;
    } else if (addr == 0x787Eu || addr == 0x787Fu) {
        if (addr == 0x787Eu)
            g_cur_hi = data;
        else
            g_cur_lo = data;
        if (g_mode == MODE_EDITING || g_mode == MODE_TYPING) mark_dirty(&g_line_dirty, R_LINE); /* the cursor moved */
    } else if (addr == 0x7880u) {
        /* ENTER: the line settles (tokenized) while still typing, the
         * mode changes after -- report it again as entered. */
        if (data != g_mode) mark_dirty(&g_line_dirty, R_LINE);
        if (data == MODE_MESSAGE) g_msg_pending = true;
        g_mode = data;
    } else if (addr == 0x764Eu || addr == 0x764Fu) {
        if (!g_muted && g_on) {
            g_status[addr - 0x764Eu] = data;
            g_status_valid = true;
        }
        text = false; /* no settling wait for it */
    } else if (addr >= MSG_BASE && addr < MSG_BASE + MSG_LEN && g_msg_pending) {
        g_msg[addr - MSG_BASE] = data;
        mark_dirty(&g_msg_dirty, R_MSG);
    } else {
        text = false;
    }
    if (text && B.now_us) g_last_write_us = B.now_us(B.ctx);
    unlock();
}

void screen_watch_mute(bool mute) {
    lock();
    if (mute && !g_muted) g_out_dirty = g_line_dirty = g_msg_dirty = false;
    g_muted = mute;
    unlock();
}

/* ---- The peer's side ---- */
static struct {
    uint32_t link;    /* the link it's synced to (0: none) */
    bool refused;     /* ...which has no screen */
    uint32_t sent;    /* the next history position to send */
    uint32_t clears;  /* g_clears it's seen */
    uint32_t edit_ver, game_ver;
    bool status_valid;
    uint8_t status[2];
    uint32_t status_us;
} P;

static void peer_reset(void) {
    memset(&P, 0, sizeof P);
}

static void peer_start(uint32_t link) {
    peer_reset();
    P.link = link;
    P.clears = g_clears - 1; /* a CLEAR first */
    P.sent = g_begin;
    while (g_end - P.sent > SCREEN_CATCHUP) P.sent += 2u + HIST(P.sent + 1);
    P.edit_ver = g_edit_ver - 1;
    P.game_ver = g_game_len ? g_game_ver - 1 : g_game_ver; /* the peer starts with none */
}

static bool add_record(uint8_t *p, uint16_t *n, uint16_t max, uint8_t kind, const uint8_t *bytes, uint16_t len) {
    if (*n + 2u + len > max) return false;
    p[*n] = kind;
    p[*n + 1] = (uint8_t)len;
    if (len) memcpy(p + *n + 2, bytes, len);
    *n = (uint16_t)(*n + 2 + len);
    return true;
}

bool screen_poll(void) {
    uint8_t payload[256];
    lock();
    if (!sync_on()) {
        unlock();
        return false;
    }
    if (B.drain) B.drain(B.ctx);
    uint32_t now = B.now_us ? B.now_us(B.ctx) : 0;
    if ((g_out_dirty || g_line_dirty || g_msg_dirty) && now - g_last_write_us >= SETTLE_US) report();
    uint32_t link = B.session ? B.session(B.ctx) : 0;
    if (link != P.link) {
        if (link)
            peer_start(link);
        else
            peer_reset();
    }
    if (!P.link || P.refused) {
        unlock();
        return false;
    }

    uint16_t max = B.display_max ? B.display_max(B.ctx) : 0;
    if (max > sizeof payload) max = sizeof payload;
    uint16_t n = 0;
    uint32_t clears = g_clears;
    if (P.clears != clears && !add_record(payload, &n, max, SCREEN_CLEAR, NULL, 0)) {
        unlock();
        return false;
    }
    uint32_t sent = P.sent;
    if (P.clears != clears || sent < g_begin || sent > g_end) sent = g_begin; /* fell behind the ring */
    while (sent < g_end) {
        uint8_t rec[2 + 255];
        uint16_t len = HIST(sent + 1);
        for (uint16_t i = 0; i < 2u + len; i++) rec[i] = HIST(sent + i);
        if (!add_record(payload, &n, max, rec[0], rec + 2, len)) break;
        sent += 2u + len;
    }
    uint32_t edit_ver = g_edit_ver;
    bool edit = sent == g_end && g_edit_len && P.edit_ver != edit_ver &&
                add_record(payload, &n, max, SCREEN_EDIT, g_edit, g_edit_len);
    uint32_t game_ver = g_game_ver;
    bool game = P.game_ver != game_ver && add_record(payload, &n, max, SCREEN_GAME, g_game, g_game_len);
    uint8_t status[2];
    memcpy(status, g_status, 2);
    bool status_due = g_status_valid && (!P.status_valid || memcmp(status, P.status, 2)) &&
                      (n || !P.status_valid || now - P.status_us >= STATUS_MIN_US);
    bool with_status = status_due && add_record(payload, &n, max, SCREEN_STATUS, status, 2);
    if (!n) {
        unlock();
        return false;
    }

    /* Unlocked while the frame's away: the sources go on meanwhile. */
    unlock();
    int r = B.display(B.ctx, payload, n);
    lock();
    if (P.link == link) {
        if (r == 0) {
            P.refused = true; /* no display: not again on this link */
        } else if (r < 0) {
            P.link = 0; /* failed: from the catch-up again */
        } else {
            P.clears = clears;
            P.sent = sent;
            if (edit) P.edit_ver = edit_ver;
            if (game) P.game_ver = game_ver;
            if (with_status) {
                memcpy(P.status, status, 2);
                P.status_valid = true;
                P.status_us = now;
            }
        }
    }
    unlock();
    return r > 0;
}

bool screen_watching(void) {
    lock();
    bool w = g_on && P.link && !P.refused;
    unlock();
    return w;
}

/* ---- the sources ---- */

void screen_init(const screen_backend_t *backend) {
    B = *backend;
    g_on = false;
    drop_all();
    peer_reset();
    memset(g_out, 0x0D, sizeof g_out);
    memset(g_line, 0x0D, sizeof g_line);
    g_out_dirty = g_line_dirty = g_msg_dirty = false;
    g_msg_pending = g_line_entered = g_muted = false;
    g_mode = g_cur_hi = g_cur_lo = 0;
    g_module_kw_n = 0;
    g_module_kw_loaded = false;
}

uint32_t screen_generation(void) {
    lock();
    uint32_t g = g_gen;
    unlock();
    return g;
}

void screen_line(uint8_t kind, const char *text, size_t len) {
    lock();
    put_line(kind, (const uint8_t *)text, len);
    unlock();
}

void screen_edit(const char *text, size_t len, int cursor) {
    lock();
    put_edit((const uint8_t *)text, len, cursor);
    unlock();
}

void screen_clear(void) {
    lock();
    put_clear();
    unlock();
}

void screen_status(const uint8_t status[2]) {
    lock();
    put_status(status);
    unlock();
}

void screen_game(const uint8_t *bytes, uint8_t len) {
    lock();
    put_game(bytes, len);
    unlock();
}

void screen_records(const uint8_t *p, uint16_t len) {
    lock();
    for (uint16_t at = 0; at + 2u <= len && at + 2u + p[at + 1] <= len; at = (uint16_t)(at + 2 + p[at + 1])) {
        const uint8_t *b = p + at + 2;
        uint8_t n = p[at + 1];
        switch (p[at]) {
        case SCREEN_LINE:
        case SCREEN_OUTPUT: put_line(p[at], b, n); break;
        case SCREEN_EDIT:
            if (n) put_edit(b + 1, n - 1u, (int8_t)b[0]);
            break;
        case SCREEN_CLEAR: put_clear(); break;
        case SCREEN_STATUS:
            if (n >= 2) put_status(b);
            break;
        case SCREEN_GAME: put_game(b, n); break;
        default: break;
        }
    }
    unlock();
}

/* What the watch saw before the module ROM drew over it goes first,
 * settled or not. */
static void report_now(void) {
    if (B.drain) B.drain(B.ctx);
    report();
}

void screen_show(const uint8_t *chars, size_t len) {
    lock();
    report_now();
    if (sync_on()) {
        commit_show(); /* a SHOW before it that nothing else replaced */
        g_show_len = lcd_text(chars, len, -1, g_show, sizeof g_show);
        if (g_show_len) {
            g_show_pending = true;
            set_edit((const uint8_t *)g_show, g_show_len, -1); /* replaced by the next SHOW, if one comes */
        }
    }
    unlock();
}

/* EXP_COMMAND_LIST_SD_DIR's wire format: [count hi][count lo], then count
 * EXP_DIR_RECORD_SIZE-byte records whose first 26 bytes are the line the
 * LCD shows, then the 26-character summary line. */
void screen_listing(const uint8_t *window) {
    uint16_t count = (uint16_t)(window[0] << 8 | window[1]);
    const uint8_t *at = window + 2;
    char text[64];
    lock();
    report_now();
    /* the listing ends before the ROM's scratch at window offset 2038 (rom.asm SD_LIST_SCRATCH_ABS) */
    if (count <= (2038u - 2u - EXP_DIR_SUMMARY_LEN) / EXP_DIR_RECORD_SIZE) {
        for (uint16_t i = 0; i < count; i++, at += EXP_DIR_RECORD_SIZE) {
            size_t n = lcd_text(at, EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN, -1, text, sizeof text);
            if (n) put_line(SCREEN_OUTPUT, (const uint8_t *)text, n);
        }
        size_t n = lcd_text(at, EXP_DIR_SUMMARY_LEN, -1, text, sizeof text);
        if (n) put_line(SCREEN_OUTPUT, (const uint8_t *)text, n);
    }
    unlock();
}
