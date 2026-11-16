"""[Weapon] bNoSpread check: ~20 shots at one fixed point with no-spread off and on (SteamVR + vremu, `load vrtest_sanctuary`).

    python -I plugin/tools/nospread_session.py [--shots 20] [--pitch -8] [--no-deploy]

Launches through fo4test.Game (refuses on hold / running game / no vremu), equips the 10mm (as gun_session.py), poses the
vremu HMD and right hand fixed (hand pitched --pitch deg below level, so shots land on the ground ~9 m ahead), `tgm` for
ammo, then per mode: `nospread off|on`, `nospread reset`, one trigger press per shot, `aim` after each (impact point of
that projectile). Metrics per mode:
  dev    = angle between each launched projectile and the shot's aim angles (`nospread hist`, from the Launch hook): the
           spread itself, independent of where it lands;
  impact = impact points seen from the mean launch origin, as angles about their mean direction (any surface), + raw points.
Writes work/nospread/<stamp>/nospread.json. Deploys the current plugin build first unless --no-deploy.
"""
import argparse
import json
import math
import os
import shutil
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo4test  # noqa: E402
from fo4test import Game, parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "work", "nospread")
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"
BUILD = os.path.join(ROOT, "plugin", "build", "windows", "x64", "releasedbg")
SAVE = "vrtest_sanctuary"
PISTOL_10MM = "00004822"
AMMO_10MM = "0001F276"


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def vec(s):
    s = (s or "").strip()
    if not s.startswith("("):
        return None
    try:
        return tuple(float(x) for x in s.strip("()").split(","))
    except ValueError:
        return None


def deploy():
    if fo4test.game_pid():
        raise SystemExit("Fallout4 running - not deploying")
    fo4test.deploy_plugin(BUILD, need=b"nospread on=")  # MO2 mod folder (backup to work/deploy-backup)


def ang_stats(dirs):
    """dirs: unit vectors; angles (deg) about their mean direction."""
    m = [sum(d[i] for d in dirs) for i in range(3)]
    n = math.sqrt(sum(x * x for x in m))
    m = [x / n for x in m]
    a = [math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(d, m)))))) for d in dirs]
    return dict(n=len(a), mean=round(sum(a) / len(a), 4), rms=round(math.sqrt(sum(x * x for x in a) / len(a)), 4), max=round(max(a), 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", type=int, default=20)
    ap.add_argument("--pitch", type=float, default=-8.0)
    ap.add_argument("--modes", default="off,on")
    ap.add_argument("--no-deploy", action="store_true")
    o = ap.parse_args()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.join(OUT, stamp)
    os.makedirs(out, exist_ok=True)
    if not o.no_deploy:
        fo4test._check_hold()
        deploy()
    res = dict(stamp=stamp, shots=o.shots, pitch=o.pitch, modes={})
    try:
        with Game(runtime_json=STEAMVR_JSON) as g:
            g.cmd(f"console load {SAVE}")
            time.sleep(2.0)
            g._wait(f"in-world ({SAVE})", 90, lambda: g.ready().get("ingame") == "1")
            time.sleep(4.0)
            vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0")
            vremu(f"/pose?dev=right&pos=0.2,1.3,-0.35&ypr=0,{39 + o.pitch},0")  # vremu pitch +39 = Touch aim pose level
            vremu("/pose?dev=left&pos=-0.2,1.3,-0.35&ypr=0,39,0")
            g.cmd(f"console player.additem {PISTOL_10MM} 1")
            g.cmd(f"console player.additem {AMMO_10MM} 300")
            time.sleep(1.0)
            g.cmd(f"aim equip0 {PISTOL_10MM}")
            time.sleep(1.5)
            vremu("/input?dev=right&trigger=1&ms=200")  # draw
            time.sleep(3.0)
            g.cmd("console tgm")  # no reloads during the series (test session only, not saved)
            res["status_start"] = g.cmd("nospread")
            for mode in o.modes.split(","):
                g.cmd(f"nospread {mode}")
                g.cmd("nospread reset")
                shots = []
                for i in range(o.shots):
                    a0 = parse_kv(g.cmd("aim", quiet=True))
                    n0 = int(a0.get("launches", "0"))
                    vremu("/press?dev=right&btn=trigger&ms=120")
                    time.sleep(1.0)
                    a1 = parse_kv(g.cmd("aim", quiet=True))
                    shots.append(dict(launches=int(a1.get("launches", "0")) - n0, launchO=vec(a1.get("launchO")),
                                      impact=vec(a1.get("projImpact")), impactDist=a1.get("impactDist")))
                hist = g.cmd("nospread hist")
                devs = [float(p.split()[0]) for p in hist.split(" | ")[1:]]
                zeroed = [p.split()[2] for p in hist.split(" | ")[1:]]
                st = g.cmd("nospread")
                hits = [s for s in shots if s["impact"] and s["launchO"]]
                m = dict(status=st, launched=sum(s["launches"] for s in shots), proj=len(devs), zeroed=zeroed.count("1"))
                if devs:
                    m["dev_deg"] = dict(mean=round(sum(devs) / len(devs), 4), max=round(max(devs), 4),
                                        rms=round(math.sqrt(sum(d * d for d in devs) / len(devs)), 4))
                if len(hits) >= 3:
                    org = [sum(s["launchO"][i] for s in hits) / len(hits) for i in range(3)]
                    dirs = []
                    for s in hits:
                        v = [s["impact"][i] - org[i] for i in range(3)]
                        n = math.sqrt(sum(x * x for x in v))
                        dirs.append([x / n for x in v])
                    m["impact_deg"] = ang_stats(dirs)
                    m["impact_dist_u"] = round(sum(math.dist(s["impact"], org) for s in hits) / len(hits), 1)
                    ctr = [sum(s["impact"][i] for s in hits) / len(hits) for i in range(3)]
                    r = [math.dist(s["impact"], ctr) for s in hits]
                    m["impact_radius_u"] = dict(rms=round(math.sqrt(sum(x * x for x in r) / len(r)), 2), max=round(max(r), 2), hits=len(hits))
                m["shots"] = shots
                m["devs"] = devs
                res["modes"][mode] = m
                say(f"mode {mode}: " + json.dumps({k: v for k, v in m.items() if k not in ("shots", "devs")}))
            g.cmd("console tgm")
            res["status_end"] = g.cmd("nospread on")  # leave it as shipped
    finally:
        with open(os.path.join(out, "nospread.json"), "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1)
        say(f"wrote {out}")


if __name__ == "__main__":
    main()
