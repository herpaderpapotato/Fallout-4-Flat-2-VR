"""VR settings panel (ConfigUI.h / ConfigStore.h) functional check under SteamVR + vremu, MO2 profile TestProfileVR, latest save.

Checks (no visual judging):
  1  load lines: settings layers merged, panel on, FRIK-flat C41 shortcut off
  2  the merged file exists (MO2 overwrite) and holds the user's values + MCM's gesture lines; fo4flatvr.ini untouched
  3  open via the pipe; ImGui frames rendered and the panel quad submitted
  4  live rows: comfort snap angle and a camera policy preview reach the module; Discard puts them back
  5  restart row: render scale marked as needing a restart; Discard clears it
  6  Save through MCM's API: the line appears in MCM's settings file (confirmed by the store)
  7  simulated pointer: clicking a page tab changes the page
  8  controller ray (vremu /aim at the panel): the right hand owns the panel; its trigger is kept from the game pad
  9  gesture action (pipe gesture test R 7 = right Up+Down): closes / opens the panel
 10  left menu button held 1.3 s (pipe bindings menu <ms>: SteamVR's Touch binding does not pass the menu button): toggles the panel; a short press opens Pause (on release)
MCM's settings file is copied to work/cfgui before the run and copied back after the game quit (the Save test writes it).
Run: python -I plugin/tools/cfgui_session.py
"""
import os
import re
import shutil
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test as t  # noqa: E402

OUT = os.path.join(t.ROOT, "work", "cfgui")
MCM_FILE = os.path.join(t.OVERWRITE, "MCM", "Settings", "fo4flatvr.ini")
EFF_FILE = os.path.join(t.OVERWRITE, "F4SE", "Plugins", "fo4flatvr.effective.ini")
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    t.say(f"{'PASS' if ok else 'FAIL'} {name}: {detail[:300]}")


def vremu(path):
    return urllib.request.urlopen("http://127.0.0.1:8766" + path, timeout=3).read().decode()


def kv(s):
    return dict(re.findall(r"(\w+)=('[^']*'|\S+)", s))


