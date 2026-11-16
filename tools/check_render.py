"""One-launch check of window size, engine render resolution and rendered FOV/aspect, with XR on
(SteamVR + vremu) or off. Prints a compact summary; no images."""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from fo4test import LOG, Game, say  # noqa: E402

STEAMVR = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"


def main():
    no_xr = "--no-xr" in sys.argv
    with Game(runtime_json=None if no_xr else STEAMVR, no_xr=no_xr) as g:
        g.coc("QASmoke")
        time.sleep(3)
        rt = g.cmd("rt", quiet=True)
        cam = g.cmd("cam", quiet=True)
        st = g.cmd("status", quiet=True)
        log = open(LOG, encoding="utf-8", errors="replace").read()
        m = re.search(r"frustum l=(\S+) r=(\S+) t=(\S+)", cam)
        r, t = (float(m.group(2)), float(m.group(3))) if m else (0, 0)
        import math
        say("window: " + "; ".join(re.findall(r"game window: .*|small window: .*", log)[-2:]))
        say("display: " + "; ".join(re.findall(r"display size override: .*", log)[-2:]))
        say("swapchain: " + "; ".join(re.findall(r"swapchain create: .*", log)[-1:]))
        say("rt: " + rt)
        say(f"frustum: hfov {2*math.degrees(math.atan(r)):.1f} vfov {2*math.degrees(math.atan(t)):.1f} aspect r/t {r/t if t else 0:.3f}")
        say("fov log: " + "; ".join(re.findall(r"game rendered FOV .*", log)[-1:]))
        say("pacing: " + "; ".join(re.findall(r"pacing: .*", log)[-2:]))
        say("status: " + st)


if __name__ == "__main__":
    main()
