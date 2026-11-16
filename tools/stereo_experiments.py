"""Single-pass stereo PoC experiments E1-E7 (analysis/stereo-poc-plan.md section 10), one launch per run.

    python -I tools/stereo_experiments.py flat     # E1-E6, flat game (no XR), one launch
    python -I tools/stereo_experiments.py vremu    # E7 headset path under SteamVR + vremu, one launch
    python -I tools/stereo_experiments.py geom     # geometry round: QASmoke + SanctuaryExt in one launch: per-family
                                                   # draw counts, coverage, E2/E3/E5 on all GBuffer targets, E6

The deployed ini gets [Stereo] iMode=1 just for the launch (restored as soon as the plugin pipe is up),
so the hooks are installed and the pipe switches modes. Results: work/stereo/<ts>/ (BMPs, sidecars,
log excerpt, results.json) and a PASS/FAIL table on stdout. Images are judged only by
proto/stereo/tools/stereo_metrics.py and the numeric checks here.
"""
import json
import math
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "proto", "stereo", "tools"))
from fo4test import Game, INI, LOG, game_pid, say  # noqa: E402
import stereo_metrics as sm  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
METRICS = os.path.join(HERE, "..", "proto", "stereo", "tools", "stereo_metrics.py")
STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"

STAT_RE = re.compile(
    r"stereo: mode (\d) per frame: DrawIndexed ([\d.]+) Draw ([\d.]+) DIInstanced ([\d.]+) DInstanced ([\d.]+) \| "
    r"world-camera draws ([\d.]+) \(unknown VS ([\d.]+)\) candidates ([\d.]+) \(instanced ([\d.]+)\) converted ([\d.]+) "
    r"identity ([\d.]+) skipped ([\d.]+) \| VS swaps ([\d.]+) PS swaps ([\d.]+) b5 binds ([\d.]+) \| VS created (\d+) "
    r"matched (\d+) convertible (\d+) \| cpu avg ([\d.]+) ms max ([\d.]+) frame avg ([\d.]+) ms \| jitter (\S+)"
    r" \| prepass ([\d.]+) DSS swaps ([\d.]+) ownDepth (\d)")
STAT_KEYS = ["mode", "drawIndexed", "draw", "dii", "di", "gateDraws", "unknownVS", "candidates", "instancedCand",
             "converted", "identity", "skipped", "vsSwaps", "psSwaps", "b5", "vsCreated", "matched", "convertible",
             "cpuAvg", "cpuMax", "frameAvg", "jitter", "prepass", "dssSwaps", "ownDepth"]
PACING_RE = re.compile(r"pacing: frame avg ([\d.]+) ms max ([\d.]+) ms.*wait avg ([\d.]+)")


def parse_stats(text):
    out = []
    for m in STAT_RE.finditer(text):
        d = {}
        for k, v in zip(STAT_KEYS, m.groups()):
            d[k] = float(v)
        d["mode"] = int(d["mode"])
        d["total"] = d["drawIndexed"] + d["draw"] + d["dii"] + d["di"]
        out.append(d)
    return out


def set_ini_mode(text, mode):
    new, n = re.subn(r"(?ms)(^\[Stereo\][ \t\r]*$.*?^iMode=)\d+", lambda m: m.group(1) + str(mode), text, count=1)
    if n != 1:
        raise SystemExit("deployed ini has no [Stereo] iMode key")
    return new


def bmp_done(path):
    try:
        with open(path, "rb") as fh:
            hdr = fh.read(6)
        import struct
        return len(hdr) == 6 and hdr[:2] == b"BM" and os.path.getsize(path) == struct.unpack_from("<I", hdr, 2)[0]
    except OSError:
        return False


