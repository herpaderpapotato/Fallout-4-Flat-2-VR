"""20 Nov camera agent: first-person lock / body camera / HUD notification check (no XR; TestProfileVR, latest save).

    python -I camlock_session.py

One launch. Steps (pipe + console, numbers from the camdir readout and the plugin log):
  1 lock on: Game.ForceThirdPerson, tfc -> state stays 0, gate blocked counts rise, HUD 'refused' lines
  2 lock off: Game.ForceThirdPerson -> state 8 (SCENE), HUD state-change line
  3 body on (still state 8): camera out == body base + head offset, bodyNow=1, HUD view-mode line
  4 lock on while in 8 -> backstop Force1stPerson -> state 0
Writes work/camlock/<ts>/camlock.json.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test as t  # noqa: E402
from fo4test import parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main():
    out = {}
    save = t.latest_save()
    ts = time.strftime("%Y%m%d-%H%M%S")
    od = os.path.join(ROOT, "work", "camlock", ts)
    os.makedirs(od, exist_ok=True)
    with t.Game(no_xr=True, frik=True) as g:
        mark = g.log_mark()
        g.cmd(f"console load {save}")
        time.sleep(2)
        g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
        time.sleep(3)

        def cd():
            return parse_kv(g.cmd("camdir", quiet=True))

        def snap(tag):
            d = cd()
            r = {k: d.get(k) for k in ("state", "name", "policy", "eff", "lockFp", "lockForces", "lastLockForce", "body", "bodyNow",
                                       "bodyEyeH", "tg", "out", "add", "hudShown", "hudRefused", "hudLast", "mode")}
            r["lock"] = g.cmd("camdir lock", quiet=True)
            out[tag] = r
            say(f"{tag}: {r}")
            return r

        snap("0 start")
        g.cmd("camdir lock on")
        time.sleep(0.5)
        g.cmd('console cgf "Game.ForceThirdPerson"')
        time.sleep(1.5)
        snap("1a lock on, ForceThirdPerson")
        g.cmd("console tfc")
        time.sleep(1.5)
        snap("1b lock on, tfc")
        g.cmd("console tfc")  # in case it went through
        time.sleep(1.0)
        g.cmd("camdir lock off")
        g.cmd("camdir cfg return 0")  # bThirdPersonReturn would bring a script third person back before the checks
        time.sleep(0.5)
        g.cmd('console cgf "Game.ForceThirdPerson"')
        time.sleep(2.0)
        snap("2 lock off, ForceThirdPerson")
        g.cmd("pose 20 0 0")  # scripted head pose: the director writes only while a head pose exists
        time.sleep(0.5)
        snap("2b SCENE with head pose")
        g.cmd("camdir body on")
        time.sleep(1.5)
        s3 = snap("3 body on")
        pos = g.cmd("console player.getpos z", quiet=True)
        out["3 player getpos z (console reply)"] = pos
        g.cmd("camdir body off")
        time.sleep(1.0)
        snap("3b body off")
        g.cmd("camdir lock on")
        time.sleep(2.0)
        snap("4 lock on while third person")
        g.cmd("camdir lock off")
        g.cmd("pose off")
        time.sleep(0.5)
        log = g.log_since(mark)
        lines = [ln for ln in log.splitlines() if "Force1stPerson" in ln or "camdir: HUD" in ln or "first-person lock" in ln or "gate: Set3rd" in ln or
                 "gate: Force3rd" in ln or "ToggleFreeCameraMode" in ln or "state " in ln and "camdir: state" in ln]
        out["log"] = lines[-80:]
        for ln in lines[-80:]:
            print(ln)
        g.quit()
    with open(os.path.join(od, "camlock.json"), "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    say(f"wrote {od}")


if __name__ == "__main__":
    main()
