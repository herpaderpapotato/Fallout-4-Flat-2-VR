"""C8 locomotion comfort experiment (plugin README "Locomotion comfort"): snap / smooth turn, stick direction
(head / hand / body), vignette layer. Needs SteamVR in vremu emulator mode (HTTP API on 127.0.0.1:8766).

    python -I comfort_experiment.py [--attach] [--only snap,smooth,game,hand,head,body,vignette] [--cell QASmoke]

One launch (or --attach). Drives the emulated controllers / HMD, reads the `comfort` and `rs` readouts, prints the
numbers per step and writes them to work/comfort/<ts>/comfort.json. Restores vremu, `rs cfg body 1` and the
comfort defaults (snap, head, vignette on) at the end.
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
STEPS = ["snap", "smooth", "game", "hand", "head", "body", "vignette"]


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def num(d, k):
    v = d.get(k)
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def vec(s):
    m = re.match(r"\(([^)]*)\)", s or "")
    return tuple(float(x) for x in m.group(1).split(",")) if m else None


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


class Exp:
    def __init__(self, g):
        self.g = g
        self.out = {}

    def cmd(self, c):
        return self.g.cmd(c, quiet=True)

    def rs(self):
        return parse_kv(self.cmd("rs"))

    def comfort(self):
        return parse_kv(self.cmd("comfort"))

    def heading_deg(self):
        r = self.rs()
        return math.degrees(float(r["heading"])), math.degrees(float(r["ownYaw"])), float(r.get("userTurnTotal", "nan")), r

    def loc(self):
        r = self.rs()
        return vec(r["loc"]), math.degrees(float(r["heading"]))

    def settle(self, s=0.6):
        time.sleep(s)

    # ---- turning --------------------------------------------------------------------------------
    def snap(self):
        self.cmd("comfort turn snap")
        self.cmd("comfort stats reset")
        self.settle()
        res = []
        for direction, n in (("right", 3), ("left", 1)):
            x = "1" if direction == "right" else "-1"
            for i in range(n):
                h0, o0, u0, _ = self.heading_deg()
                vremu(f"/input?dev=right&joy={x},0&ms=350")  # held 350 ms: one snap only (re-arm needs release)
                time.sleep(0.9)
                h1, o1, u1, _ = self.heading_deg()
                time.sleep(1.0)  # drift check: nothing changes while idle
                h2, o2, u2, _ = self.heading_deg()
                step = {"dir": direction, "dHeading": round(wrap180(h1 - h0), 4), "dOwnYaw": round(wrap180(o1 - o0), 4),
                        "dUserTurn": round(math.degrees(u1 - u0), 4), "drift1s": round(wrap180(h2 - h1), 4)}
                res.append(step)
                say(f"snap {direction}: {step}")
        c = self.comfort()
        self.out["snap"] = {"steps": res, "snaps": c.get("snaps"), "snapDegTotal": c.get("snapDegTotal")}

    def smooth_run(self, mode, x, ms):
        self.cmd(f"comfort turn {mode}")
        self.cmd("comfort stats reset")
        self.settle()
        h0, o0, u0, _ = self.heading_deg()
        vremu(f"/input?dev=right&joy={x},0&ms={ms}")
        time.sleep(ms / 1000.0 + 0.8)
        h1, o1, u1, _ = self.heading_deg()
        c = self.comfort()
        sm, ss = num(c, "smoothDegTotal"), num(c, "smoothSec")
        r = {"mode": mode, "x": x, "ms": ms, "dHeading": round(wrap180(h1 - h0), 3), "dOwnYaw": round(wrap180(o1 - o0), 3),
             "dUserTurn": round(math.degrees(u1 - u0), 3), "ratePerHold": round(wrap180(h1 - h0) / (ms / 1000.0), 2),
             "comfortRate": round(sm / ss, 3) if sm is not None and ss else None, "smoothSec": ss}
        say(f"turn {mode} x={x} {ms} ms: {r}")
        return r

    def smooth(self):
        self.out["smooth"] = [self.smooth_run("smooth", "1", 1000), self.smooth_run("smooth", "0.575", 1000)]

    def game(self):
        # baseline: the game's own right-stick look speed (rx passes to the pad)
        self.out["game"] = self.smooth_run("off", "1", 1000)
        self.cmd("comfort turn snap")

    # ---- direction --------------------------------------------------------------------------------
    def walk(self, what, ms=1200):
        self.settle(0.8)
        p0, h0 = self.loc()
        vremu(f"/input?dev=left&joy=0,1&ms={ms}")
        time.sleep(0.25)
        c = self.comfort()  # mid-walk: delta used, rotated stick
        time.sleep(ms / 1000.0 + 0.4)
        p1, h1 = self.loc()
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        dirdeg = math.degrees(math.atan2(dx, dy))  # heading convention: forward = (sin h, cos h)
        r = {"what": what, "dist": round(math.hypot(dx, dy), 1), "moveDirRelBody0": round(wrap180(dirdeg - h0), 2),
             "heading0": round(h0, 2), "dHeading": round(wrap180(h1 - h0), 2), "deltaDeg": c.get("deltaDeg"),
             "handRelDeg": c.get("handRelDeg"), "out": c.get("out"), "handUsed": c.get("handUsed")}
        say(f"walk {what}: {r}")
        return r

    def hand(self):
        self.cmd("comfort move hand")
        vremu("/pose?dev=hmd&ypr=0,0,0")
        vremu("/pose?dev=left&ypr=0,0,0")
        a = self.walk("hand yaw 0")
        vremu("/pose?dev=left&ypr=90,0,0")
        b = self.walk("hand yaw +90")
        vremu("/pose?dev=left&ypr=-90,0,0")
        c = self.walk("hand yaw -90")
        vremu("/pose?dev=left&ypr=0,0,0")
        self.out["hand"] = [a, b, c]

    def head(self):
        # body follow off, so the body stays while the HMD turns 90 deg (with it on the body turns to the head)
        self.cmd("rs cfg body 0")
        try:
            self.cmd("comfort move head")
            vremu("/pose?dev=hmd&ypr=0,0,0")
            a = self.walk("head yaw 0")
            vremu("/pose?dev=hmd&ypr=90,0,0")
            b = self.walk("head yaw +90")
            self.cmd("comfort move body")
            c = self.walk("head yaw +90, move=body")
            self.cmd("comfort move head")
        finally:
            vremu("/pose?dev=hmd&ypr=0,0,0")
            self.cmd("rs cfg body 1")
        self.out["head"] = [a, b, c]

    def body(self):
        self.cmd("comfort move body")
        vremu("/pose?dev=hmd&ypr=0,0,0")
        vremu("/pose?dev=left&ypr=90,0,0")
        a = self.walk("move=body, hand yaw +90")
        vremu("/pose?dev=left&ypr=0,0,0")
        self.cmd("comfort move head")
        self.out["body"] = [a]

    # ---- vignette -------------------------------------------------------------------------------
    def vignette(self):
        self.cmd("comfort turn snap")
        self.cmd("comfort move head")
        self.cmd("comfort vignette on")
        self.settle(1.0)
        self.cmd("comfort stats reset")
        time.sleep(1.5)
        idle = self.comfort()
        vremu("/input?dev=left&joy=0,1&ms=1500")
        time.sleep(0.6)
        moving = self.comfort()
        time.sleep(1.5)
        after = self.comfort()
        self.cmd("comfort stats reset")
        vremu("/input?dev=right&joy=1,0&ms=300")  # one snap: vignette held 200 ms + fade
        time.sleep(1.2)
        snap = self.comfort()
        self.cmd("comfort vignette off")
        self.cmd("comfort stats reset")
        vremu("/input?dev=left&joy=0,1&ms=1000")
        time.sleep(1.4)
        off = self.comfort()
        self.cmd("comfort vignette on")
        keys = ("alpha", "target", "moving", "turning", "frames", "vigFrames", "vigActive", "vigTail", "activeNoVig", "lastLayers",
                "layersWithVig", "layersWithoutVig", "uploads")
        pick = lambda d: {k: d.get(k) for k in keys}
        self.out["vignette"] = {"idle1.5s": pick(idle), "moving": pick(moving), "after": pick(after), "snap": pick(snap),
                                "vignetteOff_walk": pick(off)}
        for k, v in self.out["vignette"].items():
            say(f"vignette {k}: {v}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--attach", action="store_true")
    ap.add_argument("--only", default=",".join(STEPS))
    ap.add_argument("--cell", default="QASmoke")
    a = ap.parse_args()
    steps = [s for s in a.only.split(",") if s]
    out_dir = os.path.join(ROOT, "work", "comfort", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)
    vremu("/stopall")
    vremu("/reset")
    with Game(runtime_json=STEAMVR_JSON, attach=a.attach, quiet=True) as g:
        rd = parse_kv(g.cmd("ready", quiet=True))
        if rd.get("ingame") != "1" or rd.get("cell") != a.cell:
            g.coc(a.cell)
            time.sleep(3.0)
        g.cmd("track on", quiet=True)
        g.cmd("mode mono", quiet=True)
        g.cmd("pose off", quiet=True)
        e = Exp(g)
        e.out["cfg"] = e.cmd("comfort cfg")
        say(e.out["cfg"])
        try:
            for s in steps:
                getattr(e, s)()
        finally:
            for p in ("/stopall", "/reset"):
                try:
                    vremu(p)
                except OSError:
                    pass
            for c in ("rs cfg body 1", "comfort turn snap", "comfort move head", "comfort vignette on"):
                try:
                    g.cmd(c, quiet=True)
                except Exception:
                    pass
            with open(os.path.join(out_dir, "comfort.json"), "w", encoding="utf-8") as fh:
                json.dump(e.out, fh, indent=1)
            say(f"wrote {out_dir}")


if __name__ == "__main__":
    main()
