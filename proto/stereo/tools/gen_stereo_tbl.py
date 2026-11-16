"""Generate stereo_shaders/StereoShaders.tbl: per engine VS/PS blob (Shaders011.fxp), which cb12 (per-frame) rows its
code reads and how big its cb2 is. Used at runtime (StereoDraw.cpp, CreatePixelShader/CreateVertexShader hooks) to
decide whether a draw needs the per-eye per-frame buffer (split draw) or is eye-agnostic (instancing is enough).

    python -I proto/stereo/tools/gen_stereo_tbl.py [fxp] [out]

Format (little endian): 'FSTB' u32 version=1 u32 count, then count x 32 bytes:
    key[16] (DXBC checksum = blob[4:20]) u16 group u8 stage (0 VS, 1 PS) u8 flags (1 = cb12 relative-indexed)
    u32 cb2Rows (dcl size of cb2, 0 = none) u64 cb12RowMask (bit r = row r read; rows >= 64 fold into bit 63)
Rows are taken from the instruction operands (immediate cb12[r]); a relative-indexed cb12 access marks every
declared row. stdlib only.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402

T_IMM32, T_IMM64, T_CB = 4, 5, 8
OP_CUSTOMDATA, OP_DCL_CB = 0x35, 0x59
DECL_OPS = set(range(0x58, 0x6B)) | set(range(0x8D, 0xA3)) | {0xCE, OP_CUSTOMDATA}


def shex(blob):
    n = struct.unpack_from('<I', blob, 28)[0]
    for k in range(n):
        off = struct.unpack_from('<I', blob, 32 + 4 * k)[0]
        cc, size = blob[off:off + 4], struct.unpack_from('<I', blob, off + 4)[0]
        if cc in (b'SHEX', b'SHDR'):
            return list(struct.unpack_from('<%dI' % (size // 4), blob, off + 8))
    raise ValueError('no SHEX')


def operand(toks, i, out):
    """Parse one operand; append (type, dim, [(rep, value_or_None), ...]) for it and nested relative operands."""
    tok = toks[i]
    ncomp, typ, dim = tok & 3, (tok >> 12) & 0xFF, (tok >> 20) & 3
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
    idx = []
    for d in range(dim):
        r = reps[d]
        if r == 0:
            idx.append((0, toks[j]))
            j += 1
        elif r == 1:
            idx.append((1, toks[j]))
            j += 2
        elif r == 2:
            idx.append((2, None))
            j = operand(toks, j, out)
        elif r == 3:
            idx.append((3, toks[j]))
            j = operand(toks, j + 1, out)
        elif r == 4:
            idx.append((4, None))
            j = operand(toks, j + 2, out)
        else:
            raise ValueError('index rep %d' % r)
    out.append((typ, dim, idx))
    return j


def analyse(blob):
    toks = shex(blob)
    i = 2  # version, length
    rows, dyn, dcl = set(), False, {}
    while i < len(toks):
        t = toks[i]
        op = t & 0x7FF
        n = toks[i + 1] if op == OP_CUSTOMDATA else (t >> 24) & 0x7F
        if n == 0:
            break
        if op == OP_DCL_CB:
            ops = []
            operand(toks, i + 1, ops)
            typ, dim, idx = ops[-1]
            if typ == T_CB and dim == 2:
                dcl[idx[0][1]] = idx[1][1]
        elif op not in DECL_OPS:
            j = i + 1
            ext = t >> 31
            while ext:
                ext = toks[j] >> 31
                j += 1
            ops = []
            while j < i + n:
                j = operand(toks, j, ops)
            for typ, dim, idx in ops:
                if typ == T_CB and dim == 2 and idx[0][0] == 0 and idx[0][1] == 12:
                    rep, v = idx[1]
                    if rep == 0:
                        rows.add(v)
                    else:
                        dyn = True
        i += n
    if dyn:
        rows |= set(range(dcl.get(12, 64)))
    return rows, dyn, dcl.get(2, 0)


def main():
    fxp_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', '..', 'src', 'plugin', 'stereo_shaders',
                                                                  'StereoShaders.tbl')
    rows = {}
    stats = {}
    for gi, g in enumerate(fxp.parse_fxp(fxp_path)):
        for e in g['entries']:
            if e['stage'] not in ('vs', 'ps'):
                continue
            key = e['blob'][4:20]
            if key in rows:
                continue
            r, dyn, cb2 = analyse(e['blob'])
            mask = 0
            for x in r:
                mask |= 1 << min(x, 63)
            rows[key] = (gi, 0 if e['stage'] == 'vs' else 1, 1 if dyn else 0, cb2, mask)
            s = stats.setdefault((gi, e['stage']), [0, 0])
            s[0] += 1
            s[1] += 1 if (mask & ~0xF) else 0  # reads a row beyond View (0..3)
    out = bytearray(b'FSTB' + struct.pack('<II', 1, len(rows)))
    for key, (gi, st, fl, cb2, mask) in sorted(rows.items(), key=lambda kv: (kv[1][0], kv[1][1], kv[0])):
        out += key + struct.pack('<HBBIQ', gi, st, fl, cb2, mask)
    open(out_path, 'wb').write(out)
    print('wrote %s: %d unique VS/PS blobs' % (out_path, len(rows)))
    for (gi, st), (n, cam) in sorted(stats.items()):
        if gi in (2, 3, 4, 7, 9, 10, 125) or cam:
            pass
    for gi in (2, 3, 4, 7, 8, 9, 10, 18, 19, 125, 137):
        for st in ('vs', 'ps'):
            if (gi, st) in stats:
                n, cam = stats[(gi, st)]
                print('  g%03d %s: %d blobs, %d read cb12 rows >= 4' % (gi, st, n, cam))


if __name__ == '__main__':
    sys.exit(main())
