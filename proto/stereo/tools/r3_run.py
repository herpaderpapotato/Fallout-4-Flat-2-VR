"""Stereo round 3 driver (SteamVR + vremu, test save vrtest_sanctuary). Run with a Python that has numpy.

    python r3_run.py <out-tag> [--keys=Section.key=value,...] [--steps=a,b,...]

--keys: temporary deployed-ini changes for this launch only (per line; a missing key is inserted under its section and
removed again as soon as the plugin pipe is up; other lines are never touched).
Steps (in order given):
  base      IPD-0 test eyes, TAA per eye, SAO cut: back-buffer halves (pad-aware) + per-pass profile
  cuts      the same with each cut effect switched on in turn (mb, dof, flare, ssr, tiled, sao)
  saots     SAO switch-on time series: halves at t = 0.5 .. 12 s after "stereocut sao 0"
  fwd       water/effects mask (skip g007 + g004) halves at 2 yaws
  sky       real eyes, static + micro-motion pose: per-frame time series of edge regions (captureburst)
Results: work/stereo3/<tag>-<ts>/results.json, BMPs, log.
"""
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, r"G:\Fallout4ToVR\plugin\tools")
sys.path.insert(0, r"G:\Fallout4ToVR\work\stereo2")
sys.path.insert(0, HERE)
import fo4test  # noqa: E402
from fo4test import Game, game_pid, say, INI  # noqa: E402
from stereo_experiments import STEAMVR_JSON, bmp_done, wait_game_free  # noqa: E402
import halves_np as H  # noqa: E402

SAVE = "vrtest_sanctuary"
META_SIM_JSON = r"C:\Program Files\MetaXRSimulator\v207.1\meta_openxr_simulator.json"
RUNTIME = [STEAMVR_JSON]  # --runtime=meta: Meta XR Simulator (Quest 3 profile)


# ---------------------------------------------------------------- ini (per line, section-aware)
PREFS = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\Fallout4Prefs.ini")


def _read(path=None):
    with open(path or INI, encoding="utf-8", newline="") as fh:
        return fh.read()


def _write(text, path=None):
    tmp = (path or INI) + ".tmp-r3"
    with open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    os.replace(tmp, path or INI)


def _section_span(text, sec):
    m = re.search(rf"(?m)^\[{re.escape(sec)}\][^\r\n]*\r?\n", text)
    if not m:
        return None
    n = re.search(r"(?m)^\[", text[m.end():])
    return m.end(), (m.end() + n.start()) if n else len(text)


def get_key(sec, key):
    t = _read()
    sp = _section_span(t, sec)
    if not sp:
        return None
    m = re.search(rf"(?m)^{re.escape(key)}=([^\r\n]*)", t[sp[0]:sp[1]])
    return m.group(1) if m else None


def set_key(sec, key, value, path=None):
    """returns the previous value (None = the line did not exist and was inserted)"""
    t = _read(path)
    sp = _section_span(t, sec)
    if not sp:
        raise SystemExit(f"ini has no [{sec}]")
    body = t[sp[0]:sp[1]]
    m = re.search(rf"(?m)^{re.escape(key)}=([^\r\n]*)", body)
    nl = "\r\n" if "\r\n" in t else "\n"
    if m:
        old = m.group(1)
        body = body[:m.start()] + f"{key}={value}" + body[m.end():]
    else:
        old = None
        body = f"{key}={value}{nl}" + body
    _write(t[:sp[0]] + body + t[sp[1]:], path)
    return old


def del_key(sec, key, path=None):
    t = _read(path)
    sp = _section_span(t, sec)
    body = t[sp[0]:sp[1]]
    body2 = re.sub(rf"(?m)^{re.escape(key)}=[^\r\n]*\r?\n", "", body, count=1)
    _write(t[:sp[0]] + body2 + t[sp[1]:], path)


def launch(keys, prefs=None):
    """keys: temporary deployed-ini lines; prefs: temporary Fallout4Prefs.ini lines (read by the engine at startup),
    both restored as soon as the plugin pipe is up"""
    wait_game_free()
    orig_text = _read()
    orig_prefs = _read(PREFS)
    if not orig_text.strip() or not orig_prefs.strip():
        raise SystemExit("deployed ini / prefs empty")
    saved = []
    try:
        for (sec, key), v in keys.items():
            saved.append((None, sec, key, set_key(sec, key, v)))
        for (sec, key), v in (prefs or {}).items():
            saved.append((PREFS, sec, key, set_key(sec, key, v, PREFS)))
        g = Game(runtime_json=RUNTIME[0])
        g.__enter__()
    finally:
        for path, sec, key, old in reversed(saved):
            if old is None:
                del_key(sec, key, path)
            else:
                set_key(sec, key, old, path)
        say(f"ini restored (identical to before launch: {_read() == orig_text}); prefs restored: {_read(PREFS) == orig_prefs}")
    return g


# ---------------------------------------------------------------- game helpers
def cmd(g, c):
    r = g.cmd(c, quiet=True)
    say(f"> {c} -> {r[:140]}")
    return r


def vremu(path):
    import urllib.request
    try:
        with urllib.request.urlopen("http://127.0.0.1:8766" + path, timeout=3) as r:
            return r.read().decode("utf-8", "replace")[:80]
    except Exception as ex:  # noqa: BLE001
        return f"ERR {ex!r}"


def wait_ingame(g, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if g.ready().get("ingame") == "1":
                return time.time() - t0
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)
    return None


def capture(g, out, name, timeout=90):
    path = os.path.join(out, name + ".bmp").replace("\\", "/")
    g.cmd("capture " + path, quiet=True)
    t0 = time.time()
    while not bmp_done(path):
        if time.time() - t0 > timeout:
            raise RuntimeError("capture timed out " + name)
        time.sleep(0.1)
    time.sleep(0.1)
    return path


def burst(g, out, stem, n, timeout=120):
    stem = os.path.join(out, stem).replace("\\", "/")
    mark = Game.log_mark()
    g.cmd(f"captureburst {n} {stem}", quiet=True)
    t0 = time.time()
    while "captureburst:" not in g.log_since(mark) or not bmp_done(f"{stem}_{n - 1:02}.bmp"):
        if time.time() - t0 > timeout:
            raise RuntimeError("burst timed out")
        time.sleep(0.25)
    time.sleep(0.3)
    return [f"{stem}_{i:02}.bmp" for i in range(n)]


PASS_RE = re.compile(r"stereopass: (\S+) #(\d+) (.*?) (\d+)x(\d+) fmt (\d+) \| (.*)$")


def passcap(g, tag, timeout=120, bmpdir=None, groups=None):
    mark = Game.log_mark()
    extra = f" {bmpdir.replace(chr(92), '/')} {groups}" if bmpdir and groups else ""
    cmd(g, f"stereopasscap {tag}{extra}")
    t0 = time.time()
    while f"stereopass: {tag} done" not in g.log_since(mark):
        if time.time() - t0 > timeout:
            raise RuntimeError("passcap timed out")
        time.sleep(0.5)
    rows = []
    for ln in g.log_since(mark).splitlines():
        if f"stereopass: {tag} #" in ln:
            rows.append(ln.split("stereopass: ", 1)[1])
    return rows


def summarize_passes(rows, thr=99.9):
    """passes whose interior within1 < thr (first divergences)"""
    bad = []
    for r in rows:
        m = re.search(r"interior within1 ([\d.]+)%", r)
        if m and float(m.group(1)) < thr:
            bad.append(r)
    return bad


