"""The card's BLSCAN through the module, end to end (bring-up).

The PC advertises a Link service (connector_test.LinkPeer); the card --
RP2354B wired to the BGM220 Explorer Kit -- is told over its USB console
to run BLSCAN's command ('s', RP2350/ble_link.c ble_link_module_console())
and prints the listing, which must show this PC.

  python card_scan_test.py COM7
"""
import asyncio
import sys
import time

import serial

from connector_test import LinkPeer


async def main(port):
    peer = LinkPeer(asyncio.get_running_loop())
    await peer.start()
    print("PC: advertising the Link service")
    await asyncio.sleep(1.0)
    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = True
    s.reset_input_buffer()
    s.write(b"s")
    out = b""
    end = time.time() + 8
    while time.time() < end:
        out += s.read(4096)
        if b"found" in out and out.endswith(b"\n"):
            await asyncio.sleep(0.3)
            out += s.read(4096)
            break
        await asyncio.sleep(0.05)
    peer.stop()
    lines = [l for l in out.decode(errors="replace").splitlines() if "[ble]" in l or "[module]" in l]
    print("\n".join(lines) if lines else "(no answer from the card)")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "COM7")))
