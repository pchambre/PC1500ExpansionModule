"""Bench test for the module firmware's connector side, on the Explorer Kit.

The PC advertises a Link service (BLE_PROTOCOL.md sec.2) with Windows' own
GATT server; the module, driven over the kit's USB serial port as the
RP2354B would drive it (MODULE_PROTOCOL.md), scans for it, connects and
exchanges frames:

  python connector_test.py COM3

  1. SCAN_START (Link peers) -> SCAN_REPORT for this PC, SCAN_DONE.
  2. CONNECT -> LINK_UP (role 0, MTU >= 64).
  3. LINK_SEND -> the PC's RX characteristic gets the frame.
  4. The PC notifies a frame on TX -> LINK_RX.
  5. DISCONNECT -> LINK_DOWN.

Needs pyserial and the winrt packages bleak installs (Windows only).
"""
import asyncio
import sys
import uuid

from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattCharacteristicProperties, GattLocalCharacteristicParameters, GattProtectionLevel,
    GattServiceProvider, GattServiceProviderAdvertisingParameters, GattWriteOption)
from winrt.windows.storage.streams import DataReader, DataWriter

from host_test import Module

LINK_SERVICE = uuid.UUID("c31f0001-92a3-40ab-b63d-7cdb0a37aed0")
LINK_RX = uuid.UUID("c31f0002-92a3-40ab-b63d-7cdb0a37aed0")
LINK_TX = uuid.UUID("c31f0003-92a3-40ab-b63d-7cdb0a37aed0")


class LinkPeer:
    """A Link advertiser: RX takes writes, TX notifies."""

    def __init__(self, loop):
        self.loop = loop
        self.received = asyncio.Queue()
        self.subscribed = asyncio.Event()

    async def start(self):
        r = await GattServiceProvider.create_async(LINK_SERVICE)
        self.provider = r.service_provider
        svc = self.provider.service

        p = GattLocalCharacteristicParameters()
        p.characteristic_properties = (GattCharacteristicProperties.WRITE |
                                       GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE)
        p.write_protection_level = GattProtectionLevel.PLAIN
        self.rx = (await svc.create_characteristic_async(LINK_RX, p)).characteristic
        self.rx.add_write_requested(self._on_write)

        p = GattLocalCharacteristicParameters()
        p.characteristic_properties = GattCharacteristicProperties.NOTIFY
        p.read_protection_level = GattProtectionLevel.PLAIN
        self.tx = (await svc.create_characteristic_async(LINK_TX, p)).characteristic
        self.tx.add_subscribed_clients_changed(self._on_subscribed)

        ap = GattServiceProviderAdvertisingParameters()
        ap.is_connectable = True
        ap.is_discoverable = True
        self.provider.start_advertising_with_parameters(ap)

    def _on_write(self, sender, args):
        deferral = args.get_deferral()

        async def take():
            try:
                req = await args.get_request_async()
                data = bytes(memoryview(req.value))
                if req.option == GattWriteOption.WRITE_WITH_RESPONSE:
                    req.respond()
                self.received.put_nowait(data)
            except Exception as e:  # report it, or the test just sees "nothing"
                print(f"  PC write handler error: {e!r}")
            finally:
                deferral.complete()
        asyncio.run_coroutine_threadsafe(take(), self.loop)

    def _on_subscribed(self, sender, args):
        if sender.subscribed_clients.size:
            self.loop.call_soon_threadsafe(self.subscribed.set)

    async def notify(self, data):
        w = DataWriter()
        w.write_bytes(bytes(data))
        await self.tx.notify_value_async(w.detach_buffer())

    def stop(self):
        self.provider.stop_advertising()


async def poll_until(m, want, timeout):
    """POLL until event `want` arrives; returns its data, or None."""
    end = asyncio.get_running_loop().time() + timeout
    while asyncio.get_running_loop().time() < end:
        for ev, data in m.poll_all():
            if ev == want:
                return data
            if ev in (4,):  # LINK_FAILED
                print(f"  LINK_FAILED reason 0x{int.from_bytes(data[:2], 'little'):04X}")
                return None
        await asyncio.sleep(0.2)
    return None


async def main(port):
    loop = asyncio.get_running_loop()
    peer = LinkPeer(loop)
    await peer.start()
    print("PC: advertising the Link service")
    await asyncio.sleep(1.0)

    m = Module(port)
    a = m.request(0x01, b"\x00")
    print(f"HELLO: status {a[0]}, firmware {a[3:3 + a[2]].decode()}")
    if m.request(0x16)[0] == 0:  # a link left over from an earlier run
        print("(dropping a leftover link)")
        await poll_until(m, 5, 10)
    m.poll_all()

    print("SCAN_START:", m.request(0x12, bytes([1]) + (5000).to_bytes(2, "little"))[0])
    reports = []
    end = loop.time() + 8
    done = False
    while loop.time() < end and not done:
        for ev, data in m.poll_all():
            if ev == 1:
                reports.append(data)
            if ev == 2:
                done = True
        await asyncio.sleep(0.2)
    print(f"SCAN_DONE {'OK' if done else 'MISSING'}, {len(reports)} Link peer(s)")
    if not reports:
        print("RESULT: FAIL (the PC wasn't found)")
        peer.stop()
        return 1
    best = max(reports, key=lambda d: (d[8] if d[8] < 128 else d[8] - 256))  # strongest RSSI
    atype, addr = best[1], best[2:8]
    print(f"connecting to {addr.hex(':').upper()} (type {atype}, name '{best[10:10 + best[9]].decode(errors='replace')}')")

    print("CONNECT:", m.request(0x14, bytes([atype]) + addr + (10000).to_bytes(2, "little"))[0])
    up = await poll_until(m, 3, 12)
    ok_up = up is not None and up[0] == 0
    mtu = int.from_bytes(up[8:10], "little") if up else 0
    print(f"LINK_UP {'OK' if ok_up else 'MISSING'} (role {up[0] if up else '-'}, MTU {mtu})")

    ok_tx = ok_rx = False
    if ok_up:
        frame = bytes([0x10, 0, 3, 0]) + b"xyz"
        print("LINK_SEND:", m.request(0x17, frame)[0])
        try:
            got = await asyncio.wait_for(peer.received.get(), 3)
            ok_tx = got == frame
            print(f"PC got {got.hex(' ')}: {'OK' if ok_tx else 'WRONG'}")
        except asyncio.TimeoutError:
            print("PC got nothing: FAIL")
        back = bytes([0x00, 0, 0, 0])
        await peer.subscribed.wait()
        await peer.notify(back)
        rx = await poll_until(m, 6, 3)
        ok_rx = rx == back
        print(f"LINK_RX {rx.hex(' ') if rx else '-'}: {'OK' if ok_rx else 'FAIL'}")
        print("DISCONNECT:", m.request(0x16)[0])
    down = await poll_until(m, 5, 10)
    ok_down = down is not None
    print(f"LINK_DOWN {'OK' if ok_down else 'MISSING'}")
    peer.stop()
    print("RESULT:", "PASS" if done and ok_up and ok_tx and ok_rx and ok_down else "FAIL")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "COM3")))
