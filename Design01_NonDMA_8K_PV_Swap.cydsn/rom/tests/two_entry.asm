; two_entry.asm -- diagnostic for the multi-entry dispatch bug.
;
; Minimal 2-entry table, SLOAD then SSAVE, both 5 letters, alphabetically
; ordered -- deliberately mirrors ce150.asm's real CLOAD/CSAVE pair
; structurally as closely as possible (same lengths, same relative order,
; same second-letter progression L-then-S) to isolate whether MY table
; format itself is broken, or something specific to the full 6-entry
; production table.

	.area CODE (ABS)
	.org 0x8000

	.db 0x55
	.blkb 31

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
	.dw KEYWORD_TABLE+2  ; S
	.dw 0x0000  ; T
	.dw 0x0000  ; U
	.dw 0x0000  ; V
	.dw 0x0000  ; W
	.dw 0x0000  ; X
	.dw 0x0000  ; Y
	.dw 0x0000  ; Z

KEYWORD_TABLE:
	.db 0xD5
	.ascii "SLOAD"
	.dw 0xE887
	.dw SLOAD_ROUTINE

	.db 0xD5
	.ascii "SSAVE"
	.dw 0xE886
	.dw SSAVE_ROUTINE

	.db 0xD0  ; table terminator

KEYWORD_RETURN:
	pop a
	ldi a,0xCA
	sta (0x784E)
	ldi a,0x92
	sta (0x784F)
	ani (0x764E),0xFE
	ori (0x7874),0x01
	ldi xh,0xE2
	ldi xl,0xAA
	stx p

SLOAD_ROUTINE:
	jmp KEYWORD_RETURN

SSAVE_ROUTINE:
	jmp KEYWORD_RETURN
