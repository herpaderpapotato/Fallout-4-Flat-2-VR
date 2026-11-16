"""Compare two fxp packages group-by-group / shader-by-shader.

usage: python -I compare.py <fxpA> <fxpB> <labelA> <labelB> [groupmap]
groupmap: optional 'auto' -> align groups by an LCS over (counts, id-set) signatures.
Shaders are matched by (group, stage, id).
"""
import hashlib, os, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fxp

A = fxp.parse_fxp(sys.argv[1])
B = fxp.parse_fxp(sys.argv[2])
la, lb = sys.argv[3], sys.argv[4]


def gsig(g):
    return (tuple(sorted((e['stage'], e['id']) for e in g['entries'])))


# LCS alignment of groups on exact (stage,id)-set signature, then fill gaps positionally
sa, sb = [gsig(g) for g in A], [gsig(g) for g in B]
n, m = len(sa), len(sb)
L = [[0] * (m + 1) for _ in range(n + 1)]
for i in range(n - 1, -1, -1):
    for j in range(m - 1, -1, -1):
        L[i][j] = L[i + 1][j + 1] + 1 if sa[i] == sb[j] else max(L[i + 1][j], L[i][j + 1])
pairs, i, j = [], 0, 0
unA, unB = [], []
while i < n and j < m:
    if sa[i] == sb[j]:
        pairs.append((i, j, 'exact-idset'))
        i += 1
        j += 1
    elif L[i + 1][j] >= L[i][j + 1]:
        unA.append(i)
        i += 1
    else:
        unB.append(j)
        j += 1
unA += list(range(i, n))
unB += list(range(j, m))
print(f'groups {la}={n} {lb}={m}; aligned by identical (stage,id) sets: {len(pairs)}')
print(f'  unaligned {la}: {[(k, A[k]["counts"]) for k in unA]}')
print(f'  unaligned {lb}: {[(k, B[k]["counts"]) for k in unB]}')
# pair the unaligned ones positionally when they sit between the same aligned neighbours
for a in list(unA):
    prev = max([p for p in pairs if p[0] < a], default=(-1, -1, ''), key=lambda p: p[0])
    cand = [b for b in unB if b > prev[1]]
    nxt = min([p for p in pairs if p[0] > a], default=(n, m, ''), key=lambda p: p[0])
    cand = [b for b in cand if b < nxt[1]]
    if cand:
        pairs.append((a, cand[0], 'positional'))
        unA.remove(a)
        unB.remove(cand[0])
pairs.sort()
print(f'  after positional fill: pairs={len(pairs)} unpairedA={unA} unpairedB={unB}')

stat = Counter()
per_stage = Counter()
grp_rows = []
for ga, gb, how in pairs:
    ea = {(e['stage'], e['id']): e for e in A[ga]['entries']}
    eb = {(e['stage'], e['id']): e for e in B[gb]['entries']}
    same = diff = code_same = onlyA = onlyB = 0
    for k in ea.keys() | eb.keys():
        if k not in eb:
            onlyA += 1
            per_stage[(k[0], 'only_' + la)] += 1
            continue
        if k not in ea:
            onlyB += 1
            per_stage[(k[0], 'only_' + lb)] += 1
            continue
        x, y = ea[k]['blob'], eb[k]['blob']
        if x == y:
            same += 1
            per_stage[(k[0], 'identical')] += 1
        elif x[20:] == y[20:]:
            code_same += 1
        else:
            # same code body except checksum? compare SHEX only
            diff += 1
            per_stage[(k[0], 'different')] += 1
        if x != y and ea[k]['ctab'] != eb[k]['ctab']:
            stat['ctab_changed'] += 1
        if x == y and ea[k]['ctab'] != eb[k]['ctab']:
            stat['ctab_changed_same_code'] += 1
    grp_rows.append((ga, gb, how, A[ga]['counts'], B[gb]['counts'], same, diff, onlyA, onlyB))
    stat['identical'] += same
    stat['different'] += diff
    stat['onlyA'] += onlyA
    stat['onlyB'] += onlyB
print('totals over paired groups:', dict(stat))
print('per stage:')
for k in sorted(per_stage):
    print('  ', k, per_stage[k])
print('group rows (gA gB how countsA countsB identical different only%s only%s):' % (la, lb))
for r in grp_rows:
    if r[6] or r[7] or r[8] or r[2] != 'exact-idset':
        print('  ', r)
print('  (groups with all shaders identical omitted: %d)' % sum(1 for r in grp_rows if not (r[6] or r[7] or r[8]) and r[2] == 'exact-idset'))
# global blob-level overlap regardless of position
ha = Counter(hashlib.sha1(e['blob']).hexdigest() for g in A for e in g['entries'])
hb = Counter(hashlib.sha1(e['blob']).hexdigest() for g in B for e in g['entries'])
print('unique blobs %s=%d %s=%d shared=%d' % (la, len(ha), lb, len(hb), len(ha.keys() & hb.keys())))
