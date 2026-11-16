"""IPD-0 halves analysis with numpy (fast). Layout-aware: --pad=<px> = per-eye guard total (eye region Wp = W + pad, pad//2 guard px left of each eye's real image).

    python halves_np.py img.bmp [--pad=N] [--band=64] [--profile]
Prints within-1/within-4 over the real eye area minus `band` px at all eye edges, and with --profile the
column/row profile of the failing share (where the residual is).
"""
import struct
import sys

import numpy as np


def read_bmp(path):
    data = open(path, "rb").read()
    off, = struct.unpack_from("<I", data, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", data, 18)
    assert bpp == 32, bpp
    top = h < 0
    h = abs(h)
    a = np.frombuffer(data, dtype=np.uint8, count=w * h * 4, offset=off).reshape(h, w, 4)
    if not top:
        a = a[::-1]
    return a[:, :, :3].astype(np.int16)  # BGR


def eyes(img, pad=0):
    """real eye images: each eye region is w/2 wide with pad//2 guard px on its left and the rest on its right"""
    h, w, _ = img.shape
    wp = w // 2
    W = wp - pad
    g = pad // 2
    return img[:, g:g + W], img[:, wp + g:wp + g + W]


def diffmap(img, pad=0):
    L, R = eyes(img, pad)
    return np.abs(L - R).max(axis=2)


def halves(img, pad=0, band=64):
    d = diffmap(img, pad)
    h, W = d.shape
    c = d[:, band:W - band]
    return {"within1": float((c <= 1).mean() * 100), "within4": float((c <= 4).mean() * 100)}


def profile(d, axis_bins=16, thr=1):
    h, W = d.shape
    bad = d > thr
    cols = bad.mean(axis=0)
    rows = bad.mean(axis=1)
    out = []
    edges = [0, 8, 16, 32, 64, 96, 128, 192, 256]
    for a, b in zip(edges, edges[1:]):
        out.append(f"x[{a},{b}) L {cols[a:b].mean()*100:.2f}% | x[W-{b},W-{a}) {cols[W-b:W-a].mean()*100:.2f}%")
    mid = cols[256:W - 256].mean() * 100
    out.append(f"x interior {mid:.3f}%")
    for i in range(axis_bins):
        a, b = i * h // axis_bins, (i + 1) * h // axis_bins
        out.append(f"y[{a},{b}) {rows[a:b].mean()*100:.3f}% (interior x {bad[a:b, 256:W-256].mean()*100:.3f}%)")
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opt = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
    pad = int(opt.get("pad", 0))
    band = int(opt.get("band", 64))
    for p in args:
        img = read_bmp(p)
        print(p, img.shape, halves(img, pad, band), "band0", halves(img, pad, 0))
        if "profile" in opt:
            print("\n".join(profile(diffmap(img, pad))))
