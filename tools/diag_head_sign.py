"""Diagnose head-rotation sign under XR (SteamVR + vremu): for the same HMD yaw, compare the camera
matrix readback with the captured image shift, and the scripted 'pose' override with both."""
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import suite  # noqa: E402  (reuses its image metric + cam parsers)
from fo4test import Game, say  # noqa: E402

STEAMVR = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "work", "shots", "headsign"))


def vr(path):
    urllib.request.urlopen("http://127.0.0.1:8766" + path, timeout=3).read()


def shot(g, name):
    p = os.path.join(OUT, name + ".bmp").replace("\\", "/")
    g.cmd(f"capture {p}", quiet=True)
    for _ in range(40):
        time.sleep(0.1)
        if suite.bmp_complete(p):
            break
    time.sleep(0.3)
    return p


def yaw_of(g):
    return suite.cam_yaw_pitch(g.cmd("cam", quiet=True))


def main():
    os.makedirs(OUT, exist_ok=True)
    with Game(runtime_json=STEAMVR) as g:
        g.coc("QASmoke")
        time.sleep(2)
        g.cmd("track on")
        g.cmd("mode mono")
        g.cmd("pose off")
        vr("/pose?dev=hmd&ypr=0,0,0")
        time.sleep(1.5)
        say(f"baseline cam yaw/fz {yaw_of(g)}  status {g.cmd('status', quiet=True)[:80]}")
        b = shot(g, "base")
        vr("/pose?dev=hmd&ypr=30,0,0")
        time.sleep(1.5)
        say(f"hmd yaw+30: cam yaw/fz {yaw_of(g)}")
        h = shot(g, "hmd_yaw30")
        vr("/pose?dev=hmd&ypr=0,0,0")
        time.sleep(1.5)
        g.cmd("pose 30 0 0")
        time.sleep(1.0)
        say(f"override yaw+30: cam yaw/fz {yaw_of(g)}")
        o = shot(g, "ovr_yaw30")
        g.cmd("pose off")
        vr("/pose?dev=hmd&ypr=0,20,0")
        time.sleep(1.5)
        say(f"hmd pitch+20: cam yaw/fz {yaw_of(g)}")
        p = shot(g, "hmd_pitch20")
        vr("/pose?dev=hmd&ypr=0,0,0")
        g.cmd("track off")
        base = suite.grey_small(b) if hasattr(suite, "grey_small") else None
        for name, path in (("hmd_yaw30", h), ("ovr_yaw30", o), ("hmd_pitch20", p)):
            if base is not None:
                m = suite.best_shift(base, suite.grey_small(path))
                say(f"{name}: image content shift {m}")


if __name__ == "__main__":
    main()
