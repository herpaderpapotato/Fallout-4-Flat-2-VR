"""Sinusoidal-motion analysis of an htrace CSV (input = head pose, output = camera root the image was rendered with).

Per presented frame N (C record = camera root world at Present N):
  out   : yaw / pitch / roll of the camera root (rows 0/1/2 = right / forward / up, game basis) and its translation
  claim : head the stereo eyes / projection layer claim for that image (S record u.headQ / headP, OpenXR -> game basis)
  pub   : head published at Present N-1 (the newest pose when that frame began)
All compared as frame-to-frame deltas (the constant offsets by design - owned yaw, recentre, room anchor - drop out)
and as absolute series after removing the mean (rotation) / a fitted yaw-plane rotation + offset (position).

Reported per channel: correlation of the deltas, latency (frames) from the cross-correlation peak over -4..+4,
residual RMS of the deltas after removing that latency, residual spectrum (dominant bin, share of energy in the top
10 % of the band = near fs/2 = frame-to-frame alternation), repeated steps (output delta ~0 while the input moved)
and skipped steps (output delta ~2x input).

python htrace_sine.py trace.csv [--json out.json]
"""
import cmath
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import htrace_analyze as ha  # noqa: E402


def qrot(q, v):
    x, y, z, w = q
    # v' = q v q*
    ux, uy, uz = x, y, z
    cx = uy * v[2] - uz * v[1]
    cy = uz * v[0] - ux * v[2]
    cz = ux * v[1] - uy * v[0]
    tx, ty, tz = 2 * cx, 2 * cy, 2 * cz
    c2x = uy * tz - uz * ty
    c2y = uz * tx - ux * tz
    c2z = ux * ty - uy * tx
    return (v[0] + w * tx + c2x, v[1] + w * ty + c2y, v[2] + w * tz + c2z)


def xr_to_game(v):
    return (v[0], -v[2], v[1])


def angles_from_axes(right, fwd, up):
    yaw = math.degrees(math.atan2(fwd[0], fwd[1]))            # heading: + = right
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, fwd[2]))))
    roll = math.degrees(math.atan2(-right[2], up[2]))
    return yaw, pitch, roll


def xr_angles(q):
    right = xr_to_game(qrot(q, (1.0, 0.0, 0.0)))
    fwd = xr_to_game(qrot(q, (0.0, 0.0, -1.0)))
    up = xr_to_game(qrot(q, (0.0, 1.0, 0.0)))
    return angles_from_axes(right, fwd, up)


def root_angles(v):
    return angles_from_axes(v[0:3], v[3:6], v[6:9])


def unwrap(xs):
    out, off = [], 0.0
    for i, x in enumerate(xs):
        if i:
            d = x + off - out[-1]
            if d > 180:
                off -= 360
            elif d < -180:
                off += 360
        out.append(x + off)
    return out


def diff(xs):
    return [b - a for a, b in zip(xs, xs[1:])]


def corr(a, b):
    n = min(len(a), len(b))
    if n < 3:
        return float("nan")
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    sa = math.sqrt(sum((x - ma) ** 2 for x in a))
    sb = math.sqrt(sum((x - mb) ** 2 for x in b))
    if sa < 1e-12 or sb < 1e-12:
        return float("nan")
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb)


def shifted(out, inp, lag):
    """pairs (out[k], inp[k-lag])"""
    o, i = [], []
    for k in range(len(out)):
        j = k - lag
        if 0 <= j < len(inp):
            o.append(out[k])
            i.append(inp[j])
    return o, i


def rms(xs):
    return math.sqrt(sum(x * x for x in xs) / len(xs)) if xs else float("nan")


