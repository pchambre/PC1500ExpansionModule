# The BLE module protocol (internal card)

Status: **draft 0, 2026-10-12**. Nothing implements it yet.

The internal card (`PC1500_TARGET=card`) has no CYW43. Its radio is a Silicon Labs **BGM220S** (U8), running **our own firmware** (option C, the board owner's choice, 2026-10-12). This document is the contract between that firmware and the RP2354B.

The Link protocol itself is [BLE_PROTOCOL.md](BLE_PROTOCOL.md), and nothing in it changes: the other PC-1500, the app and the emulator can't tell a card from a dongle.

## 1. Who does what

The module does the radio work, so the RP2354B can sleep:

- Advertising the Link service under the HOSTNAME, and scanning actively, matching on the Link service UUID (BLE_PROTOCOL.md sec.2, sec.3).
- Connecting. As connector it requests MTU 247, discovers the Link service and subscribes to TX. As advertiser it hosts the service.
- **One Link frame per GATT packet, passed through unchanged** in both directions. The module never looks inside a frame.
- The BLE keyboard (HID over GATT):
  - pairing with Passkey Entry: the passkey is shown on the PC-1500, typed on the keyboard;
  - the bond, kept in the module's own flash (NVM3);
  - the background reconnection;
  - reading reports through the keyboard's report map into the 8-byte boot report.
- Keeping connections alive while the RP2354B sleeps, and waking it only when something arrives for it.

Everything the RP2354B already does for the dongle stays on the RP2354B, shared by both builds:

- the Link protocol: seq, ACK/ERR, the 5 s timeout, sec.7 security, files, messaging, CE-150, the clock;
- turning boot reports into PC-1500 keys (`kbd_seq.c`);
- the Link's pairing store (`link_store`).

**The example this design is for:** two PC-1500s are linked, and both MCUs are asleep. One runs BLSEND. Its frame reaches the other card's module, which queues a `LINK_RX` event and raises HOST_WAKE. That MCU wakes, takes the frame, sends the Link `ACK`, handles the message, and goes back to sleep.

Classic Bluetooth isn't possible: the BGM220S is BLE-only. Classic keyboards work only with the dongle.

## 2. Wiring

| Signal | RP2354B | BGM220S | Direction |
|---|---|---|---|
| UART TX → module RX | GPIO32 | PA06 | MCU → module |
| UART RX ← module TX | GPIO33 | PA05 | module → MCU |
| CTS ← module RTS | GPIO34 | PA07 | module → MCU |
| RTS → module CTS | GPIO35 | PA08 | MCU → module |
| RESETn | GPIO36 | RESETn | MCU → module (open drain: drive low, or release) |
| HOST_WAKE | GPIO37 | PA04 | module → MCU |
| MODULE_WAKE | GPIO27 | PB03 | MCU → module (PB03 is an EM4 wake pin) |
| SWCLK / SWDIO | GPIO38 / 39 | PA01 / PA02 | MCU → module (programming) |

PA05 to PA08 are the BGM220 Explorer Kit's VCOM pins, so the module firmware can be tested on the kit over its USB serial port. The HOST_WAKE and MODULE_WAKE lines would be jumpered there.

## 3. Line settings

- 8N1 with RTS/CTS hardware flow control, both ways. RTS/CTS are active low, as usual.
- **115200 baud for bring-up; the target is 1,000,000.** The baud rate is fixed per build, not negotiated: both sides take it from one `#define` each.
- Not yet verified: the module's USART at 1 Mbaud from its 38.4 MHz HFXO.

## 4. Wake and sessions

All traffic happens in a **session**. A session is open while the MCU holds MODULE_WAKE high.

1. **MCU opens a session:**
   - It raises MODULE_WAKE.
   - On the rising edge the module leaves sleep, if it was asleep, and turns its UART on. Then it asserts RTS, which the MCU sees on its CTS.
   - The MCU sends its first request only once it sees CTS. **No CTS within 10 ms means the module is unwell:** the MCU logs it, resets the module through RESETn, and starts over with `HELLO`.
2. **Inside a session, the MCU sends requests and the module answers each one.**
   - There is one request outstanding at a time.
   - **The module never sends anything except an answer.** So the MCU only listens to its UART during a session it opened, and a sleeping MCU can't miss a byte.
3. **MCU closes the session:**
   - It lowers MODULE_WAKE once it has nothing to ask and no events to collect.
   - The module may then sleep, between connection events, whenever the stack allows.

**Module → MCU:**
- **HOST_WAKE is high exactly while the module has events queued.** The module raises it when it queues the first event.
- The MCU wakes on HOST_WAKE (GPIO37, its second sleep wake source after TRIG_WR), opens a session, and sends `POLL` until the answer is "no event". **The module lowers HOST_WAKE before it sends that "no event" answer.**
- HOST_WAKE is a level, not a pulse. An MCU that was busy when the line rose still sees it high afterwards, and a sleeping MCU that wakes for any other reason checks it before going back to sleep.

## 5. Frames

```
offset  size  field
0       1     0xA5            start
1       1     type
2       1     seq
3       2     len             payload length, little-endian, at most 300
5       len   payload
5+len   2     crc             CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload, LE
```

- **Requests** have types 0x01 to 0x7F. **Answers** carry the same type with bit 7 set, and the request's seq.
- Every answer's payload starts with a **status** byte:
  - 0 OK;
  - 1 BAD_FRAME;
  - 2 UNKNOWN_TYPE;
  - 3 BAD_ARGS;
  - 4 BUSY;
  - 5 NOT_CONNECTED;
  - 6 STATE: not now, for example SCAN_START while connecting;
  - 7 FAILED.
- **Requests are answered at once** (within 20 ms). Anything that takes longer reports its result as an event.
- **The MCU numbers its requests 0, 1, 2, …**
  - No answer within 50 ms, or a bad CRC: the MCU resends the **same request with the same seq**.
  - A module that sees a repeated seq resends the answer it gave, without acting twice. That's why `LINK_SEND` and `POLL` can be retried safely.
  - After 3 failures the MCU resets the module.
- **Resync:** after a bad CRC or a timeout, the receiver discards bytes until the next 0xA5.

## 6. Requests

| Type | Request | Payload | Answer after the status byte |
|---|---|---|---|
| 0x01 | `HELLO` | protocol version (1 byte, 0 = this draft) | protocol version, firmware version (len + text), BT address (6), max payload (2) |
| 0x02 | `POLL` | — | event type (0 = none), then the event's payload (sec.7) |
| 0x03 | `SET_NAME` | name (len + text, at most 15) | — |
| 0x04 | `SET_TX_POWER` | maximum TX power, 0.1 dBm units (int16 LE) | the power actually set |
| 0x10 | `ADV_START` | — | — (advertise the Link service under the name) |
| 0x11 | `ADV_STOP` | — | — |
| 0x12 | `SCAN_START` | what (1 = Link peers, 2 = keyboards), duration ms (2) | — |
| 0x13 | `SCAN_STOP` | — | — |
| 0x14 | `CONNECT` | address type, address (6), timeout ms (2) | — |
| 0x15 | `CONNECT_CANCEL` | — | — |
| 0x16 | `DISCONNECT` | — | — (the Link connection) |
| 0x17 | `LINK_SEND` | one Link frame (BLE_PROTOCOL.md sec.4), at most MTU − 3 | — (BUSY: the previous frame hasn't gone yet; NOT_CONNECTED) |
| 0x20 | `KBD_PAIR` | timeout s (1) | — |
| 0x21 | `KBD_STOP` | — | — (stop pairing, or drop the keyboard's connection) |
| 0x22 | `KBD_FORGET` | — | — (delete the bond) |
| 0x23 | `KBD_ENABLE` | 0 or 1 | — (background reconnection to the bonded keyboard, as BLKBD) |
| 0x24 | `KBD_STATUS` | — | state (as `EXP_KBD_*`), address type, address (6), name (len + text) |

**Notes on the requests:**
- **There's one Link connection at a time,** as on the dongle. `DISCONNECT` and `LINK_SEND` refer to it.
- **The keyboard connection is separate.** The module keeps both at once. On the dongle the keyboard had to step aside while the Link scanned or connected (BTstack makes one LE connection at a time); the module doesn't have that limit.
- **`LINK_SEND` needs no "sent" event.** The Link's own ACK, which arrives as `LINK_RX`, is what the MCU waits for, exactly as on the dongle.

## 7. Events (in `POLL` answers)

| Type | Event | Payload |
|---|---|---|
| 0x01 | `SCAN_REPORT` | what (1/2), address type, address (6), RSSI (int8), name (len + text) |
| 0x02 | `SCAN_DONE` | — |
| 0x03 | `LINK_UP` | role (0 = we connected, 1 = they connected), address type, address (6), MTU (2) |
| 0x04 | `LINK_FAILED` | reason (2): 0xF001 CONNECT's timeout ran out, 0xF002 no Link service (or not both characteristics), 0xF003 MTU under 64, 0xF004 CONNECT_CANCEL; anything else is the stack's own code |
| 0x05 | `LINK_DOWN` | reason (2) |
| 0x06 | `LINK_RX` | one Link frame, unchanged |
| 0x10 | `KBD_STATE` | state (as `EXP_KBD_*`) |
| 0x11 | `KBD_PASSKEY` | passkey (4, LE; show as six digits) |
| 0x12 | `KBD_REPORT` | 8-byte boot keyboard report |
| 0x7F | `LOG` | level (1), text (len + text): module diagnostics for the MCU log, since the module has no log of its own |

**When the link is up:**
- As connector, `LINK_UP` comes once the MTU is set, the Link service is found and TX is subscribed.
- As advertiser, it comes once the peer has subscribed to TX.
- Before that, the MCU sends nothing. That matches BLE_PROTOCOL.md sec.2.

**Queue size:**
- The Link's one frame in flight per direction bounds `LINK_RX` to two frames: a frame and an ACK.
- Keyboard reports come at typing speed.
- So 8 slots of 260 bytes (about 2 KB of the module's 32 KB RAM) can't overflow in normal use.
- If the queue is full anyway, the module drops the event and queues a `LOG` saying so in a reserved slot. The Link's timeout then recovers, as it would after a radio loss.

## 8. Changes on the RP2354B

`ble_link.c` is split into two parts:
- **the Link protocol,** which keeps all the frame, security, file and message logic, shared;
- **a transport underneath:** BTstack for the dongle, this protocol for the card.

The transport's interface is the list in sec.6 and sec.7: advertise, scan, connect, disconnect, send a frame, and events up. The dongle must behave exactly as it does today, so the split is tested on dongle hardware before the card uses it.

`kbd_host.c` keeps its EXP_COMMAND_KBD_* handling. On the card, its BTstack work is replaced by `KBD_*` requests, and `KBD_REPORT` events go into `kbd_seq.c` as the boot reports do today.

## 9. Open questions

- **The module's lowest sleep with connections kept** (EM2), and its wake time on MODULE_WAKE. This is what the 10 ms CTS timeout assumes.
- **1 Mbaud on the module's USART**, or which of its UARTs runs it.
- **RTS as the "awake" signal** (sec.4, step 1). The USART drives RTS in hardware, and a sleeping module's pins hold their last state, so RTS may show "ready" while the module is still waking. The 50 ms retry (sec.5) covers a request lost that way. Measure the wake time on the card and decide whether the MCU should wait a fixed time after raising MODULE_WAKE.
- **Tested so far (2026-10-12, Explorer Kit, with a PC as the peer):**
  - `BGM220/tools/host_test.py`, the advertiser side: HELLO, SET_NAME, ADV_START, POLL, LINK_UP, LINK_RX, LINK_SEND and LINK_DOWN.
  - `BGM220/tools/connector_test.py`, the connector side: SCAN_START, SCAN_REPORT and SCAN_DONE (found by UUID); CONNECT, then LINK_UP with role 0 and MTU 247; LINK_SEND as an RX write; LINK_RX from a TX notification; DISCONNECT and LINK_DOWN.
  - Not yet: the keyboard requests, SET_TX_POWER, CONNECT_CANCEL and the timeout path.
- **Firmware updates.** The first flashing is over SWD from the kit's J-Link. In the field the RP2354B could flash the module over SWD (GPIO38/39) from a file on the SD card, or use Silicon Labs' UART bootloader.
- **The Silicon Labs SDK's licence terms** for open-source cards that are sold.
