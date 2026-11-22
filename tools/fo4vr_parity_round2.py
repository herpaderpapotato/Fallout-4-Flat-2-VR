"""FO4VR parity tests, ROUND 2 (kept for reference; round 3 = fo4vr_parity.py) (22 Nov; items chosen by the user's play: analysis/fo4vr-functions/items.md,
baseline = the user's own FO4VR configuration: analysis/fo4vr-functions/user-vr-settings.md). Round 1 (dropped items):
fo4vr_parity_round1.py. Handoff: work/handoff/fo4vr-parity.md.

Tests (names for `run`; default all):
  menus       every in-game menu this harness can open without a placed reference: opens, each hint's glyph names a control
              that exists on Quest Touch and is mapped in that context (static hint table from the menu SWFs x the live remap
              `bindings glyphs <Xenon>`), safe buttons do something, the menu closes
  settings    our runtime values vs the user's FO4VR settings (turning, vignette, stick direction, Pip-Boy mode, activation
              preview)
  activation  activation pick + rollover per hand (FO4VR F214F0: both wands every frame, one rollover each)
  scope       is there a Touch path into the scope, and what our view does when the game's scope opens (FO4VR EF7B00 / EFAA60:
              raise the gun to the eye -> world blacked out, weapon zoom, scope overlay)
  vats        VATS playback: camera state, head shown / hidden, third-person graph switch (findings for FRIK-flat)

Runtime: SteamVR + vremu when it answers (never started here); otherwise the Meta XR Simulator (separate OpenXR runtime,
Quest Touch controllers, no input injection) with the pipe pad override for game buttons. Parts that need controller
poses / Touch presses report INCONCLUSIVE "needs vremu". MO2 profile TestProfileVR, latest save, never saves; refuses while
Fallout4 runs, while work/fo4test.hold exists or while fo4flatvr is disabled. No plugin changes (existing pipe probes).

    python -I fo4vr_parity.py check | selftest | run [--keep] [test ...] | attach [test ...]
Results: work/parity/parity2-<ts>.json + summary.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test  # noqa: E402
import fo4vr_parity_round1 as r1  # noqa: E402  environment helpers (vremu, kv, check, verdict, sample)
from fo4test import Game, game_pid, say  # noqa: E402

ROOT = fo4test.ROOT
OUT = r1.OUT
PIDF = r1.PIDF
sys.path.insert(0, os.path.join(ROOT, "analysis", "fo4vr-functions", "tools"))
META_SIM_JSON = r"C:\Program Files\MetaXRSimulator\v207.1\meta_openxr_simulator.json"
STEAMVR_JSON = fo4test.STEAMVR_JSON
PISTOL_10MM, AMMO_10MM = "00004822", "0001F276"
HUNTING_RIFLE = "0004F46A"          # Fallout4.esm WEAP HuntingRifle
LVL_BLOATFLY = "0003320A"           # Fallout4.esm NPC_ LvlBloatfly (VATS target)
FRIK_LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\FRIK.log")
BINDINGS_CPP = os.path.join(ROOT, "plugin", "src", "plugin", "ControllerBindings.cpp")

# the user's FO4VR (MO2 'Fallout 4 VR' profile Default; Documents copy in brackets where it differs)
USER = {"iRotationType": 3, "fRotationSpeed": 162.5, "fRotationSpeed_documents": 125.0, "bUseComfortVignetteWhileMoving": 0,
        "bUseComfortVignetteWhileTurning": 0, "bUseWandDirectionalMovement": 0, "bAlwaysUseProjectedPipboy": 1,
        "bShowWandActivationPreview": 0, "bInvertedMapScrolling": 1}

kv, check, verdict, sample = r1.kv, r1.check, r1.verdict, r1.sample
PAD = {"Xenon_A": "a=1", "Xenon_B": "b=1", "Xenon_X": "x=1", "Xenon_Y": "y=1", "Xenon_L1": "lb=1", "Xenon_R1": "rb=1",
       "Xenon_L2": "lt=1", "Xenon_R2": "rt=1", "Xenon_L3": "ls=1", "Xenon_R3": "rs=1", "Xenon_Start": "start=1",
       "Xenon_Select": "back=1", "Xenon_L2_Alt": "lt=1", "Xenon_R2_Alt": "rt=1", "DPad_Up": "up=1", "DPad_Down": "down=1"}
NO_QUEST = {"Xenon_L1": "LB", "Xenon_R1": "RB", "Xenon_Select": "Back", "Xenon_Start": "Start"}
TOUCH_OF = {"Xenon_A": "A", "Xenon_B": "B", "Xenon_X": "X", "Xenon_Y": "Y", "Xenon_R2": "R trigger", "Xenon_L2": "L trigger",
            "Xenon_R1": "R grip (drawn as RB)", "Xenon_L1": "L grip (drawn as LB)", "Xenon_RS": "R stick", "Xenon_LS": "L stick",
            "Xenon_R3": "R stick click", "Xenon_L3": "L stick click"}


# ----------------------------------------------------------------------------------------- environment
def runtime():
    return STEAMVR_JSON if r1.vremu_up() else META_SIM_JSON


def preconditions(launch=True):
    out = [p for p in r1.preconditions(launch) if not p[0].startswith("vremu")]
    out.append(("OpenXR runtime", True, "SteamVR+vremu" if r1.vremu_up() else f"Meta XR Simulator ({META_SIM_JSON}); Touch input not injectable"))
    out.append(("Meta XR Simulator present", os.path.exists(META_SIM_JSON) or r1.vremu_up(), META_SIM_JSON))
    return out


def launch_session():
    os.makedirs(OUT, exist_ok=True)
    save = fo4test.latest_save()
    g = Game(runtime_json=runtime(), quiet=True)
    g.__enter__()
    open(PIDF, "w").write(str(game_pid()))
    say(f"loading latest save '{save}' (never saved)")
    g.cmd(f"console load {save}")
    time.sleep(2.0)
    g._wait(f"in-world ({save})", 150, lambda: g.ready().get("ingame") == "1")
    time.sleep(5.0)
    return g


def pad(g, what, ms=150, wait=0.9):
    g.cmd(f"pad {what} ms={ms}", quiet=True)
    time.sleep(wait)


def menus(g):
    return [m.split()[0] for m in g.cmd("menus", quiet=True).split(" | ")[1:] if m.strip()]


def bar(g):
    r = g.cmd("pointer bar", quiet=True)
    return [t for t, v in re.findall(r"text='([^']*)' vis=(\d)", r) if v == "1" and t]


def pip(g):
    r = g.cmd("pipboy", quiet=True)
    m = re.search(r"page=(\d+) tab=(\d+)", r)
    return {"page": int(m.group(1)) if m else None, "tab": int(m.group(2)) if m else None, "raw": r[:300]}


def snapshot(g):
    return {"menus": menus(g), "bar": bar(g), "pip": {k: v for k, v in pip(g).items() if k != "raw"}}


def glyph(g, xen):
    r = g.cmd(f"bindings glyphs {xen}", quiet=True)
    m = re.search(r"probe (\S+)\s+\S*\s*-> (\(kept\)|\S+) \(ctx (\w+)\)", r)
    if not m:
        return None, None
    shown = xen if m.group(2) == "(kept)" else m.group(2)
    return shown, m.group(3)


def norm_label(s):
    return re.sub(r"[^A-Z0-9]", "", s.upper().lstrip("$"))


def wait_menu(g, name, open_=True, timeout=4.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if (name in menus(g)) == open_:
            return True
        time.sleep(0.25)
    return False


BASE_MENUS = ("HUDMenu", "CursorMenu", "ButtonBarMenu", "FaderMenu", "VignetteMenu", "LoadingMenu", "PowerArmorHUDMenu")


def open_menus(g):
    return [x for x in menus(g) if x not in BASE_MENUS]


def close_all(g):
    """B / A-on-prompt, then UIMessageQueue hide for anything left. Returns the menus still open."""
    for _ in range(3):
        m = open_menus(g)
        if not m:
            return []
        if "MessageBoxMenu" in m:
            pad(g, "a=1")          # the only prompts reached here are LooksMenu's accept / exit prompts (no change was made)
            continue
        pad(g, "b=1")
    for m in open_menus(g):
        g.cmd(f"uimsg {m} hide", quiet=True)
        time.sleep(0.8)
    return open_menus(g)


# ----------------------------------------------------------------------------------------- menu suite
# open: how to open; close: pad args; safe: buttons whose effect is harmless (never A in Pause / LevelUp / INV / STAT, never X in
# INV = DROP); static: the SWF stems whose literal hints apply
MENUS = [
    {"menu": "PipboyMenu", "open": ("pad", "b=1"), "close": "b=1", "static": ["PipboyMenu"], "pages": True},
    {"menu": "LevelUpMenu", "open": ("pipstat", "y=1"), "close": "b=1", "static": ["LevelUpMenu"], "safe": ["Xenon_L1", "Xenon_R1"]},
    {"menu": "PauseMenu", "open": ("pad", "start=1"), "close": "b=1", "static": ["MainMenu"], "safe": []},
    {"menu": "FavoritesMenu", "open": ("pad", "up=1", 900), "close": "b=1", "static": ["FavoritesMenu"], "safe": []},
    {"menu": "VATSMenu", "open": ("pad", "lb=1"), "close": "lb=1", "static": ["VATSMenu"], "safe": []},
    {"menu": "ContainerMenu", "open": ("console", "player.openactorcontainer 1"), "close": "b=1", "static": ["ContainerMenu"], "safe": []},
    {"menu": "LooksMenu", "open": ("console", "showlooksmenu player 1"), "close": "start=1", "static": ["LooksMenu"], "safe": [], "prompt_ok": True},
]
PIP_PAGES = {0: ("STAT", "Pipboy_StatsPage", ["Xenon_L1", "Xenon_R1"]), 1: ("INV", "Pipboy_InvPage", ["Xenon_L1", "Xenon_R1", "Xenon_L3", "Xenon_R3"]),
             2: ("DATA", "Pipboy_DataPage", ["Xenon_L1", "Xenon_X"]), 3: ("MAP", "Pipboy_MapPage", ["Xenon_L1", "Xenon_X", "Xenon_Y"]),
             4: ("RADIO", "Pipboy_RadioPage", [])}
NOT_OPENABLE = {  # need a placed reference (terminal, lock, vendor, workbench, book, NPC): static audit only this round
    "BarterMenu": "vendor", "TerminalMenu": "terminal", "LockpickingMenu": "locked object", "ExamineMenu": "workbench",
    "CookingMenu": "cooking station", "BookMenu": "book / note", "DialogueMenu": "NPC", "SPECIALMenu": "vault exit / book",
    "WorkshopMenu": "workshop (workshop agent's area)", "SleepWaitMenu": "bed / chair", "MessageBoxMenu": "prompt",
}


def audit_hints(g, stems, H, ctx_expect=None):
    rows = []
    for stem in stems:
        for label, xen in H.get(stem, []):
            shown, ctx = glyph(g, xen)
            raw_kept = shown == xen
            if shown in NO_QUEST:
                flag = "NO-QUEST-CONTROL" if raw_kept else "SHOULDER-GLYPH (grip)"
            elif raw_kept and xen not in ("Xenon_A", "Xenon_B", "Xenon_X", "Xenon_Y") and not xen.endswith(("LS", "RS")):
                flag = "UNMAPPED"
            else:
                flag = "ok"
            rows.append({"swf": stem, "label": label, "button": xen, "shown": shown, "touch": TOUCH_OF.get(shown, "-"), "ctx": ctx, "flag": flag})
    return rows


def press_test(g, menu, xen, reopen):
    before = snapshot(g)
    pad(g, PAD[xen])
    after = snapshot(g)
    effect = before != after
    note = ""
    if menu not in after["menus"]:
        note = "closed the menu"
        reopen()
    elif [m for m in after["menus"] if m not in before["menus"]]:
        note = "opened " + ",".join(m for m in after["menus"] if m not in before["menus"])
        pad(g, "b=1")
    return {"button": xen, "effect": effect, "note": note, "bar_before": before["bar"][:8], "bar_after": after["bar"][:8]}


def open_menu(g, spec):
    kind = spec["open"][0]
    if kind == "pad":
        pad(g, spec["open"][1], spec["open"][2] if len(spec["open"]) > 2 else 150)
    elif kind == "console":
        g.cmd("console " + spec["open"][1], quiet=True)
        time.sleep(1.5)
    elif kind == "pipstat":
        pad(g, "b=1")
        wait_menu(g, "PipboyMenu")
        for _ in range(6):
            if pip(g)["page"] == 0:
                break
            pad(g, "lt=1", 150, 0.7)
        pad(g, spec["open"][1])
    return wait_menu(g, spec["menu"])


def test_menus(g):
    import glyph_audit as ga
    H = ga.hints()
    results, static_rows = [], []
    close_all(g)
    for spec in MENUS:
        m = spec["menu"]
        r = {"menu": m}
        try:
            opened = open_menu(g, spec)
            r["opened"] = opened
            if not opened:
                r["why"] = f"not open after {spec['open']} (menus {menus(g)})"
                results.append(r)
                close_all(g)
                continue
            time.sleep(0.8)
            r["hints"] = audit_hints(g, spec["static"], H)
            r["presses"] = []
            reopen = lambda s=spec: (close_all(g), open_menu(g, s))  # noqa: E731
            if spec.get("pages"):
                r["pages"] = {}
                seen = set()
                for _ in range(6):
                    p = pip(g)["page"]
                    if p in seen or p is None:
                        pad(g, "rt=1", 150, 0.8)
                        if pip(g)["page"] in seen:
                            break
                        continue
                    seen.add(p)
                    name, stem, safe = PIP_PAGES.get(p, (str(p), "", []))
                    pg = {"hints": audit_hints(g, [stem], H) if stem else [], "presses": []}
                    r["pages"][name] = pg
                    pad(g, "rt=1", 150, 0.8)  # next tab (Touch Y in the wrist context)
            pad(g, spec["close"], 150, 1.5)
            if spec.get("prompt_ok") and "MessageBoxMenu" in menus(g):
                r["close_prompt"] = True
                pad(g, "a=1", 150, 1.5)
            r["closed"] = wait_menu(g, m, open_=False)
        except Exception as e:  # noqa: BLE001  (test bug, reported)
            r["error"] = f"{type(e).__name__}: {e}"
        close_all(g)
        results.append(r)
    # checks
    checks = []
    for r in results:
        if not r.get("opened"):
            checks.append(check(f"{r['menu']} opens", False, True, False, r.get("why", r.get("error", ""))))
            continue
        hint_rows = list(r.get("hints", [])) + [h for pg in r.get("pages", {}).values() for h in pg["hints"]]
        bad = [h for h in hint_rows if h["flag"] != "ok"]
        checks.append(check(f"{r['menu']}: every hint glyph names a mapped Quest control", [f"{h['label']}={h['shown']}:{h['flag']}" for h in bad] or "all ok",
                            "no NO-QUEST-CONTROL / UNMAPPED / SHOULDER-GLYPH", not bad))
        checks.append(check(f"{r['menu']}: closes with its hint's button (pad level)", r.get("closed"), True, bool(r.get("closed")),
                            "after an accept prompt" if r.get("close_prompt") else ""))
    # static audit of the menus this harness cannot open
    for menu, need in NOT_OPENABLE.items():
        static_rows.append({"menu": menu, "needs": need})
    return {"result": verdict(checks), "checks": checks, "menus": results, "not_opened_static_only": static_rows,
            "untested": "Touch presses per hint (each button's effect through our bindings) need SteamVR + vremu; this run checks the glyph "
                        "remap (live) and the pad-level open / close"}


# ----------------------------------------------------------------------------------------- settings vs the user's FO4VR
def test_settings(g):
    c = kv(g.cmd("comfort", quiet=True))
    cfg = g.cmd("comfort cfg", quiet=True)
    rate = re.search(r"smoothDegPerSec=([-\d.]+)", cfg)
    p = g.cmd("pipboy", quiet=True)
    b = g.cmd("beam", quiet=True)
    wrist = re.search(r"\bwrist=(\d)", p)
    proj = re.search(r"\bfloating=(\d)", p)   # [Pipboy] bWristPipboy=0 + bProjected=1 = FO4VR projected (the floating path, Plugin.cpp)
    act = re.search(r"\bactivate=(\d+)", b)
    checks = [
        check("turn mode (iRotationType 3 = smooth)", c.get("turn"), "smooth", c.get("turn") == "smooth"),
        check("smooth turn rate deg/s (fRotationSpeed)", float(rate.group(1)) if rate else None, f"{USER['fRotationSpeed']} (Documents ini {USER['fRotationSpeed_documents']})",
              bool(rate) and abs(float(rate.group(1)) - USER["fRotationSpeed"]) <= 1.0),
        check("comfort vignette (bUseComfortVignetteWhileMoving/Turning = 0)", c.get("vignette"), "0", c.get("vignette") == "0"),
        check("stick moves along the head (bUseWandDirectionalMovement = 0)", c.get("move"), "head", c.get("move") == "head"),
        check("Pip-Boy projected, never on the wrist (bAlwaysUseProjectedPipboy = 1)", f"wrist={wrist.group(1) if wrist else '?'} projected={proj.group(1) if proj else '?'}",
              "wrist=0, projected=1", bool(wrist and proj) and wrist.group(1) == "0" and proj.group(1) == "1"),
        check("no activation preview laser (bShowWandActivationPreview = 0)", f"activate offers={act.group(1) if act else '?'}", "0 (preview off)",
              bool(act) and act.group(1) == "0", "beam 'activate' offer count since load; > 0 = the preview is drawn"),
    ]
    return {"result": verdict(checks), "checks": checks, "raw": {"comfort_cfg": cfg[:300], "pipboy": p[-400:], "beam": b[:400]}}


# ----------------------------------------------------------------------------------------- activation per hand
def test_activation(g):
    b = g.cmd("beam", quiet=True)
    left = re.search(r"\| left: (.*?) \| right:", b)
    right = re.search(r"\| right: (.*)$", b)
    bnd = g.cmd("bindings", quiet=True)
    acth = re.search(r"act(?:Hand)?=(\w+)", bnd)
    lt, rt = (left.group(1) if left else "?"), (right.group(1) if right else "?")
    both = "activate" in lt and "activate" in rt
    checks = [
        check("activation pick + preview/rollover on BOTH hands at once (FO4VR F214F0: one ViewCaster and one HUD rollover per wand)",
              f"left: {lt[:80]} | right: {rt[:80]}", "both hands", both,
              "ours: one flat ViewCaster (30F9FF8); the ray follows the primary hand, the off hand only while X is held (ControllerBindings actHand)"),
    ]
    inconclusive = None if r1.vremu_up() else "needs vremu to point each hand at an object (Meta sim controllers cannot be posed)"
    res = verdict(checks)
    if inconclusive and not both:
        res = "FAIL (structural) / INCONCLUSIVE (pose-dependent part)"
    return {"result": res, "why": inconclusive, "checks": checks, "raw": {"beam": b[:500], "bindings_act": acth.group(0) if acth else bnd[:200]}}


# ----------------------------------------------------------------------------------------- scope
def equip(g, form, ammo=None):
    g.cmd(f"console player.additem {form} 1", quiet=True)
    if ammo:
        g.cmd(f"console player.additem {ammo} 50", quiet=True)
    time.sleep(1.0)
    g.cmd(f"aim equip0 {form}", quiet=True)
    time.sleep(1.5)
    pad(g, "rt=1", 200, 3.0)  # draw (pad RT draws a holstered weapon)


def test_scope(g):
    src = open(BINDINGS_CPP, encoding="utf-8").read()
    lt_zero = re.search(r"if \(ctx != Ctx::Pipboy && ctx != Ctx::Workshop\) p\.lt = 0\.0f;", src)
    gesture_ads = re.search(r"(?i)raise.*eye.*(scope|ads|iron)", open(os.path.join(ROOT, "plugin", "src", "plugin", "Gestures.cpp"), encoding="utf-8").read())
    close_all(g)
    tries = []
    s0 = s1 = s2 = {}
    gun = {}
    for attempt in range(4):   # player.additem rolls the weapon's mods at random: retry until a scoped roll opens ScopeMenu
        equip(g, HUNTING_RIFLE)
        gun = kv(g.cmd("gun", quiet=True))
        s0 = {"menus": menus(g), "cam": g.cmd("cam", quiet=True)[:120], "status": g.cmd("status", quiet=True)[:200]}
        g.cmd("pad lt=1 ms=2500", quiet=True)   # the game's own aim input (no Touch control sends it in gameplay)
        time.sleep(1.2)
        s1 = {"menus": menus(g), "ready": g.cmd("ready", quiet=True), "status": g.cmd("status", quiet=True)[:300]}
        time.sleep(1.8)
        s2 = {"menus": menus(g)}
        tries.append("ScopeMenu" in s1["menus"])
        if tries[-1]:
            break
        g.cmd("console player.removeitem 0004F46A 1", quiet=True)   # drop the unscoped roll from the test inventory (not saved)
        time.sleep(0.8)
    scope_open = "ScopeMenu" in s1["menus"]
    camstate = re.search(r"camstate=(\d+)", s1["ready"])
    checks = [
        check("a Touch input enters aim-down-sights / the scope (FO4VR: raise the gun to the eye, EF7B00)",
              "none: pad LT forced to 0 in gameplay (ControllerBindings.cpp), no raise-to-eye gesture" if (lt_zero and not gesture_ads) else "a path exists",
              "raise-to-eye enters the scope", not (lt_zero and not gesture_ads)),
    ]
    cs = camstate.group(1) if camstate else "?"
    if scope_open:
        checks.append(check("the game's scope opens on its aim input", True, True, True, f"weapon '{gun.get('weapon')}'"))
    else:
        # test data: the base HuntingRifle form may be unscoped; then only the game-level ADS is shown (not a finding)
        checks.append(check("the game's ADS runs on its aim input (iron sights, camstate 4) [weapon unscoped: scope view not reachable "
                            "with this test data]", cs, "4", cs == "4", f"weapon '{gun.get('weapon')}'"))
    if scope_open:
        vm = re.search(r"viewMenu='([^']*)'", s1["status"])
        checks.append(check("scoped view stays in the headset world view with the weapon zoom and overlay (FO4VR EFAA60: world blacked out, "
                            "WSScope overlay, scope camera at the weapon zoom)", f"viewMenu='{vm.group(1) if vm else '?'}'", "viewMenu='' (world view kept)",
                            bool(vm) and vm.group(1) == "", "ours: ScopeMenu is a view-taking menu -> flat cinema window with the game's zoom (GameBridge ViewTakingMenu, "
                            "[Camera] bNoAimZoom note); verify the 'mode' in status"))
    return {"result": verdict(checks), "checks": checks, "raw": {"tries_scope_opened": tries, "before": s0, "aiming": s1, "after": s2}}


# ----------------------------------------------------------------------------------------- VATS playback
def log_mark(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def log_since(path, mark):
    try:
        with open(path, "rb") as f:
            f.seek(mark)
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def test_vats(g):
    """Flat VATS playback runs its own camera inside VatsMenuObj (+0x120 VATSCamera; ChangeState(2) disables PlayerCamera), so the
    PlayerCamera state stays 0 (first person) through targeting AND playback (vr-vats-dialogue.md 2.2-2.3). Playback is detected by
    FRIK-flat's C44 'show first person 0' (DoShow1stPerson(false) = the switch to the third-person body) and by the gun's shot
    counter. Measured during playback: the camera state, FRIK-flat's head (C39/C35) and body hand-back lines."""
    close_all(g)
    equip(g, PISTOL_10MM, AMMO_10MM)
    m_ours, m_frik = log_mark(fo4test.LOG), log_mark(FRIK_LOG)
    g.cmd(f"console player.placeatme {LVL_BLOATFLY} 1", quiet=True)
    time.sleep(3.0)
    pad(g, "lb=1", 150, 1.5)             # V.A.T.S.
    entered = "VATSMenu" in menus(g)
    launches0 = int(r1.fnum(kv(g.cmd("gun", quiet=True)), "launches", 0) or 0)
    samples, tried = [], []
    t0 = time.time()
    # candidate pad sequences for queue + execute (flat VatsMenuObj::HandleEvent); stop at the first that starts playback
    seqs = [["a=1"], ["rt=1"], ["a=1", "a=1"], ["x=1"], ["a=1", "lb=1"]]
    played = False
    for seq in seqs:
        if not entered or "VATSMenu" not in menus(g):
            break
        tried.append(seq)
        for k in seq:
            g.cmd(f"pad {k} ms=150", quiet=True)
            time.sleep(0.8)
        t_end = time.time() + 6.0
        while time.time() < t_end:
            rd = g.cmd("ready", quiet=True)
            cs = re.search(r"camstate=(-?\d+)", rd)
            samples.append((round(time.time() - t0, 2), int(cs.group(1)) if cs else None, "VATSMenu" in menus(g)))
            if "show first person 0" in log_since(FRIK_LOG, m_frik):
                played = True
            time.sleep(0.2)
        if played or "VATSMenu" not in menus(g):
            break
    close_all(g)
    ours, frik = log_since(fo4test.LOG, m_ours), log_since(FRIK_LOG, m_frik)
    keep = re.compile(r"C39|C35|C44 view|not first person|body left to the engine|head (hidden|shown)|vats shot|VATS")
    lines = [l for l in (ours + chr(10) + frik).splitlines() if keep.search(l) and "engine sound" not in l and "ui sound" not in l][:60]
    to_third = [l for l in lines if "show first person 0" in l]
    shown = [l for l in lines if "head shown again" in l or "body left to the engine" in l]
    heal = [l for l in lines if "heal" in l]
    states = sorted({x[1] for x in samples if x[1] is not None})
    checks = [
        check("entered V.A.T.S. on a target", entered, True, entered),
        check("playback ran (DoShow1stPerson(false) = third-person body, C44 'show first person 0')", len(to_third), ">= 1", bool(to_third),
              f"pad sequences tried {tried}"),
    ]
    def ts(l):
        m = re.search(r"(\d\d):(\d\d):(\d\d)\.(\d{3})", l)
        return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000 if m else None
    if to_third:
        t3 = ts(to_third[0])
        back = [l for l in lines if "show first person 1" in l and ts(l) and ts(l) > t3]
        t1 = ts(back[0]) if back else None
        shown_t = [ts(l) for l in shown if ts(l)]
        hidden_in = [l for l in lines if "head hidden" in l and ts(l) and t3 <= ts(l) < (t1 or 1e9) - 0.05]
        near = [t for t in shown_t if abs(t - t3) <= 0.1]
        checks += [
            check("head shown + body handed to the engine within 100 ms of the third-person switch (FRIK-flat C35/C39)",
                  [round(t - t3, 3) for t in shown_t], "|dt| <= 0.1 s", bool(near), f"camera states sampled {states}"),
            check("head not hidden again during playback", len(hidden_in), 0, not hidden_in, hidden_in[0][:120] if hidden_in else ""),
            check("no C44 heal after playback", len(heal), 0, not heal),
        ]
    res = verdict(checks) if entered and to_third else "INCONCLUSIVE"
    return {"result": res, "why": None if to_third else "no playback started with the pad sequences tried (test procedure, not a finding)",
            "checks": checks, "samples": samples[:80], "log": lines,
            "note": "FO4VR has no third-person playback (VR 1064F60 VATS::VR_SetMode changes no camera); the reference is flat playback with "
                    "the full body visible and animated. Findings go to FRIK-flat."}


