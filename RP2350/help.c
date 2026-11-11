/* help.c -- see help.h.
 *
 * BASIC's lines follow the PC-1500 Owner's Manual (its Command Reference
 * Table, Appendix Z) and the CE-150's chapter; the CE-158's follow
 * github.com/tinue/SharpBasicReference (COM$ and DEV$ are inferred from its
 * SETCOM s$ and SETDEV s$ forms). This module's follow
 * keywords.c's own comments and the README. One line per name: the CE-150
 * stand-ins (CSIZE, GRAPH, ...) are the CE-150's own lines.
 *
 * Optional parts in [ ], alternatives split by /. The PC-1500's own [ and
 * ] are square root and pi, and its apostrophe a box: the window gets the
 * ROM's TERM glyphs for them instead (pc_exp.h EXP_TERM_GLYPH_*). help_host_test.c refuses the other
 * characters that aren't ASCII's on its display. */
#include "help.h"

#include <ctype.h>
#include <string.h>

#include "kbd_seq.h"
#include "pc_exp.h"

#define REPEAT_DELAY_MS 500
#define REPEAT_MS 80
#define IND_SMALL 0x08     /* 764EH's SMALL: BASIC's own, left as it was */
#define TERM_SAVED_IND 0x1D /* the ROM's copy of 764EH (rom_defs.inc) */


