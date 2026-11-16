"""Infer how constant-buffer rows moved from flat 1.10 to VR by aligning paired shaders' instructions.

usage: python -I cbmap.py <flat_set_dir> <vr_set_dir> <slot> [<slot> ...]
Pairs shaders by name (gNNN_stage_id) -- valid because group ordinals 0..52 match and later
groups are offset; we only use the material groups 0..11 + exact-name pairs whose stage/id match.
"""
import difflib, json, os, re, sys
from collections import Counter, defaultdict

fa, fb = sys.argv[1], sys.argv[2]
slots = [int(x) for x in sys.argv[3:]]
ia = {s['name']: s for s in json.load(open(os.path.join(fa, 'index.json')))}
ib = {s['name']: s for s in json.load(open(os.path.join(fb, 'index.json')))}


def insts(path):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('//') or line.startswith('dcl_') or line[0] in '{}':
            continue
        out.append(line)
    return out


refre = {s: re.compile(r'cb%d\[(?:(r\d+\.[xyzw]) \+ )?(\d+)\]' % s) for s in slots}
maps = {s: defaultdict(Counter) for s in slots}
npairs = 0
for name in sorted(ia.keys() & ib.keys()):
    g = int(name[1:4])
    if g > 11:
        continue
    A = insts(os.path.join(fa, 'disasm', name + '.txt'))
    B = insts(os.path.join(fb, 'disasm', name + '.txt'))
    opa = [l.split()[0] for l in A]
    opb = [l.split()[0] for l in B]
    sm = difflib.SequenceMatcher(None, opa, opb, autojunk=False)
    npairs += 1
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != 'equal':
            continue
        for k in range(i2 - i1):
            la, lb = A[i1 + k], B[j1 + k]
            for s in slots:
                ra = refre[s].findall(la)
                rb = refre[s].findall(lb)
                if len(ra) == len(rb):
                    for x, y in zip(ra, rb):
                        if x[0]:
                            continue  # already dynamic in flat
                        key = int(x[1])
                        val = ('eye+' if y[0] else '') + y[1]
                        maps[s][key][val] += 1
print('paired shaders used:', npairs)
for s in slots:
    print('cb%d  flat row -> VR row (votes)' % s)
    for row in sorted(maps[s]):
        c = maps[s][row]
        tot = sum(c.values())
        top = c.most_common(3)
        print('  %3d -> %-10s %5.1f%%  (n=%d) %s' % (row, top[0][0], 100 * top[0][1] / tot, tot,
                                                   '' if len(top) == 1 else 'alt ' + str(top[1:])))
