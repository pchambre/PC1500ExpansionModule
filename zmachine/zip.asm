; zip.asm -- ZIP/1500, a version-3 Z-machine for the PC-1500 with this
; project's expansion module. See DESIGN.md for the design; in short:
;
;
; - The story is on an SDOPEN channel (STCH) ZIP opens itself: GAME FILE:
;   at the start (INIT), the last game ready -- kept in the MCU's FL store
;   as ZIPGAME -- and F1 the card's directory. Its full path stays in STORY,
;   so after a power cycle, which the PC-1500's RAM and the game survive but
;   the MCU's channels and console don't, ZIP reopens both (REVIVE) and
;   plays on. Only documented module commands are used. Dynamic
;   memory stays resident from ZBASE; every other page is read on demand
;   with SD_READ_BYTES into a 256-byte swap page, least recently used out.
; - Text goes to the MCU's console with EXP_COMMAND_ZTERM, which keeps the
;   transcript, renders the LCD line and edits the input line. Every key
;   goes there too, so this side has no line editor or scrollback.
; - Before every MCU request this side wakes the MCU, and before every
;   wait for a key it sends DONE, so the MCU may sleep (DORMANT or POWMAN).
;
; Registers inside the interpreter: Y is the stream pointer -- the Z PC,
; or the string being decoded -- as RAM page:offset, its Z page in SPAGE.
; Values travel in X (XH:XL); U is scratch. A Z page is always a whole
; 256-byte RAM page, so YL is the offset in either space.
;
; Built with build.ps1 into ZIP, an M file: SDLOAD M "ZIP" loads it here
; and calls ENTRY.

; ---- the module ----
W        = 0x8000        ; the data window
INSTR    = 0x87FF        ; the instruction/status cell
ST_BUSY  = 1
ST_OK    = 2
ST_READY = 4             ; C_READY's answer
ST_AWAKE = 5             ; C_WAKE's: the MCU up
C_DONE   = 0x2C
C_RDB    = 0x3C          ; EXP_COMMAND_SD_READ_BYTES
C_WRB    = 0x3D          ; EXP_COMMAND_SD_WRITE_BYTES
C_OPEN   = 0x18          ; EXP_COMMAND_SD_OPEN_CHANNEL
C_CLOSE  = 0x19          ; EXP_COMMAND_SD_CLOSE_CHANNEL
LENP     = 0x87FD        ; the length port: READ_BYTES' count, 2 bytes BE
C_ZTERM  = 0x7D          ; EXP_COMMAND_ZTERM
C_FLGET  = 0x7E          ; EXP_COMMAND_FL_GET: a value in the MCU's FL store
C_FLSET  = 0x7F          ; EXP_COMMAND_FL_SET
C_CWD    = 0x13          ; EXP_COMMAND_GET_SD_CWD: [len][path] at W+100H
STCH     = 15            ; the story's SDOPEN channel
C_WAKE   = 0xFF          ; the wake's two phases (RP2350/pc_exp.h EXP_COMMAND_WAKE)
C_READY  = 0xFE
ZT_OPEN   = 1
ZT_TEXT   = 2
ZT_STATUS = 3
ZT_KEY    = 4
ZT_CLOSE  = 5
ZT_FILE   = 6            ; the save file's prompt
ZT_PROMPT = 7            ; a prompt of ours: GAME FILE:
ZK_DONE   = 1            ; KEY's answer: the input line is finished

; ---- the base ROM ----
KEYWAIT  = 0xE243        ; KEYSCAN_WAIT: A = key, Carry = BREAK
KEYOFF   = 0x0F          ; its code for the OFF key
; AUTO_POWER_OFF, KEYSCAN_WAIT's own: switches off; after ON, the ROM
; restores S and runs KEYSCAN_WAIT again, which returns to the caller with
; the next key. The same in every ROM revision (A01 and A04 compared).
PWROFF   = 0xE33F
DISP26   = 0xED3B        ; DISP_N_CHARS0: U = text, XL = count

; ---- memory ----
ZIPORG   = 0x5000        ; this code; swap pages end just below it
LOADORG  = 0x4E00        ; the M file loads here: the start-up pages (INIT), then the code
ZBASEH   = 0x01          ; Z address 0 is at 0100H
V        = 0x6880        ; variables (below); the code ends below them
OUTBUF   = 0x6D40        ; output waiting for TEXT (its low byte + OUTMAX within the page)
OUTMAX   = 120
STAMPS   = 0x6900        ; per swap slot: its last use (2 bytes)
SLOTPG   = 0x6980        ; per swap slot: its Z page (2 bytes, FFFFH = none)
ZSTK     = 0x6A00        ; the Z-stack, up to ZSTKEND
ZSTKEND  = 0x6D00
STORY    = 0x6D00        ; the story's full path: its length, then up to 40 characters
OPST     = 0x6D30        ; REVIVE: the story's SDOPEN status
RKEY     = 0x6D31        ; KEYLOOP: the key a power cycle interrupted
PTAB     = 0x6E00        ; per Z page: its RAM page, 0 = not in memory

ARGC   = V+0             ; operands of this instruction
OPC    = V+1
ARG1   = V+2             ; 16-bit values are high byte first
ARG2   = V+4
ARG3   = V+6
ARG4   = V+8
SPAGE  = V+10            ; Z page of the stream (bit 16, bits 15-8)
ZSP    = V+12            ; Z-stack: next free byte
ZFP    = V+14            ; Z-stack: this routine's first local
NRES   = V+16            ; resident pages (dynamic memory)
SWAP0  = V+17            ; first swap page
SWAPN  = V+18            ; swap slots
CLOCK  = V+19            ; LRU clock
GLOBR  = V+21            ; RAM address of the globals
OBJT   = V+23            ; RAM address of the object table
OBJB   = V+25            ; OBJT + 62 - 9: object n is at OBJB + 9n
ABBR   = V+27            ; Z address of the abbreviations
DENT   = V+29            ; Z address of the first dictionary entry
DCNT   = V+31            ; dictionary entries
DELEN  = V+33            ; dictionary entry length
NSEP   = V+34            ; word separators
SEPS   = V+35            ; ... up to 12 of them
TMP    = V+47            ; scratch
TMP2   = V+49
TMP3   = V+51
RNG    = V+53            ; xorshift16 state
SEED0  = V+55            ; the seed it started from
SAVS   = V+57            ; S at ENTRY
OUTN   = V+59            ; bytes in OUTBUF
BCOND  = V+60            ; BRANCH's condition
BB     = V+61            ; ... and its first byte
ZALPH  = V+62            ; Z-string decoder: alphabet shift
ZABB   = V+63            ; ... abbreviation pending (1-3)
ZESC   = V+64            ; ... ZSCII escape state
ZESCH  = V+65
ZWH    = V+66            ; ... the current word
ZWL    = V+67
ZC     = V+68
ZTMPB  = V+69
STRSP  = V+70            ; stream stack: offset of the next entry
STRSTK = V+71            ; ... 4 entries of SPAGE (2) and YL
PGH    = V+83            ; RESOLVE's page
PGL    = V+84
VPAGE  = V+85
RDXH   = V+86
BSTH   = V+87            ; VICTIM's best so far
BSTL   = V+88
BSLOT  = V+89
LASTPG = V+90            ; the last page stamped
RESV   = V+91            ; CALL: the result variable
NLOC   = V+92
LIDX   = V+93
TYPES  = V+94
TCNT   = V+95
KEYC   = V+96
TPTR   = V+97            ; sread: the text buffer (RAM)
PPTR   = V+99            ; ... the parse buffer (RAM)
TPOS   = V+101           ; ... position in the text
WSTART = V+102
WCNT   = V+103           ; ... words found
ENC    = V+104           ; ... the word's 6 Z-chars
EW     = V+110           ; ... and its 4 encoded bytes
LO     = V+114           ; dictionary search
HI     = V+116
MID    = V+118
CMH1   = V+120
CMH2   = V+121
PNUM   = V+122           ; property number
NEG    = V+123
NDIG   = V+124
TMPO   = V+125
FLAGS1 = V+126
FNAMEL = V+127           ; SAVE/RESTORE: the file name's length
; SAVE/RESTORE/VERIFY's own, in the dictionary search's (sread only):
FPOS   = ENC             ; the file position, 3 bytes (bits 23-16 first)
FREM   = ENC+3           ; bytes still to move
FCMD   = ENC+5           ; READ_BYTES or WRITE_BYTES
FPTR   = EW              ; RAM address
FCNT   = EW+2            ; this request's bytes
FSUM   = LO              ; VERIFY's sum
FEND   = HI              ; VERIFY: the story's length, 3 bytes
; The save file (SAVE/RESTORE, ZS1), its header built in OUTBUF:
;   +0  "ZS1"
;   +3  the story's release (2), serial (6), checksum (2), static base (2)
;   +15 the PC: SPAGE (2), YL -- the SAVE instruction's branch
;   +18 ZSP - ZSTK (2), ZFP - ZSTK (2)
; then the Z-stack, ZSTK up to ZSP, then dynamic memory, Z address 0 up
; to the static base.
HDRLEN = 22
FNAME  = OUTBUF+32       ; the name typed, up to 24 characters
SAVCH  = 16              ; its SDOPEN channel: the last, out of a program's way

TMPN   = OUTBUF+64       ; INIT: the name typed

        .area CODE (ABS)
        .org LOADORG

; The M file's first bytes: things a test harness or a loader may want.
        jmp INIT                ; +0
        .db STCH                ; +3 the story's SDOPEN channel
        .dw MLOOP               ; +4 the top of the instruction loop
        .dw SPAGE               ; +6 where the Z page of the PC is
        .dw RDPAGE              ; +8 a page read from the story
        .dw WAKE, ZTSTART       ; +10 the MCU waits: WAKE up to ZTSTART

; =====================================================================
; Start-up: run once, from the pages below ZIPORG -- once the game is
; going they are swap pages like the others, so nothing here may be
; called again, and nothing else may live here.
; =====================================================================

; INIT: the console, then GAME FILE: until a story opens -- the last game
; (FL key ZIPGAME) ready to edit, F1 the card's directory, BREAK back to
; BASIC. A name not from the root gets the current directory in front:
; STORY is a full path, for REVIVE (a power cycle puts the MCU back at the
; root) and, kept as ZIPGAME, for the next start.
INIT:   ldx s
        lda xh
        sta (SAVS)
        lda xl
        sta (SAVS+1)
        ldi a,0
        sta (OUTN)
        sta (STRSP)
        sjp ZOPEN
IN1:    sjp WAKE                ; the last game
        ldi xh,>FLKEY
        ldi xl,<FLKEY
        ldi yh,>W
        ldi yl,<W
        ldi ul,8
        sjp COPY
        ldi a,C_FLGET
        sjp MCMD
        cpi a,ST_OK
        bzr IN2
        lda (W)
        cpi a,0x53              ; 'S': a string
        bzr IN2
        lda (W+1)
        cpi a,41
        bcs IN2
        sta ul
        inc ul
        ldi xh,>(W+1)
        ldi xl,<(W+1)
        ldi yh,>STORY
        ldi yl,<STORY
        sjp COPY
        bch IN3
IN2:    ldi a,0
        sta (STORY)
