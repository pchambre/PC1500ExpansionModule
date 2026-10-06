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

/* SDEOF (2026-09-30) -- used only by keywords.c itself. In: [channel].
 * Out: [1 = no value left for SDINPUT# to read, 0 = more]. ERROR if the
 * channel isn't open. */
#define EXP_COMMAND_SD_CHANNEL_EOF 31

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
 * test for its boot hook and STAGE RAM. A third byte at +2 (2026-09-28) is
 * MCONF AUTOSTAGE: 0 tells the boot hook not to stage at all. A fourth at +3
 * (2026-10-04) is MCONF BLKBD: 1 tells it to set up the external keyboard's
 * driver (EXP_COMMAND_KBD_INSTALL). EXP_STATUS_ERROR if the I2C
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
/* Written by the ROM's KW_START, read (and for STLOAD, rewritten) by the
 * MCU: the statement's argument address (Y) and the stack pointer S. */
#define EXP_KW_TEXT_HI 8
#define EXP_KW_TEXT_LO 9
#define EXP_KW_S_HI 13
#define EXP_KW_S_LO 14

/* The external keyboard's driver (2026-10-04, MCONF BLKBD) -- kbd_seq.h's
 * kbd_loop_install(). The driver is ROM1's own keyboard wait loop (E24AH-
 * E365H), which the module doesn't carry: the boot hook copies it from the
 * machine's ROM to EXP_BUFFER_START_ABS (kbd_seq.h's KBD_LOOP_LEN bytes)
 * and sends this.
 * The MCU checks it is ROM1's (a CRC), patches it to read the external
 * keyboard, and puts it in the ROM image at rom.asm's KBD_LOOP, where the
 * ROM looks for it. SUCCESS, or ERROR for another ROM's loop -- including
 * an older PC-1500 ROM whose hook doesn't work (E2B9H = D5H, kbd_seq.h),
 * where the boot hook then leaves the hook unarmed. The MCU logs which.
 * Puts any staged copy out of date (it was staged without it). */
#define EXP_COMMAND_KBD_INSTALL 0x37

/* The BLKBD keyword (2026-10-04) -- the keyboard's Bluetooth side
 * (kbd_host.h), used only by keywords.c. PAIR forgets any keyboard paired
 * before and looks for one in pairing mode; ERROR if MCONF BLKBD is 0 or
 * the radio isn't working. STATUS: [state (EXP_KBD_*)][code length][code,
 * 6 bytes -- what to type on the keyboard, then Enter][name length][name,
 * up to 16], then at +25, for BLKBD ? (2026-10-05): [reports received,
 * 2 bytes BE][the last one's length][its first 4 bytes][SET_PROTOCOL's
 * answer: handshake << 4 | mode, FFH = none yet][the step a PAIR last
 * failed at: 1 search, 2 connect, 3 connection, 4 pairing][BTstack's
 * status there]. STOP ends a PAIR's
 * search; FORGET drops the bond. */
#define EXP_COMMAND_KBD_PAIR 0x38
#define EXP_COMMAND_KBD_STATUS 0x39
#define EXP_COMMAND_KBD_STOP 0x3A
#define EXP_COMMAND_KBD_FORGET 0x3B
enum {
    EXP_KBD_NONE = 0,       /* no keyboard paired */
    EXP_KBD_SEARCHING = 1,  /* PAIR: looking for one */
    EXP_KBD_CONNECTING = 2, /* found one, connecting */
    EXP_KBD_CODE = 3,       /* it wants the code typed */
    EXP_KBD_CONNECTED = 4,
    EXP_KBD_NOT_FOUND = 5,  /* PAIR's search found none */
    EXP_KBD_FAILED = 6,     /* PAIR's connection or pairing failed */
    EXP_KBD_PAIRED = 7,     /* paired, not connected now (it connects on a key) */
};
#define EXP_KBD_NAME_MAX 16

/* The external keyboard (2026-10-04) -- kbd_seq.h, and rom.asm's keyboard
 * driver (KBD_ANY/KBD_SCAN/KBD_BREAK) on the other end. Three loose bytes
 * of the window that nothing else uses (the action block ends at 0x7EE, EXP_STORE_PARAMS is
 * 0x7F0-0x7F4, the listing cursor 0x7F6-0x7F9). KEY: the matrix index of
 * the key the keyboard is holding down, 0 = none (the MCU writes it).
 * BREAK: a count of ON presses (the MCU writes it); ACK: the count the
 * driver has acted on (the ROM writes it) -- a BREAK is due while they
 * differ. Plain window bytes: reading them needs no command. */
