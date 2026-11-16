import os,re,sys
sys.path.insert(0,r'G:\Fallout4ToVR\analysis\tools\shaders')
import align as al
from collections import Counter
fa,fb=r'G:\Fallout4ToVR\work\shaders\vrbase',r'G:\Fallout4ToVR\work\shaders\vr'
ia,ga=al.load(fa); ib,gb=al.load(fb)
pairs,_,_=al.align_dirs(fa,fb)
def ins(d,s):
    return [l.strip() for l in open(os.path.join(d,'disasm',s['name']+'.txt')) if l.strip() and not l.strip().startswith(('//','dcl_','{','}'))]
c=Counter()
for a,b in pairs:
    if a not in ga or b not in gb: continue
    ea={(s['stage'],s['id']):s for s in ga[a]}; eb={(s['stage'],s['id']):s for s in gb[b]}
    for k in ea.keys()&eb.keys():
        x,y=ea[k],eb[k]
        if x['sha1']==y['sha1']: continue
        same=ins(fa,x)==ins(fb,y)
        clipin=any(e[2]=='CLIPDIST' for e in y['sigs'].get('ISGN',[]))
        c[(k[0],'mat' if a<=11 else 'other','instructions identical' if same else 'instructions differ','VR ISGN has clip' if clipin else '')]+=1
for k,v in sorted(c.items()): print(v,k)