IN3:    ldi a,ZT_PROMPT
        sjp ZTSTART
        ldi a,1                 ; EXP_ZTERM_PROMPT_BROWSE
        sta (W+1)
        ldi a,40
        sta (W+2)
        ldi xh,>PRMSG           ; the prompt, then the text
        ldi xl,<PRMSG
        ldi yh,>(W+3)
        ldi yl,<(W+3)
        ldi ul,12
        sjp COPY
        ldi xh,>STORY
        ldi xl,<STORY
        lda (STORY)
        sta ul
        inc ul
        sjp COPY
        sjp ZTSEND
        cpi a,ST_OK
        bzs IN4
        jmp ZBREAK              ; no console
IN4:    sjp KL_ANS              ; the keys, until ENTER
        lda (W+27)
        bzs IN1                 ; nothing typed: again
        cpi a,41
        bcs IN9
        sta (TMPN)
        sta ul
        ldi xh,>(W+28)
        ldi xl,<(W+28)
        ldi yh,>(TMPN+1)
        ldi yl,<(TMPN+1)
        sjp COPY
        ldi yh,>(STORY+1)
        ldi yl,<(STORY+1)
        lda (TMPN+1)
        cpi a,0x2F              ; '/': from the root already
        bzs IN6
        sjp WAKE
        ldi a,C_CWD
        sjp MCMD
        lda (W+0x100)           ; the current directory's length
        bzs IN5
        sta ul
        ldi xh,>(W+0x101)
        ldi xl,<(W+0x101)
        sjp COPY
        dec y
        lda (y)                 ; its last character
        inc y
        cpi a,0x2F
        bzs IN6
IN5:    ldi a,0x2F              ; a '/' between
        sin y
IN6:    lda (TMPN)              ; the name
        sta ul
        ldi xh,>(TMPN+1)
        ldi xl,<(TMPN+1)
        sjp COPY
        lda yl                  ; STORY's length: YL - 1
        dec a
        sta (STORY)
        cpi a,41
        bcs IN9
        sjp OPENST
        cpi a,ST_OK
        bzs IN7
        ldi xh,>NFMSG
        ldi xl,<NFMSG
        bch IN8
IN9:    ldi xh,>TLMSG
        ldi xl,<TLMSG
IN8:    sjp PUTS
        sjp FLUSH
        jmp IN1
IN7:    sjp WAKE                ; the game kept for next time
        ldi xh,>FLKEY
        ldi xl,<FLKEY
        ldi yh,>W
        ldi yl,<W
        ldi ul,8
        sjp COPY
        ldi a,0x53
        sin y
        ldi xh,>STORY
        ldi xl,<STORY
        lda (STORY)
        sta ul
        inc ul
        sjp COPY
        ldi a,C_FLSET
        sjp MCMD
        jmp ENTRY

FLKEY:  .db 7
        .ascii "ZIPGAME"
PRMSG:  .db 11
        .ascii "GAME FILE: "
NFMSG:  .ascii "Not found."
        .db 0x0D, 0
TLMSG:  .ascii "Name too long."
        .db 0x0D, 0
INITEND:                        ; build.ps1: must not pass ZIPORG

        .org ZIPORG

; =====================================================================
; The MCU
; =====================================================================

; WAKE: makes sure the MCU is awake and listening, as rom.asm's EC_WAKE:
; WAKE until the status reads AWAKE, then READY until it reads READY
; (RP2350/pc_exp.h EXP_COMMAND_WAKE). A lost write (the MCU waking, or
; going to sleep) is sent again ~41 ms on, for as long as it takes -- never
; later: a woken MCU with no command for 100 ms sleeps again (EC_WAKE's
; comment). Preserves X, Y, U.
WAKE:   psh x
        psh y
        psh u
        ldi a,C_WAKE
        ldi xh,ST_AWAKE
        sjp WPHASE
        ldi a,C_READY
        ldi xh,ST_READY
        sjp WPHASE
        pop u
        pop y
        pop x
        rtn

; WPHASE: A, written to the cell every ~41 ms until it reads XH. Clobbers
; A, U, YL.
WPHASE: sta yl
1$:     lda yl
        sta (INSTR)
        ldi uh,0x03             ; 1000 polls, ~41 ms (rom.asm EC_WAKE_STEP_POLLS)
        ldi ul,0xE8
5$:     lda (INSTR)
        cpa xh
        bzs 7$
        dec u
        cpi uh,0x00
        bzr 5$
        cpi ul,0x00
        bzr 5$
        bch 1$
7$:     rtn

; MCMD: sends the command in A (its arguments already in the window) and
; waits for the answer. A = the status. Preserves X, Y, U.
MCMD:   sta (INSTR)
1$:     lda (INSTR)
        cpi a,ST_BUSY
        bzs 1$
        rtn

; ZTCMD: sends ZTERM with the sub-op in A, after WAKE. The caller puts the
; rest of the arguments at W+1 after this has woken the MCU, so this only
; sends; use ZTSTART first.
ZTSTART: psh a
        sjp WAKE
        pop a
        sta (W)
        rtn
ZTSEND: ldi a,C_ZTERM
        bch MCMD

; DONE: lets the MCU sleep. Nothing in the window is valid after this.
SDONE:  ldi a,C_DONE
        bch MCMD

; =====================================================================
; Paging
; =====================================================================

; RDPAGE: reads the story's page PGH:PGL into RAM page A, whole.
; Preserves Y.
RDPAGE: psh a
        sjp WAKE
        ldi a,STCH
        sta (W)
        ldi a,0
        sta (W+1)
        sta (W+4)
        sta (W+6)
        lda (PGH)
        sta (W+2)
        lda (PGL)
        sta (W+3)
        ldi a,1
        sta (W+5)               ; 0100H bytes
        ldi a,C_RDB
        sjp MCMD
        cpi a,ST_OK
        bcr 2$                  ; no answer
        cpi a,0x80
        bcs 2$                  ; ERROR
        pop a                   ; SUCCESS or EOF: copy what came
        psh y
        sta yh
        ldi yl,0
        ldi xh,>W
        ldi xl,<W
        ldi ul,0
1$:     tin
        dec ul
        bzr 1$
        pop y
        rtn
2$:     ldi a,2                 ; the story can't be read
        jmp FATAL

; RESOLVE: A = the RAM page holding Z page PGH:PGL, read in if it isn't
; there. Preserves X, Y, U.
RESOLVE: psh x
        ldi xh,>PTAB
        lda (PGH)
        bzs 1$
        inc xh
1$:     lda (PGL)
        sta xl
        lda (x)
        bzs MISS
        cpa (SWAP0)
        bcr 2$                  ; resident
        cpa (LASTPG)
        bzs 2$                  ; stamped already
        psh a
        sjp STAMP
        pop a
2$:     pop x
        rtn

; A Z page not in memory: X is its PTAB entry.
MISS:   psh u
        psh x
        sjp VICTIM
        sta (VPAGE)
        sec
        sbc (SWAP0)
        shl
        ori a,0x80
        sta xl
        ldi xh,>SLOTPG          ; X = the slot's SLOTPG entry
        lda (x)
        cpi a,0xFF
        bzs 1$                  ; empty
        rec
        adi a,>PTAB
        sta uh
        inc x
        lda (x)
        dec x
        sta ul
        ldi a,0
        sta (u)                 ; its old page is gone
1$:     lda (PGH)
        sin x
        lda (PGL)
        sta (x)
        pop x
        lda (VPAGE)
        sta (x)
        sjp RDPAGE
        lda (VPAGE)
        sjp STAMP
        pop u
        pop x
        lda (VPAGE)
        rtn

; STAMP: marks RAM page A (a swap page) as just used. Uses X.
STAMP:  sta (LASTPG)
        sec
        sbc (SWAP0)
        shl
        sta xl
        ldi xh,>STAMPS
        adi (CLOCK+1),1
        bcr 1$
        adi (CLOCK),1
        bcr 1$
        sjp CLRSTAMP            ; the clock wrapped: start the ages again
1$:     lda (CLOCK)
        sin x
        lda (CLOCK+1)
        sta (x)
        rtn

CLRSTAMP: psh x
        ldi xh,>STAMPS
        ldi xl,0
        ldi a,0
1$:     sin x
        cpi xl,0x80
        bzr 1$
        ldi a,0
        sta (CLOCK)
        ldi a,1
        sta (CLOCK+1)
        pop x
        rtn

; VICTIM: A = the least recently used swap page, never the page Y is in.
; Uses X, U.
VICTIM: ldi a,0xFF
        sta (BSTH)
        sta (BSTL)
        ldi xh,>STAMPS
        ldi xl,0
        lda (SWAPN)
        sta ul
        lda (SWAP0)
        sta uh
        sta (BSLOT)
1$:     lda uh
        cpa yh
        bzs 4$
        lda (x)
        cpa (BSTH)
        bcr 2$
        bzr 4$
        inc x
        lda (x)
        dec x
        cpa (BSTL)
        bcs 4$
2$:     lda (x)
        sta (BSTH)
        inc x
        lda (x)
        dec x
        sta (BSTL)
        lda uh
        sta (BSLOT)
4$:     inc x
        inc x
        inc uh
        dec ul
        bzr 1$
        lda (BSLOT)
        rtn

; =====================================================================
; The stream (the PC, or a string being printed)
; =====================================================================

; GETB: A = the next byte of the stream. Preserves X, U.
GETB:   lin y
        cpi yl,0
        bzs 1$
        rtn
1$:     psh a
        adi (SPAGE+1),1
        bcr 2$
        adi (SPAGE),1
2$:     sjp REPAGE
        pop a
        rtn

; SETSTR: the stream is at Z address A:X (A = bit 16). Preserves X, U.
SETSTR: sta (SPAGE)
        lda xh
        sta (SPAGE+1)
        lda xl
        sta yl
; REPAGE: YH = the RAM page of SPAGE again (it may have moved).
REPAGE: lda (SPAGE)
        sta (PGH)
        lda (SPAGE+1)
        sta (PGL)
        sjp RESOLVE
        sta yh
        rtn

; PUSHS/POPS: keep the stream while another is printed. Use X.
PUSHS:  lda (STRSP)
        rec
        adi a,<STRSTK
        sta xl
        ldi xh,>STRSTK
        lda (SPAGE)
        sin x
        lda (SPAGE+1)
        sin x
        lda yl
        sta (x)
        lda (STRSP)
        rec
        adi a,3
        sta (STRSP)
        rtn
POPS:   lda (STRSP)
        sec
        sbi a,3
        sta (STRSP)
        rec
        adi a,<STRSTK
        sta xl
        ldi xh,>STRSTK
        lin x
        sta (SPAGE)
        lin x
        sta (SPAGE+1)
        lda (x)
        sta yl
        bch REPAGE

; =====================================================================
; Memory
; =====================================================================

; RDBYTE: A = the byte at Z address X (below 64K). Preserves X, U.
RDBYTE: lda xh
        sta (RDXH)
        cpa (NRES)
        bcs 2$
        rec
        adi a,ZBASEH
1$:     sta xh
        lda (x)
        psh a
        lda (RDXH)
        sta xh
        pop a
        rtn
2$:     sta (PGL)
        ldi a,0
        sta (PGH)
        sjp RESOLVE
        bch 1$

; RDWORD: X = the word at Z address X. Preserves U.
RDWORD: sjp RDBYTE
        psh a
        inc x
        sjp RDBYTE
        sta xl
        pop a
        sta xh
        rtn

; DYNPTR: X = the RAM address of dynamic-memory Z address X.
DYNPTR: lda xh
        cpa (NRES)
        bcs 1$
        rec
        adi a,ZBASEH
        sta xh
        rtn
