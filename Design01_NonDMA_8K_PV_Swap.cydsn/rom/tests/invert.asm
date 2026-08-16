; invert.asm -- regression test for the original "display invert" ML routine.
;
; Byte-identical transcription of the original rom[3][0..22] from main.c.
; Not wired to any name-table entry in the original code (no page-1 entry
; points at page 3) -- it looks like a standalone utility meant to be
; CALLed directly from a BASIC loader (like the memcopy routines in
; ../rom.asm), not a real BASIC keyword, despite the "custom keyword"
; comment header it sat under in the original InitBuffer(). Ends in a
; plain RTN, consistent with that: a routine reached via BASIC's own
; keyword dispatch must end STX P instead (see
; PC1500_BASIC_Keyword_Extension_Mechanism.md sec. 6, and the
; pc1500_invert_error1_fix memory note -- that confirmed fix concerns a
; different, real INVERT keyword elsewhere, not this routine).
;
; Behavior: scans forward from 0x7600 for the first byte whose bitwise NOT
; is even (LSB clear after XOR 0xFF), incrementing through 0x7600-0x76FF
; then 0x7700-0x77FF, writing the complement back -- consistent with
; toggling a screen/attribute buffer's invert bit.

	.area CODE (ABS)
	.org 0x8000

INVERT_ROUTINE:
	ldi xh,0x76
	ldi xl,0x00
INVERT_SCAN:
	lda (x)
	eai a,0xFF
	sin x
	cpi xl,0x4E
	bzr INVERT_SCAN
	cpi xh,0x77
	bzs INVERT_DONE
	ldi xh,0x77
	ldi xl,0x00
	bch INVERT_SCAN
INVERT_DONE:
	rtn
