import json,os,re,sys
from collections import Counter
w='G:/Fallout4ToVR/work/shaders/'
fix=[f for f in os.listdir(w+'geo11/ShaderFixes')]
fh={f[:16]:f for f in fix}
ini=re.findall(r'(?im)^\s*hash\s*=\s*([0-9a-f]{16})', open(w+'geo11/d3dx.ini').read())
for st in ('vrbase','flat','vr'):
    idx=json.load(open(w+st+'/index.json'))
    by={}
    for s in idx: by.setdefault(s['fnv'],[]).append(s)
    c=Counter(); ci=Counter(); kinds=Counter()
    for h,f in fh.items():
        for s in by.get(h,[]):
            c[(s['group'],s['stage'])]+=1
        if h in by: kinds[re.sub(r'^[0-9a-f]{16}-','',f)]+=1
    for h in ini:
        for s in by.get(h,[]): ci[(s['group'],s['stage'])]+=1
    print(st,'fix-hash hits (entries incl. duplicates):',sum(c.values()),'by group/stage:',sorted(c.items()))
    print('   matched fix kinds:',dict(kinds))
    print('   ini override hits by group:',sorted(ci.items()))
