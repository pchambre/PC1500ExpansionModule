/* keywords.c -- see keywords.h.
 *
 * Each keyword's grammar and command sequence is what the LH5801 routines
 * in rom.asm used to do, moved here, with two changes (2026-09-25): every
 * keyword also works inside a running program, and its file names and
 * numbers are BASIC expressions ("X", F$, A*2, &4000) rather than only
 * literals.
 *
 * The statement: the ROM hands over the keyword's token and the bytes that
 * follow it, as BASIC stores them -- in DISP_BUFFER for a typed command, in
 * the program line for a running one. BASIC has already removed every
 * space outside quotes and tokenized its own keywords (detokenize() expands
 * those back); ':' or 0x0D ends the statement.
 *
 * Expressions: the MCU can't evaluate BASIC, so it asks the ROM to
 * (EXP_KW_ACTION_EVAL -- BASIC's own evaluator, which also says where the
 * expression ended). Parsing is replayed from the start after each
 * evaluation, with the results so far cached in order (expr()); every
 * keyword evaluates all of its expressions before it does anything with
 * side effects, so the replay is safe. SDPRINT is the exception: it writes
 * each value as it goes, so it evaluates them one at a time instead
 * (ST_PRINT_VALUE).
 *
 * A keyword either finishes in one EXP_COMMAND_KEYWORD (DONE, ERROR, SHOW
 * of a result, LOAD/SAVE/STAGE, which the ROM finishes by itself) or asks
 * the ROM for something only the LH5801 can do -- an evaluated expression,
 * a Y/N answer, a listing selection, a BASIC variable -- and picks up again
 * on EXP_COMMAND_KEYWORD_CONTINUE. `kw.step` says where.
 */
#include "keywords.h"

#include <stdbool.h>
#include <string.h>

#include "basic_xlate.h"
#include "mcu_config.h"
#include "pc_exp.h"
#include "plotter.h"

#define W_LENGTH_PORT (EXP_LENGTH_PORT_PAGE * 256 + EXP_LENGTH_PORT_ADDRESS)
#define W_SCRATCH (EXP_SCRATCH_PAGE * 256)
#define W_ACTION (EXP_KW_ACTION_PAGE * 256 + EXP_KW_ACTION_ADDRESS)
#define LINE_WIDTH 26 /* one LCD line */
#define CR 0x0D
#define FF 0x0C /* form feed: clears the BLE peer's console */
#define KEY_Y 0x59
#define NAME_SLOT EXP_TWO_NAME_SLOT_LEN /* [len hi][len lo][up to 40 chars] */
#define TOKEN_HIGH 0xE1                 /* this module's token codes: E1xx */

/* Arithmetic register type byte (+4), TRM sec.5-3 */
#define AR_BINARY 0xB2
#define AR_STRING 0xD0

/* Keyword ids: the low byte of each keyword's E1xx token code. */
enum {
    KW_SDMV = 0x80,
    KW_SDLS = 0x85,
    KW_SDSAVE = 0x86,
    KW_SDLOAD = 0x87,
    KW_SDRM = 0x88,
    KW_SDFMT = 0x89,
    KW_SDDF = 0x8A,
    KW_SDCP = 0x8B,
    KW_SDCD = 0x8C,
    KW_SDMKDIR = 0x8D,
    KW_SDRMDIR = 0x8E,
    KW_SDPWD = 0x8F,
    KW_SDOPEN = 0x90,
    KW_SDCLOSE = 0x91,
    KW_SDINPUT = 0x92,
    KW_SDPRINT = 0x93,
    KW_SDSKIP = 0x94,
    KW_STAGE = 0x97,
    KW_MLOG = 0x98,
    KW_MLOGMSG = 0x99,
    KW_MCONF = 0x9A,
    /* 0x9B is FNCLR, which runs in the ROM alone */
    KW_FNSAVE = 0x9C,
    KW_FNLOAD = 0x9D,
    KW_STSAVE = 0x9E,
    KW_STLOAD = 0x9F,
    KW_BLSCAN = 0xA0, /* BLE, BLE_PROTOCOL.md */
    KW_BLCON = 0xA1,
    KW_BLDISC = 0xA2,
    KW_BLPRINT = 0xA3,
    KW_BLLIST = 0xA4,
    KW_BLSAVE = 0xA5,
    KW_BLLOAD = 0xA6,
    KW_BLCLS = 0xA7,
    KW_BLADV = 0xA8, /* peer-to-peer (2026-09-28) */
    KW_BLPUT = 0xA9,
    KW_BLGET = 0xAA,
    KW_BLSEND = 0xAB, /* peer messaging (2026-09-29) */
    KW_BLRECV = 0xAC,
    KW_BLPAIR = 0xAD, /* pairing (2026-10-03, BLE_PROTOCOL.md sec.7) */
    KW_BLUNPAIR = 0xAE,
    KW_BLKBD = 0xAF, /* the external keyboard (2026-10-04) */
    /* BLSTAT is a function (E152): kw_function(), not begin() */
    /* The CE-150 printer/plotter's keywords (2026-09-30, plotter.h), for
     * when no CE-150 is attached. Eight keep the CE-150's own F0xx codes,
     * which BASIC finds on any module's page (the id is the low byte); the
     * other seven are E6xx on the CE-150, a code BASIC only looks for on the
     * CE-150's own page, so here they're E1C0-E1C6 in the same order
     * (E680-E686: CSIZE, GRAPH, GLCURSOR, LCURSOR, SORGN, ROTATE, TEXT). */
    KW_COLOR = 0xB5, /* F0B5 */
    KW_LF = 0xB6,
    KW_LINE = 0xB7,
    KW_LLIST = 0xB8,
    KW_LPRINT = 0xB9,
    KW_RLINE = 0xBA,
    KW_TAB = 0xBB,
    KW_TEST = 0xBC,
    KW_CSIZE = 0xC0, /* E1C0 */
    KW_GRAPH = 0xC1,
    KW_GLCURSOR = 0xC2,
    KW_LCURSOR = 0xC3,
    KW_SORGN = 0xC4,
    KW_ROTATE = 0xC5,
    KW_TEXT = 0xC6,
};
#define TOKEN_CE150 0xF0 /* the high byte of the CE-150's codes kept above */

/* What EXP_COMMAND_KEYWORD_CONTINUE resumes. */
enum {
    ST_NONE,
    ST_EVAL,          /* an expression was evaluated: cache it, parse again */
    ST_FINISH,        /* after a result SHOW: back to BASIC */
    ST_FMT_CONFIRM,   /* SDFMT's Y/N */
    ST_RM_CONFIRM,    /* SDRM's Y/N */
    ST_CPMV_CONFIRM,  /* SDCP/SDMV overwrite Y/N */
    ST_SAVE_CONFIRM,  /* SDSAVE overwrite Y/N */
    ST_LOAD_PICK,     /* SDLOAD browse selection */
    ST_PRINT_VALUE,   /* SDPRINT: next value evaluated */
    ST_INPUT_LOOKUP,  /* SDINPUT: variable looked up */
    ST_INPUT_VAR,     /* SDINPUT: variable stored */
    ST_FN_SAVE_PTR,   /* FNSAVE: BASIC's program-start pointer read */
    ST_FN_SAVE_DATA,  /* FNSAVE: the function keys read */
    ST_FN_LOAD_PTR,   /* FNLOAD: BASIC's program-start pointer read */
    ST_STATE_SAVE,    /* STSAVE: the next 1K of RAM read */
    ST_STATE_LOAD,    /* STLOAD: the next 1K of RAM written */
    ST_STATE_FINAL,   /* STLOAD: 7C00H written, 7800H next (inline, see RESTORE) */
    ST_BL_PICK,       /* BLSCAN: a peer picked from the listing */
    ST_BL_PRINT_VALUE,/* BLPRINT: the next value evaluated */
    ST_BL_LIST_PTRS,  /* BLLIST: BASIC's program start/end pointers read */
    ST_BL_LIST_CHUNK, /* BLLIST: the next piece of the program read */
    ST_ADV_WAIT,      /* BLADV: one POLL done, waiting for a connector */
    ST_PUT_WAIT,      /* BLPUT: one POLL done, waiting for the answer */
    ST_GET_WAIT,      /* BLGET: one POLL done, waiting for an offer */
    ST_GET_CONFIRM,   /* BLGET name: overwrite Y/N */
    ST_PUT_SENDING,   /* BLPUT/BLSAVE: "SENDING..."/"SAVING..." is up, send the bytes */
    ST_GET_RECEIVING, /* BLGET/BLLOAD: "RECEIVING..."/"LOADING..." is up, take them */
    ST_SEND_VALUE,    /* BLSEND: the next value evaluated */
    ST_SEND_RETRY,    /* BLSEND: one POLL done while the peer's inbox is full */
    ST_RECV_WAIT,     /* BLRECV: one POLL done, waiting for a message */
    ST_RECV_LOOKUP,   /* BLRECV: a variable looked up */
    ST_RECV_VAR,      /* BLRECV: a variable stored */
    ST_PAIR_PICK,     /* BLPAIR: a peer picked from the listing */
    ST_PAIR_ANSWER,   /* BLPAIR: the code's Y/N */
    ST_PAIR_WAIT,     /* BLPAIR: one POLL done, waiting for the peer's user */
    ST_UNPAIR_ALL,    /* BLUNPAIR: "forget all" Y/N */
    ST_ADV_PAIR,      /* BLADV: a connector's pairing code's Y/N */
    ST_LINK_ERROR,    /* after a SHOW explaining a link failure: ERROR 40 */
    ST_CE150_ROM,     /* a BASIC load: A000H read, is a CE-150 there? */
    ST_CE150_PAGE,    /* ...and B000H */
    ST_KBD_WAIT,      /* BLKBD: one POLL done, pairing */
};

enum { LOAD_BASIC, LOAD_M_HEADER, LOAD_M_EXPLICIT };
enum { SAVE_BASIC, SAVE_M, PUT_SD }; /* PUT_SD: BLPUT SD, a file off the card */
enum { GET_MEMORY = 3, GET_SD };     /* BLGET's, until the offer's kind is known */

/* Room for the statement with every token expanded (a 2-byte token
 * becomes up to 8 characters). */
#define KW_TEXT_MAX 200
#define MAX_EVALS 16 /* LINE: six points, a line type and a colour */
#define VALUE_TEXT_MAX 80

/* One evaluated expression: where it started and ended in the statement
 * (offsets as BASIC stores it), the arithmetic register, a string's text. */
typedef struct {
    uint8_t start, end;
    bool literal_number;          /* parsed by the MCU itself: `number` holds it */
    uint32_t number;
    uint8_t reg[8];
    uint8_t len;
    uint8_t text[VALUE_TEXT_MAX];
} value_t;

static struct {
    uint8_t id;
    uint8_t step;
    uint8_t line[KW_TEXT_MAX + 1]; /* the statement, detokenized, ending in CR */
    uint8_t raw[KW_TEXT_MAX + 1];  /* each line[] byte's offset as BASIC stores it */
    uint8_t pos;
    bool yflag;
    uint8_t mode;
    uint16_t start, end, call;
    uint8_t channel;
    uint16_t var;
    uint8_t names[2 * NAME_SLOT]; /* staged names, kept across a Y/N prompt */
    value_t evals[MAX_EVALS];     /* this statement's evaluated expressions, in order */
    uint8_t nevals;
    uint8_t next_eval;            /* the next one this parse pass will use */
    bool eval_wanted;             /* this pass stopped at an expression not yet evaluated */
    uint8_t eval_at;
    uint8_t chunk;                /* STSAVE/STLOAD: which 1K of 0000H-7FFFH */
    uint8_t saved_pos[5];         /* STSAVE/STLOAD: KW_TEXT hi/lo, END, S hi/lo */
    uint16_t list_len;            /* BLLIST: bytes in the piece being read */
    bool label_wanted;            /* LLIST "label": not found yet */
    bool label_open;              /* LLIST "label",: on to the end */
    uint8_t label_len;
    uint8_t label[VALUE_TEXT_MAX];
} kw;

static uint8_t *W;
static kw_command_fn run_fn;
static void *run_ctx;

static uint8_t run(uint8_t command) { return run_fn(command, run_ctx); }

/* BLSAVE/BLLOAD share SDSAVE/SDLOAD's code, with the BLE peer in place of the card. */
static bool is_ble(void) { return kw.id >= KW_BLSCAN && kw.id <= KW_BLRECV; }

/* ---- action block ---- */

static uint8_t action(uint8_t act, uint8_t arg, uint16_t a, uint16_t b, uint8_t next) {
    uint8_t *p = W + W_ACTION;
    p[EXP_KW_ACT] = act;
    p[EXP_KW_ARG] = arg;
    p[EXP_KW_A_HI] = (uint8_t)(a >> 8);
    p[EXP_KW_A_LO] = (uint8_t)a;
    p[EXP_KW_B_HI] = (uint8_t)(b >> 8);
    p[EXP_KW_B_LO] = (uint8_t)b;
    kw.step = next;
    return EXP_STATUS_SUCCESS;
}

static uint8_t done(void) { return action(EXP_KW_ACTION_DONE, 0, 0, 0, ST_NONE); }
static uint8_t error(uint8_t n) { return action(EXP_KW_ACTION_ERROR, n, 0, 0, ST_NONE); }

static uint8_t eval_at(uint8_t offset, uint8_t next) {
    kw.eval_at = offset;
    return action(EXP_KW_ACTION_EVAL, 0, offset, 0, next);
}

/* A parse that stopped: either at an expression that still needs
 * evaluating, or at a real syntax error (ERROR 1). */
static uint8_t fail(void) { return kw.eval_wanted ? eval_at(kw.eval_at, ST_EVAL) : error(1); }

/* Same, for SDLOAD, whose malformed arguments have always just returned. */
static uint8_t fail_quietly(void) { return kw.eval_wanted ? eval_at(kw.eval_at, ST_EVAL) : done(); }

/* Shows `len` bytes padded to a full line -- every SHOW overwrites the
 * whole line, so nothing typed earlier stays visible past the text. */
static uint8_t show(const uint8_t *text, uint8_t len, uint8_t next) {
    uint8_t line[LINE_WIDTH];
    if (len > LINE_WIDTH) len = LINE_WIDTH;
    memset(line, ' ', sizeof line);
    memcpy(line, text, len);
    memcpy(W, line, sizeof line);
    return action(EXP_KW_ACTION_SHOW, 0, 0, 0, next);
}

static uint8_t show_str(const char *text, uint8_t next) {
    return show((const uint8_t *)text, (uint8_t)strlen(text), next);
}

static uint8_t browse(uint8_t arg, uint8_t next) { return action(EXP_KW_ACTION_BROWSE, arg, 0, 0, next); }

/* ---- argument parsing ---- */

static uint8_t cur(void) { return kw.line[kw.pos]; }

/* BASIC has already dropped the spaces; this only matters for text the
 * MCU gets some other way. */
static uint8_t skip(void) {
    while (kw.line[kw.pos] == ' ') kw.pos++;
    return kw.line[kw.pos];
}

/* Matches `rest` exactly (use "\r" to require the end of the statement);
 * consumes nothing on a mismatch. */
static bool word(const char *rest) {
    uint8_t start = kw.pos;
    for (; *rest; rest++) {
        if (cur() != (uint8_t)*rest) {
            kw.pos = start;
            return false;
        }
        kw.pos++;
    }
    return true;
}

/* A plain literal -- a quoted string, or a decimal or &hex number followed
 * by ',' or the end of the statement -- is read here with no round trip;
 * it's also the only form that can be followed by a word BASIC doesn't
 * know, like SDOPEN's AS (BASIC's evaluator stops only at its own
 * delimiters, and raises ERROR 1 at "AS"). */
static value_t literal;

static bool literal_arg(const value_t **out) {
    uint8_t p = kw.pos, c = kw.line[p];
    memset(&literal, 0, sizeof literal);
    if (c == '"') {
        for (p++; kw.line[p] != '"'; p++) {
            if (kw.line[p] == CR || literal.len == VALUE_TEXT_MAX) return false;
            literal.text[literal.len++] = kw.line[p];
        }
        p++;
        if (kw.line[p] != ',' && kw.line[p] != CR && !(kw.line[p] == 'A' && kw.line[p + 1] == 'S'))
            return false; /* part of a longer expression, "A"+B$ */
        literal.reg[4] = 0xD0; /* AR_STRING */
        kw.pos = p;
        *out = &literal;
        return true;
    }
    if (c == '&') {
        bool any = false;
        for (p++;; p++) {
            uint8_t d = kw.line[p];
            if (d >= '0' && d <= '9') d = (uint8_t)(d - '0');
            else if (d >= 'A' && d <= 'F') d = (uint8_t)(d - 'A' + 10);
            else break;
            literal.number = (literal.number << 4) | d;
            if (literal.number > 0xFFFF) return false;
            any = true;
        }
        if (!any) return false;
    } else if (c >= '0' && c <= '9') {
        for (; kw.line[p] >= '0' && kw.line[p] <= '9'; p++) {
            literal.number = literal.number * 10 + (uint32_t)(kw.line[p] - '0');
            if (literal.number > 0xFFFF) return false;
        }
    } else {
        return false;
    }
    if (kw.line[p] != ',' && kw.line[p] != CR && !(kw.line[p] == 'A' && kw.line[p + 1] == 'S')) return false;
    literal.literal_number = true;
    kw.pos = p;
    *out = &literal;
    return true;
}

/* The expression starting here: a literal (above), or else evaluated by the
 * ROM. False with kw.eval_wanted set if it hasn't been yet (fail() then
 * asks for it), plain false if there's no expression here at all. A caveat
 * of detokenizing: an expression that starts inside an expanded token
 * (SDOPEN F$,C is fine, but SDOPEN F$ AS C has BASIC store "ASC" as the ASC
 * token) can't be evaluated -- ERROR 1. */