static const help_entry_t k_help[] = {
    {"ABS", "ABS x: the absolute value of x"},
    {"ACS", "ACS x: the arc cosine of x, in the angle mode set"},
    {"AND", "a AND b: logical AND (bitwise on integers)"},
    {"AREAD", "AREAD v: after a DEF key's label, puts the value displayed into v"},
    {"ARUN", "ARUN: as a program's first statement, runs it when the PC-1500 is turned on"},
    {"ASC", "ASC s$: the character code of s$'s first character"},
    {"ASN", "ASN x: the arc sine of x"},
    {"ATN", "ATN x: the arc tangent of x"},
    {"BANK", "BANK [n]: switches to SRAM bank n (0-4) with its program; none: shows it (card)"},
    {"BANKRTN", "BANKRTN: back to the bank and statement after the last BANKSUB"},
    {"BANKSUB", "BANKSUB n,line/\"label\": a GOSUB into bank n's program; BANKRTN returns"},
    {"BEEP", "BEEP n[,tone,length] / BEEP ON/OFF: sounds n beeps; OFF silences BEEP"},
    {"BLADV", "BLADV: advertises over BLE and waits for another PC-1500's BLSCAN or BLCON"},
    {"BLCLC", "BLCLC: clears the BLE peer's console"},
    {"BLCLS", "BLCLS: clears the BLE peer's screen pane"},
    {"BLCON", "BLCON name$: connects to the BLE peer advertising that name"},
    {"BLDISC", "BLDISC: disconnects the BLE link"},
    {"BLGET", "BLGET name$[,-Y]: a file from the BLE peer onto the SD card as name$"},
    {"BLKBD", "BLKBD [FORGET/?]: pairs a Bluetooth keyboard; FORGET forgets it; ? its state"},
    {"BLKEY$", "BLKEY$: as INKEY$, the key down now on either keyboard, or \"\""},
    {"BLLIST", "BLLIST [from[,to]]: lists the program as text to the BLE peer"},
    {"BLLOAD", "BLLOAD [M] [name$[,addr]]: as SDLOAD, from the BLE peer's files"},
    {"BLMSGS", "BLMSGS: BLE messages waiting (0-8), or -1 with no link and none waiting"},
    {"BLPAIR", "BLPAIR name$: pairs with the BLE peer advertising that name"},
    {"BLPRINT", "BLPRINT [v[;/,v...]]: as PRINT, to the BLE peer's text window"},
    {"BLPUT", "BLPUT M start,end[,call] / BLPUT SD [M] name$: memory or a card file to the peer"},
    {"BLRECV", "BLRECV [#t,]v[,v...]: the oldest BLE message's values; waits up to t s"},
    {"BLSAVE", "BLSAVE [M] name$[,...][,-Y]: as SDSAVE, to the BLE peer's files"},
    {"BLSCAN", "BLSCAN [s]: scans for BLE peers (default 3 s); pick one to connect"},
    {"BLSEND", "BLSEND v[,v...]: numbers and strings to the BLE peer, as one message"},
    {"BLSTAT", "BLSTAT: the BLE link: 0 off, 1 advertising, 2 linked"},
    {"BLUNPAIR", "BLUNPAIR name$: forgets the paired BLE peer with that name"},
    {"BREAK", "BREAK: only in TRANSMIT BREAK,n, a long space on RS-232C (CE-158)"},
    {"CALL", "CALL addr[,v]: runs the machine code at addr"},
    {"CHAIN", "CHAIN [\"name\"][,line]: loads a program from tape and runs it (CE-150)"},
    {"CHR$", "CHR$ n: the character whose code is n"},
    {"CLEAR", "CLEAR: clears all variables"},
    {"CLOAD", "CLOAD [\"name\"] / CLOAD? [\"name\"]: loads a program from tape; ? compares (CE-150)"},
    {"CLS", "CLS: clears the display"},
    {"COLOR", "COLOR n: the pen color, 0-3 (CE-150)"},
    {"COM$", "COM$: the RS-232C settings SETCOM made, as \"baud,bits,parity,stop\" (CE-158)"},
    {"CONSOLE", "CONSOLE n[,e1[,e2]]: RS-232C line width 0/16-255; end code 0 CR, 1 LF (CE-158)"},
    {"CONT", "CONT: continues a program halted by STOP or BREAK (RUN mode)"},
    {"COS", "COS x: the cosine of x"},
    {"CSAVE", "CSAVE [\"name\"]: saves the program and reserve keys to tape; RS-232C: SETDEV CO"},
    {"CSIZE", "CSIZE n: the printed character size, 1-9 (CE-150)"},
    {"CURSOR", "CURSOR [n]: the next PRINT starts at display column n (0-25); none: cancels"},
    {"DATA", "DATA v[,v...]: values for READ"},
    {"DEG", "DEG x: degrees.minutes-seconds (dd.mmss) to decimal degrees"},
    {"DEGREE", "DEGREE: angles in degrees"},
    {"DEV$", "DEV$: the SETDEV assignments in force, as a string (CE-158)"},
    {"DIM", "DIM a(n)[,b$(n,m)*len...]: declares arrays; *len a string element's length"},
    {"DMS", "DMS x: decimal degrees to degrees.minutes-seconds (dd.mmss)"},
    {"DTE", "DTE: terminal mode at 300,7,E,1, echo on, no XON/XOFF; ON: its menu (CE-158)"},
    {"ECINIT", "ECINIT: erases every expansion setting (asks Y/N), then restarts"},
    {"ECVER", "ECVER: shows the expansion ROM's version"},
    {"END", "END: ends the program"},
    {"ERL", "ERL: the line number where the last error happened"},
    {"ERN", "ERN: the last error's number"},
    {"ERROR", "ON ERROR GOTO line: on an error, jumps to line (see ERN, ERL)"},
    {"EXP", "EXP x: e to the power x"},
    {"FEED", "FEED [n]: sends n (1-65535) CONSOLE end codes, one if none (CE-158)"},
    {"FLDF", "FLDF: shows the room left in the FL store"},
    {"FLGET", "FLGET \"key\",v: the value kept under key into v (0 or \"\" if none)"},
    {"FLLS", "FLLS: browses the FL store's keys and their values"},
    {"FLRM", "FLRM \"key\": forgets key's value (no error if there was none)"},
    {"FLSET", "FLSET \"key\",value: keeps a number or string in flash under key"},
    {"FNCLR", "FNCLR: clears the reserve (function) key definitions"},
    {"FNLOAD", "FNLOAD: brings back the reserve key definitions FNSAVE kept"},
    {"FNSAVE", "FNSAVE: keeps the reserve key definitions in the MCU's flash"},
    {"FOR", "FOR v=a TO b [STEP c]: a loop, closed by NEXT v"},
    {"GCURSOR", "GCURSOR n: puts GPRINT's position at display dot column n (0-155)"},
    {"GLCURSOR", "GLCURSOR (x,y): moves the pen to x,y in GRAPH mode (CE-150)"},
    {"GOSUB", "GOSUB line/\"label\": calls a subroutine; RETURN comes back"},
    {"GOTO", "GOTO line/\"label\": jumps to that line or label"},
    {"GPRINT", "GPRINT \"hex\" / GPRINT n;n;...: draws dot columns on the display"},
    {"GRAD", "GRAD: angles in grads"},
    {"GRAPH", "GRAPH: the printer's mode for drawing (CE-150)"},
    {"HELP", "HELP [name/part]: help for a keyword, or every keyword whose name holds part"},
    {"HISTCLR", "HISTCLR: clears the command history, its flash erased too"},
    {"IF", "IF cond [THEN] statement/line: does the rest of the line only if cond is true"},
    {"INKEY$", "INKEY$: the key down now, or \"\""},
    {"INPUT", "INPUT [\"prompt\";]v[,...] / INPUT# v,...: asks for values; # reads tape"},
    {"INSTAT", "INSTAT: RS-232C signals: DSR 16, CD 8, CTS 4, RTS 2, DTR 1; a 1 is off (CE-158)"},
    {"INT", "INT x: x truncated to an integer"},
    {"LCURSOR", "LCURSOR n: moves the print position to column n in TEXT mode (CE-150)"},
    {"LEFT$", "LEFT$(s$,n): the first n characters of s$"},
    {"LEN", "LEN s$: the number of characters in s$"},
    {"LET", "LET v=expression: assigns a value; LET may be left out except after THEN"},
    {"LF", "LF n: feeds the paper n lines in TEXT mode (CE-150)"},
    {"LINE", "LINE [(x,y)]-(x,y)...[,type][,color][,B]: draws lines in GRAPH mode (CE-150)"},
    {"LIST", "LIST [line/\"label\"]: lists the program from there (PRO mode)"},
    {"LLIST", "LLIST [n][,m]: prints the program, or lines n to m (CE-150); RS-232C: SETDEV PO"},
    {"LN", "LN x: the natural logarithm of x"},
    {"LOCK", "LOCK: locks the mode (MODE key); UNLOCK undoes it"},
    {"LOG", "LOG x: the base-10 logarithm of x"},
    {"LPRINT", "LPRINT ...: as PRINT, on the printer (CE-150); RS-232C: SETDEV PO"},
    {"MCONF", "MCONF [NAME[=value]]: shows or sets an expansion setting, kept in flash"},
    {"MEM", "MEM: BASIC area bytes available (as STATUS 0)"},
    {"MERGE", "MERGE [\"name\"]: adds a tape program to the one in memory; RS-232C: SETDEV CI"},
    {"MID$", "MID$(s$,i,n): n characters of s$ from the i-th"},
    {"MLOG", "MLOG [VIEW/VERBOSE/QUIET/CLEAR]: the MCU's log: browse it, its detail, clear it"},
    {"MLOGMSG", "MLOGMSG s$: writes s$ into the MCU's log"},
    {"NEW", "NEW: deletes the program and variables (PRO mode); NEW0 clears all memory"},
    {"NEXT", "NEXT v: closes a FOR loop"},
    {"NOT", "NOT x: logical negation (bitwise on integers)"},
    {"OFF", "OFF: as in BEEP OFF and RMT OFF"},
    {"ON", "ON n GOTO/GOSUB a,b,...: to the n-th line or label; ON ERROR GOTO line"},
    {"OPN", "OPN \"LPRT\": LPRINT, LLIST, FEED, CONSOLE to the parallel port; OPN: off (CE-158)"},
    {"OR", "a OR b: logical OR (bitwise on integers)"},
    {"OUTSTAT", "OUTSTAT n: RTS (2) and DTR (1), 0 = on; OUTSTAT 0 before a transfer (CE-158)"},
    {"PAUSE", "PAUSE ...: as PRINT, shown for about 0.85 s, then the program goes on"},
    {"PEEK", "PEEK addr: the byte at addr"},
    {"PEEK#", "PEEK# addr: the byte at addr in ME1"},
    {"PI", "PI: 3.141592654"},
    {"POINT", "POINT n: the dot pattern of display column n (0-155)"},
    {"POKE", "POKE addr,b[,b...]: writes bytes from addr on"},
    {"POKE#", "POKE# addr,b[,b...]: writes bytes from addr on in ME1"},
    {"PRINT", "PRINT [v[;/,v...]] / PRINT# v,...: displays values; # records them on tape"},
    {"RADIAN", "RADIAN: angles in radians"},
    {"RANDOM", "RANDOM: a new seed for RND"},
    {"READ", "READ v[,v...]: the next DATA values into the variables"},
    {"REM", "REM ...: a remark, not executed"},
    {"RESTORE", "RESTORE [line]: the next READ starts at that line's DATA, or the first"},
    {"RETURN", "RETURN: back to the statement after the GOSUB"},
    {"RIGHT$", "RIGHT$(s$,n): the last n characters of s$"},
    {"RINKEY$", "RINKEY$: the last byte RS-232C received, CHR$ 0 if none; doesn't wait (CE-158)"},
    {"RLINE", "RLINE [(x,y)]-(x,y)...: draws lines relative to the pen, GRAPH mode (CE-150)"},
    {"RMT", "RMT ON/OFF: the second tape recorder's remote control (CE-150)"},
    {"RND", "RND n: a random number, 1 to n for n>1, else between 0 and 1"},
    {"ROMRPLC", "ROMRPLC [LOAD [name$]/ENABLE/DISABLE/STAGEMCU/STAGERAM]: a replacement ROM"},
    {"ROMVER", "ROMVER: which PC-1500 ROM revision this is, by its CRC-32"},
    {"ROTATE", "ROTATE n: the print direction, 0-3 (CE-150)"},
    {"RUN", "RUN [line/\"label\"]: runs the program from the start or there"},
    {"SDCD", "SDCD path$: changes the SD card's current directory"},
    {"SDCLOSE", "SDCLOSE n/ALL: closes channel n, or every channel"},
    {"SDCP", "SDCP src$,dest$[,-Y]: copies a file; -Y overwrites without asking"},
    {"SDDF", "SDDF: the SD card's free and total space"},
    {"SDEOF", "SDEOF(n): 1 once channel n has no value left for SDINPUT#, else 0"},
    {"SDFMT", "SDFMT: formats the SD card (asks Y/N)"},
    {"SDINPUT", "SDINPUT#n,v[,v...]: the next values from channel n into the variables"},
    {"SDLOAD", "SDLOAD [M] [name$[,addr]]: loads a program, or M a binary; no name: browse"},
    {"SDLOF", "SDLOF(n): the size in bytes of channel n's file"},
    {"SDLS", "SDLS: browses the current directory on the SD card"},
    {"SDMKDIR", "SDMKDIR path$: makes a directory"},
    {"SDMV", "SDMV src$,dest$[,-Y]: moves or renames a file"},
    {"SDOPEN", "SDOPEN name$ [FOR R/W] AS n: opens a file as channel n (1-16)"},
    {"SDPOS", "SDPOS(n): channel n's byte position, for SDREAD and SDWRITE"},
    {"SDPRINT", "SDPRINT#n,v[,v...]: appends the variables' values to channel n's file"},
    {"SDPWD", "SDPWD: shows the SD card's current directory"},
    {"SDREAD", "SDREAD [#]n,addr,len[,pos]: len bytes from channel n's file into RAM at addr"},
    {"SDRM", "SDRM name$[,-Y]: deletes a file (asks Y/N unless -Y)"},
    {"SDRMDIR", "SDRMDIR path$: removes an empty directory"},
    {"SDSAVE", "SDSAVE name$[,-Y] / SDSAVE M name$,start,end[,call][,-Y]: program or memory"},
    {"SDSKIP", "SDSKIP#n,count: skips count values in channel n"},
    {"SDWRITE", "SDWRITE [#]n,addr,len[,pos]: len bytes from RAM at addr to channel n's file"},
    {"SETCOM", "SETCOM [baud[,bits[,parity[,stop]]]] / SETCOM s$: none: 300,8,N,1 (CE-158)"},
    {"SETDEV", "SETDEV [KI][,DO][,PO][,CI][,CO]: what goes via RS-232C; none: nothing (CE-158)"},
    {"SGN", "SGN x: 1, 0 or -1 as x is positive, zero or negative"},
    {"SIN", "SIN x: the sine of x"},
    {"SORGN", "SORGN: makes the pen's position the origin in GRAPH mode (CE-150)"},
    {"SPACE$", "SPACE$ n: a string of n spaces"},
    {"SQR", "SQR x: the square root of x"},
    {"SSH", "SSH \"user@host[:port]\"[,pw$]: a shell on that host (WFCON first)"},
    {"SSHFORGET", "SSHFORGET [host$]: forgets a host's key, or all of them (asks Y/N)"},
    {"SSHKEY", "SSHKEY: the dongle's public key to SSHKEY.PUB, for its authorized keys"},
    {"STAGE", "STAGE [RAM/DEBUG/MCU]: serves the expansion ROM from RAM or the MCU; none: which"},
    {"STATUS", "STATUS n: memory: 0 steps free, 1 steps used"},
    {"STEP", "FOR v=a TO b STEP c: the loop's step"},
    {"STLOAD", "STLOAD: brings back the RAM STSAVE kept, and goes on after that STSAVE"},
    {"STOP", "STOP: halts the program; CONT continues it"},
    {"STR$", "STR$ x: the number x as a string"},
    {"STSAVE", "STSAVE: keeps all of RAM (0000H-7FFFH) in the MCU's flash, for STLOAD"},
    {"TAB", "TAB n: moves the print position to column n (CE-150)"},
    {"TAN", "TAN x: the tangent of x"},
    {"TERMINAL", "TERMINAL: terminal mode at SETCOM's settings, XON/XOFF on; ON: menu (CE-158)"},
    {"TEST", "TEST: prints the printer's test pattern (CE-150)"},
    {"TEXT", "TEXT: the printer's mode for text, from GRAPH (CE-150)"},
    {"THEN", "IF cond THEN statement/line: what IF does when cond is true"},
    {"TIME", "TIME: the clock as MMDDHH.mmss; TIME=MMDDHH.mmss sets it"},
    {"TO", "FOR v=a TO b: the loop's last value"},
    {"TRANSMIT", "TRANSMIT BREAK,n: a long space on RS-232C for n/64 s, n 1-255 (CE-158)"},
    {"TROFF", "TROFF: ends TRON's trace"},
    {"TRON", "TRON: traces the program a line at a time, for debugging"},
    {"UNLOCK", "UNLOCK: undoes LOCK"},
    {"USING", "USING \"format\": how PRINT shows numbers and strings (#, ., &); none: cancels"},
    {"VAL", "VAL s$: the number in the string s$"},
    {"WAIT", "WAIT [n]: how long PRINT shows before going on; none: until ENTER; 0: instant"},
    {"WFCON", "WFCON [ssid$[,pw$]]: joins a Wi-Fi network; none: the strongest remembered"},
    {"WFCONFIG", "WFCONFIG: shows the Wi-Fi interface's settings: address, gateway, ..."},
    {"WFDISC", "WFDISC: leaves the Wi-Fi network, its radio off"},
    {"WFFORGET", "WFFORGET [ssid$]: forgets a remembered network, or all of them (asks Y/N)"},
    {"WFPING", "WFPING host$: four echo requests a second apart, then a summary"},
    {"WFSCAN", "WFSCAN: scans for Wi-Fi networks; pick one with C"},
    {"WFSTAT", "WFSTAT: Wi-Fi: 0 off, 1 connecting (or lost), 2 connected"},
    {"ZONE", "ZONE n: LPRINT's comma column width, 1-31, 13 at power-on (CE-158)"},
};

