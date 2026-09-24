; rom.asm -- production ROM image for the PC1500-PSOC5 expansion board.
;
; Layout: the 8K LH5801-visible window (0x8000-0x9FFF) splits into a 2K live
; data window (0x8000-0x87FF: command/status/parameter exchange with
; DoCommand() in main.c, plus bulk data like directory listings) and a 6K
; ROM region (0x8800-0x9FFF: sentinel + keyword index + keyword table +
; routines) -- see rom_defs.inc for the exact split. There's no real "ROM"
; chip backing any of this; it's all the same RAM buffer main.c's
; InitBuffer() populates once at boot, so nothing in this file's assembled
; output may extend past 0x9FFF (see the .org guard at the end) or it would
; wrap past the window entirely. (Moved here from 0x9000 -- 4K ROM/4K data
; -- to grow the ROM region to 6K as the keyword table filled up, 2026-08-18
; session; EXP_DIR_MAX_ENTRIES was recomputed for the smaller 2K data
; window at the same time, see PC_EXP.h's own comment.)
;
; ROM still doesn't live at the board's natural 0x8000, because of two
; confirmed real-ROM quirks in the base BASIC ROM's keyword-table walker
; (see rom_defs.inc's own comment for the first one's full writeup;
; both were found by tracing live execution with entertrace, not inferred
; from documentation):
;
; 1. On a name mismatch, the walker skips forward hunting for the first
;    byte *strictly greater than* 0xE0 to find the next entry's code
;    field. Page 8000's own required PV-low code value is exactly 0xE0 --
;    sitting on that boundary, not past it -- so the skip-scan glides
;    straight through it on that page specifically. 8800's required code
;    (0xE1, page index 1) is safely clear of it.
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
; of marker; off page 8000 with correct code values, a second entry only
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
; Commands are SD-prefixed (SDLS/SDFMT/SDLOAD/SDSAVE/SDRM/SDCP/SDDF/SDMV/
; SDCD/SDMKDIR/SDRMDIR/SDPWD), not S-prefixed -- renamed from the original
; SLS/SFMT/SLOAD/SSAVE/SRM to avoid colliding with the single-letter
; S-prefix convention as more commands get added. The original "SDF"
; (unchanged from the pre-rename "SDF", which already fit the new scheme's
; spelling) turned out to collide with the *new* "SDFMT" -- SDF is a
; strict prefix of SDFMT, so the table walker greedily matched SDF as a
; complete keyword and left "MT" as untokenized literal text (confirmed
; live: typing SDFMT tokenized as SDF's own code value followed by raw
; "MT" bytes) -- the exact same class of bug already known here for
; "SFORMAT" containing the real built-in keyword FOR as a substring (see
; SDFMT's own table-entry comment). Renamed to SDDF (matching its actual
; purpose -- free space, and the Unix `df` convention) to clear the
; collision. The same SDRM-vs-SDRMDIR collision showed up again when the
; directory commands were added -- resolved this time by table *ordering*
; instead of a rename (see SDRMDIR's own table-entry comment) since the
; user wanted to keep both names as-is. SDMV, added later, shares a
; "SDM" prefix with SDMKDIR but diverges at the 4th character, so no
; ordering constraint there. Checked every other pair by hand; nothing
; else in the current set collides.
;
; All seventeen keywords (the original twelve plus SDOPEN/SDCLOSE/SDINPUT/
; SDPRINT/SDSKIP, see their own section's comment for why those five are
; table-named without a literal '#') are fully implemented. A custom
; keyword's own trailing typed text sits as raw, untokenized ASCII
; directly in DISP_BUFFER (7BB0H) right after the keyword's own 2-byte
; code, up to the 0DH terminator -- no interpreter-level expression
; evaluation happens for it (confirmed live via entertrace on a running
; pc1500emu, see SDLOAD_ROUTINE's own comment).

	.area CODE (ABS)
	.include "rom_defs.inc"

; ROMRAMTEST (standalone SRAM chip read/write diagnostic, 2026-09-21) lived
; here at 0x8000 -- removed 2026-09-22, board owner's own call, once
; RAMTEST2 below (which supersedes it -- same test, relocates itself to
; real PC-1500 RAM to also isolate the data-window relay path) had done its
; job. See git history for ROMRAMTEST's own code if it's ever needed again.

; ---------------------------------------------------------------------
; RAMTEST2 -- board owner's own request (2026-09-22), to narrow down
; whether ROMRAMTEST's remaining false mismatches (E/F equal to each
; other but to neither the real pattern nor the real SRAM byte, e.g.
; "BB") come from the RP2350's PIO/DMA-served 0x8000-0x87FF data window,
; or from the external SRAM chip/GreenPAK Remap path itself. Copies
; itself to real PC-1500 RAM at 0x4000 and runs from there; the ONLY
; remaining MCU interaction is EXP_COMMAND_ROM_COPY_BEGIN/
; EXP_COMMAND_ROM_FROM_MCU (setting/resetting the GreenPAK virtual
; inputs) -- pattern storage, expected/found capture, and hex-to-ASCII
; formatting all happen in real RAM or LH5801 registers only, never
; touching the data window again once relocated. If this version still
; shows the same false-mismatch signature, the SRAM chip/GreenPAK path
; is implicated; if it doesn't, the data window's own PIO/DMA serving is.
;
; Same relocation technique as the existing ROM_COPY_TRAMPOLINE/
; ROM_COPY_RELOCATABLE_START below (that code predates this one and is
; the reference this mirrors): only relative branches (bzs/bzr/bcr/bch)
; for control flow within RAMTEST2_BODY, since JMP/SJP are absolute (bake
; in a fixed 16-bit target at assemble time) and would still point at
; this block's ORIGINAL, pre-relocation address afterward -- there are
; none used for internal control flow here, deliberately. Every
; absolute reference (STA/LDA/CPA memory operand, or a JMP/SJP target)
; to a label DECLARED INSIDE RAMTEST2_BODY goes through its own _ABS
; .equ constant (RAMTEST2_RAM_BASE + that label's offset from
; RAMTEST2_BODY, both compile-time constants) instead of the bare label,
; so it resolves to the correct POST-relocation address regardless of
; where these bytes physically sit in ROM before being copied. JMP/SJP
; to a genuinely external, fixed address (DISP_N_CHARS0, KEYSCAN_WAIT,
; EXP_INSTRUCTION_ABS, ROM_BASE, STAGE_CHECKSUM_LEN, etc.) needs no
; translation -- those never move regardless of relocation.
;
; No subroutine calls within RAMTEST2_BODY (SJP to an internal label
; would have the same stale-address problem as JMP) -- the 4-pattern
; sweep is a genuine loop (X walks RAMTEST2_PATTERNS, not 4 unrolled
; calls the way ROMRAMTEST's own ROMRAMTEST_BEGIN_OK does it), and the
; hex-digit formatting is fully inlined per field rather than shared,
; trading code size for avoiding the relocation hazard entirely -- fine
; for a throwaway diagnostic tool.
;
; Invoked via CALL <this label's address>, immediately after boot/reset --
; lives in the general payload/scratch part of the data window (0x8000-
; 0x84FF, ROMRAMTEST's old spot, now this routine's alone), NOT collision-
; proof: it (and/or its data) may already be overwritten if some other SD/
; STAGE command has run first in this session.
RAMTEST2_RAM_BASE .equ 0x4000

	.org 0x8000
RAMTEST2:
	ldi xh,>RAMTEST2_BODY
	ldi xl,<RAMTEST2_BODY
	ldi yh,>RAMTEST2_RAM_BASE
	ldi yl,<RAMTEST2_RAM_BASE
	ldi uh,>RAMTEST2_BODY_SIZE
	ldi ul,<RAMTEST2_BODY_SIZE
RAMTEST2_STAGE_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr RAMTEST2_STAGE_LOOP
	cpi ul,0x00
	bzr RAMTEST2_STAGE_LOOP
	jmp (RAMTEST2_RAM_BASE)     ; absolute jump to a FIXED RAM address --
	                             ; correct regardless of where this
	                             ; trampoline itself executes from (it's
	                             ; not relocated) -- same technique as
	                             ; ROM_COPY_TRAMPOLINE's own jmp below

; ---- Everything from here to RAMTEST2_BODY_END is copied verbatim to
; RAMTEST2_RAM_BASE (0x4000) and must be position-independent -- see this
; section's own header comment above for the rules. ----
RAMTEST2_BODY:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
RAMTEST2_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs RAMTEST2_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs RAMTEST2_BEGIN_OK
	bch RAMTEST2_BEGIN_FAILED

RAMTEST2_BEGIN_OK:
	rie                          ; same interrupt-safety precaution as
	                             ; ROMRAMTEST/STAGE_COPY_BEGIN_OK -- see
	                             ; either's own comment for why
	ldi xh,>RAMTEST2_PATTERNS_ABS
	ldi xl,<RAMTEST2_PATTERNS_ABS
RAMTEST2_PATTERN_LOOP:
	ldi yh,>ROM_BASE
	ldi yl,<ROM_BASE
	ldi uh,>STAGE_CHECKSUM_LEN    ; 6144 -- same external, fixed constant
	ldi ul,<STAGE_CHECKSUM_LEN    ; ROMRAMTEST/STAGE's own checksum uses
RAMTEST2_BYTE_LOOP:
	lda (x)                       ; A = current pattern (X stays parked on
	                              ; this pattern-table entry for the whole
	                              ; inner loop -- only Y/U move here)
	sta (y)                       ; write pattern to SRAM at Y -- real
	                              ; GreenPAK-Remap SRAM chip access, no MCU
	                              ; relay involved either way
	lda (y)                       ; read it straight back
	cpa (x)
	bzs RAMTEST2_BYTE_OK
	bch RAMTEST2_MISMATCH
RAMTEST2_BYTE_OK:
	inc y
	dec u
	cpi uh,0x00
	bzr RAMTEST2_BYTE_LOOP
	cpi ul,0x00
	bzr RAMTEST2_BYTE_LOOP
	inc x
	lda xh
	cpi a,>RAMTEST2_PATTERNS_END_ABS
	bzr RAMTEST2_PATTERN_LOOP
	lda xl
	cpi a,<RAMTEST2_PATTERNS_END_ABS
	bzr RAMTEST2_PATTERN_LOOP

	; all 4 patterns passed
	sie
	ldi uh,>RAMTEST2_OK_MSG_ABS
	ldi ul,<RAMTEST2_OK_MSG_ABS
	ldi xl,RAMTEST2_OK_MSG_LEN
	sjp DISP_N_CHARS0             ; safe -- system ROM, unaffected by Remap
	sjp KEYSCAN_WAIT
	bch RAMTEST2_REVERT

RAMTEST2_BEGIN_FAILED:
	ldi uh,>RAMTEST2_BEGIN_FAIL_MSG_ABS
	ldi ul,<RAMTEST2_BEGIN_FAIL_MSG_ABS
	ldi xl,RAMTEST2_BEGIN_FAIL_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	rtn                           ; safe direct -- Remap never switched,
	                              ; same reasoning as ROMRAMTEST_BEGIN_
	                              ; FAILED's own bare-error return

RAMTEST2_MISMATCH:
	sta (RAMTEST2_FOUND_ABS)      ; A = found (from the failed cpa,
	                              ; unmodified since)
	lda yh
	sta (RAMTEST2_ADDR_HI_ABS)
	lda yl
	sta (RAMTEST2_ADDR_LO_ABS)
	lda (x)
	sta (RAMTEST2_EXPECTED_ABS)
	; NOTE (2026-09-22): sie deliberately NOT called here -- see
	; ROMRAMTEST_FAILED's own comment for the identical bug this used to
	; have: interrupts must stay disabled through the whole hex-nibble
	; arithmetic below, or a periodic timer interrupt landing mid-sequence
	; can clobber A between a shr/ani mask and its own cpi/adi conversion,
	; producing an out-of-range "nibble" (e.g. a displayed digit past
	; 'F'). Moved to RAMTEST2_REVERT below, matching STAGE_COPY_ROUTINE_
	; ABS's own proven convention (sie only in its shared revert tail).

	; ---- addr hi -> template offset 9-10 ----
	lda (RAMTEST2_ADDR_HI_ABS)
	sta (RAMTEST2_HEXSCR_ABS)
	shr
	shr
	shr
	shr
	cpi a,0x0A
	bcr RAMTEST2_H1_LOW
	adi a,0x37
	bch RAMTEST2_H1_DONE
RAMTEST2_H1_LOW:
	adi a,0x30
RAMTEST2_H1_DONE:
	sta (RAMTEST2_MSG_D9_ABS)
	lda (RAMTEST2_HEXSCR_ABS)
	ani a,0x0F
	cpi a,0x0A
	bcr RAMTEST2_H2_LOW
	adi a,0x37
	bch RAMTEST2_H2_DONE
RAMTEST2_H2_LOW:
	adi a,0x30
RAMTEST2_H2_DONE:
	sta (RAMTEST2_MSG_D10_ABS)

	; ---- addr lo -> template offset 11-12 ----
	lda (RAMTEST2_ADDR_LO_ABS)
	sta (RAMTEST2_HEXSCR_ABS)
	shr
	shr
	shr
	shr
	cpi a,0x0A
	bcr RAMTEST2_H3_LOW
	adi a,0x37
	bch RAMTEST2_H3_DONE
RAMTEST2_H3_LOW:
	adi a,0x30
RAMTEST2_H3_DONE:
	sta (RAMTEST2_MSG_D11_ABS)
	lda (RAMTEST2_HEXSCR_ABS)
	ani a,0x0F
	cpi a,0x0A
	bcr RAMTEST2_H4_LOW
	adi a,0x37
	bch RAMTEST2_H4_DONE
RAMTEST2_H4_LOW:
	adi a,0x30
RAMTEST2_H4_DONE:
	sta (RAMTEST2_MSG_D12_ABS)

	; ---- expected -> template offset 16-17 ----
	lda (RAMTEST2_EXPECTED_ABS)
	sta (RAMTEST2_HEXSCR_ABS)
	shr
	shr
	shr
	shr
	cpi a,0x0A
	bcr RAMTEST2_H5_LOW
	adi a,0x37
	bch RAMTEST2_H5_DONE
RAMTEST2_H5_LOW:
	adi a,0x30
RAMTEST2_H5_DONE:
	sta (RAMTEST2_MSG_D16_ABS)
	lda (RAMTEST2_HEXSCR_ABS)
	ani a,0x0F
	cpi a,0x0A
	bcr RAMTEST2_H6_LOW
	adi a,0x37
	bch RAMTEST2_H6_DONE
RAMTEST2_H6_LOW:
	adi a,0x30
RAMTEST2_H6_DONE:
	sta (RAMTEST2_MSG_D17_ABS)

	; ---- found -> template offset 21-22 ----
	lda (RAMTEST2_FOUND_ABS)
	sta (RAMTEST2_HEXSCR_ABS)
	shr
	shr
	shr
	shr
	cpi a,0x0A
	bcr RAMTEST2_H7_LOW
	adi a,0x37
	bch RAMTEST2_H7_DONE
RAMTEST2_H7_LOW:
	adi a,0x30
RAMTEST2_H7_DONE:
	sta (RAMTEST2_MSG_D21_ABS)
	lda (RAMTEST2_HEXSCR_ABS)
	ani a,0x0F
	cpi a,0x0A
	bcr RAMTEST2_H8_LOW
	adi a,0x37
	bch RAMTEST2_H8_DONE
RAMTEST2_H8_LOW:
	adi a,0x30
RAMTEST2_H8_DONE:
	sta (RAMTEST2_MSG_D22_ABS)

	ldi uh,>RAMTEST2_FAIL_TEMPLATE_ABS
	ldi ul,<RAMTEST2_FAIL_TEMPLATE_ABS
	ldi xl,RAMTEST2_FAIL_TEMPLATE_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	; falls through into RAMTEST2_REVERT

RAMTEST2_REVERT:
	sie                           ; re-enable interrupts before returning
	                              ; either way -- moved here (2026-09-22)
	                              ; from RAMTEST2_MISMATCH's own hex-build
	                              ; section, matching STAGE_COPY_REVERT's
	                              ; actual convention (see
	                              ; RAMTEST2_MISMATCH's own comment)
	ldi a,EXP_COMMAND_ROM_FROM_MCU
	sta (EXP_INSTRUCTION_ABS)
RAMTEST2_REVERT_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs RAMTEST2_REVERT_POLL
	rtn                           ; plain RTN -- unwinds to the ORIGINAL
	                              ; CALL's own return address, still on
	                              ; the stack: RAMTEST2's own bootstrap
	                              ; used a bare JMP (not SJP) to get here,
	                              ; so nothing extra was ever pushed --
	                              ; same reasoning as ROM_COPY_RELOCATABLE_
	                              ; START's own RTN, below

RAMTEST2_PATTERNS: .db 0x00,0xFF,0xAA,0x55
RAMTEST2_PATTERNS_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_PATTERNS - RAMTEST2_BODY))
RAMTEST2_PATTERNS_END:
RAMTEST2_PATTERNS_END_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_PATTERNS_END - RAMTEST2_BODY))

RAMTEST2_OK_MSG: .ascii "RAMTST2: SUCCESS"
RAMTEST2_OK_MSG_LEN .equ 16
RAMTEST2_OK_MSG_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_OK_MSG - RAMTEST2_BODY))

RAMTEST2_BEGIN_FAIL_MSG: .ascii "RAMTST2: BEGIN FAIL"
RAMTEST2_BEGIN_FAIL_MSG_LEN .equ 19
RAMTEST2_BEGIN_FAIL_MSG_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_BEGIN_FAIL_MSG - RAMTEST2_BODY))

; Mutable -- the hex-digit fields (offsets 9-12, 16-17, 21-22) get
; overwritten in place before every display; the '0' placeholders only
; matter as filler bytes of the right count/position. Same layout as
; ROMRAMTEST_FAIL_TEMPLATE (9-char "RAMTST2 @" prefix matches "RAMTEST @"'s
; own 9-char length exactly, so every offset below lines up the same way).
RAMTEST2_FAIL_TEMPLATE: .ascii "RAMTST2 @0000 E:00 F:00"
RAMTEST2_FAIL_TEMPLATE_LEN .equ 23
RAMTEST2_FAIL_TEMPLATE_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE - RAMTEST2_BODY))
RAMTEST2_MSG_D9_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+9 - RAMTEST2_BODY))
RAMTEST2_MSG_D10_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+10 - RAMTEST2_BODY))
RAMTEST2_MSG_D11_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+11 - RAMTEST2_BODY))
RAMTEST2_MSG_D12_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+12 - RAMTEST2_BODY))
RAMTEST2_MSG_D16_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+16 - RAMTEST2_BODY))
RAMTEST2_MSG_D17_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+17 - RAMTEST2_BODY))
RAMTEST2_MSG_D21_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+21 - RAMTEST2_BODY))
RAMTEST2_MSG_D22_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FAIL_TEMPLATE+22 - RAMTEST2_BODY))

RAMTEST2_ADDR_HI: .db 0x00
RAMTEST2_ADDR_HI_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_ADDR_HI - RAMTEST2_BODY))
RAMTEST2_ADDR_LO: .db 0x00
RAMTEST2_ADDR_LO_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_ADDR_LO - RAMTEST2_BODY))
RAMTEST2_EXPECTED: .db 0x00
RAMTEST2_EXPECTED_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_EXPECTED - RAMTEST2_BODY))
RAMTEST2_FOUND: .db 0x00
RAMTEST2_FOUND_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_FOUND - RAMTEST2_BODY))
RAMTEST2_HEXSCR: .db 0x00
RAMTEST2_HEXSCR_ABS .equ (RAMTEST2_RAM_BASE + (RAMTEST2_HEXSCR - RAMTEST2_BODY))

