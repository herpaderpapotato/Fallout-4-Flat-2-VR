"""Analysis of r3_s5 popsweep / shadowturn (shadow pop at the yellow container).

    python s5_pop_analyze.py <run dir>

Per pitch: sweeps S (stereo), M (mono, same cut list), S2 (stereo again) over the same static HMD yaws.
1. stereo vs mono at the same pose: each eye pixel is mapped to the mono pixel of the same view direction (tangents
   from 'stereoscissor'); 8x8-block (64 px) luminance ratio -> share of blocks brighter (> 1.35: shadow missing in
   stereo) / darker (< 0.74).
2. pops inside a sweep, motion-compensated: frame k is warped into the view of frame k+1 (pure HMD yaw rotation;
   the yaw sign is fitted), global exposure ratio removed, blocks whose luminance still jumps > thr = pop blocks.
   Compared between S, S2 and M at the same pose pair (and for M in eye-mapped form).
3. depth-only pass signatures (stereoshadow): per pose, the shadow-map passes (not the main depth) of stereo vs mono.
Writes pop_report.json and montage PNGs of the largest pops into the run dir.
"""
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from np_png import write_png  # noqa: E402

TAN_RE = re.compile(r"tan mono ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) eyeL ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) "
                    r"eyeR ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)")


def tans(s):
    v = [float(x) for x in TAN_RE.search(s).groups()]
    return v[0:4], [v[4:8], v[8:12]]


def lum(a):
    return a.astype(np.float32).mean(axis=-1)


def rays(w, h, t):
    """pixel centres -> camera rays (x right, y up, z fwd) for tangents (l, r, t, b)"""
    l, r, tp, b = t
    xs = l + (np.arange(w) + 0.5) / w * (r - l)
    ys = tp - (np.arange(h) + 0.5) / h * (tp - b)
    X, Y = np.meshgrid(xs, ys)
    return np.stack([X, Y, np.ones_like(X)], axis=-1)


def rot(yaw_deg, pitch_deg):
    y, p = np.radians(yaw_deg), np.radians(pitch_deg)
    Ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    Rp = np.array([[1, 0, 0], [0, np.cos(p), np.sin(p)], [0, -np.sin(p), np.cos(p)]])
    return Ry @ Rp


def sample(img, w, h, t, v):
    """sample img (h x w) with tangents t at camera rays v -> values, valid mask"""
    l, r, tp, b = t
    z = v[..., 2]
    ok = z > 1e-3
    tx = v[..., 0] / np.where(ok, z, 1)
    ty = v[..., 1] / np.where(ok, z, 1)
    fx = (tx - l) / (r - l) * w - 0.5
    fy = (tp - ty) / (tp - b) * h - 0.5
    ok &= (fx >= 0) & (fx <= w - 1) & (fy >= 0) & (fy <= h - 1)
    ix = np.clip(np.rint(fx).astype(int), 0, w - 1)
    iy = np.clip(np.rint(fy).astype(int), 0, h - 1)
    return img[iy, ix], ok


def blocks(a, m, blk=8):
    h, w = a.shape
    hb, wb = h // blk, w // blk
    A = a[:hb * blk, :wb * blk].reshape(hb, blk, wb, blk)
    M = m[:hb * blk, :wb * blk].reshape(hb, blk, wb, blk)
    cnt = M.sum(axis=(1, 3))
    s = (A * M).sum(axis=(1, 3))
    return s / np.maximum(cnt, 1), cnt == blk * blk


