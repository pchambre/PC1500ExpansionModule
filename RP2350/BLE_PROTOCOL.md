# PC-1500 Link: the BLE protocol

The protocol between a PC-1500 expansion board and anything it talks to over
Bluetooth Low Energy: the feature-server app (`ble_app/`), another PC-1500,
or the emulator. The firmware (`ble_link`), the app and the emulator all
implement this document. The requirements are in the design Google Doc,
`1JsBLb6IfurR44339UCfs4avSaGKE_G_6TDKPcVkkZ6s`.

Version 2, 2026-10-03: every link is authenticated and encrypted (sec.7,
"Security"); a version-1 peer is refused. Version 1 was 2026-09-27.

Milestone 1 uses `HELLO`, `TEXT`, the file messages,
`BYE`, `ACK` and `ERR`. Milestone 2 (2026-09-28) adds peer-to-peer files
between two PC-1500s: `FILE_OFFER` and `FILE_ANSWER` (sec.5, "Peer-to-peer
files"), then `MSG` (2026-09-29, "Peer messaging"). They're new message
types, not a new version: a side that doesn't know them answers
`ERR UNSUPPORTED`.

## 1. Roles

Both ends of a link are **peers** speaking the same messages. The BLE roles
decide only who finds whom:

- **Advertiser (peripheral, GATT server):** advertises the Link service and
  hosts it. This is the feature-server app, and a PC-1500 that has run `BLADV`
  (peer-to-peer, milestone 2), which waits for another PC-1500 to connect
  with `BLSCAN`/`BLCON`.
- **Connector (central, GATT client):** scans, connects, and subscribes. This
  is a PC-1500 running `BLSCAN`/`BLCON`.

So a PC-1500 always starts things: the user just leaves the app running, and
everything else is driven from the PC-1500. Every PC-1500 can take either
role, since for peer-to-peer one of two PC-1500s must advertise.

## 2. GATT service

| Item | UUID | Properties |
|---|---|---|
| Link service | `c31f0001-92a3-40ab-b63d-7cdb0a37aed0` | primary |
| RX | `c31f0002-92a3-40ab-b63d-7cdb0a37aed0` | write without response (write also allowed) |
| TX | `c31f0003-92a3-40ab-b63d-7cdb0a37aed0` | notify |

- The connector sends by writing RX; the advertiser sends by notifying TX.
  Everything above that is the same in both directions.
- No BLE pairing or bonding: the Link authenticates and encrypts its own
  frames (sec.7), the same over every radio and host.
- The connector requests an ATT MTU of 247 and subscribes to TX before
  sending anything. The link needs an ATT MTU of at least 64: at less, the
  connector disconnects and reports an error.
- **One frame per ATT packet:** a frame is at most `MTU − 3` bytes, so its
  payload is at most `MTU − 7`. Frames are never split across packets; long
  data goes in several frames (`TEXT`, `FILE_DATA`).

## 3. Finding a peer

- A connector scans **actively** (scan requests on), because many advertisers
  put their name and the 128-bit service UUID in the scan response rather
  than the advertisement itself.
- A peer is anything whose advertisement or scan response lists the Link
  service UUID. **Match on the UUID, never on the name:** Windows advertises
  under the computer's name (not the name the app asks for), and may
  advertise twice, once with no name.
- For display before connecting, use the advertised local name, or the
  address if there is none. The peer's real name comes in its `HELLO`.

## 4. Frames

```
offset  size  field
0       1     type
1       1     seq
2       2     len   (payload length, little-endian)
4       len   payload
```

- **seq:** each side numbers the frames it sends: 0, 1, 2, … wrapping at
  255, independently in each direction. `ACK` and `ERR` don't take a number
  of their own: their `seq` is the number of the frame they answer.
- **Every frame except `ACK` and `ERR` is answered** with an `ACK` or an
  `ERR`. A side sends nothing else until the answer arrives: **one frame in
  flight per direction.** This keeps both ends' buffers to one frame, and
  the order of events is always clear.
- **Timeout:** no answer within 5 s means the link has failed. The side that
  noticed disconnects.
- A frame whose `len` doesn't match the packet's length, or whose type is
  unknown, is answered with `ERR BAD_FRAME` or `ERR UNSUPPORTED`. The link
  stays up.

## 5. Messages

Strings are ASCII, preceded by a 1-byte length, with no terminator (`str8`
below). Multi-byte numbers are little-endian.

| Type | Name | Payload |
|---|---|---|
| 0x01 | `HELLO` | `version` u8 (= 2), `kind` u8 (1 PC-1500, 2 server), `name` str8, `id` 8 bytes, `nonce` 16 bytes; the advertiser's adds `known` u8 and, if 1, `proof` 16 bytes (sec.7) |
| 0x03 | `AUTH` | `proof` 16 bytes (sec.7) |
| 0x04 | `PAIR_START` | `pk` 32 bytes (sec.7) |
| 0x05 | `PAIR_NONCE` | `nonce` 16 bytes |
| 0x06 | `PAIR_CONFIRM` | `ok` u8, `mac` 16 bytes |
| 0x02 | `BYE` | none |
| 0x10 | `TEXT` | `channel` u8, then text bytes to the end of the frame |
| 0x20 | `FILE_PUT` | `target` u8, `kind` u8, `flags` u8, `size` u32, `name` str8 |
| 0x21 | `FILE_DATA` | file bytes to the end of the frame |
| 0x22 | `FILE_END` | none |
| 0x23 | `FILE_GET` | `target` u8, `name` str8 |
| 0x24 | `FILE_ABORT` | none |
| 0x25 | `FILE_OFFER` | `kind` u8, `size` u32, `name` str8 (may be empty) |
| 0x26 | `FILE_ANSWER` | `accept` u8 (1 yes, 0 no) |
| 0x30 | `MSG` | value chunks to the end of the frame (peer messaging) |
| 0x40 | `PLOT` | plotter operations to the end of the frame (CE-150 emulation) |
| 0x7E | `ACK` | none |
| 0x7F | `ERR` | `code` u8, then an optional ASCII message |

Reserved type ranges: 0x30–0x3F peer messaging (P2P), 0x40–0x5F server
features (web, plotter). An implementation answers types it doesn't know
with `ERR UNSUPPORTED`.

### Connecting: `HELLO`

The connector sends `HELLO` as soon as it has subscribed to TX. The
advertiser `ACK`s it and sends its own `HELLO`, which the connector `ACK`s.
Until then neither side sends anything else. A `version` the receiver doesn't
support is answered `ERR UNSUPPORTED`, and the connector disconnects. The
`HELLO`s also start the authentication: sec.7.

### Ending: `BYE`

`BYE` is `ACK`ed, and then the connector disconnects. A dropped link needs no
`BYE`: whatever was in progress fails with an error on the PC-1500.

### Text: `TEXT`

- `channel` 0 is the console: `BLPRINT` and `BLLIST` output, shown by the app
  in its text window. Other channel numbers are reserved (P2P messaging).
- Lines end with CR (0x0D), as on the PC-1500. Receivers display CR as a new
  line.
- FF (0x0C, form feed) clears the console; text after it starts at the top.
  `BLCLS` sends it, and so does `BLPRINT CHR$(12);`.
- Characters are the PC-1500's: ASCII for 0x20–0x7E; others are passed
  through, and the app shows them however it chooses.
- A long output is sent as several `TEXT` frames; frame boundaries mean
  nothing.

### Files

A file's bytes are exactly what `SDSAVE` writes to the SD card, so files move
between the SD card and the app unchanged:
- `kind` 0 (BASIC): BASIC's tokenized program, as it is in RAM.
- `kind` 1 (M): the 4-byte header `[load hi][load lo][call hi][call lo]`
  (call 0 = none), then the memory bytes.
- `kind` 0xFF: unknown (a server returning a file whose kind it didn't
  record). The loading keyword decides from its own flags, as `SDLOAD` does.

`name`: up to 40 characters, the same rule as SD names.

`target` names what the file is written to or read from **on the receiving
side of the request**:

| target | meaning |
|---|---|
| 0 | the server's file store (the app's folder) |
| 1, 2 | reserved (once meant for peer-to-peer; that uses `FILE_OFFER` instead, where the receiver chooses) |

