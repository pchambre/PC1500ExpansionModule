# Z-machine interpreter for the PC-1500 (ZIP/1500)

A version-3 Z-machine for the PC-1500 with this project's expansion
module. It plays Infocom's v3 stories, such as Zork I from
historicalsource/zork1, from the SD card. The design follows Infocom's own
ZIP: the CoCo 6809 ZIP from historicalsource/infocom-zcode-terps (MIT) is
the model, rewritten in LH5801 assembly rather than translated line by
line.

What makes this port different from Infocom's:
- **RAM:** 28K, less than any Infocom host had.
- **Display:** one 26-character line.
- **Storage:** an MCU that holds the story file, the text the game prints,
  and the console the player reads it on.

## The split

- **LH5801 (`zip.asm`):** the Z-machine itself. It decodes and runs
  opcodes, handles objects, properties, the dictionary, Z-strings, the
  stack and paging. Everything a story executes runs here.
- **MCU (`RP2350/zterm.c`):** the console. It keeps the transcript,
  word-wraps it, scrolls it, edits the input line, runs the settings menu
  and draws the status lines. It sends each line for the LCD back to the
  LH5801. It is plain C with no hardware access, so pc1500emu's
  ExpansionMock compiles it, as it does keywords.c, and the emulator tests
  exercise the real console.
- **The wire:** the SD channel commands for the story and save files
  (`SD_READ_BYTES`/`SD_WRITE_BYTES`), plus one `EXP_COMMAND_ZTERM`
  command whose first byte is a sub-operation. Each access is one request
  and one answer; nothing is queued or staged across accesses.

## Memory (28K: 0000H-6FFFH)

| Range | Use |
|---|---|
| below 0100H | BASIC's reserve area (F-key table etc., TRM §5-3-6), kept |
| `ZBASE` = 0100H | Z-address 0 |
| ZBASE .. ZBASE + dynamic | dynamic memory, resident (Zork I: 2C12H bytes = 45 pages) |
| next pages .. 54FFH | 256-byte swap pages for everything above dynamic memory, LRU (Zork I: 39) |
| 5500H .. 67BFH | the interpreter (`ZIPORG`; 4.8K) |
| 6800H .. 68FFH | its variables, and the output buffer |
| 6900H .. 69FFH | the swap slots' ages and Z pages (64 slots at most) |
| 6A00H .. 6DFFH | the Z-stack (1K) |
| 6E00H .. 6FFFH | the page table: per Z page, its RAM page or 0 |

- A Z-address below the end of dynamic memory is ZBASE + the address. For
  the page-number byte that's one add.
- Static and high memory are read through the page table. A missing page
  takes one `SD_READ_BYTES` of 256 bytes, written straight into its swap
  page.
- The story's own ENDLOD preload hint is ignored; only dynamic memory has
  to stay resident.