def run_metric(*args):
    r = subprocess.run([sys.executable, "-I", METRICS, *args], capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


class Runner:
    def __init__(self, g, outdir):
        self.g, self.outdir = g, outdir
        self.res = {}

    def cmd(self, c):
        r = self.g.cmd(c, quiet=True)
        say(f"> {c} -> {r[:160]}")
        return r

    def wait(self, what, timeout, pred, poll=0.25):
        t0 = time.time()
        while not pred():
            if time.time() - t0 > timeout:
                raise RuntimeError(f"{what}: timed out after {timeout}s")
            time.sleep(poll)
        return time.time() - t0

    def stats_since(self, mark):
        return parse_stats(self.g.log_since(mark))

    def collect(self, mode, n=1, timeout=120):
        """Switch to `mode`, discard the first (mixed) stats window, return the next n windows."""
        self.cmd(f"stereo {mode}")
        mark = Game.log_mark()
        self.wait(f"{n + 1} stats lines in mode {mode}", timeout,
                  lambda: len([s for s in self.stats_since(mark) if s["mode"] == mode]) >= n + 1, poll=1.0)
        return [s for s in self.stats_since(mark) if s["mode"] == mode][1:n + 1]

    def stereocap(self, name, timeout=20):
        path = os.path.join(self.outdir, name + ".bmp").replace("\\", "/")
        self.cmd("stereocap " + path)
        self.wait(f"stereocap {name}", timeout, lambda: bmp_done(path) and os.path.exists(path + ".json"), poll=0.1)
        time.sleep(0.2)
        return path

    def capture(self, name, timeout=20):
        path = os.path.join(self.outdir, name + ".bmp").replace("\\", "/")
        self.cmd("capture " + path)
        self.wait(f"capture {name}", timeout, lambda: bmp_done(path), poll=0.1)
        return path

    def metric(self, key, *args):
        rc, out = run_metric(*args)
        say(f"{key}: {out.splitlines()[-1] if out else rc}")
        self.res[key] = {"rc": rc, "out": out}
        return rc == 0, out


def num(out, pat, i=1):
    m = re.search(pat, out)
    return float(m.group(i)) if m else None


def crop_compare(a_path, b_path, half=None):
    """PSNR of a (optionally its left/right half) vs b, same size required. Returns (psnr, fracOff4)."""
    wa, ha, ra = sm.read_bmp(a_path)
    wb, hb, rb = sm.read_bmp(b_path)
    x0 = 0 if half is None else (0 if half == 0 else wa // 2)
    w = wa if half is None else wa // 2
    if (w, ha) != (wb, hb):
        return None, f"size {w}x{ha} vs {wb}x{hb}"
    se = n = big = 0
    for y in range(0, ha, 2):
        r1, r2 = ra[y], rb[y]
        for x in range(0, w, 2):
            o1, o2 = (x0 + x) * 4, x * 4
            for c in range(3):
                d = r1[o1 + c] - r2[o2 + c]
                se += d * d
                big += abs(d) > 4
                n += 1
    mse = se / n
    return (99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)), big / n


def covered_halves(path):
    """Measurement captures (RT0 cleared at the first converted draw): coverage = share of pixels drawn in
    either half this frame; identity among covered pixels (max-channel diff <= 1)."""
    w, h, px = sm.read_bmp(path)
    half = w // 2
    cov = same = n = 0
    for y in range(0, h, 2):
        r = px[y]
        for x in range(0, half, 2):
            a, b = r[x * 4:x * 4 + 3], r[(x + half) * 4:(x + half) * 4 + 3]
            n += 1
            if any(a) or any(b):
                cov += 1
                same += max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) <= 1
    return {"coverage": cov / n, "identicalCovered": (same / cov) if cov else 0.0}


def write_ini_inplace(text):
    """Rewrite the deployed ini in place (r+, overwrite, truncate the tail): the file is never empty in between
    (an earlier truncate-then-write bug emptied it once)."""
    data = text.encode("utf-8")
    with open(INI, "r+b") as fh:
        fh.seek(0)
        fh.write(data)
        fh.truncate()


def wait_game_free(poll=20):
    """The game is shared: wait until no Fallout4 runs (never touch someone else's game)."""
    t0 = time.time()
    while game_pid():
        if time.time() - t0 > 3 * 3600:
            raise SystemExit("Fallout4 has been busy for 3 h - giving up")
        say(f"Fallout4 is running (someone else's) - waiting ({time.time() - t0:.0f} s)")
        time.sleep(poll)


def launch(mode, xr, keys=None):
    """Launch with [Stereo] iMode=<mode> (+ optional other [Stereo] keys) for the plugin's load only. Each key is
    changed per line (temp file + os.replace via fo4test) and only the lines changed here are restored, as soon as
    the pipe is up - other agents' edits to other lines are never overwritten."""
    from fo4test import _set_ini_key, _get_ini_key
    wait_game_free()
    if not open(INI, "rb").read().strip():
        raise SystemExit("deployed ini is empty - not launching")
    want = dict(keys or {})
    want["iMode"] = str(mode)
    orig = {}
    for k, v in want.items():
        o = _get_ini_key(k)
        if o is None:
            raise SystemExit(f"deployed ini has no {k}= line - not launching")
        orig[k] = o
    try:
        for k, v in want.items():
            _set_ini_key(k, v)
        g = Game(runtime_json=STEAMVR_JSON) if xr else Game(no_xr=True)
        g.__enter__()
    finally:
        for k, o in orig.items():
            if _get_ini_key(k) != o:
                _set_ini_key(k, o)
    return g