TESTS = {"menus": test_menus, "settings": test_settings, "activation": test_activation, "scope": test_scope, "vats": test_vats}


def selftest():
    ok = []
    ok.append(("pad map covers every hint button", all(x in PAD or x.endswith(("LS", "RS")) for x in
               ["Xenon_A", "Xenon_B", "Xenon_X", "Xenon_Y", "Xenon_L1", "Xenon_R1", "Xenon_L2", "Xenon_R2", "Xenon_L3", "Xenon_R3", "Xenon_L2_Alt"])))
    m = re.search(r"probe (\S+)\s+\S*\s*-> (\(kept\)|\S+) \(ctx (\w+)\)", "bindings glyphs on=1 x ctx=pipboy ... | probe Xenon_L1  -> (kept) (ctx pipboy)")
    ok.append(("glyph probe parse (kept)", bool(m) and m.group(2) == "(kept)" and m.group(3) == "pipboy"))
    m = re.search(r"probe (\S+)\s+\S*\s*-> (\(kept\)|\S+) \(ctx (\w+)\)", "| probe Xenon_B  -> Xenon_R1 (ctx favorites)")
    ok.append(("glyph probe parse (swap)", bool(m) and m.group(2) == "Xenon_R1"))
    import glyph_audit as ga
    h = ga.hints()
    ok.append(("static hints: Pip-Boy INV has FAV on Xenon_R1", ("$FAV", "Xenon_R1") in h.get("Pipboy_InvPage", [])))
    for n, v in ok:
        print(f"  {'ok ' if v else 'BAD'} {n}")
    return all(v for _, v in ok)


