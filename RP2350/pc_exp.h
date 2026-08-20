/* pc_exp.h
 *
 * Copy of Design01_NonDMA_8K_PV_Swap.cydsn/PC_EXP.h's wire-protocol
 * constants (EXP_COMMAND_*, EXP_STATUS_*, EXP_DIR_*, etc). Kept in sync
 * by hand with that file -- this project already does this same thing
 * elsewhere (e.g. pc1500emu's ExpansionMock mirrors the same protocol in
 * C++; rom.asm and this MCU-side header are the two real ends of the
 * wire and have to agree). The DMA_1_* PSoC-only defines at
 * the bottom of the original are intentionally not copied -- they're
 * PSoC Creator DMA-channel boilerplate, not part of the protocol.
 *
 * See PC_EXP.h itself for the full commentary on each constant; this
 * file only reproduces the values, trimmed of PSoC-specific comments
 * that don't apply here.
 */
#pragma once

#define EXP_INSTRUCTION_ADDRESS 0xFF
#define EXP_INSTRUCTION_PAGE 0x07
#define EXP_BUFFER_START_PAGE 0
#define EXP_BUFFER_START_ADDRESS 0x00

#define EXP_STATUS_BUSY 1
#define EXP_STATUS_READY 0
#define EXP_STATUS_ERROR 128
#define EXP_STATUS_NOT_IMPLEMENTED 64
#define EXP_STATUS_SUCCESS 2
#define EXP_STATUS_EOF 3

#define EXP_COMMAND_GET_SD_FREE_SPACE 1
#define EXP_COMMAND_CREATE_SD_FILE 2
#define EXP_COMMAND_WRITE_TO_SD_FILE 3
#define EXP_COMMAND_CLOSE_SD_FILE 4
#define EXP_COMMAND_GET_SD_FILE_SIZE 5
#define EXP_COMMAND_READ_SD_VOLUME_LABEL 6
#define EXP_COMMAND_GET_SD_FILE_NAME 7
#define EXP_COMMAND_GET_SD_FILE_STATUS 8
#define EXP_COMMAND_FORMAT_SD_CARD 9

#define EXP_COMMAND_OPEN_SD_FILE_READ 10
#define EXP_COMMAND_READ_FROM_SD_FILE 11
#define EXP_COMMAND_LIST_SD_DIR 12
#define EXP_COMMAND_REMOVE_SD_FILE 14
#define EXP_COMMAND_GET_SD_VOLUME_SIZE 15

#define EXP_COMMAND_CHANGE_SD_DIR 16
#define EXP_COMMAND_MAKE_SD_DIR 17
#define EXP_COMMAND_REMOVE_SD_DIR 18
#define EXP_COMMAND_GET_SD_CWD 19

#define EXP_PATH_ARG_LEN 40

#define EXP_TWO_NAME_SLOT_LEN (2 + EXP_PATH_ARG_LEN)
#define EXP_COMMAND_COPY_SD_FILE 20
#define EXP_COMMAND_MOVE_SD_FILE 21
#define EXP_COMMAND_GET_SD_DF_TEXT 22
#define EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS 23

#define EXP_MAX_SD_CHANNELS 16

#define EXP_COMMAND_SD_OPEN_CHANNEL 24
#define EXP_COMMAND_SD_CLOSE_CHANNEL 25
#define EXP_COMMAND_SD_LIST_CHANNELS 26
#define EXP_COMMAND_SD_WRITE_VALUE 27
#define EXP_COMMAND_SD_READ_VALUE 28
#define EXP_COMMAND_SD_SKIP_VALUES 29

#define EXP_COMMAND_VALIDATE_SD_NAME 30

#define EXP_COMMAND_ROM_FROM_MCU 0x20
#define EXP_COMMAND_ROM_FROM_SRAM 0x21

#define EXP_COMMAND_TEST_COPY_STRING 129

#define EXP_COMMAND_CLEAR_STATUS 0xFF

#define EXP_SD_FILE_STATUS_CLOSED 0
#define EXP_SD_FILE_STATUS_OPEN_WRITE 1
#define EXP_SD_FILE_STATUS_OPEN_READ 2

#define EXP_SCRATCH_PAGE 1

#define EXP_DIR_NAME_LEN 16
#define EXP_DIR_SIZE_TEXT_LEN 10
#define EXP_DIR_RECORD_SIZE 30
#define EXP_DIR_SUMMARY_LEN 26
#define EXP_DIR_MAX_ENTRIES 67
