"""FO4VR parity suite (round 3, 22 Nov 2026): "is everything still working like it should?"

Each item is one FO4VR behaviour a seated Quest Touch player meets, confirmed in Fallout4VR.exe 1.2.72's code
(analysis/fo4vr-functions/evidence/<item>.md) and tested here against fo4flatvr through the real input path: SteamVR +
vremu drive the Touch controllers, so presses and poses go through our bindings. Feature index (all VR features):
analysis/fo4vr-functions/feature-index.md; the 10 picked items: items-round3.md. Earlier rounds: fo4vr_parity_round1.py,
fo4vr_parity_round2.py.

    python -I plugin/tools/fo4vr_parity.py run [item ...] [--attach] [--keep]   # launch, test, quit (default: every built item)
    python -I plugin/tools/fo4vr_parity.py list          # items, built or proposed, with their evidence files
    python -I plugin/tools/fo4vr_parity.py check         # environment + the user's config per item, no launch
    python -I plugin/tools/fo4vr_parity.py history [n]   # the last n runs side by side
    python -I plugin/tools/fo4vr_parity.py selftest      # offline: the suite's own logic
    python -I plugin/tools/fo4vr_parity.py prove <item> [--attach] [--keep] [--no-break]  # build-time proof: break the feature in this session
                                                          # (pipe switch, never the user's files) and show the test fails

Result per item and per check:
  PASS         works as FO4VR does
  FAIL         ours has it, and it does not work (now); "REGRESSED" when the previous run passed
  GAP          FO4VR has it, ours does not (yet)
  OFF          switched off by the user's config (fo4flatvr.ini, the MCM file, the profile's modlist); skipped
  NEEDS-SETUP  the environment is missing something (vremu, test data); nothing was concluded
Results: work/parity/runs/<ts>.json, work/parity/latest.json, work/parity/summary.txt (one screen), work/parity/history.csv.

Rules this follows (work/handoff/agent-rules.md): MO2 profile TestProfileVR with its latest save, never saves; refuses while
a Fallout4 it did not launch runs or work/fo4test.hold exists; never edits the user's files; starts SteamVR in emulator mode
only when no SteamVR and no game run, and never closes it.

Adding an item: write analysis/fo4vr-functions/evidence/<item>.md, add a test function and an ITEMS entry.
"""
import csv
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test  # noqa: E402
from fo4test import Game, game_pid, pid_alive, say  # noqa: E402

ROOT = fo4test.ROOT
OUT = os.path.join(ROOT, "work", "parity")
RUNS = os.path.join(OUT, "runs")
PROOFS = os.path.join(OUT, "proofs")
HISTORY = os.path.join(OUT, "history.csv")
PIDF = os.path.join(OUT, "session.pid")
EVID = os.path.join(ROOT, "analysis", "fo4vr-functions", "evidence")
VREMU = "http://127.0.0.1:8766"
VREMU_SETUP = r"C:\Users\user\source\repos\itr\third_party\VR-Emulator-Driver\vremu\setup-vremu.ps1"
DIST_INI = os.path.join(ROOT, "plugin", "dist", "Data", "F4SE", "Plugins", "fo4flatvr.ini")
FO4VR_EXE = "Fallout4VR.exe 1.2.72"

PASS, FAIL, GAP, OFF, SETUP, ERROR = "PASS", "FAIL", "GAP", "OFF", "NEEDS-SETUP", "ERROR"
RANK = {ERROR: 5, FAIL: 4, GAP: 3, SETUP: 2, OFF: 1, PASS: 0}

# Fixed test forms (Fallout4.esm; found with analysis/fo4vr-functions/tools/esm_forms.py, never random rolls)
PISTOL_10MM, AMMO_10MM = "00004822", "0001F276"
SCOPED_RIFLE, AMMO_308 = "0005BBA4", "0001F66B"   # WEAP CompMcCreadyHuntingRifle: its only template has mod_HuntingRifle_Scope_ScopeShort
# (measured: hasScope=1, zoom 000AF2A9 on every add). HuntingRifle 0004F46A rolls its scope at random and
# player.attachmodtoinventoryitem had no effect (22 Nov), so it is not used.
STIMPAK, WATER = "00023736", "000366C0"   # ALCH Stimpak / WaterPurified: two different small pickable items (findref returns
                                          # the highest ref id of a form, so the two targets must be different forms)
# Sanctuary references of the latest TestProfileVR save (fixed, placed in the world by Fallout4.esm)
REF_WEAPON_BENCH = "1A708B"  # workbenchWeaponsA
FOOTLOCKER = "00175500"      # CONT FootLocker01: placed, locked by the test, opened by pointing + Touch A (never saved)
TERMINAL = "001ADB54"        # TERM DefaultTerminalStanding02: placed, opened by pointing + Touch A
BOBBY_PIN = "0000000A"

# The game's autosave triggers, off for the suite's session only (runtime setini): TestProfileVR has bSaveOnPause=1 etc.
NO_AUTOSAVE = ["bSaveOnPause:MAIN 0", "bSaveOnTravel:MAIN 0", "bSaveOnWait:MAIN 0", "bSaveOnRest:MAIN 0", "fAutosaveEveryXMins:SaveGame 99999"]

# Seated pose (vremu standing space, metres): head 1.2 m, hands in front.
HMD0 = (0.0, 1.2, 0.0)
RHAND0 = (0.2, 1.0, -0.35)
LHAND0 = (-0.2, 1.0, -0.35)
TOUCH_PITCH = 39.4   # Touch aim pose: the game ray pitch = vremu pitch - 39.4 deg (measured 22 Nov: ypr pitch 10 -> pitchD -29.4)


# =========================================================================================== config (offline, read-only)
def _ini_value(path, section, key):
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    sec = None
    for line in txt.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1].strip().lower()
        elif sec == section.lower() and "=" in s and not s.startswith((";", "#")):
            k, v = s.split("=", 1)
            if k.strip().lower() == key.lower():
                return v.split(";")[0].strip()
    return None


def mcm_ini():
    try:
        src = fo4test.vfs_sources(r"MCM\Settings\fo4flatvr.ini")
    except OSError:
        src = []
    return src[0] if src else None


def cfg(section, key):
    """(value, layer) as the plugin resolves it: MCM file > the user's fo4flatvr.ini > the dist ini's default."""
    for layer, path in (("MCM", mcm_ini()), ("user ini", fo4test.effective_ini()), ("default", DIST_INI)):
        if path:
            v = _ini_value(path, section, key)
            if v is not None:
                return v, layer
    return None, "absent"


def game_pref(section, key):
    """The flat game's own setting in the profile's ini files (LocalSettings profiles keep them there)."""
    for name in ("Fallout4Custom.ini", "Fallout4Prefs.ini", "Fallout4.ini"):
        v = _ini_value(os.path.join(fo4test.PROFILE_DIR, name), section, key)
        if v is not None:
            return v, name
    return None, "absent"


def config_state(item):
    """OFF reasons for an item from the user's config (mods + switches); [] = on."""
    why = []
    for mod in item.get("mods", ()):
        if not fo4test.mod_enabled(mod):
            why.append(f"MO2 mod {mod} disabled in {fo4test.MO2_PROFILE}")
    for sec, key, on in item.get("switches", ()):
        v, layer = cfg(sec, key)
        if v is not None and v.strip() != str(on):
            why.append(f"[{sec}] {key}={v} ({layer})")
    for sec, key, on in item.get("game_prefs", ()):
        v, f = game_pref(sec, key)
        if v is not None and v.strip() != str(on):
            why.append(f"game [{sec}] {key}={v} ({f})")
    return why


# =========================================================================================== vremu / SteamVR
def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def vremu_state():
    return json.loads(vremu("/state"))


def vremu_up():
    try:
        vremu("/state")
        return True
    except OSError:
        return False


def steamvr_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return "vrserver.exe" in out


def ensure_vremu(start=True):
    """vremu answering -> True. SteamVR not running and no game running -> start it in emulator mode (agent-rules), wait.
    SteamVR running without vremu (normal mode, the user's headset) -> False: never touched."""
    if vremu_up():
        return True, "vremu answering"
    if steamvr_running():
        return False, "SteamVR runs in normal mode (no vremu); not touched - switch it to emulator mode to run these tests"
    if game_pid():
        return False, "a Fallout4 is running; SteamVR not started"
    if not start:
        return False, "vremu not answering"
    say("starting SteamVR in vremu emulator mode (left running)")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", VREMU_SETUP, "-Mode", "emu", "-Start"],
                   capture_output=True, text=True, timeout=120)
    t0 = time.time()
    while time.time() - t0 < 90:
        if vremu_up():
            time.sleep(3.0)
            return True, "started SteamVR (vremu)"
        time.sleep(2.0)
    return False, "SteamVR started but vremu did not answer within 90 s"


def pose(dev, pos, ypr, ms=0):
    q = f"/pose?dev={dev}&pos={pos[0]:.3f},{pos[1]:.3f},{pos[2]:.3f}&ypr={ypr[0]:.2f},{ypr[1]:.2f},{ypr[2]:.2f}"
    vremu(q + (f"&ms={ms}" if ms else ""))


def neutral():
    vremu("/stopall")
    pose("hmd", HMD0, (0, 0, 0))
    pose("right", RHAND0, (0, 0, 0))
    pose("left", LHAND0, (0, 0, 0))
    time.sleep(0.6)


