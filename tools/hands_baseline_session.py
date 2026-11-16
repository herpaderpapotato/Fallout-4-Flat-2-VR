"""Controller stand-ins + per-hand activation (FO4VR baseline), own gun_session.py session (SteamVR + vremu).

1. Controller models: holstered -> both models shown; drawn (controller trigger) -> primary hidden, off shown.
   Captures with both hands in view (work/gun/hands-<ts>-*.bmp).
2. Per-hand pick: 'aim pickhand 0|1' with the left controller pointed down at the feet and the right one level:
   the pick ray / pick ref follow the requested hand.
3. Per-hand activation: an item dropped at the feet (placeatme), left controller pointing at it, right pointing level:
   A (right) must not pick it up, X (left) must (inventory count via 'aim' equipped list is not enough - uses
   'console player.getitemcount' output in the log is not readable, so the pick ref + the 'bindings' activate counters
   + the item's disappearance from the pick are recorded).

    python -I hands_baseline_session.py   -> work/gun/hands-<ts>.json
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402

ITEM = "00023736"  # Stimpak


def aimkv(g):
    return parse_kv(g.cmd("aim", quiet=True))


def gun(g):
    return parse_kv(g.cmd("gun", quiet=True))


def main():
    g = gs.attach()
    ts = time.strftime("%Y%m%d-%H%M%S")
    res = {}
    try:
        gs.vremu("/stopall"), gs.vremu(gs.HMD0)
        # hands in front of the face, slightly apart, pointing forward-down so the models are in view
        gs.hand(pos=(0.15, 1.45, -0.35), ypr=(0, 20, 0))
        gs.left(pos=(-0.15, 1.45, -0.35), ypr=(0, 20, 0))
        time.sleep(1.0)
        d0 = gun(g)
        res["drawn"] = {k: d0.get(k) for k in ("gate", "ws", "weapon", "ctrlR", "ctrlL", "ctrlShownFrames")}
        res["drawn"]["capture"] = gs.capture(g, f"hands-{ts}-drawn")
        # holster with the controller binding (primary stick click + primary grip)
        gs.vremu("/input?dev=right&joy_click=1&ms=1500"), time.sleep(0.2), gs.vremu("/input?dev=right&grip=1&ms=1200")
        time.sleep(2.5)
        d1 = gun(g)
        res["holstered"] = {k: d1.get(k) for k in ("gate", "ws", "weapon", "ctrlR", "ctrlL", "ctrlShownFrames")}
        res["holstered"]["capture"] = gs.capture(g, f"hands-{ts}-holstered")
        # move the right hand 20 cm right: its model follows (frames keep counting, capture differs)
        gs.hand(pos=(0.35, 1.45, -0.35), ypr=(0, 20, 0))
        time.sleep(0.6)
        res["holstered_moved_capture"] = gs.capture(g, f"hands-{ts}-holstered-rmoved")
        if res["holstered"]["capture"] and res["holstered_moved_capture"]:
            res["moveDiff"] = gs.bmp_diff(res["holstered"]["capture"], res["holstered_moved_capture"])
        # ctrl off -> no models
        g.cmd("gun cfg ctrl 0", quiet=True)
        time.sleep(0.5)
        res["ctrlOff"] = {k: gun(g).get(k) for k in ("ctrlR", "ctrlL")}
        res["ctrlOff"]["capture"] = gs.capture(g, f"hands-{ts}-ctrloff")
        if res["holstered"]["capture"] and res["ctrlOff"]["capture"]:
            res["onOffDiff"] = gs.bmp_diff(res["holstered"]["capture"], res["ctrlOff"]["capture"])
        g.cmd("gun cfg ctrl 1", quiet=True)

        # ---- per-hand pick / activation: find an activatable ref with the RIGHT ray (sweep), then put the LEFT controller
        # on that pose and point the right one at the sky. A must not activate it, X must.
        g.cmd(f"console player.placeatme {ITEM} 3", quiet=True)
        time.sleep(2.5)
        g.cmd("aim pickhand 0", quiet=True)
        found = None
        for p in (-10, -20, -30, -40, -50, -60):
            for yaw in (0, 15, -15, 30, -30, 45, -45):
                pose = ((0.2, 1.3, -0.35), (yaw, gs.HAND_YPR0[1] + p, 0))
                gs.hand(*pose)
                time.sleep(0.35)
                a = aimkv(g)
                if a.get("pick") not in (None, "00000000"):
                    found = dict(pose=pose, pick=a.get("pick"), name=a.get("pickName"))
                    break
            if found:
                break
        res["found"] = found
        print("found", found)
        if found:
            pos, ypr = found["pose"]
            gs.left(pos=pos, ypr=ypr)
            gs.hand(pos=(0.2, 1.3, -0.35), ypr=(0, 80, 0))  # right points at the sky
            time.sleep(0.6)
            r0 = aimkv(g)
            g.cmd("aim pickhand 1", quiet=True)
            time.sleep(0.6)
            r1 = aimkv(g)
            g.cmd("aim pickhand 0", quiet=True)
            time.sleep(0.4)
            res["picks"] = dict(primaryPick=r0.get("pick"), offPick=r1.get("pick"), offName=r1.get("pickName"), offPitch=r1.get("offPitch"),
                                offHdg=r1.get("offHdg"), pickRunsOff=r1.get("pickRunsOff"))
            b0 = g.cmd("bindings", quiet=True)
            gs.vremu("/input?dev=right&a=1&ms=120")  # A: right ray (sky) -> nothing
            time.sleep(1.5)
            g.cmd("aim pickhand 1", quiet=True)
            time.sleep(0.5)
            afterA = aimkv(g).get("pick")
            g.cmd("aim pickhand 0", quiet=True)
            menusA = g.cmd("menus", quiet=True)[:200]
            time.sleep(0.5)
            gs.vremu("/input?dev=left&x=1&ms=120")  # X: left ray -> the ref
            time.sleep(1.5)
            menusX = g.cmd("menus", quiet=True)[:300]
            g.cmd("aim pickhand 1", quiet=True)
            time.sleep(0.5)
            afterX = aimkv(g)
            g.cmd("aim pickhand 0", quiet=True)
            b1 = g.cmd("bindings", quiet=True)
            res["activate"] = dict(ref=found["pick"], name=found["name"], offPickAfterA=afterA, menusAfterA=menusA, offPickAfterX=afterX.get("pick"),
                                   offNameAfterX=afterX.get("pickName"), menusAfterX=menusX, bindingsBefore=b0[b0.find("| activate"):],
                                   bindingsAfter=b1[b1.find("| activate"):])
            print(json.dumps(res["activate"], indent=1))
            gs.vremu("/input?dev=right&grip=1&ms=200")  # close a container menu if one opened
            time.sleep(1.0)
    finally:
        try:
            gs.vremu("/stopall"), gs.hand(), gs.left()
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, f"hands-{ts}.json")
    json.dump(res, open(out, "w"), indent=1, default=str)
    print(json.dumps({k: v for k, v in res.items() if k != "picks"}, indent=1, default=str))
    print("saved", out)


if __name__ == "__main__":
    main()
