r"""Compiles a piece of HLSL with the game's compiler (d3dcompiler_47, D3DCompile) to catch errors before the engine (which
does not log shader compile errors). The source: a C++ raw string from a CryMT source file (between `const char* const
<name> = R"(` and `)";`), with CryFX's sampler_state blocks reduced to plain samplers, then a stub prefix and a test entry.
fxc_check.py <cpp file> <raw string name> <target, e.g. ps_4_0> <prefix file or -> <entry source file>"""
import ctypes
import re
import sys

cpp, name, target, prefix_file, entry_file = sys.argv[1:6]
text = open(cpp, encoding='utf-8').read()
m = re.search(r'const char\* const ' + re.escape(name) + r' = R"\((.*?)\)";', text, re.S)
if not m:
    sys.exit('raw string %s not found' % name)
body = m.group(1)
body = body.replace('#if D3D10', '#if 1')
body = re.sub(r'(sampler2D|samplerCUBE)\s+(\w+)\s*=\s*sampler_state\s*\{.*?\};', r'\1 \2;', body, flags=re.S)
prefix = open(prefix_file).read() if prefix_file != '-' else ''
src = prefix + '\n' + body + '\n' + open(entry_file).read()
d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
blob = ctypes.c_void_p()
err = ctypes.c_void_p()
D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY = 1 << 12
data = src.encode()
hr = d3dc.D3DCompile(data, len(data), b'check.hlsl', None, None, b'main', target.encode(), D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0,
                     ctypes.byref(blob), ctypes.byref(err))


def blob_bytes(b):
    if not b.value:
        return b''
    vt = ctypes.cast(ctypes.cast(b, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(get_ptr(b), get_size(b))


msg = blob_bytes(err).decode(errors='replace')
print('compile %s %s: %s, %d bytes' % (name, target, 'ok' if hr == 0 else 'FAILED hr=%x' % (hr & 0xffffffff), len(blob_bytes(blob))))
if msg:
    print(msg)
sys.exit(0 if hr == 0 else 1)
