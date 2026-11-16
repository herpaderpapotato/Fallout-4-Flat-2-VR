"""Pixel-shader twins for the stereo split draws (stereo-lighting.md item 1: SAO noise per eye).

SAO RawAO (fxp g125, ImageSpaceEffectScalableAmbientObscurance's raw-AO PS) starts with
    mul r0.xy, v1.xyxx, cb2[2].xyxx      ; pixel position = uv * target size
and derives from r0 both the per-pixel noise hash (ftoi r0.zw -> (3x) ^ (x*y + y)) and the view position
(round_z, +0.5, * projInfo). Over the double-wide target the right eye's x is offset by W, so its noise differs.
The twin inserts right after it
    add r0.x, r0.x, -cb2[3].w            ; cb2[3].w is unused by the original
so with cb2[3].w = e * W (the plugin's per-eye cb2 copy) both eyes see eye-relative pixel x: identical noise, and
the per-eye projInfo maps the eye's own pixel range [0, W). Texture taps still use the absolute uv (v1).

Output: plugin/src/plugin/stereo_shaders/StereoPS.pack ("FPSP", version 1, same row layout as StereoVS.pack).
    python -I gen_stereo_ps.py [Shaders011.fxp] [out]
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
                           OP_ADD, OP_MUL, OP_MOV, OP_MAD, DECL_OPS, parse_instruction_operands, imm1)

SAO_RAW_GROUP = 125
SSR_TRACE_GROUP = 144
OP_GE, OP_AND = 0x1D, 0x01
T_INPUT, T_CB, T_IMM32 = 1, 8, 4


OP_DIV, OP_MAX, OP_MIN = 0x0E, 0x34, 0x33


def cb2_sel(row, c):
    return [2 | (2 << 2) | ('xyzw'.index(c) << 4) | (T_CB << 12) | (2 << 20), 2, row]


def reg_sel_neg(typ, reg, c):
    return [2 | (2 << 2) | ('xyzw'.index(c) << 4) | (typ << 12) | (1 << 20) | 0x80000000, 0x41, reg]


def cb2_sel_neg(row, c):
    t = 2 | (2 << 2) | ('xyzw'.index(c) << 4) | (T_CB << 12) | (2 << 20) | 0x80000000
    return [t, 0x41, 2, row]


def patch_sao(blob):
    chs = chunks(blob)
    k = next(i for i, (cc, _) in enumerate(chs) if cc in (b'SHEX', b'SHDR'))
    data = chs[k][1]
    toks = list(struct.unpack('<%dI' % (len(data) // 4), data))
    i = 2
    while i < len(toks) and (toks[i] & 0x7FF) in DECL_OPS:
        if (toks[i] & 0x7FF) == 0x59 and toks[i + 2] == 2 and toks[i + 3] < 5:
            toks[i + 3] = 5  # dcl_constantbuffer CB2[5]: row 4 = per-eye radial-bias uv map
        i += opcode_len(toks, i)
    first = toks[i:i + opcode_len(toks, i)]
    # mul r0.xy, v1.xyxx, cb2[2].xyxx
    want = ins(OP_MUL, reg_mask(T_TEMP, 0, 3),
               [2 | (1 << 2) | ((0 | 1 << 2 | 0 << 4 | 0 << 6) << 4) | (T_INPUT << 12) | (1 << 20), 1],
               [2 | (1 << 2) | ((0 | 1 << 2 | 0 << 4 | 0 << 6) << 4) | (T_CB << 12) | (2 << 20), 2, 2])
    if first != want:
        raise PatchError('first instruction is not "mul r0.xy, v1.xyxx, cb2[2].xyxx": %s vs %s' % (
            ' '.join('%08x' % t for t in first), ' '.join('%08x' % t for t in want)))
    add = ins(OP_ADD, reg_mask(T_TEMP, 0, 1), reg_sel(T_TEMP, 0, 'x'), cb2_sel_neg(3, 'w'))
    j = i + len(first)
    toks[j:j] = add
    # The rotation-noise texture t1 is sampled at the absolute uv (v1): give it the eye-relative uv in a new temp
    #   div r7.x, cb2[3].w, cb2[2].x ; add r7.x, v1.x, -r7.x ; mov r7.y, v1.y ; sample_d ... r7.xyxx, t1 ...
    q = 2
    ntemps = None
    taps = 0
    radial = False
    while q < len(toks):
        n = opcode_len(toks, q)
        op = toks[q] & 0x7FF
        if op == 0x68:  # dcl_temps
            ntemps = toks[q + 1]
            toks[q + 1] = ntemps + 2
        elif op == 0x49:  # sample_d
            ops = parse_instruction_operands(toks, q, n)
            # operands in order: dst, coord, resource, sampler, ddx, ddy
            coord, res = ops[1], ops[2]
            if res[2] == 7 and toks[res[1]] == 1 and coord[2] == T_INPUT and toks[coord[1]] == 1:
                t = toks[coord[0]]
                toks[coord[0]] = (t & ~(0xFF << 12)) | (T_TEMP << 12)
                toks[coord[1]] = ntemps
                pre = (ins(OP_DIV, reg_mask(T_TEMP, ntemps, 1), cb2_sel(3, 'w'), cb2_sel(2, 'x')) +
                       ins(OP_ADD, reg_mask(T_TEMP, ntemps, 8), reg_sel(T_TEMP, ntemps, 'x'), imm1(0.5, True)) +
                       ins(OP_MOV, reg_mask(T_TEMP, ntemps, 4), reg_sel(T_TEMP, ntemps, 'x')) +
                       ins(OP_ADD, reg_mask(T_TEMP, ntemps, 1), reg_sel(T_INPUT, 1, 'x'), reg_sel_neg(T_TEMP, ntemps, 'x')) +
                       ins(OP_MOV, reg_mask(T_TEMP, ntemps, 2), reg_sel(T_INPUT, 1, 'y')))
                toks[q:q] = pre
                q += len(pre) + n
                continue
        elif op == OP_MAD and ntemps is not None and not radial:
            # mad r3.xz, v1.xxyx, l(2, 0, 2, 0), l(-1, 0, -1, 0): |screen NDC|^2 feeds the AO bias (stronger towards the
            # screen edges). Over the double-wide target that is measured from the seam, not from the eye's centre: give it
            # the eye-local uv (2 x the eye-relative uv in rT.x, set by the noise patch above) in a second new temp rM.
            ops = parse_instruction_operands(toks, q, n)
            src0 = ops[1]
            if src0[2] == T_INPUT and toks[src0[1]] == 1:
                t = toks[src0[0]]
                toks[src0[0]] = (t & ~(0xFF << 12)) | (T_TEMP << 12)
                toks[src0[1]] = ntemps + 1
                # rM = (rT.x * cb2[4].x + cb2[4].y, v1.y * cb2[4].z + cb2[4].w): the uv whose 2u-1 is the MONO screen NDC
                # of this pixel's view direction (plugin: SaoEyeProjInfo), so the bias matches mono for any eye frustum
                # (asymmetric HMD eyes); symmetric eyes = mono give (2 rT.x, v1.y) as before
                pre = (ins(OP_MAD, reg_mask(T_TEMP, ntemps + 1, 1), reg_sel(T_TEMP, ntemps, 'x'), cb2_sel(4, 'x'), cb2_sel(4, 'y')) +
                       ins(OP_MAD, reg_mask(T_TEMP, ntemps + 1, 2), reg_sel(T_INPUT, 1, 'y'), cb2_sel(4, 'z'), cb2_sel(4, 'w')))
                toks[q:q] = pre
                radial = True
                q += len(pre) + n
                continue
        elif op == 0x48 and ntemps is not None:  # sample_l: the AO taps (depth t0 at r4.ywyy) -> x clamped to the eye's half
            ops = parse_instruction_operands(toks, q, n)
            coord, res = ops[1], ops[2]
            if res[2] == 7 and toks[res[1]] == 0 and coord[2] == T_TEMP:
                reg = toks[coord[1]]
                sw = (toks[coord[0]] >> 4) & 0xFF
                cx = 'xyzw'[sw & 3]
                pre = (ins(OP_MAX, reg_mask(T_TEMP, reg, 1 << 'xyzw'.index(cx)), reg_sel(T_TEMP, reg, cx), reg_sel(T_TEMP, ntemps, 'z')) +
                       ins(OP_MIN, reg_mask(T_TEMP, reg, 1 << 'xyzw'.index(cx)), reg_sel(T_TEMP, reg, cx), reg_sel(T_TEMP, ntemps, 'w')))
                toks[q:q] = pre
                taps += 1
                q += len(pre) + n
                continue
        q += n
    if not taps or not radial:
        raise PatchError('noise sample_d / AO tap sample_l / radial-bias mad not found')
    if ntemps is None:
        raise PatchError('no dcl_temps')
    toks[1] = len(toks)
    chs[k] = (chs[k][0], struct.pack('<%dI' % len(toks), *toks))
    # STAT instruction counts are informational only; leave them
    return build_container(chs)


def patch_ssr(blob):
    """SSLR ray march (g144): the ray position r6.xy is tested against the screen bounds [0, 1] (ge with immediate 0/1
    vectors); over the double-wide target a ray leaving one eye walks into the other eye's image. The twin tests against
    the pixel's own eye region instead: rE = (lo, 0, hi, 1) with lo = (v1.x >= 0.5) ? 0.5 : 0, hi = lo + 0.5, and every
    `ge` comparing r6.x/r6.y with immediate 0/1 takes rE components (x -> lo/hi, y -> 0/1). A ray now ends at its eye's
    border exactly as it ends at the screen border in mono."""
    chs = chunks(blob)
    k = next(i for i, (cc, _) in enumerate(chs) if cc in (b'SHEX', b'SHDR'))
    data = chs[k][1]
    toks = list(struct.unpack('<%dI' % (len(data) // 4), data))
    i = 2
    rE = None
    while i < len(toks) and (toks[i] & 0x7FF) in DECL_OPS:
        if (toks[i] & 0x7FF) == 0x68:
            rE = toks[i + 1]
            toks[i + 1] = rE + 1
        i += opcode_len(toks, i)
    if rE is None:
        raise PatchError('no dcl_temps')
    pro = (ins(OP_GE, reg_mask(T_TEMP, rE, 1), reg_sel(T_INPUT, 1, 'x'), imm1(0.5, True)) +
           ins(OP_AND, reg_mask(T_TEMP, rE, 1), reg_sel(T_TEMP, rE, 'x'), imm1(0x3F000000, False)) +
           ins(OP_ADD, reg_mask(T_TEMP, rE, 4), reg_sel(T_TEMP, rE, 'x'), imm1(0.5, True)) +
           ins(OP_MOV, reg_mask(T_TEMP, rE, 2), imm1(0.0, True)) +
           ins(OP_MOV, reg_mask(T_TEMP, rE, 8), imm1(1.0, True)))
    toks[i:i] = pro
    q = i + len(pro)
    fixed = 0
    while q < len(toks):
        n = opcode_len(toks, q)
        if (toks[q] & 0x7FF) == OP_GE:
            ops = parse_instruction_operands(toks, q, n)
            if len(ops) == 3:
                (d0, _, _, _, _), (a0, ai, at, _, _), (b0, bi, bt, _, _) = ops
                end = [a0, b0, q + n]
                srcs = [(a0, ai, at, toks[a0:b0]), (b0, bi, bt, toks[b0:q + n])]
                mask = (toks[d0] >> 4) & 0xF
                pick = None
                for si, (o0, oi, ot, otoks) in enumerate(srcs):
                    other = srcs[1 - si]
                    if ot == T_TEMP and toks[oi] == 6 and other[2] == T_IMM32 and (toks[other[0]] & 3) == 2:
                        pick = (si, otoks, other)
                if pick:
                    si, rtoks, other = pick
                    sel_mode = (rtoks[0] >> 2) & 3
                    vals = [struct.unpack('<f', struct.pack('<I', v))[0] for v in toks[other[0] + 1:other[0] + 5]]
                    swz = ''
                    ok = sel_mode == 1
                    for c in range(4):
                        if not (mask >> c) & 1:
                            swz += 'x'
                            continue
                        sc = (rtoks[0] >> (4 + 2 * c)) & 3
                        v = vals[c]
                        if sc == 0 and v in (0.0, 1.0):
                            swz += 'x' if v == 0.0 else 'z'
                        elif sc == 1 and v in (0.0, 1.0):
                            swz += 'y' if v == 0.0 else 'w'
                        else:
                            ok = False
                    if ok:
                        e = reg_swz(T_TEMP, rE, swz)
                        new_srcs = [rtoks, e] if si == 0 else [e, rtoks]
                        new = ins(OP_GE, toks[d0:a0], *new_srcs)
                        toks[q:q + n] = new
                        fixed += 1
                        q += len(new)
                        continue
        q += n
    if fixed < 4:
        raise PatchError('SSR bounds tests found: %d' % fixed)
    toks[1] = len(toks)
    chs[k] = (chs[k][0], struct.pack('<%dI' % len(toks), *toks))
    return build_container(chs), fixed


def main():
    fxp_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', '..', 'src', 'plugin', 'stereo_shaders', 'StereoPS.pack')
    rows = {}
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        if gi not in (SAO_RAW_GROUP, SSR_TRACE_GROUP):
            continue
        for e in g['entries']:
            if e['stage'] != 'ps':
                continue
            key = e['blob'][4:20]
            if key in rows:
                continue
            if gi == SAO_RAW_GROUP:
                b = patch_sao(e['blob'])
            else:
                b, nfix = patch_ssr(e['blob'])
                print('g144 tech %#x: %d bounds tests -> eye region' % (e['id'], nfix))
            rows[key] = dict(key=key, group=gi, tech=e['id'], blob=b)
    out = bytearray(b'FPSP' + struct.pack('<II', 1, len(rows)))
    for x in rows.values():
        b = x['blob']
        out += x['key'] + struct.pack('<HBBIHHI', x['group'], 1, 0, x['tech'], 1, 0, len(b)) + b
        while len(out) % 4:
            out += b'\0'
    open(out_path, 'wb').write(out)
    print('wrote %s: %d PS twins' % (out_path, len(rows)))
    return rows


if __name__ == '__main__':
    main()
