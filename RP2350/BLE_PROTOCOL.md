# PC-1500 Link: the BLE protocol

The protocol between a PC-1500 expansion board and anything it talks to over
Bluetooth Low Energy: the feature-server app (`ble_app/`), another PC-1500,
or the emulator. The firmware (`ble_link`), the app and the emulator all
implement this document. The requirements are in the design Google Doc,
`1JsBLb6IfurR44339UCfs4avSaGKE_G_6TDKPcVkkZ6s`.

Version 1, 2026-09-27. Milestone 1 uses `HELLO`, `TEXT`, the file messages,
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
- No pairing or bonding.
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
| 0x01 | `HELLO` | `version` u8 (= 1), `kind` u8 (1 PC-1500, 2 server), `name` str8 |
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
| 0x7E | `ACK` | none |
| 0x7F | `ERR` | `code` u8, then an optional ASCII message |

Reserved type ranges: 0x30–0x3F peer messaging (P2P), 0x40–0x5F server
features (web, plotter). An implementation answers types it doesn't know
with `ERR UNSUPPORTED`.

### Connecting: `HELLO`

The connector sends `HELLO` as soon as it has subscribed to TX. The
advertiser `ACK`s it and sends its own `HELLO`, which the connector `ACK`s.
Until then neither side sends anything else. A `version` the receiver doesn't
support is answered `ERR UNSUPPORTED`, and the connector disconnects.

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

## 6. Throughput

One frame in flight per direction means one frame per round trip, which is
two to three connection intervals. At a 30 ms interval and a 247-byte MTU
that's roughly 3 KB/s: a 10 KB program takes a few seconds. That's fine for
milestone 1. If it isn't later, the fix is a window of several frames in
flight, not a different framing.
