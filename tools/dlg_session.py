"""VR dialogue (Dialogue.h, D1 + D2) test session under SteamVR + vremu. Numbers only.

    python -I dlg_session.py launch [cell]       # wait until no Fallout4 / xmake runs, deploy, launch, coc <cell>
                                                 # (default DmndDugoutInn01), 10mm drawn; leaves it running
    python -I dlg_session.py cmd "dlg" "menus"   # pipe commands to the running (own) session
    python -I dlg_session.py vremu "/pose?dev=hmd&yaw=30"
    python -I dlg_session.py talk [name ...]     # move to + activate the first listed NPC found in the cell; waits for DialogueMenu
    python -I dlg_session.py verify              # D1/D2 checks on an open conversation (prints + work/dlg/<ts>/dlg.json)
    python -I dlg_session.py quit                # qqq the session this script launched (pid file)

The pid of the launched game is kept in work/dlg/session.pid; quit refuses for any other pid.
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
OUT = os.path.join(ROOT, "work", "dlg")
PIDF = os.path.join(OUT, "session.pid")
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
BUILD = os.path.join(ROOT, "plugin", "build", "windows", "x64", "releasedbg")
DIST = os.path.join(ROOT, "plugin", "dist", "Data", "F4SE", "Plugins")
PISTOL_10MM = "00004822"
AMMO_10MM = "0001F276"
HMD0 = "/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0"
HAND0 = (0.2, 1.3, -0.35)
HAND_YPR0 = (0, 39, 0)   # vremu pitch +39 = SteamVR Touch aim pose level
NPC_NAMES = ["Vadim", "Yefim", "Arturo", "Myrna", "Percy", "Solomon", "Moe Cronin", "Codsworth", "Piper", "Danny Sullivan", "Trashcan Carla", "Preston Garvey"]


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def hand(pos=HAND0, ypr=HAND_YPR0, ms=0):
    q = f"/pose?dev=right&pos={pos[0]},{pos[1]},{pos[2]}&ypr={ypr[0]},{ypr[1]},{ypr[2]}"
    if ms:
        q += f"&ms={ms}"
    vremu(q)


def xmake_running():
    return "xmake" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()


def deploy():
    """Copy the current build; append the [Dialogue] section to the deployed ini if it has none (append only)."""
    if game_pid():
        raise SystemExit("Fallout4 running - not deploying")
    if xmake_running():
        raise SystemExit("an xmake build is running - not deploying")
    import fo4test
    fo4test.deploy_plugin(BUILD, need=b"dialogue D-look")  # MO2 mod folder (backup to work/deploy-backup)
    ini = fo4test.effective_ini()
    text = open(ini, encoding="utf-8").read()
    if "[Dialogue]" not in text:
        dist = open(os.path.join(DIST, "fo4flatvr.ini"), encoding="utf-8").read()
        i = dist.find("[Dialogue]")
        if i >= 0:
            j = dist.find("\n[", i + 1)
            sect = dist[i:] if j < 0 else dist[i:j + 1]
            with open(ini, "a", encoding="utf-8") as fh:
                fh.write(("" if text.endswith("\n") else "\n") + "\n" + sect)
    say("deployed dll/pdb")


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


def launch(cell="DmndDugoutInn01"):
    os.makedirs(OUT, exist_ok=True)
    t0 = time.time()
    while True:
        while game_pid() or xmake_running():
            if time.time() - t0 > 3 * 3600:
                raise SystemExit("Fallout4 / xmake still running after 3 h - giving up")
            say("Fallout4 or an xmake build is running (another agent / the user) - waiting")
            time.sleep(20)
        try:
            deploy()
            break
        except SystemExit as e:
            say(f"deploy skipped ({e}) - waiting again")
            time.sleep(20)
    g = Game(runtime_json=STEAMVR_JSON)
    try:
        g.__enter__()
    except SystemExit as e:
        if "already running" in str(e) or "launching Fallout4 right now" in str(e):
            say("someone launched first - waiting again")
            return launch(cell)
        raise
    pid = game_pid()
    open(PIDF, "w").write(str(pid))
    say(f"launched pid {pid}")
    try:
        vremu("/reset")
    except OSError:
        pass
    g.coc(cell)
    time.sleep(4.0)
    vremu(HMD0)
    hand()
    g.cmd(f"console player.additem {PISTOL_10MM} 1")
    g.cmd(f"console player.additem {AMMO_10MM} 200")
    time.sleep(1.0)
    g.cmd(f"aim equip {PISTOL_10MM}")
    time.sleep(3.0)
    print(g.cmd("dlg npcs"))
    g._close()


def cmd(cmds):
    g = attach()
    for c in cmds:
        if c.startswith("sleep "):
            time.sleep(float(c.split()[1]))
            continue
        if c.startswith("vremu "):
            print(f"> {c}\n{vremu(c[6:])}"[:400])
            continue
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


def dlg(g):
    return parse_kv(g.cmd("dlg", quiet=True))


def npcs(g):
    out = []
    for seg in g.cmd("dlg npcs", quiet=True).split("|")[1:]:
        m = re.match(r"\s*([0-9A-F]{8})/([0-9A-F]{8}) '([^']*)' (\d+)", seg)
        if m:
            out.append(dict(ref=m.group(1), base=m.group(2), name=m.group(3), dist=int(m.group(4))))
    return out


def wait(pred, timeout, every=0.2):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = pred()
        if v:
            return v
        time.sleep(every)
    return None


def talk(g, names=None):
    names = names or NPC_NAMES
    lst = npcs(g)
    say(f"{len(lst)} actors in cell: " + ", ".join(f"{a['name']}({a['dist']})" for a in lst[:25]))
    cands = [a for n in names for a in lst if n.lower() in a["name"].lower()]
    cands += [a for a in lst if a["name"] and a not in cands]
    for a in cands[:8]:
        say(f"trying {a['name']} {a['ref']} at {a['dist']}")
        g.cmd(f"console player.moveto {a['ref']}")
        time.sleep(2.0)
        g.cmd(f"console {a['ref']}.activate player")
        if wait(lambda: dlg(g).get("open") == "1", 6):
            say(f"DialogueMenu open with {a['name']}")
            return a
    return None


def bmp(path):
    """32-bit BMP (as the plugin writes: top-down when height < 0) -> (w, h, bytes BGRA)."""
    d = open(path, "rb").read()
    off = int.from_bytes(d[10:14], "little")
    w = int.from_bytes(d[18:22], "little", signed=True)
    h = int.from_bytes(d[22:26], "little", signed=True)
    px = d[off:off + abs(w) * abs(h) * 4]
    if h > 0:  # bottom-up -> top-down
        rows = [px[i * w * 4:(i + 1) * w * 4] for i in range(h)]
        px = b"".join(reversed(rows))
    return w, abs(h), px


def diff(a, b, thr=24, box=None):
    """Pixels whose max channel differs by > thr; counts inside `box` (fractions l,t,r,b) and bbox (fractions)."""
    wa, ha, pa = bmp(a)
    wb, hb, pb = bmp(b)
    assert (wa, ha) == (wb, hb)
    n = inside = 0
    x0, y0, x1, y1 = wa, ha, 0, 0
    step = 2
    for y in range(0, ha, step):
        ro = y * wa * 4
        for x in range(0, wa, step):
            i = ro + x * 4
            if max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2])) > thr:
                n += 1
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
                if box and box[0] * wa <= x < box[2] * wa and box[1] * ha <= y < box[3] * ha:
                    inside += 1
    sc = step * step
    return dict(px=n * sc, pct=round(100.0 * n * sc / (wa * ha), 3), inBox=inside * sc,
                inBoxPct=round(100.0 * inside / n, 1) if n else None,
                bbox=[round(x0 / wa, 3), round(y0 / ha, 3), round((x1 + 1) / wa, 3), round((y1 + 1) / ha, 3)] if n else None)


def fl(d, k):
    try:
        return float(d.get(k))
    except (TypeError, ValueError):
        return None


def triple(s):
    return [float(x) for x in s.split(",")]


def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]


def panel_geom(g):
    s = g.cmd("dlg", quiet=True)
    m = re.search(r"pos ([-\d.]+,[-\d.]+,[-\d.]+) quat ([-\d.]+,[-\d.]+,[-\d.]+,[-\d.]+) size ([\d.]+)x([\d.]+)", s)
    if not m:
        return None
    return dict(pos=triple(m.group(1)), quat=triple(m.group(2)), w=float(m.group(3)), h=float(m.group(4)))


def pointer_aim(g):
    s = g.cmd("pointer", quiet=True)
    m = re.search(r"\| aim pos ([-\d.]+,[-\d.]+,[-\d.]+) dir ([-\d.]+,[-\d.]+,[-\d.]+)", s)
    return (triple(m.group(1)), triple(m.group(2))) if m else (None, None)


def yp(d):
    return math.degrees(math.atan2(-d[0], -d[2])), math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))


def aim_at_uv(g, u, v, hand_ypr):
    """Closed loop on the right controller's vremu yaw/pitch until the aim ray points at panel uv (u, v)."""
    geo = panel_geom(g)
    if not geo:
        return None, hand_ypr
    lx, ly = (u - 0.5) * geo["w"], (0.5 - v) * geo["h"]
    off = qrot(geo["quat"], [lx, ly, 0.0])
    T = [geo["pos"][i] + off[i] for i in range(3)]
    yaw, pit = hand_ypr[0], hand_ypr[1]
    sign = None
    for it in range(8):
        hand(HAND0, (yaw, pit, 0))
        time.sleep(0.35)
        P, D = pointer_aim(g)
        if not P:
            return None, (yaw, pit, 0)
        want = [T[i] - P[i] for i in range(3)]
        n = math.sqrt(sum(x * x for x in want))
        want = [x / n for x in want]
        cy, cp = yp(D)
        wy, wp = yp(want)
        dy, dp = (wy - cy + 180) % 360 - 180, wp - cp
        if sign is None:  # learn vremu's yaw / pitch sign with a 5 deg probe
            hand(HAND0, (yaw + 5, pit + 5, 0))
            time.sleep(0.35)
            _, D2 = pointer_aim(g)
            cy2, cp2 = yp(D2)
            sign = (1 if (cy2 - cy) > 0 else -1, 1 if (cp2 - cp) > 0 else -1)
            continue
        if abs(dy) < 0.15 and abs(dp) < 0.15:
            break
        yaw += sign[0] * dy
        pit += sign[1] * dp
    return parse_kv(g.cmd("pointer", quiet=True)), (yaw, pit, 0)


