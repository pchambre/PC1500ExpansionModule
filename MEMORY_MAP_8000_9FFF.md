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
`ECVER`, `FNCLR` and the two functions (below) points at `KW_START`, and all argument parsing and command sequencing runs on the MCU
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
| 3 | BROWSE | Browse the listing at 0x8000 (`LIST_SD_DIR` format). ARG 0: view; otherwise ARG is the pick key (`L` for SDLOAD, `C` for BLSCAN), which puts the entry's index in ANSWER and continues |
| 4 | LOAD | File already open: read it into RAM at A until 0 bytes, close. ARG bit 0: BASIC program (target = program start, then program end = last byte); bit 1: `CALL` B |
| 5 | SAVE | File already created: write RAM A..B inclusive, close. ARG bit 0: the BASIC program |
| 6 | VAR_LOOKUP | Look up variable A (D461H name code); copy its type byte and raw storage to 0x8000; continue |
| 7 | VAR_STORE | Copy the storage-sized bytes at 0x8000 into that variable; continue |
| 8 | STAGE | Run the STAGE copy routine, ARG = `STAGE_DEBUG_FLAG` |
| 9 | EVAL | Evaluate the BASIC expression A bytes into the statement (`VEJ DE`): the arithmetic register (7A00-7A07) to 0x8000, a string's characters at 0x8008, B = where it ended; continue. A bad expression is raised by BASIC itself |
| 10 | COPY_IN | Copy B bytes of RAM from A to 0x8000; continue |
| 11 | COPY_OUT | Copy B bytes from 0x8000 to RAM at A; continue |
| 12 | RESTORE | STLOAD's last step. Interrupts off, no stack use: copy B bytes from 0x8000 to A; if ARG is set, send `KEYWORD_CONTINUE` (polled inline) and repeat. Then S = `KW_S`, and `KEYWORD_RETURN` with STSAVE's own statement position |
| 13 | POLL | Sleep one timer wake (`am0`/`sie`/`hlt`), then test BREAK with the base ROM's own `VMJ A6` (IF register F00BH bit 1); ANSWER = 1 for BREAK, else 0; continue. ARG bit 0: clear an old BREAK first; bit 1: first show the 26 characters at 0x8000. How BLADV/BLPUT/BLGET/BLRECV/BLSEND wait, and show `SENDING...` etc. |

### Functions (`BLSTAT`, `SDEOF(n)`)

Two keywords are BASIC **functions**, used in expressions like `MEM` (2026-09-29/30): `S=BLSTAT`,
`IF SDEOF(1) THEN ...`, or typed alone at the prompt.
- **What makes them functions** is their code's low byte, which BASIC's evaluator checks:
  `5xH` = no argument (`BLSTAT`, E152), `6xH`-`7xH` = one argument (`SDEOF`, E170).
  Everything else is a statement. See `PC1500_BASIC_Keyword_Extension_Mechanism.md` §13.
- **How they run:** their table entries point at `BLSTAT_FN`/`SDEOF_FN`, which put the command in A
  and fall into `FN_CALL`. `FN_CALL` wakes the MCU and copies the arithmetic register
  (7A00H-7A07H) to 0x8000 (for `SDEOF`, the argument BASIC has already evaluated). It then sends
  the function's command (`FN_BLSTAT` 0x55 / `FN_SDEOF` 0x56), copies the 8-byte result from
  0x8000 back to 7A00H, and returns with UH = 0.
  - On an error, UH = the error number from byte 9 of the reply.
