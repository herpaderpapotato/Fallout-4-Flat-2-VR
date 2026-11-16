"""Inventory + stereo-relevant statistics for one dumped set.

usage: python -I stats.py <set_dir>
"""
import json, os, re, sys
from collections import Counter, defaultdict

d = sys.argv[1]
idx = json.load(open(os.path.join(d, 'index.json')))
print('== set', d)
print('groups', len({s['group'] for s in idx}), 'shaders', len(idx))
by_stage = defaultdict(list)
for s in idx:
    by_stage[s['stage']].append(s)
print('%-4s %6s %8s %10s %8s %s' % ('stg', 'count', 'unique', 'bytes', 'avg', 'shader models'))
for st in ('vs', 'hs', 'ds', 'gs', 'ps', 'cs'):
    ss = by_stage.get(st, [])
    if not ss:
        continue
    tot = sum(s['size'] for s in ss)
    print('%-4s %6d %8d %10d %8d %s' % (st, len(ss), len({s['sha1'] for s in ss}), tot, tot // len(ss),
                                       dict(Counter(s['sm'] for s in ss))))
print('total bytecode', sum(s['size'] for s in idx), 'unique blobs', len({s['sha1'] for s in idx}))
print('chunk sets', Counter(tuple(s['chunks']) for s in idx).most_common())

# system values
sv_in, sv_out = Counter(), Counter()
for s in idx:
    for k, els in s['sigs'].items():
        for e in els:
            if e[2] in ('NONE', 'TARGET'):
                continue
            (sv_in if k.startswith('I') else sv_out)[(s['stage'], e[2] if e[2] != 'NONE' else e[0])] += 1
# input SVs declared via dcl_input_sgv/siv not in signature names (e.g. SV_InstanceID appears as INSTID)
print('input system values (stage, sv): count')
for k, v in sorted(sv_in.items()):
    print('  ', k, v)
print('output system values (stage, sv): count')
for k, v in sorted(sv_out.items()):
    print('  ', k, v)
# semantic names in signatures with SV_ prefix but NONE sv (e.g. SV_InstanceID as VS input shows sv NONE in ISGN)
svname = Counter()
for s in idx:
    for k, els in s['sigs'].items():
        for e in els:
            if e[0].upper().startswith('SV_'):
                svname[(s['stage'], k, e[0])] += 1
print('SV_ semantic names by stage/chunk:')
for k, v in sorted(svname.items()):
    print('  ', k, v)

# constant buffer slots: max size, how many shaders, dynamic indexing
cbinfo = defaultdict(lambda: dict(n=0, sizes=Counter(), dyn=0))
for s in idx:
    for slot, size, mode in s['cbs']:
        ci = cbinfo[(s['stage'], slot)]
        ci['n'] += 1
        ci['sizes'][size] += 1
        ci['dyn'] += mode == 'dynamicIndexed'
print('constant buffers: (stage, slot) shaders  dynamicIndexed  max_rows  top sizes')
for k in sorted(cbinfo):
    ci = cbinfo[k]
    print('  %-4s cb%-3d %5d %5d %5d  %s' % (k[0], k[1], ci['n'], ci['dyn'], max(ci['sizes']),
                                             ci['sizes'].most_common(4)))

# eye-select idiom on instance id
eye = Counter()
for s in idx:
    if s['stage'] not in ('vs',):
        continue
    t = open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()
    m = re.search(r'dcl_input_sgv (v\d+)\.x, instance_id', t)
    if not m:
        continue
    v = m.group(1)
    and1 = re.search(r'\band (r\d+\.\w), %s\.x, l\(1\)' % v, t) is not None
    bfi = re.search(r'\bbfi r\d+\.\w, l\(1\), l\(\d\), %s\.x' % v, t) is not None
    ushr = re.search(r'\bushr r\d+\.\w, %s\.x, l\(1\)' % v, t) is not None
    eye[(and1, bfi, ushr)] += 1
print('VS with instance_id: (and v.x,1 / bfi(eye<<n) / ushr v.x,1): count', dict(eye))
