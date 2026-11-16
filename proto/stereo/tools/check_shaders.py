"""Offline check of the PoC replacement shaders (PROTOTYPE TOOL, stdlib + system d3dcompiler_47.dll).

Compiles shaders/StereoVS.hlsl (every family/mode the plugin would compile) and StereoDebugPS.hlsl with
D3DCompile, then compares signatures against the engine blobs they replace, taken from the flat 1.11.240
Shaders011.fxp:
  - ISGN: our non-system elements must equal the original's (name, index, register, mask, component type)
    in order - the engine's input layout was created against the original bytecode;
  - OSGN (faithful mode): the original OSGN must be a register-exact prefix of ours, with only
    SV_ClipDistance appended (the engine PS then links unchanged);
  - debug VS OSGN must match the debug PS ISGN.
Nothing from the game is executed; the fxp is only parsed.
usage: python -I check_shaders.py [Shaders011.fxp]
exit code 0 = all checks pass
"""
import ctypes, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SHADERS = os.path.join(HERE, '..', '..', '..', 'src', 'plugin', 'stereo_shaders')
sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
import fxp  # noqa: E402

FXP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'work', 'shaders', 'flat', 'ShadersFX', 'Shaders011.fxp')

SV = {0: 'NONE', 1: 'POS', 2: 'CLIPDIST', 3: 'CULLDIST', 6: 'VERTEXID', 8: 'INSTANCEID', 64: 'TARGET'}


def sig(blob, cc):
    for c, off, _ in fxp.dxbc_chunks(blob):
        if c == cc:
            n = struct.unpack_from('<I', blob, off)[0]
            out = []
            for i in range(n):
                e = off + 8 + i * 24
                nameoff, sidx, sv, ctype, reg = struct.unpack_from('<IIIII', blob, e)
                mask = blob[e + 20]
                end = blob.index(b'\0', off + nameoff)
                out.append((blob[off + nameoff:end].decode().upper(), sidx,  # D3D11 linkage is case-insensitive
                            SV.get(sv, str(sv)), ctype, reg, mask))
            return out
    return []


_d3d = ctypes.WinDLL(r'C:\Windows\System32\d3dcompiler_47.dll')


class Macro(ctypes.Structure):
    _fields_ = [('Name', ctypes.c_char_p), ('Definition', ctypes.c_char_p)]


_d3d.D3DCompile.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(Macro), ctypes.c_void_p,
                            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
_d3d.D3DCompile.restype = ctypes.c_long


def blob_bytes(p):
    vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    b = ctypes.string_at(getptr(p), getsize(p))
    release(p)
    return b


def load_src(name):
    # resolve #include "x" ourselves (no ID3DInclude callback needed)
    src = open(os.path.join(SHADERS, name)).read()
    return re.sub(r'#include "([^"]+)"', lambda m: open(os.path.join(SHADERS, m.group(1))).read(), src)


def compile_(name, target, defines):
    src = load_src(name).encode()
    macros = (Macro * (len(defines) + 1))(*[Macro(k.encode(), str(v).encode()) for k, v in defines.items()],
                                          Macro(None, None))
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15
    hr = _d3d.D3DCompile(src, len(src), name.encode(), macros, None, b'main', target.encode(),
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ctypes.byref(code), ctypes.byref(err))
    msg = blob_bytes(err).decode(errors='replace') if err.value else ''
    if hr != 0:
        raise SystemExit('compile %s %s failed: %s' % (name, defines, msg))
    return blob_bytes(code), msg


def find_blob(groups, gi, tech_id):
    for e in groups[gi]['entries']:
        if e['stage'] == 'vs' and e['id'] == tech_id:
            return e['blob']
    raise KeyError((gi, tech_id))


def nonsys(s):
    return [e for e in s if e[2] in ('NONE',)]