1$:     ldi a,3                 ; a write outside dynamic memory
        jmp FATAL

; LDX16: X = the 16-bit value at (U), U advanced by one. LDARG1/2: X = ARGn.
LDARG2: ldi ul,<ARG2
        bch LDARG
LDARG1: ldi ul,<ARG1
LDARG:  ldi uh,>ARG1
LDX16:  lin u
        sta xh
        lda (u)
        sta xl
        rtn

; =====================================================================
; Variables and the Z-stack
; =====================================================================

; VARREF: U = the address of variable A's value; 0 is the stack's top,
; in place (inc, dec, load, store, pull...).
VARREF: cpi a,0
        bzr VARADDR
        sjp LDZSP
        dec u
        dec u
        rtn
; VARADDR: the same for A = 1-255.
VARADDR: cpi a,16
        bcs 1$
        dec a
        psh a
        lda (ZFP)
        sta uh
        lda (ZFP+1)
        sta ul
        bch 2$
1$:     sbi a,16                ; (Carry is set)
        psh a
        lda (GLOBR)
        sta uh
        lda (GLOBR+1)
        sta ul
2$:     pop a
        adr u
        adr u
        rtn

LDZSP:  lda (ZSP)
        sta uh
        lda (ZSP+1)
        sta ul
        rtn
STZSP:  lda uh
        sta (ZSP)
        lda ul
        sta (ZSP+1)
        rtn

; GETVAR: X = variable A (0 pops). Uses U.
GETVAR: cpi a,0
        bzs POPX
        sjp VARADDR
        bch LDX16
POPX:   sjp LDZSP
        dec u
        dec u
        sjp STZSP
        bch LDX16

; SETVAR: variable A = X (0 pushes). Uses U.
SETVAR: cpi a,0
        bzs PUSHX
        sjp VARADDR
        lda xh
        sin u
        lda xl
        sta (u)
        rtn
PUSHX:  sjp LDZSP
        lda uh
        cpi a,>ZSTKEND
        bcs 1$
        lda xh
        sin u
        lda xl
        sin u
        bch STZSP
1$:     ldi a,4                 ; stack overflow
        jmp FATAL

; STOREX: stores X in the variable the next byte names.
STOREX: sjp GETB
        bch SETVAR
STORE0: ldi a,0
STOREA: sta xl                  ; stores A (0-255)
        ldi xh,0
        bch STOREX

; =====================================================================
; The instruction loop
; =====================================================================

; GETOP: X = an operand of type A (0 large, 1 small, 2 variable).
GETOP:  cpi a,1
        bzs 1$
        bcs 2$
        sjp GETB
        psh a
        sjp GETB
        sta xl
        pop a
        sta xh
        rtn
1$:     sjp GETB
        sta xl
        ldi xh,0
        rtn
2$:     sjp GETB
        bch GETVAR

; STARG: the next ARGn = X.
STARG:  lda (ARGC)
        shl
        rec
        adi a,<ARG1
        sta ul
        ldi uh,>ARG1
        lda xh
        sin u
        lda xl
        sta (u)
        adi (ARGC),1
        rtn

; A long-form operand: Z clear = variable, set = small constant.
LTYPE:  bzs 1$
        ldi a,2
        bch 2$
1$:     ldi a,1
2$:     sjp GETOP
        bch STARG

MLOOP:  ldi a,0
        sta (ARGC)
        sjp GETB
        sta (OPC)
        cpi a,0x80
        bcs NOTLONG
        ani a,0x40              ; long form: 2OP
        sjp LTYPE
        lda (OPC)
        ani a,0x20
        sjp LTYPE
        lda (OPC)
        ani a,0x1F
        ldi xh,>T2OP
        ldi xl,<T2OP
        bch DISP
NOTLONG: cpi a,0xC0
        bcs VARFORM
        cpi a,0xB0
        bcs 1$
        aex                     ; short form: 1OP, type in bits 5-4
        ani a,0x03
        sjp GETOP
        sjp STARG
        lda (OPC)
        ani a,0x0F
        ldi xh,>T1OP
        ldi xl,<T1OP
        bch DISP
1$:     ani a,0x0F              ; 0OP
        ldi xh,>T0OP
        ldi xl,<T0OP
        bch DISP
VARFORM: sjp GETB               ; the operand types
        sta (TYPES)
        ldi a,4
        sta (TCNT)
1$:     lda (TYPES)
        ani a,0xC0
        cpi a,0xC0
        bzs 2$
        aex
        shr
        shr
        sjp GETOP
        sjp STARG
        lda (TYPES)
        shl
        shl
        sta (TYPES)
        lda (TCNT)
        dec a
        sta (TCNT)
        bzr 1$
2$:     lda (OPC)
        cpi a,0xE0
        bcs 3$
        ani a,0x1F
        ldi xh,>T2OP
        ldi xl,<T2OP
        bch DISP
3$:     ani a,0x1F
        ldi xh,>TVAR
        ldi xl,<TVAR
; DISP: runs entry A of the table at X, then the next instruction.
DISP:   shl
        adr x
        lin x
        sta (TMP)
        lda (x)
        sta xl
        lda (TMP)
        sta xh
        sjp JMPX
        jmp MLOOP
JMPX:   stx p

; BRANCH: A = the condition (0 = false). Reads the branch bytes and takes
; the branch when the condition matches their polarity.
BRANCH: sta (BCOND)
        sjp GETB
        sta (BB)
        ani a,0x40
        bzs 1$
        lda (BB)                ; short: 6-bit offset
        ani a,0x3F
        sta xl
        ldi xh,0
        bch 2$
1$:     sjp GETB                ; long: 14-bit signed
        sta xl
        lda (BB)
        ani a,0x3F
        bii a,0x20
        bzs 3$
        ori a,0xC0
3$:     sta xh
2$:     lda (BCOND)
        bzs 4$
        ldi a,0x80
4$:     eor (BB)
        ani a,0x80
        bzs 5$
        rtn                     ; not taken
5$:     lda xh
        bzr 6$
        lda xl
        cpi a,2
        bcs 6$
        jmp DORET               ; 0 = return false, 1 = return true
6$:     dec x
        dec x
; ADDPC: the PC += X (signed).
ADDPC:  lda yl
        rec
        adc xl
        sta yl
        lda (SPAGE+1)
        adc xh
        sta (SPAGE+1)
        lda xh
        bii a,0x80
        bzr 1$
        lda (SPAGE)
        adi a,0
        bch 2$
1$:     lda (SPAGE)
        adi a,0xFF
2$:     ani a,1
        sta (SPAGE)
        jmp REPAGE

; =====================================================================
; Calls
; =====================================================================

OPCALL: lda (ARG1)
        bzr 1$
        lda (ARG1+1)
        bzr 1$
        jmp STORE0              ; routine 0 returns false
1$:     sjp GETB
        sta (RESV)
        sjp LDZSP               ; the frame: FP, the PC, the result variable
        lda (ZFP)
        sin u
        lda (ZFP+1)
        sin u
        lda (SPAGE)
        sin u
        lda (SPAGE+1)
        sin u
        lda yl
        sin u
        lda (RESV)
        sin u
        lda uh
        sta (ZFP)
        lda ul
        sta (ZFP+1)
        sjp STZSP
        lda (ARG1+1)            ; the PC = the packed address * 2
        shl
        sta yl
        lda (ARG1)
        rol
        sta (SPAGE+1)
        ldi a,0
        rol
        sta (SPAGE)
        sjp REPAGE
        sjp GETB                ; locals
        sta (NLOC)
        ldi a,1
        sta (LIDX)
2$:     lda (NLOC)
        bzs 4$
        sjp GETB                ; its initial value...
        psh a
        sjp GETB
        sta xl
        pop a
        sta xh
        lda (LIDX)
        cpa (ARGC)
        bcs 3$
        shl                     ; ...or the argument
        rec
        adi a,<ARG1
        sta ul
        ldi uh,>ARG1
        sjp LDX16
3$:     sjp PUSHX
        adi (LIDX),1
        lda (NLOC)
        dec a
        sta (NLOC)
        bch 2$
4$:     rtn

; DORET: returns X from the routine.
DORET:  lda (ZFP)
        sta uh
        lda (ZFP+1)
        sta ul
        dec u
        lde u
        sta (RESV)
        lde u
        sta yl
        lde u
        sta (SPAGE+1)
        lde u
        sta (SPAGE)
        lde u
        sta (TMP)
        lda (u)
        sta (ZFP)
        lda (TMP)
        sta (ZFP+1)
        sjp STZSP
        sjp REPAGE
        lda (RESV)
        jmp SETVAR

RTRUE:  ldi xh,0
        ldi xl,1
        bch DORET
RFALSE: ldi xh,0
        ldi xl,0
        bch DORET

; =====================================================================
; Output
; =====================================================================

; PUTCH: prints A. Preserves X, Y, U.
PUTCH:  psh u
        sta (ZTMPB)
        lda (OUTN)
        cpi a,OUTMAX
        bcr 1$
        sjp FLUSH
        lda (OUTN)
1$:     rec
        adi a,<OUTBUF
        sta ul
        ldi uh,>OUTBUF
        lda (ZTMPB)
        sta (u)
        adi (OUTN),1
        pop u
        rtn

; FLUSH: sends OUTBUF with TEXT. Preserves X, Y, U.
FLUSH:  lda (OUTN)
        bzr 1$
        rtn
1$:     psh x
        psh y
        psh u
        ldi a,ZT_TEXT
        sjp ZTSTART
        lda (OUTN)
        sta (W+1)
        sta ul
        ldi xh,>OUTBUF
        ldi xl,<OUTBUF
        ldi yh,>(W+2)
        ldi yl,<(W+2)
2$:     tin
        dec ul
        bzr 2$
        sjp ZTSEND
        ldi a,0
        sta (OUTN)
        pop u
        pop y
        pop x
        rtn

; PRINTZ: prints the Z-string at the stream.
PRINTZ: ldi a,0
        sta (ZALPH)
        sta (ZABB)
        sta (ZESC)
1$:     sjp GETB
        sta (ZWH)
        sjp GETB
        sta (ZWL)
        lda (ZWH)               ; z-char 0: bits 14-10
        shr
        shr
        ani a,0x1F
        sjp ZCHAR
        lda (ZWL)               ; z-char 1: bits 9-5
        aex
        shr
        ani a,0x07
        sta (ZTMPB)
        lda (ZWH)
        ani a,0x03
        shl
        shl
        shl
        ora (ZTMPB)
        sjp ZCHAR
        lda (ZWL)               ; z-char 2: bits 4-0
        ani a,0x1F
        sjp ZCHAR
        lda (ZWH)
        bii a,0x80
        bzs 1$
        rtn

; ZCHAR: one Z-char, A.
ZCHAR:  sta (ZC)
        lda (ZESC)
        bzs 2$
        cpi a,1
        bzr 1$
        lda (ZC)                ; the escape's high half
        sta (ZESCH)
        ldi a,2
        sta (ZESC)
        rtn
1$:     ldi a,0                 ; ...its low half: the ZSCII character
        sta (ZESC)
        lda (ZESCH)
        shl
        shl
        shl
        shl
        shl
        ora (ZC)
        jmp PUTCH
2$:     lda (ZABB)
        bzs 3$
        jmp ZABBREV
3$:     lda (ZC)
        bzr 4$
        sta (ZALPH)             ; 0: a space
        ldi a,0x20
        jmp PUTCH
