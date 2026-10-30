#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Developer-only instruction model for the Core 1 candidate.

This is not a product dependency or a CTest requirement. It emulates only the
instruction subset emitted by fr30/src/core1.s, using objdump text, so that the
boot/INFO path and a few ring/status paths can be checked without hardware.
It does not model the real I2C timing, electrical behaviour, ROM, HALT/RUN,
or the full FR30 ISA.

Usage:
  python3 tests/core1_model.py build/fr30-core1-test/core1.elf \
      [--objdump /path/to/fr30-elf-objdump]
"""
import argparse
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("elf", nargs="?", default="fr30/generated/core1.elf")
parser.add_argument("--objdump", default="objdump")
args = parser.parse_args()
text = subprocess.check_output([args.objdump, "-d", args.elf], text=True)
insns={}
order=[]
for line in text.splitlines():
    m=re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(.*?)\s*$', line)
    if not m: continue
    addr=int(m.group(1),16); body=m.group(2)
    if body.startswith('...') or body=='' : continue
    insns[addr]=body; order.append(addr)
order=sorted(order)
for i,a in enumerate(order):
    insns[a]=(insns[a], order[i+1] if i+1<len(order) else None)

class Mem:
    def __init__(self):
        self.b={}; self.status=0; self.control=0; self.data=0; self.data_byte=0; self.nack_value=-1
    def rb(self,a):
        a&=0xffffffff
        if a==0xfd8fe0: return (self.status>>24)&0xff
        if a==0xfd8fe1: return (self.status>>16)&0xff
        if a==0xfd8fe2: return (self.status>>8)&0xff
        if a==0xfd8fe3: return self.status&0xff
        if a==0xfd8fe4: return (self.control>>24)&0xff
        if a==0xfd8fe5: return (self.control>>16)&0xff
        if a==0xfd8fe6: return (self.control>>8)&0xff
        if a==0xfd8fe7: return self.control&0xff
        if a==0xfd8ff0: return (self.data>>24)&0xff
        if a==0xfd8ff1: return (self.data>>16)&0xff
        if a==0xfd8ff2: return (self.data>>8)&0xff
        if a==0xfd8ff3: return self.data&0xff
        return self.b.get(a,0)&0xff
    def wb(self,a,v):
        a&=0xffffffff
        if a==0xfd8ff0: self.data=(v&0xff)<<24; return
        if a==0xfd8ff1: self.data=(self.data&0xff00ffff)|((v&0xff)<<16); return
        if a==0xfd8ff2: self.data=(self.data&0xffff00ff)|((v&0xff)<<8); return
        if a==0xfd8ff3: self.data=(self.data&0xffffff00)|(v&0xff); return
        if 0xfd8fe4<=a<=0xfd8fe7:
            # assemble control byte-wise, then act on the low byte
            shift=(3-(a-0xfd8fe4))*8
            mask=0xff<<shift
            self.control=(self.control&~mask)|((v&0xff)<<shift)
            if a==0xfd8fe7:
                c=self.control&0xff
                if c==0x40:
                    self.status=0; self.control=c
                else:
                    self.status=0x10 if self.data==self.nack_value else 0; self.control=c|1
                    if c in (0x50,0x58,0x70): self.data=0xa5a5a5a5
            return
        self.b[a]=v&0xff
    def rw(self,a):
        return (self.rb(a)<<24)|(self.rb(a+1)<<16)|(self.rb(a+2)<<8)|self.rb(a+3)
    def ww(self,a,v):
        for i in range(4): self.wb(a+i, v>>(24-8*i))

class Cpu:
    def __init__(self, mem):
        self.m=mem; self.r=[0]*16; self.pc=order[0]; self.z=False; self.n=False; self.c=False; self.v=False
        self.ret=[]; self.steps=0; self.writes=[]
    def setz(self,v): self.z=(v&0xffffffff)==0
    def setn(self,v): self.n=((v>>31)&1)!=0
    def getreg(self,s): return int(s.strip().lstrip('r'))
    def imm(self,s):
        s=s.strip()
        if s.startswith('#'): s=s[1:]
        return int(s,0)
    def run(self, stop_after=200000):
        while self.steps<stop_after:
            body,nxt=insns[self.pc]; self.steps+=1
            if self.exec(body,nxt): return True
        return False
    def exec(self,body,nxt):
        b=body.strip()
        # split mnemonic and operands
        parts=b.split(None,1)
        op=parts[0].lower(); args=parts[1] if len(parts)>1 else ''
        if op.startswith('ldi:8'):
            imm,reg=args.split(','); self.r[self.getreg(reg)]=self.imm(imm)&0xff; self.pc=nxt
        elif op.startswith('ldi:20'):
            imm,reg=args.split(','); self.r[self.getreg(reg)]=self.imm(imm)&0xfffff; self.pc=nxt
        elif op.startswith('ldi:32'):
            imm,reg=args.split(','); self.r[self.getreg(reg)]=self.imm(imm)&0xffffffff; self.pc=nxt
        elif op=='mov':
            a,r=args.split(','); self.r[self.getreg(r)]=self.r[self.getreg(a)]; self.pc=nxt
        elif op=='ld':
            a,r=args.split(','); a=a.strip()
            if a.startswith('@'):
                a=a[1:]
                if a.endswith('+'): # postincrement r15
                    reg=self.getreg(a[:-1]); v=self.m.rw(self.r[reg]); self.r[reg]=(self.r[reg]+4)&0xffffffff
                elif '-' in a: raise Exception('ld predec not used')
                else:
                    reg=self.getreg(a); v=self.m.rw(self.r[reg])
            else: raise Exception('ld mode '+a)
            self.r[self.getreg(r)]=v; self.pc=nxt
        elif op=='ldub':
            a,r=args.split(','); reg=self.getreg(a.strip()[1:]); self.r[self.getreg(r)]=self.m.rb(self.r[reg]); self.pc=nxt
        elif op=='st':
            r,a=args.split(','); a=a.strip()
            if a.startswith('@'):
                a=a[1:]
                if a.startswith('-'):
                    reg=self.getreg(a[1:]); self.r[reg]=(self.r[reg]-4)&0xffffffff; self.m.ww(self.r[reg],self.r[self.getreg(r)]); self.writes.append((self.r[reg],4))
                else:
                    reg=self.getreg(a); self.m.ww(self.r[reg],self.r[self.getreg(r)]); self.writes.append((self.r[reg],4))
            else: raise Exception('st mode '+a)
            self.pc=nxt
        elif op=='stb':
            r,a=args.split(','); reg=self.getreg(a.strip()[1:]); self.m.wb(self.r[reg],self.r[self.getreg(r)]); self.writes.append((self.r[reg],1)); self.pc=nxt
        elif op in ('add','sub','and','or'):
            a,b=args.split(',')
            if a.strip().startswith('#') or re.match(r'^-?0x[0-9a-f]+$',a.strip()) or a.strip().isdigit():
                x=self.imm(a)
            else: x=self.r[self.getreg(a)]
            ri=self.getreg(b)
            if op=='add': res=(self.r[ri]+x)&0xffffffff
            elif op=='sub': res=(self.r[ri]-x)&0xffffffff
            elif op=='and': res=self.r[ri]&x
            else: res=self.r[ri]|x
            self.r[ri]=res; self.setz(res); self.setn(res); self.pc=nxt
        elif op=='cmp':
            a,b=args.split(',')
            if a.strip().startswith('#') or re.match(r'^-?0x[0-9a-f]+$',a.strip()):
                x=self.imm(a)
            else: x=self.r[self.getreg(a)]
            src=x; dst=self.r[self.getreg(b)]; res=(dst-src)&0xffffffff
            self.setz(res); self.setn(res); self.c=(dst<src); self.v=(((dst^src)&0x80000000)!=0 and ((dst^res)&0x80000000)!=0)
            self.pc=nxt
        elif op in ('lsl','lsr'):
            a,b=args.split(','); x=self.imm(a); ri=self.getreg(b)
            res=((self.r[ri]<<x)&0xffffffff) if op=='lsl' else (self.r[ri]>>x)
            self.r[ri]=res; self.setz(res); self.setn(res); self.pc=nxt
        elif op in ('beq','bne','blt','bge','bhi','bls','bra'):
            target=int(args.split()[0],16)
            take={'beq':self.z,'bne':not self.z,'blt':self.n!=self.v,'bge':self.n==self.v,'bhi': ((not self.c) and (not self.z)),'bls':(self.c or self.z),'bra':True}[op]
            self.pc=target if take else nxt
        elif op=='jmp':
            reg=self.getreg(args.strip()[1:]); self.pc=self.r[reg]
        elif op=='call':
            target=int(args.split()[0],16); self.ret.append(nxt); self.pc=target
        elif op=='ret':
            if not self.ret: raise Exception('ret without call')
            self.pc=self.ret.pop()
        elif op=='nop':
            self.pc=nxt
        else:
            raise Exception('unhandled '+body)
        return False


BASE_TAG=0x5A3C0000
def new_cpu(base=0):
    m=Mem(); m.ww(0xA3840,0x5A3C0001); m.ww(0xA3844,1); m.ww(0xA3848,0); m.ww(0xA384C,0); m.ww(0xA3850,base)
    c=Cpu(m)
    for _ in range(300000):
        c.steps=0; c.run(1)
        if m.rw(0xA3800)==0x54493243 and m.rw(0xA3890)==1 and m.rw(0xA3884)>0: break
    assert m.rw(0xA3800)==0x54493243 and m.rw(0xA3880)==2
    assert m.rw(0xA3804)==0x00010000
    assert m.rw(0xA380C)==1
    assert m.rw(0xA3810)==0x5A3C0001
    assert m.rw(0xA3818)==0xA3800
    assert m.rw(0xA381C)==0x40000
    assert m.rw(0xA3890)==1 and m.rw(0xA3894)==0
    assert m.rw(0xA3898)==base and m.rw(0xA389C)==base
    return c,m

def load_req(m,slot,tag,op,lengths,wdata,tag_end=None):
    a=0xA3900+64*slot
    m.ww(a,tag); m.ww(a+4,op); m.ww(a+8,lengths)
    for i,b in enumerate(wdata): m.wb(a+0x10+i,b)
    m.ww(a+0x3C, tag if tag_end is None else tag_end)

def run_to_progress(c,m,want):
    for _ in range(400000):
        c.steps=0; c.run(1)
        if m.rw(0xA389C)==want: return True
    return False

def completion(m,slot):
    a=0xA3B00+64*slot
    return [m.rw(a),m.rw(a+4),m.rw(a+8),m.rw(a+0xC),m.rw(a+0x3C),m.rb(a+0x10),m.rb(a+0x11),m.rb(a+0x12),m.rb(a+0x13)]

# WRITE / READ / WRITE_READ / 32B
for name,op,lengths,wdata,slot,expect_rlen,expect_acked in [
    ('write2',0x00120001,0x00000002,[0x03,0x80],0,0,2),
    ('read2',0x00120002,0x00000200,[],0,2,0),
    ('write_read',0x00120003,0x00000202,[0x03,0x80],0,2,2),
    ('write32',0x00120001,0x00000020,list(range(32)),0,0,32),
    ('read32',0x00120002,0x00002000,[],0,32,0),
]:
    c,m=new_cpu(); load_req(m,slot,BASE_TAG|slot,op,lengths,wdata); m.ww(0xA3850,1)
    assert run_to_progress(c,m,1), name
    st=m.rw(0xA3B04)
    assert (st&0xff)==0 and ((st>>8)&0xff)==0, (name,hex(st))
    assert ((st>>16)&0xff)==expect_acked, (name,hex(st))
    assert ((st>>24)&1)==1, (name,hex(st))
    assert m.rw(0xA3B08)==expect_rlen, name
    assert m.rw(0xA3B00)==BASE_TAG, name
    assert m.rw(0xA3B00+0x3C)==BASE_TAG, name
    if name=='read2': assert m.rb(0xA3B10)==0xA5
    if name=='read32': assert all(m.rb(0xA3B10+i)==0xA5 for i in range(32))
    print(name,'ok',hex(st))

# TORN: tag_end wrong, no bus activity
c,m=new_cpu(); load_req(m,0,BASE_TAG,0x00120001,0x00000002,[0x03,0x80],tag_end=BASE_TAG^1); m.ww(0xA3850,1)
assert run_to_progress(c,m,0x80000001); st=m.rw(0xA3B04)
assert (st&0xff)==0x12 and m.rw(0xA3880)==4 and m.rw(0xA38A0)==0, hex(st)
print('torn ok',hex(st))

# ADDR_NACK and DATA_NACK
for name,nack,op,lengths,wdata,expect_code,expect_stage in [
    ('addr_nack',0x12,0x00120001,0x00000002,[0x03,0x80],1,1),
    ('data_nack',0x03,0x00120001,0x00000002,[0x03,0x80],2,2),
]:
    c,m=new_cpu(); m.nack_value=nack; load_req(m,0,BASE_TAG,op,lengths,wdata); m.ww(0xA3850,1)
    assert run_to_progress(c,m,0x80000001), name
    st=m.rw(0xA3B04)
    assert (st&0xff)==expect_code and ((st>>8)&0xff)==expect_stage and m.rw(0xA3880)==4, (name,hex(st))
    print(name,'ok',hex(st))

# CONTINUE after ADDR_NACK, then process the second request
c,m=new_cpu(); m.nack_value=0x12
load_req(m,0,BASE_TAG,0x00120001,0x00000002,[0x03,0x80])
load_req(m,1,BASE_TAG|1,0x00140001,0x00000001,[0x11])
m.ww(0xA3850,2)
assert run_to_progress(c,m,0x80000001)
m.ww(0xA3858,0x01010000)  # serial=1, action=CONTINUE, index=0
ok=run_to_progress(c,m,2)
if not ok:
    print('DEBUG continue state',hex(m.rw(0xA3880)),'cons',hex(m.rw(0xA3898)),'progress',hex(m.rw(0xA389C)),'pc',hex(c.pc))
    print('SCR', [hex(m.rw(0xA3000+4*i)) for i in range(11)])
    print('ctrl',hex(m.control),'status',hex(m.status),'data',hex(m.data))
    print('req1', [hex(m.rw(0xA3940+4*i)) for i in range(5)])
    raise SystemExit(1)
st=m.rw(0xA3B40+4)
assert (st&0xff)==0 and ((st>>16)&0xff)==1, hex(st)
assert m.rw(0xA3880)==2 and m.rw(0xA38B0)==0x01010000
print('continue ok',hex(st))

# DISCARD skips the pending second request without bus activity
c,m=new_cpu(); m.nack_value=0x12
load_req(m,0,BASE_TAG,0x00120001,0x00000002,[0x03,0x80])
load_req(m,1,BASE_TAG|1,0x00140001,0x00000001,[0x11])
m.ww(0xA3850,2)
assert run_to_progress(c,m,0x80000001)
before=len([w for w in c.writes if 0xfd8fe0 <= w[0] <= 0xfd8fff])
m.ww(0xA3858,0x01020000)  # serial=1, action=DISCARD, index=0
for _ in range(400000):
    c.steps=0; c.run(1)
    if m.rw(0xA3880)==2 and m.rw(0xA389C)==2: break
assert m.rw(0xA389C)==2 and m.rw(0xA3880)==2
st=m.rw(0xA3B40+4)
assert (st&0xff)==0x11 and m.rw(0xA3B40)==(BASE_TAG|1)
after=len([w for w in c.writes if 0xfd8fe0 <= w[0] <= 0xfd8fff])
assert after==before, (after,before)
print('discard ok',hex(st))


# Non-zero base: T_ret=0 is the base wire value, slot is base mod 8.
base_wire=0x1234
c,m=new_cpu(base_wire)
slot=base_wire & 7
tag=0x5A3C0000|base_wire
load_req(m,slot,tag,0x00120001,0x00000001,[0x55])
m.ww(0xA3850,(base_wire+1)&0xFFFF)
assert run_to_progress(c,m,(base_wire+1)&0xFFFF)
st=m.rw(0xA3B00+64*slot+4)
assert (st&0xff)==0 and m.rw(0xA3B00+64*slot)==tag
print('base ok',hex(st))


# INVALID opcode: no bus activity, zero acked/rlen/polls.
c,m=new_cpu(); load_req(m,0,BASE_TAG,0x00120004,0x00000001,[0x55]); m.ww(0xA3850,1)
before=len([w for w in c.writes if 0xfd8fe0 <= w[0] <= 0xfd8fff])
assert run_to_progress(c,m,0x80000001)
st=m.rw(0xA3B04)
after=len([w for w in c.writes if 0xfd8fe0 <= w[0] <= 0xfd8fff])
assert (st&0xff)==0x10 and ((st>>16)&0xff)==0 and m.rw(0xA3B08)==0 and after==before
print('invalid ok',hex(st))


# F1: DISCARD freezes the producer and keeps HALTED/ack closed until all
# fixed-range SKIPPED completions are published. A ticket published during the
# loop must not be skipped.
c,m=new_cpu(); m.nack_value=0x12
load_req(m,0,BASE_TAG,0x00120001,0x00000002,[0x03,0x80])
load_req(m,1,BASE_TAG|1,0x00140001,0x00000001,[0x11])
load_req(m,2,BASE_TAG|2,0x00140001,0x00000001,[0x12])
load_req(m,3,BASE_TAG|3,0x00140001,0x00000001,[0x13])
m.ww(0xA3850,3)
assert run_to_progress(c,m,0x80000001)
m.ww(0xA3858,0x01020000)  # serial=1, action=DISCARD, index=0
# Stop after the first SKIPPED publication (cons=2).
assert run_to_progress(c,m,0x80000002), 'first skip did not keep HALTED'
assert m.rw(0xA38B0)==0, 'resume_ack published before DISCARD completed'
assert m.rw(0xA3880)==4, 'state left HALTED during DISCARD'
# A compliant host waits, but publish here to prove the frozen range is used.
m.ww(0xA3850,4)
assert run_to_progress(c,m,4)
st=m.rw(0xA3B00+64*3+4)
assert (st&0xff)==0 and ((st>>16)&0xff)==1, ('new ticket was not executed',hex(st))
assert m.rw(0xA3B00+64*3)==(BASE_TAG|3)
assert m.rw(0xA38B0)==0x01020000 and m.rw(0xA3880)==2
print('discard-f1 ok',hex(st))

print('ALL OK')
