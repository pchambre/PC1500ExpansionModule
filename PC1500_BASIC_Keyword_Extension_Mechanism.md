# PC-1500 BASIC Keyword Registration and Discovery

How the Sharp PC-1500's BASIC interpreter finds, tokenizes, and executes
BASIC keywords beyond the built-in set — including how expansion modules
(CE-150, CE-158) add their own commands, how a third-party tool
(BASWORD) adds fully dynamic user-defined ones, and the complete,
confirmed recipe for registering a genuinely new keyword backed by new
machine-language code, at any of the eight candidate module pages, at
either `PV` level.

In a nutshell, the range 8000H through BFFFH is available for expansion 
ROMs or ROM-like code. When trying to resolve a keyword, e.g. `CLOAD`, the
system ROM code scans for keyword tables in each of these 2K windows, 
with `PV` low and with `PV` high. Using `PV` doubles the available expansion 
space to 32K (16 pages). 8000H `PV` low is a special case, which looks like
a bug in the system ROM, as it only works for one keyword for each 
starting letter. The other pages do not have this limitation. When a 
match for the keyword is found, the keyword's code is executed with `PV`
left as it was set when the match was found. Matching requires first a hit
on the first letter of the keyword in a first-letter index (probably to 
save time by skipping a page that doesn't have any keywords that match the
first letter of the keyword being searched), and then finding the exact 
match of the keyword.