static bool expr(const value_t **out) {
    uint8_t c = skip();
    if (c == CR || c == ',') return false;
    if (literal_arg(out)) return true;
    if (c == '"') { /* an unterminated string is ERROR 1, as before */
        uint8_t p = (uint8_t)(kw.pos + 1);
        while (kw.line[p] != '"' && kw.line[p] != CR) p++;
        if (kw.line[p] == CR) return false;
    }
    if (kw.pos > 0 && kw.raw[kw.pos] == kw.raw[kw.pos - 1]) return false;
    if (kw.next_eval < kw.nevals) {
        const value_t *v = &kw.evals[kw.next_eval++];
        if (v->start != kw.raw[kw.pos]) return false;
        while (kw.line[kw.pos] != CR && kw.raw[kw.pos] < v->end) kw.pos++;
        *out = v;
        return true;
    }
    if (kw.nevals < MAX_EVALS) {
        kw.eval_wanted = true;
        kw.eval_at = kw.raw[kw.pos];
    }
    return false;
}

static bool is_string(const value_t *v) { return v->reg[4] == AR_STRING; }

/* A non-negative whole number up to `max` (a fraction is truncated). */
static bool to_uint(const value_t *v, uint16_t max, uint16_t *out) {
    uint32_t n = 0;
    if (is_string(v)) return false;
    if (v->literal_number) {
        n = v->number;
    } else if (v->reg[4] == AR_BINARY) {
        int16_t b = (int16_t)((v->reg[5] << 8) | v->reg[6]);
        if (b < 0) return false;
        n = (uint32_t)b;
    } else {
        /* decimal: exponent, sign, 10 BCD mantissa digits (TRM sec.5-3-1) */
        int8_t exponent = (int8_t)v->reg[0];
        bool zero = true;
        for (int i = 2; i < 7; i++)
            if (v->reg[i]) zero = false;
        if (!zero) {
            if (v->reg[1] & 0x80) return false;
            if (exponent > 9) return false;
            for (int i = 0; i <= exponent; i++) {
                uint8_t pair = v->reg[2 + i / 2];
                n = n * 10 + ((i & 1) ? (pair & 0x0F) : (pair >> 4));
            }
        }
    }
    if (n > max) return false;
    *out = (uint16_t)n;
    return true;
}

/* A whole number in min..max, of either sign; a fraction is truncated
 * towards 0. */
static bool to_int(const value_t *v, int32_t min, int32_t max, int32_t *out) {
    int32_t n = 0;
    if (is_string(v)) return false;
    if (v->literal_number) {
        n = (int32_t)v->number;
    } else if (v->reg[4] == AR_BINARY) {
        n = (int16_t)((v->reg[5] << 8) | v->reg[6]);
    } else {
        int8_t exponent = (int8_t)v->reg[0];
        if (exponent > 8) return false;
        for (int i = 0; i <= exponent; i++) {
            uint8_t pair = v->reg[2 + i / 2];
            n = n * 10 + ((i & 1) ? (pair & 0x0F) : (pair >> 4));
        }
        if (v->reg[1] & 0x80) n = -n;
    }
    if (n < min || n > max) return false;
    *out = n;
    return true;
}

static bool to_channel(const value_t *v) {
    uint16_t n;
    if (!to_uint(v, EXP_MAX_SD_CHANNELS, &n) || n == 0) return false;
    kw.channel = (uint8_t)n;
    return true;
}

/* A string value as [0][len][chars] at window offset 0, validated and
 * upper-cased by the MCU (EXP_COMMAND_VALIDATE_SD_NAME). */
static bool stage_name(const value_t *v) {
    if (!is_string(v) || v->len == 0 || v->len > EXP_PATH_ARG_LEN) return false;
    W[0] = 0;
    W[1] = v->len;
    memcpy(W + 2, v->text, v->len);
    return run(EXP_COMMAND_VALIDATE_SD_NAME) == EXP_STATUS_SUCCESS;
}

/* A file-name expression, staged at window offset 0. */
static bool name_arg(void) {
    const value_t *v;
    return expr(&v) && stage_name(v);
}

/* "-Y" and then the end of the statement. */
static bool dash_y(void) {
    if (!word("-Y\r")) return false;
    kw.yflag = true;
    return true;
}

/* End of the statement, or ",-Y". */
static bool parse_yflag(void) {
    uint8_t c = skip();
    if (c == CR) return true;
    if (c != ',') return false;
    kw.pos++;
    return dash_y();
}

static bool fold_letter(uint8_t *out) {
    uint8_t c = cur();
    if (c >= 0x61 && c < 0x7B) c = (uint8_t)(c - 0x20);
    if (c < 'A' || c > 'Z') return false;
    *out = c;
    return true;
}

/* 1-2 letters and an optional '$' -> the D461H name code: high byte the
 * first letter, low byte the second letter & 0x1F (0 if none), | 0x20 for
 * a string variable. */
static bool parse_var(uint16_t *code) {
    uint8_t c1, c2, lo = 0;
    skip();
    if (!fold_letter(&c1)) return false;
    kw.pos++;
    if (cur() == '$') {
        lo = 0x20;
        kw.pos++;
    } else if (fold_letter(&c2)) {
        lo = (uint8_t)(c2 & 0x1F);
        kw.pos++;
        if (cur() == '$') {
            lo |= 0x20;
            kw.pos++;
        }
    }
    *code = (uint16_t)((c1 << 8) | lo);
    return true;
}

/* [#]n */
static bool channel_arg(void) {
    const value_t *v;
    if (skip() == '#') kw.pos++;
    return expr(&v) && to_channel(v);
}

/* ---- keywords ---- */

static uint8_t show_scratch_text(uint8_t command) {
    if (run(command) != EXP_STATUS_SUCCESS) return done();
    return show(W + W_SCRATCH + 1, W[W_SCRATCH], ST_FINISH);
}

/* SDCD/SDMKDIR/SDRMDIR name -- the command's own status is ignored. */
static uint8_t one_name_command(uint8_t command) {
    if (!name_arg()) return fail();
    run(command);
    return done();
}

static uint8_t sdrm_remove(void) {
    return run(EXP_COMMAND_REMOVE_SD_FILE) == EXP_STATUS_SUCCESS ? done() : error(40);
}

/* SDRM name[,-Y] */
static uint8_t sdrm(void) {
    if (!name_arg() || !parse_yflag()) return fail();
    if (kw.yflag) return sdrm_remove();
    memcpy(kw.names, W, NAME_SLOT);
    return show_str("DELETE FILE? Y/N", ST_RM_CONFIRM);
}

static uint8_t cpmv_run(void) {
    uint8_t command = kw.id == KW_SDCP ? EXP_COMMAND_COPY_SD_FILE : EXP_COMMAND_MOVE_SD_FILE;
    return run(command) == EXP_STATUS_SUCCESS ? done() : error(40);
}

/* SDCP/SDMV src,dest[,-Y] -- the two names go in the two fixed slots at
 * window offset 0 and NAME_SLOT; asks before overwriting an existing
 * destination unless -Y. */
static uint8_t cpmv(void) {
    if (!name_arg()) return fail();
    memcpy(kw.names, W, NAME_SLOT);
    if (skip() != ',') return fail();
    kw.pos++;
    if (!name_arg()) return fail();
    memmove(W + NAME_SLOT, W, NAME_SLOT);
    memcpy(W, kw.names, NAME_SLOT);
    if (!parse_yflag()) return fail();
    if (run(EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS) == EXP_STATUS_SUCCESS && !kw.yflag) {
        memcpy(kw.names, W, 2 * NAME_SLOT);
        return show_str("FILE EXISTS. OVERWRITE Y/N", ST_CPMV_CONFIRM);
    }
    return cpmv_run();
}

/* Opens the staged name and hands the ROM a LOAD. M files start with a
 * 4-byte header: load address, then CALL address (0 = none). An explicit
 * address (SDLOAD M name,addr) overrides the header's and never calls. */
static uint8_t load_opened(void);
static uint8_t show_line(const char *text, uint8_t next);

/* SDSAVE with no name (2026-09-28) saves as the last BASIC program SDLOAD
 * loaded. Kept as a full path, so a later SDCD doesn't move it (as typed if
 * cwd + name won't fit a name slot), and only in RAM: gone once the MCU
 * loses power, and a bare SDSAVE is then ERROR 1 again. */
static uint8_t last_load[NAME_SLOT];
static bool have_last_load;

/* The name slot at W was just opened by a BASIC SDLOAD: remember it. */
static void remember_load(void) {
    uint8_t name[NAME_SLOT];
    uint16_t n;
    memcpy(name, W, NAME_SLOT);
    n = (uint16_t)((name[0] << 8) | name[1]);
    if (n > 0 && n <= EXP_PATH_ARG_LEN && name[2] != '/' && run(EXP_COMMAND_GET_SD_CWD) == EXP_STATUS_SUCCESS) {
        const uint8_t *cwd = W + W_SCRATCH + 1;
        uint8_t len = W[W_SCRATCH];
        uint8_t sep = (len > 0 && cwd[len - 1] == '/') ? 0 : 1;
        if (len > 0 && len + sep + n <= EXP_PATH_ARG_LEN) {
            uint8_t full[EXP_PATH_ARG_LEN];
            memcpy(full, cwd, len);
            if (sep) full[len] = '/';
            memcpy(full + len + sep, name + 2, n);
            n = (uint16_t)(len + sep + n);
            name[0] = 0;
            name[1] = (uint8_t)n;
            memcpy(name + 2, full, n);
        }
    }
    memcpy(last_load, name, NAME_SLOT);
    have_last_load = true;
}

static uint8_t open_and_load(void) {
    if (run(is_ble() ? EXP_COMMAND_BLE_FILE_GET : EXP_COMMAND_OPEN_SD_FILE_READ) != EXP_STATUS_SUCCESS)
        return error(40);
    if (!is_ble() && kw.mode == LOAD_BASIC) remember_load();
    /* BLE is slower than the card: say so (ST_GET_RECEIVING -> load_opened) */
    if (is_ble()) return show_line("BLLOAD: LOADING...", ST_GET_RECEIVING);
    return load_opened();
}

/* Is a CE-150 attached? Its ROM starts C0H at A000H and has a keyword page
 * (55H) at B000H (PV low, as this module runs): read both, then
 * load_basic(). basic_xlate.h says why a BASIC load needs to know. */
#define CE150_ROM 0xA000
#define CE150_PAGE 0xB000

/* The BASIC program's LOAD, its CE-150 codes made this module's own unless
 * a CE-150 is attached (basic_xlate.h). */
static uint8_t load_basic(bool ce150) {
    basic_xlate_begin(ce150 ? BASIC_XLATE_OFF : BASIC_XLATE_LOAD);
    return action(EXP_KW_ACTION_LOAD, EXP_KW_XFER_BASIC, 0, 0, ST_NONE);
}

/* The LOAD of a file already open for READ_FROM_SD_FILE (the card, or a
 * routed BLE transfer), as kw.mode says. */
static uint8_t load_opened(void) {
    uint16_t target = kw.start, call;
    uint8_t flags;
    if (kw.mode == LOAD_BASIC) return action(EXP_KW_ACTION_COPY_IN, 0, CE150_ROM, 1, ST_CE150_ROM);
    W[W_LENGTH_PORT] = 0;
    W[W_LENGTH_PORT + 1] = 4;
    if (run(EXP_COMMAND_READ_FROM_SD_FILE) != EXP_STATUS_SUCCESS || W[W_LENGTH_PORT + 1] != 4) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        return done();
    }
    call = (uint16_t)((W[2] << 8) | W[3]);
    flags = 0;
    if (kw.mode == LOAD_M_HEADER) {
        target = (uint16_t)((W[0] << 8) | W[1]);
        if (call != 0) flags = EXP_KW_LOAD_CALL;
    }
    return action(EXP_KW_ACTION_LOAD, flags, target, call, ST_NONE);
}

/* An "M" mode flag: M followed by a quote, a comma or the end. BASIC drops
 * the space, so "SDLOAD M F$" would read as the variable MF$ -- write
 * SDLOAD M,F$ for that. */
static bool m_flag(void) {
    uint8_t next;
    if (skip() != 'M') return false;
    next = kw.line[kw.pos + 1];
    if (next != '"' && next != ',' && next != CR) return false;
    kw.pos++;
    if (cur() == ',') kw.pos++;
    return true;
}

/* SDLOAD               browse, load the pick as BASIC
 * SDLOAD name          BASIC
 * SDLOAD M             browse, load the pick using its header
 * SDLOAD M name[,addr] header, or explicit address
 * Anything malformed just returns (no error), as before. */
static uint8_t sdload(void) {
    const value_t *v;
    kw.mode = m_flag() ? LOAD_M_HEADER : LOAD_BASIC;
    if (skip() == CR) {
        if (is_ble()) return error(1); /* BLLOAD needs a name: no remote listing yet */
        run(EXP_COMMAND_LIST_SD_DIR);
        return browse(EXP_KW_BROWSE_PICK_L, ST_LOAD_PICK);
    }
    if (!name_arg()) return fail_quietly();
    if (kw.mode == LOAD_M_HEADER && skip() == ',') {
        kw.pos++;
        if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.start)) return fail_quietly();
        kw.mode = LOAD_M_EXPLICIT;
    }
    return open_and_load();
}

/* The listing entry the user picked, trailing spaces trimmed, not
 * validated (it came from the card). */
static uint8_t load_picked(uint8_t index) {
    const uint8_t *record = W + 2 + (uint16_t)index * EXP_DIR_RECORD_SIZE;
    uint8_t len = 0;
    for (uint8_t i = 0; i < EXP_DIR_NAME_LEN; i++)
        if (record[i] != ' ') len = (uint8_t)(i + 1);
    if (len == 0) return done();
    memmove(W + 2, record, len);
    W[0] = 0;
    W[1] = len;
    return open_and_load();
}

/* BLSAVE's "create": starts a save to the connected peer, which answers
 * EXISTS for a file it already has unless -Y (or the Y/N prompt) said to
 * overwrite. An M file's size is known; a BASIC one is streamed by the ROM
 * and isn't. True to go ahead; false with the keyword's result in *out
 * (the Y/N prompt, or ERROR 40). */
static bool ble_put(uint8_t *out) {
    uint8_t *args = W + EXP_BLE_FILE_ARGS;
    uint32_t size = kw.mode == SAVE_BASIC ? 0xFFFFFFFFu : (uint32_t)(kw.end - kw.start) + 1u + 4u;
    memcpy(W, kw.names, NAME_SLOT);
    args[0] = kw.mode == SAVE_BASIC ? EXP_BLE_KIND_BASIC : EXP_BLE_KIND_M;
    args[1] = kw.yflag ? EXP_BLE_FLAG_OVERWRITE : 0;
    args[2] = (uint8_t)(size >> 24);
    args[3] = (uint8_t)(size >> 16);
    args[4] = (uint8_t)(size >> 8);
    args[5] = (uint8_t)size;
    if (run(EXP_COMMAND_BLE_FILE_PUT) == EXP_STATUS_SUCCESS) return true;
    if (W[0] == EXP_BLE_ERR_EXISTS && !kw.yflag) *out = show_str("FILE EXISTS. OVERWRITE Y/N", ST_SAVE_CONFIRM);
    else *out = error(40);
    return false;
}

static uint8_t save_opened(void);

static uint8_t save_create(void) {
    uint8_t result;
    if (is_ble()) {
        if (!ble_put(&result)) return result;
        return show_line("BLSAVE: SAVING...", ST_PUT_SENDING); /* -> save_opened */
    } else {
        memcpy(W, kw.names, NAME_SLOT);
        if (run(EXP_COMMAND_CREATE_SD_FILE) != EXP_STATUS_SUCCESS) return done();
    }
    return save_opened();
}

/* The SAVE into a file already open for WRITE_TO_SD_FILE (the card, or a
 * routed BLE transfer): the BASIC program, or kw.start..kw.end after the
 * M header. */
static uint8_t save_opened(void) {
    if (kw.mode == SAVE_BASIC) { /* with the CE-150's own codes, always (basic_xlate.h) */
        basic_xlate_begin(BASIC_XLATE_SAVE);
        return action(EXP_KW_ACTION_SAVE, EXP_KW_XFER_BASIC, 0, 0, ST_NONE);
    }
    W[W_LENGTH_PORT] = 0;
    W[W_LENGTH_PORT + 1] = 4;
    W[0] = (uint8_t)(kw.start >> 8);
    W[1] = (uint8_t)kw.start;
    W[2] = (uint8_t)(kw.call >> 8);
    W[3] = (uint8_t)kw.call;
    if (run(EXP_COMMAND_WRITE_TO_SD_FILE) != EXP_STATUS_SUCCESS) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        return is_ble() ? error(40) : done();
    }
    return action(EXP_KW_ACTION_SAVE, 0, kw.start, kw.end, ST_NONE);
}

/* Asks before overwriting an existing file unless -Y. */
static uint8_t create_and_write(void) {
    memcpy(kw.names, W, NAME_SLOT);
    if (is_ble()) return save_create(); /* the peer answers EXISTS itself */
    if (run(EXP_COMMAND_OPEN_SD_FILE_READ) == EXP_STATUS_SUCCESS) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        if (!kw.yflag) return show_str("FILE EXISTS. OVERWRITE Y/N", ST_SAVE_CONFIRM);
    }
    return save_create();
}

/* SDSAVE                                  the BASIC program, as the last
 *                                         one SDLOAD loaded (ERROR 1 if none)
 * SDSAVE name[,-Y]                        the BASIC program
 * SDSAVE M name,start,end[,call][,-Y]     start..end inclusive, with a
 *                                         [start][call] header */
static uint8_t sdsave(void) {
    const value_t *v;
    if (skip() == CR && !is_ble()) {
        if (!have_last_load) return error(1);
        memcpy(W, last_load, NAME_SLOT);
        kw.mode = SAVE_BASIC;
        return create_and_write(); /* asks before overwriting, as it will be there */
    }
    if (!m_flag()) {
        if (!name_arg() || !parse_yflag()) return fail();
        kw.mode = SAVE_BASIC;
        return create_and_write();
    }
    if (!name_arg() || skip() != ',') return fail();
    kw.pos++;
    if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.start) || skip() != ',') return fail();
    kw.pos++;
    if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.end) || kw.end < kw.start) return fail();
    kw.call = 0;
    if (skip() == ',') {
        kw.pos++;
        if (cur() == '-') {
            if (!dash_y()) return fail();
        } else if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.call) || !parse_yflag()) {
            return fail();
        }
    } else if (cur() != CR) {
        return fail();
    }
    kw.mode = SAVE_M;
    return create_and_write();
}

