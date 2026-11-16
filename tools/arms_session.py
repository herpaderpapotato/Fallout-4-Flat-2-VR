"""Arms-hidden regression in the natural play sequence (SteamVR + vremu, own session from gun_session.py launch).

Holster / draw with the controller bindings (draw animation plays), fists, Pip-Boy open/close, walk + turn, pause
menu. Per step: the floating-gun diag counters (A1 type skips = draws the AppCulled bit alone would have let through,
game flag clears, new parts, collects during the next update), the visible first-person geometry walk, and a
32-frame capture burst split per eye (frame-to-frame change per eye half = flash detector).

    python -I arms_session.py            -> work/gun/arms/<ts>/report.json + bursts
"""
import json
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402

KEYS = ("gate", "ws", "weapon", "armsHidden", "armsCulled", "armsCandidates", "typeSkips", "flagClears", "newParts", "staleFrames", "collects",
        "collectDuringNextUpdate", "collectWithAnimatedRig", "cullSkips", "cullAdds")


def gk(g):
    return parse_kv(g.cmd("gun", quiet=True))


def visible(g):
    m = re.search(r"visibleGeomOutsideWeapon=(\d+) \[([^\]]*)\]", g.cmd("gun dump", quiet=True))
    if not m:
        return (None, "")
    # the controller stand-ins ([Weapon] bControllerModels, FO4VR's Vive_Wand.nif) are meant to be drawn: not arms
    names = [n for n in re.findall(r"'[^']*'@'[^']*'", m.group(2)) if "vr_controller" not in n]
    return (len(names), " ".join(names))