#define COUNT (sizeof k_help / sizeof k_help[0])

const help_entry_t *help_table(size_t *count) {
    *count = COUNT;
    return k_help;
}

/* The query as a name: upper case, no spaces or quotes. */
static size_t clean(const char *q, size_t len, char *out) {
    size_t n = 0;
    for (size_t i = 0; i < len && n < HELP_QUERY_MAX; i++)
        if (q[i] != ' ' && q[i] != '"') out[n++] = (char)toupper((unsigned char)q[i]);
    return n;
}

static bool holds(const char *name, const char *q, size_t n) {
    size_t len = strlen(name);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(name + i, q, n) == 0) return true;
    return false;
}

size_t help_match(const char *query, size_t len, uint16_t *out, size_t max) {
    char q[HELP_QUERY_MAX];
    size_t n = clean(query, len, q), found = 0;
    for (size_t i = 0; i < COUNT; i++)
        if (strlen(k_help[i].name) == n && memcmp(k_help[i].name, q, n) == 0) {
            if (max) out[0] = (uint16_t)i;
            return 1;
        }
    for (size_t i = 0; i < COUNT; i++)
        if (holds(k_help[i].name, q, n)) {
            if (found < max) out[found] = (uint16_t)i;
            found++;
        }
    return found;
}

/* ---- the viewer ---- */

