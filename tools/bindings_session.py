"""FO4VR default controller bindings (ControllerBindings.cpp) - button tests under SteamVR + vremu.
Attaches to the session launched by gun_session.py (same pid file). Presses Touch inputs with vremu and checks:
the pad edge counters ('bindings'), and the game's reaction (open menus, Pip-Boy page/tab, camera height for jump).

    python -I bindings_session.py            # all tests -> work/gun/bindings-<ts>.json
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402


def bind(g):
    r = g.cmd("bindings", quiet=True)
    d = {}
    for k, v in re.findall(r"(\w+)=([-\w.]+)", r):
        d[k] = v
    return d, r


def menus(g):
    return [m.split()[0] for m in g.cmd("menus", quiet=True).split(" | ")[1:]]


def pip(g):
    s = g.cmd("pipboy", quiet=True)
    m = re.search(r"page=(\d+) tab=(\d+)", s)
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def cam_z(g):
    m = re.search(r"cam=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", g.cmd("gun", quiet=True))
    return float(m.group(3)) if m else None


def edge(before, after, k):
    return int(after.get(k, 0)) - int(before.get(k, 0))


def main():
    g = gs.attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.vremu(gs.HMD0), gs.hand(), gs.left()
        time.sleep(1.0)
        b0, raw = bind(g)
        res["idle"] = raw
        # --- gameplay ---------------------------------------------------------------------------------------------
        def press(dev, q, wait=0.8):
            b = bind(g)[0]
            gs.vremu(f"/input?dev={dev}&{q}&ms=300")
            time.sleep(wait)
            return b, bind(g)[0]

        z0 = cam_z(g)
        b, a = press("right", "joy=0,1", 0.15)
        z1 = cam_z(g)
        time.sleep(0.8)
        res["R stick up -> jump"] = dict(ctx=b.get("ctx"), yEdges=edge(b, a, "y"), camZ=(z0, z1, cam_z(g)))
        b, a = press("right", "joy=0,-1")
        res["R stick down -> sneak (R3)"] = dict(rsEdges=edge(b, a, "rs"))
        b, a = press("right", "joy=0,-1")  # toggle back
        b, a = press("right", "a=1")
        res["A -> activate (A)"] = dict(aEdges=edge(b, a, "a"))
        b, a = press("left", "x=1")
        res["X -> activate (A)"] = dict(aEdges=edge(b, a, "a"))
        b, a = press("left", "joy_click=1")
        res["L stick click -> sprint (L3)"] = dict(lsEdges=edge(b, a, "ls"))
        b = bind(g)[0]
        gs.vremu("/input?dev=right&grip=1&ms=120")
        time.sleep(0.6)
        a = bind(g)[0]
        res["R grip tap -> reload (X)"] = dict(xEdges=edge(b, a, "x"), rbEdges=edge(b, a, "rb"))
        # B -> VATS
        b = bind(g)[0]
        gs.vremu("/input?dev=right&b=1&ms=200")
        time.sleep(1.0)
        a = bind(g)[0]
        m1 = menus(g)
        res["B -> V.A.T.S. (LB)"] = dict(lbEdges=edge(b, a, "lb"), menus=m1)
        if "VATSMenu" in m1:
            gs.vremu("/input?dev=right&b=1&ms=200")
            time.sleep(1.5)
            res["B -> V.A.T.S. (LB)"]["menusAfterSecondB"] = menus(g)
        # Y tap -> pause menu, grip -> close
        b = bind(g)[0]
        gs.vremu("/input?dev=left&y=1&ms=150")
        time.sleep(1.5)
        a = bind(g)[0]
        m1 = menus(g)
        gs.vremu("/input?dev=right&grip=1&ms=200")
        time.sleep(1.5)
        res["Y tap -> pause (Start); grip -> back (B)"] = dict(startEdges=edge(b, a, "start"), menusOpen=m1, menusAfterGrip=menus(g))
        # --- Pip-Boy ------------------------------------------------------------------------------------------------
        b = bind(g)[0]
        gs.vremu("/input?dev=left&trigger=1&ms=200")
        time.sleep(2.5)
        a = bind(g)[0]
        m1 = menus(g)
        res["L trigger -> Pip-Boy (B)"] = dict(bEdges=edge(b, a, "b"), menus=m1)
        if "PipboyMenu" in m1:
            p0 = pip(g)
            ctx = bind(g)[0].get("ctx")
            b, a = press("left", "y=1", 1.0)
            p1 = pip(g)
            b2, a2 = press("left", "x=1", 1.0)
            p2 = pip(g)
            res["Pip-Boy X/Y -> tabs (LT/RT)"] = dict(ctx=ctx, pageTab0=p0, afterY=p1, afterX=p2, pass_=(p1[0] == (p0[0] + 1) % 5 and p2[0] == p0[0]))
            b, a = press("left", "joy=1,0", 1.0)
            p3 = pip(g)
            b2, a2 = press("left", "joy=-1,0", 1.0)
            p4 = pip(g)
            res["Pip-Boy L stick left/right -> sections (LS)"] = dict(before=p2, afterRight=p3, afterLeft=p4, lsRight=edge(b, a, "lsRight"),
                                                                       lsLeft=edge(b2, a2, "lsLeft"))
            b, a = press("left", "trigger=1", 1.0)
            res["Pip-Boy L trigger -> select (A)"] = dict(aEdges=edge(b, a, "a"), pageTab=pip(g))
            b = bind(g)[0]
            gs.vremu("/input?dev=right&grip=1&ms=200")
            time.sleep(2.0)
            a = bind(g)[0]
            res["Pip-Boy grip -> close (B)"] = dict(bEdges=edge(b, a, "b"), menus=menus(g))
        # favorites (R stick click), holster (R stick click + R grip), grenade hold (R grip held)
        b = bind(g)[0]
        gs.vremu("/input?dev=right&joy_click=1&ms=400")
        time.sleep(1.2)
        a = bind(g)[0]
        m1 = menus(g)
        res["R stick click -> favorites (D-pad up)"] = dict(dpadUpEdges=edge(b, a, "dpadUp"), menus=m1)
        if "FavoritesMenu" in m1:
            gs.vremu("/input?dev=right&grip=1&ms=200")
            time.sleep(1.2)
            res["R stick click -> favorites (D-pad up)"]["menusAfterGrip"] = menus(g)
        ws0 = parse_kv(g.cmd("gun", quiet=True)).get("ws")
        gs.vremu("/input?dev=right&joy_click=1&ms=1500")
        time.sleep(0.2)
        gs.vremu("/input?dev=right&grip=1&ms=1200")
        time.sleep(2.5)
        ws1 = parse_kv(g.cmd("gun", quiet=True)).get("ws")
        res["R stick click + R grip -> holster (hold X)"] = dict(weaponState=(ws0, ws1))
        gs.vremu("/input?dev=right&trigger=1&ms=200")  # draw again
        time.sleep(2.0)
        b = bind(g)[0]
        gs.vremu("/input?dev=right&grip=1&ms=800")
        time.sleep(1.2)
        a = bind(g)[0]
        res["R grip hold -> grenade (RB held)"] = dict(rbEdges=edge(b, a, "rb"), xEdges=edge(b, a, "x"), wsAfterDraw=parse_kv(g.cmd("gun", quiet=True)).get("ws"))
        res["final"] = bind(g)[1]
    finally:
        try:
            gs.vremu("/stopall")
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, time.strftime("bindings-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(out, "w"), indent=1, default=str)
    print(json.dumps(res, indent=1, default=str))
    print("saved", out)


if __name__ == "__main__":
    main()
