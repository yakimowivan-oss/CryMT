# Compiles CryMT's compute shaders (every k...Shader string in D3D12Backend.cpp: kGiShader, kVoxShader) with
# d3dcompiler_47, as the back end does at run time, so errors show up before a game start. Usage: python gishader.py
import ctypes
import re
import sys

src = open(r'D:\Games\Crysis-RE\src\crymt\D3D12Backend.cpp', encoding='utf-8').read()
shaders = re.findall(r'const char\* const (k\w+Shader) = R"\((.*?)\)";', src, re.S)

d3d = ctypes.WinDLL('d3dcompiler_47.dll')
BlobPtr = ctypes.POINTER(ctypes.c_void_p)
failed = 0
for name, text in shaders:
    hlsl = text.encode()
    for entry in re.findall(r'\[numthreads[^\]]*\]\s*void (\w+)\(', text):
        code = ctypes.c_void_p()
        errors = ctypes.c_void_p()
        hr = d3d.D3DCompile(hlsl, len(hlsl), name.encode(), None, None, entry.encode(), b'cs_5_0', 1 << 15, 0,
                            ctypes.byref(code), ctypes.byref(errors))
        if hr != 0:
            failed += 1
            vtbl = ctypes.cast(ctypes.cast(errors, BlobPtr)[0], ctypes.POINTER(ctypes.c_void_p))
            get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
            print(f'{name} {entry}: FAILED\n{ctypes.string_at(get_ptr(errors)).decode(errors="replace")}')
        else:
            print(f'{name} {entry}: ok')
sys.exit(1 if failed else 0)
