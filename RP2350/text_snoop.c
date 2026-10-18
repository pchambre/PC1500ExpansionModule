/* text_snoop.c -- see text_snoop.h. */
#include "text_snoop.h"

#ifdef PC1500_TEXT_SNOOP

#include <stdio.h>
#include <string.h>

#include "basic_detok.h"
#include "ble_link.h"
#include "board_pins.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "mcu_config.h"
#include "monitor.h"
#include "pc_exp.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"
#include "text_snoop.pio.h"

/* 8192 samples (32 KB): covers core1 being busy with a long command for a
 * while even during a LIST. A sample is A0-A12 in bits 0-12, D0-D7 in
 * 13-20. */
#define RING_LOG2 13u
#define RING_N (1u << RING_LOG2)
static uint32_t g_ring[RING_N] __attribute__((aligned(RING_N * sizeof(uint32_t))));

static uint g_dma;
static uint32_t g_rd;

/* ---- What BASIC keeps there (first capture, 2026-10-17, on the user's
 * PC-1500):
 * - 7B60H-7BAFH, the output buffer: PRINT's text, ASCII, padded with 00H
 *   (a number right-aligned, " 1" at column 24).
 * - 7BB0H-7BFFH, the line: typed in ASCII a key at a time; tokenized in
 *   place on ENTER; LIST copies each program line here still tokenized; a
 *   prompt fills it with 0DH and writes '>'.
 * - 7880H, the mode: 00 prompt, 40 typing, 50 editing, 20 a RUN-mode
 *   result shown, 14 a PRO-mode line entered or listed.
 * - 787EH/787FH, the LCD cursor -- see lcd_column().
 * Every change lands in a burst a few ms long at most, so a region is
 * reported once nothing has been written there for SETTLE_US. */
#define OUT_BASE 0x7B60u
#define LINE_BASE 0x7BB0u
#define BUF_LEN 80u
#define MODE_PROMPT 0x00u
#define MODE_TYPING 0x40u
#define MODE_EDITING 0x50u
#define SETTLE_US 20000u

/* - 7A10H-, a message (second capture, BREAK): the mode goes to 01, then
 *   " BREAK IN " and the line number are written here, ending in 00H. */
#define MSG_BASE 0x7A10u
#define MSG_LEN 32u
#define MODE_MESSAGE 0x01u

static uint8_t g_out[BUF_LEN], g_line[BUF_LEN], g_msg[MSG_LEN];
static uint8_t g_cur_hi, g_cur_lo, g_mode;
static bool g_out_dirty, g_line_dirty, g_msg_dirty;
static bool g_msg_pending;  /* the mode went to 01: the next writes at 7A10H are a message */
static bool g_line_entered; /* reported as a LINE, and not written since */
static uint32_t g_last_write_us;

/* ---- The module's own keywords (E1xx), out of the module ROM's keyword
 * table (rom.asm's format: 55H at 8800H, a first-letter index of 26 BE
 * pointers, each at its letter's first entry plus 2; entries `marker (low
 * nibble = name length) | name | code BE | address BE`, ending at a marker
 * with low nibble 0). The index's place moves with rom.asm, so it's found
 * by its pointers' letters. */
#define MAX_MODULE_KW 160
#define MAX_NAME 15
static struct {
    uint16_t code;
    char name[MAX_NAME + 1];
} g_module_kw[MAX_MODULE_KW];
static unsigned g_module_kw_n;

static uint16_t rom_word(uint16_t addr) {
    return (uint16_t)(monitor_rom_image_byte(addr) << 8 | monitor_rom_image_byte((uint16_t)(addr + 1)));
}

static bool index_at(uint16_t at, uint16_t *table) {
    uint16_t lowest = 0xFFFF;
    unsigned used = 0;
    for (unsigned i = 0; i < 26; i++) {
        uint16_t p = rom_word((uint16_t)(at + 2 * i));
        if (!p) continue;
        if (p < 0x8802u || p > 0x9FFFu || monitor_rom_image_byte((uint16_t)(p - 1)) != 'A' + i) return false;
        if (p < lowest) lowest = p;
        used++;
    }
    if (!used) return false;
    *table = (uint16_t)(lowest - 2);
    return true;
}

