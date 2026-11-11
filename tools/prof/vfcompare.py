r"""Compare a virtual function between builds of the same engine (Crysis, Warhead, Wars): find the class's vtable through
MSVC RTTI (x64; VS2005 writes COL signature 0), take the slot, and report its size (.pdata), instruction count, calls,
RIP-relative globals and immediates, so a changed function stands out.
vfcompare.py <dll name> <class> <slot> [<class> <slot> ...]   (games: D:\Games\Crysis, ...\Crysis Warhead, ...\Crysis Wars)
Run with tools\venv\Scripts\python.exe (capstone)."""
import struct
import sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs

GAMES = [r'D:\Games\Crysis', r'D:\Games\Crysis Warhead', r'D:\Games\Crysis Wars']


class Pe:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsize = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.pdata = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
        self.secs = []
        for i in range(nsec):
            vs, va, rs, rp = struct.unpack_from('<IIII', d, opt + optsize + 40 * i + 8)
            fl = struct.unpack_from('<I', d, opt + optsize + 40 * i + 36)[0]
            self.secs.append((va, max(vs, rs), rp, rs, fl))
        self.funcs = []
        o = self.off(self.pdata[0])
        for i in range(0, self.pdata[1], 12):
            b, e, _ = struct.unpack_from('<III', d, o + i)
            self.funcs.append((b, e))
        self.funcs.sort()

    def off(self, rva):
        return next(rva - va + rp for va, size, rp, rs, fl in self.secs if va <= rva < va + size)

    def rva(self, off):
        return next(off - rp + va for va, size, rp, rs, fl in self.secs if rp <= off < rp + rs)

    def is_text(self, rva):
        return any(va <= rva < va + size and fl & 0x20 for va, size, rp, rs, fl in self.secs)

    def vtable(self, cls):
        name = b'\0.?AV' + cls.encode() + b'@@\0'
        n = self.d.find(name)
        if n < 0:
            return None
        td_rva = self.rva(n + 1 - 16)
        pat = struct.pack('<I', td_rva)
        o = self.d.find(pat)
        while o >= 0:
            if o >= 12:
                sig, off, cd = struct.unpack_from('<III', self.d, o - 12)
                if sig in (0, 1) and off == 0:
                    col_va = self.base + self.rva(o - 12)
                    ref = self.d.find(struct.pack('<Q', col_va))
                    if ref >= 0:
                        return self.rva(ref + 8)
            o = self.d.find(pat, o + 1)
        return None

    def slot(self, vt_rva, i):
        return struct.unpack_from('<Q', self.d, self.off(vt_rva) + 8 * i)[0] - self.base

    def func_end(self, rva):
        for b, e in self.funcs:
            if b == rva:
                return e
        return None


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
dll = sys.argv[1]
pairs = list(zip(sys.argv[2::2], (int(x) for x in sys.argv[3::2])))
pes = [Pe(g + '\\Bin64\\' + dll) for g in GAMES]
for cls, slot in pairs:
    print('%s::vf%d' % (cls, slot))
    for g, p in zip(GAMES, pes):
        vt = p.vtable(cls)
        if vt is None:
            print('   %-28s no vtable' % g.split('\\')[-1])
            continue
        f = p.slot(vt, slot)
        end = p.func_end(f)
        size = (end - f) if end else 0
        code = p.d[p.off(f):p.off(f) + (size or 0x400)]
        ins = list(md.disasm(code, p.base + f))
        calls = sum(1 for x in ins if x.mnemonic == 'call')
        jumps = sum(1 for x in ins if x.mnemonic.startswith('j'))
        print('   %-28s %x: %5d bytes, %4d instructions, %3d calls, %3d branches' % (g.split('\\')[-1], p.base + f, size, len(ins),
                                                                                    calls, jumps))
