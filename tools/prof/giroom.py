# GI checks on a timedemo, from gidemo captures of 4 consecutive frames per point (gd_<variant>_w<point>_c<1..4>.ppm).
# The demo replays the same camera per frame in every run, so on - off is the GI's effect E. Per point: mean brightness
# of off and on, the darkening (1 - on/off), and the flicker: mean |E_t - E_t-1| over the 3 frame pairs relative to mean
# |E| (camera motion adds a little too).
# Usage: giroom.py [--labels "a,b,..."] <variant> [<variant2> ...] (each against "off"); without labels the village
# girooms points when there are 9, else the point numbers.
import glob
import sys
import numpy as np
from PIL import Image

VILLAGE = ["shed in 500", "entrance 1000", "corridor 1250", "room 1500", "room 1750", "corridor 2000", "room 2250",
           "room 2500", "windows 2750"]
D = r"D:\Games\Crysis-RE\logs\play\gd_%s_w%d_c%d.ppm"


def load(v, w):
    return [np.asarray(Image.open(D % (v, w, c))).astype(np.float32).mean(axis=2) for c in range(1, 5)]


args = sys.argv[1:]
labels = None
if args and args[0] == "--labels":
    labels = args[1].split(",")
    args = args[2:]
variants = args
points = sorted(int(f.split("_w")[1].split("_")[0]) for f in glob.glob(D.replace("%s", "off").replace("%d", "*", 1).replace("%d", "4")))
if labels is None:
    labels = VILLAGE if len(points) == 9 else ["point %d" % w for w in points]
print("%-15s %6s | " % ("point", "off") + " | ".join("%-25s" % ("%s: on, dark%%, flicker%%" % v) for v in variants))
sums = {v: [0.0, 0.0] for v in variants}
for w, name in zip(points, labels):
    o = load("off", w)
    cells = []
    for v in variants:
        on = load(v, w)
        E = [a - b for a, b in zip(on, o)]
        flick = np.mean([np.abs(E[i] - E[i - 1]).mean() for i in range(1, 4)]) / max(np.mean([np.abs(e).mean() for e in E]), 1e-3)
        dark = 1 - np.mean([a.mean() for a in on]) / np.mean([b.mean() for b in o])
        sums[v][0] += dark
        sums[v][1] += flick
        cells.append("%6.1f %6.1f %8.1f       " % (np.mean([a.mean() for a in on]), dark * 100, flick * 100))
    print("%-15s %6.1f | " % (name, np.mean([b.mean() for b in o])) + " | ".join(cells))
print("%-15s %6s | " % ("mean", "") + " | ".join("%6s %6.1f %8.1f       " % ("", sums[v][0] / len(points) * 100, sums[v][1] / len(points) * 100) for v in variants))