/* SDOPEN              browse the open channels
 * SDOPEN name AS n    (or name,n -- the only form for a name that isn't a
 *                     quoted literal, see literal_arg()) */
static uint8_t sdopen(void) {
    const value_t *v;
    if (skip() == CR) {
        run(EXP_COMMAND_SD_LIST_CHANNELS);
        return browse(0, ST_NONE);
    }
    if (!name_arg()) return fail();
    if (!word("AS")) {
        if (skip() != ',') return fail();
        kw.pos++;
    }
    if (!expr(&v) || !to_channel(v)) return fail();
    memmove(W + 1, W, NAME_SLOT);
    W[0] = kw.channel;
    return run(EXP_COMMAND_SD_OPEN_CHANNEL) == EXP_STATUS_SUCCESS ? done() : error(40);
}

/* SDCLOSE n | SDCLOSE ALL */
static uint8_t sdclose(void) {
    const value_t *v;
    if (word("ALL")) {
        kw.channel = 0;
    } else if (!expr(&v) || !to_channel(v)) {
        return fail();
    }
    W[0] = kw.channel;
    run(EXP_COMMAND_SD_CLOSE_CHANNEL);
    return done();
}

/* ---- BASIC variables and values ----
 *
 * VAR_LOOKUP leaves the variable's D461H type byte at window offset 0
 * (bit 7 set = number; clear = string, low 7 bits = capacity) and its raw
 * storage after it: a number is 8 bytes of packed-BCD float (TRM
 * sec.5-3-1, the arithmetic register's own format), a string is inline
 * ASCII zero-padded to its capacity, with no length field (confirmed live)
 * -- its length is up to the first 0x00. SD files and the MCU log hold
 * value chunks instead ('N' + the 8 bytes, or 'S' + length + characters --
 * see pc_exp.h). */

static uint8_t var_size(uint8_t type) { return (type & 0x80) ? 8 : (uint8_t)(type & 0x7F); }

/* SD_READ_VALUE's chunk at window offset 0 -> raw storage for a variable
 * of `type`, at offset 0. False (ERROR 42) if the chunk's type doesn't
 * match or a string is longer than the capacity -- only possible from a
 * corrupted or hand-made file. */
static bool chunk_to_storage(uint8_t type) {
    uint8_t len;
    if (W[0] == 'N') {
        if (!(type & 0x80)) return false;
        memmove(W, W + 1, 8);
        return true;
    }
    if (W[0] != 'S' || (type & 0x80)) return false;
    len = W[1];
    if (len > var_size(type)) return false;
    memmove(W, W + 2, len);
    memset(W + len, 0, (size_t)(var_size(type) - len));
    return true;
}

/* A 16-bit signed integer as an 8-byte decimal (TRM sec.5-3-1). */
static void int_to_decimal(int16_t value, uint8_t out[8]) {
    uint32_t n = (uint32_t)(value < 0 ? -(int32_t)value : value);
    uint8_t digits[5], count = 0;
    memset(out, 0, 8);
    if (n == 0) return;
    while (n) {
        digits[count++] = (uint8_t)(n % 10);
        n /= 10;
    }
    out[0] = (uint8_t)(count - 1);
    out[1] = value < 0 ? 0x80 : 0x00;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t d = digits[count - 1 - i];
        out[2 + i / 2] |= (i & 1) ? d : (uint8_t)(d << 4);
    }
}

/* An EVAL result in the window -> a value chunk at window offset 1. */
static void result_to_chunk(void) {
    if (W[4] == AR_STRING) {
        uint8_t len = W[7];
        if (len > VALUE_TEXT_MAX) len = VALUE_TEXT_MAX;
        for (uint8_t i = 0; i < len; i++)
            if (W[8 + i] == 0) len = i;
        memmove(W + 3, W + 8, len);
        W[1] = 'S';
        W[2] = len;
    } else {
        uint8_t reg[8];
        if (W[4] == AR_BINARY) int_to_decimal((int16_t)((W[5] << 8) | W[6]), reg);
        else memcpy(reg, W, 8);
        memcpy(W + 2, reg, 8);
        W[1] = 'N';
    }
}

/* SDPRINT [#]n,value[,value...] -- any BASIC expression, number or string,
 * written to the file as it's evaluated. */
static uint8_t print_next(void) {
    if (skip() != ',') return error(1);
    kw.pos++;
    if (skip() == CR || cur() == ',') return error(1);
    return eval_at(kw.raw[kw.pos], ST_PRINT_VALUE);
}

static uint8_t print_value(void) {
    uint8_t end = W[W_ACTION + EXP_KW_B_LO];
    result_to_chunk();
    W[0] = kw.channel;
    if (run(EXP_COMMAND_SD_WRITE_VALUE) != EXP_STATUS_SUCCESS) return error(40);
    while (cur() != CR && kw.raw[kw.pos] < end) kw.pos++;
    return skip() == ',' ? print_next() : done();
}

/* SDINPUT [#]n,var[,var...] -- one variable per round trip; a malformed
 * later variable is ERROR 1 after the earlier ones are done. */
static uint8_t input_next(void) {
    if (skip() != ',') return error(1);
    kw.pos++;
    if (!parse_var(&kw.var)) return error(1);
    return action(EXP_KW_ACTION_VAR_LOOKUP, 0, kw.var, 0, ST_INPUT_LOOKUP);
}

/* Past the end of the file the variable becomes 0 / blank. */
static uint8_t input_looked_up(void) {
    uint8_t type = W[0], status;
    W[0] = kw.channel;
    status = run(EXP_COMMAND_SD_READ_VALUE);
    if (status == EXP_STATUS_EOF) {
        memset(W, 0, var_size(type));
    } else if (status != EXP_STATUS_SUCCESS) {
        return error(40);
    } else if (!chunk_to_storage(type)) {
        return error(42);
    }
    return action(EXP_KW_ACTION_VAR_STORE, 0, 0, 0, ST_INPUT_VAR);
}

static uint8_t sdinput_sdprint(void) {
    if (!channel_arg()) return fail();
    return kw.id == KW_SDPRINT ? print_next() : input_next();
}

/* SDSKIP [#]n,count */
static uint8_t sdskip(void) {
    const value_t *v;
    uint16_t n;
    if (!channel_arg() || skip() != ',') return fail();
    kw.pos++;
    if (!expr(&v) || !to_uint(v, 0xFFFF, &n)) return fail();
    W[0] = kw.channel;
    W[1] = (uint8_t)(n >> 8);
    W[2] = (uint8_t)n;
    return run(EXP_COMMAND_SD_SKIP_VALUES) == EXP_STATUS_SUCCESS ? done() : error(40);
}

/* STAGE | STAGE RAM | STAGE DEBUG | STAGE MCU */
static uint8_t stage(void) {
    if (skip() == CR) {
        if (run(EXP_COMMAND_ROM_GET_MODE) != EXP_STATUS_SUCCESS) return show_str("STAGE: MODE UNKNOWN", ST_FINISH);
        return show_str(W[0] == 1 ? "STAGE: RAM" : "STAGE: MCU", ST_FINISH);
    }
    if (word("RAM\r")) {
        /* Always copies (2026-09-29, the board owner's request): with a copy
         * already staged this refreshes it -- after a firmware update the
         * SRAM still holds the old ROM -- in one step, no STAGE MCU first.
         * Safe while Remap is on: the copy runs from the data window, which
         * the MCU has just restored from its own (new) image, and doesn't
         * touch 0x8800+ until the copy is verified. What does run from the
         * OLD image is its KW_STAGE, which jumps to the new window's routine:
         * so STAGE_COPY_ROUTINE_ABS, STAGE_DEBUG_FLAG and STAGE_BOOT_FLAG
         * must keep their addresses in rom.asm. (The boot hook still skips
         * a verified copy; only STAGE RAM typed or in a program refreshes.) */
        return action(EXP_KW_ACTION_STAGE, 0, 0, 0, ST_NONE);
    }
    if (word("DEBUG\r")) return action(EXP_KW_ACTION_STAGE, 1, 0, 0, ST_NONE);
    if (word("MCU\r")) {
        run(EXP_COMMAND_ROM_FROM_MCU);
        return show_str("STAGE: MCU", ST_FINISH);
    }
    return error(1);
}

/* MLOG | MLOG VIEW | MLOG VERBOSE | MLOG QUIET | MLOG RESET */
static uint8_t mlog(void) {
    if (skip() == CR) {
        run(EXP_COMMAND_LOG_GET_INFO_ENABLED);
        return show_str(W[0] ? "MLOG: VERBOSE" : "MLOG: QUIET", ST_FINISH);
    }
    if (word("VIEW\r")) {
        run(EXP_COMMAND_LOG_LIST);
        return browse(0, ST_NONE);
    }
    if (word("VERBOSE\r") || word("QUIET\r")) {
        W[0] = kw.line[0] == 'V';
        run(EXP_COMMAND_LOG_SET_INFO_ENABLED);
        return done();
    }
    if (word("RESET\r")) {
        run(EXP_COMMAND_LOG_CLEAR);
        return done();
    }
    return error(1);
}

/* MLOGMSG text -- any string expression; the log command takes a string
 * chunk ('S', length, characters) at window offset 1. */
static uint8_t mlogmsg(void) {
    const value_t *v;
    if (!expr(&v) || !is_string(v) || skip() != CR) return fail();
    if (v->len == 0) return error(1);
    W[1] = 'S';
    W[2] = v->len;
    memcpy(W + 3, v->text, v->len);
    run(EXP_COMMAND_LOG_USER_MESSAGE);
    return done();
}

/* ---- MCONF ---- */

/* Setting names as typed, with their MCU_CONFIG_* numbers (mcu_config.h).
 * The functions below take an index into this table. */
static const struct {
    uint8_t id;
    const char *name;
    uint16_t max;
} kSettings[] = {
    {MCU_CONFIG_LED, "LED", 1},
    {MCU_CONFIG_SLEEPWAIT, "SLEEPWAIT", 60000},
    {MCU_CONFIG_LOGSIZE, "LOGSIZE", 65535}, /* KB; the MCU checks the real range (multiple of 4,
                                               8 up to what fits in its flash) and starts a fresh log */
    {MCU_CONFIG_AUTOSTAGE, "AUTOSTAGE", 1}, /* STAGE RAM at power-on/reset */
    {MCU_CONFIG_BLKBD, "BLKBD", 1},         /* the external keyboard's driver at power-on/reset */
};
#define SETTING_COUNT (sizeof kSettings / sizeof kSettings[0])

static bool config_get(uint8_t id, uint16_t *value) {
    W[0] = kSettings[id].id;
    if (run(EXP_COMMAND_CONFIG_GET) != EXP_STATUS_SUCCESS) return false;
    *value = (uint16_t)((W[1] << 8) | W[2]);
    return true;
}

/* "NAME=value" into `out`; returns its length. */
static uint8_t format_setting(uint8_t id, uint16_t value, uint8_t *out) {
    char digits[6];
    uint8_t len = (uint8_t)strlen(kSettings[id].name), n = 0;
    memcpy(out, kSettings[id].name, len);
    out[len++] = '=';
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) out[len++] = (uint8_t)digits[--n];
    return len;
}

/* MCONF HOSTNAME (2026-09-28): the BLE name, text rather than a number
 * (mcu_config.h). "HOSTNAME=name" into `out`, or 0 if the MCU can't say. */
static uint8_t hostname_line(uint8_t *out) {
    uint8_t len;
    if (run(EXP_COMMAND_CONFIG_HOSTNAME_GET) != EXP_STATUS_SUCCESS) return 0;
    len = W[0] > LINE_WIDTH - 9 ? LINE_WIDTH - 9 : W[0];
    memmove(out + 9, W + 1, len);
    memcpy(out, "HOSTNAME=", 9);
    return (uint8_t)(9 + len);
}

/* MCONF                      browse every setting
 * MCONF NAME                 show one
 * MCONF NAME=value           set one (saved in the MCU's flash)
 * MCONF HOSTNAME="name"      the BLE name, 1-15 characters */
static uint8_t mconf(void) {
    const value_t *v;
    uint8_t id, text[LINE_WIDTH];
    uint16_t value;
    if (skip() == CR) {
        /* A listing in LIST_SD_DIR's shape: count, 30-byte records (the
         * first 26 bytes are the displayed line), then a summary line. */
        uint8_t line[LINE_WIDTH], count = 0, host[LINE_WIDTH], host_len;
        uint16_t values[SETTING_COUNT];
        bool have[SETTING_COUNT];
        /* all reads first: they use the window's first bytes */
        for (id = 0; id < SETTING_COUNT; id++) have[id] = config_get(id, &values[id]);
        host_len = hostname_line(host);
        for (id = 0; id <= SETTING_COUNT; id++) {
            if (id == SETTING_COUNT) { /* HOSTNAME last */
                uint8_t *record = W + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE;
                if (host_len == 0) break;
                memset(record, 0, EXP_DIR_RECORD_SIZE);
                memset(record, ' ', LINE_WIDTH);
                memcpy(record, host, host_len);
                count++;
                break;
            }
            uint8_t *record = W + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE;
            if (!have[id]) continue;
            memset(line, ' ', sizeof line);
            format_setting(id, values[id], line);
            memset(record, 0, EXP_DIR_RECORD_SIZE);
            memcpy(record, line, sizeof line);
            count++;
        }
        W[0] = 0;
        W[1] = count;
        memset(W + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE, ' ', EXP_DIR_SUMMARY_LEN);
        memcpy(W + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE, "MCONF NAME=VALUE TO SET", 23);
        return browse(0, ST_NONE);
    }
    for (id = 0; id < SETTING_COUNT; id++) {
        uint8_t start = kw.pos;
        if (word(kSettings[id].name) && (cur() == '=' || cur() == CR)) break;
        kw.pos = start;
    }
    if (id == SETTING_COUNT) {
        if (!word("HOSTNAME") || (cur() != '=' && cur() != CR)) return error(1);
        if (cur() == CR) {
            uint8_t len = hostname_line(text);
            return len ? show(text, len, ST_FINISH) : error(1);
        }
        kw.pos++; /* '=' */
        if (!expr(&v) || !is_string(v) || skip() != CR) return fail();
        W[0] = v->len;
        memcpy(W + 1, v->text, v->len);
        return run(EXP_COMMAND_CONFIG_HOSTNAME_SET) == EXP_STATUS_SUCCESS ? done() : error(1);
    }
    if (cur() == CR) {
        if (!config_get(id, &value)) return error(1);
        return show(text, format_setting(id, value, text), ST_FINISH);
    }
    kw.pos++; /* '=' */
    if (!expr(&v) || !to_uint(v, kSettings[id].max, &value) || skip() != CR) return fail();
    W[0] = kSettings[id].id;
    W[1] = (uint8_t)(value >> 8);
    W[2] = (uint8_t)value;
    if (run(EXP_COMMAND_CONFIG_SET) != EXP_STATUS_SUCCESS) return error(1);
    if (kSettings[id].id == MCU_CONFIG_BLKBD && value == 0) {
        /* The keyboard's driver off at once: 79D4H = 0 (rom.asm KBD_ARM)
         * hands KEYSCAN_WAIT back to ROM1's own loop -- the driver would
         * otherwise go on reading a window byte nothing keeps up now (and a
         * sleeping MCU's window doesn't read as 0). */
        W[0] = 0;
        return action(EXP_KW_ACTION_COPY_OUT, 0, 0x79D4, 1, ST_FINISH);
    }
    return done();
}

/* ---- FNSAVE / FNLOAD / STSAVE / STLOAD ----
 *
 * Kept in the MCU's flash (mcu_store.h), one set of each. A store starts
 * with a header page -- a magic and, for the state, where to resume -- and
 * its data follows at offset 256. The header is written last, so a save
 * that fails part-way leaves no valid store. */

#define STORE_DATA 256
#define FN_KEYS_LEN 195 /* ending just before the BASIC program (FNCLR in rom.asm) */
#define BASIC_START_PTR 0x7865
#define STATE_CHUNK 1024
#define STATE_FINAL_A 30  /* 7800H: the stack page -- and 7C00H, which */
#define STATE_FINAL_B 31  /* mirrors it on a base PC-1500: both via RESTORE */

static bool store(uint8_t command, uint8_t slot, uint16_t offset, uint16_t len) {
    uint8_t *p = W + EXP_STORE_PARAMS;
    p[0] = slot;
    p[1] = (uint8_t)(offset >> 8);
    p[2] = (uint8_t)offset;
    p[3] = (uint8_t)(len >> 8);
    p[4] = (uint8_t)len;
    return run(command) == EXP_STATUS_SUCCESS;
}

static uint8_t copy_in(uint16_t addr, uint16_t len, uint8_t next) {
    return action(EXP_KW_ACTION_COPY_IN, 0, addr, len, next);
}

static uint8_t copy_out(uint16_t addr, uint16_t len, uint8_t next) {
    return action(EXP_KW_ACTION_COPY_OUT, 0, addr, len, next);
}

/* Writes the 16-byte header -- `magic`, then whatever the caller put at
 * window offset 4 -- at the start of the store. */
static bool write_header(uint8_t slot, const char *magic) {
    memcpy(W, magic, 4);
    return store(EXP_COMMAND_STORE_WRITE, slot, 0, 16);
}

/* Reads a store's header into the window; false if there's no valid one. */
static bool read_header(uint8_t slot, const char *magic) {
    return store(EXP_COMMAND_STORE_READ, slot, 0, 16) && memcmp(W, magic, 4) == 0;
}

static uint16_t basic_start(void) { return (uint16_t)((W[0] << 8) | W[1]); }

