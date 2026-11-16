import json,sys
from collections import Counter
for st in sys.argv[1:]:
    res=json.load(open('G:/Fallout4ToVR/work/shaders/%s/vs_pos.json'%st))
    c=Counter(); uniq={}
    idx={s['name']:s for s in json.load(open('G:/Fallout4ToVR/work/shaders/%s/index.json'%st))}
    for r in res:
        p=r['pos_cb']
        if p is None: k='D no SV_Position (feeds HS)'
        elif not p: k='C no CB (pass-through / fullscreen)'
        elif 'cb12' in p and any(isinstance(x,int) and 8<=x<=11 for x in p['cb12']): k='A global ViewProj cb12[8-11]'
        elif 'cb12' in p and 'dyn' in p['cb12']: k='A* per-eye cb12[dyn]'
        elif 'cb12' in p: k='E cb12 other rows'
        elif 'cb2' in p and 'dyn' in p['cb2']: k='B* per-eye cb2[dyn]'
        else: k='B per-draw CB only (pre-multiplied WVP or 2D transform)'
        cat='mat' if r['group']<=11 else 'other'
        c[(r['stage'],cat,k)]+=1
        uniq.setdefault((r['stage'],cat,k),set()).add(idx[r['name']]['sha1'])
    print('==',st)
    for k in sorted(c): print('  %-3s %-5s %-60s %5d  (unique blobs %d)'%(k[0],k[1],k[2],c[k],len(uniq[k])))