def flat(outdir):
    g = launch(1, xr=False)
    R = Runner(g, outdir)
    res = R.res
    try:
        say(R.cmd("stereostat"))
        g.coc("QASmoke")
        time.sleep(4.0)
        R.cmd("console tm")  # hide HUD/menus (stretched mono content otherwise)
        R.cmd("track off")
        time.sleep(1.0)

        # ---- E1 census -------------------------------------------------------------------
        res["E1_census"] = R.collect(1, 2)
        res["E1_off"] = R.collect(0, 2)
        res["E1_census2"] = R.collect(1, 1)

        # ---- E2 identity (mono, jitter off) ------------------------------------------------
        R.cmd("stereojitter 0")
        R.collect(1, 1)  # settle + confirm jitter 0 in the stats window
        a = R.stereocap("e2_census_a")
        a2 = R.stereocap("e2_census_b")
        R.cmd("stereo 4")
        time.sleep(1.0)
        b = R.stereocap("e2_identity_a")
        b2 = R.stereocap("e2_identity_b")
        res["E2_identity_stats"] = R.collect(4, 1)
        R.metric("E2_baseline_compare", "compare", a, a2)
        R.metric("E2_identity_compare", "compare", a, b)
        R.metric("E2_identity_compare2", "compare", a2, b2)

        # ---- E3 halves (debug stereo, test eyes, IPD 0, skip unconverted) ------------------
        # union frustum for the test eyes: tanH = max(t, t/eyeAspect * 2W/H) (eye 1440x1620 in 2880x1620)
        t = math.tan(math.radians(45.0))
        tanh_union = max(t, t / (1440 / 1620) * (2880 / 1620))
        R.cmd(f"fovreq {2 * math.degrees(math.atan(tanh_union)):.3f}")
        R.cmd("stereotest 1")
        R.cmd("stereofov 45")
        R.cmd("stereoipd 0")
        R.cmd("stereoskip 1")
        res["E3_mode0_stats"] = R.collect(0, 1)
        # R1 as planned (engine z-prepass kept, GBuffer tests against mono depth), then the own-depth fix
        R.cmd("stereodepth 0")
        res["E3_nodepth_stats"] = R.collect(2, 1)
        c = R.stereocap("e3_debug_ipd0_prepassdepth")
        R.metric("E3_halves_prepassdepth", "halves", c)
        R.cmd("stereodepth 1")
        res["E3_stats"] = R.collect(2, 1)
        c = R.stereocap("e3_debug_ipd0")
        R.metric("E3_halves", "halves", c)
        res["E3_covered"] = covered_halves(c)
        say(f"E3 covered: {res['E3_covered']}")

        # ---- E4 parallax ------------------------------------------------------------------
        for ipd in (0.064, 0.5):
            R.cmd(f"stereoipd {ipd}")
            time.sleep(0.5)
            c = R.stereocap(f"e4_debug_ipd{ipd}")
            R.metric(f"E4_disparity_{ipd}", "disparity", c)

        # ---- E5 faithful ------------------------------------------------------------------
        R.cmd("stereo 3")
        R.cmd("stereoipd 0")
        R.cmd("stereoskip 1")
        time.sleep(1.0)
        c = R.stereocap("e5_faithful_ipd0_skip")
        R.metric("E5_halves_rt0_skip", "halves", c)
        res["E5_covered"] = covered_halves(c)
        say(f"E5 covered: {res['E5_covered']}")
        R.cmd("stereoskip 0")
        time.sleep(1.0)
        c = R.stereocap("e5_faithful_ipd0_noskip")
        R.metric("E5_halves_rt0_noskip", "halves", c)
        bb = R.capture("e5_faithful_ipd0_backbuffer")
        R.metric("E5_halves_backbuffer", "halves", bb)
        res["E5_stats"] = R.collect(3, 1)
        R.cmd("stereoipd 0.064")
        time.sleep(1.0)
        R.capture("e5_faithful_ipd0.064_backbuffer")
        R.stereocap("e5_faithful_ipd0.064_rt0")
        # reference: mode 0 back buffer at the same FOV (stretched mono)
        R.cmd("stereo 0")
        time.sleep(1.0)
        R.capture("e5_mode0_backbuffer")

        # ---- E6 cost A/B (mode 0 <-> 3, same size/FOV, test eyes IPD 0.064, skip off) -----
        R.cmd("stereojitter 1")
        ab = []
        for _ in range(4):
            ab += R.collect(0, 1)
            ab += R.collect(3, 1)
        res["E6_ab"] = ab
    finally:
        mark_text = g.log_since("launch")
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(mark_text)
        g.quit()
    json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    return res


def vremu(outdir):
    g = launch(1, xr=True)
    R = Runner(g, outdir)
    res = R.res
    try:
        say(R.cmd("status"))
        g.coc("QASmoke")
        time.sleep(4.0)
        R.cmd("console tm")
        mark = Game.log_mark()
        res["E7_mode0"] = R.collect(0, 2)
        res["E7_mode3"] = R.collect(3, 2)
        stem = os.path.join(outdir, "e7").replace("\\", "/")
        R.cmd("stereoeyecap " + stem)
        R.wait("eye capture", 20, lambda: bmp_done(stem + "_eye1.bmp"), poll=0.2)
        time.sleep(0.3)
        res["E7_mode0b"] = R.collect(0, 2)
        res["E7_status"] = R.cmd("status")
        text = g.log_since(mark)
        res["E7_pacing"] = [list(map(float, m.groups())) for m in PACING_RE.finditer(text)]
        res["E7_warnings"] = [l for l in text.splitlines() if "XR:" in l or "SubmitEye" in l or "stereoeyecap" in l
                              or "stereo: submitted" in l]
    finally:
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        g.quit()
    json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    return res


def diag(outdir):
    g = launch(1, xr=False)
    try:
        g.coc("QASmoke")
        mark = Game.log_mark()
        t0 = time.time()
        while "stereo ctx:" not in g.log_since(mark) and time.time() - t0 < 60:
            time.sleep(1)
        time.sleep(11)
        print(chr(10).join(l for l in g.log_since("launch").splitlines() if "stereo ctx" in l or "per frame" in l))
    finally:
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        g.quit()
    return {}


def stereo_setup(R, ipd=0.0, skip=1, mode=2):
    """E3-E5 setup: test eyes (45 deg half FOV, 1440x1620 per eye), union culling FOV, jitter off."""
    t = math.tan(math.radians(45.0))
    tanh_union = max(t, t / (1440 / 1620) * (2880 / 1620))
    R.cmd(f"fovreq {2 * math.degrees(math.atan(tanh_union)):.3f}")
    R.cmd("stereojitter 0")
    R.cmd("stereotest 1")
    R.cmd("stereofov 45")
    R.cmd(f"stereoipd {ipd}")
    R.cmd(f"stereoskip {skip}")
    R.cmd(f"stereo {mode}")