def verify():
    g = attach()
    ts = time.strftime("%Y%m%d-%H%M%S")
    od = os.path.join(OUT, ts)
    os.makedirs(od, exist_ok=True)
    res = {}

    def rec(k, v):
        res[k] = v
        say(f"{k}: {json.dumps(v)[:600]}")
        json.dump(res, open(os.path.join(od, "dlg.json"), "w"), indent=1)

    vremu(HMD0)
    hand()
    d = dlg(g)
    if d.get("open") != "1":
        a = talk(g)
        rec("talk", a)
        if not a:
            g._close()
            raise SystemExit("no DialogueMenu")
    wait(lambda: dlg(g).get("buttons") == "1", 20)
    time.sleep(1.0)
    rec("menus", g.cmd("menus", quiet=True))
    d = dlg(g)
    rec("state_open", {k: d.get(k) for k in ("open", "menuOpen", "looking", "buttons", "accept", "exclusive", "otherFlat", "calls", "forced",
                                             "buttonEdges", "hidden", "culls", "haptics", "submits", "h2Skips", "layerActive")})
    # D-cap: capture stats
    g.cmd(f"dlg cap {od}\\cap.bmp", quiet=True)
    time.sleep(1.5)
    rec("dlgcap", g.cmd("dlg", quiet=True).split("last dlgcap")[-1])
    # D-look: hand / head sweeps
    t0 = dlg(g)
    seq = []
    for hy in (90, -90, 0):
        hand(HAND0, (hy, 39, 0))
        time.sleep(1.0)
        x = dlg(g)
        seq.append(("hand", hy, x.get("looking"), x.get("notLookingTicks")))
    for my in (60, -60, 0):
        vremu(f"/pose?dev=hmd&yaw={my}&ms=150")
        time.sleep(1.2)
        x = dlg(g)
        seq.append(("hmd", my, x.get("looking"), x.get("notLookingTicks")))
    hand(HAND0, (-90, -40, 0))  # hand lowered and away
    vremu("/pose?dev=hmd&yaw=60&ms=150")
    time.sleep(1.5)
    x = dlg(g)
    seq.append(("hand lowered + hmd 60", None, x.get("looking"), x.get("notLookingTicks")))
    t1 = dlg(g)
    rec("look_on", dict(seq=seq, notLookingDelta=int(t1["notLookingTicks"]) - int(t0["notLookingTicks"]),
                        lookOffEdgesDelta=int(t1["lookOffEdges"]) - int(t0["lookOffEdges"]), forced=t1.get("forced"), calls=t1.get("calls")))
    g.cmd("dlg look off", quiet=True)
    time.sleep(1.5)
    x = dlg(g)
    rec("look_off_hand_away", dict(looking=x.get("looking"), lookOffEdges=x.get("lookOffEdges"), canPick=x.get("canPick")))
    g.cmd("dlg look on", quiet=True)
    vremu(HMD0)
    hand()
    time.sleep(1.5)
    x = dlg(g)
    rec("look_back_on", dict(looking=x.get("looking")))
    # D-panel: lag
    vremu(HMD0)
    time.sleep(1.5)
    lag = []
    for yaw in (10, 40):
        vremu(f"/pose?dev=hmd&yaw={yaw}&ms=60")
        ts0 = time.time()
        for tt in (0.1, 0.3, 0.6, 1.0, 1.5):
            time.sleep(max(0.0, tt - (time.time() - ts0)))
            x = dlg(g)
            lag.append((yaw, round(time.time() - ts0, 2), fl(x, "panelYaw"), fl(x, "headYaw"), fl(x, "err")))
    vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=40,30,0&ms=60")
    time.sleep(1.0)
    x = dlg(g)
    pq = re.search(r"quat ([-\d.]+,[-\d.]+,[-\d.]+,[-\d.]+)", g.cmd("dlg", quiet=True))
    rec("lag", dict(samples=lag, pitch30=dict(headPitch=fl(x, "headPitch"), quat=pq.group(1) if pq else None)))
    vremu(HMD0)
    time.sleep(1.5)
    # world image with / without the options
    g.cmd(f"capture {od}\\world_on1.bmp", quiet=True)
    time.sleep(0.3)
    g.cmd(f"capture {od}\\world_on2.bmp", quiet=True)
    time.sleep(0.3)
    g.cmd("dlg panel off", quiet=True)
    time.sleep(0.6)
    x = dlg(g)
    g.cmd(f"capture {od}\\world_off.bmp", quiet=True)
    time.sleep(0.6)
    g.cmd("dlg panel on", quiet=True)
    time.sleep(0.6)
    rec("world", dict(panelOffLayerActive=x.get("layerActive"), noise=diff(os.path.join(od, "world_on1.bmp"), os.path.join(od, "world_on2.bmp")),
                      offVsOn=diff(os.path.join(od, "world_on2.bmp"), os.path.join(od, "world_off.bmp"))))
    g._close()
    return res, od


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "launch":
        launch(*(a[1:2] or []))
    elif a[0] == "cmd":
        cmd(a[1:])
    elif a[0] == "vremu":
        print(vremu(a[1]))
    elif a[0] == "talk":
        g = attach()
        print(talk(g, a[1:] or None))
        g._close()
    elif a[0] == "verify":
        verify()
    elif a[0] == "aim":  # aim u v: point the right controller at panel uv
        g = attach()
        print(aim_at_uv(g, float(a[1]), float(a[2]), HAND_YPR0))
        g._close()
    elif a[0] == "quit":
        quit_session()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
