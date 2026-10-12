"""Bench test for the module firmware on the BGM220 Explorer Kit.

Plays the RP2354B over the kit's USB serial port (MODULE_PROTOCOL.md) and,
with the PC's own Bluetooth radio, a Link peer (BLE_PROTOCOL.md sec.2):

  python host_test.py COM3

  1. HELLO, SET_NAME, ADV_START, and POLL until the queue is empty.
  2. The PC connects to the module as a peer, subscribes to TX and writes a
     frame to RX; the "MCU" must get LINK_UP and that frame as LINK_RX.
  3. The "MCU" sends a frame back with LINK_SEND; the peer must get it as a
     TX notification.
  4. The peer disconnects; the "MCU" must get LINK_DOWN.

Needs pyserial and bleak.
"""
import asyncio
import struct
import sys
import time

import serial
from bleak import BleakClient, BleakScanner

LINK_SERVICE = "c31f0001-92a3-40ab-b63d-7cdb0a37aed0"
LINK_RX = "c31f0002-92a3-40ab-b63d-7cdb0a37aed0"
LINK_TX = "c31f0003-92a3-40ab-b63d-7cdb0a37aed0"
EVENTS = {1: "SCAN_REPORT", 2: "SCAN_DONE", 3: "LINK_UP", 4: "LINK_FAILED", 5: "LINK_DOWN",
          6: "LINK_RX", 0x10: "KBD_STATE", 0x11: "KBD_PASSKEY", 0x12: "KBD_REPORT", 0x7F: "LOG"}


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
            c &= 0xFFFF
    return c


class Module:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, rtscts=True, timeout=0.05)
        self.seq = 0
        self.s.reset_input_buffer()

    def request(self, rtype, payload=b"", tries=3):
        body = bytes([rtype, self.seq]) + struct.pack("<H", len(payload)) + payload
        frame = b"\xA5" + body + struct.pack("<H", crc16(body))
        for _ in range(tries):
            self.s.write(frame)
            ans = self._read_answer(0.05 + len(frame) * 0.0001)
            if ans and ans[0] == (rtype | 0x80) and ans[1] == self.seq:
                self.seq = (self.seq + 1) & 0xFF
                return ans[2]  # payload: status, then data
        raise RuntimeError(f"no answer to request 0x{rtype:02X}")

    def _read_answer(self, timeout):
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += self.s.read(512)
            i = buf.find(b"\xA5")
            if i < 0 or len(buf) - i < 7:
                continue
            t, sq, n = buf[i + 1], buf[i + 2], struct.unpack_from("<H", buf, i + 3)[0]
            if len(buf) - i < 7 + n:
                continue
            body = buf[i + 1:i + 5 + n]
            if struct.unpack_from("<H", buf, i + 5 + n)[0] != crc16(body):
                print("  bad CRC on answer")
                return None
            return (t, sq, buf[i + 5:i + 5 + n])
        return None

    def poll_all(self):
        out = []
        while True:
            a = self.request(0x02)
            assert a[0] == 0, f"POLL status {a[0]}"
            if a[1] == 0:
                return out
            ev, data = a[1], a[2:]
            if ev == 0x7F:
                print(f"  event LOG: {data[2:2 + data[1]].decode(errors='replace')}")
            else:
                print(f"  event {EVENTS.get(ev, hex(ev))}: {data.hex(' ')}")
            out.append((ev, data))


async def main(port):
    m = Module(port)
    a = m.request(0x01, b"\x00")
    v = a[3:3 + a[2]].decode()
    addr = a[3 + a[2]:9 + a[2]]
    print(f"HELLO: status {a[0]}, protocol {a[1]}, firmware {v}, address {addr.hex(':').upper()}")
    name = b"PCTEST"
    print("SET_NAME:", m.request(0x03, bytes([len(name)]) + name)[0])
    print("ADV_START:", m.request(0x10)[0])
    print("events after start:")
    m.poll_all()

    print("peer: scanning for the Link service...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: LINK_SERVICE in [u.lower() for u in adv.service_uuids], timeout=10)
    if not dev:
        print("FAIL: no Link advertiser found")
        return 1
    print(f"peer: found {dev.address} '{dev.name}'")
    got = asyncio.Queue()
    async with BleakClient(dev) as c:
        await c.start_notify(LINK_TX, lambda _, d: got.put_nowait(bytes(d)))
        frame = bytes([0x10, 0, 3, 0]) + b"abc"  # a TEXT frame, as a peer would send
        await c.write_gatt_char(LINK_RX, frame, response=False)
        await asyncio.sleep(0.5)
        evs = m.poll_all()
        ok_up = any(e == 3 for e, _ in evs)
        ok_rx = any(e == 6 and d == frame for e, d in evs)
        print(f"LINK_UP {'OK' if ok_up else 'MISSING'}, LINK_RX {'OK' if ok_rx else 'MISSING'}")
        back = bytes([0x00, 0, 0, 0])  # an ACK for seq 0
        print("LINK_SEND:", m.request(0x17, back)[0])
        try:
            d = await asyncio.wait_for(got.get(), 3)
            ok_tx = d == back
            print(f"peer got {d.hex(' ')}: {'OK' if ok_tx else 'WRONG'}")
        except asyncio.TimeoutError:
            ok_tx = False
            print("peer got nothing: FAIL")
    # Windows can hold the connection a few seconds after the client lets go.
    ok_down = False
    for _ in range(30):
        await asyncio.sleep(0.5)
        if any(e == 5 for e, _ in m.poll_all()):
            ok_down = True
            break
    print(f"LINK_DOWN {'OK' if ok_down else 'MISSING'}")
    print("RESULT:", "PASS" if ok_up and ok_rx and ok_tx and ok_down else "FAIL")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "COM3")))