/* FNSAVE: the 195 bytes of function-key definitions (with the reserve
 * pointers), ending just before the BASIC program (whose start is at
 * 7865H). */
static uint8_t fn_save_data(void) {
    if (!store(EXP_COMMAND_STORE_ERASE, EXP_STORE_SLOT_FNKEYS, 0, 0)) return error(40);
    if (!store(EXP_COMMAND_STORE_WRITE, EXP_STORE_SLOT_FNKEYS, STORE_DATA, FN_KEYS_LEN)) return error(40);
    memset(W + 4, 0, 12);
    if (!write_header(EXP_STORE_SLOT_FNKEYS, "FNK1")) return error(40);
    return done();
}

/* FNLOAD: back to the same place relative to the program start (which may
 * have moved since, with different RAM fitted). ERROR 40 if nothing was
 * saved. */
static uint8_t fnload(void) {
    if (!read_header(EXP_STORE_SLOT_FNKEYS, "FNK1")) return error(40);
    return copy_in(BASIC_START_PTR, 2, ST_FN_LOAD_PTR);
}

static uint8_t fn_load_data(void) {
    uint16_t start = basic_start();
    if (!store(EXP_COMMAND_STORE_READ, EXP_STORE_SLOT_FNKEYS, STORE_DATA, FN_KEYS_LEN)) return error(40);
    return copy_out((uint16_t)(start - FN_KEYS_LEN), FN_KEYS_LEN, ST_FINISH);
}

/* STSAVE: all of 0000H-7FFFH, 1K at a time, plus where this statement is
 * and the stack pointer (the ROM's KW_START left both in the action block),
 * so STLOAD can resume right after it. */
static uint8_t stsave(void) {
    const uint8_t *a = W + W_ACTION;
    kw.saved_pos[0] = a[EXP_KW_TEXT_HI];
    kw.saved_pos[1] = a[EXP_KW_TEXT_LO];
    kw.saved_pos[2] = a[EXP_KW_END];
    kw.saved_pos[3] = a[EXP_KW_S_HI];
    kw.saved_pos[4] = a[EXP_KW_S_LO];
    if (!store(EXP_COMMAND_STORE_ERASE, EXP_STORE_SLOT_STATE, 0, 0)) return error(40);
    kw.chunk = 0;
    return copy_in(0, STATE_CHUNK, ST_STATE_SAVE);
}

static uint8_t state_saved_chunk(void) {
    if (!store(EXP_COMMAND_STORE_WRITE, EXP_STORE_SLOT_STATE, (uint16_t)(STORE_DATA + kw.chunk * STATE_CHUNK),
               STATE_CHUNK))
        return error(40);
    if (++kw.chunk <= STATE_FINAL_B) return copy_in((uint16_t)(kw.chunk * STATE_CHUNK), STATE_CHUNK, ST_STATE_SAVE);
    memset(W + 4, 0, 12);
    memcpy(W + 4, kw.saved_pos, sizeof kw.saved_pos);
    if (!write_header(EXP_STORE_SLOT_STATE, "STA1")) return error(40);
    return done();
}

/* A stored chunk into the window. */
static bool read_chunk(uint8_t chunk) {
    return store(EXP_COMMAND_STORE_READ, EXP_STORE_SLOT_STATE, (uint16_t)(STORE_DATA + chunk * STATE_CHUNK),
                 STATE_CHUNK);
}

/* STLOAD: 0000H-77FFH with ordinary copies, then the stack page (7C00H and
 * 7800H) by the ROM's RESTORE, which then resumes right after the STSAVE
 * that made the state -- in its program, if it ran in one. ERROR 40 if
 * nothing was saved. */
static uint8_t stload(void) {
    if (!read_header(EXP_STORE_SLOT_STATE, "STA1")) return error(40);
    memcpy(kw.saved_pos, W + 4, sizeof kw.saved_pos);
    kw.chunk = 0;
    if (!read_chunk(0)) return error(40);
    return copy_out(0, STATE_CHUNK, ST_STATE_LOAD);
}

static uint8_t state_loaded_chunk(void) {
    uint8_t *a = W + W_ACTION;
    if (++kw.chunk < STATE_FINAL_A) {
        if (!read_chunk(kw.chunk)) return error(40);
        return copy_out((uint16_t)(kw.chunk * STATE_CHUNK), STATE_CHUNK, ST_STATE_LOAD);
    }
    /* STSAVE's own statement position and stack pointer, for RESTORE and
     * the KEYWORD_RETURN it ends with */
    a[EXP_KW_TEXT_HI] = kw.saved_pos[0];
    a[EXP_KW_TEXT_LO] = kw.saved_pos[1];
    a[EXP_KW_END] = kw.saved_pos[2];
    a[EXP_KW_S_HI] = kw.saved_pos[3];
    a[EXP_KW_S_LO] = kw.saved_pos[4];
    if (!read_chunk(STATE_FINAL_B)) return error(40);
    return action(EXP_KW_ACTION_RESTORE, 1, STATE_FINAL_B * STATE_CHUNK, STATE_CHUNK, ST_STATE_FINAL);
}

/* The last block. Nothing can be reported from here: the ROM is in
 * RESTORE with interrupts off, past the point of no return. */
static uint8_t state_final(void) {
    read_chunk(STATE_FINAL_A);
    return action(EXP_KW_ACTION_RESTORE, 0, STATE_FINAL_A * STATE_CHUNK, STATE_CHUNK, ST_NONE);
}

/* Parses the statement from the start (again, after each evaluation). */
/* BLE keywords -- defined after the detokenizer, whose token table
 * BLLIST uses. */
static uint8_t blscan(void);
static uint8_t blconnect(void);
static uint8_t blprint(void);
static uint8_t blcls(void);
static uint8_t bllist(void);
static uint8_t bl_pick(uint8_t index);
static uint8_t bl_print_value(void);
static uint8_t bl_list_ptrs(void);
static uint8_t bl_list_chunk(void);
static uint8_t bladv(void);
static uint8_t blput(void);
static uint8_t blget(void);
static uint8_t adv_waited(uint8_t brk);
static uint8_t put_waited(uint8_t brk);
static uint8_t get_waited(uint8_t brk);
static uint8_t get_start(void);
static uint8_t put_send(void);
static uint8_t get_receive(void);
static uint8_t blsend(void);
static uint8_t blrecv(void);
static uint8_t send_value(void);
static uint8_t send_try(bool first);
static uint8_t recv_try(bool first);
static uint8_t recv_looked_up(void);
static uint8_t recv_stored(void);
static uint8_t plotter_keyword(void); /* the CE-150's, after the BLE ones */
static uint8_t blpair(void);
static uint8_t blunpair(void);
static uint8_t pair_begun(uint8_t status);
static uint8_t pair_confirm(bool first);
static uint8_t adv_pair_answered(uint8_t answer);
static uint8_t blkbd(void);
static uint8_t kbd_waited(uint8_t brk, bool first);

static uint8_t begin(void) {
    kw.pos = 0;
    kw.yflag = false;
    kw.step = ST_NONE;
    kw.next_eval = 0;
    kw.eval_wanted = false;
    switch (kw.id) {
        case KW_SDDF: return show_scratch_text(EXP_COMMAND_GET_SD_DF_TEXT);
        case KW_SDPWD: return show_scratch_text(EXP_COMMAND_GET_SD_CWD);
        case KW_SDLS:
            run(EXP_COMMAND_LIST_SD_DIR);
            return browse(0, ST_NONE);
        case KW_SDFMT: return show_str("FORMAT SD CARD? Y/N", ST_FMT_CONFIRM);
        case KW_SDCD: return one_name_command(EXP_COMMAND_CHANGE_SD_DIR);
        case KW_SDMKDIR: return one_name_command(EXP_COMMAND_MAKE_SD_DIR);
        case KW_SDRMDIR: return one_name_command(EXP_COMMAND_REMOVE_SD_DIR);
        case KW_SDRM: return sdrm();
        case KW_SDCP:
        case KW_SDMV: return cpmv();
        case KW_SDLOAD: return sdload();
        case KW_SDSAVE: return sdsave();
        case KW_SDOPEN: return sdopen();
        case KW_SDCLOSE: return sdclose();
        case KW_SDINPUT:
        case KW_SDPRINT: return sdinput_sdprint();
        case KW_SDSKIP: return sdskip();
        case KW_STAGE: return stage();
        case KW_MLOG: return mlog();
        case KW_MLOGMSG: return mlogmsg();
        case KW_MCONF: return mconf();
        case KW_FNSAVE: return copy_in(BASIC_START_PTR, 2, ST_FN_SAVE_PTR);
        case KW_FNLOAD: return fnload();
        case KW_STSAVE: return stsave();
        case KW_STLOAD: return stload();
        case KW_BLSCAN: return blscan();
        case KW_BLCON: return blconnect();
        case KW_BLDISC:
            run(EXP_COMMAND_BLE_DISCONNECT);
            return done();
        case KW_BLPRINT: return blprint();
        case KW_BLCLS: return blcls();
        case KW_BLLIST: return bllist();
        case KW_BLSAVE: return sdsave();
        case KW_BLLOAD: return sdload();
        case KW_BLADV: return bladv();
        case KW_BLPUT: return blput();
        case KW_BLGET: return blget();
        case KW_BLSEND: return blsend();
        case KW_BLRECV: return blrecv();
        case KW_BLPAIR: return blpair();
        case KW_BLUNPAIR: return blunpair();
        case KW_BLKBD: return blkbd();
        default: return kw.id >= KW_COLOR && kw.id <= KW_TEXT ? plotter_keyword() : EXP_STATUS_ERROR;
    }
}

/* Caches the EVAL result now in the window and parses again. */
static uint8_t evaluated(void) {
    value_t *v = &kw.evals[kw.nevals++];
    v->start = kw.eval_at;
    v->end = W[W_ACTION + EXP_KW_B_LO];
    memcpy(v->reg, W, 8);
    v->len = 0;
    if (v->reg[4] == AR_STRING) {
        uint8_t len = v->reg[7];
        if (len > VALUE_TEXT_MAX) len = VALUE_TEXT_MAX;
        memcpy(v->text, W + 8, len);
        while (v->len < len && v->text[v->len] != 0) v->len++; /* a variable's own zero padding */
    }
    return begin();
}

static uint8_t resume(uint8_t step, uint8_t answer) {
    switch (step) {
        case ST_EVAL: return evaluated();
        case ST_FINISH: return done();
        case ST_FMT_CONFIRM:
            if (answer == KEY_Y) {
                W[0] = 0;
                W[1] = 6;
                memcpy(W + 2, "PC1500", 6);
                run(EXP_COMMAND_FORMAT_SD_CARD);
            }
            return done();
        case ST_RM_CONFIRM:
            if (answer != KEY_Y) return done();
            memcpy(W, kw.names, NAME_SLOT);
            return sdrm_remove();
        case ST_CPMV_CONFIRM:
            if (answer != KEY_Y) return done();
            memcpy(W, kw.names, 2 * NAME_SLOT);
            return cpmv_run();
        case ST_SAVE_CONFIRM:
            if (answer != KEY_Y) return done();
            kw.yflag = true; /* BLSAVE asks the peer again, now to overwrite */
            return save_create();
        case ST_LOAD_PICK: return load_picked(answer);
        case ST_PRINT_VALUE: return print_value();
        case ST_INPUT_LOOKUP: return input_looked_up();
        case ST_INPUT_VAR: return skip() == ',' ? input_next() : done();
        case ST_FN_SAVE_PTR: return copy_in((uint16_t)(basic_start() - FN_KEYS_LEN), FN_KEYS_LEN, ST_FN_SAVE_DATA);
        case ST_FN_SAVE_DATA: return fn_save_data();
        case ST_FN_LOAD_PTR: return fn_load_data();
        case ST_STATE_SAVE: return state_saved_chunk();
        case ST_STATE_LOAD: return state_loaded_chunk();
        case ST_STATE_FINAL: return state_final();
        case ST_BL_PICK: return bl_pick(answer);
        case ST_BL_PRINT_VALUE: return bl_print_value();
        case ST_BL_LIST_PTRS: return bl_list_ptrs();
        case ST_BL_LIST_CHUNK: return bl_list_chunk();
        case ST_ADV_WAIT: return adv_waited(answer);
        case ST_PUT_WAIT: return put_waited(answer);
        case ST_GET_WAIT: return get_waited(answer);
        case ST_GET_CONFIRM: return answer == KEY_Y ? get_start() : done();
        case ST_PUT_SENDING: return put_send();
        case ST_GET_RECEIVING: return get_receive();
        case ST_SEND_VALUE: return send_value();
        case ST_SEND_RETRY: return answer ? done() : send_try(false); /* BREAK: not sent */
        case ST_RECV_WAIT: return answer ? done() : recv_try(false);  /* BREAK: unchanged */
        case ST_RECV_LOOKUP: return recv_looked_up();
        case ST_RECV_VAR: return recv_stored();
        case ST_PAIR_PICK:
            W[0] = 0;
            W[1] = answer;
            return pair_begun(run(EXP_COMMAND_BLE_PAIR_BEGIN));
        case ST_PAIR_ANSWER:
            kw.mode = answer == KEY_Y; /* this side's answer, kept for the retries */
            return pair_confirm(true);
        case ST_PAIR_WAIT:
            if (answer) kw.mode = 0; /* BREAK: refuse, and stop */
            return pair_confirm(false);
        case ST_UNPAIR_ALL:
            if (answer != KEY_Y) return done();
            W[0] = 0;
            return run(EXP_COMMAND_BLE_UNPAIR) == EXP_STATUS_SUCCESS ? done() : error(40);
        case ST_ADV_PAIR: return adv_pair_answered(answer);
        case ST_LINK_ERROR: return error(40);
        case ST_KBD_WAIT: return kbd_waited(answer, false);
        case ST_CE150_ROM:
            if (W[0] != 0xC0) return load_basic(false);
            return action(EXP_KW_ACTION_COPY_IN, 0, CE150_PAGE, 1, ST_CE150_PAGE);
        case ST_CE150_PAGE: return load_basic(W[0] == 0x55);
        default: return EXP_STATUS_ERROR;
    }
}

/* BASIC tokenizes its own keywords even inside an extension keyword's
 * arguments (confirmed in pc1500emu: "MCONF SLEEPWAIT=1000" arrives as
 * "SLEEP", token F1B3 (WAIT), "=1000"); only quoted text is left alone. So
 * the statement is expanded back to the characters typed before parsing.
 * The base ROM's keywords (ROM1.BIN) plus the CE-150's (tokenized when it's
 * attached), taken from their own keyword tables. */
static const struct {
    uint16_t code;
    const char *text;
} kBasicTokens[] = {
    {0xE680,"CSIZE"}, {0xE681,"GRAPH"}, {0xE682,"GLCURSOR"}, {0xE683,"LCURSOR"}, {0xE684,"SORGN"},
    {0xE685,"ROTATE"}, {0xE686,"TEXT"}, {0xE7A9,"RMT"}, {0xF084,"CURSOR"}, {0xF085,"USING"},
    {0xF088,"CLS"}, {0xF089,"CLOAD"}, {0xF08F,"MERGE"}, {0xF090,"LIST"}, {0xF091,"INPUT"},
    {0xF093,"GCURSOR"}, {0xF095,"CSAVE"}, {0xF097,"PRINT"}, {0xF09F,"GPRINT"}, {0xF0B2,"CHAIN"},
    {0xF0B5,"COLOR"}, {0xF0B6,"LF"}, {0xF0B7,"LINE"}, {0xF0B8,"LLIST"}, {0xF0B9,"LPRINT"},
    {0xF0BA,"RLINE"}, {0xF0BB,"TAB"}, {0xF0BC,"TEST"}, {0xF150,"AND"}, {0xF151,"OR"},
    {0xF158,"MEM"}, {0xF15B,"TIME"}, {0xF15C,"INKEY$"}, {0xF15D,"PI"}, {0xF160,"ASC"},
    {0xF161,"STR$"}, {0xF162,"VAL"}, {0xF163,"CHR$"}, {0xF164,"LEN"}, {0xF165,"DEG"},
    {0xF166,"DMS"}, {0xF167,"STATUS"}, {0xF168,"POINT"}, {0xF16B,"SQR"}, {0xF16D,"NOT"},
    {0xF16E,"PEEK#"}, {0xF16F,"PEEK"}, {0xF170,"ABS"}, {0xF171,"INT"}, {0xF172,"RIGHT$"},
    {0xF173,"ASN"}, {0xF174,"ACS"}, {0xF175,"ATN"}, {0xF176,"LN"}, {0xF177,"LOG"}, {0xF178,"EXP"},
    {0xF179,"SGN"}, {0xF17A,"LEFT$"}, {0xF17B,"MID$"}, {0xF17C,"RND"}, {0xF17D,"SIN"},
    {0xF17E,"COS"}, {0xF17F,"TAN"}, {0xF180,"AREAD"}, {0xF181,"ARUN"}, {0xF182,"BEEP"},
    {0xF183,"CONT"}, {0xF186,"GRAD"}, {0xF187,"CLEAR"}, {0xF18A,"CALL"}, {0xF18B,"DIM"},
    {0xF18C,"DEGREE"}, {0xF18D,"DATA"}, {0xF18E,"END"}, {0xF192,"GOTO"}, {0xF194,"GOSUB"},
    {0xF196,"IF"}, {0xF198,"LET"}, {0xF199,"RETURN"}, {0xF19A,"NEXT"}, {0xF19B,"NEW"},
    {0xF19C,"ON"}, {0xF19D,"OPN"}, {0xF19E,"OFF"}, {0xF1A0,"POKE#"}, {0xF1A1,"POKE"},
    {0xF1A2,"PAUSE"}, {0xF1A4,"RUN"}, {0xF1A5,"FOR"}, {0xF1A6,"READ"}, {0xF1A7,"RESTORE"},
    {0xF1A8,"RANDOM"}, {0xF1AA,"RADIAN"}, {0xF1AB,"REM"}, {0xF1AC,"STOP"}, {0xF1AD,"STEP"},
    {0xF1AE,"THEN"}, {0xF1AF,"TRON"}, {0xF1B0,"TROFF"}, {0xF1B1,"TO"}, {0xF1B3,"WAIT"},
    {0xF1B4,"ERROR"}, {0xF1B5,"LOCK"}, {0xF1B6,"UNLOCK"},
};

