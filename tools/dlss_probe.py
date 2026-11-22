"""20 Nov DLSS agent: short no-XR probe - load the latest save in synthetic stereo, print 'dlss' status lines and run pipe
commands given on the command line (each '; '-separated step waits 3 s).  python -I dlss_probe.py "cmd1; cmd2; ..." """
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test as t  # noqa: E402
from fo4test import say  # noqa: E402


def main():
    steps = [s.strip() for s in (sys.argv[1] if len(sys.argv) > 1 else "").split(";") if s.strip()]
    save = t.latest_save()
    with t.Game(no_xr=True, frik=True) as g:
        mark = g.log_mark()
        g.cmd(f"console load {save}")
        time.sleep(2)
        g._wait("in-world", 150, lambda: g.ready().get("ingame") == "1")
        time.sleep(3)
        g.cmd("stereotest 1")
        time.sleep(4)
        for _ in range(3):
            say(g.cmd("dlss", quiet=True)[:400])
            time.sleep(1)
        for s in steps:
            if s.startswith("sleep "):
                time.sleep(float(s[6:]))
                continue
            say(f"{s} -> {g.cmd(s, quiet=True)[:500]}")
            time.sleep(3)
        say(g.cmd("dlss", quiet=True)[:400])
        log = g.log_since(mark)
        for l in log.splitlines():
            if "dlss: gpu ms" in l or "nrstereo" in l or "dlss: engine jitter" in l or "[E]" in l:
                print(l[:420])


if __name__ == "__main__":
    main()