def dump(outdir):
    """Which draws write the GBuffer mono in stereo mode? One frame of stereodump + a capture per setup."""
    g = launch(1, xr=False)
    R = Runner(g, outdir)
    try:
        g.coc("QASmoke")
        time.sleep(4.0)
        R.cmd("console tm")
        for name, mode, skip in (("debug_skip", 2, 1), ("faithful_noskip", 3, 0)):
            stereo_setup(R, 0.0, skip, mode)
            time.sleep(2.0)
            mark = Game.log_mark()
            R.cmd("stereodump")
            R.wait("stereodump", 20, lambda: "stereodump:" in g.log_since(mark) and
                   len([l for l in g.log_since(mark).splitlines() if "stereodump: vs" in l]) > 3, poll=0.5)
            time.sleep(1.0)
            open(os.path.join(outdir, f"dump_{name}.log"), "w", encoding="utf-8").write(g.log_since(mark))
            R.stereocap(f"dump_{name}")
    finally:
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        g.quit()
    return {}


GB_RE = re.compile(r"stereogb: (\S+) rt(\d) (\d+)x(\d+) fmt (\d+) \| identical ([-\d.]+)% within 1 step ([-\d.]+)% \| nonzero ([\d.]+)%"
                   r"(?:.*within 1/255 ([-\d.]+)%)?")
COV_RE = re.compile(r"stereocov: (\S+) pixels: converted ([\d.]+)% unconverted-camera ([\d.]+)% no-stereo-camera ([\d.]+)% "
                    r"background ([\d.]+)% \| of GBuffer geometry pixels: converted ([\d.]+)%")
EXT_RE = re.compile(r"1st-person draws ([\d.]+) \| gb draws ([\d.]+) converted ([\d.]+) unconverted ([\d.]+) ungated ([\d.]+) \| "
                    r"prepass converted ([\d.]+) stereoPrepass (\d) \| instanced converted ([\d.]+) layout swaps ([\d.]+) "
                    r"not convertible ([\d.]+) \| twins (\d+) rejected (\d+) layouts per-instance (\d+) twinned (\d+)")
EXT_KEYS = ["gate1st", "gbDraws", "gbConverted", "gbUnconverted", "gbUngated", "prepassConverted", "stereoPrepass",
            "instConverted", "layoutSwaps", "notConvertible", "twins", "twinRejected", "layoutsPerInstance", "layoutTwins"]


def parse_stats_ext(text):
    """parse_stats + the fields appended in the geometry round (same lines)."""
    out = []
    for line in text.splitlines():
        base = parse_stats(line)
        if not base:
            continue
        d = base[0]
        m = EXT_RE.search(line)
        if m:
            for k, v in zip(EXT_KEYS, m.groups()):
                d[k] = float(v)
        out.append(d)
    return out


def collect_ext(R, mode, n=1, timeout=120):
    """Runner.collect with the extended fields."""
    R.cmd(f"stereo {mode}")
    mark = Game.log_mark()
    R.wait(f"{n + 1} stats lines in mode {mode}", timeout,
           lambda: len([s for s in parse_stats_ext(R.g.log_since(mark)) if s["mode"] == mode]) >= n + 1, poll=1.0)
    return [s for s in parse_stats_ext(R.g.log_since(mark)) if s["mode"] == mode][1:n + 1]


def gbcap(R, stem, coverage=False, timeout=30):
    """stereogbcap / stereocov: halves identity of every GBuffer target (+ coverage classes), parsed from the log."""
    g = R.g
    path = os.path.join(R.outdir, stem).replace("\\", "/")
    mark = Game.log_mark()
    R.cmd(("stereocov " if coverage else "stereogbcap ") + path)
    R.wait(f"stereogb {stem}", timeout, lambda: f"stereogb: {path} done" in g.log_since(mark), poll=0.25)
    text = g.log_since(mark)
    res = {"rt": {}}
    for m in GB_RE.finditer(text):
        if m.group(1) == path:
            res["rt"][int(m.group(2))] = {"w": int(m.group(3)), "h": int(m.group(4)), "fmt": int(m.group(5)),
                                          "identical": float(m.group(6)), "near": float(m.group(7)), "nonzero": float(m.group(8)),
                                          "near8": float(m.group(9)) if m.group(9) else None}
    m = COV_RE.search(text)
    if coverage and m:
        res["coverage"] = {"converted": float(m.group(2)), "unconvertedCamera": float(m.group(3)),
                           "noStereoCamera": float(m.group(4)), "background": float(m.group(5)),
                           "convertedOfGBuffer": float(m.group(6))}
    # pass (plan section 7): >= 99.9% of texels within 1 step (1 LSB / float16 rel. 1e-3) on every target
    ok = len(res["rt"]) > 0 and all(v["near"] >= 99.9 for v in res["rt"].values())
    say(f"{stem}: " + " ".join(f"rt{i} {v['identical']:.3f}/{v['near']:.4f}/{v['near8']}%" for i, v in sorted(res["rt"].items())) +
        (f" | coverage {res['coverage']}" if "coverage" in res else "") + (" PASS" if ok else " FAIL"))
    res["pass"] = ok
    return res


