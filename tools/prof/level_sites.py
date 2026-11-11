r"""Every instruction of Cry3DEngine (build 6156) that accesses the global render recursion level 0x302008E0.

    level_sites.py                 list the sites, grouped by Ghidra function
    level_sites.py --header <out>  also write the C++ table CryMT uses to give each thread its own level: each site's
                                   original bytes and the same instruction re-encoded with a gs:[disp32] operand (the
                                   thread's TEB TlsSlots entry, disp patched at run time), checked by decoding it back.
Run with tools\venv\Scripts\python.exe (capstone)."""
import bisect
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_REG_RIP, X86_REG_INVALID, X86_REG_GS

DLL = r'D:\Games\Crysis\Bin64\Cry3DEngine.dll'
FUNCS = r'D:\Games\Crysis-RE\logs\dump\funcs\Cry3DEngine.dll.funcs.txt'
TARGET = 0x302008E0
d = open(DLL, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsize = struct.unpack_from('<H', d, pe + 20)[0]
stamp = struct.unpack_from('<I', d, pe + 8)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
rows = []
for l in open(FUNCS):
    p = l.split()
    if len(p) >= 3:
        rows.append((int(p[0], 16), int(p[1], 16), p[2]))
rows.sort()
starts = [r[0] for r in rows]


def fname(va):
    i = bisect.bisect_right(starts, va) - 1
    return '%x %s' % (rows[i][0], rows[i][2]) if i >= 0 else '?'


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
text = None
for i in range(nsec):
    name = d[pe + 24 + optsize + 40 * i: pe + 24 + optsize + 40 * i + 8].rstrip(b'\0')
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + optsize + 40 * i + 8)
    if name == b'.text':
        text = (va, rs, rp)
va0, rs0, rp0 = text

sites = {}    # va -> instruction
near = []     # other RIP-relative operands close to the level (to see what else lives there)


def scan(start_va, data):
    for ins in md.disasm(data, start_va):
        if ins.id == 0:
            continue
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                a = ins.address + ins.size + op.mem.disp
                if a <= TARGET < a + max(op.size, 1) and ins.mnemonic != 'lea':
                    sites[ins.address] = ins
                elif abs(a - TARGET) < 0x10:
                    near.append((ins.address, ins.mnemonic, ins.op_str, a))


# function by function (Ghidra table), then a linear sweep of .text, so a misaligned decode hides nothing
for fs, fsz, fn in rows:
    o = fs - base - va0 + rp0
    if rp0 <= o < rp0 + rs0:
        scan(fs, d[o:o + max(fsz, 1)])
scan(base + va0, d[rp0:rp0 + rs0])

for a in sorted(sites):
    if sites[a].size < 5:
        sys.exit('site %x shorter than a jmp' % a)

by_fn = {}
for a in sorted(sites):
    ins = sites[a]
    by_fn.setdefault(fname(a), []).append('%x %s %s' % (a, ins.mnemonic, ins.op_str))
for f in sorted(by_fn):
    print(f)
    for s in by_fn[f]:
        print('   ', s)
print('sites', len(sites), 'functions', len(by_fn))
for n in sorted(set(near)):
    print('near: %x %s %s -> %x' % n)


def reencode(ins):
    raw = bytes(ins.bytes)
    disp_at = ins.disp_offset
    modrm_at = disp_at - 1
    modrm = raw[modrm_at]
    if modrm & 0xC7 != 0x05:
        sys.exit('site %x: modrm %02x is not RIP-relative' % (ins.address, modrm))
    pre = bytearray(raw[:modrm_at])
    # REX (0x40-0x4F right before the opcode bytes): clear X, which would make SIB index 100 mean r12
    for k in range(len(pre)):
        if 0x40 <= pre[k] <= 0x4F:
            pre[k] &= ~0x02
    new = bytes([0x65]) + bytes(pre) + bytes([(modrm & 0xF8) | 0x04, 0x25]) + b'\0\0\0\0' + raw[disp_at + 4:]
    return new, 1 + len(pre) + 2


def check(ins, new, disp_at):
    probe = 0x1480 + 8 * 13
    code = new[:disp_at] + struct.pack('<I', probe) + new[disp_at + 4:]
    got = list(md.disasm(code, 0x1000))
    if len(got) != 1 or got[0].size != len(code):
        sys.exit('site %x: re-encoded bytes do not decode as one instruction' % ins.address)
    g = got[0]
    if g.mnemonic != ins.mnemonic or len(g.operands) != len(ins.operands):
        sys.exit('site %x: %s became %s' % (ins.address, ins.mnemonic, g.mnemonic))
    for a, b in zip(ins.operands, g.operands):
        if a.type != b.type or a.size != b.size:
            sys.exit('site %x: operand changed' % ins.address)
        if a.type == X86_OP_MEM:
            m = b.mem
            if m.segment != X86_REG_GS or m.base != X86_REG_INVALID or m.index != X86_REG_INVALID or m.disp != probe:
                sys.exit('site %x: memory operand is not gs:[%x]: %s' % (ins.address, probe, g.op_str))
        elif a.type == X86_OP_REG:
            if a.reg != b.reg:
                sys.exit('site %x: register operand changed' % ins.address)
        elif a.imm != b.imm:
            sys.exit('site %x: immediate changed' % ins.address)
    return g


if len(sys.argv) > 2 and sys.argv[1] == '--header':
    lines = []
    for a in sorted(sites):
        ins = sites[a]
        new, disp_at = reencode(ins)
        g = check(ins, new, disp_at)
        if len(new) > 16 or ins.size > 15:
            sys.exit('site %x: instruction too long' % a)
        lines.append('    {0x%X, %d, {%s}, %d, {%s}, %d}, // %s %s -> %s %s' % (
            a, ins.size, ', '.join('0x%02X' % b for b in ins.bytes), len(new), ', '.join('0x%02X' % b for b in new),
            disp_at, ins.mnemonic, ins.op_str, g.mnemonic, g.op_str))
    with open(sys.argv[2], 'w', newline='\r\n') as f:
        f.write('// Generated by tools\\prof\\level_sites.py --header (Cry3DEngine.dll build 6156, stamp 0x%08X): every\n'
                '// instruction that accesses the render recursion level 0x302008E0, and the same instruction reading the\n'
                '// thread\'s TEB TlsSlots entry instead (gs:[disp32]; the comments show it for slot 13, gs:[0x14e8]).\n'
                '#pragma once\n#include <cstdint>\n\n'
                'struct RenderLevelSite\n{\n    uint32_t va;\n    uint8_t len;\n    uint8_t orig[15];\n    uint8_t newLen;\n'
                '    uint8_t code[16];\n    uint8_t dispAt;\n};\n\n'
                'constexpr uint32_t kRenderLevelStamp = 0x%08X;\n'
                'constexpr RenderLevelSite kRenderLevelSites[] = {\n' % (stamp, stamp))
        f.write('\n'.join(lines) + '\n};\n')
    print('header written:', len(lines), 'sites')
