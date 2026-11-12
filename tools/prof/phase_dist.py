r"""Per-frame distribution of one crymt_detail.txt column for spot runs (is a cost paid every frame or now and then?).
phase_dist.py <dir> <column> <tag_variant> [<tag_variant> ...]   e.g. phase_dist.py logs\spot rtPreproc bar_full wup_full"""
import os
import sys

d, col = sys.argv[1], sys.argv[2]
for tv in sys.argv[3:]:
    lines = open(os.path.join(d, tv + '_crymt_detail.txt')).read().splitlines()
    names = lines[0].split()[1:]
    k = names.index(col)
    vals = [float(l.split()[1:][k]) for l in lines[1:]][-900:]
    s = sorted(vals)
    print('%-20s %s median %.2f p10 %.2f p90 %.2f mean %.2f; frames over 2 ms %d of %d; first 30: %s' % (
        tv, col, s[len(s) // 2], s[len(s) // 10], s[len(s) * 9 // 10], sum(vals) / len(vals), sum(v > 2 for v in vals), len(vals),
        ' '.join('%.1f' % v for v in vals[:30])))
