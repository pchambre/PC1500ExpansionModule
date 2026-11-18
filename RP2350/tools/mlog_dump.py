"""Decode the MCU log (RP2350/mcu_log.c) from a flash dump made with
picotool save -r <base> <end> dump.bin. Prints this log's entries, oldest
first, from the newest CLEAR on (as MLOG VIEW would show them)."""
import struct
import sys

SECTOR = 4096
RECORD = 32
MAGIC = 0x4C4F4732
LEVELS = {1: 'I', 2: 'W', 3: 'E', 4: 'U'}

data = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0

sectors = []
for off in range(0, len(data) - SECTOR + 1, SECTOR):
    magic, seq, gen, size_kb = struct.unpack_from('<IIHH', data, off)
    if magic == MAGIC:
        sectors.append((seq, gen, size_kb, off))
if not sectors:
    sys.exit('no log sectors found')
# the current log: the newest sector's generation and size
newest = max(sectors)
sectors = sorted(s for s in sectors if s[1] == newest[1] and s[2] == newest[2])

entries = []  # [level, text, characters still to come in MORE records]
for seq, gen, size_kb, off in sectors:
    for slot in range(1, SECTOR // RECORD):
        r = data[off + slot * RECORD: off + (slot + 1) * RECORD]
        t = r[0]
        if t == 0xFF:
            break
        if t == 0x02:
            entries.clear()  # a CLEAR: only what follows it counts
        elif t == 0x01:
            n = min(r[2], 80)
            k = min(n, 29)  # the rest (since 2026-11-18) in MORE records, 31 each
            entries.append([LEVELS.get(r[1], '?%d' % r[1]), r[3:3 + k].decode('ascii', 'replace'), n - k])
        elif t == 0x03 and entries and entries[-1][2] > 0:
            k = min(entries[-1][2], 31)
            entries[-1][1] += r[1:1 + k].decode('ascii', 'replace')
            entries[-1][2] -= k
for level, msg, _ in entries:
    print('%-4s %s' % (level, msg))
