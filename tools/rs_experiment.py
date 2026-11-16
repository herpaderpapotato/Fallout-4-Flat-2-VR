"""Roomscale (B4) + body yaw (B3) experiment, analysis/vr-roomscale.md section 6.

    python -I rs_experiment.py [--vremu] [--attach] [--only 0,1,2,3,4,5,6,7,hmd] [--cell QASmoke]

One launch (or --attach). Drives the scripted head offset (posepos / pose) and the virtual pad, reads
the one-line `rs` readout (+ `rs hist`), prints the numbers per step and writes them to
work/rs/<ts>/rs.json. --vremu: SteamVR+vremu runtime; the 'hmd' step then moves the emulated HMD
(/pose?dev=hmd) for the real tracking path.
"""
import argparse
import json
import math
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fo4test import Game, parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def vec(s):
    m = re.match(r"\(([^)]*)\)", s or "")
    return tuple(float(x) for x in m.group(1).split(",")) if m else None


def rs_parse(line):
    d = parse_kv(line)
    for k in list(d):
        if d[k].startswith("("):
            d[k] = vec(d[k])
        else:
            try:
                d[k] = float(d[k]) if re.match(r"^-?[\d.]+(e-?\d+)?$", d[k]) else d[k]
            except ValueError:
                pass
    # values like loc=(1.00,2.00,3.00) are split by parse_kv at the first space-free token: fine
    return d


class Exp:
    def __init__(self, g):
        self.g = g
        self.out = {}

    def cmd(self, c):
        return self.g.cmd(c, quiet=True)

    def rs(self):
        return rs_parse(self.cmd("rs"))

    def sample(self, secs, every=0.02):
        """Poll rs for secs -> list of (t, dict)."""
        t0 = time.time()
        out = []
        while time.time() - t0 < secs:
            out.append((round(time.time() - t0, 3), self.rs()))
            time.sleep(every)
        return out

    def settle(self, timeout=4.0):
        """Wait until loc stops changing (5 consecutive samples within 0.05 units) -> last rs."""
        t0 = time.time()
        last, still = None, 0
        while time.time() - t0 < timeout:
            r = self.rs()
            if last and math.dist(r["loc"][:2], last["loc"][:2]) < 0.05:
                still += 1
                if still >= 5:
                    return r
            else:
                still = 0
            last = r
            time.sleep(0.05)
        return last


def cam_heading(e):
    """Heading (rad, + = right) of the camera root's forward = worldRot row 1 (engine convention)."""
    m = re.search(r"worldRot=\[([^\]]*)\]", e.cmd("cam"))
    v = [float(x) for x in m.group(1).replace("|", " ").split()]
    return math.atan2(v[3], v[4])


def wrapd(d):
    return (d + 180.0) % 360.0 - 180.0


def hdist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def fwd(heading):
    return (math.sin(heading), math.cos(heading))


def along(d, heading):
    f = fwd(heading)
    return d[0] * f[0] + d[1] * f[1], d[0] * f[1] - d[1] * f[0]  # (forward, right)


