"""Flat menus / UI layers test session (work/handoff/ui-menus.md) under SteamVR + vremu. Save vrtest_sanctuary, never QASmoke.

    python ui_session.py launch          # refuse if Fallout4 / xmake runs; deploy build; launch; load vrtest_sanctuary
    python ui_session.py cmd "status" "menupanel"
    python ui_session.py menus           # pause menu / favourites / console (cinema) with the HMD turned away; numbers -> work/ui/menus-<ts>.json
    python ui_session.py pointer         # pointer on the pause-menu panel: rows, button bar clicks
    python ui_session.py decal           # HUD layer content before / after a pause menu
    python ui_session.py vats            # VATS with the HMD turned away
    python ui_session.py pipboy          # wrist + floating Pip-Boy: world view, pose, pause counters
    python ui_session.py quit            # qqq the session this script launched (pid file); never another game

SteamVR is never closed or restarted here.
"""
import json
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402
import gun_session as gs  # noqa: E402  (vremu, deploy, hand helpers)

ROOT = gs.ROOT
OUT = os.path.join(ROOT, "work", "ui")
CAP = r"D:\Fallout4ToVR-overflow\ui"  # bulk captures (G: is nearly full)
gs.OUT = CAP  # gs.capture writes back-buffer BMPs here
PIDF = os.path.join(OUT, "session.pid")
LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\fo4flatvr.log")
HAND_POS = (0.2, 1.3, -0.35)


def hmd(yaw=0.0, pitch=0.0, pos=(0, 1.7, 0)):
    gs.vremu(f"/pose?dev=hmd&pos={pos[0]},{pos[1]},{pos[2]}&ypr={yaw},{pitch},0")


def launch():
    os.makedirs(OUT, exist_ok=True)
    if game_pid() or gs.xmake_running():
        raise SystemExit("Fallout4 or an xmake build is running - not launching")
    import fo4test
    # dll + pdb only (no ini changes) into the fo4flatvr MO2 mod folder, under the cross-agent launch lock (deploy_files
    # takes it; hold / running game / xmake refuse). FRIK-flat (its own MO2 mod) is left as it is.
    fo4test.deploy_plugin(gs.BUILD)
    g = Game(runtime_json=gs.STEAMVR_JSON)
    g.__enter__()
    pid = game_pid()
    open(PIDF, "w").write(str(pid))
    say(f"launched pid {pid}")
    g.cmd(f"console load {gs.SAVE}")
    time.sleep(2.0)
    g._wait(f"in-world ({gs.SAVE})", 90, lambda: g.ready().get("ingame") == "1")
    time.sleep(4.0)
    hmd()
    gs.hand()
    gs.left()
    time.sleep(1.0)
    print(g.cmd("status"))
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
        import subprocess
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    try:
        gs.vremu("/stopall")
        gs.vremu("/reset")
    except OSError:
        pass
    os.remove(PIDF)
    say("session closed")


# ---------------------------------------------------------------------------------------------------------------------
def log_mark():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def log_since(mark):
    with open(LOG, "rb") as fh:
        fh.seek(mark)
        return fh.read().decode("utf-8", "replace")


def st(g):
    return parse_kv(g.cmd("status", quiet=True))


def menus(g):
    return g.cmd("menus", quiet=True)


def open_menus(g):
    return re.findall(r"\| (\w+) flags", menus(g))


def anchors_in(text):
    """'menu quad anchored (why: 'key'): head yaw Y deg pos x,y,z -> quad centre x,y,z WxH m' lines -> dicts with the
    quad direction measured from the head position (yaw, OpenXR: atan2(-dx, -dz) = + left)."""
    out = []
    for m in re.finditer(r"menu quad anchored \(([^:]*): '([^']*)'\): head yaw (\S+) deg pos (\S+),(\S+),(\S+) -> quad centre (\S+),(\S+),(\S+) (\S+)x(\S+) m", text):
        why, key = m.group(1), m.group(2)
        hy = float(m.group(3))
        hp = [float(m.group(i)) for i in (4, 5, 6)]
        qc = [float(m.group(i)) for i in (7, 8, 9)]
        d = [qc[i] - hp[i] for i in range(3)]
        quad_heading = math.degrees(math.atan2(d[0], -d[2]))  # same convention as the logged head yaw (+ = right)
        out.append(dict(why=why, key=key, headYaw=hy, quadHeading=round(quad_heading, 2),
                        err=round((quad_heading - hy + 180) % 360 - 180, 2), dist=round(math.hypot(d[0], d[2]), 3),
                        dy=round(d[1], 3), size=(float(m.group(10)), float(m.group(11)))))
    return out


def pointer(g):
    return g.cmd("pointer", quiet=True)


def ptr_uv(g):
    s = pointer(g)
    k = parse_kv(s)
    on = k.get("onQuad") == "1"
    return on, float(k.get("u", "nan")), float(k.get("v", "nan")), s


