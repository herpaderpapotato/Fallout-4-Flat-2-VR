"""Floating gun (FloatingGun.h, FO4VR baseline) test session under SteamVR + vremu. Save vrtest_sanctuary, never QASmoke.

    python -I gun_session.py launch      # refuse if Fallout4 / xmake runs; deploy build; launch; load vrtest_sanctuary; 10mm drawn
    python -I gun_session.py cmd "gun" "aim"
    python -I gun_session.py verify      # numbers -> work/gun/verify-<ts>.json (+ captures arms hidden / shown)
    python -I gun_session.py quit        # qqq the session this script launched (pid file); never another game

SteamVR is never closed or restarted here (shared, vremu emulator mode); the OpenXR runtime starts it if it is not running.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "work", "gun")
PIDF = os.path.join(OUT, "session.pid")
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
BUILD = os.path.join(ROOT, "plugin", "build", "windows", "x64", "releasedbg")
SAVE = "vrtest_sanctuary"
PISTOL_10MM = "00004822"
AMMO_10MM = "0001F276"
HMD0 = "/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0"
HAND0 = (0.2, 1.3, -0.35)
HAND_YPR0 = (0, 39, 0)   # vremu pitch +39 = SteamVR Touch aim pose level
LHAND0 = (-0.2, 1.3, -0.35)


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def vec(s):
    m = re.match(r"\(([^)]*)\)", s or "")
    return tuple(float(x) for x in m.group(1).split(",")) if m else None


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(sum(x * x for x in a))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def hdg(v):
    return math.degrees(math.atan2(v[0], v[1]))


def hand(pos=HAND0, ypr=HAND_YPR0):
    vremu(f"/pose?dev=right&pos={pos[0]},{pos[1]},{pos[2]}&ypr={ypr[0]},{ypr[1]},{ypr[2]}")


def left(pos=LHAND0, ypr=HAND_YPR0):
    vremu(f"/pose?dev=left&pos={pos[0]},{pos[1]},{pos[2]}&ypr={ypr[0]},{ypr[1]},{ypr[2]}")


def xmake_running():
    return "xmake" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()


def deploy():
    """Copy the current build (dll + pdb) only. No ini changes."""
    if game_pid():
        raise SystemExit("Fallout4 running - not deploying")
    if xmake_running():
        raise SystemExit("an xmake build is running - not deploying")
    import fo4test
    # MO2 mod folder (backup to work/deploy-backup). FRIK.dll is shipped since 15 Nov (FRIK-flat MO2 mod): the old
    # "FRIK.dll must be off" refusal is gone; its state is only reported.
    fo4test.deploy_plugin(BUILD, need=b"gun on=")
    if fo4test.mod_enabled(fo4test.MOD_FRIK):
        say("note: FRIK-flat is enabled in MO2 (FRIK smooth movement/body active in this session)")


def launch():
    os.makedirs(OUT, exist_ok=True)
    if game_pid() or xmake_running():
        raise SystemExit("Fallout4 or an xmake build is running - not launching")
    deploy()
    g = Game(runtime_json=STEAMVR_JSON)
    g.__enter__()
    pid = game_pid()
    open(PIDF, "w").write(str(pid))
    say(f"launched pid {pid}")
    g.cmd(f"console load {SAVE}")
    time.sleep(2.0)
    g._wait(f"in-world ({SAVE})", 90, lambda: g.ready().get("ingame") == "1")
    time.sleep(4.0)
    vremu(HMD0)
    hand()
    left()
    g.cmd(f"console player.additem {PISTOL_10MM} 1")
    g.cmd(f"console player.additem {AMMO_10MM} 200")
    time.sleep(1.0)
    g.cmd(f"aim equip0 {PISTOL_10MM}")  # equip manager, no draw
    time.sleep(1.5)
    vremu("/input?dev=right&trigger=1&ms=200")  # draw with the controller binding (draw animation)
    time.sleep(3.0)
    print(g.cmd("gun"))
    g._close()


def attach():
    pid = game_pid()
    if not pid:
        raise SystemExit("no Fallout4 running")
    try:
        own = int(open(PIDF).read().strip())
    except (OSError, ValueError):
        own = None
    if own != pid:
        raise SystemExit(f"Fallout4 pid {pid} is not this session's ({own}) - not touching it")
    g = Game(attach=True, quiet=True)
    g.__enter__()
    return g


def cmd(cmds):
    g = attach()
    for c in cmds:
        print(f"> {c}\n{g.cmd(c, quiet=True)}")
    g._close()


def quit_session():
    pid = game_pid()
    try:
        own = int(open(PIDF).read().strip())
    except (OSError, ValueError):
        own = None
    if not pid:
        say("not running")
        return
    if pid != own:
        raise SystemExit(f"Fallout4 pid {pid} is not this session's ({own}) - not quitting it")
    g = Game(attach=True, quiet=True)
    g.__enter__()
    try:
        g.cmd("console qqq")
    except Exception:
        pass
    g._close()
    t0 = time.time()
    while game_pid() and time.time() - t0 < 20:
        time.sleep(0.25)
    if game_pid():
        say("did not exit after qqq - killing own pid")
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    try:
        vremu("/stopall")
        vremu("/reset")
    except OSError:
        pass
    os.remove(PIDF)
    say("session closed")


def gk(g):
    return parse_kv(g.cmd("gun", quiet=True))


def settle(g, s=0.6):
    time.sleep(s)
    return gk(g)


def capture(g, name):
    path = os.path.join(OUT, name + ".bmp").replace("\\", "/")
    g.cmd("capture " + path, quiet=True)
    t0 = time.time()
    while time.time() - t0 < 10:
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            time.sleep(0.3)
            return path
        time.sleep(0.1)
    return None


def bmp_diff(a, b):
    """Pixels whose RGB differs by > 24 (sum of abs) between two 24/32-bit BMPs of the same size; also their bbox."""
    import struct

    def rd(p):
        d = open(p, "rb").read()
        off, = struct.unpack_from("<I", d, 10)
        w, h = struct.unpack_from("<ii", d, 18)
        bpp, = struct.unpack_from("<H", d, 28)
        return d, off, w, abs(h), bpp // 8

    da, oa, w, h, ba = rd(a)
    db, ob, w2, h2, bb = rd(b)
    if (w, h) != (w2, h2):
        return None
    sa, sb = (w * ba + 3) & ~3, (w * bb + 3) & ~3
    n = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(0, h, 2):
        ra, rb = oa + y * sa, ob + y * sb
        for x in range(0, w, 2):
            pa, pb = ra + x * ba, rb + x * bb
            if abs(da[pa] - db[pb]) + abs(da[pa + 1] - db[pb + 1]) + abs(da[pa + 2] - db[pb + 2]) > 24:
                n += 1
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return dict(px=n * 4, pct=round(100.0 * n * 4 / (w * h), 3), w=w, h=h, bbox=(x0, h - 1 - y1, x1, h - 1 - y0) if n else None)


def pipboy_quad(g):
    s = g.cmd("pipboy", quiet=True)
    m = re.search(r"pos ([-\d.]+),([-\d.]+),([-\d.]+) quat .*? grip ([-\d.]+),([-\d.]+),([-\d.]+)", s)
    if not m:
        return None, None, s[:200]
    v = [float(x) for x in m.groups()]
    return tuple(v[:3]), tuple(v[3:]), None


def verify():
    g = attach()
    res = {}
    try:
        vremu(HMD0)
        hand()
        left()
        g.cmd("gun on", quiet=True)
        g.cmd("gun arms on", quiet=True)
        g.cmd("aim on", quiet=True)
        h0 = settle(g, 1.5)
        res["base"] = {k: h0.get(k) for k in ("gate", "weapon", "j", "err", "angErr", "act", "wWorld", "handVsWeapon", "cam", "costUs", "avg", "max",
                                             "armsHidden", "armsCulled", "armsCandidates", "weaponLocalPersisted", "weaponLocalRewritten")}
        res["dump"] = g.cmd("gun dump", quiet=True)
        res["arms"] = g.cmd("gun arms", quiet=True)
        # 1) hand 30 cm right -> WEAPON moves 21 units along camera right; the arm rig stays (RArm_Hand - WEAPON changes)
        w0 = vec(h0["wWorld"])
        hand(pos=(HAND0[0] + 0.3, HAND0[1], HAND0[2]))
        h1 = settle(g)
        d = sub(vec(h1["wWorld"]), w0)
        res["move30cm"] = dict(delta=[round(x, 3) for x in d], len=round(norm(d), 3), err=h1.get("err"),
                               handVsWeapon0=h0.get("handVsWeapon"), handVsWeapon1=h1.get("handVsWeapon"))
        hand()
        settle(g)
        # 2) hand yaw +-45 -> fire-node heading relative to the camera heading
        a = parse_kv(g.cmd("aim", quiet=True))
        cam_hdg = float(a.get("hdgCam", "nan"))
        out = {}
        for yaw in (45, -45, 0):
            hand(ypr=(yaw, HAND_YPR0[1], 0))
            h = settle(g)
            out[yaw] = round(hdg(vec(h["fireFwd"])) - cam_hdg, 2)
        res["yawVsCamera"] = out
        hand()
        # 3) head yaw 30 with the hand still -> WEAPON world unchanged
        hb = settle(g)
        vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=30,0,0")
        hy = settle(g, 1.0)
        res["headYaw30"] = dict(wDelta=round(norm(sub(vec(hy["wWorld"]), vec(hb["wWorld"]))), 3))
        vremu(HMD0)
        h5 = settle(g, 1.0)
        # 4) P vs fire node at Present
        res["pFire"] = dict(pFire=h5.get("pFire"), dFireDeg=h5.get("dFireDeg"), P=h5.get("P"), fireAtPresent=h5.get("fireAtPresent"),
                            costUs=h5.get("costUs"), avg=h5.get("avg"), max=h5.get("max"))
        # 5) shots: projectile launch point vs the fire node, velocity vs the barrel
        shots = []
        for yaw in (0, 30):
            hand(ypr=(yaw, HAND_YPR0[1], 0))
            hs = settle(g, 0.8)
            a0 = parse_kv(g.cmd("aim", quiet=True))
            n0 = int(a0.get("launches", "0"))
            vremu("/press?dev=right&btn=trigger&ms=120")
            time.sleep(1.2)
            a1 = parse_kv(g.cmd("aim", quiet=True))
            hs2 = gk(g)
            lo = vec(a1.get("launchO"))
            fire = vec(hs2.get("fireAtPresent"))
            shots.append(dict(handYaw=yaw, launches=int(a1.get("launches", "0")) - n0, launchO=a1.get("launchO"), fire=hs2.get("fireAtPresent"),
                              launchToFire=round(norm(sub(lo, fire)), 3) if lo and fire else None,
                              barrelHdg=round(hdg(vec(hs2["fireFwd"])), 2), projVelHdg=a1.get("projVelHdg"), launchHdg=a1.get("launchHdg"),
                              impactHdg=a1.get("impactHdg"), camHdg=a1.get("hdgCam")))
        res["shots"] = shots
        hand()
        settle(g)
        # 6) arms not drawn: the animated first-person arms hang above the camera (the gun no longer drags them along),
        #    so look up 45 deg and capture with the arms hidden, shown (gun arms off), hidden again
        vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,45,0")
        time.sleep(1.2)
        p_hidden = capture(g, "arms-hidden")
        g.cmd("gun arms off", quiet=True)
        time.sleep(0.8)
        res["armsShown"] = g.cmd("gun arms", quiet=True)
        p_shown = capture(g, "arms-shown")
        g.cmd("gun arms on", quiet=True)
        time.sleep(0.8)
        p_hidden2 = capture(g, "arms-hidden2")
        res["armsCull"] = {k: gk(g).get(k) for k in ("A1", "cullAdds", "cullSkips", "armsCulled", "armsCandidates")}
        vremu(HMD0)
        time.sleep(0.8)
        res["captures"] = dict(hidden=p_hidden, shown=p_shown, hidden2=p_hidden2)
        if p_hidden and p_shown and p_hidden2:
            res["capDiff"] = dict(hiddenVsShown=bmp_diff(p_hidden, p_shown), hiddenVsHidden2=bmp_diff(p_hidden, p_hidden2))
        # 7) wrist Pip-Boy quad on the left grip ([Pipboy] offsets, no bone)
        vremu("/press?dev=right&btn=b&ms=150")  # pad B -> Pip-Boy (README: open via pad B)
        time.sleep(2.5)
        q0, grip0, err0 = pipboy_quad(g)
        left(pos=(LHAND0[0] + 0.1, LHAND0[1], LHAND0[2]))
        time.sleep(0.8)
        q1, grip1, err1 = pipboy_quad(g)
        left()
        time.sleep(0.5)
        res["pipboy"] = dict(quad0=q0, grip0=grip0, quad1=q1, grip1=grip1, err=err0 or err1,
                             quadMinusGrip0=[round(x, 4) for x in sub(q0, grip0)] if q0 and grip0 else None,
                             quadMove=round(norm(sub(q1, q0)), 4) if q0 and q1 else None,
                             gripMove=round(norm(sub(grip1, grip0)), 4) if grip0 and grip1 else None,
                             line=g.cmd("pipboy", quiet=True)[:400])
        vremu("/press?dev=right&btn=b&ms=150")  # close
        time.sleep(1.5)
        res["final"] = gk(g)
    finally:
        g._close()
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, time.strftime("verify-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(path, "w"), indent=1, default=str)
    print(json.dumps(res, indent=1, default=str))
    print("saved", path)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == "launch":
        launch()
    elif a[0] == "cmd":
        cmd(a[1:])
    elif a[0] == "verify":
        verify()
    elif a[0] == "quit":
        quit_session()
    else:
        print(__doc__)
