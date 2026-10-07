/* history_session.c -- see history_session.h.
 *
 * Flash format: FLASH_HISTORY_SIZE of 256-byte pages, one command each:
 * ["H1"][seq, 4 bytes LE][len][len characters], the rest erased. Written in
 * turn round the ring; a page at a sector's start erases that sector first
 * (its 16 oldest commands go). Loaded in seq order on first use. */
#include "history_session.h"

#include <string.h>

#include "cmd_history.h"
#include "flash_layout.h"
#include "hardware/sync.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "mcu_store.h"
#include "pc_exp.h"
#include "pico/time.h"

#define PAGE 256
#define PAGES (FLASH_HISTORY_SIZE / PAGE)
#define PER_SECTOR (FLASH_SECTOR_SIZE / PAGE)
#define HEADER 7
#define IND_SMALL 0x08                   /* 764EH's SMALL: BASIC's own, left as it was */
#define TERM_SAVED_IND 0x1D              /* the ROM's copy of 764EH (rom_defs.inc) */

static hist_t g_hist;
static bool g_loaded;
static uint32_t g_seq;   /* the next command's */
static uint16_t g_page;  /* where it goes */
static hist_ui_t g_ui;
static bool g_terminal;
static char g_original[EXP_HIST_LINE_LEN]; /* the line as it was: CANCEL puts it back */
static uint8_t g_original_len;
static uint8_t g_break_seen;
static uint32_t g_shown_version;

static bool enabled(void) { return mcu_config_get(MCU_CONFIG_HISTORY) != 0; }