static void load_module_keywords(void) {
    uint16_t entry = 0;
    for (uint16_t at = 0x8801u; at < 0x8900u && !entry; at++)
        if (!index_at(at, &entry)) entry = 0;
    if (!entry) {
        printf("text snoop: no keyword index in the module ROM image\n");
        return;
    }
    while (g_module_kw_n < MAX_MODULE_KW && entry < 0x9FF0u) {
        uint8_t n = monitor_rom_image_byte(entry) & 0x0Fu;
        if (!n) break;
        uint16_t code = rom_word((uint16_t)(entry + 1 + n));
        if ((code >> 8) == 0xE1) {
            for (uint8_t k = 0; k < n; k++)
                g_module_kw[g_module_kw_n].name[k] = (char)monitor_rom_image_byte((uint16_t)(entry + 1 + k));
            g_module_kw[g_module_kw_n].name[n] = '\0';
            g_module_kw[g_module_kw_n].code = code;
            g_module_kw_n++;
        }
        entry = (uint16_t)(entry + 1 + n + 4);
    }
    printf("text snoop: %u module keywords\n", g_module_kw_n);
}

static const char *module_keyword(uint16_t code) {
    for (unsigned i = 0; i < g_module_kw_n; i++)
        if (g_module_kw[i].code == code) return g_module_kw[i].name;
    return NULL;
}

/* 787EH/787FH -> the LCD column (0-25) the cursor is on. From the capture:
 * 26 characters across four 7-or-6 character groups, 12 apart (6 dot
 * columns, two bytes each): 7400H-7448H, 7506H-7542H, 7600H-7648H,
 * 7706H-7742H. -1 if it's anywhere else. */
static int lcd_column(uint8_t hi, uint8_t lo) {
    static const uint8_t first_col[4] = {0, 7, 13, 20}, start[4] = {0x00, 0x06, 0x00, 0x06},
                         count[4] = {7, 6, 7, 6};
    if (hi < 0x74 || hi > 0x77) return -1;
    unsigned g = hi - 0x74u;
    if (lo < start[g] || (lo - start[g]) % 12u || (lo - start[g]) / 12u >= count[g]) return -1;
    return first_col[g] + (lo - start[g]) / 12;
}

/* ---- To the Link: DISPLAY records (BLE_PROTOCOL.md), `kind len bytes`,
 * as many as fit a frame. Finished lines queue in order; the line being
 * edited is only ever its latest state, sent after them -- and dropped
 * when a finished line comes, which is what it turned into. Nothing is
 * kept while there's no link. */
#define REC_LINE 1
#define REC_OUTPUT 2
#define REC_EDIT 3
#define REC_CLEAR 4 /* BLCLS, or CLS typed directly */
#define REC_STATUS 5 /* the LCD's indicators: 764EH, 764FH */

/* The indicators (ROM1's STATUS1/STATUS2; ASMADDR): 764EH bit 0 BUSY, 1
 * SHIFT, 2 KANA, 3 SML, 4-6 the reserve groups III/II/I; 764FH bit 0 DE,
 * 1 G, 2 RAD, 4 RESERVE, 5 PRO, 6 RUN, 7 DEF. Only the latest pair goes,
 * outside the queue's order, and at most every STATUS_MIN_US on its own
 * -- BUSY flickers with every statement a program runs. */
#define STATUS_MIN_US 100000u
static uint8_t g_status[2], g_status_sent[2];
static bool g_status_sent_valid;
static uint32_t g_status_sent_us;
#define QUEUE_BYTES 8192u /* a long SDLS listing fits */
static uint8_t g_queue[QUEUE_BYTES];
static uint16_t g_queue_len;
static uint8_t g_edit[2 + 1 + BUF_LEN * 2]; /* a whole record */
static uint16_t g_edit_len;
static uint32_t g_queue_dropped;

/* The module ROM's SHOW line (text_snoop_screen_line()): like the LCD, the
 * next SHOW replaces it -- WFCON's PASSWORD: prompt redraws on every key --
 * so it goes as an EDIT until anything else comes, which makes it a line. */
static char g_show[64];
static bool g_show_pending;

static void queue_raw(uint8_t kind, const char *text, int cursor);

static void queue_record(uint8_t kind, const char *text, int cursor) {
    if (g_show_pending) {
        g_show_pending = false;
        queue_raw(REC_OUTPUT, g_show, -1);
    }
    queue_raw(kind, text, cursor);
}