def aim_at(g, tu, tv, pos=HAND_POS, start=(0.0, 39.0), iters=12):
    """Closed loop: right controller yaw/pitch (vremu ypr) until the pointer hits quad uv (tu, tv). Returns (u, v, ypr)."""
    y, p = start

    def hit(yy, pp):
        gs.hand(pos, (yy, pp, 0))
        time.sleep(0.25)
        on, u, v, _ = ptr_uv(g)
        return (u, v) if on else None

    h = hit(y, p)
    # coarse search if the start misses
    if h is None:
        for dy in (0, -10, 10, -20, 20, -30, 30, -40, 40, -50, 50, -60, 60, -75, 75, -90, 90, 120, -120, 150, -150, 180):
            for dp in (0, -8, 8, -16, 16):
                h = hit(y + dy, p + dp)
                if h:
                    y, p = y + dy, p + dp
                    break
            if h:
                break
    if h is None:
        return None
    for _ in range(iters):
        u, v = h
        if abs(u - tu) < 0.004 and abs(v - tv) < 0.004:
            break
        e = 1.0
        hu = hit(y + e, p)
        hv = hit(y, p + e)
        if not hu or not hv:
            e = -1.0
            hu = hit(y + e, p)
            hv = hit(y, p + e)
            if not hu or not hv:
                break
        j = [[(hu[0] - u) / e, (hv[0] - u) / e], [(hu[1] - v) / e, (hv[1] - v) / e]]
        det = j[0][0] * j[1][1] - j[0][1] * j[1][0]
        if abs(det) < 1e-9:
            break
        du, dv = tu - u, tv - v
        sy = (j[1][1] * du - j[0][1] * dv) / det
        sp = (-j[1][0] * du + j[0][0] * dv) / det
        y, p = y + max(-15, min(15, sy)), p + max(-15, min(15, sp))
        h = hit(y, p)
        if h is None:
            break
    return (h[0], h[1], (y, p)) if h else None


def click(hold=0.25):
    gs.vremu(f"/input?dev=right&trigger=1&ms={int(hold * 1000)}")
    time.sleep(hold + 0.6)


def hudcap(g, name):
    os.makedirs(CAP, exist_ok=True)
    path = os.path.join(CAP, name + ".bmp").replace("\\", "/")
    mark = log_mark()
    g.cmd("hudcap " + path, quiet=True)
    t0 = time.time()
    while time.time() - t0 < 6:
        t = log_since(mark)
        m = re.search(r"hudcap \S+\.bmp:[^\n]*", t)
        if m:
            return m.group(0)
        time.sleep(0.2)
    return None


def save(name, res):
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, f"{name}-{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(p, "w") as fh:
        json.dump(res, fh, indent=1)
    say(f"-> {p}")
    print(json.dumps(res, indent=1)[:6000])


