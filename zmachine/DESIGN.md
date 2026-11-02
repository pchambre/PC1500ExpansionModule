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
| next pages .. 4FFFH | 256-byte swap pages for everything above dynamic memory, LRU (Zork I: 34) |
| 4E00H .. 4FFFH | ZIP's start-up code (`LOADORG`, `INIT`: the GAME FILE prompt), run once; swap pages afterwards, so it costs none |
| 5000H .. 687FH | the interpreter (`ZIPORG`; 6K with SAVE/RESTORE/VERIFY and the power-cycle revive, 2026-11-02 -- was 5500H, 4.8K, 39 swap pages) |
| 6880H .. 68FFH | its variables (`V`) |
| 6900H .. 69FFH | the swap slots' ages and Z pages (64 slots at most) |
| 6A00H .. 6CFFH | the Z-stack (768 bytes; 1K until 2026-11-02) |
| 6D00H .. 6D31H | the story's full path (`STORY`: length, up to 40 characters), and REVIVE's two bytes |
| 6D40H .. 6DB7H | the output buffer (`OUTBUF`), and the save file's name in it |
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
SDLOAD M "ZIP"          ; loads at LOADORG and runs INIT: GAME FILE:
```

`build.ps1` makes `ZIP` from `zip.asm`. ZIP is an ordinary program for a
PC-1500 with the module: it uses only documented module commands and no
ROM feature of its own (the board owner's rule, 2026-11-02 -- a ZIP
keyword was tried and taken out).

`INIT` (the start-up pages, run once):
1. Saves S (BREAK and QUIT go back to BASIC with it), sends `ZTERM OPEN`
   for a console.
2. Reads its last game from the MCU's FL store (`EXP_COMMAND_FL_GET`, key
   `ZIPGAME`).
3. Sends `ZTERM PROMPT`: `GAME FILE: ` with that game ready to edit, F1
   the card's directory (the console's browser: up/down, L takes the
   name, as in SDLOAD's browser). ENTER takes the line, CL clears it, BREAK goes back to BASIC.
4. Makes the name a full path: from the root already if it starts with
   `/`, else the current directory (`EXP_COMMAND_GET_SD_CWD`) in front.
   Kept at `STORY`; over 40 characters is "Name too long." and the prompt
   again.
5. Opens it on SDOPEN channel 15 (`STCH`); a name that won't open is
   "Not found." and the prompt again.
6. Keeps it as `ZIPGAME` (`EXP_COMMAND_FL_SET`), and jumps to `ENTRY`.

`ENTRY` then:
1. Sends `ZTERM OPEN` again -- a fresh console, which now knows the
   story's path (the save file's default name, POWMAN's session record).
2. Reads the header and dynamic memory with READ_BYTES.
3. Starts at the header's initial PC.
4. At QUIT or BREAK, closes the story's channel and the console.

**A power cycle** (2026-11-02): the PC-1500's RAM keeps the game, but the
MCU starts afresh -- no channels, no console, the directory back at the
root. The first thing ZIP sends after ON is a KEY, and the console answers
it with ERROR; ZIP then reopens the story from `STORY` (a full path, so
the directory doesn't matter), sends `ZTERM OPEN`, writes `[Resumed]`,
starts the input line again and sends the key (`REVIVE`). The scrollback
is gone; the status lines come back at the next turn. A story that can't
be reopened is ZIP ERROR 2; a console that won't open, back to BASIC.
- ON after ZIP's OFF reads as BREAK (the ROM's latched IF bit 1, in the
  emulator at least): after its power-off returns ZIP clears that latch
  and waits for the next key, rather than quit. An auto power-off inside
  KEYSCAN_WAIT itself can't be told apart -- not yet seen on hardware.
  On the real PC-1500 (2026-11-02) both OFF/ON and an auto power-off came
  back to the game.
- Tests: `testZipPowerCycle` (an MCU cold boot alone, then OFF/ON with
  one), `testZipGamePrompt` (the prompt, F1, a missing file, BREAK) and
  `testFlStore`.

## Console protocol (`EXP_COMMAND_ZTERM`, window[0] = sub-op)

| Sub-op | In | Out |
|---|---|---|
| OPEN | [story channel][flags: bit 0 = time game] | [seed for RANDOM, 2 BE] |
| TEXT | [len][bytes] — output, `0DH` = new line | |
| STATUS | [score/hours 2][moves/minutes 2][flags][len][location] | |
| KEY | [key code, 00H = an input line begins][the longest line, 0 = just read] | [state][26 chars to show]; state 1 (done) adds [len at +27][the line from +28] |
| CLOSE | | |
| FILE | [0 save / 1 restore] | as KEY's; then KEY until ENTER gives the name. F1 browses the directory |
| PROMPT | [flags: bit 0 = F1 browses][longest][prompt len][prompt][text len][text] | as FILE's |

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
- **Word wrap:** a setting, on by default (MCONF ZWRAP, or the F2 menu).
  - On: lines are wrapped to 26 columns.
  - Off: lines are wrapped at 80 characters (the display buffer's length)
    instead, and while reading the scrollback ◀/▶ move a long line
    sideways, 13 characters a press. At the input line ◀ is still the
    backspace.
  - Changing it rewraps the scrollback at once: the line being read is
    found again at the new width (the transcript is kept unwrapped).
- **Scrolling:** ▲/▼ move through the scrollback. After a turn the view
  starts at the first new line, so the player reads down to the prompt.
  The bottom line is the input line, and typing jumps there.
- **Status lines (F1):** F1 shows the location, F1 again the score and
  moves (or the time, for a time game: Flags 1 bit 1), and F1 once more
  goes back to the text. Any other key on a status line goes back to the
  text and does nothing else.
- **Settings menu (F2):** one item so far, word wrap. ENTER, ◀ or ▶
  changes it; F2 or CL goes back to the text. The settings persist as MCONF
  entries.
- **Key codes** (KEYSCAN_WAIT, measured in pc1500emu): F1-F6 11H-16H, ◀
  08H, ▶ 0CH, ▲ 0BH, ▼ 0AH, ENTER 0DH, CL 18H, RCL 19H, MODE 1FH. F3-F6 and
  RCL are free.
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
   - **Done in the emulator (2026-11-01),** not yet on hardware:
     `testZipConsoleKeys` (F1's status lines, F2's menu, wrap off with
     ◀/▶, the rewrap) and `testMconfZwrap`. zterm_session's record is
     "ZT2" since zterm_t grew.
3. **SAVE/RESTORE** to a file chosen at a console prompt, plus
   `$VERIFY`, RANDOM seeding and QUIT back to BASIC.
   - **Done in the emulator (2026-11-01),** not yet on hardware.
   - The prompt is the console's (ZTERM FILE): "Save as: " or "Restore
     from: " on the input line, the last name used ready to edit -- at
     first the story's, with .SAV (ZORK1.Z3 -> ZORK1.SAV). ENTER takes it,
     CL clears it; no name at all is Failed.
   - The file (zip.asm "ZS1"), written with WRITE_BYTES on SDOPEN channel
     16 (FOR W, so an older, longer file's tail stays, unread): a 22-byte
     header -- "ZS1", the story's release, serial, checksum and static
     base, the PC (the SAVE's branch), ZSP and ZFP as Z-stack offsets --
     then the Z-stack, then dynamic memory. Zork I: 11380 bytes.
   - RESTORE reads the header first and changes nothing unless it's this
     story's (else Failed.); then the stack and dynamic memory, Flags 2
     kept (the interpreter's), and on from the SAVE, its branch taken. A
     file that ends early after the header is ZIP ERROR 8: memory is half
     restored by then.
   - `$VERIFY` sums the story file from 40H to the header's length, 1K
     reads, against the header's checksum.
   - RANDOM's seed comes from the MCU at OPEN, QUIT goes back to BASIC
     (milestone 1).
   - Tests: `testZipSaveRestore` (the default name, the file's size,
     RESTORE back, a missing file, another story's save, no name), and
     zork1.cmd now saves before the troll, verifies, and restores at the
     end (zref.py does SAVE/RESTORE too, in memory).
4. **Mirroring** to the host app (the launch keyword came early: `ZIP`,
   2026-11-02, above).
