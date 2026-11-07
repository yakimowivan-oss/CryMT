# Share of magenta pixels (R and B clearly above G) in screenshots, below the debug text: GI test check.
# Usage: python magenta.py <png>...
import sys

from PIL import Image

for path in sys.argv[1:]:
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    total = hits = 0
    for y in range(80, h - 40, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            total += 1
            hits += r > g * 1.25 + 8 and b > g * 1.25 + 8
    print(f'{path}: magenta pixels {100.0 * hits / total:.1f}%')