#define EXP_KBD_KEY 0x7EF
#define EXP_KBD_BREAK 0x7F5
#define EXP_KBD_ACK 0x7FA

/* MCONF settings (2026-09-25) -- mcu_config.h. Byte 0 at
 * EXP_BUFFER_START_ABS is the setting number; bytes 1-2 the 16-bit BE value
 * (GET returns it, SET takes it and persists it to flash). ERROR for an
 * unknown setting number. */
#define EXP_COMMAND_CONFIG_GET 0x30
#define EXP_COMMAND_CONFIG_SET 0x31
/* MCONF HOSTNAME (2026-09-28) -- mcu_config.h: [len][chars] at
 * EXP_BUFFER_START_ABS. GET returns it; SET takes it, ERROR (unchanged)
 * for an empty, too long or unprintable name. */
#define EXP_COMMAND_CONFIG_HOSTNAME_GET 0x35
#define EXP_COMMAND_CONFIG_HOSTNAME_SET 0x36

/* FNSAVE/FNLOAD/STSAVE/STLOAD (2026-09-25) -- stores in the MCU's flash
 * (mcu_store.h). Used only by keywords.c itself, not the ROM. Parameters
 * at window offset EXP_STORE_PARAMS: [slot][offset hi][offset lo][length
 * hi][length lo]; data at EXP_BUFFER_START_ABS. ERASE empties a slot, WRITE
 * programs `length` bytes at `offset` (a multiple of 256, into erased
 * flash), READ copies them back. SUCCESS, or ERROR for a bad slot/range. */
#define EXP_COMMAND_STORE_ERASE 0x32
#define EXP_COMMAND_STORE_WRITE 0x33
#define EXP_COMMAND_STORE_READ 0x34
#define EXP_STORE_PARAMS 0x7F0
#define EXP_STORE_SLOT_FNKEYS 0
#define EXP_STORE_SLOT_STATE 1
#define EXP_STORE_SLOT_LINK 2 /* the Link's pairings (link_store.c); not for keywords.c */
#define EXP_STORE_SLOT_BTBONDS 3 /* Bluetooth bonds (bt_store.c); not for keywords.c */
#define EXP_STORE_SLOT_WIFI 4 /* remembered Wi-Fi networks (wifi_store.c); not for keywords.c */

/* BLE (2026-09-27) -- the PC-1500 Link, BLE_PROTOCOL.md. Used only by
 * keywords.c itself, not the ROM. Data at EXP_BUFFER_START_ABS (offset 0);
 * a name is a name slot ([len hi][len lo][up to 40 chars], as the SD
 * commands take). Every one is SUCCESS or ERROR; the MCU logs why an ERROR
 * happened (MLOG).
 *
 * SCAN        in: [seconds]. Out: the peers found, as a LIST_SD_DIR listing
 *             (for BROWSE; may be empty). Remembered for CONNECT.
 * CONNECT     in: [index into the last SCAN's listing]. Connects and
 *             exchanges HELLOs. Out: [len][the peer's name].
 * CONNECT_NAME in: name slot. Scans briefly and connects to the first peer
 *             advertising that name (any case). Out: as CONNECT.
 * DISCONNECT  BYE and disconnect; always SUCCESS.
 * TEXT        in: [len hi][len lo][bytes]: TEXT frames on channel 0.
 * FILE_PUT    in: name slot, then at EXP_BLE_FILE_ARGS [kind][flags][size,
 *             4 bytes BE, FFFFFFFF = unknown]. Starts a save to the peer's
 *             file store; WRITE_TO_SD_FILE and CLOSE_SD_FILE then go to it
 *             (CLOSE's status says whether the peer has the whole file).
 *             ERROR: [BLE_PROTOCOL error code, or 0 = no link / timeout].
 * FILE_GET    in: name slot. Starts a load; READ_FROM_SD_FILE and
 *             CLOSE_SD_FILE then come from the peer. Out: [kind].
 *             ERROR: as FILE_PUT (3 = not found). */
#define EXP_COMMAND_BLE_SCAN 0x40
#define EXP_COMMAND_BLE_CONNECT 0x41
#define EXP_COMMAND_BLE_CONNECT_NAME 0x42
#define EXP_COMMAND_BLE_DISCONNECT 0x43
#define EXP_COMMAND_BLE_TEXT 0x44
#define EXP_COMMAND_BLE_FILE_PUT 0x45
#define EXP_COMMAND_BLE_FILE_GET 0x46