def main():
    os.makedirs(OUT, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    mcm_backup = os.path.join(OUT, f"mcm-settings-fo4flatvr.ini.{stamp}")
    had_mcm = os.path.isfile(MCM_FILE)
    if had_mcm:
        shutil.copy2(MCM_FILE, mcm_backup)
    user_ini_before = open(t.MOD_INI, "rb").read()
    save = t.latest_save()
    try:
        with t.Game(runtime_json=t.STEAMVR_JSON, frik=True) as g:
            mark = "launch"
            g.cmd(f"console load {save}")
            time.sleep(2.0)
            g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
            time.sleep(4.0)
            log = g.log_since(mark)
            check("1 layers merged", "cfgstore: settings layers" in log and "merged file" in log,
                  next((l for l in log.splitlines() if "cfgstore: settings layers" in l), "no line"))
            check("1 panel on", "cfgui: VR settings panel on" in log, next((l for l in log.splitlines() if "cfgui: VR settings panel" in l), ""))
            flog = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\FRIK.log")
            ftxt = open(flog, encoding="utf-8", errors="replace").read() if os.path.isfile(flog) else ""
            check("1 FRIK C41 shortcut off", "C41: config shortcut (both sticks) off" in ftxt, "FRIK.log")
            st = g.cmd("cfg")
            eff = open(EFF_FILE, encoding="utf-8", errors="replace").read() if os.path.isfile(EFF_FILE) else ""
            user = open(t.MOD_INI, encoding="utf-8", errors="replace").read()
            m = re.search(r"(?m)^iWidth=(\d+)", user)
            check("2 merged file", eff.startswith("; fo4flatvr.effective.ini") and (not m or f"iWidth={m.group(1)}" in eff) and
                  ("iRUpAction=" in eff or not had_mcm), f"{EFF_FILE} {len(eff)} bytes; cfg: {st}")
            # 3 open
            g.cmd("cfgui open")
            time.sleep(2.0)
            s = kv(g.cmd("cfgui status"))
            check("3 open + rendered + submitted", s.get("open") == "1" and s.get("imgui") == "ok" and int(s.get("frames", 0)) > 10 and
                  int(s.get("submits", 0)) > 10, str({k: s.get(k) for k in ("open", "imgui", "frames", "submits", "anchored")}))
            # 4 live rows
            r = g.cmd("cfgui set Comfort fSnapTurnDeg 45")
            c = g.cmd("comfort cfg")
            check("4 live: comfort snap angle preview", "snapDeg=45 " in c, f"{r} | {c[:120]}")
            r = g.cmd("cfgui set Camera sPolicyThirdPerson CINEMA")
            tb = g.cmd("camdir table")
            check("4 live: camera policy preview", "ThirdPerson=CINEMA" in tb, f"{r} | {tb[:300]}")
            effp = open(EFF_FILE, encoding="utf-8", errors="replace").read()
            check("4 preview in the merged file", "fSnapTurnDeg=45" in effp and "sPolicyThirdPerson=CINEMA" in effp, "")
            g.cmd("cfgui discard")
            c2 = g.cmd("comfort cfg")
            tb2 = g.cmd("camdir table")
            check("4 discard restores", "snapDeg=45 " not in c2 and "snapDeg=" in c2, c2[:120])
            check("4 discard restores policy", "ThirdPerson=CINEMA" not in tb2, tb2[:300])
            # 5 restart row
            g.cmd("cfgui set Render fRenderScale 1.1")
            s = kv(g.cmd("cfgui status"))
            check("5 restart marker", s.get("restartNeeded") == "1", f"restartNeeded={s.get('restartNeeded')}")
            g.cmd("cfgui discard")
            s = kv(g.cmd("cfgui status"))
            check("5 discard clears", s.get("restartNeeded") == "0" and s.get("pending") == "0", f"{s.get('restartNeeded')} {s.get('pending')}")
            # 6 save through MCM
            g.cmd("cfgui set Comfort fVignetteStrength 0.65")
            r = g.cmd("cfgui save")
            ok6 = False
            for _ in range(12):
                time.sleep(0.5)
                txt = open(MCM_FILE, encoding="utf-8", errors="replace").read() if os.path.isfile(MCM_FILE) else ""
                if re.search(r"(?m)^fVignetteStrength=0\.65", txt):
                    ok6 = True
                    break
            st = g.cmd("cfg")
            check("6 save -> MCM settings file", ok6, f"{r} | {st}")
            check("6 user ini untouched", open(t.MOD_INI, "rb").read() == user_ini_before, t.MOD_INI)
            # 7 simulated pointer: click the 'Camera' tab (page 1); scan y over the tab row
            g.cmd("cfgui page Quick")
            ok7 = False
            for v in (0.10, 0.11, 0.12, 0.13, 0.09, 0.14, 0.08, 0.15):
                g.cmd(f"cfgui aim 0.18 {v} up")
                time.sleep(0.25)
                g.cmd(f"cfgui aim 0.18 {v} down")
                time.sleep(0.25)
                g.cmd(f"cfgui aim 0.18 {v} up")
                time.sleep(0.25)
                if kv(g.cmd("cfgui status")).get("page") == "Camera":
                    ok7 = True
                    check("7 pointer click on a tab", True, f"v={v}")
                    break
            if not ok7:
                check("7 pointer click on a tab", False, g.cmd("cfgui status"))
            g.cmd("cfgui aim off")
            # 8 controller ray: aim the right controller at the panel centre (vremu tracking space: panel 0.7 m ahead of the
            # head at open, 0.12 m below eye level)
            g.cmd("cfgui close")
            vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0")
            time.sleep(0.5)
            g.cmd("cfgui open")
            time.sleep(1.0)
            import json
            ray = g.cmd("cfgui ray")
            pm = re.search(r"panel=([-\d.]+),([-\d.]+),([-\d.]+)", ray)
            rm = re.search(r"right valid=1 pos=([-\d.]+),([-\d.]+),([-\d.]+)", ray)
            vr = json.loads(vremu("/state"))["right"]["pos"]
            if pm and rm:  # vremu tracking space = our reference space + (vremu right - our right): aim at the panel centre,
                # then correct by where our aim ray actually meets the panel plane (vremu aims its own laser, not the aim pose)
                off = [vr[i] - float(rm.group(i + 1)) for i in range(3)]
                ctr = [float(pm.group(i + 1)) for i in range(3)]
                at = [ctr[i] + off[i] for i in range(3)]
                for _ in range(5):
                    vremu(f"/aim?dev=right&at={at[0]:.3f},{at[1]:.3f},{at[2]:.3f}&ms=150")
                    time.sleep(0.6)
                    ray = g.cmd("cfgui ray", quiet=True)
                    m = re.search(r"right valid=1 pos=([-\d.]+),([-\d.]+),([-\d.]+) dir=([-\d.]+),([-\d.]+),([-\d.]+) hit=(\d)", ray)
                    if not m or m.group(7) == "1":
                        break
                    P = [float(m.group(i)) for i in (1, 2, 3)]
                    D = [float(m.group(i)) for i in (4, 5, 6)]
                    if abs(D[2]) < 1e-3:
                        break
                    k = (ctr[2] - P[2]) / D[2]
                    hit = [P[i] + k * D[i] for i in range(3)]
                    at = [at[i] + (ctr[i] - hit[i]) for i in range(3)]
            t.say("ray: " + g.cmd("cfgui ray"))
            s = kv(g.cmd("cfgui status"))
            vremu("/input?dev=right&trigger=1&ms=800")
            time.sleep(0.4)
            b = g.cmd("bindings")
            check("8 right ray owns the panel", s.get("owner") == "1", str({k: s.get(k) for k in ("owner", "hoverRow", "clicks")}))
            check("8 trigger kept from the game", re.search(r"\brt=0(\.0+)?\b", b) is not None, b[:300])
            time.sleep(1.0)
            vremu("/aim?dev=right&at=0,0.5,-1&ms=200")
            time.sleep(0.5)
            st8 = kv(g.cmd("cfgui status", quiet=True))
            t.say(f"after the ray click: pending={st8.get('pending')} clicks={st8.get('clicks')} page={st8.get('page')}")
            g.cmd("cfgui discard")  # the click may have changed the row it landed on
            # 9 gesture action
            before = kv(g.cmd("cfgui status")).get("open")
            g.cmd("gesture test R 7")
            time.sleep(0.6)
            after = kv(g.cmd("cfgui status")).get("open")
            g.cmd("gesture test R 7")
            time.sleep(0.6)
            again = kv(g.cmd("cfgui status")).get("open")
            check("9 gesture right Up+Down toggles", before != after and again == before, f"{before} -> {after} -> {again}")
            # 10 left menu button: hold = panel, tap = Pause
            before = kv(g.cmd("cfgui status")).get("open")
            g.cmd("bindings menu 1300")
            time.sleep(2.0)
            after = kv(g.cmd("cfgui status")).get("open")
            menus = g.cmd("menus")
            check("10 menu hold toggles the panel, no Pause", before != after and "PauseMenu" not in menus, f"{before} -> {after}; {menus[:200]}")
            if after == "1":
                g.cmd("cfgui close")
            g.cmd("bindings menu 150")
            time.sleep(2.0)
            menus = g.cmd("menus")
            check("10 menu tap opens Pause", "PauseMenu" in menus, menus[:300])
            g.cmd("bindings menu 150")
            time.sleep(1.5)
            g.cmd("bindings menu")
            log = g.log_since(mark)
            for ln in log.splitlines():
                if "cfgui:" in ln or "cfgstore:" in ln:
                    pass
            with open(os.path.join(OUT, f"log-{stamp}.txt"), "w", encoding="utf-8") as fh:
                fh.write("\n".join(l for l in log.splitlines() if "cfgui" in l or "cfgstore" in l or "camdir: cfg" in l or "comfort:" in l))
    finally:
        # the Save test wrote MCM's settings file: put the user's file back (after the game quit)
        if t.game_pid():
            t.say("game still running - MCM settings file NOT restored; backup at " + mcm_backup)
        elif had_mcm:
            shutil.copy2(mcm_backup, MCM_FILE)
            t.say(f"MCM settings file restored from {mcm_backup}")
        elif os.path.isfile(MCM_FILE):
            os.replace(MCM_FILE, os.path.join(OUT, f"mcm-settings-created-by-test.{stamp}"))
            t.say("MCM settings file created by the test moved to work/cfgui")
    n_ok = sum(1 for _, ok, _ in results if ok)
    t.say(f"cfgui session: {n_ok}/{len(results)} PASS")
    with open(os.path.join(OUT, f"results-{stamp}.txt"), "w", encoding="utf-8") as fh:
        for name, ok, d in results:
            fh.write(f"{'PASS' if ok else 'FAIL'} {name}: {d}\n")


if __name__ == "__main__":
    main()
