"""Session-5 steps for r3_run.py (stereo agent, 15 Nov):

  ipd0diag   vrtest_sanctuary, IPD-0 test eyes, deployed cut list: back-buffer halves with and without the GBuffer clear
             ("stereoclear"), per-pass profile (stereopasscap), column periodicity of the residual, GBuffer-capture mask
             coverage. BMPs reduced to 1/4 PNG previews and deleted.
  popsweep   vrtest_container (shadow pop, user): static HMD poses (vremu) over a yaw sweep at 3 pitches, stereo / mono /
             stereo again, 3 frames per pose (cropburst 1/8) + depth-only pass signatures (stereoshadow) per frame.
             Saves popsweep_<pitch>_<sweep>.npz (median frames uint8 + yaws) and the signature lines in results.json.
  shadowturn vrtest_container: continuous 15 deg/s HMD yaw in stereo and mono with 240 frames of stereoshadow lines.
Analysis: s5_pop_analyze.py.
"""
import json
import os
import re
import time

import numpy as np

import halves_np as H
import r3_run as R
from fo4test import Game, say
from np_png import write_png


def _dump(res, out):
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def _png_q(img, path, f=4):
    write_png(path, img[::f, ::f, ::-1].clip(0, 255).astype(np.uint8))


def _colper(d):
    bad = (d > 1).astype(np.float32)
    col = bad.mean(axis=0)
    out = {}
    for p in (8, 16, 32, 64):
        prof = np.array([col[k::p].mean() for k in range(p)])
        out[p] = round(float(prof.max() / max(prof.mean(), 1e-9)), 2)
    return out


