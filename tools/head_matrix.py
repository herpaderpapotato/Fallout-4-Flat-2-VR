"""Switch-off matrix for the head-timing trace (attaches to the head_session.py game).

    python -I head_matrix.py [frames] [motion]

Each row: set switches over the pipe, run one htrace scenario, restore. Prints one compact line per row and writes
work/headtiming/matrix-<ts>.json.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import head_session as hs  # noqa: E402
from fo4test import say  # noqa: E402

ROWS = [
    ("default (sync on)", ["htrace sync on"], []),
    ("sync off", ["htrace sync off"], ["htrace sync on"]),
    ("roomscale off", ["rs off"], ["rs on"]),
    ("head authority off", ["rs cfg auth 0"], ["rs cfg auth 1"]),
    ("body follow off", ["rs cfg body 0"], ["rs cfg body 1"]),
    ("tfix 0", ["rs cfg auth 0", "rs cfg tfix 0"], ["rs cfg tfix 1", "rs cfg auth 1"]),
    ("tfix 2", ["rs cfg auth 0", "rs cfg tfix 2"], ["rs cfg tfix 1", "rs cfg auth 1"]),
    ("owned heading off", ["rs cfg own 0"], ["rs cfg own 1"]),
    ("director off", ["camdir off"], ["camdir on"]),
    ("comfort turn off", ["comfort turn off"], ["comfort turn smooth"]),
]


def line(name, o):
    a = o["all"]
    lag = a["lag(eyesSeq-camSeq)"]
    return (f"{name:22s} lag0={lag.get('0', 0):4d} lag1={lag.get('1', 0):4d} err={a['errDeg'].split('mean=')[1].split()[0]} deg "
            f"pos={a['posErrMm'].split('mean=')[1].split()[0]} mm | camStep-headStep {o['camStepMinusHeadStepUnits'].split('mean=')[1].split(' p95')[0]} "
            f"p95={o['camStepMinusHeadStepUnits'].split('p95=')[1].split()[0]} | camZ2nd p95={o['camZ2ndDiffUnits'].split('p95=')[1].split()[0]} "
            f"| frame {o['present_interval_ms'].split('mean=')[1].split()[0]} ms")


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    motion = sys.argv[2] if len(sys.argv) > 2 else "posnoise"
    g = hs.attach()
    res = {}
    try:
        for name, on, off in ROWS:
            for c in on:
                say(f"{c}: {g.cmd(c)[:120]}")
            time.sleep(1.0)
            o = hs.run(g, motion, frames, name.replace(" ", "_").replace("(", "").replace(")", ""))
            res[name] = o
            print("ROW", line(name, o), flush=True)
            for c in off:
                say(f"{c}: {g.cmd(c)[:120]}")
            time.sleep(1.0)
    finally:
        g._close()
    out = os.path.join(hs.OUT, f"matrix-{time.strftime('%Y%m%d-%H%M%S')}-{motion}.json")
    with open(out, "w") as fh:
        json.dump(res, fh, indent=1)
    print("\n".join(line(n, o) for n, o in res.items()))
    print("->", out)


if __name__ == "__main__":
    main()
