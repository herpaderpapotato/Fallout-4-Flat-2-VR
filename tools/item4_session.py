"""FO4VR-baseline gun gaps (own gun_session.py session, SteamVR + vremu): kickback + recoil rotation on shots, melee on
the PrimaryMeleeWeaponOffsetNode, the gun in power armour. Weapons equipped through the equip manager (aim equip0) and
drawn with the controller trigger.

    python -I item4_session.py [--pa]   -> work/gun/item4-<ts>.json
"""
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402

BAT = "0008E736"         # BaseballBat (two-handed melee)
SWITCHBLADE = "000FDC81"
PA_FRAME = "0004A9C1"
FUSION_CORE = "00075FE4"
KEYS = ("gate", "ws", "weapon", "j", "melee", "inPA", "kicks", "kickOn", "kickD", "kickMaxD", "kickAmp", "kickDur", "kickK", "recoilRotDeg",
        "err", "angErr", "originInGripCm", "handleInGripCm", "fireFwd", "pFire", "dFireDeg", "armsHidden", "armsCulled", "armsCandidates", "ctrlR", "ctrlL")


def gk(g):
    return parse_kv(g.cmd("gun", quiet=True))


def pick(d):
    return {k: d.get(k) for k in KEYS}


def pitch(v):
    try:
        x, y, z = (float(t) for t in v.strip("()").split(","))
        return math.degrees(math.asin(max(-1.0, min(1.0, z / max(1e-9, math.sqrt(x * x + y * y + z * z))))))
    except Exception:
        return None


def equip_draw(g, fid):
    g.cmd(f"console player.additem {fid} 1", quiet=True)
    time.sleep(0.5)
    g.cmd(f"aim equip0 {fid}", quiet=True)
    time.sleep(1.5)
    gs.vremu("/input?dev=right&trigger=1&ms=200")  # draw with the controller
    time.sleep(2.5)


def main():
    g = gs.attach()
    res = {}
    try:
        gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
        # ---- kickback / recoil rotation: 10mm, 3 single shots, sampled every ~15 ms for 0.6 s
        equip_draw(g, gs.PISTOL_10MM)
        g.cmd("gun cfg kickreset 1", quiet=True)
        base = gk(g)
        res["kick_base"] = pick(base)
        shots = []
        for i in range(3):
            series = []
            gs.vremu("/press?dev=right&btn=trigger&ms=80")
            t0 = time.time()
            while time.time() - t0 < 0.6:
                d = gk(g)
                series.append((round(time.time() - t0, 3), float(d.get("kickD", 0)), float(d.get("recoilRotDeg", 0)), pitch(d.get("fireFwd", ""))))
            d = gk(g)
            shots.append(dict(kicks=d.get("kicks"), kickAmp=d.get("kickAmp"), kickDur=d.get("kickDur"), kickK=d.get("kickK"),
                              kickMaxD=d.get("kickMaxD"), maxKickD=max(s[1] for s in series), maxRotDeg=max(s[2] for s in series),
                              pitchMin=min(s[3] for s in series if s[3] is not None), pitchMax=max(s[3] for s in series if s[3] is not None),
                              err=d.get("err"), pFire=d.get("pFire"), series=series[:40]))
            print("shot", i, {k: v for k, v in shots[-1].items() if k != "series"})
            time.sleep(1.5)
        res["kick_shots"] = shots
        res["kick_basePitch"] = pitch(base.get("fireFwd", ""))
        # off switch
        g.cmd("gun cfg kick 0", quiet=True), g.cmd("gun cfg recoil 0", quiet=True)
        gs.vremu("/press?dev=right&btn=trigger&ms=80")
        mx = 0.0
        t0 = time.time()
        while time.time() - t0 < 0.5:
            d = gk(g)
            mx = max(mx, float(d.get("kickD", 0)), float(d.get("recoilRotDeg", 0)))
        res["kick_off_max"] = mx
        g.cmd("gun cfg kick 1", quiet=True), g.cmd("gun cfg recoil 1", quiet=True)
        time.sleep(1.0)

        # ---- melee: bat and switchblade on the melee node (expected WEAPON origin in the grip frame (0,-2.86,4.95) cm)
        for name, fid in (("bat", BAT), ("switchblade", SWITCHBLADE)):
            equip_draw(g, fid)
            d1 = gk(g)
            g.cmd("gun cfg melee 0", quiet=True)
            time.sleep(0.6)
            d0 = gk(g)
            g.cmd("gun cfg melee 1", quiet=True)
            time.sleep(0.4)
            cap = gs.capture(g, f"item4-{name}")
            res[f"melee_{name}"] = dict(on=pick(d1), off=pick(d0), capture=cap)
            print(name, "melee on", pick(d1)["originInGripCm"], d1.get("melee"), d1.get("gate"), "| off", pick(d0)["originInGripCm"], d0.get("melee"))
        equip_draw(g, gs.PISTOL_10MM)

        # ---- power armour (optional)
        if "--pa" in sys.argv:
            g.cmd(f"console player.additem {FUSION_CORE} 2", quiet=True)
            g.cmd(f"console player.placeatme {PA_FRAME} 1", quiet=True)
            time.sleep(3.0)
            a = parse_kv(g.cmd("aim", quiet=True))
            ref = None
            # the placed frame is the newest ref; find it via the pick while looking down/ahead
            for yaw in (0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150, 180):
                gs.vremu(f"/pose?dev=hmd&pos=0,1.7,0&ypr={yaw},-10,0")
                gs.hand(ypr=(yaw, gs.HAND_YPR0[1] - 15, 0))
                time.sleep(0.6)
                a = parse_kv(g.cmd("aim", quiet=True))
                if "Power Armor" in (g.cmd("aim", quiet=True)) and a.get("pick") not in (None, "00000000"):
                    ref = a.get("pick")
                    break
            res["pa_ref"] = ref
            if ref:
                g.cmd(f"console {ref}.activate player", quiet=True)
                time.sleep(9.0)
                gs.vremu(gs.HMD0), gs.hand()
                gs.vremu("/input?dev=right&trigger=1&ms=200")
                time.sleep(2.5)
                d = gk(g)
                res["pa"] = pick(d)
                res["pa"]["capture"] = gs.capture(g, "item4-pa")
                res["pa_dump"] = g.cmd("gun dump", quiet=True)[:1200]
                print("PA", res["pa"])
                # leave PA: hold activate (A) ~1 s
                gs.vremu("/input?dev=right&a=1&ms=1200")
                time.sleep(6.0)
                res["pa_exit"] = pick(gk(g))
    finally:
        try:
            gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, time.strftime("item4-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(out, "w"), indent=1, default=str)
    print("saved", out)


if __name__ == "__main__":
    main()
