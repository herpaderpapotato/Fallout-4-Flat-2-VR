"""Sinusoid protocol for the head-timing trace (attaches to the head_session.py game).

    python -I head_sine_suite.py axes [frames]      # every axis alone at 0.5 and 2 Hz, sync on / off, running
    python -I head_sine_suite.py paused [frames]    # sincombo + 2 Hz yaw/pitch/x with the wrist Pip-Boy open, sync on / off
    python -I head_sine_suite.py matrix [frames]    # sincombo with each per-update subsystem off in turn (sync on)

One line per run and channel; JSON -> work/headtiming/sine-<mode>-<ts>.json
"""
import io
import json
import os
import sys
import time
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import head_session as hs  # noqa: E402
from fo4test import say  # noqa: E402

MATRIX = [
    ("default", [], []),
    ("roomscale off", ["rs off"], ["rs on"]),
    ("head authority off", ["rs cfg auth 0"], ["rs cfg auth 1"]),
    ("body follow off", ["rs cfg body 0"], ["rs cfg body 1"]),
    ("owned heading off", ["rs cfg own 0"], ["rs cfg own 1"]),
    ("director off", ["camdir off"], ["camdir on"]),
    ("comfort turn off", ["comfort turn off"], ["comfort turn smooth"]),
    ("auth off + tfix 0", ["rs cfg auth 0", "rs cfg tfix 0"], ["rs cfg tfix 1", "rs cfg auth 1"]),
    ("auth off + tfix 2", ["rs cfg auth 0", "rs cfg tfix 2"], ["rs cfg tfix 1", "rs cfg auth 1"]),
]
CH = ("yaw vs claimed", "pitch vs claimed", "roll vs claimed", "x vs claimed", "y vs claimed", "z vs claimed")


def row(tag, sine, only=None):
    out = []
    for ch in CH:
        v = sine.get(ch)
        if not v or v.get("note") or v["inputDeltaRms"] < 1e-3 or (only and not (ch + " ").startswith(only)):
            continue
        sp = v.get("spectrumLag0") or {}
        out.append(f"{tag:34s} {ch.split()[0]:5s} in={v['inputDeltaRms']:.4f} lat={v['latencyFrames']:+d} "
                   f"res0={v['residualDeltaRmsAtLag0']:.5f} ({100 * v['residualRatioLag0']:5.1f}%) resLat={v['residualDeltaRmsAtLatency']:.5f} "
                   f"c0={v['corrAtLag0']:.4f} rep={v['repeatedSteps']} skip={v['skippedSteps']} flips={v['residualSignFlips']} "
                   f"dom={sp.get('dominantHz')}Hz nyq={sp.get('nearNyquistShare')} {v['unit']}")
    return out


def one(g, motion, frames, tag):
    buf = io.StringIO()
    with redirect_stdout(buf):
        o = hs.run(g, motion, frames, tag)
    s = o.get("sine", {})
    summ = o["all"]
    head = (f"{tag:34s} pairing lag0={summ['lag(eyesSeq-camSeq)'].get('0', 0)} lag1={summ['lag(eyesSeq-camSeq)'].get('1', 0)} "
            f"running={s.get('runningFrames')}/{s.get('frames')} fs={s.get('fsHz')}Hz frame={o['present_interval_ms'].split('mean=')[1].split()[0]}ms")
    # vremu axes (OpenVR: y up, -z forward) -> game channels (z up, y forward)
    vmap = {"yaw": "yaw", "pitch": "pitch", "roll": "roll", "x": "x ", "y": "z ", "z": "y "}
    only = vmap.get(motion.split("_")[1]) if motion.count("_") == 2 else None
    return o, [head] + row(tag, s, only=only)


def main():
    mode = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    g = hs.attach()
    res, lines = {}, []
    try:
        if mode == "axes":
            for sync in ("on", "off"):
                g.cmd(f"htrace sync {sync}")
                for ax in ("yaw", "pitch", "roll", "x", "y", "z"):
                    for f in ("0.5", "2"):
                        m = f"sin_{ax}_{f}"
                        o, ls = one(g, m, frames, f"{m} sync {sync}")
                        res[f"{m} sync {sync}"] = o
                        lines += ls
                        print("\n".join(ls), flush=True)
            g.cmd("htrace sync on")
        elif mode == "paused":
            for sync in ("on", "off"):
                g.cmd(f"htrace sync {sync}")
                for m in ("sincombo_pip", "sin_yaw_2_pip", "sin_pitch_2_pip", "sin_x_2_pip"):
                    o, ls = one(g, m, frames, f"{m} sync {sync}")
                    res[f"{m} sync {sync}"] = o
                    lines += ls
                    print("\n".join(ls), flush=True)
            g.cmd("htrace sync on")
        elif mode == "matrix":
            for sync in ("on", "off"):
                g.cmd(f"htrace sync {sync}")
                for name, on, off in MATRIX:
                    for c in on:
                        say(f"{c}: {g.cmd(c)[:100]}")
                    time.sleep(1.0)
                    o, ls = one(g, "sincombo", frames, f"{name} (sync {sync})")
                    res[f"{name} sync {sync}"] = o
                    lines += ls
                    print("\n".join(ls), flush=True)
                    for c in off:
                        say(f"{c}: {g.cmd(c)[:100]}")
                    time.sleep(1.0)
            g.cmd("htrace sync on")
    finally:
        g._close()
    p = os.path.join(hs.OUT, f"sine-{mode}-{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(p, "w") as fh:
        json.dump({k: {"sine": v.get("sine"), "all": v.get("all"), "present_interval_ms": v.get("present_interval_ms")} for k, v in res.items()},
                  fh, indent=1)
    with open(p.replace(".json", ".txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("->", p)


if __name__ == "__main__":
    main()
