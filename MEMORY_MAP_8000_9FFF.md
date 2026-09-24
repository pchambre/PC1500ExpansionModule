# Memory Map: 0x8000-0x9FFF

The expansion module's RP2350 decodes and serves **0x8000-0x9FFF (8K)** on the LH5801 bus.
Nothing above 0x9FFF belongs to this module.

Sources of truth: `Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_defs.inc` and `rom/rom.asm` (ROM
side), `PC_EXP.h` / `RP2350/pc_exp.h` (C side). They are kept in sync by hand. Routine addresses
below come from the assembled listing (`rom/rom.rst`) and move whenever `rom.asm` changes, so
re-check that file after every ROM build.

## Top-level split

| Range | Size | Backing | Purpose |
|---|---|---|---|
| 0x8000-0x87FF | 2K (pages 0-7) | `buffer[0..7]` in RP2350 RAM | Data window: command/status exchange, payloads, ROM-side scratch, and code that must keep running while the ROM region is remapped |
| 0x8800-0x9FFF | 6K (pages 8-31) | `buffer[8..31]` in RP2350 RAM, or the external SRAM when Remap is on | "ROM" region: sentinel, boot hook, keyword index and table, keyword routines |

- **The ROM region isn't real ROM.** It's the same RAM buffer (`buffer[32][256]` in
  `RP2350/monitor.c`), loaded at boot from the assembled `rom.asm` image.
- **Stray writes are only partly blocked.** `write_serve.pio` drops a write when address bit A11
  is set, which covers 0x8800-0x8FFF and 0x9800-0x9FFF. **0x9000-0x97FF has A11 clear, so a stray
  write there still lands in `buffer[]`** and corrupts the image until the next MCU reboot.
- **Remap on (STAGE RAM mode):** GreenPAK1/2's SRAM/ROM Remap is set, so reads of 0x8800-0x9FFF are
  answered by the external SRAM chip, which holds a verified copy of the same image. The RP2350
  isn't involved in those reads at all.

## MCU sleep in STAGE RAM mode

Once the ROM is staged into SRAM, the RP2350 goes DORMANT between keywords.

- **While it sleeps:**
  - Nothing drives the data bus, so the **whole 0x8000-0x87FF window reads 0xFF** (the RP2350's
    data-pin pull-ups).
  - Writes to the window are lost.
  - The access that wakes it is itself lost.
- **What wakes it:** any read or write trigger in the window.
- **Every keyword wakes it first.** Each keyword's table entry points at a `KWE_*` wrapper that
  runs `EC_WAKE`: it writes `CLEAR_STATUS` to 0x87FF twice and polls for `READY` (0x04). The
  keyword then ends with `EC_DONE` (`EXP_COMMAND_DONE`), which lets the MCU sleep again.
- **A wake that isn't followed by a command** within 100 ms (e.g. a `PEEK` into the window) goes
  back to sleep on its own.
- **Code reached by raw `CALL` bypasses `EC_WAKE`:** `RAMTST2`, `ROM_RESET_REMAP`, `MEMCOPY` and
  `MEMCOPY_PV_SWAP` all run from or use the window. In STAGE RAM mode, any of them started while
  the MCU sleeps will fetch or read 0xFF.
- **In MCU mode** (Remap off) the RP2350 never sleeps.

## 0x8000-0x87FF — data window

### Fixed protocol cells

| Address | Symbol | Contents |
|---|---|---|
| 0x8000-0x83FF | `EXP_BUFFER_START_ABS` | Payload window, 1024 bytes (`EXP_MAX_TRANSFER_LEN`), pages 0-3. Command arguments and results, directory listings, SD file data, STAGE's `GET_BLOCK` blocks. |
| 0x8100-0x81FF | `EXP_SCRATCH_ABS` | Page 1 of the payload window (not extra space). Short length-prefixed responses such as `GET_SD_CWD` / `GET_SD_FILE_NAME`. |
| 0x87FB-0x87FC | `EXP_BLOCK_CHECKSUM_ABS` | 2-byte BE additive checksum of the block `ROM_COPY_GET_BLOCK` just staged. |
| 0x87FD-0x87FE | `EXP_LENGTH_PORT_ABS` | 2-byte BE transfer length: request in, actual count out (`READ_FROM_SD_FILE`, `WRITE_TO_SD_FILE`, `ROM_COPY_GET_BLOCK`). |
| 0x87FF | `EXP_INSTRUCTION_ABS` | Command/status byte. The LH5801 writes a command code here; the status appears here. A command write is captured by its own PIO/DMA dispatch path, which stamps `BUSY` in hardware before the LH5801's next access, so the raw command byte is never stored here. |

