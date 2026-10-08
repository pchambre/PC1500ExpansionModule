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

; KEEP STAGE_DEBUG_FLAG and STAGE_BOOT_FLAG (and STAGE_COPY_ROUTINE_ABS
; itself) at the addresses they have now (0x86B2, 0x86B3; 0x8400). Since
; 2026-09-29 STAGE RAM refreshes a copy that's already staged, and after a
; firmware update that runs the OLD image's KW_STAGE (from the SRAM), which
; writes these two by the addresses it was built with and jumps here.
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
	.blkb 1
KBD_HOOK:               ; ROM_BASE+0x0E -- the keyboard driver's fixed address
                        ; for the base ROM's keyboard hook (785BH/785CH = 88H/
                        ; 0EH): even, so the hook runs it with PV low. See
                        ; KBD_ENTRY.
	jmp KBD_ENTRY
                        ; ROM_BASE+0x11: what the MCU patches into the wait
                        ; loop it's given (EXP_COMMAND_KBD_INSTALL, RP2350/
                        ; kbd_seq.h KBD_DESCRIPTOR_OFFSET) -- 4 BE addresses.
	.dw KBD_LOOP
	.dw KBD_ANY
	.dw KBD_SCAN
	.dw KBD_DISPATCH
	.blkb 7

; ---------------------------------------------------------------------
; First-letter index (26 x 2-byte BE pointers, A-Z). E/M/S are used.
KEYWORD_INDEX:
	.dw 0x0000  ; A
	.dw BLSCAN_TABLE_ENTRY+2  ; B -- 2nd character of BLSCAN, the first B-entry
	.dw COLOR_TABLE_ENTRY+2   ; C -- the CE-150 stand-in's (2026-09-30), as G/L/R/T
	.dw 0x0000  ; D
	.dw ECVER_TABLE_ENTRY+2  ; E -- 2nd character of ECVER, its own sole entry
	.dw FNCLR_TABLE_ENTRY+2  ; F -- 2nd character of FNCLR, the first F-entry
	.dw GRAPH_TABLE_ENTRY+2  ; G
	.dw 0x0000  ; H
	.dw 0x0000  ; I
	.dw 0x0000  ; J
	.dw 0x0000  ; K
	.dw LCURSOR_TABLE_ENTRY+2  ; L
	.dw MLOGMSG_TABLE_ENTRY+2  ; M -- 2nd character of MLOGMSG, the first M-entry (MLOG follows it)
	.dw 0x0000  ; N
	.dw 0x0000  ; O
	.dw 0x0000  ; P
	.dw 0x0000  ; Q
	.dw RLINE_TABLE_ENTRY+2  ; R
	.dw KEYWORD_TABLE+2  ; S -- 2nd character of SDDF, the first S-entry
	.dw TAB_TABLE_ENTRY+2  ; T
	.dw 0x0000  ; U
	.dw 0x0000  ; V
	.dw WFSCAN_TABLE_ENTRY+2  ; W -- Wi-Fi (2026-10-06)
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

	; STSAVE / STLOAD (2026-09-25) -- also in the S chain; they differ from
	; STAGE at the third letter and from each other at the third, so no
	; prefix clash.
	.db 0xC6
	.ascii "STSAVE"
	.dw 0xE19E
	.dw KW_START
	.db 0xC6
	.ascii "STLOAD"
	.dw 0xE19F
	.dw KW_START
	; SDEOF(n) (2026-09-30) -- a FUNCTION of one argument: code E170, the
	; low byte of BASIC's ABS (F170); see BLSTAT_FN. In the S chain; no S
	; name is a prefix of it or it of one.
	.db 0xC5
	.ascii "SDEOF"
	.dw 0xE170
	.dw SDEOF_FN
	; SORGN (2026-09-30) -- the CE-150 stand-in's, see COLOR below. Differs
	; from every other S name at the second letter.
	.db 0xC5
	.ascii "SORGN"
	.dw 0xE1C4
	.dw CE150_E6_ENTRY
	; SSH, SSHKEY, SSHFORGET (2026-10-07): a shell on a host over Wi-Fi.
	; SSH is a prefix of the other two, so it comes after them (the first
	; whole match wins, as SDRM after SDRMDIR).
	.db 0xC6
	.ascii "SSHKEY"
	.dw 0xE1D1
	.dw KW_START
	.db 0xC9
	.ascii "SSHFORGET"
	.dw 0xE1D2
	.dw KW_START
	.db 0xC3
	.ascii "SSH"
	.dw 0xE1D0
	.dw KW_START

	; ECVER -- no argument, own first-letter index slot (only entry starting
	; with 'E', so reached directly via the index, not the skip-scan --
	; marker high nibble doesn't matter here, same as SDDF's own comment).
	; Plain version string, no EXP_COMMAND_*/MCU round-trip at all -- exists
	; purely to verify the keyword table itself dispatches correctly and
	; that this ROM image is actually the one being served.
ECVER_TABLE_ENTRY:
	.db 0xD5
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
	.db 0xD7
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
	; FNCLR / FNSAVE / FNLOAD (2026-09-25) -- their own 'F' index slot,
	; which points at FNCLR; the other two are reached by the skip-scan
	; (markers with bit 4 clear). No prefix clash among them.
FNCLR_TABLE_ENTRY:
	.db 0xD5
	.ascii "FNCLR"
	.dw 0xE19B
	.dw FNCLR_ROUTINE
	.db 0xC6
	.ascii "FNSAVE"
	.dw 0xE19C
	.dw KW_START
	.db 0xC6
	.ascii "FNLOAD"
	.dw 0xE19D
	.dw KW_START
	; BLE (2026-09-27, RP2350/BLE_PROTOCOL.md) -- their own 'B' index slot,
	; which points at BLSCAN; the rest are reached by the skip-scan (markers
	; with bit 4 clear). No name is a prefix of a later one. BLPRINT and
	; BLLIST contain the built-ins PRINT and LIST, as SDPRINT does -- checked
	; in pc1500emu that they still reach this table.
