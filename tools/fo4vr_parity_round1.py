"""FO4VR parity tests ROUND 1 (21 Nov pilot; the user dropped these items 22 Nov - kept for reference; round 2 = fo4vr_parity.py).

Each test drives the game through the existing pipe probes and vremu, measures numbers, and compares them with the
Fallout 4 VR 1.2.72 reference read from Fallout4VR.exe (addresses and defaults in REF below). No plugin changes:
the probes used are `comfort`, `gun`, `gun cfg kickreset`, `sneakstep`, `pipboy`, `menus`, `pad`, `aim equip0`,
`console player.additem`. Measure, don't judge: no captures, no visual checks, no benchmarking.

Tests (names for `run`):
  turning     snap angle per flick, smooth-turn rate, default turn mode      (VR FCF5E0, iRotationType / fRotationAngle / fRotationSpeed)
  vignette    comfort vignette shown while moving and while turning, off at rest (bUseComfortVignetteWhileMoving/Turning:VR)
  kickback    per-shot gun kickback amplitude / duration from the aim-model recoil, (1-t/d)^2 envelope (VR EF96B0, EFD1C0)
  stance      eye height step on a sneak toggle: ratio 0.7 and same-frame step (VR EF7180, fComfortSneakHeight / bComfortSneak)
  pipboy      wrist Pip-Boy turns on by looking at it and grows 1x -> 3x (VR EF67B0 / EF6EC0, bPipboyDisableWhenNotLooking:VRUI)

Environment: SteamVR in vremu emulator mode (HTTP API 127.0.0.1:8766) - controller sticks / triggers / poses are injected
there (the comfort filter reads the XR controllers, not the pipe pad override). MO2 profile TestProfileVR with its latest
save (fo4test.latest_save()). Never saves. Refuses while Fallout4 runs (unless it is this tool's own session), while
work/fo4test.hold exists, while the fo4flatvr MO2 mod is disabled, or while vremu does not answer (never starts SteamVR).

    python -I fo4vr_parity_round1.py check                 # preconditions only (no launch)
    python -I fo4vr_parity_round1.py selftest              # offline: the analysis functions on synthetic samples
    python -I fo4vr_parity_round1.py run [test ...]        # launch, load the latest save, run (default: all 5), quit
    python -I fo4vr_parity_round1.py run --keep [test ...] # leave the session running (pid in work/parity/session.pid)
    python -I fo4vr_parity_round1.py attach [test ...]     # run on this tool's still-running session
Results: work/parity/parity-<ts>.json (+ a summary on stdout).
"""
import json
import math
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test  # noqa: E402
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402

ROOT = fo4test.ROOT
OUT = os.path.join(ROOT, "work", "parity")
PIDF = os.path.join(OUT, "session.pid")
STEAMVR_JSON = fo4test.STEAMVR_JSON
VREMU = "http://127.0.0.1:8766"
PISTOL_10MM = "00004822"
AMMO_10MM = "0001F276"

# FO4VR 1.2.72 reference (analysis/fo4vr-functions/items.md; setting = Setting object rva, value at +8)
REF = {
    "turn": {"iRotationType": 1, "fRotationAngle": 45.0, "fRotationSpeed": 80.0, "src": "VR FCF5E0; settings 37D5E70/37D5E88/37D5EA0"},
    "vignette": {"bUseComfortVignetteWhileMoving": 1, "bUseComfortVignetteWhileTurning": 1, "src": "settings 37D5F18/37D5F00"},
    "kick": {"minOff": 2.0, "maxOff": 20.0, "minDur": 0.25, "maxDur": 2.8, "exp": 2.0, "src": "VR EF96B0 (on fire), EFD1C0 (per frame, powf exp const 142C7FA70 = 2.0)"},
    "stance": {"fComfortSneakHeight": 0.7, "bComfortSneak": 1, "fDefaultSeatedHMDZHeight": 118.0, "src": "VR EF7180; settings 37CFC70/37D6170/37CF628"},
    "pipboy": {"bPipboyDisableWhenNotLooking": 1, "fPipboyMinScale": 1.0, "fPipboyMaxScale": 3.0, "src": "VR EF67B0 / EF6EC0; settings 37B41B8/37CF028/37CF040"},
}

