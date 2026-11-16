"""In-game check of the virtual pad (no headset): walk forward with the left stick, open/close the
Pip-Boy with B, and verify via player camera position and the plugin log. Run inside a Game session
or standalone (launches the game, needs UAC)."""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from fo4test import LOG, Game, say  # noqa: E402


def cam_pos(g):
    m = re.search(r"worldPos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", g.cmd("cam", quiet=True))
    return tuple(float(v) for v in m.groups()) if m else None


def run(g):
    results = {}
    mark = __import__("os").path.getsize(LOG)
    p0 = cam_pos(g)
    g.cmd("pad ly=1 ms=1500")
    time.sleep(2.0)
    p1 = cam_pos(g)
    moved = ((p1[0] - p0[0]) ** 2 + (p1[1] - p0[1]) ** 2) ** 0.5 if p0 and p1 else 0.0
    results["walk"] = ("PASS" if moved > 50 else "FAIL", f"moved {moved:.0f} units in 1.5 s of ly=1")
    g.cmd("pad b=1 ms=150")
    time.sleep(2.5)
    log = open(LOG, encoding="utf-8", errors="replace").read()[mark:]
    opened = "-> PipboyMenu" in log
    g.cmd("pad b=1 ms=150")
    time.sleep(2.5)
    log = open(LOG, encoding="utf-8", errors="replace").read()[mark:]
    closed = "PipboyMenu -> -" in log
    results["pipboy"] = ("PASS" if opened and closed else "FAIL", f"opened={opened} closed={closed} (log 'view menu')")
    for k, (r, info) in results.items():
        say(f"{k:8s} {r}  {info}")
    return results


if __name__ == "__main__":
    with Game(no_xr="--no-xr" in sys.argv) as g:
        g.coc("QASmoke")
        time.sleep(2)
        run(g)
