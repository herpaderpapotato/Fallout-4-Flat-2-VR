"""Camera step 1 (camera director, fade, owned heading) experiment.

    python -I camdir_experiment.py [--vremu] [--only setangle,ads,tfc,sit,pa,fade,gates,headsnap,pov3,vats] [--cell QASmoke]

One launch (waits until no Fallout4 runs). Reads the pipe `camdir` / `rs` readouts and the plugin log, prints
numbers per step and writes them to work/camdir/<ts>/camdir.json. --vremu: SteamVR+vremu runtime; head yaw is
then set on the emulated HMD (/pose?dev=hmd&yaw=..), otherwise with the scripted override (pipe "pose").
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
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"

PISTOL_10MM = "00004822"
AMMO_10MM = "0001F276"
FUSION_CORE = "00075FE4"
CHAIRS = ["001A6B70", "001A6E3E", "001655B2"]   # NpcChairLounge01AClean01Sit01, NpcChairHighTechOfficeChairSit01, NpcChairInstituteCafeSit01
PA_FRAMES = ["0004A9C1", "00185D62"]             # PowerArmorFurnitureT45_NoHelm, PowerArmorFurnitureT60_Military


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return v


def vec(s):
    m = re.match(r"\(([^)]*)\)", s or "")
    return tuple(float(x) for x in m.group(1).split(",")) if m else None


def wrapd(a):
    return (a + 180.0) % 360.0 - 180.0


class Exp:
    def __init__(self, g, use_vremu):
        self.g = g
        self.vr = use_vremu
        self.out = []
        self.force_pa_fade = "--force-pa-fade" in sys.argv

    def cmd(self, c):
        return self.g.cmd(c, quiet=True)

    def cd(self):
        d = parse_kv(self.cmd("camdir"))
        return {k: (vec(v) if v.startswith("(") else num(v)) for k, v in d.items()}

    def rs(self):
        d = parse_kv(self.cmd("rs"))
        return {k: (vec(v) if v.startswith("(") else num(v)) for k, v in d.items()}

    def head_yaw(self, deg):
        if self.vr:
            vremu(f"/pose?dev=hmd&yaw={deg}&ms=200")
            time.sleep(0.6)
        else:
            self.cmd(f"pose {deg} 0 0")
            time.sleep(0.4)

    def head_pitch(self, deg):
        if self.vr:
            vremu(f"/pose?dev=hmd&pitch={deg}&ms=200")
            time.sleep(0.6)
        else:
            self.cmd(f"pose 0 {deg} 0")
            time.sleep(0.4)

    def head_reset(self):
        if self.vr:
            vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0&ms=200")
            time.sleep(0.5)
        else:
            self.cmd("pose 0 0 0")
            self.cmd("posepos 0 0 0")
            time.sleep(0.3)

    # inputs: vremu controllers under SteamVR+vremu (as suite.py does), else the pipe's virtual-pad override
    def turn(self, x=0.6, ms=500):
        if self.vr:
            vremu(f"/input?dev=right&joy={x},0&ms={ms}")
        else:
            self.cmd(f"pad rx={x} ms={ms}")

    def press_a(self, ms=150):
        if self.vr:
            vremu(f"/press?dev=right&btn=a&ms={ms}")
        else:
            self.cmd(f"pad a=1 ms={ms}")

    def hold_lt(self, ms):
        if self.vr:
            vremu(f"/input?dev=left&trigger=1&ms={ms}")
        else:
            self.cmd(f"pad lt=1 ms={ms}")

    def stick_fwd(self, ms):
        if self.vr:
            vremu(f"/input?dev=left&joy=0,1&ms={ms}")
        else:
            self.cmd(f"pad ly=1 ms={ms}")

    def rec(self, d):
        say("  " + json.dumps(d))
        self.out.append(d)

    def wait_state(self, pred, timeout, every=0.1):
        t0 = time.time()
        seen = []
        while time.time() - t0 < timeout:
            d = self.cd()
            s = int(d.get("state", -9))
            if not seen or seen[-1] != s:
                seen.append(s)
            if pred(d):
                return d, seen
            time.sleep(every)
        return None, seen

    def look(self, d):
        return dict(state=d.get("state"), name=d.get("name"), eff=d.get("eff"), ownYaw=d.get("ownYaw"), heading=d.get("heading"),
                    camYaw=d.get("camYaw"), camPitch=d.get("camPitch"), wrote=d.get("wrote"), alpha=d.get("alpha"))


def step_setangle(e):
    e.head_reset()
    d0 = e.cd()
    h0 = d0["heading"]
    target = wrapd(h0 + 90.0)
    deg_engine = target % 360.0
    e.cmd(f"console player.setangle z {deg_engine:.1f}")
    time.sleep(0.25)
    d1 = e.cd()
    time.sleep(1.5)
    d2 = e.cd()
    e.rec(dict(step="c setangle +90 (own heading)", before=e.look(d0), after_0_25s=e.look(d1), after_1_75s=e.look(d2),
               dCamYaw_0_25s=round(wrapd(d1["camYaw"] - d0["camYaw"]), 3), dCamYaw_1_75s=round(wrapd(d2["camYaw"] - d0["camYaw"]), 3),
               dHeading_0_25s=round(wrapd(d1["heading"] - d0["heading"]), 2), dHeading_1_75s=round(wrapd(d2["heading"] - d0["heading"]), 2)))
    # stick turn still turns the view
    d3 = e.cd()
    e.turn(0.6, 500)
    time.sleep(1.0)
    d4 = e.cd()
    e.rec(dict(step="c pad turn (rx 0.6, 0.5 s)", dCamYaw=round(wrapd(d4["camYaw"] - d3["camYaw"]), 2),
               dOwnYaw=round(wrapd(d4["ownYaw"] - d3["ownYaw"]), 2), dHeading=round(wrapd(d4["heading"] - d3["heading"]), 2)))
    # head yaw on top
    e.head_yaw(30)
    d5 = e.cd()
    e.rec(dict(step="c head yaw 30 after the turns", camYaw_minus_ownYaw=round(wrapd(d5["camYaw"] - d5["ownYaw"]), 2), look=e.look(d5)))
    e.head_reset()
    # comparison: owned heading off -> setangle drags the view
    e.cmd("rs cfg own 0")
    time.sleep(0.5)
    d6 = e.cd()
    e.cmd(f"console player.setangle z {(d6['heading'] + 90.0) % 360.0:.1f}")
    time.sleep(0.6)
    d7 = e.cd()
    e.rec(dict(step="c setangle +90 with rs cfg own 0 (old behaviour)", dCamYaw=round(wrapd(d7["camYaw"] - d6["camYaw"]), 2),
               dHeading=round(wrapd(d7["heading"] - d6["heading"]), 2)))
    e.cmd("rs cfg own 1")
    time.sleep(0.8)
    # UserTurn API (snap turn): view and body turn together
    d8 = e.cd()
    e.cmd("rs turn 45")
    time.sleep(0.5)
    d9 = e.cd()
    e.rec(dict(step="c rs turn 45 (UserTurn API)", dCamYaw=round(wrapd(d9["camYaw"] - d8["camYaw"]), 2),
               dOwnYaw=round(wrapd(d9["ownYaw"] - d8["ownYaw"]), 2), dHeading=round(wrapd(d9["heading"] - d8["heading"]), 2)))


def step_ads(e):
    e.head_reset()
    e.cmd(f"console player.additem {PISTOL_10MM} 1")
    e.cmd(f"console player.additem {AMMO_10MM} 100")
    time.sleep(1.0)
    e.cmd(f"aim equip {PISTOL_10MM}")
    time.sleep(2.5)
    e.hold_lt(4000)
    time.sleep(1.2)
    a = e.cd()
    cc = parse_kv(e.cmd("camcfg"))
    e.head_yaw(30)
    b = e.cd()
    e.head_yaw(-30)
    c = e.cd()
    e.head_yaw(0)
    st = parse_kv(e.cmd("status"))
    e.rec(dict(step="a iron sights (lt held)", ads=e.look(a), yaw30=e.look(b), yawm30=e.look(c),
               camMinusOwn_30=round(wrapd(b["camYaw"] - b["ownYaw"]), 2), camMinusOwn_m30=round(wrapd(c["camYaw"] - c["ownYaw"]), 2),
               camcfg={k: cc.get(k) for k in ("adjustFOV", "fovAdjustCurrent", "fovAnimatorAdjust") if k in cc},
               menus=e.cmd("menus")[:300], status_fov=st.get("fov")))
    time.sleep(1.5)


def step_tfc(e):
    mark = e.g.log_mark()
    e.cmd("console tfc")
    d, seen = e.wait_state(lambda d: d.get("state") == 3, 5)
    time.sleep(0.5)
    st = parse_kv(e.cmd("status"))
    e.cmd("console tfc")
    d2, seen2 = e.wait_state(lambda d: d.get("state") == 0, 5)
    log = [l for l in e.g.log_since(mark).splitlines() if "camera state" in l or "camdir:" in l]
    e.rec(dict(step="d tfc", inFree=e.look(d) if d else None, states=seen, back=e.look(d2) if d2 else None, statesBack=seen2,
               log=log[-8:]))


def find_pick(e, name, timeout=4.0):
    """Back up a little and poll the aim report until the hand ray picks a ref called `name`; returns its id."""
    t0 = time.time()
    backed = 0
    while time.time() - t0 < timeout:
        a = parse_kv(e.cmd("aim"))
        if a.get("pickName") == name and a.get("pick") not in (None, "00000000"):
            return a["pick"]
        if backed < 3 and e.vr:
            vremu("/input?dev=left&joy=0,-1&ms=250")
            backed += 1
            time.sleep(0.6)
        time.sleep(0.1)
    return None


def try_activate(e, label, name, pred=None, wait=3.0):
    """Activate the placed `name` ref: hand-ray pick -> pad A; if nothing happens, console '<id>.activate player'."""
    if pred is None:
        pred = lambda d: d.get("state") != 0 or d.get("eff") != "TRACK" or (d.get("alpha") or 0) > 0
    e.cmd("aim on")
    ref = find_pick(e, name)
    seen_all = []
    how = None
    for attempt in ("padA", "console"):
        if attempt == "padA":
            if not ref:
                continue
            e.press_a(150)
        else:
            if not ref:
                return None, seen_all, ("no pick", name)
            e.cmd(f"console {ref}.activate player")
        d, seen = e.wait_state(pred, wait)
        seen_all += seen
        r = e.rs()
        if d is not None or r.get("sitSleep") not in (0, 0.0, None):
            return d or e.cd(), seen_all, (attempt, ref)
    return None, seen_all, ("failed", ref)


def step_sit(e):
    e.head_reset()
    results = []
    for chair in CHAIRS:
        mark = e.g.log_mark()
        e.cmd(f"console player.placeatme {chair} 1")
        time.sleep(1.5)
        d, seen, pitch = try_activate(e, chair, 'Chair')
        if d is None:
            results.append(dict(chair=chair, activated=False, how=pitch))
            continue
        sit_tl = []
        t0 = time.time()
        menus_seen = []
        yawed = 0
        while time.time() - t0 < 5.0:
            if yawed == 0 and time.time() - t0 > 1.2:
                vremu("/pose?dev=hmd&yaw=30&ms=100") if e.vr else e.cmd("pose 30 0 0")
                yawed = 1
            elif yawed == 1 and time.time() - t0 > 2.4:
                vremu("/pose?dev=hmd&yaw=0&ms=100") if e.vr else e.cmd("pose 0 0 0")
                yawed = 2
            x = e.cd()
            sit_tl.append((round(time.time() - t0, 2), x.get("state"), x.get("eff"), x.get("wrote"), x.get("tg"), x.get("add"),
                           x.get("camYaw"), x.get("heading"), x.get("alpha")))
            m = e.cmd("menus")
            names = [seg.strip().split(" ")[0] for seg in m.split("|")[1:]]
            if not menus_seen or menus_seen[-1][1] != names:
                menus_seen.append((round(time.time() - t0, 2), names))
            time.sleep(0.2)
        r = e.rs()
        a = e.cd()
        e.head_yaw(30)
        b = e.cd()
        e.head_reset()
        tr = []
        for pp in ("0 0 0", "0 30 0", "30 0 0", "0 0 0"):
            e.cmd("posepos " + pp)
            for _ in range(4):
                time.sleep(0.25)
                x = e.cd()
                cm = re.search(r"getPos=(\d+)", e.cmd("cam"))
                tr.append((pp, x.get("wrote"), x.get("tg"), x.get("add"), x.get("raw"), x.get("out"), int(cm.group(1)) if cm else None))
        e.cmd("pose off")
        c = e.cd()
        e.head_reset()
        time.sleep(0.5)
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir:" in l or "camera state" in l]
        results.append(dict(chair=chair, activated=True, pitch=pitch, states=seen, sit_timeline=sit_tl, menus=menus_seen, sitSleep=r.get("sitSleep"), seated=e.look(a),
                            yaw30=e.look(b), camMinusOwn_30=round(wrapd(b["camYaw"] - b["ownYaw"]), 2),
                            tg=a.get("tg"), out=a.get("out"), translation=tr, log=log[-10:]))
        # stand up
        e.stick_fwd(800)
        time.sleep(2.5)
        e.press_a(150)
        time.sleep(2.0)
        results[-1]["after_standup"] = e.look(e.cd())
        break
    e.rec(dict(step="b sit", results=results))


def step_pa(e):
    e.head_reset()
    e.cmd(f"console player.additem {FUSION_CORE} 2")
    time.sleep(0.5)
    for frame in PA_FRAMES:
        mark = e.g.log_mark()
        e.cmd(f"console player.placeatme {frame} 1")
        time.sleep(2.0)
        if e.force_pa_fade:
            e.cmd("camdir policy Furniture FADE_SNAP")
        d, seen_all, how = try_activate(e, frame, 'Power Armor', wait=4)
        entered = d is not None
        if not entered:
            e.rec(dict(step="e power armour", frame=frame, entered=False, states=seen_all, how=how))
            continue
        samples = []
        t0 = time.time()
        while time.time() - t0 < 12:
            x = e.cd()
            samples.append((round(time.time() - t0, 2), x.get("state"), x.get("eff"), x.get("alpha")))
            if time.time() - t0 > 3 and x.get("state") == 0 and x.get("eff") == "TRACK" and (x.get("alpha") or 0) == 0:
                break
            time.sleep(0.1)
        fin = e.cd()
        st = parse_kv(e.cmd("status"))
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir:" in l or "camera state" in l]
        compact = []
        for s in samples:
            if not compact or compact[-1][1:] != s[1:]:
                compact.append(s)
        e.rec(dict(step="e power armour entry", frame=frame, states=seen_all, timeline=compact, lastOutMs=fin.get("lastOutMs"),
                   lastHoldMs=fin.get("lastHoldMs"), lastInMs=fin.get("lastInMs"), lastEpisode=fin.get("lastEpisode"),
                   episodes=fin.get("episodes"), timeouts=fin.get("timeouts"), fadeLayers=st.get("fadeLayers"), log=log[-24:]))
        # exit: hold activate (default policy table)
        e.cmd("camdir policy Furniture TRACK")
        time.sleep(1.0)
        mark = e.g.log_mark()
        e.press_a(1500)
        samples = []
        t0 = time.time()
        while time.time() - t0 < 12:
            x = e.cd()
            samples.append((round(time.time() - t0, 2), x.get("state"), x.get("eff"), x.get("alpha")))
            if time.time() - t0 > 4 and x.get("state") == 0 and x.get("eff") == "TRACK" and (x.get("alpha") or 0) == 0:
                break
            time.sleep(0.1)
        fin = e.cd()
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir:" in l or "camera state" in l]
        compact = []
        for s in samples:
            if not compact or compact[-1][1:] != s[1:]:
                compact.append(s)
        e.rec(dict(step="e power armour exit", timeline=compact, lastOutMs=fin.get("lastOutMs"), lastHoldMs=fin.get("lastHoldMs"),
                   lastInMs=fin.get("lastInMs"), lastEpisode=fin.get("lastEpisode"), log=log[-24:]))
        break
    e.head_reset()


def timeline(e, secs, stop=None):
    samples = []
    t0 = time.time()
    while time.time() - t0 < secs:
        x = e.cd()
        samples.append((round(time.time() - t0, 2), x.get("state"), x.get("eff"), x.get("alpha")))
        if stop and stop(time.time() - t0, x):
            break
        time.sleep(0.05)
    compact = []
    for s_ in samples:
        if not compact or compact[-1][1:] != s_[1:]:
            compact.append(s_)
    return compact


def step_pov(e):
    """First <-> third person toggle (right stick click): Transition (5) = FADE_SNAP, ThirdPerson (8) = CINEMA."""
    e.head_reset()
    for label, want in (("to third person", 8), ("back to first person", 0)):
        mark = e.g.log_mark()
        if e.vr:
            vremu("/press?dev=right&btn=joy_click&ms=400")
        else:
            e.cmd("pad rs=1 ms=150")
        tl = timeline(e, 6, lambda t, x: t > 1.0 and x.get("state") == want and (x.get("alpha") or 0) == 0)
        fin = e.cd()
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir:" in l or "camera state" in l]
        e.rec(dict(step="pov " + label, timeline=tl, lastOutMs=fin.get("lastOutMs"), lastHoldMs=fin.get("lastHoldMs"),
                   lastInMs=fin.get("lastInMs"), lastEpisode=fin.get("lastEpisode"), log=log[-14:]))
        time.sleep(1.0)


def step_fade(e):
    mark = e.g.log_mark()
    s0 = parse_kv(e.cmd("status"))
    e.cmd("camdir fadetest 300")
    alphas = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        alphas.append((round(time.time() - t0, 3), e.cd().get("alpha")))
        time.sleep(0.02)
    s1 = parse_kv(e.cmd("status"))
    log = [l for l in e.g.log_since(mark).splitlines() if "camdir:" in l]
    peak = max(a for _, a in alphas)
    e.rec(dict(step="fade test (pipe camdir fadetest 300)", peak=peak, alphas=alphas[::5],
               fadeLayersDelta=int(s1.get("fadeLayers", 0)) - int(s0.get("fadeLayers", 0)), log=log))


# ---- camera step 2 (gates) -------------------------------------------------------------------------------------
DOGMEAT_NPC = "0001D162"


def gates(e):
    return parse_kv(e.cmd("camdir gates").split(" | ")[-1])


def gate_counts(e):
    r = e.cmd("camdir gates")
    return {m.group(1): (int(m.group(2)), int(m.group(3))) for m in re.finditer(r" ([\w>:\-()]+)=patched:\d/calls:(\d+)/blocked:(\d+)", r)}


def step_headsnap(e):
    """Large game-driven rotation: setangle +90 -> fade out, view turns to the new heading, fade in; +30 (< 45) -> no snap;
    'camdir gate heading off' -> the view keeps its yaw (body turns back, step-1 behaviour)."""
    e.head_reset()
    time.sleep(0.5)
    for label, delta, gate in (("setangle +90 (snap on)", 90.0, "on"), ("setangle +30 (below 45)", 30.0, "on"),
                               ("setangle +90 (snap off)", 90.0, "off")):
        e.cmd(f"camdir gate heading {gate}")
        d0 = e.cd()
        r0 = e.rs()
        mark = e.g.log_mark()
        e.cmd(f"console player.setangle z {(d0['heading'] + delta) % 360.0:.1f}")
        tl = timeline(e, 1.6)
        d1 = e.cd()
        r1 = e.rs()
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir" in l]
        e.rec(dict(step="h " + label, dCamYaw=round(wrapd(d1["camYaw"] - d0["camYaw"]), 2), dHeading=round(wrapd(d1["heading"] - d0["heading"]), 2),
                   dOwnYaw=round(wrapd(d1["ownYaw"] - d0["ownYaw"]), 2), camMinusHeading=round(wrapd(d1["camYaw"] - d1["heading"]), 2),
                   bodyRel=r1.get("bodyRel"), lastGameTurn=r1.get("lastGameTurn"), headingSnaps=(r0.get("headingSnaps"), r1.get("headingSnaps")),
                   peakAlpha=max((x[3] or 0) for x in tl), lastOutMs=d1.get("lastOutMs"), lastHoldMs=d1.get("lastHoldMs"),
                   lastInMs=d1.get("lastInMs"), timeline=tl, log=log[-6:]))
        time.sleep(0.5)
    e.cmd("camdir gate heading on")


def step_pov3(e):
    """3rd person with bBlockThirdPerson: POV toggle (virtual pad right-stick click -> Set3rdPerson) and the script call
    Game.ForceThirdPerson (console cgf -> Force3rdPerson): state stays 0, gate counters rise; gate off -> state 8."""
    e.head_reset()
    for label, gate in (("blocked", "on"), ("gate off", "off")):
        e.cmd(f"camdir gate 3rd {gate}")
        for how, act in (("pad rs click", lambda: e.cmd("pad rs=1 ms=150")),
                         ("cgf Game.ForceThirdPerson", lambda: e.cmd('console cgf "Game.ForceThirdPerson"'))):
            c0 = gate_counts(e)
            act()
            tl = timeline(e, 2.5)
            c1 = gate_counts(e)
            e.rec(dict(step=f"g 3rd person {label}: {how}", states=sorted({x[1] for x in tl}),
                       set3rd=(c0.get("Set3rdPerson"), c1.get("Set3rdPerson")), force3rd=(c0.get("Force3rdPerson"), c1.get("Force3rdPerson")),
                       timeline=tl))
            if any(x[1] not in (0, 0.0) for x in tl):  # back to first person
                e.cmd('console cgf "Game.ForceFirstPerson"')
                timeline(e, 4.0, lambda t, x: x.get("state") == 0 and (x.get("alpha") or 0) == 0 and t > 1.0)
    e.cmd("camdir gate 3rd on")
    time.sleep(1.0)


def cam_root_yaw(e):
    """Camera root world yaw (deg, heading convention) straight from the scene graph ('cam' readout)."""
    m = re.search(r"worldRot=\[([^\]]*)\]", e.cmd("cam"))
    v = [float(x) for x in m.group(1).replace("|", " ").split()]
    return round(math.degrees(math.atan2(v[3], v[4])), 2)


def frustum(e):
    m = re.search(r"frustum l=(-?[\d.]+) r=([\d.]+) t=([\d.]+) b=(-?[\d.]+)", e.cmd("cam"))
    if not m:
        return None
    l, r, t, b = (float(x) for x in m.groups())
    return round(math.degrees(math.atan(r) - math.atan(l)), 2), round(math.degrees(math.atan(t) - math.atan(b)), 2)


def step_vats(e):
    """VATS with the camera gates (VG1/VG2/VP1): camstate stays 0, VATSMenu in the world view, frustum unchanged,
    head yaw still drives the view; camera enabled byte with VG2 on/off."""
    e.head_reset()
    e.cmd(f"console player.additem {PISTOL_10MM} 1")
    e.cmd(f"console player.additem {AMMO_10MM} 100")
    time.sleep(0.8)
    e.cmd(f"aim equip {PISTOL_10MM}")
    time.sleep(2.0)
    d = e.cd()
    # a target 300 units in front of the view
    e.cmd(f"console player.placeatme {DOGMEAT_NPC} 1")
    time.sleep(2.5)
    for label, vg2 in (("VG2 on", "on"), ("VG2 off", "off")):
        e.cmd(f"camdir gate vatscam {vg2}")
        f0 = frustum(e)
        c0 = gate_counts(e)
        st0 = parse_kv(e.cmd("status"))
        mark = e.g.log_mark()
        vats_btn = (lambda: vremu("/input?dev=right&grip=1&ms=300")) if e.vr else (lambda: e.cmd("pad rb=1 ms=200"))
        vats_btn()  # VATS = right grip (Presenter maps grip > 0.6 to RB)
        t0 = time.time()
        menu = False
        while time.time() - t0 < 3.0:
            if "VATSMenu" in e.cmd("menus"):
                menu = True
                break
            time.sleep(0.1)
        time.sleep(1.0)
        a = e.cd()
        g = gates(e)
        f1 = frustum(e)
        st = parse_kv(e.cmd("status"))
        cam_root_yaws = [cam_root_yaw(e)]
        e.head_yaw(30)
        b = e.cd()
        cam_root_yaws.append(cam_root_yaw(e))
        e.head_yaw(0)
        c = e.cd()
        cam_root_yaws.append(cam_root_yaw(e))
        menus_in = e.cmd("menus")[:300]
        vats_btn()  # VATS button again leaves VATS
        tl = timeline(e, 2.5, lambda t, x: t > 1.0 and "VATSMenu" not in e.cmd("menus"))
        c1 = gate_counts(e)
        log = [l for l in e.g.log_since(mark).splitlines() if "camdir" in l or "presenter" in l.lower() or "world view" in l or "cinema" in l]
        e.rec(dict(step="v VATS " + label, menuOpened=menu, menus=menus_in, gatesInVats=e.cmd("camdir gates").split(" | ")[-1][:400], inVats=e.look(a), frustumBefore=f0, frustumInVats=f1, statusMode=st.get("mode"),
                   fadeLayers=st.get("fadeLayers"), camEnabled=(g.get("camEnabledByte"), g.get("vatsCam")), headYaw30_dCam=round(wrapd(b["camYaw"] - a["camYaw"]), 2),
                   back0_dCam=round(wrapd(c["camYaw"] - a["camYaw"]), 2), ticksDuringYaw=(c.get("ticks") or 0) - (a.get("ticks") or 0),
                   camRootYaw=cam_root_yaws, heading_change=round(wrapd(a["heading"] - d["heading"]), 2),
                   counts={k: (c0.get(k), c1.get(k)) for k in ("StartVATSMode", "ForceVATSMode", "StopVATSMode", "ChangeState(2)")}, after=e.look(e.cd()),
                   log=log[-14:]))
        time.sleep(1.5)
    e.cmd("camdir gate vatscam on")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vremu", action="store_true")
    ap.add_argument("--only", default="fade,setangle,ads,tfc,sit,pa")
    ap.add_argument("--cell", default="QASmoke")
    ap.add_argument("--force-pa-fade", action="store_true", help="camdir policy Furniture FADE_SNAP during the PA entry")
    ap.add_argument("--wait-free", type=int, default=3600, help="seconds to wait for a running Fallout4 to exit")
    a = ap.parse_args()
    t0 = time.time()
    while game_pid():
        if time.time() - t0 > a.wait_free:
            raise SystemExit("Fallout4 still running - giving up")
        say("Fallout4 is running (another agent / the user) - waiting")
        time.sleep(20)
    ts = time.strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(ROOT, "work", "camdir", ts)
    os.makedirs(out_dir, exist_ok=True)
    steps = a.only.split(",")
    if a.vremu:
        vremu("/stopall")
        vremu("/reset")  # vremu keeps poses across sessions (a pitched controller misses the furniture)
    with Game(runtime_json=STEAMVR_JSON if a.vremu else None) as g:
        e = Exp(g, a.vremu)
        g.coc(a.cell)
        time.sleep(3.0)
        e.rec(dict(step="baseline", camdir=g.cmd("camdir", quiet=True), rs_own=parse_kv(g.cmd("rs", quiet=True)).get("ownYaw")))
        fns = dict(pov=step_pov, fade=step_fade, setangle=step_setangle, ads=step_ads, tfc=step_tfc, sit=step_sit, pa=step_pa,
                   headsnap=step_headsnap, pov3=step_pov3, vats=step_vats, gates=lambda e: e.rec(dict(step="gates", gates=e.cmd("camdir gates"))))
        for s in steps:
            say(f"== step {s}")
            try:
                fns[s](e)
            except Exception as ex:  # keep going: one launch per batch
                e.rec(dict(step=s, error=repr(ex)))
        e.rec(dict(step="end", camdir=g.cmd("camdir", quiet=True)))
        if a.vremu:
            try:
                vremu("/stopall")
            except OSError:
                pass
    with open(os.path.join(out_dir, "camdir.json"), "w", encoding="utf-8") as f:
        json.dump(e.out, f, indent=1)
    say(f"wrote {out_dir}")


if __name__ == "__main__":
    main()
