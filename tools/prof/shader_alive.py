"""Whether the materials were shaded (a broken shader patch: the engine skips the shader and draws them flat, unlit):
the luminance spread on the first-person weapon (lower right of the frame) of a session_spots capture.
  python tools/prof/shader_alive.py <spot> <variant>   ->  "shaded" / "FLAT" and the spread"""
import sys
import numpy as np

spot, variant = sys.argv[1], sys.argv[2]
f = open(rf'D:\Games\Crysis-RE\logs\spot\out_{spot}_{variant}_frame.ppm', 'rb')
f.readline()
line = f.readline()
while line.startswith(b'#'):
    line = f.readline()
w, h = map(int, line.split())
f.readline()
im = np.frombuffer(f.read(), np.uint8).reshape(h, w, 3).astype(float)
crop = im[int(h * 0.62):h, int(w * 0.55):int(w * 0.8)] @ [0.2126, 0.7152, 0.0722]
spread = float(crop.std())
print(('shaded' if spread > 40 else 'FLAT') + f' (weapon luminance spread {spread:.1f})')