def geom_cell(R, cell, settle):
    g, res = R.g, {}
    tag = cell.lower()
    g.coc(cell)
    time.sleep(settle)
    R.cmd("console tm")
    R.cmd("track off")
    # a drawn weapon puts 1st-person geometry (arms + weapon) into the GBuffer
    R.cmd("console player.additem 0004822B 1")
    R.cmd("console player.equipitem 0004822B")
    time.sleep(3.0)
    res["E1_census"] = collect_ext(R, 1, 1)
    res["gb_census"] = gbcap(R, f"gb_{tag}_census")  # mono: per-target channel statistics (what each target holds)

    # ---- per-family draw counts (faithful stereo, test eyes IPD 0, skip off) ------------------------------
    stereo_setup(R, 0.0, 0, 3)
    time.sleep(2.0)
    mark = Game.log_mark()
    R.cmd("stereodump")
    R.wait("stereodump", 20, lambda: len([ln for ln in g.log_since(mark).splitlines() if "stereodump: vs" in ln]) > 3, poll=0.5)
    time.sleep(1.0)
    dumptext = g.log_since(mark)
    open(os.path.join(R.outdir, f"dump_{tag}.log"), "w", encoding="utf-8").write(dumptext)
    import stereo_families
    table, fam = stereo_families.summarize(dumptext.splitlines())
    open(os.path.join(R.outdir, f"families_{tag}.txt"), "w", encoding="utf-8").write(table + "\n")
    print(table)
    res["families"] = fam

    # ---- E5 faithful, skip off, IPD 0: all GBuffer targets ---------------------------------------------
    res["E5_noclear"] = gbcap(R, f"e5_{tag}_faithful_ipd0")
    R.cmd("stereoclear 1")
    time.sleep(0.5)
    res["E5_clear"] = gbcap(R, f"e5_{tag}_faithful_ipd0_clear")
    res["coverage"] = gbcap(R, f"cov_{tag}", coverage=True)
    # ---- E3 debug PS, skip off, IPD 0 ------------------------------------------------------------------
    R.cmd("stereo 2")
    time.sleep(1.0)
    res["E3_clear"] = gbcap(R, f"e3_{tag}_debug_ipd0_clear")
    R.cmd("stereoclear 0")
    time.sleep(0.5)
    res["E3_noclear"] = gbcap(R, f"e3_{tag}_debug_ipd0")
    # own depth (prepass skipped) instead of the stereo prepass, for comparison
    R.cmd("stereoprepass 0")
    R.cmd("stereoclear 1")
    time.sleep(1.0)
    res["E3_owndepth_clear"] = gbcap(R, f"e3_{tag}_debug_ipd0_owndepth_clear")
    R.cmd("stereo 3")
    time.sleep(1.0)
    res["E5_owndepth_clear"] = gbcap(R, f"e5_{tag}_faithful_ipd0_owndepth_clear")
    R.cmd("stereoprepass 1")
    R.cmd("stereoclear 0")
    res["E3E5_stats"] = collect_ext(R, 3, 1)
    # back buffer at IPD 0 (lighting/post still mono -> expected to differ)
    bb = R.capture(f"e5_{tag}_backbuffer_ipd0")
    R.metric(f"E5_{tag}_halves_backbuffer", "halves", bb)
    R.cmd("stereoipd 0.064")
    time.sleep(1.0)
    R.capture(f"e5_{tag}_backbuffer_ipd0.064")
    res["E5_ipd064"] = gbcap(R, f"e5_{tag}_faithful_ipd0.064")

    # ---- E2 identity: all twins, mono (jitter off) --------------------------------------------------------
    R.cmd("stereo 1")
    time.sleep(1.0)
    a = R.stereocap(f"e2_{tag}_census")
    a2 = R.stereocap(f"e2_{tag}_census_b")  # baseline: the same mode a moment later (animated scene content)
    R.cmd("stereo 4")
    time.sleep(1.0)
    b = R.stereocap(f"e2_{tag}_identity")
    R.metric(f"E2_{tag}_baseline_compare", "compare", a, a2)
    R.metric(f"E2_{tag}_identity_compare", "compare", a, b)
    R.metric(f"E2_{tag}_identity_compare_b", "compare", a2, b)
    res["E2_stats"] = collect_ext(R, 4, 1)

    # ---- E6 cost A/B: mode 0 <-> 3 (test eyes IPD 0.064, jitter on, skip off) -------------------------------
    R.cmd("stereojitter 1")
    ab = []
    for _ in range(3):
        ab += collect_ext(R, 0, 1)
        ab += collect_ext(R, 3, 1)
    res["E6_ab"] = ab
    m0 = [x for x in ab if x["mode"] == 0]
    m3 = [x for x in ab if x["mode"] == 3]
    if m0 and m3:
        def avg(xs, k):
            return sum(x.get(k, 0) for x in xs) / len(xs)
        res["E6"] = {k: (avg(m0, k), avg(m3, k)) for k in ("total", "drawIndexed", "draw", "dii", "di", "cpuAvg", "frameAvg",
                                                            "converted", "vsSwaps", "psSwaps", "b5", "dssSwaps", "layoutSwaps",
                                                            "gbConverted", "gbUnconverted", "gate1st", "prepassConverted")}
        say(f"E6 {cell}: " + json.dumps(res["E6"]))
    R.cmd("stereo 1")
    R.cmd("stereoipd 0")
    return res


