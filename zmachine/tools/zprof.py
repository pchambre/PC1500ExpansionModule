#!/usr/bin/env python3
"""zprof.py -- sums an emulator profile (ZPROF=<file>, address and cycles
per line) by zip.lst's labels, and the ROM's and the module's code apart.

    python zprof.py profile.txt [zip.lst]
"""
import re
import sys


def labels(lst):
    out = []
    for line in open(lst):
        m = re.match(r'\s+([0-9A-F]{4})\s.*?\d+\s+([A-Z][A-Z0-9_]*):', line)
        if m:
            out.append((int(m.group(1), 16), m.group(2)))
    return sorted(set(out))


def main():
    prof = sys.argv[1]
    lst = sys.argv[2] if len(sys.argv) > 2 else 'zip.lst'
    labs = labels(lst)
    lo, hi = labs[0][0], 0x6800
    sums = {}
    total = 0
    for line in open(prof):
        a, c = line.split()
        a, c = int(a, 16), int(c)
        total += c
        if a >= 0xC000:
            name = 'base ROM'
        elif 0x8000 <= a < 0xC000:
            name = 'module ROM'
        elif lo <= a < hi:
            name = '?'
            for addr, n in labs:
                if addr <= a:
                    name = n
                else:
                    break
        else:
            name = 'other %04X' % a
        sums[name] = sums.get(name, 0) + c
    for name, c in sorted(sums.items(), key=lambda x: -x[1])[:30]:
        print('%-14s %10d %5.1f%%' % (name, c, 100.0 * c / total))
    print('%-14s %10d' % ('total', total))


if __name__ == '__main__':
    main()
