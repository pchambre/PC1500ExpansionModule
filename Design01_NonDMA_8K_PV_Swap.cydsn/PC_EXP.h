/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/
//The 8K LH5801 window (0x8000-0x9FFF) splits into a 2K live data window
//(0x8000-0x87FF, pages 0-7: everything below) and a 6K ROM region
//(0x8800-0x9FFF, pages 8-31: sentinel + keyword index + keyword table +
//routines, see rom/rom.asm). Pages 8-31 must never be poked at runtime --
//there's no real ROM chip, it's the same RAM buffer, so a stray write
//there corrupts the keyword table until the next cold boot. (Moved here
//from 0x9000 -- 4K ROM/4K data -- to grow the ROM region to 6K as the
//keyword table filled up, 2026-08-18 session.)
//
//ROM still doesn't live at the board's natural 0x8000, because of a
//confirmed real-ROM bug: the base BASIC ROM's keyword-table walker skips
//forward hunting for the first byte *strictly greater than* 0xE0 to find
//the next table entry, and page 8000's own required PV-low code value is
//exactly 0xE0 -- sitting on that boundary, not past it -- so any table
//with more than one entry sharing a first letter silently breaks there.
//8800's own required code (0xE1) is safely clear of it. See
//rom/rom_defs.inc for the full writeup.
#define EXP_INSTRUCTION_ADDRESS 0xFF //Command port for PC to submit requests
#define EXP_INSTRUCTION_PAGE 0x07 //Command page for PC to submit requests -- last page of the data window
#define EXP_BUFFER_START_PAGE 0 //First page of the read/write data exchange area -- first page of the data window
#define EXP_BUFFER_START_ADDRESS 0x00 //First laddress of the read/write data exchange area

#define EXP_FULL_INSTRUCTION_ADDRESS = 0x0FFF

#define EXP_STATUS_BUSY 1
#define EXP_STATUS_READY 0
#define EXP_STATUS_ERROR 128
#define EXP_STATUS_NOT_IMPLEMENTED 64
#define EXP_STATUS_SUCCESS 2
#define EXP_STATUS_EOF 3  //EXP_COMMAND_SD_READ_VALUE only: legitimately ran out of stored
                           //values (SDINPUT# fills remaining variables with 0/blank, not an
                           //error) -- distinct from EXP_STATUS_ERROR, which SD_SKIP_VALUES uses
                           //for the same "ran out" condition, since SDSKIP# raises a real
                           //ERROR 40 for that instead.

#define EXP_COMMAND_GET_SD_FREE_SPACE 1
#define EXP_COMMAND_CREATE_SD_FILE 2
#define EXP_COMMAND_WRITE_TO_SD_FILE 3
#define EXP_COMMAND_CLOSE_SD_FILE 4
#define EXP_COMMAND_GET_SD_FILE_SIZE 5
#define EXP_COMMAND_READ_SD_VOLUME_LABEL 6
#define EXP_COMMAND_GET_SD_FILE_NAME 7 //name of the currently-open file, not a filesystem lookup
#define EXP_COMMAND_GET_SD_FILE_STATUS 8
#define EXP_COMMAND_FORMAT_SD_CARD 9

#define EXP_COMMAND_OPEN_SD_FILE_READ 10 //mirrors CREATE_SD_FILE, opens for read instead of write
#define EXP_COMMAND_READ_FROM_SD_FILE 11 //mirrors WRITE_TO_SD_FILE
#define EXP_COMMAND_LIST_SD_DIR 12       //ls: whole listing in one shot, see EXP_DIR_* below
#define EXP_COMMAND_REMOVE_SD_FILE 14    //rm
#define EXP_COMMAND_GET_SD_VOLUME_SIZE 15 //df: total size, alongside GET_SD_FREE_SPACE's free size

//Directory commands (SDCD/SDMKDIR/SDRMDIR/SDPWD in rom/rom.asm) -- all
//four operate on SEGGER emFile's own single global current-directory
//concept (FS_ChDir/FS_MkDir/FS_RmDir/FS_GetCWD), so every other command
//above that takes a bare filename (CREATE_SD_FILE, OPEN_SD_FILE_READ,
//LIST_SD_DIR, ...) is implicitly relative to wherever CHANGE_SD_DIR last
//left it, with no changes needed on their own end.
#define EXP_COMMAND_CHANGE_SD_DIR 16  //cd: argument is length-prefixed, same convention as
                                       //CREATE_SD_FILE's filename