/* A token's text: the base ROM's, the CE-150's, or this module's own (a
 * setting like AUTOSTAGE arrives as "AUTO" + the STAGE token). Below, with
 * BLLIST's table. */
static const char *token_text(uint16_t code);

/* The statement (src = window + 2, at most EXP_KW_LINE_LEN bytes) ->
 * kw.line, with kw.raw[] mapping each character back to its offset in src.
 * Stops at ':' or 0x0D outside quotes, and returns that offset: the
 * statement's length, where BASIC resumes. A token not in the table is kept
 * as its two bytes, which no parser accepts. */
static uint8_t detokenize(const uint8_t *src) {
    uint8_t in = 0, out = 0;
    bool quoted = false;
    while (in < EXP_KW_LINE_LEN && src[in] != CR && (quoted || src[in] != ':') && out < KW_TEXT_MAX) {
        uint8_t c = src[in];
        if (c == '"') quoted = !quoted;
        if (!quoted && c >= 0xE0 && in + 1 < EXP_KW_LINE_LEN) {
            const char *text = token_text((uint16_t)((c << 8) | src[in + 1]));
            if (text) {
                while (*text && out < KW_TEXT_MAX) {
                    kw.raw[out] = in;
                    kw.line[out++] = (uint8_t)*text++;
                }
                in += 2;
                continue;
            }
        }
        kw.raw[out] = in;
        kw.line[out++] = c;
        in++;
    }
    kw.raw[out] = in;
    kw.line[out] = CR;
    return in;
}

/* ---- BLE (2026-09-27) ----
 *
 * The PC-1500 Link, BLE_PROTOCOL.md. The link itself is the MCU's
 * (EXP_COMMAND_BLE_*); these only parse, and turn values and program lines
 * into text. BLSAVE/BLLOAD are SDSAVE/SDLOAD with the peer's file store in
 * place of the card (is_ble() above). */

#define BL_TEXT_MAX 1000 /* one EXP_COMMAND_BLE_TEXT: 2 length bytes + this fit the 1K area */
#define BL_ZONE 13       /* BLPRINT's ',' moves to the next 13-column zone */

static uint8_t bl_text[BL_TEXT_MAX];
static uint16_t bl_len;
static uint8_t bl_col; /* the column the peer's text is at, across BLPRINTs */
static bool bl_failed;
static uint8_t bl_chunk[EXP_MAX_TRANSFER_LEN]; /* BLLIST: a copy, since sending text reuses the window */

/* Sends what's buffered as TEXT; false (and remembered) if it failed. */
static bool bl_flush(void) {
    if (bl_len == 0 || bl_failed) return !bl_failed;
    W[0] = (uint8_t)(bl_len >> 8);
    W[1] = (uint8_t)bl_len;
    memcpy(W + 2, bl_text, bl_len);
    bl_len = 0;
    if (run(EXP_COMMAND_BLE_TEXT) != EXP_STATUS_SUCCESS) bl_failed = true;
    return !bl_failed;
}

static void bl_putc(uint8_t c) {
    if (bl_len == BL_TEXT_MAX && !bl_flush()) return;
    bl_text[bl_len++] = c;
    bl_col = c == CR || c == FF ? 0 : (uint8_t)(bl_col + 1); /* FF clears the console */
}

static void bl_puts(const char *text) {
    while (*text) bl_putc((uint8_t)*text++);
}

static uint8_t bl_finish(void) { return bl_flush() ? done() : error(40); }

/* A whole number as decimal digits into `out`; returns the length. */
static uint8_t format_uint(uint32_t n, char *out) {
    char digits[10];
    uint8_t count = 0, len = 0;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (count) out[len++] = digits[--count];
    out[len] = 0;
    return len;
}

/* The arithmetic register (TRM sec.5-3: decimal, or AR_BINARY) as text,
 * the way BASIC's STR$ writes it -- worked out from, and tested against,
 * STR$ in pc1500emu: up to 10 significant digits, trailing zeros dropped;
 * 0.5 with its leading zero, as long as at most 9 digits follow "0.";
 * otherwise, and from 1E10 up, 3.333333333E-01 / 1.5E 10 (a two-digit
 * exponent, a space for a positive one). */
static void format_number(const uint8_t *reg, char *out) {
    uint8_t digits[10], n = 0;
    int8_t exponent;
    if (reg[4] == AR_BINARY) {
        int16_t b = (int16_t)((reg[5] << 8) | reg[6]);
        if (b < 0) *out++ = '-';
        format_uint(b < 0 ? (uint32_t)(-(int32_t)b) : (uint32_t)b, out);
        return;
    }
    for (uint8_t i = 0; i < 10; i++) {
        uint8_t pair = reg[2 + i / 2];
        digits[i] = (i & 1) ? (uint8_t)(pair & 0x0F) : (uint8_t)(pair >> 4);
        if (digits[i]) n = (uint8_t)(i + 1);
    }
    if (n == 0) {
        strcpy(out, "0");
        return;
    }
    exponent = (int8_t)reg[0];
    if (reg[1] & 0x80) *out++ = '-';
    if (exponent >= 0 && exponent <= 9) {
        for (int8_t i = 0; i <= exponent; i++) *out++ = (char)('0' + (i < n ? digits[i] : 0));
        if (n > exponent + 1) {
            *out++ = '.';
            for (uint8_t i = (uint8_t)(exponent + 1); i < n; i++) *out++ = (char)('0' + digits[i]);
        }
    } else if (exponent < 0 && n - exponent - 1 <= 9) {
        *out++ = '0';
        *out++ = '.';
        for (int8_t i = -1; i > exponent; i--) *out++ = '0';
        for (uint8_t i = 0; i < n; i++) *out++ = (char)('0' + digits[i]);
    } else {
        uint8_t e = (uint8_t)(exponent < 0 ? -exponent : exponent);
        *out++ = (char)('0' + digits[0]);
        if (n > 1) {
            *out++ = '.';
            for (uint8_t i = 1; i < n; i++) *out++ = (char)('0' + digits[i]);
        }
        *out++ = 'E';
        *out++ = exponent < 0 ? '-' : ' ';
        *out++ = (char)('0' + e / 10);
        *out++ = (char)('0' + e % 10);
    }
    *out = 0;
}

/* BLSCAN [seconds]     scan (default 3s), pick a peer from the listing
 * BLCON name           connect to the peer advertising that name (not the
 *                      emulator on Windows: it can't advertise its own name)
 * BLDISC               disconnect */
/* A link that failed: an unpaired or no-longer-paired peer says so (then
 * ERROR 40); anything else is just ERROR 40. */
static uint8_t link_failed(void) {
    if (W[0] == EXP_BLE_ERR_NOT_PAIRED) return show_str("BLE: NOT PAIRED - BLPAIR", ST_LINK_ERROR);
    if (W[0] == EXP_BLE_ERR_AUTH_FAILED) return show_str("BLE: PAIRING LOST - BLPAIR", ST_LINK_ERROR);
    return error(40);
}

static uint8_t bl_connected(uint8_t status) {
    uint8_t text[LINE_WIDTH], len = W[0];
    if (status != EXP_STATUS_SUCCESS) return link_failed();
    if (len > LINE_WIDTH - 11) len = LINE_WIDTH - 11;
    memcpy(text, "CONNECTED: ", 11);
    memcpy(text + 11, W + 1, len);
    bl_col = 0;
    return show(text, (uint8_t)(11 + len), ST_FINISH);
}

static uint8_t blscan(void) {
    const value_t *v;
    uint16_t seconds = 3;
    if (skip() != CR && (!expr(&v) || !to_uint(v, 30, &seconds) || seconds == 0 || skip() != CR)) return fail();
    W[0] = (uint8_t)seconds;
    if (run(EXP_COMMAND_BLE_SCAN) != EXP_STATUS_SUCCESS) return error(40);
    if (W[0] == 0 && W[1] == 0) return show_str("BLE: NO PEERS FOUND", ST_FINISH);
    return browse(EXP_KW_BROWSE_PICK_C, ST_BL_PICK);
}

static uint8_t bl_pick(uint8_t index) {
    W[0] = index;
    return bl_connected(run(EXP_COMMAND_BLE_CONNECT));
}

static uint8_t blconnect(void) {
    if (!name_arg() || skip() != CR) return fail();
    return bl_connected(run(EXP_COMMAND_BLE_CONNECT_NAME));
}

/* ---- peer-to-peer (2026-09-28, BLE_PROTOCOL.md "Peer-to-peer files") ----
 *
 * Each of these waits for the other PC-1500, a POLL at a time, and BREAK
 * stops the wait and returns to BASIC. */

/* The first POLL of a wait, showing what it waits for. */
static uint8_t wait_start(const char *text, uint8_t next) {
    memset(W, ' ', LINE_WIDTH);
    memcpy(W, text, strlen(text));
    return action(EXP_KW_ACTION_POLL, EXP_KW_POLL_CLEAR | EXP_KW_POLL_SHOW, 0, 0, next);
}

static uint8_t wait_more(uint8_t next) { return action(EXP_KW_ACTION_POLL, 0, 0, 0, next); }

/* Shows `text` and carries on at `next` (one POLL, whose BREAK is
 * ignored): "SENDING..." once a transfer is agreed, which for a big file
 * would otherwise look like a hang. */
static uint8_t show_line(const char *text, uint8_t next) {
    memset(W, ' ', LINE_WIDTH);
    memcpy(W, text, strlen(text));
    return action(EXP_KW_ACTION_POLL, EXP_KW_POLL_SHOW, 0, 0, next);
}

/* EXP_BLE_STATUS_* (the peer's name at W+2, length W[1]), or 0. */
static uint8_t bl_status(void) { return run(EXP_COMMAND_BLE_STATUS) == EXP_STATUS_SUCCESS ? W[0] : 0; }

/* BLADV -- advertise, and wait for another PC-1500 to BLSCAN/BLCON. */
static uint8_t bladv(void) {
    if (skip() != CR) return error(1);
    W[0] = 1;
    if (run(EXP_COMMAND_BLE_ADVERTISE) != EXP_STATUS_SUCCESS) return error(40);
    if (bl_status() & EXP_BLE_STATUS_LINKED) return adv_waited(0);
    return wait_start("BLADV: WAITING", ST_ADV_WAIT);
}

static uint8_t adv_waited(uint8_t brk) {
    uint8_t s = bl_status();
    if (s & EXP_BLE_STATUS_PAIR_ASK) { /* a connector's BLPAIR: its code */
        uint8_t text[LINE_WIDTH];
        memcpy(text, "PAIR CODE ", 10);
        memcpy(text + 10, W + EXP_BLE_FILE_ARGS, 6);
        memcpy(text + 16, " Y/N", 4);
        return show(text, 20, ST_ADV_PAIR);
    }
    if (s & EXP_BLE_STATUS_LINKED) {
        W[0] = W[1]; /* STATUS's [len][name] -> CONNECT's */
        memmove(W + 1, W + 2, W[0]);
        return bl_connected(EXP_STATUS_SUCCESS);
    }
    if (brk) {
        W[0] = 0;
        run(EXP_COMMAND_BLE_ADVERTISE);
        return done();
    }
    return wait_more(ST_ADV_WAIT);
}

/* Closes BLPUT SD's file, if it has one open. */
static void put_close_card(void) {
    if (kw.mode == PUT_SD) run(EXP_COMMAND_CLOSE_SD_FILE);
}

/* BLPUT                     the BASIC program
 * BLPUT M start,end[,call]  memory, with SDSAVE M's [start][call] header
 * BLPUT SD name             a file from the card, sent as BASIC...
 * BLPUT SD M name           ...or as an M file (the card doesn't say which)
 * The receiver's BLGET says where it goes; the offer's name is the file's
 * (none for memory). */
static uint8_t blput(void) {
    const value_t *v;
    uint8_t *args, kind = EXP_BLE_KIND_BASIC;
    uint32_t size = 0xFFFFFFFFu; /* unknown: BASIC is streamed; the card's isn't sized */
    memset(kw.names, 0, NAME_SLOT);
    if (skip() == CR) {
        kw.mode = SAVE_BASIC;
    } else if (word("SD")) {
        if (m_flag()) kind = EXP_BLE_KIND_M;
        if (!name_arg() || skip() != CR) return fail();
        memcpy(kw.names, W, NAME_SLOT);
        kw.mode = PUT_SD;
    } else if (cur() == 'M') {
        kw.pos++;
        if (cur() == ',') kw.pos++;
        if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.start) || skip() != ',') return fail();
        kw.pos++;
        if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.end) || kw.end < kw.start) return fail();
        kw.call = 0;
        if (skip() == ',') {
            kw.pos++;
            if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.call)) return fail();
        }
        if (skip() != CR) return fail();
        kind = EXP_BLE_KIND_M;
        size = (uint32_t)(kw.end - kw.start) + 1u + 4u;
        kw.mode = SAVE_M;
    } else {
        return error(1);
    }
    if (kw.mode == PUT_SD) {
        memcpy(W, kw.names, NAME_SLOT);
        if (run(EXP_COMMAND_OPEN_SD_FILE_READ) != EXP_STATUS_SUCCESS) return error(40);
    }
    memcpy(W, kw.names, NAME_SLOT);
    args = W + EXP_BLE_FILE_ARGS;
    args[0] = kind;
    args[1] = 0;
    args[2] = (uint8_t)(size >> 24);
    args[3] = (uint8_t)(size >> 16);
    args[4] = (uint8_t)(size >> 8);
    args[5] = (uint8_t)size;
    if (run(EXP_COMMAND_BLE_OFFER) != EXP_STATUS_SUCCESS) {
        put_close_card();
        return error(40);
    }
    return wait_start("BLPUT: WAITING FOR BLGET", ST_PUT_WAIT);
}

/* BLPUT SD: the file, a piece at a time, from the card to the peer. */
static uint8_t put_card(void) {
    for (;;) {
        W[W_LENGTH_PORT] = (uint8_t)(EXP_MAX_TRANSFER_LEN >> 8);
        W[W_LENGTH_PORT + 1] = (uint8_t)EXP_MAX_TRANSFER_LEN;
        if (run(EXP_COMMAND_READ_FROM_SD_FILE) != EXP_STATUS_SUCCESS) break;
        if (W[W_LENGTH_PORT] == 0 && W[W_LENGTH_PORT + 1] == 0) { /* the end */
            run(EXP_COMMAND_CLOSE_SD_FILE);
            W[0] = 0;
            return run(EXP_COMMAND_BLE_DATA_CLOSE) == EXP_STATUS_SUCCESS ? done() : error(40);
        }
        if (run(EXP_COMMAND_BLE_DATA_WRITE) != EXP_STATUS_SUCCESS) break; /* the length port: READ's count */
    }
    run(EXP_COMMAND_CLOSE_SD_FILE);
    W[0] = 1; /* abandon */
    run(EXP_COMMAND_BLE_DATA_CLOSE);
    return error(40);
}

static uint8_t put_send(void) { return kw.mode == PUT_SD ? put_card() : save_opened(); }

static uint8_t put_waited(uint8_t brk) {
    uint8_t s = bl_status();
    if (s & EXP_BLE_STATUS_ANSWERED) {
        if (!(s & EXP_BLE_STATUS_ACCEPTED)) {
            put_close_card();
            return show_str("BLPUT: REFUSED", ST_FINISH);
        }
        W[0] = kw.mode != PUT_SD; /* routed: the ROM's SAVE moves the bytes */
        if (run(EXP_COMMAND_BLE_SEND) != EXP_STATUS_SUCCESS) {
            put_close_card();
            return error(40);
        }
        return show_line("BLPUT: SENDING...", ST_PUT_SENDING);
    }
    if (!(s & EXP_BLE_STATUS_LINKED)) {
        put_close_card();
        return error(40);
    }
    if (brk) {
        run(EXP_COMMAND_BLE_WITHDRAW);
        put_close_card();
        return done();
    }
    return wait_more(ST_PUT_WAIT);
}

/* BLGET              into memory: a BASIC file as the program, an M file
 *                    at its header's load address (CALLed if it has one)
 * BLGET name[,-Y]    onto the card as `name` (asks before overwriting) */
static uint8_t blget(void) {
    kw.mode = GET_MEMORY;
    if (skip() != CR) {
        if (!name_arg() || !parse_yflag()) return fail();
        memcpy(kw.names, W, NAME_SLOT);
        kw.mode = GET_SD;
        if (!kw.yflag && run(EXP_COMMAND_OPEN_SD_FILE_READ) == EXP_STATUS_SUCCESS) {
            run(EXP_COMMAND_CLOSE_SD_FILE);
            return show_str("FILE EXISTS. OVERWRITE Y/N", ST_GET_CONFIRM);
        }
    }
    return get_start();
}

static uint8_t get_start(void) {
    if (!(bl_status() & EXP_BLE_STATUS_LINKED)) return error(40);
    return wait_start("BLGET: WAITING FOR BLPUT", ST_GET_WAIT);
}

/* BLGET name: the file, a piece at a time, from the peer to the card. */
static uint8_t get_card(void) {
    memcpy(W, kw.names, NAME_SLOT);
    if (run(EXP_COMMAND_CREATE_SD_FILE) != EXP_STATUS_SUCCESS) {
        W[0] = 0; /* refuse */
        W[1] = 0;
        run(EXP_COMMAND_BLE_ANSWER);
        return error(40);
    }
    W[0] = 1; /* accept, unrouted: the SD commands stay on the card */
    W[1] = 0;
    if (run(EXP_COMMAND_BLE_ANSWER) != EXP_STATUS_SUCCESS) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        return error(40);
    }
    return show_line("BLGET: RECEIVING...", ST_GET_RECEIVING);
}

