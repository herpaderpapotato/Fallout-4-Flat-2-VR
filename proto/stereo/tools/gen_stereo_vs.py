"""Generate the stereo twins of the engine's vertex shaders by patching their DXBC bytecode (stdlib only).

    python -I gen_stereo_vs.py [Shaders011.fxp] [vs_pos.json] [out.pack]

Why bytecode patching rather than HLSL templates: the twin must reproduce the original's outputs register for
register (the engine pixel / hull stages stay bound), and there are ~1,170 camera-dependent VS (~480 unique
blobs) in many shapes (static, skinned, vertex-pulling, instanced, premultiplied WVP). Appending a fixed
epilogue to the original program is exact for all of them; only the clip position changes.

Patch (one template for every shader whose SV_Position depends on a camera: category A = cb12[8..11], category
B = a full premultiplied matrix in cb2 rows 0..3 (rows 0,1,3 for constant-z sky/effect shaders; rows 3..6 in g007);
2D transforms (cb2 scale/offset, cb12[4..7]) are left out):
  ISGN  + SV_InstanceID (new last register) unless the shader already reads it
  OSGN  + SV_ClipDistanceN (new last register; N = 0, or 1 if the original already writes a clip distance)
  SHEX  dcl_constantbuffer cb5[17] dynamicIndexed, dcl_input_sgv vI.x instance_id, dcl_output_siv oC.x clip,
        dcl_temps +3 (T0 = clipMono, T1 = instance bits, T2 = eye clip)
        every write to the SV_Position register oP is redirected to T0;
        native SV_InstanceID reads are redirected to T1.x = id >> 1 (the original instance; the draw hook
        doubles the instance count, eye = id & 1);
        prologue: [ushr T1.x, vI.x, 1] and T1.y, vI.x, 1 ; ishl T1.z, T1.y, 2
        epilogue (before the single final ret):
           T2 = T0.x*D[e][0] + T0.y*D[e][1] + T0.z*D[e][2] + T0.w*D[e][3]       D[e] = cb5[8 + 4e .. 11 + 4e]
           sign = e ? 1 : -1 ; s = cb5[16].x (1 stereo squeeze, 0 identity)
           oP   = ( (T2.x*(2-s) + sign*T2.w*s) * 0.5, T2.yzw )                  (Fallout 4 VR's squeeze idiom)
           oC.x = s > 0 ? T2.w + sign*T2.x : 1                                    (seam clip)
  D[e] = VP_mono^-1 * VP_e (b5 gEyeFromMonoClip, built per camera application in double precision) is exact
  for any clip position that is linear in the current camera's ViewProj - category A (cb12[8..11]) and
  category B (premultiplied cb2 WVP) alike - and it cancels the TAA jitter of VP_mono.

Output: a pack the plugin loads at device creation (StereoDraw.cpp LoadPack):
  'FVSP' u32 version=1 u32 count, then per entry:
  key[16] (ORIGINAL blob's DXBC checksum) u16 group u8 category (1 A, 2 B) u8 flags u32 techniqueId u16 uses
  u16 pad u32 size, patched blob (size bytes, padded to 4)
flags: 1 native SV_InstanceID, 2 SV_VertexID (vertex pulling), 4 original already writes SV_ClipDistance
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402
from dxbc_checksum import dxbc_checksum  # noqa: E402

# ---- SM5 token constants -------------------------------------------------------------------------------
OP_ADD, OP_AND, OP_ISHL, OP_LT, OP_MAD, OP_MOV, OP_MOVC, OP_MUL, OP_USHR = 0x00, 0x01, 0x29, 0x31, 0x32, 0x36, 0x37, 0x38, 0x55
OP_RET, OP_RETC, OP_CUSTOMDATA = 0x3E, 0x3F, 0x35
OP_GE, OP_MAX, OP_MIN = 0x1D, 0x34, 0x33
OP_DCL_CB, OP_DCL_INPUT_SGV, OP_DCL_OUTPUT_SIV, OP_DCL_TEMPS, OP_DCL_INDEXABLE_TEMP = 0x59, 0x60, 0x67, 0x68, 0x69
DECL_OPS = set(range(0x58, 0x6B)) | set(range(0x8D, 0xA3)) | {0xCE, OP_CUSTOMDATA}
T_TEMP, T_INPUT, T_OUTPUT, T_IMM32, T_IMM64 = 0, 1, 2, 4, 5
SB_NAME_CLIP_DISTANCE, SB_NAME_INSTANCE_ID = 2, 8
CB_SLOT, CB_ROWS, CB_DROW, CB_PARAMS = 5, 17, 8, 16


class PatchError(Exception):
    pass


def opcode_len(toks, i):
    t = toks[i]
    if (t & 0x7FF) == OP_CUSTOMDATA:
        return toks[i + 1]
    return (t >> 24) & 0x7F


def parse_operand(toks, i, out):
    """Parse one operand at toks[i]; append (token_index, idx0_index_or_None) for every operand incl. nested
    relative ones to out. Returns the index after the operand."""
    tok = toks[i]
    ncomp = tok & 3
    typ = (tok >> 12) & 0xFF
    dim = (tok >> 20) & 3
    reps = [(tok >> 22) & 7, (tok >> 25) & 7, (tok >> 28) & 7]
    j = i + 1
    ext = tok >> 31
    while ext:
        ext = toks[j] >> 31
        j += 1
    if typ == T_IMM32:
        j += 1 if ncomp == 1 else 4
    elif typ == T_IMM64:
        j += 2 if ncomp == 1 else 8
    idx0 = None
    for d in range(dim):
        r = reps[d]
        if d == 0 and r == 0:
            idx0 = j
        if r == 0:
            j += 1
        elif r == 1:
            j += 2
        elif r == 2:
            j = parse_operand(toks, j, out)
        elif r == 3:
            j = parse_operand(toks, j + 1, out)
        elif r == 4:
            j = parse_operand(toks, j + 2, out)
        else:
            raise PatchError('bad index representation %d' % r)
    out.append((i, idx0, typ, dim, reps[0]))
    return j


def parse_instruction_operands(toks, i, n):
    t = toks[i]
    j = i + 1
    ext = t >> 31
    while ext:
        ext = toks[j] >> 31
        j += 1
    ops = []
    while j < i + n:
        j = parse_operand(toks, j, ops)
    if j != i + n:
        raise PatchError('operand parse overran instruction at %d (op %#x)' % (i, t & 0x7FF))
    return ops


# ---- operand encoders -------------------------------------------------------------------------------------
def reg_mask(typ, reg, mask):
    return [(2 | (0 << 2) | (mask << 4) | (typ << 12) | (1 << 20)), reg]


def reg_swz(typ, reg, swz):  # swz: string like 'xxxx'
    s = 0
    for k, c in enumerate(swz):
        s |= 'xyzw'.index(c) << (2 * k)
    return [(2 | (1 << 2) | (s << 4) | (typ << 12) | (1 << 20)), reg]


def reg_sel(typ, reg, c):
    return [(2 | (2 << 2) | ('xyzw'.index(c) << 4) | (typ << 12) | (1 << 20)), reg]


def imm1(v, as_float):
    return [0x00004001, struct.unpack('<I', struct.pack('<f', v))[0] if as_float else v & 0xFFFFFFFF]


def cb_row(row, swz='xyzw', neg=False):
    s = 0
    for k, c in enumerate(swz):
        s |= 'xyzw'.index(c) << (2 * k)
    t = 2 | (1 << 2) | (s << 4) | (8 << 12) | (2 << 20)
    if neg:
        return [t | 0x80000000, 0x41, CB_SLOT, row]  # extended operand: modifier neg
    return [t, CB_SLOT, row]


def cb_sel(row, c, neg=False):
    t = 2 | (2 << 2) | ('xyzw'.index(c) << 4) | (8 << 12) | (2 << 20)
    if neg:
        return [t | 0x80000000, 0x41, CB_SLOT, row]
    return [t, CB_SLOT, row]


def cb_rel(row_imm, rel_reg, rel_c, swz='xyzw'):
    """cb5[rT.c + row_imm] (index 1 = immediate32 + relative)."""
    s = 0
    for k, c in enumerate(swz):
        s |= 'xyzw'.index(c) << (2 * k)
    t = 2 | (1 << 2) | (s << 4) | (8 << 12) | (2 << 20) | (0 << 22) | (3 << 25)
    return [t, CB_SLOT, row_imm] + reg_sel(T_TEMP, rel_reg, rel_c)


def ins(op, *operands):
    body = [x for o in operands for x in o]
    return [op | ((1 + len(body)) << 24)] + body


MASK = {'x': 1, 'y': 2, 'z': 4, 'w': 8, 'xyzw': 15, 'yzw': 14}


# ---- signatures ------------------------------------------------------------------------------------------
def read_sig(chunk):
    n = struct.unpack_from('<I', chunk, 0)[0]
    els = []
    for k in range(n):
        e = 8 + 24 * k
        no, si, sv, ct, reg = struct.unpack_from('<5I', chunk, e)
        m, rw = chunk[e + 20], chunk[e + 21]
        end = chunk.index(b'\0', no)
        els.append(dict(name=chunk[no:end], idx=si, sv=sv, ct=ct, reg=reg, mask=m, rw=rw))
    return els


def write_sig(els):
    head = struct.pack('<II', len(els), 8)
    strings = b''
    offs = []
    base = 8 + 24 * len(els)
    names = {}
    for e in els:
        if e['name'] not in names:
            names[e['name']] = base + len(strings)
            strings += e['name'] + b'\0'
        offs.append(names[e['name']])
    body = b''.join(struct.pack('<5IBBH', o, e['idx'], e['sv'], e['ct'], e['reg'], e['mask'], e['rw'], 0)
                    for o, e in zip(offs, els))
    out = head + body + strings
    while len(out) % 4:
        out += b'\xab'
    return out


def chunks(blob):
    n = struct.unpack_from('<I', blob, 28)[0]
    res = []
    for k in range(n):
        o = struct.unpack_from('<I', blob, 32 + 4 * k)[0]
        res.append((blob[o:o + 4], blob[o + 8:o + 8 + struct.unpack_from('<I', blob, o + 4)[0]]))
    return res


def build_container(chs):
    n = len(chs)
    hdr = 32 + 4 * n
    offs, body = [], b''
    for cc, data in chs:
        offs.append(hdr + len(body))
        body += cc + struct.pack('<I', len(data)) + data
    total = hdr + len(body)
    blob = bytearray(b'DXBC' + b'\0' * 16 + struct.pack('<III', 1, total, n) + struct.pack('<%dI' % n, *offs) + body)
    blob[4:20] = dxbc_checksum(bytes(blob))
    return bytes(blob)


# ---- the patch ---------------------------------------------------------------------------------------------
def patch_vs(blob):
    chs = chunks(blob)
    names = [c for c, _ in chs]
    if names != [b'ISGN', b'OSGN', b'SHEX']:
        raise PatchError('unexpected chunks %s' % names)
    isgn, osgn = read_sig(chs[0][1]), read_sig(chs[1][1])
    shex = chs[2][1]
    toks = list(struct.unpack('<%dI' % (len(shex) // 4), shex))
    if (toks[0] >> 16) != 1:
        raise PatchError('not a vertex shader')
    pos = [e for e in osgn if e['sv'] == 1]
    if len(pos) != 1:
        raise PatchError('no SV_Position')
    P = pos[0]['reg']
    inst = [e for e in isgn if e['sv'] == 8]
    flags = 0
    if inst:
        flags |= 1
    if any(e['sv'] == 6 for e in isgn):
        flags |= 2
    clips = [e for e in osgn if e['sv'] == 2]
    if clips:
        flags |= 4
    if len({e['reg'] for e in clips}) >= 2 or any(e['sv'] == 3 for e in osgn):
        raise PatchError('clip/cull registers exhausted')
    I = inst[0]['reg'] if inst else max([e['reg'] for e in isgn] + [-1]) + 1
    C = max(e['reg'] for e in osgn) + 1
    clip_idx = (max(e['idx'] for e in clips) + 1) if clips else 0

    # walk instructions
    i = 2
    temps_at = None
    first_code = None
    rets = []
    code = []  # (start, len)
    has_cb5 = False
    while i < len(toks):
        n = opcode_len(toks, i)
        if n <= 0:
            raise PatchError('zero-length instruction')
        op = toks[i] & 0x7FF
        if op in DECL_OPS:
            if first_code is not None:
                raise PatchError('declaration after code')
            if op == OP_DCL_TEMPS:
                temps_at = i
            if op == OP_DCL_INDEXABLE_TEMP:
                raise PatchError('indexable temps')
            if op == OP_DCL_CB and toks[i + 2] == CB_SLOT:
                has_cb5 = True
        else:
            if first_code is None:
                first_code = i
            if op == OP_RETC:
                raise PatchError('conditional return')
            if op == OP_RET:
                rets.append(i)
            code.append((i, n))
        i += n
    if has_cb5:
        raise PatchError('cb5 already used')
    if len(rets) != 1 or rets[0] != code[-1][0]:
        raise PatchError('not a single final ret')
    N = toks[temps_at + 1] if temps_at is not None else 0
    T0, T1, T2 = N, N + 1, N + 2

    # redirect oP writes -> T0, native instance id reads -> T1
    redirected = 0
    for (s, n) in code:
        for (oi, idx0, typ, dim, rep0) in parse_instruction_operands(toks, s, n):
            if typ == T_OUTPUT:
                if dim != 1 or rep0 != 0:
                    raise PatchError('relative output addressing')
                if toks[idx0] == P:
                    toks[oi] = (toks[oi] & ~(0xFF << 12)) | (T_TEMP << 12)
                    toks[idx0] = T0
                    redirected += 1
            elif typ == T_INPUT and inst:
                if dim == 1 and rep0 == 0 and toks[idx0] == I:
                    toks[oi] = (toks[oi] & ~(0xFF << 12)) | (T_TEMP << 12)
                    toks[idx0] = T1
    if not redirected:
        raise PatchError('SV_Position never written')

    # declarations (inserted before dcl_temps / first instruction)
    decls = []
    decls += [OP_DCL_CB | 0x800 | (4 << 24), 2 | (1 << 2) | (0xE4 << 4) | (8 << 12) | (2 << 20), CB_SLOT, CB_ROWS]
    if not inst:
        decls += [OP_DCL_INPUT_SGV | (4 << 24)] + reg_mask(T_INPUT, I, 1) + [SB_NAME_INSTANCE_ID]
    decls += [OP_DCL_OUTPUT_SIV | (4 << 24)] + reg_mask(T_OUTPUT, C, 1) + [SB_NAME_CLIP_DISTANCE]
    prologue = []
    if inst:
        prologue += ins(OP_USHR, reg_mask(T_TEMP, T1, MASK['x']), reg_sel(T_INPUT, I, 'x'), imm1(1, False))
    prologue += ins(OP_AND, reg_mask(T_TEMP, T1, MASK['y']), reg_sel(T_INPUT, I, 'x'), imm1(1, False))
    prologue += ins(OP_ISHL, reg_mask(T_TEMP, T1, MASK['z']), reg_sel(T_TEMP, T1, 'y'), imm1(2, False))
    epi = []
    epi += ins(OP_MUL, reg_mask(T_TEMP, T2, 15), reg_swz(T_TEMP, T0, 'xxxx'), cb_rel(CB_DROW + 0, T1, 'z'))
    epi += ins(OP_MAD, reg_mask(T_TEMP, T2, 15), reg_swz(T_TEMP, T0, 'yyyy'), cb_rel(CB_DROW + 1, T1, 'z'), reg_swz(T_TEMP, T2, 'xyzw'))
    epi += ins(OP_MAD, reg_mask(T_TEMP, T2, 15), reg_swz(T_TEMP, T0, 'zzzz'), cb_rel(CB_DROW + 2, T1, 'z'), reg_swz(T_TEMP, T2, 'xyzw'))
    epi += ins(OP_MAD, reg_mask(T_TEMP, T2, 15), reg_swz(T_TEMP, T0, 'wwww'), cb_rel(CB_DROW + 3, T1, 'z'), reg_swz(T_TEMP, T2, 'xyzw'))
    # depth-clip status kept from mono (cb5 params.w > 0): a vertex inside mono's depth range (0 <= z <= w) stays inside
    # the eye's (z clamped to [0, w]). Sky/clouds pinned to z = w and fullscreen passes pinned to the near/far plane are
    # otherwise clipped along a straight line in an eye rotated even slightly against the mono camera.
    epi += ins(OP_GE, reg_mask(T_TEMP, T1, MASK['x']), reg_sel(T_TEMP, T0, 'z'), imm1(0.0, True))
    epi += ins(OP_GE, reg_mask(T_TEMP, T0, MASK['x']), reg_sel(T_TEMP, T0, 'w'), reg_sel(T_TEMP, T0, 'z'))
    epi += ins(OP_AND, reg_mask(T_TEMP, T1, MASK['x']), reg_sel(T_TEMP, T1, 'x'), reg_sel(T_TEMP, T0, 'x'))
    epi += ins(OP_LT, reg_mask(T_TEMP, T0, MASK['x']), imm1(0.0, True), cb_sel(CB_PARAMS, 'w'))
    epi += ins(OP_AND, reg_mask(T_TEMP, T1, MASK['x']), reg_sel(T_TEMP, T1, 'x'), reg_sel(T_TEMP, T0, 'x'))
    epi += ins(OP_MAX, reg_mask(T_TEMP, T0, MASK['x']), reg_sel(T_TEMP, T2, 'z'), imm1(0.0, True))
    epi += ins(OP_MIN, reg_mask(T_TEMP, T0, MASK['x']), reg_sel(T_TEMP, T0, 'x'), reg_sel(T_TEMP, T2, 'w'))
    epi += ins(OP_MOVC, reg_mask(T_TEMP, T2, MASK['z']), reg_sel(T_TEMP, T1, 'x'), reg_sel(T_TEMP, T0, 'x'), reg_sel(T_TEMP, T2, 'z'))
    epi += ins(OP_MOVC, reg_mask(T_TEMP, T1, MASK['w']), reg_sel(T_TEMP, T1, 'y'), imm1(1.0, True), imm1(-1.0, True))
    epi += ins(OP_ADD, reg_mask(T_TEMP, T0, MASK['y']), imm1(2.0, True), cb_sel(CB_PARAMS, 'x', neg=True))
    epi += ins(OP_MUL, reg_mask(T_TEMP, T0, MASK['w']), reg_sel(T_TEMP, T1, 'w'), reg_sel(T_TEMP, T2, 'w'))
    epi += ins(OP_MUL, reg_mask(T_TEMP, T0, MASK['w']), reg_sel(T_TEMP, T0, 'w'), cb_sel(CB_PARAMS, 'x'))
    epi += ins(OP_MAD, reg_mask(T_TEMP, T0, MASK['y']), reg_sel(T_TEMP, T2, 'x'), reg_sel(T_TEMP, T0, 'y'), reg_sel(T_TEMP, T0, 'w'))
    epi += ins(OP_MUL, reg_mask(T_OUTPUT, P, MASK['x']), reg_sel(T_TEMP, T0, 'y'), imm1(0.5, True))
    epi += ins(OP_MOV, reg_mask(T_OUTPUT, P, MASK['yzw']), reg_swz(T_TEMP, T2, 'xyzw'))
    epi += ins(OP_MAD, reg_mask(T_TEMP, T0, MASK['z']), reg_sel(T_TEMP, T1, 'w'), reg_sel(T_TEMP, T2, 'x'), reg_sel(T_TEMP, T2, 'w'))
    epi += ins(OP_LT, reg_mask(T_TEMP, T0, MASK['x']), imm1(0.0, True), cb_sel(CB_PARAMS, 'x'))
    epi += ins(OP_MOVC, reg_mask(T_OUTPUT, C, MASK['x']), reg_sel(T_TEMP, T0, 'x'), reg_sel(T_TEMP, T0, 'z'), imm1(1.0, True))

    insert_at = temps_at if temps_at is not None else first_code
    new_temps = [OP_DCL_TEMPS | (2 << 24), N + 3]
    head = toks[2:insert_at]
    if temps_at is not None:
        mid = toks[temps_at + 2:first_code]
    else:
        mid = toks[insert_at:first_code]
    body = toks[first_code:rets[0]]
    out = [toks[0], 0] + head + decls + new_temps + mid + prologue + body + epi + [OP_RET | (1 << 24)]
    out[1] = len(out)
    shex_new = struct.pack('<%dI' % len(out), *out)

    isgn_new = list(isgn)
    if not inst:
        isgn_new.append(dict(name=b'SV_InstanceID', idx=0, sv=SB_NAME_INSTANCE_ID, ct=1, reg=I, mask=1, rw=1))
    osgn_new = list(osgn) + [dict(name=b'SV_ClipDistance', idx=clip_idx, sv=SB_NAME_CLIP_DISTANCE, ct=3, reg=C, mask=1, rw=0x0E)]
    patched = build_container([(b'ISGN', write_sig(isgn_new)), (b'OSGN', write_sig(osgn_new)), (b'SHEX', shex_new)])
    return patched, flags


def main():
    fxp_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    pos_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'vs_pos.json')
    out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, '..', '..', '..', 'src', 'plugin', 'stereo_shaders', 'StereoVS.pack')
    vp = {r['name']: r for r in json.load(open(pos_path))}
    rows = {}
    fails = {}
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        for e in g['entries']:
            if e['stage'] != 'vs':
                continue
            name = 'g%03d_vs_%08x' % (gi, e['id'])
            r = vp.get(name)
            if not r or 'POS' not in r['outsv'] or not r['pos_cb']:
                continue  # no SV_Position (feeds HS) or no camera dependence (fullscreen / pass-through)
            pc = r['pos_cb']
            if 'cb12' in pc and any(x in pc['cb12'] for x in (8, 9, 10, 11)):
                cat = 1  # A: global ViewProj cb12[8..11]
            elif {0, 1, 3} <= set(pc.get('cb2', [])) or {3, 4, 5, 6} <= set(pc.get('cb2', [])):
                cat = 2  # B: premultiplied WorldViewProj in cb2 (rows 0..3; g007 rows 3..6)
            else:
                continue  # 2D / screen-space transforms (cb2 scale-offset, cb12[4..7]): not a world camera
            key = e['blob'][4:20]
            if key in rows:
                rows[key]['uses'] += 1
                continue
            if key in fails:
                fails[key][1] += 1
                continue
            try:
                patched, flags = patch_vs(e['blob'])
            except PatchError as ex:
                fails[key] = [name, 1, str(ex)]
                continue
            rows[key] = dict(key=key, group=gi, cat=cat, flags=flags, tech=e['id'], uses=1, blob=patched, name=name)
    out = bytearray(b'FVSP' + struct.pack('<II', 1, len(rows)))
    for x in sorted(rows.values(), key=lambda x: (x['group'], x['tech'])):
        b = x['blob']
        out += x['key'] + struct.pack('<HBBIHHI', x['group'], x['cat'], x['flags'], x['tech'], x['uses'], 0, len(b)) + b
        while len(out) % 4:
            out += b'\0'
    open(out_path, 'wb').write(out)
    by_group = {}
    for x in rows.values():
        k = (x['group'], 'A' if x['cat'] == 1 else 'B')
        a = by_group.setdefault(k, [0, 0])
        a[0] += 1
        a[1] += x['uses']
    print('wrote %s: %d patched unique VS (%d bytes); %d not patchable' % (out_path, len(rows), len(out), len(fails)))
    for k in sorted(by_group):
        print('  g%03d cat %s: %d blobs, %d technique uses' % (k[0], k[1], by_group[k][0], by_group[k][1]))
    for k, (name, n, why) in sorted(fails.items(), key=lambda kv: kv[1][0]):
        print('  NOT PATCHED %s (+%d uses): %s' % (name, n - 1, why))
    return rows


if __name__ == '__main__':
    main()
