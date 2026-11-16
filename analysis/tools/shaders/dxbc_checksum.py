"""DXBC container checksum (the 16 bytes after 'DXBC'): MD5 compression over bytes [20:] with a
non-standard final-block layout. Pure-python; verifies against every blob of a dumped .fxp.

usage: python -I dxbc_checksum.py <file.fxp> [...]
"""
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]


def _rotl(x, c):
    return ((x << c) | (x >> (32 - c))) & 0xFFFFFFFF


def _compress(state, block):
    M = struct.unpack('<16I', block)
    a, b, c, d = state
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + K[i] + M[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + _rotl(f, S[i])) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(blob):
    data = bytes(blob[20:])
    n = len(data)
    nbits = (n * 8) & 0xFFFFFFFF
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    full = n // 64
    for k in range(full):
        state = _compress(state, data[64 * k:64 * k + 64])
    last = data[64 * full:]
    tailword = struct.pack('<I', ((nbits >> 2) | 1) & 0xFFFFFFFF)
    if len(last) >= 56:
        buf = (last + b'\x80' + b'\0' * (64 - len(last) - 1) +
               struct.pack('<I', nbits) + b'\0' * 56 + tailword)
    else:
        buf = struct.pack('<I', nbits) + last + b'\x80' + b'\0' * (56 - len(last) - 1) + tailword
    assert len(buf) % 64 == 0, len(buf)
    for k in range(len(buf) // 64):
        state = _compress(state, buf[64 * k:64 * k + 64])
    return struct.pack('<4I', *state)


if __name__ == '__main__':
    import fxp
    for path in sys.argv[1:]:
        ok = bad = 0
        for g in fxp.parse_fxp(path):
            for e in g['entries']:
                if dxbc_checksum(e['blob']) == e['blob'][4:20]:
                    ok += 1
                else:
                    bad += 1
        print(path, 'checksum reproduced', ok, 'mismatch', bad)
