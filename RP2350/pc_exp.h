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

#define EXP_LENGTH_PORT_PAGE 0x07
#define EXP_LENGTH_PORT_ADDRESS 0xFD
#define EXP_MAX_TRANSFER_LEN 1024

#define EXP_STATUS_BUSY 1
#define EXP_STATUS_READY 4 /* was 0 until 2026-09-24: a sleeping RP2350 doesn't drive the
                             bus, so the data window reads 0xFF (pull-ups) -- READY must be
                             neither that nor 0x00. See EXP_COMMAND_DONE. */
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

/* STAGE keyword support (2026-09) -- copies the 6K ROM image from the
 * MCU into external SRAM in six 1024-byte blocks, then leaves
 * ROM_FROM_SRAM active so the LH5801 reads that region directly from
 * SRAM afterward. These three command numbers already existed (added
 * 2026-08-28 for an earlier, never-completed boot-time bootstrap
 * attempt -- see rom.asm's own history) but had no real implementation
 * on any board; STAGE reuses the numbers with an entirely new
 * implementation on both sides. */
#define EXP_COMMAND_ROM_COPY_BEGIN 0x22 /* Sets GreenPAK1's Remap + write-enable and
                                            GreenPAK2's Remap virtual inputs, verifies
                                            via readback. */
#define EXP_COMMAND_ROM_COPY_GET_BLOCK 0x23 /* Stages the next 1024-byte ROM block into
                                                the payload window (EXP_BUFFER_START_ABS,
                                                same shape as READ_FROM_SD_FILE's own
                                                response); LH5801 copies it to the
                                                correct SRAM offset, then issues this
                                                again for the next block. */