### ROM-side scratch cells

These are ordinary RAM cells that `rom.asm` itself uses as working storage, all inside the window.

| Address | Symbol(s) | User |
|---|---|---|
| 0x8000-0x8005 | `MEMCOPY_PARAMS_ABS` | `MEMCOPY` / `MEMCOPY_PV_SWAP` parameter block (source, dest, count; 16-bit BE each) |
| 0x811E | `SDSAVE_NAME_STASH_ABS` | SDSAVE: stashed file name |
| 0x814D | `SD_TWONAME_STASH_ABS` | SDCP/SDMV: stashed second name |
| 0x8177-0x8178 | `SD_PTN_XSAVE_HI/LO_ABS` | Two-name parser: saved X |
| 0x8179-0x817A | `SD_VARNAME_HI/LO_ABS` | Variable name code for the base ROM's D461H lookup |
| 0x817B | `SD_VAR_TYPE_ABS` | Variable type/size byte |
| 0x817C-0x817D | `SD_VAR_ADDR_HI/LO_ABS` | Variable address |
| 0x817E-0x817F | `SD_CHUNK_LEN_ABS`, `SD_CHUNK_CAP_ABS` | String-chunk length / capacity |
| 0x8180 | `SD_CHANNEL_ABS` | SDOPEN-family channel number |
| 0x8181-0x8182 | `SD_ARG_XSAVE_HI/LO_ABS` | Saved argument pointer across a variable lookup |
| 0x87C0-0x87CB | `SDLOAD_*_ABS` | SDLOAD: write pointer, name length, mode, address, parse temps, CALL address |
| 0x87CC-0x87DA | `SDSAVE_*_ABS` | SDSAVE: range start/end, CALL address, Y flag, mode, write pointer, chunk length, done flag |
| 0x87F6-0x87F9 | `SD_LIST_INDEX/COUNT/ADDR_HI/ADDR_LO_ABS` | SDLS / MLOG VIEW / SDLOAD picker: browse cursor |

The cells at 0x8100-0x81FF are inside the payload window. Any command that fills the window
overwrites them, so a routine only relies on them between its own commands.

### Resident code in the window

| Range | Contents |
|---|---|
| 0x8000-0x8183 | `RAMTST2`, the SRAM read/write diagnostic (`CALL 32768`). It shares bytes with the payload window and the scratch cells above (0x8177-0x8182), so it only works if called straight after a reset, before any other command has used the window. |
| 0x8400-0x8716 | STAGE's ROM-to-SRAM copy routine (`STAGE_COPY_ROUTINE_ABS`) and its data. It has to run from the window, because once `ROM_COPY_BEGIN` switches Remap, the ROM region is answered by the half-written SRAM. It must never call anything in 0x8800+ until the copy is finished and verified. |
| 0x8717-0x8723 | `ROM_RESET_REMAP` (`CALL 34583`): sends `ROM_FROM_MCU` (Remap and write-enable off) and returns. It uses nothing in 0x8800+, so it's safe in any Remap state. |
| 0x8724-0x87BF | Free |

STAGE copy routine layout, 0x8400-0x8716:

| Address | Label | Contents |
|---|---|---|
| 0x8400 | `STAGE_COPY_START` | BEGIN, per-block GET_BLOCK / copy / verify, FINISH, success exit |
| 0x859A | `STAGE_COPY_BEGIN_FAILED` | BEGIN failed: revert quietly (boot hook) or ERROR 1 (from BASIC) |
| 0x85A7 | `STAGE_COPY_VERIFY_MISMATCH` | Per-byte / per-block mismatch report |
| 0x85F0 | `STAGE_BUILD_MISMATCH_MSG` | Builds the `STAGE @aaaa E:ee F:ff` mismatch message |
| 0x8611 | `HEX_BYTE_TO_ASCII`, `HEX_NIBBLE_TO_ASCII` (0x8630) | Hex formatting helpers |
| 0x863A | `STAGE_COPY_MIDFAIL`, `STAGE_COPY_BADCHECKSUM` (0x8642), `STAGE_COPY_REVERT` (0x8648) | Failure exits: RAM mode reverts to MCU-served ROM; the boot hook does too, without display |
| 0x868C | `STAGE_DEBUG_FAIL_RETURN` | DEBUG-mode failure exit: leaves Remap on and returns to BASIC without touching 0x8800+ |
| 0x86AB-0x86B5 | `STAGE_CHECKSUM_HI/LO`, `STAGE_BLOCK_INDEX`, `STAGE_BLOCK_START_HI/LO`, `STAGE_BLOCK_CKSUM_HI/LO`, `STAGE_DEBUG_FLAG`, `STAGE_BOOT_FLAG`, `STAGE_DEBUG_SRC`, `STAGE_DEBUG_FOUND` | Copy state and flags |
| 0x86B6-0x8716 | `STAGE_OK_MSG`, `STAGE_MIDFAIL_MSG`, `STAGE_CHECKSUM_MSG`, `STAGE_BYTE_MISMATCH_TEMPLATE`, `STAGE_HEX_SCRATCH`, `STAGE_REVERT_FAIL_MSG` | Messages and scratch |

## 0x8800-0x9FFF — ROM region

| Range | Contents |
|---|---|
| 0x8800 | `0x55` sentinel. The base ROM's boot scan and keyword lookup only see this page if it's present. |
| 0x8801-0x8809 | Reserved (header padding) |
| 0x880A-0x881F | `BOOT_SELFCHECK_ENTRY`: the base ROM calls page+0x0A during its boot-time module scan (`STX P`, return address pushed). It holds `jmp STAGE_BOOT_ENTRY` plus padding. The header must stay exactly 32 bytes. |
| 0x8820-0x8853 | First-letter index, 26 × 2-byte BE pointers (A-Z). Non-zero slots: D → `DOSTUFF`, E → `ECVER`, M → `MLOGMSG`, S → `SDDF`. |
| 0x8854-0x893B | Keyword table (below), terminator `0xD0` at 0x893B |
| 0x893C-0x8A35 | STAGE query messages, `STAGE_ROUTINE`, `KEYWORD_RETURN_PROMPT` (0x8A1C) |
| 0x8A36 | `KEYWORD_RETURN`: shared exit for every keyword. It sends `EC_DONE`, then does the BASIC state fix-ups and returns to the prompt. |
| 0x8A7A-0x9C37 | Keyword routines, shared helpers, keyword entry wrappers |
| 0x9C3B-0x9CA0 | `ROM_COPY_TRAMPOLINE`: an older boot-time ROM copy routine. Not called by anything. |
| 0x9CA1-0x9FFF | Free (the image must not pass 0x9FFF; `.org ROM_REGION_END` guards it) |

### Keyword table and keyword addresses

Entry format: `marker | name | code (2 bytes BE) | address (2 bytes BE)`.
- **Marker:** low nibble = name length. Any entry that isn't the first of its letter needs bit 4
  (0x10) clear, because the base ROM's skip-scan rejects a landing marker with that bit set.
- **Code:** the high byte 0xE1 is this page's PV-low code.
- **Address:** points at the keyword's `KWE_*` wrapper. That runs `EC_WAKE`, and on success jumps
  to the routine; if the MCU never answers, it raises ERROR 1.