HMD0 = "/pose?dev=hmd&pos=0,1.2,0&ypr=0,0,0"           # seated head height (vremu metres)
HAND_R0 = "/pose?dev=right&pos=0.2,1.0,-0.35&ypr=0,39,0"
HAND_L0 = "/pose?dev=left&pos=-0.2,1.0,-0.35&ypr=0,39,0"


# ----------------------------------------------------------------------------------------- environment
def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def vremu_up():
    try:
        vremu("/state")
        return True
    except OSError:
        return False


def preconditions(launch=True):
    """[(name, ok, detail)]; launch=False: for attach (game must be ours)."""
    out = []
    out.append(("no hold file", not os.path.exists(fo4test.HOLD), fo4test.HOLD))
    en = fo4test.mod_enabled(fo4test.MOD_FO4FLATVR)
    out.append((f"MO2 mod {fo4test.MOD_FO4FLATVR} enabled in {fo4test.MO2_PROFILE}", en, fo4test.MODLIST))
    out.append(("vremu answering (SteamVR emulator mode; never started here)", vremu_up(), VREMU))
    pid = game_pid()
    if launch:
        out.append(("no Fallout4 running", not pid, f"pid {pid}" if pid else ""))
        out.append(("no xmake build running", not fo4test.xmake_running(), ""))
    else:
        own = None
        try:
            own = int(open(PIDF).read().strip())
        except (OSError, ValueError):
            pass
        out.append(("running Fallout4 is this tool's session", bool(pid) and pid == own, f"running {pid}, ours {own}"))
    return out


def launch_session():
    os.makedirs(OUT, exist_ok=True)
    save = fo4test.latest_save()
    g = Game(runtime_json=STEAMVR_JSON, quiet=True)
    g.__enter__()
    open(PIDF, "w").write(str(game_pid()))
    say(f"loading latest save '{save}' (never saved)")
    g.cmd(f"console load {save}")
    time.sleep(2.0)
    g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
    time.sleep(4.0)
    neutral()
    return g


def attach_session():
    pid = game_pid()
    try:
        own = int(open(PIDF).read().strip())
    except (OSError, ValueError):
        own = None
    if not pid or pid != own:
        raise SystemExit(f"Fallout4 pid {pid} is not this tool's session ({own}) - not touching it")
    g = Game(attach=True, quiet=True)
    g.__enter__()
    return g


def neutral():
    vremu("/stopall")
    vremu(HMD0)
    vremu(HAND_R0)
    vremu(HAND_L0)
    time.sleep(0.8)


def kv(text):
    return {k: v for k, v in re.findall(r"(\w+)=([-\w.:']+)", text)}


def fnum(d, k, default=None):
    try:
        return float(str(d.get(k)).strip("'"))
    except (TypeError, ValueError):
        return default


def check(name, measured, expected, ok, note=""):
    return {"check": name, "measured": measured, "expected": expected, "ok": bool(ok), "note": note}


def verdict(checks, inconclusive=None):
    if inconclusive:
        return "INCONCLUSIVE"
    return "PASS" if checks and all(c["ok"] for c in checks) else "FAIL"


def sample(g, cmd, seconds, keys, every=0.0):
    """[(t, {key: float})] polled over the pipe for `seconds` (t from the first sample)."""
    out, t0 = [], time.perf_counter()
    while True:
        t = time.perf_counter() - t0
        if t > seconds:
            return out
        d = kv(g.cmd(cmd, quiet=True))
        out.append((t, {k: fnum(d, k) for k in keys}))
        if every:
            time.sleep(every)


