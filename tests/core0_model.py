#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Developer-only instruction model for the Core 0 candidate.

This is not a product dependency or a CTest requirement. It emulates only the
instruction subset emitted by fr30/src/core0.s, using objdump text, plus a
small fake for the core-local DMA/FIFO/packetizer registers. It is enough to
check boot, CTRL rejection, the bounded DMA stop, ring wrap, refill, one
192-byte send and the ten-unit writer-limit update without hardware.

It does not model the real FR30 timing, the TS engine, USB DMA/EP, the SDRAM
contents, HOLD/RUN, or the full ISA.

Usage:
  python3 tests/core0_model.py build/fr30-core0-test/core0.elf \
      [--objdump /path/to/fr30-elf-objdump]
"""
import argparse
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("elf", nargs="?", default="fr30/generated/core0.elf")
parser.add_argument("--objdump", default="objdump")
args = parser.parse_args()
text = subprocess.check_output([args.objdump, "-d", args.elf], text=True)
insns = {}
labels = {}
order = []
for line in text.splitlines():
    lm = re.match(r'^([0-9a-f]+) <([^>]+)>:', line)
    if lm:
        labels[lm.group(2)] = int(lm.group(1), 16)
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(.*?)\s*$', line)
    if not m:
        continue
    addr = int(m.group(1), 16)
    body = m.group(2)
    if body.startswith('...') or body == '':
        continue
    insns[addr] = body
    order.append(addr)
order = sorted(order)
for i, a in enumerate(order):
    insns[a] = (insns[a], order[i + 1] if i + 1 < len(order) else None)


class Mem:
    """Byte-addressed memory with the few core-local side effects we need."""

    def __init__(self):
        self.b = {}
        self.event = 0
        self.packetizer_busy = False
        self.stuck_dma = False
        self.refill_writes = []

    def rb(self, a):
        a &= 0xffffffff
        if 0x20 <= a <= 0x23:
            shift = (3 - (a - 0x20)) * 8
            return (self.event >> shift) & 0xff
        return self.b.get(a, 0) & 0xff

    def wb(self, a, v):
        a &= 0xffffffff
        if 0x20 <= a <= 0x23:
            shift = (3 - (a - 0x20)) * 8
            mask = 0xff << shift
            self.event = (self.event & ~mask) | ((v & 0xff) << shift)
            return
        self.b[a] = v & 0xff

    def rh(self, a):
        return (self.rb(a) << 8) | self.rb(a + 1)

    def wh(self, a, v):
        self.wb(a, (v >> 8) & 0xff)
        self.wb(a + 1, v & 0xff)

    def rw(self, a):
        return (self.rb(a) << 24) | (self.rb(a + 1) << 16) | \
            (self.rb(a + 2) << 8) | self.rb(a + 3)

    def ww(self, a, v):
        a &= 0xffffffff
        v &= 0xffffffff
        if a == 0x1068:
            if v == 0x300:
                self.ww_raw(a, v if self.stuck_dma else 0)
            elif v == 0x01000000:
                self.refill_writes.append(v)
                self.event |= 0x20000000
            else:
                self.ww_raw(a, v)
            return
        if a == 0x20:
            self.event &= ~v
            return
        if a == 0x2000:
            self.ww_raw(a, v)
            self.packetizer_busy = (v & 7) != 0
            return
        self.ww_raw(a, v)

    def ww_raw(self, a, v):
        for i in range(4):
            self.wb(a + i, (v >> (24 - 8 * i)) & 0xff)


class Cpu:
    def __init__(self, mem):
        self.m = mem
        self.r = [0] * 16
        self.pc = order[0]
        self.z = False
        self.n = False
        self.c = False
        self.v = False
        self.ret = []
        self.steps = 0
        self.writes = []

    def setz(self, v):
        self.z = (v & 0xffffffff) == 0

    def setn(self, v):
        self.n = ((v >> 31) & 1) != 0

    def getreg(self, s):
        return int(s.strip().lstrip('r'))

    def imm(self, s):
        s = s.strip()
        if s.startswith('#'):
            s = s[1:]
        return int(s, 0)

    def step(self):
        if self.pc not in insns:
            raise Exception('pc outside image: %x' % self.pc)
        body, nxt = insns[self.pc]
        self.steps += 1
        self.exec(body, nxt)

    def run(self, count):
        for _ in range(count):
            self.step()

    def exec(self, body, nxt):
        b = body.strip()
        parts = b.split(None, 1)
        op = parts[0].lower()
        a = parts[1] if len(parts) > 1 else ''
        if op.startswith('ldi:8'):
            imm, reg = a.split(',')
            self.r[self.getreg(reg)] = self.imm(imm) & 0xff
            self.pc = nxt
        elif op.startswith('ldi:20'):
            imm, reg = a.split(',')
            self.r[self.getreg(reg)] = self.imm(imm) & 0xfffff
            self.pc = nxt
        elif op.startswith('ldi:32'):
            imm, reg = a.split(',')
            self.r[self.getreg(reg)] = self.imm(imm) & 0xffffffff
            self.pc = nxt
        elif op == 'mov':
            src, reg = a.split(',')
            self.r[self.getreg(reg)] = self.r[self.getreg(src)]
            self.pc = nxt
        elif op == 'ld':
            src, reg = a.split(',')
            src = src.strip()
            if not src.startswith('@'):
                raise Exception('ld mode ' + src)
            src = src[1:]
            if src.endswith('+'):
                base = self.getreg(src[:-1])
                v = self.m.rw(self.r[base])
                self.r[base] = (self.r[base] + 4) & 0xffffffff
            elif src.startswith('-'):
                raise Exception('ld predecrement')
            else:
                v = self.m.rw(self.r[self.getreg(src)])
            self.r[self.getreg(reg)] = v
            self.pc = nxt
        elif op == 'ldub':
            src, reg = a.split(',')
            self.r[self.getreg(reg)] = self.m.rb(self.r[self.getreg(src.strip()[1:])])
            self.pc = nxt
        elif op == 'lduh':
            src, reg = a.split(',')
            self.r[self.getreg(reg)] = self.m.rh(self.r[self.getreg(src.strip()[1:])])
            self.pc = nxt
        elif op == 'st':
            reg, dst = a.split(',')
            dst = dst.strip()
            if not dst.startswith('@'):
                raise Exception('st mode ' + dst)
            dst = dst[1:]
            if dst.startswith('-'):
                base = self.getreg(dst[1:])
                self.r[base] = (self.r[base] - 4) & 0xffffffff
                self.m.ww(self.r[base], self.r[self.getreg(reg)])
                self.writes.append((self.r[base], 4))
            else:
                base = self.r[self.getreg(dst)]
                self.m.ww(base, self.r[self.getreg(reg)])
                self.writes.append((base, 4))
            self.pc = nxt
        elif op == 'stb':
            reg, dst = a.split(',')
            base = self.r[self.getreg(dst.strip()[1:])]
            self.m.wb(base, self.r[self.getreg(reg)])
            self.writes.append((base, 1))
            self.pc = nxt
        elif op == 'sth':
            reg, dst = a.split(',')
            base = self.r[self.getreg(dst.strip()[1:])]
            self.m.wh(base, self.r[self.getreg(reg)])
            self.writes.append((base, 2))
            self.pc = nxt
        elif op in ('add', 'sub', 'and', 'or'):
            x, reg = a.split(',')
            if x.strip().startswith('#') or re.match(r'^-?0x[0-9a-f]+$', x.strip()):
                value = self.imm(x)
            else:
                value = self.r[self.getreg(x)]
            r = self.getreg(reg)
            if op == 'add':
                res = (self.r[r] + value) & 0xffffffff
            elif op == 'sub':
                res = (self.r[r] - value) & 0xffffffff
            elif op == 'and':
                res = self.r[r] & value
            else:
                res = self.r[r] | value
            self.r[r] = res
            self.setz(res)
            self.setn(res)
            self.pc = nxt
        elif op == 'cmp':
            x, reg = a.split(',')
            if x.strip().startswith('#') or re.match(r'^-?0x[0-9a-f]+$', x.strip()):
                value = self.imm(x)
            else:
                value = self.r[self.getreg(x)]
            dst = self.r[self.getreg(reg)]
            res = (dst - value) & 0xffffffff
            self.setz(res)
            self.setn(res)
            self.c = dst < value
            self.v = (((dst ^ value) & 0x80000000) != 0 and
                      ((dst ^ res) & 0x80000000) != 0)
            self.pc = nxt
        elif op in ('lsl', 'lsr'):
            x, reg = a.split(',')
            shift = self.imm(x)
            r = self.getreg(reg)
            res = ((self.r[r] << shift) & 0xffffffff) if op == 'lsl' \
                else (self.r[r] >> shift)
            self.r[r] = res
            self.setz(res)
            self.setn(res)
            self.pc = nxt
        elif op in ('beq', 'bne', 'bc', 'bnc', 'blt', 'bge', 'bls', 'bhi', 'bra'):
            target = int(a.split()[0], 16)
            take = {
                'beq': self.z,
                'bne': not self.z,
                'bc': self.c,
                'bnc': not self.c,
                'blt': self.n != self.v,
                'bge': self.n == self.v,
                'bls': self.c or self.z,
                'bhi': (not self.c) and (not self.z),
                'bra': True,
            }[op]
            self.pc = target if take else nxt
        elif op == 'jmp':
            self.pc = self.r[self.getreg(a.strip()[1:])]
        elif op == 'call':
            target = int(a.split()[0], 16)
            self.ret.append(nxt)
            self.pc = target
        elif op == 'ret':
            if not self.ret:
                raise Exception('ret without call')
            self.pc = self.ret.pop()
        elif op == 'nop':
            self.pc = nxt
        else:
            raise Exception('unhandled ' + body)


CTRL = 0xA3840
STAT = 0xA3880
INFO = 0xA3800
WRITE_REG = 0xD02038
LIMIT_REG = 0xD0202C


def default_ctrl(over=None):
    values = {
        0x00: 0x5A3C0001,
        0x04: 0,
        0x08: 0,
        0x0C: 0,
        0x10: 7,
        0x14: 0x00AE4000,
        0x18: 0x00600000,
        0x1C: 192,
        0x20: WRITE_REG,
        0x24: LIMIT_REG,
        0x28: 8,
        0x2C: 10,
    }
    if over:
        values.update(over)
    return values


def boot(ctrl=None, write_offset=0x1000, stuck_dma=False, step_limit=400000):
    m = Mem()
    m.stuck_dma = stuck_dma
    for off, value in (ctrl or default_ctrl()).items():
        m.ww(CTRL + off, value)
    for off in range(0x00, 0x40, 4):
        m.ww(STAT + off, 0)
    m.ww(WRITE_REG, write_offset)
    c = Cpu(m)
    for _ in range(step_limit):
        state = m.rw(STAT)
        if state in (0x03, 0xE0):
            break
        c.step()
    if m.rw(STAT) == 0xE0:
        c.run(4)
    return c, m


def run_loop(m, c, limit=200000):
    """Run one full main-loop iteration, from main_loop back to main_loop."""
    start = labels['main_loop']
    for _ in range(limit):
        if c.pc == start:
            break
        c.step()
    else:
        return False
    c.step()
    for _ in range(limit):
        if c.pc == start:
            return True
        c.step()
    return False


def run_until(m, c, pred, limit=400000):
    for _ in range(limit):
        if pred():
            return True
        c.step()
    return False


def check_boot():
    c, m = boot()
    assert m.rw(STAT) == 0x03, hex(m.rw(STAT))
    assert m.rw(INFO) == 0x54545330, hex(m.rw(INFO))
    assert m.rw(INFO + 0x04) == 0x00010000
    assert m.rw(INFO + 0x08) != 0
    assert m.rw(INFO + 0x0C) == 1
    assert m.rw(INFO + 0x10) == 0x5A3C0001
    assert m.rw(INFO + 0x14) == 1332
    assert m.rw(INFO + 0x18) == 0x000A3800
    assert m.rw(INFO + 0x1C) == 0x840
    assert m.rw(STAT + 0x08) == 0 and m.rw(STAT + 0x18) == 7
    assert m.rw(STAT + 0x1C) == 0 and m.rw(STAT + 0x20) == 0
    assert m.rw(STAT + 0x28) == 0x1000 and m.rw(STAT + 0x2C) == 0x1000
    assert m.rw(STAT + 0x24) == 0 and m.rw(STAT + 0x30) == 0
    assert m.rw(0x1018) == 0x00AE4000 and m.rw(0x1028) == 0x00600000
    assert m.rw(0x1048) == 0x1000 and m.rw(0x1038) == 0x1000
    assert m.rw(0x10034) == 0x08000000  # halfword register in the upper half
    assert m.rw(0x10038) == 0x10000000
    assert len(m.refill_writes) == 1
    print('boot ok')


def check_bad_ctrl():
    cases = [
        ('session', default_ctrl({0x00: 0x00000001})),
        ('size-zero', default_ctrl({0x18: 0})),
        ('size-not-192', default_ctrl({0x18: 0x00600001})),
        ('unit', default_ctrl({0x1C: 188})),
        ('margin', default_ctrl({0x28: 0x00600000})),
        ('margin-room', default_ctrl({0x28: 0x00600000 - 8})),
        ('report', default_ctrl({0x2C: 0})),
        ('write-reg-zero', default_ctrl({0x20: 0})),
        ('write-reg-align', default_ctrl({0x20: 0xD0203A})),
        ('limit-reg-zero', default_ctrl({0x24: 0})),
        ('limit-reg-align', default_ctrl({0x24: 0xD0202E})),
    ]
    for name, ctrl in cases:
        c, m = boot(ctrl)
        assert m.rw(STAT) == 0xE0, (name, hex(m.rw(STAT)))
        assert m.rw(STAT + 0x08) == 1, (name, m.rw(STAT + 0x08))
    print('bad ctrl ok')


def check_dma_quiesce():
    c, m = boot(stuck_dma=True, step_limit=3000000)
    assert m.rw(STAT) == 0xE0, hex(m.rw(STAT))
    assert m.rw(STAT + 0x08) == 2, m.rw(STAT + 0x08)
    print('dma quiesce ok')


def check_wrap_and_watermark():
    c, m = boot()
    m.ww(WRITE_REG, 0x10)
    m.ww(0x1048, 0x1000)
    assert run_loop(m, c)
    assert m.rw(STAT + 0x28) == 0x10
    assert m.rw(STAT + 0x2C) == 0x1000
    assert m.rw(STAT + 0x30) == 0x5FF010, hex(m.rw(STAT + 0x30))
    assert m.rw(STAT + 0x34) == 0

    c, m = boot()
    m.ww(WRITE_REG, 0x5FFFF0)
    m.ww(0x1048, 0x10)
    assert run_loop(m, c)
    assert m.rw(STAT + 0x30) == 0x5FFFE0, hex(m.rw(STAT + 0x30))
    assert m.rw(STAT + 0x34) == 1, m.rw(STAT + 0x34)
    print('wrap/watermark ok')


def check_refill():
    c, m = boot()
    m.wh(0x10068, 0x100)
    m.wh(0x10058, 0)
    assert run_loop(m, c)
    assert len(m.refill_writes) == 1, m.refill_writes
    assert m.rw(STAT + 0x20) == 0, m.rw(STAT + 0x20)

    m.wh(0x10068, 0x600)
    m.wh(0x10058, 0)
    assert run_loop(m, c)
    assert len(m.refill_writes) == 2, m.refill_writes
    assert m.rw(STAT + 0x20) == 1, m.rw(STAT + 0x20)
    assert m.rw(0x1078) == 0x460

    c, m = boot()
    m.wh(0x10068, 0)
    m.wh(0x10058, 0)
    assert run_loop(m, c)
    assert len(m.refill_writes) == 2, m.refill_writes
    assert m.rw(STAT + 0x20) == 1, m.rw(STAT + 0x20)

    c, m = boot()
    m.wh(0x10068, 0x100)
    m.wh(0x10058, 0x800)
    assert run_loop(m, c)
    assert len(m.refill_writes) == 1, m.refill_writes
    assert m.rw(STAT + 0x20) == 0, m.rw(STAT + 0x20)
    print('refill ok')


def send_one(m, c):
    m.event |= 0x20
    m.ww(0x2000, 0)
    before = m.rw(STAT + 0x1C)
    assert run_loop(m, c), 'no loop'
    assert m.rw(STAT + 0x1C) == before + 1, 'no send'
    return m.rw(STAT + 0x1C)


def check_ten_unit_limit():
    c, m = boot()
    m.ww(WRITE_REG, 0x5000)
    m.ww(0x1048, 0x1000)
    m.wh(0x10068, 0x100)
    m.wh(0x10058, 0)
    for i in range(9):
        assert send_one(m, c) == i + 1
    assert m.rw(LIMIT_REG) == 0, hex(m.rw(LIMIT_REG))
    assert send_one(m, c) == 10
    assert m.rw(LIMIT_REG) == 0x0FF8, hex(m.rw(LIMIT_REG))
    print('ten-unit limit ok')


def check_limit_wrap():
    c, m = boot()
    m.ww(WRITE_REG, 0x5000)
    m.ww(0x1048, 0x4)
    m.wh(0x10068, 0x100)
    m.wh(0x10058, 0)
    for i in range(10):
        assert send_one(m, c) == i + 1
    assert m.rw(LIMIT_REG) == 0x5FFFFC, hex(m.rw(LIMIT_REG))
    print('limit wrap ok')


def check_nop_and_fault():
    c, m = boot()
    m.ww(CTRL + 0x04, 5)
    m.ww(CTRL + 0x08, 0)
    assert run_loop(m, c)
    assert m.rw(STAT + 0x10) == 5 and m.rw(STAT + 0x14) == 0
    m.ww(CTRL + 0x04, 6)
    m.ww(CTRL + 0x08, 1)
    assert run_loop(m, c)
    assert m.rw(STAT + 0x10) == 6 and m.rw(STAT + 0x14) == 1

    c, m = boot(default_ctrl({0x00: 0}))
    before = len([w for w in c.writes if w[0] == 0x1038])
    m.ww(WRITE_REG, 0x2000)
    for _ in range(3):
        run_loop(m, c)
    after = len([w for w in c.writes if w[0] == 0x1038])
    assert after == before, (before, after)
    print('nop/fault ok')


check_boot()
check_bad_ctrl()
check_dma_quiesce()
check_wrap_and_watermark()
check_refill()
check_ten_unit_limit()
check_limit_wrap()
check_nop_and_fault()
print('ALL OK')