- The BASIC program area is overwritten. QUIT returns to BASIC (the
  CALL's RTN, with S as it came), so the reserve area and the fixed
  variables survive and the program doesn't.
- The machine stack stays where BASIC had it. Z-calls use the Z-stack,
  not S.

## Starting a game

```
SDOPEN "ZORK1.Z3" AS 1  ; the story, on channel 1
SDLOAD M "ZIP"          ; the interpreter: its M header loads it at 5500H and calls it
```

`build.ps1` makes `ZIP` from `zip.asm`. A keyword (`ZPLAY "ZORK1.Z3"`)
can come later. Two statements are needed, because a module keyword can't
share a prompt line with a second statement (see
expansion_keyword_test.cpp).

The interpreter then:
1. Wakes the MCU.
2. Reads the header and dynamic memory with READ_BYTES.
3. Sends `ZTERM OPEN`.
4. Starts at the header's initial PC.

## Console protocol (`EXP_COMMAND_ZTERM`, window[0] = sub-op)

| Sub-op | In | Out |
|---|---|---|
| OPEN | [story channel][flags: bit 0 = time game] | [seed for RANDOM, 2 BE] |
| TEXT | [len][bytes] — output, `0DH` = new line | |
| STATUS | [score/hours 2][moves/minutes 2][flags][len][location] | |
| KEY | [key code, 00H = an input line begins][the longest line, 0 = just read] | [state][26 chars to show]; state 1 (done) adds [len at +27][the line from +28] |
| CLOSE | | |

`pc_exp.h`'s `EXP_COMMAND_ZTERM` (7DH) is the reference.

Every key goes to the MCU, so the LH5801 needs no line editor, no
scrollback and no line cache. That keeps the interpreter small, which
leaves more PC-1500 RAM for swap pages.

### Sleep: POWMAN on from the start

POWMAN behaves as it does outside a game: MCONF POWMANDELAY decides,
after a DONE and that much idle time. A POWMAN wake reboots the MCU,
which is unnoticeable even between key presses. Everything the console
needs therefore lives where a reboot doesn't reach:

- **Transcript:** in a ring behind a small store interface in zterm.c.
  - **Dongle:** MCU flash, a new `mcu_store` slot like the command
    history's ring (`history_session.c`), about 256K.
    - TEXT fills a RAM page and programs each full page.
    - The POWMAN prepare step on core1 writes the partial page first.
    - Reads go through XIP in place, so nothing is reloaded after a wake.
  - **Internal card:** its 64K of SRAM banks, which the PC-1500 keeps
    powered (the board owner's choice). Waits for the card hardware.
    - The LH5801 decides bank switching (DESIGN_DECISIONS.md, "SRAM bank
      switching"). The interpreter selects a bank with a module command
      and moves the text itself; the MCU doesn't reach the banks on its
      own.
    - While the banks are in use, nothing the interpreter needs across a
      switch may sit in 0000H-3FFFH: its code, its stack, or dynamic
      memory. The card's memory map therefore starts ZBASE above 4000H,
      and that range becomes a bank window. The window could also serve
      story pages, not only the transcript.
  - **Emulator:** a RAM array in the mock.
- **Session record:** in the ring's header sector
  (`RP2350/zterm_session.c`), a 256-byte page per record, newest wins.
  - The POWMAN prepare step writes one: zterm's state (view, input line,
    status), the ring's extent, and the story's absolute path, noted at
    OPEN. Nothing is written if nothing changed since the last one.
  - A game that has closed writes one more, saying so.
- **After a POWMAN wake:** every SDOPEN channel is reopened before the
  first command is answered, from a record monitor.c writes to flash
  before the sleep (`SaveChannels()`). If the story's channel wasn't, a
  valid session record still reopens it. The
  current directory already survives, through the scratch registers.
  `SD_READ_BYTES` always carries an explicit position, so no file position
  needs restoring.
- **The LH5801 side doesn't change:**
  - It sends DONE before every wait for a key.
  - It wakes the MCU (EC_WAKE's sequence) before every request after a
    DONE.
  - Its own state (dynamic memory, the Z-stack, the PC) is in PC-1500
    RAM, so it never sees a reboot.
- **SAVE/RESTORE and the turn itself:** these run as one stretch of
  commands with no DONE, so the MCU never powers down in the middle of
  one, and the save file's channel never needs restoring.
- **Emulator:** the mock gets a simulated POWMAN wake. It closes the
  channels and discards zterm's RAM state, keeping the store and the
  session record, so a test can play on across one.

A turn:
1. The game runs, faulting pages in with `SD_READ_BYTES`. Its output goes
   out with TEXT, and the status line with STATUS before each input.
2. At input, the LH5801 sends `KEY 00H`. The MCU answers with the first
   new line of the turn, and the LH5801 shows it with DISP_N_CHARS0
   (ED3BH).
3. Loop:
   - Send DONE.
   - Wait for a key with KEYSCAN_WAIT (E243H).
   - Wake the MCU, send KEY, and show the line it returns.
4. Repeat until KEY answers INPUT_DONE with the typed line. The LH5801
   copies it into the story's text buffer and tokenizes it.

**Console behaviour (zterm.c):**
- **Keys (milestone 1):** ▲/▼ scroll; letters, digits and symbols type
  (and jump to the input line); ◀ deletes the last character; CL clears
  the line. ENTER at the input line finishes it; anywhere else it jumps to
  the input line.
- **Transcript:** the whole game's text, kept unwrapped.
- **Word wrap:** a setting, on by default.
  - On: lines are wrapped to 26 columns.
  - Off: lines stay as printed, up to 80 characters, and ◀/▶ scroll a
    long line sideways.
- **Scrolling:** ▲/▼ move through the scrollback. After a turn the view
  starts at the first new line, so the player reads down to the prompt.
  The bottom line is the input line, and typing jumps there.
- **Status lines:** a function key shows two of them, the location and
  then the score and moves (or the time). The next key goes back to the
  text.
- **Settings menu:** a function key opens it. It holds word wrap, and the
  settings persist as MCONF entries.
- **Mirroring:** the transcript and status go to the host app when a BLE
  link is up.

## Milestones

1. **Core.** All v3 opcodes, paging, Z-strings, objects, dictionary and
   tokenizing. The console has transcript, KEY and input with the
   default view. Done when Zork I runs a scripted walkthrough in the
   emulator with the expected transcript.
   - **Done in the emulator (2026-10-29),** not yet on hardware:
     `testZipZiptest` (Infocom's own interpreter test) and `testZipZork1`
     (36 commands, the troll fight, a simulated POWMAN wake after 20,
     QUIT back to BASIC) match `tools/zref.py`'s transcripts exactly.
   - `tests/`: the stories, `*.cmd` scripts and the expected `*.txt`
     (`python tools/zref.py tests/zork1.z3 --script tests/zork1.cmd`).
     ZTRACE/ZSTATS/ZPROF in the test write a trace (as `zref.py --trace`),
     per-turn figures, and a profile for `tools/zprof.py`.
   - Speed: a Zork turn is 0.6-2 s of LH5801 time at 1.3 MHz, a combat
     turn about 3.3 s; 10-35 page reads a turn. Not yet tuned: operand
     decode and dispatch, a cheaper victim search, fewer page faults.
2. **Console.** Scrollback, status lines, word-wrap setting and settings
   menu.
3. **SAVE/RESTORE** to a file chosen at a console prompt, plus
   `$VERIFY`, RANDOM seeding and QUIT back to BASIC.
4. **Mirroring** to the host app, and ZPLAY.
