"""Parser for Bethesda Fallout 4 .fxp shader packages + DXBC helpers. stdlib only.

Container layout (reverse-engineered from Shaders011.fxp / Shaders012_VR.fxp):
  repeat until EOF:
    group header: u32 count[5]                 (stage order determined below)
    for each stage s, count[s] entries:
      u32 magic 0x11223344
      u32 bytecodeSize
      u32 id                                   (technique/permutation id within the group)
      [u64 shaderDesc]                         (VS / HS / DS / CS only - like BSGraphics::VertexShader)
      i8  constantTable[32]                    (offset in floats of engine constant i, -1 = unused)
      u8  cbSize[4]                            (per constant group, see analysis)
      DXBC blob (bytecodeSize bytes)
"""
import ctypes, struct, sys

MAGIC = 0x11223344
PAD = 0x55667788  # filler dwords seen in the 1.10-era / VR packages
STAGE_NAMES = {0: 'ps', 1: 'vs', 2: 'gs', 3: 'hs', 4: 'ds', 5: 'cs'}


def dxbc_stage(blob):
    """Return (stage, major, minor) from SHEX/SHDR version token."""
    for cc, off, size in dxbc_chunks(blob):
        if cc in (b'SHEX', b'SHDR'):
            tok = struct.unpack_from('<I', blob, off)[0]
            return STAGE_NAMES.get(tok >> 16, '?%d' % (tok >> 16)), (tok >> 4) & 0xF, tok & 0xF
    return None, 0, 0


def dxbc_chunks(blob):
    assert blob[:4] == b'DXBC'
    n = struct.unpack_from('<I', blob, 28)[0]
    out = []
    for k in range(n):
        o = struct.unpack_from('<I', blob, 32 + 4 * k)[0]
        cc = blob[o:o + 4]
        sz = struct.unpack_from('<I', blob, o + 4)[0]
        out.append((cc, o + 8, sz))
    return out


SV_NAMES = {0: 'NONE', 1: 'POS', 2: 'CLIPDIST', 3: 'CULLDIST', 4: 'RTINDEX', 5: 'VPINDEX',
            6: 'VERTEXID', 7: 'PRIMID', 8: 'INSTANCEID', 9: 'ISFRONTFACE', 10: 'SAMPLEINDEX',
            64: 'TARGET', 65: 'DEPTH', 66: 'COVERAGE', 67: 'DEPTHGE', 68: 'DEPTHLE'}


def parse_signature(blob, off, cc):
    count = struct.unpack_from('<I', blob, off)[0]
    esz = {b'ISGN': 24, b'OSGN': 24, b'PCSG': 24, b'OSG5': 28, b'ISG1': 32, b'OSG1': 32, b'PSG1': 32}[cc]
    elems = []
    for i in range(count):
        e = off + 8 + i * esz
        if cc == b'OSG5':
            e += 4
        elif cc in (b'ISG1', b'OSG1', b'PSG1'):
            e += 4
        nameoff, sidx, sv, ctype, reg = struct.unpack_from('<IIIII', blob, e)
        mask, rw = blob[e + 20], blob[e + 21]
        end = blob.index(b'\0', off + nameoff)
        name = blob[off + nameoff:end].decode('ascii', 'replace')
        elems.append(dict(name=name, idx=sidx, sv=SV_NAMES.get(sv, str(sv)), reg=reg, mask=mask, rw=rw))
    return elems


def signatures(blob):
    res = {}
    for cc, off, sz in dxbc_chunks(blob):
        if cc in (b'ISGN', b'OSGN', b'PCSG', b'OSG5', b'ISG1', b'OSG1', b'PSG1'):
            res[cc.decode()] = parse_signature(blob, off, cc)
    return res


def parse_fxp(path):
    data = open(path, 'rb').read()
    p = 0
    groups = []
    while p < len(data):
        counts = struct.unpack_from('<5I', data, p)
        gstart = p
        p += 20
        entries = []
        nulls = 0
        for slot, c in enumerate(counts):
            for _ in range(c):
                if struct.unpack_from('<I', data, p)[0] == PAD:
                    # a single PAD dword stands for an empty (null) shader slot
                    p += 4
                    nulls += 1
                    continue
                magic, size, sid = struct.unpack_from('<III', data, p)
                assert magic == MAGIC, (hex(p), hex(magic))
                q = p + 12
                # detect presence of the u64 desc: the DXBC must start right after header
                if data[q + 36:q + 40] == b'DXBC':
                    desc = None
                elif data[q + 44:q + 48] == b'DXBC':
                    desc = struct.unpack_from('<Q', data, q)[0]
                    q += 8
                else:
                    raise ValueError('cannot find DXBC at %#x' % p)
                ctab = list(struct.unpack_from('<32b', data, q))
                tail = data[q + 32:q + 36]
                q += 36
                blob = data[q:q + size]
                assert blob[:4] == b'DXBC'
                stage, ma, mi = dxbc_stage(blob)
                entries.append(dict(slot=slot, id=sid, desc=desc, ctab=ctab, tail=tail, blob=blob,
                                    stage=stage, sm='%d_%d' % (ma, mi), offset=q))
                p = q + size
        groups.append(dict(counts=counts, offset=gstart, entries=entries, nulls=nulls))
    return groups


# ---- D3DDisassemble via the system d3dcompiler_47.dll (data only, nothing from the archives is executed)
_d3d = None


def disasm(blob):
    global _d3d
    if _d3d is None:
        _d3d = ctypes.WinDLL(r'C:\Windows\System32\d3dcompiler_47.dll')
        _d3d.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                                        ctypes.POINTER(ctypes.c_void_p)]
        _d3d.D3DDisassemble.restype = ctypes.c_long
    buf = ctypes.create_string_buffer(bytes(blob), len(blob))
    out = ctypes.c_void_p()
    hr = _d3d.D3DDisassemble(buf, len(blob), 0, None, ctypes.byref(out))
    if hr != 0:
        return None
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vt = ctypes.cast(ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    s = ctypes.string_at(getptr(out), getsize(out)).decode('ascii', 'replace')
    release(out)
    return s.rstrip('\0')


def fnv1_64_3dmigoto(b):
    h = 0
    for c in b:
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        h ^= c
    return h


if __name__ == '__main__':
    gs = parse_fxp(sys.argv[1])
    print('groups', len(gs), 'shaders', sum(len(g['entries']) for g in gs), 'null slots', sum(g['nulls'] for g in gs))
    from collections import Counter
    print(Counter((e['slot'], e['stage']) for g in gs for e in g['entries']))