- **No `VEJ E2`, and no `KW_START`:** a function can be evaluated in the middle of another
  keyword. It sends `DONE` only if the reply's byte 8 says no keyword is running, and only after
  reading the reply.

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
| 0x87EF | `KBD_KEY_ABS` (`EXP_KBD_KEY`) | The external keyboard's key: the matrix index (80H-BFH) of the key it's holding down, 0 = none. Written by the MCU (`kbd_seq.c`), read by the driver's `KBD_ANY`/`KBD_SCAN`. |
| 0x87F0-0x87F4 | `EXP_STORE_PARAMS` | Parameters of the MCU's own flash-store commands (MCU-internal): slot, offset (BE), length (BE) |
| 0x87F5 | `KBD_BREAK_ABS` (`EXP_KBD_BREAK`) | The external keyboard's ON presses, counted by the MCU |
| 0x87FA | `KBD_ACK_ABS` (`EXP_KBD_ACK`) | The ON count the driver has acted on (written by the ROM's `KBD_BREAK`): a BREAK is due while it differs from 0x87F5 |
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
| 0x880A-0x881F | `BOOT_SELFCHECK_ENTRY`: the base ROM calls page+0x0A during its boot-time module scan (`STX P`, return address pushed). It holds `jmp STAGE_BOOT_ENTRY`; then, at 0x880E, `KBD_HOOK` (`jmp KBD_ENTRY`, the keyboard driver's fixed address for the base ROM's hook vector at 785BH/785CH -- even, so PV low), and at 0x8811 the four BE addresses the MCU patches into the driver's wait loop (`KBD_LOOP`, `KBD_ANY`, `KBD_SCAN`, `KBD_DISPATCH`); then padding. The header must stay exactly 32 bytes. |
| 0x8820-0x8853 | First-letter index, 26 × 2-byte BE pointers (A-Z). Non-zero slots: B → `BLSCAN`, C → `COLOR`, E → `ECVER`, F → `FNCLR`, G → `GRAPH`, L → `LCURSOR`, M → `MLOGMSG`, R → `RLINE`, S → `SDDF`, T → `TAB`. |
| 0x8854-0x8AC6 | Keyword table (below), terminator `0xD0` at 0x8AC6 |
| 0x8AC7 | `KEYWORD_RETURN`: shared exit for every keyword the MCU ran. Y to the end of the statement, `EC_DONE`, `VEJ E2`. |
| 0x8ADC | `ECVER_ROUTINE` and its message |
| 0x8B03 | `FNCLR_ROUTINE`; `SD_LIST_BLANK` (26 spaces) at 0x8B15 |
| 0x8B2F | `CE150_E6_ENTRY` (2026-10-01): the CE-150 stand-in's seven E1C0-E1C6 keywords. With a CE-150 attached (C0H at A000H, 55H at B000H) it runs the CE-150's own routine for E680 + the low nibble, found in its table at B054H; otherwise it falls into `KW_START` |
| 0x8B66-0x9092 | Keyword executor (`KW_START`), action handlers, the functions' `FN_CALL`, browse/load/save loops, EC helpers, variable lookup, `MEMCOPY`, STAGE boot entry |
| 0x9093-0x923D | The external keyboard's driver (MCONF `BLKBD`, below) |
| 0x923E-0x9FFF | Free (the image must not pass 0x9FFF; `.org ROM_REGION_END` guards it) |

### Keyword table and keyword addresses

Entry format: `marker | name | code (2 bytes BE) | address (2 bytes BE)`.
- **Marker:** low nibble = name length. Any entry that isn't the first of its letter needs bit 4
  (0x10) clear, because the base ROM's skip-scan rejects a landing marker with that bit set.
  Conversely, each letter's first entry has bit 4 SET (0xD_), as the CE-150's table does: that
  ends a skip-scan coming from the letter before. Without it the scan runs on into the next
  letter's entries, comparing only from their second character (2026-09-30: after the new L chain
  was added, `A=&FF` became ERROR 1 -- the second F matched `LF`).