def geom(outdir):
    """Geometry-coverage round (stereo-poc-plan.md section 13): QASmoke + an exterior cell in one launch."""
    g = launch(1, xr=False)
    R = Runner(g, outdir)
    res = R.res
    try:
        n = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Fallout4.exe", "/FO", "CSV", "/NH"], capture_output=True,
                           text=True).stdout.count("Fallout4.exe")
        if n != 1:
            raise RuntimeError(f"{n} Fallout4 processes after launch (launch collision with another agent) - aborting")
        say(R.cmd("stereostat"))
        for cell, settle in (("QASmoke", 5.0), ("SanctuaryExt", 15.0)):
            try:
                res[cell] = geom_cell(R, cell, settle)
            except Exception as ex:  # keep the other cell's results
                say(f"{cell}: {ex!r}")
                res[cell] = {"error": repr(ex)}
            json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    finally:
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        g.quit()
    json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    return res


LIGHT_RE = re.compile(
    r"stereo light: mode (\d) split ([\d.]+) \(light ([\d.]+) composite ([\d.]+) gbuffer ([\d.]+) twin ([\d.]+) fullscreen ([\d.]+) "
    r"sao ([\d.]+) saoFixed ([\d.]+) (?:saoTwin ([\d.]+) )?fallback ([\d.]+)\) \| forward instanced ([\d.]+) \| b12 builds ([\d.]+) refresh ([\d.]+) "
    r"notWritten (\d+) \| dispatch ([\d.]+) \(stereo camera ([\d.]+)\) tiled frames (\d+) \| gpu avg ([\d.]+) ms \((\d+) frames\) \| "
    r"stereo frames (\d) \| cuts (.*)")
LIGHT_KEYS = ["mode", "split", "splitLight", "splitComposite", "splitGb", "splitTwin", "splitFullscreen", "splitSao", "splitSaoFixed",
              "splitSaoTwin", "splitFallback", "forwardInst", "b12Builds", "b12Refresh", "b12NotWritten", "dispatch", "gateDispatch", "tiledFrames",
              "gpuAvg", "gpuFrames", "stereoFrames", "cuts"]
STAGE_RE = re.compile(r"stereostage: (\S+) (\w+) (\d+)x(\d+) fmt (\d+) draws (\d+) \| identical ([\d.]+)% within 1% ([\d.]+)% within 4% "
                      r"([\d.]+)% \| without (\d+)-px seam/edge band: identical ([\d.]+)% within 1% ([\d.]+)% within 4% ([\d.]+)% \| "
                      r"nonzero ([\d.]+)% \| mean L ([-\d.e]+) R ([-\d.e]+)")


def collect_light(R, mode, n=1, timeout=120):
    """Switch to `mode`; the next n complete windows of (stereo: ..., stereo light: ...) lines after a discarded one."""
    R.cmd(f"stereo {mode}")
    mark = Game.log_mark()

    def windows():
        text = R.g.log_since(mark)
        out, base = [], None
        for line in text.splitlines():
            b = parse_stats_ext(line)
            if b:
                base = b[0]
                continue
            m = LIGHT_RE.search(line)
            if m and base is not None:
                d = dict(base)
                for k, v in zip(LIGHT_KEYS, m.groups()):
                    d[k] = v if k == "cuts" else float(v or 0)
                if d["mode"] == mode:
                    out.append(d)
                base = None
        return out
    R.wait(f"{n + 1} light stats windows in mode {mode}", timeout, lambda: len(windows()) >= n + 1, poll=1.0)
    return windows()[1:n + 1]


def stagecap(R, stem, timeout=30):
    path = os.path.join(R.outdir, stem).replace("\\", "/")
    mark = Game.log_mark()
    R.cmd("stereostagecap " + path)
    R.wait(f"stereostage {stem}", timeout, lambda: f"stereostage: {path} done" in R.g.log_since(mark), poll=0.25)
    out = {}
    for m in STAGE_RE.finditer(R.g.log_since(mark)):
        if m.group(1) != path:
            continue
        g = m.groups()
        out[g[1]] = {"w": int(g[2]), "h": int(g[3]), "fmt": int(g[4]), "draws": int(g[5]), "identical": float(g[6]),
                     "within1": float(g[7]), "within4": float(g[8]), "band": int(g[9]), "bandIdentical": float(g[10]),
                     "bandWithin1": float(g[11]), "bandWithin4": float(g[12]), "nonzero": float(g[13]),
                     "meanL": float(g[14]), "meanR": float(g[15])}
    say(f"{stem}: " + " | ".join(f"{k} {v['identical']:.3f}/{v['within1']:.3f}/{v['within4']:.3f}% "
                                  f"(band-excl {v['bandWithin1']:.3f}/{v['bandWithin4']:.3f}%) draws {v['draws']}"
                                  for k, v in out.items()))
    return out


def bb_halves(R, name, band=64):
    bb = R.capture(name)
    rc, out = run_metric("halvesband", bb, str(band), "1")
    say(f"{name}: {out}")
    rc4, out4 = run_metric("halvesband", bb, str(band), "4")
    return {"path": bb, "out1": out, "out4": out4, "pass1": rc == 0, "pass4": rc4 == 0,
            "full1": num(out, r"full ([\d.]+)%"), "band1": num(out, r"band ([\d.]+)%"),
            "full4": num(out4, r"full ([\d.]+)%"), "band4": num(out4, r"band ([\d.]+)%")}


