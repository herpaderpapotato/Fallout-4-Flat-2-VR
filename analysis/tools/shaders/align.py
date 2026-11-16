"""Group alignment between two dumped sets, computed on the .fxp group list (empty/null groups included,
so group ordinals stay the fxp ordinals used in index.json names).

LCS on per-group (stage,id) sets, then positional fill of unaligned groups lying between the same
aligned neighbours.
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fxp


def load(d):
    idx = json.load(open(os.path.join(d, 'index.json')))
    groups = {}
    for s in idx:
        groups.setdefault(s['group'], []).append(s)
    return idx, groups


def find_fxp(d):
    sd = os.path.join(d, 'ShadersFX')
    return os.path.join(sd, [f for f in os.listdir(sd) if f.endswith('.fxp')][0])


def align_dirs(da, db):
    A = fxp.parse_fxp(find_fxp(da))
    B = fxp.parse_fxp(find_fxp(db))
    sa = [(g['counts'], tuple(sorted((e['stage'], e['id']) for e in g['entries']))) for g in A]
    sb = [(g['counts'], tuple(sorted((e['stage'], e['id']) for e in g['entries']))) for g in B]
    # match on id-set only when non-empty, else on counts
    ka = [s[1] if s[1] else ('counts', s[0]) for s in sa]
    kb = [s[1] if s[1] else ('counts', s[0]) for s in sb]
    n, m = len(ka), len(kb)
    L = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            L[i][j] = L[i + 1][j + 1] + 1 if ka[i] == kb[j] else max(L[i + 1][j], L[i][j + 1])
    pairs, i, j, una, unb = [], 0, 0, [], []
    while i < n and j < m:
        if ka[i] == kb[j]:
            pairs.append((i, j))
            i += 1
            j += 1
        elif L[i + 1][j] >= L[i][j + 1]:
            una.append(i)
            i += 1
        else:
            unb.append(j)
            j += 1
    una += list(range(i, n))
    unb += list(range(j, m))
    for a in list(una):
        lo = max([p for p in pairs if p[0] < a], default=(-1, -1))[1]
        hi = min([p for p in pairs if p[0] > a], default=(10 ** 9, 10 ** 9))[1]
        cand = [b for b in unb if lo < b < hi]
        if cand:
            pairs.append((a, cand[0]))
            una.remove(a)
            unb.remove(cand[0])
    return sorted(pairs), una, unb
