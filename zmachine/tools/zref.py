#!/usr/bin/env python3
"""zref.py -- a small reference v3 Z-machine for checking zip.asm.

It plays a story from a script of commands and writes the transcript.
With --trace it also writes the address of every instruction, one per
line, so a run of the LH5801 interpreter can be compared against it.
Its random-number generator is the same xorshift as zip.asm's, so the
two runs match even in games that use randomness.

    python zref.py STORY.z3 --script cmds.txt [--trace trace.txt] [--seed N]

In a script, {LEFT} is the console's backspace (the < key) and {CL} its CL.
SAVE and RESTORE ask for a file name as ZIP's console does: the script's
next line, typed after the last name used (at first ZORK1.SAV: the tests
copy every story to ZORK1.Z3); saves are kept in memory.
"""
import argparse
import sys

A0 = "abcdefghijklmnopqrstuvwxyz"
A1 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
A2 = "^\n0123456789.,!?_#'\"/\\-:()"   # from z-char 6; ^ is the escape


class Quit(Exception):
    pass


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Rng:
    """xorshift16 (7, 9, 8), as zip.asm's RANDOM."""

    def __init__(self, seed):
        self.x = seed & 0xFFFF or 1

    def next(self):
        x = self.x
        x ^= (x << 7) & 0xFFFF
        x ^= x >> 9
        x ^= (x << 8) & 0xFFFF
        self.x = x
        return x


