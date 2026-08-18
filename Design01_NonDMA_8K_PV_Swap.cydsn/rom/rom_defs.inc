; rom_defs.inc -- sdas .equ mirror of the PSoC-side constants in PC_EXP.h.
;
; NOT auto-derived from PC_EXP.h -- there is no single source of truth
; across the C/asm boundary. If you add/change an EXP_* value in PC_EXP.h,
; update the matching .equ here by hand, or a routine that pokes the
; instruction page/address will silently target the wrong byte.
;
; Layout (see rom.asm's header comment for the full picture): the 8K LH5801
; window splits into a 4K live data window (0x8000-0x8FFF, pages 0-15:
; command/status/parameter exchange with DoCommand(), plus bulk data like
; directory listings) and a 4K ROM region (0x9000-0x9FFF, pages 16-31:
; sentinel + keyword index + keyword table + routines). Pages 16-31 must
; never be poked at runtime -- there's no backing "ROM" chip, it's the same
; RAM buffer as the data window, so a stray write there corrupts the
; keyword table/routines until the next cold boot.
;
; ROM lives at 0x9000, not 0x8000 (the board's own natural base), because
; of a confirmed real-ROM bug:
; the base BASIC ROM's keyword-table walker, on a name mismatch, skips
; forward hunting for the first byte *strictly greater than* 0xE0 to find
; the next entry's code field. Page 8000's own required PV-low code value
; (0xE0 | page_index, page_index=0) is exactly 0xE0 -- sitting *on* that
; boundary, not past it -- so the skip-scan glides straight through it and
; corrupts any multi-entry-per-letter table on that page. Every other page
; (8800->0xE1 ... B800->0xE7) is strictly above the boundary and skip-scans
; correctly -- confirmed live via entertrace, both the failure at 8000 and
; the fix at 8800. 9000's code (0xE2) is safely clear of this.

WINDOW_BASE .equ 0x8000        ; the LH5801-visible window's absolute start (hardware fact, fixed)
ROM_BASE .equ 0x9000           ; where the keyword table/routines actually live
DATA_WINDOW_BASE .equ 0x8000   ; first byte of the live data window

EXP_INSTRUCTION_PAGE .equ 0x0F     ; PC_EXP.h: EXP_INSTRUCTION_PAGE -- last page of the data window
EXP_INSTRUCTION_ADDRESS .equ 0xFF  ; PC_EXP.h: EXP_INSTRUCTION_ADDRESS
EXP_INSTRUCTION_ABS .equ (WINDOW_BASE + (EXP_INSTRUCTION_PAGE * 256) + EXP_INSTRUCTION_ADDRESS)
; = 0x8FFF -- last byte of the 4K data window

EXP_BUFFER_START_PAGE .equ 0       ; first page of the 4K data window
EXP_BUFFER_START_ADDRESS .equ 0x00
EXP_BUFFER_START_ABS .equ (WINDOW_BASE + (EXP_BUFFER_START_PAGE * 256) + EXP_BUFFER_START_ADDRESS)
; = 0x8000

EXP_SCRATCH_PAGE .equ 1            ; second page of the data window (0x8100)
EXP_SCRATCH_ABS .equ (WINDOW_BASE + (EXP_SCRATCH_PAGE * 256))

EXP_STATUS_BUSY .equ 1
EXP_STATUS_READY .equ 0
EXP_STATUS_ERROR .equ 128
EXP_STATUS_NOT_IMPLEMENTED .equ 64
EXP_STATUS_SUCCESS .equ 2

EXP_COMMAND_GET_SD_FREE_SPACE .equ 1
EXP_COMMAND_CREATE_SD_FILE .equ 2
EXP_COMMAND_WRITE_TO_SD_FILE .equ 3
EXP_COMMAND_CLOSE_SD_FILE .equ 4
EXP_COMMAND_GET_SD_FILE_SIZE .equ 5
EXP_COMMAND_READ_SD_VOLUME_LABEL .equ 6
EXP_COMMAND_GET_SD_FILE_NAME .equ 7
EXP_COMMAND_GET_SD_FILE_STATUS .equ 8
EXP_COMMAND_FORMAT_SD_CARD .equ 9
EXP_COMMAND_OPEN_SD_FILE_READ .equ 10
EXP_COMMAND_READ_FROM_SD_FILE .equ 11
EXP_COMMAND_LIST_SD_DIR .equ 12    ; bulk listing -- replaces the old FIRST/NEXT pair
EXP_COMMAND_REMOVE_SD_FILE .equ 14
EXP_COMMAND_GET_SD_VOLUME_SIZE .equ 15

; Directory commands (SDCD/SDMKDIR/SDRMDIR/SDPWD) -- see PC_EXP.h's own
; comment for why every other command above is implicitly relative to
; wherever CHANGE_SD_DIR last left the MCU's single global cwd.
EXP_COMMAND_CHANGE_SD_DIR .equ 16
EXP_COMMAND_MAKE_SD_DIR .equ 17
EXP_COMMAND_REMOVE_SD_DIR .equ 18
EXP_COMMAND_GET_SD_CWD .equ 19

EXP_COMMAND_ROM_FROM_MCU .equ 0x20
EXP_COMMAND_ROM_FROM_SRAM .equ 0x21

EXP_COMMAND_TEST_COPY_STRING .equ 129
EXP_COMMAND_CLEAR_STATUS .equ 0xFF

; EXP_COMMAND_LIST_SD_DIR's bulk listing format -- see PC_EXP.h's own
; comment for the wire format. Per entry: name(16), then size pre-rendered
; as ASCII decimal(10, right-justified/space-padded) -- back-to-back with
; the name so SLS can blit both in one DISP_N_CHARS0 call -- then the same
; size as raw binary(4) trailing. The ROM side has no decimal-to-ASCII
; routine of its own and doesn't need one, since the MCU/mock side already
; renders display-ready text. A further EXP_DIR_SUMMARY_LEN bytes of plain-
; text summary (also MCU/mock-rendered) follow right after the last entry.
EXP_DIR_NAME_LEN .equ 16
EXP_DIR_SIZE_TEXT_LEN .equ 10
EXP_DIR_RECORD_SIZE .equ 30
EXP_DIR_SUMMARY_LEN .equ 26
EXP_DIR_MAX_ENTRIES .equ 135

; Confirmed-correct PV-low code-value formula, page index counted from
; 8000=0 through B800=7 (code_high = 0xE0 | page_index). ROM_BASE=9000 is
; index 2.
ROM_CODE_HIGH .equ 0xE2

ROM_REGION_END .equ (ROM_BASE + 16*256)   ; = 0xA000, one past the last ROM page

; ---------------------------------------------------------------------
; KEYSCAN_WAIT (E243H) key codes -- ACC value on return, no Carry.
; Measured live via a throwaway KTEST keyword (rom/tests/keyprobe.asm)
; that calls KEYSCAN_WAIT and stores ACC/Carry into real user RAM
; (0x4100/0x4101 -- NOT the 0x8000+ data window, which pc1500emu models as
; permanently unwritable, see that file's header comment), confirmed on a
; live pc1500emu instance with the user pressing each key in turn.
; KEY_BREAK is separately confirmed via KEYSCAN_WAIT's own documented
; Carry=1 behavior, not needed here.
KEY_ENTER .equ 0x0D  ; confirmed live
KEY_CL    .equ 0x18  ; confirmed live
KEY_UP    .equ 0x0B  ; confirmed live
KEY_DOWN  .equ 0x0A  ; confirmed live
KEY_Y     .equ 0x59  ; ASCII 'Y' -- NOT YET independently confirmed live like
                      ; the codes above; letter keys are assumed to return
                      ; their plain uppercase ASCII code from KEYSCAN_WAIT
                      ; (matching typed-text tokenization elsewhere), but
                      ; verify against a real keypress before trusting this
                      ; for anything destructive if it hasn't been checked yet
KEY_L     .equ 0x4C  ; ASCII 'L' -- same not-yet-independently-confirmed
                      ; caveat as KEY_Y above

; Base ROM system subroutines (PC-2 Assembly Language manual / known_symbols.cpp),
; called via SJP like any other subroutine.
KEYSCAN_WAIT     .equ 0xE243  ; scan keyboard, block until a key is down; ACC=key code, Carry=1 only for BREAK
DISP_N_CHARS0    .equ 0xED3B  ; output XL chars from (U) to LCD starting at cursor 0
; DISP_N_CHARS (0xED00) -- output ACC chars from (U) to LCD from the
; *current* cursor position, per the PC-2 manual's one-line summary. NOT
; safe as a general-purpose blitter: confirmed via real ROM1.BIN
; disassembly that it compares its own read pointer's low byte against
; 0x787B every iteration and can branch into the BASIC line editor's own
; cursor-blink handling when it matches -- it's coupled to the input-line
; editor's context, not independent of it. Produced garbled output live
; when used from SLS to print from a data-window address. Left defined
; for reference/future real use (e.g. genuinely continuing a line-editor
; echo) but don't reach for it as "DISP_N_CHARS0 but mid-line" again.
DISP_N_CHARS     .equ 0xED00