def step_move(e, name, posepos_from, posepos_to, secs=1.5):
    """Head jump posepos_from -> posepos_to (cm); returns the numbers for the step."""
    e.cmd("posepos %g %g %g" % posepos_from)
    r0 = e.settle()
    e.cmd("posepos %g %g %g" % posepos_to)
    s = e.sample(secs)
    r1 = e.settle()
    hist = e.cmd("rs hist")
    d = (r1["loc"][0] - r0["loc"][0], r1["loc"][1] - r0["loc"][1])
    fw, rt = along(d, r0["heading"])
    cam_d = (r1["camPos"][0] - r0["camPos"][0], r1["camPos"][1] - r0["camPos"][1])
    # after the first sample where the camera jumped, how far did the camera wander (vection)?
    cams = [x[1]["camPos"] for x in s if x[1].get("camPos")]
    cam_final = r1["camPos"]
    cam_wander = max((hdist(c, cam_final) for c in cams[3:]), default=0.0)
    frames_to = None
    tgt = math.hypot(posepos_to[0] - posepos_from[0], posepos_to[1] - posepos_from[1]) * 0.7
    for t, x in s:
        if hdist(x["loc"], r0["loc"]) >= 0.9 * tgt:
            frames_to = (x["frame"] - s[0][1]["frame"], t)
            break
    springs = [x[1].get("spring") for x in s if x[1].get("spring")]
    spring_max = max((math.hypot(sp[0], sp[1]) for sp in springs), default=0.0)
    res = dict(step=name, loc_moved=round(math.hypot(*d), 2), along_forward=round(fw, 2), along_right=round(rt, 2),
               expected=round(tgt, 2), cam_moved=round(math.hypot(*cam_d), 2), cam_wander_after_jump=round(cam_wander, 2),
               e_final_cm=r1["eCm"], residual_final_cm=r1.get("residualCm"), frames_to_90pct=frames_to,
               spring_max=round(spring_max, 3), springPatched=r1.get("springPatched"), numColl=r1.get("numColl"),
               ctrlState=r1.get("ctrlState"), gate=r1.get("gate"), calls=r1.get("calls"))
    e.out.setdefault("traces", {})[name] = {"samples": [(t, x.get("frame"), x.get("loc"), x.get("camPos"), x.get("eCm"),
                                                         x.get("inj"), x.get("dLoc"), x.get("spring")) for t, x in s],
                                            "hist": hist}
    return res, r0, r1


def camhist(e, n=900):
    """'rs camhist n' -> list of dicts per camera update (see Roomscale.cpp NoteCameraFrame)."""
    r = e.cmd("rs camhist %d" % n)
    out = []
    for part in r.split(" | ")[1:]:
        v = part.split()
        if len(v) < 23:
            continue
        f = [float(x) for x in v]
        out.append(dict(t=f[0], mv=int(f[1]), root=f[2:5], loc=f[5:8], raw=f[8:11], add=f[11:13], d=f[13:15], hmd=f[15:17],
                        anchor=f[17:19], yaw=f[19], heading=f[20], gin=int(f[21]), gout=int(f[22])))
    return out


def room_to_world(yaw, x, y):
    """Engine heading convention: room (x right, y fwd) at yaw -> world."""
    c, s = math.cos(yaw), math.sin(yaw)
    return x * c + y * s, -x * s + y * c


def overshoot_analyse(frames, upm=70.0, t_from=None):
    """Camera vs room-stable target over a camera trace. frames[0] = reference (before the move).
    err = root - (root0 + R(yaw)(hmd - hmd0) upm) (horizontal), split into
      book = (loc - loc0) - R(yaw)(anchor - anchor0) upm   (capsule vs anchor bookkeeping)
      fb   = err - book                                      (camera composition / GetTranslation feedback)."""
    f0 = frames[0]
    rows = []
    for f in frames:
        yaw = f["yaw"]
        hx, hy = room_to_world(yaw, (f["hmd"][0] - f0["hmd"][0]) * upm, (f["hmd"][1] - f0["hmd"][1]) * upm)
        ax, ay = room_to_world(yaw, (f["anchor"][0] - f0["anchor"][0]) * upm, (f["anchor"][1] - f0["anchor"][1]) * upm)
        ex, ey = f["root"][0] - f0["root"][0] - hx, f["root"][1] - f0["root"][1] - hy
        bx, by = f["loc"][0] - f0["loc"][0] - ax, f["loc"][1] - f0["loc"][1] - ay
        rows.append(dict(t=f["t"] - f0["t"], err=math.hypot(ex, ey), errv=(ex, ey), book=math.hypot(bx, by), fb=math.hypot(ex - bx, ey - by),
                         cam=math.hypot(f["root"][0] - f0["root"][0], f["root"][1] - f0["root"][1]), exp=math.hypot(hx, hy),
                         loc=math.hypot(f["loc"][0] - f0["loc"][0], f["loc"][1] - f0["loc"][1]),
                         gin=f["gin"], gout=f["gout"]))
    return rows