class ZMachine:
    def __init__(self, story, inputs, seed, trace=None):
        self.orig = bytes(story)
        self.mem = bytearray(story)
        self.inputs = list(inputs)
        self.out = []
        self.trace = trace
        self.rng = Rng(seed)
        self.seed = seed
        self.savename = "ZORK1.SAV"
        self.saves = {}
        self.restart()

    def restart(self):
        static = self.w(0x0E)
        self.mem[:static] = self.orig[:static]
        self.mem[1] &= ~0x70 & 0xFF       # status line available, no split, fixed pitch
        self.pc = self.w(0x06)
        self.stack = []
        self.frames = []                  # (return pc, result var, locals, stack depth)
        self.locals = []
        self.dict = self.w(0x08)
        self.objs = self.w(0x0A)
        self.globals = self.w(0x0C)
        self.abbrevs = self.w(0x18)

    # ---- memory ----
    def b(self, a):
        return self.mem[a & 0x1FFFF]

    def w(self, a):
        return (self.mem[a] << 8) | self.mem[a + 1]

    def sb(self, a, v):
        self.mem[a & 0xFFFF] = v & 0xFF

    def sw(self, a, v):
        self.mem[a & 0xFFFF] = (v >> 8) & 0xFF
        self.mem[(a + 1) & 0xFFFF] = v & 0xFF

    def fetch(self):
        v = self.mem[self.pc]
        self.pc += 1
        return v

    def fetchw(self):
        v = self.w(self.pc)
        self.pc += 2
        return v

    # ---- variables ----
    def getvar(self, n):
        if n == 0:
            return self.stack.pop()
        if n < 16:
            return self.locals[n - 1]
        return self.w(self.globals + 2 * (n - 16))

    def setvar(self, n, v):
        v &= 0xFFFF
        if n == 0:
            self.stack.append(v)
        elif n < 16:
            self.locals[n - 1] = v
        else:
            self.sw(self.globals + 2 * (n - 16), v)

    # The indirect forms (inc, dec, load, store, pull...) use the stack
    # top in place.
    def peekvar(self, n):
        return self.stack[-1] if n == 0 else self.getvar(n)

    def pokevar(self, n, v):
        if n == 0:
            self.stack[-1] = v & 0xFFFF
        else:
            self.setvar(n, v)

    def store(self, v):
        self.setvar(self.fetch(), v)

    def branch(self, cond):
        b = self.fetch()
        if b & 0x40:
            off = b & 0x3F
        else:
            off = ((b & 0x3F) << 8) | self.fetch()
            if off & 0x2000:
                off -= 0x4000
        if bool(b & 0x80) == bool(cond):
            if off == 0 or off == 1:
                self.ret(off)
            else:
                self.pc += off - 2

    # ---- calls ----
    def call(self, args, resvar):
        addr = args[0] * 2
        if addr == 0:
            self.setvar(resvar, 0)
            return
        n = self.b(addr)
        addr += 1
        locs = []
        for i in range(n):
            locs.append(self.w(addr))
            addr += 2
        for i, a in enumerate(args[1:]):
            if i < n:
                locs[i] = a
        self.frames.append((self.pc, resvar, self.locals, len(self.stack)))
        self.locals = locs
        self.pc = addr

    def ret(self, v):
        pc, resvar, locs, depth = self.frames.pop()
        del self.stack[depth:]
        self.pc = pc
        self.locals = locs
        self.setvar(resvar, v)

    # ---- text ----
    def emit(self, s):
        self.out.append(s)

    def zstring(self, addr):
        """Decodes the Z-string at addr; returns (text, address after it)."""
        zc = []
        while True:
            word = self.w(addr)
            addr += 2
            zc += [(word >> 10) & 31, (word >> 5) & 31, word & 31]
            if word & 0x8000:
                break
        out = []
        alpha = 0
        i = 0
        while i < len(zc):
            c = zc[i]
            i += 1
            if c == 0:
                out.append(' ')
                alpha = 0
            elif c <= 3:
                if i < len(zc):
                    idx = 32 * (c - 1) + zc[i]
                    i += 1
                    out.append(self.zstring(self.w(self.abbrevs + 2 * idx) * 2)[0])
                alpha = 0
            elif c == 4:
                alpha = 1
            elif c == 5:
                alpha = 2
            else:
                if alpha == 2 and c == 6:
                    if i + 1 < len(zc):
                        out.append(chr((zc[i] << 5) | zc[i + 1]))
                    i += 2
                elif alpha == 2:
                    out.append(A2[c - 6])
                elif alpha == 1:
                    out.append(A1[c - 6])
                else:
                    out.append(A0[c - 6])
                alpha = 0
        return ''.join(out), addr

    def print_at(self, addr):
        text, after = self.zstring(addr)
        self.emit(text)
        return after

    # ---- objects ----
    def obj(self, n):
        return self.objs + 62 + 9 * (n - 1)

    def props(self, n):
        a = self.w(self.obj(n) + 7)
        return a + 1 + 2 * self.b(a)

    def find_prop(self, n, p):
        a = self.props(n)
        while True:
            s = self.b(a)
            if s == 0:
                return 0
            if s & 31 == p:
                return a + 1
            if s & 31 < p:
                return 0
            a += 2 + (s >> 5)

    def remove(self, o):
        oa = self.obj(o)
        parent = self.b(oa + 4)
        if parent == 0:
            return
        pa = self.obj(parent)
        sib = self.b(oa + 5)
        if self.b(pa + 6) == o:
            self.sb(pa + 6, sib)
        else:
            c = self.b(pa + 6)
            while c:
                ca = self.obj(c)
                if self.b(ca + 5) == o:
                    self.sb(ca + 5, sib)
                    break
                c = self.b(ca + 5)
        self.sb(oa + 4, 0)
        self.sb(oa + 5, 0)

    # ---- input ----
    def encode(self, word):
        zc = []
        for ch in word:
            if ch in A0:
                zc.append(A0.index(ch) + 6)
            elif ch in A2[2:]:
                zc += [5, A2.index(ch) + 6]
            else:
                o = ord(ch)
                zc += [5, 6, o >> 5, o & 31]
        zc = (zc + [5] * 6)[:6]
        w1 = (zc[0] << 10) | (zc[1] << 5) | zc[2]
        w2 = (zc[3] << 10) | (zc[4] << 5) | zc[5] | 0x8000
        return w1, w2

    def file_name(self, prompt):
        """The console's file prompt: the last name, then the script's line
        typed after it ({CL} and {LEFT} as the keys), up to 24 characters."""
        self.emit(prompt)
        line = self.inputs.pop(0) if self.inputs else ""
        name = list(self.savename)
        while line:
            if line.startswith('{CL}'):
                name, line = [], line[4:]
            elif line.startswith('{LEFT}'):
                name, line = name[:-1], line[6:]
            else:
                if len(name) < 24:
                    name.append(line[0])
                line = line[1:]
        name = ''.join(name)
        self.emit(name + "\n")
        if name:
            self.savename = name
        return name

    def sread(self, text, parse):
        self.show_status()
        line = self.inputs.pop(0) if self.inputs else None
        if line is None:
            raise Quit()
        while '{LEFT}' in line:                         # the console's backspace
            i = line.index('{LEFT}')
            line = line[:max(i - 1, 0)] + line[i + 6:]
        self.emit(line + "\n")
        line = line.lower()[: self.b(text) - 1]
        for i, ch in enumerate(line):
            self.sb(text + 1 + i, ord(ch))
        self.sb(text + 1 + len(line), 0)
        nsep = self.b(self.dict)
        seps = [chr(self.b(self.dict + 1 + i)) for i in range(nsep)]
        elen = self.b(self.dict + 1 + nsep)
        count = s16(self.w(self.dict + 2 + nsep))
        entries = self.dict + 4 + nsep
        words = []
        i = 0
        while i < len(line):
            ch = line[i]
            if ch == ' ':
                i += 1
            elif ch in seps:
                words.append((i, 1))
                i += 1
            else:
                j = i
                while j < len(line) and line[j] != ' ' and line[j] not in seps:
                    j += 1
                words.append((i, j - i))
                i = j
        words = words[: self.b(parse)]
        self.sb(parse + 1, len(words))
        for k, (start, ln) in enumerate(words):
            w1, w2 = self.encode(line[start:start + ln])
            found = 0
            for e in range(count):
                ea = entries + e * elen
                if self.w(ea) == w1 and self.w(ea + 2) == w2:
                    found = ea
                    break
            pa = parse + 2 + 4 * k
            self.sw(pa, found)
            self.sb(pa + 2, ln)
            self.sb(pa + 3, start + 1)

    def show_status(self):
        loc = self.getvar(16)
        name = self.zstring(self.w(self.obj(loc) + 7) + 1)[0] if loc else ""
        a, b = s16(self.getvar(17)), s16(self.getvar(18))
        if self.b(1) & 2:
            self.status = "%s  %d:%02d" % (name, a, b)
        else:
            self.status = "%s  %d/%d" % (name, a, b)

    # ---- the loop ----
    def run(self):
        try:
            while True:
                self.step()
        except Quit:
            pass

    def operand(self, t):
        if t == 0:
            return self.fetchw()
        if t == 1:
            return self.fetch()
        return self.getvar(self.fetch())

    def step(self):
        start = self.pc
        op = self.fetch()
        if self.trace:
            self.trace.write("%05x %02x\n" % (start, op))
        args = []
        if op < 0x80:                                   # long 2OP
            args = [self.operand(1 + ((op >> 6) & 1)), self.operand(1 + ((op >> 5) & 1))]
            self.op2(op & 31, args)
        elif op < 0xB0:                                 # short 1OP
            args = [self.operand((op >> 4) & 3)]
            self.op1(op & 15, args[0])
        elif op < 0xC0:                                 # 0OP
            self.op0(op & 15)
        else:                                           # variable
            types = self.fetch()
            for sh in (6, 4, 2, 0):
                t = (types >> sh) & 3
                if t == 3:
                    break
                args.append(self.operand(t))
            if op < 0xE0:
                self.op2(op & 31, args)
            else:
                self.opv(op & 31, args)

    def op0(self, n):
        if n == 0:
            self.ret(1)
        elif n == 1:
            self.ret(0)
        elif n == 2:
            self.pc = self.print_at(self.pc)
        elif n == 3:
            self.pc = self.print_at(self.pc)
            self.emit("\n")
            self.ret(1)
        elif n == 4:
            pass
        elif n == 5:                                    # save
            name = self.file_name("Save as: ")
            if name:
                static = self.w(0x0E)
                self.saves[name] = (bytes(self.mem[:static]), list(self.stack),
                                    [(f[0], f[1], list(f[2]), f[3]) for f in self.frames],
                                    list(self.locals), self.pc)
            self.branch(bool(name))
        elif n == 6:                                    # restore
            name = self.file_name("Restore from: ")
            s = self.saves.get(name)
            if s:
                mem, stack, frames, locs, pc = s
                flags2 = self.mem[0x10:0x12]            # the interpreter's: kept
                self.mem[:len(mem)] = mem
                self.mem[0x10:0x12] = flags2
                self.mem[1] &= ~0x70 & 0xFF
                self.stack = list(stack)
                self.frames = [(f[0], f[1], list(f[2]), f[3]) for f in frames]
                self.locals = list(locs)
                self.pc = pc
            self.branch(bool(s))
        elif n == 7:
            self.restart()
        elif n == 8:
            self.ret(self.stack.pop())
        elif n == 9:
            self.stack.pop()
        elif n == 10:
            raise Quit()
        elif n == 11:
            self.emit("\n")
        elif n == 12:
            self.show_status()
        elif n == 13:
            self.branch(True)
        else:
            raise RuntimeError("bad 0OP %d at %05x" % (n, self.pc))

    def op1(self, n, a):
        if n == 0:
            self.branch(a == 0)
        elif n == 1:
            v = self.b(self.obj(a) + 5)
            self.store(v)
            self.branch(v != 0)
        elif n == 2:
            v = self.b(self.obj(a) + 6)
            self.store(v)
            self.branch(v != 0)
        elif n == 3:
            self.store(self.b(self.obj(a) + 4))
        elif n == 4:
            self.store(0 if a == 0 else (self.b(a - 1) >> 5) + 1)
        elif n == 5:
            self.pokevar(a, self.peekvar(a) + 1)
        elif n == 6:
            self.pokevar(a, self.peekvar(a) - 1)
        elif n == 7:
            self.print_at(a)
        elif n == 9:
            self.remove(a)
        elif n == 10:
            self.print_at(self.w(self.obj(a) + 7) + 1)
        elif n == 11:
            self.ret(a)
        elif n == 12:
            self.pc += s16(a) - 2
        elif n == 13:
            self.print_at(a * 2)
        elif n == 14:
            self.store(self.peekvar(a))
        elif n == 15:
            self.store(~a & 0xFFFF)
        else:
            raise RuntimeError("bad 1OP %d at %05x" % (n, self.pc))

    def op2(self, n, args):
        a = args[0] if args else 0
        b = args[1] if len(args) > 1 else 0
        if n == 1:
            self.branch(a in args[1:])
        elif n == 2:
            self.branch(s16(a) < s16(b))
        elif n == 3:
            self.branch(s16(a) > s16(b))
        elif n == 4:
            v = (s16(self.peekvar(a)) - 1) & 0xFFFF
            self.pokevar(a, v)
            self.branch(s16(v) < s16(b))
        elif n == 5:
            v = (s16(self.peekvar(a)) + 1) & 0xFFFF
            self.pokevar(a, v)
            self.branch(s16(v) > s16(b))
        elif n == 6:
            self.branch(self.b(self.obj(a) + 4) == b)
        elif n == 7:
            self.branch(a & b == b)
        elif n == 8:
            self.store(a | b)
        elif n == 9:
            self.store(a & b)
        elif n == 10:
            self.branch(self.b(self.obj(a) + b // 8) & (0x80 >> (b % 8)))
        elif n == 11:
            o = self.obj(a) + b // 8
            self.sb(o, self.b(o) | (0x80 >> (b % 8)))
        elif n == 12:
            o = self.obj(a) + b // 8
            self.sb(o, self.b(o) & ~(0x80 >> (b % 8)))
        elif n == 13:
            self.pokevar(a, b)
        elif n == 14:
            self.remove(a)
            oa, da = self.obj(a), self.obj(b)
            self.sb(oa + 4, b)
            self.sb(oa + 5, self.b(da + 6))
            self.sb(da + 6, a)
        elif n == 15:
            self.store(self.w((a + 2 * b) & 0xFFFF))
        elif n == 16:
            self.store(self.b((a + b) & 0xFFFF))
        elif n == 17:
            p = self.find_prop(a, b)
            if p == 0:
                self.store(self.w(self.objs + 2 * (b - 1)))
            elif self.b(p - 1) >> 5:
                self.store(self.w(p))
            else:
                self.store(self.b(p))
        elif n == 18:
            self.store(self.find_prop(a, b))
        elif n == 19:
            if b == 0:
                p = self.props(a)
            else:
                p = self.find_prop(a, b)
                p += 1 + (self.b(p - 1) >> 5)
            self.store(self.b(p) & 31)
        elif n == 20:
            self.store(a + b)
        elif n == 21:
            self.store(a - b)
        elif n == 22:
            self.store(s16(a) * s16(b))
        elif n == 23:
            x, y = s16(a), s16(b)
            q = abs(x) // abs(y)
            self.store(q if (x < 0) == (y < 0) else -q)
        elif n == 24:
            x, y = s16(a), s16(b)
            r = abs(x) % abs(y)
            self.store(-r if x < 0 else r)
        else:
            raise RuntimeError("bad 2OP %d at %05x" % (n, self.pc))

    def opv(self, n, args):
        if n == 0:
            self.call(args, self.fetch())
        elif n == 1:
            self.sw(args[0] + 2 * args[1], args[2])
        elif n == 2:
            self.sb(args[0] + args[1], args[2])
        elif n == 3:
            p = self.find_prop(args[0], args[1])
            if self.b(p - 1) >> 5:
                self.sw(p, args[2])
            else:
                self.sb(p, args[2])
        elif n == 4:
            self.sread(args[0], args[1])
        elif n == 5:
            self.emit(chr(args[0]) if args[0] != 13 else "\n")
        elif n == 6:
            self.emit(str(s16(args[0])))
        elif n == 7:
            r = s16(args[0])
            if r > 0:
                self.store(self.rng.next() % r + 1)
            else:
                self.rng = Rng(-r if r else self.seed)
                self.store(0)
        elif n == 8:
            self.setvar(0, args[0])
        elif n == 9:
            v = self.stack.pop()
            self.pokevar(args[0], v)
        elif n in (10, 11, 19, 20, 21):                 # split/set window, streams, sound
            pass
        else:
            raise RuntimeError("bad VAR %d at %05x" % (n, self.pc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("story")
    ap.add_argument("--script", help="commands, one per line")
    ap.add_argument("--trace", help="write instruction addresses here")
    ap.add_argument("--seed", type=int, default=0x1234)
    a = ap.parse_args()
    story = open(a.story, "rb").read()
    inputs = []
    if a.script:
        inputs = [l.rstrip("\r\n") for l in open(a.script)]
    trace = open(a.trace, "w") if a.trace else None
    z = ZMachine(story, inputs, a.seed, trace)
    z.run()
    sys.stdout.write(''.join(z.out))
    if trace:
        trace.close()


if __name__ == "__main__":
    main()
