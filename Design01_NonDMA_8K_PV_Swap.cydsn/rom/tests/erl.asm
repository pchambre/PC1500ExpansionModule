; erl.asm -- regression test for the original "ERL" test keyword.
;
; Byte-identical transcription of the original rom[1][8..15] (name-table
; entry) and rom[2][6..11] (routine) from main.c. See ern.asm's header for
; the same caveat about the name-table format's real-hardware validity.
;
; Routine: this is very likely a bug in the ORIGINAL PSoC ROM, preserved
; here verbatim rather than silently corrected. Byte 0 is 0xF4 (VEJ 0xF4 --
; a one-byte vector call to 0xFFF4, transferring control permanently), not
; 0xA5 (LDA absolute) as ERN's otherwise-identical-shaped routine uses. If
; the intent was to mirror ERN (LDA (0x78B4); JMP 0xDA6C -- reading a
; different system variable and a different ROM continuation), someone
; likely mistyped the first opcode byte when copying ERN's routine. The
; remaining 5 bytes (0x78,0xB4,0xBA,0xDA,0x6C) are consequently dead code:
; unreachable after the VEJ transfers control away. Flagged for the user;
; not fixed here since this keyword was test-only, not shipped.

	.area CODE (ABS)
	.org 0x8000

ERL_NAME_ENTRY:
	.db 0xC5, 'E', 'R', 'L', 0xF0, 0x5F, 0x82, 0x06

ERL_ROUTINE:
	vej 0xF4
	.db 0x78, 0xB4, 0xBA, 0xDA, 0x6C   ; dead bytes -- see header comment
