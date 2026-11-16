"""Survey obstacles for a mechanical single-pass-stereo rewrite of the last pre-raster stage (VS/DS).

usage: python -I rewrite_survey.py <set_dir>
"""
import json, os, re, sys
from collections import Counter

d = sys.argv[1]
idx = json.load(open(os.path.join(d, 'index.json')))
c = Counter()
temps = []
maxo = Counter()
for s in idx:
    if s['stage'] not in ('vs', 'ds'):
        continue
    osg = s['sigs'].get('OSGN', [])
    pos = [e for e in osg if e[2] == 'POS']
    if not pos:
        c['no SV_Position (feeds tessellation)'] += 1
        continue
    c['last pre-raster stage (has SV_Position)'] += 1
    t = open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()
    body = [l.strip() for l in t.splitlines() if l.strip() and not l.strip().startswith('//')]
    preg = 'o%d' % pos[0][3]
    depth = 0
    in_flow = False
    rets = 0
    for l in body:
        op = l.split()[0]
        if op in ('if_z', 'if_nz', 'loop', 'switch'):
            depth += 1
        elif op in ('endif', 'endloop', 'endswitch'):
            depth -= 1
        elif op.startswith('ret'):
            rets += 1
        if re.match(r'^\S+ %s\.' % preg, l) and depth > 0:
            in_flow = True
    if in_flow:
        c['SV_Position written inside flow control'] += 1
    if rets > 1:
        c['more than one ret/retc'] += 1
    if 'instance_id' in t:
        c['already reads SV_InstanceID (native instancing -> needs id>>1)'] += 1
    if 'clip_distance' in t or 'cull_distance' in t:
        c['already writes clip/cull distance'] += 1
    m = re.search(r'dcl_temps (\d+)', t)
    temps.append(int(m.group(1)) if m else 0)
    regs = [e[3] for e in osg]
    maxo[max(regs)] += 1
    if 'dcl_indexableTemp' in t:
        c['uses indexable temps (x#)'] += 1
    if re.search(r'dcl_output o\d+\.[xyzw]+\s*$', t, re.M) is None:
        pass
print('stage VS/DS survey for', d)
for k, v in c.most_common():
    print('  %5d %s' % (v, k))
print('  dcl_temps max', max(temps), 'mean %.1f' % (sum(temps) / len(temps)))
print('  highest output register used: ', dict(sorted(maxo.items())))