BLSCAN_TABLE_ENTRY:
	.db 0xD6
	.ascii "BLSCAN"
	.dw 0xE1A0
	.dw KW_START
	.db 0xC5                   ; BLCON (was BLCONNECT until 2026-09-28)
	.ascii "BLCON"
	.dw 0xE1A1
	.dw KW_START
	.db 0xC6
	.ascii "BLDISC"
	.dw 0xE1A2
	.dw KW_START
	.db 0xC7
	.ascii "BLPRINT"
	.dw 0xE1A3
	.dw KW_START
	.db 0xC6
	.ascii "BLLIST"
	.dw 0xE1A4
	.dw KW_START
	.db 0xC6
	.ascii "BLSAVE"
	.dw 0xE1A5
	.dw KW_START
	.db 0xC6
	.ascii "BLLOAD"
	.dw 0xE1A6
	.dw KW_START
	.db 0xC5                   ; BLCLS (2026-09-28): clears the peer's console
	.ascii "BLCLS"
	.dw 0xE1A7
	.dw KW_START
	.db 0xC5                   ; BLADV/BLPUT/BLGET (2026-09-28): peer-to-peer
	.ascii "BLADV"             ; (BLE_PROTOCOL.md). No clash: BLPUT differs from
	.dw 0xE1A8                 ; BLPRINT at the 4th letter
	.dw KW_START
	.db 0xC5
	.ascii "BLPUT"
	.dw 0xE1A9
	.dw KW_START
	.db 0xC5
	.ascii "BLGET"
	.dw 0xE1AA
	.dw KW_START
	.db 0xC6                   ; BLSEND/BLRECV/BLSTAT (2026-09-29): peer
	.ascii "BLSEND"            ; messaging. No clash with BLSCAN/BLSAVE (they
	.dw 0xE1AB                 ; differ by the 4th letter)
	.dw KW_START
	.db 0xC6
	.ascii "BLRECV"
	.dw 0xE1AC
	.dw KW_START
	.db 0xC6                   ; BLPAIR/BLUNPAIR (2026-10-03): the Link's pairing
	.ascii "BLPAIR"            ; (BLE_PROTOCOL.md sec.7). BLPAIR differs from BLPUT
	.dw 0xE1AD                 ; and BLPRINT at the 4th letter
	.dw KW_START
	.db 0xC8
	.ascii "BLUNPAIR"
	.dw 0xE1AE
	.dw KW_START
	.db 0xC5                   ; BLKBD (2026-10-04): pairs the external keyboard
	.ascii "BLKBD"             ; (MCONF BLKBD). The only BLK... name
	.dw 0xE1AF
	.dw KW_START
	.db 0xC6                   ; BLSTAT is a FUNCTION (S=BLSTAT, PRINT BLSTAT),
	.ascii "BLSTAT"            ; like MEM (F158): what makes a keyword a function
	.dw 0xE152                 ; is its code's low byte, 5xH = no argument
	.dw BLSTAT_FN              ; (ROM1 LD8AD) -- not the marker
	.db 0xC6                   ; BLKEY$ (2026-10-06): INKEY$ for either keyboard,
	.ascii "BLKEY$"            ; a function like it. Differs from BLKBD at the
	.dw 0xE153                 ; 4th letter
	.dw BLKEY_FN
	; The CE-150 printer/plotter's keywords (2026-09-30), drawn on the BLE
	; peer by the MCU (keywords.c, plotter.h) when no CE-150 is attached. A
	; CE-150's own table (B000, PV low) is searched before this page, both
	; to tokenize and to run, so with one attached these are never reached
	; by name. Eight keep the CE-150's own F0xx codes, which BASIC looks for
	; on every page; its other seven are E6xx, which BASIC only looks for on
	; the CE-150's own page, so they're E1C0-E1C6 here, in the CE-150's
	; order (E680-E686). SORGN is in the S chain above. Their own index
	; slots (C, G, L, R, T), the rest reached by the skip-scan (markers with
	; bit 4 clear); no name is a prefix of another.
	;
	; Every letter's FIRST entry (reached only through its index slot) has
	; a marker with bit 4 SET, as the CE-150's own table does: that's what
	; ends a skip-scan coming from the letter before. Without it the scan
	; runs on into the next letter's entries, comparing only from their
	; SECOND character (the first is taken as matched) -- found 2026-09-30
	; when "A=&FF" became ERROR 1: after the F chain, "F" matched LF.
COLOR_TABLE_ENTRY:
	.db 0xD5
	.ascii "COLOR"
	.dw 0xF0B5
	.dw KW_START
	.db 0xC5
	.ascii "CSIZE"
	.dw 0xE1C0
	.dw CE150_E6_ENTRY
GRAPH_TABLE_ENTRY:
	.db 0xD5
	.ascii "GRAPH"
	.dw 0xE1C1
	.dw CE150_E6_ENTRY
	.db 0xC8
	.ascii "GLCURSOR"
	.dw 0xE1C2
	.dw CE150_E6_ENTRY
LCURSOR_TABLE_ENTRY:
	.db 0xD7
	.ascii "LCURSOR"
	.dw 0xE1C3
	.dw CE150_E6_ENTRY
	.db 0xC2
	.ascii "LF"
	.dw 0xF0B6
	.dw KW_START
	.db 0xC4
	.ascii "LINE"
	.dw 0xF0B7
	.dw KW_START
	.db 0xC5
	.ascii "LLIST"
	.dw 0xF0B8
	.dw KW_START
	.db 0xC6
	.ascii "LPRINT"
	.dw 0xF0B9
	.dw KW_START
RLINE_TABLE_ENTRY:
	.db 0xD5
	.ascii "RLINE"
	.dw 0xF0BA
	.dw KW_START
	.db 0xC6
	.ascii "ROTATE"
	.dw 0xE1C5
	.dw CE150_E6_ENTRY
TAB_TABLE_ENTRY:
	.db 0xD3
	.ascii "TAB"
	.dw 0xF0BB
	.dw KW_START
	.db 0xC4
	.ascii "TEST"
	.dw 0xF0BC
	.dw KW_START
	.db 0xC4
	.ascii "TEXT"
	.dw 0xE1C6
	.dw CE150_E6_ENTRY
	; Wi-Fi (2026-10-06, RP2350/wifi_link.h) -- their own 'W' index slot,
	; which points at WFSCAN (marker bit 4 set, as every letter's first
	; entry); the rest are reached by the skip-scan. No name is a prefix of
	; another. WFFORGET contains the built-in FOR, as BLPRINT contains PRINT.
	; WFSTAT is a no-argument function, as BLSTAT.