Verified by cross-checking real ROM byte content (`ROM1.BIN`,
`CE-150.ROM`, CE-158 dumps, BASWORD's own binary) against the *PC-1500
Technical Reference Manual*, and by live dispatch/instruction tracing on
`pc1500emu`, a from-scratch, hardware-validated LH5801/PC-1500 emulator,
via its scriptable command interface (`loadrommodule <base hex>
<requirePv 0|1> <usePuBank 0|1> <path>`, `entertrace`, `dump`, etc. —
see the emulator's own `README.md`).

**A note on methodology, since it materially affected which findings in
an earlier version of this document turned out to be wrong**: testing a
live keypress's dispatch by sending separate `key`/`type` pipe commands
and then waiting a fixed sleep before checking `status`/`display` is
**not reliable** — the emulator's ~60fps frame loop can be caught
mid-transition, producing a stale read that looks like failure (or
success) when the real outcome is the opposite. The only method
confirmed trustworthy for observing dispatch is `entertrace <cycles>`,
which presses Enter and traces execution as one atomic, unbroken
instruction stream from the command handler itself, immune to the
frame-loop race. Every finding below that depends on "did this word
dispatch" was confirmed via `entertrace`, and several claims from an
earlier draft of this document were retracted after being caught by
comparing sleep-based results against `entertrace` results for the same
ROM.

## Sources

- *PC-1500 Technical Reference Manual* — §5-2 "Internal code chart" (the
  ~119 built-in keyword codes), §5-3-5/5-3-6 (program/reserve-area byte
  structure), §5-4 (system subroutine conventions, including the
  "arithmetic register" argument-passing pattern used throughout the ROM).
- **BASWORD** (Christophe Gottheimer, 1986–2013), a third-party "BASIC
  keyword manager" — `README`, `basword.log`, and `BASWORD-00C5.ASM` (a
  disassembly of its own binary), from `basword.zip` at
  `http://www.pc1500.com/basword.html`.
- **CE-150.ROM** — a real Sharp printer/plotter/cassette-interface module
  ROM dump.
- **CE-158** (RS-232C interface module) ROM dump (16KB, PU-banked, PV-high).
- **`ROM1.BIN`** — a base PC-1500 ROM.
- **`pc1500emu`** — the live test bed for every dispatch/trace result
  below, including a purpose-built bisection methodology (§5) used to
  empirically isolate exactly which bytes of a real module are load-
  bearing for dispatch, by systematically zeroing regions of a working
  file and re-testing. https://github.com/pchambre/pc1500emu. 

## 1. Keyword table format

Every keyword table on the system — the base ROM's own built-in table,
every expansion module's own table, and BASWORD's resident user table —
uses the same entry layout:

```
+--------+------------------------+----------+-----------+
| 1 byte | name (ASCII)           | 2 bytes  | 2 bytes   |
| marker | length = marker & 0x0F | code     | address   |
+--------+------------------------+----------+-----------+
```

- **Marker byte**: low nibble = the name's length in characters (1–15).
  The high nibble varies per entry within the same table (observed
  values `9, A, B, C, D, E, F`) and is believed to encode BASWORD's
  documented three-value execution-context restriction (§10) — the exact
  nibble→letter mapping isn't pinned down. **Correction to an earlier
  version of this document, which claimed the high nibble is entirely
  free (a made-up `D` "works fine"):** that was true only because the
  single-entry test it was based on never exercised the skip-scan path.
  **The high nibble's bit 4 (`0x10`) is load-bearing for any entry that
  isn't the first one under its letter** — see §4's skip-scan finding.
  `D`/`F`/`B`/`9` (odd nibble value) all have bit 4 set and make the
  walker abandon the match attempt when skipping *onto* that entry;
  `C`/`A`/`8`/`E` (even) don't. Real CE-150 confirms the pattern
  unprompted: `CLOAD`(`A5`)/`CSAVE`(`C5`) are both only reachable by
  skipping past a preceding same-letter entry and use even nibbles;
  `CHAIN`(`95`)/`MERGE`(`D5`) are both reached directly via their own
  index slot and don't need to.
- **Name**: plain ASCII, exactly `length` bytes, no terminator.
- **Code**: 2 bytes, big-endian. This is **not a free choice and not
  simply "must not collide with an existing code"** — its high byte is a
  page-specific identifier, load-bearing for both completing the
  name-based table-walk (§4) and for execution-time re-dispatch (§5).
  See §5 for what's actually known about valid values. Its **low byte**
  decides whether the keyword is a statement or a function (§13): `5xH`
  a function with no argument (`MEM`), `6xH`-`7xH` a function with one
  (`COS`), `80H` and up a statement.
- **Address**: 2 bytes, big-endian, an absolute address of the routine's
  entry point. Read and used only by the *execution-time* lookup (§5),
  not the name-based lookup (§4).
- **Table terminator**: a single byte with low nibble `0` (e.g. `D0`) —
  "next entry has a zero-length name," no fields follow. This is also
  what a *corrupted* marker (accidentally zeroed) looks like to the
  walker — see §4's "sequential walk" finding for why that matters even
  for entries that aren't the corrupted one.

Worked example, from CE-158's own ROM (`SETCOM`):
`D6 53 45 54 43 4F 4D E8 82 82 D6` → marker `D6` (length 6), name
`SETCOM`, code `E882`, address `82D6`.

## 2. The first-letter index

**Immediately before every keyword table (the base ROM's own, and every
peripheral module's own) is a 52-byte, 26-entry index — one 2-byte
big-endian pointer per letter A–Z.** For letter `X`, the value at
`indexBase + (X-'A')*2` is either:

- `0x0000` — no keyword in this table starts with `X`, or
- a pointer to the **second letter** of that letter's entry in the table
  (the first letter is never re-stored in the pointer, since the index
  lookup is already keyed by it).

Confirmed byte-for-byte in the real `CE-150.ROM`, which contains two
separate keyword tables, each with its own index immediately before it:

- **Printer/graphics table** (`COLOR, CSIZE, GRAPH, GLCURSOR, LCURSOR,
  LF, LINE, LLIST, LPRINT, RLINE, ROTATE, SORGN, TAB, TEST, TEXT`, page
  `B000`): index has non-zero slots at `C, G, L, R, S, T`, each
  independently confirmed to point at that letter's first entry's second
  letter.
- **Cassette table** (`CHAIN, CLOAD, CSAVE, MERGE, RMT`, page `B800`):
  index has `C → 0xB856` (2nd letter of `CHAIN`, its first C-entry),
  `M → 0xB874` (`MERGE`), `R → 0xB87E` (`RMT`); all other 23 letters
  `0x0000`.

```
; first-letter index
LB820:
	.dw 0x0000  ; A
	.dw 0x0000  ; B
	.dw 0xB856  ; C
	.dw 0x0000  ; D
	.dw 0x0000  ; E
	.dw 0x0000  ; F
	.dw 0x0000  ; G
	.dw 0x0000  ; H
	.dw 0x0000  ; I
	.dw 0x0000  ; J
	.dw 0x0000  ; K
	.dw 0x0000  ; L
	.dw 0xB874  ; M
	.dw 0x0000  ; N
	.dw 0x0000  ; O
	.dw 0x0000  ; P
	.dw 0x0000  ; Q
	.dw 0xB87E  ; R
	.dw 0x0000  ; S
```

The base ROM's own ~119-keyword built-in table uses the identical
design, at `C01EH`.

**Important correction: the index is a hint for where the word's own
letter-bucket begins, not a direct jump to the matching entry.** See §4
— the index tells the walker where in the table to *start* comparing,
but a table entry can only be reached by successfully walking every
entry before it in the table, in order, regardless of which letter each
one starts with. A page whose table happens to have entries for several
different letters before the one you're adding still needs all of those
*preceding* entries' marker bytes intact.

Table order within a letter-bucket is not required to be strictly
alphabetical (`GRAPH` sits before `GLCURSOR` in the real ROM despite
`G-R` > `G-L`) — the index slot just needs to point directly at whatever
comes first for that letter.

Typing an abbreviated form (`CS.`, `CSA.`, per the PC-1500's documented
`CS.`/`CSA.`/`CSAV.` convention) needs no separate storage: matching is a
genuine letter-by-letter prefix search against the table's full stored
name, stopping early at the period.

## 3. Where tables live

### The eight candidate pages, and PV

The base ROM's peripheral-module scan covers eight fixed 2KB-aligned
pages: `8000, 8800, 9000, 9800, A000, A800, B000, B800`. **Every one of
these eight pages works, at either `PV` level — `PV` is not a fixed,
hardwired property of a given page.** This corrects an earlier version
of this document, which claimed (self-contradictorily, in different
sections) that e.g. `0x8000` specifically "needs PV-high." It doesn't —
a self-consistent custom module answering at PV-**low** was confirmed
dispatching cleanly, via direct `entertrace`, at **all eight** pages in
one sweep, using code values `0xE0`-`0xE7` respectively (§5's PV-low
formula). PV-high dispatch was independently confirmed working at both
`8000` (real CE-158's `SETCOM`) and `B800` (real CE-150's `CHAIN` /
`CLOAD` / `CSAVE`, and a wholly from-scratch module built from nothing
but the mechanism described in this document).

What *is* real: the base ROM's own scan loop derives `PV` per candidate
page as it walks (`RPV` unconditionally, then a conditional `SPV`
depending on which candidate is being probed — see the disassembly
below), and whichever `PV` level a given probe uses, **your module must
answer at that exact `PV` level for that specific page to be found at
all** — this is what makes "the module's own declared `PV` requirement
matching the page" necessary, not evidence that a page is hardwired to
one level. Determining the code value (§5) needed for a *new* PV-high
page you haven't already seen a real example on is the only genuinely
unsolved part of this mechanism — see §5's "what's not known" note.

CE-159 (a plain RAM expansion, no onboard firmware) provides no table at
all — there's no PV/PU gating below `0x8000` in the first place, so
there's nothing for the ROM to probe there.

### The module-presence signature (printer-specific, unrelated to keyword dispatch)

Separately from the table/index/code mechanism above, the TRM documents
that the processor expects to read `C0H` at `A000H` (PV low) specifically
when a printer-class module is connected, `FFH` when not — confirmed
real: `CE-150.ROM`'s own first byte is literally `C0`. **This byte is
not required for keyword dispatch to work** — confirmed directly:
zeroing the entire first 512 bytes of a real `CE-150.ROM` image
(including this signature byte) during bisection testing (§5) had no
effect on whether `CSAVE`/a renamed custom entry on the same module
still dispatched correctly. It's a separate, TRM-documented check
(presumably consulted by whatever code implements `LPRINT`'s
"is a printer connected" logic), not part of the generic 8-page
candidate scan.

### The 8-page scan structure

Disassembled and traced directly (`ROM1.BIN`, `FA40`–`FA90`, called
fragment at `FB30`–`FB40`):

```
FA44: LDA UL          ; loop top -- UL holds the current candidate page
FA45: SJP FA58        ; candidate-check subroutine
FA48: STA UL
FA49: BCS +06         ; carry set -> exit loop
FA4B: VMJ FF3C        ; -> FB32: SJP FA65 (RPV/SPV probe) -> FB35: POP A -> RTN
FA4D: BZS -85         ; (not exercised for a "no module" scan)
FA4F: JR -0D          ; loop back to FA44
```

```
FA65: ANI A,FE        ; clear bit 0
FA67: RIE             ; disable interrupts
FA69: RPV             ; `PV` -> low
FA6A: BCR +02         ; skip SPV if carry reset
FA6C: SPV             ; `PV` -> high
FA6D: INC A
FA6E: STA (79D0)      ; counter
FA71: SIE             ; re-enable interrupts
FA73: RTN
```

Across a full pass, the candidate page value visits exactly eight
values — `B8, B0, A8, A0, 98, 90, 88, 80` — i.e. every 2KB-aligned page
from `0x8000` to `0xBFFF`, `PV` derived per-page as part of the same walk
(not two separate four-page passes for each `PV` level). This single
8-iteration loop, reused for both the name-based lookup (§4) and the
execution-time code-based lookup (§5), is the concrete mechanism behind
"eight candidate 2KB pages" referenced throughout this document.

## 4. Tokenization (finding a keyword by name)

A typed line is scanned left to right. For each candidate word, the
tokenizer first checks the base ROM's own ~119-keyword table (via its
own first-letter index at `C01EH`); on failure, it falls through (`VMJ
FF3CH`) into the peripheral scan, which walks the same eight 2KB pages
(§3), and for each page whose sentinel (`0x55` at the page's very first
byte) and `PV` match, consults that page's first-letter index (§2) for the
word's first letter.

**Correction to an earlier version of this document — the walk is
sequential from the table's start, not a direct jump to the matched
entry.** The index gives a starting hint, but reaching any entry
requires stepping through every entry before it *in table order*
(regardless of which letter each one starts with). **A zeroed (or
otherwise corrupted) marker byte on *any* entry earlier in the table is
read as the table terminator and stops the walk right there — even for
a perfectly well-formed target entry sitting later in the same table.**

**Second correction, this one to the mechanism itself, not just wording
— the earlier version's claim that the skip-to-next-entry distance comes
from "the marker byte's low nibble" is wrong.** Traced directly
(`ROM1.BIN`, live `entertrace` through an actual mismatch-then-retry):
on a name mismatch, the walker does **not** compute a skip distance from
the marker at all. It falls into a byte-by-byte scan (`F9F5`-`FA3E`)
that uses `CIN` (compare-and-increment) to walk forward one byte at a
time, comparing each against the literal constant `0xE0`, continuing
*while the table byte is ≤ `0xE0`* and stopping on the first byte
*strictly greater than* `0xE0` — i.e. it's hunting for what it assumes
is the start of the next entry's **code** field (whose high byte is
always `≥0xE0` by convention, see §5), not measuring a length. Once
found, it does three more fixed `INC X`s (skipping the rest of the code
field and the two-byte address field) and lands on what should be the
next entry's marker byte. This has two direct, confirmed consequences
that a "skip by length" model wouldn't predict:

1. **Any entry whose own code-field high byte is exactly `0xE0` breaks
   the scan for everything after it on that page.** `0xE0 ≤ 0xE0` is
   true, so the hunt doesn't stop there — it keeps scanning straight
   through that entry's own code+address fields, into the *next*
   entry's marker/name/code/address, and beyond, corrupting where it
   eventually (if ever) lands. This is exactly the code value page
   `8000` requires under the PV-low formula (§5) — **page `8000` cannot
   host more than one keyword under the same first letter, at PV-low,
   full stop; every other page's PV-low code value (`8800→0xE1` …
   `B800→0xE7`) is strictly above the boundary and unaffected.**
   Confirmed live: a from-scratch two-entry same-letter table dispatches
   its first entry fine at `8000`/PV-low but never reaches the second,
   regardless of marker or code low-byte changes; moving the identical
   table to `8800` (code `0xE1xx`) fixes it with no other change.