def overshoot_step(e, name, move, back, secs=2.5, settle_s=1.5, use_vremu=True):
    """move()/back(): start/undo the head displacement. Returns the summary + rows."""
    time.sleep(settle_s)
    t_mark = camhist(e, 1)[-1]["t"]
    move()
    time.sleep(0.6)
    e.out.setdefault("overshoot_hist", {})[name] = e.cmd("rs hist")
    time.sleep(secs - 0.6)
    fr = [f for f in camhist(e) if f["t"] >= t_mark]
    rows = overshoot_analyse(fr)
    tail = [r for r in rows if r["t"] > rows[-1]["t"] - 500]  # last 0.5 s = steady state
    res = dict(step=name, frames=len(rows), cam_moved=round(rows[-1]["cam"], 2), expected=round(rows[-1]["exp"], 2),
               loc_moved=round(rows[-1]["loc"], 2), err_steady=round(max(r["err"] for r in tail), 3),
               err_max=round(max(r["err"] for r in rows), 2), book_steady=round(max(r["book"] for r in tail), 3),
               fb_steady=round(max(r["fb"] for r in tail), 3), fb_max=round(max(r["fb"] for r in rows), 2),
               book_max=round(max(r["book"] for r in rows), 2), getpos_in=max(r["gin"] for r in rows),
               getpos_out=max(r["gout"] for r in rows))
    e.out.setdefault("overshoot_raw", {})[name] = fr[:120]
    e.out.setdefault("overshoot", {})[name] = [(round(r["t"], 1), round(r["err"], 3), round(r["book"], 3), round(r["fb"], 3),
                                                round(r["cam"], 2), round(r["exp"], 2), round(r["loc"], 2)) for r in rows]
    back()
    time.sleep(secs)
    return res