static void queue_raw(uint8_t kind, const char *text, int cursor) {
    size_t n = strlen(text);
    if (n > 200) n = 200;
    if (kind == REC_EDIT) {
        g_edit[0] = kind;
        g_edit[1] = (uint8_t)(n + 1);
        g_edit[2] = (uint8_t)(int8_t)cursor;
        memcpy(g_edit + 3, text, n);
        g_edit_len = (uint16_t)(n + 3);
        return;
    }
    g_edit_len = 0;
    if (g_queue_len + 2 + n > QUEUE_BYTES) {
        g_queue_dropped++;
        return;
    }
    g_queue[g_queue_len] = kind;
    g_queue[g_queue_len + 1] = (uint8_t)n;
    memcpy(g_queue + g_queue_len + 2, text, n);
    g_queue_len = (uint16_t)(g_queue_len + 2 + n);
}

static void report_out(char *text, size_t size, bool usb) {
    /* 00H is padding: blanks, the trailing ones dropped. */
    size_t n = 0;
    for (size_t i = 0; i < BUF_LEN && g_out[i] != 0x0D && n + 1 < size; i++)
        text[n++] = g_out[i] >= 0x20 && g_out[i] < 0x7F ? (char)g_out[i] : ' ';
    while (n && text[n - 1] == ' ') n--;
    text[n] = '\0';
    if (n) {
        queue_record(REC_OUTPUT, text, -1);
        if (usb) printf("[out ] %s\n", text);
    }
}

static void report_line(char *text, size_t size, bool usb) {
    if (g_mode == MODE_PROMPT) return; /* the prompt's own clear */
    basic_detok_line(g_line, BUF_LEN, text, size, module_keyword);
    int col = lcd_column(g_cur_hi, g_cur_lo);
    /* ENTER on a statement that runs on (RUN): the line is tokenized and
     * the cursor leaves the LCD (787EH = 00), but the mode stays 40H
     * until it ends -- so a cursor off the LCD means entered too. */
    if ((g_mode == MODE_TYPING || g_mode == MODE_EDITING) && col >= 0) {
        queue_record(REC_EDIT, text, col);
        if (usb) printf("[%s] %s  @%d\n", g_mode == MODE_EDITING ? "edit" : "type", text, col);
    } else if (!g_line_entered) { /* not again when the mode changes after */
        g_line_entered = true;
        /* CLS typed on its own: the ROM's CLS only resets the cursor
         * (ROM1 LE865H/LECAEH), nothing to see -- clear the pane instead.
         * In a program it leaves no trace; BLCLS does it there. */
        if (g_line[0] == 0xF0 && g_line[1] == 0x88 && g_line[2] == 0x0D) {
            queue_record(REC_CLEAR, "", -1);
            if (usb) printf("[clr ] CLS\n");
            return;
        }
        queue_record(REC_LINE, text, -1);
        if (usb) printf("[line] %s\n", text);
    }
}

static void report_msg(char *text, size_t size, bool usb) {
    g_msg_pending = false;
    size_t n = 0;
    for (size_t i = 0; i < MSG_LEN && g_msg[i] && n + 1 < size; i++)
        text[n++] = g_msg[i] >= 0x20 && g_msg[i] < 0x7F ? (char)g_msg[i] : ' ';
    while (n && text[n - 1] == ' ') n--;
    text[n] = '\0';
    if (n) {
        queue_record(REC_OUTPUT, text, -1);
        if (usb) printf("[msg ] %s\n", text);
    }
}

/* Which region a burst wrote to first: reported in that order, so a line
 * entered comes before the output it starts, even when both settle at once. */
enum { R_OUT, R_LINE, R_MSG, R_COUNT };
static uint32_t g_dirty_order[R_COUNT], g_dirty_count;

static void mark_dirty(bool *flag, unsigned region) {
    if (!*flag) g_dirty_order[region] = ++g_dirty_count;
    *flag = true;
}

static void report(void) {
    char text[200];
    bool usb = stdio_usb_connected();
    bool *flags[R_COUNT] = {&g_out_dirty, &g_line_dirty, &g_msg_dirty};
    for (;;) {
        int next = -1;
        for (int r = 0; r < R_COUNT; r++)
            if (*flags[r] && (next < 0 || g_dirty_order[r] < g_dirty_order[next])) next = r;
        if (next < 0) return;
        *flags[next] = false;
        if (next == R_OUT)
            report_out(text, sizeof(text), usb);
        else if (next == R_LINE)
            report_line(text, sizeof(text), usb);
        else
            report_msg(text, sizeof(text), usb);
    }
}

