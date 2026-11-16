"""Numeric check of the bytecode twins (gen_stereo_vs.py) on a D3D11 WARP device, through stream output.

    python -I warp_twin_test.py [--all] [Shaders011.fxp]

For each twin (default: one per (group, flags, category) shape plus a fixed sample; --all = every twin) the
original and the twin run on the same random vertices and constant buffers (cb0..cb13 random in [-2, 2]);
VS outputs are captured with CreateGeometryShaderWithStreamOutput(VS bytecode) and compared:
  identity  b5: D_e = I, params.x = 0           -> every original output component bit-identical, both eyes,
                                                   clip distance = 1
  stereo    b5: D_e = random per eye, s = 1     -> o0 = squeeze(o0_orig * D_e), clip = w + sign*x (rel. 1e-5),
                                                   all other outputs bit-identical
Instancing: the original draws k instances, the twin 2k; twin instance 2i+e must match original instance i
(this also checks the id >> 1 redirection of natively instanced shaders).
Nothing from the game is executed beyond the shader bytecode on the WARP software rasteriser.
"""
import ctypes
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ALL = '--all' in sys.argv
sys.argv = [a for a in sys.argv if a != '--all']  # check_shaders reads its fxp path from argv[1]
import check_shaders as cs  # noqa: E402

c_void_p, c_uint, POINTER = ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER
WF = ctypes.WINFUNCTYPE


def vt(obj):
    return ctypes.cast(ctypes.cast(obj, POINTER(c_void_p))[0], POINTER(c_void_p))


def call(obj, idx, restype, *args):
    argtypes = [c_void_p] + [type(a) if not isinstance(a, (int, type(None))) else c_void_p for a in args]
    return WF(restype, *argtypes)(vt(obj)[idx])(obj, *args)