| Keyword | Table entry | Code | Dispatch (`KWE_*`) | Routine |
|---|---|---|---|---|
| `SDDF` | 0x8854 | 0xE18A | 0x9B88 | 0x9834 |
| `SDFMT` | 0x885D | 0xE189 | 0x9B90 | 0x8C8A |
| `SDLOAD` | 0x8867 | 0xE187 | 0x9B98 | 0x9010 |
| `SDLS` | 0x8872 | 0xE185 | 0x9BA0 | 0x8FD8 |
| `SDRMDIR` | 0x887B | 0xE18E | 0x9BA8 | 0x8E8A |
| `SDRM` | 0x8887 | 0xE188 | 0x9BB0 | 0x8CC7 |
| `SDSAVE` | 0x8890 | 0xE186 | 0x9BB8 | 0x937F |
| `SDCP` | 0x889B | 0xE18B | 0x9BC0 | 0x8D9A |
| `SDCD` | 0x88A4 | 0xE18C | 0x9BC8 | 0x8E46 |
| `SDMKDIR` | 0x88AD | 0xE18D | 0x9BD0 | 0x8E68 |
| `SDPWD` | 0x88B9 | 0xE18F | 0x9BD8 | 0x8EAC |
| `SDMV` | 0x88C3 | 0xE180 | 0x9BE0 | 0x8DF0 |
| `SDOPEN` | 0x88CC | 0xE190 | 0x9BE8 | 0x98AE |
| `SDCLOSE` | 0x88D7 | 0xE191 | 0x9BF0 | 0x9961 |
| `SDINPUT` | 0x88E3 | 0xE192 | 0x9BF8 | 0x99AC |
| `SDPRINT` | 0x88EF | 0xE193 | 0x9C00 | 0x9A17 |
| `SDSKIP` | 0x88FB | 0xE194 | 0x9C08 | 0x9A75 |
| `STAGE` | 0x8906 | 0xE197 | 0x9C10 | 0x8963 |
| `ECVER` | 0x8910 | 0xE195 | 0x9C18 | 0x8A7A |
| `DOSTUFF` | 0x891A | 0xE196 | 0x9C20 | 0x8AA5 |
| `MLOGMSG` | 0x8926 | 0xE199 | 0x9C30 | 0x9B27 |
| `MLOG` | 0x8932 | 0xE198 | 0x9C28 | 0x8ADE |

Table order is load-bearing:
- **The S chain is contiguous**, starting at `SDDF`.
- **`STAGE` is placed inside the S chain**, because the base ROM's skip-scan can't be relied on to
  pass a foreign-letter entry.
- **`MLOGMSG` precedes `MLOG`**, because name matching is a prefix search.
- **The terminator directly follows `MLOG`.**

Keyword arguments:
- `STAGE`:
  - no argument: show the current mode (`STAGE: MCU` / `RAM` / `MODE UNKNOWN`);
  - `RAM`: stage the ROM into SRAM, or return at once if a verified copy is already there;
  - `DEBUG`: slower per-byte write-and-verify copy, which leaves Remap on after a failure for
    inspection;
  - `MCU`: switch back to MCU-served ROM.
- `MLOG`:
  - no argument: show VERBOSE/QUIET;
  - `VIEW`: browse the log;
  - `VERBOSE` / `QUIET`: turn INFO logging on or off;
  - `RESET`: clear the log.
- `MLOGMSG "text"` or `MLOGMSG A$`: add a `U:` note to the MCU log, up to 23 characters.
- `ECVER`: show the expansion ROM version.
- `DOSTUFF [n]`: diagnostic; the MCU stays BUSY for n seconds (default 1).

### `CALL` entry points

| Address | `CALL` | Routine |
|---|---|---|
| 0x8000 | `CALL 32768` | `RAMTST2`: SRAM read/write diagnostic (see its entry above) |
| 0x8717 | `CALL 34583` | `ROM_RESET_REMAP`: force MCU-served ROM (Remap and write-enable off) |
| 0x9863 | `CALL 39011` | `MEMCOPY`: copy `count` bytes, parameters at 0x8000-0x8005 |
| 0x9886 | `CALL 39046` | `MEMCOPY_PV_SWAP`: same, with PV low for each read and high for each write |

### Other internal entry points

| Address | Label | Purpose |
|---|---|---|
| 0x8F7C | `EC_WAIT_NOT_BUSY` | Poll the status byte until it isn't BUSY (HLT between polls) |
| 0x8F8F | `EC_WAKE` | Wake the MCU and wait for READY (tight poll, ~3 s timeout per wait, Carry set on timeout) |
| 0x8FCB | `EC_DONE` | Send `DONE` and wait for it to leave BUSY |
| 0x94A2 | `SD_RAISE_ERROR_1` | Send `DONE`, then raise ERROR 1 (`SD_RAISE_ERROR_40` / `_42` follow it) |
| 0x9ABC | `STAGE_BOOT_ENTRY` | Boot hook body: wake, skip if already staged, otherwise run the copy in boot mode, then `DONE` |
| 0x9ADF | `STAGE_RAM_ENTRY` | `STAGE RAM`: skip if already staged, otherwise run the copy |
| 0x9AEC | `STAGE_DEBUG_ENTRY` | `STAGE DEBUG`: always run the copy |
| 0x9AF4 | `STAGE_SHOW_OK` | Blank the line, show `STAGE: OK`, wait for a key, return |
| 0x9B0C | `STAGE_IS_STAGED` | `ROM_GET_MODE` query; Carry set if Remap is on and the MCU vouches for the copy |

