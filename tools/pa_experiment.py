"""Power armour HUD + projected Pip-Boy test (SteamVR + vremu, QASmoke). Numbers only.
python pa_test.py [--only hud,pip] """
import json
import math
import os
import re
import struct
import sys
import time
import urllib.request

sys.path.insert(0, r"G:\Fallout4ToVR\plugin\tools")
from fo4test import Game, game_pid, parse_kv, say  # noqa: E402
import camdir_experiment as ce  # noqa: E402
import numpy as np  # noqa: E402

STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
VREMU = "http://127.0.0.1:8766"
TS = time.strftime("%Y%m%d-%H%M%S")
OUT = os.path.join(r"G:\Fallout4ToVR\work\pa", TS)
os.makedirs(OUT, exist_ok=True)
RES = []


def vremu(path):
    with urllib.request.urlopen(VREMU + path, timeout=3) as r:
        return r.read().decode("utf-8", "replace")


def rec(d):
    say("  " + json.dumps(d))
    RES.append(d)


def bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    a = np.frombuffer(d, np.uint8, offset=off, count=abs(h) * w * bpp // 8).reshape(abs(h), w, bpp // 8)
    return a if h < 0 else a[::-1]


def diff(a, b, thr=8):
    m = np.abs(a[..., :3].astype(int) - b[..., :3].astype(int)).max(axis=2) > thr
    n = int(m.sum())
    if not n:
        return dict(px=0)
    ys, xs = np.nonzero(m)
    h, w = m.shape
    grid = [int(m[i * h // 3:(i + 1) * h // 3, j * w // 3:(j + 1) * w // 3].sum()) for i in range(3) for j in range(3)]
    return dict(px=n, pct=round(100.0 * n / m.size, 2), bbox=[int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())], grid=grid)


def num_after(s, key):
    m = re.search(r"\b" + key + r"=(-?[\d.]+)", s)
    return float(m.group(1)) if m else None


def vec_after(s, key):
    m = re.search(key + r" (-?[\d.]+),(-?[\d.]+),(-?[\d.]+)(?:,(-?[\d.]+))?", s)
    return [float(x) for x in m.groups() if x is not None] if m else None


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # v' = q v q*
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return [vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)]


def yaw_pitch(d):
    return math.degrees(math.atan2(-d[0], -d[2])), math.degrees(math.asin(max(-1, min(1, d[1]))))


class Pointer:
    def __init__(self, g):
        self.g = g
        self.pos = [0.15, 1.35, -0.15]
        self.ypr = [0.0, 20.0, 0.0]
        self.sy = 1.0
        self.sp = 1.0

    def set(self):
        vremu(f"/pose?dev=right&pos={self.pos[0]:.4f},{self.pos[1]:.4f},{self.pos[2]:.4f}&ypr={self.ypr[0]:.3f},{self.ypr[1]:.3f},{self.ypr[2]:.3f}")
        time.sleep(0.35)

    def status(self):
        return self.g.cmd("pipboy", quiet=True)

    def aim_at(self, u, v, iters=8):
        s = self.status()
        qp = vec_after(s, "quad pos")
        qq = vec_after(s, "quat")
        sz = re.search(r"size ([\d.]+)x([\d.]+)", s)
        if not (qp and qq and sz):
            return dict(err="no quad pose", s=s[:300])
        W, H = float(sz.group(1)), float(sz.group(2))
        last_err = None
        for it in range(iters):
            self.set()
            s = self.status()
            qp = vec_after(s, "quad pos")
            qq = vec_after(s, "quat")
            ap = vec_after(s, r"\| aim pos")
            ad = vec_after(s, "dir")
            if not (ap and ad and qp):
                return dict(err="no aim", s=s[:300])
            off = qrot(qq, [(u - 0.5) * W, (0.5 - v) * H, 0.0])
            tgt = [qp[i] + off[i] for i in range(3)]
            want = [tgt[i] - ap[i] for i in range(3)]
            n = math.sqrt(sum(x * x for x in want))
            want = [x / n for x in want]
            wy, wp = yaw_pitch(want)
            ay, apt = yaw_pitch(ad)
            ey = (wy - ay + 180) % 360 - 180
            ep = wp - apt
            err = math.hypot(ey, ep)
            if last_err is not None and err > last_err * 1.3 and it < 4:
                # a sign is wrong: flip both and retry from here (cheap heuristic)
                self.sy, self.sp = -self.sy, -self.sp
            last_err = err
            if err < 0.05:
                break
            self.ypr[0] += self.sy * ey
            self.ypr[1] += self.sp * ep
        s = self.status()
        return dict(onQuad=num_after(s, "onQuad"), u=num_after(s, "u"), v=num_after(s, "v"), err_deg=round(last_err or -1, 3), ypr=[round(x, 2) for x in self.ypr])

    def click(self):
        vremu("/input?dev=right&trigger=1&ms=250")
        time.sleep(0.9)


def pa_counts(g):
    s = g.cmd("pa", quiet=True)
    return {k: num_after(s, k) for k in ("captures", "composites", "h2PowerArmor", "paCaptured", "h2Rain", "rainSkips", "pipScreenSkips")}, s


def step_enter(g, e):
    g.cmd(f"console player.additem {ce.FUSION_CORE} 2")
    time.sleep(0.5)
    g.cmd("console player.placeatme 0004A9C1 1")
    time.sleep(2.0)
    ref = parse_kv(g.cmd("pointer findref 4A9C1", quiet=True)).get("ref")
    if ref and ref != "00000000":
        g.cmd(f"console {ref}.activate player", quiet=True)
        how = ("console", ref)
    else:
        d, seen, how = ce.try_activate(e, "0004A9C1", "Power Armor", wait=4)
    rec(dict(step="activate", how=str(how)))
    t0 = time.time()
    while time.time() - t0 < 25:
        c, _ = pa_counts(g)
        st = parse_kv(g.cmd("camdir", quiet=True)).get("state")
        if (c["paCaptured"] or 0) > 30 and st == "0":
            break
        time.sleep(0.5)
    time.sleep(2.0)
    c, s = pa_counts(g)
    rec(dict(step="in PA", after_s=round(time.time() - t0, 1), counts=c, camstate=parse_kv(g.cmd("camdir", quiet=True)).get("state"), pa=s[:900]))
    return (c["paCaptured"] or 0) > 0


def step_hud(g):
    menus = g.cmd("menus", quiet=True)
    rec(dict(step="menus in PA", menus=menus))
    p = lambda n: os.path.join(OUT, n).replace("\\", "/")
    g.cmd("hudcap " + p("hud_on.bmp"), quiet=True)
    g.cmd("capture " + p("world_on.bmp"), quiet=True)
    time.sleep(1.5)
    c1, _ = pa_counts(g)
    g.cmd("pa hud off", quiet=True)
    time.sleep(1.0)
    c2, _ = pa_counts(g)
    g.cmd("capture " + p("world_off.bmp"), quiet=True)
    g.cmd("hudcap " + p("hud_off.bmp"), quiet=True)
    time.sleep(1.5)
    g.cmd("pa hud on", quiet=True)
    time.sleep(1.0)
    c3, _ = pa_counts(g)
    g.cmd("capture " + p("world_on2.bmp"), quiet=True)
    time.sleep(1.0)
    hudline = g.cmd("hud", quiet=True)
    last = re.search(r"last hudcap (.*?) \| paHud", hudline)
    r = dict(step="PA HUD layer", counts_on=c1, counts_off=c2, counts_on_again=c3, hudcap=(last.group(1) if last else hudline[-1500:]))
    try:
        won, woff, won2 = bmp(p("world_on.bmp")), bmp(p("world_off.bmp")), bmp(p("world_on2.bmp"))
        r["world_on_vs_off"] = diff(won, woff)
        r["world_on_vs_on2"] = diff(won, won2)
        pa = bmp(p("hud_on_pa.bmp"))
        alpha = pa[..., 3]
        r["pa_alpha_gt4"] = int((alpha > 4).sum())
        # do the PA dash pixels (alpha>32) account for the world difference?
        m = np.abs(won[..., :3].astype(int) - woff[..., :3].astype(int)).max(axis=2) > 8
        if pa.shape[:2] == m.shape:
            a = alpha > 4
            # dilate a by 4 px
            from numpy.lib.stride_tricks import sliding_window_view
            ap = np.pad(a, 4)
            dil = sliding_window_view(ap, (9, 9)).any(axis=(2, 3))
            r["worlddiff_in_pa_alpha_pct"] = round(100.0 * float((m & dil).sum()) / max(1, int(m.sum())), 2)
            r["pa_alpha_in_worlddiff_pct"] = round(100.0 * float((a & m).sum()) / max(1, int(a.sum())), 2)
    except Exception as ex:
        r["bmp_error"] = repr(ex)
    rec(r)


def step_pip(g):
    vremu("/press?dev=right&btn=b&ms=150")
    time.sleep(2.0)
    s = g.cmd("pipboy", quiet=True)
    rec(dict(step="pip open", camstate=parse_kv(g.cmd("camdir", quiet=True)).get("state"), status=s[:2500],
              counts=pa_counts(g)[0], view=g.cmd("status", quiet=True)[:400]))
    p = lambda n: os.path.join(OUT, n).replace("\\", "/")
    g.cmd("pipcap " + p("pip.bmp"), quiet=True)
    g.cmd("capture " + p("world_pip.bmp"), quiet=True)
    time.sleep(1.5)
    s = g.cmd("pipboy", quiet=True)
    m = re.search(r"last pipcap (.*?) \| tint", s)
    rec(dict(step="pipcap", pipcap=m.group(1) if m else s[-1200:]))
    ptr = Pointer(g)
    # aim accuracy at a few targets
    acc = []
    for (u, v) in ((0.5, 0.5), (0.2, 0.3), (0.8, 0.7), (0.5, 0.05)):
        acc.append(dict(target=[u, v], got=ptr.aim_at(u, v)))
    rec(dict(step="aim accuracy", results=acc))
    # header clicks: page per click
    clicks = []
    for u in (0.35, 0.5, 0.65, 0.8, 0.2):
        a = ptr.aim_at(u, 0.035)
        s0 = g.cmd("pipboy", quiet=True)
        page0, tab0 = num_after(s0, "page"), num_after(s0, "tab")
        ptr.click()
        s1 = g.cmd("pipboy", quiet=True)
        clicks.append(dict(u=u, v=0.035, aim=a, page=[page0, num_after(s1, "page")], tab=[tab0, num_after(s1, "tab")],
                           sent_down=num_after(s1, "down"), lastInject=(re.search(r"lastInject (.*?) \| last pipcap", s1) or [None, None])[1]))
    rec(dict(step="header clicks", clicks=clicks))
    g.cmd("pipcap " + p("pip_after.bmp"), quiet=True)
    time.sleep(1.0)
    # close
    vremu("/pose?dev=right&ypr=0,0,0")
    vremu("/press?dev=right&btn=b&ms=150")
    time.sleep(2.0)
    s = g.cmd("pipboy", quiet=True)
    rec(dict(step="pip close", wrist=num_after(s, "wrist"), projected=num_after(s, "projected"), camstate=parse_kv(g.cmd("camdir", quiet=True)).get("state"),
              menus=g.cmd("menus", quiet=True)[:600], counts=pa_counts(g)[0]))


def main():
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else ["hud", "pip"]
    while game_pid():
        say("Fallout4 running - waiting")
        time.sleep(20)
    vremu("/stopall")
    vremu("/reset")
    with Game(runtime_json=STEAMVR_JSON) as g:
        mark = g.log_mark()
        e = ce.Exp(g, True)
        g.coc("QASmoke")
        time.sleep(3.0)
        rec(dict(step="baseline", pa=g.cmd("pa", quiet=True)[:600]))
        ok = False
        try:
            ok = step_enter(g, e)
        except Exception as ex:
            rec(dict(step="enter", error=repr(ex)))
        if ok:
            for s in only:
                try:
                    dict(hud=lambda: step_hud(g), pip=lambda: step_pip(g))[s]()
                except Exception as ex:
                    rec(dict(step=s, error=repr(ex)))
        log = [l for l in g.log_since(mark).splitlines() if re.search(r"pipboy|hud pa|hud H2|power armour|ERROR|view menu|camera state", l)]
        rec(dict(step="log", lines=log[-60:]))
        try:
            vremu("/stopall")
        except OSError:
            pass
    with open(os.path.join(OUT, "pa_test.json"), "w", encoding="utf-8") as f:
        json.dump(RES, f, indent=1)
    say("wrote " + OUT)


if __name__ == "__main__":
    main()