def main():
    groups = fxp.parse_fxp(FXP)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(('  PASS ' if cond else '  FAIL ') + what)
        ok = ok and cond

    cases = [  # (label, original group, technique id, defines)
        ('PTNB faithful', 8, 0x0100001A, dict(STEREO_FAMILY=1, STEREO_FAITHFUL=1)),
        ('PTNBC faithful', 8, 0x0100001B, dict(STEREO_FAMILY=2, STEREO_FAITHFUL=1)),
        ('PTNB debug', 8, 0x0100001A, dict(STEREO_FAMILY=1, STEREO_FAITHFUL=0)),
        ('PTNBC debug', 8, 0x0100001B, dict(STEREO_FAMILY=2, STEREO_FAITHFUL=0)),
        ('PTNB debug (0180001a variant)', 8, 0x0180001A, dict(STEREO_FAMILY=1, STEREO_FAITHFUL=0)),
        ('cat-B debug (g004 POSITION)', 4, 0x00000000, dict(STEREO_FAMILY=0, STEREO_FAITHFUL=0)),
    ]
    ps, _ = compile_('StereoDebugPS.hlsl', 'ps_5_0', {})
    ps_in = sig(ps, b'ISGN')
    for label, gi, tid, d in cases:
        print('%s  (g%03d_vs_%08x)' % (label, gi, tid))
        orig = find_blob(groups, gi, tid)
        ours, warn = compile_('StereoVS.hlsl', 'vs_5_0', d)
        oi, ni = sig(orig, b'ISGN'), sig(ours, b'ISGN')
        check(nonsys(oi) == nonsys(ni), 'ISGN non-system elements identical: %s' % [(e[0], e[1], e[4], e[5]) for e in nonsys(ni)])
        check([e for e in ni if e[2] != 'NONE'] == [e for e in ni if e[2] == 'INSTANCEID'] and ni[-1][2] == 'INSTANCEID',
              'only SV_InstanceID added, last')
        oo, no = sig(orig, b'OSGN'), sig(ours, b'OSGN')
        if d['STEREO_FAITHFUL']:
            check(no[:len(oo)] == oo, 'original OSGN is a register-exact prefix of ours (%d elements)' % len(oo))
            check([e[2] for e in no[len(oo):]] == ['CLIPDIST'], 'appended: SV_ClipDistance0 only (reg %d)' % no[-1][4])
        else:
            pin = [(e[0], e[1], e[4]) for e in ps_in]
            vout = [(e[0], e[1], e[4]) for e in no if e[2] != 'CLIPDIST']
            check(vout == pin, 'debug VS OSGN links to debug PS ISGN')
        if warn.strip():
            print('    compiler: ' + warn.strip().replace('\n', '\n    compiler: '))
    ok = check_pack(groups, check) and ok
    print('ALL PASS' if ok else 'FAILURES')
    return 0 if ok else 1


# ---- generated bytecode twins (gen_stereo_vs.py -> stereo_shaders/StereoVS.pack) ---------------------------
PACK = os.path.join(SHADERS, 'StereoVS.pack')


def read_pack(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'FVSP', 'not a stereo VS pack'
    n = struct.unpack_from('<I', data, 8)[0]
    p, out = 12, []
    for _ in range(n):
        key = data[p:p + 16]
        g, cat, fl, tech, uses, _, sz = struct.unpack_from('<HBBIHHI', data, p + 16)
        p += 32
        out.append(dict(key=key, group=g, cat=cat, flags=fl, tech=tech, uses=uses, blob=data[p:p + sz]))
        p += (sz + 3) & ~3
    return out


class _Warp:
    """Minimal D3D11 WARP device through ctypes: CreateVertexShader / CreateInputLayout validation only."""

    def __init__(self):
        d3d11 = ctypes.WinDLL('d3d11.dll')
        self.dev = ctypes.c_void_p()
        fl = ctypes.c_uint()
        lv = (ctypes.c_uint * 1)(0xb000)  # D3D_FEATURE_LEVEL_11_0
        hr = d3d11.D3D11CreateDevice(None, 5, None, 0, lv, 1, 7, ctypes.byref(self.dev), ctypes.byref(fl), None)
        if hr != 0:
            raise OSError('D3D11CreateDevice(WARP) failed %#x' % (hr & 0xFFFFFFFF))
        self.vt = ctypes.cast(ctypes.cast(self.dev, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))

    def _release(self, p):
        vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(p)

    def create_vs(self, blob):
        f = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_void_p))(self.vt[12])
        out = ctypes.c_void_p()
        hr = f(self.dev, blob, len(blob), None, ctypes.byref(out))
        if hr == 0 and out.value:
            self._release(out)
        return hr & 0xFFFFFFFF

    def create_layout(self, elements, blob):
        class Desc(ctypes.Structure):
            _fields_ = [('name', ctypes.c_char_p), ('idx', ctypes.c_uint), ('fmt', ctypes.c_uint), ('slot', ctypes.c_uint),
                        ('off', ctypes.c_uint), ('cls', ctypes.c_uint), ('step', ctypes.c_uint)]
        arr = (Desc * max(1, len(elements)))()
        names = []
        off = 0
        for k, (name, idx, ctype, mask) in enumerate(elements):
            nc = bin(mask).count('1') if mask in (1, 3, 7, 15) else 4
            fmt = {1: {1: 42, 2: 16, 3: 6, 4: 2}, 2: {1: 43, 2: 17, 3: 7, 4: 3}, 3: {1: 41, 2: 16, 3: 6, 4: 2}}
            names.append(name.encode())
            # component type 1 uint, 2 sint, 3 float
            f_ = {1: (42, 17, 7, 3), 2: (43, 18, 8, 4), 3: (41, 16, 6, 2)}[ctype][nc - 1]
            arr[k] = Desc(names[-1], idx, f_, 0, off, 0, 0)
            off += 4 * nc
        f = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_size_t,
                               ctypes.POINTER(ctypes.c_void_p))(self.vt[11])
        out = ctypes.c_void_p()
        hr = f(self.dev, ctypes.cast(arr, ctypes.c_void_p), len(elements), blob, len(blob), ctypes.byref(out))
        if hr == 0 and out.value:
            self._release(out)
        return hr & 0xFFFFFFFF


