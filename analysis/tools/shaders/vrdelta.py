"""Classify what changed between flat 1.10 shaders and their Fallout 4 VR counterparts.

usage: python -I vrdelta.py <flat_set_dir> <vr_set_dir>
"""
import os, re, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import align as al

fa, fb = sys.argv[1], sys.argv[2]
ia, ga = al.load(fa)
ib, gb = al.load(fb)
pairs, una, unb = al.align_dirs(fa, fb)
pairs = [(a, b) for a, b in pairs if a in ga and b in gb]
print('group pairs', len(pairs), 'unpaired flat', una, 'unpaired vr', unb)


def text(d, s):
    return open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()


def feats(s, t):
    f = set()
    if re.search(r'dcl_input_sgv v\d+\.x, instance_id', t):
        f.add('in:SV_InstanceID')
    if 'clip_distance' in t:
        f.add('out/in:ClipDistance')
    if 'cull_distance' in t:
        f.add('out/in:CullDistance')
    if re.search(r'viewport_array_index|rendertarget_array_index', t):
        f.add('VP/RT array index')
    if 'CB8[' in t:
        f.add('cb8 (stereo params)')
    if re.search(r'dcl_constantbuffer CB12\[\d+\], dynamicIndexed', t):
        f.add('cb12 dynamicIndexed')
    if re.search(r'dcl_constantbuffer CB2\[\d+\], dynamicIndexed', t):
        f.add('cb2 dynamicIndexed')
    if 'dcl_immediateConstantBuffer' in t:
        f.add('icb')
    if re.search(r'dcl_input_ps constant v\d+\.x', t):
        f.add('ps: nointerpolation scalar input')
    if re.search(r'dcl_output o\d+\.\w*\n', t) and s['stage'] == 'vs':
        pass
    return f


def category(g):
    return 'material(g0-11)' if g <= 11 else 'imagespace/utility(g12+)'


res = Counter()
featdiff = Counter()
feat_vr_total = Counter()
examples = {}
for a, b in pairs:
    ea = {(s['stage'], s['id']): s for s in ga[a]}
    eb = {(s['stage'], s['id']): s for s in gb[b]}
    for k in ea.keys() & eb.keys():
        x, y = ea[k], eb[k]
        cat = category(a)
        if x['sha1'] == y['sha1']:
            res[(cat, k[0], 'identical')] += 1
            continue
        res[(cat, k[0], 'changed')] += 1
        fx, fy = feats(x, text(fa, x)), feats(y, text(fb, y))
        added = fy - fx
        key = (cat, k[0], tuple(sorted(added)) or ('(no stereo feature added)',))
        featdiff[key] += 1
        examples.setdefault(key, (x['name'], y['name']))
for s in ib:
    for f in feats(s, text(fb, s)):
        feat_vr_total[(s['stage'], f)] += 1
print('\n== identical vs changed by category/stage')
for k in sorted(res):
    print('  ', k, res[k])
print('\n== stereo features ADDED in VR version of changed shaders')
for k, v in sorted(featdiff.items(), key=lambda kv: (kv[0][0], kv[0][1], -kv[1])):
    print('  %5d %s %s %s   e.g. %s' % (v, k[0], k[1], ' + '.join(k[2]), examples[k][1]))
print('\n== feature totals over whole VR set')
for k in sorted(feat_vr_total):
    print('  ', k, feat_vr_total[k])