#define EXP_COMMAND_ROM_COPY_FINISH 0x24 /* LH5801 writes a 2-byte BE additive checksum
                                             (sum of all 6144 copied bytes, natural
                                             16-bit wraparound -- no multiply, LH5801
                                             has none) to EXP_BUFFER_START_ABS/+1 before
                                             issuing this. MCU computes the same
                                             checksum over its own buffer[8..31] and
                                             compares, then clears GreenPAK1's
                                             write-enable and verifies via readback --
                                             EXP_STATUS_SUCCESS only if both the
                                             checksum matched and the write-enable
                                             clear verified; EXP_STATUS_ERROR
                                             otherwise (Remap is left as-is either way --
                                             this command doesn't revert it). */

/* Live query of GreenPAK1's Remap virtual input (0=ROM_FROM_MCU,
 * 1=ROM_FROM_SRAM), for STAGE's no-argument query mode -- a real I2C
 * round trip, not ROM-side state tracking, so a warm reset that left a
 * prior session's Remap bit set is still reported correctly. Response:
 * 1 byte at EXP_BUFFER_START_ABS (0 or 1), plus (2026-09-24) a second
 * byte at EXP_BUFFER_START_ABS+1: 1 only if Remap is on AND the SRAM copy
 * is known good (a full ROM_COPY_FINISH succeeded and nothing has since
 * reverted/restarted it) -- the ROM's "already staged, skip the copy"
 * test for its boot hook and STAGE RAM. EXP_STATUS_ERROR if the I2C
 * read itself failed -- the response bytes are then undefined and must
 * not be trusted. */
#define EXP_COMMAND_ROM_GET_MODE 0x25

/* MLOG VIEW / MLOG INFO ON/OFF / MLOG CLEAR (2026-09-21) -- a small, durable
 * (flash-backed, see mcu_log.h/.c) rolling log of internal MCU failures a
 * user can't otherwise see any evidence of (e.g. a GreenPAK virtual-input
 * readback that didn't match what was just written). */
#define EXP_COMMAND_LOG_LIST 0x26 /* Same wire shape as EXP_COMMAND_LIST_SD_DIR's
                                      response (count + EXP_DIR_RECORD_SIZE-byte
                                      records + a summary line), reusing SDLS's own
                                      SD_LIST_DISPLAY/UP/DOWN ROM routines verbatim
                                      -- newest entry first, oldest last, then a
                                      "<count> ENTRIES" summary. */
#define EXP_COMMAND_LOG_CLEAR 0x27 /* Erases all entries; does not change
                                       LOG_SET_INFO_ENABLED's own setting. */
#define EXP_COMMAND_LOG_SET_INFO_ENABLED 0x28 /* 1 byte at EXP_BUFFER_START_ABS:
                                                   0=OFF, nonzero=ON. Persisted;
                                                   default OFF. WARN/ERROR are
                                                   always logged regardless of
                                                   this setting. */

/* Per-block SRAM readback verification (2026-09-21) -- added after a real
 * STAGE RAM run staged all 6 blocks cleanly (MLOG VIEW showed "STAGE blk 0
 * staged" through "STAGE blk 5 staged", no duplicates) but then still hit
 * "STAGE GET_BLOCK refused" -- a real 7th GET_BLOCK request the LH5801
 * genuinely issued, despite the block-count arithmetic on both sides being
 * exact (6*1024=6144=ROM_REGION_END-ROM_BASE, confirmed against the actual
 * .equ/#define values, not assumed). Since the counting logic is provably
 * exact, the remaining explanation is that the copy loop's running SRAM
 * pointer didn't end up where it should have -- this closes that gap with
 * hard evidence instead of another assumption: the MCU computes a 16-bit
 * additive checksum (same algorithm as EXP_COMMAND_ROM_COPY_FINISH's
 * whole-image one) over the exact 1024 bytes it just staged for
 * EXP_COMMAND_ROM_COPY_GET_BLOCK, and writes it here, 2 bytes BE, same
 * page as EXP_LENGTH_PORT/EXP_INSTRUCTION and equally outside the
 * 1024-byte payload (0x8000-0x83FF is the ONLY region the block data
 * itself occupies). The ROM's copy routine then reads the block back FROM
 * SRAM (not the payload window, which the next GET_BLOCK is about to
 * overwrite anyway) after its own tin copy and compares against this --
 * proving the SRAM chip genuinely retained what was written, byte for
 * byte, not just that the copy loop ran the expected number of
 * iterations. See EXP_COMMAND_LOG_BLOCK_CHECKSUM for how the result gets
 * reported back. */
#define EXP_BLOCK_CHECKSUM_PAGE 0x07
#define EXP_BLOCK_CHECKSUM_ADDRESS 0xFB

/* ROM reports one block's readback verification result: 1 byte block index
 * (0-5) at EXP_BUFFER_START_ABS, 1 byte match flag (1=match, 0=mismatch) at
 * EXP_BUFFER_START_ABS+1. On a mismatch only, 2 more bytes BE at
 * EXP_BUFFER_START_ABS+2/+3 carry the ROM's own (found) checksum -- the
 * MCU's expected one is still sitting at EXP_BLOCK_CHECKSUM_ABS from this
 * same block's GET_BLOCK response, so it doesn't need resending. MCU logs
 * "STAGE blk N cksum OK" (INFO, gated on MLOG VERBOSE like the rest of
 * STAGE's tracing) or "STAGE blk N EXXXX FYYYY" (ERROR, always logged,
 * E=expected/F=found in 4-digit hex -- exactly MCU_LOG_MSG_MAX characters)
 * accordingly. A mismatch is treated as a real data-integrity failure on
 * the ROM side -- same abort-and-revert path as a GET_BLOCK failure, just
 * pinpointing exactly which block it was instead of only a whole-image
 * mismatch surfacing later at FINISH. */
#define EXP_COMMAND_LOG_BLOCK_CHECKSUM 0x29

/* STAGE DEBUG's per-byte SRAM readback verification (2026-09-21) -- an
 * even finer-grained sibling of EXP_BLOCK_CHECKSUM_PAGE's per-block check,
 * added after that feature narrowed a real failure to "block 0, off by
 * 0x28 overall" but couldn't say which byte. In debug mode only ("STAGE
 * DEBUG" instead of "STAGE RAM"), the ROM's copy routine writes each byte
 * to SRAM and reads it straight back, byte by byte, instead of the normal
 * tin-based bulk copy -- much slower, but pinpoints the exact failing
 * address the instant it happens rather than only a whole-block sum
 * afterward. On a mismatch: 2-byte BE SRAM address at
 * EXP_BUFFER_START_ABS/+1, 1-byte expected (what was written) at +2,
 * 1-byte found (what was read back) at +3. MCU responds with a
 * ready-to-display message reusing the SAME buffer: 1-byte length at
 * EXP_BUFFER_START_ABS, followed by that many ASCII bytes starting at
 * EXP_BUFFER_START_ABS+1 -- the ROM blits this directly via DISP_N_CHARS0
 * (fits comfortably under its 26-char line limit) and the MCU also logs
 * the same text via mcu_log_error (always logged, regardless of MLOG
 * VERBOSE/QUIET) so it's still available via MLOG VIEW afterward. */
#define EXP_COMMAND_STAGE_BYTE_MISMATCH 0x2A

/* MLOG with no argument (2026-09-22, board owner's own request) -- query
 * the current VERBOSE/QUIET state rather than change it. Response: 1
 * byte at EXP_BUFFER_START_ABS, 1 if info-level logging is currently
 * enabled (VERBOSE) or 0 if not (QUIET) -- mirrors
 * mcu_log_get_info_enabled()'s own bool exactly. Cheap: that accessor
 * only ever reads the RAM-mirrored copy of the log header, never flash
 * (see mcu_log.c's own top comment), so this never triggers a flash
 * write the way EXP_COMMAND_LOG_SET_INFO_ENABLED can. */
#define EXP_COMMAND_LOG_GET_INFO_ENABLED 0x2B

/* End-of-keyword marker (2026-09-24), the other half of the STAGE RAM
 * sleep protocol. Every expansion keyword now starts with EC_WAKE (write
 * EXP_COMMAND_CLEAR_STATUS, then poll until EXP_STATUS_READY -- the first
 * write may be lost if the MCU is asleep, which is why the ROM polls for
 * READY rather than trusting that write) and ends with this command
 * (EC_DONE: KEYWORD_RETURN and the SD_RAISE_ERROR_* exits). In STAGE RAM
 * mode the MCU then goes DORMANT until the next read/write trigger; in MCU
 * mode it's a no-op. Status: EXP_STATUS_SUCCESS. See monitor.c's "STAGE RAM
 * sleep" section. */
#define EXP_COMMAND_DONE 0x2C

/* MLOGMSG "text" / MLOGMSG A$ (2026-09-24) -- adds a user note to the MCU
 * log at level USER ("U:" in MLOG VIEW, always recorded). Argument: a
 * string value chunk at EXP_BUFFER_START_ABS+1, the same shape
 * SD_WRITE_VALUE uses -- 'S', 1-byte length, the characters (byte 0
 * unused). Truncated to MCU_LOG_MSG_MAX (23). SUCCESS, or ERROR if the tag
 * isn't 'S'. */
#define EXP_COMMAND_LOG_USER_MESSAGE 0x2D

/* Keyword executor (2026-09-25) -- every expansion keyword's argument
 * parsing and command sequencing now runs on the MCU (keywords.c); the ROM
 * only does what has to happen on the LH5801 side: display, key input,
 * copying to and from its RAM, BASIC variable lookup and raising BASIC
 * errors, and evaluating BASIC expressions. The ROM's KW_START wakes the
 * MCU, copies the keyword's own E1xx token (whose low byte says which
 * keyword this is) and the next EXP_KW_LINE_LEN bytes of the statement to
 * EXP_BUFFER_START_ABS, and sends EXP_COMMAND_KEYWORD. The statement is
 * wherever BASIC's text pointer is -- DISP_BUFFER for a typed command, the
 * program line in a running program -- as BASIC stores it: no spaces
 * outside quotes, BASIC's own keywords tokenized, ended by ':' or 0x0D.
 * SUCCESS means the MCU has written the statement's length to EXP_KW_END
 * and an action block to EXP_KW_ACTION_ABS; anything else is ERROR 1.
 * Actions that need the MCU again afterwards answer with EXP_COMMAND_KEYWORD_
 * CONTINUE, which returns the next action block the same way. */
#define EXP_COMMAND_KEYWORD 0x2E
#define EXP_COMMAND_KEYWORD_CONTINUE 0x2F

#define EXP_KW_LINE_LEN 78

/* Action block: 8 bytes at window offset 0x7E0 -- past the longest
 * listing (2 + EXP_DIR_MAX_ENTRIES * 30 + 26 = 2008 = 0x7D8) and before
 * the listing cursor at 0x7F6. */
#define EXP_KW_ACTION_PAGE 0x07
#define EXP_KW_ACTION_ADDRESS 0xE0
#define EXP_KW_ACT 0        /* action code, below */
#define EXP_KW_ARG 1        /* action argument/flags */
#define EXP_KW_A_HI 2       /* 16-bit operand A, big-endian */
#define EXP_KW_A_LO 3
#define EXP_KW_B_HI 4       /* 16-bit operand B, big-endian */
#define EXP_KW_B_LO 5
#define EXP_KW_ANSWER 6     /* written by the ROM before CONTINUE */
#define EXP_KW_END 7        /* the statement's length (to its ':' or CR), set by
                               the MCU with the first action: every exit resumes
                               BASIC there (VEJ E2) */

/* MCONF settings (2026-09-25) -- mcu_config.h. Byte 0 at
 * EXP_BUFFER_START_ABS is the setting number; bytes 1-2 the 16-bit BE value
 * (GET returns it, SET takes it and persists it to flash). ERROR for an
 * unknown setting number. */
#define EXP_COMMAND_CONFIG_GET 0x30
#define EXP_COMMAND_CONFIG_SET 0x31

#define EXP_KW_ACTION_DONE 0    /* back to BASIC (KEYWORD_RETURN) */
#define EXP_KW_ACTION_SHOW 1    /* show the 26 bytes at EXP_BUFFER_START_ABS, wait for a
                                   key, ANSWER = key (0 for BREAK), CONTINUE */
#define EXP_KW_ACTION_ERROR 2   /* BASIC ERROR ARG */
#define EXP_KW_ACTION_BROWSE 3  /* listing at EXP_BUFFER_START_ABS (LIST_SD_DIR format).
                                   ARG 0: view, CL/Enter/BREAK return to BASIC. ARG
                                   EXP_KW_BROWSE_SELECT: CL/BREAK return, L on an entry
                                   sets ANSWER = its index and CONTINUEs */
#define EXP_KW_ACTION_LOAD 4    /* file already open: READ_FROM_SD_FILE until 0 bytes
                                   into RAM at A, CLOSE, then back to BASIC. ARG flags:
                                   EXP_KW_XFER_BASIC (target = BASIC program start, and
                                   afterwards program end = last byte written),
                                   EXP_KW_LOAD_CALL (CALL B first) */
#define EXP_KW_ACTION_SAVE 5    /* file already created: WRITE_TO_SD_FILE the inclusive
                                   RAM range A..B, CLOSE, back to BASIC. ARG
                                   EXP_KW_XFER_BASIC: the range is the BASIC program */
#define EXP_KW_ACTION_VAR_LOOKUP 6 /* look up variable A (D461H name code) and copy
                                      its type byte, then its raw storage (8 bytes
                                      for a number, the capacity for a string), to
                                      EXP_BUFFER_START_ABS; CONTINUE */
#define EXP_KW_ACTION_VAR_STORE 7  /* copy the storage-sized bytes at
                                      EXP_BUFFER_START_ABS into the variable of the
                                      last VAR_LOOKUP; CONTINUE */
#define EXP_KW_ACTION_STAGE 8   /* run the STAGE copy routine, ARG = STAGE_DEBUG_FLAG */
#define EXP_KW_ACTION_EVAL 9    /* evaluate the BASIC expression A bytes into the
                                   statement: the arithmetic register (8 bytes, TRM
                                   sec.5-3) to EXP_BUFFER_START_ABS, a string's
                                   characters after it at +8, B low byte = where the
                                   expression ended; CONTINUE. A bad expression is
                                   raised as a BASIC error by the ROM itself */

#define EXP_KW_BROWSE_SELECT 0x01
#define EXP_KW_XFER_BASIC 0x01
#define EXP_KW_LOAD_CALL 0x02

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
#define EXP_DIR_MAX_ENTRIES 66 /* was 67 until 2026-09-25: room for the keyword action block */
