"""One line per group of a dumped set: counts, VS input semantics, cb slots used."""
import json, sys
from collections import defaultdict, Counter

idx = json.load(open(sys.argv[1]))
by = defaultdict(list)
for s in idx:
    by[s['group']].append(s)
for g, ss in sorted(by.items()):
    c = Counter(s['stage'] for s in ss)
    vs = [s for s in ss if s['stage'] == 'vs']
    ins = Counter()
    for s in vs:
        for e in s['sigs'].get('ISGN', []):
            ins[e[0] + ('' if e[1] == 0 else str(e[1]))] += 1
    cbs = Counter()
    for s in ss:
        for slot, size, _ in s['cbs']:
            cbs['%s:cb%d' % (s['stage'], slot)] += 1
    ids = sorted(s['id'] for s in ss)
    print(f"g{g:03d} {dict(c)} ids[{ids[0]:#x}..{ids[-1]:#x}] VSin={dict(ins) if vs else '-'} cbs={dict(cbs)}")