- **Code:** the high byte 0xE1 is this page's PV-low code. The exceptions are eight of the CE-150
  stand-in's keywords (2026-09-30), which keep the CE-150's own F0xx codes: BASIC looks for F0xx
  codes on every page, but for E6xx (the CE-150's other seven) only on the CE-150's own page, so
  those seven are E1C0-E1C6 here.
- **Address:** every statement but `ECVER`, `FNCLR` and the E1C0-E1C6 seven points at `KW_START`
  (0x8B66); the MCU tells the keywords apart by the token code the ROM hands it. `ECVER` points at
  `ECVER_ROUTINE` (0x8ADC), `FNCLR` at `FNCLR_ROUTINE` (0x8B03), and `CSIZE`, `GRAPH`, `GLCURSOR`,
  `LCURSOR`, `SORGN`, `ROTATE` and `TEXT` at `CE150_E6_ENTRY` (0x8B2F), which hands them to a real
  CE-150 when one is attached. The functions point at `BLSTAT_FN` (0x8DC2) and `SDEOF_FN` (0x8DC6).

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
| `SDEOF` (function) | 0x8926 | 0xE170 |
| `SORGN` | 0x8930 | 0xE1C4 |
| `ECVER` | 0x893A | 0xE195 |
| `MLOGMSG` | 0x8944 | 0xE199 |
| `MLOG` | 0x8950 | 0xE198 |
| `MCONF` | 0x8959 | 0xE19A |
| `FNCLR` | 0x8963 | 0xE19B |
| `FNSAVE` | 0x896D | 0xE19C |
| `FNLOAD` | 0x8978 | 0xE19D |
| `BLSCAN` | 0x8983 | 0xE1A0 |
| `BLCON` | 0x898E | 0xE1A1 |
| `BLDISC` | 0x8998 | 0xE1A2 |
| `BLPRINT` | 0x89A3 | 0xE1A3 |
| `BLLIST` | 0x89AF | 0xE1A4 |
| `BLSAVE` | 0x89BA | 0xE1A5 |
| `BLLOAD` | 0x89C5 | 0xE1A6 |
| `BLCLS` | 0x89D0 | 0xE1A7 |
| `BLADV` | 0x89DA | 0xE1A8 |
| `BLPUT` | 0x89E4 | 0xE1A9 |
| `BLGET` | 0x89EE | 0xE1AA |
| `BLSEND` | 0x89F8 | 0xE1AB |
| `BLRECV` | 0x8A03 | 0xE1AC |
| `BLPAIR` | 0x8A0E | 0xE1AD |
| `BLUNPAIR` | 0x8A19 | 0xE1AE |
| `BLKBD` | 0x8A26 | 0xE1AF |
| `BLSTAT` (function) | 0x8A30 | 0xE152 |
| `COLOR` | 0x8A3B | 0xF0B5 |
| `CSIZE` | 0x8A45 | 0xE1C0 |
| `GRAPH` | 0x8A4F | 0xE1C1 |
| `GLCURSOR` | 0x8A59 | 0xE1C2 |
| `LCURSOR` | 0x8A66 | 0xE1C3 |
| `LF` | 0x8A72 | 0xF0B6 |
| `LINE` | 0x8A79 | 0xF0B7 |
| `LLIST` | 0x8A82 | 0xF0B8 |
| `LPRINT` | 0x8A8C | 0xF0B9 |
| `RLINE` | 0x8A97 | 0xF0BA |
| `ROTATE` | 0x8AA1 | 0xE1C5 |
| `TAB` | 0x8AAC | 0xF0BB |
| `TEST` | 0x8AB4 | 0xF0BC |
| `TEXT` | 0x8ABD | 0xE1C6 |

Table order is load-bearing:
- **The S chain is contiguous**, starting at `SDDF`.
- **`STAGE` is placed inside the S chain**, because the base ROM's skip-scan can't be relied on to
  pass a foreign-letter entry.
- **`MLOGMSG` precedes `MLOG`**, because name matching is a prefix search.
- **`STSAVE`/`STLOAD`, then `SDEOF` and `SORGN`, follow `STAGE` in the S chain.**
- **`MCONF` follows `MLOG`; the F chain (`FNCLR`, the F index slot's entry, then `FNSAVE`,
  `FNLOAD`) follows `MCONF`.**
- **The B chain (`BLSCAN`, the B index slot's entry, through `BLSTAT`) follows `FNLOAD`.** No B
  name is a prefix of a later one (`BLPUT` differs from `BLPRINT` at the 4th letter,
  `BLSEND`/`BLSTAT` from `BLSCAN`/`BLSAVE` by the 4th). `BLCON` was `BLCONNECT` until
  2026-09-28; the token is the same.
- **The CE-150 stand-in's C, G, L, R and T chains (2026-09-30) follow `BLSTAT`, each starting at
  its index slot's entry, and the terminator directly follows `TEXT`.** The names are the
  CE-150's own; none is a prefix of another. A CE-150's table (B000, PV low) is searched before
  this page, so with one attached its keywords win, both when a line is typed and when it runs.

Keyword arguments:
- `STAGE`:
  - no argument: show the current mode (`STAGE: MCU` / `RAM` / `MODE UNKNOWN`);
  - `RAM`: stage the ROM into SRAM; with a copy already there it copies again (refreshing it
    after a firmware update, 2026-09-29). The boot hook still skips a verified copy;
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
    fresh log. `AUTOSTAGE` (0/1, default 0): STAGE RAM at power-on/reset (the boot hook).
    `BLKBD` (0/1, default 0): set up the external keyboard's driver at power-on/reset (the boot
    hook; see "The external keyboard's driver").
  - `HOSTNAME` / `HOSTNAME="name"`: this PC-1500's name on the BLE link (1-15 characters,
    default `PC-1500`), listed last.
