"""Name the vertex shaders in "stereodump" log lines (DXBC header checksum -> fxp group/technique + ISGN).

    python -I vs_lookup.py <fo4flatvr.log> [Shaders011.fxp]

Prints each stereodump line with the matching fxp entries (all technique ids sharing that blob) and the
blob's input signature, so unconverted GBuffer draws can be classified.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402


def main():
    log = sys.argv[1]
    fxp_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    names = {}
    isgn = {}
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        for e in g['entries']:
            if e['stage'] != 'vs':
                continue
            k = e['blob'][4:20].hex()
            names.setdefault(k, []).append('g%03d_vs_%08x' % (gi, e['id']))
            if k not in isgn:
                sigs = fxp.signatures(e['blob'])
                isgn[k] = ' '.join('%s%d' % (s['name'], s['idx']) for s in sigs.get('ISGN', []))
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'stereodump: vs (\S+) key (\w+|-)(.*)$', line)
        if not m:
            continue
        k = m.group(2)
        n = names.get(k, ['(not in fxp)'])
        print('%s | %s%s | ISGN %s' % (m.group(3).strip(), n[0], (' +%d' % (len(n) - 1)) if len(n) > 1 else '', isgn.get(k, '?')))


if __name__ == '__main__':
    main()