def press(dev, btn, ms=150, wait=0.8):
    vremu(f"/press?dev={dev}&btn={btn}&ms={ms}")
    time.sleep(ms / 1000.0 + wait)


def haptics():
    s = vremu_state()
    return {h: int(s[h].get("haptics") or 0) for h in ("left", "right")}


# =========================================================================================== game session
class Session:
    """The game this suite launched (or its kept session). Never touches a Fallout4 it did not launch."""

    def __init__(self, attach=False):
        self.g = None
        self.own = False
        self.attach = attach

    def __enter__(self):
        os.makedirs(OUT, exist_ok=True)
        pid = game_pid()
        if pid:
            try:
                own = int(open(PIDF).read().strip())
            except (OSError, ValueError):
                own = None
            if pid != own:
                raise SystemExit(f"Fallout4 pid {pid} is running and is not this suite's session - not touching it")
            self.g = Game(attach=True, quiet=True)
            self.g.__enter__()
            self.own = True
            self.no_autosave()
            return self
        if self.attach:
            raise SystemExit("--attach: no session of this suite is running")
        self.g = Game(runtime_json=fo4test.STEAMVR_JSON, quiet=True)
        self.g.__enter__()
        open(PIDF, "w").write(str(game_pid()))
        self.own = True
        save = fo4test.latest_save()
        say(f"loading the latest save '{save}' (never saved)")
        self.g.cmd(f"console load {save}")
        time.sleep(2.0)
        self.g._wait(f"in-world ({save})", 150, lambda: self.g.ready().get("ingame") == "1")
        time.sleep(5.0)
        self.no_autosave()
        return self

    def no_autosave(self):
        """The game's own autosaves would land in the test profile (opening the Pause menu saves with bSaveOnPause=1):
        switch them off for this session only (runtime setini; nothing is written to the ini files)."""
        for k in NO_AUTOSAVE:
            self.cmd(f'console setini "{k}"')
        time.sleep(0.5)

    def cmd(self, c):
        return self.g.cmd(c, quiet=True)

    def console(self, c, wait=0.4):
        self.cmd("console " + c)
        time.sleep(wait)

    def quit(self):
        try:
            pid = int(open(PIDF).read().strip())
        except (OSError, ValueError):
            pid = None
        if not pid or pid != game_pid():
            return
        try:
            self.cmd("console qqq")
        except Exception:  # noqa: BLE001
            pass
        t0 = time.time()
        while pid_alive(pid) and time.time() - t0 < 25:
            time.sleep(0.5)
        if pid_alive(pid):
            say(f"own session pid {pid} did not exit after qqq - killing it")
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        else:
            say(f"own session exited after {time.time() - t0:.1f}s")

    def close(self):
        self.g._close()


def kv(text):
    return {k: v.strip("'") for k, v in re.findall(r"(\w+)=('[^']*'|[-\w.:]+)", text)}


def fnum(d, k, default=None):
    try:
        return float(d.get(k))
    except (TypeError, ValueError):
        return default


def menus(s):
    return [m.split()[0] for m in s.cmd("menus").split(" | ")[1:] if m.strip()]


BASE_MENUS = ("HUDMenu", "CursorMenu", "ButtonBarMenu", "FaderMenu", "VignetteMenu", "LoadingMenu", "PowerArmorHUDMenu")


def open_menus(s):
    return [x for x in menus(s) if x not in BASE_MENUS]


