r"""Compile a raw-string shader of D3D12Backend.cpp offline with d3dcompiler_47 (as the DLL does at runtime: cs_5_0,
optimization level 3) and print the errors and warnings, or OK with the size, per entry.
  python tools\prof\hlsl_check.py <source.cpp> <kGiShader> <entry> [<entry> ...]
"""
import ctypes
import re
import sys

src_path, name = sys.argv[1], sys.argv[2]
entries = sys.argv[3:]
text = open(src_path, encoding='utf-8').read()
m = re.search(r'const char\* const ' + re.escape(name) + r' = R"\((.*?)\)";', text, re.S)
if not m:
    raise SystemExit('%s not found' % name)
code = m.group(1).encode('utf-8')

d3d = ctypes.WinDLL('d3dcompiler_47.dll')


class Blob(ctypes.Structure):
    pass


BlobPtr = ctypes.POINTER(ctypes.c_void_p)


def blob_text(p):
    if not p:
        return ''
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(get_ptr(p), get_size(p)).decode('utf-8', 'replace')


def blob_size(p):
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(p)


d3d.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                           ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
d3d.D3DCompile.restype = ctypes.c_long
failed = 0
for e in entries:
    out, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = d3d.D3DCompile(code, len(code), name.encode(), None, None, e.encode(), b'cs_5_0', 1 << 15, 0, ctypes.byref(out), ctypes.byref(err))
    msg = blob_text(err.value)
    if hr < 0:
        failed += 1
        print('%s: FAILED\n%s' % (e, msg))
    else:
        print('%s: OK, %d bytes%s' % (e, blob_size(out.value), ('\n' + msg) if msg.strip() else ''))
sys.exit(1 if failed else 0)
