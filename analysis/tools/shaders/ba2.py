"""Minimal BA2 (BTDX, GNRL) reader/extractor. stdlib only.

usage: python -I ba2.py <archive.ba2> <out_dir>      (out_dir must exist and be empty)
       python -I ba2.py <archive.ba2>                (list only)
"""
import os, struct, sys, zlib


def read_ba2(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, ver, typ, count, nameoff = struct.unpack_from('<4sI4sIQ', data, 0)
    assert magic == b'BTDX', magic
    assert typ == b'GNRL', typ
    hdr = 24
    if ver in (2, 3):  # Starfield variants have extra header fields
        hdr += 8 if ver == 2 else 12
    recs = []
    for i in range(count):
        o = hdr + i * 36
        nh, ext, dh, flags, off, packed, unpacked, align = struct.unpack_from('<I4sIIQIII', data, o)
        recs.append(dict(nameHash=nh, ext=ext.rstrip(b'\0').decode(), dirHash=dh, flags=flags,
                         offset=off, packed=packed, unpacked=unpacked))
    p = nameoff
    for r in recs:
        (n,) = struct.unpack_from('<H', data, p)
        r['name'] = data[p + 2:p + 2 + n].decode('ascii', 'replace')
        p += 2 + n
    return ver, recs, data


def payload(r, data):
    if r['packed']:
        return zlib.decompress(data[r['offset']:r['offset'] + r['packed']])
    return data[r['offset']:r['offset'] + r['unpacked']]


def main():
    path = sys.argv[1]
    ver, recs, data = read_ba2(path)
    print(f'version={ver} files={len(recs)}')
    out = sys.argv[2] if len(sys.argv) > 2 else None
    for r in recs:
        print(f"{r['name']}\tpacked={r['packed']}\tunpacked={r['unpacked']}")
        if out:
            dst = os.path.join(out, r['name'].replace('\\', os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            b = payload(r, data)
            assert len(b) == r['unpacked']
            with open(dst, 'xb') as f:
                f.write(b)


if __name__ == '__main__':
    main()