static void load(void) {
    static uint32_t seqs[PAGES];
    static uint16_t order[PAGES];
    uint16_t n = 0;
    uint8_t rec[PAGE];
    if (g_loaded) return;
    g_loaded = true;
    hist_init(&g_hist);
    g_seq = 1;
    g_page = 0;
    for (uint16_t p = 0; p < PAGES; p++) {
        if (!mcu_store_read(EXP_STORE_SLOT_HISTORY, (uint32_t)p * PAGE, rec, HEADER) || rec[0] != 'H' || rec[1] != '1')
            continue;
        seqs[p] = (uint32_t)rec[2] | (uint32_t)rec[3] << 8 | (uint32_t)rec[4] << 16 | (uint32_t)rec[5] << 24;
        order[n++] = p;
    }
    for (uint16_t i = 1; i < n; i++) /* oldest first: by seq */
        for (uint16_t j = i; j > 0 && seqs[order[j]] < seqs[order[j - 1]]; j--) {
            uint16_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (uint16_t i = 0; i < n; i++) {
        mcu_store_read(EXP_STORE_SLOT_HISTORY, (uint32_t)order[i] * PAGE, rec, PAGE);
        hist_add(&g_hist, (const char *)rec + HEADER, rec[6] > HIST_LINE_MAX ? HIST_LINE_MAX : rec[6]);
    }
    if (n) {
        g_seq = seqs[order[n - 1]] + 1;
        g_page = (uint16_t)((order[n - 1] + 1) % PAGES);
    }
}

/* The newest command (just added) to flash. */
static void save_newest(void) {
    const hist_entry_t *e = hist_get(&g_hist, 1);
    uint8_t rec[HEADER + HIST_LINE_MAX];
    if (g_page % PER_SECTOR == 0 &&
        !mcu_store_erase_range(EXP_STORE_SLOT_HISTORY, (uint32_t)g_page * PAGE, FLASH_SECTOR_SIZE)) {
        mcu_log_warn("HISTORY erase failed");
        return;
    }
    rec[0] = 'H';
    rec[1] = '1';
    rec[2] = (uint8_t)g_seq;
    rec[3] = (uint8_t)(g_seq >> 8);
    rec[4] = (uint8_t)(g_seq >> 16);
    rec[5] = (uint8_t)(g_seq >> 24);
    rec[6] = e->len;
    memcpy(rec + HEADER, e->text, e->len);
    if (!mcu_store_write(EXP_STORE_SLOT_HISTORY, (uint32_t)g_page * PAGE, rec, (uint32_t)(HEADER + e->len)))
        mcu_log_warn("HISTORY write failed");
    g_seq++;
    g_page = (uint16_t)((g_page + 1) % PAGES);
}

static void add(const char *line, size_t len) {
    if (hist_add(&g_hist, line, len)) save_newest();
}

/* BASIC's line from the window: up to its 0DH. */
static uint8_t line_len(const uint8_t *w) {
    uint8_t n = 0;
    while (n < EXP_HIST_LINE_LEN && w[n] != 0x0D) n++;
    return n;
}

static void publish(uint8_t *w) {
    char line[HIST_WIDTH];
    w[EXP_SSH_TERM_INDICATORS] = (uint8_t)(hist_ui_indicators(&g_ui) | (w[TERM_SAVED_IND] & IND_SMALL));
    if (g_ui.version == g_shown_version) return;
    g_shown_version = g_ui.version;
    hist_ui_render(&g_ui, line);
    memcpy(w + EXP_SSH_TERM_LINE, line, HIST_WIDTH);
    __dmb(); /* the line before its count */
    w[EXP_SSH_TERM_LINE_COUNT]++;
}

uint8_t history_session_command(uint8_t command, uint8_t *w) {
    switch (command) {
        case EXP_COMMAND_HIST_ADD:
            if (enabled()) {
                load();
                add((const char *)w, line_len(w));
            }
            return EXP_STATUS_SUCCESS;
        case EXP_COMMAND_HIST_BEGIN: {
            hist_start_t how = w[0] == EXP_HIST_START_SEARCH ? HIST_START_SEARCH
                               : w[0] == EXP_HIST_START_NEWER ? HIST_START_NEWER
                                                               : HIST_START_OLDER;
            if (!enabled()) return EXP_STATUS_ERROR;
            load();
            g_original_len = line_len(w + 1);
            memcpy(g_original, w + 1, g_original_len);
            if (!hist_ui_start(&g_ui, &g_hist, how)) return EXP_STATUS_ERROR;
            g_break_seen = w[EXP_SSH_TERM_BREAK_COUNT];
            w[EXP_SSH_TERM_KEY] = g_ui.held; /* the arrow that opened it, still down */
            w[EXP_SSH_TERM_CLOSED] = 0;
            g_shown_version = g_ui.version - 1;
            publish(w);
            g_terminal = true;
            return EXP_STATUS_SUCCESS;
        }
        default: return EXP_STATUS_NOT_IMPLEMENTED;
    }
}

bool history_session_terminal(void) { return g_terminal; }

void history_session_poll(uint8_t *w) {
    const hist_entry_t *e;
    uint8_t result;
    if (!g_terminal) return;
    hist_ui_key(&g_ui, w[EXP_SSH_TERM_KEY], to_ms_since_boot(get_absolute_time()));
    if (w[EXP_SSH_TERM_BREAK_COUNT] != g_break_seen) {
        g_break_seen = w[EXP_SSH_TERM_BREAK_COUNT];
        hist_ui_break(&g_ui);
    }
    publish(w);
    if (hist_ui_result(&g_ui) == HIST_UI_OPEN) return;
    /* the line for 7BB0H, and what the ROM does with it */
    memset(w + EXP_HIST_LINE, 0x0D, EXP_HIST_LINE_LEN);
    e = hist_ui_selected(&g_ui);
    if (e) {
        memcpy(w + EXP_HIST_LINE, e->text, e->len);
        result = hist_ui_result(&g_ui) == HIST_UI_RUN ? EXP_HIST_RUN : EXP_HIST_EDIT;
        if (result == EXP_HIST_RUN) {
            char line[HIST_LINE_MAX];
            uint8_t n = e->len;
            memcpy(line, e->text, n); /* hist_add may reuse its slot */
            add(line, n);             /* run again: the newest now, as bash has it */
        }
    } else {
        memcpy(w + EXP_HIST_LINE, g_original, g_original_len);
        result = hist_ui_result(&g_ui) == HIST_UI_BREAK ? EXP_HIST_BREAK : EXP_HIST_CANCEL;
    }
    w[EXP_HIST_LINE_LENGTH] = (uint8_t)(e ? e->len : g_original_len);
    g_terminal = false;
    __dmb(); /* the line before the result: the ROM ends its loop on it */
    w[EXP_SSH_TERM_CLOSED] = result;
}