WFSCAN_TABLE_ENTRY:
	.db 0xD6
	.ascii "WFSCAN"
	.dw 0xE1B0
	.dw KW_START
	.db 0xC5
	.ascii "WFCON"
	.dw 0xE1B1
	.dw KW_START
	.db 0xC6
	.ascii "WFDISC"
	.dw 0xE1B2
	.dw KW_START
	.db 0xC8
	.ascii "WFFORGET"
	.dw 0xE1B3
	.dw KW_START
	.db 0xC6
	.ascii "WFSTAT"
	.dw 0xE154
	.dw WFSTAT_FN
	.db 0xC6                   ; WFPING (2026-10-07, RP2350/net_ping.h): Wi-Fi's
	.ascii "WFPING"            ; PING (a BLPING could go through the Link's app)
	.dw 0xE1D3
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
	.ascii "LH5801 Expansion Card 0.6 "   ; exactly 26: a full line

; ---------------------------------------------------------------------
; FNCLR -- zeroes the function-key (reserve) definitions: the 195 bytes
; ending just before the BASIC program's start (7865H/7866H), which tend
; to get corrupted by a crash. Pure ROM, like ECVER: no MCU needed, so it
; works even when the MCU doesn't.
FN_KEYS_LEN .equ 195
FNCLR_ROUTINE:
	lda (BASIC_PROGRAM_START_HI_ABS)
	sta xh
	lda (BASIC_PROGRAM_START_LO_ABS)
	sta xl
	dec x                      ; the last byte of the definitions
	ldi ul,FN_KEYS_LEN
	ldi a,0x00
FNCLR_LOOP:
	sde x                      ; (X) = 0, X-1
	dec ul
	bzr FNCLR_LOOP
	vej 0xE2
SD_LIST_BLANK:                 ; a blank LCD line
	.ascii "                          "

; The CE-150's seven E6xx keywords under this module's own codes, E1C0-E1C6
; (2026-10-01): CSIZE, GRAPH, GLCURSOR, LCURSOR, SORGN, ROTATE, TEXT. A
; program typed without a CE-150 holds these codes (BASIC only looks for an
; E6xx code on the CE-150's own page), so when one IS attached, its own
; routine runs: found by code (E680 + the low nibble) in its keyword table
; at B054H, the layout every keyword page has (CE-150 ROM LB054), and
; entered as BASIC would, Y at the arguments. Otherwise -- no C0H at A000H
; and 55H at B000H (PV low), or the code isn't there -- the MCU's own
; (KW_START, straight below). Registers only: in STAGE RAM mode the MCU may
; be asleep, so nothing in the data window is safe to use yet.
CE150_E6_ENTRY:
	lda (0xA000)
	cpi a,0xC0
	bzr KW_START
	lda (0xB000)
	cpi a,0x55
	bzr KW_START
	dec y
	lda (y)                    ; the token's low byte: C0-C6
	inc y
	eai a,0x40                 ; the CE-150's: 80-86
	sta ul
	ldi xh,0xB0
	ldi xl,0x54
CE150_E6_SCAN:
	lda (x)                    ; an entry's marker: low nibble = name length
	ani a,0x0F
	bzs KW_START               ; the table's end: not there
	inc x
	adr x                      ; X at its code
	lin x
	cpi a,0xE6
	bzr CE150_E6_NEXT
	lda ul
	cpa (x)
	bzr CE150_E6_NEXT
	inc x                      ; found: its address
	lin x
	sta uh
	lda (x)
	sta xl
	lda uh
	sta xh
	stx p
CE150_E6_NEXT:                 ; X at the code's low byte: on to the next entry
	inc x
	inc x
	inc x
	bch CE150_E6_SCAN

KW_START:
	sjp EC_WAKE
	bcs SD_RAISE_ERROR_1_NO_DONE
	ldx s                      ; the stack pointer every exit runs at -- STSAVE
	lda xh                     ; saves it, STLOAD returns through it
	sta (KW_S_HI_ABS)
	lda xl
	sta (KW_S_LO_ABS)
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
	.dw KW_COPY_IN           ; EXP_KW_ACTION_COPY_IN
	.dw KW_COPY_OUT          ; EXP_KW_ACTION_COPY_OUT
	.dw KW_RESTORE           ; EXP_KW_ACTION_RESTORE
	.dw KW_POLL              ; EXP_KW_ACTION_POLL
	.dw KW_TERM              ; EXP_KW_ACTION_TERM

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
; through the general VEJ 0xE0 path with UH = the number. The number is
; read BEFORE EC_DONE (2026-09-27): in STAGE RAM mode DONE lets the MCU go
; DORMANT, and a sleeping MCU's window reads 0x00 -- reading it after DONE
; raced the MCU going to sleep and raised a garbage number (ERROR 237 seen
; live for a failed BLCON(NECT) that should have been ERROR 40).
KW_ERROR:
	lda (KW_ARG_ABS)
	psh a
	sjp EC_DONE
	pop a
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
; to BASIC. ARG 0 (view): Enter also returns. Otherwise ARG is the key
; that picks an entry (2026-09-27; it was always L): L for SDLOAD's Load,
; C for BLSCAN's Connect. Enter then does nothing -- deliberately, so the
; same listing serves both purposes without retyping SDLS first -- and the
; key on a real entry (not the summary line) blanks the line, puts the
; entry's index in ANSWER and continues.
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
	cpa (KW_ARG_ABS)           ; the pick key (0 = view only: no key is 0)
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
	bzs KW_EVAL_STRING
	cpi a,0xC1                 ; CHR$'s result (TRM p.123): C1H, then the same
	bzr KW_EVAL_DONE           ; address/length layout as D0H -- else a number
	ldi a,0xD0                 ; the MCU sees an ordinary string (2026-09-28:
	sta (EXP_BUFFER_START_ABS+4) ; a CHR$ was handed over as a number)
	lda (0x7A05)
	sta xh
	lda (0x7A06)
	sta xl
	lda (0x7A07)
	sta ul
	bch KW_EVAL_COPY
KW_EVAL_STRING:
	vej 0xDC                   ; X = string address, UL = its length
KW_EVAL_COPY:
	lda ul
	bzs KW_EVAL_DONE           ; ""
	ldi uh,0x00                ; Y is already at +8
	sjp SD_COPY_BYTES
KW_EVAL_DONE:
	jmp KW_CONTINUE
KW_EVAL_ERROR:
	sjp EC_DONE
	vej 0xE0

; COPY_IN / COPY_OUT: B bytes between RAM at A and EXP_BUFFER_START_ABS
; (FNSAVE/FNLOAD, STSAVE, most of STLOAD), then continue.
KW_COPY_IN:
	sjp KW_COPY_REGS
	bch KW_COPY_RUN
KW_COPY_OUT:
	sjp KW_COPY_REGS
	sjp KW_SWAP_XY
KW_COPY_RUN:
	sjp SD_COPY_BYTES
	jmp KW_CONTINUE

; X = A, Y = EXP_BUFFER_START_ABS, U = B. Clobbers A.
KW_COPY_REGS:
	lda (KW_A_HI_ABS)
	sta xh
	lda (KW_A_LO_ABS)
	sta xl
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	lda (KW_B_HI_ABS)
	sta uh
	lda (KW_B_LO_ABS)
	sta ul
	rtn

KW_SWAP_XY:
	psh x
	psh y
	pop x
	pop y
	rtn

; RESTORE: STLOAD's last step, for the stack page. 7800H-7BFFH holds the
; stack this very code runs on (and on a base PC-1500, 7C00H-7FFFH is a
; mirror of it), so it's written with interrupts off by inline loops that
; never touch the stack: copy B bytes from the window to A; if ARG says
; more follows, ask the MCU for the next block with an inline poll (not
; EC_SEND, which calls and HLTs) and repeat. Then S = the stack pointer
; STSAVE ran with (KW_S_*) -- the restored stack is STSAVE's -- and leave
; through KEYWORD_RETURN with STSAVE's own statement position, which the
; MCU has put back in KW_TEXT_* / EXP_KW_END_ABS: execution carries on
; right after the STSAVE that made the state.
KW_RESTORE:
	rie
KW_RESTORE_BLOCK:
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	lda (KW_A_HI_ABS)
	sta yh
	lda (KW_A_LO_ABS)
	sta yl
	lda (KW_B_HI_ABS)
	sta uh
	lda (KW_B_LO_ABS)
	sta ul
KW_RESTORE_COPY:
	tin
	dec u
	cpi uh,0x00
	bzr KW_RESTORE_COPY
	cpi ul,0x00
	bzr KW_RESTORE_COPY
	lda (KW_ARG_ABS)
	bzs KW_RESTORE_DONE
	ldi a,EXP_COMMAND_KEYWORD_CONTINUE
	sta (EXP_INSTRUCTION_ABS)
KW_RESTORE_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs KW_RESTORE_POLL
	bch KW_RESTORE_BLOCK
KW_RESTORE_DONE:
	lda (KW_S_HI_ABS)
	sta xh
	lda (KW_S_LO_ABS)
	sta xl
	stx s
	sie
	jmp KEYWORD_RETURN

; POLL (2026-09-28): sleep one timer wake, then check for BREAK, and put
; the result in ANSWER (1 = BREAK). A keyword that waits on the MCU
; (BLADV, BLPUT, BLGET) repeats this, so BREAK can cancel the wait. ARG
; bit 0 clears the latched BREAK flag first -- a wait's first POLL, so an
; old BREAK doesn't cancel a new wait. Bit 1 first shows the line at
; EXP_BUFFER_START_ABS (what it's waiting for), without SHOW's key wait.
;
; The BREAK test is the base ROM's own, VMJ A6 (E451H: bii #(F00BH),02H;
; Z clear = BREAK), the same test the interpreter makes between statements
; (ROM1 LC42A) and KEYSCAN_WAIT makes after each HLT wake (LE269). A BREAK
; seen here is cleared the way BASIC clears it after acting on one (ROM1
; LC4C5: ani #(F00BH),FDH), so it doesn't also stop the program afterwards
; -- the keyword decides. The HLT wait is EC_WAIT_NOT_BUSY's.
KW_POLL:
	bii (KW_ARG_ABS),0x01
	bzs KW_POLL_SHOW
	ani #(0xF00B),0xFD         ; IF bit 1: the latched BREAK flag
KW_POLL_SHOW:
	bii (KW_ARG_ABS),0x02
	bzs KW_POLL_WAIT
	ldi uh,>EXP_BUFFER_START_ABS
	ldi ul,<EXP_BUFFER_START_ABS
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
KW_POLL_WAIT:
	ldi a,0x57
	am0
	sie
	hlt
	vmj 0xA6
	bzs KW_POLL_NONE
	ani #(0xF00B),0xFD
	ldi a,0x01
	jmp KW_ANSWER
KW_POLL_NONE:
	ldi a,0x00
	jmp KW_ANSWER

; TERM (2026-10-07): the SSH terminal (RP2350/ssh_session.h). The MCU runs
; the session between commands and owns the line; this only shows it and
; reports the keys, a timer wake (~25ms) at a time, until the MCU says the
; session is over (TERM_CLOSED), then CONTINUEs. Each wake:
; - the line at TERM_LINE, if TERM_LINE_COUNT moved since it was drawn;
; - the matrix key held now (KBD_SCAN: the real keyboard's, else the
;   external one's; 0 = none) to TERM_KEY -- the MCU does SHIFT/DEF/SML,
;   the key's meaning and auto-repeat itself (ssh_keys.h);
; - ON, either keyboard's (KBD_BREAK, then the latch as KW_POLL tests it),
;   counted in TERM_BREAK_COUNT: Ctrl-C;
; - the MCU's SHIFT/DEF/SMALL (TERM_IND) into the LCD's indicators at
;   764EH -- the rest of that byte left alone, and all of it put back as it
;   was when the session ends (BASIC's own SML comes back).
; TERM_RUN is the loop itself, a subroutine: the command history's browsing
; and search (KBD_DISPATCH, 2026-10-07) run in it too.
KW_TERM:
	sjp TERM_RUN
	jmp KW_CONTINUE

TERM_RUN:
	ani #(0xF00B),0xFD         ; an old BREAK isn't this session's
	lda (STATUS1_ABS)
	sta (TERM_SAVED_IND_ABS)
	lda (TERM_LINE_COUNT_ABS)
	dec a
	sta (TERM_SHOWN_ABS)       ; drawn at the first wake
KW_TERM_LOOP:
	lda (TERM_CLOSED_ABS)
	bzs KW_TERM_DRAW
	lda (TERM_SAVED_IND_ABS)
	sta (STATUS1_ABS)
	rtn
KW_TERM_DRAW:
	lda (TERM_LINE_COUNT_ABS)
	cpa (TERM_SHOWN_ABS)
	bzs KW_TERM_KEYS
	sta (TERM_SHOWN_ABS)
	ldi uh,>TERM_LINE_ABS
	ldi ul,<TERM_LINE_ABS
	ldi xl,SD_LIST_LINE_WIDTH
	sjp DISP_N_CHARS0
KW_TERM_KEYS:
	lda (STATUS1_ABS)
	ani a,0xFF-TERM_IND_MASK
	ora (TERM_IND_ABS)
	sta (STATUS1_ABS)
	sjp KBD_SCAN
	bcr KW_TERM_HELD
	ldi xl,0x00                ; no key
KW_TERM_HELD:
	lda xl
	sta (TERM_KEY_ABS)
	sjp KBD_BREAK
	bzr KW_TERM_BREAK
	vmj 0xA6
	bzs KW_TERM_WAIT
	ani #(0xF00B),0xFD
KW_TERM_BREAK:
	lda (TERM_BREAK_COUNT_ABS)
	inc a
	sta (TERM_BREAK_COUNT_ABS)
KW_TERM_WAIT:
	ldi a,0x57
	am0
	sie
	hlt
	jmp KW_TERM_LOOP

; BLSTAT (2026-09-29) -- a no-argument FUNCTION, not a statement: S=BLSTAT,
; PRINT BLSTAT, IF BLSTAT>0..., or just BLSTAT at the prompt. Messages
; waiting in the inbox (0-8), or -1 with no link and none waiting.
;
; BASIC's own functions are ordinary keyword-table entries too; the code's
; low byte makes one a function (ROM1's evaluator, LD8AD): 5xH takes no
; argument (MEM F158, TIME F15B, INKEY$ F15C, PI F15D), 6xH-7xH one (COS
; F17E). The evaluator calls the routine and expects the value in the
; arithmetic register (7A00H-7A07H) and UH = 0, or UH = an error number --
; MEM's own exit (LDA41: ldi uh,0 / rtn) after vmj 10H, which builds the
; number there. Here the MCU builds it (keywords.c's kw_function) and this
; copies it in.
;
; EC_DONE only when the MCU's reply says no keyword is running (byte 8): a
; function can be evaluated in the middle of another keyword (BLPRINT
; BLSTAT), and DONE there would let a STAGE RAM MCU sleep under it. Sent
; after the value is read -- DONE lets the MCU go DORMANT at once, and a
; sleeping MCU's window reads 0x00 (2026-09-30: the first version had the
; MCU request sleep itself, which beat the ROM to the value: ERROR 1).
;
; SDEOF(n) (2026-09-30) is the same with one argument, code 6xH-7xH: the
; evaluator has already evaluated it into the arithmetic register when the
; routine is called (PEEK and CHR$ start by converting it, with VEJ D0) --
; so FN_CALL hands the register's 8 bytes to the MCU, which reads the
; number itself (keywords.c). Every function goes through FN_CALL, with its
; EXP_COMMAND_FN_* in A.
BLSTAT_FN:
	ldi a,EXP_COMMAND_FN_BLSTAT
	bch FN_CALL
WFSTAT_FN:                     ; WFSTAT (2026-10-06): 0 off, 1 connecting, 2 connected
	ldi a,EXP_COMMAND_FN_WFSTAT
	bch FN_CALL

; BLKEY$ (2026-10-06) -- INKEY$ (F15C, ROM1 LD9AA) for either keyboard,
; with no MCU round trip: the key down on the PC-1500's own keyboard
; (KEYSCAN_NOWAIT, as INKEY$), else the external keyboard's (the matrix
; index the MCU publishes, through the same code table, as KBD_SCAN), else
; none. INKEY$'s own tail (LD9B6, X high = D0H) makes A the 1-character
; string, or "" for 0 -- the same in every known ROM. The external key only
; while the driver is armed (79D4H = 55H, the vector = KBD_HOOK): without it
; the MCU may be asleep, and a read of the window would wake it on every
; poll. A load turns a program's INKEY$ into this with MCONF BLKBD=1, and a
; save turns it back (RP2350/basic_xlate.h).
BLKEY_FN:
	sjp 0xE42C                 ; Carry clear: A = the key's code
	bcr BLKEY_FN_KEY
	lda (0x79D4)
	cpi a,0x55
	bzr BLKEY_FN_NONE
	lda (0x785B)
	cpi a,>KBD_HOOK
	bzr BLKEY_FN_NONE
	lda (0x785C)
	cpi a,<KBD_HOOK
	bzr BLKEY_FN_NONE
	lda (KBD_KEY_ABS)
	bzs BLKEY_FN_KEY           ; none: A = 0
	sta xl
	ldi xh,0xFE
	lda (x)
	bch BLKEY_FN_KEY
BLKEY_FN_NONE:
	ldi a,0x00
BLKEY_FN_KEY:
	ldi xh,0xD0
	jmp 0xD9B6
SDEOF_FN:
	ldi a,EXP_COMMAND_FN_SDEOF
FN_CALL:
	psh x
	psh y
	sjp EC_WAKE                ; (keeps A)
	bcs FN_CALL_NO_MCU
	psh a
	ldi xh,0x7A                ; the register (the argument, if any) -> the window;
	ldi xl,0x00                ; after EC_WAKE: a sleeping MCU drops writes
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,0x00
	ldi ul,0x08
	sjp SD_COPY_BYTES
	pop a
	sjp EC_SEND
	cpi a,EXP_STATUS_SUCCESS
	bzr FN_CALL_ERROR
	ldi xh,>EXP_BUFFER_START_ABS
	ldi xl,<EXP_BUFFER_START_ABS
	ldi yh,0x7A
	ldi yl,0x00
	ldi uh,0x00
	ldi ul,0x08
	sjp SD_COPY_BYTES          ; the 8-byte value -> 7A00H
	sjp FN_CALL_END
	pop y
	pop x
	ldi uh,0x00
	rtn
FN_CALL_ERROR:                 ; the MCU says which error (the reply's byte
	lda (EXP_BUFFER_START_ABS+9) ; 9) -- read before DONE, which lets it sleep
	bzr FN_CALL_ERROR_N
	ldi a,0x01                 ; (none given: ERROR 1)
FN_CALL_ERROR_N:
	psh a
	sjp FN_CALL_END
	pop a
	sta uh
	pop y
	pop x
	rtn
FN_CALL_NO_MCU:                ; no MCU: ERROR 1, as a keyword whose MCU
	pop y                      ; doesn't answer
	pop x
	ldi uh,0x01
	rtn

; DONE if the MCU said no keyword is running (the reply's byte 8); inside
; one, that keyword's own DONE comes later. Clobbers A.
FN_CALL_END:
	lda (EXP_BUFFER_START_ABS+8)
	bzs FN_CALL_END_RET
	sjp EC_DONE
FN_CALL_END_RET:
	rtn

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
	cpi a,EXP_STATUS_SUCCESS     ; the MCU says the transfer failed (2026-09-27:
	bzr KW_XFER_FAILED           ; a BLE link can drop part-way) -- ERROR 40
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
	cpi a,EXP_STATUS_SUCCESS
	bzr KW_XFER_FAILED
	jmp KEYWORD_RETURN

; A LOAD or SAVE whose CLOSE wasn't SUCCESS: the MCU found the transfer
; incomplete (BLLOAD/BLSAVE over a link that failed; an SD CLOSE is
; SUCCESS). ERROR 40, as for any other file failure.
KW_XFER_FAILED:
	ldi a,0x28                   ; 40
	sta (KW_ARG_ABS)
	jmp KW_ERROR

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
; same early exit, from the MCU (keywords.c). And it's skipped altogether
; unless MCONF AUTOSTAGE=1 (ROM_GET_MODE's third byte; default 0).
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
;
; MCONF BLKBD=1 (2026-10-04) adds the external keyboard's driver: before
; any staging (the stage copies whatever the MCU's ROM image then holds),
; KBD_BOOT_COPY gives the MCU ROM1's wait loop to put in at KBD_LOOP; and
; once the ROM is served as it will stay, KBD_ARM sets the base ROM's
; keyboard hook to it -- reset has just cleared 79D4H (at F765H, before the
; module scan at E107H; seen in pc1500emu).
STAGE_BOOT_ENTRY:
	sjp EC_WAKE
	bcs STAGE_BOOT_ENTRY_NO_MCU
	sjp STAGE_IS_STAGED
	lda (EXP_BUFFER_START_ABS+3)   ; MCONF BLKBD
	bzs STAGE_BOOT_ENTRY_STAGED    ; (LDA/BZS leave Carry alone)
	sjp KBD_BOOT_COPY
	sjp STAGE_IS_STAGED            ; again: a staged copy without the loop is now stale
STAGE_BOOT_ENTRY_STAGED:
	bcs STAGE_BOOT_ENTRY_DONE      ; verified copy already in SRAM
	lda (EXP_BUFFER_START_ABS+2)   ; MCONF AUTOSTAGE (2026-09-28): 0 = don't
	bzs STAGE_BOOT_ENTRY_DONE      ; stage at boot (also a failed query)
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
	sjp STAGE_IS_STAGED            ; BLKBD again: staging used the window
	lda (EXP_BUFFER_START_ABS+3)
	bzs STAGE_BOOT_ENTRY_END
	sjp KBD_ARM
STAGE_BOOT_ENTRY_END:
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
; that one SIE+HLTs, and the boot hook runs with interrupts off. Clears
; the AUTOSTAGE and BLKBD bytes (+2, +3) first, so a failed query leaves
; them 0: no staging, no keyboard.
STAGE_IS_STAGED:
	ldi a,0x00
	sta (EXP_BUFFER_START_ABS+2)
	sta (EXP_BUFFER_START_ABS+3)
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
; The external keyboard's driver (2026-10-04, MCONF BLKBD) -- for a BLE
; keyboard, whose key the MCU publishes as a matrix index (EXP_KBD_KEY,
; RP2350/kbd_seq.h). It sits on the base ROM's own keyboard hook, the one
; BASWORD uses: with 79D4H = 55H, KEYSCAN_WAIT (E243H) jumps through the
; vector at 785BH/785CH instead of scanning (ROM1 E2B7: VEJ CCH loads X from
; there, then PV = the vector's bit 0, RIE, STX P). KBD_ARM sets it to
; KBD_HOOK at boot.
;
; The driver is ROM1's own wait loop, E24AH-E365H (KEYSCAN_WAIT past its
; hook test, through AUTO_POWER_OFF), so the external key goes through the
; ROM's own code table, debounce, auto-repeat and SHIFT/DEF/SML handling
; (SML_DISPATCH, E366H) like a physical one. ROM1's E24AH itself can't be
; used (BASWORD calls it): it waits in its own HLT loop until a matrix key
; goes down, and would never look at the external one. This module doesn't
; carry Sharp's code: KBD_BOOT_COPY copies the loop from the machine's own
; ROM, and the MCU checks it (a CRC of ROM1's) and patches it in at KBD_LOOP
; (kbd_seq.c kbd_loop_install(), from the addresses at ROM_BASE+11H):
; - its keyboard reads call KBD_ANY and KBD_SCAN, which add the external
;   key to the real matrix -- and KBD_ANY answers the external ON (BREAK);
; - its two branches to SML_DISPATCH go to KBD_DISPATCH, which turns OFF
;   into the loop's own power-off (below);
; - its power-off resumes its own loop rather than ROM1's.
; In place, every other byte as ROM1 has it -- the loop's own branches keep
; their targets.
;
; OFF: handed to BASIC, OFF rewrites 785BH/785CH (seen in pc1500emu) while
; 79D4H stays 55H, so the next KEYSCAN_WAIT after ON would jump into
; nowhere. BASWORD turns OFF into AUTO_POWER_OFF for the same reason.

; The hook's target: the loop at KBD_LOOP if it's there -- its first byte
; is ROM1's (the MCU doesn't patch that one) -- else ROM1's own loop (an MCU
; restarted under a running PC-1500 has lost it).
KBD_ENTRY:
	lda (KBD_LOOP)
	cpa (0xE24A)
	bzr KBD_ENTRY_ROM1
	jmp KBD_LOOP
KBD_ENTRY_ROM1:
	jmp 0xE24A

KBD_LOOP_LEN .equ 284          ; RP2350/kbd_seq.h KBD_LOOP_LEN
KBD_LOOP:
	.blkb KBD_LOOP_LEN

; Where the loop hands a key to SML_DISPATCH, X = FE00H + its matrix
; index: OFF goes to the loop's own power-off (ROM1 E33FH) instead.
;
; The command history (2026-10-07, MCONF HISTORY, RP2350/cmd_history.h),
; in RUN mode at BASIC's prompt only -- not in PRO mode, and not while a
; program runs (788AH bit 40H, BASIC's run state: 50H at an INPUT, 00H at
; the prompt -- measured in pc1500emu, and what ROM1's CONT sets):
; - ENTER on a line typed or edited (7880H = 40H, the line editor's mode
;   byte) first hands the line at 7BB0H to the MCU (HIST_ADD) -- still
;   plain text here, ROM1 tokenizes it in place after;
; - DEF+Up/Down (browse) or DEF+Left (search), which ROM1 treats as the
;   plain key: the MCU shows the history in TERM_RUN instead, then gives
;   back the line for 7BB0H and what to do with it. RUN, EDIT and CANCEL
;   put the line up as being typed, with ROM1's DISP_BUFFER (not its
;   recall, which leaves the cursor where it was); RUN then hands ROM1 an
;   ENTER, as if typed. With no history (or MCONF HISTORY=0) the key is
;   ROM1's as always.
KBD_DISPATCH:
	lda (x)
	cpi a,KEY_OFF
	bzr KBD_DISPATCH_HIST
	jmp KBD_LOOP+(0xE33F-0xE24A)
KBD_DISPATCH_HIST:
	bii (0x764F),0x40              ; RUN mode
	bzs KBD_DISPATCH_ROM1
	bii (0x788A),0x40              ; a program running: its INPUT
	bzr KBD_DISPATCH_ROM1
	cpi a,0x0D
	bzs HIST_ENTER
	bii (STATUS1_ABS),0x80         ; DEF
	bzs KBD_DISPATCH_ROM1
	cpi a,0x0B                     ; Up
	bzs HIST_OLDER
	cpi a,0x0A                     ; Down
	bzs HIST_NEWER
	cpi a,0x08                     ; Left
	bzs HIST_SEARCH
KBD_DISPATCH_ROM1:
	jmp 0xE366

HIST_ENTER:
	lda (0x7880)
	cpi a,0x40                     ; a line typed (not ENTER on a result)
	bzr KBD_DISPATCH_ROM1
	psh x
	psh y
	psh u
	sjp EC_WAKE
	bcs HIST_ENTER_END             ; no MCU: just ENTER
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	sjp HIST_LINE_OUT
	ldi a,EXP_COMMAND_HIST_ADD
	sjp EC_SEND
	sjp EC_DONE
HIST_ENTER_END:
	pop u
	pop y
	pop x
	jmp 0xE366

HIST_OLDER:
	ldi a,EXP_HIST_START_OLDER
	bch HIST_OPEN
HIST_NEWER:
	ldi a,EXP_HIST_START_NEWER
	bch HIST_OPEN
HIST_SEARCH:
	ldi a,EXP_HIST_START_SEARCH
HIST_OPEN:
	psh x
	psh y
	psh u
	sjp EC_WAKE                    ; (keeps A)
	bcs HIST_PLAIN
	sta (EXP_BUFFER_START_ABS)     ; how
	ldi yh,>(EXP_BUFFER_START_ABS+1)
	ldi yl,<(EXP_BUFFER_START_ABS+1)
	sjp HIST_LINE_OUT
	ldi a,EXP_COMMAND_HIST_BEGIN
	sjp EC_SEND
	cpi a,EXP_STATUS_SUCCESS
	bzr HIST_PLAIN_DONE            ; none, or HISTORY=0: the key as always
	sjp TERM_RUN
	ldi xh,>HIST_RESULT_ABS        ; the line back to 7BB0H
	ldi xl,<HIST_RESULT_ABS
	ldi yh,0x7B
	ldi yl,0xB0
	ldi uh,0x00
	ldi ul,HIST_LINE_LEN
	sjp SD_COPY_BYTES
	lda (HIST_RESULT_ABS+HIST_LINE_LEN) ; its length: the cursor at its end, as
	rec                            ; ROM1's recall leaves 787BH (08H + the
	adi a,0x08                     ; position) as it finds it (ADI adds the
	sta (0x787B)                   ; carry too)
	lda (TERM_CLOSED_ABS)          ; what to do with it
	psh a
	sjp EC_DONE
	pop a
	pop u
	pop y
	pop x
	; RUN, EDIT, CANCEL, BREAK: the line up as being typed (an empty one too), the cursor at
	; its end -- ROM1's DISP_BUFFER (E8CAH) draws 7BB0H with the cursor at Y
	; -- then, for RUN, ROM1's ENTER at once, as if typed; else on to the
	; next key. Not ROM1's recall (7880H = 20H, Left): it leaves the cursor
	; where the last line run left it, and for RUN that put ROM1's blinking
	; block (787CH) on the run keyword's display -- SDLS's listing -- where a
	; typed ENTER leaves none (measured in pc1500emu, 2026-10-08). CANCEL or
	; BREAK with no line being typed (7880H not 40H -- 00H at a bare prompt,
	; measured in pc1500emu; nothing in TERM_RUN changes it) draws nothing
	; and goes on as BREAK: ROM1's own prompt, no cursor (2026-10-08)
	psh a                          ; the result: BREAK goes on to ROM1
	cpi a,EXP_HIST_EDIT
	bzs HIST_EDIT_LINE
	cpi a,EXP_HIST_RUN
	bzs HIST_EDIT_LINE
	lda (0x7880)
	cpi a,0x40
	bzr HIST_EDIT_KEYS             ; nothing typed: as BREAK, below
HIST_EDIT_LINE:
	ldi a,0x40
	sta (0x7880)
	ldi yh,0x7B
	lda (0x787B)                   ; 08H + the length (above)
	rec
	adi a,0xA8                     ; 7BB0H + the length
	sta yl
	bcr HIST_EDIT_DRAW
	inc yh
HIST_EDIT_DRAW:
	sjp 0xE8CA
HIST_EDIT_KEYS:
	ani (STATUS1_ABS),0x7D         ; as ROM1's dispatch leaves a key (E366H): DEF
	ori (0x7B0E),0x01              ; and SHIFT used up, its key gate shut (the
	pop a                          ; held Left isn't a key again)
	cpi a,EXP_HIST_BREAK
	bzs HIST_DO_BREAK
	cpi a,EXP_HIST_RUN
	bzs HIST_DO_RUN
	cpi a,EXP_HIST_EDIT
	bzs HIST_NEXT_KEY
	lda (0x7880)                   ; CANCEL with no line typed: as BREAK
	cpi a,0x40
	bzr HIST_DO_BREAK
HIST_NEXT_KEY:
	rie                            ; interrupts off, as the hook enters the
	jmp KBD_LOOP                   ; wait (E2B7H): the next key
HIST_DO_BREAK:                     ; BREAK: KEYSCAN_WAIT's own, to its caller
	ldi a,0x0E                     ; (ROM1 E33AH, as KBD_ANY gives the external ON)
	sec
	rtn
HIST_DO_RUN:                       ; RUN: ENTER, as KBD_DISPATCH hands a typed one on
	ldi xh,0xFE
	ldi xl,0x98                    ; ENTER
	jmp 0xE366
HIST_PLAIN_DONE:
	sjp EC_DONE
HIST_PLAIN:
	pop u
	pop y
	pop x
	jmp 0xE366                     ; X still the key

; 7BB0H's 80 bytes to the window at Y: HIST_ADD's at its start, HIST_BEGIN's
; after its "how" byte.
HIST_LINE_OUT:
	ldi xh,0x7B
	ldi xl,0xB0
	ldi uh,0x00
	ldi ul,HIST_LINE_LEN
	sjp SD_COPY_BYTES
	rtn

; LE418's "is any key down?" (A nonzero, Z clear if so), counting the
; external keyboard's key too. The loop calls this right after its own ON
; test, so this also gives the external ON the same return: KEYSCAN_WAIT's
; caller gets BREAK (Carry set, A = 0EH, ROM1 E33AH) -- dropping this
; call's own return address first.
KBD_ANY:
	sjp KBD_BREAK
	bzr KBD_ANY_BREAK
	sjp 0xE418
	bzr KBD_ANY_RET
	sjp KBD_EXT
	bzs KBD_ANY_RET                ; no external keyboard: A = 0
	lda (KBD_KEY_ABS)
KBD_ANY_RET:
	rtn
KBD_ANY_BREAK:
	pop u
	ldi a,0x0E
	sec
	rtn

; KEYSCAN_NOWAIT (E42CH): Carry clear, X = FE00H + the key's matrix index
; and A = its code from there -- the real matrix's key first, else the
; external keyboard's. Carry set and A = 0 for no key, as E42CH leaves it.
KBD_SCAN:
	sjp 0xE42C
	bcr KBD_SCAN_RET
	sjp KBD_EXT
	bzs KBD_SCAN_NONE
	lda (KBD_KEY_ABS)
	bzs KBD_SCAN_NONE
	sta xl
	ldi xh,0xFE
	lda (x)
	rec
KBD_SCAN_RET:
	rtn
KBD_SCAN_NONE:
	sec
	rtn

; Z clear when the external keyboard's ON count is ahead of what's been
; acted on -- which this then acknowledges (a BREAK per press, as the real
; ON key's latch gives). Clobbers A.
KBD_BREAK:
	sjp KBD_EXT
	bzs KBD_BREAK_RET              ; no external keyboard: none (Z set)
	lda (KBD_BREAK_ABS)
	cpa (KBD_ACK_ABS)
	bzs KBD_BREAK_RET
	sta (KBD_ACK_ABS)
KBD_BREAK_RET:
	rtn

; Z clear if the external keyboard is in use: the hook goes to KBD_HOOK.
; Armed for the command history alone (KBD_HOOK_LOCAL), its window bytes
; aren't kept up -- and a sleeping MCU's window can't be read -- so they're
; never looked at then (2026-10-07). Clobbers A.
KBD_EXT:
	lda (0x785B)
	cpi a,>KBD_HOOK
	bzr KBD_EXT_NO
	lda (0x785C)
	cpi a,<KBD_HOOK
	bzr KBD_EXT_NO
	ldi a,0x01
	rtn
KBD_EXT_NO:
	ldi a,0x00
	rtn

; The hook's entry for the command history alone (MCONF HISTORY without
; BLKBD): the same driver, at an address KBD_EXT tells apart. Even, as
; KBD_HOOK: the hook runs it with PV low.
	.even
KBD_HOOK_LOCAL:
	jmp KBD_ENTRY

; Boot (STAGE_BOOT_ENTRY, interrupts off): unless KBD_LOOP already holds
; the loop, copy ROM1's to the window for the MCU to check and put in.
; Keeps U and Y (the base ROM's module scan needs UH); no HLT.
KBD_BOOT_COPY:
	lda (KBD_LOOP)
	cpa (0xE24A)
	bzs KBD_BOOT_COPY_RET
	psh u
	psh y
	ldi xh,0xE2
	ldi xl,0x4A
	ldi yh,>EXP_BUFFER_START_ABS
	ldi yl,<EXP_BUFFER_START_ABS
	ldi uh,>KBD_LOOP_LEN
	ldi ul,<KBD_LOOP_LEN
	sjp SD_COPY_BYTES
	ldi a,EXP_COMMAND_KBD_INSTALL
	sta (EXP_INSTRUCTION_ABS)
KBD_BOOT_COPY_POLL:
	lda (EXP_INSTRUCTION_ABS)
	cpi a,EXP_STATUS_BUSY
	bzs KBD_BOOT_COPY_POLL
	pop y
	pop u
KBD_BOOT_COPY_RET:
	rtn

; Boot: the hook, if the loop is being served at KBD_LOOP -- a staged copy
; made before the loop was put in doesn't have it (STAGE RAM again, or
; AUTOSTAGE, brings it in). A = ROM_GET_MODE's arm flags (2026-10-07):
; bit 0 MCONF BLKBD (KBD_HOOK, the external keyboard read), else HISTORY
; alone (KBD_HOOK_LOCAL).
KBD_ARM:
	sta xl
	lda (KBD_LOOP)
	cpa (0xE24A)
	bzr KBD_ARM_RET
	lda xl
	ani a,0x01
	bzs KBD_ARM_LOCAL
	ldi a,>KBD_HOOK
	sta (0x785B)
	ldi a,<KBD_HOOK
	sta (0x785C)
	bch KBD_ARM_ON
KBD_ARM_LOCAL:
	ldi a,>KBD_HOOK_LOCAL
	sta (0x785B)
	ldi a,<KBD_HOOK_LOCAL
	sta (0x785C)
KBD_ARM_ON:
	ldi a,0x55
	sta (0x79D4)
KBD_ARM_RET:
	rtn

; ---------------------------------------------------------------------
; Guard: everything above must fit in the 6K ROM region (0x8800-0x9FFF).
; .org can only move the location counter forward within one absolute area
; -- if the content above already overran past 0xA000, this line itself
; fails to assemble instead of silently wrapping past the window.
	.org ROM_REGION_END
