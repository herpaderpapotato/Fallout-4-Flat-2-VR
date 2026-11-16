"""Analyse an htrace CSV (pipe: htrace arm <frames>; htrace dump <path>; or [Debug] bPoseTrace=1 files).

Event records (every publish/read/write; qpc, thread, frame = presents counted so far, seq = head publish number):
  P  head::PublishXr at Present (xr head quat v0-3, pos v4-6; v8 = ms in xrWaitFrame+BeginFrame)
  L  xrLocateViews result (eye 0/1 poses)
  S  projection-layer submit at Present N: head the stereo eyes were built from (u.headQ v0-3, u.headP v4-6),
     eye 0 (v7-13), seq = publish of the XR frame being ended
  E  stereo eye-matrix build (SetCameraData): used.headQ v0-3, game V rotation v4-12, v13 = 1 world / 2 1st person
  C  at Present N: camera root world (rotation 3x3 v0-8, translate v9-11) = what the presented image used
  R  FirstPersonState::GetRotation: v0-3 result quat (w,x,y,z), v4-7 game quat, v8 ownYaw, seq = head publish read
  T  GetTranslation: result v0-2, raw v3-5, v6 in Update, v7-9 head offset added (units), v10 path, v11/12 caller rva
  U  FirstPersonState::Update end: camera root world (same layout as C)
  M  roomscale primary movement update (only while the game updates): v0 dt, v1 heading, v2 ownYaw, v4-6 capsule

Per presented frame N:
  camSeq  = head publish of the R whose U matches C(N) exactly (the camera the image was rendered with)
  eyesSeq = head publish whose quat equals S(N).u.headQ (what the eye matrices and the layer claim)
  lag     = eyesSeq - camSeq (0 in a correct pipeline)
  err     = rotation angle head[camSeq] vs head[eyesSeq] (deg), signed yaw part, position error (mm)

python htrace_analyze.py trace.csv [--rows N] [--json out.json]
"""
import csv
import json
import math
import sys
from collections import Counter, defaultdict


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(a):
    return (-a[0], -a[1], -a[2], a[3])


def qangle_deg(a, b):
    d = qmul(a, qconj(b))
    n = math.sqrt(sum(x * x for x in d)) or 1.0
    return 2.0 * math.degrees(math.acos(min(1.0, abs(d[3]) / n)))


def xr_yaw_deg(q):
    """Heading of an OpenXR orientation (forward = -Z), degrees, + = turned left (about +Y)."""
    x, y, z, w = q
    fx = -(2 * (x * z + w * y))
    fz = -(1 - 2 * (x * x + y * y))
    return math.degrees(math.atan2(-fx, -fz))


def wrap(d):
    while d > 180:
        d -= 360
    while d < -180:
        d += 360
    return d


def pct(xs, p):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))]


def stats(xs):
    if not xs:
        return "n=0"
    return (f"n={len(xs)} mean={sum(xs) / len(xs):.5f} p50={pct(xs, 50):.5f} p95={pct(xs, 95):.5f} "
            f"max={max(xs):.5f} min={min(xs):.5f}")


def load(path):
    freq = 1e7
    recs = []
    with open(path, newline="") as fh:
        first = fh.readline()
        if first.startswith("# qpcfreq"):
            try:
                f = float(first.split()[2])
                if 1e5 < f < 1e11:
                    freq = f
            except ValueError:
                pass
        for row in csv.DictReader(fh):
            try:
                recs.append({"k": row["kind"], "tid": int(row["tid"]), "qpc": int(row["qpc"]), "frame": int(row["frame"]),
                             "seq": int(row["seq"]), "xrt": int(row["xrTime"]),
                             "v": [float(row[f"v{i}"]) for i in range(16)]})
            except (ValueError, KeyError, TypeError):
                continue  # torn row (continuous mode buffer flip)
    recs.sort(key=lambda r: r["qpc"])
    return recs, freq


