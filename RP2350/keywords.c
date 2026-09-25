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

#include "pc_exp.h"

#define W_LENGTH_PORT (EXP_LENGTH_PORT_PAGE * 256 + EXP_LENGTH_PORT_ADDRESS)
#define W_SCRATCH (EXP_SCRATCH_PAGE * 256)
#define W_ACTION (EXP_KW_ACTION_PAGE * 256 + EXP_KW_ACTION_ADDRESS)
#define LINE_WIDTH 26 /* one LCD line */
#define CR 0x0D
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
};

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
};

enum { LOAD_BASIC, LOAD_M_HEADER, LOAD_M_EXPLICIT };
enum { SAVE_BASIC, SAVE_M };

/* Room for the statement with every token expanded (a 2-byte token
 * becomes up to 8 characters). */
#define KW_TEXT_MAX 200
#define MAX_EVALS 6
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
} kw;

static uint8_t *W;
static kw_command_fn run_fn;
static void *run_ctx;

static uint8_t run(uint8_t command) { return run_fn(command, run_ctx); }

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
static uint8_t open_and_load(void) {
    uint16_t target = kw.start, call;
    uint8_t flags;
    if (run(EXP_COMMAND_OPEN_SD_FILE_READ) != EXP_STATUS_SUCCESS) return error(40);
    if (kw.mode == LOAD_BASIC) return action(EXP_KW_ACTION_LOAD, EXP_KW_XFER_BASIC, 0, 0, ST_NONE);
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
        run(EXP_COMMAND_LIST_SD_DIR);
        return browse(EXP_KW_BROWSE_SELECT, ST_LOAD_PICK);
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

static uint8_t save_create(void) {
    memcpy(W, kw.names, NAME_SLOT);
    if (run(EXP_COMMAND_CREATE_SD_FILE) != EXP_STATUS_SUCCESS) return done();
    if (kw.mode == SAVE_BASIC) return action(EXP_KW_ACTION_SAVE, EXP_KW_XFER_BASIC, 0, 0, ST_NONE);
    W[W_LENGTH_PORT] = 0;
    W[W_LENGTH_PORT + 1] = 4;
    W[0] = (uint8_t)(kw.start >> 8);
    W[1] = (uint8_t)kw.start;
    W[2] = (uint8_t)(kw.call >> 8);
    W[3] = (uint8_t)kw.call;
    if (run(EXP_COMMAND_WRITE_TO_SD_FILE) != EXP_STATUS_SUCCESS) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        return done();
    }
    return action(EXP_KW_ACTION_SAVE, 0, kw.start, kw.end, ST_NONE);
}

/* Asks before overwriting an existing file unless -Y. */
static uint8_t create_and_write(void) {
    memcpy(kw.names, W, NAME_SLOT);
    if (run(EXP_COMMAND_OPEN_SD_FILE_READ) == EXP_STATUS_SUCCESS) {
        run(EXP_COMMAND_CLOSE_SD_FILE);
        if (!kw.yflag) return show_str("FILE EXISTS. OVERWRITE Y/N", ST_SAVE_CONFIRM);
    }
    return save_create();
}

/* SDSAVE name[,-Y]                        the BASIC program
 * SDSAVE M name,start,end[,call][,-Y]     start..end inclusive, with a
 *                                         [start][call] header */
static uint8_t sdsave(void) {
    const value_t *v;
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
        /* already staged and verified: nothing to copy */
        if (run(EXP_COMMAND_ROM_GET_MODE) == EXP_STATUS_SUCCESS && W[1] == 1) return show_str("STAGE: OK", ST_FINISH);
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

/* Setting names as typed; the index is the MCU_CONFIG_* number
 * (mcu_config.h). */
static const struct {
    const char *name;
    uint16_t max;
} kSettings[] = {
    {"LED", 1},
    {"SLEEPWAIT", 65535},
};
#define SETTING_COUNT (sizeof kSettings / sizeof kSettings[0])

static bool config_get(uint8_t id, uint16_t *value) {
    W[0] = id;
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

/* MCONF             browse every setting
 * MCONF NAME        show one
 * MCONF NAME=value  set one (saved in the MCU's flash) */
static uint8_t mconf(void) {
    const value_t *v;
    uint8_t id, text[LINE_WIDTH];
    uint16_t value;
    if (skip() == CR) {
        /* A listing in LIST_SD_DIR's shape: count, 30-byte records (the
         * first 26 bytes are the displayed line), then a summary line. */
        uint8_t line[LINE_WIDTH], count = 0;
        uint16_t values[SETTING_COUNT];
        bool have[SETTING_COUNT];
        /* all reads first: each one uses window bytes 0-2 */
        for (id = 0; id < SETTING_COUNT; id++) have[id] = config_get(id, &values[id]);
        for (id = 0; id < SETTING_COUNT; id++) {
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
    if (id == SETTING_COUNT) return error(1);
    if (cur() == CR) {
        if (!config_get(id, &value)) return error(1);
        return show(text, format_setting(id, value, text), ST_FINISH);
    }
    kw.pos++; /* '=' */
    if (!expr(&v) || !to_uint(v, kSettings[id].max, &value) || skip() != CR) return fail();
    W[0] = id;
    W[1] = (uint8_t)(value >> 8);
    W[2] = (uint8_t)value;
    return run(EXP_COMMAND_CONFIG_SET) == EXP_STATUS_SUCCESS ? done() : error(1);
}

/* Parses the statement from the start (again, after each evaluation). */
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
        default: return EXP_STATUS_ERROR;
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
        case ST_SAVE_CONFIRM: return answer == KEY_Y ? save_create() : done();
        case ST_LOAD_PICK: return load_picked(answer);
        case ST_PRINT_VALUE: return print_value();
        case ST_INPUT_LOOKUP: return input_looked_up();
        case ST_INPUT_VAR: return skip() == ',' ? input_next() : done();
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
            uint16_t code = (uint16_t)((c << 8) | src[in + 1]);
            const char *text = NULL;
            for (size_t i = 0; i < sizeof kBasicTokens / sizeof kBasicTokens[0]; i++)
                if (kBasicTokens[i].code == code) text = kBasicTokens[i].text;
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

uint8_t kw_command(uint8_t command, uint8_t *window, kw_command_fn run_command, void *ctx) {
    uint8_t step;
    W = window;
    run_fn = run_command;
    run_ctx = ctx;
    if (command == EXP_COMMAND_KEYWORD) {
        if (window[0] != TOKEN_HIGH) return EXP_STATUS_ERROR;
        kw.id = window[1];
        W[W_ACTION + EXP_KW_END] = detokenize(window + 2);
        kw.nevals = 0;
        return begin();
    }
    step = kw.step;
    kw.step = ST_NONE;
    return resume(step, window[W_ACTION + EXP_KW_ANSWER]);
}
