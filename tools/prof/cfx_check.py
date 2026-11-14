"""Compiles a CryFX declaration string of Settings.cpp (the shader patches) offline with fxc: sampler_state blocks become
plain samplers, half becomes float, D3D10 is defined, and a test entry point calls the given function.
  python tools/prof/cfx_check.py kWaterEnvDecl "CryMT_WaterReflection(float4(0,0,0,1), normalize(float3(0,0.3,1)), float3(0,0,1), p, CryMT_WaterDepthSampler)"
The entry's input is float4 p : TEXCOORD0; it returns float4. Exit code 1 when fxc fails."""
import os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'

name, call = sys.argv[1], sys.argv[2]
src = open(os.path.join(ROOT, 'src', 'crymt', 'settings', 'Settings.cpp'), encoding='utf-8').read()
m = re.search(r'const char\* const ' + re.escape(name) + r' = R"\((.*?)\)";', src, re.S)
if not m:
    sys.exit('no raw string ' + name)
code = re.sub(r'(sampler(?:2D|3D|CUBE)\s+\w+)\s*=\s*sampler_state\s*\{.*?\};', r'\1;', m.group(1), flags=re.S)
code = '#define D3D10 1\n#define half float\n#define half2 float2\n#define half3 float3\n#define half4 float4\n' + code
code += '\nfloat4 crymt_test(float4 p : TEXCOORD0) : SV_Target0\n{\n  return ' + call + ';\n}\n'
path = os.path.join(tempfile.gettempdir(), 'crymt_' + name + '.hlsl')
open(path, 'w', encoding='utf-8').write(code)
r = subprocess.run([FXC, '/nologo', '/T', 'ps_4_0', '/Gec', '/E', 'crymt_test', '/O3', '/Fo', os.devnull, path], capture_output=True, text=True)
out = (r.stdout + r.stderr).replace(path, name)
print(('ok   ' if r.returncode == 0 else 'FAIL ') + name)
for l in out.splitlines():
    if 'error' in l or 'warning X' in l:
        print('     ' + l)
sys.exit(1 if r.returncode else 0)
