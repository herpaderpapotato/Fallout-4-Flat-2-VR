"""Analyze a 'gun trace' CSV (FloatingGun.cpp per-event ring): per frame, which writer produced the WEAPON / hand
values each reader saw. Prints the render-collect series (what the renderer read) attributed to the last writer.

    python -I gun_trace_analyze.py <trace.csv>
"""
import csv
import math
import sys
from collections import Counter, defaultdict


def v(r, k):
    return (float(r[k + "_x"]), float(r[k + "_y"]), float(r[k + "_z"]))


def d(a, b):
    return math.dist(a, b)


def main(path):
    rows = list(csv.DictReader(open(path)))
    print(f"{len(rows)} events, threads: " + ", ".join(f"{t}:{n}" for t, n in Counter((r['site'], r['tid']) for r in rows).most_common(12)))
    last = {}           # site -> last row
    reads = []
    writers_seen = Counter()
    order = defaultdict(list)
    for r in rows:
        s = r["site"]
        order[r["frame"]].append(s)
        if s in ("render_collect", "present"):
            # attribute: which known writer ran last before this read
            cand = {k: float(last[k]["tUs"]) for k in ("fpupdate_post", "gun_write", "anim_post") if k in last}
            lw = max(cand, key=cand.get) if cand else "-"
            w = v(r, "wW")
            gw = v(last["gun_write"], "wW") if "gun_write" in last else None
            fp = v(last["fpupdate_post"], "wW") if "fpupdate_post" in last else None
            reads.append(dict(site=s, frame=r["frame"], t=float(r["tUs"]), lastWriter=lw, wW=w,
                              dGun=d(w, gw) if gw else None, dEngine=d(w, fp) if fp else None,
                              rh=v(r, "rh"), cam=v(r, "cam"), culled=r["culledMask"]))
        if s == "watch_change":
            cand = {k: float(last[k]["tUs"]) for k in ("anim_pre", "anim_post", "fpupdate_pre", "fpupdate_post", "gun_entry", "gun_write", "gun_exit") if k in last}
            lw = max(cand, key=cand.get) if cand else "-"
            writers_seen[lw] += 1
        last[s] = r
    # typical per-frame order
    seqs = Counter(" > ".join(x for x in o if x != "watch_change") for o in order.values())
    print("\nper-frame site order (top):")
    for k, n in seqs.most_common(6):
        print(f"  {n:4d}x {k}")
    print("\nwatcher-seen changes, by the last known site before them:", dict(writers_seen))
    for site in ("render_collect", "present"):
        rs = [x for x in reads if x["site"] == site]
        if not rs:
            continue
        cls = Counter()
        series = []
        for x in rs:
            c = "gun" if x["dGun"] is not None and x["dGun"] < 0.5 else "engine" if x["dEngine"] is not None and x["dEngine"] < 0.5 else "other"
            cls[c] += 1
            series.append(c[0])
        print(f"\n{site}: {len(rs)} reads; value matches: {dict(cls)}; last writer before the read: {dict(Counter(x['lastWriter'] for x in rs))}")
        print("  series (g=our value, e=engine/animated value, o=other):", "".join(series[:200]))
        wz = [x["wW"] for x in rs]
        jumps = [d(wz[i], wz[i - 1]) for i in range(1, len(wz))]
        if jumps:
            print(f"  WEAPON frame-to-frame step at {site}: max {max(jumps):.2f} u, median {sorted(jumps)[len(jumps)//2]:.2f} u")
        rh = [x["rh"] for x in rs]
        rj = [d(rh[i], rh[i - 1]) for i in range(1, len(rh))]
        if rj:
            print(f"  RArm_Hand step: max {max(rj):.2f} u, median {sorted(rj)[len(rj)//2]:.2f} u; culled masks {dict(Counter(x['culled'] for x in rs))}")
        cz = [x["cam"] for x in rs]
        cj = [d(cz[i], cz[i - 1]) for i in range(1, len(cz))]
        if cj:
            print(f"  camera root step: max {max(cj):.2f} u, median {sorted(cj)[len(cj)//2]:.2f} u")


if __name__ == "__main__":
    main(sys.argv[1])
