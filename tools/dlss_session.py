"""20 Nov DLSS agent: functional check of per-eye DLSS (DLAA) + neural rendering (no XR; TestProfileVR, latest save).

    python -I dlss_session.py [--quick]

One launch, synthetic stereo eyes ("stereotest 1"). Steps (pipe + plugin log, numbers only):
  1 defaults (DLSS per eye + NR per eye at 0.5): NGX init, feature creation, evaluate results, GPU cost line, NR stereo probe
  2 NR at full resolution (cfgui preview fResolutionScale 1.0): GPU cost
  3 shared instances (DLSS bPerEye 0, NR bPerEye 0): GPU cost, NR stereo probe
  4 NR off, then DLSS off: the engine's TAA again (status)
  stereocap images at 1 (on) and 4 (off) for a sanity look. Writes work/dlss/<ts>/dlss.json + the 'dlss:' log lines.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test as t  # noqa: E402
from fo4test import say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main():
    quick = "--quick" in sys.argv
    out = {}
    save = t.latest_save()
    ts = time.strftime("%Y%m%d-%H%M%S")
    od = os.path.join(ROOT, "work", "dlss", ts)
    os.makedirs(od, exist_ok=True)
    with t.Game(no_xr=True, frik=True) as g:
        mark = g.log_mark()
        g.cmd(f"console load {save}")
        time.sleep(2)
        g._wait(f"in-world ({save})", 150, lambda: g.ready().get("ingame") == "1")
        time.sleep(3)
        g.cmd("stereotest 1")
        time.sleep(4)
        g.cmd("dlss log")

        def status(tag):
            s = g.cmd("dlss", quiet=True)
            out[tag] = s
            say(f"{tag}: {s[:600]}")
            return s

        def wait_cost(tag, secs):
            m = g.log_mark()
            time.sleep(secs)
            lines = [l for l in g.log_since(m).splitlines() if "dlss: gpu ms" in l]
            out[tag + " cost"] = lines[-1] if lines else "(no cost line yet)"
            say(f"{tag} cost: {out[tag + ' cost']}")

        status("1 defaults")
        wait_cost("1 defaults", 25)
        g.cmd("dlss nrstereo")
        time.sleep(6)
        status("1 defaults after nrstereo")
        g.cmd(f"stereocap {os.path.join(od, 'on.bmp')}")
        time.sleep(2)
        if not quick:
            g.cmd("cfgui set NeuralRendering fResolutionScale 1.0")
            time.sleep(3)
            status("2 NR full res")
            wait_cost("2 NR full res", 25)
            g.cmd("cfgui set NeuralRendering fResolutionScale 0.5")
            g.cmd("cfgui set DLSS bPerEye 0")
            g.cmd("cfgui set NeuralRendering bPerEye 0")
            time.sleep(3)
            status("3 shared")
            wait_cost("3 shared", 25)
            g.cmd("dlss nrstereo")
            time.sleep(6)
            status("3 shared after nrstereo")
            g.cmd("cfgui set DLSS bPerEye 1")
            g.cmd("cfgui set NeuralRendering bPerEye 1")
            g.cmd("cfgui set NeuralRendering iPasses 2")
            time.sleep(3)
            wait_cost("3b two NR passes", 25)
            status("3b two NR passes")
            g.cmd("cfgui set NeuralRendering iPasses 1")
            g.cmd("cfgui set NeuralRendering iHookPoint 1")
            time.sleep(3)
            wait_cost("3c NR before DLSS", 25)
            status("3c NR before DLSS")
            g.cmd("cfgui set NeuralRendering iHookPoint 0")
            g.cmd("cfgui set DLSS bEnabled 0")
            time.sleep(3)
            wait_cost("3d NR on the game's TAA (DLSS off)", 25)
            status("3d NR only")
            g.cmd(f"stereocap {os.path.join(od, 'nr-only.bmp')}")
            time.sleep(2)
            g.cmd("cfgui set DLSS bEnabled 1")
            g.cmd("cfgui set NeuralRendering bEnabled 0")
            time.sleep(3)
            wait_cost("4a NR off (DLSS only)", 25)
            status("4a NR off")
        g.cmd("cfgui set NeuralRendering bEnabled 0")
        g.cmd("cfgui set DLSS bEnabled 0")
        time.sleep(4)
        status("4b all off")
        g.cmd(f"stereocap {os.path.join(od, 'off.bmp')}")
        time.sleep(2)
        out["stereo status"] = g.cmd("stereostat", quiet=True)
        g.cmd("cfgui discard")
        time.sleep(1)
        log = g.log_since(mark)
        keep = [l for l in log.splitlines() if "dlss" in l or "OpenXR" in l or "stereo cuts" in l or "TAA/jitter" in l or "jitter" in l.lower()[:80]]
        with open(os.path.join(od, "dlss-log.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(keep))
    with open(os.path.join(od, "dlss.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    say(f"wrote {od}")


if __name__ == "__main__":
    main()