/* ...and the pieces, once "RECEIVING..." is up. */
static uint8_t get_card_data(void) {
    for (;;) {
        W[W_LENGTH_PORT] = (uint8_t)(EXP_MAX_TRANSFER_LEN >> 8);
        W[W_LENGTH_PORT + 1] = (uint8_t)EXP_MAX_TRANSFER_LEN;
        if (run(EXP_COMMAND_BLE_DATA_READ) != EXP_STATUS_SUCCESS) break;
        if (W[W_LENGTH_PORT] == 0 && W[W_LENGTH_PORT + 1] == 0) { /* FILE_END */
            bool ok;
            W[0] = 0;
            ok = run(EXP_COMMAND_BLE_DATA_CLOSE) == EXP_STATUS_SUCCESS;
            ok = run(EXP_COMMAND_CLOSE_SD_FILE) == EXP_STATUS_SUCCESS && ok;
            return ok ? done() : error(40);
        }
        if (run(EXP_COMMAND_WRITE_TO_SD_FILE) != EXP_STATUS_SUCCESS) break; /* READ's count */
    }
    W[0] = 1; /* abandon */
    run(EXP_COMMAND_BLE_DATA_CLOSE);
    run(EXP_COMMAND_CLOSE_SD_FILE);
    return error(40);
}

static uint8_t get_receive(void) { return kw.mode == GET_SD ? get_card_data() : load_opened(); }

static uint8_t get_waited(uint8_t brk) {
    uint8_t s = bl_status();
    if (s & EXP_BLE_STATUS_OFFER_IN) {
        uint8_t kind;
        if (run(EXP_COMMAND_BLE_OFFER_GET) != EXP_STATUS_SUCCESS) return error(40);
        kind = W[EXP_BLE_FILE_ARGS];
        if (kw.mode == GET_SD) return get_card();
        W[0] = 1; /* accept, routed: the ROM's LOAD moves the bytes */
        W[1] = 1;
        if (run(EXP_COMMAND_BLE_ANSWER) != EXP_STATUS_SUCCESS) return error(40);
        kw.mode = kind == EXP_BLE_KIND_M ? LOAD_M_HEADER : LOAD_BASIC;
        return show_line("BLGET: RECEIVING...", ST_GET_RECEIVING);
    }
    if (!(s & EXP_BLE_STATUS_LINKED)) return error(40);
    if (brk) return done();
    return wait_more(ST_GET_WAIT);
}

/* ---- pairing (2026-10-03, BLE_PROTOCOL.md sec.7) ----
 *
 * Once per pair of devices: BLPAIR here, the app's Accept (or BLADV's Y/N
 * on another PC-1500) there, after both people have checked the code is
 * the same at both ends. Every BLCON after that authenticates by itself. */

/* BLPAIR          scan, pick a peer with P, and pair with it
 * BLPAIR name     pair with the peer advertising that name */
static uint8_t blpair(void) {
    const value_t *v;
    if (skip() == CR) {
        W[0] = 3;
        if (run(EXP_COMMAND_BLE_SCAN) != EXP_STATUS_SUCCESS) return error(40);
        if (W[0] == 0 && W[1] == 0) return show_str("BLE: NO PEERS FOUND", ST_FINISH);
        return browse(EXP_KW_BROWSE_PICK_P, ST_PAIR_PICK);
    }
    if (!expr(&v) || !is_string(v) || v->len == 0 || skip() != CR) return fail();
    W[0] = 1;
    W[1] = v->len > EXP_PATH_ARG_LEN ? EXP_PATH_ARG_LEN : v->len;
    memcpy(W + 2, v->text, W[1]);
    return pair_begun(run(EXP_COMMAND_BLE_PAIR_BEGIN));
}

/* Keys exchanged: show the code, for this side's Y/N. */
static uint8_t pair_begun(uint8_t status) {
    uint8_t text[LINE_WIDTH];
    if (status != EXP_STATUS_SUCCESS) return W[0] == EXP_BLE_ERR_AUTH_FAILED ? show_str("BLPAIR: FAILED", ST_LINK_ERROR) : error(40);
    memcpy(text, "PAIR CODE ", 10);
    memcpy(text + 10, W, 6);
    memcpy(text + 16, " Y/N", 4);
    return show(text, 20, ST_PAIR_ANSWER);
}

/* This side's answer (kw.mode) to the peer; until the peer's user answers,
 * again after each POLL. */
static uint8_t pair_confirm(bool first) {
    W[0] = kw.mode ? 1 : 0;
    if (run(EXP_COMMAND_BLE_PAIR_CONFIRM) != EXP_STATUS_SUCCESS) return link_failed();
    if (W[0] == 1) { /* paired, and the link authenticated: as BLCON's */
        memmove(W, W + 1, (uint16_t)(1 + W[1]));
        return bl_connected(EXP_STATUS_SUCCESS);
    }
    if (W[0] == 2) return kw.mode ? show_str("BLPAIR: REFUSED", ST_FINISH) : done();
    return first ? wait_start("BLPAIR: WAITING FOR PEER", ST_PAIR_WAIT) : wait_more(ST_PAIR_WAIT);
}

/* BLUNPAIR        forget every pairing (asks first)
 * BLUNPAIR name   forget the one with that name (ERROR 40 if none) */
static uint8_t blunpair(void) {
    const value_t *v;
    if (skip() == CR) return show_str("FORGET ALL PAIRINGS Y/N", ST_UNPAIR_ALL);
    if (!expr(&v) || !is_string(v) || v->len == 0 || skip() != CR) return fail();
    W[0] = v->len > 16 ? 16 : v->len;
    memcpy(W + 1, v->text, W[0]);
    if (run(EXP_COMMAND_BLE_UNPAIR) != EXP_STATUS_SUCCESS || W[0] == 0) return error(40);
    return done();
}

/* BLADV's side: this user's answer to the connector's pairing; then on
 * waiting (the connector's next HELLO makes the link). */
static uint8_t adv_pair_answered(uint8_t answer) {
    W[0] = answer == KEY_Y ? 1 : 0;
    run(EXP_COMMAND_BLE_PAIR_ANSWER);
    return wait_more(ST_ADV_WAIT);
}

/* ---- the external keyboard (2026-10-04, MCONF BLKBD, kbd_host.h) ---- */

/* BLKBD          pair a keyboard (put it in pairing mode first), replacing
 *                any paired before; the code it asks for is shown, to type
 *                on the keyboard and end with Enter. BREAK stops.
 * BLKBD FORGET   forget the paired keyboard
 * BLKBD ?        what the keyboard is sending: "S4 R37 L10 A1010004 P01" -- the
 *                state, reports received, the last one's length and first
 *                bytes, SET_PROTOCOL's answer (handshake, mode; FF none);
 *                after a failed pairing, "F2:0C" in place of the bytes: the
 *                step (1 search, 2 connect, 3 connection, 4 pairing) and
 *                BTstack's status */
static uint8_t blkbd(void) {
    if (word("?")) { /* what's arriving from the keyboard (2026-10-05, pc_exp.h KBD_STATUS) */
        static const char hex[] = "0123456789ABCDEF";
        char text[LINE_WIDTH + 1];
        uint8_t n = 0;
        if (skip() != CR) return error(1);
        if (run(EXP_COMMAND_KBD_STATUS) != EXP_STATUS_SUCCESS) return error(40);
        uint16_t reports = (uint16_t)(W[25] << 8 | W[26]);
        char digits[6];
        uint8_t d;
        text[n++] = 'S';
        text[n++] = (char)('0' + (W[0] % 10));
        text[n++] = ' ';
        text[n++] = 'R';
        d = 0;
        do digits[d++] = (char)('0' + reports % 10); while ((reports /= 10) != 0);
        while (d) text[n++] = digits[--d];
        text[n++] = ' ';
        text[n++] = 'L';
        d = 0;
        uint8_t len = W[27];
        do digits[d++] = (char)('0' + len % 10); while ((len /= 10) != 0);
        while (d) text[n++] = digits[--d];
        text[n++] = ' ';
        if (W[0] == EXP_KBD_FAILED) { /* where it failed instead: F step:status */
            text[n++] = 'F';
            text[n++] = (char)('0' + W[33] % 10);
            text[n++] = ':';
            text[n++] = hex[W[34] >> 4];
            text[n++] = hex[W[34] & 15];
        } else {
            for (int i = 0; i < 4; i++) {
                text[n++] = hex[W[28 + i] >> 4];
                text[n++] = hex[W[28 + i] & 15];
            }
        }
        text[n++] = ' ';
        text[n++] = 'P';
        text[n++] = hex[W[32] >> 4];
        text[n++] = hex[W[32] & 15];
        return show((const uint8_t *)text, n, ST_FINISH);
    }
    if (word("FORGET")) {
        if (skip() != CR) return error(1);
        return run(EXP_COMMAND_KBD_FORGET) == EXP_STATUS_SUCCESS ? done() : error(40);
    }
    if (skip() != CR) return error(1);
    W[0] = MCU_CONFIG_BLKBD;
    if (run(EXP_COMMAND_CONFIG_GET) != EXP_STATUS_SUCCESS || (W[1] | W[2]) == 0)
        return show_str("BLKBD: MCONF BLKBD=1 FIRST", ST_FINISH);
    if (run(EXP_COMMAND_KBD_PAIR) != EXP_STATUS_SUCCESS) return error(40);
    return kbd_waited(0, true);
}

/* One POLL at a time, showing how pairing is going, until it's done
 * (shown until a key: the new keyboard's own, if it likes) or BREAK. */
static uint8_t kbd_waited(uint8_t brk, bool first) {
    uint8_t text[LINE_WIDTH], n;
    if (brk) {
        run(EXP_COMMAND_KBD_STOP);
        return done();
    }
    if (run(EXP_COMMAND_KBD_STATUS) != EXP_STATUS_SUCCESS) return error(40);
    switch (W[0]) {
        case EXP_KBD_CONNECTED:
            n = W[8] > EXP_KBD_NAME_MAX ? EXP_KBD_NAME_MAX : W[8];
            memcpy(text, "BLKBD OK: ", 10);
            memcpy(text + 10, W + 9, n);
            return show(text, (uint8_t)(10 + n), ST_FINISH);
        case EXP_KBD_NOT_FOUND: return show_str("BLKBD: NO KEYBOARD FOUND", ST_FINISH);
        case EXP_KBD_FAILED: return show_str("BLKBD: PAIRING FAILED", ST_FINISH);
        case EXP_KBD_NONE:
        case EXP_KBD_PAIRED: return done(); /* stopped elsewhere */
        case EXP_KBD_CODE:
            memcpy(text, "TYPE ", 5);
            memcpy(text + 5, W + 2, 6);
            memcpy(text + 11, " + ENTER", 8);
            n = 19;
            break;
        case EXP_KBD_CONNECTING:
            memcpy(text, "BLKBD: CONNECTING", 17);
            n = 17;
            break;
        default:
            memcpy(text, "BLKBD: SEARCHING", 16);
            n = 16;
            break;
    }
    memset(W, ' ', LINE_WIDTH);
    memcpy(W, text, n);
    return action(EXP_KW_ACTION_POLL, (uint8_t)((first ? EXP_KW_POLL_CLEAR : 0) | EXP_KW_POLL_SHOW), 0, 0,
                  ST_KBD_WAIT);
}

/* ---- peer messaging (2026-09-29, BLE_PROTOCOL.md "Peer messaging") ----
 *
 * A message is one BLSEND's values as SDPRINT-style chunks, taken by one
 * BLRECV. The peer's MCU keeps up to 8 until then. */

static uint8_t msg_buf[EXP_BLE_MSG_MAX];
static uint16_t msg_len, msg_pos;

/* BLSEND value[,value...] -- numbers and strings, any expressions. */
static uint8_t blsend(void) {
    msg_len = 0;
    if (skip() == CR || cur() == ',') return error(1);
    return eval_at(kw.raw[kw.pos], ST_SEND_VALUE);
}

static uint8_t send_value(void) {
    uint8_t end = W[W_ACTION + EXP_KW_B_LO], n;
    result_to_chunk(); /* at W+1 */
    n = (uint8_t)(W[1] == 'N' ? 9 : 2 + W[2]);
    if (msg_len + n > EXP_BLE_MSG_MAX) return error(40); /* too long for one message */
    memcpy(msg_buf + msg_len, W + 1, n);
    msg_len = (uint16_t)(msg_len + n);
    while (cur() != CR && kw.raw[kw.pos] < end) kw.pos++;
    if (skip() == ',') {
        kw.pos++;
        if (skip() == CR || cur() == ',') return error(1);
        return eval_at(kw.raw[kw.pos], ST_SEND_VALUE);
    }
    return skip() == CR ? send_try(true) : error(1);
}

/* Sends the message; while the peer's inbox is full, tries again after
 * each POLL until BREAK. */
static uint8_t send_try(bool first) {
    W[0] = (uint8_t)(msg_len >> 8);
    W[1] = (uint8_t)msg_len;
    memcpy(W + 2, msg_buf, msg_len);
    if (run(EXP_COMMAND_BLE_MSG_SEND) == EXP_STATUS_SUCCESS) return done();
    if (W[0] != 6) return error(40); /* BLE_PROTOCOL.md's ERR BUSY is the only wait */
    return first ? wait_start("BLSEND: PEER INBOX FULL", ST_SEND_RETRY) : wait_more(ST_SEND_RETRY);
}

/* BLRECV [#t,]var[,var...] -- the oldest message's values, in order; extra
 * variables become 0 / blank, a number for a string (or the reverse) is
 * ERROR 42. Waits for a message until BREAK, or up to t seconds (#0: not
 * at all); if none comes the variables are left as they were. */
static uint8_t blrecv(void) {
    const value_t *v;
    uint16_t t = 0xFFFF, code;
    uint8_t list;
    if (skip() == '#') {
        kw.pos++;
        if (!expr(&v) || !to_uint(v, 0xFFFE, &t) || skip() != ',') return fail();
        kw.pos++;
    }
    list = kw.pos;
    if (!parse_var(&code)) return error(1); /* at least one, checked before waiting */
    kw.pos = list;
    W[0] = (uint8_t)(t >> 8);
    W[1] = (uint8_t)t;
    run(EXP_COMMAND_BLE_MSG_WAIT);
    return recv_try(true);
}

static uint8_t recv_var(void) {
    if (!parse_var(&kw.var)) return error(1);
    return action(EXP_KW_ACTION_VAR_LOOKUP, 0, kw.var, 0, ST_RECV_LOOKUP);
}

static uint8_t recv_try(bool first) {
    if (run(EXP_COMMAND_BLE_MSG_RECV) == EXP_STATUS_SUCCESS) {
        msg_len = (uint16_t)((W[0] << 8) | W[1]);
        if (msg_len > EXP_BLE_MSG_MAX) msg_len = EXP_BLE_MSG_MAX;
        memcpy(msg_buf, W + 2, msg_len);
        msg_pos = 0;
        return recv_var();
    }
    if (W[0] == 1) return done();    /* the wait's time is up */
    if (W[0] == 2) return error(40); /* no link, so nothing can come */
    return first ? wait_start("BLRECV: WAITING", ST_RECV_WAIT) : wait_more(ST_RECV_WAIT);
}

/* The next chunk into the variable just looked up (type at W[0]). */
static uint8_t recv_looked_up(void) {
    uint8_t type = W[0];
    if (msg_pos >= msg_len) {
        memset(W, 0, var_size(type));
    } else {
        uint16_t n = msg_buf[msg_pos] == 'N' ? 9 : (uint16_t)(2 + msg_buf[msg_pos + 1]);
        if (msg_pos + n > msg_len) n = (uint16_t)(msg_len - msg_pos);
        memcpy(W, msg_buf + msg_pos, n);
        msg_pos = (uint16_t)(msg_pos + n);
        if (!chunk_to_storage(type)) return error(42);
    }
    return action(EXP_KW_ACTION_VAR_STORE, 0, 0, 0, ST_RECV_VAR);
}

static uint8_t recv_stored(void) {
    if (skip() == CR) return done();
    if (cur() != ',') return error(1);
    kw.pos++;
    return recv_var();
}

/* Functions (kw_function() below; rom.asm's FN_CALL). Each leaves its
 * value at W as the arithmetic register's 8 bytes, or returns 0 after
 * setting *error to the BASIC error number. */

/* BLSTAT -- messages waiting (0-8), or -1 with no link and none waiting. */
static uint8_t blstat_value(uint8_t *error) {
    int16_t s;
    if (run(EXP_COMMAND_BLE_MSG_COUNT) != EXP_STATUS_SUCCESS) return (uint8_t)(*error = 40, 0);
    s = W[0] ? (int16_t)W[0] : (W[1] ? 0 : -1);
    int_to_decimal(s, W);
    return 1;
}

/* SDEOF(n) (2026-09-30) -- 1 once channel n has no value left for SDINPUT#
 * to read (it would get 0 / blank), else 0; so a program can stop a read
 * loop at the end of the file. A channel that isn't 1-16 is ERROR 1, one
 * that isn't open ERROR 40, as SDINPUT#. The argument arrives evaluated,
 * as the arithmetic register's 8 bytes at W. */
static uint8_t sdeof_value(uint8_t *error) {
    value_t v;
    uint16_t n;
    memset(&v, 0, sizeof v);
    memcpy(v.reg, W, 8);
    if (!to_uint(&v, EXP_MAX_SD_CHANNELS, &n) || n == 0) return (uint8_t)(*error = 1, 0);
    W[0] = (uint8_t)n;
    if (run(EXP_COMMAND_SD_CHANNEL_EOF) != EXP_STATUS_SUCCESS) return (uint8_t)(*error = 40, 0);
    int_to_decimal(W[0] ? 1 : 0, W);
    return 1;
}

/* BLPRINT [value[{;|,}value...][;|,]] -- to the peer's text window, like
 * PRINT: ';' runs values together, ',' moves to the next 13-column zone,
 * and a trailing separator leaves the line open. */