def halves(path, pad):
    img = H.read_bmp(path)
    d = H.diffmap(img, pad)
    W = d.shape[1]
    c = d[:, 64:W - 64]
    bad = c > 1
    return {"within1": round(float((~bad).mean() * 100), 4), "within4": round(float((c <= 4).mean() * 100), 4),
            "interior256": round(float((d[:, 256:W - 256] <= 1).mean() * 100), 4),
            "seam64_256": round(float((d[:, W - 256:W - 64] <= 1).mean() * 100), 4),
            "outer64_256": round(float((d[:, 64:256] <= 1).mean() * 100), 4)}


def ipd0(g):
    for c in ("stereotest 1", "stereofov 45", "stereoipd 0", "stereoclear 1"):
        cmd(g, c)


# ---------------------------------------------------------------- steps
def step_base(g, out, res, pad, variants=None):
    ipd0(g)
    base = ["stereocut sao 1", "stereocut taa 0", "stereotaa 1"]
    for c in base:
        cmd(g, c)
    variants = variants or [("base", [], "9,10,18,23,137")]
    for v in variants:
        tag, cmds = v[0], v[1]
        groups = v[2] if len(v) > 2 else None
        spin = len(v) > 3 and v[3]
        for c in cmds:
            cmd(g, c)
        time.sleep(5)
        if spin:
            vremu("/rate?dev=hmd&wyaw=40&ms=8000&timeout=8000")
            time.sleep(1.5)
        bb = capture(g, out, f"bb_{tag}")
        rows = passcap(g, tag, bmpdir=out if groups else None, groups=groups)
        bb2 = capture(g, out, f"bb_{tag}_b")
        if spin:
            vremu("/rate?dev=hmd&stop=1")
            vremu("/pose?dev=hmd&ypr=0,0,0")
        r = {"halves": halves(bb, pad), "halves_b": halves(bb2, pad), "passes": rows, "bad": summarize_passes(rows)}
        res[tag] = r
        say(f"{tag}: halves {r['halves']} | b {r['halves_b']['within1']} | passes {len(rows)}, below 99.9: {len(r['bad'])}")
        for b in r["bad"][:12]:
            say("   " + b[:230])
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def step_cuts(g, out, res, pad):
    variants = [("fxaa", ["stereocut taa 1"]), ("taa", ["stereocut taa 0"]),
                ("spin_taa", [], None, True),
                ("mb_spin", ["stereosetting mb 1", "stereocut mb 0"], "136,137", True),
                ("mb_spin_mono", ["stereopost 0"], "136,137", True),
                ("dof", ["stereopost 1", "stereosetting mb 0", "stereocut mb 1", "stereocut dof 0"]),
                ("ssr", ["stereocut dof 1", "stereocut ssr 0"], "147,144,145,146,10"),
                ("tiled", ["stereocut ssr 1", "stereocut tiled 0"]), ("sao", ["stereocut tiled 1", "stereocut sao 0"], "125,126,127,124")]
    step_base(g, out, res, pad, variants)
    cmd(g, "stereocut sao 1")


def step_allon(g, out, res, pad):
    """IPD 0, everything per eye switched on (SAO, TAA, DOF, flare, SSR, MB setting as the user's prefs) + TAA variants"""
    ipd0(g)
    on = ["stereocut sao 0", "stereocut taa 0", "stereocut dof 0", "stereocut flare 0", "stereocut ssr 0", "stereocut mb 0",
          "stereotaa 1", "stereojitter 1", "stereopost 1"]
    variants = [("allon", on), ("allon_noaa", ["stereojitter 0"]),
                ("allon_fxaa", ["stereojitter 1", "stereocut taa 1"]), ("allon_taa", ["stereocut taa 0"]),
                ("allon_ssr_noeye", ["stereopost 0"]), ("allon_nossr", ["stereopost 1", "stereocut ssr 1"]),
                ("allon_nosao", ["stereocut ssr 0", "stereocut sao 1"]), ("allon_b", ["stereocut sao 0"])]
    for v in variants:
        tag, cmds = v[0], v[1]
        groups = v[2] if len(v) > 2 else None
        for c in cmds:
            cmd(g, c)
        time.sleep(5)
        bb = capture(g, out, f"bb_{tag}")
        rows = passcap(g, tag, bmpdir=out if groups else None, groups=groups)
        r = {"halves": halves(bb, pad), "passes": rows, "bad": summarize_passes(rows)}
        res[tag] = r
        say(f"{tag}: halves {r['halves']} | below 99.9: {len(r['bad'])}")
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    say(cmd(g, "stereopost 1"))


def step_pause(g, out, res, pad):
    """edge quads after the pause menu (gun agent, 20:38 captures): real eyes, pause open/close, then frames + passes"""
    big = os.path.join("E:/Fallout4ToVR-overflow/stereo3", os.path.basename(out))
    os.makedirs(big, exist_ok=True)
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    res["pause"] = {}
    a = capture(g, out, "pause_before")
    cmd(g, "uimsg PauseMenu show")
    time.sleep(3)
    cmd(g, "uimsg PauseMenu hide")
    time.sleep(2)
    b = capture(g, out, "pause_after")
    rows = passcap(g, "pauseafter", bmpdir=big, groups="all")
    res["pause"]["passes"] = rows
    mark = Game.log_mark()
    cmd(g, "stereodump")
    time.sleep(2)
    res["pause"]["dump"] = [ln for ln in g.log_since(mark).splitlines() if "stereodump:" in ln]
    time.sleep(6)
    c = capture(g, out, "pause_after8s")
    say(f"pause: {len(rows)} passes, dump {len(res['pause']['dump'])} lines")
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def step_flare(g, out, res, pad):
    """lens flare: find a head direction where flares draw (stereopost counters), then halves + passes"""
    ipd0(g)
    cmd(g, "stereocut taa 0")
    cmd(g, "stereocut flare 0")
    found = None
    for pitch in (45, 30, 60, 15):
        for yaw in range(0, 360, 30):
            vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
            time.sleep(1.2)
            a = cmd(g, "stereopost 1")
            time.sleep(0.8)
            b = cmd(g, "stereopost 1")
            fa = int(re.search(r"flare (\d+)", a).group(1))
            fb = int(re.search(r"flare (\d+)", b).group(1))
            if fb > fa:
                found = (yaw, pitch)
                break
        if found:
            break
    res["flare_dir"] = found
    say(f"flare direction: {found}")
    if found:
        step_base(g, out, res, pad, [("flare", [], "151,155,137"), ("flare_mono", ["stereopost 0"], "151,155,137")])
        cmd(g, "stereopost 1")
    cmd(g, "stereocut flare 1")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def step_saots(g, out, res, pad):
    ipd0(g)
    for c in ("stereocut taa 0", "stereocut sao 1"):
        cmd(g, c)
    time.sleep(5)
    t0 = time.time()
    cmd(g, "stereocut sao 0")
    ts = []
    for i in range(14):
        p = capture(g, out, f"saots_{i:02}")
        t = time.time() - t0
        ts.append({"t": round(t, 2), **halves(p, pad)})
        say(f"saots t={t:.1f}s {ts[-1]}")
    res["saots"] = ts
    cmd(g, "stereocut sao 1")
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def step_fwd(g, out, res, pad):
    ipd0(g)
    cmd(g, "stereocut taa 0")
    res["fwd"] = {}
    for yaw in (0, 180):
        vremu(f"/pose?dev=hmd&ypr={yaw},-10,0")
        time.sleep(4)
        full = capture(g, out, f"fwd_all_{yaw}")
        rows = passcap(g, f"fwd{yaw}", bmpdir=out, groups="9,10,4,18,137")
        cmd(g, "stereoskipgroups 7 4")
        time.sleep(3)
        none = capture(g, out, f"fwd_none_{yaw}")
        cmd(g, "stereoskipgroups -1 -1")
        a, b = H.read_bmp(full), H.read_bmp(none)
        La, Ra = H.eyes(a, pad)
        Lb, Rb = H.eyes(b, pad)
        mask = (abs(La - Lb).max(axis=2) > 2) | (abs(Ra - Rb).max(axis=2) > 2)
        d = abs(La - Ra).max(axis=2)
        W = d.shape[1]
        m = mask[:, 64:W - 64]
        dd = d[:, 64:W - 64]
        res["fwd"][yaw] = {"maskShare": float(m.mean() * 100), "within1": float((dd[m] <= 1).mean() * 100),
                           "within4": float((dd[m] <= 4).mean() * 100), "restWithin1": float((dd[~m] <= 1).mean() * 100),
                           "bad": summarize_passes(rows), "passes": rows}
        say(f"fwd yaw {yaw}: { {k: v for k, v in res['fwd'][yaw].items() if k not in ('passes', 'bad')} }")
        for b in res["fwd"][yaw]["bad"][:12]:
            say("   " + b[:230])
    vremu("/pose?dev=hmd&ypr=0,0,0")
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