static void changed(help_ui_t *u) { u->version++; }

static size_t text_len(const help_ui_t *u) { return strlen(k_help[u->match[u->sel]].text); }

bool help_ui_start(help_ui_t *u, const char *query, size_t len) {
    size_t n;
    memset(u, 0, sizeof *u);
    n = help_match(query, len, u->match, sizeof u->match / sizeof u->match[0]);
    if (n == 0) return false;
    u->count = (uint16_t)(n > sizeof u->match / sizeof u->match[0] ? sizeof u->match / sizeof u->match[0] : n);
    u->result = HELP_UI_OPEN;
    u->held = KBD_K_ENTER; /* the ENTER that ran HELP may still be down */
    return true;
}

/* A key going down. True if it repeats when held. */
static bool press(help_ui_t *u, uint8_t key) {
    switch (key) {
        case KBD_K_UP:
            if (u->sel > 0) u->sel--, u->col = 0;
            changed(u);
            return true;
        case KBD_K_DOWN:
            if (u->sel + 1 < u->count) u->sel++, u->col = 0;
            changed(u);
            return true;
        case KBD_K_LEFT:
            if (u->col > 0) u->col--;
            changed(u);
            return true;
        case KBD_K_RIGHT:
            if ((size_t)u->col + HELP_WIDTH < text_len(u)) u->col++;
            changed(u);
            return true;
        case KBD_K_ENTER:
        case KBD_K_CL:
        case KBD_K_DEF:
            u->result = HELP_UI_DONE;
            changed(u);
            return false;
        default: return false;
    }
}