def check_pack(groups, check):
    if not os.path.exists(PACK):
        print('StereoVS.pack: not generated (run gen_stereo_vs.py)')
        return False
    sys.path.insert(0, os.path.join(ROOT, 'analysis', 'tools', 'shaders'))
    from dxbc_checksum import dxbc_checksum
    orig = {}
    for gi, g in enumerate(groups):
        for e in g['entries']:
            if e['stage'] == 'vs':
                orig.setdefault(e['blob'][4:20], e['blob'])
    warp = _Warp()
    ents = read_pack(PACK)
    print('StereoVS.pack: %d bytecode twins' % len(ents))
    fails = {}

    def bad(what, e):
        fails.setdefault(what, []).append('g%03d_vs_%08x' % (e['group'], e['tech']))

    for e in ents:
        o, b = orig.get(e['key']), e['blob']
        if o is None:
            bad('key not in fxp', e)
            continue
        if dxbc_checksum(b) != b[4:20]:
            bad('checksum', e)
        if fxp.disasm(b) is None:
            bad('D3DDisassemble', e)
        oi, ni = sig(o, b'ISGN'), sig(b, b'ISGN')
        if nonsys(oi) != nonsys(ni):
            bad('ISGN non-system elements differ', e)
        if ni[:len(oi)] != oi or [x[2] for x in ni[len(oi):]] not in ([], ['INSTANCEID']) or                 [x[2] for x in ni].count('INSTANCEID') != 1:
            bad('ISGN: original + SV_InstanceID', e)
        oo, no = sig(o, b'OSGN'), sig(b, b'OSGN')
        if no[:len(oo)] != oo or [x[2] for x in no[len(oo):]] != ['CLIPDIST']:
            bad('OSGN: original prefix + SV_ClipDistance', e)
        if warp.create_vs(b) != 0:
            bad('WARP CreateVertexShader', e)
        layout = [(x[0], x[1], x[3], x[5]) for x in oi if x[2] == 'NONE']
        if layout and warp.create_layout(layout, b) != 0:
            bad('WARP CreateInputLayout (original elements vs twin)', e)
        if layout and warp.create_layout(layout, o) != 0:
            bad('WARP CreateInputLayout (original elements vs original)', e)
    for what in ('checksum', 'D3DDisassemble', 'ISGN non-system elements differ', 'ISGN: original + SV_InstanceID',
                 'OSGN: original prefix + SV_ClipDistance', 'WARP CreateVertexShader',
                 'WARP CreateInputLayout (original elements vs twin)'):
        f = fails.get(what, [])
        check(not f, '%s: %d/%d ok%s' % (what, len(ents) - len(f), len(ents), (' e.g. ' + ', '.join(f[:4])) if f else ''))
    f = fails.get('WARP CreateInputLayout (original elements vs original)', [])
    if f:
        print('  note: the synthetic layout also fails for %d originals (test artefact, not a twin problem)' % len(f))
    return not any(k for k in fails if 'vs original)' not in k)


if __name__ == '__main__':
    sys.exit(main())
