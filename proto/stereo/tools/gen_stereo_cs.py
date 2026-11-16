"""Compute-shader twins for per-eye tiled deferred lighting (fxp g012 BSDFTiledLighting; stereo-lighting.md item 7e).

The engine runs tiled lighting as three dispatches over the double-wide target, one 8x8-pixel tile per group:
  1. tile depth bounds (g012 cs 0, 4x4 threads x 2x2 px): min/max view z per tile from depth with cb12 rows 22/23 (.zw).
     Depth was written by the eye's projection with the mono near/far, so this already yields the EYE's view z; with eye
     regions a multiple of 8 px no tile straddles the seam -> NOT patched.
  2. light culling (16 variants, N x N threads = one light each): tile frustum planes through the view origin from mono
     tangents tan = g * cb0[3].z + cb0[1].x (x, g = tile column) / row * cb0[3].w + cb0[1].w (y), lights (t6 +4: view
     position, radius) in MONO (head) view space.
  3. shading (2 variants, 8x8 threads = one pixel each): view position = cb12 rows 20-27 (inverse projection) of the
     absolute double-wide NDC, lights from the tile's list.
Per eye, each culling/shading dispatch is split into two half-width dispatches (one per eye region, plugin side). The
twins read a per-eye constant buffer in slot 13 (unused by g012):
  cb13[0].x  (int) group x offset of the eye's dispatch: every vThreadGroupID operand becomes rG = (gx + off, gy), so
             tile indices, pixel positions and the mono tangent mapping stay absolute
  cb13[1]    culling: affine map mono tile tangent -> eye tile tangent of the same pixel column/row,
             x' = x * cb13[1].x + cb13[1].y, y' = y * cb13[1].z + cb13[1].w (inserted after each of the 4 tangent mads)
  cb13[2..4] culling: head view -> eye view (rows of the 3x4 transform, dp4 with (p, 1)) applied to the light position
             right after it is loaded (t6 byte offset 4) -> the plane and depth tests run in the eye's space
Shading needs only the offset; the plugin binds the eye's b12 copy (rows 20-27 map double-wide NDC of that eye to
head view space, the space of the light positions).

Output: plugin/src/plugin/stereo_shaders/StereoCS.pack ("FCSP", version 1; rows: key16, group u16, kind u8 (1 culling,
2 shading), flags u8, tech u32, uses u16, pad u16, size u32, blob).
    python -I gen_stereo_cs.py [Shaders011.fxp] [out] [--disasm DIR]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402
from gen_stereo_vs import (PatchError, chunks, build_container, opcode_len, ins, reg_mask, reg_sel, reg_swz, T_TEMP,  # noqa: E402
                           OP_MAD, OP_MOV, DECL_OPS, parse_instruction_operands, imm1)

TILED_GROUP = 12
OP_IADD, OP_DP4, OP_LD_STRUCTURED, OP_STORE_UAV_TYPED, OP_DCL_CB, OP_DCL_TEMPS = 0x1E, 0x11, 0xA7, 0xA4, 0x59, 0x68
T_RESOURCE, T_CB, T_GROUPID = 7, 8, 0x21
SLOT = 13
KIND_CULL, KIND_SHADE = 1, 2


def cb_op(slot, row, swz):
    s = 0
    for k, c in enumerate(swz):
        s |= 'xyzw'.index(c) << (2 * k)
    return [2 | (1 << 2) | (s << 4) | (T_CB << 12) | (2 << 20), slot, row]


def cb_sel_op(slot, row, c):
    return [2 | (2 << 2) | ('xyzw'.index(c) << 4) | (T_CB << 12) | (2 << 20), slot, row]


def groupid_op(swz):
    s = 0
    for k, c in enumerate(swz):
        s |= 'xyzw'.index(c) << (2 * k)
    return [2 | (1 << 2) | (s << 4) | (T_GROUPID << 12) | (0 << 20)]


def operand_token_count(tok_list, i):
    """number of operand-token + extended tokens at i (before immediates / indices)"""
    j = i + 1
    ext = tok_list[i] >> 31
    while ext:
        ext = tok_list[j] >> 31
        j += 1
    return j - i


def rewrite_groupid(toks, q, n, rg):
    """instruction at q (n tokens): every vThreadGroupID operand -> temp rg (same swizzle/modifiers). Returns new tokens."""
    ops = parse_instruction_operands(toks, q, n)
    hits = [o for o in ops if o[2] == T_GROUPID]
    if not hits:
        return None
    out = list(toks[q:q + n])
    # process from the back so earlier positions stay valid
    for (i, _idx0, _typ, dim, _rep) in sorted(hits, key=lambda h: -h[0]):
        if dim != 0:
            raise PatchError('vThreadGroupID with an index dimension')
        rel = i - q
        t = out[rel]
        t = (t & ~(0xFF << 12)) | (T_TEMP << 12)        # type temp
        t = (t & ~(3 << 20)) | (1 << 20)                # 1 index dimension
        t &= ~(7 << 22)                                 # index 0 immediate32
        out[rel] = t
        k = rel + operand_token_count(out, rel)
        out[k:k] = [rg]
    out[0] = (out[0] & ~(0x7F << 24)) | (len(out) << 24)
    return out


def patch_cs(blob):
    chs = chunks(blob)
    k = next(i for i, (cc, _) in enumerate(chs) if cc in (b'SHEX', b'SHDR'))
    data = chs[k][1]
    toks = list(struct.unpack('<%dI' % (len(data) // 4), data))
    # classify
    q = 2
    has_typed_store = has_tgsm_u5 = False
    cull_mads = 0
    while q < len(toks):
        n = opcode_len(toks, q)
        op = toks[q] & 0x7FF
        if op == OP_STORE_UAV_TYPED:
            has_typed_store = True
        if op == OP_MAD:
            ops = parse_instruction_operands(toks, q, n)
            if len(ops) == 4 and ops[3][2] == T_CB and toks[ops[3][0] + 1] == 0 and toks[ops[3][0] + 2] == 1:
                cull_mads += 1
        q += n
    if cull_mads:
        kind = KIND_CULL
    elif has_typed_store:
        kind = KIND_SHADE
    else:
        return None, 0  # tile depth bounds: unchanged
    # declarations
    i = 2
    rg = None
    last_cb = None
    while i < len(toks) and (toks[i] & 0x7FF) in DECL_OPS:
        op = toks[i] & 0x7FF
        if op == OP_DCL_TEMPS:
            rg = toks[i + 1]
            toks[i + 1] = rg + 2
        elif op == OP_DCL_CB:
            last_cb = i
            if toks[i + 2] == SLOT:
                raise PatchError('cb13 already declared')
        i += opcode_len(toks, i)
    if rg is None or last_cb is None:
        raise PatchError('no dcl_temps / dcl_constantbuffer')
    rt = rg + 1
    dcl = list(toks[last_cb:last_cb + opcode_len(toks, last_cb)])
    dcl[2] = SLOT
    dcl[3] = 5
    ins_at = last_cb + opcode_len(toks, last_cb)
    toks[ins_at:ins_at] = dcl
    i += len(dcl)
    # prologue: rG.xy = vThreadGroupID.xy; rG.x += cb13[0].x
    pro = (ins(OP_MOV, reg_mask(T_TEMP, rg, 3), groupid_op('xyxx')) +
           ins(OP_IADD, reg_mask(T_TEMP, rg, 1), reg_sel(T_TEMP, rg, 'x'), cb_sel_op(SLOT, 0, 'x')))
    toks[i:i] = pro
    q = i + len(pro)
    gid = 0
    tan = 0
    light = 0
    while q < len(toks):
        n = opcode_len(toks, q)
        op = toks[q] & 0x7FF
        new = rewrite_groupid(toks, q, n, rg)
        if new is not None:
            toks[q:q + n] = new
            gid += 1
            n = len(new)
        if kind == KIND_CULL and op == OP_MAD:
            ops = parse_instruction_operands(toks, q, n)
            s2 = ops[3]
            if len(ops) == 4 and s2[2] == T_CB and toks[s2[0] + 1] == 0 and toks[s2[0] + 2] == 1:
                d = ops[0]
                if d[2] != T_TEMP:
                    raise PatchError('tangent mad dst not a temp')
                dreg = toks[d[0] + 1]
                dmask = (toks[d[0]] >> 4) & 0xF
                if dmask not in (1, 2, 4, 8):
                    raise PatchError('tangent mad dst mask %x' % dmask)
                dc = 'xyzw'[[1, 2, 4, 8].index(dmask)]
                sel = (toks[s2[0]] >> 4) & 3 if ((toks[s2[0]] >> 2) & 3) == 2 else None
                axis = {0: 'x', 3: 'y'}.get(sel)
                if axis is None:
                    raise PatchError('tangent start component %r' % sel)
                a, b = ('x', 'y') if axis == 'x' else ('z', 'w')
                post = ins(OP_MAD, reg_mask(T_TEMP, dreg, dmask), reg_sel(T_TEMP, dreg, dc), cb_sel_op(SLOT, 1, a), cb_sel_op(SLOT, 1, b))
                toks[q + n:q + n] = post
                tan += 1
                q += n + len(post)
                continue
        if kind == KIND_CULL and op == OP_LD_STRUCTURED:
            ops = parse_instruction_operands(toks, q, n)
            # dst, address, byte offset, resource
            d, off, res = ops[0], ops[2], ops[3]
            if res[2] == T_RESOURCE and toks[res[0] + 1] == 6 and off[2] == 4 and toks[off[0] + 1] == 4:
                if d[2] != T_TEMP or ((toks[d[0]] >> 4) & 0xF) & 7 != 7:
                    raise PatchError('light position load dst')
                dreg = toks[d[0] + 1]
                post = (ins(OP_MOV, reg_mask(T_TEMP, rt, 7), reg_swz(T_TEMP, dreg, 'xyzx')) +
                        ins(OP_MOV, reg_mask(T_TEMP, rt, 8), imm1(1.0, True)) +
                        ins(OP_DP4, reg_mask(T_TEMP, dreg, 1), cb_op(SLOT, 2, 'xyzw'), reg_swz(T_TEMP, rt, 'xyzw')) +
                        ins(OP_DP4, reg_mask(T_TEMP, dreg, 2), cb_op(SLOT, 3, 'xyzw'), reg_swz(T_TEMP, rt, 'xyzw')) +
                        ins(OP_DP4, reg_mask(T_TEMP, dreg, 4), cb_op(SLOT, 4, 'xyzw'), reg_swz(T_TEMP, rt, 'xyzw')))
                toks[q + n:q + n] = post
                light += 1
                q += n + len(post)
                continue
        q += n
    if not gid:
        raise PatchError('no vThreadGroupID use')
    if kind == KIND_CULL and (tan != 4 or light != 1):
        raise PatchError('culling: %d tangent mads, %d light loads' % (tan, light))
    toks[1] = len(toks)
    chs[k] = (chs[k][0], struct.pack('<%dI' % len(toks), *toks))
    return build_container(chs), kind


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dis_dir = None
    if '--disasm' in sys.argv:
        dis_dir = sys.argv[sys.argv.index('--disasm') + 1]
        args = [a for a in args if a != dis_dir]
    fxp_path = args[0] if len(args) > 0 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    out_path = args[1] if len(args) > 1 else os.path.join(HERE, '..', '..', '..', 'src', 'plugin', 'stereo_shaders', 'StereoCS.pack')
    rows = {}
    skipped = 0
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        if gi != TILED_GROUP:
            continue
        for e in g['entries']:
            if e['stage'] != 'cs':
                continue
            key = e['blob'][4:20]
            if key in rows:
                rows[key]['uses'] += 1
                continue
            b, kind = patch_cs(e['blob'])
            if b is None:
                skipped += 1
                continue
            if fxp.disasm(b) is None:
                raise PatchError('D3DDisassemble rejects the twin of tech %#x' % e['id'])
            rows[key] = dict(key=key, group=gi, kind=kind, tech=e['id'], uses=1, blob=b)
            if dis_dir:
                os.makedirs(dis_dir, exist_ok=True)
                open(os.path.join(dis_dir, 'g012_cs_%08x_twin.txt' % e['id']), 'w').write(fxp.disasm(b))
    out = bytearray(b'FCSP' + struct.pack('<II', 1, len(rows)))
    for x in sorted(rows.values(), key=lambda x: x['tech']):
        b = x['blob']
        out += x['key'] + struct.pack('<HBBIHHI', x['group'], x['kind'], 0, x['tech'], x['uses'], 0, len(b)) + b
        while len(out) % 4:
            out += b'\0'
    open(out_path, 'wb').write(out)
    nk = [sum(1 for x in rows.values() if x['kind'] == k) for k in (KIND_CULL, KIND_SHADE)]
    print('wrote %s: %d CS twins (%d culling, %d shading), %d unchanged (tile depth bounds)' % (out_path, len(rows), nk[0], nk[1], skipped))
    return rows


if __name__ == '__main__':
    main()