`flags` bit 0: overwrite an existing file (otherwise `ERR EXISTS`).

**Saving** (`BLSAVE`): the PC-1500 sends `FILE_PUT`, then any number of
`FILE_DATA`, then `FILE_END`. Each frame is `ACK`ed:
- `FILE_PUT`'s `ACK` means "accepted, go ahead". An `ERR` (`EXISTS`, `IO`,
  `BUSY`, `UNSUPPORTED` for a target it doesn't have) ends the transfer.
- `FILE_END`'s `ACK` means the file is completely written. The receiver
  checks that it got exactly `size` bytes, else `ERR IO`. `size`
  0xFFFFFFFF means "not known in advance" (a BASIC save streamed by the ROM),
  and there's no check.
- The sender may give up part-way with `FILE_ABORT` (`ACK`ed); the receiver
  throws the partial file away. The receiver may give up instead by
  answering a `FILE_DATA` with `ERR ABORTED` (e.g. no room left); the sender
  then stops.

**Loading** (`BLLOAD`): the PC-1500 sends `FILE_GET`. The other side `ACK`s
it and becomes the sender: `FILE_PUT` (whose `target`, `flags` are 0),
`FILE_DATA`…, `FILE_END`, with the PC-1500 `ACK`ing each frame. Or it
answers `FILE_GET` with `ERR NOT_FOUND`.

