"""Run a list of pipe commands in one launch (debug helper).

    python -I pipe_script.py [--vremu] "cmd1" "sleep 1.5" "vremu /input?dev=right&joy=0.6,0&ms=500" "grep rs heading,gate" ...

Each argument: a pipe command; "sleep <s>"; "vremu <path>" (GET on the vremu control server); "grep <cmd> <k1,k2>"
(run <cmd>, print only those keys). Waits until no Fallout4 runs, launches, coc QASmoke, runs, quits.
"""
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402

STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"


def main():
    args = sys.argv[1:]
    use_vr = "--vremu" in args
    args = [a for a in args if a != "--vremu"]
    while game_pid():
        say("Fallout4 running - waiting")
        time.sleep(20)
    with Game(runtime_json=STEAMVR_JSON if use_vr else None, quiet=True) as g:
        g.coc("QASmoke")
        time.sleep(3)
        for a in args:
            if a.startswith("sleep "):
                time.sleep(float(a[6:]))
            elif a.startswith("vremu "):
                with urllib.request.urlopen("http://127.0.0.1:8766" + a[6:], timeout=3) as r:
                    say(f"vremu {a[6:]} -> {r.read()[:80]!r}")
            elif a.startswith("grep "):
                _, c, keys = a.split(" ", 2)
                d = parse_kv(g.cmd(c))
                say(f"{c}: " + " ".join(f"{k}={d.get(k)}" for k in keys.split(",")))
            else:
                say(f"> {a}\n    {g.cmd(a)[:600]}")
        if use_vr:
            try:
                urllib.request.urlopen("http://127.0.0.1:8766/stopall", timeout=3).read()
            except OSError:
                pass


if __name__ == "__main__":
    main()
