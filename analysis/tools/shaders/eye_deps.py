"""Counts used in stereo-single-pass-evidence.md:
 (1) VS/DS whose NON-position outputs depend on cb12 rows that Fallout 4 VR made per-eye
 (2) PS/CS that read per-eye cb12 rows and/or SV_Position.xy
usage: python -I eye_deps.py <set_dir>   (needs vs_pos.json from vs_pos.py)
"""
import json, os, re, sys
from collections import Counter
d = sys.argv[1]
EYE = set(range(4, 12)) | set(range(20, 28)) | set(range(31, 41))   # flat rows that became eye-indexed in VR (cbmap.py)
res = json.load(open(os.path.join(d, 'vs_pos.json')))
c = Counter(); rows = Counter()
for r in res:
    hit = set()
    for k, v in r['other_cb'].items():
        h = {x for x in v.get('cb12', []) if isinstance(x, int) and x in EYE}
        if h:
            hit.add(k); rows.update(h)
    pos = r['pos_cb'] or {}
    pe = any(isinstance(x, int) and x in EYE for x in pos.get('cb12', []))
    c[('pos uses per-eye cb12' if pe else 'pos: no per-eye cb12', 'other outputs use per-eye cb12' if hit else '-')] += 1
for k, v in sorted(c.items()):
    print(v, k)
print('cb12 rows feeding non-position outputs:', sorted(rows.items()))
idx = json.load(open(os.path.join(d, 'index.json')))
c = Counter(); u = Counter(); seen = set()
for s in idx:
    if s['stage'] not in ('ps', 'cs'):
        continue
    t = open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()
    eye = {int(x) for x in re.findall(r'cb12\[(\d+)\]', t)} & EYE
    body = t.split('dcl_temps')[-1] if 'dcl_temps' in t else t
    posused = bool(re.search(r'dcl_input_ps_siv linear noperspective v0\.\w+, position', t)) and bool(re.search(r'[ ,-]v0\.[xy]', body))
    k = (s['stage'], 'mat' if s['group'] <= 11 else 'other', 'per-eye cb12' if eye else '-', 'SV_Position.xy' if posused else '-')
    c[k] += 1
    if s['sha1'] not in seen:
        u[k] += 1; seen.add(s['sha1'])
for k in sorted(c):
    print('%5d (unique %4d) %s' % (c[k], u[k], k))