void text_snoop_init(void) {
    memset(g_out, 0x0D, sizeof(g_out));
    memset(g_line, 0x0D, sizeof(g_line));

    uint offset = pio_add_program(pio2, &text_snoop_program);
    uint sm = pio_claim_unused_sm(pio2, true);
    pio_sm_config c = text_snoop_program_get_default_config(offset);

    gpio_init(PIN_TEXT_TRIG);
    gpio_set_dir(PIN_TEXT_TRIG, GPIO_IN);
    gpio_set_pulls(PIN_TEXT_TRIG, false, true); /* low with no GreenPAK output there */

    sm_config_set_in_pins(&c, 0);               /* GPIO0: A0 */
    sm_config_set_jmp_pin(&c, PIN_TEXT_TRIG);
    sm_config_set_in_shift(&c, false, true, 21); /* shift left: the sample right-justified */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio2, sm, offset, &c);

    /* The ring: the write address wraps at RING_N words; ENDLESS count
     * (RP2350 TRANS_COUNT mode 0xF), so it never stops. */
    g_dma = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(g_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, RING_LOG2 + 2u);
    channel_config_set_dreq(&d, pio_get_dreq(pio2, sm, false));
    dma_channel_configure(g_dma, &d, g_ring, &pio2->rxf[sm], (0xFu << 28) | 1u, true);

    pio_sm_set_enabled(pio2, sm, true);
}

/* The ring into the copy, and records once a burst settles. Also run while
 * a frame waits for its answer (ble_link_set_wait_hook()). */
static bool process(void) {
    uint32_t wr = (((uint32_t)(uintptr_t)dma_channel_hw_addr(g_dma)->write_addr -
                    (uint32_t)(uintptr_t)g_ring) / sizeof(uint32_t)) & (RING_N - 1u);
    bool any = wr != g_rd;
    while (g_rd != wr) {
        uint32_t s = g_ring[g_rd];
        g_rd = (g_rd + 1u) & (RING_N - 1u);
        /* GP2 only fires for 7000H-7FFFH: A12-A14 are 1 there, A15 0. */
        uint16_t addr = (uint16_t)(0x6000u | (s & 0x1FFFu));
        uint8_t data = (uint8_t)(s >> 13);
#ifdef TEXT_SNOOP_ALL
        /* Survey build: every write but the stack's (7800H-784FH). */
        if (addr < 0x7800u || addr >= 0x7850u) printf("W %04X %02X\n", addr, data);
#endif
        if (addr >= OUT_BASE && addr < OUT_BASE + BUF_LEN) {
            g_out[addr - OUT_BASE] = data;
            mark_dirty(&g_out_dirty, R_OUT);
        } else if (addr >= LINE_BASE && addr < LINE_BASE + BUF_LEN) {
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
            g_status[addr - 0x764Eu] = data;
            continue; /* not text: no settling wait for it */
        } else if (addr >= MSG_BASE && addr < MSG_BASE + MSG_LEN && g_msg_pending) {
            g_msg[addr - MSG_BASE] = data;
            mark_dirty(&g_msg_dirty, R_MSG);
        } else {
            continue;
        }
#ifdef TEXT_SNOOP_RAW
        printf("W %04X %02X\n", addr, data);
#endif
        g_last_write_us = time_us_32();
    }
    if ((g_out_dirty || g_line_dirty || g_msg_dirty) && time_us_32() - g_last_write_us >= SETTLE_US) report();
    return any;
}

/* Queued records to the Link, a frame at a time. process() keeps running
 * meanwhile and may add to the queue's end; what went out is taken off its
 * front afterwards. */
static bool status_due(bool with_others) {
    if (g_status_sent_valid && !memcmp(g_status, g_status_sent, sizeof g_status)) return false;
    return with_others || !g_status_sent_valid || time_us_32() - g_status_sent_us >= STATUS_MIN_US;
}

