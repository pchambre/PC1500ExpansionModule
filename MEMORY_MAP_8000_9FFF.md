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
| 0x8000-0x87FF | 2K (pages 0-7) | `buffer[0..7]` in RP2350 RAM | Data window: command/status exchange, payloads, ROM-side scratch, and the code that must keep running while the ROM region is remapped |
| 0x8800-0x9FFF | 6K (pages 8-31) | `buffer[8..31]` in RP2350 RAM, or the external SRAM when Remap is on | "ROM" region: sentinel, boot hook, keyword index and table, keyword executor |

- **The ROM region isn't real ROM.** It's the same RAM buffer (`buffer[32][256]` in
  `RP2350/monitor.c`), loaded at boot from the assembled `rom.asm` image.
- **Writes to the ROM region never reach `buffer[]`.** GreenPAK2's LUT only raises the write
  trigger for 0x8000-0x87FF, so the RP2350 never sees a write to 0x8800-0x9FFF. `write_serve.pio`
  also drops any write with address bit A11 set, as a second, partial layer behind that.
- **Remap on (STAGE RAM mode):** GreenPAK1/2's SRAM/ROM Remap is set, so reads of 0x8800-0x9FFF are
  answered by the external SRAM chip, which holds a verified copy of the same image. The RP2350
  isn't involved in those reads at all.

## How keywords run (keyword executor)

