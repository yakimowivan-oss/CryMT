import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, r'D:\Games\Crysis-RE\tools\prof')
from dlaa_seq import read_f16, tone
# seqcrop.py <dir> <tag> x y w h <out png> [frames]: input (left) and DLAA output (right) of the crop for the frames
folder, tag = sys.argv[1], sys.argv[2]
x, y, w, h = map(int, sys.argv[3:7])
out = sys.argv[7]
frames = list(map(int, sys.argv[8].split(','))) if len(sys.argv) > 8 else [0, 3, 6]
rows = []
for n in frames:
    a = tone(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_in.f16'), 4))[y:y + h, x:x + w]
    b = tone(read_f16(os.path.join(folder, f'{tag}_mv{n:02d}_out.f16'), 4))[y:y + h, x:x + w]
    rows.append(np.concatenate([a, np.ones((h, 4, 3)), b], axis=1))
img = np.concatenate(rows, axis=0)
Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out)