def read_bmp(p):
    d = open(p, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    bpp, = struct.unpack_from("<H", d, 28)
    return d, off, w, abs(h), bpp // 8


def half_diff(a, b, step=4):
    """Mean abs RGB difference per eye half (left, right) between two burst frames."""
    da, oa, w, h, ba = read_bmp(a)
    db, ob, _, _, bb = read_bmp(b)
    sa, sb = (w * ba + 3) & ~3, (w * bb + 3) & ~3
    out = []
    for x0, x1 in ((0, w // 2), (w // 2, w)):
        tot = n = 0
        for y in range(0, h, step * 2):
            ra, rb = oa + y * sa, ob + y * sb
            for x in range(x0, x1, step * 2):
                pa, pb = ra + x * ba, rb + x * bb
                tot += abs(da[pa] - db[pb]) + abs(da[pa + 1] - db[pb + 1]) + abs(da[pa + 2] - db[pb + 2])
                n += 1
        out.append(round(tot / max(n, 1), 2))
    return out


def burst(g, outdir, name, n=int(os.environ.get("FO4_BURST_N", "8"))):
    stem = os.path.join(outdir, name).replace("\\", "/")
    g.cmd(f"captureburst {n} {stem}", quiet=True)
    t0 = time.time()
    files = [f"{stem}_{i:02}.bmp" for i in range(n)]
    while time.time() - t0 < 30 and not all(os.path.exists(f) and os.path.getsize(f) > 1000 for f in files):
        time.sleep(0.3)
    time.sleep(0.5)
    have = [f for f in files if os.path.exists(f)]
    diffs = [half_diff(have[i - 1], have[i]) for i in range(1, len(have))]
    return dict(frames=len(have), maxLeft=max((d[0] for d in diffs), default=None), maxRight=max((d[1] for d in diffs), default=None),
                series=diffs)


def step(g, outdir, res, name, action, wait=0.4, cap=True):
    before = gk(g)
    action()
    time.sleep(wait)
    b = burst(g, outdir, name) if cap else None
    after = gk(g)
    vis = visible(g)
    delta = {k: (int(after[k]) - int(before[k])) for k in ("typeSkips", "flagClears", "newParts", "staleFrames", "collects", "collectDuringNextUpdate",
                                                          "collectWithAnimatedRig") if k in after and k in before}
    res[name] = dict(delta=delta, ws=(before.get("ws"), after.get("ws")), gate=after.get("gate"), visibleOutsideGun=vis, burst=b)
    print(name, json.dumps(res[name]["delta"]), "ws", res[name]["ws"], "visible", vis[0], "burst max diff L/R",
          b and (b["maxLeft"], b["maxRight"]))


def main():
    # capture bursts are ~60 MB per frame at 5376x2880 (one run of 16 steps x 32 frames filled G: on 14 Nov, 84 GB):
    # they go to BURST_DIR (env FO4_BURST_DIR, default E:\Fallout4ToVR-overflow\bursts), the report stays in work/gun/arms
    ts = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(gs.OUT, "arms", ts)
    burstdir = os.path.join(os.environ.get("FO4_BURST_DIR", r"E:\Fallout4ToVR-overflow\bursts"), ts)
    os.makedirs(burstdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    g = gs.attach()
    res = {}
    try:
        print(g.cmd("gun diag on", quiet=True))
        gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
        time.sleep(1.0)
        step(g, burstdir, res, "00_baseline", lambda: None)
        step(g, burstdir, res, "01_holster", lambda: (gs.vremu("/input?dev=right&joy_click=1&ms=1500"), time.sleep(0.2),
                                                   gs.vremu("/input?dev=right&grip=1&ms=1200")), wait=0.3)
        time.sleep(2.0)
        step(g, burstdir, res, "02_draw", lambda: gs.vremu("/input?dev=right&trigger=1&ms=200"), wait=0.1)
        time.sleep(2.0)
        step(g, burstdir, res, "03_unequip_to_fists", lambda: g.cmd("aim unequip 00004822", quiet=True), wait=1.0)
        step(g, burstdir, res, "04_raise_fists", lambda: gs.vremu("/input?dev=right&trigger=1&ms=200"), wait=0.1)
        time.sleep(2.0)
        step(g, burstdir, res, "05_pipboy_open", lambda: gs.vremu("/input?dev=left&trigger=1&ms=200"), wait=1.5)
        step(g, burstdir, res, "06_pipboy_close", lambda: gs.vremu("/input?dev=right&grip=1&ms=200"), wait=0.1)
        time.sleep(1.5)
        step(g, burstdir, res, "07_walk_turn", lambda: (gs.vremu("/input?dev=left&joy=0,1&ms=2500"), gs.vremu("/input?dev=right&joy=0.6,0&ms=2500")),
             wait=0.5)
        time.sleep(2.5)
        step(g, burstdir, res, "08_pause_open", lambda: gs.vremu("/input?dev=left&y=1&ms=150"), wait=1.5)
        step(g, burstdir, res, "09_pause_close", lambda: gs.vremu("/input?dev=right&grip=1&ms=200"), wait=0.1)
        time.sleep(1.5)
        step(g, burstdir, res, "10_equip_10mm_draw", lambda: (g.cmd("aim equip0 00004822", quiet=True), time.sleep(1.5),
                                                          gs.vremu("/input?dev=right&trigger=1&ms=200")), wait=0.1)
        time.sleep(2.0)
        # equip changes (user report: arms appear after unequipping the Vault suit and stay after re-equipping it;
        # with a multi-part outfit they show all the time)
        step(g, burstdir, res, "11_unequip_vault_suit", lambda: g.cmd("console player.unequipitem 0001EED7", quiet=True), wait=1.5)
        step(g, burstdir, res, "12_reequip_vault_suit", lambda: g.cmd("console player.equipitem 0001EED7", quiet=True), wait=1.5)
        step(g, burstdir, res, "13_equip_minuteman_outfit", lambda: (g.cmd("console player.additem 000E517B 1", quiet=True), time.sleep(0.5),
                                                                 g.cmd("console player.equipitem 000E517B", quiet=True)), wait=1.5)
        res["13_dump"] = g.cmd("gun dump", quiet=True)[:1500]
        step(g, burstdir, res, "14_unequip_minuteman_outfit", lambda: g.cmd("console player.unequipitem 000E517B", quiet=True), wait=1.5)
        step(g, burstdir, res, "15_reequip_vault_suit", lambda: g.cmd("console player.equipitem 0001EED7", quiet=True), wait=1.5)
        res["final"] = {k: gk(g).get(k) for k in KEYS}
    finally:
        try:
            gs.vremu("/stopall")
        except OSError:
            pass
        g._close()
    json.dump(res, open(os.path.join(outdir, "report.json"), "w"), indent=1, default=str)
    print("saved", outdir)


if __name__ == "__main__":
    main()
