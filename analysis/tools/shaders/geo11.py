"""Survey the 3DMigoto (HelixMod / DHR) Fallout 4 fix.

usage: python -I geo11.py <geo11_dir> <set_dir> [<set_dir> ...]
 - counts overrides by type
 - classifies the stereo edits (lines touching StereoParams t125 / IniParams t120)
 - recovers the original shader body embedded in each file and tries to match it against our sets
   (exact 3DMigoto hash = FNV-1 64 over the bytecode; plus a fuzzy match on instruction text)
"""
import json, os, re, sys
from collections import Counter, defaultdict

g = sys.argv[1]
sets = sys.argv[2:]
sf = os.path.join(g, 'ShaderFixes')
files = sorted(os.listdir(sf))
kind = Counter(re.sub(r'^[0-9a-f]{16}-', '', f) for f in files)
print('files', len(files), dict(kind))

# ini hashes
ini = open(os.path.join(g, 'd3dx.ini'), errors='replace').read()
ini_hashes = re.findall(r'(?im)^\s*hash\s*=\s*([0-9a-f]{16})', ini)
print('d3dx.ini ShaderOverride hashes', len(ini_hashes))

# fix classification
cat = Counter()
stereo_files = 0
asm_stereo_patterns = Counter()
for f in files:
    t = open(os.path.join(sf, f), errors='replace').read()
    if f.endswith('_replace.txt'):
        body = t.split('/*~~~~')[0]
        uses = 'StereoParams' in body
    else:
        body = t
        uses = re.search(r't125', body) is not None
    if uses:
        stereo_files += 1
    # what is the corrected quantity: clip-space x (VS) vs reconstructed position (PS)
    if f.endswith('-ps.txt'):
        # asm: count inserted lines (between the stereo ld and the next original instr)
        n_ld = len(re.findall(r'ld_indexable\(texture2d\)\S* r\d+\.xyzw, l\(0, 0, 0, 0\), t125', body))
        asm_stereo_patterns['ps asm: StereoParams loads=%d' % n_ld] += 1
        if re.search(r'cb12\[2[0-7]\]', body):
            cat['ps asm: reads cb12[20-27] (inverse proj / unprojection matrices)'] += 1
        if re.search(r'dcl_input_ps_siv linear noperspective v0\.xy, position', body):
            cat['ps asm: uses SV_Position screen coords'] += 1
    elif f.endswith('vs_replace.txt'):
        cat['vs hlsl'] += 1
    elif f.endswith('ps_replace.txt'):
        cat['ps hlsl'] += 1
    elif f.endswith('cs.txt'):
        cat['cs asm'] += 1
print('files referencing StereoParams/t125:', stereo_files)
for k, v in sorted(cat.items()):
    print('  ', k, v)
for k, v in sorted(asm_stereo_patterns.items()):
    print('  ', k, v)

# match: exact hash
import sys as _s
_s.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
fix_hashes = {f[:16] for f in files}
for d in sets:
    idx = json.load(open(os.path.join(d, 'index.json')))
    fnvs = {s['fnv'] for s in idx}
    print('set %s: exact 3DMigoto-hash matches: ShaderFixes %d/%d, ini %d/%d' % (
        os.path.basename(d), len(fix_hashes & fnvs), len(fix_hashes), len(set(ini_hashes) & fnvs), len(set(ini_hashes))))


# fuzzy: compare the original-asm instruction sequence (asm files: strip our-added lines heuristically = drop
# lines touching r2x temps beyond original dcl_temps and t125). Use the opcode+operand text of SHEX instrs.
def norm_instrs(text):
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('//') or line.startswith('dcl_') or line[0] in '{}' or re.match(r'^[pvcgdh]s_5_0$', line):
            continue
        line = re.sub(r'\s+', ' ', line).lower()
        out.append(line)
    return out


def orig_from_fix(f, t):
    if f.endswith('_replace.txt'):
        if '/*~~~~' not in t:
            return None
        return norm_instrs(t.split('/*~~~~')[1])
    lines = norm_instrs(t)
    # drop inserted stereo lines: those referencing t125/t120 or temps >= original dcl_temps
    m = re.search(r'//dcl_temps (\d+)', t)
    nt = int(m.group(1)) if m else None
    keep = []
    for l in lines:
        if 't125' in l or 't120' in l:
            continue
        if nt is not None and any(int(x) >= nt for x in re.findall(r'\br(\d+)\.', l)):
            continue
        keep.append(l)
    return keep


fixes = {}
for f in files:
    t = open(os.path.join(sf, f), errors='replace').read()
    o = orig_from_fix(f, t)
    if o:
        fixes[f] = o
print('fix files with recoverable original instruction stream:', len(fixes))
import difflib
for d in sets:
    idx = json.load(open(os.path.join(d, 'index.json')))
    by_stage = defaultdict(list)
    for s in idx:
        by_stage[s['stage']].append(s)
    cache = {}
    exact = near = 0
    groups_hit = Counter()
    res = {}
    for f, o in fixes.items():
        st = 'vs' if '-vs' in f else 'ps' if '-ps' in f else 'cs'
        so = '\n'.join(o)
        best = (0.0, None)
        for s in by_stage[st]:
            if s['sha1'] in cache:
                ins = cache[s['sha1']]
            else:
                ins = norm_instrs(open(os.path.join(d, 'disasm', s['name'] + '.txt')).read())
                cache[s['sha1']] = ins
            if abs(len(ins) - len(o)) > max(3, len(o) // 5):
                continue
            if ins == o:
                best = (1.0, s['name'])
                break
            r = difflib.SequenceMatcher(None, ins, o, autojunk=False).quick_ratio()
            if r > best[0] and r > 0.8:
                r = difflib.SequenceMatcher(None, ins, o, autojunk=False).ratio()
                if r > best[0]:
                    best = (r, s['name'])
        res[f] = best
        if best[0] == 1.0:
            exact += 1
            groups_hit[best[1][:4]] += 1
        elif best[0] >= 0.9:
            near += 1
            groups_hit[best[1][:4]] += 1
    print('set %s: fix originals matched by instruction text: exact %d, >=90%% similar %d, of %d' % (
        os.path.basename(d), exact, near, len(fixes)))
    print('   matched groups:', dict(groups_hit.most_common()))
    json.dump(res, open(os.path.join(d, 'geo11_match.json'), 'w'), indent=0)
