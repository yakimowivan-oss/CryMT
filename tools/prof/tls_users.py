"""List functions (by .pdata) that read the TLS slot array via gs:[0x58]: tls_users.py <dll>"""
import struct, sys
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]; optsize = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24; base = struct.unpack_from("<Q", d, opt + 24)[0]
pr, ps = struct.unpack_from("<II", d, opt + 112 + 3 * 8); tr, ts = struct.unpack_from("<II", d, opt + 112 + 9 * 8)
secs = [struct.unpack_from("<IIII", d, opt + optsize + 40 * i + 8) for i in range(nsec)]
def off(r): return next(r - va + rp for vs, va, rs, rp in secs if va <= r < va + max(vs, rs))
print("tls dir rva", hex(tr), "size", ts)
t = struct.unpack_from("<QQQQII", d, off(tr)) if tr else None
if t: print(f"TLS template {t[0]:#x}..{t[1]:#x} ({t[1]-t[0]} bytes) zerofill={t[4]} index@{t[2]:#x}")
funcs = [struct.unpack_from("<III", d, off(pr) + i) for i in range(0, ps, 12)]
md = Cs(CS_ARCH_X86, CS_MODE_64)
hits = []
for b, e, _ in funcs:
    code = d[off(b):off(b) + (e - b)]
    if b"\x65\x48\x8b" not in code and b"\x65\x4c\x8b" not in code:
        continue
    for ins in md.disasm(code, base + b):
        if "gs:[0x58]" in ins.op_str:
            hits.append(base + b); break
print(len(hits), "functions:", " ".join(f"{h - base:X}" for h in hits))