static uint8_t blprint(void) {
    bl_len = 0;
    bl_failed = false;
    if (skip() == CR) {
        bl_putc(CR);
        return bl_finish();
    }
    return eval_at(kw.raw[kw.pos], ST_BL_PRINT_VALUE);
}

/* BLCLS -- clears the peer's console: FF in the text (BLE_PROTOCOL.md). */
static uint8_t blcls(void) {
    if (skip() != CR) return error(1);
    bl_len = 0;
    bl_failed = false;
    bl_putc(FF);
    return bl_finish();
}

static uint8_t bl_print_value(void) {
    uint8_t end = W[W_ACTION + EXP_KW_B_LO];
    char text[VALUE_TEXT_MAX + 1];
    if (W[4] == AR_STRING) {
        uint8_t len = W[7] > VALUE_TEXT_MAX ? VALUE_TEXT_MAX : W[7];
        for (uint8_t i = 0; i < len; i++)
            if (W[8 + i] == 0) len = i;
        memcpy(text, W + 8, len);
        text[len] = 0;
    } else {
        format_number(W, text);
    }
    bl_puts(text);
    while (cur() != CR && kw.raw[kw.pos] < end) kw.pos++;
    switch (skip()) {
        case ',':
            do bl_putc(' ');
            while (bl_col % BL_ZONE != 0);
            /* fall through */
        case ';':
            kw.pos++;
            if (skip() == CR) return bl_finish();
            return eval_at(kw.raw[kw.pos], ST_BL_PRINT_VALUE);
        case CR:
            bl_putc(CR);
            return bl_finish();
        default: return error(1);
    }
}

/* BLLIST [from[,to]] -- the BASIC program (or the lines from..to) as text,
 * "<line> <statement>" per line. Spacing is pc1500emu's own listing rule
 * (src/basic/basic_text.cpp), so the two agree: one space around each
 * keyword unless it would sit next to ( ) , ; : or a quote. (The ROM's own
 * LIST only draws the LCD.) */
static const struct {
    uint16_t code;
    const char *text;
} kModuleTokens[] = {
    {0xE180,"SDMV"}, {0xE185,"SDLS"}, {0xE186,"SDSAVE"}, {0xE187,"SDLOAD"}, {0xE188,"SDRM"},
    {0xE189,"SDFMT"}, {0xE18A,"SDDF"}, {0xE18B,"SDCP"}, {0xE18C,"SDCD"}, {0xE18D,"SDMKDIR"},
    {0xE18E,"SDRMDIR"}, {0xE18F,"SDPWD"}, {0xE190,"SDOPEN"}, {0xE191,"SDCLOSE"}, {0xE192,"SDINPUT"},
    {0xE193,"SDPRINT"}, {0xE194,"SDSKIP"}, {0xE195,"ECVER"}, {0xE197,"STAGE"}, {0xE198,"MLOG"},
    {0xE199,"MLOGMSG"}, {0xE19A,"MCONF"}, {0xE19B,"FNCLR"}, {0xE19C,"FNSAVE"}, {0xE19D,"FNLOAD"},
    {0xE19E,"STSAVE"}, {0xE19F,"STLOAD"}, {0xE1A0,"BLSCAN"}, {0xE1A1,"BLCON"}, {0xE1A2,"BLDISC"},
    {0xE1A3,"BLPRINT"}, {0xE1A4,"BLLIST"}, {0xE1A5,"BLSAVE"}, {0xE1A6,"BLLOAD"}, {0xE1A7,"BLCLS"},
    {0xE1A8,"BLADV"}, {0xE1A9,"BLPUT"}, {0xE1AA,"BLGET"}, {0xE1AB,"BLSEND"}, {0xE1AC,"BLRECV"},
    {0xE1AD,"BLPAIR"}, {0xE1AE,"BLUNPAIR"}, {0xE1AF,"BLKBD"}, {0xE152,"BLSTAT"}, {0xE170,"SDEOF"},
    {0xE1C0,"CSIZE"}, {0xE1C1,"GRAPH"}, {0xE1C2,"GLCURSOR"}, {0xE1C3,"LCURSOR"}, {0xE1C4,"SORGN"},
    {0xE1C5,"ROTATE"}, {0xE1C6,"TEXT"},
};

static const char *token_text(uint16_t code) {
    for (size_t i = 0; i < sizeof kBasicTokens / sizeof kBasicTokens[0]; i++)
        if (kBasicTokens[i].code == code) return kBasicTokens[i].text;
    for (size_t i = 0; i < sizeof kModuleTokens / sizeof kModuleTokens[0]; i++)
        if (kModuleTokens[i].code == code) return kModuleTokens[i].text;
    return NULL;
}

static bool no_space_char(uint8_t c) { return c == '(' || c == ')' || c == ',' || c == ';' || c == ':' || c == '"'; }

/* A program line's statements as text, in list_text (BLLIST's and LLIST's). */
#define LIST_TEXT_MAX 1024 /* a 255-byte line of tokens, expanded */
static char list_text[LIST_TEXT_MAX];
static uint16_t list_len;

static void list_putc(uint8_t c) {
    if (list_len < LIST_TEXT_MAX) list_text[list_len++] = (char)c;
}

static void list_puts(const char *text) {
    while (*text) list_putc((uint8_t)*text++);
}

/* `content` is the line's bytes before the closing CR. */
static void list_line_text(const uint8_t *content, uint8_t len) {
    static const char kHex[] = "0123456789ABCDEF";
    char text[2];
    bool quoted = false, pending = false;
    uint8_t last = ' ';
    list_len = 0;
    for (uint8_t i = 0; i < len;) {
        uint8_t c = content[i];
        const char *unit;
        char hex[7];
        bool keyword = false, space;
        if (quoted) {
            list_putc(c);
            if (c == '"') quoted = false;
            last = c;
            i++;
            continue;
        }
        if (c == '"') {
            if (pending) list_putc(' ');
            list_putc(c);
            quoted = true;
            pending = false;
            last = c;
            i++;
            continue;
        }
        if (c < 0x80) {
            text[0] = (char)c;
            text[1] = 0;
            unit = text;
            i++;
        } else {
            uint16_t code;
            if (i + 1 >= len) break;
            code = (uint16_t)((c << 8) | content[i + 1]);
            unit = token_text(code);
            if (!unit) {
                hex[0] = '[';
                for (uint8_t d = 0; d < 4; d++) hex[1 + d] = kHex[(code >> (12 - 4 * d)) & 0x0F];
                hex[5] = ']';
                hex[6] = 0;
                unit = hex;
            }
            keyword = true;
            i += 2;
        }
        space = pending && !no_space_char((uint8_t)unit[0]);
        if (keyword && last != ' ' && !no_space_char(last)) space = true;
        if (space) list_putc(' ');
        list_puts(unit);
        last = (uint8_t)unit[strlen(unit) - 1];
        pending = keyword;
    }
}

/* BLLIST's line: "<number> <statements>". */
static void bl_list_line(uint16_t number, const uint8_t *content, uint8_t len) {
    char text[12];
    format_uint(number, text);
    bl_puts(text);
    bl_putc(' ');
    list_line_text(content, len);
    for (uint16_t i = 0; i < list_len; i++) bl_putc((uint8_t)list_text[i]);
    bl_putc(CR);
}

static void llist_line(uint16_t number, const uint8_t *content, uint8_t len);
static uint8_t plot_finish(void);

/* The end of a listing: BLLIST's text sent, or LLIST's drawing -- or
 * LLIST's ERROR 11 for a label no line has (p.119). */
static uint8_t list_finish(void) {
    if (kw.id != KW_LLIST) return bl_finish();
    return kw.label_wanted ? error(11) : plot_finish();
}

/* LLIST "label": does this line start with it? */
static bool line_has_label(const uint8_t *content, uint8_t len) {
    return len >= kw.label_len + 2 && content[0] == '"' && memcmp(content + 1, kw.label, kw.label_len) == 0 &&
           content[kw.label_len + 1] == '"';
}

static uint8_t bllist(void) {
    const value_t *v;
    kw.start = 0;
    kw.end = 0xFFFF;
    if (skip() != CR) {
        if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.start)) return fail();
        if (skip() == ',') {
            kw.pos++;
            if (!expr(&v) || !to_uint(v, 0xFFFF, &kw.end)) return fail();
        }
        if (skip() != CR) return fail();
    }
    return copy_in(BASIC_START_PTR, 4, ST_BL_LIST_PTRS);
}

/* The next piece of the program: from kw.call up to its last byte, kw.var. */
static uint8_t bl_list_read(void) {
    uint32_t left;
    if (kw.call > kw.var) return list_finish();
    left =(uint32_t)kw.var - kw.call + 1;
    kw.list_len = left > EXP_MAX_TRANSFER_LEN ? EXP_MAX_TRANSFER_LEN : (uint16_t)left;
    return copy_in(kw.call, kw.list_len, ST_BL_LIST_CHUNK);
}

static uint8_t bl_list_ptrs(void) {
    kw.call = (uint16_t)((W[0] << 8) | W[1]);
    kw.var = (uint16_t)((W[2] << 8) | W[3]);
    bl_len = 0;
    bl_failed = false;
    return bl_list_read();
}

/* Whole lines from this piece; one cut off at its end is read again from
 * its start next time. A line is [number hi][lo][size][size bytes ending
 * in CR]; FF follows the last one (TRM sec.5-3-5). */
static uint8_t bl_list_chunk(void) {
    uint16_t p = 0, len = kw.list_len;
    memcpy(bl_chunk, W, len);
    while (p < len && bl_chunk[p] != 0xFF) {
        uint16_t number;
        uint8_t size;
        if (p + 3 > len || p + 3 + bl_chunk[p + 2] > len) break;
        number = (uint16_t)((bl_chunk[p] << 8) | bl_chunk[p + 1]);
        size = bl_chunk[p + 2];
        if (kw.label_wanted && kw.id == KW_LLIST) {
            if (size == 0 || !line_has_label(bl_chunk + p + 3, (uint8_t)(size - 1))) {
                p = (uint16_t)(p + 3 + size);
                continue;
            }
            kw.label_wanted = false;
            kw.start = number;
            if (!kw.label_open) kw.end = number;
        }
        if (number > kw.end) return list_finish();
        if (number >= kw.start && size > 0) {
            if (kw.id == KW_LLIST) llist_line(number, bl_chunk + p + 3, (uint8_t)(size - 1));
            else bl_list_line(number, bl_chunk + p + 3, (uint8_t)(size - 1));
        }
        if (bl_failed) return error(40);
        p = (uint16_t)(p + 3 + size);
    }
    if (p < len && bl_chunk[p] == 0xFF) return list_finish();
    if (p == 0) return list_finish(); /* a line that can't be whole: malformed, stop */
    kw.call = (uint16_t)(kw.call + p);
    return bl_list_read();
}

/* ---- the CE-150 printer/plotter (2026-09-30, plotter.h) ----
 *
 * Its fifteen keywords, drawn on the BLE peer as PLOT operations
 * (BLE_PROTOCOL.md) instead of on paper, for when no CE-150 is attached (one
 * that is has its own keywords found first). What they do is the Owner's
 * Manual's (pp.116-128), and where that doesn't say, the CE-150 ROM's
 * (CE-150.asm): ERROR 19 for a value out of range, ERROR 73 for a command in
 * the wrong mode -- LF, LCURSOR and TAB are TEXT only, SORGN and ROTATE
 * GRAPH only (LB102/LB0FC), and TEXT and GRAPH set CSIZE 2. No link means no
 * printer: ERROR 27, the base ROM's own, before anything else.
 *
 * Not done yet: LPRINT USING, LLIST "label", and the CE-150's bare-LPRINT
 * quirk in GRAPH mode (p.123: the pen moves but the GRAPH counters don't). */

#define ERR_RANGE 19
#define ERR_NO_PRINTER 27
#define ERR_MODE 73
#define ERR_TOO_LONG 76 /* a number wider than the line */

static bool plot_send(const uint8_t *payload, uint16_t len, void *ctx) {
    (void)ctx;
    W[0] = (uint8_t)(len >> 8);
    W[1] = (uint8_t)len;
    memcpy(W + 2, payload, len);
    return run(EXP_COMMAND_BLE_PLOT) == EXP_STATUS_SUCCESS;
}

static uint8_t plot_finish(void) { return plot_end() ? done() : error(ERR_NO_PRINTER); }

/* A number in min..max into *out; else false, with *status the evaluation
 * still to do, ERROR 1 (none there, or a string) or ERROR 19. */
static bool num_arg(int32_t min, int32_t max, int32_t *out, uint8_t *status) {
    const value_t *v;
    if (!expr(&v)) {
        *status = fail();
        return false;
    }
    if (is_string(v) || !to_int(v, min, max, out)) {
        *status = error(is_string(v) ? 1 : ERR_RANGE);
        return false;
    }
    return true;
}

/* "(x,y)", each in min..max. */
static bool point_arg(int32_t min, int32_t max, int32_t *x, int32_t *y, uint8_t *status) {
    *status = error(1);
    if (skip() != '(') return false;
    kw.pos++;
    if (!num_arg(min, max, x, status)) return false;
    *status = error(1);
    if (skip() != ',') return false;
    kw.pos++;
    if (!num_arg(min, max, y, status)) return false;
    *status = error(1);
    if (skip() != ')') return false;
    kw.pos++;
    return true;
}

/* LINE [(x,y)]-(x,y)...[,[type][,[colour][,B]]] -- from the first point (or,
 * with the leading '-', from the pen) through each of up to six points, in
 * line type 0-9 and pen 0-3 (left out: the last ones used). RLINE's points
 * are each relative to the one before, the first to the pen. B draws the box
 * whose diagonal the two points (or the pen and one point) are. */
static uint8_t plot_lines(bool relative) {
    int32_t xs[6], ys[6], type = plot_line_type(), color = plot_color();
    uint8_t n = 0, status;
    bool from_pen = false, box = false;
    if (skip() == '-') {
        from_pen = true;
        kw.pos++;
    }
    for (;;) {
        if (n == 6) return error(1);
        if (!point_arg(-PLOT_COORD_MAX - 1, PLOT_COORD_MAX, &xs[n], &ys[n], &status)) return status;
        n++;
        if (skip() != '-') break;
        kw.pos++;
    }
    if (skip() == ',') {
        kw.pos++;
        if (skip() != ',' && cur() != CR && !num_arg(0, 9, &type, &status)) return status;
        if (skip() == ',') {
            kw.pos++;
            if (skip() != ',' && cur() != CR && !num_arg(0, 3, &color, &status)) return status;
            if (skip() == ',') {
                kw.pos++;
                if (!word("B")) return error(1);
                box = true;
            }
        }
    }
    if (skip() != CR || (box && n != (from_pen ? 1 : 2))) return error(1);
    plot_begin(plot_send, NULL);
    plot_set_line_type((uint8_t)type);
    plot_set_pen((uint8_t)color);
    {
        int32_t x, y, ox, oy, ax, ay;
        plot_origin(&ox, &oy);
        plot_position(&x, &y);
        ax = x;
        ay = y;
        for (uint8_t i = 0; i < n; i++) {
            if (relative) {
                x += xs[i] * PLOT_Q;
                y += ys[i] * PLOT_Q;
            } else {
                x = ox + xs[i] * PLOT_Q;
                y = oy + ys[i] * PLOT_Q;
            }
            if (i == 0 && !from_pen) {
                plot_move(x, y);
                ax = x;
                ay = y;
            } else if (!box) {
                plot_line(x, y);
            }
        }
        if (box) { /* across, up, back and down, as the manual's p.128 */
            plot_line(x, ay);
            plot_line(x, y);
            plot_line(ax, y);
            plot_line(ax, ay);
        }
    }
    return plot_finish();
}

/* ---- LPRINT ---- */

static value_t lp_items[MAX_EVALS];
static uint8_t lp_seps[MAX_EVALS]; /* what follows each: ';', ',' or CR */

/* A value as LPRINT shows it: a string's characters, a number as STR$. */
static uint8_t item_text(const value_t *v, char *out) {
    if (is_string(v)) {
        memcpy(out, v->text, v->len);
        out[v->len] = 0;
        return v->len;
    }
    if (v->literal_number) return format_uint(v->number, out);
    format_number(v->reg, out);
    return (uint8_t)strlen(out);
}

/* Characters at the pen; with `wrap`, carrying on at the start of the next
 * line when this one is full. */
static void lp_put(const char *text, uint8_t len, bool wrap) {
    for (uint8_t i = 0; i < len; i++) {
        if (wrap && plot_column() >= plot_columns()) plot_newline();
        plot_char((uint8_t)text[i]);
    }
}

/* One item on its own: a string from the pen on, a number right-justified
 * (on the next line if the pen is already past where it would start). */
static void lp_single(const value_t *v) {
    char text[VALUE_TEXT_MAX + 1];
    uint8_t len = item_text(v, text), cols = plot_columns();
    if (is_string(v)) {
        lp_put(text, len, true);
        return;
    }
    if (plot_column() > cols - len) plot_newline();
    plot_set_column((uint8_t)(cols - len));
    lp_put(text, len, false);
}

/* An item justified in `width` columns from `base`. */
static void lp_half(const value_t *v, uint8_t base, uint8_t width) {
    char text[VALUE_TEXT_MAX + 1];
    uint8_t len = item_text(v, text);
    plot_set_column((uint8_t)(is_string(v) ? base : base + width - len));
    lp_put(text, len, false);
}

/* ---- LPRINT USING (2026-10-01, Owner's Manual pp.80-83, 123) ----
 *
 * The CE-150 takes USING only in GRAPH mode. A format holds fields: a run
 * of '&' takes a string, left-justified and cut to fit; a run of # * . , ^
 * + takes a number: right-justified in the #s and *s before the point, one
 * of which is the sign's ('-', or '+' with a '+' in the field); the #s after
 * the point are its decimals, the rest cut off (the manual's PI is 3.141);
 * a '*' fills the empty digit positions with stars; a ',' puts commas
 * between thousands; a '^' shows it as 3.14E 00. A number too wide for its
 * field is ERROR 36. Items take the fields in turn, round again after the
 * last; an item that doesn't match its field's kind is shown as without
 * USING. Other characters in the format are ignored. The format lasts
 * until the next USING, also in later LPRINTs; USING alone ends it. */