# ----------------------------------------------------------------------------------------- analysis (pure)
def analyse_kick(samples, ref=REF["kick"]):
    """samples: [(t, {kickOn, kickD, kickAmp, kickDur, kickK, kicks})] from before the shot until after it ends.
    Checks the FO4VR EF96B0 / EFD1C0 model: amp = lerp(minOff, maxOff, k), dur = lerp(minDur, maxDur, k) (both clamped),
    d(t) = amp (1 - t/dur)^2, end at t = dur."""
    on = [(t, s) for t, s in samples if s.get("kickOn") == 1]
    checks = []
    if len(on) < 5:
        return checks, f"only {len(on)} samples with kickOn=1"
    t_first, s0 = on[0]
    k, amp, dur = s0["kickK"], s0["kickAmp"], s0["kickDur"]
    exp_amp = min(max(ref["minOff"] + (ref["maxOff"] - ref["minOff"]) * k, ref["minOff"]), ref["maxOff"])
    exp_dur = min(max(ref["minDur"] + (ref["maxDur"] - ref["minDur"]) * k, ref["minDur"]), ref["maxDur"])
    checks.append(check("amplitude = lerp(2, 20, k)", round(amp, 3), round(exp_amp, 3), abs(amp - exp_amp) <= 0.02 + 0.002 * exp_amp, f"k={k:.3f}"))
    checks.append(check("duration = lerp(0.25, 2.8, k) s", round(dur, 3), round(exp_dur, 3), abs(dur - exp_dur) <= 0.01))
    # envelope: invert d = amp (1 - t/dur)^p for the elapsed time and compare its slope with wall-clock time
    pts = [(t, dur * (1.0 - (s["kickD"] / amp) ** (1.0 / ref["exp"]))) for t, s in on if amp > 0 and 0.02 * amp < s["kickD"] < 0.98 * amp]
    if len(pts) >= 5:
        n = len(pts)
        mt = sum(p[0] for p in pts) / n
        me = sum(p[1] for p in pts) / n
        var = sum((p[0] - mt) ** 2 for p in pts)
        slope = sum((p[0] - mt) * (p[1] - me) for p in pts) / var if var > 0 else float("nan")
        resid = math.sqrt(sum((p[1] - (me + slope * (p[0] - mt))) ** 2 for p in pts) / n)
        checks.append(check("envelope (1-t/d)^2: elapsed-time slope", round(slope, 3), "1.00 +- 0.15", abs(slope - 1.0) <= 0.15, f"{n} points"))
        checks.append(check("envelope residual (s)", round(resid, 4), "<= 0.05", resid <= 0.05))
    else:
        checks.append(check("envelope points", len(pts), ">= 5", False))
    last_on = on[-1][0]
    off_after = [t for t, s in samples if t > last_on and s.get("kickOn") == 0]
    if off_after:
        active = off_after[0] - t_first
        checks.append(check("kick ends at duration (s, first-seen-on to first-seen-off)", round(active, 3), round(dur, 3), abs(active - dur) <= 0.15))
    else:
        checks.append(check("kick ended within the sampling window", False, True, False))
    return checks, None


