"""Dump every shader of an .fxp: disassembly text + index.json.

usage: python -I dump.py <file.fxp> <out_dir>
"""
import hashlib, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fxp

path, out = sys.argv[1], sys.argv[2]
groups = fxp.parse_fxp(path)
dd = os.path.join(out, 'disasm')
os.makedirs(dd, exist_ok=True)
index = []
for gi, g in enumerate(groups):
    for e in g['entries']:
        blob = e['blob']
        txt = fxp.disasm(blob)
        name = 'g%03d_%s_%08x' % (gi, e['stage'], e['id'])
        if txt is not None:
            with open(os.path.join(dd, name + '.txt'), 'w', newline='\n') as f:
                f.write(txt)
        sigs = fxp.signatures(blob)
        cbs = re.findall(r'dcl_constantbuffer (?:CB|cb)(\d+)\[(\d+)\], (\w+)', txt or '')
        index.append(dict(
            name=name, group=gi, counts=g['counts'], stage=e['stage'], slot=e['slot'], id=e['id'], sm=e['sm'],
            size=len(blob), sha1=hashlib.sha1(blob).hexdigest(),
            sha1_code=hashlib.sha1(blob[20:]).hexdigest(),
            fnv=format(fxp.fnv1_64_3dmigoto(blob), '016x'),
            chunks=[c.decode() for c, _, _ in fxp.dxbc_chunks(blob)],
            desc=None if e['desc'] is None else format(e['desc'], '016x'),
            ctab=e['ctab'], tail=e['tail'].hex(),
            sigs={k: [(s['name'], s['idx'], s['sv'], s['reg'], s['mask'], s['rw']) for s in v] for k, v in sigs.items()},
            cbs=[(int(a), int(b), c) for a, b, c in cbs],
            disasm_ok=txt is not None,
        ))
with open(os.path.join(out, 'index.json'), 'w') as f:
    json.dump(index, f)
print(len(index), 'shaders dumped;', sum(not i['disasm_ok'] for i in index), 'disasm failures')