def analyse(recs, freq):
    out = {}
    ms = 1000.0 / freq
    head_q, head_p = {}, {}
    p_list = []  # (qpc, seq, quat)
    for r in recs:
        if r["k"] == "P":
            head_q[r["seq"]] = tuple(r["v"][0:4])
            head_p[r["seq"]] = tuple(r["v"][4:7])
            p_list.append((r["qpc"], r["seq"], tuple(r["v"][0:4])))

    def seq_of_quat(q, before_qpc):
        """newest publish at/before before_qpc whose quat equals q exactly"""
        for qpc, seq, pq in reversed(p_list):
            if qpc <= before_qpc and pq == q:
                return seq
        return None

    # threads per kind
    out["threads"] = {k: sorted({r["tid"] for r in recs if r["k"] == k}) for k in sorted({r["k"] for r in recs})}
    out["counts"] = dict(Counter(r["k"] for r in recs))

    # read-vs-newest lag at the moment of each read
    newest = None
    lag_at_read = defaultdict(Counter)
    for r in recs:
        if r["k"] == "P":
            newest = r["seq"]
        elif r["k"] in "RTMEU" and newest is not None:
            if r["k"] == "E":
                s = seq_of_quat(tuple(r["v"][0:4]), r["qpc"])
                if s is not None:
                    lag_at_read["E"][newest - s] += 1
            else:
                lag_at_read[r["k"]][newest - r["seq"]] += 1
    out["lagAtRead(newest-seqUsed)"] = {k: dict(sorted(v.items())) for k, v in lag_at_read.items()}

    # T callers
    callers = Counter()
    for r in recs:
        if r["k"] == "T":
            rva = int(r["v"][11]) + (int(r["v"][12]) << 24)
            callers[(f"{rva:X}", "upd" if r["v"][6] else "out", r["tid"])] += 1
    out["T_callers"] = {f"{a} {b} tid{c}": n for (a, b, c), n in callers.most_common(12)}

    # U -> R (the R on the same thread immediately before the U)
    u_key = {}
    lastR_by_tid = {}
    for r in recs:
        if r["k"] == "R":
            lastR_by_tid[r["tid"]] = r
        elif r["k"] == "U":
            key = tuple(r["v"][0:12])
            rr = lastR_by_tid.get(r["tid"])
            u_key[key] = (r, rr)

    # timing of R relative to the Present hook (P = after xrWaitFrame) of its frame
    p_by_frame = {}
    s_by_frame = {}
    for r in recs:
        if r["k"] == "P":
            p_by_frame[r["frame"]] = r
        elif r["k"] == "S":
            s_by_frame[r["frame"]] = r
    r_before_p, r_after_s = [], []
    r_vs_p = Counter()
    for r in recs:
        if r["k"] == "R" and r["frame"] in p_by_frame:
            dp = (r["qpc"] - p_by_frame[r["frame"]]["qpc"]) * ms
            r_vs_p["before P" if dp < 0 else "after P"] += 1
            r_before_p.append(dp)
            if r["frame"] in s_by_frame:
                r_after_s.append((r["qpc"] - s_by_frame[r["frame"]]["qpc"]) * ms)
    out["R_minus_P_ms (same frame; <0 = camera read before the new pose was published)"] = stats(r_before_p)
    out["R_minus_S_ms"] = stats(r_after_s)
    out["R_vs_P"] = dict(r_vs_p)
    waits = [r["v"][8] for r in recs if r["k"] == "P"]
    out["xrWaitFrame+Begin_ms"] = stats(waits)
    pq = [r["qpc"] for r in recs if r["k"] == "P"]
    out["present_interval_ms"] = stats([(b - a) * ms for a, b in zip(pq, pq[1:])])

    # running flag per frame interval: M record seen with that frame number
    m_frames = {r["frame"] for r in recs if r["k"] == "M"}

    rows = []
    for r in recs:
        if r["k"] != "C":
            continue
        n = r["frame"]
        row = {"frame": n, "running": (n - 1) in m_frames or n in m_frames, "camT": r["v"][9:12]}
        hit = u_key.get(tuple(r["v"][0:12]))
        if hit and hit[1] is not None:
            row["camSeq"] = hit[1]["seq"]
            row["camFrame"] = hit[0]["frame"]
        s = s_by_frame.get(n)
        if s:
            row["frameSeq"] = s["seq"]
            row["eyesSeq"] = seq_of_quat(tuple(s["v"][0:4]), s["qpc"])
        if row.get("camSeq") in head_q and row.get("eyesSeq") in head_q:
            a, b = head_q[row["camSeq"]], head_q[row["eyesSeq"]]
            row["lag"] = row["eyesSeq"] - row["camSeq"]
            row["errDeg"] = qangle_deg(a, b)
            row["yawErrDeg"] = wrap(xr_yaw_deg(a) - xr_yaw_deg(b))
            pa, pb = head_p[row["camSeq"]], head_p[row["eyesSeq"]]
            row["posErrMm"] = 1000.0 * math.sqrt(sum((x - y) ** 2 for x, y in zip(pa, pb)))
        rows.append(row)

    def summarize(sel, name):
        e = [x["errDeg"] for x in sel if "errDeg" in x]
        ye = [x["yawErrDeg"] for x in sel if "yawErrDeg" in x]
        pe = [x["posErrMm"] for x in sel if "posErrMm" in x]
        lags = Counter(x.get("lag", "n/a") for x in sel)
        # camera seq step frame to frame (1 = every frame a fresh pose; 0 = repeated; 2 = skipped)
        cs = [x["camSeq"] for x in sel if "camSeq" in x]
        steps = Counter(b - a for a, b in zip(cs, cs[1:]))
        es = [x["eyesSeq"] for x in sel if x.get("eyesSeq") is not None]
        esteps = Counter(b - a for a, b in zip(es, es[1:]))
        # sign alternation of the signed yaw residual
        alt = sum(1 for a, b in zip(ye, ye[1:]) if a * b < 0 and abs(a) > 1e-4 and abs(b) > 1e-4)
        out[name] = {"frames": len(sel), "lag(eyesSeq-camSeq)": {str(k): v for k, v in sorted(lags.items(), key=lambda kv: str(kv[0]))},
                     "camSeqStep": dict(sorted(steps.items())), "eyesSeqStep": dict(sorted(esteps.items())),
                     "errDeg": stats(e), "yawErrDeg": stats(ye), "posErrMm": stats(pe), "yawErrSignFlips": alt,
                     "unmatchedC": sum(1 for x in sel if "camSeq" not in x)}

    summarize(rows, "all")
    summarize([x for x in rows if x["running"]], "running")
    summarize([x for x in rows if not x["running"]], "paused")

    # position: camera root step vs 70 x head step (rotation-invariant), with the capsule step of the frame
    m_loc = {}
    for r in recs:
        if r["k"] == "M":
            m_loc[r["frame"]] = r["v"][4:7]
    upm = 70.0
    dcam, dhead, resid, dcap = [], [], [], []
    for a, b in zip(rows, rows[1:]):
        if b["frame"] != a["frame"] + 1 or a.get("camSeq") not in head_p or b.get("camSeq") not in head_p:
            continue
        dc = math.dist(a["camT"], b["camT"])
        dh = upm * math.dist(head_p[a["camSeq"]], head_p[b["camSeq"]])
        dcam.append(dc)
        dhead.append(dh)
        resid.append(dc - dh)
        cf = b.get("camFrame")
        la, lb = (m_loc.get(cf - 1), m_loc.get(cf)) if cf else (None, None)
        if la and lb:
            dcap.append(math.dist(la, lb))
    out["camRootStepUnits"] = stats(dcam)
    out["headStepUnits(x70)"] = stats(dhead)
    out["camStepMinusHeadStepUnits"] = stats(resid)
    out["capsuleStepUnits"] = stats(dcap)
    cz = [x["camT"][2] for x in rows]
    out["camZ2ndDiffUnits"] = stats([abs(c - 2 * b + a) for a, b, c in zip(cz, cz[1:], cz[2:])])
    # yaw owned / heading series from R and M
    own = [r["v"][8] for r in recs if r["k"] == "R"]
    out["ownYawStepDeg"] = stats([abs(math.degrees(b - a)) for a, b in zip(own, own[1:])])
    hd = [r["v"][1] for r in recs if r["k"] == "M"]
    out["bodyHeadingStepDeg"] = stats([abs(math.degrees(b - a)) for a, b in zip(hd, hd[1:])])

    # input motion per publish (scale)
    seqs = sorted(head_q)
    out["publishedStepDeg"] = stats([qangle_deg(head_q[a], head_q[b]) for a, b in zip(seqs, seqs[1:]) if b == a + 1])
    # rendered yaw series (from camSeq) vs claimed yaw series (eyesSeq): first and second differences
    ry = [xr_yaw_deg(head_q[x["camSeq"]]) for x in rows if x.get("camSeq") in head_q]
    cy = [xr_yaw_deg(head_q[x["eyesSeq"]]) for x in rows if x.get("eyesSeq") in head_q]
    d1 = [wrap(b - a) for a, b in zip(ry, ry[1:])]
    out["renderedYawStepDeg"] = stats(d1)
    out["renderedYaw2ndDiffDeg"] = stats([abs(b - a) for a, b in zip(d1, d1[1:])])
    d1c = [wrap(b - a) for a, b in zip(cy, cy[1:])]
    out["claimedYawStepDeg"] = stats(d1c)
    out["claimedYaw2ndDiffDeg"] = stats([abs(b - a) for a, b in zip(d1c, d1c[1:])])
    return out, rows


def main():
    path = sys.argv[1]
    recs, freq = load(path)
    out, rows = analyse(recs, freq)
    if "--rows" in sys.argv:
        for x in rows[:int(sys.argv[sys.argv.index("--rows") + 1])]:
            print(x)
    print(json.dumps(out, indent=1))
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as fh:
            json.dump({"summary": out, "rows": rows}, fh, indent=1)


if __name__ == "__main__":
    main()