def spectrum(xs, fs):
    n = len(xs)
    if n < 8:
        return None
    m = sum(xs) / n
    xs = [x - m for x in xs]
    mags = []
    for k in range(1, n // 2 + 1):
        s = sum(x * cmath.exp(-2j * math.pi * k * t / n) for t, x in enumerate(xs))
        mags.append((k * fs / n, abs(s) ** 2))
    tot = sum(p for _, p in mags) or 1.0
    top = max(mags, key=lambda fp: fp[1])
    hi = sum(p for f, p in mags if f >= 0.9 * fs / 2) / tot
    return {"dominantHz": round(top[0], 2), "dominantShare": round(top[1] / tot, 3), "nearNyquistShare": round(hi, 3)}


def channel(name, out, inp, fs, unit):
    do, di = diff(out), diff(inp)
    res = {"inputDeltaRms": rms(di), "outputDeltaRms": rms(do)}
    best = None
    for lag in range(-4, 5):
        o, i = shifted(do, di, lag)
        c = corr(o, i)
        if not math.isnan(c) and (best is None or c > best[1]):
            best = (lag, c)
    if best is None:
        res["note"] = "no motion"
        return res
    lag = best[0]
    o, i = shifted(do, di, lag)
    r = [a - b for a, b in zip(o, i)]
    o0, i0 = shifted(do, di, 0)
    r0 = [a - b for a, b in zip(o0, i0)]
    moving = [abs(b) > 0.05 * (res["inputDeltaRms"] or 1) for b in i0]
    rep = sum(1 for a, b, mv in zip(o0, i0, moving) if mv and abs(a) < 0.05 * abs(b))
    skip = sum(1 for a, b, mv in zip(o0, i0, moving) if mv and abs(a - 2 * b) < 0.1 * abs(b))
    flips = sum(1 for a, b in zip(r0, r0[1:]) if a * b < 0 and abs(a) > 1e-6 and abs(b) > 1e-6)
    # absolute residual at lag 0, mean removed
    n = min(len(out), len(inp))
    ab = [out[k] - inp[k] for k in range(n)]
    mab = sum(ab) / n if n else 0.0
    res.update({"latencyFrames": lag, "corrAtLatency": round(best[1], 6), "corrAtLag0": round(corr(o0, i0), 6),
                "residualDeltaRmsAtLatency": rms(r), "residualDeltaRmsAtLag0": rms(r0),
                "residualRatioLag0": rms(r0) / res["inputDeltaRms"] if res["inputDeltaRms"] else float("nan"),
                "absResidualRmsLag0": rms([x - mab for x in ab]), "repeatedSteps": rep, "skippedSteps": skip,
                "residualSignFlips": flips, "spectrumLag0": spectrum(r0, fs), "unit": unit})
    return res


def analyse(recs, freq):
    ms = 1000.0 / freq
    s_by_frame = {r["frame"]: r for r in recs if r["k"] == "S"}
    p_by_frame = {r["frame"]: r for r in recs if r["k"] == "P"}
    m_frames = {r["frame"] for r in recs if r["k"] == "M"}
    frames = []
    for c in recs:
        if c["k"] != "C":
            continue
        n = c["frame"]
        s = s_by_frame.get(n)
        p = p_by_frame.get(n - 1)
        if not s or not p:
            continue
        frames.append((c, s, p, (n - 1) in m_frames or n in m_frames))
    if len(frames) < 20:
        return {"error": "too few frames"}
    pq = [f[2]["qpc"] for f in frames]
    fs = 1000.0 / (sum((b - a) * ms for a, b in zip(pq, pq[1:])) / (len(pq) - 1))
    out_ang = [root_angles(f[0]["v"]) for f in frames]
    cl_ang = [xr_angles(tuple(f[1]["v"][0:4])) for f in frames]
    pb_ang = [xr_angles(tuple(f[2]["v"][0:4])) for f in frames]
    res = {"frames": len(frames), "fsHz": round(fs, 2), "runningFrames": sum(1 for f in frames if f[3])}
    for i, nm in enumerate(("yaw", "pitch", "roll")):
        o = unwrap([a[i] for a in out_ang])
        res[nm + " vs claimed"] = channel(nm, o, unwrap([a[i] for a in cl_ang]), fs, "deg")
        res[nm + " vs published"] = channel(nm, o, unwrap([a[i] for a in pb_ang]), fs, "deg")
    # position: camera root translate vs claimed head position x70 in the game basis, yaw-plane rotation fitted
    ot = [f[0]["v"][9:12] for f in frames]
    ct = [tuple(70.0 * x for x in xr_to_game(f[1]["v"][4:7])) for f in frames]
    dox, doy = diff([t[0] for t in ot]), diff([t[1] for t in ot])
    dcx, dcy = diff([t[0] for t in ct]), diff([t[1] for t in ct])
    sc = sum(a * d - b * c for a, b, c, d in zip(dcx, dcy, dox, doy))  # cross
    sd = sum(a * c + b * d for a, b, c, d in zip(dcx, dcy, dox, doy))  # dot
    th = math.atan2(sc, sd)
    cs, sn = math.cos(th), math.sin(th)
    rx = [cs * t[0] - sn * t[1] for t in ct]
    ry = [sn * t[0] + cs * t[1] for t in ct]
    res["posFitYawDeg"] = round(math.degrees(th), 3)
    res["x vs claimed"] = channel("x", [t[0] for t in ot], rx, fs, "units")
    res["y vs claimed"] = channel("y", [t[1] for t in ot], ry, fs, "units")
    res["z vs claimed"] = channel("z", [t[2] for t in ot], [t[2] for t in ct], fs, "units")
    return res


def compact(res):
    lines = [f"frames={res.get('frames')} fs={res.get('fsHz')} Hz running={res.get('runningFrames')}"]
    for k, v in res.items():
        if isinstance(v, dict) and "inputDeltaRms" in v:
            if v.get("note") or v["inputDeltaRms"] < 1e-4:
                continue
            sp = v.get("spectrumLag0") or {}
            lines.append(f"  {k:18s} in={v['inputDeltaRms']:.4f} lat={v['latencyFrames']:+d} r0={v['residualDeltaRmsAtLag0']:.5f} "
                         f"({100 * v['residualRatioLag0']:.1f}% of input) rLat={v['residualDeltaRmsAtLatency']:.5f} abs={v['absResidualRmsLag0']:.5f} "
                         f"c0={v['corrAtLag0']:.4f} rep={v['repeatedSteps']} skip={v['skippedSteps']} flips={v['residualSignFlips']} "
                         f"spec dom={sp.get('dominantHz')}Hz nyq={sp.get('nearNyquistShare')} {v['unit']}")
    return "\n".join(lines)


def main():
    recs, freq = ha.load(sys.argv[1])
    res = analyse(recs, freq)
    print(compact(res))
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as fh:
            json.dump(res, fh, indent=1)


if __name__ == "__main__":
    main()