SETTLE = 6.0


def light_cell(R, cell, settle):
    g, res = R.g, {}
    tag = cell.lower()
    g.coc(cell)
    time.sleep(settle)
    if not getattr(R, "hud_hidden", False):  # "tm" toggles: hide menus/HUD once per launch
        R.cmd("console tm")
        R.hud_hidden = True
    R.cmd("track off")
    R.cmd("console player.additem 0004822B 1")
    R.cmd("console player.equipitem 0004822B")
    time.sleep(3.0)
    # ---- stereo, test eyes, IPD 0 (cuts are applied by the plugin from the first stereo frame on) ----------
    stereo_setup(R, 0.0, 0, 3)
    R.cmd("stereoclear 1")  # measurement: GBuffer + HDR target cleared each frame (no stale sky history)
    time.sleep(SETTLE)       # SAO blends a reprojected history (weight up to 0.99): let it converge
    res["stats_stereo"] = collect_light(R, 3, 1)
    say(R.cmd("stereocuts status"))
    mark = Game.log_mark()
    R.cmd("stereodump")
    R.wait("stereodump", 20, lambda: len([ln for ln in g.log_since(mark).splitlines() if "stereodump: vs" in ln]) > 3, poll=0.5)
    time.sleep(1.0)
    open(os.path.join(R.outdir, f"dump_{tag}.log"), "w", encoding="utf-8").write(g.log_since(mark))
    res["stage_stereo"] = stagecap(R, f"st_{tag}_stereo_ipd0")
    res["bb_stereo"] = bb_halves(R, f"bb_{tag}_stereo_ipd0")
    res["gb_stereo"] = gbcap(R, f"gb_{tag}_stereo_ipd0")
    mask = os.path.join(R.outdir, f"gb_{tag}_stereo_ipd0_rt0.bmp")
    R.metric(f"halves_geometry_{tag}", "halvesmasked", res["bb_stereo"]["path"], mask)
    # diagnostic: SAO off (its noise pattern depends on the absolute pixel position -> per-eye by construction)
    R.cmd("stereocut sao 1")
    time.sleep(SETTLE)
    res["stage_nosao_all"] = stagecap(R, f"st_{tag}_saooff_ipd0")
    res["bb_saooff"] = bb_halves(R, f"bb_{tag}_saooff_ipd0")
    R.metric(f"halves_geometry_saooff_{tag}", "halvesmasked", res["bb_saooff"]["path"], mask)
    R.cmd("stereocut sao 0")
    # discrimination: the same with the split draws off (lighting/composite mono over both halves) -> must fail
    R.cmd("stereosplit 0")
    time.sleep(SETTLE)
    res["stage_nosplit"] = stagecap(R, f"st_{tag}_nosplit_ipd0")
    res["bb_nosplit"] = bb_halves(R, f"bb_{tag}_nosplit_ipd0")
    R.metric(f"halves_geometry_nosplit_{tag}", "halvesmasked", res["bb_nosplit"]["path"], mask)
    R.cmd("stereosplit 1")
    R.cmd("stereosao 0")
    time.sleep(SETTLE)
    res["stage_nosao"] = stagecap(R, f"st_{tag}_nosaofix_ipd0")
    res["bb_nosao"] = bb_halves(R, f"bb_{tag}_nosaofix_ipd0")
    R.cmd("stereosao 1")
    # ---- mono reference with the same cuts, and identity (split machinery must reproduce mono) ---------------
    R.cmd("stereocuts force")
    R.cmd("stereo 0")
    time.sleep(SETTLE)
    res["stats_mono_cuts"] = collect_light(R, 0, 1)
    res["stage_mono"] = stagecap(R, f"st_{tag}_mono_cuts")
    res["bb_mono"] = bb_halves(R, f"bb_{tag}_mono_cuts")
    R.metric(f"halves_geometry_mono_{tag}", "halvesmasked", res["bb_mono"]["path"], mask)
    mono_bb = res["bb_mono"]["path"]
    # baseline: the same mono view captured again (animated content, SAO/temporal noise)
    stagecap(R, f"st_{tag}_mono_cuts_b")
    mono_bb_b = R.capture(f"bb_{tag}_mono_cuts_b")
    R.metric(f"mono_vs_mono_bb_{tag}", "compare", mono_bb, mono_bb_b)
    for st in ("light", "composite", "sao"):
        a = os.path.join(R.outdir, f"st_{tag}_mono_cuts_{st}.bmp")
        b = os.path.join(R.outdir, f"st_{tag}_mono_cuts_b_{st}.bmp")
        if os.path.exists(a) and os.path.exists(b):
            R.metric(f"mono_vs_mono_{st}_{tag}", "compare", a, b)
    R.cmd("stereo 4")
    time.sleep(SETTLE)
    res["stage_identity"] = stagecap(R, f"st_{tag}_identity_cuts")
    res["bb_identity"] = bb_halves(R, f"bb_{tag}_identity_cuts")
    R.metric(f"identity_vs_mono_bb_{tag}", "compare", mono_bb, res["bb_identity"]["path"])
    for st in ("light", "composite", "sao"):
        a = os.path.join(R.outdir, f"st_{tag}_mono_cuts_{st}.bmp")
        b = os.path.join(R.outdir, f"st_{tag}_identity_cuts_{st}.bmp")
        if os.path.exists(a) and os.path.exists(b):
            R.metric(f"identity_vs_mono_{st}_{tag}", "compare", a, b)
    R.cmd("stereocuts auto")
    R.cmd("stereoclear 0")
    # ---- disparity at a normal IPD on the lit image (depth from a debug-mode capture of the same view) --------
    R.cmd("stereoipd 0.064")
    R.cmd("stereo 3")
    time.sleep(2.0)
    lit = R.capture(f"bb_{tag}_stereo_ipd0.064")
    R.cmd("stereoprepass 0")  # own depth: debug mode then uses the PoC hash PS (R = log depth band)
    R.cmd("stereoskip 1")
    R.cmd("stereo 2")
    time.sleep(1.5)
    dep = R.stereocap(f"dbg_{tag}_ipd0.064")
    R.metric(f"litdisp_{tag}", "litdisp", lit, dep)
    if cell != "QASmoke":  # QASmoke has no sky
        R.metric(f"skydisp_{tag}", "skydisp", lit)
    R.cmd("stereoskip 0")
    R.cmd("stereoprepass 1")
    R.cmd("stereo 3")
    R.cmd("stereoipd 0")
    # ---- cost A/B: mono with the same cuts forced vs stereo (test eyes IPD 0.064, same render size/FOV) --------
    R.cmd("stereoipd 0.064")
    ab = []
    for i in range(5):
        R.cmd("stereocuts force")
        a0 = collect_light(R, 0, 1)
        R.cmd("stereocuts auto")
        a3 = collect_light(R, 3, 1)
        if i:  # the first pair is warm-up (streaming after the earlier mode switches)
            ab += a0 + a3
    res["cost_ab"] = ab
    m0 = [x for x in ab if x["mode"] == 0]
    m3 = [x for x in ab if x["mode"] == 3]
    if m0 and m3:
        def avg(xs, k):  # median of the windows (robust against streaming spikes)
            v = sorted(x.get(k, 0) for x in xs)
            return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])
        res["cost"] = {k: (avg(m0, k), avg(m3, k)) for k in ("total", "drawIndexed", "draw", "dii", "di", "cpuAvg", "frameAvg", "gpuAvg",
                                                            "converted", "split", "splitTwin", "splitLight", "splitComposite",
                                                            "forwardInst", "vsSwaps", "psSwaps", "layoutSwaps", "dispatch")}
        say(f"cost {cell}: " + json.dumps(res["cost"]))
    R.cmd("stereoipd 0")
    R.cmd("stereo 1")
    return res