/* Peer-to-peer (2026-09-28, BLE_PROTOCOL.md "Peer-to-peer files"). A
 * transfer is "routed" when the ROM moves the bytes (a LOAD/SAVE action):
 * WRITE/READ/CLOSE_SD_FILE then go to the peer, as for FILE_PUT/FILE_GET.
 * Unrouted, the SD commands stay on the card and the MCU copies between
 * the two with DATA_WRITE/DATA_READ/DATA_CLOSE (BLPUT SD, BLGET "name").
 *
 * ADVERTISE   in: [1 start / 0 stop]. Advertises the Link service (BLADV);
 *             a PC-1500 that connects is a link once HELLOs are exchanged.
 * STATUS      out: [EXP_BLE_STATUS_* flags][len][the peer's name].
 * OFFER       in: as FILE_PUT (flags unused). Sends FILE_OFFER; SUCCESS once
 *             the peer holds it. ERROR: as FILE_PUT (6 = busy).
 * WITHDRAW    FILE_ABORT for our offer; always SUCCESS.
 * OFFER_GET   out: the offer the peer made us, as FILE_PUT's input (the name
 *             slot may be empty). ERROR if there isn't one.
 * ANSWER      in: [1 accept / 0 refuse][1 routed / 0 not]. Sends FILE_ANSWER
 *             for that offer; accepting opens a receiving transfer.
 * SEND        in: [1 routed / 0 not]. Opens the sending transfer once STATUS
 *             says our offer was accepted.
 * DATA_WRITE / DATA_READ / DATA_CLOSE: an unrouted transfer's
 *             WRITE_TO_SD_FILE / READ_FROM_SD_FILE / CLOSE_SD_FILE.
 *             DATA_CLOSE in: [1 = abandon it, telling the peer / 0 = done]. */
#define EXP_COMMAND_BLE_ADVERTISE 0x47
#define EXP_COMMAND_BLE_STATUS 0x48
#define EXP_COMMAND_BLE_OFFER 0x49
#define EXP_COMMAND_BLE_WITHDRAW 0x4A
#define EXP_COMMAND_BLE_OFFER_GET 0x4B
#define EXP_COMMAND_BLE_ANSWER 0x4C
#define EXP_COMMAND_BLE_SEND 0x4D
#define EXP_COMMAND_BLE_DATA_WRITE 0x4E
#define EXP_COMMAND_BLE_DATA_READ 0x4F
#define EXP_COMMAND_BLE_DATA_CLOSE 0x50

/* Peer messaging (2026-09-29, BLE_PROTOCOL.md "Peer messaging": BLSEND,
 * BLRECV, BLSTAT). A message is value chunks ('N' + 8 bytes, 'S' + length +
 * characters, as SD_WRITE_VALUE's), up to EXP_BLE_MSG_MAX bytes.
 *
 * MSG_SEND    in: [len hi][len lo][chunks]. SUCCESS once the peer stored it.
 *             ERROR: [BLE_PROTOCOL error code: 6 = its inbox is full, try
 *             again; 0 = no link / timeout].
 * MSG_WAIT    in: [seconds hi][lo]: the longest the next MSG_RECVs may wait
 *             (0 = not at all, 0xFFFF = for ever). BLRECV sends it first.
 * MSG_RECV    out: [len hi][len lo][chunks], the oldest message (taken out
 *             of the inbox). ERROR if none: [1 = MSG_WAIT's time is up,
 *             0 = keep waiting (the link is up), 2 = no link].
 * MSG_COUNT   out: [messages waiting][1 = linked / 0 = not]. */
#define EXP_COMMAND_BLE_MSG_SEND 0x51
#define EXP_COMMAND_BLE_MSG_WAIT 0x52
#define EXP_COMMAND_BLE_MSG_RECV 0x53
#define EXP_COMMAND_BLE_MSG_COUNT 0x54
/* Keywords used as BASIC functions (2026-09-29): BLSTAT, SDEOF(n). Sent by
 * the ROM itself (rom.asm's FN_CALL), not keywords.c, from inside BASIC's
 * expression evaluator -- possibly in the middle of another keyword.
 * In: the arithmetic register's 8 bytes at EXP_BUFFER_START_ABS -- for a
 * function with an argument, the argument, already evaluated.
 * Out: SUCCESS with the value as 8 bytes of the register's number format
 * at EXP_BUFFER_START_ABS, or ERROR with the BASIC error number at
 * EXP_FN_ERROR. Either way, at EXP_FN_END_OF_KEYWORD, 1 if no keyword is
 * running: this was the end of one, and the ROM sends DONE once it has
 * read the reply (inside a keyword, 0, and that keyword's DONE comes
 * later). */
