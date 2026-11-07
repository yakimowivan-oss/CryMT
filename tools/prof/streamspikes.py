# Frames of a stream capture (CRYMT_CAPTURE_STREAM) whose picture changes from the frame before by more than a threshold
# of pixels (difference over 30, the info text rows left out) after a world frame: one-frame flicker on a static camera.
# Usage: streamspikes.py <stream.raw> <from world frame> <threshold pixels> <tag>
import struct
import sys
import numpy as np

path, start, threshold, tag = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
d = open(path, "rb").read()
w, h = struct.unpack_from("<II", d, 0)
size = 4 + w * h * 3
n = (len(d) - 8) // size
prev, prevWorld, spikes, counted = None, None, [], 0
for i in range(n):
    o = 8 + i * size
    world = struct.unpack_from("<I", d, o)[0]
    g = np.frombuffer(d, np.uint8, w * h * 3, o + 4).reshape(h, w, 3).astype(np.float32).mean(axis=2)
    if prev is not None and world > start and world != prevWorld:
        diff = np.abs(g - prev)
        diff[: int(h * 0.11)] = 0
        k = int((diff > 30).sum())
        counted += 1
        if k > threshold:
            spikes.append("%d:%d" % (world, k))
    prev, prevWorld = g, world
print("%s: %d frames checked, flicker frames %d: %s" % (tag, counted, len(spikes), " ".join(spikes)))