def wait_menu(g, name, present=True, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if (name in open_menus(g)) == present:
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------------------------------------------------
def test_menus():
    """User repro: HMD turned away from the character heading, then open a menu."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        for yaw in (0.0, 90.0, -45.0):
            hmd(yaw)
            time.sleep(2.5)  # body-follow settles
            s0 = st(g)
            mark = log_mark()
            # pause menu (FO4VR: Y tap -> pause)
            g.cmd("uimsg PauseMenu show", quiet=True)
            ok = wait_menu(g, "PauseMenu")
            time.sleep(1.5)
            s1 = st(g)
            mp = g.cmd("menupanel", quiet=True)
            cam1 = g.cmd("cam", quiet=True)
            # head turned while the menu is open: the quad must stay where it was, the world view must keep tracking
            hmd(yaw + 30.0)
            time.sleep(1.0)
            cam2 = g.cmd("cam", quiet=True)
            q2 = parse_kv(pointer(g).split("| quad ")[1]) if "| quad " in pointer(g) else {}
            quadline = pointer(g).split("| quad ")[1].split(" |")[0] if "| quad " in pointer(g) else ""
            s2 = st(g)
            mp2 = g.cmd("menupanel", quiet=True)
            mn = menus(g)
            g.cmd("uimsg PauseMenu hide", quiet=True)
            wait_menu(g, "PauseMenu", False)
            hmd(yaw)
            time.sleep(1.5)
            s3 = st(g)
            res[f"pause hmdYaw={yaw}"] = dict(opened=ok, before=dict(mode=s0.get("mode"), layers=s0.get("layers")),
                                              open=dict(viewMenu=s1.get("viewMenu"), panel=s1.get("menuPanel"), layers=s1.get("layers"),
                                                        anchors=s1.get("quadAnchors"), menupanel=mp),
                                              headTurned30=dict(layers=s2.get("layers"), menupanel=mp2, quad=quadline),
                                              cam=[cam1[:400], cam2[:400]], menus=mn[:600],
                                              closed=dict(viewMenu=s3.get("viewMenu"), panel=s3.get("menuPanel"), layers=s3.get("layers")),
                                              anchorsLogged=anchors_in(log_since(mark)))
        # a flat-fallback (cinema) menu: the console, HMD turned 120 deg
        hmd(120.0)
        time.sleep(2.5)
        mark = log_mark()
        g.cmd("uimsg Console show", quiet=True)
        ok = wait_menu(g, "Console")
        time.sleep(1.5)
        s1 = st(g)
        quadline = pointer(g).split("| quad ")[1].split(" |")[0] if "| quad " in pointer(g) else ""
        g.cmd("uimsg Console hide", quiet=True)
        wait_menu(g, "Console", False)
        time.sleep(1.0)
        s2 = st(g)
        res["console hmdYaw=120"] = dict(opened=ok, open=dict(viewMenu=s1.get("viewMenu"), panel=s1.get("menuPanel"), layers=s1.get("layers"),
                                                             backdropFrames=s1.get("backdropFrames"), quad=quadline),
                                         closed=dict(viewMenu=s2.get("viewMenu"), layers=s2.get("layers"), backdropFrames=s2.get("backdropFrames")),
                                         anchorsLogged=anchors_in(log_since(mark)))
        hmd(0.0)
    finally:
        g._close()
    save("menus", res)


def test_pointer():
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        hmd(0.0)
        gs.left()
        time.sleep(1.0)
        g.cmd("uimsg PauseMenu show", quiet=True)
        res["opened"] = wait_menu(g, "PauseMenu")
        time.sleep(1.5)
        res["hudcap"] = hudcap(g, "pause_panel")
        res["menus"] = menus(g)[:800]
        # sweep v down the centre column and the bottom band; record where the pointer lands and what the target is
        pts = {}
        for (u, v) in ((0.5, 0.5), (0.3, 0.45), (0.3, 0.6), (0.5, 0.9), (0.2, 0.93), (0.5, 0.95), (0.8, 0.93)):
            r = aim_at(g, u, v)
            pts[f"{u},{v}"] = dict(hit=r, pointer=pointer(g)[-700:])
        res["aim"] = pts
        # button bar: click along the bottom band; the pause menu should react (BACK closes it)
        clicks = []
        for u in (0.15, 0.3, 0.45, 0.55, 0.7, 0.85):
            if "PauseMenu" not in open_menus(g):
                g.cmd("uimsg PauseMenu show", quiet=True)
                wait_menu(g, "PauseMenu")
                time.sleep(1.2)
            r = aim_at(g, u, 0.94)
            before = open_menus(g)
            click()
            time.sleep(0.8)
            after = open_menus(g)
            clicks.append(dict(u=u, hit=r, before=before, after=after, game=parse_kv(pointer(g).split("| game ")[-1]) if "| game " in pointer(g) else None,
                               bar=pointer(g).split("buttonBar ")[-1][:200]))
        res["barClicks"] = clicks
        if "PauseMenu" in open_menus(g):
            g.cmd("uimsg PauseMenu hide", quiet=True)
        gs.hand()
    finally:
        g._close()
    save("pointer", res)


def bar_dump(g):
    return g.cmd("pointer bar", quiet=True)


def parse_hints(dump):
    hs = []
    for m in re.finditer(r"#(\d+) '([^']*)' text='([^']*)' vis=(\S+) enabled=(\S+) x=(\S+) y=(\S+) w=(\S+) h=(\S+) mouseEnabled=(\S+) hit=(\d)", dump):
        hs.append(dict(i=int(m.group(1)), name=m.group(2), text=m.group(3), vis=m.group(4), enabled=m.group(5), x=float(m.group(6)),
                       y=float(m.group(7)), w=float(m.group(8)), h=float(m.group(9)), mouseEnabled=m.group(10), hit=m.group(11)))
    return hs


def stage_mouse(dump):
    m = re.search(r"stage mouse (\S+),(\S+) bar x=(\S+) y=(\S+)", dump)
    return (float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4))) if m else None


def test_bar():
    """Button-bar hints: dump, map stage coords <-> quad uv from two pointer positions, click each hint's centre."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        hmd(0.0)
        gs.left()
        time.sleep(1.0)
        if "PauseMenu" not in open_menus(g):
            g.cmd("uimsg PauseMenu show", quiet=True)
            wait_menu(g, "PauseMenu")
            time.sleep(1.5)
        cal = []
        for (u, v) in ((0.3, 0.3), (0.7, 0.8)):
            r = aim_at(g, u, v)
            time.sleep(0.4)
            d = bar_dump(g)
            cal.append((r[0], r[1], stage_mouse(d)))
        res["calibration"] = cal
        d = bar_dump(g)
        res["dump"] = d[:3000]
        (u0, v0, s0), (u1, v1, s1) = cal
        if not s0 or not s1 or s1[0] == s0[0] or s1[1] == s0[1]:
            res["error"] = "no stage mouse mapping"
            return
        # stage x = a*u + b ; stage y = c*v + d  (the bar clip's own x/y are added to child x/y)
        a = (s1[0] - s0[0]) / (u1 - u0)
        b = s0[0] - a * u0
        c = (s1[1] - s0[1]) / (v1 - v0)
        dd = s0[1] - c * v0
        bx, by = s0[2], s0[3]
        hints = [h for h in parse_hints(d) if h["vis"] in ("1", "true") and h["w"] > 0]
        res["hints"] = hints
        clicks = []
        for h in hints:
            if "PauseMenu" not in open_menus(g):
                g.cmd("uimsg PauseMenu show", quiet=True)
                wait_menu(g, "PauseMenu")
                time.sleep(1.2)
            sx, sy = bx + h["x"] + h["w"] / 2, by + h["y"] + h["h"] / 2
            tu, tv = (sx - b) / a, (sy - dd) / c
            r = aim_at(g, tu, tv)
            time.sleep(0.3)
            pre = parse_hints(bar_dump(g))
            hit_idx = [p["i"] for p in pre if p["hit"] == "1"]
            before = open_menus(g)
            mark = log_mark()
            click()
            time.sleep(1.0)
            after = open_menus(g)
            clicks.append(dict(hint=h["text"], target_uv=(round(tu, 4), round(tv, 4)), hit=r, hitIdx=hit_idx, before=before, after=after,
                               log=[l for l in log_since(mark).splitlines() if "button bar" in l][:3], dump=bar_dump(g)[:200]))
        res["clicks"] = clicks
        if "PauseMenu" in open_menus(g):
            g.cmd("uimsg PauseMenu hide", quiet=True)
        gs.hand()
    finally:
        g._close()
        save("bar", res)


def stereo_submits():
    """Last 'stereo: submitted double-wide halves as eyes (N frames)' count in the log (None = never)."""
    try:
        t = open(LOG, "rb").read().decode("utf-8", "replace")
    except OSError:
        return None
    m = re.findall(r"stereo: submitted double-wide halves as eyes \((\d+) frames\)", t)
    return int(m[-1]) if m else None


def test_pausebg():
    """World behind the pause panel: does the image content follow the head (world-locked) or stay screen-fixed?
    Back-buffer captures at the same HMD pose and after +30 deg, with the panel on; camera report each time."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        hmd(0.0)
        time.sleep(1.5)
        a0 = gs.capture(g, "pbg_world_a")
        hmd(30.0)
        time.sleep(1.2)
        a1 = gs.capture(g, "pbg_world_b30")
        hmd(0.0)
        time.sleep(1.2)
        res["no menu: diff yaw0 vs yaw30"] = gs.bmp_diff(a0, a1)
        g.cmd("uimsg PauseMenu show", quiet=True)
        wait_menu(g, "PauseMenu")
        time.sleep(2.0)
        res["status"] = g.cmd("status", quiet=True)[-160:]
        res["menupanel"] = g.cmd("menupanel", quiet=True)[-120:]
        c0 = gs.capture(g, "pbg_pause_a")
        cam0 = g.cmd("cam", quiet=True)
        time.sleep(0.6)
        c1 = gs.capture(g, "pbg_pause_a2")
        hmd(30.0)
        time.sleep(1.2)
        c2 = gs.capture(g, "pbg_pause_b30")
        cam2 = g.cmd("cam", quiet=True)
        res["pause: diff same pose (noise)"] = gs.bmp_diff(c0, c1)
        res["pause: diff yaw0 vs yaw30"] = gs.bmp_diff(c0, c2)
        res["cam"] = [re.search(r"fp update=\d+", cam0).group(0), re.search(r"fp update=\d+", cam2).group(0),
                      re.search(r"NiCamera worldRot=(\[[^\]]*\]?)", cam0).group(1), re.search(r"NiCamera worldRot=(\[[^\]]*\]?)", cam2).group(1)]
        res["stereoSubmits"] = stereo_submits()
        g.cmd("uimsg PauseMenu hide", quiet=True)
        hmd(0.0)
    finally:
        g._close()
    save("pausebg", res)


def hud_cov(line):
    m = re.search(r"3E: \S+ fmt \d+ fresh=(\d) alpha>0 (\d+) \(([\d.]+)%\)[^;]*?bbox (\[[^\]]*\])", line or "")
    return (int(m.group(2)), m.group(4)) if m else None


def test_hudfade():
    """HUD-layer pixel count over time after a HUD message (item added) and after sneaking (HIDDEN), with
    `hud clear` off and on."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        hmd(0.0)
        time.sleep(1.0)
        for mode in ("off", "on"):
            g.cmd(f"hud clear {mode}", quiet=True)
            time.sleep(1.0)
            series = [("base", hud_cov(hudcap(g, f"hf_{mode}_base")))]
            g.cmd("console player.additem 0001F276 1", quiet=True)  # "10mm Round added" message
            t0 = time.time()
            for i in range(7):
                time.sleep(1.5)
                series.append((round(time.time() - t0, 1), hud_cov(hudcap(g, f"hf_{mode}_{i}"))))
            res[f"item message, clear {mode}"] = series
            # sneak on/off (FO4VR: primary stick down = sneak)
            gs.vremu("/input?dev=right&joy=0,-1&ms=300")
            time.sleep(2.5)
            sneak = hud_cov(hudcap(g, f"hf_{mode}_sneak"))
            gs.vremu("/input?dev=right&joy=0,-1&ms=300")
            seq = []
            t0 = time.time()
            for i in range(5):
                time.sleep(1.5)
                seq.append((round(time.time() - t0, 1), hud_cov(hudcap(g, f"hf_{mode}_unsneak{i}"))))
            res[f"sneak, clear {mode}"] = dict(sneaking=sneak, after=seq)
        g.cmd("hud clear on", quiet=True)
    finally:
        g._close()
    save("hudfade", res)


def test_decal():
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        hmd(0.0)
        time.sleep(1.0)
        res["hud_before"] = hudcap(g, "decal_before")
        res["hud_status_before"] = g.cmd("hud", quiet=True)[:500]
        bb0 = gs.capture(g, "decal_bb_before")
        bb0b = gs.capture(g, "decal_bb_before2")
        res["bb noise (before vs before2)"] = gs.bmp_diff(bb0, bb0b) if bb0 and bb0b else None
        for i in range(2):
            g.cmd("uimsg PauseMenu show", quiet=True)
            wait_menu(g, "PauseMenu")
            time.sleep(1.5)
            res[f"hud_during_{i}"] = hudcap(g, f"decal_during_{i}")
            g.cmd("uimsg PauseMenu hide", quiet=True)
            wait_menu(g, "PauseMenu", False)
            time.sleep(0.2)
            res[f"hud_after_{i}_0.2s"] = hudcap(g, f"decal_after_{i}_a")
            time.sleep(2.0)
            res[f"hud_after_{i}_2s"] = hudcap(g, f"decal_after_{i}_b")
            res[f"status_after_{i}"] = g.cmd("status", quiet=True)[-200:]
            bb = gs.capture(g, f"decal_bb_after_{i}")
            res[f"bb diff before vs after {i}"] = gs.bmp_diff(bb0, bb) if bb0 and bb else None
        # the same through the controller (FO4VR Y tap = pause), close with B (right grip = back in menus)
        gs.vremu("/input?dev=left&y=1&ms=150")
        time.sleep(2.0)
        res["pad_open_menus"] = open_menus(g)
        res["hud_during_pad"] = hudcap(g, "decal_during_pad")
        gs.vremu("/input?dev=right&grip=1&ms=200")
        time.sleep(2.5)
        res["pad_close_menus"] = open_menus(g)
        res["hud_after_pad"] = hudcap(g, "decal_after_pad")
    finally:
        g._close()
    save("decal", res)


def test_vats():
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        # a target in front (VATS needs one) and a drawn 10mm
        g.cmd("console player.additem 00004822 1", quiet=True)
        g.cmd("console player.additem 0001F276 100", quiet=True)
        g.cmd("aim equip0 00004822", quiet=True)
        time.sleep(1.5)
        gs.vremu("/input?dev=right&trigger=1&ms=200")
        time.sleep(2.0)
        hmd(70.0)
        time.sleep(2.5)  # body follows the head
        g.cmd("console player.placeatme 0001D162", quiet=True)
        time.sleep(3.0)
        mark = log_mark()
        gs.vremu("/input?dev=right&b=1&ms=400")
        time.sleep(1.5)
        res["menus"] = open_menus(g)
        s1 = st(g)
        res["open"] = dict(viewMenu=s1.get("viewMenu"), panel=s1.get("menuPanel"), layers=s1.get("layers"), backdropFrames=s1.get("backdropFrames"),
                           quad=pointer(g).split("| quad ")[1].split(" |")[0] if "| quad " in pointer(g) else "")
        time.sleep(1.0)
        res["open+1s"] = dict(backdropFrames=st(g).get("backdropFrames"))
        res["cam"] = g.cmd("cam", quiet=True)[:300]
        gs.vremu("/input?dev=right&b=1&ms=200")
        time.sleep(2.0)
        res["after"] = dict(menus=open_menus(g), status=g.cmd("status", quiet=True)[-200:])
        res["anchorsLogged"] = anchors_in(log_since(mark))
        res["log"] = [l for l in log_since(mark).splitlines() if "camera state" in l or "view menu" in l or "menu panel" in l][:20]
        hmd(0.0)
    finally:
        g._close()
    save("vats", res)


def test_pipboy():
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        hmd(0.0)
        time.sleep(1.5)
        for mode in ("wrist", "floating"):
            g.cmd("pipboy floating " + ("on" if mode == "floating" else "off"), quiet=True)
            hmd(-60.0 if mode == "floating" else 0.0)
            time.sleep(2.0)
            s0 = st(g)
            cam0 = g.cmd("cam", quiet=True)
            mark = log_mark()
            gs.vremu("/input?dev=left&trigger=1&ms=200")
            ok = wait_menu(g, "PipboyMenu", True, 6.0)
            time.sleep(2.0)
            s1 = st(g)
            mn = menus(g)
            pip = g.cmd("pipboy", quiet=True)
            hmd((-60.0 if mode == "floating" else 0.0) + 25.0)
            time.sleep(1.0)
            s2 = st(g)
            cam2 = g.cmd("cam", quiet=True)
            pip2 = g.cmd("pipboy", quiet=True)
            gs.vremu("/input?dev=right&grip=1&ms=200")  # FO4VR: grip closes the Pip-Boy
            time.sleep(2.5)
            s3 = st(g)
            res[mode] = dict(opened=ok, before=dict(mode=s0.get("mode"), layers=s0.get("layers"), fov=s0.get("fov")),
                             open=dict(viewMenu=s1.get("viewMenu"), panel=s1.get("menuPanel"), mode=s1.get("mode"), layers=s1.get("layers"),
                                       fov=s1.get("fov")),
                             headTurned=dict(layers=s2.get("layers"), fov=s2.get("fov")),
                             ui=mn[:300], pipboy=pip[:900], pipboyTurned=pip2[:900], cam=[cam0[:300], cam2[:300]],
                             closed=dict(menus=open_menus(g), layers=s3.get("layers")),
                             log=[l for l in log_since(mark).splitlines() if "pipboy" in l.lower() or "view menu" in l][:12])
        g.cmd("pipboy floating off", quiet=True)
        hmd(0.0)
    finally:
        g._close()
    save("pipboy", res)


def _bmp(p):
    import struct
    d = open(p, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    bp = struct.unpack_from("<H", d, 28)[0] // 8
    stride = (w * bp + 3) & ~3
    return d, off, w, h, bp, stride


def _px(b, x, y):
    d, off, w, h, bp, stride = b
    row = y if h < 0 else abs(h) - 1 - y
    i = off + row * stride + x * bp
    return d[i], d[i + 1], d[i + 2]  # B, G, R


def np_diff(a, b, crop=None, step=4):
    """BMP difference on a step x step grid: % of sampled pixels with sum |dRGB| > 24, bbox; crop = (x0, y0, x1, y1)."""
    if not a or not b or not os.path.exists(a) or not os.path.exists(b):
        return None
    A, B = _bmp(a), _bmp(b)
    if (A[2], abs(A[3])) != (B[2], abs(B[3])):
        return dict(size=[(A[2], A[3]), (B[2], B[3])])
    w, h = A[2], abs(A[3])
    x0, y0, x1, y1 = crop or (0, 0, w, h)
    n = tot = 0
    bx0, by0, bx1, by1 = w, h, -1, -1
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            p, q = _px(A, x, y), _px(B, x, y)
            tot += 1
            if abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]) > 24:
                n += 1
                bx0, by0, bx1, by1 = min(bx0, x), min(by0, y), max(bx1, x), max(by1, y)
    out = dict(pct=round(100.0 * n / max(tot, 1), 3), w=w, h=h)
    if n:
        out["bbox"] = [bx0, by0, bx1, by1]
    return out


