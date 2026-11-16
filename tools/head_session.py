"""Head-timing session (htrace) under SteamVR + vremu. Save vrtest_sanctuary, never QASmoke.

    python -I head_session.py launch               # refuse if Fallout4 / xmake runs; deploy build; launch; load save
    python -I head_session.py run <scenario> [frames] [tag]
         scenarios: static | yaw | noise | yawnoise | pitch
                    + suffix "_pip" -> wrist Pip-Boy open (game paused) during the trace
    python -I head_session.py cmd "htrace" "rs"
    python -I head_session.py quit                 # qqq the session this script launched (pid file); never another game

Traces -> work/headtiming/<ts>-<scenario>[-tag].csv + .json (tools/htrace_analyze.py).
SteamVR is never closed or restarted here (shared, vremu emulator mode).
"""
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402
import htrace_analyze  # noqa: E402
import htrace_sine  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "work", "headtiming")
PIDF = os.path.join(OUT, "session.pid")
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
BUILD = os.path.join(ROOT, "plugin", "build", "windows", "x64", "releasedbg")
SAVE = "vrtest_sanctuary"
HMD0 = "/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0"


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def xmake_running():
    return "xmake" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()


def deploy():
    if game_pid():
        raise SystemExit("Fallout4 running - not deploying")
    if xmake_running():
        raise SystemExit("an xmake build is running - not deploying")
    import fo4test
    # MO2 mod folder (backup to work/deploy-backup). FRIK.dll is shipped since 15 Nov (FRIK-flat MO2 mod): reported only;
    # for camera measurements use the `frik smooth off` pipe command.
    fo4test.deploy_plugin(BUILD, need=b"htrace armed")
    if fo4test.mod_enabled(fo4test.MOD_FRIK):
        say("note: FRIK-flat is enabled in MO2 (FRIK smoothing adds to the camera z; `frik smooth off` for timing runs)")


META_SIM_JSON = r"C:\Program Files\MetaXRSimulator\v207.1\meta_openxr_simulator.json"


def launch(no_deploy=False, runtime=STEAMVR_JSON):
    os.makedirs(OUT, exist_ok=True)
    if game_pid() or xmake_running():
        raise SystemExit("Fallout4 or an xmake build is running - not launching")
    if not no_deploy:
        deploy()
    g = Game(runtime_json=runtime)
    g.__enter__()
    pid = game_pid()
    open(PIDF, "w").write(str(pid))
    say(f"launched pid {pid}")
    g.cmd(f"console load {SAVE}")
    time.sleep(2.0)
    g._wait(f"in-world ({SAVE})", 90, lambda: g.ready().get("ingame") == "1")
    time.sleep(4.0)
    vremu(HMD0)
    g._close()


def attach():
    pid = game_pid()
    if not pid:
        raise SystemExit("no Fallout4 running")
    try:
        own = int(open(PIDF).read().strip())
    except (OSError, ValueError):
        own = None
    if own != pid:
        raise SystemExit(f"Fallout4 pid {pid} is not this session's ({own}) - not touching it")
    g = Game(attach=True, quiet=True)
    g.__enter__()
    return g


def write_noise_vrt(path, seconds=30.0, hz=90.0, ang=0.05, pos=0.0005, yaw_rate=0.0, seed=1):
    """HMD timeline: small random ypr/pos keys at hz (vremu interpolates between keys), optional steady yaw."""
    rnd = random.Random(seed)
    lines = ["# htrace noise timeline"]
    n = int(seconds * hz)
    for i in range(n + 1):
        t = i / hz
        x, y, z = (rnd.uniform(-pos, pos) for _ in range(3))
        yw, pt, rl = (rnd.uniform(-ang, ang) for _ in range(3))
        yw += yaw_rate * t
        lines.append(f"{t:.4f} hmd ypr {x:.5f} {1.7 + y:.5f} {z:.5f} {yw:.4f} {pt:.4f} {rl:.4f}")
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


AXES = ("x", "y", "z", "yaw", "pitch", "roll")


def write_sine_vrt(path, seconds, comps, hz=250.0):
    """comps: list of (axis, amplitude (m or deg), frequency Hz, phase rad). HMD around (0, 1.7, 0), ypr 0."""
    lines = ["# htrace sinusoid timeline " + " ".join(f"{a}:{amp}@{f}" for a, amp, f, _ in comps)]
    n = int(seconds * hz)
    for i in range(n + 1):
        t = i / hz
        v = {a: 0.0 for a in AXES}
        for a, amp, f, ph in comps:
            v[a] += amp * math.sin(2 * math.pi * f * t + ph)
        lines.append(f"{t:.4f} hmd ypr {v['x']:.5f} {1.7 + v['y']:.5f} {v['z']:.5f} {v['yaw']:.4f} {v['pitch']:.4f} {v['roll']:.4f}")
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