def analyse_stance(samples, toggle_t, ref=REF["stance"]):
    """samples: [(t, {eyeH, sneaking})]; toggle_t: time of the sneak press. Measures the stand level before, the sneak
    level after it settles, the ratio, and how long the eye height took to cover 90 % of the change after the game's
    sneak flag flipped (FO4VR: same frame)."""
    before = [s["eyeH"] for t, s in samples if t < toggle_t and s.get("sneaking") == 0 and s["eyeH"] and s["eyeH"] > 0]
    flips = [t for t, s in samples if t >= toggle_t and s.get("sneaking") == 1]
    checks = []
    if len(before) < 3 or not flips:
        return checks, f"stand samples {len(before)}, sneak flag seen {bool(flips)}"
    stand = sorted(before)[len(before) // 2]
    t_flip = flips[0]
    after = [(t, s["eyeH"]) for t, s in samples if t >= t_flip and s.get("sneaking") == 1]
    tail = [h for t, h in after if t >= t_flip + 1.0] or [after[-1][1]]
    sneak = sorted(tail)[len(tail) // 2]
    ratio = sneak / stand if stand else float("nan")
    target = stand + 0.9 * (sneak - stand)
    reach = next((t for t, h in after if (h - target) * (sneak - stand) >= 0), None)
    step_s = (reach - t_flip) if reach is not None else float("nan")
    checks.append(check("sneak / stand eye height ratio (fComfortSneakHeight)", round(ratio, 3), ref["fComfortSneakHeight"], abs(ratio - ref["fComfortSneakHeight"]) <= 0.02,
                        f"stand {stand:.1f} u, sneak {sneak:.1f} u"))
    checks.append(check("time from sneak flag to 90 % of the step (s)", round(step_s, 3), "<= 0.05 (FO4VR: same frame)", step_s <= 0.05))
    return checks, None


def analyse_turn(snap, smooth, default_mode, ref=REF["turn"]):
    """snap: (snaps, snapDegTotal) for one flick; smooth: (smoothDegTotal, smoothSec)."""
    checks = [check("default turn mode (iRotationType 1 = snap)", default_mode, "snap", default_mode == "snap")]
    n, deg = snap
    per = deg / n if n else float("nan")
    checks.append(check("snaps per flick", n, 1, n == 1))
    checks.append(check("snap angle (deg, fRotationAngle)", round(per, 2), ref["fRotationAngle"], abs(abs(per) - ref["fRotationAngle"]) <= 0.5))
    sd, ss = smooth
    rate = sd / ss if ss else float("nan")
    checks.append(check("smooth turn rate at full stick (deg/s, fRotationSpeed)", round(rate, 1), ref["fRotationSpeed"], abs(abs(rate) - ref["fRotationSpeed"]) <= 4.0))
    return checks


def analyse_vignette(rest, moving, turning, after):
    """each: list of alpha samples; after: [(t, alpha)] after releasing the stick."""
    checks = [check("alpha at rest", round(max(rest or [0.0]), 3), 0.0, max(rest or [1.0]) == 0.0)]
    checks.append(check("vignette while moving (max alpha)", round(max(moving or [0.0]), 3), "> 0.05", max(moving or [0.0]) > 0.05))
    checks.append(check("vignette while turning (max alpha)", round(max(turning or [0.0]), 3), "> 0.05", max(turning or [0.0]) > 0.05))
    off = next((t for t, a in after if a == 0.0), None)
    checks.append(check("vignette gone after the stick is released (s)", off, "<= 1.5 (FO4VR fade timing not located: info)", off is not None and off <= 1.5))
    return checks


def analyse_pipboy(opened_by_look, closed_by_look_away, width_look, width_away, ref=REF["pipboy"]):
    checks = [check("wrist Pip-Boy turns on when looked at (bPipboyDisableWhenNotLooking=1)", opened_by_look, True, opened_by_look is True)]
    checks.append(check("wrist Pip-Boy turns off when not looked at", closed_by_look_away, True, closed_by_look_away is True))
    ratio = (width_look / width_away) if (width_look and width_away) else None
    checks.append(check("screen scale looked-at / not looked-at (fPipboyMax/MinScale)", ratio, ref["fPipboyMaxScale"] / ref["fPipboyMinScale"],
                        ratio is not None and abs(ratio - 3.0) <= 0.1))
    return checks


# ----------------------------------------------------------------------------------------- tests (game)
def comfort(g):
    return kv(g.cmd("comfort", quiet=True))


def world_view_ok(g):
    c = comfort(g)
    return c.get("live") == "1", c


def test_turning(g):
    ok, c0 = world_view_ok(g)
    if not ok:
        return {"result": "INCONCLUSIVE", "why": "comfort path not live (no head-tracked world view)", "comfort": c0}
    default_mode = c0.get("turn")
    try:
        g.cmd("comfort turn snap", quiet=True)
        neutral()
        g.cmd("comfort stats reset", quiet=True)
        vremu("/input?dev=right&joy=1,0&ms=350")   # one flick, held 350 ms (one snap; re-arm needs release)
        time.sleep(1.2)
        c = comfort(g)
        snap = (int(fnum(c, "snaps", 0)), fnum(c, "snapDegTotal", 0.0))
        g.cmd("comfort turn smooth", quiet=True)
        neutral()
        g.cmd("comfort stats reset", quiet=True)
        vremu("/input?dev=right&joy=1,0&ms=2000")
        time.sleep(2.6)
        c = comfort(g)
        smooth = (fnum(c, "smoothDegTotal", 0.0), fnum(c, "smoothSec", 0.0))
    finally:
        g.cmd(f"comfort turn {default_mode if default_mode in ('snap', 'smooth', 'game') else 'smooth'}", quiet=True)
        neutral()
    checks = analyse_turn(snap, smooth, default_mode)
    return {"result": verdict(checks), "checks": checks, "raw": {"snap": snap, "smooth": smooth, "default": default_mode}}


def test_vignette(g):
    ok, c0 = world_view_ok(g)
    if not ok:
        return {"result": "INCONCLUSIVE", "why": "comfort path not live", "comfort": c0}
    if c0.get("vignette") != "1":
        checks = [check("vignette enabled", 0, 1, False)]
        return {"result": "FAIL", "checks": checks}
    neutral()
    rest = [s["alpha"] for _, s in sample(g, "comfort", 1.0, ["alpha"])]
    vremu("/input?dev=left&joy=0,1&ms=1500")
    moving = [s["alpha"] for _, s in sample(g, "comfort", 1.4, ["alpha"])]
    time.sleep(0.2)
    after = [(round(t, 3), s["alpha"]) for t, s in sample(g, "comfort", 2.0, ["alpha"])]
    mode = c0.get("turn")
    try:
        g.cmd("comfort turn smooth", quiet=True)
        vremu("/input?dev=right&joy=1,0&ms=1000")
        turning = [s["alpha"] for _, s in sample(g, "comfort", 0.9, ["alpha"])]
    finally:
        g.cmd(f"comfort turn {mode if mode in ('snap', 'smooth', 'game') else 'smooth'}", quiet=True)
        neutral()
    checks = analyse_vignette(rest, moving, turning, after)
    return {"result": verdict(checks), "checks": checks, "raw": {"rest": rest[:20], "moving_max": max(moving or [0]), "after": after[:60]}}


def test_kickback(g):
    neutral()
    vremu("/pose?dev=right&pos=0.2,1.0,-0.35&ypr=0,80,0")   # muzzle up into the sky: no NPC is hit
    g.cmd(f"console player.additem {PISTOL_10MM} 1", quiet=True)
    g.cmd(f"console player.additem {AMMO_10MM} 50", quiet=True)
    time.sleep(1.0)
    g.cmd(f"aim equip0 {PISTOL_10MM}", quiet=True)
    time.sleep(1.5)
    vremu("/input?dev=right&trigger=1&ms=200")                # draw
    time.sleep(3.0)
    d = kv(g.cmd("gun", quiet=True))
    if d.get("active") not in ("1", "true"):
        return {"result": "INCONCLUSIVE", "why": f"floating gun not active (weapon '{d.get('weapon')}', gate {d.get('gate')})"}
    g.cmd("gun cfg kickreset 0", quiet=True)
    kicks0 = int(fnum(d, "kicks", 0))
    keys = ["kickOn", "kickD", "kickAmp", "kickDur", "kickK", "kicks"]
    pre = sample(g, "gun", 0.2, keys)
    vremu("/input?dev=right&trigger=1&ms=120")
    post = sample(g, "gun", 3.4, keys)
    samples = [(t - 0.2, s) for t, s in pre] + [(t, s) for t, s in post]
    kicks1 = int(post[-1][1].get("kicks") or 0) if post else kicks0
    checks = [check("one kick per shot", kicks1 - kicks0, 1, kicks1 - kicks0 == 1)]
    c2, why = analyse_kick(post)
    checks += c2
    neutral()
    return {"result": "INCONCLUSIVE" if why else verdict(checks), "why": why, "checks": checks,
            "raw": {"n_samples": len(post), "first_on": next(({**s, "t": round(t, 3)} for t, s in post if s.get("kickOn") == 1), None)}}


def test_stance(g):
    neutral()
    s0 = kv(g.cmd("sneakstep", quiet=True))
    if s0.get("on") != "1":
        return {"result": "FAIL", "checks": [check("[Roomscale] bSneakHeightStep on", s0.get("on"), 1, False)]}
    if s0.get("sneaking") == "1":
        g.cmd("pad ls=1 ms=150", quiet=True)
        time.sleep(2.0)
    keys = ["eyeH", "sneaking"]
    pre = sample(g, "sneakstep", 1.0, keys)
    g.cmd("pad ls=1 ms=150", quiet=True)     # Sneak = left stick click (pad override, 150 ms)
    post = sample(g, "sneakstep", 2.5, keys)
    samples = pre + [(1.0 + t, s) for t, s in post]
    checks, why = analyse_stance(samples, 1.0)
    g.cmd("pad ls=1 ms=150", quiet=True)     # back to standing
    time.sleep(1.5)
    info = {"stand_eyeH": next((s["eyeH"] for t, s in pre), None), "FO4VR seated HMD height (fDefaultSeatedHMDZHeight)": 118.0,
            "readout": g.cmd("sneakstep", quiet=True)}
    return {"result": "INCONCLUSIVE" if why else verdict(checks), "why": why, "checks": checks, "info": info}


def pip_state(g):
    p = g.cmd("pipboy", quiet=True)
    m = g.cmd("menus", quiet=True)
    w = re.search(r"\bwidth=([-\d.]+)", p)
    shown = re.search(r"\bshown=([0-9A-Fa-f]+)", p)
    return {"menu_open": "PipboyMenu" in m, "width": float(w.group(1)) if w else None, "shown": shown.group(1) if shown else None}


def test_pipboy(g):
    neutral()
    st0 = pip_state(g)
    if st0["menu_open"]:
        return {"result": "INCONCLUSIVE", "why": "Pip-Boy already open"}
    # look at the left wrist: hand raised in front of the face, back of the hand toward the eye, head pitched down 35 deg
    vremu("/pose?dev=left&pos=-0.05,1.0,-0.3&ypr=0,0,90")
    vremu("/pose?dev=hmd&pos=0,1.2,0&ypr=-5,-35,0")
    time.sleep(2.0)
    st_look = pip_state(g)
    vremu("/pose?dev=hmd&pos=0,1.2,0&ypr=90,0,0")      # look 90 deg away
    time.sleep(2.0)
    st_away = pip_state(g)
    width_look = width_away = None
    if not st_look["menu_open"]:
        # our way (button): open, then compare the screen size looked-at vs not
        vremu("/input?dev=left&trigger=1&ms=200")  # L trigger = Pip-Boy (pad B, ControllerBindings)
        time.sleep(2.0)
        vremu("/pose?dev=hmd&pos=0,1.2,0&ypr=-5,-35,0")
        time.sleep(1.0)
        width_look = pip_state(g)["width"]
        vremu("/pose?dev=hmd&pos=0,1.2,0&ypr=90,0,0")
        time.sleep(1.0)
        width_away = pip_state(g)["width"]
        if pip_state(g)["menu_open"]:
            vremu("/input?dev=left&trigger=1&ms=200")  # L trigger = Pip-Boy (pad B, ControllerBindings)
            time.sleep(1.5)
    else:
        width_look, width_away = st_look["width"], st_away["width"]
    neutral()
    checks = analyse_pipboy(st_look["menu_open"], not st_away["menu_open"] if st_look["menu_open"] else False, width_look, width_away)
    return {"result": verdict(checks), "checks": checks, "raw": {"look": st_look, "away": st_away}}


TESTS = {"turning": test_turning, "vignette": test_vignette, "kickback": test_kickback, "stance": test_stance, "pipboy": test_pipboy}


# ----------------------------------------------------------------------------------------- selftest (offline)
def selftest():
    """The analysis functions on synthetic samples that follow (or break) the FO4VR model."""
    res = []
    # kickback: FO4VR-exact samples at 90 Hz, k = 0.3
    k = 0.3
    amp, dur = 2 + 18 * k, 0.25 + 2.55 * k
    s = [(i / 90.0, {"kickOn": 1 if i / 90.0 < dur else 0, "kickD": amp * (1 - (i / 90.0) / dur) ** 2 if i / 90.0 < dur else 0.0,
                     "kickAmp": amp, "kickDur": dur, "kickK": k, "kicks": 1}) for i in range(0, int(90 * (dur + 0.5)))]
    c, why = analyse_kick(s)
    res.append(("kick: exact model passes", why is None and all(x["ok"] for x in c)))
    s_lin = [(t, {**v, "kickD": amp * (1 - t / dur) if v["kickOn"] else 0.0}) for t, v in s]   # linear envelope must fail
    c, why = analyse_kick(s_lin)
    res.append(("kick: linear envelope fails", why is None and not all(x["ok"] for x in c)))
    s_amp = [(t, {**v, "kickAmp": amp * 1.2}) for t, v in s]
    c, why = analyse_kick(s_amp)
    res.append(("kick: wrong amplitude fails", why is None and not all(x["ok"] for x in c)))
    # stance: step at the flag, ratio 0.7 -> pass; 0.668 ratio with a 0.6 s low-pass -> fail
    def stance_samples(ratio, tau):
        out = []
        for i in range(0, 350):
            t = i / 100.0
            sneaking = 1 if t >= 1.05 else 0
            if tau == 0:
                h = 120.0 * (ratio if sneaking else 1.0)
            else:
                h = 120.0 * (1.0 if not sneaking else ratio + (1 - ratio) * math.exp(-(t - 1.05) / tau))
            out.append((t, {"eyeH": h, "sneaking": sneaking}))
        return out
    c, why = analyse_stance(stance_samples(0.7, 0), 1.0)
    res.append(("stance: FO4VR step passes", why is None and all(x["ok"] for x in c)))
    c, why = analyse_stance(stance_samples(0.668, 0.6), 1.0)
    res.append(("stance: low-passed 0.668 fails", why is None and not all(x["ok"] for x in c)))
    c = analyse_turn((1, 45.0), (160.0, 2.0), "snap")
    res.append(("turn: FO4VR numbers pass", all(x["ok"] for x in c)))
    c = analyse_turn((1, 30.0), (240.0, 2.0), "smooth")
    res.append(("turn: 30 deg / 120 deg/s / smooth fails", not any(x["ok"] for x in c[:1] + c[2:])))
    c = analyse_vignette([0.0] * 5, [0.0, 0.4, 0.6], [0.5], [(0.1, 0.3), (0.2, 0.0)])
    res.append(("vignette: on/off passes", all(x["ok"] for x in c)))
    c = analyse_pipboy(True, True, 0.6, 0.2)
    res.append(("pipboy: look-open + 3x passes", all(x["ok"] for x in c)))
    c = analyse_pipboy(False, False, 0.2, 0.2)
    res.append(("pipboy: button + fixed size fails", not any(x["ok"] for x in c)))
    for name, ok in res:
        print(f"  {'ok ' if ok else 'BAD'} {name}")
    return all(ok for _, ok in res)


# ----------------------------------------------------------------------------------------- main
def run(names, mode):
    pre = preconditions(launch=(mode == "launch"))
    bad = [p for p in pre if not p[1]]
    for n, ok, d in pre:
        say(f"precondition {'ok ' if ok else 'NO '} {n} {d}")
    if bad:
        raise SystemExit("preconditions not met - not running (tests are ready; see above)")
    g = launch_session() if mode == "launch" else attach_session()
    results = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"), "save": fo4test.latest_save(), "ref": REF, "tests": {}}
    try:
        for n in names:
            say(f"--- {n}")
            try:
                r = TESTS[n](g)
            except Exception as e:  # a harness error is a test bug, reported as such
                r = {"result": "ERROR", "why": f"{type(e).__name__}: {e}"}
            results["tests"][n] = r
            say(f"{n}: {r['result']}" + (f" ({r.get('why')})" if r.get("why") else ""))
            for c in r.get("checks", []):
                say(f"    {'ok ' if c['ok'] else 'NO '} {c['check']}: {c['measured']} (FO4VR {c['expected']}) {c.get('note', '')}")
    finally:
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, f"parity-{time.strftime('%Y%m%d-%H%M%S')}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1, default=str)
        say(f"results -> {path}")
        if mode == "launch" and "--keep" not in sys.argv:
            g.quit()
        else:
            g._close()


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "check"
    names = [x for x in a[1:] if not x.startswith("--")] or list(TESTS)
    for n in names:
        if n not in TESTS:
            raise SystemExit(f"unknown test {n}; one of {', '.join(TESTS)}")
    if cmd == "check":
        pre = preconditions(launch=True)
        for n, ok, d in pre:
            print(f"{'ok ' if ok else 'NO '} {n} {d}")
        sys.exit(0 if all(p[1] for p in pre) else 1)
    if cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    if cmd == "run":
        run(names, "launch")
    elif cmd == "attach":
        run(names, "attach")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
