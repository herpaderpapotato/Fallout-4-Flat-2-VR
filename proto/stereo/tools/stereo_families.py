"""Per-family draw counts from one "stereodump" frame (fo4flatvr.log lines), converted vs not converted.

    python -I stereo_families.py <log-or-dump-file> [Shaders011.fxp]

Each dump line is one (VS, camera, GBuffer-bound, call, instanced, RTVs, action) group with its draw count. The
VS key is mapped to its fxp group/technique and input signature, and classified:
  family = g<group> + shape: prepass (DoZPrePass VS), pulling (SV_VertexID), skinned (BLENDWEIGHT),
           instanced (native SV_InstanceID or per-instance TEXCOORD4-7 stream), static
  camera = world / 1st-person / none (no stereo camera applied)
Prints, for GBuffer writers and the prepass, draws per frame converted / not converted by (camera, family), plus a
one-line total. stdlib only.
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402

LINE = re.compile(r'stereodump: vs (\S+) key (\w+|-)(.*?) gate (\d) gb (\d) (\w+)( inst)? rtvs (\d+) -> (\w+) : (\d+) draws (\d+) indices')
CAM = {'0': 'none', '1': 'world', '2': '1st-person'}


def vs_index(fxp_path):
    names, isgn = {}, {}
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        for e in g['entries']:
            if e['stage'] != 'vs':
                continue
            k = e['blob'][4:20].hex()
            names.setdefault(k, []).append((gi, e['id']))
            if k not in isgn:
                isgn[k] = [('%s%d' % (s['name'], s['idx'])).upper() for s in fxp.signatures(e['blob']).get('ISGN', [])]
    return names, isgn


def family(key, tags, names, isgn):
    if key not in names:
        return 'other (not in fxp)'
    g = names[key][0][0]
    sig = isgn.get(key, [])
    if 'prepass' in tags:
        shape = 'prepass'
    elif 'SV_VERTEXID0' in sig:
        shape = 'pulling'
    elif any(s.startswith('BLENDWEIGHT') for s in sig):
        shape = 'skinned'
    elif 'SV_INSTANCEID0' in sig or any(s in sig for s in ('TEXCOORD4', 'TEXCOORD5', 'TEXCOORD6', 'TEXCOORD7')):
        shape = 'instanced'
    else:
        shape = 'static'
    return 'g%03d %s' % (g, shape)


def summarize(lines, fxp_path=None):
    fxp_path = fxp_path or os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    names, isgn = vs_index(fxp_path)
    tab = collections.defaultdict(lambda: [0, 0, 0])  # (camera, family) -> converted, not converted, skipped
    other = collections.defaultdict(int)
    tot = [0, 0, 0]
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        key, tags, gate, gb, kind, inst, rtvs, act, draws = (m.group(2), m.group(3), m.group(4), m.group(5), m.group(6), m.group(7),
                                                              m.group(8), m.group(9), int(m.group(10)))
        fam = family(key, tags, names, isgn)
        if inst:
            fam += ' (inst. call)'
        if gb == '1' or 'prepass' in tags:
            col = 0 if act == 'stereo' else 2 if act == 'skip' else 1
            tab[(CAM[gate], fam)][col] += draws
            tot[col] += draws
        elif gate != '0':
            other[(CAM[gate], fam, act)] += draws
    out = ['%-12s %-34s %9s %9s %9s' % ('camera', 'family (GBuffer writers + prepass)', 'converted', 'not conv.', 'skipped')]
    for (cam, fam), v in sorted(tab.items(), key=lambda kv: (kv[0][0], -sum(kv[1]))):
        out.append('%-12s %-34s %9d %9d %9d' % (cam, fam, v[0], v[1], v[2]))
    out.append('TOTAL GBuffer+prepass draws: converted %d, not converted %d, skipped %d (%.1f%% converted)' % (
        tot[0], tot[1], tot[2], 100.0 * tot[0] / max(1, sum(tot))))
    if other:
        out.append('stereo-camera draws outside the GBuffer (not in this round\'s scope):')
        for (cam, fam, act), n in sorted(other.items(), key=lambda kv: -kv[1])[:25]:
            out.append('  %-12s %-34s %-7s %d' % (cam, fam, act, n))
    return '\n'.join(out), {'table': {'%s|%s' % k: v for k, v in tab.items()}, 'total': tot}


if __name__ == '__main__':
    text, _ = summarize(open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines(),
                        sys.argv[2] if len(sys.argv) > 2 else None)
    print(text)
