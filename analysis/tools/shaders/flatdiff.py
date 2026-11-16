"""Characterise differences between flat 1.10 (VR-era) and flat 1.11.240 shaders.

usage: python -I flatdiff.py <flat110_dir> <flat111_dir>
"""
import os, re, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import align as al

da, db = sys.argv[1], sys.argv[2]
ia, ga = al.load(da)
ib, gb = al.load(db)
pairs, una, unb = al.align_dirs(da, db)
pairs = [(a, b) for a, b in pairs if a in ga and b in gb]
print('pairs', len(pairs), 'unpaired 1.10', una, 'unpaired 1.11', unb)
for b in unb:
    if b in gb:
        print('  new 1.11 group', b, [(s['name'], s['sigs'].get('ISGN'), s['cbs']) for s in gb[b]])


def cbrefs(d, s, slot):
    t = open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()
    return sorted(set(re.findall(r'cb%d\[(\d+)\]' % slot, t)))


c = Counter()
grp = Counter()
ex = {}
for a, b in pairs:
    ea = {(s['stage'], s['id']): s for s in ga[a]}
    eb = {(s['stage'], s['id']): s for s in gb[b]}
    for k in ea.keys() & eb.keys():
        x, y = ea[k], eb[k]
        if x['sha1'] == y['sha1']:
            c['identical'] += 1
            if x['ctab'] != y['ctab'] or x['tail'][:6] != y['tail'][:6]:
                c['identical_code_but_ctab_or_tail_changed'] += 1
            continue
        grp[a] += 1
        same_sig = x['sigs'] == y['sigs']
        same_cbdecl = x['cbs'] == y['cbs']
        same_ctab = x['ctab'] == y['ctab'] and x['tail'][:6] == y['tail'][:6]
        same_cb12 = cbrefs(da, x, 12) == cbrefs(db, y, 12)
        key = ('sig_same' if same_sig else 'sig_DIFF', 'cbdecl_same' if same_cbdecl else 'cbdecl_DIFF',
               'ctab_same' if same_ctab else 'ctab_DIFF', 'cb12rows_same' if same_cb12 else 'cb12rows_DIFF')
        c[key] += 1
        ex.setdefault(key, x['name'])
print('changed shaders per 1.10 group:', dict(grp))
for k, v in c.most_common():
    print('  %5d %s %s' % (v, k, ('e.g. ' + ex[k]) if k in ex else ''))