void help_ui_key(help_ui_t *u, uint8_t key, uint32_t now_ms) {
    if (u->result != HELP_UI_OPEN) return;
    if (key != u->held) {
        u->held = key;
        u->repeats = key && press(u, key);
        u->next_repeat_ms = now_ms + REPEAT_DELAY_MS;
        return;
    }
    if (key && u->repeats && (int32_t)(now_ms - u->next_repeat_ms) >= 0) {
        u->next_repeat_ms = now_ms + REPEAT_MS;
        press(u, key);
    }
}

void help_ui_break(help_ui_t *u) {
    if (u->result == HELP_UI_OPEN) {
        u->result = HELP_UI_BREAK;
        changed(u);
    }
}

void help_ui_render(const help_ui_t *u, char out[HELP_WIDTH]) {
    const char *t = k_help[u->match[u->sel]].text;
    size_t len = strlen(t);
    memset(out, ' ', HELP_WIDTH);
    for (size_t i = 0; i < HELP_WIDTH && u->col + i < len; i++) out[i] = t[u->col + i];
}

/* ---- in TERM's window ---- */

static help_ui_t g_ui;
static bool g_terminal;
static uint8_t g_break_seen;
static uint32_t g_shown_version;

/* Ordering: the line before its count, the line before TERM_CLOSED -- the
 * ROM reads them from the other side of the bus. */