2. **After landing on the next entry, the walker reads its marker byte
   and `ANI`s it with `0x10`** (bit 4) **— if the result is nonzero, it
   gives up on the whole match attempt immediately** (falls through to
   the "not found" path) instead of retrying against that entry. See the
   marker-byte bullet in §1 for the fix and real-ROM precedent. This is
   a completely separate, independently-required condition from (1) —
   fixing only one still fails silently.

Both of these were invisible to the original bisection-based
investigation below, because that investigation's one worked example
(`CE-150`'s cassette table, `CSAVE` renamed) happened to already satisfy
both conditions by construction (`B800`'s code value is `0xF0`, nowhere
near the `0xE0` boundary; `CLOAD`/`CSAVE`'s real markers already have
bit 4 clear) — so "skip by low-nibble length" was never falsified by
that test, it just happened to predict the same landing address as the
real byte-scan mechanism for that specific, lucky case.

This was isolated by bisection: starting from a known-working hybrid
module (real `CE-150.ROM` with just its `CSAVE` entry renamed to a
custom keyword), systematically zeroing 512-byte, then 64-byte, then
16-byte, then individual-byte ranges and re-testing dispatch via
`entertrace` after each. The critical bytes converged on exactly:

- the page's sentinel byte,
- the target entry's own first-letter index slot,
- the target entry itself (marker + name + code + address),
- **and the marker bytes — only the marker bytes, not the rest of their
  content — of every table entry positioned before the target in table
  order** (in the tested case, `CHAIN`'s and `CLOAD`'s own markers,
  which precede the renamed entry in `CE-150`'s table),
- plus, empirically, the marker byte of the entry immediately
  *following* the target too (`MERGE`'s marker in the same test) —
  consistent with the walker reading one entry ahead as part of
  confirming/finalizing a match, rather than stopping the instant a name
  matches.

Zeroing literally everything else in an 8KB real module file — including
the entire rest of the ROM outside this one 2KB page, and including
every byte of the intervening entries' own *names*, *codes*, and
*addresses* (only their marker bytes preserved) — left dispatch fully
working. **If your new entry is the very first one in a page's table
(i.e. nothing precedes it), none of this matters — no preceding markers
are needed, confirmed directly with a genuinely single-entry, wholly
from-scratch module.**

## 5. Execution (running the tokenized code) and the code value

**Execution is a second, separate lookup from tokenization — driven by
the 2-byte code value, not the name.** When the interpreter reaches a
token in the line, it needs to re-locate which of the eight candidate
pages holds the matching table entry (having only the 2-byte code, not
the page it was originally found on), then reuses the same
candidate-scan machinery (§3) a second time — but this time comparing
each entry's stored **code** field against the token, not its name. On a
match, the entry's own stored **address** field (§1) is read and used to
transfer control.

If no page's table has a matching code entry, execution falls through to
a **hardcoded `LDI UH,0x1B`** — `ERROR 27` ("optional printer not
connected") — a literal fallback value, not a genuine device-presence
read, regardless of what the failing command actually does.

### Borrowing another module's codes: `F0xx` works from any page, `E6xx` doesn't (2026-09-30)

Tested in `pc1500emu` with no CE-150 attached, by putting two CE-150
keywords into the PC1500-PSOC5 module's own table (page `8800H`, PV low),
under the CE-150's own codes:
- **`COLOR`, code `F0B5`:** typing it stored the token `F0 B5`, and
  running it reached the module's routine (its MCU then rejected the
  foreign token: `ERROR 1`).
- **`CSIZE`, code `E680`:** typing it stored `E6 80` too, but running it
  gave `ERROR 27`, i.e. no entry found.

So:
- **Tokenizing finds the name on any page,** whatever the code.
- **Running an `F0xx` token searches every page's table for the code.**
  These are the codes the base ROM itself knows as printer statements
  (`ERROR 27` when no module answers).
- **Running an `E6xx` token only looks on the one page that code
  belongs to** (`E6` = `B000H`, PV low, the CE-150's). Another module
  can't answer it.

A module offering the CE-150's commands can therefore reuse its
`F0B5`-`F0BC` codes (`COLOR`, `LF`, `LINE`, `LLIST`, `LPRINT`, `RLINE`,
`TAB`, `TEST`). It needs its own codes for the `E680`-`E686` ones
(`CSIZE`, `GRAPH`, `GLCURSOR`, `LCURSOR`, `SORGN`, `ROTATE`, `TEXT`).

**How the PC1500-PSOC5 module lives with that (2026-10-01):**
- **Its own codes for the seven (`E1C0`-`E1C6`) hand over to a real
  CE-150.** Their table entries point at a ROM stub, not the usual entry.
  If it sees a CE-150 (`C0H` at `A000H` and the `55H` page sentinel at
  `B000H`, PV low), it reads the token's low byte at `Y-1`. It then finds
  `E680` + its low nibble in the CE-150's table at `B054H`, and jumps to
  that routine with `Y` untouched, as BASIC itself would. Confirmed in
  pc1500emu with the real `CE-150.ROM`: the module's `CSIZE` stored its
  argument at `79F4H` as the CE-150's `LB180` does.
- **Programs are translated as they're saved and loaded.**
  - **Files always hold `E680`-`E686`.** A load turns them into
    `E1C0`-`E1C6` only when no CE-150 is attached.
  - **Only real tokens change.** The translation follows the program's
    line structure and skips quoted text.
  - **Tokens split across transfer chunks are handled.** Writes carry the
    first byte over to the next chunk. Reads hand it on one read later;
    the ROM's load loop takes short reads and stops only at a read of 0.
- **The `F0xx` eight need neither.** A real CE-150's table is searched
  first, both to tokenize and to run.

### The code's high byte is not derived by a simple formula — this corrects the previous version of this document

An earlier version of this document claimed
`target_page_high = 0x80 | ((code_high_byte & 0x07) << 3)` — i.e. "only
the low 3 bits of the code's high byte matter" — reverse-engineered from
a disassembled fragment (`FAA9H`-`FAB0H`) that does mask the low 3 bits.
**This formula is disproven.** Direct counter-example, confirmed via
`entertrace`: code `0xEA85` and code `0xE285` share identical low 3 bits
(both `010`, both "select" page `0x90` under the formula) — placed at
the same page (`9000`), only `0xE285` actually dispatches; `0xEA85`
throws `ERROR 27`. A second, independent counter-example: code `0xF0xx`
lives on page `B800` (confirmed real, for three different commands —
see below) — but `0xF0`'s low 3 bits are `000`, which the old formula
would resolve to page `0x80`, not `0xB8`. The masked-bits fragment
appears to be real ROM code, but isn't the *complete* mechanism (or
isn't used the way previously assumed); the actual requirement is
stricter than "match on 3 bits."

**What's actually confirmed, empirically, per `PV` level:**

- **PV-low: a clean, confirmed formula.** `code_high = 0xE0 | page_index`,
  where `page_index` is 0 for `8000` counting up to 7 for `B800` (i.e.
  `8000→E0, 8800→E1, 9000→E2, 9800→E3, A000→E4, A800→E5, B000→E6,
  B800→E7`). Verified by direct `entertrace` dispatch confirmation at
  **all eight pages simultaneously**, each with its own module using
  this formula's value, all successful — **for a single keyword per
  page.** With more than one keyword sharing a first letter on the same
  page, `8000` is a special case: its own formula-required value
  (`0xE0`) is exactly the threshold the §4 skip-scan hunts past, so it
  breaks that page specifically for multi-entry-per-letter tables (see
  §4's skip-scan finding for the full mechanism and confirmation). The
  other seven pages are unaffected. If a keyword set needs more than one
  entry under the same letter, don't place the table at `8000`/PV-low.
- **PV-high: no known formula — each page's required value has to be
  read off a real example, or found by brute-force.** Confirmed real
  values, from actual Sharp ROM content:
  - Page `8000`: `0xE8` (from CE-158's real `SETCOM` entry).
  - Page `B800`: `0xF0` (from CE-150's real `CHAIN`, `CLOAD`, *and*
    `CSAVE` entries — all three different commands on the same page
    share this exact value, strong confirmation it's a per-page
    constant rather than per-entry). Confirmed as the *only* working
    value for that page by brute-forcing the entire `0xF0`-`0xF7` range
    plus `0xFF` and `0x70` against a from-scratch module — only `0xF0`
    dispatched; every other candidate, including ones that "should"
    work under the old low-3-bits theory, failed.
  - The other six pages' PV-high values (`8800, 9000, 9800, A000, A800,
    B000`) are **not known** — no real ROM content sampled here uses
    them, and they haven't been brute-forced. If you need a custom
    keyword at PV-high on one of these, either read the value off a real
    module known to live there, or repeat the brute-force approach used
    for `B800` (build a table entry with everything else correct, sweep
    candidate code high bytes, and check via `entertrace` which one
    lands in your routine).
  - The **low byte** of the code, in every test performed, was
    irrelevant to whether dispatch succeeds (`0xF000` dispatched
    identically to `0xF095`) — it doesn't appear to be checked by the
    table-walk or the page-resolution step, only the stored value is
    presumably read back if something else downstream cares.

## 6. The dispatched routine's calling convention

### Minimal tail for a clean, immediate dispatch

A dispatched routine returns via a **register-set-then-`STX P`
indirect jump**, not a subroutine `RTN`. A bare `RTN` pops whatever's
left on the stack from the dispatch call chain — not a valid return
address — and lands execution essentially at random, which manifests as
a constant, misleading `ERROR 1` ("syntax error") regardless of the
routine's own logic.

The minimal, confirmed-sufficient tail for a routine to be reached,
execute, and cleanly return to BASIC's idle prompt on **this** keypress
is just:

```
LDI XH, 0xE2
LDI XL, 0xAA
STX P
```

(`0xE2AA` is BASIC's own stable idle-prompt address.) Confirmed via
direct `entertrace`: `P` lands on the routine's own address, executes
these three instructions, and returns to `E2AA`, repeatedly, across
every one of the eight PV-low pages and both confirmed PV-high pages.

**This alone is not sufficient to avoid corrupting state that only
manifests on the *next* keypress** — see below. **And it only works for a
keyword typed at the prompt**: jumping to the idle loop from inside a
running program abandons the program. §12 describes the exit the ROM's own
statements (and the CE-150's) use instead, `VEJ E2`, which works in both
modes and needs none of the fixes below.

### Two additional, separately-discovered fixes needed to avoid corrupting the *next* keypress

Even with the minimal tail above and nothing else added, a real dispatch
leaves the machine in a state one stack level shallower than the
pristine idle baseline (`S=784C` instead of the true idle `S=784D`,
consistently observed across every successful dispatch trace in this
document). This is a **live, latent bug**, not a display artifact — left
unaddressed, it corrupts state that causes the next *ordinary* keypress
(not even a further custom-keyword dispatch) to throw a spurious
`ERROR 1`. Two independent root causes were isolated (both via direct
`ROM1.BIN` disassembly and trace, in an earlier phase of this same
investigation, on `pc1500emu`'s dispatcher path):

1. **The dispatcher's own un-popped `PSH A`** (at ROM address `FAFE`)
   is never matched by a corresponding pop anywhere in the normal flow
   for the peripheral-scan dispatch path. Left alone, it causes a later,
   completely unrelated subroutine's `RTN` (at `EE7F`, part of ordinary
   VRAM-clearing housekeeping that runs on *any* subsequent keypress) to
   pop garbage and jump into unmapped memory. **Fix**: `POP A` as the
   very first instruction of your custom routine, before anything else.
2. **A stale `784EH`/`784FH` "continuation pointer."** The dispatcher's
   `PSH X` at `FA8C` (pushing `0xC455`) is legitimate for the
   peripheral-scan path, but nothing refreshes `784EH`/`784FH` to the
   value the base-ROM dispatch path would naturally leave there
   (`0xCA92`), so a later keyboard-state-machine `RTN` (at `E3A0`) reads
   stale content and routes to the wrong follow-on code, corrupting an
   LCD-buffer-mode byte at `7880H`. **Fix**: explicitly
   `LDI A,0xCA; STA (784EH); LDI A,0x92; STA (784FH)`.
3. **A related, smaller issue**: the `BUSY` flag (`764EH` bit 0) can be
   left stuck set. **Fix**: `ANI (764EH),0xFE`.
4. **Documented separately** (PC-2 Assembly Language manual, p.25): "If
   you execute a ROM call which resets the cursor pointer and are going
   to return to BASIC, you must set bit 0 of location `7874H`" — via
   `ORI (7874H),0x01`.

**A genuinely minimal, fully-correct custom routine's shape is therefore**:

```
POP A                          ; undo dispatcher's un-popped PSH A
LDI A, 0xCA
STA (784EH)
LDI A, 0x92
STA (784FH)                    ; refresh stale continuation pointer
ANI (764EH), 0xFE              ; clear stuck BUSY flag
ORI (7874H), 0x01              ; per PC-2 manual, cursor-pointer reset
... your own routine logic here ...
LDI XH, 0xE2
LDI XL, 0xAA
STX P                          ; clean return to idle prompt
```

This was confirmed end-to-end, live, on `pc1500emu`: after a routine
using only the naive tail (no `POP A`/`784E` fixes) was dispatched, the
*next*, ordinary keypress (not even another custom keyword — a plain
`PRINT 1`) threw `ERROR 1`. Adding all four fixes above eliminated it —
confirmed by typing `PRINT 1` immediately after the custom keyword and
getting a clean `1`, no error, with `S` and `764EH` both back at their
exact pre-dispatch baseline.

**Separately, an unrelated mistake to avoid**: don't copy a real ROM
command's own tail convention when your routine's own preceding logic
doesn't match its assumptions. `CLOAD` (a base-ROM cassette command)
ends with `POP Y; LDX U; STX P` — but that `POP Y` pops something
*`CLOAD`'s own* preceding code pushed for its own bookkeeping. A custom
routine that doesn't itself push anything and blindly copies this tail
pops a value that was never pushed *for it*, silently unbalancing the
stack in the same class of way as the dispatcher-level `PSH A` issue
above, but self-inflicted rather than inherited.

### Arguments: the arithmetic register

**Superseded by §12** (2026-09-25, traced in `pc1500emu`). The earlier
hypothesis here — that the interpreter evaluates a keyword's arguments into
the arithmetic register before calling it — is wrong: nothing is evaluated
beforehand. The routine is entered with `Y` pointing just past its own
token, at the raw (tokenized) statement text, and parses its arguments
itself, calling the expression evaluator (`VEJ DE`) for each expression.
The arithmetic register is where that evaluator leaves each result:

- **X**: `7A00H`–`7A07H` (first/only operand)
- **Y**: `7A10H`–`7A17H` (second operand, two-operand routines only)

in the ROM's packed-BCD floating-point format (TRM §5-3-1), or the
alternate layouts for binary integers (§5-3-2: `B2H` at `+4`, value at
`+5`/`+6`) and character strings (§5-3-3: `D0H` at `+4`, address at
`+5`/`+6`, length at `+7`; a computed string's bytes are in the string
buffer `7B10H`–`7B5FH`).

## 7. Complete recipe: registering a real custom keyword

Confirmed end to end, repeatedly, across both `PV` levels and multiple
pages — including from genuinely from-scratch modules containing no real
CE-150/158 content at all:

1. **Table entry** (§1): marker + name + code + address. If placed
   anywhere other than the very first entry in the page's table, every
   entry before it in table order needs an intact marker byte (its low
   nibble correct for its own name length) — see §4. Content of those
   preceding entries beyond their marker byte doesn't matter and can be
   garbage. The entry *immediately following* yours also needs an intact
   marker byte, per §4's bisection finding — **and specifically needs
   bit 4 (`0x10`) of that marker byte clear**, per §4's skip-scan
   finding, or the walker abandons the match attempt when it reaches
   your entry. Any entry that's the first under its own letter (reached
   directly via the index, not by skipping a mismatch) is exempt from
   this — its own marker's bit 4 doesn't matter *for reaching it*.
   **But with more than one letter in the table, give each letter's first
   entry a marker with bit 4 SET** (`0xD_`/`0xF_`), as the CE-150's own
   table does (`GRAPH` F5, `LCURSOR` F7, `RLINE` F5, `SORGN` F5, `TAB` F3).
   That set bit is what stops a skip-scan coming from the letter before.
   Without it, a word that mismatches every entry of its own letter runs
   on into the next letter's entries, and those are compared **from their
   second character** (the walker takes the first as already matched via
   the index). Confirmed 2026-09-30 in pc1500emu: with an L chain
   (`LCURSOR`, `LF`, ...) following the F chain behind a bit-4-clear
   marker, `A=&FF` became ERROR 1 — the second `F` tokenized as `LF`.
2. **First-letter index** (§2): set the new keyword's first letter's
   2-byte slot to point at the entry's second letter.
3. **A `0x55` sentinel byte at the very start of whichever 2KB page the
   entry lives on.** Separate from the printer-presence check in §3.
   Confirmed required (page invisible without it) and confirmed
   sufficient on its own — no other page-wide "presence" signature is
   needed for keyword dispatch specifically.
4. **A `PV` level for the page that matches what your module declares**
   (`loadrommodule`'s `requirePv` argument, or a real module's own
   electrical answer-level). Any of the eight pages works at PV-low with
   a self-chosen code (step 5). PV-high works too, but only if you
   already know (or brute-force) the correct code high byte for that
   specific page — see §5.
5. **Code value** (§5): for PV-low, `0xE0 | page_index`
   (`8000→E0` … `B800→E7`) is a confirmed, general formula. For PV-high,
   use a known real value (`8000→E8`, `B800→F0`) or discover it
   empirically for other pages. The low byte doesn't matter. **If your
   table has more than one entry sharing a first letter, don't use page
   `8000`/PV-low** — its own required code value sits exactly on the §4
   skip-scan's boundary and breaks multi-entry tables there specifically.
6. **Address field** (§5): set to the routine's real, physical address.
7. **Routine** (§12): reads its arguments at `Y` (evaluating expressions
   with `VEJ DE`), leaves `Y` at the end of its statement, and ends with
   `VEJ E2` — which works typed at the prompt and inside a running
   program, with no fix-ups. Not a bare `RTN`, and not a copied convention
   from an unrelated real command whose own preceding code doesn't match.
   (§6's older prompt-only tail — `POP A`, the `784E`/`784F`/`764E`/`7874`
   fixes, `LDI XH,0xE2; LDI XL,0xAA; STX P` — also works, but only for a
   typed command.)

**Live end-to-end confirmations on file, this investigation**:

- A genuinely minimal (2KB), single-table-entry, from-scratch module
  dispatching `INVERT` cleanly at `9000`/PV-low, with the full §6 fix
  set applied, confirmed not to corrupt the following `PRINT 1`.
- All eight candidate pages (`8000, 8800, 9000, 9800, A000, A800, B000,
  B800`) independently confirmed dispatching a self-consistent PV-low
  module, via `entertrace`, in one sweep, using the `0xE0|page_index`
  formula.
- `8000`/PV-high confirmed working for real CE-158 content (`SETCOM`),
  both PU-banked and not.
- `B800`/PV-high confirmed working for a **wholly from-scratch, single-
  entry module** (no real CE-150/158 bytes anywhere), once the correct
  page-specific code value (`0xF0`) was used — the earlier failure to
  get *any* PV-high custom module working was entirely due to using an
  invented, wrong code value, not any deeper limitation of the
  mechanism, PU-banking, or module size.
- A wholly from-scratch **six-entry, single-letter table** (`SDF`,
  `SFMT`, `SLOAD`, `SLS`, `SRM`, `SSAVE`, all under `S`) at `9000`/
  PV-low, once both the §4 skip-scan fixes were applied (page moved off
  `8000`, every non-first entry's marker given an even high nibble):
  first (`SDF`), middle (`SLOAD`), and last (`SSAVE`) entries all
  independently confirmed dispatching correctly via `entertrace` — this
  is the case that originally isolated both bugs, via bisection against
  a two-entry table down to `SLOAD`+`TSAVE` (different letters, works
  unconditionally) vs `SLOAD`+`SSAVE` (shared letter, only the first
  entry dispatched until both fixes were applied).

This makes the ROM-module approach (§3) a fully general, working
alternative to BASWORD's resident-driver approach (§9) for any of the
eight candidate pages, at either `PV` level — not limited to the two slots
real Sharp hardware happens to occupy, and not requiring any of a real
peripheral's own unrelated content, provided the page-specific PV-high
code value (if needed) is already known or discovered first.

## 8. Bisection methodology (for discovering an unknown page's PV-high code, or debugging a non-dispatching module)

The technique used throughout §4 and §5 to isolate exact load-bearing
bytes/values, generalizable to any similar "why doesn't this dispatch"
question:

1. Start from a **known-working** file (either a real module confirmed
   dispatching some real command, or your own module confirmed
   dispatching *something*).
2. Make the smallest possible change toward what you're trying to
   isolate (e.g. zero a byte range; swap one field's value), keeping
   everything else fixed.
3. Test via the full clean cycle: kill/relaunch the emulator, `automation
   on`, `loadrommodule`, `key cl`, `type NEW0`, `key enter`, `type
   <word>`, then **`entertrace <cycles>`** (not a sleep-based
   `key enter` + `status`/`display` check — see this document's opening
   methodology note) and grep the trace for whether `P` ever reaches
   your routine's address.
4. Binary-search the range: if a whole chunk breaks it, halve the chunk
   and retest each half; individual bytes that don't break dispatch
   alone can still break it in combination (observed directly — a
   table's marker chain only failed when *paired* zeroed bytes were
   tested, not singly), so don't stop at the first passing single-byte
   result if a larger enclosing range still fails.
5. Once a candidate value/byte is isolated, confirm by testing the
   *opposite* direction too — e.g. once a candidate code value is found,
   confirm neighboring/plausible-looking values in the same family
   (`F1`-`F7` in the `B800` case) actually fail, to rule out a
   coincidence.

## 9. BASWORD: a separate, RAM-resident mechanism

BASWORD (a third-party tool, not a ROM feature) implements fully
dynamic, user-managed keyword registration by an entirely different
route: it **becomes the keyboard driver** rather than presenting a table
at one of the eight hardcoded pages.

- `ROM1.BIN` at `E243` contains `LDA (79D4); CPA 55; JR Z,+6D` — the
  stock ROM's *enhanced keyboard driver* path (auto-repeat, `DEF`-key
  combos, "`ENTER` compiles new keywords") is gated on this flag, which
  BASWORD sets via `POKE &79D4,&55` as part of installing itself.
  `&79FC`/`&79FD`/`&79FE` (BASWORD's own table start/end pointers) and
  `&785B` (the keyboard-hook vector, repointed at BASWORD's resident
  code) are read only by BASWORD's own code once installed — the stock
  ROM has no independent keyword-search hook here; BASWORD's table is
  walked entirely inside its own dispatcher, using the same §1 entry
  format by convention, not because the ROM requires it.
- **Compatibility gate**: `PEEK &E2B9` must return `56` for BASWORD (or
  anything relying on the same hook points) to work — how it detects a
  compatible ROM revision. `pc1500emu`'s `ROM1.BIN` returns `56`.

**Install sequence** (from BASWORD's own README, using its `BASWORD-40C5`
image — base `0x4000`, bare PC-1500 RAM, no ROM slot involved):

1. `CALL &E000` — triggers the ROM's `NEW0?:CHECK` prompt; dismiss with
   `CL`.
2. `NEW &4861` — reserves BASIC-program space above where BASWORD's
   4219-byte resident code will live. Requires extension RAM (a bare 2K
   machine can't accept this reserve boundary — `BASWORD-40C5` documents
   needing a CE-151 or PC-1500A, i.e. 4K+).
3. `CLOAD M` — loads BASWORD's binary as a raw memory image.
4. `NEW &4900` — reserves further space for BASWORD's own user-keyword
   table (grows as keywords are registered, 5 bytes + name length per
   entry).
5. Four `POKE`s wiring it up for this image/base: `POKE &79FC,&48,&48,&60`
   (table start/end pointers), `POKE &79D1,&24`, `POKE &785B,&45,&88`
   (keyboard-hook vector → BASWORD's dispatcher), `POKE &79D4,&55`
   (enhanced-driver-installed flag).

After install, `BASWORD +"NAME";"ccaaaap"` registers a new keyword live —
BASWORD's own resident driver appends a §1-format entry to its table and
updates `&79FD`/`&79FE`. Confirmed genuinely working end to end
(register, then successfully dispatch the new keyword) once
`pc1500emu`'s own timer/keyboard-timing were fixed to accurately model
the real hardware's keyboard-dispatch gate timing.

## 10. The "type" restriction (N / P / U)

BASWORD's registration command, `BASWORD +"name";"ccaaaap"`, takes a
trailing type letter:

- **N** — usable in RUN mode or while a program is running (like
  `PRINT`, `POKE`, `SIN`)
- **P** — programmable only, executes while a program is running (like
  `FOR`, `DATA`, `AREAD`)
- **U** — usable only in RUN or PRO mode directly, not while a program
  is running (like `NEW`, `LIST`, `CONT`)

This strongly maps to the table entry's marker high nibble (§1) — the
only per-entry field otherwise unaccounted for — but the exact nibble→
letter mapping is not pinned down, and §4's finding that the high nibble
is *not* checked by the table-walk itself (only the low/length nibble
is) means this restriction, if real, must be enforced somewhere other
than the walk/dispatch mechanism documented here.

Two data points from 2026-09-25 (`pc1500emu`, real `ROM1.BIN`):

- A table entry with marker `C5`/`C7`/`C4` runs fine both typed at the
  prompt and inside a running program (§12's tests), so a `C` high nibble
  is at least N-like.
- A likely enforcement site, **not traced**: `ROM1.BIN` `FB7A`–`FB9C`, a
  statement-dispatch check that masks a byte with `0x60`, consults `764FH`
  (the run/pro/reserve status byte) and raises `ERROR 26` (`UH=1AH`) or
  `ERROR 1`. Don't mistake an `ERROR 26` in testing for this: typing `RUN`
  while the machine is still in PRO mode (where it's left after entering
  program lines) raises `ERROR 26` by itself, before any program runs —
  press MODE first.

## 11. Boot-time peripheral init/self-check (separate from keyword dispatch)

A completely different mechanism from §§3-5's keystroke-time name/code
lookup: on every `RESET` (cold reset button, `Ctrl+F12` in `pc1500emu`, or
the `reset` FIFO command — none of which clear RAM, matching that
command's own documented behavior), BASIC's boot sequence calls into
*each* candidate page that has a valid `0x55` sentinel, once per page,
letting it run its own initialization/self-check before continuing.
Confirmed concretely two ways: (1) disassembly-traced end to end in
`ROM1.BIN`, and (2) reproduced live with the *real* `CE-150.ROM` cartridge
attached — with it, `RESET` doesn't reach the normal `NEW0?:CHECK` prompt
at all; the CPU jumps into and keeps executing CE-150's own code (its real
printer self-check, which polls for a printer response that never comes
with none attached — per Paul's own hardware knowledge, it eventually
times out and *does* reach `NEW0?:CHECK`, just much later than a normal
boot). A custom keyword module (§1-§7 of this document) that provides no
real routine at the entry point below will instead have the CPU execute
whatever garbage bytes happen to sit there (e.g. an all-zero reserved
header, or the keyword table's own data bytes past it) as if they were
code — wandering unpredictably before eventually stumbling into a stable
state, rather than either hanging (like CE-150 with no printer) or
returning cleanly.

**The call site** (`ROM1.BIN`, traced live via an in-process lockstep
CPU comparison — same machine, same post-`NEW0` RAM state, single-stepped
side by side with vs. without a module attached, to find the exact
instruction where the two runs first diverge):

```
E107: VMJ BE           ; -> E4A8 (see "the scan" below): find the FIRST
                        ;    candidate page with a valid sentinel
E109: BCR E120          ; no candidate found at all -- skip straight to
                        ;    normal boot continuation
E10B: PSH A             ; save A (=55H, the matched sentinel byte)
E10D: PSH X             ; save X (= matched candidate page : 00H)
E10F: LDI XL,0A         ; X = candidate page : 0AH -- the init entry point
E111: LDI UH,E1
E113: LDI UL,18         ; U = E118 -- the return address
E115: JMP E20E
  E20E: PSH U            ; push the return address (E118)
  E210: STX P            ; jump to X (candidate page + 0AH) -- this IS the
                          ;    call; the callee's own RTN pops U back here
E118: POP X              ; restore the saved candidate page:00H
E11A: POP A              ; restore the saved sentinel byte (55H)
E11C: VMJ BC            ; -> E4B7: resume the SAME scan loop mid-stream
                        ;    (NOT a fresh restart -- continues from the
                        ;    next lower candidate page) looking for
                        ;    ANOTHER installed module
E11E: BCS -15  (E10B)   ; another candidate found -- call ITS init entry
                        ;    too (loop back to E10B); else fall through
E120: ...                ; normal boot continues (eventually NEW0?:CHECK)
```

**The scan itself** (`E4A8`-`E4CD`, entered fresh via `VMJ BE`, resumed
mid-loop via `VMJ BC` for each subsequent candidate — this is a
*different* 8-page scan loop than §3's `FA44`-`FA73`, at different
addresses, with no confirmed relationship between the two beyond being
structurally similar (both walk the same eight 2KB-aligned candidate
pages, deriving `PV` per page) — not verified to be the same code reused,
so don't conflate them):

```
E4B0: LDI A,55          ; A = the sentinel byte to look for
E4B2: LDI XL,00
E4B4: CPA (X)            ; does candidate page:00H hold 55H?
E4B5: BZS E4CD           ; yes -- early-exit RTN, leaving Carry SET
                        ;    (the CPA comparison's own flag result --
                        ;    this is what E109/E11E's BCR/BCS test)
E4B7: LDA UH             ; no match -- continue scanning: UH holds a
                        ;    saved page-counter/limit value
E4B8: CPI XH,88
E4BA: BCR E4C2           ; all 8 pages exhausted, none matched
E4BC: LDA XH
E4BD: ADI A,F7            ; step to the next candidate page
E4BF: STA XH
E4C0: BCH E4B0            ; loop
E4C2: SHR / BCS E4C8 / SPV / BCH E4AD  ; exhausted-scan bookkeeping
E4C8: VMJ A0 / REC / SIE               ; REC explicitly clears Carry here
E4CD: RTN                              ; shared exit -- both paths land
                                        ;   here, Carry is the only
                                        ;   observable difference
```

**The convention, stated plainly:** a candidate page with a valid `0x55`
sentinel is expected to have real, executable code at **page-base + 0AH**
(10 bytes past the sentinel — comfortably inside the 32-byte reserved
header §1 already sets aside before the first-letter index). That code is
`STX P`-called (not `SJP`), with the return address already pushed onto
the stack by the caller — so the callee's own plain `RTN` is the complete,
correct way to return; no special register/flag convention is needed on
return (the caller immediately re-loads A/X itself at `E118`/`E11A` from
what *it* saved, not from anything the callee left behind).
`rom.asm`'s own fix for this: `BOOT_SELFCHECK_ENTRY` at `ROM_BASE+0AH` is
now a bare `RTN`, unconditionally reporting "self-check passed" — good
enough to stop `RESET` corrupting the boot sequence, not a real health
check. A future revision should replace it with genuine PSoC 5
communication, `RTN`-ing normally only once that check actually passes.

## 12. Arguments, statement end, and running inside programs

Everything in this section was confirmed on 2026-09-25 by tracing in
`pc1500emu` with the real `ROM1.BIN`, using the PC1500-PSOC5 expansion ROM
(page `8800`, PV low) — first with a probe keyword that recorded `Y`, then
with the full keyword set, whose regression suite now runs keywords both
at the prompt and inside programs. VEJ vector behaviour comes from reading
the vector routines in `ROM1.asm` together with how the CE-150's own
statements use them; the entries marked *inferred* were not traced.

### Where the arguments are

On entry to a dispatched routine, **`Y` points at the byte right after
the keyword's own 2-byte token**, in whatever text BASIC is executing:

- typed at the prompt: in `DISP_BUFFER`, so `Y = 7BB2H` (the token at
  `7BB0H`–`7BB1H`);
- in a running program: in the program line itself — e.g. `10 ECVER:A=7`
  is stored at `40C5H` as `00 0A 07 E1 95 3A 41 3D 37 0D` and the routine
  was entered with `Y = 40CAH`, pointing at the `:`.

So the keyword's own code is always at `Y-2`/`Y-1`, and a routine that
reads its arguments from `DISP_BUFFER` (as the PC1500-PSOC5 ROM did until
this finding) only works for typed commands.

### What the argument text looks like

It is BASIC's stored form, not what was typed:

- **Every space outside quotes is removed.** `ECVER F$ AS 1` is stored as
  `E1 95 'F$AS1'`; `ECVER A * 2 , 3` as `E1 95 'A*2,3'`; quoted text keeps
  its spaces (`"A B"`).
- **BASIC's own keywords are tokenized inside the arguments**, even as
  part of a longer word: `MCONF SLEEPWAIT=1000` is stored as `E1 9A
  'SLEEP' F1 B3 '=1000'` (`F1B3` = `WAIT`), and `SDOPEN F$ AS C` would get
  `ASC` (`F160`). The CE-150's keywords are tokenized too when it's
  attached. Quoted text is never tokenized.
- A statement ends at `:` or `0DH`. At the prompt only one statement is
  allowed: BASIC's own `E=1:D=3` typed at the prompt raises `ERROR 1`.

A keyword whose arguments are words (not expressions) must therefore
match them against this form — or expand tokens back first (the
PC1500-PSOC5 firmware carries the base ROM's and CE-150's keyword tables
for that).

### Ending the statement: `VEJ E2`

The routine should finish with **`VEJ E2`** (vector `FFE2` → `C400H`),
which is how the base ROM's statements and the CE-150's (`CSIZE`,
`COLOR`, ...) end, **with `Y` at the end of its statement** (the `:` or
`0DH`). `C400H` fetches the next element (`VEJ C0`) and checks whether a
program line is executing (`VEJ D8`):

- running a program: it carries on with the next statement or line;
- at the prompt: `0DH` completes the command normally (`VZS 42`), anything
  else raises `ERROR 1`.

Confirmed both ways: `10 ECVER:A=7` / `20 B=8` ran to completion with
`A=7`, `B=8`; typed `ECVER 7` raised `ERROR 1` at the stray `7`. It also
replaces §6's whole prompt-only tail: with every keyword of the
PC1500-PSOC5 ROM ending in `VEJ E2` instead of `POP A` / `784EH` / `764EH`
/ `7874H` / `7880H` fixes and a jump to `E2AA`, its entire regression
suite still passes — including the tests written for §6's "next keypress"
bugs (typing a new command straight after a keyword, a stray Enter after
one). No fix-ups needed.

### The statement-parsing vectors

`VEJ n` jumps through the vector at `FF00H+n`; some take inline operand
bytes after the opcode, and an inline `e` is a forward branch offset,
**relative to the byte after `e`** (checked on the CE-150's `LB10D`:
`DE 05 D0 10 02 F9 9A FB 9A` — both offsets land on the `FB 9A`, `SEC;
RTN`).

| Vector | Routine | What it does |
|---|---|---|
| `VEJ C0` | `DD08` | Next element at `(Y)` into `U`: a token (`>= E0H`) as `UH:UL`, a character as `UH=00`, `UL=char`; `A = UL`; advances `Y`. |
| `VEJ C2 c,e` | `DCD4` | `VEJ C0`, then if it isn't character `c`, branch `+e`. (`POKE` uses `C2 2C e` for its `,`.) |
| `VEJ C4 hh,ll,e` | `DCD5` | *Inferred*: the same for a token `hhll` (the routine reads a second inline byte when the first is `>= E0H`). |
| `VEJ C6` | `DD13` | Step `Y` back over the element just fetched. |
| `VEJ C8 e` | `DCC5` | Next element; at the end of the statement (`0DH`, or `:` in a program) fall through, otherwise branch `+e`. (`CURSOR`'s no-argument case is the fall-through.) |
| `VEJ D8` | `DF3B` | `Z` set if the current line number (`789CH`/`789DH`) is 0 — not executing a program line. |
| `VEJ DC` | `DEBC` | `X` = string address (`7A05H`/`7A06H`), `UL` = length (`7A07H`) of a string in the arithmetic register. |
| `VEJ DE e` | `D6DF` | Evaluate the expression at `(Y)` into the arithmetic register X (`7A00H`), leaving `Y` just past it; on an error, branch `+e` with the error number in `UH` (callers then `VEJ E0`, as the CE-150 does with `VCS E0`). |
| `VEJ D0 n,e` | `D5F9` | *Inferred*: convert the arithmetic register to an integer; `n` selects the range (`POKE`: `00` for the address, `08` for the byte; `CSIZE`/`CURSOR`: `10`), error branch `+e`; the result ends up in `A`/`UL`. |
| `VEJ E0` | `CD8B` | Raise `ERROR UH`. |
| `VEJ E2` | `C400` | End of statement (above). |
| `VEJ E4` | `CD89` | Raise `ERROR 1`. |

`VEJ DE` in practice (confirmed, via the PC1500-PSOC5 ROM's `KW_EVAL`):
`2*500`, `A*2`, `"X"+"Y"`, `F$` and `&4000` all evaluate, and the
evaluator stops at `,`, `:`, `0DH` and BASIC's own separators. It does
**not** stop at a word BASIC doesn't know: `"X"AS1` (from `SDOPEN "X" AS
1`) raises `ERROR 1` inside the evaluator — an argument followed by a
non-BASIC word has to be parsed some other way (the firmware reads plain
quoted and numeric literals itself). A string result from a string
variable points at the variable's own storage; the firmware stops such a
string at its first `00H` byte, which is safe whether or not the length
includes the variable's zero padding (not traced).

## 13. Keywords as functions (`S=BLSTAT`, `PRINT BLSTAT`)

Found 2026-09-29, while making the PC1500-PSOC5 expansion module's
`BLSTAT` usable the way BASIC's own `MEM` is.

**BASIC's functions are ordinary keyword-table entries** (§1), in the same
table and format as its statements. What makes one a function is **the
low byte of its code, not its marker byte.** From `ROM1.BIN`'s own table:

| Keyword | Kind | Marker | Code | Address |
|---|---|---|---|---|
| `MEM` | function, no argument | `D3` | `F158` | `DA5D` |
| `TIME` | function, no argument | `A4` | `F15B` | `DE82` |
| `INKEY$` | function, no argument | `A6` | `F15C` | `D9AA` |
| `PI` | function, no argument | `C2` | `F15D` | `F5B5` |
| `ASC` | function, one argument | `A3` | `F160` | `D9DD` |
| `CHR$` | function, one argument | `A4` | `F163` | `D9B1` |
| `PEEK` | function, one argument | `A4` | `F16F` | `D993` |
| `SIN` | function, one argument | `A3` | `F17D` | `F3A2` |
| `COS` | function, one argument | `C3` | `F17E` | `F391` |
| `CLS` | statement | `C3` | `F088` | `E865` |
| `INPUT` | statement | `D5` | `F091` | `C8FA` |
| `PRINT` | statement | `B5` | `F097` | `E4EB` |
| `BEEP` | statement | `B4` | `F182` | `E5C1` |

The same marker (`C3`) serves `COS` and `CLS`, and `A3`/`A4` both kinds.
The low byte is what the expression evaluator branches on (`LD89D`-`LD8CF`,
after `VEJ C0` has fetched the token into `U`):

- `UL >= 80H` (`cpi ul,0x80`): a statement. It ends the expression.
- `UL >= 60H` (`cpi ul,0x60`, `LD8C6`): a function taking an argument.
- `UL = 50H`/`51H` with `UH = F1H` (`LD8B9`): `AND`/`OR`, operators. With
  any other high byte they're treated like the rest of `5xH`.
- Otherwise (`5xH`, `LD8CF`): a function with no argument. `LD87C` saves
  `Y`, and for a token `>= E0H` goes through `VMJ 1C`, then back at `LD8F2`
  restores `Y`/`X` and does `LDA UH / VZR 48` (a non-zero `UH` is an
  error). *Inferred from the code:* that `VMJ 1C` looks the token up
  (module tables included) and calls its routine. The module test below
  shows the routine does get called.

**The routine's convention** is `MEM`'s (`LDA5D`), after it works out its
value:
1. The value goes in the arithmetic register X, `7A00H`-`7A07H`, in the
   same 8-byte format a BASIC number variable is stored in.
   - `MEM` builds it with `VMJ 10` (`LDD2D`: an integer in `U` to BCD at
     `7A00H`, with the sign as `80H` in `7A01H`).
   - A module can instead just copy 8 bytes it already has.
2. `LDI UH,00H` / `RTN` (`LDA41`).
   - A non-zero `UH` is taken as an error number (`VZR 48` at `LD8F2`).
     *Read from the code, not tested.*

**Confirmed for a module keyword, in `pc1500emu`:**
- The setup: page `8800H`, PV low, code `E152`, routine `BLSTAT_FN`.
- Its routine asks the module's MCU for the number, copies the 8 bytes to
  `7A00H`, and returns with `UH = 0`. It pushes/pops `X` and `Y` around
  that work; whether that's needed isn't tested.
- All of these work:
  - `S=BLSTAT` and `S=BLSTAT*10+1`;
  - the function evaluated inside another module keyword's argument
    (`BLPRINT BLSTAT`, i.e. from within that keyword's own `VEJ DE`);
  - in a running program: `10 T=0:IF BLSTAT>0 THEN LET T=BLSTAT+4`;
  - `BLSTAT` typed alone at the prompt (no error; shown like `MEM`, on the
    LCD, which the test couldn't read).
- Nothing about the entry itself differs from a statement: the §4
  name-walk finds it by name like any other keyword.

Two things a function routine has to allow for, which a statement's
don't:
- **It runs in the middle of an expression**, and possibly in the middle
  of another keyword's statement. So it must not end with `VEJ E2`, and
  must leave BASIC's own state (the statement pointer, the arithmetic
  register's other operands) as it found it.
- **For the PC1500-PSOC5 module specifically:** its "keyword finished"
  command (`DONE`) lets the MCU go DORMANT at once in STAGE RAM mode, and
  a sleeping MCU's window reads `00H`. So:
  - A function sends `DONE` only when no keyword is running, which the
    MCU's reply says (inside `BLPRINT BLSTAT`, `BLPRINT`'s own `DONE`
    comes later).
  - It sends it only **after** reading its value.
  - A first version had the MCU treat the function's command as the end
    by itself. The MCU fell asleep before the ROM had read the status and
    the value, giving `ERROR 1` in STAGE RAM mode only. Confirmed on
    hardware 2026-09-30, where `MCONF SLEEPWAIT=1000` hid it. Fixed as
    described above, and confirmed working.

Typed at the prompt, `IF ... LET` without `THEN` is `ERROR 1` for `MEM`
just as for a module function: assigning after `IF` needs `THEN LET`.

**Functions with an argument (`6xH`-`7xH`), found 2026-09-30 with
`SDEOF(n)`:**
- **The argument arrives already evaluated,** in the arithmetic register X
  (`7A00H`). The evaluator has done the `(...)` itself before calling the
  routine. `PEEK` (`D993`) and `CHR$` (`D9B1`) both start with `VEJ D0`,
  which converts the register to an integer; `ASC`/`LEN` (`D9DD`) start
  with `VEJ DC`, which gets a string's address and length from it.
- **The result is returned the same way:** in the register, then `UH = 0`.
  `ASC`/`LEN`/`PEEK` finish at `LD9E7` with a *binary* integer rather than
  BCD: `B2H` in `7A04H` (the binary-integer marker) and the 16-bit value in
  `7A05H`/`7A06H`. The BCD form (as `MEM`) works too.
- **Confirmed for a module in `pc1500emu`:** `SDEOF` (code `E170`, `ABS`'s
  low byte).
  - Its routine doesn't convert the argument itself: it hands the
    register's 8 bytes to the module's MCU, which reads the number and
    replies with the result.
  - These all work:
    - `BLPRINT SDEOF(1)` (inside another keyword);
    - `E=SDEOF(C)+5` (a variable as the argument, inside an expression);
    - a read loop in a running program, `20 IF SDEOF(1) THEN 50`.
  - **A non-zero `UH` on return raises that error:** `SDEOF(2)` on a
    channel that isn't open returns `UH = 40` and BASIC reports ERROR 40;
    `UH = 1` gives ERROR 1. That confirms what was inferred above.
- Not yet tried: a *string* argument (`VEJ DC` suggests how), a string
  result (`INKEY$`/`CHR$` build one in the register with `D0H` in `7A04H`),
  or more than one argument.

## Open questions

- **PV-high code values for six of the eight pages** (`8800, 9000,
  9800, A000, A800, B000`) are unknown — no real ROM content sampled
  here provides an example, and they haven't been brute-forced. §8
  describes the method to find them if needed.
- **Whether §11's boot-time scan (`E4A8`-`E4CD`) and §3's keystroke-time
  scan (`FA44`-`FA73`) are the same underlying routine at different
  addresses (different ROM revision?) or two genuinely separate,
  structurally-similar implementations** is not resolved — they were
  traced independently, in different sessions, against what should be
  the same `ROM1.BIN`, and the addresses don't match.
- ~~Why does the code value need to match a specific page identity at
  all, for the name-based table-walk (§4)~~ — **partially resolved.**
  The code field's *high byte* matters during the name-walk not because
  it's checked against a specific page identity, but because the §4
  skip-scan uses the literal constant `0xE0` as a generic "is this the
  code field yet" threshold while hunting for the next entry — any value
  `>0xE0` works structurally regardless of which page it's "supposed" to
  belong to (this is why a wrong-but-still->0xE0 code value doesn't
  break the *walk*, only the later execution-time re-dispatch in §5).
  What's still open: whether the walk cares about the code value being
  correct *for its page* at all, versus just needing to clear `0xE0` —
  not yet isolated from the `8000`-boundary/marker-bit4 findings above.
- The marker byte's high-nibble → N/P/U mapping (§10) — would need
  either a source stating it explicitly, or a live test registering the
  same keyword under BASWORD with different type letters and diffing
  the resulting table bytes.
- ~~Argument passing (§6)~~ — **resolved in §12**: `Y` points at the
  statement text, and the routine parses and evaluates its arguments
  itself.
- `VEJ D0`'s range selector and result registers, and `VEJ C4`'s token
  compare (§12) are inferred from code and usage, not traced.
- Module functions (§13):
  - One numeric argument (`6xH`/`7xH`) works: it arrives evaluated in the
    arithmetic register (§13, `SDEOF`). String arguments, string results
    and several arguments are untried.
  - A no-argument module function (`BLSTAT`) works on a real PC-1500 too
    (2026-09-30), at the prompt and with `PRINT`/assignment.
  - `VMJ 1C`'s part in the lookup is inferred. (The error-on-non-zero-`UH`
    convention is confirmed now, with `SDEOF`.)
- The text of a keyword running inside a program is read straight from
  the program line; whether `Y` ever points elsewhere (e.g. `AREAD`,
  reserve-key execution, `DEF` keys) is untested.
- BASWORD's documented fourth table, "Cassette interface" (`/C`) —
  address/PV binding not investigated.
- The base ROM's `CPI A,0x3C` compare in the candidate-check subroutine
  (`FA58`) — purpose not determined; never observed to take its branch.
- A real-hardware-vs-emulator discrepancy: typing `LPRINT` with no
  module connected reportedly gives `ERROR 1` on real hardware, `ERROR
  15` in `pc1500emu` — doesn't affect the scan-loop structure (confirmed
  independently via trace) but the code-value mismatch itself is
  unexplained.