#define EXP_COMMAND_FN_BLSTAT 0x55
#define EXP_COMMAND_FN_SDEOF 0x56
/* The CE-150 printer/plotter's drawing (2026-09-30, keywords.c/plotter.h).
 * PLOT        in: [len hi][len lo][a PLOT payload: pen, x, y, operations --
 *             plotter.h, BLE_PROTOCOL.md], up to 1000 bytes. Sent as PLOT
 *             frames, split as the link's frame size needs. ERROR: [the
 *             peer's error code (2 = it doesn't take PLOT), or 0 = no link
 *             / timeout]. */
#define EXP_COMMAND_BLE_PLOT 0x57
/* Pairing and authentication (2026-10-03, BLE_PROTOCOL.md sec.7). A link
 * CONNECT/CONNECT_NAME makes is authenticated, or it fails: ERROR with
 * [EXP_BLE_ERR_NOT_PAIRED] (no pairing with that peer) or
 * [EXP_BLE_ERR_AUTH_FAILED] (one side forgot it), the link dropped.
 *
 * PAIR_BEGIN   in: [0][index into the last SCAN] or [1][len][name]. Connects
 *              (or takes the link already up) and exchanges keys. Out:
 *              [6 ASCII digits: the code][len][the peer's name]. ERROR:
 *              [code, 0 = no link].
 * PAIR_CONFIRM in: [1 accept / 0 refuse], this side's user's answer. Out:
 *              [0 = the peer's user hasn't answered yet: ask again after a
 *              POLL; 1 = paired, and the link is now authenticated, then
 *              [len][name]; 2 = refused]. ERROR: [code, 0 = no link].
 * PAIR_ANSWER  in: [1 accept / 0 refuse]: the user's answer to a connector's
 *              pairing (STATUS's EXP_BLE_STATUS_PAIR_ASK; its code is at
 *              EXP_BLE_FILE_ARGS of STATUS's reply, 6 ASCII digits).
 * UNPAIR       in: [len][name], len 0 = every pairing. Out: [how many were
 *              forgotten]. */
#define EXP_COMMAND_BLE_PAIR_BEGIN 0x58
#define EXP_COMMAND_BLE_PAIR_CONFIRM 0x59
#define EXP_COMMAND_BLE_PAIR_ANSWER 0x5A
#define EXP_COMMAND_BLE_UNPAIR 0x5B
#define EXP_BLE_ERR_NOT_PAIRED 8
#define EXP_BLE_ERR_AUTH_FAILED 9

/* Wi-Fi (2026-10-06, wifi_link.h): the WF* keywords. The radio's station
 * mode is up from a connect until DISCONNECT, and the MCU doesn't sleep
 * meanwhile, as for a BLE link. A password is up to EXP_WIFI_PW_MAX
 * characters, an SSID up to EXP_WIFI_SSID_MAX; a network that connects is
 * remembered with its password (EXP_WIFI_REMEMBERED of them).
 *
 * SCAN         Out: the networks found, strongest first, as a LIST_SD_DIR
 *              listing (for BROWSE): the SSID (cut to the name width), and
 *              in the size text the signal and security, "-52 WPA2", with
 *              a '*' after a remembered one. Remembered for CONNECT.
 * CONNECT      in: [index into the last SCAN][pw len, EXP_WIFI_PW_NONE =
 *              none given][pw]. With none given: the remembered password,
 *              or none for an open network. Out: [len][the IP address as
 *              text]. ERROR: [EXP_WIFI_ERR_*].
 * CONNECT_NAME in: [ssid len][ssid, EXP_WIFI_SSID_MAX][pw len][pw], as
 *              CONNECT's; ssid len 0 = the strongest remembered network a
 *              scan finds. Out and ERROR: as CONNECT.
 * DISCONNECT   leaves the network, station mode off; always SUCCESS.
 * STATUS       out: [EXP_WIFI_STATE_*][ssid len][ssid][ip len][ip text].
 * FORGET       in: [len][ssid], len 0 = every remembered network. Out:
 *              [how many were forgotten].
 * FN_WFSTAT    the WFSTAT function (as FN_BLSTAT): STATUS's state. */