def wait_menu(s, name, open_=True, timeout=4.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if (name in menus(s)) == open_:
            return True
        time.sleep(0.25)
    return False


def close_all(s):
    for _ in range(3):
        m = open_menus(s)
        if not m:
            return []
        press("right", "b", 120, 0.9)
    for m in open_menus(s):
        s.cmd(f"uimsg {m} hide")
        time.sleep(0.8)
    return open_menus(s)


def aim(s):
    return kv(s.cmd("aim"))


def player_frame(s):
    """(x, y, z of the player's feet, heading deg) from the gun / aim probes."""
    g = s.cmd("gun")
    m = re.search(r"\bloc=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", g)
    a = aim(s)
    return (float(m.group(1)), float(m.group(2)), float(m.group(3))) if m else None, fnum(a, "hdgCam", 0.0)


def check(name, state, measured, expected, note=""):
    return {"check": name, "state": state, "measured": measured, "expected": expected, "note": note}


def judge(ok, implemented):
    """A check's state: matches FO4VR -> PASS; ours implements it -> FAIL; else GAP."""
    return PASS if ok else (FAIL if implemented else GAP)


def equip_draw(s, form, ammo=None, ammo_n=50, mods=()):
    s.console(f"player.additem {form} 1", 0.6)
    if ammo:
        s.console(f"player.additem {ammo} {ammo_n}", 0.4)
    for m in mods:
        s.console(f"player.attachmodtoinventoryitem {form} {m}", 0.6)
    s.cmd(f"aim equip0 {form}")
    time.sleep(1.5)
    for _ in range(3):   # the trigger draws a holstered weapon (gameplay binding)
        if kv(s.cmd("gun")).get("ws") == "3":
            return True
        press("right", "trigger", 150, 2.5)
    return kv(s.cmd("gun")).get("ws") == "3"


def aim_hand(dev, yaw, ray_pitch, pos=None, wait=0.45):
    """Point a hand: yaw relative to the tracking forward, the game ray's pitch in degrees (Touch pose tilt compensated)."""
    pose(dev, pos or (RHAND0 if dev == "right" else LHAND0), (yaw, ray_pitch + TOUCH_PITCH, 0))
    time.sleep(wait)


def angdiff(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


def inject(ctx, point):
    """prove mode, positive control for a GAP item: make the thing the test looks for happen (e.g. a haptic pulse), so the
    run shows the detector reports PASS when it is there. Never used in normal runs."""
    fn = ctx.get("inject")
    if fn:
        r = fn(ctx["session"], point)
        if r:
            ctx["inject_log"].append((point, str(r)[:120]))


BENIGN_GESTURE_ACTIONS = ("recentre", "recenter", "height", "none")


def benign_pulse(s):
    """One controller haptic pulse from our own haptics queue: a gesture slot whose action is harmless (recentre), run with
    'gesture test' (Gestures.cpp Run -> haptics::Request). Returns the reply, or None when no such slot is assigned."""
    st = s.cmd("gesture status")
    for hand, slot, action in re.findall(r" ([RL])(\w+)=(\S+)", st):
        if any(b in action.lower() for b in ("recent",)):
            names = ["Tap", "Up", "Down", "Left", "Right", "Back", "Fwd", "UpDown", "DownUp", "LeftRight", "RightLeft", "BackFwd", "FwdBack"]
            if slot in names:
                return s.cmd(f"gesture test {hand} {names.index(slot)}")
    return None


# =========================================================================================== tests
# --- aim: shots leave along the controller (evidence/aim.md: TESObjectWEAP::Fire 333740, Projectile::SetOriginAndDirection
#     104F3A0, PlayerCharacter::UpdateAutoAimActor F7A870)
AIM_POSES = [(-25.0, -10.0), (0.0, 5.0), (20.0, -20.0)]   # (hand yaw, ray pitch)


def test_aim(s, ctx):
    checks = []
    if not ctx.get("pistol"):
        ctx["pistol"] = equip_draw(s, PISTOL_10MM, AMMO_10MM)
    if not ctx["pistol"]:
        return [check("10mm pistol drawn (test data)", SETUP, kv(s.cmd("gun")).get("gate"), "drawn")]
    s.cmd("nospread reset")
    shots = []
    for yaw, pitch in AIM_POSES:
        aim_hand("right", yaw, pitch)
        a0 = aim(s)
        press("right", "trigger", 120, 1.0)
        a1 = aim(s)
        shots.append({"pose": (yaw, pitch), "rayHdg": fnum(a0, "hdgD"), "rayPitch": fnum(a0, "pitchD"), "camHdg": fnum(a0, "hdgCam"),
                      "launched": int(fnum(a1, "launches", 0)) > int(fnum(a0, "launches", 0)),
                      "launchHdg": fnum(a1, "launchHdg"), "launchPitch": fnum(a1, "launchPitch"),
                      "velHdg": fnum(a1, "projVelHdg"), "velPitch": fnum(a1, "projVelPitch")})
    neutral()
    fired = [x for x in shots if x["launched"]]
    checks.append(check("Touch trigger fires (one projectile per press)", PASS if len(fired) == len(shots) else FAIL,
                        f"{len(fired)}/{len(shots)}", "3/3"))
    errs = []
    for x in fired:
        errs.append(max(abs(angdiff(x["launchHdg"], x["rayHdg"])), abs(x["launchPitch"] - x["rayPitch"]),
                        abs(angdiff(x["velHdg"], x["rayHdg"])), abs(x["velPitch"] - x["rayPitch"])))
    ok = bool(fired) and max(errs) <= 1.0
    checks.append(check("shot direction = the controller's pointing direction (launch and flight, heading and pitch)",
                        judge(ok, True), f"max error {max(errs):.2f} deg" if errs else "no shots", "<= 1.0 deg"))
    away = [x for x in fired if abs(angdiff(x["rayHdg"], x["camHdg"])) >= 15.0]
    ok2 = bool(away) and all(abs(angdiff(x["launchHdg"], x["rayHdg"])) <= 1.0 and abs(angdiff(x["launchHdg"], x["camHdg"])) >= 10.0 for x in away)
    checks.append(check("the shot follows the hand, not the head (hand turned >= 15 deg away from the view)", judge(ok2, True),
                        [(round(x["rayHdg"] - x["camHdg"], 1), round(angdiff(x["launchHdg"], x["camHdg"]), 1)) for x in away] or "no such shot",
                        "launch heading on the hand ray, >= 10 deg off the view", "(hand-vs-view, shot-vs-view) per shot"))
    ns = kv(s.cmd("nospread"))
    checks.append(check("no random spread on the player's shots (user: no RNG aiming; ours [Weapon] bNoSpread)",
                        PASS if fnum(ns, "devMax", 0.0) <= 0.1 else FAIL, f"devMax={ns.get('devMax')} deg on={ns.get('on')}", "<= 0.1 deg (float noise)",
                        "FO4VR's own default keeps the cone of fire (bIgnoreConeOfFireCalculationsForPlayer=0); the user removes it by mod"))
    ctx["aim_shots"] = shots
    return checks


# --- weapon-fire haptics (evidence/fire-haptics.md: Fire 333740 -> 1BA9BF0 on the primary wand)
def test_fire_haptics(s, ctx):
    if not ctx.get("pistol"):
        ctx["pistol"] = equip_draw(s, PISTOL_10MM, AMMO_10MM)
    if not ctx["pistol"]:
        return [check("10mm pistol drawn (test data)", SETUP, "-", "drawn")]
    aim_hand("right", 0.0, 10.0)
    rows = []
    for _ in range(3):
        h0 = haptics()
        a0 = int(fnum(aim(s), "launches", 0))
        inject(ctx, "shot")
        press("right", "trigger", 120, 0.6)
        h1 = haptics()
        rows.append({"shot": int(fnum(aim(s), "launches", 0)) > a0, "right": h1["right"] - h0["right"], "left": h1["left"] - h0["left"]})
    neutral()
    shots = sum(1 for r in rows if r["shot"])
    pulses = [r["right"] for r in rows if r["shot"]]
    ok = shots > 0 and all(p >= 1 for p in pulses)
    impl = False   # no weapon-rumble -> controller path in ours (evidence/fire-haptics.md)
    return [check("shots fired through the Touch trigger", PASS if shots == 3 else SETUP, f"{shots}/3", "3/3"),
            check("a haptic pulse on the gun hand per shot (FO4VR: weapon rumble -> 0.01 s pulse at 2x rumble strength)",
                  judge(ok, impl), f"right-hand pulses per shot {pulses}, left {[r['left'] for r in rows]}", ">= 1 each",
                  "ours: no weapon-rumble -> controller haptics path (fo4flatvr, FRIK-flat, ROCK-flat)")]


# --- activation at each hand + rollover haptic (evidence/activation.md: F214F0, AC4360)
def place_near(s, form, fwd, left, not_ref=None):
    """Place a fixed test form at the player's feet, then move it fwd / left units in the player's frame. Ref id or None."""
    s.console(f"player.placeatme {form} 1", 1.0)
    ref = None
    for _ in range(8):
        ref = kv(s.cmd(f"pointer findref {form.lstrip('0')}")).get("ref")
        if ref and ref != "00000000" and ref != not_ref:
            break
        ref = None
        time.sleep(0.5)
    if not ref:
        return None
    p, hdg = player_frame(s)
    if not p:
        return ref
    h = math.radians(hdg)
    fx, fy = math.sin(h), math.cos(h)
    lx, ly = -math.cos(h), math.sin(h)
    x, y = p[0] + fx * fwd + lx * left, p[1] + fy * fwd + ly * left
    s.console(f"{ref}.setpos x {x:.1f}", 0.2)
    s.console(f"{ref}.setpos y {y:.1f}", 0.2)
    s.console(f"{ref}.setpos z {p[2] + 2:.1f}", 0.8)
    return ref


def find_pose(s, dev, ref, yaws, pitches, need_hand=None):
    for pitch in pitches:
        for yaw in yaws:
            aim_hand(dev, yaw, pitch, wait=0.35)
            a = aim(s)
            if a.get("pick") == ref and (need_hand is None or a.get("pickHand") == need_hand):
                return (yaw, pitch), a
    return None, None


def test_activation(s, ctx):
    checks = []
    close_all(s)
    neutral()
    pose("left", (-0.2, 0.9, -0.1), (0, -60, 0))   # off hand down at the side while the primary hand is located
    a_ref = place_near(s, STIMPAK, 55, 0)
    b_ref = place_near(s, WATER, 40, 40, not_ref=a_ref)
    if not a_ref or not b_ref:
        return [check("test references placed", SETUP, f"A={a_ref} B={b_ref}", "both")]
    yaws, pitches = [-50, -40, -30, -60, -20, -70, -10, -80, 0, -90, 10, -100, 20], [-55, -65, -45, -35, -75, -25]
    pa, _ = find_pose(s, "right", a_ref, yaws, pitches)
    pb, _ = find_pose(s, "right", b_ref, yaws, pitches)
    checks.append(check("primary (right) hand picks what it points at", judge(bool(pa), True), f"A pose {pa}", "a pose picks A"))
    checks.append(check("test geometry: B is pickable at all (right hand)", PASS if pb else SETUP, f"B pose {pb}", "a pose picks B"))
    # off hand: its own pick with no button, while the primary hand points at the sky
    aim_hand("right", 0, 40)
    lhit = None
    inject(ctx, "offhand")
    if pb:
        for yaw in [pb[0] - 10, pb[0], pb[0] + 10, pb[0] + 20]:
            for pitch in [pb[1] - 10, pb[1], pb[1] + 10]:
                aim_hand("left", yaw, pitch, pos=LHAND0, wait=0.35)
                a = aim(s)
                if a.get("pick") == b_ref:
                    lhit = (yaw, pitch, a.get("pickHand"))
                    break
            if lhit:
                break
    inject(ctx, "offhand_end")
    if pb:
        checks.append(check("off (left) hand has its own pick, no button held (FO4VR: second ViewCaster per wand)",
                            judge(bool(lhit), False), f"{lhit or 'no pick from the left hand'}; aim offWhy='{aim(s).get('offWhy')}'",
                            "B picked from the left hand",
                            "ours: one pick; the off hand only while X is held (ControllerBindings actHand)"))
    # haptic tick on a pick change, primary hand
    if pa:
        h0, changes, last = haptics(), 0, aim(s).get("pick")
        for i in range(4):
            if i % 2 == 0:
                aim_hand("right", pa[0], pa[1], wait=0.1)
            else:
                aim_hand("right", 0, 40, wait=0.1)
            inject(ctx, "pick")
            time.sleep(0.5)
            cur = aim(s).get("pick")
            changes += cur != last
            last = cur
        h1 = haptics()
        n = h1["right"] - h0["right"]
        ok = changes > 0 and n >= changes
        checks.append(check("haptic tick on the hand whose pick changes (FO4VR fActivateRumbleIntensity 0.3, 0.01 s)",
                            judge(ok, False),
                            f"{changes} pick changes, {n} right-hand pulses", "one pulse per change"))
    neutral()
    for r in (a_ref, b_ref):
        s.console(f"{r}.disable", 0.2)
    return checks


# --- scope by raising the gun to the eye (evidence/scope.md: EF7B00, EFAA60)
def test_scope(s, ctx):
    close_all(s)
    neutral()
    drawn = equip_draw(s, SCOPED_RIFLE, AMMO_308, 20)
    w = kv(s.cmd("aim weapon"))
    checks = [check("test data: scoped hunting rifle (fixed template), drawn", PASS if drawn and w.get("hasScope") == "1" else SETUP,
                    f"drawn={drawn} form={w.get('form')} hasScope={w.get('hasScope')} zoom={w.get('zoom')}", "drawn, hasScope=1")]
    if checks[0]["state"] != PASS:
        return checks
    # held normally: no scope
    aim_hand("right", 0, 0)
    time.sleep(1.0)
    away = kv(s.cmd("aim weapon")).get("scopeMenu")
    # raised to the eye: the hand just below and in front of the HMD, pointing along the view
    hx, hy, hz = HMD0
    t0 = time.time()
    pose("right", (hx + 0.03, hy - 0.06, hz - 0.15), (0, TOUCH_PITCH, 0), ms=400)
    inject(ctx, "raise")
    seen = []
    while time.time() - t0 < 3.0:
        ww = kv(s.cmd("aim weapon"))
        st = kv(s.cmd("status"))
        seen.append((round(time.time() - t0, 2), ww.get("scopeMenu"), st.get("viewMenu")))
        time.sleep(0.3)
    raised = any(x[1] == "1" for x in seen)
    views = sorted({x[2] for x in seen if x[1] == "1"})
    neutral()
    impl = False   # no raise-to-eye scope in ours (evidence/scope.md)
    checks.append(check("held away from the eye: no scope", PASS if away == "0" else FAIL, f"scopeMenu={away}", "0"))
    checks.append(check("raising the scoped gun to the eye enters the scope (FO4VR: eye-scope < 38 u, cones 25 / 7 deg)",
                        judge(raised, impl), f"scope seen in {sum(1 for x in seen if x[1] == '1')}/{len(seen)} samples", "scope engaged",
                        "ours: no raise-to-eye detection; Touch has no aim-down-sights input in gameplay"))
    if raised:
        checks.append(check("the scoped view stays in the headset (FO4VR EFAA60: world-space scope overlay, world blacked out)",
                            judge(views == [""], impl), f"viewMenu while scoped: {views}", "'' (head-tracked world view kept)",
                            "ours: ScopeMenu is a view-taking menu -> flat cinema window"))
    else:
        checks.append(check("the scoped view stays in the headset", GAP if not impl else FAIL, "not reachable: no Touch path into the scope",
                            "world-space scope overlay"))
    return checks


# --- menus: every hint glyph usable on Touch, presses through our bindings (evidence/menus.md)
GLYPH_TOUCH = {  # glyph the game shows (after our remap) -> the Touch control it names (vremu dev, btn)
    "Xenon_A": ("right", "a"), "Xenon_B": ("right", "b"), "Xenon_X": ("left", "a"), "Xenon_Y": ("left", "b"),
    "Xenon_R2": ("right", "trigger"), "Xenon_L2": ("left", "trigger"), "Xenon_R1": ("right", "grip"), "Xenon_L1": ("left", "grip"),
    "Xenon_R3": ("right", "joy_click"), "Xenon_L3": ("left", "joy_click"),
}
NO_TOUCH = {"Xenon_Select", "Xenon_Start", "Xenon_L2_Alt", "Xenon_R2_Alt"}
# per menu: how to open it (Touch where a Touch control opens it), the Touch control that closes it, and the hint
# buttons safe to press (no item used / dropped / sold, no level-up spent)
MENUS = [
    {"menu": "PipboyMenu", "open": ("touch", "left", "trigger"), "close": ("right", "grip"), "swf": ["PipboyMenu"], "pages": True,
     "safe": {"Xenon_L1", "Xenon_R1", "Xenon_L3", "Xenon_R3"}},
    {"menu": "PauseMenu", "open": ("touch", "left", "b"), "close": ("right", "b"), "swf": ["MainMenu"], "safe": set()},
    {"menu": "VATSMenu", "open": ("touch", "right", "b"), "close": ("right", "b"), "swf": ["VATSMenu"], "safe": set(), "needs_target": True},
    {"menu": "ContainerMenu", "open": ("console", "player.openactorcontainer 1"), "close": ("right", "grip"), "swf": ["ContainerMenu"], "safe": set()},
    {"menu": "LockpickingMenu", "open": ("point", FOOTLOCKER, f"player.additem {BOBBY_PIN} 10", "{ref}.lock 25"), "close": ("right", "grip"),
     "swf": ["LockpickingMenu"], "safe": set()},
    {"menu": "TerminalMenu", "open": ("point", TERMINAL), "close": ("right", "grip"), "swf": ["TerminalMenu"], "safe": set()},
    {"menu": "ExamineMenu", "open": ("console", f"{REF_WEAPON_BENCH}.activate player"), "close": ("right", "grip"), "swf": ["ExamineMenu"],
     "safe": set(), "moveto": REF_WEAPON_BENCH},
]
NOT_YET = {"BarterMenu": "no fixed vendor reference yet",
           "DialogueMenu": "item 10 (dialogue)", "FavoritesMenu": "item 7 (favorites)", "WorkshopMenu": "workshop agent's area",
           "BookMenu": "no fixed book reference yet", "SleepWaitMenu": "no fixed bed reference yet"}


def glyph_shown(s, xen):
    r = s.cmd(f"bindings glyphs {xen}")
    m = re.search(r"probe (\S+)\s+\S*\s*-> (\(kept\)|\S+) \(ctx (\w+)\)", r)
    if not m:
        return None, None
    return (xen if m.group(2) == "(kept)" else m.group(2)), m.group(3)


def menu_snapshot(s):
    b = s.cmd("pointer bar")
    bar = [t for t, v in re.findall(r"text='([^']*)' vis=(\d)", b) if v == "1" and t]
    p = re.search(r"page=(\d+) tab=(\d+)", s.cmd("pipboy"))
    return {"menus": menus(s), "bar": bar, "pip": p.groups() if p else None}


def audit_menu(s, spec, stems, H):
    rows, seen = [], set()
    for stem in stems:
        for label, xen in H.get(stem, []):
            if (stem, xen) in seen:      # one row per hint button (the static table repeats PC-key / label pairs)
                continue
            seen.add((stem, xen))
            shown, ctx = glyph_shown(s, xen)
            touch = GLYPH_TOUCH.get(shown)
            row = {"swf": stem, "label": label, "button": xen, "shown": shown, "touch": "/".join(touch) if touch else None, "ctx": ctx}
            if touch is None and not (shown or "").endswith(("_LS", "_RS")) and shown not in ("Xenon_LS", "Xenon_RS"):
                row["state"] = GAP
                row["why"] = "glyph names no Quest Touch control" if shown in NO_TOUCH or shown is None else f"glyph {shown} unmapped"
            elif touch and xen in spec["safe"] and xen == shown and shown in ("Xenon_L1", "Xenon_R1"):
                row["state"] = GAP
                row["why"] = "shoulder glyph: Touch grip drawn as a bumper"
            else:
                row["state"] = PASS
            if touch and xen in spec["safe"]:
                before = menu_snapshot(s)
                press(touch[0], touch[1], 120, 0.8)
                after = menu_snapshot(s)
                row["pressed"] = touch
                row["effect"] = before != after
                if spec["menu"] not in after["menus"]:
                    row["note"] = "closed the menu"
                    open_menu(s, spec)
                if not row["effect"] and row["state"] == PASS:
                    row["state"] = GAP
                    row["why"] = "the Touch control the glyph names did nothing"
            rows.append(row)
    return rows


def dismiss_box(s):
    """A MessageBoxMenu on top (e.g. the first-use lockpicking tutorial): press the Touch control its accept glyph (Xenon_A)
    names, hand off the panel. None when there is no box; else what was pressed and whether it closed."""
    if "MessageBoxMenu" not in menus(s):
        return None
    shown, ctx = glyph_shown(s, "Xenon_A")
    t = GLYPH_TOUCH.get(shown)
    if t:
        aim_hand("right", 0, -80, wait=0.3)
        press(t[0], t[1], 150, 1.5)
    return {"glyph": shown, "ctx": ctx, "touch": "/".join(t) if t else None, "closed": "MessageBoxMenu" not in menus(s)}


def open_menu(s, spec):
    kind = spec["open"][0]
    if kind == "touch":
        press(spec["open"][1], spec["open"][2], 150, 1.2)
    elif kind == "point":    # place a fixed form beside the player, point the right hand at it, Touch A (the real activation path)
        ref = spec.get("_ref") or place_near(s, spec["open"][1], 70, 0)   # ahead: a terminal is activated from its front
        spec["_ref"] = ref
        if not ref:
            return False
        for c in spec["open"][2:]:
            s.console(c.format(ref=ref), 0.6)
        pz, _ = find_pose(s, "right", ref, [0, -10, 10, -20, 20, -30, 30], [-50, -60, -40, -30, -20, -10, -70, 0])
        spec["_pose"] = pz
        if not pz:
            return False
        press("right", "a", 150, 2.0)
        aim_hand("right", 0, -80)
    else:
        for c in spec["open"][1:]:
            s.console(c, 1.0)
    return wait_menu(s, spec["menu"], True, 5.0)


def test_menus(s, ctx):
    sys.path.insert(0, os.path.join(ROOT, "analysis", "fo4vr-functions", "tools"))
    import glyph_audit as ga
    H = ga.hints()
    checks, detail = [], []
    neutral()
    close_all(s)
    for spec in MENUS:
        m = spec["menu"]
        r = {"menu": m}
        if spec.get("moveto"):
            s.console(f"player.moveto {spec['moveto']} 0 -110 0", 4.0)
            s.cmd("uimsg DialogueMenu hide")
        if spec.get("needs_target"):
            ctx["pistol"] = equip_draw(s, PISTOL_10MM, AMMO_10MM)
            opened = False
            for _ in range(3):          # LvlBloatfly: something to target in V.A.T.S. (it flies; retry until one is in view)
                s.console("player.placeatme 0003320A 1", 3.0)
                if open_menu(s, spec):
                    opened = True
                    break
        else:
            opened = open_menu(s, spec)
        r["opened"] = opened
        how = {"touch": "Touch " + "/".join(spec["open"][1:]), "point": "placed, pointed at, Touch A"}.get(spec["open"][0], "console (needs a reference)")
        if not opened:
            checks.append(check(f"{m}: opens ({how})", FAIL if spec["open"][0] == "touch" else SETUP, open_menus(s), m))
            close_all(s)
            detail.append(r)
            continue
        time.sleep(0.8)
        box = dismiss_box(s)
        if box:
            r["message_box"] = box
            checks.append(check(f"{m}: the message box over it closes with the Touch control its accept glyph names",
                                PASS if box["closed"] else FAIL, box, "closed",
                                "ours: the bindings stay in the underlying menu's context while a MessageBoxMenu is on top" if not box["closed"] else ""))
            if not box["closed"]:
                s.cmd("uimsg MessageBoxMenu hide")
                time.sleep(1.0)
        rows = audit_menu(s, spec, spec["swf"], H)
        bar_texts = [t for t in menu_snapshot(s)["bar"] if t.strip("- ")]
        static_hints = any(H.get(x) for x in spec["swf"])
        if spec.get("pages"):
            seen = set()
            pages = {"0": "Pipboy_StatsPage", "1": "Pipboy_InvPage", "2": "Pipboy_DataPage", "3": "Pipboy_MapPage", "4": "Pipboy_RadioPage"}
            for step in ("b", "a"):            # Y = next tab, then X = previous tab (FO4VR Pip-Boy bindings), from wherever it opened
                for _ in range(6):
                    p = menu_snapshot(s)["pip"]
                    if not p:
                        break
                    if p[0] not in seen:
                        seen.add(p[0])
                        if pages.get(p[0]):
                            rows += audit_menu(s, spec, [pages[p[0]]], H)
                    before = p[0]
                    press("left", step, 120, 0.9)
                    q = menu_snapshot(s)["pip"]
                    if not q or q[0] == before:
                        break
            r["pages_seen"] = sorted(seen)
            checks.append(check("PipboyMenu: Touch Y steps through the tabs", PASS if len(seen) >= 4 else FAIL, sorted(seen), ">= 4 pages"))
        r["hints"] = rows
        r["bar"] = bar_texts
        bad = [x for x in rows if x["state"] != PASS]
        if static_hints:
            checks.append(check(f"{m}: every hint glyph names a Touch control that works", GAP if bad else PASS,
                                [f"{x['label']}={x['shown']}:{x.get('why')}" for x in bad][:12] or f"{len(rows)} hints ok", "all usable on Touch"))
        else:
            # no literal hints in the SWF: the hints are built at runtime (often in the menu's own bar, which 'pointer bar'
            # - the shared ButtonBarMenu - does not show), so nothing is concluded about these glyphs
            checks.append(check(f"{m}: hint glyphs audited", SETUP, f"no literal hints in the SWF; shared bar {bar_texts[:6]}",
                                "a live hint-button probe (not built yet)", "glyphs of runtime-built hints are not read yet"))
        press(spec["close"][0], spec["close"][1], 150, 1.5)
        for conf in ("ExamineConfirmMenu", "MessageBoxMenu"):
            if conf in menus(s):     # "Exit?" prompt: press the Touch control its accept glyph names
                shown, _ = glyph_shown(s, "Xenon_A")
                t = GLYPH_TOUCH.get(shown)
                r["confirm"] = (conf, shown, t)
                if t:
                    aim_hand("right", 0, -80, wait=0.3)   # off the panel: on it, the trigger is the pointer's click
                    press(t[0], t[1], 150, 1.5)
        closed = wait_menu(s, m, False, 4.0)
        checks.append(check(f"{m}: closes with Touch {'/'.join(spec['close'])}", PASS if closed else FAIL, closed, True))
        close_all(s)
        if spec.get("_ref"):
            s.console(f"{spec.pop('_ref')}.disable", 0.3)
        detail.append(r)
    for m, why in NOT_YET.items():
        detail.append({"menu": m, "not_tested": why})
    ctx["menus_detail"] = detail
    return checks



# --- vats: VATS playback shows the whole player, head included, animated by the game (evidence/vats-playback.md; user 22 Nov
#     05:36 "First vats test I did, nora had no head"). Render-level probe: fo4flatvr `facediag skin` (BSSkin::Instance +0xBC =
#     the frame its bone matrices were last built, set by CalculateBoneMatrices 182E190 only when that piece is drawn; skin
#     entries vs the live bones). FRIK-flat C39 hides the head in first person; C35/C39/C45 show it outside first person.
LVL_BLOATFLY = "0003320A"   # NPC_ LvlBloatfly (round-2 target; not used since 22 Nov 14:20)
CODSWORTH = "179FF"         # NPC_ Codsworth (Fallout4.esm 000179FF): essential, does not fight back - the user's VATS target


def codsworth_here(s):
    """The user's test target: Codsworth's reference (pointer findref on his base) moved to the player. No spawning."""
    ref = kv(s.cmd(f"pointer findref {CODSWORTH}")).get("ref") or "1CA7D"
    s.cmd(f"console {ref.lstrip('0') or ref}.moveto player")
    return ref
FRIK_LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\FRIK.log")
SKIN_ENTRY = re.compile(r"\| (F:)?'([^']*)' cull (\d)(\^?) drawn \d+ \(([-+]\d+)\) bones \d+/\d+ notLive (\d+) far (\d+)")


def _log_mark(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def _log_since(path, mark):
    try:
        with open(path, "rb") as f:
            f.seek(mark)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def skin_sample(s):
    """One `facediag skin` line (next Present) -> {"body": [...], "face": [...]} with (name, cull, ancestorCulled, drawnDelta,
    notLive) per skinned piece of the player's third-person 3D; None when no line came."""
    m = _log_mark(fo4test.LOG)
    s.cmd("facediag skin")
    line = None
    for _ in range(10):
        time.sleep(0.1)
        line = next((l for l in _log_since(fo4test.LOG, m).splitlines() if "facediag skin:" in l), None)
        if line:
            break
    if not line:
        return None
    out = {"body": [], "face": [], "extra": line.split("||", 1)[1].strip() if "||" in line else ""}
    for face, name, cull, anc, d, nl, _far in SKIN_ENTRY.findall(line):
        out["face" if face else "body"].append((name, int(cull), anc == "^", int(d), int(nl)))
    return out


def head_drawn(smp):
    """(ok, text): the head's pieces are drawn in the same frame as the body: no culled FaceGen node above them, at least one
    head piece not culled, and its last-drawn frame within 2 of the body's."""
    if not smp or not smp["face"] or not smp["body"]:
        return False, "no sample / no FaceGen head"
    body_d = max(b[3] for b in smp["body"])
    vis = [f for f in smp["face"] if f[1] == 0 and not f[2]]
    if not vis:
        anc = any(f[2] for f in smp["face"])
        return False, "FaceGen head node culled" if anc else "every head piece culled"
    face_d = max(f[3] for f in vis)
    return face_d >= body_d - 2, f"head drawn {face_d:+d} vs body {body_d:+d} frames"


VATS_ROUNDS = int(os.environ.get("VATS_ROUNDS", "1"))   # load cycles (each: one playback right after the load, one well after)
SWAP_RE = re.compile(r"facegen: customization textures rendered \(UpdatePendingCustomizationTextures\)")


def _tod(line):
    m = re.search(r"(\d\d):(\d\d):(\d\d)\.(\d{3})", line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000 if m else None


def _now_tod():
    t = time.localtime()
    return t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec + (time.time() % 1)


def fast_arm(s):
    """Real play: the gun is out. After a (re)load: 10mm added, equipped and drawn with Touch R trigger, polled fast."""
    s.cmd(f"console player.additem {PISTOL_10MM} 1")
    s.cmd(f"console player.additem {AMMO_10MM} 50")
    s.cmd(f"aim equip0 {PISTOL_10MM}")
    t0 = time.time()
    while time.time() - t0 < 5.0:
        if kv(s.cmd("gun")).get("ws") == "3":
            return True
        vremu("/press?dev=right&btn=trigger&ms=100")
        time.sleep(0.4)
    return kv(s.cmd("gun")).get("ws") == "3"


def fast_playback(s, ctx, tag, t_load):
    """One V.A.T.S. at player speed, Touch only: B in; R trigger x3 (pad RT = queue a shot) and L trigger (pad A = execute),
    0.2 s apart, once the game's zoom-in accepts input (taps from 0.5 s, one repeat if nothing starts); render samples through
    the playback; the shots counted from fo4flatvr's `vats shot #` lines (22 Nov 14:20: RT queues, A executes - every earlier
    playback started on the first A after an RT)."""
    r = {"tag": tag, "samples": [], "taps": 0}
    pre = skin_sample(s)
    r["pre"] = pre.get("extra", "")[:900] if pre else ""
    m_frik, m_ours = _log_mark(FRIK_LOG), _log_mark(fo4test.LOG)
    t0 = time.time()
    vremu("/press?dev=right&btn=b&ms=100")             # V.A.T.S. (gameplay: primary upper button)
    while time.time() - t0 < 2.0 and "VATSMenu" not in menus(s):
        time.sleep(0.05)
    r["via"] = "Touch B"
    if "VATSMenu" not in menus(s):
        s.cmd("pad lb=1 ms=100")
        r["via"] = "pipe pad LB"
        while time.time() - t0 < 3.5 and "VATSMenu" not in menus(s):
            time.sleep(0.05)
    r["opened"] = "VATSMenu" in menus(s)
    r["t_open"] = round(time.time() - t0, 2)
    played = False
    if r["opened"]:
        time.sleep(0.5)
        for _ in range(2):
            for dev in ("right", "right", "right", "left"):
                vremu(f"/press?dev={dev}&btn=trigger&ms=90")
                r["taps"] += 1
                time.sleep(0.2)
            t1 = time.time()
            while time.time() - t1 < 1.5 and not played:
                played = "show first person 0" in _log_since(FRIK_LOG, m_frik)
                time.sleep(0.05)
            if played or "VATSMenu" not in menus(s):
                break
    r["played"] = played
    r["t_play"] = round(time.time() - t0, 2)
    r["since_load"] = round(time.time() - t_load, 1)
    if played:
        inject(ctx, "playback")                        # prove only: hide the head as an unknown hider would
        while len(r["samples"]) < 4 and "VATSMenu" in menus(s):
            smp = skin_sample(s)
            if smp:
                r["samples"].append(smp)
    wait_menu(s, "VATSMenu", False, 10.0)
    ours, frik = _log_since(fo4test.LOG, m_ours), _log_since(FRIK_LOG, m_frik)
    r["shots"] = len(re.findall(r"vats shot #", ours))
    r["lines"] = [l[:260] for l in frik.splitlines() if re.search(r"C39: |C45: |C23 skin restore", l)][:8]
    return r


def test_vats(s, ctx):
    checks = []
    if skin_sample(s) is None:
        return [check("render probe (fo4flatvr `facediag skin`)", SETUP, "no facediag skin line", "a line per call",
                      "fo4flatvr.dll older than 22 Nov 13:13")]
    save = fo4test.latest_save()
    runs = []
    for rnd in range(VATS_ROUNDS):
        close_all(s)
        m_ours = _log_mark(fo4test.LOG)
        s.cmd(f"console load {save}")                   # a fresh load each round: the head swap comes ~5 s after it
        t1 = time.time()
        while time.time() - t1 < 8 and s.g.ready().get("loading") != "1":    # the load starting
            time.sleep(0.05)
        while time.time() - t1 < 60 and not (s.g.ready().get("ingame") == "1" and s.g.ready().get("loading") == "0"):
            time.sleep(0.1)
        t_load = time.time()
        s.no_autosave()
        codsworth_here(s)
        armed = fast_arm(s)
        early = fast_playback(s, ctx, f"round {rnd + 1}: right after the load", t_load) if armed else \
            {"tag": f"round {rnd + 1}: right after the load (gun not drawn)", "played": False, "samples": []}
        while time.time() - t_load < 20.0:             # well after the head swap (~5 s after the load)
            time.sleep(0.5)
        close_all(s)
        codsworth_here(s)
        time.sleep(0.3)
        late = fast_playback(s, ctx, f"round {rnd + 1}: 20 s after the load", t_load)
        extra = []
        if os.environ.get("VATS_AB") == "1":   # diagnosis: the same playback with FRIK-flat C39 (first-person head cull) off
            s.cmd("cfgui set FRIK bHeadlessFirstPerson 0")
            time.sleep(2.0)
            codsworth_here(s)
            time.sleep(0.3)
            extra.append(fast_playback(s, ctx, f"round {rnd + 1}: C39 off (bHeadlessFirstPerson 0)", t_load))
            s.cmd("cfgui reset FRIK bHeadlessFirstPerson")
            time.sleep(2.0)
            codsworth_here(s)
            time.sleep(0.3)
            extra.append(fast_playback(s, ctx, f"round {rnd + 1}: C39 back on", t_load))
        load_tod = _now_tod() - (time.time() - t_load)
        swaps = [_tod(l) for l in _log_since(fo4test.LOG, m_ours).splitlines() if SWAP_RE.search(l)]
        for r in [early, late] + extra:
            r["swaps_s_after_load"] = [round(t - load_tod, 1) for t in swaps if t]
            runs.append(r)
    ctx["vats_runs"] = [dict({k: v for k, v in r.items() if k != "samples"}, heads=[head_drawn(x)[1] for x in r.get("samples", [])],
                             bounds=[x.get("extra", "")[:900] for x in r.get("samples", [])[:1]])
                        for r in runs]
    played = [r for r in runs if r.get("played")]
    checks.append(check("V.A.T.S. on Codsworth at player speed fires real shots (Touch B, R trigger x3 queue, L trigger execute)",
                        PASS if len(played) == len(runs) and all(r.get("shots") for r in played) else SETUP,
                        [f"{r['tag']}: open {r.get('t_open')} s, playback {r.get('t_play')} s, {r.get('since_load')} s after load, "
                         f"shots {r.get('shots')}" for r in runs], "every playback runs"))
    if not played:
        return checks
    sampled = False
    for r in played:
        if not r["samples"]:
            checks.append(check(f"head visible during playback ({r['tag']})", SETUP, "playback ended before a render sample", ">= 1"))
            continue
        sampled = True
        res = [head_drawn(x) for x in r["samples"]]
        checks.append(check(f"the player's head is drawn during the playback ({r['tag']}; head swap at {r['swaps_s_after_load']} s)",
                            judge(all(x[0] for x in res), True), "; ".join(x[1] for x in res), "drawn every sample"))
        stale = [max([b[4] for b in x["body"]] + [f[4] for f in x["face"]]) for x in r["samples"]]
        checks.append(check(f"body and head skinned from the live animated bones ({r['tag']})", judge(all(v == 0 for v in stale), True),
                            f"entries off their live bone per sample {stale}", "0"))
    if sampled and cfg("FRIK", "bHeadlessFirstPerson")[0] in (None, "1"):
        close_all(s)
        time.sleep(1.0)
        after = skin_sample(s)
        hid = bool(after and after["face"] and all(f[1] or f[2] for f in after["face"]))
        checks.append(check("back in first person the head is hidden again (FRIK-flat C39)", judge(hid, True),
                            "hidden" if hid else "shown", "hidden"))
    return checks


# --- dialogue: a primary-stick flick picks the response by FO4VR's fixed directions (evidence/dialogue.md: DialogueMenu
#     thumbstick slot VR 12ED530 -> MenuTopicManager::OnMenuButtonPress CEA480 / flat BAF7C0; direction rule VR 1BBFDD0)
DLG_DIRS = [("up", (0.0, 1.0), 3, "Question"), ("right", (1.0, 0.0), 1, "Negative"), ("down", (0.0, -1.0), 0, "Positive"),
            ("left", (-1.0, 0.0), 2, "Neutral")]
DLG_NPC = "00002CC0"   # Myrna (Fallout4.esm ACHR, Diamond City market; her greeting offers all 4 responses). The player is
                       # moved next to her (console; never saved). Codsworth 0001CA7D does not talk in the TestNora save.


def dlg(s):
    return kv(s.cmd("dlg status"))


def xdi_slots(st):
    """XDI rows -> {optionID: row} from the status line (xdi='rows=N alpha=.. sel=.. [0:3"text" 1:0"text" ...]')."""
    return {int(i): int(r) for r, i in re.findall(r'(\d+):(-?\d+)"', st.get("xdi", ""))}


def dlg_wait_pickable(s, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = dlg(s)
        if st.get("canPick") == "1" and (st.get("xdiMode") in ("none", "slots", "long")):
            return st
        if st.get("open") != "1" and time.time() - t0 > 3.0:
            return None
        time.sleep(0.05)
    return None


def dlg_where(s, npc):
    """(distance, bearing, dz) of the NPC from the player (`dlg npcs`), or None when it is not in the player's cell."""
    m = re.search(rf"{npc}/\w+ '[^']*' ([\d.]+) b=([-\d.]+) dz=([-\d.]+)", s.cmd("dlg npcs"))
    return (float(m.group(1)), float(m.group(2)), float(m.group(3))) if m else None


def dlg_bring(s, npc):
    """The player next to the fixed NPC (console move when she is not close; never saved). Returns dlg_where()."""
    w = dlg_where(s, npc)
    if w and w[0] < 400:
        return w
    s.console(f"player.moveto {npc}", 3.0)
    t0 = time.time()
    while time.time() - t0 < 30:   # cell load
        w = dlg_where(s, npc)
        if w:
            break
        time.sleep(1.0)
    time.sleep(1.5)
    return dlg_where(s, npc)


def dlg_talk(s, npc):
    """Start a conversation with the fixed NPC the way a player does: point the right hand at her, Touch A."""
    if dlg(s).get("open") == "1":
        return True
    w = dlg_bring(s, npc)
    if not w:
        return False
    hdg = fnum(aim(s), "hdgCam", 0.0)
    yaw = angdiff(w[1], hdg)
    pitch = math.degrees(math.atan2(w[2] - 30.0, max(w[0], 1.0)))   # chest height vs the eye (about 30 units)
    for dp in (0, -8, 8, -16):
        aim_hand("right", yaw, pitch + dp, wait=0.25)
        if aim(s).get("pick") == npc:
            press("right", "a", 150, 0.2)
            ok = wait_menu(s, "DialogueMenu", True, 4.0)
            aim_hand("right", 0, -30, wait=0.05)
            return ok
    return False


def dlg_leave(s, npc):
    """End the conversation the way a player does: walk away (the player is moved 1500 units; never DialogueMenu hide,
    which leaves the scene half-open)."""
    if dlg(s).get("open") != "1":
        return True
    p, _ = player_frame(s)
    if p:
        s.console(f"player.setpos x {p[0] + 1500:.1f}", 2.5)
    t0 = time.time()
    while time.time() - t0 < 8 and dlg(s).get("open") == "1":
        time.sleep(0.25)
    return dlg(s).get("open") != "1"


def test_dialogue(s, ctx):
    checks = []
    npc = ctx.get("dlg_npc") or DLG_NPC
    if "responses=" not in s.cmd("dlg status"):
        return [check("fo4flatvr dialogue stick probe (`dlg status` responses / engine picks)", SETUP, "missing", "present",
                      "fo4flatvr.dll older than 22 Nov 14:16")]
    if dlg(s).get("open") != "1":
        close_all(s)
        neutral()
        if not dlg_bring(s, npc):
            return [check("fixed NPC reachable (test data)", SETUP, f"{npc} not found after player.moveto", "in the cell")]
    xdi = dlg(s).get("xdiDll") == "1"
    todo = list(DLG_DIRS)
    results, haptic, talks, t_end = {}, None, 0, time.time() + 90
    while todo and time.time() < t_end:
        if dlg(s).get("open") != "1":
            if talks >= 5:
                break
            talks += 1
            h0 = haptics()["right"]
            if not dlg_talk(s, npc):
                continue
            st = dlg_wait_pickable(s, 15.0)
            if st is not None and haptic is None:
                haptic = haptics()["right"] > h0
        else:
            st = dlg_wait_pickable(s, 15.0)
        if st is None:
            continue
        # the next direction whose response this option set offers (XDI shows its rows; vanilla always has the 4 buttons)
        offered = xdi_slots(st) if st.get("xdiMode") == "slots" else None
        pick = next((d for d in todo if offered is None or d[2] in offered), None)
        if pick is None:
            dlg_leave(s, npc)
            continue
        name, (jx, jy), want, label = pick
        picks0 = int(st.get("picks", 0))
        m = _log_mark(fo4test.LOG)
        t0 = time.time()
        vremu(f"/input?dev=right&joy={jx:.2f},{jy:.2f}&ms=120")       # a real-speed flick: 120 ms out and back
        got = None
        while time.time() - t0 < 2.0:
            st2 = dlg(s)
            if int(st2.get("picks", 0)) > picks0:
                got = int(st2.get("lastType", -1))
                break
            time.sleep(0.03)
        ms = round((time.time() - t0) * 1000)
        log = _log_since(fo4test.LOG, m)
        mrow = re.search(r"XDI row (\d+) picked", log)
        via = f"XDI row {mrow.group(1)} (optionID {want})" if mrow else "OnMenuButtonPress" if got is not None else "-"
        results[name] = (got, f"engine got {got} via {via} after {ms} ms; options were {st.get('xdi', 'vanilla')[:120]}")
        todo.remove(pick)
    dlg_leave(s, npc)
    for name, _j, want, label in DLG_DIRS:
        if name not in results:
            checks.append(check(f"R stick {name} -> {label} ({want})", SETUP, f"no option set offered {label} ({talks} conversations)",
                                f"{label} ({want})"))
            continue
        got, note = results[name]
        checks.append(check(f"R stick {name} picks the {label} response (OnMenuButtonPress type {want})", judge(got == want, True),
                            note, f"type {want}"))
    if haptic is not None:
        checks.append(check("a haptic pulse on the right controller when the options appear", judge(haptic, True),
                            "pulse" if haptic else "none", "pulse (FO4VR 0.1 for 0.5 s)"))
    ctx["dialogue_detail"] = {"npc": npc, "xdi": xdi, "talks": talks, "results": results}
    return checks


# =========================================================================================== registry
# switches: (section, key, on value) of OUR feature; any off -> OFF. mods: MO2 mods the item needs.
# break: pipe commands that switch the feature off inside the session (prove), restore: switch it back.
ITEMS = {
    "aim": {"title": "Aim with the controller: the shot leaves along the hand's pointing direction", "feature": "aim",
            "evidence": "aim.md", "test": test_aim, "switches": [("Controllers", "bAimWithController", 1), ("General", "bEnabled", 1)],
            "mods": ["fo4flatvr"], "break": ["aim off"], "restore": ["aim on"]},
    "firehaptics": {"title": "Weapon fire haptics on the gun hand", "feature": "haptics", "evidence": "fire-haptics.md",
                    "test": test_fire_haptics, "switches": [("General", "bEnabled", 1)], "game_prefs": [("Controls", "bGamePadRumble", 1)],
                    "mods": ["fo4flatvr"], "inject": lambda s, point: point == "shot" and s.cmd("haptic R 0.3 0.01")},
    "activation": {"title": "Activation at each hand: pick per hand, haptic tick on a pick change", "feature": "activation",
                   "evidence": "activation.md", "test": test_activation,
                   "switches": [("Controllers", "bAimWithController", 1), ("General", "bEnabled", 1)], "mods": ["fo4flatvr"],
                   "break": ["aim off"], "restore": ["aim on"]},
    "scope": {"title": "Scope by raising a scoped gun to the eye", "feature": "scope", "evidence": "scope.md", "test": test_scope,
              "switches": [("General", "bEnabled", 1)], "mods": ["fo4flatvr"],
              # positive control: the game's own aim-down-sights (pad LT, bindings off in this session) opens the scope
              "inject": lambda s, point: point == "raise" and (s.cmd("bindings off"), s.cmd("pad lt=1 ms=2500"))[1],
              "inject_restore": lambda s: (time.sleep(2.0), s.cmd("bindings on"))},
    "menus": {"title": "Menus: every button hint usable on Touch, opened and closed with Touch", "feature": "menus",
              "evidence": "menus.md", "test": test_menus,
              "switches": [("Controllers", "bFO4VRBindings", 1), ("General", "bEnabled", 1)], "mods": ["fo4flatvr"],
              "break": ["bindings off"], "restore": ["bindings on"]},
    # proposed (evidence done, test designed in the evidence file, not built yet)
    "vats": {"title": "VATS playback: body animated, head visible", "feature": "vats", "evidence": "vats-playback.md", "test": test_vats,
             "switches": [("General", "bEnabled", 1)], "mods": ["fo4flatvr", "FRIK-flat"],
             # prove: the head hide back on the direct cull bit (the 22 Nov cause: no engine "changed" mark -> head not drawn)
             "break": ["cfgui set FRIK bEngineCullFlag 0"], "restore": ["cfgui reset FRIK bEngineCullFlag"]},
    "favorites": {"title": "Favorites quick-select on the stick, with a haptic per sector", "feature": "favorites",
                  "evidence": "favorites.md", "test": None},
    "quickloot": {"title": "Quick loot at the hand, with a haptic when the container changes", "feature": "quickloot",
                  "evidence": "quickloot.md", "test": None},
    "wandhud": {"title": "Ammo counter / status on the gun hand, offset per weapon", "feature": "hud", "evidence": "wand-hud.md", "test": None},
    # 22 Nov (dialogue agent): FO4VR right-stick responses (+ the options haptic); under XDI its row for the slot
    "dialogue": {"title": "Dialogue: a right-stick flick picks the response by FO4VR's fixed directions; haptic on options",
                 "feature": "dialogue", "evidence": "dialogue.md", "test": test_dialogue,
                 "switches": [("Dialogue", "bStickResponses", 1), ("General", "bEnabled", 1)], "mods": ["fo4flatvr"],
                 "break": ["dlg stick off"], "restore": ["dlg stick on"]},
}
ORDER = ["aim", "firehaptics", "activation", "scope", "menus", "vats", "dialogue"]


def aggregate(checks):
    if not checks:
        return SETUP
    return max((c["state"] for c in checks), key=lambda st: RANK[st])


# =========================================================================================== run / output
def last_states():
    if not os.path.exists(HISTORY):
        return {}
    last = {}
    with open(HISTORY, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            last[r["item"]] = (r["state"], r["ts"])
    return last


def save_files():
    try:
        return {f for f in os.listdir(fo4test.saves_dir()) if f.lower().endswith((".fos", ".f4se"))}
    except OSError:
        return set()


def summary_text(res):
    lines = [f"FO4VR parity {res['ts']}  ({FO4VR_EXE} vs fo4flatvr; runtime {res['runtime']}; save {res['save']})"]
    if res.get("new_saves"):
        lines.append(f"  WARNING new save files in the test profile: {res['new_saves']}")
    for k, r in res["items"].items():
        tag = r["state"] + (" (REGRESSED)" if r.get("regressed") else "")
        lines.append(f"  {tag:<20} {k:<12} {ITEMS[k]['title']}")
        for c in r.get("checks", []):
            if c["state"] != PASS:
                lines.append(f"      {c['state']:<12} {c['check'][:90]}: {str(c['measured'])[:110]}")
        if r.get("why"):
            lines.append(f"      {r['why']}")
    lines.append(f"  details: {res.get('path', '')}")
    return "\n".join(lines)


def run(names, attach=False, keep=False, prove=None, no_break=False):
    os.makedirs(RUNS, exist_ok=True)
    res = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"), "save": fo4test.latest_save(), "runtime": "SteamVR + vremu", "items": {}, "prove": prove}
    if os.path.exists(fo4test.HOLD):
        raise SystemExit(f"{fo4test.HOLD} exists - not running")
    pid = game_pid()   # 22 Nov: refuse before any result file is written (an aborted run overwrote latest.json / summary.txt)
    if pid:
        try:
            own = int(open(PIDF).read().strip())
        except (OSError, ValueError):
            own = None
        if pid != own:
            raise SystemExit(f"Fallout4 pid {pid} is running and is not this suite's session - not touching it")
    todo = []
    for n in names:
        it = ITEMS[n]
        off = config_state(it)
        if off:
            res["items"][n] = {"state": OFF, "why": "; ".join(off)}
        elif not it.get("test"):
            res["items"][n] = {"state": SETUP, "why": "test not built yet (proposed in the evidence file)"}
        else:
            todo.append(n)
    if todo:
        ok, why = ensure_vremu()
        if not ok:
            for n in todo:
                res["items"][n] = {"state": SETUP, "why": why}
            todo = []
    prev = last_states()
    sess = None
    saves0 = save_files()
    try:
        if todo:
            sess = Session(attach=attach).__enter__()
            neutral()
            ctx = {"session": sess}
            for n in todo:
                it = ITEMS[n]
                say(f"--- {n}")
                broke = []
                if prove == n:
                    for c in ([] if no_break else it.get("break", [])):
                        broke.append((c, sess.cmd(c)))
                    if it.get("inject"):
                        ctx["inject"] = it["inject"]
                        ctx["inject_log"] = []
                try:
                    checks = it["test"](sess, ctx)
                    r = {"state": aggregate(checks), "checks": checks}
                except Exception as e:  # noqa: BLE001
                    r = {"state": ERROR, "why": f"{type(e).__name__}: {e}", "checks": []}
                finally:
                    for c in it.get("restore", []) if (prove == n and not no_break) else []:
                        sess.cmd(c)
                    if ctx.pop("inject", None):
                        r["injected"] = ctx.pop("inject_log", [])
                        if it.get("inject_restore"):
                            it["inject_restore"](sess)
                if broke:
                    r["broken_with"] = broke
                p = prev.get(n)
                if p and p[0] == PASS and r["state"] in (FAIL, GAP) and not prove:
                    r["state"] = FAIL
                    r["regressed"] = f"passed on {p[1]}"
                res["items"][n] = r
                say(f"{n}: {r['state']}" + (f" ({r.get('why')})" if r.get("why") else ""))
                for c in r["checks"]:
                    say(f"    {c['state']:<12} {c['check']}: {c['measured']} (FO4VR: {c['expected']})")
            if "menus_detail" in ctx:
                res["menus_detail"] = ctx["menus_detail"]
            if "vats_runs" in ctx:
                res["vats_runs"] = ctx["vats_runs"]
            if "dialogue_detail" in ctx:
                res["dialogue_detail"] = ctx["dialogue_detail"]
            if "aim_shots" in ctx:
                res["aim_shots"] = ctx["aim_shots"]
    finally:
        if sess:
            try:
                neutral()
            except OSError:
                pass
            if keep:
                sess.close()
            else:
                sess.quit()
        new = sorted(save_files() - saves0)
        if new:      # agent-rules: never leave a save in the test profile; rename once no game runs, and report it
            if game_pid():
                res["new_saves"] = {"files": new, "renamed": False, "note": "game still running (--keep): rename to .old-<stamp> after it exits"}
            else:
                st = time.strftime("%Y%m%d-%H%M")
                for f in new:
                    os.rename(os.path.join(fo4test.saves_dir(), f), os.path.join(fo4test.saves_dir(), f"{f}.old-{st}"))
                res["new_saves"] = {"files": new, "renamed": True}
        stamp = time.strftime("%Y%m%d-%H%M%S")
        dest = PROOFS if prove else RUNS
        os.makedirs(dest, exist_ok=True)
        path = os.path.join(dest, f"{('prove-' + prove + ('-nobreak' if no_break else '') + '-') if prove else ''}{stamp}.json")
        res["path"] = os.path.relpath(path, ROOT)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1, default=str)
        txt = summary_text(res)
        if not prove:
            with open(os.path.join(OUT, "latest.json"), "w", encoding="utf-8") as f:
                json.dump(res, f, indent=1, default=str)
            with open(os.path.join(OUT, "summary.txt"), "w", encoding="utf-8") as f:
                f.write(txt + "\n")
            new = not os.path.exists(HISTORY)
            with open(HISTORY, "a", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                if new:
                    w.writerow(["ts", "item", "state", "checks_not_pass", "run"])
                for k, r in res["items"].items():
                    w.writerow([res["ts"], k, r["state"], sum(1 for c in r.get("checks", []) if c["state"] != PASS), res["path"]])
        print(txt)
    return res


def cmd_check(names):
    ok, why = (vremu_up(), "vremu answering") if vremu_up() else (False, "vremu not answering (run starts SteamVR in emulator mode if nothing runs)")
    print(f"{'ok ' if ok else 'NO '} SteamVR + vremu: {why}")
    print(f"{'ok ' if not os.path.exists(fo4test.HOLD) else 'NO '} no hold file ({fo4test.HOLD})")
    pid = game_pid()
    print(f"{'ok ' if not pid else 'NO '} no Fallout4 running" + (f" (pid {pid})" if pid else ""))
    print(f"    MCM layer: {mcm_ini()}; user ini: {fo4test.effective_ini()}")
    for n in names:
        it = ITEMS[n]
        off = config_state(it)
        built = "built" if it.get("test") else "proposed"
        print(f"  {n:<12} {built:<9} {'OFF: ' + '; '.join(off) if off else 'on'}   evidence/{it['evidence']}")


def cmd_history(n=8):
    if not os.path.exists(HISTORY):
        print("no runs yet")
        return
    rows = list(csv.DictReader(open(HISTORY, encoding="utf-8")))
    runs = sorted({r["ts"] for r in rows})[-n:]
    items = list(dict.fromkeys(r["item"] for r in rows))
    print(f"{'item':<12} " + " ".join(f"{t[5:16]:>12}" for t in runs))
    for it in items:
        cells = []
        for t in runs:
            st = next((r["state"] for r in rows if r["ts"] == t and r["item"] == it), "")
            cells.append(f"{st:>12}")
        print(f"{it:<12} " + " ".join(cells))


def selftest():
    """Offline checks of the suite's own logic (no game, no SteamVR)."""
    import tempfile
    ok = []
    d = tempfile.mkdtemp(prefix="parity_selftest_")
    f = os.path.join(d, "t.ini")
    open(f, "w").write("[Controllers]" + chr(10) + "bAimWithController=0 ; comment" + chr(10) + "[General]" + chr(10) + "bEnabled=1" + chr(10))
    ok.append(("ini value with a trailing comment", _ini_value(f, "controllers", "BAIMWITHCONTROLLER") == "0"))
    ok.append(("ini missing key", _ini_value(f, "General", "nope") is None))
    ok.append(("aggregate: worst state wins", aggregate([check("a", PASS, 1, 1), check("b", GAP, 1, 1), check("c", SETUP, 1, 1)]) == GAP))
    ok.append(("aggregate: FAIL beats GAP", aggregate([check("a", GAP, 1, 1), check("b", FAIL, 1, 1)]) == FAIL))
    ok.append(("judge", judge(True, False) == PASS and judge(False, True) == FAIL and judge(False, False) == GAP))
    k = kv("weapon | form=0005BBA4 name='Hunting Rifle' instance=1 flags=0x00720100 hasScope=1 zoom=000AF2A9 | weapons=1 scopeMenu=0")
    ok.append(("kv: aim weapon readout", k.get("hasScope") == "1" and k.get("name") == "Hunting Rifle" and k.get("scopeMenu") == "0"))
    ok.append(("angdiff wraps", abs(angdiff(359.0, 1.0) + 2.0) < 1e-9))
    sk = {"body": [], "face": []}
    for face, name, cull, anc, dd, nl, _f in SKIN_ENTRY.findall(
            "facediag skin: frame 9 | root cull 0 | 'CBBE' cull 0 drawn 9 (+0) bones 66/66 notLive 0 far 0 | F:'FemaleHeadHuman' cull 0^ "
            "drawn 7 (-2) bones 10/10 notLive 0 far 0 | F:'HairFemale01' cull 0^ drawn 7 (-2) bones 11/11 notLive 0 far 0"):
        sk["face" if face else "body"].append((name, int(cull), anc == "^", int(dd), int(nl)))
    ok.append(("vats: a culled FaceGen node is 'not drawn'", head_drawn(sk)[0] is False and len(sk["face"]) == 2))
    sk["face"] = [(n, c, False, d, nl) for n, c, _a, d, nl in sk["face"]]
    ok.append(("vats: head drawn 2 frames behind the body counts as drawn", head_drawn(sk)[0] is True))
    ok.append(("every item has an evidence file", all(os.path.exists(os.path.join(EVID, it["evidence"])) for it in ITEMS.values())))
    ok.append(("every built item has a config switch list", all("switches" in it for it in ITEMS.values() if it.get("test"))))
    for n, v in ok:
        print(f"  {'ok ' if v else 'BAD'} {n}")
    return all(v for _, v in ok)


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    names = [x for x in a[1:] if not x.startswith("--")]
    for n in names:
        if n not in ITEMS and cmd not in ("history",):
            raise SystemExit(f"unknown item {n}; one of {', '.join(ITEMS)}")
    if cmd == "list":
        for k, it in ITEMS.items():
            print(f"{k:<12} {'built' if it.get('test') else 'proposed':<9} {it['title']}  (evidence/{it['evidence']})")
    elif cmd == "check":
        cmd_check(names or list(ITEMS))
    elif cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    elif cmd == "history":
        cmd_history(int(names[0]) if names else 8)
    elif cmd == "run":
        run(names or ORDER, attach="--attach" in a, keep="--keep" in a)
    elif cmd == "prove":
        if len(names) != 1:
            raise SystemExit("prove <item>")
        run(names, attach="--attach" in a, keep="--keep" in a, prove=names[0], no_break="--no-break" in a)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
