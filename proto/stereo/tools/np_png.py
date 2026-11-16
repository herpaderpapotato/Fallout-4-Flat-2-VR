"""Minimal PNG writer for numpy arrays (gray HxW or RGB HxWx3, uint8)."""
import struct
import zlib

import numpy as np


def write_png(path, a):
    a = np.ascontiguousarray(a.astype(np.uint8))
    h, w = a.shape[:2]
    ct = 0 if a.ndim == 2 else 2
    raw = b"".join(b"\x00" + a[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