def step_ipd0diag(g, out, res, pad):
    D = res.setdefault("ipd0diag", {})
    for c in ("stereotest 1", "stereofov 45", "stereoipd 0", "stereoclear 0", "stereo 3"):
        R.cmd(g, c)
    D["cuts"] = R.cmd(g, "stereocuts status")
    for tag, cmds in (("noclear", []), ("noclear2", []), ("clear", ["stereoclear 1"])):
        for c in cmds:
            R.cmd(g, c)
        time.sleep(4)
        bb = R.capture(g, out, f"d_{tag}")
        rows = R.passcap(g, tag)
        img = H.read_bmp(bb)
        d = H.diffmap(img, pad)
        r = {"halves": R.halves(bb, pad), "mean": round(float(img.mean()), 1), "colPeriodMaxOverMean": _colper(d[:, 64:-64]),
             "rowsBad": [round(float((d[y:y + d.shape[0] // 8] > 1).mean() * 100), 2) for y in range(0, d.shape[0], d.shape[0] // 8)],
             "passes": rows, "bad": R.summarize_passes(rows)}
        _png_q(img, os.path.join(out, f"d_{tag}_q.png"))
        write_png(os.path.join(out, f"d_{tag}_diff_q.png"), (np.clip(d[::2, ::2] * 16, 0, 255)).astype(np.uint8))
        os.remove(bb)
        if tag == "clear":
            stem = os.path.join(out, "gbmask").replace("\\", "/")
            mark = Game.log_mark()
            R.cmd(g, "stereogbcap " + stem)
            t0 = time.time()
            while "stereogb: " + stem not in g.log_since(mark) and time.time() - t0 < 30:
                time.sleep(0.5)
            r["gbcapLog"] = [ln.split("stereogb: ", 1)[1][:400] for ln in g.log_since(mark).splitlines() if "stereogb:" in ln]
            m0 = stem + "_rt0.bmp"
            if R.bmp_done(m0):
                mk = H.read_bmp(m0)
                r["maskCoverage"] = round(float((mk.max(axis=2) > 0).mean() * 100), 2)
                _png_q(mk, os.path.join(out, "gbmask_rt0_q.png"))
            for fn in os.listdir(out):
                if fn.startswith("gbmask_") and (fn.endswith(".bmp") or fn.endswith(".pgm")):
                    os.remove(os.path.join(out, fn))
        D[tag] = r
        say(f"ipd0diag {tag}: {r['halves']} mean {r['mean']} colper {r['colPeriodMaxOverMean']} mask {r.get('maskCoverage')} "
            f"| passes below 99.9: {len(r['bad'])}")
        for b in r["bad"][:14]:
            say("   " + b[:230])
        _dump(res, out)
    for c in ("stereoclear 0", "stereotest 0"):
        R.cmd(g, c)


def _stripe(d, period=43):
    """strength of a column pattern of the given period in the halves residual (diff > 1), middle 30-60 % rows"""
    h = d.shape[0]
    col = (d[int(h * 0.3):int(h * 0.6)] > 1).mean(axis=0)
    prof = np.array([col[k::period].mean() for k in range(period)])
    return {"bad": round(float(col.mean() * 100), 3), "peakOverMean": round(float(prof.max() / max(prof.mean(), 1e-9)), 2),
            "peakPhase": int(prof.argmax())}


def step_stripes(g, out, res, pad, groups="9,10,3,20,4,44,23,18,19,7,47,49"):
    """IPD-0 halves residual column stripes (period 43 px = render width / 128, mid-distance band, s5a): per-pass BMPs
    of the listed PS groups (no GBuffer clear) -> where the stripes first appear"""
    S = res.setdefault("stripes", {})
    for c in ("stereotest 1", "stereofov 45", "stereoipd 0", "stereoclear 0", "stereo 3"):
        R.cmd(g, c)
    time.sleep(4)
    sub = os.path.join(out, "stripes")
    os.makedirs(sub, exist_ok=True)
    rows = R.passcap(g, "st", bmpdir=sub, groups=groups)
    S["passes"] = rows
    time.sleep(3)
    per = {}
    for fn in sorted(os.listdir(sub)):
        if not fn.endswith(".bmp"):
            continue
        p = os.path.join(sub, fn)
        t0 = time.time()
        while not R.bmp_done(p) and time.time() - t0 < 30:
            time.sleep(0.2)
        img = H.read_bmp(p)
        W = img.shape[1]
        pd = int(round(pad * W / 5504))
        dd = H.diffmap(img, pd)
        per[fn] = {"size": [int(img.shape[1]), int(img.shape[0])], "stripe": _stripe(dd, max(1, int(round(43 * W / 5504)))),
                   "within1": round(float((dd[:, 16:-16] <= 1).mean() * 100), 3)}
        write_png(os.path.join(sub, fn[:-4] + "_diff.png"), np.clip(dd[::2, ::2] * 16, 0, 255).astype(np.uint8))
        os.remove(p)
        say(f"stripes {fn}: {per[fn]}")
    S["perPass"] = per
    _dump(res, out)
    for c in ("stereotest 0",):
        R.cmd(g, c)


SIG_RE = re.compile(r"stereoshadow: f (\d+) st (\d) n (\d+)(.*)$")


def _sig_lines(text):
    return [ln.split("stereoshadow: ", 1)[1] for ln in text.splitlines() if "stereoshadow: f " in ln]


HALF = [2752]  # eye region width of this launch (render width / 2), set by the steps from res["renderWidth"]


def _cropmed(g, out, name, frames=3, step=8):
    raw = os.path.join(out, name + ".raw").replace("\\", "/")
    mark = Game.log_mark()
    g.cmd(f"cropburst {frames} {raw} 0 {HALF[0]} 0 {HALF[0]} 2880 {step}", quiet=True)
    t0 = time.time()
    while "cropburst:" not in g.log_since(mark):
        if time.time() - t0 > 60:
            raise RuntimeError("cropburst timed out")
        time.sleep(0.1)
    fr = R.read_crop(raw)
    os.remove(raw)
    return np.median(fr, axis=0).astype(np.uint8), int(np.abs(np.diff(fr, axis=0)).max()) if len(fr) > 1 else 0


def _load_container(g):
    mark = Game.log_mark()
    R.cmd(g, "console load vrtest_container")
    time.sleep(3)
    if R.wait_ingame(g, 120) is None:
        raise RuntimeError("container load failed")
    time.sleep(10)
    for c in ("console set timescale to 1", "console tgm", "console tcai", "console tai"):
        R.cmd(g, c)
        time.sleep(0.4)
    return R.cam_pos(g)


def _setpos(g, x, y, zfeet, heading):
    for c in (f"console player.setpos x {x:.1f}", f"console player.setpos y {y:.1f}", f"console player.setpos z {zfeet:.1f}",
              f"console player.setangle z {heading}"):
        R.cmd(g, c)
        time.sleep(0.4)
    time.sleep(3)
    return R.cam_pos(g)


def _env_pose():
    """S5_POSE=x,y,zfeet,heading: popsweep / shadowturn pose (default: the save's)"""
    v = os.environ.get("S5_POSE")
    return [float(t) for t in v.split(",")] if v else None


def _half(res):
    if res.get("renderWidth"):
        HALF[0] = int(res["renderWidth"]) // 2


def step_explore(g, out, res, pad):
    """walk into the yellow container: camera positions along the save heading, one 1/8 stereo frame each"""
    E = res.setdefault("explore", {})
    _half(res)
    c0 = _load_container(g)
    E["save"] = c0
    import math
    hd = 105.0
    dx, dy = math.sin(math.radians(hd)), math.cos(math.radians(hd))
    for d in (300, 500, 700, 900, 1100, 1300, 1500):
        x, y = c0[0] + dx * d, c0[1] + dy * d
        cam = _setpos(g, x, y, c0[2] - 120 + 80, hd)
        R.vremu("/pose?dev=hmd&ypr=0,0,0")
        time.sleep(1.5)
        med, _ = _cropmed(g, out, f"ex_{d}", frames=1)
        write_png(os.path.join(out, f"ex_{d}.png"), med[..., ::-1])
        E[str(d)] = {"target": [x, y], "camera": cam}
        say(f"explore {d}: camera {cam}")
        _dump(res, out)


def step_popsweep(g, out, res, pad):
    P = res.setdefault("popsweep", {})
    _half(res)
    for c in ("stereotest 0", "stereoclear 0"):
        R.cmd(g, c)
    P["camera"] = _load_container(g)
    if _env_pose():
        P["camera"] = _setpos(g, *_env_pose())
    yaws = [round(-40 + 2.5 * i, 1) for i in range(33)]
    P["yaws"] = yaws
    for pitch in (0, 15, -20):
        for mode, name in ((3, "S"), (0, "M"), (3, "S2")):
            R.cmd(g, f"stereo {mode}")
            R.cmd(g, "stereocuts force" if mode == 0 else "stereocuts auto")
            key = f"p{pitch}_{name}"
            rec = {"sigs": [], "burstMaxDiff": []}
            meds = []
            for i, yaw in enumerate(yaws):
                R.vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
                time.sleep(3.0 if i == 0 else 1.0)
                if i == 0:
                    rec["tans"] = R.cmd(g, "stereoscissor")
                    rec["status"] = g.cmd("status", quiet=True)[:600]
                mark = Game.log_mark()
                g.cmd("stereoshadow 3", quiet=True)
                med, bmd = _cropmed(g, out, f"ps_{key}_{i:02}")
                time.sleep(0.15)
                rec["sigs"].append(_sig_lines(g.log_since(mark)))
                rec["burstMaxDiff"].append(bmd)
                meds.append(med)
            np.savez_compressed(os.path.join(out, f"popsweep_{key}.npz"), frames=np.stack(meds), yaws=np.array(yaws), pitch=pitch,
                                mode=mode)
            P[key] = rec
            say(f"popsweep {key}: {len(meds)} poses, first sig {rec['sigs'][0][:1]}")
            _dump(res, out)
    R.cmd(g, "stereocuts auto")
    R.cmd(g, "stereo 3")
    R.vremu("/pose?dev=hmd&ypr=0,0,0")


def step_shadowturn(g, out, res, pad):
    T = res.setdefault("shadowturn", {})
    if "popsweep" not in res:
        T["camera"] = _load_container(g)
        if _env_pose():
            T["camera"] = _setpos(g, *_env_pose())
    for mode in (3, 0):
        R.cmd(g, f"stereo {mode}")
        R.cmd(g, "stereocuts force" if mode == 0 else "stereocuts auto")
        for d in (1, -1):
            R.vremu(f"/pose?dev=hmd&ypr={-30 * d},0,0")
            time.sleep(2.5)
            mark = Game.log_mark()
            R.vremu(f"/rate?dev=hmd&wyaw={15 * d}&ms=4000&timeout=4000")
            g.cmd("stereoshadow 240", quiet=True)
            time.sleep(4.5)
            R.vremu("/rate?dev=hmd&stop=1")
            T[f"m{mode}_d{d:+d}"] = _sig_lines(g.log_since(mark))
            say(f"shadowturn mode {mode} dir {d:+d}: {len(T[f'm{mode}_d{d:+d}'])} frames")
            _dump(res, out)
    R.cmd(g, "stereocuts auto")
    R.cmd(g, "stereo 3")
    R.vremu("/pose?dev=hmd&ypr=0,0,0")


STEPS = {"explore": step_explore, "ipd0diag": step_ipd0diag, "stripes": step_stripes, "popsweep": step_popsweep, "shadowturn": step_shadowturn}