## Status and command reference

**`EXP_STATUS_*`** (read at 0x87FF): `BUSY=1`, `SUCCESS=2`, `EOF=3` (`SD_READ_VALUE` only),
`READY=4`, `NOT_IMPLEMENTED=64`, `ERROR=128`. A sleeping MCU reads as 0xFF, which is why no
status uses 0x00 or 0xFF.

**`EXP_COMMAND_*`** (written to 0x87FF):

| Dec | Hex | Command |
|---|---|---|
| 1 | 0x01 | GET_SD_FREE_SPACE |
| 2 | 0x02 | CREATE_SD_FILE |
| 3 | 0x03 | WRITE_TO_SD_FILE |
| 4 | 0x04 | CLOSE_SD_FILE |
| 5 | 0x05 | GET_SD_FILE_SIZE |
| 6 | 0x06 | READ_SD_VOLUME_LABEL |
| 7 | 0x07 | GET_SD_FILE_NAME |
| 8 | 0x08 | GET_SD_FILE_STATUS |
| 9 | 0x09 | FORMAT_SD_CARD |
| 10 | 0x0A | OPEN_SD_FILE_READ |
| 11 | 0x0B | READ_FROM_SD_FILE |
| 12 | 0x0C | LIST_SD_DIR |
| 13 | 0x0D | TEST_DELAY (diagnostic, `DOSTUFF`) |
| 14 | 0x0E | REMOVE_SD_FILE |
| 15 | 0x0F | GET_SD_VOLUME_SIZE |
| 16 | 0x10 | CHANGE_SD_DIR |
| 17 | 0x11 | MAKE_SD_DIR |
| 18 | 0x12 | REMOVE_SD_DIR |
| 19 | 0x13 | GET_SD_CWD |
| 20 | 0x14 | COPY_SD_FILE |
| 21 | 0x15 | MOVE_SD_FILE |
| 22 | 0x16 | GET_SD_DF_TEXT |
| 23 | 0x17 | CHECK_SD_COPY_MOVE_DEST_EXISTS |
| 24 | 0x18 | SD_OPEN_CHANNEL |
| 25 | 0x19 | SD_CLOSE_CHANNEL |
| 26 | 0x1A | SD_LIST_CHANNELS |
| 27 | 0x1B | SD_WRITE_VALUE |
| 28 | 0x1C | SD_READ_VALUE |
| 29 | 0x1D | SD_SKIP_VALUES |
| 30 | 0x1E | VALIDATE_SD_NAME |
| 32 | 0x20 | ROM_FROM_MCU |
| 33 | 0x21 | ROM_FROM_SRAM |
| 34 | 0x22 | ROM_COPY_BEGIN |
| 35 | 0x23 | ROM_COPY_GET_BLOCK |
| 36 | 0x24 | ROM_COPY_FINISH |
| 37 | 0x25 | ROM_GET_MODE (byte 0: Remap on; byte 1: Remap on and verified copy) |
| 38 | 0x26 | LOG_LIST |
| 39 | 0x27 | LOG_CLEAR |
| 40 | 0x28 | LOG_SET_INFO_ENABLED |
| 41 | 0x29 | LOG_BLOCK_CHECKSUM |
| 42 | 0x2A | STAGE_BYTE_MISMATCH |
| 43 | 0x2B | LOG_GET_INFO_ENABLED |
| 44 | 0x2C | DONE (end of keyword; the MCU may sleep in STAGE RAM mode) |
| 45 | 0x2D | LOG_USER_MESSAGE (`MLOGMSG`; string chunk at 0x8001: `'S'`, length, text) |
| 129 | 0x81 | TEST_COPY_STRING |
| 255 | 0xFF | CLEAR_STATUS (sets READY; also the write `EC_WAKE` uses to wake the MCU) |