def views(frame, mode, tm, te):
    """-> list of (image lum, tangents): mono = the whole frame, stereo = two eye regions"""
    L = lum(frame)
    h, w = L.shape
    if mode == 0:
        return [(L, tm)]
    return [(L[:, :w // 2], te[0]), (L[:, w // 2:], te[1])]


def warp_pair(a, b, t, dyaw, pitch):
    """warp image a (pose k) into b's view (pose k+1 = k + dyaw); returns a', valid"""
    h, w = b.shape
    v = rays(w, h, t)
    Rk = rot(0, pitch)
    Rk1 = rot(dyaw, pitch)
    vw = v @ Rk1.T            # world
    va = vw @ Rk               # into frame k camera (R^T applied as row-vector @ R)
    return sample(a, w, h, t, va)


def pop_series(frames, mode, tm, te, yaws, pitch, sign, thr=25.0):
    out = []
    for k in range(len(frames) - 1):
        va, vb = views(frames[k], mode, tm, te), views(frames[k + 1], mode, tm, te)
        dy = sign * (yaws[k + 1] - yaws[k])
        n = tot = 0
        maps = []
        for (a, t), (b, _) in zip(va, vb):
            aw, ok = warp_pair(a, b, t, dy, pitch)
            ba, okb = blocks(b, ok)
            wa, _ = blocks(aw, ok)
            g = np.median((ba[okb] + 4) / (wa[okb] + 4)) if okb.any() else 1.0
            d = np.abs(ba - wa * g)
            d[~okb] = 0
            # ignore the outer 2 block rows/cols (content entering the view, warp edge effects)
            d[:2], d[-2:], d[:, :2], d[:, -2:] = 0, 0, 0, 0
            n += int((d > thr).sum())
            tot += int(okb.sum())
            maps.append(d)
        out.append({"pair": k, "pop": n, "share": round(n / max(tot, 1) * 100, 2), "maps": maps})
    return out


def fit_sign(frames, mode, tm, te, yaws, pitch):
    errs = {}
    for s in (1, -1):
        e = 0.0
        for k in range(0, len(frames) - 1, 4):
            va, vb = views(frames[k], mode, tm, te), views(frames[k + 1], mode, tm, te)
            for (a, t), (b, _) in zip(va, vb):
                aw, ok = warp_pair(a, b, t, s * (yaws[k + 1] - yaws[k]), pitch)
                e += float(np.abs(aw - b)[ok].mean())
        errs[s] = e
    return min(errs, key=errs.get), errs


def stereo_vs_mono(fs, fm, tm, te, blk=8):
    """per eye: block ratio stereo / mono (same view direction)"""
    S = lum(fs)
    M = lum(fm)
    h, w = S.shape
    hm, wm = M.shape
    res = []
    for e in range(2):
        E = S[:, e * w // 2:(e + 1) * w // 2]
        v = rays(w // 2, h, te[e])
        mv, ok = sample(M, wm, hm, tm, v)
        a, okb = blocks(E, ok)
        m, _ = blocks(mv, ok)
        ratio = (a + 4) / (m + 4)
        res.append((ratio, okb))
    return res


def sig_parse(lines, main_w=5504):
    """-> list of frames: dict key -> (draws, prims, hash) for depth-only passes that are not the main depth"""
    out = []
    for ln in lines:
        m = re.match(r"f (\d+) st (\d) n (\d+)(.*)$", ln)
        if not m:
            continue
        fr = {}
        for part in m.group(4).split(" | ")[1:]:
            mm = re.match(r"(\d+)x(\d+)s(\d+) vp ([-\d]+),([-\d]+),(\d+)x(\d+) d (\d+) p (\d+) h ([0-9a-f]+)", part.strip())
            if not mm:
                continue
            W = int(mm.group(1))
            if W == main_w:
                continue
            key = f"{mm.group(1)}x{mm.group(2)}s{mm.group(3)}"  # cascade slice (its fitted viewport is the 4th field)
            fr[key] = (int(mm.group(8)), int(mm.group(9)), mm.group(10), f"{mm.group(4)},{mm.group(5)},{mm.group(6)}x{mm.group(7)}")
        out.append((int(m.group(2)), fr))
    return out


def montage(path, imgs, scale=1):
    h = max(i.shape[0] for i in imgs)
    row = np.concatenate([np.pad(i, ((0, h - i.shape[0]), (0, 4), (0, 0))) for i in imgs], axis=1)
    write_png(path, row[::scale, ::scale])


def main():
    d = sys.argv[1]
    res = json.load(open(os.path.join(d, "results.json")))
    P = res["popsweep"]
    yaws = P["yaws"]
    rep = {}
    for pitch in (0, 15, -20):
        keyS, keyM, keyS2 = f"p{pitch}_S", f"p{pitch}_M", f"p{pitch}_S2"
        if keyS2 not in P:
            continue
        F = {k: np.load(os.path.join(d, f"popsweep_{k}.npz"))["frames"][..., ::-1] for k in (keyS, keyM, keyS2)}  # -> RGB
        tm, te = tans(P[keyS]["tans"])
        R = rep[f"p{pitch}"] = {}
        sgn, errs = fit_sign(F[keyM], 0, tm, te, yaws, pitch)
        R["yawSign"] = sgn
        R["yawSignErr"] = errs
        # 1. stereo vs mono per pose
        svm = []
        for k in range(len(yaws)):
            row = {}
            for name, key in (("S", keyS), ("S2", keyS2)):
                rr = stereo_vs_mono(F[key][k], F[keyM][k], tm, te)
                row[name] = [(round(float((r[ok] > 1.35).mean() * 100), 1), round(float((r[ok] < 0.74).mean() * 100), 1)) for r, ok in rr]
            svm.append(row)
        R["stereoVsMono_brighter_darker_perEye"] = svm
        # 2. pops
        pops = {}
        for name, key, mode in (("S", keyS, 3), ("M", keyM, 0), ("S2", keyS2, 3)):
            ps = pop_series(F[key], mode, tm, te, yaws, pitch, sgn)
            pops[name] = ps
        R["pops"] = {n: [p["pop"] for p in v] for n, v in pops.items()}
        # worst pops (stereo) with the mono pair at the same poses
        order = np.argsort([-p["pop"] for p in pops["S"]])[:4]
        R["worstS"] = []
        for k in order:
            k = int(k)
            R["worstS"].append({"pair": k, "yaw": [yaws[k], yaws[k + 1]], "S": pops["S"][k]["pop"], "S2": pops["S2"][k]["pop"],
                                "M": pops["M"][k]["pop"]})
            montage(os.path.join(d, f"pop_p{pitch}_pair{k:02}.png"),
                    [F[keyS][k], F[keyS][k + 1], F[keyM][k], F[keyM][k + 1]])
        # 3. shadow-map signatures
        sig = {}
        for name, key in (("S", keyS), ("M", keyM), ("S2", keyS2)):
            sig[name] = [sig_parse(lines) for lines in P[key]["sigs"]]
        same = []
        changes = {n: 0 for n in sig}
        for k in range(len(yaws)):
            def last(n):
                fr = [f for st, f in sig[n][k]]
                return fr[-1] if fr else {}
            s, m = last("S"), last("M")
            keys = sorted(set(s) | set(m))
            same.append({"keys": len(keys), "sameDraws": sum(1 for x in keys if s.get(x, (None,))[0] == m.get(x, (None,))[0]),
                         "sameHash": sum(1 for x in keys if s.get(x, (0, 0, None))[2] == m.get(x, (0, 0, None))[2]),
                         "S": {x: s[x][0] for x in keys if x in s}, "M": {x: m[x][0] for x in keys if x in m}})
            for n in sig:
                if k and sig[n][k] and sig[n][k - 1]:
                    a = sig[n][k][-1][1]
                    b = sig[n][k - 1][-1][1]
                    changes[n] += int({x: v[2] for x, v in a.items()} != {x: v[2] for x, v in b.items()})
        R["sigStereoVsMono"] = same
        R["sigChangesAlongSweep"] = changes
        print(f"pitch {pitch}: yaw sign {sgn} {errs}")
        print("  pops S ", R["pops"]["S"])
        print("  pops S2", R["pops"]["S2"])
        print("  pops M ", R["pops"]["M"])
        print("  worst S", R["worstS"])
        print("  stereo vs mono brighter% e0/e1 per pose:", [(r["S"][0][0], r["S"][1][0]) for r in svm])
        print("  sig draws per cascade S | M:", [(x["S"], x["M"]) for x in same][::4])
        print("  sig changes along sweep:", changes)
    if "shadowturn" in res:
        T = res["shadowturn"]
        rep["shadowturn"] = {}
        for k, lines in T.items():
            if not isinstance(lines, list):
                continue
            fr = sig_parse(lines)
            hs = [{x: v[2] for x, v in f.items()} for st, f in fr]
            dr = [sum(v[0] for v in f.values()) for st, f in fr]
            ch = sum(1 for a, b in zip(hs, hs[1:]) if a != b)
            rep["shadowturn"][k] = {"frames": len(fr), "sigChanges": ch, "drawsMin": min(dr) if dr else None, "drawsMax": max(dr) if dr else None,
                                    "keys": sorted(set().union(*[set(h) for h in hs])) if hs else []}
            print("shadowturn", k, {a: b for a, b in rep["shadowturn"][k].items() if a != "keys"})
    json.dump(rep, open(os.path.join(d, "pop_report.json"), "w"), indent=1,
              default=lambda o: o.tolist() if hasattr(o, "tolist") else str(o))


if __name__ == "__main__":
    main()