- BLE (`RP2350/BLE_PROTOCOL.md`); every failure is ERROR 40 (MLOG VIEW says why):
  - `BLSCAN [seconds]`: scan (default 3 s) and browse the Link peers found; `C` connects.
  - `BLCON name`: connect to the peer advertising that name. (Not the emulator on Windows,
    which can't advertise its own name: use BLSCAN.) `BLDISC`: disconnect.
  - `BLPRINT` (like PRINT), `BLLIST [from[,to]]`, `BLCLS`: text to the peer's console.
  - `BLSAVE` / `BLLOAD`: like `SDSAVE` / `SDLOAD`, with the feature server's file store in
    place of the card.
  - `BLADV`: advertise, and wait for another PC-1500 to connect (BREAK stops).
  - `BLPUT` (the program), `BLPUT M start,end[,call]`, `BLPUT SD [M] name`: offer a file to the
    peer PC-1500, and send it once its `BLGET` accepts. `BLGET` (into memory) or
    `BLGET name[,-Y]` (onto the card): wait for an offer and take it. Either may start first;
    BREAK stops a wait.
  - `BLSEND value[,value...]`: numbers and strings to the peer PC-1500 as one message (waits
    while its 8-message inbox is full; BREAK stops). `BLRECV [#t,]var[,var...]`: the oldest
    message into the variables (extra ones 0 / blank; a type mismatch is ERROR 42); waits until
    one comes (BREAK stops), or up to t seconds (`#0`: not at all), leaving the variables as they
    were if none does.
  - `BLSTAT` (a function): messages waiting (0-8), or -1 with no link and none waiting.
  - `BLKBD`: pair an external Bluetooth keyboard (classic for now), replacing any paired before.
    Needs `MCONF BLKBD=1` (and a reset since, for the driver). Put the keyboard in pairing mode,
    then type the code shown on it and press Enter; BREAK stops. After that it connects by
    itself when a key is pressed. `BLKBD FORGET`: forget it. On an older PC-1500 ROM
    (`PEEK &E2B9` is 213, not 56) the base ROM's keyboard hook doesn't work, so there is no
    driver: `BLKBD` shows `BLKBD: NOT ON THIS ROM`, and the boot hook leaves the hook unarmed.
- `SDEOF(n)` (a function): 1 once SD channel n has nothing left for `SDINPUT#`, else 0 (not
  open: ERROR 40).
- `SDSAVE` with no name: save as the last BASIC program `SDLOAD` loaded (full path, asks before
  overwriting); ERROR 1 if none since the MCU powered up.
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
| 0x8FCF | `CALL 36815` | `MEMCOPY`: copy `count` bytes, parameters at 0x8000-0x8005 |
| 0x8FF2 | `CALL 36850` | `MEMCOPY_PV_SWAP`: same, with PV low for each read and high for each write |

### Other internal entry points

| Address | Label | Purpose |
|---|---|---|
| 0x8B66 | `KW_START` | Every MCU-run keyword: wake the MCU, save S, hand it the statement at Y, run the actions it returns |
| 0x8BD8 | `KW_CONTINUE` | Send `KEYWORD_CONTINUE` and run the next action |
| 0x8BEC | `SD_RAISE_ERROR_1` | Send `DONE`, then raise ERROR 1 |
| 0x8CCD | `KW_EVAL` | The EVAL action: `VEJ DE` at the statement offset in A |
| 0x8D4B | `KW_RESTORE` | The RESTORE action (STLOAD's stack page) |
| 0x8D8D | `KW_POLL` | The POLL action: one timer wake, then BREAK? (`VMJ A6`) into ANSWER |
| 0x8DC2 / 0x8DC6 | `BLSTAT_FN` / `SDEOF_FN` | The functions' table addresses: the command into A, then `FN_CALL` |
| 0x8DC8 | `FN_CALL` | A function's MCU round trip (above, "Functions") |
| 0x8F42 | `EC_SEND` | Write the command in A, then fall into `EC_WAIT_NOT_BUSY` |
| 0x8F45 | `EC_WAIT_NOT_BUSY` | Poll the status byte until it isn't BUSY (HLT between polls) |
| 0x8F58 | `EC_WAKE` | Wake the MCU and wait for READY (tight poll, ~3 s timeout per wait, Carry set on timeout) |
| 0x8F94 | `EC_DONE` | Send `DONE` and wait for it to leave BUSY |
| 0x901A | `STAGE_BOOT_ENTRY` | Boot hook body: wake; with `BLKBD`, `KBD_BOOT_COPY` first; skip staging if already staged or `AUTOSTAGE` is 0, otherwise run the copy in boot mode; with `BLKBD`, `KBD_ARM`; then `DONE` |
| 0x9058 | `STAGE_SHOW_OK` | Blank the line, show `STAGE: OK`, wait for a key, return (the copy routine's success exit) |
| 0x9070 | `STAGE_IS_STAGED` | `ROM_GET_MODE` query (clears the AUTOSTAGE and BLKBD bytes first); Carry set if Remap is on and the MCU vouches for the copy |
| 0x9093 | `KBD_ENTRY` | The keyboard hook's target (via `KBD_HOOK`): `KBD_LOOP` if the loop is there, else ROM1's own E24AH |
| 0x90A1-0x91BC | `KBD_LOOP` | 284 bytes left empty in the image: ROM1's wait loop goes here (see below) |
| 0x91BD | `KBD_DISPATCH` | Where the loop hands a key to SML_DISPATCH (E366H): OFF goes to the loop's own power-off instead |
| 0x91C8 / 0x91DC / 0x91EE | `KBD_ANY` / `KBD_SCAN` / `KBD_BREAK` | The loop's keyboard reads with the external key added, and the external ON as BREAK |
| 0x91FA / 0x9226 | `KBD_BOOT_COPY` / `KBD_ARM` | Boot: copy ROM1's loop to the MCU (`KBD_INSTALL`); set the hook (785BH/785CH, 79D4H = 55H) if the loop is served |

## The external keyboard's driver

For a BLE keyboard (MCONF `BLKBD=1`, default 0). The MCU turns its keys into taps of the
PC-1500's own key matrix (`RP2350/kbd_seq.c`, timed like pc1500emu's host keyboard) and
publishes the key being "held" at 0x87EF. The ROM reads it through the base ROM's keyboard hook:
with 79D4H = 55H, `KEYSCAN_WAIT` (E243H) jumps through the vector at 785BH/785CH (bit 0 of
the low byte picks PV) instead of scanning -- the hook BASWORD uses.

The driver is ROM1's own wait loop, E24AH-E365H, so the external key goes through ROM1's code
table, debounce, auto-repeat and SHIFT/DEF/SML handling like a physical key. The module doesn't
carry Sharp's code. At power-on/reset the boot hook copies the loop from the machine's own ROM to
0x8000 and sends `KBD_INSTALL`. The MCU checks it is ROM1's (CRC-32 8B43EF78H) and patches it
into the ROM image at `KBD_LOOP`, in place:

- its two keyboard reads call `KBD_ANY` and `KBD_SCAN`;
- its two branches to SML_DISPATCH go to `KBD_DISPATCH`;
- its power-off resumes its own loop.

Older ROMs: a real PC-1500's ROM (2026-10-05) has `VEJ F4H,79H,D5H` at E2B7H where ROM1 has
`VEJ CCH,5BH` + NOP. VEJ F4H loads the word at 79D5H into U, not X, and the `STX P` after it
jumps to whatever X held, so that ROM's hook can't be used (BASWORD's `PEEK &E2B9` = 56 test
rejects it too). The MCU refuses its loop (E2B9H isn't 38H; MLOG "KBD: ROM hook unusable"; an
unknown loop logs its CRC), `KBD_ARM` then leaves the hook unarmed, and `BLKBD` shows
`NOT ON THIS ROM`. Known system ROMs (C000H-FFFFH, CRC-32): DCA8F879H and D480B50DH have the
working hook; A4713655H is the older one.

The boot hook sends `KBD_INSTALL` before any staging, so an AUTOSTAGE copy includes the loop, and
sets the hook last (`KBD_ARM`), once the ROM is served as it will stay. Reset clears 79D4H before
the module scan, so the hook only ever comes from here.

- OFF: given to BASIC, the OFF key rewrites 785BH/785CH while 79D4H stays 55H, so the next
  `KEYSCAN_WAIT` after ON would jump into nowhere. `KBD_DISPATCH` sends it to the loop's own
  power-off instead, as BASWORD does.
- ON (BREAK): the MCU counts presses at 0x87F5, and the driver acknowledges each at 0x87FA and
  returns BREAK from `KEYSCAN_WAIT`. A running program can't be stopped this way: it tests the
  ON key's hardware latch (F00BH bit 1).
- `INKEY$` reads the matrix directly (E42CH) and doesn't see the external key.
- A staged copy made without the loop doesn't get the hook until it's staged again (AUTOSTAGE,
  or STAGE RAM then a reset). If the MCU loses the loop under a running PC-1500, `KBD_ENTRY` falls
  back to ROM1's own loop.

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
| 31 | 0x1F | SD_CHANNEL_EOF (SDEOF: in: [channel]; out: [1 = nothing left for SDINPUT#]) |
| 32 | 0x20 | ROM_FROM_MCU |
| 33 | 0x21 | ROM_FROM_SRAM |
| 34 | 0x22 | ROM_COPY_BEGIN |
| 35 | 0x23 | ROM_COPY_GET_BLOCK |
| 36 | 0x24 | ROM_COPY_FINISH |
| 37 | 0x25 | ROM_GET_MODE (byte 0: Remap on; byte 1: Remap on and verified copy; byte 2: MCONF AUTOSTAGE; byte 3: MCONF BLKBD) |
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
| 53 | 0x35 | CONFIG_HOSTNAME_GET (MCONF HOSTNAME; out: [len][name]) |
| 54 | 0x36 | CONFIG_HOSTNAME_SET (in: [len][name], saved to flash) |
| 55 | 0x37 | KBD_INSTALL (in: ROM1's 284-byte wait loop at 0x8000; the MCU checks its CRC and patches it into the ROM image at `KBD_LOOP`; ERROR for another ROM's) |
| 56 | 0x38 | KBD_PAIR (BLKBD: forget any paired keyboard, look for one in pairing mode; ERROR if MCONF BLKBD is 0) |
| 57 | 0x39 | KBD_STATUS (out: [state][code len][code 6][name len][name 16]; states: none, searching, connecting, code, connected, not found, failed, paired) |
| 58 | 0x3A | KBD_STOP (end PAIR's search) |
| 59 | 0x3B | KBD_FORGET (drop the keyboard's bond) |
| 64 | 0x40 | BLE_SCAN (in: [seconds]; out: the Link peers as a `LIST_SD_DIR` listing) |
| 65 | 0x41 | BLE_CONNECT (in: [index into the last scan]; HELLOs; out: [len][peer name]) |
| 66 | 0x42 | BLE_CONNECT_NAME (in: name slot; out: as BLE_CONNECT) |
| 67 | 0x43 | BLE_DISCONNECT |
| 68 | 0x44 | BLE_TEXT (in: [len hi][len lo][text]) |
| 69 | 0x45 | BLE_FILE_PUT (BLSAVE: name slot, [kind][flags][size BE] at 0x802A; then WRITE/CLOSE go to the peer) |
| 70 | 0x46 | BLE_FILE_GET (BLLOAD: name slot; then READ/CLOSE come from the peer; out: [kind]) |
| 71 | 0x47 | BLE_ADVERTISE (in: [1 start / 0 stop]) |
| 72 | 0x48 | BLE_STATUS (out: [flags: linked, advertising, offer in, answered, accepted][len][peer name]) |
| 73 | 0x49 | BLE_OFFER (BLPUT: as BLE_FILE_PUT; SUCCESS once the peer holds the offer) |
| 74 | 0x4A | BLE_WITHDRAW (our offer) |
| 75 | 0x4B | BLE_OFFER_GET (the peer's offer: name slot, [kind][0][size BE] at 0x802A) |
| 76 | 0x4C | BLE_ANSWER (BLGET: in: [1 accept / 0 refuse][1 routed / 0 not]) |
| 77 | 0x4D | BLE_SEND (BLPUT, once accepted: in: [1 routed / 0 not]) |
| 78 | 0x4E | BLE_DATA_WRITE (an unrouted transfer's WRITE: BLPUT SD) |
| 79 | 0x4F | BLE_DATA_READ (an unrouted transfer's READ: BLGET name) |
| 80 | 0x50 | BLE_DATA_CLOSE (in: [1 = abandon]) |
| 81 | 0x51 | BLE_MSG_SEND (BLSEND: in: [len hi][len lo][value chunks]; ERROR [6] = the peer's inbox is full) |
| 82 | 0x52 | BLE_MSG_WAIT (in: [seconds hi][lo], 0xFFFF = no limit, for the next MSG_RECVs) |
| 83 | 0x53 | BLE_MSG_RECV (out: [len hi][len lo][chunks], the oldest message; ERROR [1 = time's up / 0 = keep waiting / 2 = no link]) |
| 84 | 0x54 | BLE_MSG_COUNT (out: [waiting][1 = linked]) |
| 85 | 0x55 | FN_BLSTAT (sent by the ROM's `FN_CALL`: the value at 0x8000, [end of keyword] at 0x8008, [error] at 0x8009) |
| 86 | 0x56 | FN_SDEOF (as FN_BLSTAT; in: the argument, the arithmetic register's 8 bytes at 0x8000) |
| 129 | 0x81 | TEST_COPY_STRING |
| 255 | 0xFF | CLEAR_STATUS (sets READY; also the write `EC_WAKE` uses to wake the MCU) |

The BLE commands and `SD_CHANNEL_EOF` are MCU-internal (keywords.c runs them), like STORE_*. A
"routed" transfer takes over `WRITE_TO_SD_FILE`/`READ_FROM_SD_FILE`/`CLOSE_SD_FILE`, so the
ROM's LOAD/SAVE actions move the bytes to or from the peer. The `FN_*` commands come from the
ROM itself, from inside BASIC's expression evaluator ("Functions", above).
