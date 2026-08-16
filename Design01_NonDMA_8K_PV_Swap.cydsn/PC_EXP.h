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
//The 8K LH5801 window (0x8000-0x9FFF) splits into a 4K live data window
//(0x8000-0x8FFF, pages 0-15: everything below) and a 4K ROM region
//(0x9000-0x9FFF, pages 16-31: sentinel + keyword index + keyword table +
//routines, see rom/rom.asm). Pages 16-31 must never be poked at runtime --
//there's no real ROM chip, it's the same RAM buffer, so a stray write
//there corrupts the keyword table until the next cold boot.
//
//ROM lives at 0x9000, not the board's natural 0x8000, because of a
//confirmed real-ROM bug: the base BASIC ROM's keyword-table walker skips
//forward hunting for the first byte *strictly greater than* 0xE0 to find
//the next table entry, and page 8000's own required PV-low code value is
//exactly 0xE0 -- sitting on that boundary, not past it -- so any table
//with more than one entry sharing a first letter silently breaks there.
//9000's required code (0xE2) is safely clear of it. See rom/rom_defs.inc
//for the full writeup.
#define EXP_INSTRUCTION_ADDRESS 0xFF //Command port for PC to submit requests
#define EXP_INSTRUCTION_PAGE 0x0F //Command page for PC to submit requests -- last page of the data window
#define EXP_BUFFER_START_PAGE 0 //First page of the read/write data exchange area -- first page of the data window
#define EXP_BUFFER_START_ADDRESS 0x00 //First laddress of the read/write data exchange area

#define EXP_FULL_INSTRUCTION_ADDRESS = 0x0FFF

#define EXP_STATUS_BUSY 1
#define EXP_STATUS_READY 0
#define EXP_STATUS_ERROR 128
#define EXP_STATUS_NOT_IMPLEMENTED 64
#define EXP_STATUS_SUCCESS 2

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

#define EXP_COMMAND_ROM_FROM_MCU 0x20
#define EXP_COMMAND_ROM_FROM_SRAM 0x21

#define EXP_COMMAND_TEST_COPY_STRING 129

#define EXP_COMMAND_CLEAR_STATUS 0xFF

#define EXP_SD_FILE_STATUS_CLOSED 0
#define EXP_SD_FILE_STATUS_OPEN_WRITE 1
#define EXP_SD_FILE_STATUS_OPEN_READ 2

//Page used to stage string-type command responses (volume label, file
//name). Must stay inside the data window (0-15) -- pages 16-31 are the
//real ROM/keyword-table region in rom/rom.asm and must never be written
//at runtime. (EXP_COMMAND_TEST_COPY_STRING's hardcoded buffer[4] is a
//harmless data-window page under this layout -- the ROM content it used
//to collide with, before the 9000-base swap, now lives at page 20, not
//page 4.)
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
//summary line that follows them -- inside the 4K data window, clear of
//the instruction byte at its last address: (4096 - 1 - 2 - 26) /
//EXP_DIR_RECORD_SIZE = 135.
#define EXP_DIR_NAME_LEN 16
#define EXP_DIR_SIZE_TEXT_LEN 10
#define EXP_DIR_RECORD_SIZE 30
#define EXP_DIR_SUMMARY_LEN 26
#define EXP_DIR_MAX_ENTRIES 135

    /* Defines for DMA_1 */
#define DMA_1_BYTES_PER_BURST 1
#define DMA_1_REQUEST_PER_BURST 1
#define DMA_1_SRC_BASE (CYDEV_PERIPH_BASE)
#define DMA_1_DST_BASE (CYDEV_SRAM_BASE)

/* [] END OF FILE */