static void flush(void) {
    uint8_t payload[256];
    if (!mcu_config_get(MCU_CONFIG_BLDISP)) { /* MCONF BLDISP=0: no mirroring, nothing kept */
        g_queue_len = 0;
        g_edit_len = 0;
        g_status_sent_valid = false;
        return;
    }
    while (g_queue_len || g_edit_len || status_due(false)) {
        uint16_t max = ble_link_display_max();
        if (max > sizeof(payload)) max = sizeof(payload);
        uint16_t n = 0, taken = 0;
        while (taken < g_queue_len) {
            uint16_t rec = (uint16_t)(2 + g_queue[taken + 1]);
            if (n + rec > max) break;
            memcpy(payload + n, g_queue + taken, rec);
            n = (uint16_t)(n + rec);
            taken = (uint16_t)(taken + rec);
        }
        bool edit = taken == g_queue_len && g_edit_len && n + g_edit_len <= max;
        if (edit) {
            memcpy(payload + n, g_edit, g_edit_len);
            n = (uint16_t)(n + g_edit_len);
        }
        uint8_t status[2];
        bool with_status = status_due(n != 0) && n + 4u <= max;
        if (with_status) {
            memcpy(status, g_status, sizeof status);
            payload[n] = REC_STATUS;
            payload[n + 1] = 2;
            memcpy(payload + n + 2, status, sizeof status);
            n = (uint16_t)(n + 4);
        }
        if (!n) { /* a record bigger than a frame: can't go */
            g_queue_len = 0;
            g_edit_len = 0;
            return;
        }
        uint16_t edit_sent = edit ? g_edit_len : 0;
        int r = ble_link_display(payload, n);
        if (r <= 0) { /* no link, or it failed: start afresh next time */
            g_queue_len = 0;
            g_edit_len = 0;
            g_status_sent_valid = false; /* a new link gets them at once */
            return;
        }
        if (with_status) {
            memcpy(g_status_sent, status, sizeof status);
            g_status_sent_valid = true;
            g_status_sent_us = time_us_32();
        }
        memmove(g_queue, g_queue + taken, g_queue_len - taken);
        g_queue_len = (uint16_t)(g_queue_len - taken);
        /* sent unless a newer one came meanwhile (same length is close enough) */
        if (edit && g_edit_len == edit_sent && !memcmp(g_edit, payload + n - edit_sent, edit_sent)) g_edit_len = 0;
    }
}

static void wait_hook(void) { process(); }

/* `len` LCD characters as text: blanks for anything not printable, the
 * trailing ones dropped. Returns the length. */
static size_t lcd_text(const uint8_t *chars, size_t len, char *text, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < size; i++)
        text[n++] = chars[i] >= 0x20 && chars[i] < 0x7F ? (char)chars[i] : ' ';
    while (n && text[n - 1] == ' ') n--;
    text[n] = '\0';
    return n;
}

static void queue_chars(const uint8_t *chars, size_t len) {
    char text[64];
    if (lcd_text(chars, len, text, sizeof text)) queue_record(REC_OUTPUT, text, -1);
}

void text_snoop_screen_line(const uint8_t *chars, size_t len) {
    process();
    report(); /* what came before it first */
    if (!lcd_text(chars, len, g_show, sizeof g_show)) return;
    g_show_pending = true;
    queue_raw(REC_EDIT, g_show, -1); /* replaced by the next SHOW, if one comes */
}

/* EXP_COMMAND_LIST_SD_DIR's wire format: [count hi][count lo], then count
 * EXP_DIR_RECORD_SIZE-byte records whose first 26 bytes are the line the
 * LCD shows, then the 26-character summary line. */
void text_snoop_listing(const uint8_t *window) {
    uint16_t count = (uint16_t)(window[0] << 8 | window[1]);
    const uint8_t *at = window + 2;
    process();
    report();
    /* the listing ends before the ROM's scratch at window offset 2038 (rom.asm SD_LIST_SCRATCH_ABS) */
    if (count > (2038u - 2u - EXP_DIR_SUMMARY_LEN) / EXP_DIR_RECORD_SIZE) return;
    for (uint16_t i = 0; i < count; i++, at += EXP_DIR_RECORD_SIZE)
        queue_chars(at, EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN);
    queue_chars(at, EXP_DIR_SUMMARY_LEN);
}

void text_snoop_terminal(const char *text, size_t len, int cursor, bool finished) {
    char line[96];
    if (len > sizeof line - 1) len = sizeof line - 1;
    memcpy(line, text, len);
    line[len] = '\0';
    queue_record(finished ? REC_OUTPUT : REC_EDIT, line, finished ? -1 : cursor); /* a blank line stays one */
}

void text_snoop_set_status(const uint8_t status[2]) {
    memcpy(g_status, status, sizeof g_status);
    g_status_sent_valid = false; /* goes with the next flush */
}

void text_snoop_clear(void) {
    process();
    report(); /* not waiting for the burst to settle: it's before BLCLS */
    queue_record(REC_CLEAR, "", -1);
    flush();
}

bool text_snoop_poll(void) {
    static bool started;
    if (!started) {
        started = true; /* here, not in init: core1, with the USB console up */
        load_module_keywords();
        ble_link_set_wait_hook(wait_hook);
    }
    bool any = process();
    flush();
    return any;
}

#endif