4$:     cpi a,4
        bcs 5$
        sta (ZABB)              ; 1-3: an abbreviation follows
        rtn
5$:     bzr 6$
        ldi a,1                 ; 4: the next is upper case
        sta (ZALPH)
        rtn
6$:     cpi a,5
        bzr 7$
        ldi a,2                 ; 5: the next is punctuation
        sta (ZALPH)
        rtn
7$:     lda (ZALPH)
        cpi a,2
        bzr 8$
        lda (ZC)
        cpi a,6
        bzr 8$
        ldi a,1                 ; A2's 6: a ZSCII escape
        sta (ZESC)
        ldi a,0
        sta (ZALPH)
        rtn
8$:     ldi xh,>(ALPHA-6)
        ldi xl,<(ALPHA-6)
        lda (ZC)
        adr x
        lda (ZALPH)
        bzs 10$
        cpi a,1
        bzs 9$
        ldi a,26
        adr x
9$:     ldi a,26
        adr x
10$:    ldi a,0
        sta (ZALPH)
        lda (x)
        jmp PUTCH

; ZABBREV: abbreviation 32 * (ZABB - 1) + ZC.
ZABBREV: lda (ZABB)
        dec a
        shl
        shl
        shl
        shl
        shl
        ora (ZC)
        shl                     ; (bit 7 is clear: at most 95 * 2)
        sta (TMP3)
        ldi a,0
        sta (ZABB)
        lda (ZWH)
        psh a
        lda (ZWL)
        psh a
        sjp PUSHS
        lda (ABBR)
        sta xh
        lda (ABBR+1)
        sta xl
        lda (TMP3)
        adr x
        sjp RDWORD              ; a word address
        sjp WADDR
        sjp SETSTR
        sjp PRINTZ
        sjp POPS
        pop a
        sta (ZWL)
        pop a
        sta (ZWH)
        ldi a,0
        sta (ZALPH)
        sta (ZESC)
        sta (ZABB)
        rtn

; WADDR: A:X = 2 * X, a word or packed address as a byte address.
WADDR:  lda xl
        shl
        sta xl
        lda xh
        rol
        sta xh
        ldi a,0
        rol
        rtn

; PRSTR: prints the Z-string at Z address A:X, keeping the stream.
PRSTR:  psh a
        psh x
        sjp PUSHS
        pop x
        pop a
        sjp SETSTR
        sjp PRINTZ
        jmp POPS

; =====================================================================
; Objects
; =====================================================================

; OBJADDR: X = the RAM address of object A.
OBJADDR: sta (TMPO)
        aex
        ani a,0x0F
        shr
        sta xh
        lda (TMPO)
        shl
        shl
        shl
        sta xl                  ; X = 8n
        lda (TMPO)
        adr x                   ; 9n
        lda (OBJB+1)
        adr x
        lda (OBJB)
        rec
        adc xh
        sta xh
        rtn

; OBJ1: X = object ARG1's address + A.
OBJ1:   psh a
        lda (ARG1+1)
        sjp OBJADDR
        pop a
        adr x
        rtn

; PROPS: X = the RAM address of object A's first property.
PROPS:  sjp OBJADDR
        ldi a,7
        adr x
        lin x
        sta (TMP)
        lda (x)
        sta xl
        lda (TMP)
        sta xh
        sjp DYNPTR
        lda (x)                 ; skip the name
        inc x
        adr x
        adr x
        rtn

; FINDP: object A's property PNUM: X = its data, A = its size byte, or
; A = 0 (Z set) if it hasn't one.
FINDP:  sjp PROPS
1$:     lda (x)
        bzs 3$
        ani a,0x1F
        cpa (PNUM)
        bzs 2$
        bcr 3$
        lda (x)                 ; skip it: 1 + its length
        sjp PSIZE
        adr x
        inc x
        bch 1$
2$:     lda (x)
        inc x
        rtn
3$:     ldi a,0
        rtn

; PSIZE: A = the length of a property from its size byte A.
PSIZE:  aex
        shr
        ani a,0x07
        inc a
        rtn

; REMOVE: takes object A out of its parent.
REMOVE: sta (TMP3)
        sjp OBJADDR
        ldi a,4
        adr x
        lda (x)                 ; its parent
        bzs 9$
        psh x
        sjp OBJADDR
        ldi a,6
        adr x                   ; X = the parent's child
        pop u                   ; U = the object's parent
        lda (TMP3)
        cpa (x)
        bzr 2$
        inc u                   ; first child: the parent's child = its sibling
        lda (u)
        dec u
        sta (x)
        bch 4$
2$:     lda (x)                 ; find the sibling before it
        bzs 4$
        sjp OBJADDR
        ldi a,5
        adr x
        lda (TMP3)
        cpa (x)
        bzr 2$
        inc u
        lda (u)
        dec u
        sta (x)
4$:     ldi a,0
        sin u                   ; no parent, no sibling
        sta (u)
9$:     rtn

; =====================================================================
; Numbers
; =====================================================================

; NEGX: X = -X.
NEGX:   lda xl
        eai a,0xFF
        sta xl
        lda xh
        eai a,0xFF
        sta xh
        inc x
        rtn

; UDIV: TMP = TMP / TMP2, TMP3 = the remainder (unsigned).
UDIV:   ldi a,0
        sta (TMP3)
        sta (TMP3+1)
        ldi ul,15
1$:     lda (TMP+1)
        shl
        sta (TMP+1)
        lda (TMP)
        rol
        sta (TMP)
        lda (TMP3+1)
        rol
        sta (TMP3+1)
        lda (TMP3)
        rol
        sta (TMP3)
        sec
        lda (TMP3+1)
        sbc (TMP2+1)
        sta xl
        lda (TMP3)
        sbc (TMP2)
        bcr 2$
        sta (TMP3)
        lda xl
        sta (TMP3+1)
        adi (TMP+1),1           ; the quotient bit (bit 0 was clear)
2$:     dec ul
        bcs 1$
        rtn

; SDIV: signed TMP / TMP2 (ARG1 / ARG2): TMP quotient, TMP3 remainder;
; NEG bit 0 = the quotient's sign, bit 1 = the remainder's.
SDIV:   lda (ARG2)
        bzr 1$
        lda (ARG2+1)
        bzr 1$
        ldi a,5                 ; division by zero
        jmp FATAL
1$:     ldi a,0
        sta (NEG)
        sjp LDARG1
        lda xh
        bii a,0x80
        bzs 2$
        sjp NEGX
        ldi a,3
        sta (NEG)
2$:     lda xh
        sta (TMP)
        lda xl
        sta (TMP+1)
        sjp LDARG2
        lda xh
        bii a,0x80
        bzs 3$
        sjp NEGX
        lda (NEG)
        eai a,1
        sta (NEG)
3$:     lda xh
        sta (TMP2)
        lda xl
        sta (TMP2+1)
        bch UDIV

; =====================================================================
; Input
; =====================================================================

; SHOWST: sends the status line: the location (global 0) and global 1
; and 2.
SHOWST: sjp FLUSH
        ldi a,16
        sjp GETVAR
        lda xl
        bzs 1$
        sjp PROPS1
        ldi a,0
        sjp PRSTR               ; the name, into OUTBUF
1$:     ldi a,ZT_STATUS
        sjp ZTSTART
        ldi a,17
        sjp GETVAR
        lda xh
        sta (W+1)
        lda xl
        sta (W+2)
        ldi a,18
        sjp GETVAR
        lda xh
        sta (W+3)
        lda xl
        sta (W+4)
        lda (FLAGS1)
        sta (W+5)
        psh y
        lda (OUTN)
        sta (W+6)
        bzs 3$
        sta ul
        ldi xh,>OUTBUF
        ldi xl,<OUTBUF
        ldi yh,>(W+7)
        ldi yl,<(W+7)
2$:     tin
        dec ul
        bzr 2$
3$:     pop y
        ldi a,0
        sta (OUTN)
        jmp ZTSEND

; PROPS1: A:X = the Z address of object XL's name (its property table + 1).
PROPS1: lda xl
        sjp OBJADDR
        ldi a,7
        adr x
        lin x
        sta (TMP)
        lda (x)
        sta xl
        lda (TMP)
        sta xh
        inc x
        ldi a,0
        rtn

; KEYLOOP: the console's input loop, A = the longest line (0 = none, just
; read until ENTER). Returns with the window's KEY answer: the line's
; length at W+27, its text from W+28.
KEYLOOP: sta (TMP3)
        ldi a,0
KL_KEY: sta (KEYC)
        sjp KSEND
        cpi a,ST_OK
        bzs KL_ANS
        sjp REVIVE              ; a power cycle: no console, no story
        lda (KEYC)
        sta (RKEY)
        ldi a,0                 ; the input line begins again,
        sta (KEYC)
        sjp KSEND
        lda (RKEY)              ; then the key
        bzr KL_KEY
KL_ANS: lda (W)                 ; (GETFILE joins here, after FILE)
        cpi a,ZK_DONE
        bzr KL_SHOW
        rtn
KL_SHOW: psh y
        ldi uh,>(W+1)
        ldi ul,<(W+1)
        ldi xl,26
        sjp DISP26
        sjp SDONE
        sjp KEYWAIT
KL_GOT: bcr KL_NB
        jmp ZBREAK              ; BREAK: back to BASIC (S comes from SAVS)
KL_NB:  cpi a,KEYOFF
        bzr KL_ON
        ldi uh,0xF8             ; the idle count KEYSCAN_WAIT starts with
        sjp PWROFF              ; after ON: the next key, as KEYWAIT
        bcr KL_GOT
        ani #(0xF00B),0xFD      ; BREAK: the ON that woke it (IF bit 1, latched) -- not one
        sjp KEYWAIT
        bch KL_GOT
KL_ON:  pop y
        bch KL_KEY

; KSEND: KEY with KEYC and the longest line TMP3. A = the status.
KSEND:  ldi a,ZT_KEY
        sjp ZTSTART
        lda (KEYC)
        sta (W+1)
        lda (TMP3)
        sta (W+2)
        jmp ZTSEND

; REVIVE: the MCU started afresh (a power cycle) while the game stayed in
; RAM: the story reopened from STORY, a new console -- its scrollback
; gone -- and a note. No console at all: back to BASIC. Preserves Y.
REVIVE: psh y
        sjp OPENST
        sta (OPST)
        sjp ZOPEN
        cpi a,ST_OK
        bzs 1$
        jmp ZBREAK
1$:     lda (OPST)
        cpi a,ST_OK
        bzs 2$
        ldi a,2                 ; the story can't be read
        jmp FATAL
2$:     ldi xh,>RESMSG
        ldi xl,<RESMSG
        sjp PUTS
        sjp FLUSH
        pop y
        rtn

; ZOPEN: the console's OPEN, for the story on STCH. A = the status; the
; seed for RANDOM at W.
ZOPEN:  ldi a,ZT_OPEN
        sjp ZTSTART
        ldi a,STCH
        sta (W+1)
        ldi a,0
        sta (W+2)
        jmp ZTSEND

; ISSEP: Carry set if A is one of the dictionary's separators.
; Preserves A, X.
ISSEP:  psh x
        sta (TMP)
        ldi xh,>SEPS
        ldi xl,<SEPS
        lda (NSEP)
        sta (TMP+1)
1$:     lda (TMP+1)
        bzs 2$
        dec a
        sta (TMP+1)
        lin x
        cpa (TMP)
        bzr 1$
        sec
        bch 3$