#define EXP_COMMAND_MAKE_SD_DIR 17    //mkdir
#define EXP_COMMAND_REMOVE_SD_DIR 18  //rmdir -- emFile's FS_RmDir only removes an empty directory
#define EXP_COMMAND_GET_SD_CWD 19     //pwd: response is length-prefixed into EXP_SCRATCH_PAGE,
                                       //same convention as GET_SD_FILE_NAME's response

//Max length of a single quoted path/name argument (SDLOAD/SDSAVE/SDCD/
//SDMKDIR/SDRMDIR/SDRM/SDCP/SDMV all use this, via rom.asm's SD_PARSE_
//QUOTED_NAME) -- deliberately separate from EXP_DIR_NAME_LEN, which is
//only the SDLS *display column* width and has nothing to do with how long
//a path argument may be. Every SD command accepts a full path now: a
//plain filename, a relative path ("SUB/FILE.BAS", "../FILE.BAS"), or an
//absolute one from the SD root ("/SUB/FILE.BAS") -- see main.c's
//ConvertSdPathToFsSeparators/PrepareFsName and ExpansionMock::resolvePath
//for how each side interprets these the same way.
#define EXP_PATH_ARG_LEN 40

//SDCP/SDMV (rom/rom.asm) both take two quoted names -- source and
//destination, each a full path per EXP_PATH_ARG_LEN's own comment. The
//wire layout is two fixed-size EXP_TWO_NAME_SLOT_LEN-byte slots
//back-to-back starting at EXP_BUFFER_START_ABS (source first, then
//destination), each shaped exactly like every other quoted-name argument
//(2-byte BE length + up to EXP_PATH_ARG_LEN bytes) -- fixed-width
//specifically so the ROM side never needs to compute the second slot's
//offset from the first name's actual length (the LH5801 has no multiply
//instruction, and this keeps SD_PARSE_TWO_QUOTED_NAMES a plain two-slot
//copy rather than address arithmetic). If the destination resolves to an
//existing directory, the real target is that directory plus the source's
//own basename (matching Unix cp/mv's own "copy/move INTO a directory"
//behavior) -- see main.c's ResolveCopyOrMoveDestination. An existing
//destination is overwritten only after confirmation (EXP_COMMAND_CHECK_
//SD_COPY_MOVE_DEST_EXISTS below), unless a trailing ",-Y" is given,
//matching SDSAVE/SDRM's own convention.
#define EXP_TWO_NAME_SLOT_LEN (2 + EXP_PATH_ARG_LEN)
#define EXP_COMMAND_COPY_SD_FILE 20   //cp
#define EXP_COMMAND_MOVE_SD_FILE 21   //mv
#define EXP_COMMAND_GET_SD_DF_TEXT 22 //df: pre-rendered "<free>F / <total>T" text response,
                                       //length-prefixed into EXP_SCRATCH_PAGE like GET_SD_CWD --
                                       //the ROM has no decimal-to-ASCII conversion of its own
                                       //(see EXP_COMMAND_LIST_SD_DIR's own comment on this), so
                                       //unlike GET_SD_FREE_SPACE/GET_SD_VOLUME_SIZE's raw 4-byte
                                       //binary values, this one arrives pre-formatted for display.
#define EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS 23  //same two-name wire layout and destination
                                       //resolution as COPY_SD_FILE/MOVE_SD_FILE -- SUCCESS means
                                       //the real (resolved) target already exists and needs an
                                       //overwrite prompt, ERROR means it doesn't (or the
                                       //arguments were malformed, which COPY/MOVE_SD_FILE will
                                       //also independently fail on right after).

//SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP# (rom.asm) -- up to EXP_MAX_SD_CHANNELS
//files open at once, numbered 1..EXP_MAX_SD_CHANNELS (channel 0 is reserved
//as the "ALL" sentinel for EXP_COMMAND_SD_CLOSE_CHANNEL only). Unlike every
//other SD command, these are *variable*-oriented, not filename-oriented:
//main.c/ExpansionMock never see a BASIC variable name or value at all --
//rom.asm resolves each named variable's real address itself (via the base
//ROM's own D461H "variable address search" system subroutine, confirmed
//live this session against both a numeric and a string simple variable;
//see rom.asm's own SD_LOOKUP_VARIABLE comment) and builds/consumes a
//self-describing "chunk" directly from that address:
//  - numeric: ['N'] + 8 raw bytes (the variable's own in-memory decimal-
//    float representation, copied byte for byte -- no ASCII conversion).
//  - string:  ['S'] + 1-byte length + that many raw ASCII bytes (a simple
//    variable's string storage is always <=16 characters -- confirmed
//    live -- so 1 byte is always enough).
//This keeps main.c/ExpansionMock's own job purely mechanical: manage
//EXP_MAX_SD_CHANNELS open files and move opaque chunk bytes to/from
//whichever one is named, tracking each channel's own read position
//separately from where writes land (SDPRINT# always appends to the end,
//SDINPUT#/SDSKIP# advance a persistent read cursor -- see
//EXP_COMMAND_SD_WRITE_VALUE/READ_VALUE/SKIP_VALUES below).
#define EXP_MAX_SD_CHANNELS 16

#define EXP_COMMAND_SD_OPEN_CHANNEL 24  //open: window[0]=channel(1-16), then a length-prefixed
                                       //filename (2-byte BE length + up to EXP_PATH_ARG_LEN
                                       //bytes) starting at window[1], same shape as every other
                                       //quoted-name argument. Reusing an already-open channel
                                       //number closes it first.
#define EXP_COMMAND_SD_CLOSE_CHANNEL 25  //close: window[0]=channel, or 0 for "close all"
#define EXP_COMMAND_SD_LIST_CHANNELS 26  //bare SDOPEN's listing -- reuses EXP_COMMAND_LIST_SD_DIR's
                                       //own wire format exactly (see EXP_DIR_* below), one record
                                       //per open channel, name field showing "<n>:<filename>", so
                                       //rom.asm's existing SD_LIST_INIT/SD_LIST_DISPLAY browse
                                       //machinery can be reused verbatim, just pointed at this
                                       //command instead of LIST_SD_DIR.
#define EXP_COMMAND_SD_WRITE_VALUE 27  //SDPRINT#: window[0]=channel, window[1..]=one chunk (see
                                       //above) built by rom.asm from a looked-up variable's own
                                       //storage. Always appends to the end of the channel's file,
                                       //regardless of the channel's own read position.
#define EXP_COMMAND_SD_READ_VALUE 28   //SDINPUT#/SDSKIP#'s own per-value read: window[0]=channel
                                       //(request); response overwrites window[0..] with the next
                                       //chunk read from the channel's persistent read position
                                       //(advanced past it on return), or EXP_STATUS_EOF if the
                                       //channel has no more stored values -- not an error; SDINPUT#
                                       //fills any remaining requested variables with 0/blank.
#define EXP_COMMAND_SD_SKIP_VALUES 29  //SDSKIP#: window[0]=channel, window[1..2]=count (2-byte BE)
                                       //of values to skip forward. Advances the channel's read
                                       //position past that many whole chunks (without transferring
                                       //their content) if all of them exist; EXP_STATUS_ERROR (not
                                       //EOF) and the read position left unchanged if the channel
                                       //runs out first -- SDSKIP# raises a real ERROR 40 for this.

//Validates+uppercase-folds, IN PLACE, the length-prefixed raw name
//SD_PARSE_QUOTED_NAME (rom.asm) has already staged as a length-prefixed
//argument at EXP_BUFFER_START_ABS -- window[0..1]=length (2-byte BE),
//window[2..]=the raw characters (overwritten with the folded/validated
//result on success; contents undefined on failure). Moved here from
//rom.asm 2026-08-19 -- was pure inline character classification (an
//LH5801 state machine), much more naturally expressed in C, and the MCU
//already receives the full name for every command that uses one.
//
//Every SD command's name argument is a full path (a plain filename, a
//relative path with '/'/'.'/'..' components, or an absolute one starting
//with '/' from the SD root -- see ExpansionMock::resolvePath/main.c's
//PrepareFsName for how each side interprets these), so each
//'/'-separated segment must independently be <=8 characters, optionally
//followed by '.' and <=3 more, with at most one '.' -- except a segment
//that is exactly "." or "..", which is always allowed through untouched.
//'+' (the SD path convention's typable stand-in for a real FAT short
//name's '~') needs no special handling here -- it's an ordinary
//character for shape-counting purposes; the actual '+'<->'~' translation
//happens at the PSoC/mock filesystem boundary, not here.
//EXP_STATUS_SUCCESS = valid (window already updated in place);
//EXP_STATUS_ERROR = shape violation (rom.asm's SD_PARSE_QUOTED_NAME maps
//this onto the same "malformed" Carry-set exit it always had, so every
//call site needed zero changes).
#define EXP_COMMAND_VALIDATE_SD_NAME 30