RAMTEST2_BODY_END:
RAMTEST2_BODY_SIZE .equ (RAMTEST2_BODY_END - RAMTEST2_BODY)
; Budget: 0x8000-0x83FF, shared with RAMTEST2's own bootstrap +
; RAMTEST2_BODY_SIZE above -- check against rom.lst/rom.map once
; assembled. RAMTEST2_BODY_SIZE itself is also the byte count actually
; copied to 0x4000 -- confirm against real PC-1500 RAM availability there
; before relying on this (board owner's own call, not verified here).
; NOTE: this range overlaps EXP_BUFFER_START_ABS's own 1024-byte payload
; window (0x8000-0x83FF) -- fine for RAMTEST2 itself (a plain SRAM
; POKE/PEEK test, never invokes GET_BLOCK), but anything placed here that
; DOES use STAGE's BEGIN/GET_BLOCK commands will get its own code
; overwritten the instant GET_BLOCK stages a block.
;
; STAGE_MANUAL_BLOCK0 (a one-block, no-report manual-inspection diagnostic,
; CALL 33792 at 0x8400) used to live in the pocket below -- removed
; 2026-09-23 (board owner's own call): STAGE DEBUG itself now leaves the
; buffer completely untouched on any failure too (see STAGE_COPY_
; ROUTINE_ABS's own header below), making this a redundant, separate
; command for the same job. 0x8400-0x84FF is folded into STAGE_COPY_
; ROUTINE_ABS's own pocket below instead, both for the reclaimed space
; and because it's the same kind of protected, nothing-else-writes-here
; region.
; ---------------------------------------------------------------------
; STAGE keyword's own ROM-to-SRAM copy routine (2026-09) -- permanently
; resident here at a fixed address, NOT relocated at runtime the way the
; old, disabled ROM_COPY_TRAMPOLINE/ROM_COPY_RELOCATABLE_START (further
; down this file) used to be. That trampoline only ever ran once at cold
; boot, before any user program existed, so borrowing a data-window
; scratch offset for it was harmless. STAGE runs from an already-live
; BASIC session -- borrowing general user RAM the way a runtime
; trampoline does would risk colliding with a running program's own
; variables/arrays, so this lives at a fixed address instead (board
; owner's own design choice).
;
; STAGE_COPY_ROUTINE_ABS = 0x8400 (2026-09-23, moved down from 0x8500 --
; STAGE_MANUAL_BLOCK0, the routine this pocket used to be kept clear of,
; was removed the same day, so its own 0x8400-0x84FF page is folded in
; here instead, both to reclaim the space and because it's the same kind
; of "nothing else writes here at runtime" protected region).
; ROM_COPY_STAGE_ABS's own old trampoline concern (re-copying itself into
; 0x8400+ on every cold boot via BOOT_SELFCHECK_ENTRY) is moot regardless
; -- that call is disabled (see BOOT_SELFCHECK_ENTRY's own comment).
;
; Free-space audit of the 2K data window (0x8000-0x87FF), confirmed by
; reading pc_exp.h/rom_defs.inc directly, not assumed: 0x8000-0x83FF
; (1024 bytes) is the EXP_BUFFER_START_*/EXP_MAX_TRANSFER_LEN bulk
; payload window (EXP_SCRATCH_PAGE, 0x8100-0x81FF, is a SUBSET of this,
; not additional space); 0x87FD-0x87FE is EXP_LENGTH_PORT_*; 0x87FF is
; EXP_INSTRUCTION_*. That leaves 0x8400-0x87FC (1020 bytes) genuinely
; free and collision-proof for this routine plus ROM_RESET_REMAP
; together (see STAGE_COPY_SIZE below once assembled, and
; ROM_RESET_REMAP's own header comment for why it has to live in this
; same protected pocket rather than the general 0x8000-0x83FF scratch
; area).
;
; SAFETY RULE, the same hazard ROM_COPY_RELOCATABLE_START's own header
; comment documents for itself: from the instant EXP_COMMAND_
; ROM_COPY_BEGIN succeeds until the last GET_BLOCK lands, this routine
; must never SJP/JMP to anything physically resident at ROM_BASE+
; (EC_WAIT_NOT_BUSY, KEYWORD_RETURN, SD_RAISE_ERROR_1, etc.) -- once
; BEGIN's I2C write flips GreenPAK1/2's Remap virtual inputs, every
; further bus read of 0x8800-0x9FFF (instruction fetches included) is
; physically answered by the SRAM chip, which isn't fully populated
; until the final block lands. System-ROM calls (KEYSCAN_WAIT 0xE243,
; DISP_N_CHARS0 0xED3B) are always safe -- a completely separate address
; range, unaffected by this module's own Remap state. Once either the
; final GET_BLOCK+checksum+FINISH sequence succeeds, or (RAM mode only,
; see below) the revert-and-report failure path confirms EXP_COMMAND_
; ROM_FROM_MCU succeeded, ROM_BASE+ mirrors buffer[8..31] again and is
; safe.
;
; Failure handling never halts (board owner's explicit choice, so testing
; this feature doesn't need a reboot after every failed attempt), and
; differs by mode (2026-09-23, board owner's own request: "STAGE DEBUG
; leaves state as is when it fails, and STAGE RAM will revert remap and
; write enable when it fails"). Both modes show a failure message; after
; that, RAM mode issues EXP_COMMAND_ROM_FROM_MCU (already real -- see
; monitor.c) to revert both chips' Remap and clear write-enable, confirms
; it succeeded, then returns to BASIC normally -- only if that revert
; itself fails is there truly no safe address left to return to, and that
; one case still halts. DEBUG mode never touches EXP_BUFFER_START_ABS or
; dispatches anything to the MCU on failure at all -- it returns via a
; local copy of KEYWORD_RETURN's own fixup sequence instead (see
; STAGE_DEBUG_FAIL_RETURN's own comment), leaving Remap/WE and the
; payload window exactly as they were for direct PEEK inspection.
STAGE_COPY_ROUTINE_ABS .equ 0x8400
STAGE_CHECKSUM_LEN .equ (ROM_REGION_END - ROM_BASE)   ; 6144

	.org STAGE_COPY_ROUTINE_ABS
STAGE_COPY_START:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_COPY_BEGIN_OK
	jmp STAGE_COPY_BEGIN_FAILED   ; couldn't begin -- Remap never switched,
	                              ; ROM_BASE+ never at risk, safe to raise
	                              ; a normal error directly (bzr's own
	                              ; short range can't reach this far down
	                              ; past the per-block verify code below,
	                              ; hence the bzs-then-jmp split)
STAGE_COPY_BEGIN_OK:
	; Interrupts disabled for the whole copy (2026-09-21) -- found the hard
	; way, via STAGE DEBUG's own per-byte verify loop reporting a real,
	; reproducible "mismatch" where expected and found were IDENTICAL
	; (E:26 F:26 on real hardware): EC_WAIT_NOT_BUSY (used by every OTHER
	; command in this ROM) explicitly SIEs before its own HLT while
	; polling, confirming interrupts are normally left enabled through
	; command dispatch -- but this routine hand-rolls its OWN polling
	; instead of EC_WAIT_NOT_BUSY (see this block's own top comment for
	; why: Remap safety, unrelated to interrupts), and never touched the
	; interrupt-enable flag either way, so it silently inherited whatever
	; was already active. A real periodic timer interrupt landing between
	; the debug loop's own `lda (y)` readback and `cpa`, or between `cpa`
	; and its branch, corrupts A and/or the flags mid-comparison -- easy
	; to hit once the per-byte loop's much slower wall-clock time (an
	; extra live bus read-back per byte, not just per block) widens the
	; window, but a latent risk for the fast tin-based copy below too,
	; just a much smaller window historically not yet observed to land on
	; it. RIE here doesn't stall anything -- every poll in this routine is
	; a tight busy-spin already, none of them HLT/rely on a wakeup.
	rie
	ldi yh,>ROM_BASE              ; Y = running SRAM write pointer
	ldi yl,<ROM_BASE
	ldi a,0x00
	sta (STAGE_BLOCK_INDEX)       ; 0-based block counter, purely for the
	                              ; per-block log messages below
STAGE_COPY_BLOCK_LOOP:
	lda yh                        ; save this block's SRAM start address --
	sta (STAGE_BLOCK_START_HI)    ; needed after the tin copy below to read
	lda yl                        ; the block back for verification (X gets
	sta (STAGE_BLOCK_START_LO)    ; reused as the copy source in the meantime)

	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_BLOCK_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_COPY_BLOCK_GOT_IT
	jmp STAGE_COPY_MIDFAIL        ; GET_BLOCK failed mid-copy -- Remap
	                              ; already switched, ROM_BASE+ unsafe;
	                              ; revert-and-report, not a normal error
	                              ; (bzs-then-jmp split, same reason as
	                              ; STAGE_COPY_BEGIN_OK above)
STAGE_COPY_BLOCK_GOT_IT:
	ldi xh,>EXP_BUFFER_START_ABS  ; X = freshly-staged block (source),
	ldi xl,<EXP_BUFFER_START_ABS  ; always exactly EXP_MAX_TRANSFER_LEN
	ldi uh,>EXP_MAX_TRANSFER_LEN  ; bytes (6144/1024=6 exact blocks, no
	ldi ul,<EXP_MAX_TRANSFER_LEN  ; partial final block)
	lda (STAGE_DEBUG_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_COPY_LOOP       ; normal mode -- fast tin copy, below
	jmp STAGE_COPY_VERIFY_COPY_LOOP ; STAGE DEBUG -- slow per-byte write+
	                                ; verify copy, further down
STAGE_COPY_COPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr STAGE_COPY_COPY_LOOP
	cpi ul,0x00
	bzr STAGE_COPY_COPY_LOOP
	jmp STAGE_COPY_BLOCK_COPIED

; STAGE DEBUG's own copy loop (2026-09-21) -- see STAGE_CHECK_DEBUG's own
; comment for why this exists. Writes each source byte to SRAM, then
; IMMEDIATELY reads it straight back and compares -- much slower than a
; plain tin-only copy (an extra live bus read-back per byte, not just per
; block), but pinpoints the exact failing SRAM address, and its expected/
; found values, the instant a mismatch happens, rather than only a
; whole-block checksum difference after the fact.
;
; Uses `tin` itself for the actual write (2026-09-21, board owner's own
; direction, after a hand-rolled `lda (x)`/`sta (y)` version kept reporting
; mismatches where the expected and found bytes were IDENTICAL): tin is a
; single, purpose-built "external memory" transfer instruction ((Y)<-(X),
; then X++ and Y++ together) already proven reliable by the normal fast
; copy path above -- reusing it here for the write keeps that half of this
; loop byte-for-byte identical to the already-trusted path, isolating
; whatever's wrong to the read-back/compare half specifically, rather than
; also relying on a hand-rolled store whose own bus timing was never
; independently validated. tin's own increment leaves Y one past the byte
; it just wrote, so the read-back steps back one, reads, then restores Y
; to exactly where tin left it (net +1, matching the loop's own per-byte
; advance -- no separate `inc y` needed).
STAGE_COPY_VERIFY_COPY_LOOP:
	lda (x)
	sta (STAGE_DEBUG_SRC)        ; stash source byte for the compare below --
	                              ; a plain peek, doesn't move X; tin (next)
	                              ; re-reads the same (X) itself as part of
	                              ; its own transfer
	; Confirm the write above has actually landed before trusting it later
	; in this same iteration (2026-09-21) -- the SAME ordinary-write-vs-
	; dispatch DMA-relay race already found and fixed for the mismatch
	; report's own writes (see STAGE_COPY_VERIFY_MISMATCH below), but
	; unfixed here: STAGE_DEBUG_SRC lives in the same data window as
	; everything else the LH5801 writes, so a write here is subject to
	; the exact same relay latency. Without this, the later `cpa
	; (STAGE_DEBUG_SRC)` a few instructions down could read a STALE value
	; left over from an earlier iteration instead of the byte this
	; iteration just wrote -- explaining "expected" values that match
	; neither the true source byte nor anything obviously nearby (a fixed
	; few-iterations-stale offset would drift depending on how far behind
	; the relay actually is, not land on a clean, constant offset). A
	; still holds the exact value just written (sta doesn't touch it, and
	; tin below doesn't either), so this is a direct, unambiguous check --
	; not a guess at how long is "long enough."
STAGE_COPY_VERIFY_SRC_CONFIRM:
	cpa (STAGE_DEBUG_SRC)
	bzs STAGE_COPY_VERIFY_SRC_CONFIRMED
	jmp STAGE_COPY_VERIFY_SRC_CONFIRM
STAGE_COPY_VERIFY_SRC_CONFIRMED:
	tin                          ; (Y)<-(X), the SAME instruction the proven
	                              ; fast copy path already uses -- then X++, Y++
	dec y                        ; back to the byte tin just wrote
	lda (y)                      ; read it straight back (Y unchanged by a
	                              ; plain lda)
	inc y                        ; restore Y to exactly where tin left it
	cpa (STAGE_DEBUG_SRC)
	bzs STAGE_COPY_VERIFY_BYTE_OK
	jmp STAGE_COPY_VERIFY_MISMATCH
STAGE_COPY_VERIFY_BYTE_OK:
	dec u
	cpi uh,0x00
	bzr STAGE_COPY_VERIFY_COPY_LOOP
	cpi ul,0x00
	bzr STAGE_COPY_VERIFY_COPY_LOOP

STAGE_COPY_BLOCK_COPIED:
	; Per-block SRAM readback verification (2026-09-21) -- added after a
	; real STAGE RAM run staged all 6 blocks cleanly (MLOG VIEW showed
	; "STAGE blk 0 staged" through "STAGE blk 5 staged", no duplicates)
	; but then still hit a real, distinct "STAGE GET_BLOCK refused" --
	; the LH5801 genuinely re-entered this loop a 7th time. The
	; block-count arithmetic above is exact (6*1024=6144=ROM_REGION_END-
	; ROM_BASE, confirmed against the real .equ values, not assumed), so
	; a 7th real request can only mean Y didn't land where it should have
	; after the 6th block -- i.e. something corrupted the copy itself,
	; not the counting logic. This reads the block just written BACK from
	; SRAM (via the pointer saved above, not the payload window, which
	; the next GET_BLOCK is about to overwrite anyway) and checksums it
	; independently, proving the SRAM chip genuinely retained what tin
	; just wrote, byte for byte, rather than assuming it did.
	lda (STAGE_BLOCK_START_HI)
	sta xh
	lda (STAGE_BLOCK_START_LO)
	sta xl
	ldi uh,>EXP_MAX_TRANSFER_LEN
	ldi ul,<EXP_MAX_TRANSFER_LEN
	ldi a,0x00
	sta (STAGE_BLOCK_CKSUM_HI)
	sta (STAGE_BLOCK_CKSUM_LO)
STAGE_COPY_BLOCK_VERIFY_LOOP:
	lda (x)
	rec
	adc (STAGE_BLOCK_CKSUM_LO)
	sta (STAGE_BLOCK_CKSUM_LO)
	lda (STAGE_BLOCK_CKSUM_HI)
	adi a,0x00
	sta (STAGE_BLOCK_CKSUM_HI)
	inc x
	dec u
	cpi uh,0x00
	bzr STAGE_COPY_BLOCK_VERIFY_LOOP
	cpi ul,0x00
	bzr STAGE_COPY_BLOCK_VERIFY_LOOP

	; Compare against the MCU's own checksum of the same 1024 bytes,
	; computed when it staged this block (EXP_COMMAND_ROM_COPY_GET_BLOCK's
	; response, before tin ever ran) -- see EXP_BLOCK_CHECKSUM_ABS's own
	; comment in pc_exp.h.
	lda (STAGE_BLOCK_CKSUM_HI)
	cpa (EXP_BLOCK_CHECKSUM_ABS)
	bzr STAGE_COPY_BLOCK_FIND_DIFF
	lda (STAGE_BLOCK_CKSUM_LO)
	cpa (EXP_BLOCK_CHECKSUM_ABS+1)
	bzr STAGE_COPY_BLOCK_FIND_DIFF

	lda (STAGE_BLOCK_INDEX)
	sta (EXP_BUFFER_START_ABS)
	ldi a,0x01
	sta (EXP_BUFFER_START_ABS+1)
	bch STAGE_COPY_BLOCK_REPORT

; Pinpoints the first differing byte on a block-checksum mismatch
; (2026-09-22, board owner's own request) -- this failure shape (checksum
; differs, but STAGE DEBUG's own per-byte write-then-immediately-verify
; loop above reported zero mismatches) pointed at something changing
; AFTER the immediate per-byte check but BEFORE this later, separate
; re-read -- a genuine SRAM/GreenPAK data-retention question, not a
; write-time or read-time bus glitch. Reuses STAGE_COPY_VERIFY_MISMATCH
; (STAGE DEBUG's own per-byte mismatch handler, see its own header
; comment) directly rather than duplicating it -- same entry contract (A
; = found value, Y = failing SRAM address, STAGE_DEBUG_SRC = expected
; value already stored+confirmed).
;
; MUST run before STAGE_COPY_BLOCK_MISMATCH below ever touches
; EXP_BUFFER_START_ABS: this block's ORIGINAL source bytes are still
; sitting there, untouched, since the next GET_BLOCK (which would
; overwrite them) is never requested after a checksum failure -- but
; STAGE_COPY_BLOCK_MISMATCH's own report writes into that exact same
; window, so this comparison has to happen first, while the source is
; still intact.
;
; Y (not X) walks the SRAM side -- matching STAGE_COPY_VERIFY_MISMATCH's
; own expectation that Y already holds the failing address on entry, so
; no extra register shuffle is needed at the point of a mismatch. X walks
; the source/payload side instead.
STAGE_COPY_BLOCK_FIND_DIFF:
	lda (STAGE_BLOCK_START_HI)
	sta yh
	lda (STAGE_BLOCK_START_LO)
	sta yl
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi uh,>EXP_MAX_TRANSFER_LEN
	ldi ul,<EXP_MAX_TRANSFER_LEN
STAGE_COPY_BLOCK_DIFF_LOOP:
	lda (x)                       ; source/expected byte, from the still-
	sta (STAGE_DEBUG_SRC)         ; intact original payload window
STAGE_COPY_BLOCK_DIFF_SRC_CONFIRM:
	cpa (STAGE_DEBUG_SRC)         ; same write-confirm idiom as STAGE_COPY_
	bzs STAGE_COPY_BLOCK_DIFF_SRC_CONFIRMED  ; VERIFY_COPY_LOOP's own SRC
	jmp STAGE_COPY_BLOCK_DIFF_SRC_CONFIRM     ; stash -- see that routine's
	                                          ; own comment for why
STAGE_COPY_BLOCK_DIFF_SRC_CONFIRMED:
	lda (y)                       ; found/SRAM byte -- real bus access via
	                               ; Remap, not the relayed data window, so
	                               ; no confirm needed on this side
	cpa (STAGE_DEBUG_SRC)
	bzs STAGE_COPY_BLOCK_DIFF_OK
	jmp STAGE_COPY_VERIFY_MISMATCH ; A=found, Y=SRAM addr, STAGE_DEBUG_SRC=
	                                ; expected -- exact entry contract match
STAGE_COPY_BLOCK_DIFF_OK:
	inc x
	inc y
	dec u
	cpi uh,0x00
	bzr STAGE_COPY_BLOCK_DIFF_LOOP
	cpi ul,0x00
	bzr STAGE_COPY_BLOCK_DIFF_LOOP
	; Fell through the whole block with no per-byte difference found,
	; despite the checksum mismatch that got us here -- shouldn't happen
	; (same bytes, same order, deterministic checksum), but if it does,
	; fall back to the generic block-checksum report below rather than
	; claim a specific byte that was never actually found.

STAGE_COPY_BLOCK_MISMATCH:
	; DEBUG mode (2026-09-23, board owner's own request): never touch
	; EXP_BUFFER_START_ABS on a failure. Already known to be a failure
	; just by being in this handler, so DEBUG mode skips the whole
	; write/dispatch/report-readback dance below and jumps straight to the
	; shared failure tail. RAM mode still reports via LOG_BLOCK_CHECKSUM
	; exactly as before. (bzs-then-jmp split -- STAGE_COPY_MIDFAIL is out
	; of short-branch range from here, same reason as the other splits in
	; this file.)
	lda (STAGE_DEBUG_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_BLOCK_MISMATCH_REPORT
	jmp STAGE_COPY_MIDFAIL
STAGE_COPY_BLOCK_MISMATCH_REPORT:
	lda (STAGE_BLOCK_INDEX)
	sta (EXP_BUFFER_START_ABS)
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+1)
	; found checksum (what was actually read back from SRAM) -- the MCU
	; already has the expected one from its own GET_BLOCK response, still
	; sitting at EXP_BLOCK_CHECKSUM_ABS, so only this one needs sending
	lda (STAGE_BLOCK_CKSUM_HI)
	sta (EXP_BUFFER_START_ABS+2)
	lda (STAGE_BLOCK_CKSUM_LO)
	sta (EXP_BUFFER_START_ABS+3)

STAGE_COPY_BLOCK_REPORT:
	ldi a,EXP_COMMAND_LOG_BLOCK_CHECKSUM
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_BLOCK_REPORT_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_BLOCK_REPORT_POLL
	                              ; status (SUCCESS/NOT_IMPLEMENTED/etc.)
	                              ; deliberately not checked here -- this
	                              ; report is diagnostic only and must
	                              ; never itself gate STAGE's own outcome

	lda (EXP_BUFFER_START_ABS+1)  ; the match flag this routine itself
	cpi a,0x00                    ; wrote above -- LOG_BLOCK_CHECKSUM's
	bzr STAGE_COPY_BLOCK_VERIFY_OK ; own case doesn't touch this byte, so
	jmp STAGE_COPY_MIDFAIL        ; it's still ours to read back; a real
	                              ; mismatch is a genuine data-integrity
	                              ; failure -- abort and revert exactly
	                              ; like a GET_BLOCK failure, now
	                              ; pinpointing exactly which block
	                              ; (bzr-then-jmp split, same reason as
	                              ; STAGE_COPY_BEGIN_OK above)
STAGE_COPY_BLOCK_VERIFY_OK:
	lda (STAGE_BLOCK_INDEX)
	inc a
	sta (STAGE_BLOCK_INDEX)

	; Loop-exit check, restructured the same way and for the same reason
	; as the bzs/jmp splits above -- STAGE_COPY_BLOCK_LOOP is now far
	; enough back (past all the per-block verify code above) that a plain
	; bzr can't reach it either, so the "not done yet" case takes an
	; unconditional jmp instead, gated by a short bzs past it.
	lda yh                        ; one block done -- Y now points just
	cpi a,>ROM_REGION_END         ; past it; all 6K copied once Y reaches
	bzs STAGE_COPY_CHECK_YL        ; high byte matches -- check low byte too
	jmp STAGE_COPY_BLOCK_LOOP      ; not done yet
STAGE_COPY_CHECK_YL:
	lda yl
	cpi a,<ROM_REGION_END
	bzs STAGE_COPY_ALL_BLOCKS_DONE ; both match -- all 6 blocks landed
	jmp STAGE_COPY_BLOCK_LOOP      ; not done yet
STAGE_COPY_ALL_BLOCKS_DONE:

	; All 6 blocks landed -- 16-bit additive checksum over the
	; just-written SRAM region (plain sum, natural wraparound, no
	; multiply -- LH5801 has none; same carry-propagation idiom already
	; used elsewhere in this file, e.g. SD_PARSE_NUMBER's own decimal
	; accumulation). Separate pass from the tin-based copy above since
	; tin doesn't touch ACC.
	ldi xh,>ROM_BASE
	ldi xl,<ROM_BASE
	ldi uh,>STAGE_CHECKSUM_LEN
	ldi ul,<STAGE_CHECKSUM_LEN
	ldi a,0x00
	sta (STAGE_CHECKSUM_HI)
	sta (STAGE_CHECKSUM_LO)
STAGE_COPY_SUM_LOOP:
	lda (x)
	rec
	adc (STAGE_CHECKSUM_LO)
	sta (STAGE_CHECKSUM_LO)
	lda (STAGE_CHECKSUM_HI)
	adi a,0x00                    ; propagate carry into the high byte,
	sta (STAGE_CHECKSUM_HI)        ; no other change
	inc x
	dec u
	cpi uh,0x00
	bzr STAGE_COPY_SUM_LOOP
	cpi ul,0x00
	bzr STAGE_COPY_SUM_LOOP

	lda (STAGE_CHECKSUM_HI)
	sta (EXP_BUFFER_START_ABS)
	lda (STAGE_CHECKSUM_LO)
	sta (EXP_BUFFER_START_ABS+1)
	ldi a,EXP_COMMAND_ROM_COPY_FINISH
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_FINISH_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_FINISH_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzr STAGE_COPY_BADCHECKSUM    ; FINISH reported failure (bad checksum
	                              ; or WE-clear-readback failed) --
	                              ; revert-and-report, same as a mid-copy
	                              ; GET_BLOCK failure

	sie                             ; re-enable interrupts before returning
	                                ; to BASIC -- see STAGE_COPY_BEGIN_OK's
	                                ; own comment for why they were off
	ldi uh,>SD_LIST_BLANK           ; blank the line first -- DISP_N_CHARS0
	ldi ul,<SD_LIST_BLANK           ; doesn't clear past what it draws, so the
	ldi xl,SD_LIST_LINE_WIDTH       ; short OK message alone left "UG" from
	sjp DISP_N_CHARS0               ; "STAGE DEBUG" behind. (Blanking here, not
	                                ; padding STAGE_OK_MSG, because this pocket
	                                ; is nearly full -- padding pushed the
	                                ; STAGE_STAGE0_* diagnostics onto
	                                ; EXP_LENGTH_PORT at 0x87FD. SD_LIST_BLANK
	                                ; sits at ROM_BASE+, safe to read now for
	                                ; the same reason as the draw below.)
	ldi uh,>STAGE_OK_MSG
	ldi ul,<STAGE_OK_MSG
	ldi xl,STAGE_OK_MSG_LEN
	sjp DISP_N_CHARS0              ; safe now -- FINISH succeeded, so
	                                ; ROM_BASE+ mirrors buffer[8..31]
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

STAGE_COPY_BEGIN_FAILED:
	jmp SD_RAISE_ERROR_1           ; safe direct -- Remap never switched

; STAGE DEBUG's own per-byte mismatch handler -- reports the exact SRAM
; address plus expected/found values via EXP_COMMAND_STAGE_BYTE_MISMATCH
; (see pc_exp.h's own comment), then falls into the SAME shared
; display+revert tail every other failure path below already uses. A
; is the readback (found) value at entry -- cpa doesn't modify it, so it's
; still valid straight through the bzs/jmp that got here.
STAGE_COPY_VERIFY_MISMATCH:
	sta (STAGE_DEBUG_FOUND)
	; Confirm this write landed too (2026-09-21) -- the one write-confirm
	; gap left after closing the same one for STAGE_DEBUG_SRC and for the
	; wire-transfer bytes below: this is the VERY FIRST write this handler
	; does, and STAGE_DEBUG_FOUND is only ever written here (once per
	; mismatch, not every loop iteration), so a stale read of it a few
	; instructions down (line "lda (STAGE_DEBUG_FOUND)" below) could
	; silently return whatever was left over from an EARLIER mismatch in
	; this same run -- or, on the very first mismatch, its untouched
	; initial value -- instead of the byte that actually just failed. A
	; still holds the exact value being stashed (sta doesn't touch it).
STAGE_COPY_VERIFY_FOUND_CONFIRM:
	cpa (STAGE_DEBUG_FOUND)
	bzs STAGE_COPY_VERIFY_FOUND_CONFIRMED
	jmp STAGE_COPY_VERIFY_FOUND_CONFIRM
STAGE_COPY_VERIFY_FOUND_CONFIRMED:
	; DEBUG mode (2026-09-23, board owner's own request): never touch
	; EXP_BUFFER_START_ABS on a failure -- that's the exact payload the
	; board owner needs to PEEK unmolested to debug STAGE itself. The
	; on-screen message below is already built entirely from ROM-side
	; memory (Y/STAGE_DEBUG_SRC/STAGE_DEBUG_FOUND, see STAGE_BUILD_
	; MISMATCH_MSG), so DEBUG mode skips straight there, never writing to
	; or reading from the buffer and never dispatching
	; EXP_COMMAND_STAGE_BYTE_MISMATCH at all. RAM mode (which can only
	; reach this handler via STAGE_COPY_BLOCK_FIND_DIFF, never the live
	; per-byte loop below, which is DEBUG-only) keeps reporting to the MCU
	; as before.
	lda (STAGE_DEBUG_FLAG)
	cpi a,0x00
	bzr STAGE_COPY_VERIFY_MISMATCH_BUILD_MSG

	lda yh
	sta (EXP_BUFFER_START_ABS)
	lda yl
	sta (EXP_BUFFER_START_ABS+1)
	lda (STAGE_DEBUG_SRC)
	sta (EXP_BUFFER_START_ABS+2)
	lda (STAGE_DEBUG_FOUND)
	sta (EXP_BUFFER_START_ABS+3)
	; Confirm the last ordinary write above has actually landed in the
	; MCU's buffer before dispatching (2026-09-21, board owner's own
	; diagnosis: "I think your expected/found in STAGE DEBUG is correct
	; about a mismatch, but reporting it incorrectly"). Ordinary writes
	; and the dispatch trigger travel through SEPARATE PIO/DMA paths
	; (write_serve.pio's own design deliberately bypasses the ordinary-
	; write DMA relay for dispatch writes specifically, so they can be
	; handled immediately, without buffer[] ever seeing a raw command
	; byte) -- a dispatch fired before the four writes above finish
	; landing would let the MCU read stale/leftover buffer content as
	; "expected"/"found" instead of what was just written here, exactly
	; matching what was seen live: expected and found reported as
	; IDENTICAL even though the underlying cpa comparison a few
	; instructions up (entirely internal to the LH5801, unaffected by any
	; of this) correctly found a genuine difference. Confirming only the
	; LAST write (this one) is sufficient -- ordinary writes travel
	; through one single relay channel in order, so if this one has
	; landed, the three before it (Y-hi, Y-lo, expected) already have too.
STAGE_COPY_VERIFY_MISMATCH_CONFIRM:
	lda (EXP_BUFFER_START_ABS+3)
	cpa (STAGE_DEBUG_FOUND)
	bzs STAGE_COPY_VERIFY_MISMATCH_CONFIRMED
	jmp STAGE_COPY_VERIFY_MISMATCH_CONFIRM
STAGE_COPY_VERIFY_MISMATCH_CONFIRMED:
	ldi a,EXP_COMMAND_STAGE_BYTE_MISMATCH
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_VERIFY_MISMATCH_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_VERIFY_MISMATCH_POLL
STAGE_COPY_VERIFY_MISMATCH_BUILD_MSG:
	; Static message (2026-09-21) -- was a live read-back of the MCU's own
	; dynamically-formatted "STAGE @addr E:xx F:yy" response, replaced
	; after confirming a real reliability gap: even with the write-confirm
	; above making the ROM->MCU direction of this exact transaction
	; trustworthy, real-hardware testing showed the MCU->ROM direction
	; (reading the RESPONSE straight back for immediate display) still
	; wasn't -- garbled/truncated on-screen output was observed even
	; though the SAME data, logged via mcu_log_error inside the SAME MCU
	; command handler, was later confirmed CORRECT via MLOG VIEW. A
	; single fixed byte (the write-confirm case) is simple to make
	; provably reliable with a retry loop; a variable-length, multi-byte
	; response is not, without much more machinery. Sidestepping the
	; whole problem entirely for the wire transaction: the MLOG entry above
	; (via mcu_log_error, in the MCU's own EXP_COMMAND_STAGE_BYTE_MISMATCH
	; handler) is kept as a durable backup record, but the ON-SCREEN
	; message is no longer a live read-back of anything the MCU sends --
	; see STAGE_BUILD_MISMATCH_MSG below.
	;
	; Round 2 (2026-09-21, board owner's own request): checking MLOG VIEW
	; after every single failure was inconvenient, and by this point the
	; ROM already has all three values (Y, STAGE_DEBUG_SRC, STAGE_DEBUG_
	; FOUND) sitting reliably in its own memory -- confirmed via the
	; write-confirm loops above, no MCU round trip involved at all. There
	; is no reason to ask the MCU to format and send back a response just
	; to redisplay data the ROM already has firsthand -- so it builds the
	; message itself instead.
	sjp STAGE_BUILD_MISMATCH_MSG
	ldi uh,>STAGE_BYTE_MISMATCH_TEMPLATE
	ldi ul,<STAGE_BYTE_MISMATCH_TEMPLATE
	ldi xl,STAGE_BYTE_MISMATCH_TEMPLATE_LEN
	jmp STAGE_COPY_REVERT          ; reuse the shared display+revert tail

; Fills in the 4 hex-digit fields of STAGE_BYTE_MISMATCH_TEMPLATE from Y
; (the failing SRAM address), STAGE_DEBUG_SRC (expected), and
; STAGE_DEBUG_FOUND (found) -- see STAGE_COPY_VERIFY_MISMATCH's own comment
; above for why this replaced an MCU round trip. Template layout
; ("STAGE @0000 E:00 F:00"), byte offsets confirmed against its own
; .ascii text:
;   0-6   "STAGE @" (static)
;   7-10  address, 4 hex digits    <- Y
;   11-13 " E:" (static)
;   14-15 expected, 2 hex digits   <- STAGE_DEBUG_SRC
;   16-18 " F:" (static)
;   19-20 found, 2 hex digits      <- STAGE_DEBUG_FOUND
STAGE_BUILD_MISMATCH_MSG:
	ldi xh,>(STAGE_BYTE_MISMATCH_TEMPLATE+7)
	ldi xl,<(STAGE_BYTE_MISMATCH_TEMPLATE+7)
	lda yh
	sjp HEX_BYTE_TO_ASCII          ; writes offset 7-8, leaves X at 9
	lda yl
	sjp HEX_BYTE_TO_ASCII          ; writes offset 9-10

	ldi xh,>(STAGE_BYTE_MISMATCH_TEMPLATE+14)
	ldi xl,<(STAGE_BYTE_MISMATCH_TEMPLATE+14)
	lda (STAGE_DEBUG_SRC)
	sjp HEX_BYTE_TO_ASCII          ; writes offset 14-15

	ldi xh,>(STAGE_BYTE_MISMATCH_TEMPLATE+19)
	ldi xl,<(STAGE_BYTE_MISMATCH_TEMPLATE+19)
	lda (STAGE_DEBUG_FOUND)
	sjp HEX_BYTE_TO_ASCII          ; writes offset 19-20
	rtn

; Converts the byte in A to 2 ASCII hex digits, written to (X), leaving X
; pointing just past them. Clobbers A and STAGE_HEX_SCRATCH.
HEX_BYTE_TO_ASCII:
	sta (STAGE_HEX_SCRATCH)        ; stash the full byte -- sta doesn't
	                                ; touch A, so it's still here for the
	                                ; high-nibble extraction below
HEX_BYTE_SCRATCH_CONFIRM:
	cpa (STAGE_HEX_SCRATCH)         ; STAGE_HEX_SCRATCH lives in the same
	bzs HEX_BYTE_SCRATCH_CONFIRMED  ; MCU-relayed 2K data window as every
	jmp HEX_BYTE_SCRATCH_CONFIRM    ; other scratch byte this file confirms
	                                 ; -- without this, the lda below (for
	                                 ; the low-nibble extraction) can read
	                                 ; back stale data, corrupting whichever
	                                 ; digit loses the race. Shared by both
	                                 ; STAGE_BUILD_MISMATCH_MSG and
	                                 ; ROMRAMTEST_BUILD_FAIL_MSG, called 4x
	                                 ; per report -- found live: this alone
	                                 ; explained both a garbled address
	                                 ; digit ("928G") and bogus E/F digits
	                                 ; ("BB") that weren't fixed by
	                                 ; confirming the caller's own bytes.
HEX_BYTE_SCRATCH_CONFIRMED:
	shr
	shr
	shr
	shr                             ; A = high nibble (0-15), confirmed SHR
	                                ; semantics (logical, 0-filled, matching
	                                ; the emulator's own faithful LH5801
	                                ; implementation): 4x halves A four
	                                ; times, isolating bits 7-4 into 3-0
	sjp HEX_NIBBLE_TO_ASCII
	sta (x)
	inc x
	lda (STAGE_HEX_SCRATCH)
	ani a,0x0F                      ; A = low nibble -- same mask SD_PARSE_
	                                ; HEX_DIGIT already uses for the reverse
	                                ; conversion
	sjp HEX_NIBBLE_TO_ASCII
	sta (x)
	inc x
	rtn

; Converts the nibble in A (0-15, bits 7-4 must be 0) to its ASCII hex
; digit ('0'-'9','A'-'F'), in A. Same cpi/bcr "less than" convention
; SD_PARSE_HEX_DIGIT's own reverse conversion already established
; (bcr branches when A < the immediate) -- confirmed via that routine's
; own "cpi a,0x30 ('0'); bcr ..." checks, not guessed.
HEX_NIBBLE_TO_ASCII:
	cpi a,0x0A
	bcr HEX_NIBBLE_LOW              ; A < 10 -- '0'-'9'
	adi a,0x37                      ; 10-15 -> 'A'-'F'
	rtn
HEX_NIBBLE_LOW:
	adi a,0x30                      ; 0-9 -> '0'-'9'
	rtn

; Shared failure-report path for a mid-copy GET_BLOCK failure or a
; FINISH-reported bad checksum. Branches on STAGE_DEBUG_FLAG afterward
; (2026-09-23, board owner's own request: "STAGE DEBUG leaves state as is
; when it fails, and STAGE RAM will revert remap and write enable when it
; fails" -- fewer commands to remember than a separate always-leaves-it-set
; diagnostic, and debugging happens on the same failure-reporting code path
; STAGE RAM itself uses, just without the revert) -- RAM mode falls into
; STAGE_COPY_DO_REVERT below (unchanged from before), DEBUG mode jumps to
; STAGE_DEBUG_FAIL_RETURN instead, which leaves Remap/WE exactly as they
; are. See STAGE_DEBUG_FAIL_RETURN's own comment for why that can't just
; reuse KEYWORD_RETURN.
STAGE_COPY_MIDFAIL:
	ldi uh,>STAGE_MIDFAIL_MSG
	ldi ul,<STAGE_MIDFAIL_MSG
	ldi xl,STAGE_MIDFAIL_MSG_LEN
	bch STAGE_COPY_REVERT
STAGE_COPY_BADCHECKSUM:
	ldi uh,>STAGE_CHECKSUM_MSG
	ldi ul,<STAGE_CHECKSUM_MSG
	ldi xl,STAGE_CHECKSUM_MSG_LEN
STAGE_COPY_REVERT:
	sie                              ; re-enable interrupts before returning
	                                 ; to BASIC either way (revert, DEBUG-
	                                 ; mode no-revert return, or the
	                                 ; unrecoverable REVERT_FAILED case
	                                 ; below) -- see STAGE_COPY_BEGIN_OK's
	                                 ; own comment for why they were off
	sjp DISP_N_CHARS0               ; safe -- system ROM, unaffected by Remap
	lda (STAGE_DEBUG_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_DO_REVERT         ; RAM mode -- revert Remap/WE, below
	jmp STAGE_DEBUG_FAIL_RETURN      ; DEBUG mode -- leave state as is

STAGE_COPY_DO_REVERT:
	ldi a,EXP_COMMAND_ROM_FROM_MCU
	sta (EXP_INSTRUCTION_ABS)
STAGE_COPY_REVERT_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_COPY_REVERT_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzr STAGE_COPY_REVERT_FAILED     ; the one truly unrecoverable case --
	                                 ; no safe address left to return to
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN               ; safe now -- Remap reverted,
	                                  ; ROM_BASE+ served by buffer[8..31]

STAGE_COPY_REVERT_FAILED:
	ldi uh,>STAGE_REVERT_FAIL_MSG
	ldi ul,<STAGE_REVERT_FAIL_MSG
	ldi xl,STAGE_REVERT_FAIL_MSG_LEN
	sjp DISP_N_CHARS0
STAGE_COPY_HALT:
	bch STAGE_COPY_HALT

; DEBUG-mode failure return -- leaves Remap/WE exactly as they were: 0x8800+
; still mirrors whatever's actually in the external SRAM chip right now (a
; partial/failed copy), for the board owner's own PEEK inspection.
;
; Deliberately does NOT jump through the shared KEYWORD_RETURN (0x8A29) or
; draw its own ">" prompt via KEYWORD_RETURN_PROMPT (0x8A0F) -- both live in
; ROM_BASE+, which Remap is still actively redirecting to the SRAM chip at
; this exact point, so fetching either from there would execute/display
; whatever's actually in SRAM instead of the real base-ROM bytes -- unsafe
; by the same "nothing may execute from ROM_BASE+ once Remap has switched"
; reasoning used throughout this file. This is a local copy of
; KEYWORD_RETURN's own state-fixup sequence (see that label's own header
; comment for the full why of each fixup), entirely within this module's
; own safe 0x8000-0x87FF data window, skipping only the cosmetic ">"
; redraw -- a stale display line until the next keypress is a fine
; tradeoff for a debug-only path whose whole point is PEEK-based
; inspection, not evidence anything else is broken.
STAGE_DEBUG_FAIL_RETURN:
	pop a
	ldi a,0xCA
	sta (0x784E)
	ldi a,0x92
	sta (0x784F)
	ani (0x764E),0xFE
	ani (0x7874),0xFE
	ldi a,0x00
	sta (0x7880)
	ldi xh,0xE2
	ldi xl,0xAA
	stx p

STAGE_CHECKSUM_HI: .db 0x00
STAGE_CHECKSUM_LO: .db 0x00
STAGE_BLOCK_INDEX: .db 0x00
STAGE_BLOCK_START_HI: .db 0x00
STAGE_BLOCK_START_LO: .db 0x00
STAGE_BLOCK_CKSUM_HI: .db 0x00
STAGE_BLOCK_CKSUM_LO: .db 0x00
STAGE_DEBUG_FLAG: .db 0x00    ; 0=normal (fast tin copy), 1=STAGE DEBUG
                               ; (slow per-byte write+verify copy)
STAGE_DEBUG_SRC: .db 0x00     ; per-byte verify scratch -- stashed source
                               ; byte, compared against SRAM's own readback
STAGE_DEBUG_FOUND: .db 0x00   ; per-byte verify scratch -- the mismatching
                               ; readback value, stashed for the mismatch report
STAGE_PRIME_READ: .db 0x00    ; STAGE_STAGE0_PRIME_AND_COPY's own scratch --
                               ; what the priming read of EXP_BUFFER_START_ABS
                               ; actually saw, right before the tin loop
STAGE_PRIME_READ2: .db 0x00   ; second successive read, same address
STAGE_PRIME_READ3: .db 0x00   ; third successive read, same address
STAGE_PRIME_DUMP: .db 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
                               ; 16 bytes -- STAGE_STAGE0_SEQUENTIAL_PRIME's
                               ; own scratch: EXP_BUFFER_START_ABS+0..15,
                               ; each read and stashed separately, in order
STAGE_OK_MSG: .ascii "STAGE: OK"
STAGE_OK_MSG_LEN .equ 9
STAGE_MIDFAIL_MSG: .ascii "STAGE: COPY FAILED"
STAGE_MIDFAIL_MSG_LEN .equ 18
STAGE_CHECKSUM_MSG: .ascii "STAGE: BAD CHECKSUM"
STAGE_CHECKSUM_MSG_LEN .equ 19
; Mutable -- STAGE_BUILD_MISMATCH_MSG overwrites the 4 hex-digit fields
; (offsets 7-10, 14-15, 19-20) in place before every display; the '0'
; placeholders here only matter as filler bytes of the right count/
; position, never shown as-is (a real mismatch always fills them in first).
STAGE_BYTE_MISMATCH_TEMPLATE: .ascii "STAGE @0000 E:00 F:00"
STAGE_BYTE_MISMATCH_TEMPLATE_LEN .equ 21
STAGE_HEX_SCRATCH: .db 0x00
STAGE_REVERT_FAIL_MSG: .ascii "STAGE: REVERT FAILED - REBOOT"
STAGE_REVERT_FAIL_MSG_LEN .equ 29
STAGE_COPY_END:
STAGE_COPY_SIZE .equ (STAGE_COPY_END - STAGE_COPY_START)
; Budget: 0x8500-0x87FC = 765 bytes free; check this constant's real
; value in rom.lst/rom.map once assembled to confirm it fits.
; (ROMRAMTEST used to live here too but didn't fit -- moved to its own
; .org 0x8000 block; removed entirely 2026-09-22, see that block's own
; header comment.)

; ---------------------------------------------------------------------
; ROM_RESET_REMAP -- standalone recovery entry point (2026-09-22, board
; owner's own request). CALL this address directly, independent of any
; STAGE/RAMTEST2 success or failure path, to force GreenPAK1/2's SRAM/ROM
; Remap back to ROM_FROM_MCU after a failed or hung STAGE/RAMTEST2 leaves
; ROM_BASE+ stuck answering with raw SRAM content instead of the real
; keyword table (or leaves write-enable set) -- e.g. after a hang bad
; enough to need a manual reboot, which skips every normal revert path.
;
; Deliberately placed here, in STAGE_COPY_ROUTINE_ABS's own collision-proof
; pocket (0x8500-0x87FC, 765 bytes budgeted, STAGE_COPY_SIZE bytes actually
; used -- see that constant, comfortably under budget), NOT the general
; scratch/payload area RAMTEST2 uses (0x8000-0x84FF) -- recovering from a
; hang that may have left that whole area in an unknown state is the exact
; scenario this exists for, so it can't depend on that area being intact.
;
; Touches nothing at ROM_BASE+ (no DISP_N_CHARS0/KEYSCAN_WAIT, unlike
; STAGE_COPY_ROUTINE_ABS's own revert path) -- safe to CALL regardless of
; current Remap state, matching the same safety rule STAGE_COPY_ROUTINE_ABS
; documents for itself above. No status check or display after the revert
; completes -- if EXP_COMMAND_ROM_FROM_MCU itself doesn't finish, there's no
; safer address left to report failure from anyway (same reasoning as
; STAGE_COPY_ROUTINE_ABS's own "truly no safe address left" case); a plain
; RTN is the correct, complete return for a CALLed routine either way.
ROM_RESET_REMAP:
	ldi a,EXP_COMMAND_ROM_FROM_MCU
	sta (EXP_INSTRUCTION_ABS)
ROM_RESET_REMAP_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs ROM_RESET_REMAP_POLL
	rtn

; STAGE_STAGE0_ONLY (2026-09-23, board owner's own request) -- does ONLY
; BEGIN + GET_BLOCK for block 0, then stops immediately: no tin copy to
; ROM_BASE, no revert, no display. Isolates whether the payload window's
; own staleness (see the board owner's real-hardware first-~15-bytes
; finding) is already present at EXP_BUFFER_START_ABS right after
; GET_BLOCK itself -- before any tin read ever happens -- or only shows up
; once STAGE's own tin loop actually reads it. CALLed directly (not
; table-dispatched), so a plain rtn is the correct, complete return, same
; reasoning as ROM_RESET_REMAP above. Same collision-proof pocket, same
; hand-rolled (not EC_WAIT_NOT_BUSY) polling for the same Remap-safety
; reason documented at STAGE_COPY_ROUTINE_ABS's own header. Leaves Remap
; engaged and the payload window exactly as GET_BLOCK left it -- PEEK
; 0x8000+ by hand afterward. ROM_RESET_REMAP (above) or STAGE MCU releases
; Remap when done inspecting.
STAGE_STAGE0_ONLY:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_ONLY_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_ONLY_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_ONLY_BEGIN_OK
	rtn                              ; BEGIN failed -- Remap never
	                                 ; switched, nothing to inspect, safe
	                                 ; direct return
STAGE_STAGE0_ONLY_BEGIN_OK:
	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_ONLY_GET_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_ONLY_GET_BLOCK_POLL
	rtn                              ; stop here either way -- the whole
	                                 ; point is to PEEK EXP_BUFFER_START_ABS
	                                 ; by hand right now, before anything
	                                 ; else touches it

; STAGE_STAGE0_AND_COPY (2026-09-23, board owner's own request) -- extends
; STAGE_STAGE0_ONLY above with a plain tin copy of block 0 to ROM_BASE,
; then stops -- no per-byte verify, no checksum, no revert. Isolates
; whether the destination-side staleness the board owner found (0x8800
; ending up with the PRE-GET_BLOCK value) reproduces from `tin` alone,
; run immediately after GET_BLOCK with no intervening delay, or needs
; something specific to STAGE DEBUG's own per-byte verify loop (more
; instructions/reads between GET_BLOCK completing and the actual copy)
; to show up. Same collision-proof pocket, same hand-rolled polling and
; CALL-return reasoning as STAGE_STAGE0_ONLY/ROM_RESET_REMAP above.
STAGE_STAGE0_AND_COPY:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_AND_COPY_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_AND_COPY_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_AND_COPY_BEGIN_OK
	rtn                              ; BEGIN failed -- Remap never
	                                 ; switched, safe direct return
STAGE_STAGE0_AND_COPY_BEGIN_OK:
	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_AND_COPY_GET_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_AND_COPY_GET_BLOCK_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_AND_COPY_GET_BLOCK_OK
	rtn                              ; GET_BLOCK failed -- Remap IS
	                                 ; switched by now, but nothing here
	                                 ; executes from ROM_BASE+, still safe
	                                 ; to return without reverting
STAGE_STAGE0_AND_COPY_GET_BLOCK_OK:
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>ROM_BASE
	ldi yl,<ROM_BASE
	ldi uh,>EXP_MAX_TRANSFER_LEN
	ldi ul,<EXP_MAX_TRANSFER_LEN
STAGE_STAGE0_AND_COPY_LOOP:
	tin                              ; plain fast copy -- no per-byte
	                                 ; verify, matching STAGE_COPY_COPY_
	                                 ; LOOP's own non-DEBUG path exactly
	dec u
	cpi uh,0x00
	bzr STAGE_STAGE0_AND_COPY_LOOP
	cpi ul,0x00
	bzr STAGE_STAGE0_AND_COPY_LOOP
	rtn                              ; stop here -- PEEK 0x8800+ by hand;
	                                 ; Remap still engaged, revert via
	                                 ; STAGE MCU/ROM_RESET_REMAP when done

; STAGE_STAGE0_PRIME_AND_COPY (2026-09-23, board owner's own request) --
; identical to STAGE_STAGE0_AND_COPY above, except for ONE extra
; throwaway read of EXP_BUFFER_START_ABS (result discarded) inserted
; right before the tin loop starts. A direct, falsifiable test: if this
; one dummy read changes whether the first byte comes out stale, that's
; real evidence of a read-path priming effect; if it changes nothing,
; that's ruled out too.
STAGE_STAGE0_PRIME_AND_COPY:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_PRIME_AND_COPY_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_PRIME_AND_COPY_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_PRIME_AND_COPY_BEGIN_OK
	rtn
STAGE_STAGE0_PRIME_AND_COPY_BEGIN_OK:
	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_PRIME_AND_COPY_GET_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_PRIME_AND_COPY_GET_BLOCK_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_PRIME_AND_COPY_GET_BLOCK_OK
	rtn
STAGE_STAGE0_PRIME_AND_COPY_GET_BLOCK_OK:
	lda (EXP_BUFFER_START_ABS)       ; three successive reads of the SAME
	sta (STAGE_PRIME_READ)           ; address, each stashed separately --
	lda (EXP_BUFFER_START_ABS)       ; how many reads deep is the stale
	sta (STAGE_PRIME_READ2)          ; window? PEEK all three afterward.
	lda (EXP_BUFFER_START_ABS)
	sta (STAGE_PRIME_READ3)
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>ROM_BASE
	ldi yl,<ROM_BASE
	ldi uh,>EXP_MAX_TRANSFER_LEN
	ldi ul,<EXP_MAX_TRANSFER_LEN
STAGE_STAGE0_PRIME_AND_COPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr STAGE_STAGE0_PRIME_AND_COPY_LOOP
	cpi ul,0x00
	bzr STAGE_STAGE0_PRIME_AND_COPY_LOOP
	rtn                              ; stop here -- PEEK 0x8800+ by hand

; STAGE_STAGE0_SEQUENTIAL_PRIME (2026-09-23, board owner's own request) --
; reads EXP_BUFFER_START_ABS+0 through +15 SEQUENTIALLY (no tin, no copy),
; stashing each byte separately into STAGE_PRIME_DUMP so every one can be
; PEEKed afterward. Tests the board owner's own hypothesis directly: does
; the stale window clear after ~15 DIFFERENT addresses are read (not just
; time passing, already ruled out by STAGE_STAGE0_PRIME_AND_COPY's three
; same-address reads all coming back stale)?
STAGE_STAGE0_SEQUENTIAL_PRIME:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_SEQ_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_SEQ_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_SEQ_BEGIN_OK
	rtn
STAGE_STAGE0_SEQ_BEGIN_OK:
	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
STAGE_STAGE0_SEQ_GET_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_STAGE0_SEQ_GET_BLOCK_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzs STAGE_STAGE0_SEQ_GET_BLOCK_OK
	rtn
STAGE_STAGE0_SEQ_GET_BLOCK_OK:
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>STAGE_PRIME_DUMP
	ldi yl,<STAGE_PRIME_DUMP
	ldi ul,16
STAGE_STAGE0_SEQ_LOOP:
	lda (x)
	sta (y)
	inc x
	inc y
	dec ul
	bzr STAGE_STAGE0_SEQ_LOOP
	rtn                              ; stop here -- PEEK STAGE_PRIME_DUMP+0
	                                 ; through +15 by hand

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
                        ; 22 bytes budgeted here -- an off-by-one in an
                        ; earlier version of this comment caused a real
                        ; regression: shrinking this entry point's total
                        ; footprint by even 1 byte shifts KEYWORD_INDEX and
                        ; everything after it 1 byte off the fixed 32-byte
                        ; header boundary the base ROM's own dispatch
                        ; mechanism expects, corrupting every keyword's
                        ; lookup in a different way -- confirmed by bisecting
                        ; a ~88-test regression down to exactly this.
                        ;
                        ; The `jmp ROM_COPY_TRAMPOLINE` that briefly lived
                        ; here (2026-08-28 - 2026-09) is disabled again
                        ; (STAGE keyword plan, 2026-09): that trampoline's
                        ; own MCU-side commands (ROM_COPY_BEGIN/GET_BLOCK/
                        ; FINISH) had never been implemented on ANY board
                        ; until the STAGE keyword implemented them for real
                        ; -- meaning this boot-time call had silently done
                        ; nothing, ever, on real hardware, right up until
                        ; that implementation landed. Leaving the call
                        ; enabled at that point would have made this
                        ; dormant call start actually running a full,
                        ; unplanned ROM copy on every single boot, using
                        ; the old runtime-relocated mechanism (no checksum)
                        ; instead of STAGE's own permanently-placed,
                        ; checksummed one. Automatic self-test/init
                        ; invocation is a real, explicitly deferred future
                        ; goal -- it should eventually call STAGE's own
                        ; routine, not this old one, once that's decided.
                        ; Restored to the original bare `rtn` + `.blkb 21`
                        ; (1+21=22) layout in the meantime.
	rtn
	.blkb 21

; ---------------------------------------------------------------------
; First-letter index (26 x 2-byte BE pointers, A-Z). D/E/L/S are used.
KEYWORD_INDEX:
	.dw 0x0000  ; A
	.dw 0x0000  ; B
	.dw 0x0000  ; C
	.dw DOSTUFF_TABLE_ENTRY+2  ; D -- 2nd character of DOSTUFF, its own sole entry
	.dw ECVER_TABLE_ENTRY+2  ; E -- 2nd character of ECVER, its own sole entry
	.dw 0x0000  ; F
	.dw 0x0000  ; G
	.dw 0x0000  ; H
	.dw 0x0000  ; I
	.dw 0x0000  ; J
	.dw 0x0000  ; K
	.dw 0x0000  ; L
	.dw MLOG_TABLE_ENTRY+2  ; M -- 2nd character of MLOG, its own sole entry
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
	.dw 0xE18A
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
	.dw 0xE189
	.dw SDFMT_ROUTINE

	.db 0xC6
	.ascii "SDLOAD"
	.dw 0xE187
	.dw SDLOAD_ROUTINE

	.db 0xC4
	.ascii "SDLS"
	.dw 0xE185
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
	.dw 0xE18E
	.dw SDRMDIR_ROUTINE

	.db 0xC4
	.ascii "SDRM"
	.dw 0xE188
	.dw SDRM_ROUTINE

	.db 0xC6
	.ascii "SDSAVE"
	.dw 0xE186
	.dw SDSAVE_ROUTINE

	.db 0xC4
	.ascii "SDCP"
	.dw 0xE18B
	.dw SDCP_ROUTINE

	.db 0xC4
	.ascii "SDCD"
	.dw 0xE18C
	.dw SDCD_ROUTINE

	.db 0xC7
	.ascii "SDMKDIR"
	.dw 0xE18D
	.dw SDMKDIR_ROUTINE

	.db 0xC5
	.ascii "SDPWD"
	.dw 0xE18F
	.dw SDPWD_ROUTINE

	; Not a prefix collision with SDMKDIR -- both share "SDM" but diverge
	; at the 4th character (V vs K), so table order doesn't matter here
	; (unlike SDRM/SDRMDIR above). E180 is the next unused code value --
	; E185-E18F are already taken by the eleven entries above.
	.db 0xC4
	.ascii "SDMV"
	.dw 0xE180
	.dw SDMV_ROUTINE

	; SDOPEN/SDCLOSE/SDINPUT/SDPRINT/SDSKIP -- table names deliberately
	; omit '#' (see SDINPUT_ROUTINE's own comment). Checked every pair
	; against all other entries above and each other for strict-prefix
	; collisions (the SDF-vs-SDFMT/SDRM-vs-SDRMDIR class of bug) -- none
	; found, so order doesn't matter here. Code values E190-E194 are the
	; next unused ones (E180-E18F already taken above).
	.db 0xC6
	.ascii "SDOPEN"
	.dw 0xE190
	.dw SDOPEN_ROUTINE

	.db 0xC7
	.ascii "SDCLOSE"
	.dw 0xE191
	.dw SDCLOSE_ROUTINE

	.db 0xC7
	.ascii "SDINPUT"
	.dw 0xE192
	.dw SDINPUT_ROUTINE

	.db 0xC7
	.ascii "SDPRINT"
	.dw 0xE193
	.dw SDPRINT_ROUTINE

	.db 0xC6
	.ascii "SDSKIP"
	.dw 0xE194
	.dw SDSKIP_ROUTINE

	; STAGE -- kept inside the contiguous SDxxx run (reached via the same
	; skip-scan chain as every entry above), not after ECVER/DOSTUFF's
	; own dedicated-first-letter-slot entries below -- see STAGE_ROUTINE's
	; own comment (with DOSTUFF's table entry, further down this file)
	; for why that first placement didn't work and the full mechanism
	; citation. Diverges from every SDxxx name at the second character
	; ('T' vs 'D'), so no prefix-collision risk with any entry in this
	; chain.
	.db 0xC5
	.ascii "STAGE"
	.dw 0xE197
	.dw STAGE_ROUTINE

	; ECVER -- no argument, own first-letter index slot (only entry starting
	; with 'E', so reached directly via the index, not the skip-scan --
	; marker high nibble doesn't matter here, same as SDDF's own comment).
	; Plain version string, no EXP_COMMAND_*/MCU round-trip at all -- exists
	; purely to verify the keyword table itself dispatches correctly and
	; that this ROM image is actually the one being served.
ECVER_TABLE_ENTRY:
	.db 0xC5
	.ascii "ECVER"
	.dw 0xE195
	.dw ECVER_ROUTINE

	; DOSTUFF -- diagnostic only (2026-09-17), own first-letter index slot
	; ('D', same pattern as ECVER's own 'E'). Optional numeric argument
	; (seconds to block core1 for, default 1 -- see EXP_COMMAND_TEST_DELAY's
	; own comment in pc_exp.h). Exists purely to test the LH5801-waits-
	; on-status / core0-serves-status mechanism with every other real
	; command's own complexity (SD card, I2C bridge, GreenPAK) removed --
	; see EC_WAIT_NOT_BUSY's own comment for the real-hardware issues that
	; motivated wanting this isolated test.
DOSTUFF_TABLE_ENTRY:
	.db 0xC7
	.ascii "DOSTUFF"
	.dw 0xE196
	.dw DOSTUFF_ROUTINE

	; MLOG -- own first-letter index slot ('M', same pattern as ECVER/
	; DOSTUFF's own 'E'/'D'). Named MLOG, not LOG -- LOG collides with
	; the PC-1500's own built-in LN/LOG/EXP logarithm functions (confirmed
	; live 2026-09-21: "LOG VIEW" was silently tokenized as the built-in
	; LOG(VIEW) function call, never reaching this ROM's own keyword table
	; at all, and failed with the base ROM's own ERROR 39 "illogical
	; calculation" -- the exact symptom a botched logarithm call produces,
	; not anything this file's own code raises). See MLOG_ROUTINE's own
	; header comment.
MLOG_TABLE_ENTRY:
	.db 0xC4
	.ascii "MLOG"
	.dw 0xE198
	.dw MLOG_ROUTINE

	; STAGE -- no argument: live-queries GreenPAK1's Remap virtual input
	; over I2C (EXP_COMMAND_ROM_GET_MODE) and reports "MCU"/"RAM"/"MODE
	; UNKNOWN" (the last for a board that doesn't implement GET_MODE,
	; e.g. PSoC5 today -- EXP_STATUS_NOT_IMPLEMENTED, not SUCCESS/ERROR,
	; so this must not be misread as either). Argument "RAM": jumps to
	; STAGE_COPY_ROUTINE_ABS (see that block's own header comment, near
	; the top of this file, for the full copy sequence and its safety
	; rules). Case-sensitive uppercase match only, matching this file's
	; existing KEY_Y/KEY_L convention.
	;
	; The actual keyword TABLE ENTRY lives with the SDxxx chain, right
	; after SDSKIP's own entry (before ECVER_TABLE_ENTRY) -- NOT here.
	; First attempt (2026-09) placed it here, after DOSTUFF/ECVER's own
	; dedicated-first-letter-slot entries, reasoning that "STAGE" shares
	; the existing 'S' index with the SDxxx chain and would just be
	; reached by skip-scanning further. Confirmed wrong on real hardware
	; (STAGE wasn't recognized at all) and root-caused against
	; PC1500_BASIC_Keyword_Extension_Mechanism.md's own bisected
	; disassembly (sec.4): a name mismatch does NOT skip by the marker's
	; encoded length -- it byte-scans forward hunting for the next byte
	; >=0xE0 to locate the CURRENT entry's own code field, then lands on
	; what should be the NEXT entry's marker. Every entry in this file
	; that's ever been reached this way has been contiguous within its
	; own letter's chain; nothing had ever needed to skip-scan THROUGH a
	; foreign entry with its own dedicated index slot (ECVER/DOSTUFF) to
	; reach something beyond it -- an untested path, and not one worth
	; re-litigating now that the safe, proven alternative (stay inside
	; the contiguous SDxxx run) is just as easy.
STAGE_MODE_MCU_MSG: .ascii "STAGE: MCU"
STAGE_MODE_MCU_MSG_LEN .equ 10
STAGE_MODE_RAM_MSG: .ascii "STAGE: RAM"
STAGE_MODE_RAM_MSG_LEN .equ 10
STAGE_MODE_UNKNOWN_MSG: .ascii "STAGE: MODE UNKNOWN"
STAGE_MODE_UNKNOWN_MSG_LEN .equ 19

STAGE_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES           ; leaves ACC holding the first non-space
	                             ; char (or 0DH at end of line), X pointing
	                             ; at it -- same convention DOSTUFF_ROUTINE
	                             ; itself relies on
	cpi a,0x0D                   ; end of line -- no argument, query mode
	bzs STAGE_QUERY
	cpi a,0x52                   ; 'R' -- RAM (normal, fast tin copy)
	bzs STAGE_CHECK_RAM
	cpi a,0x44                   ; 'D' -- DEBUG (slow, per-byte write+
	bzs STAGE_CHECK_DEBUG        ; verify copy, see STAGE_COPY_ROUTINE_ABS)
	cpi a,0x4D                   ; 'M' -- MCU (revert Remap back to
	bzs STAGE_CHECK_MCU          ; MCU-served; see STAGE_CHECK_MCU's own
	                              ; comment)
	jmp STAGE_BAD_ARG

STAGE_CHECK_RAM:
	inc x
	lda (x)
	cpi a,0x41                   ; 'A'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x4D                   ; 'M'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after RAM
	bzr STAGE_BAD_ARG
	ldi a,0x00                   ; defensive -- ensure normal (fast) mode
	sta (STAGE_DEBUG_FLAG)       ; even if a prior STAGE DEBUG run left it set
	jmp STAGE_COPY_ROUTINE_ABS    ; safe -- Remap hasn't switched yet, this
	                              ; jmp itself still executes from ROM_BASE+

; STAGE MCU (2026-09-23, board owner's own request) -- a "prettier"
; alternative to CALL 34701 (ROM_RESET_REMAP), added after finding that
; neither STAGE RAM nor STAGE DEBUG's own success path ever reverts Remap
; (EXP_COMMAND_ROM_FROM_MCU is only ever sent from the failure/revert paths,
; STAGE_COPY_MIDFAIL/STAGE_COPY_BADCHECKSUM) -- so a successful copy leaves
; 0x8800+ served by the external SRAM chip indefinitely, with no keyword-
; table-dispatched way back. Safe to use EC_WAIT_NOT_BUSY here (unlike
; STAGE_COPY_ROUTINE_ABS's own hand-rolled polling) -- ROM_FROM_MCU never
; switches Remap on, so nothing here ever executes from a Remap-redirected
; ROM_BASE+ the way BEGIN/GET_BLOCK's own callers have to guard against.
; Reuses STAGE_MODE_MCU_MSG/STAGE_QUERY's own confirm-and-return shape
; rather than duplicating it.
STAGE_CHECK_MCU:
	inc x
	lda (x)
	cpi a,0x43                   ; 'C'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x55                   ; 'U'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after MCU
	bzr STAGE_BAD_ARG
	ldi a,EXP_COMMAND_ROM_FROM_MCU
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	ldi uh,>STAGE_MODE_MCU_MSG
	ldi ul,<STAGE_MODE_MCU_MSG
	ldi xl,STAGE_MODE_MCU_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

; STAGE DEBUG (2026-09-21) -- board owner's own request, after the
; per-block checksum feature (see STAGE_COPY_ROUTINE_ABS's own comment)
; narrowed a real failure to "block 0, off by 0x28 overall" but couldn't
; say which byte. Dispatches into the SAME STAGE_COPY_ROUTINE_ABS, just
; with STAGE_DEBUG_FLAG set -- see that routine's own per-byte
; write+verify loop.
STAGE_CHECK_DEBUG:
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x42                   ; 'B'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x55                   ; 'U'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x47                   ; 'G'
	bzr STAGE_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after DEBUG
	bzr STAGE_BAD_ARG
	ldi a,0x01
	sta (STAGE_DEBUG_FLAG)
	jmp STAGE_COPY_ROUTINE_ABS

STAGE_BAD_ARG:
	jmp SD_RAISE_ERROR_1

STAGE_QUERY:
	ldi a,EXP_COMMAND_ROM_GET_MODE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr STAGE_QUERY_UNKNOWN
	lda (EXP_BUFFER_START_ABS)
	cpi a,0x01
	bzs STAGE_QUERY_RAM
	ldi uh,>STAGE_MODE_MCU_MSG
	ldi ul,<STAGE_MODE_MCU_MSG
	ldi xl,STAGE_MODE_MCU_MSG_LEN
	bch STAGE_QUERY_SHOW
STAGE_QUERY_RAM:
	ldi uh,>STAGE_MODE_RAM_MSG
	ldi ul,<STAGE_MODE_RAM_MSG
	ldi xl,STAGE_MODE_RAM_MSG_LEN
	bch STAGE_QUERY_SHOW
STAGE_QUERY_UNKNOWN:
	ldi uh,>STAGE_MODE_UNKNOWN_MSG
	ldi ul,<STAGE_MODE_UNKNOWN_MSG
	ldi xl,STAGE_MODE_UNKNOWN_MSG_LEN
STAGE_QUERY_SHOW:
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

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
; ECVER -- no argument. Prints a fixed product/version string and returns,
; same shape as SDFMT's own confirmation-message print below but with no
; EXP_COMMAND_*/MCU interaction at all -- a pure ROM-side check that the
; keyword table dispatches correctly and this exact ROM image is the one
; actually being served (useful for the RP2350 port's own bring-up,
; independent of whether its SD/DoCommand() path works at all).
;
; Blocks on KEYSCAN_WAIT before returning -- confirmed live 2026-09-17 that
; without it, the version string appeared to "die"/clear itself instantly:
; jmp KEYWORD_RETURN directly after DISP_N_CHARS0 immediately runs
; KEYWORD_RETURN's OWN DISP_N_CHARS0 call, which unconditionally blanks the
; whole line to draw the idle ">" prompt (see KEYWORD_RETURN's own comment --
; deliberate there, needed for keywords that don't print anything worth
; keeping). Any keypress (including BREAK) dismisses, same as SDFMT's
; confirmation prompt -- no need to branch on KEYSCAN_WAIT's Carry/ACC
; result here, unlike SDFMT_ROUTINE, since ECVER has no Y/N distinction to
; make.
ECVER_VERSION_MSG:
	.ascii "LH5801 Expansion Card 0.1"
ECVER_VERSION_MSG_LEN .equ 25

ECVER_ROUTINE:
	ldi uh,>ECVER_VERSION_MSG
	ldi ul,<ECVER_VERSION_MSG
	ldi xl,ECVER_VERSION_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; DOSTUFF -- diagnostic only, see this keyword's own table-entry comment.
; Optional argument: seconds to block core1 for (decimal, via the same
; SD_PARSE_NUMBER used elsewhere -- a leading '&' would parse hex instead,
; matching that routine's own PEEK/POKE-style convention, but nothing
; stops a hex value being used here too, it's just an unusual way to type
; a small number of seconds). No argument, or an unparseable one, means 1.
DOSTUFF_OK_MSG:
	.ascii "DOSTUFF OK"
DOSTUFF_OK_MSG_LEN .equ 10
DOSTUFF_BAD_MSG:
	.ascii "DOSTUFF BAD STATUS"
DOSTUFF_BAD_MSG_LEN .equ 18

DOSTUFF_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcs DOSTUFF_DEFAULT_SECONDS  ; no valid digits -- default to 1
	lda (SDLOAD_ADDR_LO_ABS)     ; shared scratch this parser always writes to,
	                             ; regardless of caller -- low byte is enough,
	                             ; nothing sane types more than a few hundred
	                             ; seconds here
	bch DOSTUFF_HAVE_SECONDS
DOSTUFF_DEFAULT_SECONDS:
	ldi a,0x01
DOSTUFF_HAVE_SECONDS:
	sta (EXP_BUFFER_START_ABS)
	ldi a,EXP_COMMAND_TEST_DELAY
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr DOSTUFF_SHOW_BAD
	ldi uh,>DOSTUFF_OK_MSG
	ldi ul,<DOSTUFF_OK_MSG
	ldi xl,DOSTUFF_OK_MSG_LEN
	bch DOSTUFF_SHOW_DONE
DOSTUFF_SHOW_BAD:
	ldi uh,>DOSTUFF_BAD_MSG
	ldi ul,<DOSTUFF_BAD_MSG
	ldi xl,DOSTUFF_BAD_MSG_LEN
DOSTUFF_SHOW_DONE:
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; MLOG -- own first-letter index slot (2026-09-21), same pattern as ECVER/
; DOSTUFF. Named MLOG, not LOG -- see MLOG_TABLE_ENTRY's own comment for
; why (collides with the PC-1500's own built-in LOG() function). Small,
; durable (flash-backed, see RP2350/mcu_log.h) rolling log of internal MCU
; failures a user has no other way to see evidence of.
;
; ARGUMENT WORDS, ROUND 3 (2026-09-21, same day as rounds 1/2 below): round 2
; settled on VIEW/INFO YES/INFO NO/RESET, but INFO NO still failed even
; though YES/NO are individually letters-only and collision-free -- a direct
; memory dump in pc1500emu showed WHY: "INFO"'s trailing 'O' plus "NO"'s
; leading 'N' were tokenized together as the reserved word ON, spanning the
; intended word boundary and corrupting the stored byte stream (49 4E 46 F1
; 9C 4F 0D instead of the literal "INFONO" bytes the two words should have
; produced back-to-back with the spaces stripped). This is a DIFFERENT
; hazard than round 2's: not a whole word being reserved, but two
; individually-safe words whose ADJACENT letters happen to spell a third
; reserved word right at the seam. Round 1's history (kept for context):
; VIEW/INFO ON/INFO OFF/CLEAR -- confirmed live (and reproduced
; deterministically in pc1500emu, not just once on real hardware) that ON,
; OFF, AND CLEAR are ALL themselves separately-reserved PC-1500 BASIC
; keywords too (ON/OFF: real syntax errors even typed bare, consistent with
; ON...GOTO/GOSUB; CLEAR: executes cleanly bare, consistent with a real
; no-argument statement that clears variables). Digits (1/0, the board
; owner's own first choice for the INFO toggle) were ALSO tried and ALSO
; failed -- the base ROM's own tokenizer evidently converts numeric literals
; at entry time too, not just reserved keywords, so a literal ASCII digit
; comparison here could never match either.
;
; Fixed by eliminating the two-word "INFO x" structure entirely in favor of
; two single top-level words, VERBOSE/QUIET, matching VIEW/RESET's
; already-proven-safe pattern -- nothing adjacent for a boundary to spell.
; Five forms, dispatched on the trailing argument text (case-sensitive
; uppercase, matching this file's own KEY_Y/KEY_L convention):
;   MLOG            (bare, no argument) query and display the current
;                    VERBOSE/QUIET INFO-logging state (2026-09-22, board
;                    owner's own request) -- see MLOG_QUERY_DO below
;   MLOG VIEW       browse entries (view-only, same SD_LIST_DISPLAY/UP/
;                    DOWN machinery SDLS/SDOPEN already use -- see
;                    MLOG_LIST_INIT below)
;   MLOG VERBOSE     enable INFO-level logging (default off -- WARN/ERROR
;   MLOG QUIET       are always logged regardless of this setting)
;   MLOG RESET       erase all entries (does not change the INFO setting)
; Anything else is SD_RAISE_ERROR_1, matching STAGE_BAD_ARG's own convention
; for an unrecognized argument.
;
; NOTE: VIEW and VERBOSE share a first letter ('V'), unlike every other pair
; here -- the top-level dispatch below can't branch on the first character
; alone the way it does for I/Q/R, so MLOG_CHECK_V disambiguates on the 2nd
; character ('I' vs 'E') before committing to either check chain.
MLOG_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	cpi a,0x0D                   ; bare MLOG, no argument -- query current
	bzs MLOG_QUERY_DO            ; VERBOSE/QUIET state, see MLOG_QUERY_DO
	cpi a,0x56                   ; 'V' -- VIEW or VERBOSE, see MLOG_CHECK_V
	bzs MLOG_CHECK_V
	cpi a,0x51                   ; 'Q' -- QUIET
	bzs MLOG_CHECK_QUIET
	cpi a,0x52                   ; 'R' -- RESET
	bzs MLOG_CHECK_RESET
	jmp MLOG_BAD_ARG

MLOG_CHECK_V:
	inc x
	lda (x)
	cpi a,0x49                   ; 'I' -- VIEW
	bzs MLOG_CHECK_VIEW_REST
	cpi a,0x45                   ; 'E' -- VERBOSE
	bzs MLOG_CHECK_VERBOSE_REST
	jmp MLOG_BAD_ARG

MLOG_CHECK_VIEW_REST:
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x57                   ; 'W'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after VIEW
	bzr MLOG_BAD_ARG
	jmp MLOG_VIEW_START

MLOG_CHECK_VERBOSE_REST:
	inc x
	lda (x)
	cpi a,0x52                   ; 'R'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x42                   ; 'B'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x4F                   ; 'O'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x53                   ; 'S'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after VERBOSE
	bzr MLOG_BAD_ARG
	jmp MLOG_VERBOSE_DO

MLOG_CHECK_QUIET:
	inc x
	lda (x)
	cpi a,0x55                   ; 'U'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x49                   ; 'I'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x54                   ; 'T'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after QUIET
	bzr MLOG_BAD_ARG
	jmp MLOG_QUIET_DO

MLOG_CHECK_RESET:
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x53                   ; 'S'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x45                   ; 'E'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x54                   ; 'T'
	bzr MLOG_BAD_ARG
	inc x
	lda (x)
	cpi a,0x0D                   ; nothing trailing after RESET
	bzr MLOG_BAD_ARG
	jmp MLOG_CLEAR_DO

MLOG_BAD_ARG:
	jmp SD_RAISE_ERROR_1

; Display-only text below -- never re-parsed/re-typed, so this wording
; carries no collision risk regardless of what the actual typed command
; words are (matches what the datasheet-style output elsewhere in this file
; already does -- human-readable text is free to differ from the wire/
; command vocabulary).
; 26 spaces -- SD_LIST_LINE_WIDTH, DISP_N_CHARS0's own max -- blanks
; whatever MLOG's own DISP_BUFFER argument echo left on screen (board
; owner's own request, 2026-09-21: RESET/VERBOSE/QUIET clear and return
; immediately rather than showing a message that blocks on a keypress).
MLOG_BLANK_MSG: .ascii "                          "
MLOG_BLANK_MSG_LEN .equ 26

; Bare "MLOG" (no argument) -- query the MCU's current INFO-logging state
; rather than raising SD_RAISE_ERROR_1, same idea as STAGE_QUERY's own bare-
; STAGE handling. Blocks on a keypress to read the result (STAGE_QUERY_SHOW's
; own convention), unlike VERBOSE_DO/QUIET_DO/CLEAR_DO's fire-and-clear
; convention above -- this is a read, not an action, so there's no reason to
; rush past it.
MLOG_STATE_VERBOSE_MSG: .ascii "MLOG: VERBOSE"
MLOG_STATE_VERBOSE_MSG_LEN .equ 13
MLOG_STATE_QUIET_MSG: .ascii "MLOG: QUIET"
MLOG_STATE_QUIET_MSG_LEN .equ 11

MLOG_QUERY_DO:
	ldi a,EXP_COMMAND_LOG_GET_INFO_ENABLED
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	lda (EXP_BUFFER_START_ABS)
	cpi a,0x00
	bzs MLOG_QUERY_SHOW_QUIET
	ldi uh,>MLOG_STATE_VERBOSE_MSG
	ldi ul,<MLOG_STATE_VERBOSE_MSG
	ldi xl,MLOG_STATE_VERBOSE_MSG_LEN
	bch MLOG_QUERY_SHOW
MLOG_QUERY_SHOW_QUIET:
	ldi uh,>MLOG_STATE_QUIET_MSG
	ldi ul,<MLOG_STATE_QUIET_MSG
	ldi xl,MLOG_STATE_QUIET_MSG_LEN
MLOG_QUERY_SHOW:
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

MLOG_VERBOSE_DO:
	ldi a,0x01
	sta (EXP_BUFFER_START_ABS)
	ldi a,EXP_COMMAND_LOG_SET_INFO_ENABLED
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	bch MLOG_ACTION_CLEAR

MLOG_QUIET_DO:
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS)
	ldi a,EXP_COMMAND_LOG_SET_INFO_ENABLED
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	bch MLOG_ACTION_CLEAR

MLOG_CLEAR_DO:
	ldi a,EXP_COMMAND_LOG_CLEAR
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	; falls through -- no per-action confirmation text (board owner's own
	; request, 2026-09-21: these three are fast toggles/actions, not
	; something worth blocking on a keypress to read, unlike MLOG VIEW's
	; own browse mode). Blanks the line and returns straight to READY.
MLOG_ACTION_CLEAR:
	ldi uh,>MLOG_BLANK_MSG
	ldi ul,<MLOG_BLANK_MSG
	ldi xl,MLOG_BLANK_MSG_LEN
	sjp DISP_N_CHARS0
	jmp KEYWORD_RETURN

; View-only browse, same shape as SDOPEN's own bare-argument channel
; listing (SD_CHANNEL_LIST_INIT/SDOPEN_WAITKEY) -- Enter, CL, or BREAK all
; exit back to READY; UP/DOWN scroll via the same format-agnostic
; SD_LIST_DISPLAY/UP/DOWN every other listing in this ROM already shares.
MLOG_VIEW_START:
	sjp MLOG_LIST_INIT
MLOG_VIEW_WAITKEY:
	sjp KEYSCAN_WAIT
	bcs MLOG_VIEW_EXIT             ; BREAK
	cpi a,KEY_CL
	bzs MLOG_VIEW_EXIT
	cpi a,KEY_ENTER
	bzs MLOG_VIEW_EXIT
	cpi a,KEY_UP
	bzs MLOG_VIEW_DO_UP
	cpi a,KEY_DOWN
	bzs MLOG_VIEW_DO_DOWN
	bch MLOG_VIEW_WAITKEY
MLOG_VIEW_DO_UP:
	sjp SD_LIST_UP
	sjp SD_LIST_DISPLAY
	bch MLOG_VIEW_WAITKEY
MLOG_VIEW_DO_DOWN:
	sjp SD_LIST_DOWN
	sjp SD_LIST_DISPLAY
	bch MLOG_VIEW_WAITKEY
MLOG_VIEW_EXIT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	jmp KEYWORD_RETURN

; Same as SD_CHANNEL_LIST_INIT but triggers EXP_COMMAND_LOG_LIST instead
; -- LOG_LIST reuses LIST_SD_DIR's own wire format exactly (see
; pc_exp.h), so SD_LIST_DISPLAY/UP/DOWN are reused verbatim here too.
MLOG_LIST_INIT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0

	ldi a,EXP_COMMAND_LOG_LIST
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY

	lda (EXP_BUFFER_START_ABS+1)
	sta (SD_LIST_COUNT_ABS)
	ldi a,0x00
	sta (SD_LIST_INDEX_ABS)
	ldi a,>(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_HI_ABS)
	ldi a,<(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_LO_ABS)
	jmp SD_LIST_DISPLAY          ; NOT sjp -- same reasoning as
	                              ; SD_CHANNEL_LIST_INIT's own tail

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
	sjp EC_WAIT_NOT_BUSY
SDFMT_ABORT:
	jmp KEYWORD_RETURN       ; KEYWORD_RETURN's own DISP_N_CHARS0 call blanks
	                         ; the confirmation prompt and redraws the ">"
	                         ; prompt either way, so nothing extra to clean up.

; ---------------------------------------------------------------------
; SDRM -- deletes a single file (never a directory -- see
; EXP_COMMAND_REMOVE_SD_FILE's own comment for the FS_GetFileAttributes
; guard that enforces this; SDRMDIR is the only way to remove a
; directory). Confirms first ("DELETE FILE? Y/N", explicit 'Y' proceeds,
; anything else including BREAK aborts) unless a trailing ",-Y" is given,
; matching SDSAVE's own overwrite-confirmation shape exactly -- reuses
; SD_PARSE_YFLAG/SDSAVE_YFLAG_ABS directly (a generic "-Y" flag, not
; actually SDSAVE-specific despite the name; safe to share since only one
; keyword ever dispatches at a time). A missing filename raises ERROR 1;
; the file genuinely not existing (or any other removal failure) raises
; ERROR 40, same as SDLOAD's own file-not-found convention.
SDRM_CONFIRM_MSG:
	.ascii "DELETE FILE? Y/N"
SDRM_CONFIRM_MSG_LEN .equ 16

SDRM_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22                     ; '"'
	bzs SDRM_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1           ; SDRM always requires "<filename>"
SDRM_HAVE_QUOTE:
	ldi a,0x00
	sta (SDSAVE_YFLAG_ABS)
	sjp SD_PARSE_QUOTED_NAME
	bcr SDRM_NAME_OK
	jmp SD_RAISE_ERROR_1
SDRM_NAME_OK:
	sjp SD_PARSE_YFLAG              ; optional trailing ",-Y"
	bcr SDRM_YFLAG_OK
	jmp SD_RAISE_ERROR_1
SDRM_YFLAG_OK:
	lda (SDSAVE_YFLAG_ABS)
	cpi a,0x00
	bzr SDRM_DO_REMOVE               ; -Y given -- skip the confirm prompt
	ldi uh,>SDRM_CONFIRM_MSG
	ldi ul,<SDRM_CONFIRM_MSG
	ldi xl,SDRM_CONFIRM_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcs SDRM_ABORT                   ; BREAK -- abort
	cpi a,KEY_Y
	bzr SDRM_ABORT                   ; anything other than 'Y' -- abort, no delete
SDRM_DO_REMOVE:
	ldi a,EXP_COMMAND_REMOVE_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDRM_DONE
	jmp SD_RAISE_ERROR_40
SDRM_DONE:
SDRM_ABORT:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDCP/SDMV -- both take two required quoted names, `SDCP "<src>","<dest>"`
; / `SDMV "<src>","<dest>"`, plain filenames within the current directory
; (same scope SDLOAD/SDSAVE already have -- no cross-directory paths).
; Overwrites an existing destination silently, matching Unix cp/mv's own
; default (no confirmation, unlike SDRM/SDSAVE above). A missing/malformed
; argument raises ERROR 1; the source not existing (or any other
; copy/move failure) raises ERROR 40.
;
; SD_PARSE_TWO_QUOTED_NAMES lays both names into two fixed
; EXP_TWO_NAME_SLOT_LEN-byte slots back-to-back at EXP_BUFFER_START_ABS
; (source first) -- see PC_EXP.h's own comment for why fixed-width rather
; than packed. SD_PARSE_QUOTED_NAME itself only ever writes to
; EXP_BUFFER_START_ABS, so each name is parsed there and then relocated:
; the first name is stashed to SD_TWONAME_STASH_ABS out of the way while
; the second is parsed (which reuses EXP_BUFFER_START_ABS), then both are
; copied into their final slots via SD_COPY_BYTES.
SD_TWONAME_STASH_ABS .equ (EXP_SCRATCH_ABS+77)  ; through +118 (EXP_TWO_NAME_SLOT_LEN=42 bytes)
SD_PTN_XSAVE_HI_ABS .equ (EXP_SCRATCH_ABS+119)  ; saves X (the DISP_BUFFER parse position) across
SD_PTN_XSAVE_LO_ABS .equ (EXP_SCRATCH_ABS+120)  ; the stash-copy below, which clobbers X itself

; SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP# -- see PC_EXP.h's own comment
; for the full wire-format writeup and this session's SD_LOOKUP_VARIABLE/
; SD_BUILD_VALUE_CHUNK/SD_CONSUME_VALUE_CHUNK comments for the D461H-based
; variable resolution and chunk format these six commands are built on.
SD_VARNAME_HI_ABS   .equ (EXP_SCRATCH_ABS+121)  ; D461H name code (TRM sec.5-3-4), high byte
SD_VARNAME_LO_ABS   .equ (EXP_SCRATCH_ABS+122)  ; ...low byte
SD_VAR_TYPE_ABS     .equ (EXP_SCRATCH_ABS+123)  ; SD_LOOKUP_VARIABLE's own returned 7A07H type/size
                                                  ; byte -- bit7 set = numeric, clear = string (low 7
                                                  ; bits = capacity)
SD_VAR_ADDR_HI_ABS  .equ (EXP_SCRATCH_ABS+124)  ; SD_LOOKUP_VARIABLE's own returned variable address
SD_VAR_ADDR_LO_ABS  .equ (EXP_SCRATCH_ABS+125)
SD_CHUNK_LEN_ABS    .equ (EXP_SCRATCH_ABS+126)  ; SD_BUILD/CONSUME_VALUE_CHUNK's own scratch: a
                                                  ; string chunk's real length (not the fixed capacity)
SD_CHUNK_CAP_ABS    .equ (EXP_SCRATCH_ABS+127)  ; ...the target string variable's own real capacity
SD_CHANNEL_ABS      .equ (EXP_SCRATCH_ABS+128)  ; parsed channel number, 1-16 (or 0 = "ALL", SDCLOSE
                                                  ; only)
SD_ARG_XSAVE_HI_ABS .equ (EXP_SCRATCH_ABS+129)  ; saves X (the DISP_BUFFER parse position) across
SD_ARG_XSAVE_LO_ABS .equ (EXP_SCRATCH_ABS+130)  ; SD_LOOKUP_VARIABLE/chunk-transfer calls, which are
                                                  ; not guaranteed to leave X alone (D461H is real
                                                  ; base-ROM code; its own internal register usage
                                                  ; beyond the documented U/7A07H results is
                                                  ; unconfirmed) -- SDINPUT#/SDPRINT#'s own per-
                                                  ; variable loop needs X to still be exactly where it
                                                  ; left off in DISP_BUFFER afterward, to keep parsing
                                                  ; the rest of the variable list

; Copies UL bytes from (X) to (Y), advancing both -- UH must be 0 (small
; counts only; every call site here uses EXP_TWO_NAME_SLOT_LEN, which fits
; in a byte).
SD_COPY_BYTES:
	tin
	dec u
	cpi uh,0x00
	bzr SD_COPY_BYTES
	cpi ul,0x00
	bzr SD_COPY_BYTES
	rtn

SD_PARSE_TWO_QUOTED_NAMES:
	lda (x)
	cpi a,0x22
	bzs SD_PTN_QUOTE1
	sec
	rtn
SD_PTN_QUOTE1:
	sjp SD_PARSE_QUOTED_NAME
	bcr SD_PTN_NAME1_OK
	sec
	rtn
SD_PTN_NAME1_OK:
	; save X (current DISP_BUFFER parse position, just past name1's
	; closing quote) -- the stash-copy below points X at the data window
	; instead, and it must be restored before any further DISP_BUFFER
	; parsing (the comma/name2 check right after).
	lda xh
	sta (SD_PTN_XSAVE_HI_ABS)
	lda xl
	sta (SD_PTN_XSAVE_LO_ABS)

	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>SD_TWONAME_STASH_ABS
	ldi yl,<SD_TWONAME_STASH_ABS
	ldi uh,0x00
	ldi ul,EXP_TWO_NAME_SLOT_LEN
	sjp SD_COPY_BYTES                ; stash name1 while name2 is parsed

	lda (SD_PTN_XSAVE_HI_ABS)
	sta xh
	lda (SD_PTN_XSAVE_LO_ABS)
	sta xl

	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C                       ; ','
	bzs SD_PTN_COMMA
	sec
	rtn
SD_PTN_COMMA:
	inc x
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22
	bzs SD_PTN_QUOTE2
	sec
	rtn
SD_PTN_QUOTE2:
	sjp SD_PARSE_QUOTED_NAME
	bcr SD_PTN_NAME2_OK
	sec
	rtn
SD_PTN_NAME2_OK:
	; save X (just past name2's closing quote) -- the two relocation
	; copies below clobber X again, and the caller needs it afterward to
	; parse an optional trailing ",-Y" (SD_PARSE_YFLAG).
	lda xh
	sta (SD_PTN_XSAVE_HI_ABS)
	lda xl
	sta (SD_PTN_XSAVE_LO_ABS)

	; name2 is at EXP_BUFFER_START_ABS -- move it to the second slot
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>(EXP_BUFFER_START_ABS+EXP_TWO_NAME_SLOT_LEN)
	ldi yl,<(EXP_BUFFER_START_ABS+EXP_TWO_NAME_SLOT_LEN)
	ldi uh,0x00
	ldi ul,EXP_TWO_NAME_SLOT_LEN
	sjp SD_COPY_BYTES
	; restore name1 into the first slot
	ldi xh,>SD_TWONAME_STASH_ABS
	ldi xl,<SD_TWONAME_STASH_ABS
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,0x00
	ldi ul,EXP_TWO_NAME_SLOT_LEN
	sjp SD_COPY_BYTES

	lda (SD_PTN_XSAVE_HI_ABS)
	sta xh
	lda (SD_PTN_XSAVE_LO_ABS)
	sta xl
	rec
	rtn

; SDCP/SDMV both confirm before overwriting an existing destination
; ("FILE EXISTS. OVERWRITE Y/N", reusing SDSAVE's own message/constants)
; unless a trailing ",-Y" is given -- matching SDSAVE/SDRM's own
; established convention. EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS does
; the same destination resolution (including directory-target basename-
; join) the actual COPY/MOVE_SD_FILE command will, so the ROM never needs
; to know the real resolved target itself -- just whether to prompt.
SDCP_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	ldi a,0x00
	sta (SDSAVE_YFLAG_ABS)
	sjp SD_PARSE_TWO_QUOTED_NAMES
	bcr SDCP_NAMES_OK
	jmp SD_RAISE_ERROR_1
SDCP_NAMES_OK:
	sjp SD_PARSE_YFLAG
	bcr SDCP_YFLAG_OK
	jmp SD_RAISE_ERROR_1
SDCP_YFLAG_OK:
	ldi a,EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr SDCP_DO_COPY                 ; doesn't exist -- no confirmation needed
	lda (SDSAVE_YFLAG_ABS)
	cpi a,0x00
	bzr SDCP_DO_COPY                 ; -Y given -- skip the prompt, overwrite unconditionally
	ldi uh,>SDSAVE_CONFIRM_MSG
	ldi ul,<SDSAVE_CONFIRM_MSG
	ldi xl,SDSAVE_CONFIRM_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcs SDCP_ABORT                   ; BREAK -- abort
	cpi a,KEY_Y
	bzs SDCP_DO_COPY
	jmp SDCP_ABORT                   ; anything but Y -- abort
SDCP_DO_COPY:
	ldi a,EXP_COMMAND_COPY_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDCP_DONE
	jmp SD_RAISE_ERROR_40
SDCP_DONE:
SDCP_ABORT:
	jmp KEYWORD_RETURN

SDMV_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	ldi a,0x00
	sta (SDSAVE_YFLAG_ABS)
	sjp SD_PARSE_TWO_QUOTED_NAMES
	bcr SDMV_NAMES_OK
	jmp SD_RAISE_ERROR_1
SDMV_NAMES_OK:
	sjp SD_PARSE_YFLAG
	bcr SDMV_YFLAG_OK
	jmp SD_RAISE_ERROR_1
SDMV_YFLAG_OK:
	ldi a,EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr SDMV_DO_MOVE                 ; doesn't exist -- no confirmation needed
	lda (SDSAVE_YFLAG_ABS)
	cpi a,0x00
	bzr SDMV_DO_MOVE                 ; -Y given -- skip the prompt, overwrite unconditionally
	ldi uh,>SDSAVE_CONFIRM_MSG
	ldi ul,<SDSAVE_CONFIRM_MSG
	ldi xl,SDSAVE_CONFIRM_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcs SDMV_ABORT                   ; BREAK -- abort
	cpi a,KEY_Y
	bzs SDMV_DO_MOVE
	jmp SDMV_ABORT                   ; anything but Y -- abort
SDMV_DO_MOVE:
	ldi a,EXP_COMMAND_MOVE_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDMV_DONE
	jmp SD_RAISE_ERROR_40
SDMV_DONE:
SDMV_ABORT:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDCD/SDMKDIR/SDRMDIR -- all three take a single required "<dirname>"
; argument (same SD_PARSE_QUOTED_NAME parse SDSAVE's own filename
; argument uses) and operate on SEGGER emFile's single global current-
; directory concept via EXP_COMMAND_CHANGE_SD_DIR/MAKE_SD_DIR/
; REMOVE_SD_DIR -- see PC_EXP.h's own comment for why every other SD
; command that takes a bare filename is implicitly relative to wherever
; SDCD last left it, with no changes needed on their own end.
;
; Missing/malformed argument raises a genuine ERROR 1 (SD_RAISE_ERROR_1),
; matching SDSAVE's own convention -- these describe a specific,
; deliberate operation the user asked for, not a passive browsing
; command like SDLOAD's own silent-abort-on-malformed-argument. A
; well-formed argument that the filesystem itself then rejects (e.g. CD
; into a directory that doesn't exist, RMDIR on a non-empty one -- emFile's
; FS_RmDir only removes an *empty* directory) is a silent abort instead,
; matching SD_CREATE_AND_WRITE's own existing convention for I/O-layer
; failures (as opposed to SDLOAD's file-not-found, which the user
; separately asked to raise ERROR 40 -- no equivalent request has come in
; yet for these three, so they stay silent for now; trivial to add later
; if wanted).
SDCD_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22                    ; '"'
	bzs SDCD_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1          ; SDCD always requires "<dirname>"
SDCD_HAVE_QUOTE:
	sjp SD_PARSE_QUOTED_NAME
	bcr SDCD_NAME_OK
	jmp SD_RAISE_ERROR_1
SDCD_NAME_OK:
	ldi a,EXP_COMMAND_CHANGE_SD_DIR
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY           ; status not checked further -- silent abort either way,
	                               ; see this section's own block comment
	jmp KEYWORD_RETURN

SDMKDIR_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22
	bzs SDMKDIR_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1          ; SDMKDIR always requires "<dirname>"
SDMKDIR_HAVE_QUOTE:
	sjp SD_PARSE_QUOTED_NAME
	bcr SDMKDIR_NAME_OK
	jmp SD_RAISE_ERROR_1
SDMKDIR_NAME_OK:
	ldi a,EXP_COMMAND_MAKE_SD_DIR
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	jmp KEYWORD_RETURN

SDRMDIR_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x22
	bzs SDRMDIR_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1          ; SDRMDIR always requires "<dirname>"
SDRMDIR_HAVE_QUOTE:
	sjp SD_PARSE_QUOTED_NAME
	bcr SDRMDIR_NAME_OK
	jmp SD_RAISE_ERROR_1
SDRMDIR_NAME_OK:
	ldi a,EXP_COMMAND_REMOVE_SD_DIR
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	jmp KEYWORD_RETURN

; SDPWD -- no argument. Triggers EXP_COMMAND_GET_SD_CWD, which comes back
; length-prefixed into EXP_SCRATCH_ABS (same convention as the browse
; listing's own entries -- see SD_LIST_DISPLAY), then blits it. The line
; is blanked first (SD_LIST_BLANK, reused from the browse listing) since
; the real path is usually shorter than the display width and
; DISP_N_CHARS0 only touches the bytes it's told to draw -- without this,
; whatever was on screen before would show through past the path's own
; end. Length is clamped to SD_LIST_LINE_WIDTH in the (currently
; unreachable in practice, cwd's own main.c-side buffer is 64 bytes but
; nothing stops a deep path from exceeding the display width) case it's
; longer than one line.
SDPWD_ROUTINE:
	ldi a,EXP_COMMAND_GET_SD_CWD
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDPWD_GOT_CWD
	jmp KEYWORD_RETURN             ; failed -- silent abort, matching this section's own convention
SDPWD_GOT_CWD:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	lda (EXP_SCRATCH_ABS)          ; the MCU's own length-prefix byte
	cpi a,SD_LIST_LINE_WIDTH
	bcr SDPWD_LEN_OK               ; a < SD_LIST_LINE_WIDTH -- use as-is
	ldi a,SD_LIST_LINE_WIDTH       ; clamp to the display width
SDPWD_LEN_OK:
	sta xl
	ldi uh,>(EXP_SCRATCH_ABS+1)
	ldi ul,<(EXP_SCRATCH_ABS+1)
	sjp DISP_N_CHARS0
	; Same fix as ECVER's own (see that routine's header comment,
	; 2026-09-17) and SDDF_ROUTINE's identical case just below --
	; KEYWORD_RETURN unconditionally redraws the idle ">" prompt over
	; the whole line, so this would otherwise get overwritten instantly.
	sjp KEYSCAN_WAIT
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
; live in the data window at SD_LIST_SCRATCH_ABS (not EXP_SCRATCH_ABS --
; see that constant's own comment for why: it collides with real entry
; #8's data once a listing has 9+ files), not CPU registers -- SJPing
; into KEYSCAN_WAIT/DISP_N_CHARS0 doesn't preserve registers across the
; call. This only actually persists when the ROM is attached via
; pc1500emu's `loadexpansionmodule` (a genuinely writable data window) --
; plain `loadrommodule` models true read-only ROM there, so a write to
; SD_LIST_SCRATCH_ABS silently no-ops and the browser gets stuck
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
; Deliberately NOT EXP_SCRATCH_ABS (0x8100, window offset 256) -- confirmed
; live 2026-09-19/20 (real hardware, reported as "scrolling down is fine,
; scrolling back up sometimes shows corrupted file names"): a directory
; listing's own entries (EXP_DIR_RECORD_SIZE=30 bytes each, starting at
; window offset 2) reach window offset 256 once there are 9+ files --
; entry #8 specifically occupies [242, 272), which contains [256, 260),
; exactly EXP_SCRATCH_ABS's own 4 bytes. Every SD_LIST_UP/DOWN keypress
; overwrites the last 2 characters of entry #8's name (byte 256, 257) and
; the first 2 characters of its size text (258, 259) with whatever the
; CURRENT index/count/address values happen to be -- invisible while
; scrolling past it (you're not looking at it that instant), only
; visible the next time it's redrawn, which is why going back up to
; revisit it looked "random": the garbage bytes are whatever navigation
; state was last written, not a hardware/PIO read glitch at all. Fixed
; by moving these 4 bytes to window offset 2038 instead -- past every
; real entry's maximum reach (2 + EXP_DIR_MAX_ENTRIES(67)*30 = 2012) AND
; past the summary line's own maximum reach at that same worst case
; (2012 + EXP_DIR_SUMMARY_LEN(26) = 2038) -- and still safely before
; EXP_LENGTH_PORT_* at window offset 2045, leaving a genuinely free,
; count-independent 7-byte gap these 4 bytes fit in regardless of how
; many files are actually listed.
SD_LIST_SCRATCH_ABS .equ (WINDOW_BASE + 2038)
SD_LIST_INDEX_ABS   .equ (SD_LIST_SCRATCH_ABS+0)  ; current entry index, 0..count (count = summary)
SD_LIST_COUNT_ABS   .equ (SD_LIST_SCRATCH_ABS+1)  ; real entry count, low byte only (see comment above)
SD_LIST_ADDR_HI_ABS .equ (SD_LIST_SCRATCH_ABS+2)  ; current entry/summary address, high byte
SD_LIST_ADDR_LO_ABS .equ (SD_LIST_SCRATCH_ABS+3)  ; ...low byte
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
	sjp EC_WAIT_NOT_BUSY
	bcs SD_LIST_INIT_BREAK       ; BREAK during dispatch -- bail before touching
	                             ; anything EC_WAIT_NOT_BUSY might not have
	                             ; finished populating yet; propagate Carry to
	                             ; our own caller (SDLS_ROUTINE/SDLOAD_BROWSE_COMMON),
	                             ; which already knows how to exit on it.

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
	; DISP_N_CHARS0 (BASIC's own shared display routine, ROM1.BIN 0xED3B)
	; has a side effect of moving BLINK_CURSOR_H/L (787EH/787FH) to point
	; right after whatever it just drew -- correct for its real purpose
	; (echoing typed input, where the cursor belongs right after the new
	; text), but wrong here: confirmed live this leaves a visible blinking
	; block cursor mid-line while browsing, driven by BASIC's own
	; background blink-refresh interrupt blindly rendering whatever
	; BLINK_CURSOR_H/L currently points at, regardless of context. Reset
	; it to 7400H -- confirmed live this is exactly what a genuinely idle
	; prompt (nothing drawn/typed since boot) holds there, i.e. "no live
	; cursor target."
	ldi a,0x74
	sta (0x787E)
	ldi a,0x00
	sta (0x787F)
	; Explicit rec before returning (2026-09-17) -- SD_LIST_INIT falls
	; through into this routine on its own normal (non-BREAK) path, and
	; SDLS_ROUTINE/SDLOAD_BROWSE_COMMON both check Carry immediately after
	; their own sjp SD_LIST_INIT to catch a BREAK that happened during
	; dispatch (see EC_WAIT_NOT_BUSY's own header comment). Without this,
	; that check was actually testing whatever Carry state DISP_N_CHARS0
	; above (a real ROM1 routine we don't control) happened to leave
	; behind for the specific text it just rendered -- pure happenstance,
	; not a real BREAK signal. Confirmed live: this made SDLOAD's own
	; "L" file-selection silently, spuriously abort back to READY right
	; after every dispatch, for a directory listing that never involved a
	; real BREAK at all -- SDLS's equivalent check merely got lucky with
	; whatever Carry its own test fixture's listing happened to leave.
	rec
	rtn

; Reached only via SD_LIST_INIT's own bcs above (BREAK during dispatch) --
; Carry is still set from EC_WAIT_NOT_BUSY's own return, so this just
; propagates it straight up to whichever of SDLS_ROUTINE/
; SDLOAD_BROWSE_COMMON called us; each already has its own bcs right after
; its sjp SD_LIST_INIT to catch it, matching KEYSCAN_WAIT's Carry
; convention used everywhere else in this ROM.
SD_LIST_INIT_BREAK:
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

; Waits for the expansion status byte to leave BUSY -- shared by every
; command's own poll below instead of each duplicating the same
; lda/cpi/bzs (2026-09-17, when DoCommand() moved to its own MCU core;
; renamed from SD_WAIT_NOT_BUSY since it's not SD-specific -- reusable for
; any future expansion-card command that can take a while, e.g. BLE/WiFi
; work on the Pico 2 W).
;
; HLT-based (sleep until the CPU's own polynomial timer interrupt,
; instead of busy-spinning) so expansion commands wait more like built-in
; ones do, and for power consumption. A first attempt at this hung the
; machine on real hardware (confirmed working in the emulator, including
; a dedicated empirical test of HLT-then-interrupt-then-resume against
; the real CPU core, but NOT on the real board) because it called plain
; `hlt` with no `am0` before it. Root-caused by reading the real ROM1
; disassembly's own idle loop (_bisect/rom1.asm ~LE29E, confirmed via the
; actual PC-1500 dump on this machine, not a guess): the timer is a 9-bit
; polynomial (LFSR) counter that decays to a fixed point at 0 and PARKS
; THERE PERMANENTLY once it reaches it (real hardware behavior, also
; modeled in the emulator's own tickTimer()) -- it has to be explicitly
; re-seeded with a nonzero value via AM0/AM1 before every wait that wants
; a fresh interrupt out of it, which the first attempt never did.
; `ldi a,0x57 / am0` immediately before `sie / hlt` below is the exact
; same sequence the real idle loop itself uses -- same seed value too,
; reusing a real, hardware-proven period rather than picking a new one.
;
; BREAK detection REMOVED 2026-09-17 after real-hardware testing: reading
; the IF register (F00BH) bit 0x02 right after a routine TIMER-driven
; wake came back set every single time, with no real BREAK pressed at
; all -- confirmed live (DOSTUFF showing BAD STATUS almost immediately,
; every time, regardless of the requested delay). Either this specific
; timer interrupt's own dispatch has some real coupling with this
; register on actual silicon that the emulator's model doesn't capture,
; or something else not yet understood -- but the bit clearly can't
; currently distinguish "routine timer wake" from "real BREAK" in this
; context, unlike its use elsewhere in the base ROM (which never combines
; it with a MI source this routine also wakes on). Reverted to a plain
; timer-driven wake with no BREAK check at all pending real
; understanding of that coupling -- don't reintroduce this specific
; check without confirming what's actually different here.
;
; Returns with Carry CLEAR and ACC holding the final status. Bounded by
; the MCU side's own command watchdog regardless of how long the real
; command takes.
EC_WAIT_NOT_BUSY:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzr EC_WAIT_NOT_BUSY_DONE
	ldi a,0x57
	am0
	sie
	hlt
	bch EC_WAIT_NOT_BUSY
EC_WAIT_NOT_BUSY_DONE:
	rec
	rtn

; ---------------------------------------------------------------------
; SDLS -- browse only. Enter, CL, or BREAK all exit back to READY.
SDLS_ROUTINE:
	sjp SD_LIST_INIT
	bcs SDLS_EXIT              ; BREAK during dispatch itself
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

; Relocated off EXP_SCRATCH_ABS (page 1, 0x8100) to page 7's now-otherwise-
; empty tail, 2026 session: EXP_COMMAND_READ_FROM_SD_FILE's widened
; 1024-byte payload now spans pages 0-3, which would otherwise stomp these
; mid-transfer (SD_OPEN_AND_LOAD_READ_LOOP runs concurrently with these
; being live). See PC_EXP.h's EXP_LENGTH_PORT comment for the full layout.
SDLOAD_SCRATCH_BASE_ABS .equ 0x87C0  ; 12 bytes, 0x87C0-0x87CB -- clear of
                                       ; EXP_LENGTH_PORT_ABS (0x87FD) and
                                       ; EXP_INSTRUCTION_ABS (0x87FF)
SDLOAD_WRITE_HI_ABS .equ (SDLOAD_SCRATCH_BASE_ABS+0)  ; running program-area write pointer, high byte
SDLOAD_WRITE_LO_ABS .equ (SDLOAD_SCRATCH_BASE_ABS+1)  ; ...low byte
SDLOAD_NAMELEN_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+2)  ; filename length -- shared by both the browse-listing
                                                ; trim (SD_STAGE_LISTED_NAME) and the typed-argument
                                                ; quoted-string parse (SD_PARSE_QUOTED_NAME); never
                                                ; needed simultaneously, so one slot covers both
SDLOAD_MODE_ABS     .equ (SDLOAD_SCRATCH_BASE_ABS+3)  ; one of the SDLOAD_MODE_* values above
SDLOAD_ADDR_HI_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+4)  ; M-mode target address (explicit-parsed or read
SDLOAD_ADDR_LO_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+5)  ; from the file's own header), 2-byte BE
SDLOAD_TEMP_HI_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+6)  ; SD_PARSE_DECIMAL's own 16-bit multiply scratch
SDLOAD_TEMP_LO_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+7)
SDLOAD_DIGITVAL_ABS .equ (SDLOAD_SCRATCH_BASE_ABS+8)  ; SD_PARSE_DECIMAL/SD_PARSE_HEX's own
                                                 ; "consumed >=1 digit" flag
SDLOAD_DIGITNIBBLE_ABS .equ (SDLOAD_SCRATCH_BASE_ABS+9)  ; ...this digit's numeric value, 0-15 (0-9 for
                                                    ; decimal, 0-15 for hex)
SDLOAD_CALL_HI_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+10)  ; M-mode call address read from the file's own
SDLOAD_CALL_LO_ABS  .equ (SDLOAD_SCRATCH_BASE_ABS+11)  ; header (0x0000 = none); see the M-mode format
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
	bcs SDLOAD_ABORT           ; BREAK during dispatch itself
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
; terminator before a closing quote, an empty "", too long, or a name
; violating the 8.3 shape check -- see EXP_COMMAND_VALIDATE_SD_NAME's own
; comment for the exact rule), CLEAR = success. Capped at EXP_PATH_ARG_LEN
; characters (deliberately separate from EXP_DIR_NAME_LEN, the SDLS
; display column width -- a path argument isn't displayed in that fixed
; column, so it can be longer).
;
; Uppercase-folding and 8.3-shape validation moved to the MCU/mock side
; (EXP_COMMAND_VALIDATE_SD_NAME) 2026-08-19 -- this routine now only
; copies the raw typed characters verbatim and checks the three things
; that have to happen before any wire traffic makes sense at all
; (unterminated/overlong/empty), then hands the staged name to that
; command for the actual shape check, mapping its status onto the exact
; same Carry contract this routine always had so every call site (SDCP/
; SDMV's own two-name parser included) needed zero changes. Moved because
; the shape check was pure character classification -- much more
; naturally expressed in C than as a hand-rolled LH5801 state machine --
; and the MCU already receives the full name for every command that uses
; one anyway.
SD_PARSE_QUOTED_NAME:
	inc x                       ; past the opening quote
	ldi yh,>(EXP_BUFFER_START_ABS+2)
	ldi yl,<(EXP_BUFFER_START_ABS+2)
	ldi a,0x00
	sta (SDLOAD_NAMELEN_ABS)
SD_PARSE_QUOTED_LOOP:
	lda (x)
	cpi a,0x0D
	bzr SD_PQ_NOTTERM
	jmp SD_PARSE_QUOTED_UNTERMINATED
SD_PQ_NOTTERM:
	lda (x)
	cpi a,0x22                  ; closing quote?
	bzr SD_PQ_NOTQUOTE
	jmp SD_PARSE_QUOTED_DONE
SD_PQ_NOTQUOTE:
	lda (SDLOAD_NAMELEN_ABS)
	cpi a,EXP_PATH_ARG_LEN
	bzr SD_PQ_NOTLONG
	jmp SD_PARSE_QUOTED_UNTERMINATED  ; too long -- treat as malformed rather than truncate silently
SD_PQ_NOTLONG:
	lda (x)
	sta (y)
	inc y
	inc x
	lda (SDLOAD_NAMELEN_ABS)
	inc a
	sta (SDLOAD_NAMELEN_ABS)
	jmp SD_PARSE_QUOTED_LOOP

SD_PARSE_QUOTED_DONE:
	inc x                       ; past the closing quote
	lda (SDLOAD_NAMELEN_ABS)
	cpi a,0x00
	bzs SD_PARSE_QUOTED_UNTERMINATED  ; empty "" -- reject same as malformed
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDLOAD_NAMELEN_ABS)
	sta (EXP_BUFFER_START_ABS+1)

	ldi a,EXP_COMMAND_VALIDATE_SD_NAME
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SD_PARSE_QUOTED_VALID
	jmp SD_PARSE_QUOTED_UNTERMINATED  ; shape violation -- same "malformed" exit every caller already handles
SD_PARSE_QUOTED_VALID:
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
; reads it in EXP_COMMAND_READ_FROM_SD_FILE's own max-EXP_MAX_TRANSFER_LEN-byte chunks into
; memory starting at whatever SDLOAD_MODE_ABS says the target should be,
; closes the file, and (BASIC mode only) updates the program end-pointer.
; Ends by jumping straight to KEYWORD_RETURN -- every call site wants that
; as its own final step, so this never returns to its caller normally.
SD_OPEN_AND_LOAD:
	ldi a,EXP_COMMAND_OPEN_SD_FILE_READ
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
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
	; payload starts" unambiguous either way). Length lives at
	; EXP_LENGTH_PORT_ABS now, outside the payload -- the header's 4 bytes
	; are the full, un-prefixed EXP_BUFFER_START_ABS+0..+3 (was +2..+5).
	ldi a,0x00
	sta (EXP_LENGTH_PORT_ABS+0)
	ldi a,0x04
	sta (EXP_LENGTH_PORT_ABS+1)
	ldi a,EXP_COMMAND_READ_FROM_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_OPEN_AND_LOAD_FAIL  ; couldn't even read the header -- bail
	lda (EXP_LENGTH_PORT_ABS+1)
	cpi a,0x04
	bzr SD_OPEN_AND_LOAD_FAIL  ; short read on the header -- file too small/corrupt, bail
	bch SD_OPEN_AND_LOAD_HDR_OK
SD_OPEN_AND_LOAD_FAIL:
	jmp KEYWORD_RETURN         ; local trampoline -- SDLOAD_ABORT (also just `jmp KEYWORD_RETURN`)
	                           ; is too far away for an 8-bit relative branch to reach from here
SD_OPEN_AND_LOAD_HDR_OK:
	lda (EXP_BUFFER_START_ABS+2)     ; call address -- stashed regardless of mode; only actually
	sta (SDLOAD_CALL_HI_ABS)         ; used below once loading is done, and only in M_HEADER mode
	lda (EXP_BUFFER_START_ABS+3)
	sta (SDLOAD_CALL_LO_ABS)

	lda (SDLOAD_MODE_ABS)
	cpi a,SDLOAD_MODE_M_HEADER
	bzr SD_OPEN_AND_LOAD_SET_TARGET  ; explicit mode -- SDLOAD_ADDR_HI/LO_ABS already set, keep it
	lda (EXP_BUFFER_START_ABS+0)     ; header mode -- use the file's own address
	sta (SDLOAD_ADDR_HI_ABS)
	lda (EXP_BUFFER_START_ABS+1)
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
	ldi a,>EXP_MAX_TRANSFER_LEN  ; request EXP_MAX_TRANSFER_LEN (1024) bytes --
	sta (EXP_LENGTH_PORT_ABS+0)  ; EXP_COMMAND_READ_FROM_SD_FILE's own max; length
	ldi a,<EXP_MAX_TRANSFER_LEN  ; lives at EXP_LENGTH_PORT_ABS, outside the payload
	sta (EXP_LENGTH_PORT_ABS+1)
	ldi a,EXP_COMMAND_READ_FROM_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_OPEN_AND_LOAD_CLOSE  ; read error -- stop, close/finalize with what we have

	lda (EXP_LENGTH_PORT_ABS+0)  ; actual bytes-read count, high byte (now genuinely
	sta uh                       ; used -- up to 1024 no longer fits the low byte alone)
	cpi a,0x00
	bzr SD_OPEN_AND_LOAD_HAVE_DATA
	lda (EXP_LENGTH_PORT_ABS+1)  ; ...low byte
	cpi a,0x00
	bzs SD_OPEN_AND_LOAD_CLOSE  ; 0 bytes -- end of file, done

SD_OPEN_AND_LOAD_HAVE_DATA:
	ldi xh,>EXP_BUFFER_START_ABS  ; payload is now the full, un-prefixed window --
	ldi xl,<EXP_BUFFER_START_ABS  ; no +2 offset, length no longer lives in-band
	lda (SDLOAD_WRITE_HI_ABS)
	sta yh
	lda (SDLOAD_WRITE_LO_ABS)
	sta yl
	lda (EXP_LENGTH_PORT_ABS+1)
	sta ul                       ; uh already loaded above
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
	sjp EC_WAIT_NOT_BUSY

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

; Relocated off EXP_SCRATCH_ABS (page 1) for the same reason as the SDLOAD_*
; block above -- see its own comment. SDSAVE_CHUNKLEN_ABS additionally
; widened from a single accumulator byte (inherently capped at 254 by
; construction) to a real 2-byte counter, since a chunk can now be up to
; EXP_MAX_TRANSFER_LEN (1024) bytes.
SDSAVE_SCRATCH_BASE_ABS .equ 0x87CC  ; 15 bytes, 0x87CC-0x87DA
SDSAVE_START_HI_ABS      .equ (SDSAVE_SCRATCH_BASE_ABS+0)  ; M mode -- parsed <start> address, 2-byte BE
SDSAVE_START_LO_ABS      .equ (SDSAVE_SCRATCH_BASE_ABS+1)
SDSAVE_END_HI_ABS        .equ (SDSAVE_SCRATCH_BASE_ABS+2)  ; M mode -- parsed <end> address (inclusive), BE
SDSAVE_END_LO_ABS        .equ (SDSAVE_SCRATCH_BASE_ABS+3)
SDSAVE_CALL_HI_ABS       .equ (SDSAVE_SCRATCH_BASE_ABS+4)  ; M mode -- parsed <call> address, or 0x0000
SDSAVE_CALL_LO_ABS       .equ (SDSAVE_SCRATCH_BASE_ABS+5)
SDSAVE_YFLAG_ABS         .equ (SDSAVE_SCRATCH_BASE_ABS+6)  ; nonzero = -Y given, skip the overwrite prompt
SDSAVE_MODE_ABS          .equ (SDSAVE_SCRATCH_BASE_ABS+7)  ; one of the SDSAVE_MODE_* values above
SDSAVE_WRITE_HI_ABS      .equ (SDSAVE_SCRATCH_BASE_ABS+8)  ; SD_WRITE_RANGE's own running source pointer
SDSAVE_WRITE_LO_ABS      .equ (SDSAVE_SCRATCH_BASE_ABS+9)
SDSAVE_RANGE_END_HI_ABS  .equ (SDSAVE_SCRATCH_BASE_ABS+10)  ; SD_WRITE_RANGE's own inclusive end address
SDSAVE_RANGE_END_LO_ABS  .equ (SDSAVE_SCRATCH_BASE_ABS+11)
SDSAVE_CHUNKLEN_HI_ABS   .equ (SDSAVE_SCRATCH_BASE_ABS+12)  ; SD_WRITE_RANGE's own current-chunk byte
SDSAVE_CHUNKLEN_LO_ABS   .equ (SDSAVE_SCRATCH_BASE_ABS+13)  ; count, 2-byte BE (was 1 byte pre-1024)
SDSAVE_RANGE_DONE_ABS    .equ (SDSAVE_SCRATCH_BASE_ABS+14)  ; SD_WRITE_RANGE's own "this is the last chunk" flag
SDSAVE_NAME_STASH_LEN .equ (2 + EXP_PATH_ARG_LEN)  ; 2-byte length prefix + up to EXP_PATH_ARG_LEN
                                                   ; path-argument chars (SDSAVE's name is a full
                                                   ; path now too, same as every other SD command)
SDSAVE_NAME_STASH_ABS .equ (EXP_SCRATCH_ABS+30)  ; through +71 -- see SD_CREATE_AND_WRITE's own
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

; Raises BASIC "ERROR 42" -- SDINPUT# only, when a value chunk read back
; from an SD file is a string longer than the target variable's own real
; capacity (always <=16 characters for a simple variable, confirmed live --
; this can only happen from a corrupted or hand-crafted SD file, since a
; genuine SDPRINT# of a real variable's own value can never write a chunk
; that overflows what any variable could legitimately hold).
SD_RAISE_ERROR_42:
	ldi uh,42
	vej 0xE0

; ---------------------------------------------------------------------
; SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP# support -- variable name
; parsing, D461H-based address lookup, and value-chunk build/consume.
; See PC_EXP.h's own comment for the chunk wire format and the overall
; "rom.asm resolves variables, main.c/ExpansionMock only move opaque
; bytes" architecture.

D461_VAR_SEARCH     .equ 0xD461  ; base ROM system subroutine, TRM sec.5-4-4,
                                  ; "variable address search" -- confirmed live
                                  ; this session via a hand-assembled test
                                  ; routine (against both a numeric and a
                                  ; string simple variable)
D461_ARRAY_FLAG_ABS .equ 0x788C  ; must be 0x00 before calling D461H for a
                                  ; simple (non-array) variable -- arrays are
                                  ; out of scope for this session's design
D461_TYPE_ABS       .equ 0x7A07  ; on success, D461H leaves the variable's
                                  ; type/size byte here (bit7 set = numeric;
                                  ; clear = string, low 7 bits = capacity)

; Parses a bare variable name at (X) in DISP_BUFFER: 1-2 letters, optionally
; followed by a trailing '$' (string-variable suffix) -- no quotes, no
; array subscripts (arrays are out of scope, see this session's design).
; Folds lowercase letters to uppercase as it goes. Builds the 2-byte D461H
; name code (TRM sec.5-3-4, confirmed live this session against real
; variable addresses -- e.g. T$ -> 0x7790, A -> 0x7900) into
; SD_VARNAME_HI_ABS/LO_ABS: high byte = first char's ASCII code; low byte
; = second char's ASCII ANDed with 0x1F (0x00 if there's no second
; letter), ORed with 0x20 if a trailing '$' follows. Advances X past
; everything consumed (including a trailing '$', if present). Returns via
; Carry: SET = malformed (doesn't start with a letter A-Z after folding,
; X left at the bad character), CLEAR = success.
SD_PARSE_VARIABLE_NAME:
	sjp SD_PVN_FOLD_CHAR
	bcr SD_PVN_C1_OK
	jmp SD_PVN_FAIL
SD_PVN_C1_OK:
	sta (SD_VARNAME_HI_ABS)
	ldi a,0x00
	sta (SD_VARNAME_LO_ABS)
	inc x

	lda (x)
	cpi a,0x24                  ; '$' right after the first letter?
	bzs SD_PVN_DOLLAR1
	jmp SD_PVN_TRY_C2
SD_PVN_TRY_C2:
	sjp SD_PVN_FOLD_CHAR
	bcr SD_PVN_C2_OK
	jmp SD_PVN_ONE_CHAR
SD_PVN_C2_OK:
	ani a,0x1F
	sta (SD_VARNAME_LO_ABS)
	inc x
	lda (x)
	cpi a,0x24                  ; trailing '$' after a two-letter name?
	bzs SD_PVN_DOLLAR2
	jmp SD_PVN_DONE
SD_PVN_DOLLAR2:
	ori (SD_VARNAME_LO_ABS),0x20
	inc x
	jmp SD_PVN_DONE
SD_PVN_ONE_CHAR:
	jmp SD_PVN_DONE              ; single-letter numeric variable, X already past it
SD_PVN_DOLLAR1:
	ori (SD_VARNAME_LO_ABS),0x20
	inc x
	jmp SD_PVN_DONE
SD_PVN_DONE:
	rec
	rtn
SD_PVN_FAIL:
	sec
	rtn

; Reads (X) (without advancing), folds lowercase a-z to uppercase, and
; validates it's a letter A-Z. Returns the (possibly folded) char in A
; either way; Carry SET = not a letter, CLEAR = success.
SD_PVN_FOLD_CHAR:
	lda (x)
	cpi a,0x61                  ; 'a'
	bcs SD_PVN_FC_MAYBELOWER
	jmp SD_PVN_FC_CHECK
SD_PVN_FC_MAYBELOWER:
	cpi a,0x7B                  ; 'z'+1
	bcr SD_PVN_FC_ISLOWER
	jmp SD_PVN_FC_CHECK
SD_PVN_FC_ISLOWER:
	lda (x)
	sec                          ; SBI borrows from Carry -- SET means a clean subtract
	sbi a,0x20                  ; lowercase -> uppercase
SD_PVN_FC_CHECK:
	cpi a,0x41                  ; 'A'
	bcr SD_PVN_FC_FAIL
	cpi a,0x5B                  ; 'Z'+1
	bcs SD_PVN_FC_FAIL
	rec
	rtn
SD_PVN_FC_FAIL:
	sec
	rtn

; Looks up the variable named by SD_VARNAME_HI/LO_ABS via D461H --
; auto-creates the variable, zeroed, if it doesn't already exist
; (confirmed live this session: an unassigned T$ still returned SUCCESS
; with a fresh, zeroed slot). Sets D461_ARRAY_FLAG_ABS to 0x00 first
; (simple, non-array lookup).
;
; D461H's own calling convention -- CORRECTED 2026-08-19, via direct
; `entertrace` on a live instance, after the ROM_BASE move exposed a real
; bug in the original understanding (below): SJP D461H, immediately
; followed by a one-byte 0xFA marker and ONE more filler byte -- on
; success, execution resumes at (return address)+2, NOT +3. The earlier
; version of this routine reserved 3 bytes (0xFA + a 2-byte embedded
; error-handler address, mimicking the TRM's own "search of program line"
; idiom at D2EAH) and put success-path code at +3; this happened to work
; by coincidence at the ROM's previous 0x9000 base (whatever real
; instruction the CPU decoded starting at +2 there was harmless enough to
; fall through), and broke -- silently returning U=0xFFFF instead of the
; variable's real address -- once ROM_BASE moved to 0x8800 and the byte
; landing at +2 happened to decode as a bare RTN, unwinding the stack
; early. Confirmed via direct trace: SJP at 9184H, return address 9187H,
; execution genuinely resumes at 9189H (9187H+2) with U already holding
; the correct variable address (7900H for "A") -- D461H's own success
; path does not need to read anything from the 2 filler bytes at all.
; The embedded-error-address half of the old design is UNVALIDATED and
; removed here, not just moved -- D461H's actual on-error behavior was
; never independently confirmed (every lookup this project's own tests
; exercise is for a syntactically-valid name, which D461H auto-creates
; rather than failing), so SD_LOOKUP_VARIABLE_ERR below is dead code kept
; only as a documented placeholder, not something proven reachable.
;
; On success: stashes the variable's own address (SD_VAR_ADDR_HI/LO_ABS)
; and type/size byte (SD_VAR_TYPE_ABS, from D461_TYPE_ABS). Returns via
; Carry: CLEAR = success, SET = D461H itself failed (see the dead-code
; note above -- this path is not currently known to be reachable). Does
; NOT preserve X -- see SD_ARG_XSAVE_HI/LO_ABS's own comment; callers
; that still need their own DISP_BUFFER position afterward must
; save/restore it around this call themselves.
SD_LOOKUP_VARIABLE:
	ldi a,0x00
	sta (D461_ARRAY_FLAG_ABS)
	lda (SD_VARNAME_HI_ABS)
	sta uh
	lda (SD_VARNAME_LO_ABS)
	sta ul
	sjp D461_VAR_SEARCH
	.db 0xFA
	.db 0x00                    ; filler -- see this routine's own comment; D461H's
	                             ; confirmed success path resumes at +2, not +3
	lda uh
	sta (SD_VAR_ADDR_HI_ABS)
	lda ul
	sta (SD_VAR_ADDR_LO_ABS)
	lda (D461_TYPE_ABS)
	sta (SD_VAR_TYPE_ABS)
	rec
	rtn
SD_LOOKUP_VARIABLE_ERR:         ; dead code -- see this routine's own comment
	sec
	rtn

; Builds a value chunk (tag + payload -- see PC_EXP.h's own comment for
; the exact format) from the variable whose address/type SD_LOOKUP_
; VARIABLE just left in SD_VAR_ADDR_HI/LO_ABS/SD_VAR_TYPE_ABS, staging it
; at EXP_BUFFER_START_ABS+1 onward (offset 0 is reserved for the caller's
; own channel-number byte -- see EXP_COMMAND_SD_WRITE_VALUE's wire
; format). Numeric (type byte bit7 set): 'N' + 8 raw bytes copied
; verbatim from the variable's own storage (TRM sec.5-3-1 packed-BCD
; float, confirmed live -- e.g. A=1500 -> 03 00 15 00 00 00 00 00). String
; (bit7 clear): 'S' + 1-byte length + that many raw ASCII bytes -- a
; simple string variable's real storage is zero-padded inline ASCII with
; NO separate length field (confirmed live -- e.g. T$="HI" -> 48 49 00...
; -- this contradicts the *different*, transient arithmetic-register
; string format from TRM sec.5-3-3, which is not what a variable's own
; storage actually looks like), so the real length has to be found by
; scanning forward for the first 0x00 byte, capped at the variable's own
; capacity (type byte's low 7 bits). SD_COPY_BYTES always copies >=1 byte
; even for a 0-length request (its own documented limitation), so a
; genuinely empty string is copied via an explicit guard instead of
; calling it with UL=0. Does not preserve X (see SD_LOOKUP_VARIABLE's own
; note -- same caveat applies here).
SD_BUILD_VALUE_CHUNK:
	lda (SD_VAR_ADDR_HI_ABS)
	sta xh
	lda (SD_VAR_ADDR_LO_ABS)
	sta xl
	lda (SD_VAR_TYPE_ABS)
	ani a,0x80
	cpi a,0x00
	bzs SD_BVC_STRING
	jmp SD_BVC_NUMERIC
SD_BVC_NUMERIC:
	ldi a,0x4E                  ; 'N'
	sta (EXP_BUFFER_START_ABS+1)
	ldi yh,>(EXP_BUFFER_START_ABS+2)
	ldi yl,<(EXP_BUFFER_START_ABS+2)
	ldi uh,0x00
	ldi ul,0x08
	sjp SD_COPY_BYTES
	rtn
SD_BVC_STRING:
	lda (SD_VAR_TYPE_ABS)
	ani a,0x7F
	sta (SD_CHUNK_CAP_ABS)
	; scan forward from (X) via Y (a throwaway scan pointer -- X itself
	; must stay put, it's still needed below as the real copy source)
	lda xh
	sta yh
	lda xl
	sta yl
	ldi a,0x00
	sta (SD_CHUNK_LEN_ABS)
SD_BVC_SCAN_LOOP:
	lda (SD_CHUNK_LEN_ABS)
	cpa (SD_CHUNK_CAP_ABS)
	bcr SD_BVC_SCAN_CHECKBYTE     ; LEN < CAP -- keep scanning
	jmp SD_BVC_SCAN_DONE           ; LEN >= CAP -- capacity reached, stop
SD_BVC_SCAN_CHECKBYTE:
	lda (y)
	cpi a,0x00
	bzr SD_BVC_SCAN_ADVANCE
	jmp SD_BVC_SCAN_DONE           ; hit the zero-pad -- this is the real length
SD_BVC_SCAN_ADVANCE:
	inc y
	lda (SD_CHUNK_LEN_ABS)
	inc a
	sta (SD_CHUNK_LEN_ABS)
	jmp SD_BVC_SCAN_LOOP
SD_BVC_SCAN_DONE:
	ldi a,0x53                  ; 'S'
	sta (EXP_BUFFER_START_ABS+1)
	lda (SD_CHUNK_LEN_ABS)
	sta (EXP_BUFFER_START_ABS+2)
	cpi a,0x00
	bzr SD_BVC_STRING_COPY
	jmp SD_BVC_DONE                ; zero-length string -- nothing to copy
SD_BVC_STRING_COPY:
	ldi yh,>(EXP_BUFFER_START_ABS+3)
	ldi yl,<(EXP_BUFFER_START_ABS+3)
	ldi uh,0x00
	lda (SD_CHUNK_LEN_ABS)
	sta ul
	sjp SD_COPY_BYTES
SD_BVC_DONE:
	rtn

; Resets the variable at SD_VAR_ADDR_HI/LO_ABS/SD_VAR_TYPE_ABS to "empty"
; -- 0 for a numeric variable (all 8 storage bytes zeroed, matching a
; freshly auto-created variable's own all-zero state, confirmed live) or
; blank for a string variable (just the first storage byte needs
; zeroing -- a simple string variable's real length is always found by
; scanning for the first 0x00 byte, so a single leading zero byte alone
; is already a fully valid, correctly-blank string regardless of
; whatever's left over in the rest of its capacity). Used by SDINPUT#
; when a requested variable has no more stored values left to read (per
; the user's own spec: excess variables become 0/blank, not an error).
; Does not preserve X.
SD_ZERO_VARIABLE:
	lda (SD_VAR_ADDR_HI_ABS)
	sta xh
	lda (SD_VAR_ADDR_LO_ABS)
	sta xl
	lda (SD_VAR_TYPE_ABS)
	ani a,0x80
	cpi a,0x00
	bzs SD_ZV_STRING
	jmp SD_ZV_NUMERIC
SD_ZV_NUMERIC:
	ldi ul,0x08
SD_ZV_NUM_LOOP:
	ldi a,0x00
	sta (x)
	inc x
	dec ul
	cpi ul,0x00
	bzr SD_ZV_NUM_LOOP
	rtn
SD_ZV_STRING:
	ldi a,0x00
	sta (x)
	rtn

; Consumes a value chunk already staged at EXP_BUFFER_START_ABS (by
; EXP_COMMAND_SD_READ_VALUE's own response -- tag directly at offset 0, NO
; channel-number prefix in the response, unlike the write-side wire
; format), writing it into the variable whose address/type SD_LOOKUP_
; VARIABLE left in SD_VAR_ADDR_HI/LO_ABS/SD_VAR_TYPE_ABS. Raises ERROR 42
; (SD_RAISE_ERROR_42, never returns) if the chunk's own type tag doesn't
; match the target variable's type, or if a string chunk's length exceeds
; the target's real capacity -- both only possible from a corrupted or
; hand-crafted SD file, see SD_RAISE_ERROR_42's own comment. Does not
; preserve X.
SD_CONSUME_VALUE_CHUNK:
	lda (SD_VAR_ADDR_HI_ABS)
	sta yh
	lda (SD_VAR_ADDR_LO_ABS)
	sta yl
	lda (EXP_BUFFER_START_ABS+0)
	cpi a,0x4E                  ; 'N'
	bzs SD_CVC_TAG_NUMERIC
	jmp SD_CVC_TAG_STRING
SD_CVC_TAG_NUMERIC:
	lda (SD_VAR_TYPE_ABS)
	ani a,0x80
	cpi a,0x00
	bzr SD_CVC_NUMERIC_OK
	jmp SD_RAISE_ERROR_42        ; 'N' chunk into a string variable
SD_CVC_NUMERIC_OK:
	ldi xh,>(EXP_BUFFER_START_ABS+1)
	ldi xl,<(EXP_BUFFER_START_ABS+1)
	ldi uh,0x00
	ldi ul,0x08
	sjp SD_COPY_BYTES
	rtn
SD_CVC_TAG_STRING:
	cpi a,0x53                  ; 'S'
	bzs SD_CVC_TAG_STRING_OK
	jmp SD_RAISE_ERROR_42        ; unrecognized tag -- treat as corrupted
SD_CVC_TAG_STRING_OK:
	lda (SD_VAR_TYPE_ABS)
	ani a,0x80
	cpi a,0x00
	bzs SD_CVC_STRING_OK
	jmp SD_RAISE_ERROR_42        ; 'S' chunk into a numeric variable
SD_CVC_STRING_OK:
	lda (EXP_BUFFER_START_ABS+1)
	sta (SD_CHUNK_LEN_ABS)
	lda (SD_VAR_TYPE_ABS)
	ani a,0x7F
	sta (SD_CHUNK_CAP_ABS)
	lda (SD_CHUNK_LEN_ABS)
	cpa (SD_CHUNK_CAP_ABS)
	bcr SD_CVC_LEN_OK             ; LEN < CAP -- fine
	bzs SD_CVC_LEN_OK             ; LEN == CAP -- also fine (CPA sets Z on equality)
	jmp SD_RAISE_ERROR_42          ; LEN > CAP -- real overflow
SD_CVC_LEN_OK:
	lda (SD_CHUNK_LEN_ABS)
	cpi a,0x00
	bzr SD_CVC_STRING_COPY
	jmp SD_CVC_PAD                  ; zero-length string -- nothing to copy, pad from the start
SD_CVC_STRING_COPY:
	ldi xh,>(EXP_BUFFER_START_ABS+2)
	ldi xl,<(EXP_BUFFER_START_ABS+2)
	ldi uh,0x00
	lda (SD_CHUNK_LEN_ABS)
	sta ul
	sjp SD_COPY_BYTES               ; advances Y past exactly what was copied
SD_CVC_PAD:
	lda (SD_CHUNK_CAP_ABS)
	sec
	sbc (SD_CHUNK_LEN_ABS)
	sta (SD_CHUNK_LEN_ABS)          ; reuse as the remaining pad-byte count
	cpi a,0x00
	bzr SD_CVC_PAD_LOOP
	rtn                              ; capacity exactly filled -- no padding needed
SD_CVC_PAD_LOOP:
	ldi a,0x00
	sta (y)
	inc y
	lda (SD_CHUNK_LEN_ABS)
	dec a
	sta (SD_CHUNK_LEN_ABS)
	cpi a,0x00
	bzr SD_CVC_PAD_LOOP
	rtn

; Shared range/store helper for SDOPEN/SDCLOSE/SDSKIP#'s own channel-
; number arguments: validates the 16-bit value SD_PARSE_NUMBER just left
; in SDLOAD_ADDR_HI/LO_ABS is in range 1-16, storing it to SD_CHANNEL_ABS
; on success. Returns via Carry: SET = out of range, CLEAR = success.
SD_VALIDATE_CHANNEL_RANGE:
	lda (SDLOAD_ADDR_HI_ABS)
	cpi a,0x00
	bzs SD_VCR_LO_OK
	jmp SD_VCR_FAIL
SD_VCR_LO_OK:
	lda (SDLOAD_ADDR_LO_ABS)
	cpi a,0x01
	bcs SD_VCR_HI_CHECK
	jmp SD_VCR_FAIL
SD_VCR_HI_CHECK:
	cpi a,17                    ; EXP_MAX_SD_CHANNELS+1
	bcr SD_VCR_OK
	jmp SD_VCR_FAIL
SD_VCR_OK:
	sta (SD_CHANNEL_ABS)
	rec
	rtn
SD_VCR_FAIL:
	sec
	rtn

; Shared by SDINPUT#/SDPRINT#/SDSKIP#'s own argument parsers: skips an
; optional leading '#' (purely cosmetic, matching real PRINT#/INPUT#'s
; own visual convention -- see SDINPUT_ROUTINE's comment for why the
; keyword table entries themselves are named without '#'), then parses a
; decimal channel number and validates it via SD_VALIDATE_CHANNEL_RANGE.
; Advances X past everything consumed. Returns via Carry: SET = malformed
; or out of range, CLEAR = success.
SD_PARSE_CHANNEL_ARG:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x23                  ; '#'
	bzr SD_PCA_NUM
	inc x
SD_PCA_NUM:
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcr SD_PCA_VALIDATE
	jmp SD_PCA_FAIL
SD_PCA_VALIDATE:
	sjp SD_VALIDATE_CHANNEL_RANGE
	bcr SD_PCA_OK
	jmp SD_PCA_FAIL
SD_PCA_OK:
	rec
	rtn
SD_PCA_FAIL:
	sec
	rtn

; Same as SD_LIST_INIT but triggers EXP_COMMAND_SD_LIST_CHANNELS instead
; of LIST_SD_DIR -- everything past the initial dispatch (poll, index
; setup, drawing the first entry) is identical, since SD_LIST_CHANNELS
; reuses LIST_SD_DIR's own wire format exactly (see PC_EXP.h), so
; SD_LIST_DISPLAY/SD_LIST_UP/SD_LIST_DOWN (all format-agnostic -- they
; just walk EXP_DIR_RECORD_SIZE-byte records wherever SD_LIST_ADDR_HI/
; LO_ABS points) are reused verbatim by SDOPEN's own bare-argument browse.
SD_CHANNEL_LIST_INIT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0

	ldi a,EXP_COMMAND_SD_LIST_CHANNELS
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY

	lda (EXP_BUFFER_START_ABS+1)
	sta (SD_LIST_COUNT_ABS)
	ldi a,0x00
	sta (SD_LIST_INDEX_ABS)
	ldi a,>(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_HI_ABS)
	ldi a,<(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_LO_ABS)
	jmp SD_LIST_DISPLAY          ; NOT sjp -- SD_LIST_DISPLAY's own RTN pops the
	                              ; return address our own caller's SJP pushed,
	                              ; same effect as SD_LIST_INIT's own fall-through

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
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_CREATE_AND_WRITE_RESTORE  ; doesn't exist -- no confirmation needed

	ldi a,EXP_COMMAND_CLOSE_SD_FILE  ; exists -- close the probe-open first
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
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
	sjp EC_WAIT_NOT_BUSY
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
	; Length lives at EXP_LENGTH_PORT_ABS now, outside the payload -- the
	; header's 4 bytes are the full, un-prefixed EXP_BUFFER_START_ABS+0..+3
	; (was +2..+5).
	ldi a,0x00
	sta (EXP_LENGTH_PORT_ABS+0)
	ldi a,0x04
	sta (EXP_LENGTH_PORT_ABS+1)
	lda (SDSAVE_START_HI_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDSAVE_START_LO_ABS)
	sta (EXP_BUFFER_START_ABS+1)
	lda (SDSAVE_CALL_HI_ABS)
	sta (EXP_BUFFER_START_ABS+2)
	lda (SDSAVE_CALL_LO_ABS)
	sta (EXP_BUFFER_START_ABS+3)
	ldi a,EXP_COMMAND_WRITE_TO_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
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
	sjp EC_WAIT_NOT_BUSY
	jmp KEYWORD_RETURN

; Writes the inclusive byte range [SDSAVE_WRITE_HI/LO_ABS,
; SDSAVE_RANGE_END_HI/LO_ABS] from RAM to the currently-open SD file,
; chunked at <=EXP_MAX_TRANSFER_LEN bytes per EXP_COMMAND_WRITE_TO_SD_FILE
; call (matching SD_OPEN_AND_LOAD's own read-side chunk size -- length lives
; at EXP_LENGTH_PORT_ABS, outside the payload). Advances
; SDSAVE_WRITE_HI/LO_ABS as it goes. On a WRITE_TO_SD_FILE failure
; partway through, stops immediately (matching SD_OPEN_AND_LOAD_READ_
; LOOP's own "stop, let the caller close/finalize with what we have"
; precedent -- no separate error signaling back to the caller).
; Chunk-length tracking uses a 16-bit up-counter with manual carry
; propagation (increment low byte; if it wraps to 0x00, increment high byte
; too) rather than a register-pair countdown, since there's no confirmed
; LH5801 16-bit-subtract available to recover "bytes copied" cleanly from a
; countdown once the loop is done -- this is a direct, mechanical widening
; of the original single-byte accumulator, same shape, just carried across
; two bytes.
SD_WRITE_RANGE:
SD_WRITE_RANGE_CHUNK:
	ldi yh,>EXP_BUFFER_START_ABS  ; destination is now the full, un-prefixed window --
	ldi yl,<EXP_BUFFER_START_ABS  ; no +2 offset, length no longer lives in-band
	lda (SDSAVE_WRITE_HI_ABS)
	sta xh
	lda (SDSAVE_WRITE_LO_ABS)
	sta xl
	ldi a,0x00
	sta (SDSAVE_CHUNKLEN_HI_ABS)
	sta (SDSAVE_CHUNKLEN_LO_ABS)
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
	lda (SDSAVE_CHUNKLEN_LO_ABS)  ; inc a, not adi a,0x01 -- ADI incorporates any leftover carry
	inc a                         ; from the cpa comparisons above, which would silently add 2
	sta (SDSAVE_CHUNKLEN_LO_ABS)
	cpi a,0x00                   ; wrapped 0xFF->0x00? bump the high byte too
	bzr SD_WRITE_RANGE_CHECK_FULL
	lda (SDSAVE_CHUNKLEN_HI_ABS)
	inc a
	sta (SDSAVE_CHUNKLEN_HI_ABS)
SD_WRITE_RANGE_CHECK_FULL:
	lda (SDSAVE_CHUNKLEN_HI_ABS)
	cpi a,>EXP_MAX_TRANSFER_LEN
	bzr SD_WRITE_RANGE_COPY_LOOP   ; high byte not yet at target -- keep going
	lda (SDSAVE_CHUNKLEN_LO_ABS)
	cpi a,<EXP_MAX_TRANSFER_LEN
	bzr SD_WRITE_RANGE_COPY_LOOP   ; low byte not yet at target -- keep going
	bch SD_WRITE_RANGE_FLUSH       ; both matched -- chunk is exactly EXP_MAX_TRANSFER_LEN full
SD_WRITE_RANGE_AT_END:
	tin                          ; copy the final byte
	lda (SDSAVE_CHUNKLEN_LO_ABS)
	inc a
	sta (SDSAVE_CHUNKLEN_LO_ABS)
	cpi a,0x00
	bzr SD_WRITE_RANGE_AT_END_DONE
	lda (SDSAVE_CHUNKLEN_HI_ABS)
	inc a
	sta (SDSAVE_CHUNKLEN_HI_ABS)
SD_WRITE_RANGE_AT_END_DONE:
	ldi a,0x01
	sta (SDSAVE_RANGE_DONE_ABS)

SD_WRITE_RANGE_FLUSH:
	lda (SDSAVE_CHUNKLEN_HI_ABS)
	sta (EXP_LENGTH_PORT_ABS+0)
	lda (SDSAVE_CHUNKLEN_LO_ABS)
	sta (EXP_LENGTH_PORT_ABS+1)
	ldi a,EXP_COMMAND_WRITE_TO_SD_FILE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
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

; SDDF -- prints free/total SD space. The ROM has no binary-to-decimal-
; ASCII conversion of its own (see EXP_COMMAND_LIST_SD_DIR's own comment
; on this, PC_EXP.h), so EXP_COMMAND_GET_SD_DF_TEXT returns an already-
; formatted string (like GET_SD_CWD's response) instead of the raw 4-byte
; values GET_SD_FREE_SPACE/GET_SD_VOLUME_SIZE return -- this routine is
; otherwise identical in shape to SDPWD_ROUTINE.
SDDF_ROUTINE:
	ldi a,EXP_COMMAND_GET_SD_DF_TEXT
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDDF_GOT_TEXT
	jmp KEYWORD_RETURN             ; failed (no card) -- silent abort, matching SDPWD's own convention
SDDF_GOT_TEXT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	lda (EXP_SCRATCH_ABS)          ; the MCU's own length-prefix byte
	cpi a,SD_LIST_LINE_WIDTH
	bcr SDDF_LEN_OK                ; a < SD_LIST_LINE_WIDTH -- use as-is
	ldi a,SD_LIST_LINE_WIDTH       ; clamp to the display width
SDDF_LEN_OK:
	sta xl
	ldi uh,>(EXP_SCRATCH_ABS+1)
	ldi ul,<(EXP_SCRATCH_ABS+1)
	sjp DISP_N_CHARS0
	; Block on KEYSCAN_WAIT before returning -- same fix as ECVER's own
	; (see that routine's header comment, 2026-09-17): KEYWORD_RETURN
	; unconditionally redraws the idle ">" prompt over the whole line,
	; so jmp KEYWORD_RETURN directly after a DISP_N_CHARS0 immediately
	; overwrites whatever was just drawn. First suspected a
	; BLINK_CURSOR_H/L race here (2026-09-19, wrong guess, reverted) --
	; confirmed live the real cause is this, the exact same pattern
	; ECVER already had fixed. Any keypress dismisses, same as ECVER.
	sjp KEYSCAN_WAIT
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
; SDOPEN "<filename>" AS <n> -- opens (creating if necessary) a file on
; channel <n> (1-16); reusing an already-open channel number closes its
; previous file first (EXP_COMMAND_SD_OPEN_CHANNEL's own documented
; behavior -- see PC_EXP.h). Bare SDOPEN (no arguments) lists currently
; open channels and their numbers, reusing the same SD_LIST_UP/DOWN/
; DISPLAY browse machinery SDLS already uses via SD_CHANNEL_LIST_INIT --
; view only, no L-select (nothing meaningful to "select" from a channel
; listing); Enter, CL, or BREAK all exit, matching SDLS's own convention.
SDOPEN_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x0D
	bzs SDOPEN_LIST
	jmp SDOPEN_HAVE_ARG

SDOPEN_LIST:
	sjp SD_CHANNEL_LIST_INIT
SDOPEN_WAITKEY:
	sjp KEYSCAN_WAIT
	bcs SDOPEN_LIST_EXIT          ; BREAK
	cpi a,KEY_CL
	bzs SDOPEN_LIST_EXIT
	cpi a,KEY_ENTER
	bzs SDOPEN_LIST_EXIT
	cpi a,KEY_UP
	bzs SDOPEN_DO_UP
	cpi a,KEY_DOWN
	bzs SDOPEN_DO_DOWN
	bch SDOPEN_WAITKEY
SDOPEN_DO_UP:
	sjp SD_LIST_UP
	sjp SD_LIST_DISPLAY
	bch SDOPEN_WAITKEY
SDOPEN_DO_DOWN:
	sjp SD_LIST_DOWN
	sjp SD_LIST_DISPLAY
	bch SDOPEN_WAITKEY
SDOPEN_LIST_EXIT:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	jmp SDOPEN_ABORT

SDOPEN_HAVE_ARG:
	lda (x)
	cpi a,0x22                   ; '"'
	bzs SDOPEN_HAVE_QUOTE
	jmp SD_RAISE_ERROR_1          ; SDOPEN with an argument always needs "<filename>" AS <n>
SDOPEN_HAVE_QUOTE:
	sjp SD_PARSE_QUOTED_NAME      ; stages a length-prefixed filename at EXP_BUFFER_START_ABS+0..
	bcr SDOPEN_NAME_OK
	jmp SD_RAISE_ERROR_1
SDOPEN_NAME_OK:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x41                   ; 'A'
	bzs SDOPEN_CHECK_S
	jmp SD_RAISE_ERROR_1          ; expected "AS"
SDOPEN_CHECK_S:
	inc x
	lda (x)
	cpi a,0x53                   ; 'S'
	bzs SDOPEN_AS_OK
	jmp SD_RAISE_ERROR_1
SDOPEN_AS_OK:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcr SDOPEN_NUM_OK
	jmp SD_RAISE_ERROR_1
SDOPEN_NUM_OK:
	sjp SD_VALIDATE_CHANNEL_RANGE
	bcr SDOPEN_RANGE_OK
	jmp SD_RAISE_ERROR_1

SDOPEN_RANGE_OK:
	; Relocate the name SD_PARSE_QUOTED_NAME already staged at
	; EXP_BUFFER_START_ABS+0(len-hi)/+1(len-lo)/+2..(chars) into
	; EXP_COMMAND_SD_OPEN_CHANNEL's own wire shape: channel byte at +0,
	; then the same 2-byte-BE length + chars shifted right one byte, into
	; +1.. -- via SD_TWONAME_STASH_ABS, same fixed-width relocation trick
	; SD_PARSE_TWO_QUOTED_NAMES already uses (safe to reuse: only one
	; keyword ever dispatches at a time).
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,>SD_TWONAME_STASH_ABS
	ldi yl,<SD_TWONAME_STASH_ABS
	ldi uh,0x00
	ldi ul,EXP_TWO_NAME_SLOT_LEN
	sjp SD_COPY_BYTES

	lda (SD_CHANNEL_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	ldi xh,>SD_TWONAME_STASH_ABS
	ldi xl,<SD_TWONAME_STASH_ABS
	ldi yh,>(EXP_BUFFER_START_ABS+1)
	ldi yl,<(EXP_BUFFER_START_ABS+1)
	ldi uh,0x00
	ldi ul,EXP_TWO_NAME_SLOT_LEN
	sjp SD_COPY_BYTES

	ldi a,EXP_COMMAND_SD_OPEN_CHANNEL
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDOPEN_DONE
	jmp SD_RAISE_ERROR_40
SDOPEN_DONE:
SDOPEN_ABORT:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDCLOSE <n|ALL> -- closes channel <n> (1-16), or every open channel if
; the literal "ALL" is given (case-sensitive, matching -Y's own exact-case
; convention elsewhere in this file). A missing/malformed argument raises
; ERROR 1. Closing an already-closed channel number, or ALL when nothing
; is open, is a silent no-op (EXP_COMMAND_SD_CLOSE_CHANNEL's own main.c/
; ExpansionMock side handles both cases without a real filesystem error).
SDCLOSE_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x41                    ; 'A' -- possible start of "ALL"
	bzs SDCLOSE_TRY_ALL
	jmp SDCLOSE_TRY_NUMBER
SDCLOSE_TRY_ALL:
	inc x
	lda (x)
	cpi a,0x4C                    ; 'L'
	bzs SDCLOSE_ALL_L1
	jmp SD_RAISE_ERROR_1
SDCLOSE_ALL_L1:
	inc x
	lda (x)
	cpi a,0x4C                    ; 'L'
	bzs SDCLOSE_ALL_L2
	jmp SD_RAISE_ERROR_1
SDCLOSE_ALL_L2:
	inc x
	ldi a,0x00
	sta (SD_CHANNEL_ABS)          ; 0 = the ALL sentinel
	jmp SDCLOSE_DISPATCH
SDCLOSE_TRY_NUMBER:
	sjp SD_PARSE_NUMBER
	bcr SDCLOSE_NUM_OK
	jmp SD_RAISE_ERROR_1
SDCLOSE_NUM_OK:
	sjp SD_VALIDATE_CHANNEL_RANGE
	bcr SDCLOSE_DISPATCH
	jmp SD_RAISE_ERROR_1
SDCLOSE_DISPATCH:
	lda (SD_CHANNEL_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	ldi a,EXP_COMMAND_SD_CLOSE_CHANNEL
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY            ; status not checked further -- see this section's own comment
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; SDINPUT#/SDPRINT#/SDSKIP# -- table names are "SDINPUT"/"SDPRINT"/
; "SDSKIP", deliberately WITHOUT a literal '#': the base ROM's real
; PRINT#/INPUT# are themselves just PRINT/INPUT (genuine table entries)
; followed by a bare '#' as untokenized trailing text (confirmed via the
; Owner's Manual -- there's no file-handle mechanism behind them at all,
; just a variable list), not a '#' baked into any table entry's own name
; -- no confirmed precedent exists here for a table name containing '#',
; so rather than risk it, these three follow the same, proven-safe shape:
; each routine's own argument parser (SD_PARSE_CHANNEL_ARG) just skips a
; leading '#' as an optional, purely cosmetic character, exactly like a
; real PRINT#/INPUT#'s own trailing '#'. "SDINPUT#1,A,B$" and
; "SDINPUT #1,A,B$" both work identically either way.

; SDINPUT# <n>,<var>[,<var>...] -- reads one value per listed variable
; from channel <n>'s own persistent read position, advancing past each
; one read; if the channel runs out, every remaining requested variable
; is set to 0 (numeric) or blank (string) instead of raising an error --
; the user's own explicit spec.
SDINPUT_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_PARSE_CHANNEL_ARG
	bcr SDINPUT_VAR_LOOP
	jmp SD_RAISE_ERROR_1

SDINPUT_VAR_LOOP:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C                    ; ','
	bzs SDINPUT_NEXT_VAR
	jmp SD_RAISE_ERROR_1           ; every variable must be introduced by ','
SDINPUT_NEXT_VAR:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_VARIABLE_NAME
	bcr SDINPUT_VARNAME_OK
	jmp SD_RAISE_ERROR_1
SDINPUT_VARNAME_OK:
	lda xh
	sta (SD_ARG_XSAVE_HI_ABS)
	lda xl
	sta (SD_ARG_XSAVE_LO_ABS)

	sjp SD_LOOKUP_VARIABLE
	bcr SDINPUT_LOOKUP_OK
	jmp SD_RAISE_ERROR_1
SDINPUT_LOOKUP_OK:
	lda (SD_CHANNEL_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	ldi a,EXP_COMMAND_SD_READ_VALUE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_EOF
	bzs SDINPUT_ZERO_VAR
	jmp SDINPUT_CHECK_SUCCESS
SDINPUT_CHECK_SUCCESS:
	cpi a,EXP_STATUS_SUCCESS
	bzs SDINPUT_CONSUME
	jmp SD_RAISE_ERROR_40           ; channel not open, or some other failure -- matches
	                                 ; SDPRINT#/SDSKIP#'s own convention for this same
	                                 ; class of failure (the wire protocol can't tell "not
	                                 ; open" apart from a genuine I/O failure, so neither
	                                 ; can this ROM -- both get the same ERROR 40 every
	                                 ; other SD-operation-failed case already uses,
	                                 ; distinct from ERROR 1's "malformed syntax" meaning)
SDINPUT_CONSUME:
	sjp SD_CONSUME_VALUE_CHUNK      ; never returns on a real ERROR-42 mismatch
	jmp SDINPUT_RESTORE_X
SDINPUT_ZERO_VAR:
	sjp SD_ZERO_VARIABLE
SDINPUT_RESTORE_X:
	lda (SD_ARG_XSAVE_HI_ABS)
	sta xh
	lda (SD_ARG_XSAVE_LO_ABS)
	sta xl
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C
	bzs SDINPUT_VAR_LOOP
	jmp KEYWORD_RETURN

; SDPRINT# <n>,<var>[,<var>...] -- appends one value per listed variable
; to the end of channel <n>'s file (SD_WRITE_VALUE always appends,
; regardless of the channel's own read position -- see PC_EXP.h).
SDPRINT_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_PARSE_CHANNEL_ARG
	bcr SDPRINT_VAR_LOOP
	jmp SD_RAISE_ERROR_1

SDPRINT_VAR_LOOP:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C                    ; ','
	bzs SDPRINT_NEXT_VAR
	jmp SD_RAISE_ERROR_1
SDPRINT_NEXT_VAR:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_VARIABLE_NAME
	bcr SDPRINT_VARNAME_OK
	jmp SD_RAISE_ERROR_1
SDPRINT_VARNAME_OK:
	lda xh
	sta (SD_ARG_XSAVE_HI_ABS)
	lda xl
	sta (SD_ARG_XSAVE_LO_ABS)

	sjp SD_LOOKUP_VARIABLE
	bcr SDPRINT_LOOKUP_OK
	jmp SD_RAISE_ERROR_1
SDPRINT_LOOKUP_OK:
	lda (SD_CHANNEL_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	sjp SD_BUILD_VALUE_CHUNK
	ldi a,EXP_COMMAND_SD_WRITE_VALUE
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDPRINT_RESTORE_X
	jmp SD_RAISE_ERROR_40
SDPRINT_RESTORE_X:
	lda (SD_ARG_XSAVE_HI_ABS)
	sta xh
	lda (SD_ARG_XSAVE_LO_ABS)
	sta xl
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C
	bzs SDPRINT_VAR_LOOP
	jmp KEYWORD_RETURN

; SDSKIP#<n>,<count> -- skips forward <count> values in channel <n>'s own
; persistent read position (SDINPUT#'s own cursor -- writes are
; unaffected). All-or-nothing: EXP_COMMAND_SD_SKIP_VALUES itself leaves
; the channel's read position completely untouched if it runs out partway
; through (see PC_EXP.h), so a partial-skip failure here raises a genuine
; ERROR 40, per the user's own explicit spec ("if you attempt to skip
; past the end of the file, an ERROR 40 should be thrown"). The comma
; between <n> and <count> is REQUIRED, not optional, despite the user's
; own spec literally showing a bare space ("SKIP# <number> <variables to
; skip>") -- confirmed live this session that BASIC's own line editor
; does not reliably preserve a bare space between two digit runs outside
; a quoted string (typing "SDSKIP#1 2" landed in DISP_BUFFER as "#12", the
; space silently dropped, which then parsed as channel 12 with a missing
; count and raised ERROR 1) -- exactly the same reason every other multi-
; value argument in this file (SDCP/SDMV's two names, SDLOAD M's address)
; already uses a comma, never a bare space, as its real separator.
SDSKIP_ROUTINE:
	ldi xh,>(DISP_BUFFER_ABS+2)
	ldi xl,<(DISP_BUFFER_ABS+2)
	sjp SD_PARSE_CHANNEL_ARG
	bcr SDSKIP_COUNT_SEP
	jmp SD_RAISE_ERROR_1

SDSKIP_COUNT_SEP:
	sjp SD_SKIP_SPACES
	lda (x)
	cpi a,0x2C                    ; ',' -- required, see this routine's own comment
	bzs SDSKIP_COUNT_START
	jmp SD_RAISE_ERROR_1
SDSKIP_COUNT_START:
	inc x
	sjp SD_SKIP_SPACES
	sjp SD_PARSE_NUMBER
	bcr SDSKIP_COUNT_OK
	jmp SD_RAISE_ERROR_1
SDSKIP_COUNT_OK:
	lda (SD_CHANNEL_ABS)
	sta (EXP_BUFFER_START_ABS+0)
	lda (SDLOAD_ADDR_HI_ABS)
	sta (EXP_BUFFER_START_ABS+1)
	lda (SDLOAD_ADDR_LO_ABS)
	sta (EXP_BUFFER_START_ABS+2)
	ldi a,EXP_COMMAND_SD_SKIP_VALUES
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAIT_NOT_BUSY
	cpi a,EXP_STATUS_SUCCESS
	bzs SDSKIP_DONE
	jmp SD_RAISE_ERROR_40
SDSKIP_DONE:
	jmp KEYWORD_RETURN

; ---------------------------------------------------------------------
; ROM-to-SRAM bootstrap copy, called from BOOT_SELFCHECK_ENTRY. Copies the
; 6K ROM image from the MCU into the real SRAM chip's upper 6K, since only
; the LH5801 can drive the address bus to write there -- the MCU can't do
; this copy itself. See PC_EXP.h's own EXP_COMMAND_ROM_COPY_* comment for
; the full protocol and monitor.c's DoCommand() for the MCU side.
;
; Two parts, deliberately separate:
;   ROM_COPY_TRAMPOLINE -- runs at its normal ROM_BASE+ address (fine, this
;     part only executes BEFORE EXP_COMMAND_ROM_COPY_BEGIN is issued, so
;     the ROM/SRAM-serving flip-flop hasn't switched yet -- reads from this
;     address range are still safely answered by the MCU's own bit-banged
;     buffer). Its only job is to relocate ROM_COPY_RELOCATABLE_START..END
;     into the data window and jump there.
;   ROM_COPY_RELOCATABLE_START..END -- the actual copy loop. MUST run from
;     inside the window (0x8000-0x87FF, always MCU-answered regardless of
;     the ROM/SRAM flip-flop), never from its own native ROM_BASE+ address:
;     once EXP_COMMAND_ROM_COPY_BEGIN switches that flip-flop to SRAM, any
;     further instruction fetch from 0x8800-0x9FFF would be answered by the
;     still-largely-uninitialized SRAM instead of this code, corrupting
;     execution mid-loop. Only relative branches are used for control flow
;     within this block (position-independent after relocation, unlike an
;     absolute jump/call to a label inside the same block, which would
;     still point at the old ROM_BASE+ address post-copy -- there are none
;     here, deliberately).
;
; ROM_COPY_STAGE_ABS is 0x8400 (window offset 0x400), NOT the payload
; region (0x8000-0x83FF): EXP_COMMAND_ROM_COPY_GET_BLOCK overwrites that
; region with fresh block data every iteration, and this code has to keep
; running throughout -- staging itself into the same bytes GET_BLOCK writes
; to would self-overwrite mid-loop. 0x8400-0x86FF (768 bytes, otherwise
; entirely unused) is comfortably more than this routine needs.
ROM_COPY_STAGE_ABS .equ (EXP_BUFFER_START_ABS + 0x400)

ROM_COPY_TRAMPOLINE:
	ldi xh,>ROM_COPY_RELOCATABLE_START
	ldi xl,<ROM_COPY_RELOCATABLE_START
	ldi yh,>ROM_COPY_STAGE_ABS
	ldi yl,<ROM_COPY_STAGE_ABS
	ldi uh,>ROM_COPY_RELOCATABLE_SIZE
	ldi ul,<ROM_COPY_RELOCATABLE_SIZE
ROM_COPY_STAGE_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr ROM_COPY_STAGE_LOOP
	cpi ul,0x00
	bzr ROM_COPY_STAGE_LOOP
	jmp (ROM_COPY_STAGE_ABS)     ; absolute jump to a FIXED window address --
	                             ; correct regardless of where this trampoline
	                             ; itself executes from (it's not relocated)

; Not yet resolvable without the real GreenPAK1 register sequence (same
; open dependency as monitor.c's EXP_COMMAND_ROM_COPY_* comment): this
; always attempts the full copy unconditionally on every self-check, rather
; than first checking whether SRAM is already serving ROM from a prior
; warm-boot and skipping if so, per the original design description. Safe
; either way (copying identical data again is idempotent), just not
; skip-if-already-done yet.
; This trampoline's three poll loops (BEGIN/BLOCK/FINISH) deliberately do
; NOT use EC_WAIT_NOT_BUSY (2026-09-17) even though it replaced the exact
; same tight lda/cpi/bzs pattern everywhere else in this ROM -- this runs
; during the boot-time ROM copy, before there's any way to confirm the
; CPU's own periodic timer interrupt is already running normally at this
; point in the reset sequence. HLT-ing here on an assumption that turned
; out wrong would mean the CPU never wakes up at all, on every single
; boot -- far worse than the tight loop's own real but bounded cost
; (spinning for however long the copy takes, once, at startup). Revisit
; only with real confirmation of interrupt state this early in boot, not
; another assumption.
ROM_COPY_RELOCATABLE_START:
	ldi a,EXP_COMMAND_ROM_COPY_BEGIN
	sta (EXP_INSTRUCTION_ABS)
ROM_COPY_BEGIN_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs ROM_COPY_BEGIN_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzr ROM_COPY_DONE            ; couldn't begin -- bail, nothing written yet

	ldi yh,>ROM_BASE             ; Y = running SRAM write pointer, starts at
	ldi yl,<ROM_BASE             ; ROM_BASE (0x8800) -- see this section's own
	                             ; header comment: safe to write here ONLY
	                             ; because EXP_COMMAND_ROM_COPY_BEGIN has
	                             ; already switched the ROM/SRAM-serving
	                             ; flip-flop, unlike the general "never poke
	                             ; 8800H+ at runtime" rule elsewhere in this
	                             ; file (that rule describes the DEFAULT
	                             ; ROM_FROM_MCU state, not this one)
ROM_COPY_BLOCK_LOOP:
	ldi a,EXP_COMMAND_ROM_COPY_GET_BLOCK
	sta (EXP_INSTRUCTION_ABS)
ROM_COPY_BLOCK_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs ROM_COPY_BLOCK_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzr ROM_COPY_DONE            ; GET_BLOCK failed -- stop, SRAM partially
	                             ; written; unexpected, no recovery attempted

	ldi xh,>EXP_BUFFER_START_ABS  ; X = window payload (source) -- freshly
	ldi xl,<EXP_BUFFER_START_ABS  ; staged block, always exactly
	ldi uh,>EXP_MAX_TRANSFER_LEN  ; EXP_MAX_TRANSFER_LEN bytes (6144/1024=6
	ldi ul,<EXP_MAX_TRANSFER_LEN  ; exact blocks, no partial final block)
ROM_COPY_COPY_LOOP:
	tin
	dec u
	cpi uh,0x00
	bzr ROM_COPY_COPY_LOOP
	cpi ul,0x00
	bzr ROM_COPY_COPY_LOOP

	lda yh                       ; one block done -- Y now points just past
	cpi a,>ROM_REGION_END        ; it; all 6K copied once Y reaches
	bzr ROM_COPY_BLOCK_LOOP       ; ROM_REGION_END (0xA000)
	lda yl
	cpi a,<ROM_REGION_END
	bzr ROM_COPY_BLOCK_LOOP

	ldi a,EXP_COMMAND_ROM_COPY_FINISH
	sta (EXP_INSTRUCTION_ABS)
ROM_COPY_FINISH_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs ROM_COPY_FINISH_POLL

ROM_COPY_DONE:
	rtn                          ; unwinds to BOOT_SELFCHECK_ENTRY's own
	                             ; caller -- neither this routine nor
	                             ; ROM_COPY_TRAMPOLINE push anything onto
	                             ; the stack, so the return address the
	                             ; caller originally pushed is still on top
ROM_COPY_RELOCATABLE_END:
ROM_COPY_RELOCATABLE_SIZE .equ (ROM_COPY_RELOCATABLE_END - ROM_COPY_RELOCATABLE_START)

; ---------------------------------------------------------------------
; Guard: everything above must fit in the 6K ROM region (0x8800-0x9FFF).
; .org can only move the location counter forward within one absolute area
; -- if the content above already overran past 0xA000, this line itself
; fails to assemble instead of silently wrapping past the window.
	.org ROM_REGION_END