#define ERR_USING 36

static char lp_using[VALUE_TEXT_MAX];
static uint8_t lp_using_len; /* 0: no format */
static uint8_t lp_using_at;  /* where the next field is looked for */

static bool is_number_edit(char c) { return c == '#' || c == '*' || c == '.' || c == ',' || c == '^' || c == '+'; }

/* The format's next field: [*start, +*len), and whether it's a string one;
 * false if the format has none. */
static bool next_field(uint8_t *start, uint8_t *len, bool *string) {
    for (int pass = 0; pass < 2; pass++) {
        uint8_t i = lp_using_at;
        while (i < lp_using_len && lp_using[i] != '&' && !is_number_edit(lp_using[i])) i++;
        if (i < lp_using_len) {
            const bool s = lp_using[i] == '&';
            uint8_t j = i;
            while (j < lp_using_len && (s ? lp_using[j] == '&' : is_number_edit(lp_using[j]))) j++;
            *start = i;
            *len = (uint8_t)(j - i);
            *string = s;
            lp_using_at = j;
            return true;
        }
        lp_using_at = 0;
    }
    return false;
}

/* A number as a sign and ten decimal digits d0.d1d2... times 10^*exp. */
static void number_digits(const value_t *v, bool *neg, uint8_t d[10], int *exp) {
    memset(d, 0, 10);
    *neg = false;
    *exp = 0;
    if (v->literal_number || v->reg[4] == AR_BINARY) {
        int32_t s = v->literal_number ? (int32_t)v->number : (int16_t)((v->reg[5] << 8) | v->reg[6]);
        char t[12];
        uint8_t len;
        if (s < 0) {
            *neg = true;
            s = -s;
        }
        len = format_uint((uint32_t)s, t);
        for (uint8_t i = 0; i < len && i < 10; i++) d[i] = (uint8_t)(t[i] - '0');
        *exp = s ? len - 1 : 0;
        return;
    }
    for (uint8_t i = 0; i < 10; i++) d[i] = (i & 1) ? (uint8_t)(v->reg[2 + i / 2] & 0x0F) : (uint8_t)(v->reg[2 + i / 2] >> 4);
    *exp = (int8_t)v->reg[0];
    *neg = (v->reg[1] & 0x80) != 0;
}

/* A number in the numeric field f[0..flen) appended to out at *n; false
 * if it doesn't fit (ERROR 36). */
static bool using_number(const value_t *v, const char *f, uint8_t flen, char *out, uint16_t *n) {
    bool neg, star = false, plus = false, point = false, comma = false, sci = false, zero = true;
    uint8_t before = 0, after = 0, d[10], len = 0;
    char text[48];
    int e;
    for (uint8_t i = 0; i < flen; i++) {
        const char c = f[i];
        if (c == '#' || c == '*') {
            if (point) after++;
            else before++;
            star = star || c == '*';
        }
        point = point || c == '.';
        comma = comma || c == ',';
        sci = sci || c == '^';
        plus = plus || c == '+';
    }
    number_digits(v, &neg, d, &e);
    for (uint8_t i = 0; i < 10; i++)
        if (d[i]) zero = false;
    if (zero) {
        neg = false;
        e = 0;
    }
    if (sci) { /* d0[.d1...]E sxx */
        int a = e < 0 ? -e : e;
        text[len++] = (char)('0' + d[0]);
        if (point) text[len++] = '.';
        for (uint8_t k = 1; k <= after && k < 10; k++) text[len++] = (char)('0' + d[k]);
        text[len++] = 'E';
        text[len++] = e < 0 ? '-' : ' ';
        text[len++] = (char)('0' + a / 10 % 10);
        text[len++] = (char)('0' + a % 10);
    } else {
        const int whole = zero || e < 0 ? 0 : e + 1; /* digits before the point */
        if (whole == 0) text[len++] = '0';
        for (int k = 0; k < whole; k++) {
            if (comma && k > 0 && (whole - k) % 3 == 0) text[len++] = ',';
            if (len >= sizeof text - 12) return false;
            text[len++] = (char)('0' + (k < 10 ? d[k] : 0));
        }
        if (before == 0 || len > before - 1) return false; /* one position is the sign's */
        if (point) {
            text[len++] = '.';
            for (uint8_t k = 1; k <= after; k++) {
                const int at = e + k;
                text[len++] = (char)('0' + (at >= 0 && at < 10 ? d[at] : 0));
            }
        }
    }
    {
        const char sign = neg ? '-' : plus ? '+' : ' ';
        const uint8_t width = (uint8_t)(before + (point ? 1 + after : 0));
        int pad = (int)width - 1 - len;
        if (star) { /* the sign's position, then the stars */
            out[(*n)++] = sign;
            for (; pad > 0; pad--) out[(*n)++] = '*';
        } else {
            for (; pad > 0; pad--) out[(*n)++] = ' ';
            out[(*n)++] = sign;
        }
        memcpy(out + *n, text, len);
        *n = (uint16_t)(*n + len);
    }
    return true;
}

/* A string in a field of `width` '&'s. */
static void using_string(const value_t *v, uint8_t width, char *out, uint16_t *n) {
    for (uint8_t i = 0; i < width; i++) out[(*n)++] = i < v->len ? (char)v->text[i] : ' ';
}

/* What each LPRINT item is. */
enum { LP_VALUE, LP_USING, LP_USING_OFF };
static uint8_t lp_kinds[MAX_EVALS];

#define LP_GRAPH_MAX 320

/* GRAPH mode's text: the items in turn, through the USING format when one
 * is in force; ERROR 36 before anything is drawn. *n is its length. */
static uint8_t lp_graph_text(uint8_t count, char *out, uint16_t *n) {
    uint8_t shown = 0; /* values so far */
    *n = 0;
    for (uint8_t i = 0; i < count; i++) {
        const value_t *v = &lp_items[i];
        uint8_t start, len;
        bool string;
        if (lp_kinds[i] == LP_USING) {
            lp_using_len = v->len;
            memcpy(lp_using, v->text, v->len);
            lp_using_at = 0;
            continue;
        }
        if (lp_kinds[i] == LP_USING_OFF) {
            lp_using_len = 0;
            continue;
        }
        if (*n > LP_GRAPH_MAX - VALUE_TEXT_MAX - 50) break;
        shown++;
        if (lp_using_len && next_field(&start, &len, &string)) {
            if (string && is_string(v)) {
                using_string(v, len, out, n);
                continue;
            }
            if (!string && !is_string(v)) {
                if (!using_number(v, lp_using + start, len, out, n)) return ERR_USING;
                continue;
            }
            lp_using_at = start; /* not its kind: shown as without USING, the field kept */
        }
        {
            uint8_t l = item_text(v, out + *n + 1);
            if (shown > 1 && !is_string(v) && out[*n + 1] != '-') out[(*n)++] = ' ';
            else memmove(out + *n, out + *n + 1, l);
            *n = (uint16_t)(*n + l);
        }
    }
    return 0;
}

/* LPRINT [TAB n;][item[{;|,}item...][;|,]]
 * TEXT mode (p.122): one item alone is justified -- strings left, numbers
 * right; two items with ',' share the line's halves at CSIZE 1 (if they
 * fit), else take a line each; items with ';' run on, a number after the
 * first getting a space before it, wrapping at the end of the line. A
 * number wider than the line is ERROR 76. A trailing ';' or ',' leaves the
 * line open; LPRINT alone is a carriage return and line feed.
 * GRAPH mode: the items run on from the pen in the ROTATE direction, with
 * USING "format" (or USING alone, to end one) among them (above); LPRINT
 * alone is a carriage return and line feed that the GRAPH counters don't
 * see (p.123, plot_graph_newline()). USING in TEXT mode is ERROR 73. */
static uint8_t lprint(void) {
    char text[LP_GRAPH_MAX];
    int32_t tab = -1;
    uint8_t n = 0, status, cols = plot_columns();
    const bool graph = plot_graph_mode();
    bool open;
    if (word("TAB")) {
        if (graph) return error(ERR_MODE);
        if (!num_arg(0, cols - 1, &tab, &status)) return status;
        if (skip() != ';') return error(1);
        kw.pos++;
    }
    while (skip() != CR) {
        const value_t *v;
        uint8_t c;
        if (n == MAX_EVALS) return error(1);
        if (word("USING")) {
            if (!graph) return error(ERR_MODE);
            c = skip();
            if (c == ';' || c == ',' || c == CR) {
                lp_kinds[n] = LP_USING_OFF;
            } else {
                if (!expr(&v)) return fail();
                if (!is_string(v)) return error(1);
                lp_items[n] = *v;
                lp_kinds[n] = LP_USING;
            }
        } else {
            if (!expr(&v)) return fail();
            lp_items[n] = *v;
            lp_kinds[n] = LP_VALUE;
        }
        c = skip();
        if (c != ';' && c != ',' && c != CR) return error(1);
        lp_seps[n++] = c;
        if (c != CR) kw.pos++;
    }
    open = n > 0 && lp_seps[n - 1] != CR;
    if (graph) {
        char saved[VALUE_TEXT_MAX];
        uint8_t saved_len = lp_using_len, saved_at = lp_using_at;
        uint16_t len;
        memcpy(saved, lp_using, sizeof saved);
        if ((status = lp_graph_text(n, text, &len)) != 0) {
            memcpy(lp_using, saved, sizeof saved); /* not drawn: the format as it was */
            lp_using_len = saved_len;
            lp_using_at = saved_at;
            return error(status);
        }
        plot_begin(plot_send, NULL);
        if (n == 0) plot_graph_newline();
        for (uint16_t i = 0; i < len; i++) plot_char((uint8_t)text[i]);
        return plot_finish();
    }
    for (uint8_t i = 0; i < n; i++)
        if (!is_string(&lp_items[i]) && item_text(&lp_items[i], text) > cols) return error(ERR_TOO_LONG);
    plot_begin(plot_send, NULL);
    if (tab >= 0) plot_set_column((uint8_t)tab);
    if (n == 0) {
        plot_newline();
    } else if (n == 1) {
        lp_single(&lp_items[0]);
    } else if (n == 2 && lp_seps[0] == ',') {
        uint8_t half = (uint8_t)(cols / 2);
        if (plot_csize() == 1 && item_text(&lp_items[0], text) <= half && item_text(&lp_items[1], text) <= half) {
            if (plot_column() > 0) plot_newline();
            lp_half(&lp_items[0], 0, half);
            lp_half(&lp_items[1], half, half);
        } else {
            lp_single(&lp_items[0]);
            plot_newline();
            lp_single(&lp_items[1]);
        }
    } else {
        for (uint8_t i = 0; i < n; i++) {
            uint8_t len = item_text(&lp_items[i], text + 1);
            bool spaced = i > 0 && !is_string(&lp_items[i]) && text[1] != '-';
            text[0] = ' ';
            lp_put(spaced ? text : text + 1, (uint8_t)(len + spaced), true);
        }
    }
    if (n > 0 && !open) plot_newline();
    return plot_finish();
}

/* ---- LLIST ---- */

/* LLIST [n] | [n],[m] | "label"[,] -- the program, line n alone, or lines n
 * to m (either left out: from the start / to the end); the line that starts
 * with "label" alone, or from it to the end (ERROR 11 if there's none). In
 * TEXT mode at CSIZE 1 or 2 (p.118-119). Shares BLLIST's reading of the
 * program (bl_list_read()). */
static uint8_t llist(void) {
    int32_t a, b;
    uint8_t status, size;
    kw.start = 0;
    kw.end = 0xFFFF;
    kw.label_wanted = false;
    if (skip() != CR) {
        bool first = cur() != ',';
        if (first) {
            const value_t *v;
            if (!expr(&v)) return fail();
            if (is_string(v)) {
                if (v->len == 0) return error(1);
                kw.label_wanted = true;
                kw.label_open = false;
                kw.label_len = v->len;
                memcpy(kw.label, v->text, v->len);
            } else {
                if (!to_int(v, 0, 0xFFFF, &a)) return error(ERR_RANGE);
                kw.start = kw.end = (uint16_t)a;
            }
        }
        if (skip() == ',') {
            kw.pos++;
            if (first) kw.end = 0xFFFF;
            kw.label_open = true;
            if (skip() != CR) {
                if (kw.label_wanted) return error(1);
                if (!num_arg(0, 0xFFFF, &b, &status)) return status;
                kw.end = (uint16_t)b;
            }
        } else if (!first) {
            return error(1);
        }
        if (skip() != CR) return error(1);
    }
    plot_begin(plot_send, NULL);
    size = plot_csize();
    if (plot_graph_mode()) plot_text_mode();
    plot_set_csize(size > 2 ? 2 : size);
    if (plot_column() > 0) plot_newline();
    return copy_in(BASIC_START_PTR, 4, ST_BL_LIST_PTRS);
}

/* One line: its number right-justified in 3 columns (5 for 4-5 digits),
 * ':', and its statements, continued under themselves when they wrap. */
static void llist_line(uint16_t number, const uint8_t *content, uint8_t len) {
    char text[12];
    uint8_t digits = format_uint(number, text), field = digits <= 3 ? 3 : 5, cols = plot_columns();
    uint8_t indent = (uint8_t)(field + 2 < cols ? field + 2 : 0);
    plot_set_column((uint8_t)(field - digits));
    lp_put(text, digits, false);
    lp_put(": ", 2, false);
    list_line_text(content, len);
    for (uint16_t i = 0; i < list_len; i++) {
        if (plot_column() >= cols) {
            plot_newline();
            plot_set_column(indent);
        }
        plot_char((uint8_t)list_text[i]);
    }
    plot_newline();
}

/* ---- the rest ---- */

static uint8_t plotter_keyword(void) {
    int32_t n = 0, x = 0, y = 0, min = 0, max = 0;
    uint8_t status;
    const bool text_only = kw.id == KW_LF || kw.id == KW_LCURSOR || kw.id == KW_TAB;
    const bool graph_only = kw.id == KW_SORGN || kw.id == KW_ROTATE;
    if (!(bl_status() & EXP_BLE_STATUS_LINKED)) return error(ERR_NO_PRINTER);
    switch (kw.id) {
        case KW_LINE: return plot_lines(false);
        case KW_RLINE: return plot_lines(true);
        case KW_LPRINT: return lprint();
        case KW_LLIST: return llist();
        default: break;
    }
    if ((text_only && plot_graph_mode()) || (graph_only && !plot_graph_mode())) return error(ERR_MODE);
    switch (kw.id) {
        case KW_COLOR:
        case KW_ROTATE: max = 3; break;
        case KW_CSIZE: min = 1, max = 9; break;
        case KW_LF: min = -255, max = 255; break;
        case KW_LCURSOR:
        case KW_TAB: max = plot_columns() - 1; break;
        default: break;
    }
    if (max > 0) {
        if (!num_arg(min, max, &n, &status)) return status;
    } else if (kw.id == KW_GLCURSOR) {
        if (!point_arg(-PLOT_COORD_MAX, PLOT_COORD_MAX, &x, &y, &status)) return status;
    }
    if (skip() != CR) return error(1);
    plot_begin(plot_send, NULL);
    switch (kw.id) {
        case KW_COLOR: plot_color_command((uint8_t)n); break;
        case KW_CSIZE: plot_set_csize((uint8_t)n); break;
        case KW_ROTATE: plot_set_rotate((uint8_t)n); break;
        case KW_LF: plot_feed(n); break;
        case KW_LCURSOR:
        case KW_TAB: plot_set_column((uint8_t)n); break;
        case KW_SORGN: plot_set_origin(); break;
        case KW_GRAPH: plot_graph_mode_on(); break;
        case KW_TEXT: plot_text_mode(); break;
        case KW_TEST: plot_test(); break;
        case KW_GLCURSOR: {
            int32_t ox, oy;
            plot_origin(&ox, &oy);
            plot_move(ox + x * PLOT_Q, oy + y * PLOT_Q);
            break;
        }
        default: break;
    }
    return plot_finish();
}

void kw_reset(void) {
    have_last_load = false;
    memset(&kw, 0, sizeof kw);
    plot_reset();
    basic_xlate_end();
    lp_using_len = 0;
}

bool kw_in_progress(void) { return kw.step != ST_NONE; }

uint8_t kw_function(uint8_t command, uint8_t *window, kw_command_fn run_command, void *ctx) {
    uint8_t *saved_w = W; /* a function may be evaluated inside a keyword */
    kw_command_fn saved_fn = run_fn;
    void *saved_ctx = run_ctx;
    uint8_t ok = 0, error = 1;
    W = window;
    run_fn = run_command;
    run_ctx = ctx;
    if (command == EXP_COMMAND_FN_BLSTAT) ok = blstat_value(&error);
    else if (command == EXP_COMMAND_FN_SDEOF) ok = sdeof_value(&error);
    W[EXP_FN_ERROR] = error;
    W[EXP_FN_END_OF_KEYWORD] = kw_in_progress() ? 0 : 1; /* the ROM then sends DONE */
    W = saved_w;
    run_fn = saved_fn;
    run_ctx = saved_ctx;
    return ok ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
}

uint8_t kw_command(uint8_t command, uint8_t *window, kw_command_fn run_command, void *ctx) {
    uint8_t step;
    W = window;
    run_fn = run_command;
    run_ctx = ctx;
    if (command == EXP_COMMAND_KEYWORD) {
        if (window[0] != TOKEN_HIGH && !(window[0] == TOKEN_CE150 && window[1] >= KW_COLOR && window[1] <= KW_TEST))
            return EXP_STATUS_ERROR;
        kw.id = window[1];
        W[W_ACTION + EXP_KW_END] = detokenize(window + 2);
        kw.nevals = 0;
        return begin();
    }
    step = kw.step;
    kw.step = ST_NONE;
    return resume(step, window[W_ACTION + EXP_KW_ANSWER]);
}