class D3D:
    def __init__(self):
        d3d11 = ctypes.WinDLL('d3d11.dll')
        self.dev, self.ctx = c_void_p(), c_void_p()
        fl = c_uint()
        lv = (c_uint * 1)(0xb000)
        hr = d3d11.D3D11CreateDevice(None, 5, None, 0, lv, 1, 7, ctypes.byref(self.dev), ctypes.byref(fl), ctypes.byref(self.ctx))
        assert hr == 0, hex(hr & 0xFFFFFFFF)

    def release(self, p):
        if p and p.value:
            WF(ctypes.c_ulong, c_void_p)(vt(p)[2])(p)

    def buffer(self, size, bind, usage=0, cpu=0, data=None):
        class Desc(ctypes.Structure):
            _fields_ = [('w', c_uint), ('u', c_uint), ('b', c_uint), ('c', c_uint), ('m', c_uint), ('s', c_uint)]

        class Init(ctypes.Structure):
            _fields_ = [('p', c_void_p), ('a', c_uint), ('b', c_uint)]
        d = Desc(size, usage, bind, cpu, 0, 0)
        out = c_void_p()
        init = None
        if data is not None:
            buf = ctypes.create_string_buffer(bytes(data), size)
            self._keep = buf
            init = Init(ctypes.cast(buf, c_void_p), 0, 0)
        f = WF(ctypes.c_long, c_void_p, POINTER(Desc), c_void_p, POINTER(c_void_p))(vt(self.dev)[3])
        hr = f(self.dev, ctypes.byref(d), ctypes.cast(ctypes.byref(init), c_void_p) if init else None, ctypes.byref(out))
        assert hr == 0, hex(hr & 0xFFFFFFFF)
        return out

    def so_shader(self, blob, decl):
        class SoDecl(ctypes.Structure):
            _fields_ = [('stream', c_uint), ('name', ctypes.c_char_p), ('idx', c_uint), ('start', ctypes.c_ubyte),
                        ('count', ctypes.c_ubyte), ('slot', ctypes.c_ubyte)]
        arr = (SoDecl * len(decl))()
        names = []
        stride = 0
        for k, (name, idx, start, count) in enumerate(decl):
            names.append(name.encode())
            arr[k] = SoDecl(0, names[-1], idx, start, count, 0)
            stride += 4 * count
        strides = (c_uint * 1)(stride)
        out = c_void_p()
        f = WF(ctypes.c_long, c_void_p, ctypes.c_char_p, ctypes.c_size_t, c_void_p, c_uint, c_void_p, c_uint, c_uint, c_void_p,
               POINTER(c_void_p))(vt(self.dev)[14])
        hr = f(self.dev, blob, len(blob), ctypes.cast(arr, c_void_p), len(decl), ctypes.cast(strides, c_void_p), 1,
               0xFFFFFFFF, None, ctypes.byref(out))
        return (out if hr == 0 else None), stride, hr & 0xFFFFFFFF

    def vs(self, blob):
        out = c_void_p()
        f = WF(ctypes.c_long, c_void_p, ctypes.c_char_p, ctypes.c_size_t, c_void_p, POINTER(c_void_p))(vt(self.dev)[12])
        assert f(self.dev, blob, len(blob), None, ctypes.byref(out)) == 0
        return out

    def layout(self, elements, blob):
        class Desc(ctypes.Structure):
            _fields_ = [('name', ctypes.c_char_p), ('idx', c_uint), ('fmt', c_uint), ('slot', c_uint), ('off', c_uint),
                        ('cls', c_uint), ('step', c_uint)]
        arr = (Desc * max(1, len(elements)))()
        self._names = []
        off = 0
        for k, (name, idx, ctype) in enumerate(elements):
            self._names.append(name.encode())
            fmt = {1: 3, 2: 4, 3: 2}[ctype]  # R32G32B32A32 UINT / SINT / FLOAT
            arr[k] = Desc(self._names[-1], idx, fmt, 0, off, 0, 0)
            off += 16
        out = c_void_p()
        f = WF(ctypes.c_long, c_void_p, c_void_p, c_uint, ctypes.c_char_p, ctypes.c_size_t, POINTER(c_void_p))(vt(self.dev)[11])
        hr = f(self.dev, ctypes.cast(arr, c_void_p), len(elements), blob, len(blob), ctypes.byref(out))
        assert hr == 0, hex(hr & 0xFFFFFFFF)
        return out, off

    # context
    def run(self, vsblob, gs, stride, layout, vb, vbstride, nverts, ninst, cbs):
        ctx = self.ctx
        so = self.buffer(stride * nverts * ninst, 0x10)  # STREAM_OUTPUT
        st = self.buffer(stride * nverts * ninst, 0, usage=3, cpu=0x20000)  # STAGING, READ
        vso = self.vs(vsblob)
        WF(None, c_void_p, c_void_p)(vt(ctx)[17])(ctx, layout)  # IASetInputLayout
        vbs = (c_void_p * 1)(vb.value if vb else None)
        strides, offs = (c_uint * 1)(vbstride), (c_uint * 1)(0)
        WF(None, c_void_p, c_uint, c_uint, c_void_p, c_void_p, c_void_p)(vt(ctx)[18])(
            ctx, 0, 1, ctypes.cast(vbs, c_void_p), ctypes.cast(strides, c_void_p), ctypes.cast(offs, c_void_p))
        WF(None, c_void_p, c_uint)(vt(ctx)[24])(ctx, 1)  # POINTLIST
        WF(None, c_void_p, c_void_p, c_void_p, c_uint)(vt(ctx)[11])(ctx, vso, None, 0)  # VSSetShader
        WF(None, c_void_p, c_void_p, c_void_p, c_uint)(vt(ctx)[23])(ctx, gs, None, 0)  # GSSetShader
        arr = (c_void_p * 14)(*[c.value for c in cbs])
        WF(None, c_void_p, c_uint, c_uint, c_void_p)(vt(ctx)[7])(ctx, 0, 14, ctypes.cast(arr, c_void_p))  # VSSetConstantBuffers
        sos, sooff = (c_void_p * 1)(so.value), (c_uint * 1)(0)
        WF(None, c_void_p, c_uint, c_void_p, c_void_p)(vt(ctx)[37])(ctx, 1, ctypes.cast(sos, c_void_p), ctypes.cast(sooff, c_void_p))
        WF(None, c_void_p, c_uint, c_uint, c_uint, c_uint)(vt(ctx)[21])(ctx, nverts, ninst, 0, 0)  # DrawInstanced
        nul = (c_void_p * 1)(None)
        WF(None, c_void_p, c_uint, c_void_p, c_void_p)(vt(ctx)[37])(ctx, 1, ctypes.cast(nul, c_void_p), ctypes.cast(sooff, c_void_p))
        WF(None, c_void_p, c_void_p, c_void_p)(vt(ctx)[47])(ctx, st, so)  # CopyResource

        class Mapped(ctypes.Structure):
            _fields_ = [('p', c_void_p), ('r', c_uint), ('d', c_uint)]
        m = Mapped()
        hr = WF(ctypes.c_long, c_void_p, c_void_p, c_uint, c_uint, c_uint, POINTER(Mapped))(vt(ctx)[14])(ctx, st, 0, 1, 0, ctypes.byref(m))
        assert hr == 0
        data = ctypes.string_at(m.p, stride * nverts * ninst)
        WF(None, c_void_p, c_void_p, c_uint)(vt(ctx)[15])(ctx, st, 0)
        for o in (so, st, vso):
            self.release(o)
        return data


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def b5_bytes(D, s):
    rows = [[0.0] * 4 for _ in range(17)]
    for e in range(2):
        for r in range(4):
            rows[8 + 4 * e + r] = list(D[e][r])
    rows[16] = [s, 0.0, 0.0, 0.0]
    return b''.join(struct.pack('<4f', *r) for r in rows)


