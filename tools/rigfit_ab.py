"""Rig-fit A/B (own gun_session.py session): [Roomscale] head authority on, 'gun cfg rigfit 0|1'. Per case: perturb the
camera (roomscale off/on toggle + head step), then count first-person root / RArm_Hand step reversals at the render
collect for 4 s still and 4 s sway, and record camera root z / player loc z (camera must be unchanged by rigfit).

    python -I rigfit_ab.py   -> work/gun/rigfit-<ts>.json
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun_session as gs  # noqa: E402
from alternation_ab import counters, motion  # noqa: E402
from fo4test import parse_kv  # noqa: E402


def main():
    g = gs.attach()
    res = {}
    try:
        print(g.cmd("gun diag on", quiet=True))
        g.cmd("rs cfg auth 1", quiet=True)
        for fit in (0, 1, 0, 1):
            g.cmd(f"gun cfg rigfit {fit}", quiet=True)
            gs.vremu("/stopall"), gs.vremu(gs.HMD0), gs.hand(), gs.left()
            g.cmd("rs off", quiet=True)
            gs.vremu("/pose?dev=hmd&pos=0.1,1.6,0.05&ypr=0,0,0")
            time.sleep(0.5)
            g.cmd("rs on", quiet=True)
            gs.vremu(gs.HMD0)
            time.sleep(1.0)
            for label in ("still", "sway"):
                c0, _ = counters(g)
                zs = []
                if label == "still":
                    for _ in range(8):
                        d = parse_kv(g.cmd("gun", quiet=True))
                        zs.append((d.get("cam"), d.get("loc")))
                        time.sleep(0.5)
                else:
                    motion(4.0, False)
                c1, amp = counters(g)
                dump = g.cmd("gun dump", quiet=True)
                i = dump.find("rootAt=")
                key = f"rigfit{fit}/{label}/{len([k for k in res if k.startswith(f'rigfit{fit}/{label}')])}"
                res[key] = dict(delta={k: c1[k] - c0[k] for k in c1}, amp=amp, camLoc=zs[:3], rootAt=dump[i:i + 40] if i >= 0 else None,
                                rootKids=dump[dump.find("rootKids="):dump.find("treeKids=")])
                print(key, res[key]["delta"], "amp", amp, res[key]["rootAt"], zs[:1])
        g.cmd("gun cfg rigfit 1", quiet=True)
    finally:
        try:
            gs.vremu("/stopall"), gs.vremu(gs.HMD0)
        except OSError:
            pass
        g._close()
    out = os.path.join(gs.OUT, time.strftime("rigfit-%Y%m%d-%H%M%S.json"))
    json.dump(res, open(out, "w"), indent=1)
    print("saved", out)


if __name__ == "__main__":
    main()