def light(outdir):
    """Lighting/post round (stereo-poc-plan.md section 14): QASmoke + SanctuaryExt in one launch."""
    g = launch(1, xr=False)
    R = Runner(g, outdir)
    res = R.res
    try:
        n = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Fallout4.exe", "/FO", "CSV", "/NH"], capture_output=True,
                           text=True).stdout.count("Fallout4.exe")
        if n != 1:
            raise RuntimeError(f"{n} Fallout4 processes after launch (launch collision with another agent) - aborting")
        say(R.cmd("stereostat"))
        cells = [c for c in (sys.argv[2:] or ["QASmoke", "SanctuaryExt"])]
        for cell in cells:
            try:
                res[cell] = light_cell(R, cell, 15.0 if cell != "QASmoke" else 5.0)
            except Exception as ex:  # keep the other cell's results
                say(f"{cell}: {ex!r}")
                res[cell] = {"error": repr(ex)}
            json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    finally:
        open(os.path.join(outdir, "fo4flatvr.log"), "w", encoding="utf-8").write(g.log_since("launch"))
        g.quit()
    json.dump(res, open(os.path.join(outdir, "results.json"), "w"), indent=1)
    return res


def main():
    a = sys.argv[1:]
    if a and a[0] in ("diag", "dump", "geom", "light"):
        outdir = os.path.join(ROOT, "work", "stereo", time.strftime("%Y%m%d-%H%M%S") + "-" + a[0])
        os.makedirs(outdir, exist_ok=True)
        r = {"diag": diag, "dump": dump, "geom": geom, "light": light}[a[0]](outdir)
        if a[0] == "light":
            print(json.dumps({c: {k: v for k, v in (r.get(c) or {}).items() if k not in ("cost_ab",)}
                              for c in r if isinstance(r.get(c), dict)}, indent=1)[:40000])
        if a[0] == "geom":
            print(json.dumps({c: {k: v for k, v in (r.get(c) or {}).items() if k not in ("E6_ab", "families")}
                              for c in ("QASmoke", "SanctuaryExt")}, indent=1)[:30000])
        print("output:", outdir)
        return 0
    if not a or a[0] not in ("flat", "vremu"):
        print(__doc__)
        return 2
    outdir = os.path.join(ROOT, "work", "stereo", time.strftime("%Y%m%d-%H%M%S") + "-" + a[0])
    os.makedirs(outdir, exist_ok=True)
    say(f"output: {outdir}")
    res = flat(outdir) if a[0] == "flat" else vremu(outdir)
    print(json.dumps({k: (v if not isinstance(v, dict) else v.get("out", v)) for k, v in res.items()}, indent=1)[:20000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
