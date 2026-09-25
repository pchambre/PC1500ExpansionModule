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
; A custom keyword's own trailing typed text sits in DISP_BUFFER (7BB0H)
; right after the keyword's own 2-byte code, up to the 0DH terminator --
; no interpreter-level expression evaluation happens for it (confirmed live
; via entertrace on a running pc1500emu). It is NOT raw ASCII, though:
; BASIC still tokenizes its own keywords in it outside quotes, and drops
; the space after the keyword ("MCONF SLEEPWAIT=1" arrives as "SLEEP", the
; WAIT token F1B3, "=1" -- confirmed in pc1500emu 2026-09-25); the MCU
; expands those back to text before parsing. Since 2026-09-25 every table entry except ECVER points at KW_START, which
; hands that whole buffer to the MCU: the MCU parses the arguments and
; sequences the commands (RP2350/keywords.c), and this ROM only carries
; out the display/keyboard/RAM/BASIC-variable actions it gets back -- see
; KW_START.

	.area CODE (ABS)
	.include "rom_defs.inc"

; ---------------------------------------------------------------------
; STAGE keyword's own ROM-to-SRAM copy routine (2026-09) -- permanently
; resident here at a fixed address in the data window, not relocated into
; user RAM at runtime (which could collide with a running program's own
; variables/arrays -- board owner's own design choice).
;
; STAGE_COPY_ROUTINE_ABS = 0x8400: above the 1024-byte payload window
; (0x8000-0x83FF), which every GET_BLOCK overwrites.
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
; SAFETY RULE: from the instant EXP_COMMAND_
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

; STAGE DEBUG's own copy loop (2026-09-21). Writes each source byte to SRAM, then
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
	; used by the old LH5801 number parser). Separate pass from the tin-based copy above since
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

	lda (STAGE_BOOT_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_DONE_BASIC
	ldi a,0x00                      ; boot hook (STAGE_BOOT_ENTRY): no SIE --
	sta (STAGE_BOOT_FLAG)           ; the reset path runs every module hook
	rtn                             ; with interrupts off -- no display, no
	                                ; key wait; RTN back to STAGE_BOOT_ENTRY,
	                                ; which restores U/Y for the scan loop
STAGE_COPY_DONE_BASIC:
	sie                             ; re-enable interrupts before returning
	                                ; to BASIC -- see STAGE_COPY_BEGIN_OK's
	                                ; own comment for why they were off
	jmp STAGE_SHOW_OK               ; safe now -- FINISH succeeded, so
	                                ; ROM_BASE+ mirrors buffer[8..31]

STAGE_COPY_BEGIN_FAILED:
	lda (STAGE_BOOT_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_BEGIN_FAILED_BASIC
	jmp STAGE_COPY_DO_REVERT       ; boot hook: no BASIC error to raise --
	                               ; force MCU-served ROM (in case BEGIN
	                               ; set some of its bits before failing)
	                               ; and return quietly; MCU already logged it
STAGE_COPY_BEGIN_FAILED_BASIC:
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
	                                 ; STAGE_BUILD_MISMATCH_MSG's own
	                                 ; calls, 4x per report -- found live: this alone
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
	ani a,0x0F                      ; A = low nibble
	sjp HEX_NIBBLE_TO_ASCII
	sta (x)
	inc x
	rtn

; Converts the nibble in A (0-15, bits 7-4 must be 0) to its ASCII hex
; digit ('0'-'9','A'-'F'), in A. cpi/bcr "less than" convention: bcr
; branches when A < the immediate.
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
	lda (STAGE_BOOT_FLAG)            ; (A isn't live here -- U/XL hold the
	cpi a,0x00                       ; message for DISP_N_CHARS0 below)
	bzr STAGE_COPY_DO_REVERT         ; boot hook: no SIE, no display --
	                                 ; just revert and return quietly
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
	lda (STAGE_BOOT_FLAG)
	cpi a,0x00
	bzs STAGE_COPY_REVERT_DONE_BASIC
	ldi a,0x00                       ; boot hook -- back to STAGE_BOOT_ENTRY,
	sta (STAGE_BOOT_FLAG)            ; booting on MCU-served ROM
	rtn
STAGE_COPY_REVERT_DONE_BASIC:
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
; Deliberately does NOT jump through the shared KEYWORD_RETURN or draw its
; own ">" prompt -- both live in
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
STAGE_BOOT_FLAG: .db 0x00     ; 1=entered from the boot hook via
                               ; STAGE_BOOT_ENTRY: every exit is a plain RTN
                               ; with no SIE/display/key wait/BASIC error;
                               ; cleared again on each of those exits
STAGE_DEBUG_SRC: .db 0x00     ; per-byte verify scratch -- stashed source
                               ; byte, compared against SRAM's own readback
STAGE_DEBUG_FOUND: .db 0x00   ; per-byte verify scratch -- the mismatching
                               ; readback value, stashed for the mismatch report
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
; Budget: this routine plus ROM_RESET_REMAP must end below the keyword
; action block at 0x87E0 (EXP_KW_ACTION_ABS) -- check rom.rst after any
; change here. The longest listings run over this area; the MCU restores it
; from the ROM image at the start of every keyword (RestoreWindowCode()),
; so STAGE always finds it intact.

; ---------------------------------------------------------------------
; ROM_RESET_REMAP -- standalone recovery entry point (2026-09-22, board
; owner's own request). CALL this address directly, independent of any
; STAGE success or failure path, to force GreenPAK1/2's SRAM/ROM
; Remap back to ROM_FROM_MCU after a failed or hung STAGE leaves
; ROM_BASE+ stuck answering with raw SRAM content instead of the real
; keyword table (or leaves write-enable set) -- e.g. after a hang bad
; enough to need a manual reboot, which skips every normal revert path.
;
; Deliberately placed here, right after STAGE_COPY_ROUTINE_ABS's own code,
; NOT in the payload window (0x8000-0x83FF) -- recovering from a
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
                        ; Jumps to STAGE_BOOT_ENTRY (STAGE RAM's own checksummed copy,
                        ; boot-mode exits, skipped when a verified copy is
                        ; already in SRAM). jmp (3) + .blkb 19 = 22, keeping
                        ; the header size fixed per the note above.
	jmp STAGE_BOOT_ENTRY
	.blkb 19

; ---------------------------------------------------------------------
; First-letter index (26 x 2-byte BE pointers, A-Z). E/M/S are used.
KEYWORD_INDEX:
	.dw 0x0000  ; A
	.dw 0x0000  ; B
	.dw 0x0000  ; C
	.dw 0x0000  ; D
	.dw ECVER_TABLE_ENTRY+2  ; E -- 2nd character of ECVER, its own sole entry
	.dw 0x0000  ; F
	.dw 0x0000  ; G
	.dw 0x0000  ; H
	.dw 0x0000  ; I
	.dw 0x0000  ; J
	.dw 0x0000  ; K
	.dw 0x0000  ; L
	.dw MLOGMSG_TABLE_ENTRY+2  ; M -- 2nd character of MLOGMSG, the first M-entry (MLOG follows it)
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
	.dw KW_START

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
	.dw KW_START

	.db 0xC6
	.ascii "SDLOAD"
	.dw 0xE187
	.dw KW_START

	.db 0xC4
	.ascii "SDLS"
	.dw 0xE185
	.dw KW_START

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
	.dw KW_START

	.db 0xC4
	.ascii "SDRM"
	.dw 0xE188
	.dw KW_START

	.db 0xC6
	.ascii "SDSAVE"
	.dw 0xE186
	.dw KW_START

	.db 0xC4
	.ascii "SDCP"
	.dw 0xE18B
	.dw KW_START

	.db 0xC4
	.ascii "SDCD"
	.dw 0xE18C
	.dw KW_START

	.db 0xC7
	.ascii "SDMKDIR"
	.dw 0xE18D
	.dw KW_START

	.db 0xC5
	.ascii "SDPWD"
	.dw 0xE18F
	.dw KW_START

	; Not a prefix collision with SDMKDIR -- both share "SDM" but diverge
	; at the 4th character (V vs K), so table order doesn't matter here
	; (unlike SDRM/SDRMDIR above). E180 is the next unused code value --
	; E185-E18F are already taken by the eleven entries above.
	.db 0xC4
	.ascii "SDMV"
	.dw 0xE180
	.dw KW_START

	; SDOPEN/SDCLOSE/SDINPUT/SDPRINT/SDSKIP -- table names deliberately
	; omit '#' (SDINPUT #1,A still parses: the '#' is an optional part of
	; the argument, see keywords.c). Checked every pair
	; against all other entries above and each other for strict-prefix
	; collisions (the SDF-vs-SDFMT/SDRM-vs-SDRMDIR class of bug) -- none
	; found, so order doesn't matter here. Code values E190-E194 are the
	; next unused ones (E180-E18F already taken above).
	.db 0xC6
	.ascii "SDOPEN"
	.dw 0xE190
	.dw KW_START

	.db 0xC7
	.ascii "SDCLOSE"
	.dw 0xE191
	.dw KW_START

	.db 0xC7
	.ascii "SDINPUT"
	.dw 0xE192
	.dw KW_START

	.db 0xC7
	.ascii "SDPRINT"
	.dw 0xE193
	.dw KW_START

	.db 0xC6
	.ascii "SDSKIP"
	.dw 0xE194
	.dw KW_START

	; STAGE -- kept inside the contiguous SDxxx run (reached via the same
	; skip-scan chain as every entry above), not after ECVER's own
	; dedicated-first-letter-slot entry below -- the first placement,
	; after ECVER, was never reached (only an index slot's first entry is
	; exempt from the marker check). Diverges from every SDxxx name at the second character
	; ('T' vs 'D'), so no prefix-collision risk with any entry in this
	; chain.
	.db 0xC5
	.ascii "STAGE"
	.dw 0xE197
	.dw KW_START

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

	; MLOG -- own first-letter index slot ('M', same pattern as ECVER's
	; own 'E'). Named MLOG, not LOG -- LOG collides with
	; the PC-1500's own built-in LN/LOG/EXP logarithm functions (confirmed
	; live 2026-09-21: "LOG VIEW" was silently tokenized as the built-in
	; LOG(VIEW) function call, never reaching this ROM's own keyword table
	; at all, and failed with the base ROM's own ERROR 39 "illogical
	; calculation" -- the exact symptom a botched logarithm call produces,
	; not anything this file's own code raises).
	;
	; MLOGMSG (2026-09-24) sits BEFORE MLOG and now owns the 'M' index slot:
	; name matching is a letter-by-letter prefix search
	; (PC1500_BASIC_Keyword_Extension_Mechanism.md sec.2), so with MLOG
	; first, "MLOGMSG" would match MLOG and hand it "MSG" as an argument.
	; A mismatch on MLOGMSG ("MLOG VIEW" differs at the 5th letter) skip-
	; scans onto MLOG, whose marker C4 has bit 4 clear as a non-first entry
	; must (sec.4). The terminator right after MLOG is also new: the bytes
	; that used to follow it (STAGE's display strings and routine, then the
	; old terminator further down) start with 'S' = 0x53, bit 4 set, which
	; only went unnoticed while MLOG was the first M entry and so exempt.
MLOGMSG_TABLE_ENTRY:
	.db 0xC7
	.ascii "MLOGMSG"
	.dw 0xE199
	.dw KW_START
MLOG_TABLE_ENTRY:
	.db 0xC4
	.ascii "MLOG"
	.dw 0xE198
	.dw KW_START
	; MCONF (2026-09-25) -- after MLOG, reached by the skip-scan from the
	; M slot's MLOGMSG (a non-first entry: marker C5, bit 4 clear). Differs
	; from MLOG/MLOGMSG at the second letter, so no prefix clash.
	.db 0xC5
	.ascii "MCONF"
	.dw 0xE19A
	.dw KW_START
	.db 0xD0  ; table terminator (see MLOGMSG's note above)

BASIC_PROGRAM_START_HI_ABS .equ 0x7865  ; BASIC's own program-start pointer, BE
BASIC_PROGRAM_START_LO_ABS .equ 0x7866
BASIC_PROGRAM_END_HI_ABS   .equ 0x7867  ; ...and program-end pointer (last byte)
BASIC_PROGRAM_END_LO_ABS   .equ 0x7868

; ---------------------------------------------------------------------
; Keyword executor (2026-09-25). Every table entry above but ECVER points at
; KW_START: all argument parsing and command sequencing runs on the MCU
; (RP2350/keywords.c), and this ROM only carries out the actions the MCU
; hands back -- the things that have to happen on the LH5801 side: the
; display, the keyboard, copying to and from the PC-1500's own RAM, BASIC
; variable lookup, evaluating BASIC expressions, and raising BASIC errors.
; See RP2350/pc_exp.h's EXP_KW_* block for the wire format.
;
; Works the same at the prompt and inside a running program (confirmed in
; pc1500emu): when BASIC dispatches an extension keyword, Y points just
; past its 2-byte token, at its arguments -- in DISP_BUFFER (7BB2H) for a
; typed command, in the program line for a running one. KW_START wakes the
; MCU (EC_WAKE), remembers that argument address (KW_TEXT_*), copies the
; token and the next 78 bytes into the data window, and sends
; EXP_COMMAND_KEYWORD. The MCU answers with the statement's length
; (EXP_KW_END_ABS -- up to the ':' or CR that ends it) and an action block
; at EXP_KW_ACTION_ABS; any status but SUCCESS (an MCU firmware without the
; executor, an unknown keyword) is ERROR 1. An action that needs the MCU
; again afterwards (a Y/N answer, a listing pick, a variable, an evaluated
; expression) finishes with KW_CONTINUE, which sends
; EXP_COMMAND_KEYWORD_CONTINUE and dispatches the next action block.
;
; Note: BASIC removes every space outside quotes and tokenizes its own
; keywords inside the arguments ("MCONF SLEEPWAIT=1" is stored as "SLEEP",
; the WAIT token, "=1"); the MCU expands those back before parsing.
KW_TEXT_HI_ABS     .equ (EXP_KW_ACTION_ABS+8)  ; the statement's argument address
KW_TEXT_LO_ABS     .equ (EXP_KW_ACTION_ABS+9)  ; (Y at KW_START)
SD_VAR_TYPE_ABS    .equ (EXP_KW_ACTION_ABS+10) ; SD_LOOKUP_VARIABLE's 7A07H type/size
                                                ; byte -- bit7 set = numeric, clear =
                                                ; string (low 7 bits = capacity)
SD_VAR_ADDR_HI_ABS .equ (EXP_KW_ACTION_ABS+11) ; SD_LOOKUP_VARIABLE's variable address
SD_VAR_ADDR_LO_ABS .equ (EXP_KW_ACTION_ABS+12)
SD_VARNAME_HI_ABS  .equ KW_A_HI_ABS            ; VAR_LOOKUP: the MCU's operand A is
SD_VARNAME_LO_ABS  .equ KW_A_LO_ABS            ; the D461H name code

; Shared exit for every keyword the MCU ran (and the STAGE copy routine's
; success path): Y to the end of the statement, EC_DONE, then VEJ E2 --
; BASIC's own end-of-statement vector, the same exit the CE-150's
; statements use. In a program it carries on with the next statement; at
; the prompt it finishes the command (and raises ERROR 1 if anything but
; ':' or CR follows). This replaced a prompt-only tail (2026-09-25) that
; hand-patched BASIC's state and jumped to the idle loop at E2AA, which
; could never have worked inside a running program.
KEYWORD_RETURN:
	sjp KW_TEXT_Y
	lda (EXP_KW_END_ABS)
	adr y
	sjp EC_DONE            ; keyword over -- see EC_WAKE/EC_DONE (STAGE RAM sleep)
	vej 0xE2

; Y = the statement's argument address. Clobbers A.
KW_TEXT_Y:
	lda (KW_TEXT_HI_ABS)
	sta yh
	lda (KW_TEXT_LO_ABS)
	sta yl
	rtn

; ---------------------------------------------------------------------
; ECVER -- shows the ROM version. Pure ROM: it never wakes the MCU. Waits
; for a key so the message stays up; Y still points past the token, where
; VEJ E2 expects the end of the statement.
ECVER_ROUTINE:
	ldi uh,>ECVER_MSG
	ldi ul,<ECVER_MSG
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	vej 0xE2
ECVER_MSG:
	.ascii "LH5801 Expansion Card 0.2 "   ; exactly 26: a full line
SD_LIST_BLANK:                 ; a blank LCD line
	.ascii "                          "

KW_START:
	sjp EC_WAKE
	bcs SD_RAISE_ERROR_1_NO_DONE
	lda yh
	sta (KW_TEXT_HI_ABS)
	lda yl
	sta (KW_TEXT_LO_ABS)
	dec y                      ; from the token
	dec y
	lda yh
	sta xh
	lda yl
	sta xl
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,0x00
	ldi ul,2+EXP_KW_LINE_LEN
	sjp SD_COPY_BYTES
	ldi a,EXP_COMMAND_KEYWORD
KW_SEND:
	sjp EC_SEND
	cpi a,EXP_STATUS_SUCCESS
	bzr SD_RAISE_ERROR_1
	lda (KW_ACT_ABS)             ; jump through KW_ACTION_TABLE[action]
	shl
	ldi xh,>KW_ACTION_TABLE
	ldi xl,<KW_ACTION_TABLE
	adr x
	lin x
	sta yh
	lda (x)
	sta xl
	lda yh
	sta xh
	stx p
KW_ACTION_TABLE:
	.dw KEYWORD_RETURN       ; EXP_KW_ACTION_DONE
	.dw KW_SHOW              ; EXP_KW_ACTION_SHOW
	.dw KW_ERROR             ; EXP_KW_ACTION_ERROR
	.dw KW_BROWSE            ; EXP_KW_ACTION_BROWSE
	.dw KW_LOAD              ; EXP_KW_ACTION_LOAD
	.dw KW_SAVE              ; EXP_KW_ACTION_SAVE
	.dw KW_VAR_LOOKUP        ; EXP_KW_ACTION_VAR_LOOKUP
	.dw KW_VAR_STORE         ; EXP_KW_ACTION_VAR_STORE
	.dw KW_STAGE             ; EXP_KW_ACTION_STAGE
	.dw KW_EVAL              ; EXP_KW_ACTION_EVAL

; SHOW: the 26 characters at EXP_BUFFER_START_ABS (the MCU pads them to a
; full line), then wait for a key -- a result stays on screen until
; dismissed, and a Y/N prompt gets its answer the same way. The key goes
; back in ANSWER (0 for BREAK).
KW_SHOW:
	ldi uh,>EXP_BUFFER_START_ABS
	ldi ul,<EXP_BUFFER_START_ABS
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	bcr KW_ANSWER
	ldi a,0x00
KW_ANSWER:
	sta (KW_ANSWER_ABS)
KW_CONTINUE:
	ldi a,EXP_COMMAND_KEYWORD_CONTINUE
	bch KW_SEND

; ERROR n. ERROR 1 has its own base-ROM vector; any other number goes
; through the general VEJ 0xE0 path with UH = the number.
KW_ERROR:
	sjp EC_DONE
	lda (KW_ARG_ABS)
	cpi a,0x01
	bzs SD_RAISE_ERROR_1_NO_DONE
	sta uh
	vej 0xE0

; Raises a genuine BASIC "ERROR 1" (the same syntax-error state/message a
; real mistyped statement produces -- confirmed live, comparing typing an
; actually-invalid statement against this exact vector call: identical
; ERL/idle-return state either way) and resets to the idle prompt. Never
; returns; safe from a custom keyword's immediate-mode dispatch context
; regardless of stack depth (the handler resets S itself first).
SD_RAISE_ERROR_1:
	sjp EC_DONE                ; keyword over -- see EC_WAKE/EC_DONE
SD_RAISE_ERROR_1_NO_DONE:      ; for EC_WAKE's own failure: a DONE would
	vej 0xE4                   ; only wake the MCU again, with nothing after it


; BROWSE: the listing the MCU just produced at EXP_BUFFER_START_ABS, in
; EXP_COMMAND_LIST_SD_DIR's wire format (SDLS, SDLOAD's picker, SDOPEN's
; channel list, MLOG VIEW). Up/Down step through it, CL and BREAK return
; to BASIC. ARG 0 (view): Enter also returns, L does nothing. ARG
; EXP_KW_BROWSE_SELECT (SDLOAD): Enter does nothing -- deliberately, so the
; same listing serves both purposes without retyping SDLS first -- and L
; on a real entry (not the summary line) blanks the line, puts the entry's
; index in ANSWER and continues.
KW_BROWSE:
	lda (EXP_BUFFER_START_ABS+1)
	sta (SD_LIST_COUNT_ABS)
	ldi a,0x00
	sta (SD_LIST_INDEX_ABS)
	ldi a,>(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_HI_ABS)
	ldi a,<(EXP_BUFFER_START_ABS+2)
	sta (SD_LIST_ADDR_LO_ABS)
KW_BROWSE_DRAW:
	sjp SD_LIST_DISPLAY
KW_BROWSE_KEY:
	sjp KEYSCAN_WAIT           ; ACC=code, Carry=1 only for BREAK
	bcs KW_BROWSE_EXIT
	cpi a,KEY_CL
	bzs KW_BROWSE_EXIT
	cpi a,KEY_UP
	bzs KW_BROWSE_UP
	cpi a,KEY_DOWN
	bzs KW_BROWSE_DOWN
	cpi a,KEY_L
	bzs KW_BROWSE_PICK
	cpi a,KEY_ENTER
	bzr KW_BROWSE_KEY
	lda (KW_ARG_ABS)           ; Enter: selecting ignores it
	bzr KW_BROWSE_KEY
KW_BROWSE_EXIT:
	jmp KEYWORD_RETURN
KW_BROWSE_UP:
	sjp SD_LIST_UP
	bch KW_BROWSE_DRAW
KW_BROWSE_DOWN:
	sjp SD_LIST_DOWN
	bch KW_BROWSE_DRAW
KW_BROWSE_PICK:
	lda (KW_ARG_ABS)           ; viewing ignores L
	bzs KW_BROWSE_KEY
	lda (SD_LIST_INDEX_ABS)
	cpa (SD_LIST_COUNT_ABS)
	bzs KW_BROWSE_KEY          ; the summary line isn't a file
	sta (KW_ANSWER_ABS)
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	bch KW_CONTINUE

; Listing browser state lives in the data window, not CPU registers --
; KEYSCAN_WAIT/DISP_N_CHARS0 don't preserve registers. Deliberately NOT
; EXP_SCRATCH_ABS (0x8100): a listing's own entries reach window offset
; 256 once there are 9+ files, and every Up/Down used to overwrite entry
; #8's name with the cursor bytes (confirmed live 2026-09-19/20). Offset
; 2038 is past the longest listing (2 + EXP_DIR_MAX_ENTRIES*30 + 26), past
; the keyword action block, and before EXP_BLOCK_CHECKSUM_ABS.
;
; Pages are always full 26-character lines (an entry's name + size text
; are back-to-back in the wire format; the summary line is 26 characters
; too), so each redraw is one DISP_N_CHARS0 call that overwrites the whole
; line. Don't split a draw into DISP_N_CHARS0 + DISP_N_CHARS: DISP_N_CHARS
; (ED00H) is coupled to the line editor's cursor-blink handling and
; garbled the output live. Up/Down walk the entry address one
; EXP_DIR_RECORD_SIZE step at a time (no multiply instruction); index ==
; count means the summary line, which sits exactly one record past the
; last entry.
SD_LIST_LINE_WIDTH .equ 0x1A  ; 26 -- DISP_N_CHARS0's own max, one LCD line
SD_LIST_SCRATCH_ABS .equ (WINDOW_BASE + 2038)
SD_LIST_INDEX_ABS   .equ (SD_LIST_SCRATCH_ABS+0)  ; current entry index, 0..count (count = summary)
SD_LIST_COUNT_ABS   .equ (SD_LIST_SCRATCH_ABS+1)  ; real entry count, low byte only
SD_LIST_ADDR_HI_ABS .equ (SD_LIST_SCRATCH_ABS+2)  ; current entry/summary address
SD_LIST_ADDR_LO_ABS .equ (SD_LIST_SCRATCH_ABS+3)

SD_LIST_DISPLAY:
	lda (SD_LIST_ADDR_HI_ABS)
	sta uh
	lda (SD_LIST_ADDR_LO_ABS)
	sta ul
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	; DISP_N_CHARS0 moves BLINK_CURSOR_H/L (787EH/787FH) to just after
	; what it drew -- right for echoing typed input, but here it left a
	; blinking block cursor mid-line while browsing (confirmed live). 7400H
	; is what a genuinely idle prompt holds there: "no live cursor target".
	ldi a,0x74
	sta (0x787E)
	ldi a,0x00
	sta (0x787F)
	rtn

; One record toward the first entry; a no-op if already there.
SD_LIST_UP:
	lda (SD_LIST_INDEX_ABS)
	bzs SD_LIST_UP_DONE        ; already at the first entry
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
	bch SD_LIST_STORE_ADDR
SD_LIST_UP_DONE:
	rtn

; One record toward the summary line; a no-op if already there.
SD_LIST_DOWN:
	lda (SD_LIST_INDEX_ABS)
	cpa (SD_LIST_COUNT_ABS)
	bzs SD_LIST_UP_DONE        ; already at the summary (index == count)
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
SD_LIST_STORE_ADDR:
	lda xh
	sta (SD_LIST_ADDR_HI_ABS)
	lda xl
	sta (SD_LIST_ADDR_LO_ABS)
	rtn

; VAR_LOOKUP: look up variable A (a D461H name code -- D461H creates it,
; zeroed, if it doesn't exist yet) and copy its type byte and raw storage
; to EXP_BUFFER_START_ABS, then continue. VAR_STORE: copy the storage-sized
; bytes the MCU left at EXP_BUFFER_START_ABS into the variable found by the
; last VAR_LOOKUP, then continue.
KW_VAR_LOOKUP:
	sjp SD_LOOKUP_VARIABLE
	lda (SD_VAR_TYPE_ABS)
	sta (EXP_BUFFER_START_ABS)
	sjp KW_VAR_SIZE
	lda (SD_VAR_ADDR_HI_ABS)
	sta xh
	lda (SD_VAR_ADDR_LO_ABS)
	sta xl
	ldi yh,>(EXP_BUFFER_START_ABS+1)
	ldi yl,<(EXP_BUFFER_START_ABS+1)
	bch KW_VAR_COPY
KW_VAR_STORE:
	sjp KW_VAR_SIZE
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	lda (SD_VAR_ADDR_HI_ABS)
	sta yh
	lda (SD_VAR_ADDR_LO_ABS)
	sta yl
KW_VAR_COPY:
	sjp SD_COPY_BYTES
	bch KW_CONTINUE

; EVAL: evaluate the BASIC expression A bytes into the statement with
; BASIC's own evaluator (VEJ DEH -- the same one the CE-150's statements
; use), so an argument can be any expression: "X", F$, A*2, &4000. The
; result goes to EXP_BUFFER_START_ABS as the arithmetic register's 8 bytes
; (7A00H-7A07H, TRM sec.5-3: a decimal number, B2H at +4 for a binary
; integer, D0H at +4 for a string with address and length at +5..+7), and
; a string's characters follow at +8. B's low byte = where the expression
; ended, again from the statement's start. An evaluation error (a bad
; expression, an undefined array...) is raised as BASIC raises it: the
; evaluator leaves the error number in UH.
KW_EVAL:
	sjp KW_TEXT_Y
	lda (KW_A_LO_ABS)
	adr y
	vej 0xDE
	.db KW_EVAL_ERROR-(.+1)
	lda yl                     ; end of the expression, from the start
	sec
	sbc (KW_TEXT_LO_ABS)
	sta (KW_B_LO_ABS)
	ldi xh,0x7A
	ldi xl,0x00
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,0x00
	ldi ul,0x08
	sjp SD_COPY_BYTES
	lda (0x7A04)
	cpi a,0xD0
	bzr KW_EVAL_DONE           ; a number
	vej 0xDC                   ; X = string address, UL = its length
	lda ul
	bzs KW_EVAL_DONE           ; ""
	ldi uh,0x00                ; Y is already at +8
	sjp SD_COPY_BYTES
KW_EVAL_DONE:
	jmp KW_CONTINUE
KW_EVAL_ERROR:
	sjp EC_DONE
	vej 0xE0

; STAGE RAM / STAGE DEBUG (ARG = STAGE_DEBUG_FLAG) -- the MCU has already
; checked that a verified copy isn't there (STAGE RAM only). See
; STAGE_COPY_ROUTINE_ABS.
KW_STAGE:
	lda (KW_ARG_ABS)
	sta (STAGE_DEBUG_FLAG)
	ldi a,0x00
	sta (STAGE_BOOT_FLAG)
	jmp STAGE_COPY_ROUTINE_ABS

; LOAD: the MCU has the file open (and has read an M file's header). Read
; it in EXP_MAX_TRANSFER_LEN chunks into RAM starting at A until a read
; returns 0 bytes or fails, then close it. ARG EXP_KW_XFER_BASIC: the
; target is BASIC's live program-start pointer (7865H -- not a hard-coded
; address, so a RAM-expanded machine loads where its own BASIC expects
; it), and afterwards the program-end pointer is set to the last byte
; written. ARG EXP_KW_LOAD_CALL: then CALL B, returning to BASIC from
; there.
KW_LOAD:
	lda (KW_ARG_ABS)
	ani a,EXP_KW_XFER_BASIC
	bzs KW_LOAD_READ
	lda (BASIC_PROGRAM_START_HI_ABS)
	sta (KW_A_HI_ABS)
	lda (BASIC_PROGRAM_START_LO_ABS)
	sta (KW_A_LO_ABS)
KW_LOAD_READ:
	ldi a,>EXP_MAX_TRANSFER_LEN
	sta (EXP_LENGTH_PORT_ABS+0)
	ldi a,<EXP_MAX_TRANSFER_LEN
	sta (EXP_LENGTH_PORT_ABS+1)
	ldi a,EXP_COMMAND_READ_FROM_SD_FILE
	sjp EC_SEND
	cpi a,EXP_STATUS_SUCCESS
	bzr KW_LOAD_CLOSE
	lda (EXP_LENGTH_PORT_ABS+1)
	sta ul
	lda (EXP_LENGTH_PORT_ABS+0)
	sta uh
	bzr KW_LOAD_COPY
	lda ul
	bzs KW_LOAD_CLOSE          ; 0 bytes: end of file
KW_LOAD_COPY:
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	lda (KW_A_HI_ABS)
	sta yh
	lda (KW_A_LO_ABS)
	sta yl
	sjp SD_COPY_BYTES
	lda yh
	sta (KW_A_HI_ABS)
	lda yl
	sta (KW_A_LO_ABS)
	bch KW_LOAD_READ
KW_LOAD_CLOSE:
	ldi a,EXP_COMMAND_CLOSE_SD_FILE
	sjp EC_SEND
	lda (KW_ARG_ABS)
	ani a,EXP_KW_XFER_BASIC
	bzs KW_LOAD_CALL
	lda (KW_A_HI_ABS)
	sta xh
	lda (KW_A_LO_ABS)
	sta xl
	dec x
	lda xh
	sta (BASIC_PROGRAM_END_HI_ABS)
	lda xl
	sta (BASIC_PROGRAM_END_LO_ABS)
KW_LOAD_CALL:
	lda (KW_ARG_ABS)
	ani a,EXP_KW_LOAD_CALL
	bzs KW_LOAD_RETURN
	ldi uh,>KEYWORD_RETURN     ; the called code's RTN lands in KEYWORD_RETURN
	ldi ul,<KEYWORD_RETURN
	psh u
	lda (KW_B_HI_ABS)
	sta xh
	lda (KW_B_LO_ABS)
	sta xl
	stx p
KW_LOAD_RETURN:
	jmp KEYWORD_RETURN

; SAVE: the MCU has created the file (and written an M file's header).
; Write RAM A..B inclusive to it, at most EXP_MAX_TRANSFER_LEN bytes per
; WRITE_TO_SD_FILE (the length goes at EXP_LENGTH_PORT_ABS, outside the
; payload), stopping early if a write fails, then close it. ARG
; EXP_KW_XFER_BASIC: the range is BASIC's own program-start..program-end.
KW_SAVE:
	lda (KW_ARG_ABS)
	ani a,EXP_KW_XFER_BASIC
	bzs KW_SAVE_CHUNK
	lda (BASIC_PROGRAM_START_HI_ABS)
	sta (KW_A_HI_ABS)
	lda (BASIC_PROGRAM_START_LO_ABS)
	sta (KW_A_LO_ABS)
	lda (BASIC_PROGRAM_END_HI_ABS)
	sta (KW_B_HI_ABS)
	lda (BASIC_PROGRAM_END_LO_ABS)
	sta (KW_B_LO_ABS)
KW_SAVE_CHUNK:
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	lda (KW_A_HI_ABS)
	sta xh
	lda (KW_A_LO_ABS)
	sta xl
	ldi uh,0x00                ; U counts this chunk's bytes
	ldi ul,0x00
KW_SAVE_COPY:
	lda xh
	cpa (KW_B_HI_ABS)
	bzr KW_SAVE_BYTE
	lda xl
	cpa (KW_B_LO_ABS)
	bzs KW_SAVE_LAST           ; X is at B: copy it and finish
KW_SAVE_BYTE:
	tin
	inc u
	lda uh
	cpi a,>EXP_MAX_TRANSFER_LEN
	bzr KW_SAVE_COPY
	sjp KW_SAVE_WRITE          ; a full chunk; Z set if it was written
	bzs KW_SAVE_CHUNK
	bch KW_SAVE_CLOSE
KW_SAVE_LAST:
	tin
	inc u
	sjp KW_SAVE_WRITE
KW_SAVE_CLOSE:
	ldi a,EXP_COMMAND_CLOSE_SD_FILE
	sjp EC_SEND
	jmp KEYWORD_RETURN

; Writes the U bytes staged at EXP_BUFFER_START_ABS; saves X (the next
; byte to copy) in A first. Z set = SUCCESS.
KW_SAVE_WRITE:
	lda xh
	sta (KW_A_HI_ABS)
	lda xl
	sta (KW_A_LO_ABS)
	lda uh
	sta (EXP_LENGTH_PORT_ABS+0)
	lda ul
	sta (EXP_LENGTH_PORT_ABS+1)
	ldi a,EXP_COMMAND_WRITE_TO_SD_FILE
	sjp EC_SEND
	cpi a,EXP_STATUS_SUCCESS
	rtn

; Copies U bytes from (X) to (Y), advancing both. U must not be 0 (it
; would wrap and copy 64K).
SD_COPY_BYTES:
	tin
	dec u
	cpi uh,0x00
	bzr SD_COPY_BYTES
	cpi ul,0x00
	bzr SD_COPY_BYTES
	rtn

; Sends the command in A and waits for its final status (EC_WAIT_NOT_BUSY).
EC_SEND:
	sta (EXP_INSTRUCTION_ABS)
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
; EC_WAKE / EC_DONE (2026-09-24) -- the two halves of the RP2350's STAGE
; RAM sleep protocol (RP2350/pc_exp.h EXP_COMMAND_DONE, monitor.c "STAGE
; RAM sleep"). Once the ROM is staged into SRAM the MCU goes DORMANT
; between keywords; while it's asleep nothing drives the data bus, so the
; whole 0x8000-0x87FF window reads 0x00 (pull-downs) and writes to it are lost. Every
; keyword therefore starts with EC_WAKE (KW_START) and ends with EC_DONE (KEYWORD_RETURN, SD_RAISE_ERROR_*). In MCU
; mode the MCU never sleeps and both are just two quick extra commands.
;
; EC_WAKE: write CLEAR_STATUS -- a write trigger, which wakes a sleeping
; MCU (that write itself is lost; the MCU sets READY as part of waking) or
; is dispatched normally by an awake one (-> READY) -- then poll until the
; status cell reads READY specifically: not merely "not 0xFF", since an
; awake MCU's status cell still holds the previous command's final result.
; Tight poll with no HLT (the boot hook calls this with interrupts off).
;
; Then a SECOND CLEAR_STATUS, waited on the same way: when the first write
; was the one that woke the MCU it was lost, and the MCU treats a wake with
; no command after it as stray (a PEEK/POKE into the window) and goes back
; to sleep after ~100ms -- this second write is the command that tells it
; a keyword really has started, whatever the keyword does next (a Y/N
; prompt, SDLS browsing) before its own first command. See monitor.c's
; STRAY_WAKE_TIMEOUT_US.
;
; Preserves A and U; Carry set if READY never arrives (~3s per wait,
; 65536 polls).
EC_WAKE:
	psh a
	psh u
	ldi a,EXP_COMMAND_CLEAR_STATUS
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAKE_WAIT_READY
	bcs EC_WAKE_TIMEOUT
	ldi a,EXP_COMMAND_CLEAR_STATUS
	sta (EXP_INSTRUCTION_ABS)
	sjp EC_WAKE_WAIT_READY
	bcs EC_WAKE_TIMEOUT
	pop u
	pop a
	rec
	rtn
EC_WAKE_TIMEOUT:
	pop u
	pop a
	sec
	rtn

; Carry clear once the status cell reads READY, set after 65536 polls.
; Clobbers A and U (EC_WAKE saves both).
EC_WAKE_WAIT_READY:
	ldi uh,0xFF
	ldi ul,0xFF
EC_WAKE_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_READY
	bzs EC_WAKE_GOT_READY
	dec u
	cpi uh,0x00
	bzr EC_WAKE_POLL
	cpi ul,0x00
	bzr EC_WAKE_POLL
	sec
	rtn
EC_WAKE_GOT_READY:
	rec
	rtn

; EC_DONE: tell the MCU this keyword is finished (in STAGE RAM mode it goes
; back to sleep). Waits for DONE's own status to leave BUSY, so the MCU has
; registered it before this keyword's caller can start the next one.
; Clobbers A only.
EC_DONE:
	ldi a,EXP_COMMAND_DONE
	sta (EXP_INSTRUCTION_ABS)
EC_DONE_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs EC_DONE_POLL
	rtn

; ---------------------------------------------------------------------
; BASIC variable support for SDINPUT#/SDPRINT#/MLOGMSG A$ -- D461H-based
; address lookup. The MCU parses variable names into D461H name codes and
; converts between a variable's raw storage and the SD value-chunk format
; itself (keywords.c); this ROM only finds the variable and copies bytes.
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
; rather than failing), so no error path is handled here.
;
; Stashes the variable's own address (SD_VAR_ADDR_HI/LO_ABS) and type/size
; byte (SD_VAR_TYPE_ABS, from D461_TYPE_ABS). Does NOT preserve X.
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
	rtn

; U = the storage size of the variable SD_LOOKUP_VARIABLE just found: 8
; bytes for a number (packed-BCD float, TRM sec.5-3-1), the capacity (type
; byte's low 7 bits) for a string (zero-padded inline ASCII, no length
; field -- confirmed live). Leaves A clobbered.
KW_VAR_SIZE:
	ldi uh,0x00
	ldi ul,0x08
	lda (SD_VAR_TYPE_ABS)
	shl                         ; bit7 (numeric) into Carry
	bcs KW_VAR_SIZE_DONE
	shr                         ; back to the capacity
	sta ul
KW_VAR_SIZE_DONE:
	rtn

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
; STAGE entry points (2026-09-24). All of these only ever run BEFORE
; ROM_COPY_BEGIN switches
; Remap, or after a verified copy is already in SRAM (identical bytes), so
; living at ROM_BASE+ is safe -- see STAGE_COPY_ROUTINE_ABS's own header.
;
; STAGE_BOOT_ENTRY -- jumped to from BOOT_SELFCHECK_ENTRY (ROM_BASE+0AH),
; the base ROM's boot-time module scan hook (PC1500_BASIC_Keyword_
; Extension_Mechanism.md sec.11): entered via STX P with the return
; address already pushed, so every exit is a plain RTN. Runs with
; interrupts off -- ROM1's reset vector (E000) starts with RIE and only
; SIEs at E122, after every module hook has run -- so STAGE_BOOT_FLAG makes
; the copy routine skip its own SIE, display, key wait and BASIC error on
; the way out. A failure reverts to MCU-served ROM and returns quietly
; (the MCU has already logged the cause for MLOG VIEW).
;
; The hook runs on every reset that reaches the scan (and possibly every
; power-on), while the GreenPAKs and SRAM keep their state as long as the
; module stays powered -- so the copy is skipped whenever STAGE_IS_STAGED
; says a verified copy is already there. STAGE RAM from BASIC gets the
; same early exit, from the MCU (keywords.c).
;
; U and Y are saved around the copy: the base ROM's scan loop keeps its
; page limit in UH across each hook call (E4B7, sec.11) and only restores
; A/X itself (E118/E11A), while the copy routine uses U/Y as its counters
; and pointers. Found in pc1500emu: without this, the boot that actually
; copied never left the scan loop (spinning at E4B0-E4BC). The copy
; routine is SJP'd, not JMP'd, so its boot-mode RTNs land back here --
; safe at ROM_BASE+ either way: after success SRAM holds the verified copy,
; after a failure the routine has already reverted to MCU-served ROM.
;
; Wakes the MCU first and sends DONE on the way out, like every keyword
; (EC_WAKE/EC_DONE) -- a staged reset/power-on usually finds it asleep, and
; DONE puts it back to sleep once the ROM is (still) staged. If it never
; answers, just return: nothing was touched, boot continues on whatever is
; serving ROM_BASE+.
STAGE_BOOT_ENTRY:
	sjp EC_WAKE
	bcs STAGE_BOOT_ENTRY_NO_MCU
	sjp STAGE_IS_STAGED
	bcs STAGE_BOOT_ENTRY_DONE      ; verified copy already in SRAM
	psh u
	psh y
	ldi a,0x00
	sta (STAGE_DEBUG_FLAG)
	ldi a,0x01
	sta (STAGE_BOOT_FLAG)
	sjp STAGE_COPY_ROUTINE_ABS
	pop y
	pop u
STAGE_BOOT_ENTRY_DONE:
	sjp EC_DONE
STAGE_BOOT_ENTRY_NO_MCU:
	rtn

; "STAGE: OK" and back to BASIC -- the copy routine's own success path. Blanks the line first:
; DISP_N_CHARS0 doesn't clear past what it draws, so the short message
; alone left "UG" from "STAGE DEBUG" behind.
STAGE_SHOW_OK:
	ldi uh,>SD_LIST_BLANK
	ldi ul,<SD_LIST_BLANK
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
	ldi uh,>STAGE_OK_MSG
	ldi ul,<STAGE_OK_MSG
	ldi xl,STAGE_OK_MSG_LEN
	sjp DISP_N_CHARS0
	sjp KEYSCAN_WAIT
	jmp KEYWORD_RETURN

; Carry set = Remap is on AND the MCU vouches for the SRAM copy (a full
; ROM_COPY_FINISH succeeded since the last BEGIN/revert/MCU boot) --
; EXP_COMMAND_ROM_GET_MODE's second response byte. Carry clear on anything
; else, including a failed query. Hand-rolled poll, not EC_WAIT_NOT_BUSY:
; that one SIE+HLTs, and the boot hook runs with interrupts off.
STAGE_IS_STAGED:
	ldi a,EXP_COMMAND_ROM_GET_MODE
	sta (EXP_INSTRUCTION_ABS)
STAGE_IS_STAGED_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs STAGE_IS_STAGED_POLL
	cpi a,EXP_STATUS_SUCCESS
	bzr STAGE_IS_STAGED_NO
	lda (EXP_BUFFER_START_ABS+1)
	cpi a,0x01
	bzr STAGE_IS_STAGED_NO
	sec
	rtn
STAGE_IS_STAGED_NO:
	rec
	rtn

; ---------------------------------------------------------------------
; Guard: everything above must fit in the 6K ROM region (0x8800-0x9FFF).
; .org can only move the location counter forward within one absolute area
; -- if the content above already overran past 0xA000, this line itself
; fails to assemble instead of silently wrapping past the window.
	.org ROM_REGION_END