#define EXP_COMMAND_ROM_FROM_MCU 0x20
#define EXP_COMMAND_ROM_FROM_SRAM 0x21

#define EXP_COMMAND_TEST_COPY_STRING 129

#define EXP_COMMAND_CLEAR_STATUS 0xFF

#define EXP_SD_FILE_STATUS_CLOSED 0
#define EXP_SD_FILE_STATUS_OPEN_WRITE 1
#define EXP_SD_FILE_STATUS_OPEN_READ 2

//Page used to stage string-type command responses (volume label, file
//name). Must stay inside the data window (0-7) -- pages 8-31 are the
//real ROM/keyword-table region in rom/rom.asm and must never be written
//at runtime. (EXP_COMMAND_TEST_COPY_STRING's hardcoded buffer[4] is a
//harmless data-window page under this layout -- still true after the
//8800-base move, since pages 0-7 are unaffected either way.)
#define EXP_SCRATCH_PAGE 1

//EXP_COMMAND_LIST_SD_DIR's bulk listing format, written starting at
//EXP_BUFFER_START_PAGE/ADDRESS: a 2-byte BE entry count, then that many
//fixed-width records (EXP_DIR_RECORD_SIZE bytes each: EXP_DIR_NAME_LEN
//bytes of space-padded/truncated ASCII name, then EXP_DIR_SIZE_TEXT_LEN
//bytes of that file's size pre-rendered as right-justified space-padded
//ASCII decimal, then the same size again as a 4-byte BE binary value.
//Name+size-text deliberately sit back-to-back first (not name+binary+text)
//so the ROM side can blit both in one shot -- see rom/rom.asm's SLS,
//which draws EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN bytes straight off the
//window with a single DISP_N_CHARS0 call. The ROM has no decimal-to-ASCII
//conversion of its own -- deliberately: the MCU (or, for testing,
//pc1500emu's ExpansionMock) has trivial access to real number formatting,
//so it renders display-ready text once here instead. The 4-byte binary
//size trails the record for any future consumer that needs the real
//value (e.g. summing sizes), not because the ROM uses it.
//Right after the last entry, a further EXP_DIR_SUMMARY_LEN bytes of
//free-form text summarize the listing (e.g. "3 FILES 23051B 2122343F") --
//again plain text, not a fixed-format record, since there's nothing for
//the ROM side to parse out of it beyond blitting it whole.
//Fixed-width so the ROM-side browser can index directly (entry_addr =
//window + 2 + i * EXP_DIR_RECORD_SIZE) instead of parsing variable-length
//records. EXP_DIR_MAX_ENTRIES keeps the whole listing -- entries *and* the
//summary line that follows them -- inside the 2K data window, clear of
//the instruction byte at its last address: (2048 - 1 - 2 - 26) /
//EXP_DIR_RECORD_SIZE = 67. (Was 135 at the old 4K data window size --
//recomputed for the 8800-base move's smaller 2K window, 2026-08-18.)
#define EXP_DIR_NAME_LEN 16
#define EXP_DIR_SIZE_TEXT_LEN 10
#define EXP_DIR_RECORD_SIZE 30
#define EXP_DIR_SUMMARY_LEN 26
#define EXP_DIR_MAX_ENTRIES 67

    /* Defines for DMA_1 */
#define DMA_1_BYTES_PER_BURST 1
#define DMA_1_REQUEST_PER_BURST 1
#define DMA_1_SRC_BASE (CYDEV_PERIPH_BASE)
#define DMA_1_DST_BASE (CYDEV_SRAM_BASE)

/* [] END OF FILE */