def sine_comps(kind):
    """sin_<axis>_<hz> (3 deg / 3 cm) | sincombo (all six axes, mixed 0.5/2 Hz, different phases)"""
    if kind == "sincombo":
        return [("yaw", 3.0, 0.5, 0.0), ("pitch", 2.0, 2.0, 0.7), ("roll", 2.0, 0.5, 1.3),
                ("x", 0.03, 2.0, 0.3), ("y", 0.02, 0.5, 2.1), ("z", 0.03, 0.5, 1.0)]
    _, axis, f = kind.split("_")
    return [(axis, 3.0 if axis in ("yaw", "pitch", "roll") else 0.03, float(f), 0.0)]


def start_motion(kind, seconds):
    vremu("/stopall")
    vremu(HMD0)
    time.sleep(0.5)
    if kind == "static":
        return
    if kind == "yaw":
        vremu(f"/rate?dev=hmd&w=40,0,0&ms={int(seconds * 1000)}&timeout={int(seconds * 1000)}")
    elif kind == "pitch":
        vremu(f"/rate?dev=hmd&w=0,15,0&ms={int(seconds * 1000)}&timeout={int(seconds * 1000)}")
    elif kind in ("noise", "yawnoise", "posnoise"):
        p = os.path.join(OUT, f"noise-{kind}.vrt")
        write_noise_vrt(p, seconds=seconds + 2, yaw_rate=40.0 if kind == "yawnoise" else 0.0,
                        pos=0.005 if kind == "posnoise" else 0.0005)
        vremu(f"/play?file={urllib.request.quote(p)}&hmd=1")
    elif kind.startswith("sin"):
        p = os.path.join(OUT, f"motion-{kind}.vrt")
        write_sine_vrt(p, seconds + 2, sine_comps(kind))
        vremu(f"/play?file={urllib.request.quote(p)}&hmd=1")
    else:
        raise SystemExit(f"unknown motion {kind}")
    time.sleep(0.3)


def run(g, scenario, frames=600, tag=""):
    motion = scenario.replace("_pip", "")
    pip = scenario.endswith("_pip")
    ts = time.strftime("%Y%m%d-%H%M%S")
    stem = os.path.join(OUT, f"{ts}-{scenario}{('-' + tag) if tag else ''}")
    if pip:  # FO4VR bindings (ControllerBindings.cpp): off-hand (left) trigger opens the wrist Pip-Boy, a grip closes it
        vremu("/press?dev=left&btn=trigger&ms=150")
        time.sleep(2.0)
        say("pipboy: " + g.cmd("pipboy")[:160])
    secs = frames / 25.0 + 3.0
    start_motion(motion, secs)
    say(g.cmd(f"htrace arm {frames}"))
    t0 = time.time()
    while time.time() - t0 < secs + 10:
        st = g.cmd("htrace")
        if "armed=0" in st:
            break
        time.sleep(0.5)
    vremu("/stopall")
    vremu(HMD0)
    say(g.cmd(f"htrace dump {stem}.csv"))
    if pip:
        vremu("/press?dev=left&btn=grip&ms=150")
        time.sleep(1.5)
    recs, freq = htrace_analyze.load(stem + ".csv")
    out, rows = htrace_analyze.analyse(recs, freq)
    out["scenario"] = scenario
    out["tag"] = tag
    out["status"] = g.cmd("status")
    if motion.startswith("sin"):
        out["sine"] = htrace_sine.analyse(recs, freq)
        print(htrace_sine.compact(out["sine"]), flush=True)
    with open(stem + ".json", "w") as fh:
        json.dump({"summary": out, "rows": rows}, fh, indent=1)
    print(json.dumps(compact(out), indent=1))
    return out


def compact(out):
    keep = {}
    for k, v in out.items():
        if k in ("all", "running", "paused"):
            v = dict(v)
            lag = v.get("lag(eyesSeq-camSeq)", {})
            v["lag(eyesSeq-camSeq)"] = {kk: n for kk, n in lag.items() if kk in ("0", "1", "2", "-1", "n/a")}
            v["lag_other"] = sum(n for kk, n in lag.items() if kk not in ("0", "1", "2", "-1", "n/a"))
        if k in ("threads", "status"):
            continue
        keep[k] = v
    return keep


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "launch":
        launch(no_deploy="--no-deploy" in a, runtime=META_SIM_JSON if "--metasim" in a else STEAMVR_JSON)
    elif a[0] == "deploy":
        deploy()
    elif a[0] == "run":
        g = attach()
        try:
            run(g, a[1], int(a[2]) if len(a) > 2 else 600, a[3] if len(a) > 3 else "")
        finally:
            g._close()
    elif a[0] == "cmd":
        g = attach()
        try:
            for c in a[1:]:
                print(g.cmd(c))
        finally:
            g._close()
    elif a[0] == "quit":
        g = attach()
        g.launched = True
        g.quit()
        try:
            os.replace(PIDF, PIDF + ".done")
        except OSError:
            pass
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