class Wobble:
    """small HMD yaw/pitch micro-motion through vremu /pose (amplitude deg, frequency Hz), like a resting head"""
    def __init__(self, amp=0.15, hz=1.3, yaw0=0.0, pitch0=0.0):
        import threading
        self.amp, self.hz, self.yaw0, self.pitch0 = amp, hz, yaw0, pitch0
        self.ev = threading.Event()
        self.th = threading.Thread(target=self.run, daemon=True)

    def run(self):
        import math
        t0 = time.time()
        while not self.ev.is_set():
            t = time.time() - t0
            y = self.yaw0 + self.amp * math.sin(2 * math.pi * self.hz * t)
            p = self.pitch0 + 0.6 * self.amp * math.sin(2 * math.pi * self.hz * 0.77 * t + 1.0)
            vremu(f"/pose?dev=hmd&ypr={y:.4f},{p:.4f},0")
            time.sleep(0.01)

    def __enter__(self):
        self.th.start()
        return self

    def __exit__(self, *a):
        self.ev.set()
        self.th.join(2)


def region_stats(files, pad):
    """per consecutive frame pair: % of px changing > 8 levels, per region of each eye: sky (top 45 %, bright/blue,
    excluding the outer/inner 12 % columns), outer edge band, inner edge band, top band (12 % rows), interior"""
    import numpy as np
    imgs = [H.read_bmp(f) for f in files]
    out = []
    for a, b in zip(imgs, imgs[1:]):
        r = {}
        for e, (A, B) in enumerate(zip(H.eyes(a, pad), H.eyes(b, pad))):
            h, W, _ = A.shape
            d = np.abs(A - B).max(axis=2) > 8
            sky = (A[:, :, 0] > A[:, :, 2] + 10) & (A[:, :, 0] > 90)  # BGR: blue dominant and bright
            sky[int(h * 0.55):] = False
            cw, ch = int(W * 0.12), int(h * 0.12)
            outer = np.zeros_like(d)
            inner = np.zeros_like(d)
            if e == 0:
                outer[:, :cw] = True
                inner[:, W - cw:] = True
            else:
                outer[:, W - cw:] = True
                inner[:, :cw] = True
            top = np.zeros_like(d)
            top[:ch] = True
            mid = ~(outer | inner | top)
            ground = np.zeros_like(d)
            ground[int(h * 0.55):] = True
            lum = A.mean(axis=2)
            shadow = ground & (lum < 45)
            lit = ground & (lum >= 45)
            for name, m in (("sky_outer", sky & outer), ("sky_inner", sky & inner), ("sky_top", sky & top),
                            ("sky_mid", sky & mid), ("all", np.ones_like(d)), ("outer", outer), ("inner", inner),
                            ("ground_shadow", shadow), ("ground_lit", lit)):
                r[f"e{e}_{name}"] = round(float(d[m].mean() * 100), 3) if m.any() else None
        out.append(r)
    return out


