"""Tiny SM5 disassembly parser + backward slicer (which constant-buffer rows feed an output).

Works on D3DDisassemble text. Component-granular, linear, kills definitions only outside
conditional blocks (conservative over-approximation inside if/loop bodies).
"""
import re

TWO_DST = {'sincos', 'udiv', 'imul', 'umul', 'swapc', 'uaddc', 'usubb'}
DOT = {'dp2': 2, 'dp3': 3, 'dp4': 4}
ALLDEP_PREFIX = ('sample', 'ld', 'gather', 'resinfo', 'bufinfo', 'lod', 'sampleinfo', 'samplepos', 'imm_atomic',
                 'atomic', 'store')
COMP = 'xyzw'
reg_re = re.compile(r'^(-)?\|?(-)?([a-z]+)(\d+)(?:\[([^\]]+)\])?(?:\.([xyzw]+))?\|?$')
cb_re = re.compile(r'^-?\|?-?(?:cb|CB)(\d+)\[([^\]]+)\](?:\.([xyzw]+))?\|?$')


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse(text):
    """Return list of (opcode, [operands], depth) for executable instructions."""
    insts = []
    depth = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in '{}' or line.startswith('//') or line.startswith('dcl_') or re.match(r'^[a-z]s_\d_\d$', line):
            continue
        m = re.match(r'^(\S+)\s*(.*)$', line)
        op, rest = m.group(1), m.group(2)
        base = op.split('(')[0]
        if base in ('if_z', 'if_nz', 'loop', 'switch'):
            insts.append((base, split_ops(rest), depth))
            depth += 1
            continue
        if base in ('endif', 'endloop', 'endswitch'):
            depth -= 1
            insts.append((base, [], depth))
            continue
        if base in ('else', 'case', 'default'):
            insts.append((base, [], depth))
            continue
        # strip "(texture2d)(float,..)" decorations from the opcode, keep operands
        rest = re.sub(r'^\([^)]*\)(\([^)]*\))?\s*', '', rest) if op.endswith(')') or rest.startswith('(') else rest
        insts.append((base, split_ops(rest), depth))
    return insts


def regcomps(opnd, ncomp_default=4):
    """(name, comps) for a register operand like r1.xy / o0.xyzw / v2.x ; comps list of chars."""
    m = reg_re.match(opnd)
    if not m:
        return None
    name = m.group(3) + m.group(4)
    if m.group(5) is not None:
        name += '[' + m.group(5) + ']'
    comps = list(m.group(6)) if m.group(6) else list(COMP[:ncomp_default])
    return name, comps


def src_refs(opnd, comps_needed):
    """Return (set of (reg, comp) deps, set of cb refs) for reading comps_needed (list of positions 0..3)."""
    deps, cbs = set(), set()
    opnd = opnd.strip()
    if opnd.startswith('l(') or opnd.startswith('-l('):
        return deps, cbs
    m = cb_re.match(opnd)
    if m:
        slot, idx, sw = int(m.group(1)), m.group(2).strip(), m.group(3) or 'xyzw'
        comps = {sw[min(i, len(sw) - 1)] for i in comps_needed}
        if re.fullmatch(r'\d+', idx):
            for c in comps:
                cbs.add((slot, int(idx), c))
        else:
            cbs.add((slot, 'dyn:' + idx, '*'))
            for rm in re.finditer(r'([rvx]\d+(?:\[\d+\])?)\.([xyzw])', idx):
                deps.add((rm.group(1), rm.group(2)))
        return deps, cbs
    rc = regcomps(opnd)
    if rc:
        name, sw = rc
        if not name[0] in 'rvox' and not name.startswith('icb'):
            return deps, cbs
        for i in comps_needed:
            deps.add((name, sw[min(i, len(sw) - 1)]))
        # dynamic index inside brackets of x# / icb
        ib = re.search(r'\[([^\]]+)\]', opnd)
        if ib:
            for rm in re.finditer(r'(r\d+)\.([xyzw])', ib.group(1)):
                deps.add((rm.group(1), rm.group(2)))
    return deps, cbs


def slice_output(insts, out_reg, out_comps='xyzw'):
    """Backward slice: returns (cb refs, input regs, depends_on_dynamic_flow)."""
    need = {(out_reg, c) for c in out_comps}
    cbs, inputs = set(), set()
    for k in range(len(insts) - 1, -1, -1):
        op, ops, depth = insts[k]
        if op in ('if_z', 'if_nz', 'switch', 'loop', 'endif', 'endloop', 'endswitch', 'else', 'case',
                  'default', 'ret', 'retc_z', 'retc_nz', 'break', 'breakc_z', 'breakc_nz', 'continue',
                  'discard_z', 'discard_nz', 'nop', 'emit', 'cut'):
            continue
        if not ops:
            continue
        ndst = 2 if op in TWO_DST else 1
        dsts = ops[:ndst]
        srcs = ops[ndst:]
        hit_any = False
        for di, d in enumerate(dsts):
            if d == 'null':
                continue
            rc = regcomps(d)
            if not rc:
                continue
            dname, dcomps = rc
            hits = [i for i, c in enumerate(COMP) if c in dcomps and (dname, c) in need]
            if not hits:
                continue
            hit_any = True
            if depth == 0:
                for i in hits:
                    need.discard((dname, COMP[i]))
            # which source components feed these dest components
            if op in DOT:
                comps_needed = list(range(DOT[op]))
            elif op.startswith(ALLDEP_PREFIX) or op in ('sincos',):
                comps_needed = [0, 1, 2, 3] if op.startswith(ALLDEP_PREFIX) else hits
            else:
                # component-wise: dest comp i reads src swizzle position i (positions in dest mask order)
                comps_needed = hits
            for s in srcs:
                sc = comps_needed
                if not (op in DOT or op.startswith(ALLDEP_PREFIX)):
                    sc = hits
                dps, cb = src_refs(s, sc)
                cbs |= cb
                for r in dps:
                    if r[0].startswith('v'):
                        inputs.add(r)
                    else:
                        need.add(r)
        _ = hit_any
    return cbs, inputs


def cb_rows(cbs):
    """Compress cb refs to {'cb12': [8,9,10,11], 'cb10': ['dyn']}"""
    out = {}
    for slot, idx, c in cbs:
        out.setdefault('cb%d' % slot, set()).add(idx if isinstance(idx, int) else 'dyn')
    return {k: sorted(v, key=lambda x: (isinstance(x, str), x if isinstance(x, int) else 0)) for k, v in out.items()}
