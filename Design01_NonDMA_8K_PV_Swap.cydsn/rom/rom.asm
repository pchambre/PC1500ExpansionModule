; rom.asm -- production ROM image for the PC1500-PSOC5 expansion board.
;
; Layout: the 8K LH5801-visible window (0x8000-0x9FFF) splits into a 4K live
; data window (0x8000-0x8FFF: command/status/parameter exchange with
; DoCommand() in main.c, plus bulk data like directory listings) and a 4K
; ROM region (0x9000-0x9FFF: sentinel + keyword index + keyword table +
; routines) -- see rom_defs.inc for the exact split. There's no real "ROM"
; chip backing any of this; it's all the same RAM buffer main.c's
; InitBuffer() populates once at boot, so nothing in this file's assembled
; output may extend past 0x9FFF (see the .org guard at the end) or it would
; wrap past the window entirely.
;
; ROM lives at 0x9000, not the board's natural 0x8000, because of two
; confirmed real-ROM quirks in the base BASIC ROM's keyword-table walker
; (see rom_defs.inc's own comment for the first one's full writeup;
; both were found by tracing live execution with entertrace, not inferred
; from documentation):
;
; 1. On a name mismatch, the walker skips forward hunting for the first
;    byte *strictly greater than* 0xE0 to find the next entry's code
;    field. Page 8000's own required PV-low code value is exactly 0xE0 --
;    sitting on that boundary, not past it -- so the skip-scan glides
;    straight through it on that page specifically. 9000's required code
;    (0xE2, page index 2) is safely clear of it.
; 2. Having found the next entry, the walker reads its marker byte and
;    gives up entirely (does not attempt to match that entry) if bit 4
;    (0x10) of the marker is set. High nibble D (1101) always has bit 4
;    set; C (1100) doesn't. This only applies to entries reached by
;    skipping past a mismatch -- an entry reached directly via its own
;    first-letter index slot has no such check. See the per-entry comment
;    below.
;
; Both bugs independently block multi-entry-per-letter tables; fixing only
; one still fails silently. Confirmed live: SLOAD dispatches alone at
; 8000; SLOAD+SSAVE together never reach the second entry there regardless
; of marker; at 9000 with correct code values, a second entry only
; dispatches once its marker's bit 4 is also cleared.
;
; Keyword table format (confirmed against the real, working reference
; C:\Users\paulc\Documents\PC1500\INVERT_real_9000_pvlow_fullfix.ROM, which
; matches PC1500_BASIC_Keyword_Extension_Mechanism.md exactly):
;   - 0x55 sentinel at ROM_BASE -- required for BASIC's boot-time
;     expansion-ROM scan to consider this page present at all.
;   - a 26-entry/52-byte first-letter index immediately after (one 2-byte
;     BE pointer per A-Z), each pointing at the *second* character of that
;     letter's first table entry (confirmed via INVERT's own I-index entry
;     and CE-150's MERGE entry in ce150.asm -- both point 2 bytes into
;     their entry, not at the marker byte).
;   - the keyword table itself: one entry per keyword, back-to-back, each
;     `marker (1 byte, low nibble = name length) | name (ASCII, no
;     terminator) | code (2 bytes BE) | address (2 bytes BE)`, terminated
;     by a marker byte with low nibble 0. All six keywords below start with
;     'S', so they're laid out contiguously and the index's one 'S' entry
;     points at the first of them -- matches CE-150's own CHAIN/CLOAD/CSAVE
;     run (three consecutive C-entries under one index slot). Order doesn't
;     matter (confirmed: alphabetical sorting made no difference on its
;     own) -- only the two bugs above did.
;   - the marker's high nibble matters only via its bit 4 (quirk 2 above);
;     the code field's high byte matters via the 0xE0 boundary (quirk 1).
;
; Commands are SD-prefixed (SDLS/SDFMT/SDLOAD/SDSAVE/SDRM/SDCP/SDDF/SDCD/
; SDMKDIR/SDRMDIR/SDPWD), not S-prefixed -- renamed from the original SLS/
; SFMT/SLOAD/SSAVE/SRM to avoid colliding with the single-letter S-prefix
; convention as more commands get added. The original "SDF" (unchanged
; from the pre-rename "SDF", which already fit the new scheme's spelling)
; turned out to collide with the *new* "SDFMT" -- SDF is a strict prefix
; of SDFMT, so the table walker greedily matched SDF as a complete keyword
; and left "MT" as untokenized literal text (confirmed live: typing SDFMT
; tokenized as SDF's own code value followed by raw "MT" bytes) -- the
; exact same class of bug already known here for "SFORMAT" containing the
; real built-in keyword FOR as a substring (see SDFMT's own table-entry
; comment). Renamed to SDDF (matching its actual purpose -- free space,
; and the Unix `df` convention) to clear the collision. The same
; SDRM-vs-SDRMDIR collision showed up again when the directory commands
; were added -- resolved this time by table *ordering* instead of a
; rename (see SDRMDIR's own table-entry comment) since the user wanted to
; keep both names as-is. Checked every other pair by hand; nothing else
; in the current set collides.
; SDFMT, SDLS, and now SDLOAD (all its forms -- no-argument browse, a
; direct quoted filename, and M/machine-language mode, see SDLOAD_ROUTINE's
; own comment) are fully implemented; SDSAVE/SDRM/SDCP/SDCD/SDMKDIR/SDRMDIR
; are still stubs -- structurally real keyword-table entries (so BASIC's
; scanner can already find them) but their routines just return cleanly
; without doing anything yet. Reading their own filename/path argument(s)
; is no longer a research blocker -- confirmed live (entertrace on a
; running pc1500emu, see SDLOAD_ROUTINE's comment) that a custom keyword's
; trailing typed text just sits as raw, untokenized ASCII directly in
; DISP_BUFFER (7BB0H) right after the keyword's own 2-byte code, up to the
; 0DH terminator; no interpreter-level expression evaluation happens for
; it. (An earlier version of this comment cited "VMJ 0xFFB8 then VMJ
; 0xFFB6, per ce150.asm's CSAVE/CLOAD routines" as the mechanism -- that
; was wrong, traced and corrected this session: those two vectors resolve
; to CE-150's own cassette-tape I/O routines specifically, TAPE_HDR_WRITE/
; TAPE_PUTC, populated into the shared FF00H-FFFFH vector table by CE-150's
; own boot-time init the same way this project's own BOOT_SELFCHECK_ENTRY
; is -- not a general base-ROM string-fetch facility at all. Our module
; doesn't populate those slots, so calling them jumps into garbage.) SDCD/
; SDMKDIR/SDRMDIR additionally need real FS_ChDir/FS_MkDir/FS_RmDir
; wiring in main.c/ExpansionMock, which doesn't exist yet either -- parsing
; their argument text is no longer the blocker, the underlying directory
; operations are. SDPWD needs a "current directory" to track and display,
; which also doesn't exist on either side yet. Don't build on the stubs'
; addresses/behavior yet.

	.area CODE (ABS)
	.include "rom_defs.inc"
	.org ROM_BASE

; ---------------------------------------------------------------------
; Sentinel + reserved header (32 bytes total, matching INVERT_minimal's
; own layout exactly). Byte 0x0A (offset 10, ROM_BASE+0x0A) is the base
; ROM's own boot-time peripheral init/self-check entry point -- see
; PC1500_BASIC_Keyword_Extension_Mechanism.md's "Boot-time peripheral
; init/self-check" section for the full, disassembly-confirmed calling
; convention (found by tracing a real reset with the real CE-150 cartridge
; attached, then confirming byte-for-byte against ROM1.BIN). Left as a
; reserved zero byte here until this session, `reset`/Ctrl+F12 with this
; module attached never reached BASIC's normal NEW0?:CHECK cold-boot
; prompt -- the CPU just executed whatever the zero-filled reserved bytes
; (and, past them, the keyword table's own data bytes) happened to decode
; to as garbage instructions, wandering unpredictably before eventually
; stumbling into a stable state. A bare RTN here (matching what a real,
; healthy peripheral is expected to do once its own init/self-check
; passes) fixes that -- confirmed live: reset now reaches NEW0?:CHECK
; exactly like with no module attached. A later session should replace
; this with a genuine PSoC 5 health check (communicate with the board,
; RTN normally on success); for now this always reports success.
	.db 0x55
	.blkb 9
BOOT_SELFCHECK_ENTRY:  ; ROM_BASE+0x0A -- called as `stx p` (not `sjp`) with
                        ; the return address already pushed by the caller,
                        ; so a bare RTN is the correct, complete return --
                        ; see the doc section above for the full convention.
	rtn
	.blkb 21

; ---------------------------------------------------------------------
; First-letter index (26 x 2-byte BE pointers, A-Z). Only 'S' is used.
KEYWORD_INDEX:
	.dw 0x0000  ; A
	.dw 0x0000  ; B
	.dw 0x0000  ; C
	.dw 0x0000  ; D
	.dw 0x0000  ; E
	.dw 0x0000  ; F
	.dw 0x0000  ; G
	.dw 0x0000  ; H
	.dw 0x0000  ; I
	.dw 0x0000  ; J
	.dw 0x0000  ; K
	.dw 0x0000  ; L
	.dw 0x0000  ; M
	.dw 0x0000  ; N
	.dw 0x0000  ; O
	.dw 0x0000  ; P
	.dw 0x0000  ; Q
	.dw 0x0000  ; R
	.dw KEYWORD_TABLE+2  ; S -- 2nd character of SDDF, the first S-entry
	.dw 0x0000  ; T
	.dw 0x0000  ; U
	.dw 0x0000  ; V
	.dw 0x0000  ; W
	.dw 0x0000  ; X
	.dw 0x0000  ; Y
	.dw 0x0000  ; Z

; ---------------------------------------------------------------------
; Keyword table -- eleven entries, all under the single 'S' index slot
; above, back-to-back, terminated by a low-nibble-0 marker. Order doesn't
; strictly matter (confirmed) except that SDDF must stay first -- it's
; the one entry reached directly via the index slot rather than the
; skip-scan (see the per-entry comment below), so it anchors the chain --
; and that SDRMDIR must precede SDRM (see SDRMDIR's own entry comment).
; New entries were appended/inserted rather than re-sorting everything
; alphabetically, to minimize touching entries that already work.
KEYWORD_TABLE:
	.db 0xD4  ; first S-entry, reached directly via the index -- marker high
	          ; nibble doesn't matter here (not reached via the skip-scan)
	.ascii "SDDF"
	.dw 0xE28A
	.dw SDDF_ROUTINE

	; Every entry below is only ever reached by skipping past a mismatched
	; preceding entry, which requires bit 4 (0x10) of the *entry being
	; skipped to* to be clear -- confirmed live: the walker's skip-to-next
	; logic ANIs the newly-read marker with 0x10 and gives up entirely if
	; it's set. High nibble D (1101) always has bit 4 set; C (1100) doesn't
	; -- matches real CE-150's own CLOAD(0xA5)/CSAVE(0xC5), both reached
	; only via the skip path, versus CHAIN(0x95)/MERGE(0xD5), both reached
	; directly via their own index slot.
	.db 0xC5  ; SDFMT, not SDFORMAT -- "SDFORMAT" contains the real built-in
	          ; keyword FOR as a substring, confirmed live (as SFORMAT, pre-
	          ; rename) to get mis-tokenized mid-word (typed text corrupted
	          ; to non-ASCII token bytes right where "FOR" sits)
	.ascii "SDFMT"
	.dw 0xE289
	.dw SDFMT_ROUTINE

	.db 0xC6
	.ascii "SDLOAD"
	.dw 0xE287
	.dw SDLOAD_ROUTINE

	.db 0xC4
	.ascii "SDLS"
	.dw 0xE285
	.dw SDLS_ROUTINE

	; SDRMDIR must precede SDRM here -- SDRM is a strict prefix of SDRMDIR
	; (same class of collision as the original SDF-vs-SDFMT one, see this
	; file's header comment), and the walker accepts the *first* complete
	; name match it finds while scanning, not the longest one (confirmed by
	; that earlier bug). Placing the longer name first means it's tried
	; before SDRM, so typing "SDRM" alone correctly falls through (SDRMDIR
	; needs 3 more characters that aren't there) to match SDRM's own entry
	; right after, while typing "SDRMDIR" fully matches here first --
	; confirmed live both ways after this reordering (see
	; tests/expansion_keyword_test.cpp).
	.db 0xC7
	.ascii "SDRMDIR"
	.dw 0xE28E
	.dw SDRMDIR_ROUTINE

	.db 0xC4
	.ascii "SDRM"
	.dw 0xE288
	.dw SDRM_ROUTINE

	.db 0xC6
	.ascii "SDSAVE"
	.dw 0xE286
	.dw SDSAVE_ROUTINE

	.db 0xC4
	.ascii "SDCP"
	.dw 0xE28B
	.dw SDCP_ROUTINE

	.db 0xC4
	.ascii "SDCD"
	.dw 0xE28C
	.dw SDCD_ROUTINE

	.db 0xC7
	.ascii "SDMKDIR"
	.dw 0xE28D
	.dw SDMKDIR_ROUTINE

	.db 0xC5
	.ascii "SDPWD"
	.dw 0xE28F
	.dw SDPWD_ROUTINE

	.db 0xD0  ; table terminator

; ---------------------------------------------------------------------
; Shared dispatch-back tail. Every keyword routine ends here instead of
; duplicating this sequence -- per
; PC1500_BASIC_Keyword_Extension_Mechanism.md sec.6, a bare RTN (or the
; naive 3-instruction LDI/LDI/STX-P-only tail used by INVERT_minimal)
; corrupts the *next* keypress; this is the fully-corrected version.
;
; Several earlier rounds of hand-poking specific RAM addresses here (trying
; to make a keyword-dispatched return look identical to a real statement's,
; e.g. PRINT's, own recovery) each looked right in isolation but broke
; something else -- ERROR 1 from injecting a bare ">" into DISP_BUFFER
; (7BB0H) and landing at LCA80 (0xCA80), which re-parses it as pending
; input; broken `reset`/Ctrl+F12 cold-boot detection from a "safer"
; empty-line version of the same poke; and, most recently, typing a new
; command right after exiting SLS producing a corrupted, concatenated
; tokenized line ("SLS MEM" instead of "MEM"). That last one was root-
; caused (not guessed) via an in-process test harness
; (tests/expansion_keyword_test.cpp) that boots a real ROM and drives
; keystrokes directly, letting a *systematic* diff of every byte in
; 7860H-78FFH/7B00H-7BFFH between a confirmed-clean MEM recovery and SLS's
; post-return state replace guessing individual fields one at a time. Two
; real bugs fell out of that diff:
;   - 7874H (a "cursor-pointer" bit per the PC-2 manual, p.25) needs
;     CLEARING, not setting -- the original `ori ...,0x01` here was
;     backwards.
;   - 7880H -- documented in docs/pc1500_hardware_reference.md (PC-2
;     manual section 5-4-5) as a "parameter FF" byte controlling how the
;     built-in program-display subroutine renders numeric/string/program
;     data -- needs resetting to 00H. DISP_N_CHARS0 (used below, and by
;     SLS's own text blits) evidently leaves this in a non-default
;     rendering mode; the next real keystroke-echo, reading it unreset,
;     rendered as an append onto the old line instead of a fresh one. This
;     was the actual root cause of the concatenation bug -- confirmed by
;     bisecting the diff's other candidate fields out one at a time via
;     the same test harness until only this one remained load-bearing.
; Landing at E2AA (not LCA80) and redrawing the prompt directly via
; DISP_N_CHARS0 (rather than relying on LCA80's real Enter-handling to do
; it) remains deliberate -- see the jump target's own comment below.
KEYWORD_RETURN_PROMPT:
	.ascii ">                         "
KEYWORD_RETURN_PROMPT_LEN .equ 26  ; '>' + 25 spaces -- a full blank line, not
                                    ; just the prompt char, since not every
                                    ; keyword blanks its own line first (SLS
                                    ; does; SFMT doesn't -- confirmed live:
                                    ; drawing only ">" left SFMT's stale VRAM
                                    ; content visible past column 0)

KEYWORD_RETURN:
	pop a                  ; undo the dispatcher's own unpaired PSH A
	ldi a,0xCA
	sta (0x784E)            ; refresh stale continuation pointer, hi
	ldi a,0x92
	sta (0x784F)             ; ...lo
	ani (0x764E),0xFE        ; clear stuck BUSY flag
	ani (0x7874),0xFE        ; cursor-pointer bit reset (see block comment above)
	ldi uh,>KEYWORD_RETURN_PROMPT
	ldi ul,<KEYWORD_RETURN_PROMPT
	ldi xl,KEYWORD_RETURN_PROMPT_LEN
	sjp DISP_N_CHARS0        ; actually DRAW ">" (and blank the rest of the
	                         ; line) on the real screen -- purely cosmetic, a
	                         ; direct VRAM blit like SLS's own blank-line draw,
	                         ; touching no BASIC-internal state. Confirmed
	                         ; live this is what was missing: E2AA (below)
	                         ; never redraws anything itself, it only reacts to
	                         ; the next real keypress, which is why landing
	                         ; there with no draw of our own left a stale/blank
	                         ; line until a second Enter came through.
	ldi a,0x00
	sta (0x7880)             ; program-display render-mode reset (see block
	                         ; comment above) -- DISP_N_CHARS0 just above is
	                         ; what leaves this non-zero
	ldi xh,0xE2
	ldi xl,0xAA
	stx p                    ; jump to BASIC's idle prompt. NOT LCA80 (0xCA80)
	                         ; -- confirmed live LCA80's own real Enter-
	                         ; handling re-parses DISP_BUFFER's current content
	                         ; as pending BASIC input on the *next* keypress;
	                         ; poking a bare ">" there (to make LCA80 see a
	                         ; harmless empty line) reproduced the exact same
	                         ; ERROR-1 regression as the earlier, reverted
	                         ; DISP_BUFFER-poking attempt, just delayed by one
	                         ; extra keypress. E2AA doesn't re-parse anything,
	                         ; so it's safe -- the VRAM draw above already
	                         ; solves the actual visible symptom without
	                         ; touching DISP_BUFFER/BLINK_CURSOR at all.
;
; FIXED (was a KNOWN SEPARATE BUG here): a third Enter, pressed on the
; idle prompt with nothing typed after exiting SLS, used to silently
; re-dispatch SLS_ROUTINE from its very start. Resolved by the same 7880H
; fix documented in the block comment above -- confirmed via
; tests/expansion_keyword_test.cpp's testStrayEnterAfterSlsExitDoesNotRedispatch,
; which failed before that fix and passes after it with no other change.
; Never fully root-caused *why* a stuck program-display render-mode byte
; also fools the keyword-table walker (E234, via E366's SML_DISPATCH) into
; re-matching SLS's table entry -- plausible but unconfirmed guess: the
; walker's match logic reads through the same rendering path this byte
; gates, so a stuck non-zero value causes it to mis-scan whatever's
; sitting where it expects fresh input. Not investigated further since the
; test harness confirms the practical symptom is gone either way.

; ---------------------------------------------------------------------
; SDFMT -- no argument. Destructive (wipes every file at the SD root, see
; ExpansionMock::FORMAT_SD_CARD / DoCommand()'s own real handler), so it
; first draws a confirmation prompt and blocks on KEYSCAN_WAIT: only an
; explicit 'Y' proceeds, any other key (including BREAK) aborts without
; touching the card. Writes a hardcoded "PC1500" volume name (length-
; prefixed, matching EXP_COMMAND_FORMAT_SD_CARD's existing parameter
; contract) into the data window via a TIN copy loop, then triggers it.
SDFMT_CONFIRM_MSG:
	.ascii "FORMAT SD CARD? Y/N"
SDFMT_CONFIRM_MSG_LEN .equ 20

SDFMT_VOLUME_NAME:
	.db 0x00, 0x06
	.ascii "PC1500"
SDFMT_VOLUME_NAME_LEN .equ 8

SDFMT_ROUTINE:
	ldi uh,>SDFMT_CONFIRM_MSG
	ldi ul,<SDFMT_CONFIRM_MSG
	ldi xl,SDFMT_CONFIRM_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcs SDFMT_ABORT           ; BREAK -- abort
	cpi a,KEY_Y
	bzr SDFMT_ABORT           ; anything other than 'Y' -- abort, no format

	ldi xh,>SDFMT_VOLUME_NAME
	ldi xl,<SDFMT_VOLUME_NAME
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi ul,SDFMT_VOLUME_NAME_LEN
SDFMT_COPY_LOOP:
	tin
	dec u
	cpi ul,0x00
	bzr SDFMT_COPY_LOOP
	ldi a,EXP_COMMAND_FORMAT_SD_CARD
	sta (EXP_INSTRUCTION_ABS)
SDFMT_ABORT:
	jmp KEYWORD_RETURN       ; KEYWORD_RETURN's own DISP_N_CHARS0 call blanks
	                         ; the confirmation prompt and redraws the ">"
	                         ; prompt either way, so nothing extra to clean up.

; ---------------------------------------------------------------------
; Stubs -- real keyword-table entries (BASIC's scanner can already find
; and tokenize these), but the routines themselves just return cleanly
; without doing anything yet. See this file's header comment for what's
; blocking each one.
;
; SDSAVE is now fully implemented (see its own routine, below the SDLOAD
; block) -- SDRM/SDCP/SDCD/SDMKDIR/SDRMDIR are still stubs. Reading their
; own filename/path argument text is no longer a research blocker -- see
; SDLOAD_ROUTINE's own comment for the confirmed mechanism (raw ASCII
; sitting in DISP_BUFFER right after the keyword's own tokenized code) and
; SDLOAD's own SD_PARSE_QUOTED_NAME helper, directly reusable here once
; someone gets to these. What's still actually blocking them: SDCD/
; SDMKDIR/SDRMDIR need real FS_ChDir/FS_MkDir/FS_RmDir wiring in main.c/
; ExpansionMock, which doesn't exist yet (see below); SDRM (delete) and
; SDCP (copy, needs a *second* argument -- a destination name after the
; source) just haven't been gotten to yet.
;
; SDPWD takes no argument but isn't implemented either: it needs a
; "current directory" to actually print, and neither main.c nor
; ExpansionMock track one yet -- both currently operate on a single flat
; root directory (see the "does the PSoC SD library support directories"
; discussion this session). Wiring up real FS_ChDir/FS_MkDir/FS_RmDir/
; FS_GetCWD calls (SEGGER emFile, confirmed by main.c's own FS_* naming)
; and the matching ExpansionMock support is future work, deliberately not
; done yet -- these four are table entries only for now, same as the
; other stubs.
SDRM_ROUTINE:
SDCP_ROUTINE:
SDCD_ROUTINE:
SDMKDIR_ROUTINE:
SDRMDIR_ROUTINE:
SDPWD_ROUTINE:
	jmp KEYWORD_RETURN

; SD_LIST_* -- shared directory-browser engine behind both SDLS (browse
; only) and SDLOAD's no-argument form (browse, then L to load). On entry
; (SD_LIST_INIT), writes EXP_COMMAND_LIST_SD_DIR to the instruction byte
; and polls that same address for a non-BUSY status -- the real
; DoCommand()/WriteStatus() protocol (see PC_EXP.h and main.c's own
; handler), not a ROM-baked shortcut. Whatever answers (today, pc1500emu's
; ExpansionMock mock; eventually the real PSoC MCU) populates the data
; window in the real EXP_DIR_* wire format, which SD_LIST_DISPLAY reads
; directly.
;
; Display is entirely real data, including the byte size and the summary
; page: the ROM side has no binary-to-decimal routine of its own (SDDF's
; own stub below still has that gap) and doesn't need one -- the MCU/mock
; side renders both the per-file size and a free-text summary line (e.g.
; "3 FILES 23051B 2122343F") directly into the wire format
; (EXP_DIR_SIZE_TEXT_LEN/EXP_DIR_SUMMARY_LEN, see PC_EXP.h), so the ROM
; just blits whichever page is selected -- one DISP_N_CHARS0 call per
; page, 26 contiguous bytes, whether that's a file entry (name+size-text,
; back-to-back by construction in the wire format) or the summary line
; (also exactly 26 bytes of plain text); see SD_LIST_DISPLAY below. An
; earlier version tried splitting a file entry's draw across two calls
; (DISP_N_CHARS0 for the name, then DISP_N_CHARS to append the size text
; from the cursor) -- don't do that: confirmed via real ROM1.BIN
; disassembly that DISP_N_CHARS (ED00H) compares its read pointer's low
; byte against 0x787B every iteration and can branch into the line
; editor's own cursor-blink handling when it matches -- it's coupled to
; BASIC's input-line context, not a safe generic blitter, and produced
; garbled output live (confirmed). All pages fill the full 26-character
; line so every redraw overwrites whatever was there before -- the
; single-line LCD has nowhere else stale characters from a longer
; previous page could hide.
;
; Current index, the real entry count, and the current entry's address all
; live in the data window's scratch page (EXP_SCRATCH_ABS+), not CPU
; registers -- SJPing into KEYSCAN_WAIT/DISP_N_CHARS0 doesn't preserve
; registers across the call. This only actually persists when the ROM is
; attached via pc1500emu's `loadexpansionmodule` (a genuinely writable
; data window) -- plain `loadrommodule` models true read-only ROM there,
; so a write to EXP_SCRATCH_ABS silently no-ops and the browser gets stuck
; (confirmed live this session -- see src/bus/bus.h's RomModule comment in
; pc1500emu).
;
; Navigation walks the current entry's address one EXP_DIR_RECORD_SIZE
; step at a time as Up/Down are pressed (an earlier version hardcoded
; three fixed per-entry addresses matching a since-removed always-3-files
; mock; broke as soon as a real directory's count wasn't 3 -- confirmed
; live, paging past the real files read uninitialized/leftover bytes,
; rendering as solid block glyphs). The LH5801 has no multiply instruction,
; but this only ever needs +/-1 step, so a small fixed-count INC X/DEC X
; loop does the 16-bit address math safely without one. Index == count
; means "showing the summary" (one record-size step past the last real
; entry, which is exactly where the wire format's own summary line sits --
; see PC_EXP.h) -- no separate "page 4" concept needed.
;
; SD_LIST_DISPLAY/UP/DOWN are real subroutines (RTN-terminated), not
; fall-through labels, specifically so more than one keyword's own
; WAITKEY loop can drive the same browser with different key bindings --
; SDLS exits on Enter/CL/BREAK; SDLOAD instead uses L to select-and-load,
; ignores Enter, and only CL/BREAK aborts (deliberately non-standard --
; keeps the same listing usable for either purpose without retyping
; SDLS first, per the actual feature request this was built for).
;
; KEY_CL/KEY_ENTER/KEY_UP/KEY_DOWN/KEY_L (rom_defs.inc) -- KEY_L shares
; KEY_Y's own "not yet independently confirmed live" caveat (see that
; file's comment).
SD_LIST_LINE_WIDTH .equ 0x1A  ; 26 -- DISP_N_CHARS0's own max, and this file's fixed page width
SD_LIST_INDEX_ABS   .equ (EXP_SCRATCH_ABS+0)  ; current entry index, 0..count (count = summary)
SD_LIST_COUNT_ABS   .equ (EXP_SCRATCH_ABS+1)  ; real entry count, low byte only (see comment above)
SD_LIST_ADDR_HI_ABS .equ (EXP_SCRATCH_ABS+2)  ; current entry/summary address, high byte
SD_LIST_ADDR_LO_ABS .equ (EXP_SCRATCH_ABS+3)  ; ...low byte
SD_LIST_BLANK:
	.ascii "                          "

; Clears the line and resets the cursor to the left edge, triggers
; EXP_COMMAND_LIST_SD_DIR and polls for completion, initializes navigation
; state, and falls through into SD_LIST_DISPLAY to draw the first entry
; (or the summary line, if the directory is empty) -- callers SJP here
; once, then drive their own WAITKEY loop using the subroutines below.
; The initial blank-and-reset-cursor step matters even before the first
; real draw: without it, whatever was still on screen from typing the
; command itself (and wherever its cursor position happened to land)
; persists through the dispatch/poll below (confirmed live: the first
; file name appeared overlapping leftover input-line content).
SD_LIST_INIT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0

	ldi a,EXP_COMMAND_LIST_SD_DIR
	sta (EXP_INSTRUCTION_ABS)
SD_LIST_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs SD_LIST_POLL

	lda (EXP_BUFFER_START_ABS+1)  ; real count, low byte of the 2-byte BE count field
	sta (SD_LIST_COUNT_ABS)
	ldi a,0x00
	sta (SD_LIST_INDEX_ABS)
	ldi a,>(EXP_BUFFER_START_ABS+2)  ; first entry's address (or the summary's, if count=0)
	sta (SD_LIST_ADDR_HI_ABS)
	ldi a,<(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_LO_ABS)
	; falls through to SD_LIST_DISPLAY

; Draws whatever SD_LIST_ADDR_HI/LO_ABS currently points at -- 26
; contiguous bytes, one DISP_N_CHARS0 call, works identically for a file
; entry (name+size-text, back-to-back by construction in the wire format)
; or the summary line (also exactly 26 bytes of plain text) since both are
; just "26 bytes starting here" from this routine's point of view.
SD_LIST_DISPLAY:
	lda (SD_LIST_ADDR_HI_ABS)
	sta uh
	lda (SD_LIST_ADDR_LO_ABS)
	sta ul
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	rtn

; Steps SD_LIST_INDEX_ABS/ADDR_HI/LO_ABS one record toward the first entry;
; a no-op if already there. Caller redraws (SD_LIST_DISPLAY) afterward.
SD_LIST_UP:
	lda (SD_LIST_INDEX_ABS)
	cpi a,0x00
	bzs SD_LIST_UP_DONE        ; already at the first file -- ignore
	dec a
	sta (SD_LIST_INDEX_ABS)
	lda (SD_LIST_ADDR_HI_ABS)
	sta xh
	lda (SD_LIST_ADDR_LO_ABS)
	sta xl
	ldi a,EXP_DIR_RECORD_SIZE
SD_LIST_UP_LOOP:
	dec x
	dec a
	bzr SD_LIST_UP_LOOP
	lda xh
	sta (SD_LIST_ADDR_HI_ABS)
	lda xl
	sta (SD_LIST_ADDR_LO_ABS)
SD_LIST_UP_DONE:
	rtn

SD_LIST_DOWN:
	lda (SD_LIST_INDEX_ABS)
	cpa (SD_LIST_COUNT_ABS)
	bzs SD_LIST_DOWN_DONE      ; already at the summary (index == count) -- ignore
	inc a
	sta (SD_LIST_INDEX_ABS)
	lda (SD_LIST_ADDR_HI_ABS)
	sta xh
	lda (SD_LIST_ADDR_LO_ABS)
	sta xl
	ldi a,EXP_DIR_RECORD_SIZE
SD_LIST_DOWN_LOOP:
	inc x
	dec a
	bzr SD_LIST_DOWN_LOOP
	lda xh
	sta (SD_LIST_ADDR_HI_ABS)
	lda xl
	sta (SD_LIST_ADDR_LO_ABS)
SD_LIST_DOWN_DONE:
	rtn

; ---------------------------------------------------------------------
; SDLS -- browse only. Enter, CL, or BREAK all exit back to READY.
SDLS_ROUTINE:
	sjp SD_LIST_INIT
SDLS_WAITKEY:
	sjp KEYSCAN_WAIT           ; blocks until a key is down; ACC=code, Carry=1 only for BREAK
	bcs SDLS_EXIT              ; BREAK
	cpi a,KEY_CL
	bzs SDLS_EXIT
	cpi a,KEY_ENTER
	bzs SDLS_EXIT
	cpi a,KEY_UP
	bzs SDLS_DO_UP
	cpi a,KEY_DOWN
	bzs SDLS_DO_DOWN
	bch SDLS_WAITKEY           ; any other key -- ignore, keep waiting

SDLS_DO_UP:
	sjp SD_LIST_UP
	sjp SD_LIST_DISPLAY
	bch SDLS_WAITKEY

SDLS_DO_DOWN:
	sjp SD_LIST_DOWN
	sjp SD_LIST_DISPLAY
	bch SDLS_WAITKEY

SDLS_EXIT:
	; Clear the one display line SDLS itself drew into before returning.
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDLOAD -- five forms, dispatched on the first byte of the trailing
; argument text:
;
;   SDLOAD                          browse+L (SD_LIST_*), BASIC program
;   SDLOAD "<filename>"             direct load, no browse, BASIC program
;   SDLOAD M                        browse+L, M (binary) mode, header addr
;   SDLOAD M "<filename>"           direct load, M mode, header's own addr
;   SDLOAD M "<filename>",<addr>    direct load, M mode, relocated to addr
;
; Argument text location, confirmed live via entertrace on a running
; pc1500emu (not assumed): a custom keyword's trailing typed text sits as
; raw, untokenized ASCII directly in DISP_BUFFER (7BB0H) right after this
; keyword's own 2-byte tokenized code, up to the 0DH terminator -- no
; interpreter-level expression evaluation happens to it at all. Typing
; "SDLOAD X" + Enter settles with DISP_BUFFER holding "E2 87 58 0D": the
; code, then the raw 'X', untouched. This replaces an earlier, wrong
; assumption in this file that CSAVE/CLOAD's VMJ 0xFFB8/0xFFB6 vectors were
; a reusable string-argument-fetch facility -- they're actually CE-150's
; own cassette-tape I/O routines specifically, not populated by our module,
; so calling them would jump into garbage (see this file's header comment
; for the full correction). Since nothing pre-parses this text, SDLOAD's
; own argument grammar below is entirely hand-rolled, reading characters
; directly out of DISP_BUFFER.
;
; Enter is deliberately ignored in the browse forms (not a browser-exit
; key here -- L selects, CL/BREAK abort); this is unrelated to the
; argument-text finding above and predates it.
;
; BASIC-mode loading copies the file's raw bytes directly into BASIC's
; program area starting at whatever BASIC_PROGRAM_START_HI/LO_ABS's live
; pointer currently says, then points the program's own end-pointer
; variable at the last byte copied. This assumes the file
; *is* a raw tokenized program dump (the same format pc1500::basic::
; saveBasicProgram/readBasicProgramBytes produce/consume on the host side,
; and the format a matching SDSAVE will need to write once implemented) --
; ending with its own trailing 0xFFH terminator byte already in place,
; exactly as BASIC_PROGRAM_END_PTR is documented to expect. It is *not* a
; text listing typed character-by-character through the line editor (that
; would need a whole separate mechanism -- CE-150's own CLOAD does this
; for cassette tape, but that's a serial bit-stream protocol, not
; something to reuse for a byte-addressable SD file).
;
; M-mode loading is a new file format, defined this session and now written
; by SDSAVE M (see that routine's own comment): the file's first 4 bytes
; are a BE target address followed by a BE call address (0x0000 = "don't
; call anything"), followed immediately by the raw binary payload -- no
; length field, read until EOF same as BASIC mode. The header is always
; read and consumed in full (4 dedicated bytes, not folded into the
; general 254-byte chunk loop, to keep "where does the real payload start"
; unambiguous); the target address is only actually *used* as the write
; target when no explicit address argument was given -- when one was, the
; header's target bytes are still consumed/skipped, just ignored in favor
; of the explicit one. The call address, if non-zero, is CALLed
; immediately after a successful load -- but ONLY in SDLOAD_MODE_M_HEADER
; (the file's own target address was used as-is); SDLOAD_MODE_M_EXPLICIT
; (relocated) never calls it, since the embedded call address was computed
; relative to the file's original, non-relocated target and would be
; meaningless at a different load address. All copying (BASIC and both M
; sub-modes) shares SD_OPEN_AND_LOAD, which ends by jumping straight to
; KEYWORD_RETURN itself (every call site wants that as its own last step).
; BASIC_PROGRAM_START used to be a hardcoded 0x40C5 constant (matching
; text_loader.h's kBasicProgramStart), correct only for a bare 2KB-RAM
; machine. Real hardware shifts the program area's own origin depending on
; installed RAM (e.g. the CE-155 module: 2K at 3800H + the standard 2K at
; 4000H + 6K at 4800H, giving a contiguous 3800H-5FFFH span, with the
; program area starting at 38C5H instead of 40C5H) -- confirmed live this
; session (see project memory) that BASIC's own boot sequence computes and
; records this origin in RAM at 7865H/7866H (2-byte BE), immediately before
; the already-established PROGEND pointer, rather than it being a ROM
; constant: booting with different emulated RAM shapes changed the value
; there, while it stayed exactly stable across loading/growing a real
; program (unlike PROGEND, which tracks the *current* end and grows).
; SDLOAD/SDSAVE's BASIC-mode target/source now reads this live pointer
; instead of assuming 0x40C5, so they work correctly regardless of
; installed RAM.
BASIC_PROGRAM_START_HI_ABS .equ 0x7865
BASIC_PROGRAM_START_LO_ABS .equ 0x7866
BASIC_PROGRAM_END_HI_ABS .equ 0x7867  ; matches text_loader.h's kProgramEndPointerAddr (2-byte BE)
BASIC_PROGRAM_END_LO_ABS .equ 0x7868
DISP_BUFFER_ABS .equ 0x7BB0  ; BASIC's own 80-byte LCD/line-editor text buffer

SDLOAD_MODE_BASIC      .equ 0  ; raw tokenized BASIC program -> BASIC_PROGRAM_START_HI/LO_ABS
SDLOAD_MODE_M_HEADER   .equ 1  ; raw binary; write target = the file's own 2-byte header
SDLOAD_MODE_M_EXPLICIT .equ 2  ; raw binary; write target = SDLOAD_ADDR_HI/LO_ABS (already parsed)

SDLOAD_WRITE_HI_ABS .equ (EXP_SCRATCH_ABS+4)  ; running program-area write pointer, high byte
SDLOAD_WRITE_LO_ABS .equ (EXP_SCRATCH_ABS+5)  ; ...low byte
SDLOAD_NAMELEN_ABS  .equ (EXP_SCRATCH_ABS+6)  ; filename length -- shared by both the browse-listing
                                                ; trim (SD_STAGE_LISTED_NAME) and the typed-argument
                                                ; quoted-string parse (SD_PARSE_QUOTED_NAME); never
                                                ; needed simultaneously, so one slot covers both
SDLOAD_MODE_ABS     .equ (EXP_SCRATCH_ABS+7)  ; one of the SDLOAD_MODE_* values above
SDLOAD_ADDR_HI_ABS  .equ (EXP_SCRATCH_ABS+8)  ; M-mode target address (explicit-parsed or read
SDLOAD_ADDR_LO_ABS  .equ (EXP_SCRATCH_ABS+9)  ; from the file's own header), 2-byte BE
SDLOAD_TEMP_HI_ABS  .equ (EXP_SCRATCH_ABS+10)  ; SD_PARSE_DECIMAL's own 16-bit multiply scratch
SDLOAD_TEMP_LO_ABS  .equ (EXP_SCRATCH_ABS+11)
SDLOAD_DIGITVAL_ABS .equ (EXP_SCRATCH_ABS+12)  ; SD_PARSE_DECIMAL/SD_PARSE_HEX's own
                                                 ; "consumed >=1 digit" flag
SDLOAD_DIGITNIBBLE_ABS .equ (EXP_SCRATCH_ABS+13)  ; ...this digit's numeric value, 0-15 (0-9 for
                                                    ; decimal, 0-15 for hex)
SDLOAD_CALL_HI_ABS  .equ (EXP_SCRATCH_ABS+14)  ; M-mode call address read from the file's own
SDLOAD_CALL_LO_ABS  .equ (EXP_SCRATCH_ABS+15)  ; header (0x0000 = none); see the M-mode format
                                                 ; comment above for when this is actually CALLed

SDLOAD_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES            ; tolerate "SDLOAD M"/"SDLOAD "file""'s natural leading
	                               ; space -- the dispatch checks below look at the very
	                               ; first non-space character only
	lda (x)
	cpi a,0x0D
	bzs SDLOAD_NOARG_BASIC
	cpi a,0x22                    ; '"'
	bzs SDLOAD_ARG_FILENAME_BASIC
	cpi a,0x4D                    ; 'M'
	bzs SDLOAD_ARG_M
	bch SDLOAD_ABORT              ; unrecognized argument -- bail, nothing touched yet

SDLOAD_ARG_FILENAME_BASIC:
	sjp SD_PARSE_QUOTED_NAME
	bcs SDLOAD_ABORT
	ldi a,SDLOAD_MODE_BASIC
	sta (SDLOAD_MODE_ABS)
	jmp SD_OPEN_AND_LOAD

SDLOAD_ARG_M:
	inc x
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x0D
	bzs SDLOAD_BROWSE_M
	cpi a,0x22
	bzr SDLOAD_ABORT               ; expected a quoted filename here -- malformed, bail
	sjp SD_PARSE_QUOTED_NAME
	bcs SDLOAD_ABORT
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C                     ; ','
	bzr SDLOAD_M_NO_RELOCATE       ; no comma -- header's own address, no relocation
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcs SDLOAD_ABORT
	ldi a,SDLOAD_MODE_M_EXPLICIT
	sta (SDLOAD_MODE_ABS)
	jmp SD_OPEN_AND_LOAD
SDLOAD_M_NO_RELOCATE:
	ldi a,SDLOAD_MODE_M_HEADER
	sta (SDLOAD_MODE_ABS)
	jmp SD_OPEN_AND_LOAD

SDLOAD_BROWSE_M:
	ldi a,SDLOAD_MODE_M_HEADER
	sta (SDLOAD_MODE_ABS)
	bch SDLOAD_BROWSE_COMMON

SDLOAD_NOARG_BASIC:
	ldi a,SDLOAD_MODE_BASIC
	sta (SDLOAD_MODE_ABS)
SDLOAD_BROWSE_COMMON:
	sjp SD_LIST_INIT
SDLOAD_WAITKEY:
	sjp KEYSCAN_WAIT
	bcs SDLOAD_ABORT           ; BREAK
	cpi a,KEY_CL
	bzs SDLOAD_ABORT
	cpi a,KEY_ENTER
	bzs SDLOAD_WAITKEY         ; deliberately ignored -- keep waiting, don't select or exit
	cpi a,KEY_UP
	bzs SDLOAD_DO_UP
	cpi a,KEY_DOWN
	bzs SDLOAD_DO_DOWN
	cpi a,KEY_L
	bzs SDLOAD_SELECTED
	bch SDLOAD_WAITKEY         ; any other key -- ignore, keep waiting

SDLOAD_DO_UP:
	sjp SD_LIST_UP
	sjp SD_LIST_DISPLAY
	bch SDLOAD_WAITKEY

SDLOAD_DO_DOWN:
	sjp SD_LIST_DOWN
	sjp SD_LIST_DISPLAY
	bch SDLOAD_WAITKEY

SDLOAD_SELECTED:
	lda (SD_LIST_INDEX_ABS)
	cpa (SD_LIST_COUNT_ABS)
	bzs SDLOAD_WAITKEY         ; summary row selected -- not a real file, ignore

	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0

	sjp SD_STAGE_LISTED_NAME
	bcs SDLOAD_ABORT           ; empty name -- shouldn't happen for a real listed
	                           ; file, bail safely rather than open() an empty path
	jmp SD_OPEN_AND_LOAD       ; SDLOAD_MODE_ABS was already set above, before SD_LIST_INIT

SDLOAD_ABORT:
	jmp KEYWORD_RETURN

; Copies the currently-selected browse entry's space-padded
; EXP_DIR_NAME_LEN-byte name field into a length-prefixed filename
; argument at EXP_BUFFER_START_ABS, trimming trailing spaces --
; OPEN_SD_FILE_READ's filename argument is length-prefixed (readLength-
; PrefixedString's own convention), so the padding has to come off before
; this can be used as a real host filename; passing it through with
; trailing spaces intact would look for a file literally named e.g.
; "TEST.BAS        " and never match. Returns via Carry: SET = empty name
; (nothing copied), CLEAR = success. Safe to overwrite the data window's
; own directory-listing bytes here: the source name lives at SD_LIST_ADDR_
; HI/LO_ABS, which for entry 0 *is* this same destination address (an
; identity copy, harmless) and for every later entry is always well past
; where the trimmed name (<=16 bytes) is written, since each record is
; EXP_DIR_RECORD_SIZE=30 bytes apart -- never overlapping.
SD_STAGE_LISTED_NAME:
	lda (SD_LIST_ADDR_HI_ABS)
	sta xh
	lda (SD_LIST_ADDR_LO_ABS)
	sta xl
	ldi a,0x00
	sta (SDLOAD_NAMELEN_ABS)
	ldi ul,EXP_DIR_NAME_LEN     ; bytes remaining to scan
	ldi uh,0x00                 ; current 1-based position within the name
SD_STAGE_LISTED_TRIM_LOOP:
	lda (x)
	cpi a,0x20                  ; space?
	bzs SD_STAGE_LISTED_SKIP
	inc uh
	lda uh
	sta (SDLOAD_NAMELEN_ABS)     ; non-space -- trimmed length so far = this position
	bch SD_STAGE_LISTED_NEXT
SD_STAGE_LISTED_SKIP:
	inc uh
SD_STAGE_LISTED_NEXT:
	inc x
	dec ul
	cpi ul,0x00
	bzr SD_STAGE_LISTED_TRIM_LOOP

	lda (SDLOAD_NAMELEN_ABS)
	cpi a,0x00
	bzs SD_STAGE_LISTED_EMPTY

	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDLOAD_NAMELEN_ABS)
	sta (EXP_BUFFER_START_ABS+1)
	lda (SD_LIST_ADDR_HI_ABS)
	sta xh
	lda (SD_LIST_ADDR_LO_ABS)
	sta xl
	ldi yh,>(EXP_BUFFER_START_ABS+2)
	ldi yl,<(EXP_BUFFER_START_ABS+2)
	ldi uh,0x00
	lda (SDLOAD_NAMELEN_ABS)
	sta ul
SD_STAGE_LISTED_COPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr SD_STAGE_LISTED_COPY_LOOP
	cpi ul,0x00
	bzr SD_STAGE_LISTED_COPY_LOOP
	rec
	rtn
SD_STAGE_LISTED_EMPTY:
	sec
	rtn

; Parses a double-quoted string starting at (X) (X must point at the
; opening '"'), writing it directly as a length-prefixed argument at
; EXP_BUFFER_START_ABS (2-byte BE length, then the raw characters) --
; ready for OPEN_SD_FILE_READ/etc. with no separate staging buffer needed,
; since DISP_BUFFER (where the source text lives) and EXP_BUFFER_START_ABS
; (the data window) are entirely separate memory. Advances X to just past
; the closing quote. Returns via Carry: SET = malformed (hit the 0DH
; terminator before a closing quote, or an empty ""), CLEAR = success.
; Capped at EXP_DIR_NAME_LEN characters, matching every other filename
; field's own limit in this ROM.
SD_PARSE_QUOTED_NAME:
	inc x                       ; past the opening quote
	ldi yh,>(EXP_BUFFER_START_ABS+2)
	ldi yl,<(EXP_BUFFER_START_ABS+2)
	ldi a,0x00
	sta (SDLOAD_NAMELEN_ABS)
SD_PARSE_QUOTED_LOOP:
	lda (x)
	cpi a,0x0D
	bzs SD_PARSE_QUOTED_UNTERMINATED
	cpi a,0x22                  ; closing quote?
	bzs SD_PARSE_QUOTED_DONE
	lda (SDLOAD_NAMELEN_ABS)
	cpi a,EXP_DIR_NAME_LEN
	bzs SD_PARSE_QUOTED_UNTERMINATED  ; too long -- treat as malformed rather than truncate silently
	lda (x)
	sta (y)
	inc y
	inc x
	lda (SDLOAD_NAMELEN_ABS)
	inc a
	sta (SDLOAD_NAMELEN_ABS)
	bch SD_PARSE_QUOTED_LOOP
SD_PARSE_QUOTED_DONE:
	inc x                       ; past the closing quote
	lda (SDLOAD_NAMELEN_ABS)
	cpi a,0x00
	bzs SD_PARSE_QUOTED_UNTERMINATED  ; empty "" -- reject same as malformed
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDLOAD_NAMELEN_ABS)
	sta (EXP_BUFFER_START_ABS+1)
	rec
	rtn
SD_PARSE_QUOTED_UNTERMINATED:
	sec
	rtn

; Advances (X) past any 0x20 (space) bytes.
SD_SKIP_SPACES:
	lda (x)
	cpi a,0x20
	bzr SD_SKIP_SPACES_DONE
	inc x
	bch SD_SKIP_SPACES
SD_SKIP_SPACES_DONE:
	rtn

; Parses a number at (X): decimal (bare digits) or hex (a leading '&' --
; matching PEEK/POKE's own hex-literal convention, confirmed by the user --
; then hex digits). Leaves the 16-bit result in SDLOAD_ADDR_HI/LO_ABS,
; advances X past the digits consumed. Returns via Carry: SET = no valid
; digits found, CLEAR = success.
SD_PARSE_NUMBER:
	lda (x)
	cpi a,0x26                  ; '&'
	bzr SD_PARSE_DECIMAL
	inc x
	; falls through to SD_PARSE_HEX

; SD_PARSE_HEX/SD_PARSE_DECIMAL both accumulate into SDLOAD_ADDR_HI/LO_ABS
; via repeated doubling (SD_SHL16_ADDR) -- hex doubles 4x per digit
; (value*16 + digit), decimal doubles-and-adds (value*2 + value*8 = *10,
; via SDLOAD_TEMP_HI/LO_ABS) then adds the digit, since this CPU has no
; multiply instruction.
SD_PARSE_HEX:
	ldi a,0x00
	sta (SDLOAD_ADDR_HI_ABS)
	sta (SDLOAD_ADDR_LO_ABS)
	ldi a,0x00
	sta (SDLOAD_DIGITVAL_ABS)   ; reused here as a "consumed >=1 digit" flag
SD_PARSE_HEX_LOOP:
	lda (x)
	cpi a,0x30                  ; '0'
	bcr SD_PARSE_HEX_CHECK_END
	cpi a,0x3A                  ; '9'+1
	bcr SD_PARSE_HEX_DIGIT       ; '0'-'9'
SD_PARSE_HEX_CHECK_END:
	lda (x)
	cpi a,0x41                  ; 'A'
	bcr SD_PARSE_HEX_END
	cpi a,0x47                  ; 'F'+1
	bcs SD_PARSE_HEX_END
	lda (x)
	adi a,0xC9                  ; A + (256-0x37) = A - 0x37 (mod 256) = A - 'A' + 10 --
	                            ; converts 'A'-'F' (0x41-0x46) to 10-15
	bch SD_PARSE_HEX_APPLY
SD_PARSE_HEX_DIGIT:
	lda (x)
	ani a,0x0F                  ; ASCII '0'-'9' -> 0-9
SD_PARSE_HEX_APPLY:
	sta (SDLOAD_DIGITNIBBLE_ABS) ; hex nibble value, 0-15
	ldi a,0x01
	sta (SDLOAD_DIGITVAL_ABS)   ; mark "consumed >=1 digit"
	sjp SD_SHL16_ADDR
	sjp SD_SHL16_ADDR
	sjp SD_SHL16_ADDR
	sjp SD_SHL16_ADDR           ; value <<= 4 (value * 16)
	rec
	lda (SDLOAD_ADDR_LO_ABS)
	adc (SDLOAD_DIGITNIBBLE_ABS)
	sta (SDLOAD_ADDR_LO_ABS)
	lda (SDLOAD_ADDR_HI_ABS)
	adi a,0x00                  ; propagate carry into the high byte, no other change
	sta (SDLOAD_ADDR_HI_ABS)
	inc x
	bch SD_PARSE_HEX_LOOP
SD_PARSE_HEX_END:
	lda (SDLOAD_DIGITVAL_ABS)
	cpi a,0x00
	bzs SD_PARSE_QUOTED_UNTERMINATED  ; reuses the same "fail" tail as the quote parser
	rec
	rtn

SD_PARSE_DECIMAL:
	ldi a,0x00
	sta (SDLOAD_ADDR_HI_ABS)
	sta (SDLOAD_ADDR_LO_ABS)
	ldi a,0x00
	sta (SDLOAD_DIGITVAL_ABS)   ; reused here as a "consumed >=1 digit" flag
SD_PARSE_DECIMAL_LOOP:
	lda (x)
	cpi a,0x30                  ; '0'
	bcr SD_PARSE_DECIMAL_END
	cpi a,0x3A                  ; '9'+1
	bcs SD_PARSE_DECIMAL_END
	lda (x)
	ani a,0x0F                  ; ASCII '0'-'9' -> 0-9
	sta (SDLOAD_DIGITNIBBLE_ABS)
	ldi a,0x01
	sta (SDLOAD_DIGITVAL_ABS)
	; t = value
	lda (SDLOAD_ADDR_HI_ABS)
	sta (SDLOAD_TEMP_HI_ABS)
	lda (SDLOAD_ADDR_LO_ABS)
	sta (SDLOAD_TEMP_LO_ABS)
	sjp SD_SHL16_ADDR            ; value <<= 1 (value * 2)
	sjp SD_SHL16_TEMP
	sjp SD_SHL16_TEMP
	sjp SD_SHL16_TEMP            ; t <<= 3 (t * 8)
	rec
	lda (SDLOAD_ADDR_LO_ABS)
	adc (SDLOAD_TEMP_LO_ABS)
	sta (SDLOAD_ADDR_LO_ABS)
	lda (SDLOAD_ADDR_HI_ABS)
	adc (SDLOAD_TEMP_HI_ABS)
	sta (SDLOAD_ADDR_HI_ABS)     ; value = value*2 + value*8 = value*10
	rec
	lda (SDLOAD_ADDR_LO_ABS)
	adc (SDLOAD_DIGITNIBBLE_ABS)
	sta (SDLOAD_ADDR_LO_ABS)
	lda (SDLOAD_ADDR_HI_ABS)
	adi a,0x00                  ; propagate carry into the high byte, no other change
	sta (SDLOAD_ADDR_HI_ABS)
	inc x
	bch SD_PARSE_DECIMAL_LOOP
SD_PARSE_DECIMAL_END:
	lda (SDLOAD_DIGITVAL_ABS)
	cpi a,0x00
	bzs SD_PARSE_QUOTED_UNTERMINATED
	rec
	rtn

; Doubles the 16-bit value at SDLOAD_ADDR_HI/LO_ABS (resp. TEMP) in place.
SD_SHL16_ADDR:
	lda (SDLOAD_ADDR_LO_ABS)
	shl
	sta (SDLOAD_ADDR_LO_ABS)
	lda (SDLOAD_ADDR_HI_ABS)
	rol
	sta (SDLOAD_ADDR_HI_ABS)
	rtn

SD_SHL16_TEMP:
	lda (SDLOAD_TEMP_LO_ABS)
	shl
	sta (SDLOAD_TEMP_LO_ABS)
	lda (SDLOAD_TEMP_HI_ABS)
	rol
	sta (SDLOAD_TEMP_HI_ABS)
	rtn

; Opens the filename already staged as a length-prefixed argument at
; EXP_BUFFER_START_ABS (by SD_STAGE_LISTED_NAME or SD_PARSE_QUOTED_NAME),
; reads it in EXP_COMMAND_READ_FROM_SD_FILE's own max-254-byte chunks into
; memory starting at whatever SDLOAD_MODE_ABS says the target should be,
; closes the file, and (BASIC mode only) updates the program end-pointer.
; Ends by jumping straight to KEYWORD_RETURN -- every call site wants that
; as its own final step, so this never returns to its caller normally.
SD_OPEN_AND_LOAD:
	ldi a,EXP_COMMAND_OPEN_SD_FILE_READ
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzs SD_OPEN_AND_LOAD_OPENED  ; success -- skip the far jump below
	jmp SD_RAISE_ERROR_40        ; open failed -- file not found, a real user-facing error
	                             ; (unlike the header-read failures below, which stay silent
	                             ; aborts: those mean "file exists but is corrupt/too short",
	                             ; a different condition nobody's asked for an error code for)
SD_OPEN_AND_LOAD_OPENED:

	lda (SDLOAD_MODE_ABS)
	cpi a,SDLOAD_MODE_BASIC
	bzs SD_OPEN_AND_LOAD_BASIC_TARGET

	; M mode (either sub-mode): read and consume the file's own 4-byte BE
	; header unconditionally (target address, then call address) -- see
	; this section's own block comment for why (keeps "where the real
	; payload starts" unambiguous either way).
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	ldi a,0x04
	sta (EXP_BUFFER_START_ABS+1)
	ldi a,EXP_COMMAND_READ_FROM_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_OPEN_AND_LOAD_FAIL  ; couldn't even read the header -- bail
	lda (EXP_BUFFER_START_ABS+1)
	cpi a,0x04
	bzr SD_OPEN_AND_LOAD_FAIL  ; short read on the header -- file too small/corrupt, bail
	bch SD_OPEN_AND_LOAD_HDR_OK
SD_OPEN_AND_LOAD_FAIL:
	jmp KEYWORD_RETURN         ; local trampoline -- SDLOAD_ABORT (also just `jmp KEYWORD_RETURN`)
	                           ; is too far away for an 8-bit relative branch to reach from here
SD_OPEN_AND_LOAD_HDR_OK:
	lda (EXP_BUFFER_START_ABS+4)     ; call address -- stashed regardless of mode; only actually
	sta (SDLOAD_CALL_HI_ABS)         ; used below once loading is done, and only in M_HEADER mode
	lda (EXP_BUFFER_START_ABS+5)
	sta (SDLOAD_CALL_LO_ABS)

	lda (SDLOAD_MODE_ABS)
	cpi a,SDLOAD_MODE_M_HEADER
	bzr SD_OPEN_AND_LOAD_SET_TARGET  ; explicit mode -- SDLOAD_ADDR_HI/LO_ABS already set, keep it
	lda (EXP_BUFFER_START_ABS+2)     ; header mode -- use the file's own address
	sta (SDLOAD_ADDR_HI_ABS)
	lda (EXP_BUFFER_START_ABS+3)
	sta (SDLOAD_ADDR_LO_ABS)
	bch SD_OPEN_AND_LOAD_SET_TARGET

SD_OPEN_AND_LOAD_BASIC_TARGET:
	lda (BASIC_PROGRAM_START_HI_ABS)  ; live pointer, not a hardcoded constant -- see its own
	sta (SDLOAD_ADDR_HI_ABS)          ; comment for why (shifts with installed RAM on real hardware)
	lda (BASIC_PROGRAM_START_LO_ABS)
	sta (SDLOAD_ADDR_LO_ABS)

SD_OPEN_AND_LOAD_SET_TARGET:
	lda (SDLOAD_ADDR_HI_ABS)
	sta (SDLOAD_WRITE_HI_ABS)
	lda (SDLOAD_ADDR_LO_ABS)
	sta (SDLOAD_WRITE_LO_ABS)

SD_OPEN_AND_LOAD_READ_LOOP:
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	ldi a,0xFE                  ; request 254 bytes -- EXP_COMMAND_READ_FROM_SD_FILE's own max
	sta (EXP_BUFFER_START_ABS+1)
	ldi a,EXP_COMMAND_READ_FROM_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_OPEN_AND_LOAD_CLOSE  ; read error -- stop, close/finalize with what we have

	lda (EXP_BUFFER_START_ABS+0)  ; actual bytes-read count, high byte (0..254 fits low byte alone)
	cpi a,0x00
	bzr SD_OPEN_AND_LOAD_HAVE_DATA
	lda (EXP_BUFFER_START_ABS+1)  ; ...low byte
	cpi a,0x00
	bzs SD_OPEN_AND_LOAD_CLOSE  ; 0 bytes -- end of file, done

SD_OPEN_AND_LOAD_HAVE_DATA:
	ldi xh,>(EXP_BUFFER_START_ABS+2)
	ldi xl,<(EXP_BUFFER_START_ABS+2)
	lda (SDLOAD_WRITE_HI_ABS)
	sta yh
	lda (SDLOAD_WRITE_LO_ABS)
	sta yl
	ldi uh,0x00
	lda (EXP_BUFFER_START_ABS+1)
	sta ul
SD_OPEN_AND_LOAD_COPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr SD_OPEN_AND_LOAD_COPY_LOOP
	cpi ul,0x00
	bzr SD_OPEN_AND_LOAD_COPY_LOOP
	lda yh
	sta (SDLOAD_WRITE_HI_ABS)
	lda yl
	sta (SDLOAD_WRITE_LO_ABS)
	bch SD_OPEN_AND_LOAD_READ_LOOP

SD_OPEN_AND_LOAD_CLOSE:
	ldi a,EXP_COMMAND_CLOSE_SD_FILE
	sta (EXP_INSTRUCTION_ABS)

	lda (SDLOAD_MODE_ABS)
	cpi a,SDLOAD_MODE_BASIC
	bzr SD_OPEN_AND_LOAD_DONE  ; M mode -- no program-area bookkeeping, just return

	; BASIC mode: program-end pointer = write pointer - 1, the address of
	; the file's own trailing 0xFFH byte, matching BASIC_PROGRAM_END_HI/
	; LO_ABS's documented meaning (see this section's block comment).
	lda (SDLOAD_WRITE_LO_ABS)
	sta xl
	lda (SDLOAD_WRITE_HI_ABS)
	sta xh
	dec x
	lda xh
	sta (BASIC_PROGRAM_END_HI_ABS)
	lda xl
	sta (BASIC_PROGRAM_END_LO_ABS)
	bch SD_OPEN_AND_LOAD_DONE

SD_OPEN_AND_LOAD_DONE:
	lda (SDLOAD_MODE_ABS)
	cpi a,SDLOAD_MODE_M_HEADER
	bzr SD_OPEN_AND_LOAD_NO_CALL  ; BASIC or M_EXPLICIT (relocated) -- never call, see the M-mode
	                              ; format comment above for why explicit/relocated never calls
	lda (SDLOAD_CALL_HI_ABS)
	cpi a,0x00
	bzr SD_OPEN_AND_LOAD_DO_CALL
	lda (SDLOAD_CALL_LO_ABS)
	cpi a,0x00
	bzr SD_OPEN_AND_LOAD_DO_CALL
SD_OPEN_AND_LOAD_NO_CALL:
	jmp KEYWORD_RETURN

; Indirect call to the address the file's own header asked to be CALLed
; after loading -- same "push a return address into a register pair, PSH
; it, then STX P to the target" convention BASIC's own boot-time self-check
; dispatcher uses (PC1500_BASIC_Keyword_Extension_Mechanism.md section 11);
; the called routine's own plain RTN pops that pushed address straight back
; here, resuming normally.
SD_OPEN_AND_LOAD_DO_CALL:
	ldi uh,>SD_OPEN_AND_LOAD_AFTER_CALL
	ldi ul,<SD_OPEN_AND_LOAD_AFTER_CALL
	psh u
	lda (SDLOAD_CALL_HI_ABS)
	sta xh
	lda (SDLOAD_CALL_LO_ABS)
	sta xl
	stx p
SD_OPEN_AND_LOAD_AFTER_CALL:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDSAVE -- writes the current BASIC program, or an arbitrary RAM byte
; range (M mode), to a named SD file. Two forms:
;
;   SDSAVE "<filename>"[,-Y]
;     Saves the current BASIC program (BASIC_PROGRAM_START_HI/LO_ABS's live
;     pointer through BASIC_PROGRAM_END_HI/LO_ABS inclusive -- the same raw tokenized-bytes
;     range SD_OPEN_AND_LOAD's own BASIC mode reads back) as-is, no header.
;
;   SDSAVE M "<filename>",<start>,<end>[,<call>][,-Y]
;     Saves the inclusive RAM byte range [<start>,<end>] as a binary file,
;     preceded by the 4-byte BE header SDLOAD_ROUTINE's own M-mode comment
;     documents (target address = <start>, call address = <call> or
;     0x0000 if omitted). <start>/<end>/<call> are decimal or &-prefixed
;     hex, same convention as SDLOAD M's own address argument
;     (SD_PARSE_NUMBER). <call>, if given, is what SDLOAD M (loading this
;     same file back in with no relocation argument) will CALL immediately
;     after loading.
;
; Both forms: with no arguments at all, or a malformed/incomplete argument
; list (missing required pieces, a stray trailing comma, an unrecognized
; -Y spelling, <end> before <start>, etc.), raises a genuine BASIC ERROR 1
; (SD_RAISE_ERROR_1) rather than silently doing nothing -- unlike SDLOAD's
; own malformed-argument handling (which just quietly aborts, matching
; browsing-command precedent), SDSAVE's arguments describe a specific,
; deliberate write the user asked for, so silently no-op'ing a typo felt
; more likely to surprise/lose data than a normal Sharp BASIC syntax error
; would.
;
; If the target file already exists, prompts for confirmation and blocks
; on KEYSCAN_WAIT (same pattern as SDFMT_ROUTINE's own confirmation) --
; any answer but Y aborts without touching the file. -Y (the final
; comma-separated argument in either form) skips this check
; unconditionally, matching SDFMT's own risk posture of trusting an
; explicit override rather than adding a second confirmation on top.
SDSAVE_CONFIRM_MSG:
	.ascii "FILE EXISTS. OVERWRITE Y/N"
SDSAVE_CONFIRM_MSG_LEN .equ 26

SDSAVE_MODE_BASIC .equ 0
SDSAVE_MODE_M     .equ 1

SDSAVE_START_HI_ABS      .equ (EXP_SCRATCH_ABS+16)  ; M mode -- parsed <start> address, 2-byte BE
SDSAVE_START_LO_ABS      .equ (EXP_SCRATCH_ABS+17)
SDSAVE_END_HI_ABS        .equ (EXP_SCRATCH_ABS+18)  ; M mode -- parsed <end> address (inclusive), BE
SDSAVE_END_LO_ABS        .equ (EXP_SCRATCH_ABS+19)
SDSAVE_CALL_HI_ABS       .equ (EXP_SCRATCH_ABS+20)  ; M mode -- parsed <call> address, or 0x0000
SDSAVE_CALL_LO_ABS       .equ (EXP_SCRATCH_ABS+21)
SDSAVE_YFLAG_ABS         .equ (EXP_SCRATCH_ABS+22)  ; nonzero = -Y given, skip the overwrite prompt
SDSAVE_MODE_ABS          .equ (EXP_SCRATCH_ABS+23)  ; one of the SDSAVE_MODE_* values above
SDSAVE_WRITE_HI_ABS      .equ (EXP_SCRATCH_ABS+24)  ; SD_WRITE_RANGE's own running source pointer
SDSAVE_WRITE_LO_ABS      .equ (EXP_SCRATCH_ABS+25)
SDSAVE_RANGE_END_HI_ABS  .equ (EXP_SCRATCH_ABS+26)  ; SD_WRITE_RANGE's own inclusive end address
SDSAVE_RANGE_END_LO_ABS  .equ (EXP_SCRATCH_ABS+27)
SDSAVE_CHUNKLEN_ABS      .equ (EXP_SCRATCH_ABS+28)  ; SD_WRITE_RANGE's own current-chunk byte count
SDSAVE_RANGE_DONE_ABS    .equ (EXP_SCRATCH_ABS+29)  ; SD_WRITE_RANGE's own "this is the last chunk" flag
SDSAVE_NAME_STASH_LEN .equ 18  ; 2-byte length prefix + up to EXP_DIR_NAME_LEN(16) filename chars
SDSAVE_NAME_STASH_ABS .equ (EXP_SCRATCH_ABS+30)  ; through +47 -- see SD_CREATE_AND_WRITE's own
                                                   ; comment for why this stash/restore exists

SDSAVE_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22                    ; '"'
	bzs SDSAVE_ARG_FILENAME_BASIC
	cpi a,0x4D                    ; 'M'
	bzs SDSAVE_ARG_M
	jmp SD_RAISE_ERROR_1          ; no arguments at all, or an unrecognized first token

SDSAVE_ARG_FILENAME_BASIC:
	sjp SD_PARSE_QUOTED_NAME
	bcr SDSAVE_BASIC_NAME_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_BASIC_NAME_OK:
	ldi a,0x00
	sta (SDSAVE_YFLAG_ABS)
	sjp SD_PARSE_YFLAG             ; optional trailing ",-Y"
	bcr SDSAVE_BASIC_YFLAG_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_BASIC_YFLAG_OK:
	ldi a,SDSAVE_MODE_BASIC
	sta (SDSAVE_MODE_ABS)
	jmp SD_CREATE_AND_WRITE

SDSAVE_ARG_M:
	inc x
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22
	bzs SDSAVE_M_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1           ; SDSAVE M always requires "<filename>" next (no bare-M form)
SDSAVE_M_HAVE_QUOTE:
	sjp SD_PARSE_QUOTED_NAME
	bcr SDSAVE_M_NAME_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_M_NAME_OK:

	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C
	bzs SDSAVE_M_HAVE_START_COMMA
	jmp SD_RAISE_ERROR_1           ; <start> is required
SDSAVE_M_HAVE_START_COMMA:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcr SDSAVE_M_START_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_M_START_OK:
	lda (SDLOAD_ADDR_HI_ABS)        ; SD_PARSE_NUMBER always writes here -- copy out before
	sta (SDSAVE_START_HI_ABS)       ; parsing the next number, which reuses the same scratch
	lda (SDLOAD_ADDR_LO_ABS)
	sta (SDSAVE_START_LO_ABS)

	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C
	bzs SDSAVE_M_HAVE_END_COMMA
	jmp SD_RAISE_ERROR_1            ; <end> is required
SDSAVE_M_HAVE_END_COMMA:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcr SDSAVE_M_END_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_M_END_OK:
	lda (SDLOAD_ADDR_HI_ABS)
	sta (SDSAVE_END_HI_ABS)
	lda (SDLOAD_ADDR_LO_ABS)
	sta (SDSAVE_END_LO_ABS)

	; <end> must be >= <start> -- without this check a reversed range would
	; make SD_WRITE_RANGE's own "have we reached the end address yet" scan
	; run away looking for an address it'll never reach going forward.
	; cpa's carry convention here matches SD_PARSE_HEX's own established
	; usage elsewhere in this file: carry RESET after cpa/cpi means the
	; accumulator was LESS THAN the operand.
	lda (SDSAVE_END_HI_ABS)
	cpa (SDSAVE_START_HI_ABS)
	bcr SDSAVE_M_RANGE_BAD           ; end_hi < start_hi -- invalid regardless of lo bytes
	bzr SDSAVE_M_RANGE_OK            ; end_hi > start_hi -- valid regardless of lo bytes
	lda (SDSAVE_END_LO_ABS)
	cpa (SDSAVE_START_LO_ABS)
	bcr SDSAVE_M_RANGE_BAD           ; same hi byte, end_lo < start_lo -- invalid
	bch SDSAVE_M_RANGE_OK
SDSAVE_M_RANGE_BAD:
	jmp SD_RAISE_ERROR_1
SDSAVE_M_RANGE_OK:

	; optional 4th field: either a <call> address, or a bare "-Y" (in which
	; case there's no <call> and no 5th field); optional 5th field: "-Y",
	; only reachable if the 4th field was a <call> address.
	ldi a,0x00
	sta (SDSAVE_CALL_HI_ABS)
	sta (SDSAVE_CALL_LO_ABS)
	sta (SDSAVE_YFLAG_ABS)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x0D
	bzs SDSAVE_M_ARGS_DONE            ; nothing more -- no call, no -Y
	cpi a,0x2C
	bzs SDSAVE_M_HAVE_4TH_COMMA
	jmp SD_RAISE_ERROR_1              ; trailing junk that's neither end-of-line nor a comma
SDSAVE_M_HAVE_4TH_COMMA:
	inc x
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2D                        ; '-' -- bare "-Y" as the 4th field, no <call>
	bzs SDSAVE_M_4TH_IS_YFLAG
	sjp SD_PARSE_NUMBER                ; 4th field is <call>
	bcr SDSAVE_M_CALL_OK
	jmp SD_RAISE_ERROR_1
SDSAVE_M_CALL_OK:
	lda (SDLOAD_ADDR_HI_ABS)
	sta (SDSAVE_CALL_HI_ABS)
	lda (SDLOAD_ADDR_LO_ABS)
	sta (SDSAVE_CALL_LO_ABS)
	sjp SD_PARSE_YFLAG                 ; optional trailing ",-Y" (5th field)
	bcr SDSAVE_M_ARGS_DONE
	jmp SD_RAISE_ERROR_1
SDSAVE_M_4TH_IS_YFLAG:
	sjp SD_PARSE_DASH_Y_LITERAL
	bcr SDSAVE_M_ARGS_DONE
	jmp SD_RAISE_ERROR_1

SDSAVE_M_ARGS_DONE:
	ldi a,SDSAVE_MODE_M
	sta (SDSAVE_MODE_ABS)
	jmp SD_CREATE_AND_WRITE

; Optionally consumes ",-Y" (spaces tolerated around the comma and before
; "-Y") starting at (X), setting SDSAVE_YFLAG_ABS to 1 if found. If (X)
; (after skipping spaces) is already the 0x0D terminator, there's simply
; no suffix -- not an error, SDSAVE_YFLAG_ABS is left as whatever the
; caller already set it to (callers zero it first). Returns via Carry:
; SET = there was more text but it wasn't a valid ",-Y", CLEAR = success
; (whether or not a suffix was actually found).
SD_PARSE_YFLAG:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x0D
	bzs SD_PARSE_YFLAG_OK          ; nothing more -- fine
	cpi a,0x2C
	bzr SD_PARSE_YFLAG_FAIL        ; anything other than a comma here is malformed
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_DASH_Y_LITERAL
	bcr SD_PARSE_YFLAG_OK
SD_PARSE_YFLAG_FAIL:
	sec
	rtn
SD_PARSE_YFLAG_OK:
	rec
	rtn

; Consumes a literal "-Y" at (X), requiring the 0x0D terminator
; immediately after it (nothing may follow -Y). Sets SDSAVE_YFLAG_ABS to 1
; on success. Returns via Carry: SET = didn't match, CLEAR = success.
SD_PARSE_DASH_Y_LITERAL:
	lda (x)
	cpi a,0x2D                      ; '-'
	bzr SD_PARSE_DASH_Y_FAIL
	inc x
	lda (x)
	cpi a,0x59                      ; 'Y'
	bzr SD_PARSE_DASH_Y_FAIL
	inc x
	lda (x)
	cpi a,0x0D
	bzr SD_PARSE_DASH_Y_FAIL         ; trailing junk after -Y -- malformed
	ldi a,0x01
	sta (SDSAVE_YFLAG_ABS)
	rec
	rtn
SD_PARSE_DASH_Y_FAIL:
	sec
	rtn

; Raises a genuine BASIC "ERROR 1" (the same syntax-error state/message a
; real mistyped statement produces -- confirmed live via a dedicated
; research pass this session, comparing typing an actually-invalid
; statement against this exact vector call: identical ERL/idle-return
; state either way) and resets to the idle prompt. Never returns; safe to
; call from a custom keyword's own immediate-mode dispatch context
; regardless of stack depth (confirmed live -- the handler resets S
; itself before doing anything else).
SD_RAISE_ERROR_1:
	vej 0xE4

; Raises BASIC "ERROR 40" (file not found) -- used by SD_OPEN_AND_LOAD when
; EXP_COMMAND_OPEN_SD_FILE_READ fails for the file SDLOAD was asked to
; load. Unlike error 1, ROM1.BIN has no dedicated one-byte VEJ shortcut for
; this code, so UH is set explicitly before the same general VEJ 0xE0 path
; SD_RAISE_ERROR_1 itself ultimately relies on. Never returns.
SD_RAISE_ERROR_40:
	ldi uh,40
	vej 0xE0

; Creates (overwriting, after an optional confirmation) the filename
; already staged as a length-prefixed argument at EXP_BUFFER_START_ABS (by
; SD_PARSE_QUOTED_NAME), writes SDSAVE_MODE_ABS's own payload to it via
; SD_WRITE_RANGE, closes it, and jumps to KEYWORD_RETURN -- every call
; site wants that as its own final step, so this never returns to its
; caller normally, matching SD_OPEN_AND_LOAD's own convention.
;
; Existence check: there's no dedicated "does this filename exist" wire
; command, so this opens the name for READ first (harmless -- read-only,
; and closed again immediately) purely to find out; EXP_STATUS_SUCCESS
; means it exists and (unless -Y) needs the overwrite prompt, ERROR means
; it doesn't and CREATE_SD_FILE can proceed straight away.
;
; The staged filename has to be stashed away and restored around this
; probe: EXP_COMMAND_CLOSE_SD_FILE's own response writes a 4-byte
; bytes-written count into the data window's first 4 bytes -- exactly
; where SD_PARSE_QUOTED_NAME's length prefix and the name's first two
; characters live. Confirmed the hard way (a failing in-process test):
; without the stash/restore below, SDSAVE onto an *existing* filename (the
; only path that probes-then-closes before creating) silently corrupted
; the name before CREATE_SD_FILE ever saw it, leaving the real target file
; on disk completely untouched.
SD_CREATE_AND_WRITE:
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>SDSAVE_NAME_STASH_ABS
	ldi yl,<SDSAVE_NAME_STASH_ABS
	ldi uh,0x00
	ldi ul,SDSAVE_NAME_STASH_LEN
SD_CREATE_AND_WRITE_STASH_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr SD_CREATE_AND_WRITE_STASH_LOOP
	cpi ul,0x00
	bzr SD_CREATE_AND_WRITE_STASH_LOOP

	ldi a,EXP_COMMAND_OPEN_SD_FILE_READ
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_CREATE_AND_WRITE_RESTORE  ; doesn't exist -- no confirmation needed

	ldi a,EXP_COMMAND_CLOSE_SD_FILE  ; exists -- close the probe-open first
	sta (EXP_INSTRUCTION_ABS)
	lda (SDSAVE_YFLAG_ABS)
	cpi a,0x00
	bzr SD_CREATE_AND_WRITE_RESTORE  ; -Y given -- skip the prompt, overwrite unconditionally

	ldi uh,>SDSAVE_CONFIRM_MSG
	ldi ul,<SDSAVE_CONFIRM_MSG
	ldi xl,SDSAVE_CONFIRM_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcs SD_CREATE_AND_WRITE_ABORT     ; BREAK -- abort, no write
	cpi a,KEY_Y
	bzs SD_CREATE_AND_WRITE_RESTORE
	bch SD_CREATE_AND_WRITE_ABORT     ; anything but Y -- abort

SD_CREATE_AND_WRITE_RESTORE:
	ldi xh,>SDSAVE_NAME_STASH_ABS
	ldi xl,<SDSAVE_NAME_STASH_ABS
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,0x00
	ldi ul,SDSAVE_NAME_STASH_LEN
SD_CREATE_AND_WRITE_RESTORE_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr SD_CREATE_AND_WRITE_RESTORE_LOOP
	cpi ul,0x00
	bzr SD_CREATE_AND_WRITE_RESTORE_LOOP

	ldi a,EXP_COMMAND_CREATE_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzs SD_CREATE_AND_WRITE_CREATED
SD_CREATE_AND_WRITE_ABORT:
	jmp KEYWORD_RETURN

SD_CREATE_AND_WRITE_CREATED:
	lda (SDSAVE_MODE_ABS)
	cpi a,SDSAVE_MODE_BASIC
	bzs SD_CREATE_AND_WRITE_BASIC_RANGE

	; M mode: write the 4-byte header (target = <start>, call = <call> or
	; 0x0000) first, matching SD_OPEN_AND_LOAD's own read side exactly.
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	ldi a,0x04
	sta (EXP_BUFFER_START_ABS+1)
	lda (SDSAVE_START_HI_ABS)
	sta (EXP_BUFFER_START_ABS+2)
	lda (SDSAVE_START_LO_ABS)
	sta (EXP_BUFFER_START_ABS+3)
	lda (SDSAVE_CALL_HI_ABS)
	sta (EXP_BUFFER_START_ABS+4)
	lda (SDSAVE_CALL_LO_ABS)
	sta (EXP_BUFFER_START_ABS+5)
	ldi a,EXP_COMMAND_WRITE_TO_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_CREATE_AND_WRITE_CLOSE     ; header write failed -- still close/finalize below

	lda (SDSAVE_START_HI_ABS)
	sta (SDSAVE_WRITE_HI_ABS)
	lda (SDSAVE_START_LO_ABS)
	sta (SDSAVE_WRITE_LO_ABS)
	lda (SDSAVE_END_HI_ABS)
	sta (SDSAVE_RANGE_END_HI_ABS)
	lda (SDSAVE_END_LO_ABS)
	sta (SDSAVE_RANGE_END_LO_ABS)
	sjp SD_WRITE_RANGE
	bch SD_CREATE_AND_WRITE_CLOSE

SD_CREATE_AND_WRITE_BASIC_RANGE:
	lda (BASIC_PROGRAM_START_HI_ABS)  ; live pointer, not a hardcoded constant -- see its own
	sta (SDSAVE_WRITE_HI_ABS)         ; comment for why (shifts with installed RAM on real hardware)
	lda (BASIC_PROGRAM_START_LO_ABS)
	sta (SDSAVE_WRITE_LO_ABS)
	lda (BASIC_PROGRAM_END_HI_ABS)
	sta (SDSAVE_RANGE_END_HI_ABS)
	lda (BASIC_PROGRAM_END_LO_ABS)
	sta (SDSAVE_RANGE_END_LO_ABS)
	sjp SD_WRITE_RANGE

SD_CREATE_AND_WRITE_CLOSE:
	ldi a,EXP_COMMAND_CLOSE_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	jmp KEYWORD_RETURN

; Writes the inclusive byte range [SDSAVE_WRITE_HI/LO_ABS,
; SDSAVE_RANGE_END_HI/LO_ABS] from RAM to the currently-open SD file,
; chunked at <=254 bytes per EXP_COMMAND_WRITE_TO_SD_FILE call (matching
; SD_OPEN_AND_LOAD's own read-side chunk size -- the data window page has
; 256 bytes, 2 of which are the BE length prefix). Advances
; SDSAVE_WRITE_HI/LO_ABS as it goes. On a WRITE_TO_SD_FILE failure
; partway through, stops immediately (matching SD_OPEN_AND_LOAD_READ_
; LOOP's own "stop, let the caller close/finalize with what we have"
; precedent -- no separate error signaling back to the caller).
SD_WRITE_RANGE:
SD_WRITE_RANGE_CHUNK:
	ldi yh,>(EXP_BUFFER_START_ABS+2)
	ldi yl,<(EXP_BUFFER_START_ABS+2)
	lda (SDSAVE_WRITE_HI_ABS)
	sta xh
	lda (SDSAVE_WRITE_LO_ABS)
	sta xl
	ldi a,0x00
	sta (SDSAVE_CHUNKLEN_ABS)
	sta (SDSAVE_RANGE_DONE_ABS)
SD_WRITE_RANGE_COPY_LOOP:
	lda xh
	cpa (SDSAVE_RANGE_END_HI_ABS)
	bzr SD_WRITE_RANGE_NOT_AT_END
	lda xl
	cpa (SDSAVE_RANGE_END_LO_ABS)
	bzs SD_WRITE_RANGE_AT_END
SD_WRITE_RANGE_NOT_AT_END:
	tin
	lda (SDSAVE_CHUNKLEN_ABS)     ; inc a, not adi a,0x01 -- ADI incorporates any leftover carry
	inc a                         ; from the cpa comparisons above, which would silently add 2
	sta (SDSAVE_CHUNKLEN_ABS)
	cpi a,0xFE                   ; 254 -- chunk full
	bzr SD_WRITE_RANGE_COPY_LOOP
	bch SD_WRITE_RANGE_FLUSH
SD_WRITE_RANGE_AT_END:
	tin                          ; copy the final byte
	lda (SDSAVE_CHUNKLEN_ABS)
	inc a
	sta (SDSAVE_CHUNKLEN_ABS)
	ldi a,0x01
	sta (SDSAVE_RANGE_DONE_ABS)

SD_WRITE_RANGE_FLUSH:
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDSAVE_CHUNKLEN_ABS)
	sta (EXP_BUFFER_START_ABS+1)
	ldi a,EXP_COMMAND_WRITE_TO_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_WRITE_RANGE_STOP        ; write failed -- stop, let the caller close/finalize

	lda xh
	sta (SDSAVE_WRITE_HI_ABS)
	lda xl
	sta (SDSAVE_WRITE_LO_ABS)

	lda (SDSAVE_RANGE_DONE_ABS)
	cpi a,0x00
	bzs SD_WRITE_RANGE_CHUNK        ; still 0 -- not done yet, start another chunk
SD_WRITE_RANGE_STOP:
	rtn

; SDDF needs a binary-to-decimal-ASCII conversion (or a reused ROM
; routine) to print FS_GetVolumeFreeSpace/FS_GetVolumeSize's uint32
; results.
SDDF_ROUTINE:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; Memcopy utilities -- unrelated to the SD keywords, CALLed directly (not
; BASIC keywords), unchanged from the prior layout other than moving here.
; Reads a 6-byte parameter block (source, dest, count; each 16-bit BE)
; from the data window's first 6 bytes, then copies `count` bytes.
MEMCOPY_PARAMS_ABS .equ EXP_BUFFER_START_ABS

MEMCOPY_ROUTINE:
	lda (MEMCOPY_PARAMS_ABS+0)
	sta xh
	lda (MEMCOPY_PARAMS_ABS+1)
	sta xl
	lda (MEMCOPY_PARAMS_ABS+2)
	sta yh
	lda (MEMCOPY_PARAMS_ABS+3)
	sta yl
	lda (MEMCOPY_PARAMS_ABS+4)
	sta uh
	lda (MEMCOPY_PARAMS_ABS+5)
	sta ul
MEMCOPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr MEMCOPY_LOOP
	cpi ul,0x00
	bzr MEMCOPY_LOOP
	rtn

; PV-swap variant: PV low for the read, high for the write, around each
; byte, instead of TIN's single-PV-level transfer. Same parameter layout.
MEMCOPY_PV_SWAP_ROUTINE:
	lda (MEMCOPY_PARAMS_ABS+0)
	sta xh
	lda (MEMCOPY_PARAMS_ABS+1)
	sta xl
	lda (MEMCOPY_PARAMS_ABS+2)
	sta yh
	lda (MEMCOPY_PARAMS_ABS+3)
	sta yl
	lda (MEMCOPY_PARAMS_ABS+4)
	sta uh
	lda (MEMCOPY_PARAMS_ABS+5)
	sta ul
MEMCOPY_PV_SWAP_LOOP:
	rpv
	lda (x)
	spv
	sta (y)
	inc x
	inc y
	dec u
	cpi uh,0x00
	bzr MEMCOPY_PV_SWAP_LOOP
	cpi ul,0x00
	bzr MEMCOPY_PV_SWAP_LOOP
	rtn

; ---------------------------------------------------------------------
; Guard: everything above must fit in the 4K ROM region (0x9000-0x9FFF).
; .org can only move the location counter forward within one absolute area
; -- if the content above already overran past 0xA000, this line itself
; fails to assemble instead of silently wrapping past the window.
	.org ROM_REGION_END