2$:     rec
3$:     pop x
        lda (TMP)
        rtn

; OPREAD: sread text, parse.
OPREAD: sjp SHOWST
        sjp FLUSH
        sjp LDARG1
        sjp DYNPTR
        lda xh
        sta (TPTR)
        lda xl
        sta (TPTR+1)
        lda (x)                 ; the longest line, minus 1
        dec a
        sjp KEYLOOP
        ; copy the line in, lower case, 0 after it
        lda (TPTR)
        sta uh
        lda (TPTR+1)
        sta ul
        lda (u)
        dec a
        sta (TMP)               ; at most this many
        inc u
        ldi xh,>(W+28)
        ldi xl,<(W+28)
        lda (W+27)
        sta (TMP+1)
1$:     lda (TMP+1)
        bzs 3$
        lda (TMP)
        bzs 3$
        lin x
        cpi a,0x41
        bcr 2$
        cpi a,0x5B
        bcs 2$
        ori a,0x20
2$:     sin u
        lda (TMP)
        dec a
        sta (TMP)
        lda (TMP+1)
        dec a
        sta (TMP+1)
        bch 1$
3$:     ldi a,0
        sta (u)
        ; tokenise
        sjp LDARG2
        sjp DYNPTR
        lda xh
        sta (PPTR)
        lda xl
        sta (PPTR+1)
        ldi a,0
        sta (WCNT)
        ldi a,1
        sta (TPOS)
4$:     sjp TCHAR
        bzs 9$                  ; the end
        cpi a,0x20
        bzr 5$
        adi (TPOS),1
        bch 4$
5$:     lda (TPOS)
        sta (WSTART)
        sjp TCHAR
        sjp ISSEP
        bcr 6$
        adi (TPOS),1            ; a separator is a word of its own
        bch 8$
6$:     adi (TPOS),1
        sjp TCHAR
        bzs 8$
        cpi a,0x20
        bzs 8$
        sjp ISSEP
        bcr 6$
8$:     sjp WORD
        bch 4$
9$:     lda (PPTR)
        sta xh
        lda (PPTR+1)
        sta xl
        inc x
        lda (WCNT)
        sta (x)
        rtn

; TCHAR: A = the text buffer's character at TPOS (Z set at the 0).
TCHAR:  lda (TPTR)
        sta xh
        lda (TPTR+1)
        sta xl
        lda (TPOS)
        adr x
        lda (x)
        rtn

; WORD: the word from WSTART to TPOS into the parse buffer.
WORD:   lda (PPTR)
        sta xh
        lda (PPTR+1)
        sta xl
        lda (WCNT)
        cpa (x)
        bcr 1$
        rtn                     ; the parse buffer is full
1$:     shl
        shl
        inc a
        inc a
        adr x
        psh x                   ; its entry
        sjp ENCODE
        sjp LOOKUP              ; X = the dictionary address, or 0
        pop u
        lda xh
        sin u
        lda xl
        sin u
        lda (TPOS)
        sec
        sbc (WSTART)
        sin u
        lda (WSTART)
        sta (u)
        adi (WCNT),1
        rtn

; ENCODE: the word from WSTART to TPOS as 6 Z-chars at ENC, then as 4
; bytes at EW.
ENCODE: ldi uh,>ENC
        ldi ul,<ENC
        lda (WSTART)
        sta (TMP2)
1$:     lda ul
        cpi a,<(ENC+6)
        bcs 8$
        lda (TMP2)
        cpa (TPOS)
        bcs 7$
        lda (TPTR)
        sta xh
        lda (TPTR+1)
        sta xl
        lda (TMP2)
        adr x
        adi (TMP2),1
        lda (x)
        cpi a,0x61
        bcr 2$
        cpi a,0x7B
        bcs 2$
        sec
        sbi a,0x5B              ; a-z: 6-31
        sin u
        bch 1$
2$:     sta (TMP)               ; A2's punctuation?
        ldi xh,>A2PUNCT
        ldi xl,<A2PUNCT
3$:     lda (x)
        bzs 4$
        inc x
        cpa (TMP)
        bzr 3$
        ldi a,5
        sin u
        lda xl
        sec
        sbi a,<(A2PUNCT-7)      ; '0' (the first) is Z-char 8
        sin u
        bch 1$
4$:     ldi a,5                 ; anything else: escape
        sin u
        ldi a,6
        sin u
        lda (TMP)
        aex
        shr
        ani a,0x07
        sin u
        lda (TMP)
        ani a,0x1F
        sin u
        bch 1$
7$:     ldi a,5                 ; pad with 5s
        sin u
        bch 1$
8$:     ldi xh,>ENC
        ldi xl,<ENC
        ldi uh,>EW
        ldi ul,<EW
        sjp PACK3
        inc u
        sjp PACK3
        lda (EW+2)
        ori a,0x80
        sta (EW+2)
        rtn

; PACK3: three Z-chars at X (X advanced) as a word at U (U at its low byte).
PACK3:  lin x
        shl
        shl
        sta (TMP)
        lda (x)
        shr
        shr
        shr
        ora (TMP)
        sin u
        lin x
        aex
        shl
        ani a,0xE0
        sta (TMP)
        lin x
        ora (TMP)
        sta (u)
        rtn

; LOOKUP: X = the dictionary entry matching EW, or 0. Binary search.
LOOKUP: ldi a,0
        sta (LO)
        sta (LO+1)
        lda (DCNT)
        sta xh
        lda (DCNT+1)
        sta xl
        dec x
        lda xh
        sta (HI)
        lda xl
        sta (HI+1)
1$:     lda (HI)                ; while 0 <= LO <= HI
        bii a,0x80
        bzr 6$
        sec
        lda (HI+1)
        sbc (LO+1)
        lda (HI)
        sbc (LO)
        bcs 2$
6$:     ldi xh,0
        ldi xl,0
        rtn
2$:     rec                     ; MID = (LO + HI) / 2
        lda (LO+1)
        adc (HI+1)
        sta (MID+1)
        lda (LO)
        adc (HI)
        ror
        sta (MID)
        lda (MID+1)
        ror
        sta (MID+1)
        lda (MID)               ; X = DENT + MID * DELEN
        sta (TMP)
        lda (MID+1)
        sta (TMP+1)
        lda (DELEN)
        sta (TMP2+1)
        ldi a,0
        sta (TMP2)
        sjp MUL
        lda (DENT+1)
        adr x
        lda (DENT)
        rec
        adc xh
        sta xh
        ldi uh,>EW              ; compare
        ldi ul,<EW
        psh x
        ldi a,4
        sta (TCNT)
3$:     sjp RDBYTE
        cpa (u)
        bzr 4$
        inc x
        inc u
        lda (TCNT)
        dec a
        sta (TCNT)
        bzr 3$
        pop x                   ; found
        rtn
4$:     pop x
        bcs 5$                  ; the entry > EW: HI = MID - 1
        lda (MID)
        sta xh
        lda (MID+1)
        sta xl
        inc x
        lda xh
        sta (LO)
        lda xl
        sta (LO+1)
        bch 1$
5$:     lda (MID)
        sta xh
        lda (MID+1)
        sta xl
        dec x
        lda xh
        sta (HI)
        lda xl
        sta (HI+1)
        jmp 1$

; MUL: X = TMP * TMP2 (low 16 bits).
MUL:    ldi xh,0
        ldi xl,0
        ldi ul,15
1$:     lda xl
        shl
        sta xl
        lda xh
        rol
        sta xh
        lda (TMP+1)
        shl
        sta (TMP+1)
        lda (TMP)
        rol
        sta (TMP)
        bcr 2$
        lda (TMP2+1)
        adr x
        lda (TMP2)
        rec
        adc xh
        sta xh
2$:     dec ul
        bcs 1$
        rtn

; =====================================================================
; Start, restart, quit, errors
; =====================================================================

; ENTRY: INIT has opened the story on STCH: a new console, which now knows
; its path (the save file's first name, POWMAN's session record), then
; the game.
ENTRY:  sjp ZOPEN
        lda (W)                 ; a seed for RANDOM
        sta (SEED0)
        lda (W+1)
        sta (SEED0+1)
        bzr 1$
        lda (SEED0)
        bzr 1$
        ldi a,1
        sta (SEED0+1)
1$:     sjp RESEED
        ldi xh,>PTAB            ; nothing in memory yet
        ldi xl,<PTAB
        ldi a,0
2$:     sin x
        cpi xh,0x70
        bzr 2$
        ldi a,0                 ; the header first
        sta (PGH)
        sta (PGL)
        ldi a,ZBASEH
        sjp RDPAGE
        lda (ZBASEH*256)
        cpi a,3
        bzs 3$
        ldi a,1                 ; not a version-3 story
        jmp FATAL
3$:     lda (ZBASEH*256+0x0F)   ; resident pages = the static base, rounded up
        cpi a,1
        lda (ZBASEH*256+0x0E)
        bcr 4$
        inc a
4$:     sta (NRES)
        rec
        adi a,ZBASEH
        sta (SWAP0)
        sta xl
        ldi a,>ZIPORG
        sec
        sbc xl
        bcr 5$
        cpi a,2
        bcr 5$
        cpi a,65
        bcr 9$
        ldi a,64                ; (the slot tables hold 64)
9$:     sta (SWAPN)
        bch 6$
5$:     ldi a,6                 ; the story's dynamic memory doesn't fit
        jmp FATAL
6$:     ldi xh,>PTAB            ; the resident pages' entries
        ldi xl,<PTAB
        ldi a,ZBASEH
7$:     sin x
        inc a
        cpa (SWAP0)
        bzr 7$
        ldi xh,>SLOTPG          ; empty swap slots
        ldi xl,<SLOTPG
        ldi a,0xFF
8$:     sin x
        cpi xl,0
        bzr 8$
        sjp CLRSTAMP
        ldi a,0
        sta (LASTPG)
        sjp LOADDYN
        sjp INITHDR
        jmp MLOOP

; LOADDYN: reads dynamic memory (all resident pages) and resets the
; machine: the header's flags, the stack, the PC.
LOADDYN: ldi a,0
        sta (PGH)
        sta (PGL)
1$:     lda (PGL)
        rec
        adi a,ZBASEH
        sjp RDPAGE
        adi (PGL),1
        lda (PGL)
        cpa (NRES)
        bzr 1$
        lda (ZBASEH*256+1)      ; status line, no split, fixed pitch
        ani a,0x8F
        sta (ZBASEH*256+1)
        ani a,0x02
        sta (FLAGS1)
        ldi a,>ZSTK
        sta (ZSP)
        sta (ZFP)
        ldi a,<ZSTK
        sta (ZSP+1)
        sta (ZFP+1)
        ldi a,0
        sta (SPAGE)
        lda (ZBASEH*256+6)      ; the initial PC
        sta xh
        lda (ZBASEH*256+7)
        sta xl
        ldi a,0
        jmp SETSTR

; INITHDR: the tables the header points at.
INITHDR: ldi uh,ZBASEH          ; globals
        ldi ul,0x0C
        sjp ZPTR
        lda xh
        sta (GLOBR)
        lda xl
        sta (GLOBR+1)
        ldi ul,0x0A             ; objects
        sjp ZPTR
        lda xh
        sta (OBJT)
        lda xl
        sta (OBJT+1)
        ldi a,53
        adr x
        lda xh
        sta (OBJB)
        lda xl
        sta (OBJB+1)
        ldi ul,0x18             ; abbreviations
        sjp LDX16
        lda xh
        sta (ABBR)
        lda xl
        sta (ABBR+1)
        ldi ul,0x08             ; the dictionary
        sjp LDX16
        sjp RDBYTE
        cpi a,13
        bcr 3$
        ldi a,12                ; (room for 12)