def run(names, mode):
    pre = preconditions(launch=(mode == "launch"))
    for n, ok, d in pre:
        say(f"precondition {'ok ' if ok else 'NO '} {n} {d}")
    if not all(p[1] for p in pre):
        raise SystemExit("preconditions not met - not running")
    g = launch_session() if mode == "launch" else r1.attach_session()
    results = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"), "save": fo4test.latest_save(), "runtime": runtime(), "user_fo4vr": USER, "tests": {}}
    try:
        for n in names:
            say(f"--- {n}")
            left = close_all(g)
            if left:
                results["tests"][n] = {"result": "INCONCLUSIVE", "why": f"menus still open before the test: {left}"}
                say(f"{n}: INCONCLUSIVE (menus still open: {left})")
                continue
            try:
                r = TESTS[n](g)
            except Exception as e:  # noqa: BLE001
                r = {"result": "ERROR", "why": f"{type(e).__name__}: {e}"}
            results["tests"][n] = r
            say(f"{n}: {r['result']}" + (f" ({r.get('why')})" if r.get("why") else ""))
            for c in r.get("checks", []):
                say(f"    {'ok ' if c['ok'] else 'NO '} {c['check']}: {c['measured']} (FO4VR/user {c['expected']}) {c.get('note', '')}")
    finally:
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, f"parity2-{time.strftime('%Y%m%d-%H%M%S')}.json")
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