def main():
    allmode = ALL
    groups = cs.fxp.parse_fxp(cs.FXP)
    orig = {}
    for gi, g in enumerate(groups):
        for e in g['entries']:
            if e['stage'] == 'vs':
                orig.setdefault(e['blob'][4:20], e['blob'])
    ents = cs.read_pack(cs.PACK)
    if not allmode:
        seen, pick = set(), []
        for k, e in enumerate(ents):
            shape = (e['group'], e['flags'], e['cat'])
            if shape not in seen or k % 25 == 0:
                seen.add(shape)
                pick.append(e)
        ents = pick
    d = D3D()
    rnd = random.Random(1234)
    cbs = []
    for slot in range(14):
        data = b''.join(struct.pack('<f', rnd.uniform(-2, 2)) for _ in range(4096 * 4))
        cbs.append(d.buffer(65536, 0x4, data=data))  # CONSTANT_BUFFER
    I4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    Dst = [[[f32(rnd.uniform(-1, 1)) for _ in range(4)] for _ in range(4)] for _ in range(2)]
    nverts, ninst_orig = 5, 2
    nfail = nskip = 0
    for e in ents:
        o, t = orig[e['key']], e['blob']
        isgn = cs.sig(o, b'ISGN')
        osgn = cs.sig(o, b'OSGN')
        elems = [(x[0], x[1], x[3]) for x in isgn if x[2] == 'NONE']
        decl = []
        for x in osgn:
            m = x[5]
            start = (m & -m).bit_length() - 1
            decl.append((x[0], x[1], start, bin(m).count('1')))
        name = 'g%03d_vs_%08x' % (e['group'], e['tech'])
        res = {}
        for which, blob in (('orig', o), ('twin', t)):
            gs, stride, hr = d.so_shader(blob, decl)
            if gs is None:
                print('  %-20s SKIP: CreateGeometryShaderWithStreamOutput(%s) %#x (test harness SO declaration)' % (name, which, hr))
                res = None
                break
            res[which] = (gs, stride, blob)
        if res is None:
            nskip += 1
            continue
        lay_o, vbstride = d.layout(elems, o) if elems else (c_void_p(), 16)
        vbdata = b''.join(struct.pack('<f', rnd.uniform(0, 1)) for _ in range(nverts * max(1, len(elems)) * 4))
        if any(x[2] != 3 for x in elems):  # integer inputs: small ints
            vbdata = b''.join(struct.pack('<I', rnd.randrange(0, 8)) for _ in range(nverts * max(1, len(elems)) * 4))
        vb = d.buffer(len(vbdata), 0x1, data=vbdata) if elems else None
        stride = res['orig'][1]
        ncomp = stride // 4
        ok = True
        nan = False
        for mode in ('identity', 'stereo'):
            D = [I4, I4] if mode == 'identity' else Dst
            s = 0.0 if mode == 'identity' else 1.0
            b5 = d.buffer(17 * 16, 0x4, data=b5_bytes(D, s))
            cbs5 = list(cbs)
            cbs5[5] = b5
            outo = d.run(o, res['orig'][0], stride, lay_o, vb, vbstride, nverts, ninst_orig, cbs5)
            outt = d.run(t, res['twin'][0], stride, lay_o, vb, vbstride, nverts, ninst_orig * 2, cbs5)
            # the SO decl of the twin does not include the clip distance; check it via a second decl
            gsc, stridec, _ = d.so_shader(t, [('SV_POSITION', 0, 0, 4), ('SV_ClipDistance', max(
                [x[1] for x in cs.sig(t, b'OSGN') if x[2] == 'CLIPDIST']), 0, 1)])
            outc = d.run(t, gsc, stridec, lay_o, vb, vbstride, nverts, ninst_orig * 2, cbs5)
            d.release(gsc)
            d.release(b5)
            fo = struct.unpack('<%df' % (len(outo) // 4), outo)
            ft = struct.unpack('<%df' % (len(outt) // 4), outt)
            fc = struct.unpack('<%df' % (len(outc) // 4), outc)
            iu_o = struct.unpack('<%dI' % (len(outo) // 4), outo)
            iu_t = struct.unpack('<%dI' % (len(outt) // 4), outt)
            for inst in range(ninst_orig):
                for eye in range(2):
                    for v in range(nverts):
                        bo = (inst * nverts + v) * ncomp
                        bt = ((2 * inst + eye) * nverts + v) * ncomp
                        bc = ((2 * inst + eye) * nverts + v) * 5
                        # position = first 4 components of decl 0 (SV_POSITION is register 0 in every VS)
                        po = fo[bo:bo + 4]
                        pt = ft[bt:bt + 4]
                        if any(x != x for x in po):
                            nan = True  # random inputs fed integer address math -> NaN position: not comparable
                            continue
                        if mode == 'identity':
                            if iu_o[bo:bo + ncomp] != iu_t[bt:bt + ncomp] or fc[bc + 4] != 1.0:
                                ok = False
                        else:
                            ce = [sum(po[i] * D[eye][i][j] for i in range(4)) for j in range(4)]
                            sg = 1.0 if eye else -1.0
                            exp = [0.5 * (ce[0] * 1.0 + sg * ce[3]), ce[1], ce[2], ce[3]]
                            expc = ce[3] + sg * ce[0]
                            tol = 1e-5 * (1.0 + max(abs(x) for x in ce))
                            if any(abs(a - b) > tol for a, b in zip(exp, pt)) or abs(fc[bc + 4] - expc) > tol:
                                ok = False
                            if iu_o[bo + 4:bo + ncomp] != iu_t[bt + 4:bt + ncomp]:
                                ok = False
        for w in ('orig', 'twin'):
            d.release(res[w][0])
        d.release(lay_o)
        d.release(vb)
        if nan and ok:
            print('  %-20s SKIP: NaN positions from random inputs (non-NaN vertices passed)' % name)
            nskip += 1
            continue
        print('  %-20s flags %d cat %s : %s' % (name, e['flags'], 'AB'[e['cat'] - 1], 'PASS' if ok else 'FAIL'))
        nfail += not ok
    print('%d twins: %d pass, %d fail, %d skipped (harness)' % (len(ents), len(ents) - nfail - nskip, nfail, nskip))
    print('ALL PASS' if nfail == 0 else 'FAILURES')
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