static void barrier(void) {
#if defined(__arm__)
    __asm volatile("dmb" ::: "memory");
#endif
}

static void publish(uint8_t *w) {
    char line[HELP_WIDTH];
    w[EXP_SSH_TERM_INDICATORS] = (uint8_t)(w[TERM_SAVED_IND] & IND_SMALL);
    if (g_ui.version == g_shown_version) return;
    g_shown_version = g_ui.version;
    help_ui_render(&g_ui, line);
    for (int i = 0; i < HELP_WIDTH; i++) /* [ ] ': the ROM's glyphs */
        line[i] = line[i] == '[' ? (char)EXP_TERM_GLYPH_LBRACKET
                  : line[i] == ']' ? (char)EXP_TERM_GLYPH_RBRACKET
                  : line[i] == 0x27 ? (char)EXP_TERM_GLYPH_APOSTROPHE
                                   : line[i];
    memcpy(w + EXP_SSH_TERM_LINE, line, HELP_WIDTH);
    barrier();
    w[EXP_SSH_TERM_LINE_COUNT]++;
}

uint8_t help_command(uint8_t *w) {
    char query[HELP_QUERY_MAX];
    uint8_t n = w[0] > HELP_QUERY_MAX ? HELP_QUERY_MAX : w[0];
    memcpy(query, w + 1, n);
    g_terminal = false;
    if (!help_ui_start(&g_ui, query, n)) return EXP_STATUS_ERROR;
    g_break_seen = w[EXP_SSH_TERM_BREAK_COUNT];
    w[EXP_SSH_TERM_KEY] = g_ui.held;
    w[EXP_SSH_TERM_CLOSED] = 0;
    g_shown_version = g_ui.version - 1;
    publish(w);
    g_terminal = true;
    return EXP_STATUS_SUCCESS;
}

bool help_terminal(void) { return g_terminal; }

void help_poll(uint8_t *w, uint32_t now_ms) {
    if (!g_terminal) return;
    help_ui_key(&g_ui, w[EXP_SSH_TERM_KEY], now_ms);
    if (w[EXP_SSH_TERM_BREAK_COUNT] != g_break_seen) {
        g_break_seen = w[EXP_SSH_TERM_BREAK_COUNT];
        help_ui_break(&g_ui);
    }
    if (g_ui.result == HELP_UI_OPEN) {
        publish(w);
        return;
    }
    g_terminal = false;
    barrier();
    w[EXP_SSH_TERM_CLOSED] = 1; /* the last line stays on the display */
}