#define EXP_COMMAND_WIFI_SCAN 0x60
#define EXP_COMMAND_WIFI_CONNECT 0x61
#define EXP_COMMAND_WIFI_CONNECT_NAME 0x62
#define EXP_COMMAND_WIFI_DISCONNECT 0x63
#define EXP_COMMAND_WIFI_STATUS 0x64
#define EXP_COMMAND_WIFI_FORGET 0x65
#define EXP_COMMAND_FN_WFSTAT 0x66
#define EXP_WIFI_SSID_MAX 32
#define EXP_WIFI_PW_MAX 63
#define EXP_WIFI_PW_NONE 0xFF
#define EXP_WIFI_REMEMBERED 4
#define EXP_WIFI_ERR_FAILED 0        /* no radio, or no answer in time */
#define EXP_WIFI_ERR_NOT_FOUND 1     /* no such network in range */
#define EXP_WIFI_ERR_BAD_PASSWORD 2
#define EXP_WIFI_ERR_NEED_PASSWORD 3 /* secured, and no password given or remembered */
#define EXP_WIFI_ERR_NONE_KNOWN 4    /* CONNECT_NAME "": no remembered network in range */
#define EXP_WIFI_ERR_WEP 5           /* WEP isn't supported */
#define EXP_WIFI_STATE_OFF 0
#define EXP_WIFI_STATE_CONNECTING 1  /* associated, no IP address yet, or lost */
#define EXP_WIFI_STATE_CONNECTED 2
#define EXP_FN_END_OF_KEYWORD 8
#define EXP_FN_ERROR 9
#define EXP_BLE_MSG_MAX 220 /* fits a sealed frame at the link's 247-byte ATT MTU (was 240) */
#define EXP_BLE_MSG_INBOX 8
#define EXP_BLE_STATUS_LINKED 0x01      /* a link, HELLOs exchanged */
#define EXP_BLE_STATUS_ADVERTISING 0x02
#define EXP_BLE_STATUS_OFFER_IN 0x04    /* the peer offered us a file (OFFER_GET) */
#define EXP_BLE_STATUS_ANSWERED 0x08    /* the peer answered our offer... */
#define EXP_BLE_STATUS_ACCEPTED 0x10    /* ...and accepted it */
#define EXP_BLE_STATUS_PAIR_ASK 0x20    /* a connector wants to pair: its code waits for PAIR_ANSWER */
#define EXP_BLE_FILE_ARGS 42 /* after the name slot */
#define EXP_BLE_KIND_BASIC 0
#define EXP_BLE_KIND_M 1
#define EXP_BLE_KIND_UNKNOWN 0xFF
#define EXP_BLE_FLAG_OVERWRITE 0x01
#define EXP_BLE_ERR_EXISTS 4

#define EXP_KW_ACTION_DONE 0    /* back to BASIC (KEYWORD_RETURN) */
#define EXP_KW_ACTION_SHOW 1    /* show the 26 bytes at EXP_BUFFER_START_ABS, wait for a
                                   key, ANSWER = key (0 for BREAK), CONTINUE */
#define EXP_KW_ACTION_ERROR 2   /* BASIC ERROR ARG */
#define EXP_KW_ACTION_BROWSE 3  /* listing at EXP_BUFFER_START_ABS (LIST_SD_DIR format).
                                   ARG 0: view, CL/Enter/BREAK return to BASIC. ARG =
                                   a key (EXP_KW_BROWSE_PICK_*): CL/BREAK return, that
                                   key on an entry sets ANSWER = its index and
                                   CONTINUEs */
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
#define EXP_KW_ACTION_COPY_IN 10  /* copy B bytes of RAM from A to EXP_BUFFER_START_ABS;
                                     CONTINUE */
#define EXP_KW_ACTION_COPY_OUT 11 /* copy B bytes from EXP_BUFFER_START_ABS to RAM at A;
                                     CONTINUE */
#define EXP_KW_ACTION_RESTORE 12  /* STLOAD's last step: with interrupts off and no stack
                                     use, copy B bytes from EXP_BUFFER_START_ABS to RAM
                                     at A; if ARG is non-zero, send CONTINUE (polled
                                     inline) and repeat with the new action block;
                                     then S = EXP_KW_S, and KEYWORD_RETURN */
#define EXP_KW_ACTION_POLL 13     /* sleep one timer wake, then ANSWER = 1 if BREAK was
                                     pressed, else 0; CONTINUE. ARG: EXP_KW_POLL_*. How
                                     a waiting keyword (BLADV, BLPUT, BLGET) lets BREAK
                                     cancel */
#define EXP_KW_POLL_CLEAR 0x01    /* clear an old BREAK first (a wait's first POLL) */
#define EXP_KW_POLL_SHOW 0x02     /* first show the 26 bytes at EXP_BUFFER_START_ABS */

#define EXP_KW_BROWSE_PICK_L 0x4C /* L: SDLOAD's Load */
#define EXP_KW_BROWSE_PICK_C 0x43 /* C: BLSCAN's Connect */
#define EXP_KW_BROWSE_PICK_P 0x50 /* P: BLPAIR's Pair */
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