def thumb(src, dst, scale=8):
    """Small PNG preview of a BMP (every scale-th pixel); alpha dropped."""
    import struct
    import zlib
    B = _bmp(src)
    w, h = B[2], abs(B[3])
    rows = []
    for y in range(0, h, scale):
        r = bytearray(b"\x00")
        for x in range(0, w, scale):
            b, g, rr = _px(B, x, y)
            r += bytes((rr, g, b))
        rows.append(bytes(r))
    tw, th = len(range(0, w, scale)), len(rows)

    def chunk(k, v):
        return struct.pack(">I", len(v)) + k + v + struct.pack(">I", zlib.crc32(k + v) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", tw, th, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    return dst

def test_pipzoom():
    """Pip-Boy regression ("half zoom / warp"): back buffer + eye images with the wrist Pip-Boy closed vs open at the same
    HMD pose, status/cam/pipboy readouts, layers. Thumbnails -> work/ui/pz_*.png."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        hmd(0.0)
        time.sleep(2.0)
        res["stereo"] = g.cmd("stereo", quiet=True)[:200]
        res["closed"] = dict(status=g.cmd("status", quiet=True)[-330:], cam=g.cmd("cam", quiet=True)[:400])
        b0 = gs.capture(g, "pz_closed")
        g.cmd("stereoeyecap " + os.path.join(CAP, "pz_closed").replace("\\", "/"), quiet=True)
        time.sleep(1.5)
        mark = log_mark()
        gs.vremu("/input?dev=left&trigger=1&ms=200")
        res["opened"] = wait_menu(g, "PipboyMenu", True, 6.0)
        time.sleep(2.5)
        res["open"] = dict(status=g.cmd("status", quiet=True)[-330:], cam=g.cmd("cam", quiet=True)[:400], pipboy=g.cmd("pipboy", quiet=True)[:700],
                           menus=open_menus(g), hud=g.cmd("hud", quiet=True)[:300])
        b1 = gs.capture(g, "pz_open")
        g.cmd("stereoeyecap " + os.path.join(CAP, "pz_open").replace("\\", "/"), quiet=True)
        time.sleep(1.5)
        res["bb closed vs open"] = np_diff(b0, b1)
        for e in (0, 1):
            res[f"eye{e} closed vs open"] = np_diff(os.path.join(CAP, f"pz_closed_eye{e}.bmp"), os.path.join(CAP, f"pz_open_eye{e}.bmp"))
        for n in ("pz_closed", "pz_open"):
            p = os.path.join(CAP, n + ".bmp")
            if os.path.exists(p):
                thumb(p, os.path.join(OUT, n + ".png"))
            p = os.path.join(CAP, n + "_eye0.bmp")
            if os.path.exists(p):
                thumb(p, os.path.join(OUT, n + "_eye0.png"), 6)
        gs.vremu("/input?dev=right&grip=1&ms=200")
        time.sleep(2.5)
        res["closed after"] = dict(menus=open_menus(g), status=g.cmd("status", quiet=True)[-200:])
        res["log"] = [l[:220] for l in log_since(mark).splitlines() if any(k in l for k in ("pipboy", "view menu", "menu ", "FOV", "camdir"))][:20]
    finally:
        g._close()
    save("pipzoom", res)


def test_vatswarp():
    """VATS zoom/warp report: eye images + back buffer before VATS and in VATS at the same HMD pose, the overlay copy
    (hudcap), open menus (VignetteMenu?), imod stop log. Thumbnails -> work/ui/vw_*.png."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        hmd(0.0)
        g.cmd("console player.additem 00004822 1", quiet=True)
        g.cmd("console player.additem 0001F276 100", quiet=True)
        g.cmd("aim equip0 00004822", quiet=True)
        time.sleep(1.5)
        gs.vremu("/input?dev=right&trigger=1&ms=200")
        time.sleep(2.0)
        g.cmd("console player.placeatme 0001D162", quiet=True)
        time.sleep(3.0)
        stem0 = os.path.join(CAP, "vw_before").replace("\\", "/")
        g.cmd("stereoeyecap " + stem0, quiet=True)
        time.sleep(2.0)
        mark = log_mark()
        gs.vremu("/input?dev=right&b=1&ms=400")
        time.sleep(2.5)
        res["menus"] = menus(g)[:900]
        res["status"] = g.cmd("status", quiet=True)[-260:]
        res["hudcap"] = (hudcap(g, "vw_in") or "")[-700:]
        stem1 = os.path.join(CAP, "vw_in").replace("\\", "/")
        g.cmd("stereoeyecap " + stem1, quiet=True)
        time.sleep(2.0)
        for e in (0, 1):
            res[f"eye{e} before vs in VATS"] = np_diff(f"{stem0}_eye{e}.bmp", f"{stem1}_eye{e}.bmp")
        for n, sc in (("vw_before_eye0", 6), ("vw_in_eye0", 6), ("vw_in_menu3e", 8)):
            p = os.path.join(CAP, n + ".bmp")
            if os.path.exists(p):
                thumb(p, os.path.join(OUT, n + ".png"), sc)
        gs.vremu("/input?dev=right&b=1&ms=200")
        time.sleep(2.0)
        res["after"] = dict(menus=open_menus(g))
        res["log"] = [l[:230] for l in log_since(mark).splitlines() if any(k in l for k in ("camdir gate", "overlay", "FOV", "Imod", "imod"))][:20]
    finally:
        g._close()
    save("vatswarp", res)


def place_bench(g, base, words=("Workbench", "Weapons", "Armor")):
    """player.placeatme <base> ahead, then sweep the right hand ray to read the new ref id from the 'aim A1: pick ref' log."""
    mark = log_mark()
    g.cmd(f"console player.placeatme {base} 1 160 0", quiet=True)
    time.sleep(2.0)
    for pitch in (-20, -30, -40, -50, -15, -60):
        for yaw in (0, -10, 10, -20, 20):
            gs.hand((0.2, 1.3, -0.35), (yaw, pitch, 0))
            time.sleep(0.35)
            for m in re.finditer(r"aim A1: pick ref -> ([0-9A-F]{8}) '([^']*)'", log_since(mark)):
                if any(w.lower() in m.group(2).lower() for w in words):
                    gs.hand()
                    return m.group(1), m.group(2)
    gs.hand()
    seen = sorted(set(m.group(1) + " " + m.group(2) for m in re.finditer(r"aim A1: pick ref -> ([0-9A-F]{8}) '([^']*)'", log_since(mark))))
    return None, "not found; picks seen: " + "; ".join(seen[:12])


def test_benches():
    """Workbench menus under the furniture gate (bCraftingLiveWorld=1 in the ini): cooking 6625B, a placed weapons bench
    (17B3A4) and armour bench (12EA9B). Each: activate, menu open?, panel?, camera state 0?, close with B, closed + state."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        hmd(0.0)
        g.cmd("console tgm", quiet=True)
        jobs = [("cooking", "6625B", "CookingMenu")]
        for name, base, menu in (("weapons", "17B3A4", "ExamineMenu"), ("armor", "12EA9B", "ExamineMenu")):
            jobs.append((name, base, menu))
        for name, ref, menu in jobs:
            r = {}
            mark = log_mark()
            if name == "cooking":
                g.cmd("console player.moveto 6625B 0 -150 0", quiet=True)
                time.sleep(6.0)
            else:
                # place at the player, walk back, point the right hand at it, A = activate (FO4VR binding)
                g.cmd(f"console player.placeatme {ref}", quiet=True)
                time.sleep(1.5)
                gs.vremu("/input?dev=left&joy=0,-1&ms=900")
                time.sleep(1.5)
                r["placed"] = ref
            g.cmd("uimsg DialogueMenu hide", quiet=True)
            time.sleep(0.5)
            if name == "cooking":
                g.cmd(f"console {ref}.activate player", quiet=True)
            else:
                for pitch in (-25, -35, -15, -45):
                    gs.hand((0.2, 1.3, -0.35), (0, pitch, 0))
                    time.sleep(0.4)
                    gs.vremu("/input?dev=right&a=1&ms=150")
                    if wait_menu(g, menu, True, 3.0):
                        break
                gs.hand()
            r["opened"] = wait_menu(g, menu, True, 10.0)
            time.sleep(2.5)
            s1 = st(g)
            r["open"] = dict(panel=s1.get("menuPanel"), viewMenu=s1.get("viewMenu"), layers=s1.get("layers"), menus=open_menus(g),
                             cam=(g.cmd("cam", quiet=True) or "")[:70], mp=g.cmd("menupanel", quiet=True)[-260:])
            r["hudcap"] = (hudcap(g, f"bench_{name}") or "")[-300:]
            for _ in range(3):
                if menu not in open_menus(g):
                    break
                gs.vremu("/input?dev=right&grip=1&ms=200")
                time.sleep(2.0)
                if "ExamineConfirmMenu" in open_menus(g):  # "Exit workbench?"
                    gs.vremu("/input?dev=left&trigger=1&ms=150")  # menu context: a trigger off the panel = pad A
                    time.sleep(2.0)
            r["closed"] = dict(menus=open_menus(g), cam=(g.cmd("cam", quiet=True) or "")[:70])
            time.sleep(5.0)
            r["after5s"] = dict(menus=open_menus(g), cam=(g.cmd("cam", quiet=True) or "")[:70], status=g.cmd("status", quiet=True)[-120:])
            r["log"] = [l[:200] for l in log_since(mark).splitlines()
                        if any(k in l for k in ("camdir gate", "camera state", "view menu", "menu panel", "hud preview", "workbench"))][:14]
            p = os.path.join(CAP, f"bench_{name}_panel.bmp")
            if os.path.exists(p):
                thumb(p, os.path.join(OUT, f"bench_{name}_panel.png"))
            res[name] = r
    finally:
        g._close()
    save("benches", res)


CRAFT = {
    # name: (reference, menu, prep console commands)
    "cooking": ("6625B", "CookingMenu", []),  # moveto with an offset: standing in the fire kills (bleedout -> reload)
    "container": ("player", "ContainerMenu", []),
    "weapons": ("1A708B", "ExamineMenu", []),   # Sanctuary workbenchWeaponsA
    "armor": ("178D9F", "ExamineMenu", []),     # Sanctuary WorkbenchArmorA
    "chem": ("1AB6BE", "CookingMenu", []),      # Sanctuary WorkbenchChemistryB
    "lockpick": ("57478", "LockpickingMenu", ["player.additem 0000000A 10", "57478.lock 25"]),
}


def test_crafting(names):
    """Crafting / container menus: panel over the live world (status, camera state), the 3D preview on the panel
    (hudcap _panel.bmp), world follows the head (back buffer at yaw 0 vs 30), close."""
    g = attach()
    res = {}
    try:
        gs.vremu("/stopall")
        gs.hand(), gs.left()
        hmd(0.0)
        g.cmd("console tgm", quiet=True)  # stations are fires; the session is quit afterwards (no save)
        time.sleep(1.0)
        for name in names:
            ref, menu, prep = CRAFT[name]
            r = {}
            mark = log_mark()
            g.cmd(f"console player.moveto {ref} 0 -110 0", quiet=True)
            time.sleep(3.0)
            for c in prep:
                g.cmd("console " + c, quiet=True)
                time.sleep(0.5)
            g.cmd("uimsg DialogueMenu hide", quiet=True)  # Codsworth greets near the stations
            time.sleep(0.5)
            g.cmd(f"console {ref}.activate player" if ref != "player" else "console player.openactorcontainer 1", quiet=True)
            r["opened"] = wait_menu(g, menu, True, 15.0)
            time.sleep(3.0)
            s1 = st(g)
            cam1 = g.cmd("cam", quiet=True) or ""
            r["open"] = dict(viewMenu=s1.get("viewMenu"), panel=s1.get("menuPanel"), layers=s1.get("layers"), backdropFrames=s1.get("backdropFrames"),
                             menus=open_menus(g), cam=cam1[:60])
            r["menupanel"] = g.cmd("menupanel", quiet=True)[-700:]
            r["hudcap"] = (hudcap(g, f"craft_{name}") or "")[-900:]
            b0 = gs.capture(g, f"craft_{name}_y0")
            hmd(30.0)
            time.sleep(1.5)
            b1 = gs.capture(g, f"craft_{name}_y30")
            s2 = st(g)
            r["turned"] = dict(panel=s2.get("menuPanel"), layers=s2.get("layers"), cam=(g.cmd("cam", quiet=True) or "")[:60])
            r["bb yaw0 vs yaw30 (live world follows the head)"] = np_diff(b0, b1)
            hmd(0.0)
            time.sleep(1.0)
            for n in (f"craft_{name}_panel", f"craft_{name}_y0"):
                p = os.path.join(CAP, n + ".bmp")
                if os.path.exists(p):
                    thumb(p, os.path.join(OUT, n + ".png"))
            for _ in range(3):
                if menu not in open_menus(g):
                    break
                gs.vremu("/input?dev=right&grip=1&ms=200")
                time.sleep(2.0)
                if "ExamineConfirmMenu" in open_menus(g):  # "Exit workbench?"
                    gs.vremu("/input?dev=left&trigger=1&ms=150")  # menu context: a trigger off the panel = pad A
                    time.sleep(2.0)
            r["closed"] = dict(menus=open_menus(g), status=g.cmd("status", quiet=True)[-160:])
            r["log"] = [l[:230] for l in log_since(mark).splitlines()
                        if any(k in l for k in ("camdir", "camera state", "view menu", "menu panel", "hud preview", "anchored", "H2: renderer"))][:30]
            res[name] = r
            time.sleep(2.0)
    finally:
        g._close()
    save("crafting", res)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "launch":
        launch()
    elif a[0] == "quit":
        quit_session()
    elif a[0] == "cmd":
        g = attach()
        for c in a[1:]:
            print(f"> {c}\n{g.cmd(c, quiet=True)}")
        g._close()
    elif a[0] == "menus":
        test_menus()
    elif a[0] == "pointer":
        test_pointer()
    elif a[0] == "decal":
        test_decal()
    elif a[0] == "bar":
        test_bar()
    elif a[0] == "pausebg":
        test_pausebg()
    elif a[0] == "hudfade":
        test_hudfade()
    elif a[0] == "vats":
        test_vats()
    elif a[0] == "pipboy":
        test_pipboy()
    elif a[0] == "benches":
        test_benches()
    elif a[0] == "vatswarp":
        test_vatswarp()
    elif a[0] == "pipzoom":
        test_pipzoom()
    elif a[0] == "crafting":
        test_crafting(a[1:] or list(CRAFT))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
