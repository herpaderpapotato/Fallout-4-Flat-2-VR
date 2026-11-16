"""For every vertex (and domain) shader: which constant-buffer rows feed SV_Position.

usage: python -I vs_pos.py <set_dir> [--detail]
writes <set_dir>/vs_pos.json
"""
import json, os, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slice as sl

d = sys.argv[1]
idx = json.load(open(os.path.join(d, 'index.json')))
res = []
for s in idx:
    if s['stage'] not in ('vs', 'ds'):
        continue
    osg = s['sigs'].get('OSGN') or s['sigs'].get('OSG5') or []
    pos = [e for e in osg if e[2] == 'POS']
    text = open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()
    insts = sl.parse(text)
    item = dict(name=s['name'], group=s['group'], stage=s['stage'], id=s['id'],
                insem=[e[0] for e in s['sigs'].get('ISGN', [])],
                outsv=[e[2] for e in osg if e[2] != 'NONE'])
    if pos:
        cbs, inputs = sl.slice_output(insts, 'o%d' % pos[0][3])
        item['pos_cb'] = sl.cb_rows(cbs)
        item['pos_inputs'] = sorted({r[0] for r in inputs})
    else:
        item['pos_cb'] = None  # VS feeding HS (no SV_Position)
    # every other output: cb rows
    other = {}
    for e in osg:
        if e[2] == 'POS':
            continue
        cbs, _ = sl.slice_output(insts, 'o%d' % e[3], ''.join(c for i, c in enumerate('xyzw') if e[4] >> i & 1))
        other['%s%d' % (e[0], e[1])] = sl.cb_rows(cbs)
    item['other_cb'] = other
    res.append(item)
json.dump(res, open(os.path.join(d, 'vs_pos.json'), 'w'), indent=0)


def sig(rows):
    if rows is None:
        return '(no SV_Position: feeds tessellation)'
    parts = []
    for k in sorted(rows, key=lambda x: int(x[2:])):
        v = rows[k]
        ints = [x for x in v if isinstance(x, int)]
        s = k + '['
        if ints:
            # compress ranges
            rr, a = [], ints[0]
            prev = a
            for x in ints[1:] + [None]:
                if x is not None and x == prev + 1:
                    prev = x
                    continue
                rr.append('%d' % a if a == prev else '%d-%d' % (a, prev))
                if x is not None:
                    a = prev = x
            s += ','.join(rr)
        if 'dyn' in v:
            s += (',' if ints else '') + 'dyn'
        parts.append(s + ']')
    return ' + '.join(parts) if parts else '(no CB: position passed through / computed from inputs)'


c = Counter()
ex = {}
for r in res:
    k = (r['stage'], sig(r['pos_cb']), 'SV_InstanceID' in r['insem'])
    c[k] += 1
    ex.setdefault(k, r['name'])
print('VS/DS total', len(res))
for k, n in c.most_common():
    print(f'{n:5d}  {k[0]}  inst={int(k[2])}  {k[1]}   e.g. {ex[k]}')
