; keyprobe.asm -- throwaway diagnostic keyword ("KTEST") that calls the base
; ROM's own KEYSCAN_WAIT (E243H) and records its raw return value, so real
; key codes for CL/ENTER/UP/DOWN can be read back with `peek` instead of
; guessed from documentation. Not part of the production rom.asm build.
; Single index-reached entry, so none of the multi-entry quirks documented
; in rom.asm/rom_defs.inc apply here.
;
; Result storage is 0x4100/0x4101 (real user RAM, well clear of the
; 0x4000-0x40C4 reserve area), NOT 0x8000 -- confirmed via pc1500emu's own
; source (src/bus/bus.h's isUnmapped()) that ALL writes to 0x8000-0xBFFF are
; unconditionally discarded in this emulator (it models real CE-150-style
; cartridges, which have no writable component at all), regardless of any
; ROM module loaded there. A `STA (0x8000)` silently no-ops every time.
;
; Type KTEST and press Enter -- it blocks waiting for a keypress. Press the
; key to measure, then read back:
;   peek 4100  -- raw ACC key code returned by KEYSCAN_WAIT
;   peek 4101  -- 0xFF if Carry was set (BREAK per KEYSCAN_WAIT's own doc), else 0x00

	.area CODE (ABS)
	.org 0x9000

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
	.dw KEYWORD_TABLE+2  ; K
	.dw 0x0000  ; L
	.dw 0x0000  ; M
	.dw 0x0000  ; N
	.dw 0x0000  ; O
	.dw 0x0000  ; P
	.dw 0x0000  ; Q
	.dw 0x0000  ; R
	.dw 0x0000  ; S
	.dw 0x0000  ; T
	.dw 0x0000  ; U
	.dw 0x0000  ; V
	.dw 0x0000  ; W
	.dw 0x0000  ; X
	.dw 0x0000  ; Y
	.dw 0x0000  ; Z

KEYWORD_TABLE:
	.db 0xD5  ; index-reached directly -- marker high nibble doesn't matter
	.ascii "KTEST"
	.dw 0xE285
	.dw KTEST_ROUTINE
	.db 0xD0  ; table terminator

KTEST_ROUTINE:
	sjp 0xE243        ; KEYSCAN_WAIT -- blocks until a key is down; ACC=code, Carry=1 only for BREAK
	sta (0x4100)      ; raw key code, regardless of carry
	bcs KTEST_BREAK
	ldi a,0x00
	sta (0x4101)
	jmp KEYWORD_RETURN
KTEST_BREAK:
	ldi a,0xFF
	sta (0x4101)
	; fall through

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

	.org 0xA000
