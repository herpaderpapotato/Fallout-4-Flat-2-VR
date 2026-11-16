"""Rig alternation A/B (SteamVR + vremu, own gun_session.py session): first-person rig / camera step reversals at the
render's first-person collect, with [Roomscale] bHeadAuthority on / off and roomscale on / off, while the head sways
and the player walks. Needs the floating-gun diagnostics ('gun diag on' is sent here).

    python -I alternation_ab.py   -> work/gun/alternation-<ts>.json
"""
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402

KEYS = ("altHand", "altRoot", "altCam", "altFpCam", "altFpOff", "collects", "typeSkips")


def counters(g):
    d = parse_kv(g.cmd("gun", quiet=True))
    return {k: int(d.get(k, 0)) for k in KEYS}, d.get("altHandAmp")


def motion(seconds, walk):
    t0 = time.time()
    if walk:
        gs.vremu(f"/input?dev=left&joy=0,0.8&ms={int(seconds * 1000)}")
    while time.time() - t0 < seconds:
        gs.vremu("/rate?dev=hmd&vx=0.2&wyaw=20&timeout=800")
        time.sleep(0.5)
        gs.vremu("/rate?dev=hmd&vx=-0.2&wyaw=-20&timeout=800")
        time.sleep(0.5)
    gs.vremu("/rate?dev=hmd&stop=1")


def main():
    g = gs.attach()
    res = {}
    try:
        print(g.cmd("gun diag on", quiet=True))
        for name, cmds in (("auth1_rs_on", ["rs cfg auth 1", "rs on"]), ("auth0_rs_on", ["rs cfg auth 0", "rs on"]),
                           ("auth1_rs_off", ["rs cfg auth 1", "rs off"]), ("auth0_rs_off", ["rs cfg auth 0", "rs off"])):
            for c in cmds:
                g.cmd(c, quiet=True)
            gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
            time.sleep(1.0)
            for label, walk in (("still", False), ("sway", False), ("sway_walk", True)):
                c0, _ = counters(g)
                if label == "still":
                    time.sleep(4.0)
                else:
                    motion(4.0, walk)
                c1, amp = counters(g)
                d = {k: c1[k] - c0[k] for k in KEYS}
                res[f"{name}/{label}"] = dict(delta=d, lastHandAmp=amp)
                print(name, label, d, "amp", amp)
        g.cmd("rs cfg auth 1", quiet=True)
        g.cmd("rs on", quiet=True)
    finally:
        try:
            gs.vremu("/stopall")
            gs.vremu(gs.HMD0)
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, time.strftime("alternation-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(out, "w"), indent=1)
    print("saved", out)


if __name__ == "__main__":
    main()
