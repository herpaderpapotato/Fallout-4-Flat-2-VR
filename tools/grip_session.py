"""Where the gun's handle sits relative to the controller grip (own gun_session.py session, SteamVR + vremu).

For the 10mm and the .44: equip through the equip manager (aim equip0), draw with the controller, then for the old chain ('gun cfg anchor 0':
WEAPON origin from the FO4VR wand offset + FRIK glue) and the handle anchor ('anchor 1'): handle centre (grip mod bound
centre) and WEAPON origin in the OpenXR grip frame (cm: x right, y forward through the fist, z up), plus the placement
residual and the muzzle consistency (P vs fire node). Captures one frame per case.

    python -I grip_session.py   -> work/gun/grip-<ts>.json + captures
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from fo4test import parse_kv  # noqa: E402

WEAPONS = (("10mm", "00004822"), (".44", "000CE97D"))


def main():
    g = gs.attach()
    res = {}
    try:
        gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
        for name, fid in WEAPONS:
            g.cmd(f"console player.additem {fid} 1", quiet=True)
            time.sleep(0.5)
            g.cmd(f"aim equip0 {fid}", quiet=True)
            time.sleep(1.5)
            gs.vremu("/input?dev=right&trigger=1&ms=200")  # draw
            time.sleep(2.5)
            for anchor in (0, 1):
                g.cmd(f"gun cfg anchor {anchor}", quiet=True)
                time.sleep(1.0)
                d = parse_kv(g.cmd("gun", quiet=True))
                cap = gs.capture(g, f"grip-{name.strip('.')}-anchor{anchor}")
                res[f"{name}/anchor{anchor}"] = {k: d.get(k) for k in ("gate", "weapon", "j", "handleNode", "handleValid", "handleInGripCm", "originInGripCm",
                                                                      "err", "angErr", "pFire", "dFireDeg", "fwdInGrip", "aimInGrip")}
                res[f"{name}/anchor{anchor}"]["capture"] = cap
                print(name, anchor, json.dumps(res[f"{name}/anchor{anchor}"]))
        g.cmd("gun cfg anchor 1", quiet=True)
    finally:
        try:
            gs.vremu("/stopall")
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, time.strftime("grip-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(out, "w"), indent=1)
    print("saved", out)


if __name__ == "__main__":
    main()