3$:     sta (NSEP)
        sta (TCNT)
        ldi a,<SEPS
        sta (TMP)
1$:     lda (TCNT)
        bzs 2$
        dec a
        sta (TCNT)
        inc x
        sjp RDBYTE
        psh x
        ldi xh,>SEPS
        psh a
        lda (TMP)
        sta xl
        pop a
        sta (x)
        adi (TMP),1
        pop x
        bch 1$
2$:     inc x
        sjp RDBYTE
        sta (DELEN)
        inc x
        sjp RDWORD
        lda xh
        sta (DCNT)
        lda xl
        sta (DCNT+1)
        ldi uh,ZBASEH           ; the entries: dictionary + 4 + separators
        ldi ul,0x08
        sjp LDX16
        lda (NSEP)
        adr x
        ldi a,4
        adr x
        lda xh
        sta (DENT)
        lda xl
        sta (DENT+1)
        rtn

; ZPTR: X = the RAM address the header word at U points at (dynamic).
ZPTR:   sjp LDX16
        dec u
        lda xh
        rec
        adi a,ZBASEH
        sta xh
        rtn

; RESEED: the generator starts again from SEED0.
RESEED: lda (SEED0)
        sta (RNG)
        lda (SEED0+1)
        sta (RNG+1)
        rtn

; FATAL: stops with error A: "*ZIP ERROR n*", then quits.
; PUTS: the text at X, up to its 0, to OUTBUF.
PUTS:   lin x
        bzs 1$
        sjp PUTCH
        bch PUTS
1$:     rtn

; COPY: UL bytes (1-256, 0 = 256) from X to Y.
COPY:   tin
        dec ul
        bzr COPY
        rtn

FATAL:  psh a
        ldi a,0x0D
        sjp PUTCH
        ldi xh,>FATMSG
        ldi xl,<FATMSG
        sjp PUTS
        pop a
        rec
        adi a,0x30
        sjp PUTCH
        ldi a,0x2A
        sjp PUTCH
        ldi a,0x0D
        sjp PUTCH
; QUIT: shows the last text until ENTER, closes the console, returns to
; BASIC. BREAK at any key wait (KEYLOOP) does the same at once.
OPQUIT: sjp FLUSH
        ldi a,0
        sjp KEYLOOP
ZBREAK: sjp CLOSEST
        ldi a,ZT_CLOSE
        sjp ZTSTART
        sjp ZTSEND
        sjp SDONE
        lda (SAVS)
        sta xh
        lda (SAVS+1)
        sta xl
        stx s
        rtn

OPRESTART: sjp FLUSH
        sjp LOADDYN
        ldi a,0
        sta (STRSP)
        rtn

; =====================================================================
; 0OP
; =====================================================================

OPPRINT: jmp PRINTZ
OPPRRET: sjp PRINTZ
        ldi a,0x0D
        sjp PUTCH
        jmp RTRUE
OPRETPOP: sjp POPX
        jmp DORET
OPPOP:  jmp POPX
OPNL:   ldi a,0x0D
        jmp PUTCH
OPFALSE: ldi a,0
        jmp BRANCH
OPNOP:  rtn
OPBAD:  ldi a,7
        jmp FATAL

; =====================================================================
; SAVE, RESTORE, VERIFY
; =====================================================================

; SAVE: the file name at the console's prompt, then the save file (ZS1,
; above) through WRITE_BYTES on SAVCH. The branch: taken if it's saved.
OPSAVE: sjp FLUSH
        sjp MKHDR               ; the PC: this instruction's branch
        psh y
        ldi a,0
        sjp GETFILE
        bcs SVNO
        ldi a,1                 ; FOR W: created if it isn't there
        sjp OPENF
        bcs SVNO
        sjp FZERO
        sjp FHDR
        ldi a,C_WRB
        sjp FXFER
        bcs SVBAD
        sjp FSTACK
        ldi a,C_WRB
        sjp FXFER
        bcs SVBAD
        sjp FDYN
        ldi a,C_WRB
        sjp FXFER
        bcs SVBAD
        sjp CLOSEF
        pop y
        ldi a,1
        jmp BRANCH
SVBAD:  sjp CLOSEF
SVNO:   pop y
        ldi a,0
        jmp BRANCH

; RESTORE: the file name, then the save file back -- the header first, and
; nothing changed unless it's this story's. Then on as the SAVE that made
; it, its branch taken. A file that ends early after that leaves the
; machine half restored: ZIP ERROR 8.
OPREST: sjp FLUSH
        psh y
        ldi a,1
        sjp GETFILE
        bcs RSNO
        ldi a,0                 ; read only
        sjp OPENF
        bcs RSNO
        sjp FZERO
        sjp FHDR
        ldi a,C_RDB
        sjp FXFER
        bcs RSBAD
        sjp CHKHDR
        bcs RSBAD
        lda (ZBASEH*256+0x10)   ; Flags 2 is the interpreter's: kept
        sta (TMP2)
        lda (ZBASEH*256+0x11)
        sta (TMP2+1)
        ldi a,>ZSTK             ; the stack
        sta (FPTR)
        ldi a,<ZSTK
        sta (FPTR+1)
        lda (OUTBUF+18)
        sta (FREM)
        lda (OUTBUF+19)
        sta (FREM+1)
        ldi a,C_RDB
        sjp FXFER
        bcs RSDEAD
        sjp FDYN
        ldi a,C_RDB
        sjp FXFER
        bcs RSDEAD
        sjp CLOSEF
        lda (TMP2)
        sta (ZBASEH*256+0x10)
        lda (TMP2+1)
        sta (ZBASEH*256+0x11)
        lda (ZBASEH*256+1)      ; as LOADDYN: status line, no split, fixed pitch
        ani a,0x8F
        sta (ZBASEH*256+1)
        ani a,0x02
        sta (FLAGS1)
        lda (OUTBUF+19)         ; ZSP, ZFP
        rec
        adi a,<ZSTK
        sta (ZSP+1)
        lda (OUTBUF+18)
        adi a,>ZSTK
        sta (ZSP)
        lda (OUTBUF+21)
        rec
        adi a,<ZSTK
        sta (ZFP+1)
        lda (OUTBUF+20)
        adi a,>ZSTK
        sta (ZFP)
        ldi a,0
        sta (STRSP)
        lda (OUTBUF+15)         ; the PC
        sta (SPAGE)
        lda (OUTBUF+16)
        sta (SPAGE+1)
        pop y
        lda (OUTBUF+17)
        sta yl
        sjp REPAGE
        ldi a,1
        jmp BRANCH
RSDEAD: ldi a,8
        jmp FATAL
RSBAD:  sjp CLOSEF
RSNO:   pop y
        ldi a,0
        jmp BRANCH

; VERIFY: the story file summed from 0040H to its length (the header's,
; in words), against the header's checksum. The branch: taken if equal.
OPVERIFY: psh y
        sjp FZERO
        ldi a,0
        sta (FSUM)
        sta (FSUM+1)
        lda (ZBASEH*256+0x1B)   ; the length: 2 x the header's
        shl
        sta (FEND+2)
        lda (ZBASEH*256+0x1A)
        rol
        sta (FEND+1)
        ldi a,0
        rol
        sta (FEND)
VF1:    lda (FEND+2)            ; what's left: FEND - FPOS
        sec
        sbc (FPOS+2)
        sta (FREM+1)
        lda (FEND+1)
        sbc (FPOS+1)
        sta (FREM)
        lda (FEND)
        sbc (FPOS)
        bcr VFEND               ; past it
        bzr VFBIG               ; 64K or more
        lda (FREM)
        cpi a,0x04
        bcs VFBIG
        ora (FREM+1)
        bzs VFEND
        lda (FREM)
        sta (FCNT)
        lda (FREM+1)
        sta (FCNT+1)
        bch VFRD
VFBIG:  ldi a,0x04              ; 1K at a time
        sta (FCNT)
        ldi a,0
        sta (FCNT+1)
VFRD:   sjp WAKE
        ldi a,STCH
        sta (W)
        ldi a,0
        sta (W+1)
        lda (FPOS)
        sta (W+2)
        lda (FPOS+1)
        sta (W+3)
        lda (FPOS+2)
        sta (W+4)
        lda (FCNT)
        sta (W+5)
        lda (FCNT+1)
        sta (W+6)
        ldi a,C_RDB
        sjp MCMD
        cpi a,ST_OK
        bzr VFNO
        ldi xh,>W
        ldi xl,<W
        lda (FCNT)
        sta uh
        lda (FCNT+1)
        sta ul
        lda (FPOS)              ; the header's 64 bytes aren't summed
        ora (FPOS+1)
        ora (FPOS+2)
        bzr VF3
        ldi a,0x40
        adr x
        lda ul
        sec
        sbi a,0x40
        sta ul
        lda uh
        sbi a,0
        sta uh
VF3:    lin x
        rec
        adc (FSUM+1)
        sta (FSUM+1)
        lda (FSUM)
        adi a,0
        sta (FSUM)
        dec u
        cpi uh,0x00
        bzr VF3
        cpi ul,0x00
        bzr VF3
        sjp FNEXT
        jmp VF1
VFEND:  lda (FSUM)
        cpa (ZBASEH*256+0x1C)
        bzr VFNO
        lda (FSUM+1)
        cpa (ZBASEH*256+0x1D)
        bzr VFNO
        pop y
        ldi a,1
        jmp BRANCH
VFNO:   pop y
        ldi a,0
        jmp BRANCH

; GETFILE: the console's file prompt (A = 0 save, 1 restore), its keys as
; KEYLOOP's; the name to FNAME and FNAMEL. Carry set if none.
GETFILE: sta (TMP3)
        ldi a,ZT_FILE
        sjp ZTSTART
        lda (TMP3)
        sta (W+1)
        sjp ZTSEND
        cpi a,ST_OK
        bzr GF9                 ; no console
        sjp KL_ANS              ; the keys, until ENTER
        lda (W+27)
        cpi a,25
        bcr GF1
        ldi a,24
GF1:    sta (FNAMEL)
        cpi a,0x00              ; (STA leaves the flags: cpi 25 set them)
        bzs GF9
        sta ul
        ldi xh,>(W+28)
        ldi xl,<(W+28)
        ldi yh,>FNAME
        ldi yl,<FNAME
GF2:    tin
        dec ul
        bzr GF2
        rec
        rtn
GF9:    sec
        rtn

; OPENST: SDOPEN of the story (STORY) on STCH, to read. A = the status.
; Clobbers Y.
OPENST: sjp WAKE
        ldi a,STCH
        sta (W)
        ldi a,0
        sta (FCMD)
        ldi xh,>STORY
        ldi xl,<STORY
        lin x
        bch OPENN

; OPENF: SDOPEN of FNAME on SAVCH, mode A (0 read, 1 write). Carry set if
; it didn't open.
OPENF:  sta (FCMD)
        sjp WAKE
        ldi a,SAVCH
        sta (W)
        ldi xh,>FNAME
        ldi xl,<FNAME
        lda (FNAMEL)
        sjp OPENN
        cpi a,ST_OK
        bzr 2$
        rec
        rtn
