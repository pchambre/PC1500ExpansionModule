; ern.asm -- regression test for the original "ERN" test keyword.
;
; Byte-identical transcription of the original rom[1][0..7] (name-table
; entry) and rom[2][0..5] (routine) from main.c, before ERN/ERL/INVERT were
; demoted out of the production ROM image (see ../rom.asm's header comment).
;
; NOT confirmed to be a real, walkable BASIC keyword-table entry -- the
; name-table byte layout here (0xC5 marker + name + 0xF0 + 2 bytes) doesn't
; obviously match the marker-encodes-name-length format documented in
; PC1500_BASIC_Keyword_Extension_Mechanism.md (marker low nibble 5 vs. a
; 3-letter name "ERN"). It may only ever have been exercised by directly
; CALLing the routine, not by typing ERN in BASIC. Test via `call` at
; ERN_ROUTINE, not by relying on BASIC's own keyword recognition, unless
; you've independently confirmed the table format on real hardware first.
;
; Routine: loads the real ROM's ERL system variable (0x789B, "last error
; number" per pc1500disasm's known-symbols table) and jumps into the ROM's
; own number-printing routine at 0xD9E4.

	.area CODE (ABS)
	.org 0x8000

ERN_NAME_ENTRY:
	.db 0xC5, 'E', 'R', 'N', 0xF0, 0x5E, 0x82, 0x00

ERN_ROUTINE:
	lda (0x789B)   ; ERL -- last error number
	jmp 0xD9E4     ; ROM's own number-printing continuation