### Peer-to-peer files: `FILE_OFFER`, `FILE_ANSWER`

Between two PC-1500s (milestone 2) the **receiver** decides where a file
goes, and a person runs a keyword on each side, in either order: `BLPUT`
sends, `BLGET` receives. So a transfer starts with an offer, which may wait
a long time for its answer:

1. The sender sends `FILE_OFFER`: the file's `kind` (as above: 0 BASIC,
   1 M), its `size` (0xFFFFFFFF if not known in advance) and a `name`, for
   information only (a program in memory has none, so it may be empty).
2. The receiving side `ACK`s it **at once**, whether or not anyone there is
   running `BLGET` yet: the `ACK` means "held", not "accepted". It answers
   `ERR BUSY` if it already holds an offer or is in a transfer, and a side
   with no peer-to-peer support (the feature-server app) answers
   `ERR UNSUPPORTED`.
3. When `BLGET` takes the offer, the receiver sends `FILE_ANSWER` (1 to
   accept, 0 to refuse -- e.g. it couldn't create the file). The sender
   `ACK`s it. There's no time limit between steps 2 and 3: nothing is in
   flight while an offer is held.
4. After `ACK`ing an accepting `FILE_ANSWER`, the sender sends `FILE_DATA`…
   `FILE_END`, each `ACK`ed by the receiver, exactly as in saving above
   (and the receiver may stop it with `ERR ABORTED`).

The sender may withdraw a held offer with `FILE_ABORT` (`ACK`ed), e.g. on
BREAK; the receiver drops it. A `FILE_ABORT` that crosses an accepting
`FILE_ANSWER` reaches the receiver where it expects `FILE_DATA`, and ends
the transfer there. A dropped link drops any held offer.

Either side may offer, whichever BLE role it has, and each side holds at
most one offer at a time. Where the file goes is the receiver's business:
`BLGET "name"` saves it on its SD card, a bare `BLGET` loads it into memory
(a BASIC file as the program, an M file at its header's load address).

### Peer messaging: `MSG`

Short messages between two PC-1500s' BASIC programs (milestone 2, 2026-09-29):
text, or a game's moves. A message is the values of one `BLSEND`, as value
chunks, the format `SDPRINT#` writes to a file:
- a number: `'N'` (0x4E), then its 8 bytes as BASIC stores them (packed-BCD
  float, TRM sec.5-3-1);
- a string: `'S'` (0x53), its length u8, then its characters.

A `MSG` holds at least one chunk and fits one frame. The receiving side keeps
up to 8 messages, oldest first, until its `BLRECV` takes them:
- it `ACK`s a `MSG` once it's stored, whether or not anyone is running
  `BLRECV`;
- with 8 already waiting it answers `ERR BUSY`, and the sender tries again
  later (`BLSEND` waits, and BREAK stops it);
- a malformed `MSG` (a bad chunk, or one running past the end) is
  `ERR BAD_FRAME`.

A new link empties the inbox; a dropped one doesn't, so a program can still
read what arrived. Either side may send, whichever BLE role it has.

### Plotter: `PLOT` (2026-09-30)

The expansion module can stand in for a Sharp CE-150 printer/plotter. It
answers the CE-150's BASIC commands (`LPRINT`, `LLIST`, `LINE`, `RLINE`,
`GLCURSOR`, `COLOR`, `CSIZE`, `ROTATE`, `GRAPH`, `TEXT`, ...) when no real
CE-150 is attached, and draws on the connected app instead of on paper.

**The PC-1500 sends pen movements, never text or shapes.** Its MCU does
everything a CE-150 does:
- It turns characters into pen strokes (Hershey Roman Simplex, scaled to the
  CE-150's nine `CSIZE` cells and four `ROTATE` directions).
- It turns dashed line types into short strokes.
- It tracks the BASIC coordinate origin (`SORGN`) and TEXT/GRAPH mode.
- It lifts the pen outside the paper, as a CE-150 does.

So the receiver only draws lines, and can print what it drew as-is.

**Units are quarter steps, 0.05 mm.** The CE-150's own plotter step is
0.2 mm (PC-2 service manual, p.39). BASIC's coordinates and the line
drawing stay on whole steps, i.e. multiples of 4. Lettering uses the finer
grid, since a stroke font on a 4 x 6-step `CSIZE 1` cell would be mush.
- **X:** 0-860 across the 43.2 mm plotting width (216 steps, 0-215; the 58
  mm roll has 5 mm left and 9.8 mm right margins outside it). 0 is the left
  edge. The pen never goes outside it.
- **Y:** the position along the paper. Positive is *up* the paper (towards
  what was printed earlier), as on the CE-150, so later output usually has
  smaller Y. Paper feed is simply the pen's Y changing. Its zero is
  arbitrary (where the PC-1500's module started); a receiver places its
  roll relative to the first Y it's sent. Absolute Y is 32-bit, because an
  i16 would only cover 1.6 m of roll.
- The relative ops (i8, ±127 quarter steps = ±6.35 mm) carry most
  lettering strokes in 3 bytes instead of 7.

**Receiver state:** where the pen is, and which pen. Each `PLOT` sets both
first, so a frame needs nothing from earlier ones (a reconnect, or a frame
lost to a dropped link, doesn't throw later drawing off). The receiver keeps
one continuous roll, growing it to include every position the pen reaches;
clearing or saving it is the app's business.

A `PLOT` payload is the pen's state, then a sequence of operations, as many
as fit the frame; multi-byte numbers are little-endian and signed:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `pen` u8 | the pen (0-3) |
| 1 | `x` i16 | where the pen is, up |
| 3 | `y` i32 | |
| 7 | operations | below, to the end of the frame (there may be none: then the frame just says where the pen now rests, e.g. after a paper feed) |

| Op | Name | Operands | Meaning |
|---|---|---|---|
| 0x01 | `MOVE` | `x` i16, `y` i32 | pen up, go to (x, y) |
| 0x02 | `DRAW` | `x` i16, `y` i32 | pen down, draw a line to (x, y) |
| 0x03 | `MOVE_REL` | `dx` i8, `dy` i8 | pen up, move by (dx, dy) |
| 0x04 | `DRAW_REL` | `dx` i8, `dy` i8 | pen down, draw by (dx, dy) |
| 0x05 | `PEN` | `pen` u8 (0-3) | change pen; the receiver maps pens to colours (default: 0 black, 1 blue, 2 green, 3 red, the colours the CE-150 ships with) |

- Operations apply in order. A frame never ends in the middle of one.
  Anything malformed or unknown is `ERR BAD_FRAME` for the whole frame,
  which is then ignored.
- **Each `PLOT` is `ACK`ed once drawn** (one frame in flight, sec.4). A BASIC
  statement returns only after its frames are `ACK`ed, so drawing keeps pace
  with the program, as it would on paper. Each statement's `PLOT`s end with
  the pen where it rests; a statement that only moves the pen (`GLCURSOR`,
  `LF`, a pen-up `LINE`) sends one with no operations, and one that changes
  nothing on paper (`CSIZE`, `COLOR` in GRAPH mode) sends none.
- A side that can't draw (a PC-1500 peer) answers `ERR UNSUPPORTED`. The
  keyword then raises `ERROR 27`, which is also what it raises with no link:
  the PC-1500's own "printer not connected".
- The firmware's `plotter.c` has a portable C decoder (`plot_decode()`) and
  the frame splitter; pc1500emu uses both (its printer panel, `PlotPaper`).
  The laptop app's decoder is `ble_app/lib/plot.dart`.

*Open:*
- whether one `PLOT` per statement is fast enough for drawing-heavy programs
  (e.g. GLOBE's thousands of short `LINE`s), or the MCU should gather
  several statements' operations and send them after a short pause;
- whether a "new sheet"/"cut" operation is wanted.

### Errors: `ERR`

| code | name | meaning |
|---|---|---|
| 1 | `BAD_FRAME` | malformed frame |
| 2 | `UNSUPPORTED` | unknown type, version or target |
| 3 | `NOT_FOUND` | no such file |
| 4 | `EXISTS` | file exists and overwrite wasn't requested |
| 5 | `IO` | couldn't read or write, or the size was wrong |
| 6 | `BUSY` | already in a transfer |
| 7 | `ABORTED` | the receiver gave up on the transfer (answering `FILE_DATA`) |
| 8 | `NOT_PAIRED` | the link isn't authenticated: pair first (sec.7) |
| 9 | `AUTH_FAILED` | a proof or confirmation didn't check out |

## 6. Throughput

One frame in flight per direction means one frame per round trip, which is
two to three connection intervals. At a 30 ms interval and a 247-byte MTU
that's roughly 3 KB/s: a 10 KB program takes a few seconds. That's fine for
milestone 1. If it isn't later, the fix is a window of several frames in
flight, not a different framing.

## 7. Security (version 2, 2026-10-03)

The advertiser -- the laptop app above all -- reads and writes files on its
host, and a PC-1500 runs what it loads (`BLGET` of an M file can `CALL` it).
So both ends must know who they're talking to, and nobody else should read
or change what passes. BLE's own pairing can't be relied on for that: what
it offers differs between radios (the Pico 2 W's, an RN4871) and hosts
(Windows, macOS, Linux, iOS, Android), and a GATT server app often can't
control it. So the Link does it itself, the same everywhere:

- **Once per pair of devices, pairing:** an X25519 key exchange, confirmed by
  the people at both ends comparing a six-digit code (as BLE's "numeric
  comparison"). It leaves both sides holding a 32-byte long-term key.
- **Every link:** each side proves it holds that key, and every frame after
  that is encrypted and authenticated (ChaCha20-Poly1305).

### Identities and keys

- Every device has an `id`: 8 random bytes, made once and kept. Pairings are
  found by it (BLE addresses change).
- A pairing record holds the peer's `id`, its name (from its `HELLO`), and
  the long-term key `ltk`. Devices keep them in protected storage: the MCU's
  flash; the app's platform secure storage (Windows DPAPI, the macOS/iOS
  Keychain, the Android Keystore, libsecret on Linux), not synced or backed
  up; the emulator's own file, DPAPI-protected on Windows.
- Either side can forget a pairing; the other then finds it no longer
  authenticates (`ERR AUTH_FAILED` or `NOT_PAIRED`), and the two pair again.

### Starting a link

1. The connector's `HELLO` carries its `id` and a fresh random `nonce_c`.
2. The advertiser `ACK`s it, and looks for a pairing with that `id`. Its own
   `HELLO` carries its `id`, a fresh `nonce_s`, `known` (1 if it found one)
   and, if so, `proof_s` = `auth("S")`.
3. The connector `ACK`s that, then looks for a pairing with the advertiser's
   `id`. If both sides have one and `proof_s` checks out, it sends `AUTH`
   with `proof_c` = `auth("C")`. The advertiser checks it: `ACK`, or
   `ERR AUTH_FAILED` (the connector disconnects).
4. **The `ACK` of `AUTH` is the last frame in the clear.** After it, both
   sides seal every frame, `ACK`s and `ERR`s included (below).

If either side has no pairing, the link stays up but **unpaired**: either
side answers anything but `HELLO`, `BYE` and the pairing frames with
`ERR NOT_PAIRED`. A PC-1500 reports it (`BLCON`: "NOT PAIRED"), and `BLPAIR`
pairs over the same link.

`auth(role)` = the first 16 bytes of HMAC-SHA512(`ltk`, `"PC1500 auth "` |
`role` | `nonce_c` | `nonce_s` | `id_c` | `id_s`), where `role` is the
character `C` or `S` and `|` is concatenation. The session keys are
HKDF-SHA512(`ltk`, salt `nonce_c` | `nonce_s`, info `"PC1500 session"`), 64
bytes: the first 32 encrypt what the connector sends, the rest what the
advertiser sends. Strings are ASCII, without terminators.

### Pairing

The connector starts it (a PC-1500's `BLPAIR`) on an unpaired link. The
advertiser only answers: its `ACK`s carry its side's data, the one place an
`ACK` has a payload.

1. `PAIR_START` (`pk_c`, the connector's fresh X25519 public key). The
   advertiser makes its own key pair and a random `n_s`, and `ACK`s with
   `pk_s` (32) | `commit` (16), `commit` = the first 16 bytes of
   SHA-512(`"PC1500 commit"` | `pk_s` | `pk_c` | `n_s`).
2. `PAIR_NONCE` (`n_c`, 16 random bytes). The advertiser `ACK`s with `n_s`,
   and the connector checks it against `commit`: committing first means a
   man in the middle can't choose its keys to make the codes match.
3. Both sides show the code: the first 4 bytes of SHA-512(`"PC1500 code"` |
   `pk_c` | `pk_s` | `n_c` | `n_s`), read big-endian, mod 1000000, as six
   digits. The people at both ends check that they're the same and accept
   (or refuse) on each side.
4. Both compute `ltk` = HKDF-SHA512(X25519(own secret, peer's public), salt
   `n_c` | `n_s`, info `"PC1500 pair"` | `pk_c` | `pk_s`), 32 bytes. An
   all-zero X25519 result fails the pairing.
5. Once its user has answered, the connector sends `PAIR_CONFIRM` (`ok` 1/0,
   `mac_c` = `confirm("C")`). The advertiser answers:
   - `ERR BUSY` if its user hasn't answered yet: the connector asks again
     shortly, as long as its user waits (BREAK gives up);
   - `ACK` with `ok` 0 if either user refused;
   - `ACK` with `ok` 1 and `mac_s` = `confirm("S")` if both accepted and
     `mac_c` checks out: it keeps the pairing (the connector's `id` and name,
     `ltk`);
   - `ERR AUTH_FAILED` if `mac_c` doesn't check out.

   The connector checks `mac_s` and keeps the pairing too.
6. The connector then starts over with a fresh `HELLO`, which now
   authenticates (above).

`confirm(role)` = the first 16 bytes of HMAC-SHA512(`ltk`, `"PC1500 confirm "`
| `role` | `id_c` | `id_s`). The ephemeral secret keys are wiped when the
pairing ends; a pairing abandoned part-way (a dropped link, BREAK, a refusal)
leaves nothing behind.

### Sealed frames

After `AUTH`'s `ACK`, a frame `[type][seq][len][payload]` goes out as

```
offset  size  field
0       1     type
1       1     seq
2       2     len  = 4 + payload + 16
4       4     counter (u32, little-endian)
8       n     the payload, encrypted
8+n     16    tag
```

- ChaCha20-Poly1305 as in RFC 8439, with the sender's session key; the
  12-byte nonce is 4 zero bytes then the counter as 8 bytes, little-endian;
  the associated data is `type` | `seq` | `counter` (6 bytes).
- Each side counts its frames from 0 for the session. A receiver takes a
  counter it hasn't taken before and no more than 32 below the highest it
  has (frames can cross, e.g. an `ACK` and a request); anything else, and
  any frame whose tag fails, is dropped unanswered.
- So a frame's payload can be 20 bytes shorter than before: at most
  `MTU − 27`. `MSG` holds at most 220 bytes (it was 240).