Since 2026-09-25 the ROM holds almost no per-keyword code. Every keyword table entry except
`ECVER` and `FNCLR` points at `KW_START`, and all argument parsing and command sequencing runs on the MCU
(`RP2350/keywords.c`, which pc1500emu's ExpansionMock compiles too). `ECVER` and `FNCLR` are
ROM-only routines that never wake the MCU. Keywords work both typed at the prompt and inside a running program,
and their file names and numbers can be BASIC expressions.

1. BASIC enters `KW_START` with Y pointing just past the keyword's own E1xx token: in
   `DISP_BUFFER` (0x7BB2) for a typed command, in the program line for a running one.
   `KW_START` wakes the MCU (`EC_WAKE`), saves Y (`KW_TEXT_*`), and copies the token and the
   next 78 bytes to 0x8000.
2. It sends `KEYWORD` (0x2E). The MCU parses the statement and runs whatever SD/log/STAGE
   commands it needs internally (their statuses never reach 0x87FF). It answers `SUCCESS`, with
   the statement's length at 0x87E7 and an action block at 0x87E0. Any other status is ERROR 1.
3. The ROM carries out the action. Actions that need the MCU again finish with
   `KEYWORD_CONTINUE` (0x2F), which returns the next action block.
4. Every exit (`KEYWORD_RETURN`) sets Y to the end of the statement, sends `DONE`, and ends
   with `VEJ E2`, BASIC's own end-of-statement vector. In a program that carries on with the
   next statement; at the prompt it finishes the command.

The statement is BASIC's stored form, which affects parsing:
- every space outside quotes is removed (`SDOPEN "X" AS 1` is stored as `"X"AS1`);
- BASIC's own keywords are tokenized inside it (`SLEEPWAIT` becomes `SLEEP` + F1B3, `WAIT`), and
  the MCU expands them back before parsing;
- `:` or CR ends it.

Arguments are handled like this:
- **Plain literals** are read by the MCU directly: a quoted string, or a decimal or `&hex`
  number followed by `,`, the end, or `AS`.
- **Anything else** goes to BASIC's evaluator through the EVAL action.
- **`AS` after an expression doesn't work:** BASIC's evaluator raises ERROR 1 at a word it
  doesn't know. So `SDOPEN` also takes `name,n`, e.g. `SDOPEN F$,1`.
- **The `M` mode flag** of `SDLOAD`/`SDSAVE` needs a quote, comma or end after it: `SDLOAD M,F$`
  (`SDLOAD MF$` is the variable `MF$`).

| Code | Action | ROM does |
|---|---|---|
| 0 | DONE | `KEYWORD_RETURN` (sends `DONE`, `VEJ E2`) |
| 1 | SHOW | Show the 26 characters at 0x8000, wait for a key, ANSWER = key (0 for BREAK), continue |
| 2 | ERROR | `DONE`, then BASIC `ERROR ARG` |
| 3 | BROWSE | Browse the listing at 0x8000 (`LIST_SD_DIR` format). ARG 0: view; ARG 1: select, `L` puts the index in ANSWER and continues |
| 4 | LOAD | File already open: read it into RAM at A until 0 bytes, close. ARG bit 0: BASIC program (target = program start, then program end = last byte); bit 1: `CALL` B |
| 5 | SAVE | File already created: write RAM A..B inclusive, close. ARG bit 0: the BASIC program |
| 6 | VAR_LOOKUP | Look up variable A (D461H name code); copy its type byte and raw storage to 0x8000; continue |
| 7 | VAR_STORE | Copy the storage-sized bytes at 0x8000 into that variable; continue |
| 8 | STAGE | Run the STAGE copy routine, ARG = `STAGE_DEBUG_FLAG` |
| 9 | EVAL | Evaluate the BASIC expression A bytes into the statement (`VEJ DE`): the arithmetic register (7A00-7A07) to 0x8000, a string's characters at 0x8008, B = where it ended; continue. A bad expression is raised by BASIC itself |
| 10 | COPY_IN | Copy B bytes of RAM from A to 0x8000; continue |
| 11 | COPY_OUT | Copy B bytes from 0x8000 to RAM at A; continue |
| 12 | RESTORE | STLOAD's last step. Interrupts off, no stack use: copy B bytes from 0x8000 to A; if ARG is set, send `KEYWORD_CONTINUE` (polled inline) and repeat. Then S = `KW_S`, and `KEYWORD_RETURN` with STSAVE's own statement position |

## MCU sleep in STAGE RAM mode

Once the ROM is staged into SRAM, the RP2350 goes DORMANT between keywords.

- **While it sleeps:**
  - Nothing drives the data bus, so the **whole 0x8000-0x87FF window reads 0x00** (the RP2350's
    data-pin pull-downs).
  - Writes to the window are lost.
  - The access that wakes it is itself lost.
- **What wakes it:** any read or write trigger in the window.
- **Every keyword wakes it first.** `KW_START` runs `EC_WAKE`: it writes `CLEAR_STATUS` to
  0x87FF twice and polls for `READY` (0x04). The keyword then ends with `EC_DONE`
  (`EXP_COMMAND_DONE`), which lets the MCU sleep again.
- **A wake that isn't followed by a command** within 100 ms (e.g. a `PEEK` into the window) goes
  back to sleep on its own.
- **Code reached by raw `CALL` bypasses `EC_WAKE`:** `ROM_RESET_REMAP`, `MEMCOPY` and
  `MEMCOPY_PV_SWAP` all run from or use the window. In STAGE RAM mode, any of them started while
  the MCU sleeps will fetch or read 0x00.
- **In MCU mode** (Remap off) the RP2350 never sleeps.

## 0x8000-0x87FF — data window

### Fixed protocol cells

| Address | Symbol | Contents |
|---|---|---|
| 0x8000-0x83FF | `EXP_BUFFER_START_ABS` | Payload window, 1024 bytes (`EXP_MAX_TRANSFER_LEN`), pages 0-3. Command arguments and results, the keyword line, SD file data, STAGE's `GET_BLOCK` blocks. Listings start here too but can run on to 0x87D7 (`EXP_DIR_MAX_ENTRIES` = 66). |
| 0x8100-0x81FF | `EXP_SCRATCH_ABS` | Page 1 of the payload window (not extra space). Short length-prefixed responses such as `GET_SD_CWD` / `GET_SD_FILE_NAME`. |
| 0x87E0-0x87E7 | `EXP_KW_ACTION_ABS` | Keyword action block: +0 action, +1 ARG, +2/+3 A (BE), +4/+5 B (BE), +6 ANSWER (written by the ROM before `KEYWORD_CONTINUE`), +7 the statement's length (from the MCU) |
| 0x87F0-0x87F4 | `EXP_STORE_PARAMS` | Parameters of the MCU's own flash-store commands (MCU-internal): slot, offset (BE), length (BE) |
| 0x87FB-0x87FC | `EXP_BLOCK_CHECKSUM_ABS` | 2-byte BE additive checksum of the block `ROM_COPY_GET_BLOCK` just staged. |
| 0x87FD-0x87FE | `EXP_LENGTH_PORT_ABS` | 2-byte BE transfer length: request in, actual count out (`READ_FROM_SD_FILE`, `WRITE_TO_SD_FILE`, `ROM_COPY_GET_BLOCK`). |
| 0x87FF | `EXP_INSTRUCTION_ABS` | Command/status byte. The LH5801 writes a command code here; the status appears here. A command write is captured by its own PIO/DMA dispatch path, which stamps `BUSY` in hardware before the LH5801's next access, so the raw command byte is never stored here. |

### ROM-side scratch cells

Ordinary RAM cells that `rom.asm` itself uses as working storage, all inside the window.

| Address | Symbol(s) | User |
|---|---|---|
| 0x8000-0x8005 | `MEMCOPY_PARAMS_ABS` | `MEMCOPY` / `MEMCOPY_PV_SWAP` parameter block (source, dest, count; 16-bit BE each) |
| 0x87E8-0x87E9 | `KW_TEXT_HI/LO_ABS` | The statement's argument address (Y at `KW_START`) |
| 0x87EA | `SD_VAR_TYPE_ABS` | VAR_LOOKUP/VAR_STORE: the variable's type/size byte |
| 0x87EB-0x87EC | `SD_VAR_ADDR_HI/LO_ABS` | VAR_LOOKUP/VAR_STORE: the variable's address |
| 0x87ED-0x87EE | `KW_S_HI/LO_ABS` | The stack pointer at `KW_START` (STSAVE saves it, STLOAD's RESTORE returns through it) |
| 0x87F6-0x87F9 | `SD_LIST_INDEX/COUNT/ADDR_HI/ADDR_LO_ABS` | BROWSE cursor (SDLS, SDLOAD picker, SDOPEN, MLOG VIEW) |

LOAD and SAVE use the action block's A cells as their running RAM pointer.

### Resident code in the window

| Range | Contents |
|---|---|
| 0x8400-0x8716 | STAGE's ROM-to-SRAM copy routine (`STAGE_COPY_ROUTINE_ABS`) and its data. It has to run from the window, because once `ROM_COPY_BEGIN` switches Remap, the ROM region is answered by the half-written SRAM. It must never call anything in 0x8800+ until the copy is finished and verified. |
| 0x8717-0x8723 | `ROM_RESET_REMAP` (`CALL 34583`): sends `ROM_FROM_MCU` (Remap and write-enable off) and returns. It uses nothing in 0x8800+, so it's safe in any Remap state. |
| 0x8724-0x87DF | Free |

The longest listings run over 0x8400-0x87D7. The MCU copies the resident code back from the ROM
image at the start of every keyword (`RestoreWindowCode()` in `monitor.c`), so STAGE and
`ROM_RESET_REMAP` always find it intact.

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
| 0x8820-0x8853 | First-letter index, 26 × 2-byte BE pointers (A-Z). Non-zero slots: E → `ECVER`, M → `MLOGMSG`, S → `SDDF`. |
| 0x8854-0x896F | Keyword table (below), terminator `0xD0` at 0x896F |
| 0x8970 | `KEYWORD_RETURN`: shared exit for every keyword the MCU ran. Y to the end of the statement, `EC_DONE`, `VEJ E2`. |
| 0x8985 | `ECVER_ROUTINE` and its message |
| 0x89AC | `FNCLR_ROUTINE`; `SD_LIST_BLANK` (26 spaces) at 0x89BE |
| 0x89D8-0x8E1D | Keyword executor (`KW_START`), action handlers, browse/load/save loops, EC helpers, variable lookup, `MEMCOPY`, STAGE boot entry |
| 0x8E1E-0x9FFF | Free (the image must not pass 0x9FFF; `.org ROM_REGION_END` guards it) |

### Keyword table and keyword addresses

Entry format: `marker | name | code (2 bytes BE) | address (2 bytes BE)`.
- **Marker:** low nibble = name length. Any entry that isn't the first of its letter needs bit 4
  (0x10) clear, because the base ROM's skip-scan rejects a landing marker with that bit set.
- **Code:** the high byte 0xE1 is this page's PV-low code.
- **Address:** every entry but `ECVER` and `FNCLR` points at `KW_START` (0x89D8); the MCU
  tells the keywords apart by the token code the ROM hands it. `ECVER` points at `ECVER_ROUTINE`
  (0x8985), `FNCLR` at `FNCLR_ROUTINE` (0x89AC).

| Keyword | Table entry | Code |
|---|---|---|
| `SDDF` | 0x8854 | 0xE18A |
| `SDFMT` | 0x885D | 0xE189 |
| `SDLOAD` | 0x8867 | 0xE187 |
| `SDLS` | 0x8872 | 0xE185 |
| `SDRMDIR` | 0x887B | 0xE18E |
| `SDRM` | 0x8887 | 0xE188 |
| `SDSAVE` | 0x8890 | 0xE186 |
| `SDCP` | 0x889B | 0xE18B |
| `SDCD` | 0x88A4 | 0xE18C |
| `SDMKDIR` | 0x88AD | 0xE18D |
| `SDPWD` | 0x88B9 | 0xE18F |
| `SDMV` | 0x88C3 | 0xE180 |
| `SDOPEN` | 0x88CC | 0xE190 |
| `SDCLOSE` | 0x88D7 | 0xE191 |
| `SDINPUT` | 0x88E3 | 0xE192 |
| `SDPRINT` | 0x88EF | 0xE193 |
| `SDSKIP` | 0x88FB | 0xE194 |
| `STAGE` | 0x8906 | 0xE197 |
| `STSAVE` | 0x8910 | 0xE19E |
| `STLOAD` | 0x891B | 0xE19F |
| `ECVER` | 0x8926 | 0xE195 |
| `MLOGMSG` | 0x8930 | 0xE199 |
| `MLOG` | 0x893C | 0xE198 |
| `MCONF` | 0x8945 | 0xE19A |
| `FNCLR` | 0x894F | 0xE19B |
| `FNSAVE` | 0x8959 | 0xE19C |
| `FNLOAD` | 0x8964 | 0xE19D |

Table order is load-bearing:
- **The S chain is contiguous**, starting at `SDDF`.
- **`STAGE` is placed inside the S chain**, because the base ROM's skip-scan can't be relied on to
  pass a foreign-letter entry.
- **`MLOGMSG` precedes `MLOG`**, because name matching is a prefix search.
- **`STSAVE`/`STLOAD` follow `STAGE` in the S chain.**
- **`MCONF` follows `MLOG`; the F chain (`FNCLR`, the F index slot's entry, then `FNSAVE`,
  `FNLOAD`) follows `MCONF`, and the terminator directly follows `FNLOAD`.**

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
- `MCONF`: MCU firmware settings, saved in the MCU's flash:
  - no argument: browse every setting;
  - `NAME`: show one (`LED=1`);
  - `NAME=value`: set one. `LED` (0/1, default 1): flash the LED for SD activity. `SLEEPWAIT`
    (0-65535 ms, default 0): in STAGE RAM mode, stay awake that long after a keyword before
    going DORMANT.
    `LOGSIZE` (KB, default 100, a multiple of 4): the MCU log's size; changing it starts a
    fresh log.
- `ECVER`: show the expansion ROM version (ROM only, doesn't wake the MCU).
- `FNCLR`: zero the function-key definitions, the 195 bytes ending just before the BASIC program
  (7865H/7866H) -- for after a crash has corrupted them. ROM only.
- `FNSAVE` / `FNLOAD`: save the function-key definitions to the MCU's flash / put them back
  (relative to the current program start). `FNLOAD` with nothing saved: ERROR 40.
- `STSAVE` / `STLOAD`: save all of 0000H-7FFFH (and where the `STSAVE` statement was) to the
  MCU's flash / put it back. `STLOAD` resumes right after the `STSAVE` that made the state -- at
  the prompt that just ends the command; saved inside a program, the program carries on from
  there. Nothing saved: ERROR 40. One saved set of each for now.

### `CALL` entry points

| Address | `CALL` | Routine |
|---|---|---|
| 0x8717 | `CALL 34583` | `ROM_RESET_REMAP`: force MCU-served ROM (Remap and write-enable off) |
| 0x8D7D | `CALL 36221` | `MEMCOPY`: copy `count` bytes, parameters at 0x8000-0x8005 |
| 0x8DA0 | `CALL 36256` | `MEMCOPY_PV_SWAP`: same, with PV low for each read and high for each write |

### Other internal entry points

| Address | Label | Purpose |
|---|---|---|
| 0x89D8 | `KW_START` | Every MCU-run keyword: wake the MCU, save S, hand it the statement at Y, run the actions it returns |
| 0x8A48 | `KW_CONTINUE` | Send `KEYWORD_CONTINUE` and run the next action |
| 0x8A58 | `SD_RAISE_ERROR_1` | Send `DONE`, then raise ERROR 1 |
| 0x8B3D | `KW_EVAL` | The EVAL action: `VEJ DE` at the statement offset in A |
| 0x8BA4 | `KW_RESTORE` | The RESTORE action (STLOAD's stack page) |
| 0x8CF0 | `EC_SEND` | Write the command in A, then fall into `EC_WAIT_NOT_BUSY` |
| 0x8CF3 | `EC_WAIT_NOT_BUSY` | Poll the status byte until it isn't BUSY (HLT between polls) |
| 0x8D06 | `EC_WAKE` | Wake the MCU and wait for READY (tight poll, ~3 s timeout per wait, Carry set on timeout) |
| 0x8D42 | `EC_DONE` | Send `DONE` and wait for it to leave BUSY |
| 0x8DC8 | `STAGE_BOOT_ENTRY` | Boot hook body: wake, skip if already staged, otherwise run the copy in boot mode, then `DONE` |
| 0x8DEB | `STAGE_SHOW_OK` | Blank the line, show `STAGE: OK`, wait for a key, return (the copy routine's success exit) |
| 0x8E03 | `STAGE_IS_STAGED` | `ROM_GET_MODE` query; Carry set if Remap is on and the MCU vouches for the copy |

## Status and command reference

**`EXP_STATUS_*`** (read at 0x87FF): `BUSY=1`, `SUCCESS=2`, `EOF=3` (`SD_READ_VALUE` only),
`READY=4`, `NOT_IMPLEMENTED=64`, `ERROR=128`. A sleeping MCU reads as 0x00 (0xFF before the
data-pin pull-downs), which is why no status uses 0x00 or 0xFF.

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
| 46 | 0x2E | KEYWORD (start a keyword: token + statement at 0x8000; action block back at 0x87E0) |
| 47 | 0x2F | KEYWORD_CONTINUE (after an action that needs the MCU again; ANSWER at 0x87E6) |
| 48 | 0x30 | CONFIG_GET (MCONF; byte 0 = setting number, bytes 1-2 = BE value back) |
| 49 | 0x31 | CONFIG_SET (MCONF; byte 0 = setting number, bytes 1-2 = BE value, saved to flash) |
| 50 | 0x32 | STORE_ERASE (MCU-internal: FNSAVE/STSAVE flash store; parameters at 0x87F0) |
| 51 | 0x33 | STORE_WRITE (MCU-internal) |
| 52 | 0x34 | STORE_READ (MCU-internal) |
| 129 | 0x81 | TEST_COPY_STRING |
| 255 | 0xFF | CLEAR_STATUS (sets READY; also the write `EC_WAKE` uses to wake the MCU) |