2$:     sec
        rtn

; OPENN: the rest of an SDOPEN: the channel at W already, A = the name's
; length (1 or more), X = the name, FCMD = the mode. A = the status.
OPENN:  sta (W+2)
        sta ul
        ldi a,0
        sta (W+1)
        ldi yh,>(W+3)
        ldi yl,<(W+3)
1$:     tin
        dec ul
        bzr 1$
        lda (FCMD)              ; the mode, after the name
        sin y
        ldi a,C_OPEN
        jmp MCMD

; CLOSEF: SDCLOSE of SAVCH; CLOSEST, of the story's channel.
CLOSEST: ldi a,STCH
        bch CLOSEA
CLOSEF: ldi a,SAVCH
CLOSEA: psh a
        sjp WAKE
        pop a
        sta (W)
        ldi a,C_CLOSE
        jmp MCMD

; MKHDR: the save file's header into OUTBUF (ZS1, above); YL = the PC's
; offset. Preserves Y.
MKHDR:  ldi xh,>OUTBUF
        ldi xl,<OUTBUF
        ldi a,0x5A              ; "ZS1"
        sin x
        ldi a,0x53
        sin x
        ldi a,0x31
        sin x
        psh y
        lda yl
        sta (TMP)
        ldi uh,>IDTAB
        ldi ul,<IDTAB
        ldi yh,ZBASEH
        ldi a,12
        sta (TCNT)
1$:     lin u
        sta yl
        lin y
        sin x
        lda (TCNT)
        dec a
        sta (TCNT)
        bzr 1$
        pop y
        lda (SPAGE)
        sin x
        lda (SPAGE+1)
        sin x
        lda (TMP)
        sin x
        lda (ZSP+1)
        sec
        sbi a,<ZSTK
        sta (TMP)
        lda (ZSP)
        sbi a,>ZSTK
        sin x
        lda (TMP)
        sin x
        lda (ZFP+1)
        sec
        sbi a,<ZSTK
        sta (TMP)
        lda (ZFP)
        sbi a,>ZSTK
        sin x
        lda (TMP)
        sin x
        rtn

; CHKHDR: Carry set unless OUTBUF holds a save of this story.
CHKHDR: lda (OUTBUF)
        cpi a,0x5A
        bzr 9$
        lda (OUTBUF+1)
        cpi a,0x53
        bzr 9$
        lda (OUTBUF+2)
        cpi a,0x31
        bzr 9$
        ldi xh,>(OUTBUF+3)
        ldi xl,<(OUTBUF+3)
        ldi uh,>IDTAB
        ldi ul,<IDTAB
        ldi yh,ZBASEH
        ldi a,12
        sta (TCNT)
1$:     lin u
        sta yl
        lin y
        sta (TMP)
        lin x
        cpa (TMP)
        bzr 9$
        lda (TCNT)
        dec a
        sta (TCNT)
        bzr 1$
        lda (OUTBUF+18)         ; the stack fits
        cpi a,>(ZSTKEND-ZSTK)
        bcs 9$
        rec
        rtn
9$:     sec
        rtn

; The story header's bytes that tell its saves: release, serial,
; checksum, static base.
IDTAB:  .db 0x02, 0x03, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x0E, 0x0F

FZERO:  ldi a,0                 ; FPOS = 0
        sta (FPOS)
        sta (FPOS+1)
        sta (FPOS+2)
        rtn
FHDR:   ldi a,>OUTBUF           ; the header, at OUTBUF
        sta (FPTR)
        ldi a,<OUTBUF
        sta (FPTR+1)
        ldi a,0
        sta (FREM)
        ldi a,HDRLEN
        sta (FREM+1)
        rtn
FSTACK: ldi a,>ZSTK             ; the stack, ZSTK up to ZSP
        sta (FPTR)
        ldi a,<ZSTK
        sta (FPTR+1)
        lda (ZSP+1)
        sec
        sbi a,<ZSTK
        sta (FREM+1)
        lda (ZSP)
        sbi a,>ZSTK
        sta (FREM)
        rtn
FDYN:   ldi a,ZBASEH            ; dynamic memory, up to the static base
        sta (FPTR)
        ldi a,0
        sta (FPTR+1)
        lda (ZBASEH*256+0x0E)
        sta (FREM)
        lda (ZBASEH*256+0x0F)
        sta (FREM+1)
        rtn

; FXFER: FREM bytes between RAM at FPTR and SAVCH's file at FPOS, 1K a
; request; A = C_WRB (RAM to the file) or C_RDB (the file to RAM). Carry
; set if a request failed or a read came up short. Moves FPOS and FPTR
; on, FREM to 0. Uses X, Y, U.
FXFER:  sta (FCMD)
FX1:    lda (FREM)
        ora (FREM+1)
        bzr FX2
        rec
        rtn
FX2:    lda (FREM)              ; this request: FREM, at most 1K
        cpi a,0x04
        bcr FX3
        ldi a,0x04
        sta (FCNT)
        ldi a,0
        sta (FCNT+1)
        bch FX4
FX3:    sta (FCNT)
        lda (FREM+1)
        sta (FCNT+1)
FX4:    sjp WAKE
        ldi a,SAVCH
        sta (W)
        ldi a,0
        sta (W+1)
        lda (FPOS)
        sta (W+2)
        lda (FPOS+1)
        sta (W+3)
        lda (FPOS+2)
        sta (W+4)
        lda (FCNT)
        sta (W+5)
        lda (FCNT+1)
        sta (W+6)
        lda (FCMD)
        cpi a,C_WRB
        bzr FX5
        lda (FPTR)              ; writing: the bytes at W+8
        sta xh
        lda (FPTR+1)
        sta xl
        ldi yh,>(W+8)
        ldi yl,<(W+8)
        sjp FCOPY
FX5:    lda (FCMD)
        sjp MCMD
        cpi a,ST_OK
        bzr FXBAD
        lda (FCMD)
        cpi a,C_RDB
        bzr FX6
        lda (LENP)              ; reading: all of it, or the file is short
        cpa (FCNT)
        bzr FXBAD
        lda (LENP+1)
        cpa (FCNT+1)
        bzr FXBAD
        ldi xh,>W
        ldi xl,<W
        lda (FPTR)
        sta yh
        lda (FPTR+1)
        sta yl
        sjp FCOPY
FX6:    sjp FNEXT
        lda (FPTR+1)            ; FPTR += FCNT, FREM -= FCNT
        rec
        adc (FCNT+1)
        sta (FPTR+1)
        lda (FPTR)
        adc (FCNT)
        sta (FPTR)
        lda (FREM+1)
        sec
        sbc (FCNT+1)
        sta (FREM+1)
        lda (FREM)
        sbc (FCNT)
        sta (FREM)
        jmp FX1
FXBAD:  sec
        rtn

; FNEXT: FPOS += FCNT.
FNEXT:  lda (FPOS+2)
        rec
        adc (FCNT+1)
        sta (FPOS+2)
        lda (FPOS+1)
        adc (FCNT)
        sta (FPOS+1)
        lda (FPOS)
        adi a,0
        sta (FPOS)
        rtn

; FCOPY: FCNT bytes (not 0) from X to Y.
FCOPY:  lda (FCNT)
        sta uh
        lda (FCNT+1)
        sta ul
1$:     tin
        dec u
        cpi uh,0x00
        bzr 1$
        cpi ul,0x00
        bzr 1$
        rtn

; =====================================================================
; 1OP
; =====================================================================

OPJZ:   lda (ARG1)
        bzr 1$
        lda (ARG1+1)
        bzr 1$
        ldi a,1
        jmp BRANCH
1$:     ldi a,0
        jmp BRANCH

OPSIB:  ldi a,5
        bch OPKIN
OPCHILD: ldi a,6
OPKIN:  sjp OBJ1
        lda (x)
        psh a
        sjp STOREA
        pop a
        jmp BRANCH
OPPARENT: ldi a,4
        sjp OBJ1
        lda (x)
        jmp STOREA

OPPLEN: sjp LDARG1
        lda xh
        bzr 1$
        lda xl
        bzr 1$
        jmp STORE0
1$:     dec x
        sjp RDBYTE
        sjp PSIZE
        jmp STOREA

OPINC:  sjp INCDEC
        inc x
        bch WRREF
OPDEC:  sjp INCDEC
        dec x
; WRREF: the variable at U (as INCDEC left it) = X.
WRREF:  lda xl
        sde u
        lda xh
        sta (u)
        rtn
; INCDEC: X = the variable ARG1 names, U at its low byte.
INCDEC: lda (ARG1+1)
        sjp VARREF
        jmp LDX16

OPPRADDR: sjp LDARG1
        ldi a,0
        jmp PRSTR
OPPRPADDR: sjp LDARG1
        sjp WADDR
        jmp PRSTR
OPPROBJ: lda (ARG1+1)
        sta xl
        sjp PROPS1
        jmp PRSTR

OPREMOVE: lda (ARG1+1)
        jmp REMOVE

OPRET:  sjp LDARG1
        jmp DORET
OPJUMP: sjp LDARG1
        dec x
        dec x
        jmp ADDPC
OPLOAD: sjp INCDEC
        jmp STOREX
OPNOT:  sjp LDARG1
        lda xl
        eai a,0xFF
        sta xl
        lda xh
        eai a,0xFF
        sta xh
        jmp STOREX

; =====================================================================
; 2OP
; =====================================================================

OPJE:   ldi xh,>ARG2
        ldi xl,<ARG2
        lda (ARGC)
        dec a
        sta (TCNT)
1$:     lda (TCNT)
        bzs 3$
        lin x
        cpa (ARG1)
        bzr 2$
        lda (x)
        cpa (ARG1+1)
        bzr 2$
        ldi a,1
        jmp BRANCH
2$:     inc x
        lda (TCNT)
        dec a
        sta (TCNT)
        bch 1$
3$:     ldi a,0
        jmp BRANCH

; CMPS: A = 1 if the value at X < the value at U (signed), else 0.
CMPS:   lda (x)
        eai a,0x80
        sta (CMH1)
        lda (u)
        eai a,0x80
        sta (CMH2)
        inc x
        inc u
        sec
        lda (x)
        sbc (u)
        lda (CMH1)
        sbc (CMH2)
        ldi a,0
        bcs 1$
        ldi a,1
1$:     rtn

OPJL:   ldi xh,>ARG1
        ldi xl,<ARG1
        ldi uh,>ARG2
        ldi ul,<ARG2
        bch JCMP
OPJG:   ldi xh,>ARG2
        ldi xl,<ARG2
        ldi uh,>ARG1
        ldi ul,<ARG1
JCMP:   sjp CMPS
        jmp BRANCH

OPDECCHK: sjp INCDEC
        dec x
        sjp WRREF
        sjp STTMP
        ldi xh,>TMP
        ldi xl,<TMP
        ldi uh,>ARG2
        ldi ul,<ARG2
        bch JCMP
OPINCCHK: sjp INCDEC
        inc x
        sjp WRREF
        sjp STTMP
        ldi xh,>ARG2
        ldi xl,<ARG2
        ldi uh,>TMP
        ldi ul,<TMP
        bch JCMP
STTMP:  lda xh
        sta (TMP)
        lda xl
        sta (TMP+1)
        rtn

OPJIN:  ldi a,4
        sjp OBJ1
        lda (x)
        cpa (ARG2+1)
        bzs 1$
        ldi a,0
        jmp BRANCH