def step_sky(g, out, res, pad):
    """sky / edge flicker: real eyes, user AA (FXAA cut as deployed) and TAA; static and micro-motion; stereo vs mono"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    res["sky"] = {}
    for yaw, pitch in ((0, 15),):
        vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
        for aa in ("fxaa", "taa"):
            cmd(g, f"stereocut taa {1 if aa == 'fxaa' else 0}")
            for mode in (3, 0):
                cmd(g, f"stereo {mode}")
                for motion in ("static", "wobble"):
                    tag = f"sky_{aa}_m{mode}_{motion}"
                    time.sleep(3)
                    if motion == "wobble":
                        with Wobble(yaw0=yaw, pitch0=pitch):
                            time.sleep(1.5)
                            files = burst(g, out, tag, 10)
                    else:
                        files = burst(g, out, tag, 10)
                    st = region_stats(files, pad if mode == 3 else 0)
                    keys = st[0].keys()
                    mean = {k: round(sum(x[k] for x in st if x[k] is not None) / max(1, sum(1 for x in st if x[k] is not None)), 3) for k in keys}
                    res["sky"][tag] = {"pairs": st, "mean": mean}
                    say(f"{tag}: {mean}")
                    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
        cmd(g, "stereo 3")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def step_saoghost(g, out, res, pad):
    """SAO switch-on convergence, stereo (mode 3) vs mono (mode 0), real eyes: SAO off, look away (yaw 90) and back,
    SAO on, then captures; metric = % px differing > 8 levels from the final (t ~ 14 s) frame"""
    import numpy as np
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    cmd(g, "stereocut taa 0")
    res["saoghost"] = {}
    for mode in (3, 0):
        cmd(g, f"stereo {mode}")
        cmd(g, "stereocut sao 1")
        if mode == 0:
            cmd(g, "stereocuts force")  # mono: the forced cut list switches SAO off
        time.sleep(3)
        vremu("/pose?dev=hmd&ypr=90,0,0")
        time.sleep(4)
        vremu("/pose?dev=hmd&ypr=0,0,0")
        time.sleep(3)
        t0 = time.time()
        cmd(g, "stereocut sao 0" if mode == 3 else "stereocuts auto")  # mono: back to auto = no cuts = SAO on
        caps = []
        for i in range(12):
            caps.append((time.time() - t0, capture(g, out, f"saoghost_m{mode}_{i:02}")))
        time.sleep(4)
        final = H.read_bmp(capture(g, out, f"saoghost_m{mode}_final"))
        ts = []
        for t, pth in caps:
            a = H.read_bmp(pth)
            d = np.abs(a - final).max(axis=2)
            ts.append({"t": round(t, 2), "diff8": round(float((d > 8).mean() * 100), 3), "diff2": round(float((d > 2).mean() * 100), 3)})
        res["saoghost"][f"m{mode}"] = ts
        say(f"saoghost mode {mode}: {[(x['t'], x['diff8']) for x in ts]}")
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "stereocuts auto")
    cmd(g, "stereo 3")
    cmd(g, "stereocut sao 1")


STAT_RE = re.compile(r"stereo: mode (\d) per frame:.*?cpu avg ([\d.]+) ms max ([\d.]+) frame avg ([\d.]+) ms")
GPU_RE = re.compile(r"stereo light: mode (\d).*?gpu avg ([\d.]+) ms \((\d+) frames\)")


def step_perf(g, out, res, pad):
    """cost and pacing, real eyes, user AA: mode 3 vs mode 0, static and turning; 3 stats windows each
    (stats lines every iLogEveryFrames presents: present-to-present frame time, render-thread CPU, GPU world+post)"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    res["perf"] = {}
    for spin in (False, True):
        for mode in (3, 0, 3):
            cmd(g, f"stereo {mode}")
            if spin:
                vremu("/rate?dev=hmd&wyaw=30&ms=60000&timeout=60000")
            time.sleep(2)
            mark = Game.log_mark()
            t0 = time.time()
            rows = []
            while time.time() - t0 < 40:
                txt = g.log_since(mark)
                st = [m.groups() for m in STAT_RE.finditer(txt)]
                gp = [m.groups() for m in GPU_RE.finditer(txt)]
                if len(st) >= 4 and len(gp) >= 4:
                    rows = list(zip(st[1:4], gp[1:4]))
                    break
                time.sleep(0.5)
            status = cmd(g, "status")
            if spin:
                vremu("/rate?dev=hmd&stop=1")
                vremu("/pose?dev=hmd&ypr=0,0,0")
            key = f"m{mode}_{'turn' if spin else 'static'}" + ("_b" if f"m{mode}_{'turn' if spin else 'static'}" in res["perf"] else "")
            res["perf"][key] = {"windows": [{"frameMs": float(a[3]), "cpuMs": float(a[1]), "cpuMax": float(a[2]), "gpuMs": float(b[1])}
                                            for a, b in rows], "status": status}
            say(f"perf {key}: {res['perf'][key]['windows']}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "stereo 3")


def eye_delta(a_path, b_path, pad, mono=False):
    """per-eye difference of two captures of the same view: % px > 4 levels (max over BGR) and mean abs, 64-px bands cut"""
    import numpy as np
    a, b = H.read_bmp(a_path), H.read_bmp(b_path)
    out = {}
    pairs = [("mono", a, b)] if mono else [(f"e{e}", x, y) for e, (x, y) in enumerate(zip(H.eyes(a, pad), H.eyes(b, pad)))]
    for name, x, y in pairs:
        d = np.abs(x - y).max(axis=2)[:, 64:-64]
        out[name] = {"gt4": round(float((d > 4).mean() * 100), 3), "gt16": round(float((d > 16).mean() * 100), 3),
                     "mean": round(float(d.mean()), 3)}
    return out


def step_tiled(g, out, res, pad):
    """tiled deferred lighting per eye (StereoTiled.inl): IPD-0 test eyes, engine tiled lighting forced on with
    iTiledLightingMinLights 0, no AA jitter / SAO / SSR (less temporal noise). Variants: volume path (cut), per-eye tiled,
    mono tiled over the double-wide image (old behaviour); then mono (mode 0) tiled vs volume as the reference delta."""
    ipd0(g)
    for c in ("stereocut sao 1", "stereocut ssr 1", "stereocut taa 1", "stereojitter 0", "stereosetting tiled 1",
              "stereosetting tiledmin 1"):
        cmd(g, c)
    T = res.setdefault("tiled", {})
    variants = [("vol", ["stereocut tiled 1"]), ("vol2", []), ("eye", ["stereotiled 1", "stereocut tiled 0"]),
                ("eye2", []), ("monotiled", ["stereotiled mono"]), ("eye3", ["stereotiled 1"])]
    caps = {}
    for tag, cmds in variants:
        for c in cmds:
            cmd(g, c)
        time.sleep(4)
        bb = capture(g, out, f"tl_{tag}")
        rows = passcap(g, f"tl_{tag}")
        caps[tag] = bb
        st = cmd(g, "stereotiled status")
        tiled_rows = [r for r in rows if " CS (" in r]
        T[tag] = {"halves": halves(bb, pad), "status": st, "cs": tiled_rows,
                  "light": [r for r in rows if " g009" in r or " g010" in r][:6], "bad": summarize_passes(rows)}
        say(f"tiled {tag}: halves {T[tag]['halves']} | {st[:200]}")
        for r in tiled_rows[:12]:
            say("   " + r[:230])
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    T["delta"] = {k: eye_delta(caps[a], caps[b], pad) for k, (a, b) in
                  {"vol_vs_vol2(noise)": ("vol", "vol2"), "eye_vs_eye2(noise)": ("eye", "eye2"), "eye_vs_vol": ("eye", "vol2"),
                   "eye3_vs_vol": ("eye3", "vol2"), "monotiled_vs_vol": ("monotiled", "vol2")}.items()}
    say(f"tiled deltas: {T['delta']}")
    # mono reference: the same switch in mode 0 (cuts keyed on the mode: not applied in mode 0)
    cmd(g, "stereo 0")
    mcaps = {}
    for tag, v in (("mvol", 0), ("mvol2", 0), ("mtiled", 1), ("mtiled2", 1)):
        cmd(g, f"stereosetting tiled {v}")
        time.sleep(4)
        mcaps[tag] = capture(g, out, f"tl_{tag}")
    T["mono_delta"] = {k: eye_delta(mcaps[a], mcaps[b], 0, mono=True) for k, (a, b) in
                       {"vol_vs_vol2(noise)": ("mvol", "mvol2"), "tiled_vs_tiled2(noise)": ("mtiled", "mtiled2"),
                        "tiled_vs_vol": ("mtiled", "mvol2")}.items()}
    say(f"mono tiled deltas: {T['mono_delta']}")
    cmd(g, "stereo 3")
    cmd(g, "stereosetting tiled 1")
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def step_tiledperf(g, out, res, pad):
    """GPU cost, real eyes, user AA: stereo per-eye tiled / stereo volume path / mono tiled / mono volume (tiled forced
    with iTiledLightingMinLights 0); static and turning; stats windows as step_perf"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    cmd(g, "stereosetting tiledmin 1")
    P = res.setdefault("tiledperf", {})
    for spin in (False, True):
        for name, mode, cmds in (("m3_eye", 3, ["stereotiled 1", "stereocut tiled 0", "stereosetting tiled 1"]),
                                 ("m3_vol", 3, ["stereocut tiled 1"]),
                                 ("m0_tiled", 0, ["stereocut tiled 0", "stereosetting tiled 1"]),
                                 ("m0_vol", 0, ["stereosetting tiled 0"]),
                                 ("m3_eye_b", 3, ["stereosetting tiled 1"])):
            cmd(g, f"stereo {mode}")
            for c in cmds:
                cmd(g, c)
            if spin:
                vremu("/rate?dev=hmd&wyaw=30&ms=60000&timeout=60000")
            time.sleep(2)
            mark = Game.log_mark()
            t0 = time.time()
            rows = []
            while time.time() - t0 < 40:
                txt = g.log_since(mark)
                st = [m.groups() for m in STAT_RE.finditer(txt)]
                gp = [m.groups() for m in GPU_RE.finditer(txt)]
                tl = re.findall(r"tiled frames (\d+) eye (\d+) (\d+) fail (\d+)", txt)
                if len(st) >= 4 and len(gp) >= 4:
                    rows = list(zip(st[1:4], gp[1:4]))
                    break
                time.sleep(0.5)
            if spin:
                vremu("/rate?dev=hmd&stop=1")
                vremu("/pose?dev=hmd&ypr=0,0,0")
            key = f"{name}_{'turn' if spin else 'static'}"
            P[key] = {"windows": [{"frameMs": float(a[3]), "cpuMs": float(a[1]), "cpuMax": float(a[2]), "gpuMs": float(b[1])}
                                  for a, b in rows], "tiled": tl[-3:] if tl else None}
            say(f"tiledperf {key}: {P[key]}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "stereo 3")
    cmd(g, "stereosetting tiled 1")


def parse_tans(st):
    m = re.search(r"tan mono ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) eyeL ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) "
                  r"eyeR ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)", st)
    v = [float(x) for x in m.groups()]
    return v[0:4], [v[4:8], v[8:12]]


def shadow_map(stereo_path, mono_path, mono_tan, eye_tans, blk=64):
    """per eye: map every eye-region pixel to the mono image pixel of the same view direction (IPD parallax ignored),
    block-average luminance (blk px), ratio stereo/mono. Returns per eye the share of blocks per eye-local column bin
    (16 bins over the region) with ratio > 1.35 (stereo brighter = shadow missing) and < 0.74 (stereo darker)."""
    import numpy as np
    S = H.read_bmp(stereo_path).astype(np.float32)
    M = H.read_bmp(mono_path).astype(np.float32)
    h, w, _ = S.shape
    hm, wm, _ = M.shape
    wp = w // 2
    l, r, t, b = mono_tan
    out = {}
    for e in range(2):
        le, re_, te, be = eye_tans[e]
        reg = S[:, e * wp:(e + 1) * wp].mean(axis=2)
        xs = (np.arange(wp) + 0.5) / wp
        ys = (np.arange(h) + 0.5) / h
        tx = le + xs * (re_ - le)
        ty = te - ys * (te - be)
        mx = np.clip(((tx - l) / (r - l) * wm).astype(int), 0, wm - 1)
        my = np.clip(((t - ty) / (t - b) * hm).astype(int), 0, hm - 1)
        mono = M.mean(axis=2)[my][:, mx]
        nb_y, nb_x = h // blk, wp // blk
        a = reg[:nb_y * blk, :nb_x * blk].reshape(nb_y, blk, nb_x, blk).mean(axis=(1, 3))
        m = mono[:nb_y * blk, :nb_x * blk].reshape(nb_y, blk, nb_x, blk).mean(axis=(1, 3))
        ratio = (a + 4) / (m + 4)
        bins = 16
        cols = np.array_split(np.arange(nb_x), bins)
        out[f"e{e}"] = {"brighter": [round(float((ratio[:, c] > 1.35).mean() * 100), 1) for c in cols],
                        "darker": [round(float((ratio[:, c] < 0.74).mean() * 100), 1) for c in cols],
                        "meanAbs": round(float(np.abs(reg - mono).mean()), 2)}
    return out


def step_shadow(g, out, res, pad):
    """shadow strip (user, headset): real eyes, look at shadowed ground at several yaws; stereo with scissor remap /
    old intersect / no engine scissor vs the mono image of the same pose (block luminance ratio per eye column)"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    S = res.setdefault("shadow", {})
    for yaw, pitch in ((0, -25), (90, -25), (180, -25), (270, -25)):
        vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
        r = S.setdefault(f"y{yaw}", {})
        cmd(g, "stereo 3")
        time.sleep(3)
        mark = Game.log_mark()
        cmd(g, "stereoscissor log")
        time.sleep(1)
        r["scissorLog"] = [ln.split("stereoscissor: ", 1)[1] for ln in g.log_since(mark).splitlines() if "stereoscissor:" in ln][:40]
        st = cmd(g, "stereoscissor")
        r["status"] = st
        mono_tan, eye_tans = parse_tans(st)
        caps = {}
        for mode in (1, 0, 2):
            cmd(g, f"stereoscissor {mode}")
            time.sleep(1.5)
            caps[mode] = capture(g, out, f"sh_y{yaw}_s{mode}")
        cmd(g, "stereoscissor 1")
        cmd(g, "stereo 0")
        time.sleep(3)
        caps["mono"] = capture(g, out, f"sh_y{yaw}_mono")
        cmd(g, "stereo 3")
        for mode in (1, 0, 2):
            r[f"s{mode}_vs_mono"] = shadow_map(caps[mode], caps["mono"], mono_tan, eye_tans)
        r["s0_vs_s1"] = eye_delta(caps[0], caps[1], pad)
        r["s2_vs_s1"] = eye_delta(caps[2], caps[1], pad)
        say(f"shadow y{yaw}: scissored split draws logged {len(r['scissorLog'])}; s1 vs mono {r['s1_vs_mono']}; s0 vs s1 {r['s0_vs_s1']}")
        for ln in r["scissorLog"][:8]:
            say("   " + ln[:220])
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    vremu("/pose?dev=hmd&ypr=0,0,0")


def thumb(path_png, imgs, scale=8):
    """side-by-side downscaled BGR images (list of HxWx3 int arrays) -> PNG (RGB)"""
    import numpy as np
    from np_png import write_png
    parts = [im[::scale, ::scale, ::-1].clip(0, 255).astype(np.uint8) for im in imgs]
    h = min(p.shape[0] for p in parts)
    write_png(path_png, np.concatenate([p[:h] for p in parts], axis=1))


def far_delta(a_path, b_path, pad, out_png=None):
    """per eye: % px differing > 8 levels between two captures of the same pose (far-plane A/B), by eye-local
    quadrant (tl, tr, bl, br) + a difference thumbnail"""
    import numpy as np
    a, b = H.read_bmp(a_path), H.read_bmp(b_path)
    res = {}
    diffs = []
    for e, (x, y) in enumerate(zip(H.eyes(a, pad), H.eyes(b, pad))):
        d = np.abs(x - y).max(axis=2) > 8
        h, w = d.shape
        q = {"all": d.mean(), "tl": d[:h // 2, :w // 2].mean(), "tr": d[:h // 2, w // 2:].mean(),
             "bl": d[h // 2:, :w // 2].mean(), "br": d[h // 2:, w // 2:].mean()}
        res[f"e{e}"] = {k: round(float(v * 100), 3) for k, v in q.items()}
        diffs.append(np.repeat((d * 255).astype(np.int16)[:, :, None], 3, axis=2))
    if out_png:
        thumb(out_png, [a, b, np.concatenate(diffs, axis=1)])
    return res


def step_far(g, out, res, pad):
    """far-plane clipping of z = w sky / fullscreen geometry in the eyes (sky flicker, shadow triangle): real eyes, extra
    eye cant (stereocant) as HMD eyes canted against the head, eye far scale 1 (old) vs 2 (fix), sky and ground views"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    F = res.setdefault("far", {})
    for yaw, pitch in ((0, 15), (0, -25), (120, 10)):
        vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
        for cant in ("0 0", "0.6 0.4", "-0.6 -0.4"):
            cmd(g, f"stereocant {cant}")
            caps = {}
            for far in (1, 2):
                cmd(g, f"stereofar {far}")
                time.sleep(1.5)
                caps[far] = capture(g, out, f"far_y{yaw}p{pitch}_c{cant.replace(' ', '_')}_f{far}")
            tag = f"y{yaw}p{pitch}_c{cant.replace(' ', '_')}"
            F[tag] = far_delta(caps[1], caps[2], pad, os.path.join(out, f"fard_{tag}.png"))
            say(f"far {tag}: f1 vs f2 {F[tag]}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "stereocant 0 0")
    cmd(g, "stereofar 2")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def step_tiledcrash(g, out, res, pad):
    """tiled lighting forced on at runtime (prefs had it off at startup): mono first (engine only), then stereo with the
    cut, then engine tiled in stereo without the split (stereotiled mono), then the per-eye split. Status after each."""
    T = res.setdefault("tiledcrash", {})

    def alive(tag):
        time.sleep(4)
        try:
            st = g.cmd("stereotiled status", quiet=True)
            T[tag] = st[:300]
            say(f"tiledcrash {tag}: alive | {st[:200]}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
            return True
        except Exception as ex:  # noqa: BLE001
            T[tag] = f"DEAD {ex!r}"
            say(f"tiledcrash {tag}: GAME GONE ({ex!r})")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
            return False

    cmd(g, "stereotest 1")
    cmd(g, "stereoipd 0")
    cmd(g, "stereo 0")
    cmd(g, "stereosetting tiled 1")
    if not alive("mono_engine_tiled_minlights_default"):
        return
    cmd(g, "stereosetting tiledmin 1")
    if not alive("mono_engine_tiled_minlights1"):
        return
    cmd(g, "stereo 3")
    cmd(g, "stereocut tiled 1")
    if not alive("stereo_cut"):
        return
    cmd(g, "stereotiled mono")
    cmd(g, "stereocut tiled 0")
    if not alive("stereo_engine_tiled_nosplit"):
        return
    cmd(g, "stereotiled 1")
    alive("stereo_split")


def step_far2(g, out, res, pad):
    """eye vs mono depth-clip: old (far 1, no clamp) / far 2 / far 2 + twin z clamp, under eye cant (canted HMD eyes),
    sky view and ground view; deltas against the full fix with thumbnails"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    F = res.setdefault("far2", {})
    for yaw, pitch in ((0, -25), (0, 15)):
        vremu(f"/pose?dev=hmd&ypr={yaw},{pitch},0")
        for cant in ("0.6 0.4", "0.6 -0.4", "-0.6 0.4", "0 0"):
            cmd(g, f"stereocant {cant}")
            caps = {}
            for tag, cmds in (("old", ["stereofar 1", "stereozclamp 0"]), ("far", ["stereofar 2"]), ("fix", ["stereozclamp 1"]),
                              ("fix2", [])):
                for c in cmds:
                    cmd(g, c)
                time.sleep(1.5)
                caps[tag] = capture(g, out, f"f2_y{yaw}p{pitch}_c{cant.replace(' ', '_')}_{tag}")
            k = f"y{yaw}p{pitch}_c{cant.replace(' ', '_')}"
            F[k] = {"old_vs_fix": far_delta(caps["old"], caps["fix"], pad, os.path.join(out, f"f2d_{k}_old.png")),
                    "far_vs_fix": far_delta(caps["far"], caps["fix"], pad, os.path.join(out, f"f2d_{k}_far.png")),
                    "fix_vs_fix2(noise)": far_delta(caps["fix"], caps["fix2"], pad)}
            say(f"far2 {k}: {F[k]}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "stereocant 0 0")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def eye_vs_mono_png(stereo_path, mono_path, mono_tan, eye_tans, out_png, scale=8):
    """thumbnail: [eye L | eye R] over [mono remapped into L | R] over [ratio map L | R] (stereo/mono luminance,
    red = stereo brighter = shadow missing, blue = stereo darker); 32-px blocks"""
    import numpy as np
    from np_png import write_png
    S = H.read_bmp(stereo_path).astype(np.float32)
    M = H.read_bmp(mono_path).astype(np.float32)
    h, w, _ = S.shape
    hm, wm, _ = M.shape
    wp = w // 2
    l, r, t, b = mono_tan
    rows = [[], [], []]
    for e in range(2):
        le, re_, te, be = eye_tans[e]
        reg = S[:, e * wp:(e + 1) * wp]
        tx = le + (np.arange(wp) + 0.5) / wp * (re_ - le)
        ty = te - (np.arange(h) + 0.5) / h * (te - be)
        mx = np.clip(((tx - l) / (r - l) * wm).astype(int), 0, wm - 1)
        my = np.clip(((t - ty) / (t - b) * hm).astype(int), 0, hm - 1)
        mono = M[my][:, mx]
        blk = 16
        nb_y, nb_x = h // blk, wp // blk
        a = reg.mean(axis=2)[:nb_y * blk, :nb_x * blk].reshape(nb_y, blk, nb_x, blk).mean(axis=(1, 3))
        m = mono.mean(axis=2)[:nb_y * blk, :nb_x * blk].reshape(nb_y, blk, nb_x, blk).mean(axis=(1, 3))
        ratio = (a + 4) / (m + 4)
        img = np.zeros((nb_y, nb_x, 3), np.float32)
        img[:, :, 2] = np.clip((ratio - 1.0) * 400, 0, 255)  # BGR: red = brighter
        img[:, :, 0] = np.clip((1.0 - ratio) * 400, 0, 255)
        img = np.repeat(np.repeat(img, blk // scale, axis=0), blk // scale, axis=1)
        rows[0].append(reg[::scale, ::scale])
        rows[1].append(mono[::scale, ::scale])
        rows[2].append(img)
    hh = min(x.shape[0] for r_ in rows for x in r_)
    ww = min(x.shape[1] for r_ in rows for x in r_)
    grid = np.concatenate([np.concatenate([x[:hh, :ww] for x in r_], axis=1) for r_ in rows], axis=0)
    write_png(out_png, grid[:, :, ::-1].clip(0, 255).astype(np.uint8))


def step_metashadow(g, out, res, pad):
    """shadow edge with the runtime's own eye geometry (Meta XR Simulator Quest 3 profile when --runtime=meta):
    stereo (defaults) / stereo old depth handling / mono at 4 headings (rs turn); eye-vs-mono luminance maps"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    R = res.setdefault("metashadow", {})
    for i in range(4):
        if i:
            cmd(g, "rs turn 90")
        time.sleep(3)
        st = cmd(g, "stereoscissor")
        mono_tan, eye_tans = parse_tans(st)
        caps = {}
        caps["fix"] = capture(g, out, f"ms_h{i}_fix")
        cmd(g, "stereofar 1")
        cmd(g, "stereozclamp 0")
        time.sleep(1.5)
        caps["old"] = capture(g, out, f"ms_h{i}_old")
        cmd(g, "stereofar 2")
        cmd(g, "stereozclamp 1")
        cmd(g, "stereo 0")
        time.sleep(3)
        caps["mono"] = capture(g, out, f"ms_h{i}_mono")
        cmd(g, "stereo 3")
        time.sleep(1)
        r = {"status": st, "fix_vs_mono": shadow_map(caps["fix"], caps["mono"], mono_tan, eye_tans),
             "old_vs_fix": far_delta(caps["old"], caps["fix"], pad, os.path.join(out, f"msd_h{i}_oldfix.png"))}
        eye_vs_mono_png(caps["fix"], caps["mono"], mono_tan, eye_tans, os.path.join(out, f"msm_h{i}.png"))
        R[f"h{i}"] = r
        say(f"metashadow h{i}: {st[-200:]} | old_vs_fix {r['old_vs_fix']}")
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)


def read_crop(path):
    import numpy as np
    d = np.fromfile(path, dtype=np.uint8)
    magic, w, h, n = np.frombuffer(d[:16].tobytes(), dtype='<u4')
    fr = (len(d) - 16) // (w * h * 3)
    return d[16:16 + fr * w * h * 3].reshape(fr, h, w, 3).astype(np.int16)


def flicker_stats(frames, thr=40, share=0.002):
    """transient single-frame changes: pixel p flickers at frame i if it differs from both neighbours by > thr while
    the neighbours agree (< thr/2). A frame is a flicker frame when > share of the crop flickers. Per half (eye)."""
    import numpy as np
    L = frames.max(axis=3)
    n, h, w = L.shape
    out = {}
    for e, sl in ((0, slice(0, w // 2)), (1, slice(w // 2, w))):
        x = L[:, :, sl]
        a, b, c = x[:-2], x[1:-1], x[2:]
        fl = (np.abs(b - a) > thr) & (np.abs(b - c) > thr) & (np.abs(c - a) < thr // 2)
        per = fl.reshape(fl.shape[0], -1).mean(axis=1)
        out[f"e{e}"] = {"frames": int(n), "flickerFrames": int((per > share).sum()),
                        "rate": round(float((per > share).mean() * 100), 2), "maxShare": round(float(per.max() * 100), 3),
                        "meanShare": round(float(per.mean() * 100), 4)}
    return out, per


def step_skyts(g, out, res, pad):
    """sky flicker time series: real eyes, head pitched down with small motion, 400-frame crops of the top 720 rows of
    both eyes (step 4), conditions old depth handling / fix / fix + old FOV loop / old + old FOV loop"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    S = res.setdefault("skyts", {})
    W = 5504 // 2
    for pitch in (-25, -35, 10):
        for name, cmds in (("old", ["stereofar 1", "stereozclamp 0", "fovloop new"]), ("fix", ["stereofar 2", "stereozclamp 1"]),
                           ("fix_oldfov", ["fovloop old"]), ("old_oldfov", ["stereofar 1", "stereozclamp 0"])):
            for c in cmds:
                cmd(g, c)
            with Wobble(amp=0.6, hz=0.7, yaw0=0, pitch0=pitch):
                time.sleep(1.5)
                path = os.path.join(out, f"sky_p{pitch}_{name}.raw").replace("\\", "/")
                mark = Game.log_mark()
                g.cmd(f"cropburst 400 {path} 32 {W + 32} 0 2688 720 4", quiet=True)
                t0 = time.time()
                while "cropburst:" not in g.log_since(mark):
                    if time.time() - t0 > 60:
                        raise RuntimeError("cropburst timed out")
                    time.sleep(0.25)
            fr = read_crop(path)
            st, per = flicker_stats(fr)
            fov = len(re.findall(r"game rendered FOV", g.log_since(mark)))
            S[f"p{pitch}_{name}"] = {"stats": st, "fovLines": fov}
            say(f"skyts p{pitch} {name}: {st} | FOV lines {fov}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
            if per.max() > 0.002:
                import numpy as np
                i = int(per.argmax()) + 1
                thumb(os.path.join(out, f"skyts_p{pitch}_{name}_worst.png"), [fr[i - 1], fr[i], fr[i + 1]], scale=1)
    cmd(g, "stereofar 2")
    cmd(g, "stereozclamp 1")
    cmd(g, "fovloop new")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def write_bmp32(path, img):
    """BGR int array -> 32-bpp top-down BMP (readable by halves_np.read_bmp)"""
    import numpy as np
    h, w, _ = img.shape
    px = np.zeros((h, w, 4), np.uint8)
    px[:, :, :3] = img.clip(0, 255).astype(np.uint8)
    px[:, :, 3] = 255
    import struct as st
    hdr = st.pack('<2sIHHI', b'BM', 54 + px.nbytes, 0, 0, 54) + st.pack('<IiiHHIIiiII', 40, w, -h, 1, 32, 0, px.nbytes, 0, 0, 0, 0)
    with open(path, 'wb') as fh:
        fh.write(hdr)
        fh.write(px.tobytes())


def capture_med(g, out, name, frames=3, step=4):
    """a few consecutive frames of both eye regions at 1/step resolution (cropburst), per-pixel median -> one BMP
    (rules out single-frame flukes at a fraction of the size of full captures)"""
    import numpy as np
    raw = os.path.join(out, name + ".raw").replace("\\", "/")
    mark = Game.log_mark()
    g.cmd(f"cropburst {frames} {raw} 0 2752 0 2752 2880 {step}", quiet=True)
    t0 = time.time()
    while "cropburst:" not in g.log_since(mark):
        if time.time() - t0 > 60:
            raise RuntimeError("cropburst timed out")
        time.sleep(0.2)
    fr = read_crop(raw)
    med = np.median(fr, axis=0)
    bmp = os.path.join(out, name + ".bmp")
    write_bmp32(bmp, med)
    os.replace(raw, raw + ".done")  # keep the frames (small) next to the median
    return bmp, fr


def cam_pos(g):
    r = g.cmd("cam", quiet=True)
    m = re.search(r"worldPos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", r)
    return tuple(float(x) for x in m.groups()) if m else None


def calm(g):
    """controlled scene for repeatable captures: god mode, combat AI and AI off, hostiles removed, noon (call ONCE per
    session: tgm/tcai/tai toggle)"""
    for c in ("console tgm", "console tcai", "console tai", "console killall", "console set gamehour to 12"):
        cmd(g, c)
        time.sleep(0.4)


def goto_pose(g, x, y, zcam, heading, tol=25.0):
    """exterior pose by camera position: cow into the cell (cells load), then setpos (feet = camera - 120) + setangle,
    wait for the load, check the camera world position; retries; raises if it never arrives (no captures from a
    wrong pose)"""
    cx, cy = int(x // 4096), int(y // 4096)
    cmd(g, f"console cow Commonwealth {cx} {cy}")
    time.sleep(3)
    if wait_ingame(g, 120) is None:
        raise RuntimeError("cow did not finish loading")
    time.sleep(5)
    calm(g)
    last = None
    for attempt in range(4):
        # drop onto loaded ground: z 300 above first (cells load around the player), then 100 above the target feet
        drop = 300.0 if attempt == 0 else 100.0
        feet = zcam - 120.0 + drop
        for c in (f"console player.setpos z {feet:.1f}", f"console player.setpos x {x:.1f}", f"console player.setpos y {y:.1f}",
                  f"console player.setpos z {feet:.1f}", f"console player.setangle z {heading}"):
            cmd(g, c)
            time.sleep(0.5)
        time.sleep(6)
        wait_ingame(g, 60)
        time.sleep(2)
        last = cam_pos(g)
        say(f"pose attempt {attempt}: camera {last} target ({x}, {y}, {zcam})")
        if attempt and last and abs(last[0] - x) < tol and abs(last[1] - y) < tol and abs(last[2] - zcam) < 150.0:  # z: head height differs per runtime (ground-limited)
            cmd(g, "console killall")  # encounters can spawn on the cell load (tgm/tcai/tai are toggles: set once)
            time.sleep(3)
            cmd(g, "console save vrtest_container")
            time.sleep(4)
            return {"camera": last, "attempts": attempt + 1, "saved": "vrtest_container"}
    raise RuntimeError(f"pose not reached: camera {last}")


def step_aoyaw(g, out, res, pad):
    """user repro (yellow sea container, work/stereo-user/user-positions.md): SAO diagonal triangle and shadow popping.
    Poses: saved, +15, -15, +30, -30 deg yaw (rs turn). At each: stereo and mono, SAO on and off, 3 frames each
    (median, half resolution) -> eye-vs-mono luminance maps + per-eye column stats. Plus g125 RawAO pass capture and
    SAO on/off under a small eye cant at the saved pose."""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    A = res.setdefault("aoyaw", {})
    A["pose"] = goto_pose(g, -49966.4, 71077.8, 5511.9, 105)
    cmd(g, "stereocut sao 0")
    cmd(g, "stereo 3")
    time.sleep(2)
    A["passes0"] = passcap(g, "aopass", bmpdir=out, groups="125")
    for cant in ("0 0", "0.6 0.4", "-0.6 -0.4"):
        cmd(g, f"stereocant {cant}")
        caps = {}
        for sao in (0, 1):
            cmd(g, f"stereocut sao {sao}")
            time.sleep(2)
            caps[sao], _ = capture_med(g, out, f"aycant_{cant.replace(' ', '_')}_sao{1 - sao}")
        A[f"cant_{cant.replace(' ', '_')}_saoOn_vs_off"] = far_delta(caps[0], caps[1], 0,
                                                                     os.path.join(out, f"aycant_{cant.replace(' ', '_')}.png"))
    cmd(g, "stereocant 0 0")
    cmd(g, "stereocut sao 0")
    # SAO edge-bias A/B at the saved pose: eye-local NDC (old) vs mono NDC of the direction (fix), stereo vs mono
    st0 = cmd(g, "stereoscissor")
    mt0, et0 = parse_tans(st0)
    cmd(g, "stereo 0")
    time.sleep(2.5)
    mono0, _ = capture_med(g, out, "bias_mono")
    cmd(g, "stereo 3")
    for bias in (0, 1):
        cmd(g, f"stereosaobias {bias}")
        time.sleep(2)
        sp, _ = capture_med(g, out, f"bias{bias}_st")
        A[f"bias{bias}_vs_mono"] = shadow_map(sp, mono0, mt0, et0, blk=16)
        eye_vs_mono_png(sp, mono0, mt0, et0, os.path.join(out, f"bias{bias}_map.png"), scale=1)
    A["tans"] = st0
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    turns = [(0, 0), (15, 15), (-15, -30), (30, 45), (-30, -60)]
    for yaw, turn in turns:
        if turn:
            cmd(g, f"rs turn {turn}")
        time.sleep(2.5)
        st = cmd(g, "stereoscissor")
        mono_tan, eye_tans = parse_tans(st)
        r = {"status": st}
        for sao in (0, 1):
            cmd(g, f"stereocut sao {sao}")
            cmd(g, "stereo 3")
            time.sleep(2)
            sp, sfr = capture_med(g, out, f"ay{yaw:+d}_sao{1 - sao}_st")
            cmd(g, "stereo 0")
            if sao:
                cmd(g, "stereocuts force")  # mono: the forced cut list switches SAO off as in stereo
            time.sleep(2.5)
            mp, mfr = capture_med(g, out, f"ay{yaw:+d}_sao{1 - sao}_mono")
            if sao:
                cmd(g, "stereocuts auto")
            tag = f"sao{1 - sao}"
            import numpy as np
            r[tag] = shadow_map(sp, mp, mono_tan, eye_tans, blk=16)
            # frame-to-frame stability of the 3 frames (a pop inside the burst would show here)
            r[tag]["burstMaxDiff"] = {"stereo": int(np.abs(np.diff(sfr, axis=0)).max()), "mono": int(np.abs(np.diff(mfr, axis=0)).max())}
            eye_vs_mono_png(sp, mp, mono_tan, eye_tans, os.path.join(out, f"aym_{yaw:+d}_{tag}.png"), scale=1)
        cmd(g, "stereo 3")
        cmd(g, "stereocut sao 0")
        A[f"y{yaw:+d}"] = r
        say(f"aoyaw {yaw:+d}: sao on brighter e0 {r['sao1']['e0']['brighter']} | sao off e0 {r['sao0']['e0']['brighter']}")
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    cmd(g, "rs turn 30")


def pop_stats(fr, blk=8, thr=25):
    """per frame: number of blk x blk blocks (crop resolution) whose mean luminance jumps > thr from the previous frame;
    pop frames = frames whose count exceeds median + 4 * MAD (+ 3 blocks)"""
    import numpy as np
    L = fr.astype(np.float32).mean(axis=3)
    n, h, w = L.shape
    B = L[:, :h // blk * blk, :w // blk * blk].reshape(n, h // blk, blk, w // blk, blk).mean(axis=(2, 4))
    jumps = (np.abs(np.diff(B, axis=0)) > thr).reshape(n - 1, -1).sum(axis=1)
    med = float(np.median(jumps))
    mad = float(np.median(np.abs(jumps - med))) + 1e-6
    pops = jumps > med + 4 * mad + 3
    return {"frames": int(n), "medianJumpBlocks": med, "popFrames": int(pops.sum()), "maxJumpBlocks": int(jumps.max())}, jumps


def step_popts(g, out, res, pad):
    """shadow pop while turning (user, container): container pose, SAO off, HMD yaw rotation 15 deg/s (vremu) while
    300 frames of the lower 60 % of both eye regions are captured (1/4 res); stereo vs mono, both directions"""
    cmd(g, "stereotest 0")
    cmd(g, "stereoclear 0")
    P = res.setdefault("popts", {})
    if "aoyaw" not in res:
        P["pose"] = goto_pose(g, -49966.4, 71077.8, 5511.9, 105)
    for mode in (3, 0):
        cmd(g, f"stereo {mode}")
        cmd(g, "stereocut sao 1")
        if mode == 0:
            cmd(g, "stereocuts force")
        for direction in (1, -1):
            vremu("/pose?dev=hmd&ypr=0,0,0")
            time.sleep(2.5)
            vremu(f"/rate?dev=hmd&wyaw={15 * direction}&ms=6000&timeout=6000")
            time.sleep(0.5)
            raw = os.path.join(out, f"pop_m{mode}_d{direction:+d}.raw").replace("\\", "/")
            mark = Game.log_mark()
            # mono: the whole frame as two halves; stereo: both eye regions (the same call: x0 0, x1 2752)
            g.cmd(f"cropburst 300 {raw} 0 2752 1152 2752 1728 4", quiet=True)
            t0 = time.time()
            while "cropburst:" not in g.log_since(mark):
                if time.time() - t0 > 60:
                    raise RuntimeError("cropburst timed out")
                time.sleep(0.25)
            vremu("/rate?dev=hmd&stop=1")
            st, _ = pop_stats(read_crop(raw))
            P[f"m{mode}_d{direction:+d}"] = st
            say(f"popts mode {mode} dir {direction:+d}: {st}")
            json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
        if mode == 0:
            cmd(g, "stereocuts auto")
    cmd(g, "stereo 3")
    vremu("/pose?dev=hmd&ypr=0,0,0")


def _extra_steps():
    import r3_s5  # session-5 steps (ipd0diag, popsweep, shadowturn)
    return r3_s5.STEPS


def main():
    tag = sys.argv[1]
    opt = dict(a[2:].split("=", 1) for a in sys.argv[2:] if a.startswith("--") and "=" in a)
    keys = {}
    for kv in filter(None, opt.get("keys", "").split(",")):
        k, v = kv.split("=", 1)
        sec, key = k.split(".", 1)
        keys[(sec, key)] = v
    steps = opt.get("steps", "base").split(",")
    out = os.path.join(opt.get("out", r"G:\Fallout4ToVR\work\stereo3"), time.strftime(f"{tag}-%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    res = {"keys": {f"{s}.{k}": v for (s, k), v in keys.items()}}
    if opt.get("runtime") == "meta":
        RUNTIME[0] = META_SIM_JSON
    prefs = {}
    for kv in filter(None, opt.get("prefs", "").split(",")):
        k, v = kv.split("=", 1)
        sec, key = k.split(".", 1)
        prefs[(sec, key)] = v
    res["prefs"] = {f"{s_}.{k_}": v for (s_, k_), v in prefs.items()}
    save = opt.get("save", SAVE)
    g = launch(keys, prefs)
    try:
        say("vremu reset: " + vremu("/reset"))
        cmd(g, f"console load {save}")
        time.sleep(2)
        if wait_ingame(g) is None:
            raise RuntimeError("load failed")
        time.sleep(12)
        cmd(g, "console tm")
        time.sleep(2)
        log = g.log_since("launch")
        m = re.search(r"seam guard (\d+) px \(render width ([\d.]+)\)", log)
        pad = int(m.group(1)) if m else 0
        res["pad"] = pad
        res["renderWidth"] = float(m.group(2)) if m else None
        say(f"seam guard {pad} px, render width {res['renderWidth']}")
        res["cuts"] = cmd(g, "stereocuts status")
        for s in steps:
            {"base": step_base, "cuts": step_cuts, "saots": step_saots, "fwd": step_fwd, "sky": step_sky, "saoghost": step_saoghost, "flare": step_flare, "perf": step_perf, "allon": step_allon, "pause": step_pause, "tiled": step_tiled, "tiledperf": step_tiledperf, "shadow": step_shadow, "far": step_far, "tiledcrash": step_tiledcrash, "far2": step_far2, "metashadow": step_metashadow, "skyts": step_skyts, "aoyaw": step_aoyaw, "popts": step_popts, **_extra_steps()}[s](g, out, res, pad)
        json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    finally:
        try:
            open(os.path.join(out, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        except Exception:  # noqa: BLE001
            pass
        vremu("/stopall")
        g.quit()
    say(f"output: {out}")


if __name__ == "__main__":
    main()