def overshoot_suite(e, use_vremu, results_cb, sizes=(5, 20, 50), tfix=None):
    """Steps of 5/20/50 cm forward + 20 cm right, instant and slow (1 s), via vremu HMD (or posepos)."""
    out = []
    if tfix is not None:
        e.cmd("rs cfg tfix %d" % tfix)
    for cm in sizes:
        for ms in (0, 1000):
            for axis in ("fwd", "right") if cm == 20 else ("fwd",):
                dx, dy = (cm / 100.0, 0.0) if axis == "right" else (0.0, cm / 100.0)
                if use_vremu:
                    # vremu: +x right, -z forward (OpenXR); relative tween
                    mv = lambda dx=dx, dy=dy, ms=ms: vremu("/pose?dev=hmd&rel=1&x=%g&z=%g&ms=%d" % (dx, -dy, ms))
                    bk = lambda dx=dx, dy=dy, ms=ms: vremu("/pose?dev=hmd&rel=1&x=%g&z=%g&ms=%d" % (-dx, dy, ms))
                else:
                    mv = lambda dx=dx, dy=dy: e.cmd("posepos %g %g 0" % (dx * 100, dy * 100))
                    bk = lambda: e.cmd("posepos 0 0 0")
                name = "os %dcm %s %s%s" % (cm, axis, "instant" if ms == 0 else "slow %dms" % ms,
                                            "" if tfix is None else " tfix%d" % tfix)
                r = overshoot_step(e, name, mv, bk)
                out.append(r)
                results_cb(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vremu", action="store_true")
    ap.add_argument("--attach", action="store_true")
    ap.add_argument("--cell", default="QASmoke")
    ap.add_argument("--only", default="0,1,2,3,6,7,4,5,hmd")
    ap.add_argument("--os-sizes", default="5,20,50", help="overshoot step sizes (cm)")
    a = ap.parse_args()
    steps = [s.strip() for s in a.only.split(",")]
    outdir = os.path.join(ROOT, "work", "rs", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    results = []
    with Game(runtime_json=STEAMVR_JSON if a.vremu else None, no_xr=not a.vremu, attach=a.attach, quiet=True) as g:
        e = Exp(g)
        if g.ready().get("ingame") != "1" or g.ready().get("cell") != a.cell:
            g.coc(a.cell)
            time.sleep(3)
        say("status: " + g.cmd("status", quiet=True)[:200])
        e.cmd("track on")
        if a.vremu:
            e.cmd("mode mono")
        e.cmd("pose 0 0 0")
        e.cmd("posepos 0 0 0")
        time.sleep(1.0)
        say("rs: " + e.cmd("rs")[:600])

        def rec(r):
            results.append(r)
            say(json.dumps(r))

        try:
            if "0" in steps:
                s = e.sample(1.0)
                f0, f1 = s[0][1], s[-1][1]
                c0, c1 = f0["calls"], f1["calls"]
                rec(dict(step="0 idle", frames=f1["frame"] - f0["frame"], calls_first=c0, calls_last=c1,
                         loc_drift=round(hdist(f0["loc"], f1["loc"]), 3), residual=f1.get("residualCm"), gate=f1["gate"],
                         inputFlags=f1.get("inputFlags"), controlsDriven=f1.get("controlsDriven"), sitSleep=f1.get("sitSleep"),
                         camState=f1.get("camState"), ctrlState=f1.get("ctrlState"), radius=f1.get("radius"),
                         heading=f1.get("heading"), baseYaw=f1.get("baseYaw"), camYaw=f1.get("camYaw")))
            if "6" in steps:
                # spring kept (game's own) vs disabled, same 50 cm forward step each
                e.cmd("rs cfg spring 1")
                time.sleep(0.3)
                r, _, _ = step_move(e, "6a spring kept: posepos 0->50 fwd", (0, 0, 0), (0, 50, 0))
                rec(r)
                e.cmd("rs cfg spring 0")
                time.sleep(0.3)
                r, _, _ = step_move(e, "6b spring off: posepos 50->0", (0, 50, 0), (0, 0, 0))
                rec(r)
            if "1" in steps:
                r, r0, r1 = step_move(e, "1 posepos 0->50 fwd", (0, 0, 0), (0, 50, 0))
                rec(r)
                r, r0, r1 = step_move(e, "1b posepos 0->50 right", (0, 0, 0), (50, 0, 0))
                rec(r)
                e.cmd("posepos 0 0 0")
                e.settle()
            if "2" in steps:
                e.cmd("posepos 0 0 0")
                r0 = e.settle()
                for i in range(30):
                    e.cmd("posepos 0 20 0")
                    time.sleep(0.15)
                    e.cmd("posepos 0 -20 0")
                    time.sleep(0.15)
                e.cmd("posepos 0 0 0")
                r1 = e.settle(6)
                rec(dict(step="2 +-20cm x30 then 0", drift=round(hdist(r0["loc"], r1["loc"]), 3), e_final_cm=r1["eCm"],
                         anchor0=r0["anchor"], anchor1=r1["anchor"], resets=r1["resets"]))
            if "3" in steps:
                e.cmd("posepos 0 0 0")
                r0 = e.settle()
                e.cmd("posepos 0 8 0")
                time.sleep(1.0)
                r1 = e.rs()
                rec(dict(step="3 posepos 8cm (deadzone)", loc_moved=round(hdist(r0["loc"], r1["loc"]), 3),
                         cam_moved=round(hdist(r0["camPos"], r1["camPos"]), 2), residual_cm=r1.get("residualCm"),
                         moving=r1.get("moving")))
                e.cmd("posepos 0 0 0")
                e.settle()
            if "7" in steps:
                e.cmd("posepos 0 0 0")
                e.cmd("pose 0 0 0")
                r0 = e.settle()
                time.sleep(0.5)
                r0 = e.rs()
                c0 = cam_heading(e)
                e.cmd("pose 30 0 0")
                time.sleep(1.0)
                r1 = e.rs()
                c1 = cam_heading(e)
                e.cmd("pose 60 0 0")
                s = e.sample(1.0)
                r2 = e.rs()
                c2 = cam_heading(e)
                cam_yaws = [x[1]["camYaw"] for x in s]
                rec(dict(step="7 B3 head yaw 30 then 60",
                         yaw30_dheading=round(r1["heading"] - r0["heading"], 4), yaw30_dcamYaw_deg=round(math.degrees(r1["camYaw"] - r0["camYaw"]), 2),
                         yaw60_dheading=round(r2["heading"] - r0["heading"], 4), yaw60_dcamYaw_deg=round(math.degrees(r2["camYaw"] - r0["camYaw"]), 2),
                         camYaw_max_dev_deg=round(max(abs(math.degrees(c - r0["camYaw"]) - 60) for c in cam_yaws[3:]), 2),
                         roomYaw_change=round(r2["roomYaw"] - r0["roomYaw"], 4), bodyRel=r2["bodyRel"],
                         camRoot_yaw30_deg=round(wrapd(math.degrees(c1 - c0)), 2), camRoot_yaw60_deg=round(wrapd(math.degrees(c2 - c0)), 2),
                         samples_heading=[round(x[1]["heading"], 3) for x in s[::5]]))
                # stick turn: room follows the stick, body-relative yaw unchanged
                r3 = e.rs()
                e.cmd("pad rx=0.6 ms=500")
                time.sleep(1.2)
                r4 = e.rs()
                rec(dict(step="7b pad turn", dheading=round(r4["heading"] - r3["heading"], 4),
                         droomYaw=round(r4["roomYaw"] - r3["roomYaw"], 4), dbodyRel=round(r4["bodyRel"] - r3["bodyRel"], 4),
                         dcamYaw=round(r4["camYaw"] - r3["camYaw"], 4)))
                e.cmd("pose 0 0 0")
                time.sleep(1.0)
                r5 = e.rs()
                rec(dict(step="7c head back to 0", bodyRel=r5["bodyRel"], dheading=round(r5["heading"] - r4["heading"], 4)))
            if "4" in steps:
                e.cmd("posepos 0 0 0")
                e.settle()
                # walk forward until blocked
                t0 = time.time()
                blocked = False
                prev = e.rs()
                stuck = 0
                while time.time() - t0 < 20:
                    e.cmd("pad ly=1 ms=400")
                    time.sleep(0.2)
                    r = e.rs()
                    v = hdist(r["loc"], prev["loc"])
                    prev = r
                    if v < 0.5 and time.time() - t0 > 1.0:
                        stuck += 1
                        if stuck >= 3:
                            blocked = True
                            break
                    else:
                        stuck = 0
                time.sleep(0.6)
                r0 = e.settle()
                e.cmd("posepos 0 50 0")
                s = e.sample(1.5)
                r1 = e.settle()
                d = (r1["loc"][0] - r0["loc"][0], r1["loc"][1] - r0["loc"][1])
                fw, rt = along(d, r0["heading"])
                rec(dict(step="4 wall + posepos 50 fwd", blocked_by_stick=blocked, walk_s=round(time.time() - t0, 1),
                         along_forward=round(fw, 2), along_right=round(rt, 2), eCm=r1["eCm"], residualCm=r1["residualCm"],
                         eta=r1["eta"], numColl_max=max(x[1]["numColl"] for x in s), gate=r1["gate"],
                         cam_minus_loc=round(hdist(r1["camPos"], r1["loc"]), 2), anchor0=r0["anchor"], anchor1=r1["anchor"]))
                e.out["wall_hist"] = e.cmd("rs hist")
                if "5" in steps:
                    e.cmd("console tcl")
                    time.sleep(0.5)
                    r2 = e.settle()
                    e.cmd("posepos 0 100 0")
                    e.sample(1.5)
                    r3 = e.settle()
                    d = (r3["loc"][0] - r2["loc"][0], r3["loc"][1] - r2["loc"][1])
                    fw, rt = along(d, r2["heading"])
                    rec(dict(step="5 tcl + posepos 50->100", tcl=r3["tcl"], along_forward=round(fw, 2), along_right=round(rt, 2),
                             eCm=r3["eCm"]))
                    e.cmd("console tcl")
                    time.sleep(0.5)
                e.cmd("posepos 0 0 0")
                g.coc(a.cell)
                time.sleep(3)
                r = e.rs()
                rec(dict(step="reset after coc", lastReset=r["lastReset"], resets=r["resets"], eCm=r["eCm"]))
            if any(x.startswith("os") for x in steps):
                # overshoot regression: camera vs room-stable target (start + HMD displacement x 70)
                if a.vremu:
                    e.cmd("pose off")
                    time.sleep(0.5)
                    vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0")
                    time.sleep(1.0)
                    e.cmd("recenter")
                    time.sleep(1.0)
                else:
                    e.cmd("posepos 0 0 0")
                for x in steps:
                    if x.startswith("os"):
                        # os = current config; os0/os2 = feedback mode tfix 0/2; osX = old attribution (attr 0)
                        tf = int(x[2:]) if x[2:].isdigit() else None
                        e.cmd("rs cfg attr %d" % (0 if x == "osX" else 1))
                        r = overshoot_suite(e, a.vremu, lambda r, x=x: rec(dict(r, step=r["step"] + (" attr0" if x == "osX" else ""))),
                                            sizes=[int(v) for v in a.os_sizes.split(",")], tfix=tf)
                e.cmd("rs cfg tfix 1")
                e.cmd("rs cfg attr 1")
            if "hmd" in steps and a.vremu:
                e.cmd("pose off")
                time.sleep(0.5)
                vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0")
                time.sleep(1.0)
                e.cmd("recenter")
                time.sleep(1.0)
                r0 = e.settle()
                say("hmd base rs: " + json.dumps({k: r0.get(k) for k in ("hmd", "anchor", "lastReset", "gate", "camYaw")}))
                vremu("/pose?dev=hmd&rel=1&z=-0.5&ms=500")  # 50 cm forward over 0.5 s
                s = e.sample(1.5)
                r1 = e.settle()
                d = (r1["loc"][0] - r0["loc"][0], r1["loc"][1] - r0["loc"][1])
                fw, rt = along(d, r0["heading"])
                cams = [x[1]["camPos"] for x in s]
                # camera path smoothness: max per-sample backwards step along the motion
                steps_f = [along((b[0] - a_[0], b[1] - a_[1]), r0["heading"])[0] for a_, b in zip(cams, cams[1:])]
                rec(dict(step="hmd 50cm fwd (0.5 s tween)", hmd=r1["hmd"], along_forward=round(fw, 2), along_right=round(rt, 2),
                         cam_fwd=round(along((r1["camPos"][0] - r0["camPos"][0], r1["camPos"][1] - r0["camPos"][1]), r0["heading"])[0], 2),
                         cam_back_steps=round(min(steps_f, default=0), 2), eCm=r1["eCm"], residual=r1["residualCm"]))
                vremu("/pose?dev=hmd&rel=1&x=0.3&ms=400")
                e.sample(1.0)
                r2 = e.settle()
                d = (r2["loc"][0] - r1["loc"][0], r2["loc"][1] - r1["loc"][1])
                fw, rt = along(d, r1["heading"])
                rec(dict(step="hmd 30cm right", along_forward=round(fw, 2), along_right=round(rt, 2), eCm=r2["eCm"]))
                vremu("/pose?dev=hmd&yaw=-60&ms=300")  # vremu yaw +: left? record whichever
                time.sleep(1.5)
                r3 = e.rs()
                rec(dict(step="hmd yaw 60 (vremu yaw=-60)", dheading=round(r3["heading"] - r2["heading"], 4),
                         dcamYaw_deg=round(math.degrees(r3["camYaw"] - r2["camYaw"]), 2), bodyRel=r3["bodyRel"],
                         hmdYaw=r3["hmdYaw"]))
                vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0&ms=500")
                time.sleep(1.5)
                r4 = e.rs()
                rec(dict(step="hmd back to origin", bodyRel=r4["bodyRel"], eCm=r4["eCm"]))
        finally:
            try:
                e.cmd("pose off")
                e.cmd("rs cfg spring 0")
                if a.vremu:
                    vremu("/stopall")
                    vremu("/reset")
            except Exception:
                pass
            e.out["results"] = results
            e.out["final_rs"] = e.cmd("rs")
            json.dump(e.out, open(os.path.join(outdir, "rs.json"), "w"), indent=1, default=str)
            say(f"wrote {outdir}")


if __name__ == "__main__":
    main()