1$:     ldi a,1
        jmp BRANCH

OPTEST: lda (ARG1)
        and (ARG2)
        cpa (ARG2)
        bzr 1$
        lda (ARG1+1)
        and (ARG2+1)
        cpa (ARG2+1)
        bzr 1$
        ldi a,1
        jmp BRANCH
1$:     ldi a,0
        jmp BRANCH

OPOR:   lda (ARG1)
        ora (ARG2)
        sta xh
        lda (ARG1+1)
        ora (ARG2+1)
        sta xl
        jmp STOREX
OPAND:  lda (ARG1)
        and (ARG2)
        sta xh
        lda (ARG1+1)
        and (ARG2+1)
        sta xl
        jmp STOREX

; ATTRU: U = the byte holding attribute ARG2 of object ARG1, A = its bit.
ATTRU:  lda (ARG2+1)
        shr
        shr
        shr
        sjp OBJ1
        lda xh
        sta uh
        lda xl
        sta ul
        lda (ARG2+1)
        ani a,0x07
        ldi xh,>BITS
        ldi xl,<BITS
        adr x
        lda (x)
        rtn

OPTATTR: sjp ATTRU
        and (u)
        jmp BRANCH
OPSATTR: sjp ATTRU
        ora (u)
        sta (u)
        rtn
OPCATTR: sjp ATTRU
        eai a,0xFF
        and (u)
        sta (u)
        rtn

OPSTORE: sjp LDARG2
        lda (ARG1+1)
        sjp VARREF
        lda xh
        sin u
        lda xl
        sta (u)
        rtn

OPINSERT: lda (ARG1+1)
        sjp REMOVE
        lda (ARG2+1)
        sjp OBJADDR
        ldi a,6
        adr x                   ; the destination's first child...
        lda (x)
        sta (TMP2)
        lda (ARG1+1)
        sta (x)                 ; ...is now the object
        ldi a,4
        sjp OBJ1
        lda (ARG2+1)
        sin x                   ; its parent
        lda (TMP2)
        sta (x)                 ; and its sibling, the old first child
        rtn

OPLOADW: lda (ARG2+1)
        shl
        sta xl
        lda (ARG2)
        rol
        sta xh
        bch LOADW
OPLOADB: sjp LDARG2
LOADW:  lda (ARG1+1)
        adr x
        lda (ARG1)
        rec
        adc xh
        sta xh
        lda (OPC)
        ani a,0x1F
        cpi a,0x10
        bzs 1$
        sjp RDWORD
        jmp STOREX
1$:     sjp RDBYTE
        jmp STOREA

OPGETP: lda (ARG2+1)
        sta (PNUM)
        lda (ARG1+1)
        sjp FINDP
        bzs 2$
        aex
        ani a,0x0E
        bzr 1$
        lda (x)                 ; a byte
        jmp STOREA
1$:     sjp LDXX                ; a word
        jmp STOREX
2$:     lda (OBJT)              ; the default
        sta xh
        lda (OBJT+1)
        sta xl
        lda (PNUM)
        dec a
        shl
        adr x
        sjp LDXX
        jmp STOREX
; LDXX: X = the word at RAM address X.
LDXX:   lin x
        sta (TMP)
        lda (x)
        sta xl
        lda (TMP)
        sta xh
        rtn

OPGETPA: lda (ARG2+1)
        sta (PNUM)
        lda (ARG1+1)
        sjp FINDP
        bzr 1$
        jmp STORE0
1$:     lda xh                  ; as a Z address
        sec
        sbi a,ZBASEH
        sta xh
        jmp STOREX

OPNEXTP: lda (ARG2+1)
        bzr 1$
        lda (ARG1+1)            ; 0: the first property
        sjp PROPS
        bch 2$
1$:     sta (PNUM)
        lda (ARG1+1)
        sjp FINDP
        sjp PSIZE
        adr x                   ; the one after it
2$:     lda (x)
        ani a,0x1F
        jmp STOREA

OPADD:  rec
        lda (ARG1+1)
        adc (ARG2+1)
        sta xl
        lda (ARG1)
        adc (ARG2)
        sta xh
        jmp STOREX
OPSUB:  sec
        lda (ARG1+1)
        sbc (ARG2+1)
        sta xl
        lda (ARG1)
        sbc (ARG2)
        sta xh
        jmp STOREX
OPMUL:  lda (ARG1)
        sta (TMP)
        lda (ARG1+1)
        sta (TMP+1)
        lda (ARG2)
        sta (TMP2)
        lda (ARG2+1)
        sta (TMP2+1)
        sjp MUL
        jmp STOREX
OPDIV:  sjp SDIV
        lda (TMP)
        sta xh
        lda (TMP+1)
        sta xl
        lda (NEG)
        ani a,1
        bzs 1$
        sjp NEGX
1$:     jmp STOREX
OPMOD:  sjp SDIV
        lda (TMP3)
        sta xh
        lda (TMP3+1)
        sta xl
        lda (NEG)
        ani a,2
        bzs 1$
        sjp NEGX
1$:     jmp STOREX

; =====================================================================
; VAR
; =====================================================================

OPSTOREW: lda (ARG2+1)
        shl
        sta xl
        lda (ARG2)
        rol
        sta xh
        sjp ADDA1
        sjp DYNPTR
        lda (ARG3)
        sin x
        lda (ARG3+1)
        sta (x)
        rtn
OPSTOREB: sjp LDARG2
        sjp ADDA1
        sjp DYNPTR
        lda (ARG3+1)
        sta (x)
        rtn
; ADDA1: X += ARG1.
ADDA1:  lda (ARG1+1)
        adr x
        lda (ARG1)
        rec
        adc xh
        sta xh
        rtn

OPPUTP: lda (ARG2+1)
        sta (PNUM)
        lda (ARG1+1)
        sjp FINDP
        bzr 1$
        ldi a,8                 ; no such property
        jmp FATAL
1$:     aex
        ani a,0x0E
        bzr 2$
        lda (ARG3+1)
        sta (x)
        rtn
2$:     lda (ARG3)
        sin x
        lda (ARG3+1)
        sta (x)
        rtn

OPPRCHAR: lda (ARG1+1)
        jmp PUTCH

OPPRNUM: sjp LDARG1
        lda xh
        bii a,0x80
        bzs 1$
        sjp NEGX
        ldi a,0x2D
        sjp PUTCH
1$:     ldi a,0
        sta (NDIG)              ; no digit printed yet
        ldi uh,>POW10
        ldi ul,<POW10
2$:     lin u                   ; the next power of ten
        sta (TMP)
        lin u
        sta (TMP+1)
        ldi a,0x30
        sta (TMPO)
3$:     sec                     ; subtract it while it goes
        lda xl
        sbc (TMP+1)
        sta (TMP2+1)
        lda xh
        sbc (TMP)
        bcr 4$
        sta xh
        lda (TMP2+1)
        sta xl
        adi (TMPO),1
        bch 3$
4$:     lda (TMPO)
        cpi a,0x30
        bzr 5$                  ; not 0: print it
        lda (NDIG)
        bzr 5$                  ; a 0 after other digits
        lda ul
        cpi a,<(POW10+10)
        bzr 6$                  ; a leading 0, unless it's the last digit
5$:     lda (TMPO)
        sjp PUTCH
        ldi a,1
        sta (NDIG)
6$:     lda ul
        cpi a,<(POW10+10)
        bzr 2$
        rtn

OPRANDOM: lda (ARG1)
        bii a,0x80
        bzs 2$
        sjp LDARG1              ; negative: seed with -n
        sjp NEGX
        lda xh
        sta (RNG)
        lda xl
        sta (RNG+1)
        jmp STORE0
2$:     lda (ARG1+1)
        bzr 5$
        lda (ARG1)
        bzr 5$
        sjp RESEED              ; 0: start again
        jmp STORE0
5$:     sjp XORSH               ; X = the next value
        lda xh
        sta (TMP)
        lda xl
        sta (TMP+1)
        lda (ARG1)
        sta (TMP2)
        lda (ARG1+1)
        sta (TMP2+1)
        sjp UDIV                ; TMP3 = it mod n
        lda (TMP3)
        sta xh
        lda (TMP3+1)
        sta xl
        inc x
        jmp STOREX

; XORSH: xorshift16 (7, 9, 8) -- X = RNG = the next value. With x = H:L:
; x ^= x << 7 is H ^= H0:L7-1, L ^= L0 << 7; x ^= x >> 9 is L ^= H >> 1;
; x ^= x << 8 is H ^= L.
XORSH:  lda (RNG)
        shr                     ; Carry = H bit 0
        lda (RNG+1)
        ror                     ; H bit 0, L bits 7-1; Carry = L bit 0
        eor (RNG)
        sta (RNG)
        ldi a,0
        ror
        eor (RNG+1)
        sta (RNG+1)
        lda (RNG)
        shr
        eor (RNG+1)
        sta (RNG+1)
        eor (RNG)
        sta (RNG)
        sta xh
        lda (RNG+1)
        sta xl
        rtn

OPPUSH: sjp LDARG1
        jmp PUSHX
OPPULL: sjp POPX
        lda (ARG1+1)
        sjp VARREF
        lda xh
        sin u
        lda xl
        sta (u)
        rtn

; =====================================================================
; Tables
; =====================================================================

T0OP:   .dw RTRUE, RFALSE, OPPRINT, OPPRRET, OPNOP, OPSAVE, OPREST, OPRESTART
        .dw OPRETPOP, OPPOP, OPQUIT, OPNL, SHOWST, OPVERIFY, OPBAD, OPBAD
T1OP:   .dw OPJZ, OPSIB, OPCHILD, OPPARENT, OPPLEN, OPINC, OPDEC, OPPRADDR
        .dw OPBAD, OPREMOVE, OPPROBJ, OPRET, OPJUMP, OPPRPADDR, OPLOAD, OPNOT
T2OP:   .dw OPBAD, OPJE, OPJL, OPJG, OPDECCHK, OPINCCHK, OPJIN, OPTEST
        .dw OPOR, OPAND, OPTATTR, OPSATTR, OPCATTR, OPSTORE, OPINSERT, OPLOADW
        .dw OPLOADB, OPGETP, OPGETPA, OPNEXTP, OPADD, OPSUB, OPMUL, OPDIV
        .dw OPMOD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD
TVAR:   .dw OPCALL, OPSTOREW, OPSTOREB, OPPUTP, OPREAD, OPPRCHAR, OPPRNUM, OPRANDOM
        .dw OPPUSH, OPPULL, OPNOP, OPNOP, OPBAD, OPBAD, OPBAD, OPBAD
        .dw OPBAD, OPBAD, OPBAD, OPNOP, OPNOP, OPNOP, OPBAD, OPBAD
        .dw OPBAD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD, OPBAD

ALPHA:  .ascii "abcdefghijklmnopqrstuvwxyz"
        .ascii "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        .db 0x20, 0x0D          ; A2: (the escape), new line, then...
A2PUNCT:                        ; ...from Z-char 8, ended by 0 for ENCODE
        .ascii "0123456789.,!?_#"
        .db 0x27, 0x22, 0x2F, 0x5C, 0x2D, 0x3A, 0x28, 0x29, 0
BITS:   .db 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
POW10:  .dw 10000, 1000, 100, 10, 1
FATMSG: .ascii "*ZIP ERROR "
        .db 0
RESMSG: .ascii "[Resumed]"
        .db 0x0D, 0
ZIPEND:
