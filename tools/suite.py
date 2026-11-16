"""One game launch -> many checks -> one compact report.

    python -I suite.py [--no-xr] [--only smoke,headpose,fov,camstate,timing,exit]
                       [--runtime-json PATH] [--attach] [--cell QASmoke] [--keep-bmp] [--vremu]
    python -I suite.py --selftest     # offline: parsers + image metric on work/shots/headpose/*.bmp

--no-xr points the plugin at a nonexistent OpenXR runtime json (flat game; use it whenever the
headset may be off). --attach uses an already-running game instead of refusing (never quits it).

--vremu implies --runtime-json <SteamVR steamxr_win64.json> and enables the vr_head / vr_move /
vr_buttons checks, which drive the vremu SteamVR emulator (HTTP API on 127.0.0.1:8766). Without it, or
when vremu's /state is unreachable, those checks SKIP. /stopall + /reset are sent after the checks.

Prints `check | PASS/FAIL/SKIP | key numbers` and the total wall time; writes the same as JSON to
work/suite/<timestamp>/report.json, with captures (PNG; BMP with --keep-bmp) next to it. Every check
runs in a worker thread with a hard timeout; a progress line is printed every ~5 s.
"""
import argparse
import json
import math
import operator
import os
import re
import struct
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I does not add the script dir
import fo4test  # noqa: E402
from fo4test import Game, parse_kv, say  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SUITE_DIR = os.path.join(ROOT, "work", "suite")
SHOTS = os.path.join(ROOT, "work", "shots", "headpose")
PROGRESS_EVERY = 5.0
VREMU_URL = "http://127.0.0.1:8766"
VREMU_HTTP_TIMEOUT = 3.0
STEAMVR_RUNTIME_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"
CMD_STUCK = 10.0  # a pipe reply that takes longer = game hung; fail the check without waiting for its timeout


# ==== pipe reply parsing ======================================================================
def truthy(v):
    return str(v).lower() in ("1", "true", "yes", "on")


def parse_frustum(cam_reply):
    """'... frustum l=-1.1918 r=1.1918 t=0.6704 b=-0.6704 n=..' -> {'l':..,'r':..,'t':..,'b':..}"""
    m = re.search(r"frustum l=(\S+) r=(\S+) t=(\S+) b=(\S+)", cam_reply)
    if not m:
        return None
    try:
        return dict(zip("lrtb", map(float, m.groups())))
    except ValueError:
        return None


def hfov_deg(r, l=None):
    """Horizontal FOV from frustum tangents (symmetric if l is None)."""
    return math.degrees(math.atan(r) + math.atan(-l if l is not None else r))


# ==== camera rotation (cam reply) =============================================================
def parse_cam_rot(cam_reply):
    """First `worldRot=[r00 r01 r02 | r10 r11 r12 | r20 r21 r22]` (the camera root's) -> 9 floats or None."""
    m = re.search(r"worldRot=\[([^\]]*)\]", cam_reply)
    if not m:
        return None
    try:
        v = [float(x) for x in m.group(1).replace("|", " ").split()]
    except ValueError:
        return None
    return v if len(v) == 9 else None


def parse_cam_pos(cam_reply):
    """First `worldPos=(x,y,z)` -> (x, y, z) or None."""
    m = re.search(r"worldPos=\(([^)]*)\)", cam_reply)
    if not m:
        return None
    try:
        v = tuple(float(x) for x in m.group(1).split(","))
    except ValueError:
        return None
    return v if len(v) == 3 else None


def cam_forward(cam_reply):
    """Forward vector of the camera root: local +Y in world = NiMatrix3 **row 1** = (r10, r11, r12).
    Measured in game (aim work, 14 Nov 2026): camera-local axis i maps to world as row i - the engine's
    own pick direction equals camera-root row 1 exactly, column 1 is its mirror image."""
    r = parse_cam_rot(cam_reply)
    return None if r is None else (r[3], r[4], r[5])


def cam_yaw_pitch(cam_reply):
    """-> (yaw_deg, fz): yaw = atan2(fy, fx) of the forward vector (identity = 90; turning right
    decreases it), fz = forward Z component (sin of pitch; looking up increases it). None if no worldRot."""
    f = cam_forward(cam_reply)
    if f is None:
        return None
    return math.degrees(math.atan2(f[1], f[0])), f[2]


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


# ==== vremu HTTP ==============================================================================
def vremu_get(path, **params):
    """GET http://127.0.0.1:8766/<path>?k=v (commas kept, so 'ypr=30,0,0' works). 3 s timeout.
    Returns the parsed JSON (or raw text if it is not JSON). Raises on any error."""
    q = "&".join(f"{k}={urllib.parse.quote(str(v), safe=',.-')}" for k, v in params.items())
    with urllib.request.urlopen(f"{VREMU_URL}{path}" + (f"?{q}" if q else ""), timeout=VREMU_HTTP_TIMEOUT) as r:
        body = r.read().decode("utf-8", "replace")
    try:
        return json.loads(body)
    except ValueError:
        return body


def vremu_up():
    try:
        vremu_get("/state")
        return True
    except Exception:
        return False


def vremu_cleanup():
    """/stopall then /reset; never raises. Returns a short status string."""
    out = []
    for p in ("/stopall", "/reset"):
        try:
            vremu_get(p)
            out.append(p + " ok")
        except Exception as e:
            out.append(f"{p} failed: {type(e).__name__}")
    return ", ".join(out)


# ==== images ==================================================================================
def read_bmp(path):
    """-> (w, h, bytes_per_pixel, rows top-to-bottom). 24/32-bit uncompressed BMP."""
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:2] != b"BM":
        raise ValueError(f"{os.path.basename(path)}: not a BMP")
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp not in (24, 32):
        raise ValueError(f"{os.path.basename(path)}: {bpp} bpp unsupported")
    bpx = bpp // 8
    stride = (w * bpp + 31) // 32 * 4
    top = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top else h - 1 - y
        rows.append(d[off + sy * stride: off + sy * stride + w * bpx])
    return w, h, bpx, rows


def bmp_complete(path):
    """True once the game has finished writing the file (it holds it with share mode 0)."""
    try:
        with open(path, "rb") as fh:
            hdr = fh.read(6)
        return len(hdr) == 6 and hdr[:2] == b"BM" and os.path.getsize(path) == struct.unpack_from("<I", hdr, 2)[0]
    except OSError:
        return False


def grey_small(path, step=8):
    """Downscaled (every `step` px) grey image as a list of rows of floats (0..255)."""
    w, h, bpx, rows = read_bmp(path)
    k = bpx * step
    return [[(b + g + r) / 3 for b, g, r in zip(rows[y][0::k], rows[y][1::k], rows[y][2::k])]
            for y in range(0, h, step)]


def image_stats(g):
    vals = [v for row in g for v in row]
    mean = sum(vals) / len(vals)
    return mean, math.sqrt(sum((v - mean) ** 2 for v in vals) / len(vals))


def best_shift(a, b, max_dx=64, max_dy=40, refine=True):
    """Where did the centre content of `a` move to in `b`? Block matching (mean abs diff over the
    centre half of the image, every 2nd sample) on downscaled grey images; coarse search in steps of
    2, then +-1 refinement. dx<0 = content moved left, dy>0 = content moved down (downscaled px).
    Same sign convention as test_headpose.best_shift (which searched +-40 x +-20, steps of 2)."""
    H, W = len(a), len(a[0])
    y0, y1, x0, x1 = H // 4, 3 * H // 4, W // 4, 3 * W // 4
    full = len(range(y0, y1, 2)) * len(range(x0, x1, 2))
    sub = operator.sub

    def cost(dx, dy):
        s = n = 0
        xs, xe = max(x0, -dx), min(x1, W - dx)
        if xs > x0:
            xs = x0 + -(-(xs - x0) // 2) * 2  # stay on the x0 + 2k sample grid
        if xs >= xe:
            return 0, 0
        for y in range(y0, y1, 2):
            yy = y + dy
            if 0 <= yy < H:
                ra = a[y][xs:xe:2]
                s += sum(map(abs, map(sub, ra, b[yy][xs + dx:xe + dx:2])))
                n += len(ra)
        return s, n

    best = None
    for dy in range(-max_dy, max_dy + 1, 2):
        for dx in range(-max_dx, max_dx + 1, 2):
            s, n = cost(dx, dy)
            if n >= 0.3 * full and (best is None or s / n < best[0]):
                best = (s / n, dx, dy)
    if best is None:
        return None
    if refine:
        _, bx, by = best
        for dy in (by - 1, by, by + 1):
            for dx in (bx - 1, bx, bx + 1):
                s, n = cost(dx, dy)
                if n >= 0.3 * full and s / n < best[0]:
                    best = (s / n, dx, dy)
    s0, n0 = cost(0, 0)
    res, dx, dy = best
    return {"dx": dx, "dy": dy, "res": round(res, 2), "res0": round(s0 / n0, 2) if n0 else None,
            "edge": abs(dx) >= max_dx - 1 or abs(dy) >= max_dy - 1}


def bmp_to_png(src, dst):
    w, h, bpx, rows = read_bmp(src)
    raw = []
    for r in rows:
        px = bytearray(w * 3)
        px[0::3] = r[2::bpx]
        px[1::3] = r[1::bpx]
        px[2::3] = r[0::bpx]
        raw.append(b"\0" + bytes(px))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(b"".join(raw), 6)) + chunk(b"IEND", b""))


def bmp_to_png_small(src, dst, f=4):
    """1/f point-sampled PNG preview of a BMP (keeps the multi-MB stereo captures out of the run folder)."""
    w, h, bpx, rows = read_bmp(src)
    ws = w // f
    raw = []
    for r in rows[::f]:
        px = bytearray(ws * 3)
        step = bpx * f
        px[0::3] = r[2::step][:ws]
        px[1::3] = r[1::step][:ws]
        px[2::3] = r[0::step][:ws]
        raw.append(b"\0" + bytes(px))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ws, len(raw), 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(b"".join(raw), 6)) + chunk(b"IEND", b""))


# ==== head pose evaluation ====================================================================
HEADPOSE_SHOTS = [  # name, pipe commands before the capture, settle seconds
    ("base", ["track off", "pose off"], 1.0),
    ("base2", [], 1.0),                     # noise floor: same view again
    ("yaw30", ["track on", "pose 30 0 0"], 1.0),
    ("yaw30_again", [], 3.0),               # accumulation check
    ("pitch20", ["pose 0 20 0"], 1.0),
    ("up30cm", ["pose off", "posepos 0 0 30"], 1.0),
    ("right30cm", ["pose off", "posepos 30 0 0"], 1.0),
]
FLAT_STD = 4.0  # base image std below this = blank/loading screen, metrics meaningless


def evaluate_headpose(m, base_std=None):
    """m: name -> best_shift() dict. Returns (ok, [(criterion, ok, value)])."""
    def g(n, k):
        return (m.get(n) or {}).get(k)

    crit = []
    if base_std is not None:
        crit.append(("base not flat", base_std >= FLAT_STD, f"std {base_std:.1f}"))
    y, ya = m.get("yaw30"), m.get("yaw30_again")
    crit.append(("yaw30 dx<-10", y is not None and y["dx"] < -10, g("yaw30", "dx")))
    crit.append(("yaw stable 3s", bool(y and ya and abs(y["dx"] - ya["dx"]) <= 1 and abs(y["dy"] - ya["dy"]) <= 3),
                 f"{g('yaw30', 'dx')},{g('yaw30', 'dy')} vs {g('yaw30_again', 'dx')},{g('yaw30_again', 'dy')}"))
    crit.append(("pitch20 dy>+10", m.get("pitch20") is not None and m["pitch20"]["dy"] > 10, g("pitch20", "dy")))
    crit.append(("up30cm dy>0", m.get("up30cm") is not None and m["up30cm"]["dy"] > 0, g("up30cm", "dy")))
    crit.append(("right30cm dx<0", m.get("right30cm") is not None and m["right30cm"]["dx"] < 0, g("right30cm", "dx")))
    return all(c[1] for c in crit), crit


def implied_hfov(m, width_px, step=8):
    """Horizontal FOV implied by the centre shift of a pure rotation: shift = f*tan(angle),
    hFOV = 2*atan((w/2)/f). Info only - a consistency check between yaw and pitch (square pixels)."""
    out = {}
    for name, key, ang in (("yaw30", "dx", 30.0), ("pitch20", "dy", 20.0)):
        d = abs((m.get(name) or {}).get(key) or 0) * step
        if d:
            f = d / math.tan(math.radians(ang))
            out[name] = round(2 * math.degrees(math.atan(width_px / 2 / f)), 1)
    return out


def headpose_summary(m, hfov=None):
    def f(n, k):
        v = (m.get(n) or {}).get(k)
        return "?" if v is None else f"{v:+d}"
    s = (f"yaw dx{f('yaw30', 'dx')} again{f('yaw30_again', 'dx')} | pitch dy{f('pitch20', 'dy')} | "
         f"up dy{f('up30cm', 'dy')} | right dx{f('right30cm', 'dx')} (x8px)")
    if hfov:
        s += " | implied hFOV " + "/".join(f"{v}" for v in hfov.values())
    if m.get("base2"):
        s += f" | noise {m['base2']['dx']:+d},{m['base2']['dy']:+d} res{m['base2']['res']}"
    edges = [n for n, v in m.items() if v and v.get("edge")]
    if edges:
        s += " | at search edge: " + ",".join(edges)
    return s


# ==== check plumbing ==========================================================================
class CheckFail(Exception):
    pass


class CheckSkip(Exception):
    pass


class Ctx:
    def __init__(self, g, outdir, args):
        self.g, self.outdir, self.args = g, outdir, args
        self.phase = ""
        self.cmd_started = None  # set while blocked in a pipe round trip
        self.xr = None          # XR up (status xrOk), learnt by the first check that reads status
        self.pipe_stuck = False

    def cmd(self, c, expect_ok=True):
        self.phase = f"> {c}"
        self.cmd_started = time.time()
        try:
            r = self.g.cmd(c, quiet=True)
        finally:
            self.cmd_started = None
        if expect_ok and r.startswith("error"):
            raise CheckFail(f"'{c}' -> {r[:120]}")
        return r

    def ready(self):
        return parse_kv(self.cmd("ready"))

    def status(self):
        r = self.cmd("status")
        st = parse_kv(r)
        if "xrOk" in st:
            self.xr = truthy(st["xrOk"])
        return st, r

    def wait(self, pred, timeout, what, poll=0.25):
        """Poll pred() until truthy; returns seconds taken; raises CheckFail on timeout."""
        t0 = time.time()
        while True:
            self.phase = f"waiting: {what} ({time.time() - t0:.0f}/{timeout}s)"
            if pred():
                return time.time() - t0
            if time.time() - t0 > timeout:
                raise CheckFail(f"{what}: timed out after {timeout}s")
            time.sleep(poll)

    def sleep(self, s, what):
        self.phase = what
        time.sleep(s)

    def capture(self, name, timeout=10):
        path = os.path.join(self.outdir, name + ".bmp")
        self.cmd("capture " + path.replace("\\", "/"))
        self.wait(lambda: bmp_complete(path), timeout, f"capture {name}", poll=0.1)
        return path

    def ensure_world(self, force=False):
        """In-world (ingame=1)? If not (or force), coc into the suite's cell. Returns the ready dict."""
        rd = self.ready()
        if rd.get("ingame") == "1" and not force:
            return rd
        load = getattr(self.args, "load", None)
        r = self.cmd(f"console load {load}" if load else f"console coc {self.args.cell}")
        if r != "queued":
            raise CheckFail(f"{'load' if load else 'coc'} -> {r}")
        self.sleep(1.0, "loading menu opening")
        self.wait(lambda: self.ready().get("ingame") == "1", 90, f"in-world at {load or self.args.cell}")
        self.sleep(3.0, "settle after load")
        return self.ready()


# ==== checks ==================================================================================
def check_smoke(ctx, data):
    t0 = time.time()
    r = ctx.cmd("ping")
    data["ping"] = r
    if r != "pong":
        raise CheckFail(f"ping -> {r!r}")
    st, raw = ctx.status()
    data["status"] = raw[:300]
    if "presents" not in st:
        raise CheckFail(f"status unparsable: {raw[:120]}")
    rd = ctx.ready()
    load = getattr(ctx.args, "load", None)
    if rd.get("ingame") == "1" and (load or rd.get("cell") == ctx.args.cell):
        data["coc"] = "already in world" if load else "already in cell"
    else:
        rd = ctx.ensure_world(force=True)
        data["coc"] = "load" if load else "coc"
    data["in_world_s"] = round(time.time() - t0, 1)
    data["ready"] = rd
    if load:
        if rd.get("ingame") != "1":
            raise CheckFail(f"not in world after load {load}: {rd}")
    elif rd.get("cell") not in (ctx.args.cell, "?"):
        raise CheckFail(f"in world but cell={rd.get('cell')} (wanted {ctx.args.cell})")
    return "PASS", (f"pong; in-world {data['in_world_s']}s cell={rd.get('cell')} camstate={rd.get('camstate')}; "
                    f"xrOk={st.get('xrOk')} state={st.get('state')} mode={st.get('mode')}")


def check_headpose(ctx, data):
    st, _ = ctx.status()
    if truthy(st.get("xrOk")):
        raise CheckSkip("image metric not usable at the XR render FOV; vr_head covers rotation under XR")
    ctx.ensure_world()
    f = parse_frustum(ctx.cmd("cam"))
    if f:
        data["frustum_hfov"] = round(hfov_deg(f["r"], f["l"]), 1)
    greys, paths = {}, {}
    try:
        for name, cmds, settle in HEADPOSE_SHOTS:
            for c in cmds:
                ctx.cmd(c)
            ctx.sleep(settle, f"settle before {name}")
            paths[name] = ctx.capture(name)
            greys[name] = grey_small(paths[name])
    finally:
        try:
            ctx.cmd("pose off", expect_ok=False)
            ctx.cmd("track off", expect_ok=False)
        except Exception:
            pass
    ctx.phase = "computing shifts"
    base = greys["base"]
    mean, std = image_stats(base)
    data["base_mean"], data["base_std"] = round(mean, 1), round(std, 1)
    shifts = {n: best_shift(base, g) for n, g in greys.items() if n != "base"}
    data["shifts"] = shifts
    data["implied_hfov"] = implied_hfov(shifts, len(base[0]) * 8)
    ok, crit = evaluate_headpose(shifts, std)
    data["criteria"] = [{"criterion": c, "ok": o, "value": v} for c, o, v in crit]
    ctx.phase = "writing PNGs"
    data["images"] = []
    for name, p in paths.items():
        png = p[:-4] + ".png"
        bmp_to_png(p, png)
        data["images"].append(os.path.basename(png))
        if not ctx.args.keep_bmp:
            os.remove(p)
    summary = headpose_summary(shifts, data["implied_hfov"])
    if "frustum_hfov" in data:
        summary += f" (frustum {data['frustum_hfov']})"
    if not ok:
        summary = "failed: " + ", ".join(f"{c}={v}" for c, o, v in crit if not o) + " | " + summary
    return ("PASS" if ok else "FAIL"), summary


def check_fov(ctx, data):
    st, raw = ctx.status()
    if ctx.xr:
        raise CheckSkip("XR running: the presenter sets worldFOV every frame (fovreq does not stick)")
    rd = ctx.ensure_world()
    if rd.get("camstate") != "0":
        raise CheckFail(f"camstate={rd.get('camstate')} (fovreq applies in first person only)")

    def fov_now():
        r = ctx.cmd("cam")
        f = parse_frustum(r)
        if not f:
            raise CheckFail(f"no frustum in cam reply: {r[:120]}")
        data["last_r"] = f["r"]
        return hfov_deg(f["r"])

    fov0 = fov_now()
    data["fov_default"] = round(fov0, 2)
    got = None
    try:
        ctx.cmd("fovreq 100")
        try:
            data["apply_s"] = round(ctx.wait(lambda: abs(fov_now() - 100) < 2, 5, "frustum ~100 deg"), 2)
        except CheckFail:
            pass
        got = fov_now()
        data["fov_req100"], data["r_req100"] = round(got, 2), data["last_r"]
    finally:
        ctx.cmd("fovreq 0", expect_ok=False)
    try:
        ctx.wait(lambda: abs(fov_now() - fov0) < 0.5, 5, "frustum back to default")
        restored = True
    except CheckFail:
        restored = False
    data["fov_after"], data["restored"] = round(fov_now(), 2), restored
    ok = abs(got - 100) < 2 and restored
    s = (f"default {fov0:.1f} -> req100 got {got:.2f} (r={data['r_req100']:.4f}) -> after fovreq 0: "
         f"{data['fov_after']:.1f}{'' if restored else ' NOT restored'}")
    return ("PASS" if ok else "FAIL"), s


def check_camstate(ctx, data):
    st, raw = ctx.status()
    mode0 = st.get("mode", "cinema")
    data["xr"], data["mode_before"] = ctx.xr, mode0
    rd = ctx.ensure_world()
    if rd.get("camstate") != "0":
        raise CheckFail(f"camstate={rd.get('camstate')} before tfc (want 0)")
    g = ctx.g
    toggled = False
    log_ok = {}
    try:
        ctx.cmd("track on")
        ctx.cmd("mode mono")
        ctx.sleep(0.5, "mode switch")
        mark = g.log_mark()
        ctx.cmd("console tfc")
        toggled = True
        data["to_free_s"] = round(ctx.wait(lambda: ctx.ready().get("camstate") not in ("0", None), 10,
                                           "camstate != 0 after tfc"), 2)
        data["camstate_free"] = cs1 = ctx.ready().get("camstate")
        if True:
            try:
                ctx.wait(lambda: "(cinema fallback)" in g.log_since(mark), 5, "log 'cinema fallback'")
                log_ok["cinema fallback"] = True
            except CheckFail:
                log_ok["cinema fallback"] = False
        mark = g.log_mark()
        ctx.cmd("console tfc")
        toggled = False
        data["back_s"] = round(ctx.wait(lambda: ctx.ready().get("camstate") == "0", 10, "camstate 0 after 2nd tfc"), 2)
        if True:
            try:
                ctx.wait(lambda: "(world view)" in g.log_since(mark), 5, "log 'world view'")
                log_ok["world view"] = True
            except CheckFail:
                log_ok["world view"] = False
    finally:
        try:
            if toggled:
                ctx.cmd("console tfc", expect_ok=False)
                ctx.wait(lambda: ctx.ready().get("camstate") == "0", 10, "restore camstate 0")
            ctx.cmd(f"mode {mode0 if mode0 in ('mono', 'cinema') else 'cinema'}", expect_ok=False)
            ctx.cmd("track off", expect_ok=False)
        except Exception:
            pass
    data["log"] = log_ok
    states = f"camstate 0->{cs1} in {data['to_free_s']}s, ->0 in {data['back_s']}s"
    if cs1 != "3":
        return "FAIL", states + " (expected 3 = free camera)"
    missing = [k for k, v in log_ok.items() if not v]
    if missing:
        return "FAIL", states + "; log missing: " + ", ".join(missing)
    return "PASS", states + "; log: cinema fallback + world view"


def vr_prepare(ctx, data):
    """Common setup for the vremu checks: skip rules, in-world, track on, mode mono, pose off.
    Returns the previous mode (restore with vr_restore)."""
    if not ctx.args.vremu:
        raise CheckSkip("--vremu not given")
    if not vremu_up():
        raise CheckSkip(f"vremu /state unreachable at {VREMU_URL}")
    st, raw = ctx.status()
    data["xrOk"], data["state"] = ctx.xr, st.get("state")
    if not ctx.xr:
        raise CheckSkip("XR not running (xrOk=0)")
    ctx.ensure_world()
    mode0 = st.get("mode", "cinema")
    ctx.cmd("track on")
    ctx.cmd("mode mono")
    ctx.cmd("pose off")
    ctx.sleep(0.5, "mode switch")
    return mode0


def vr_restore(ctx, mode0):
    try:
        ctx.cmd(f"mode {mode0 if mode0 in ('mono', 'cinema') else 'cinema'}", expect_ok=False)
        ctx.cmd("track off", expect_ok=False)
    except Exception:
        pass


def cam_read(ctx):
    r = ctx.cmd("cam")
    yp = cam_yaw_pitch(r)
    if yp is None:
        raise CheckFail(f"no worldRot in cam reply: {r[:120]}")
    return yp


def check_vr_head(ctx, data):
    mode0 = vr_prepare(ctx, data)
    try:
        vremu_get("/pose", dev="hmd", ypr="0,0,0")
        ctx.sleep(1.0, "hmd to 0,0,0")
        ctx.cmd("pose off")

        def pair(hmd_ypr, ovr_cmd, what):
            b = cam_read(ctx)
            vremu_get("/pose", dev="hmd", ypr=hmd_ypr)
            ctx.sleep(1.0, f"hmd {what}")
            h = cam_read(ctx)
            vremu_get("/pose", dev="hmd", ypr="0,0,0")
            ctx.sleep(1.0, "hmd reset")
            ctx.cmd(ovr_cmd)
            ctx.sleep(1.0, f"override {what}")
            o = cam_read(ctx)
            ctx.cmd("pose off")
            ctx.sleep(0.5, "pose off")
            return b, h, o

        b, h, o = pair("30,0,0", "pose 30 0 0", "yaw 30")
        dyaw_hmd = wrap180(h[0] - b[0])
        dyaw_ovr = wrap180(o[0] - b[0])
        yaw_ok = abs(abs(dyaw_ovr) - 30.0) <= 5.0 and abs(dyaw_hmd - dyaw_ovr) <= 5.0
        data.update(yaw_base=round(b[0], 2), yaw_hmd=round(h[0], 2), yaw_ovr=round(o[0], 2),
                    dyaw_hmd=round(dyaw_hmd, 2), dyaw_ovr=round(dyaw_ovr, 2))
        b, h, o = pair("0,20,0", "pose 0 20 0", "pitch 20")
        dfz_hmd = h[1] - b[1]
        dfz_ovr = o[1] - b[1]
        pitch_ok = abs(dfz_ovr) >= 0.15 and abs(dfz_hmd - dfz_ovr) <= 0.08
        data.update(fz_base=round(b[1], 4), fz_hmd=round(h[1], 4), fz_ovr=round(o[1], 4),
                    dfz_hmd=round(dfz_hmd, 4), dfz_ovr=round(dfz_ovr, 4))
    finally:
        try:
            vremu_get("/pose", dev="hmd", ypr="0,0,0")
        except Exception:
            pass
        try:
            ctx.cmd("pose off", expect_ok=False)
        except Exception:
            pass
        vr_restore(ctx, mode0)
    s = (f"yaw hmd {dyaw_hmd:+.1f} ovr {dyaw_ovr:+.1f}; pitch dfz hmd {dfz_hmd:+.3f} ovr {dfz_ovr:+.3f}")
    return ("PASS" if yaw_ok and pitch_ok else "FAIL"), s


def check_vr_move(ctx, data):
    mode0 = vr_prepare(ctx, data)
    try:
        r = ctx.cmd("cam")
        p0 = parse_cam_pos(r)
        if p0 is None:
            raise CheckFail(f"no worldPos in cam reply: {r[:120]}")
        vremu_get("/input", dev="left", joy="0,1", ms=1500)
        ctx.sleep(2.0, "walking forward")
        r = ctx.cmd("cam")
        p1 = parse_cam_pos(r)
        if p1 is None:
            raise CheckFail(f"no worldPos in cam reply: {r[:120]}")
    finally:
        try:
            vremu_get("/release", dev="left", btn="all")
        except Exception:
            pass
        vr_restore(ctx, mode0)
    dist = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
    data.update(pos0=p0, pos1=p1, dist=round(dist, 1))
    return ("PASS" if dist > 50 else "FAIL"), f"moved {dist:.1f} units horizontally (want > 50) {p0} -> {p1}"


def check_vr_comfort(ctx, data):
    """C8: one snap flick turns the heading (and the owned view yaw) by exactly fSnapTurnDeg with no drift;
    the vignette layer is submitted while walking with the stick and gone again after the fade."""
    mode0 = vr_prepare(ctx, data)
    try:
        cfg = parse_kv(ctx.cmd("comfort cfg"))
        snap_deg = float(cfg.get("snapDeg", "30"))
        ctx.cmd("comfort turn snap")
        ctx.cmd("comfort vignette on")
        vremu_get("/pose", dev="hmd", ypr="0,0,0")
        ctx.sleep(1.0, "settle")

        def hdg():
            r = parse_kv(ctx.cmd("rs"))
            return math.degrees(float(r["heading"])), math.degrees(float(r["ownYaw"]))

        h0, o0 = hdg()
        vremu_get("/input", dev="right", joy="1,0", ms=350)
        ctx.sleep(1.0, "snap right")
        h1, o1 = hdg()
        ctx.sleep(1.0, "drift window")
        h2, _ = hdg()
        d_h, d_o, drift = wrap180(h1 - h0), wrap180(o1 - o0), wrap180(h2 - h1)
        snap_ok = abs(d_h - snap_deg) < 0.5 and abs(d_o - snap_deg) < 0.5 and abs(drift) < 0.2
        data.update(snap_deg=snap_deg, d_heading=round(d_h, 3), d_own=round(d_o, 3), drift=round(drift, 3))
        vremu_get("/input", dev="right", joy="-1,0", ms=350)  # turn back
        ctx.sleep(1.0, "snap back")

        ctx.cmd("comfort stats reset")
        ctx.sleep(1.0, "idle")
        idle = parse_kv(ctx.cmd("comfort"))
        vremu_get("/input", dev="left", joy="0,1", ms=1200)
        ctx.sleep(0.7, "walking")
        walk = parse_kv(ctx.cmd("comfort"))
        ctx.sleep(1.3, "fade out")
        after = parse_kv(ctx.cmd("comfort"))
        li, lw = int(walk.get("layersWithoutVig", "0")), int(walk.get("layersWithVig", "0"))
        vig_ok = (idle.get("vigFrames") == "0" and int(walk.get("vigFrames", "0")) > 0 and walk.get("alpha") == "1.000" and
                  lw == li + 1 and after.get("alpha") == "0.000" and after.get("lastLayers") == str(li))
        data.update(idle_vig=idle.get("vigFrames"), walk_alpha=walk.get("alpha"), layers_without=li, layers_with=lw,
                    after_alpha=after.get("alpha"), after_layers=after.get("lastLayers"), vig_frames=after.get("vigFrames"),
                    vig_tail=after.get("vigTail"))
    finally:
        try:
            vremu_get("/release", dev="left", btn="all")
            vremu_get("/release", dev="right", btn="all")
        except Exception:
            pass
        vr_restore(ctx, mode0)
    s = (f"snap {data['d_heading']:+.2f} deg (own {data['d_own']:+.2f}, want {snap_deg:+.0f}), drift {data['drift']:+.2f}; "
         f"vignette idle {data['idle_vig']} frames, walking alpha {data['walk_alpha']} layers {li}->{lw}, after alpha "
         f"{data['after_alpha']} layers {data['after_layers']} (tail {data['vig_tail']} frames)")
    return ("PASS" if snap_ok and vig_ok else "FAIL"), s


def check_vr_roomscale(ctx, data):
    """B4 overshoot regression: an instant 20 cm HMD step forward moves the camera to the room-stable position
    (start + HMD displacement x 70 units/m, from the plugin's per-camera-update trace 'rs camhist') within 0.5
    units at steady state while the capsule follows (moved > 5 units). Also records the camera step-2 gate patches."""
    mode0 = vr_prepare(ctx, data)
    upm = 70.0
    try:
        vremu_get("/pose", dev="hmd", pos="0,1.7,0", ypr="0,0,0")
        ctx.sleep(1.0, "hmd to origin")
        ctx.cmd("recenter")
        ctx.sleep(1.5, "recenter settle")

        def trace(n=600):
            out = []
            for part in ctx.cmd(f"rs camhist {n}").split(" | ")[1:]:
                v = part.split()
                if len(v) >= 23:
                    f = [float(x) for x in v]
                    out.append(dict(t=f[0], root=f[2:4], loc=f[5:7], hmd=f[15:17], yaw=f[19]))
            return out

        t0 = trace(1)[-1]["t"]
        vremu_get("/pose", dev="hmd", rel="1", z="-0.2", ms="0")
        ctx.sleep(2.0, "20 cm step")
        fr = [f for f in trace() if f["t"] >= t0]
        if len(fr) < 30:
            raise CheckFail(f"camera trace too short ({len(fr)} updates)")
        f0 = fr[0]

        def err(f):
            c, sn = math.cos(f["yaw"]), math.sin(f["yaw"])
            dx, dy = (f["hmd"][0] - f0["hmd"][0]) * upm, (f["hmd"][1] - f0["hmd"][1]) * upm
            ex = f["root"][0] - f0["root"][0] - (dx * c + dy * sn)
            ey = f["root"][1] - f0["root"][1] - (-dx * sn + dy * c)
            return math.hypot(ex, ey)

        tail = [f for f in fr if f["t"] > fr[-1]["t"] - 500]
        e_ss = max(err(f) for f in tail)
        e_max = max(err(f) for f in fr)
        loc = math.hypot(fr[-1]["loc"][0] - f0["loc"][0], fr[-1]["loc"][1] - f0["loc"][1])
        cam = math.hypot(fr[-1]["root"][0] - f0["root"][0], fr[-1]["root"][1] - f0["root"][1])
        data.update(updates=len(fr), cam_moved=round(cam, 2), loc_moved=round(loc, 2), err_steady=round(e_ss, 3), err_max=round(e_max, 2))
        gates = ctx.cmd("camdir gates", expect_ok=False)
        data["gates"] = gates
    finally:
        try:
            vremu_get("/pose", dev="hmd", pos="0,1.7,0", ypr="0,0,0")
        except Exception:
            pass
        vr_restore(ctx, mode0)
    ok = e_ss <= 0.5 and loc > 5.0 and abs(cam - 14.0) <= 0.75
    return ("PASS" if ok else "FAIL"), (f"20 cm step: camera {cam:.2f} u (want 14), capsule {loc:.2f} u, error steady {e_ss:.3f} "
                                        f"(<= 0.5) max {e_max:.2f} over {len(fr)} updates")


def check_vr_gun(ctx, data):
    """Floating gun = FO4VR baseline ([Weapon] bFloatingGun; replaces the retired vr_hands IK check): with the 10mm drawn
    the WEAPON node sits on the primary controller (err ~0), a 30 cm hand move moves it 21 units, P/D come from the fire
    node, and the first-person arm / Pip-Boy models are culled and skipped by A1 (not drawn)."""
    mode0 = vr_prepare(ctx, data)

    def gun():
        return parse_kv(ctx.cmd("gun"))

    def v3(s):
        m = re.match(r"\(([^)]*)\)", s or "")
        return tuple(float(x) for x in m.group(1).split(",")) if m else None

    try:
        g0 = gun()
        if g0.get("W1") != "ok":
            raise CheckSkip(f"[Weapon] bFloatingGun off or not installed: {ctx.cmd('gun')[:100]}")
        ctx.cmd("console player.additem 00004822 1")
        ctx.cmd("console player.additem 0001F276 50")
        ctx.sleep(1.0, "additem")
        ctx.cmd("aim equip 00004822")
        vremu_get("/pose", dev="hmd", pos="0,1.7,0", ypr="0,0,0")
        vremu_get("/pose", dev="right", pos="0.2,1.3,-0.35", ypr="0,39,0")
        ctx.wait(lambda: gun().get("gate") == "active", 12, "floating gun active (10mm drawn)", poll=0.5)
        ctx.sleep(1.0, "settle")
        a = gun()
        vremu_get("/pose", dev="right", pos="0.5,1.3,-0.35", ypr="0,39,0")
        ctx.sleep(0.8, "hand 30 cm right")
        b = gun()
        moved = math.dist(v3(a["wWorld"]), v3(b["wWorld"]))
        ctx.sleep(1.0, "skips accumulate")
        c = gun()
    finally:
        try:
            vremu_get("/pose", dev="right", pos="0.2,1.3,-0.35", ypr="0,39,0")
        except Exception:
            pass
        vr_restore(ctx, mode0)
    skips = int(c.get("cullSkips", "0")) - int(a.get("cullSkips", "0"))
    data.update(err=a.get("err"), angErr=a.get("angErr"), moved=round(moved, 3), pFire=b.get("pFire"), dFireDeg=b.get("dFireDeg"),
                armsCulled=c.get("armsCulled"), armsCandidates=c.get("armsCandidates"), A1=c.get("A1"), cullSkips=skips, costAvgUs=c.get("avg"))
    ok = (float(a["err"]) < 0.05 and float(a["angErr"]) < 0.05 and abs(moved - 21.0) < 0.5 and float(b["pFire"]) < 0.1 and
          float(b["dFireDeg"]) < 0.5 and c.get("A1") == "ok" and int(c.get("armsCulled", "0")) >= 2 and skips > 0)
    s = (f"WEAPON err {a['err']} u / {a['angErr']} deg; hand 30 cm -> {moved:.2f} u (want 21); P-fire {b['pFire']} u / {b['dFireDeg']} deg; "
         f"arms culled {c.get('armsCulled')}/{c.get('armsCandidates')}, A1 {c.get('A1')} skipped {skips} adds in ~1 s; cost avg {c.get('avg')} us")
    return ("PASS" if ok else "FAIL"), s


def pad_polls(ctx):
    r = ctx.cmd("pad ms=0", expect_ok=False)
    m = re.search(r"polls so far (\d+)", r)
    if not m:
        raise CheckFail(f"no 'polls so far' in pad reply: {r[:120]}")
    return int(m.group(1))


def check_vr_buttons(ctx, data):
    mode0 = vr_prepare(ctx, data)
    try:
        n0 = pad_polls(ctx)
        try:
            vremu_get("/press", dev="right", btn="a", ms=150)
        except Exception as e:
            raise CheckFail(f"/press errored: {type(e).__name__}: {e}")
        ctx.sleep(1.0, "after press")
        n1 = pad_polls(ctx)
    finally:
        vr_restore(ctx, mode0)
    data.update(polls_before=n0, polls_after=n1)
    return ("PASS" if n1 > n0 else "FAIL"), f"pad polls {n0} -> {n1} (want increase); /press ok"


def check_timing(ctx, data):
    def edge(timeout=3.0):
        """Wait for the status line to refresh (it updates every 30 presents) -> (t, presents, xrFrames)."""
        p_prev = parse_kv(ctx.cmd("status")).get("presents")
        t0 = time.time()
        while time.time() - t0 < timeout:
            st = parse_kv(ctx.cmd("status"))
            if st.get("presents") != p_prev:
                return time.time(), int(st["presents"]), int(st.get("xrFrames", 0)), st
            time.sleep(0.02)
        raise CheckFail(f"status presents stuck at {p_prev} for {timeout}s (game not presenting?)")

    t0, p0, x0, st = edge()
    ctx.xr = truthy(st.get("xrOk"))
    ctx.sleep(4.5, "sampling presents")
    t1, p1, x1, st = edge()
    dt = t1 - t0
    data.update(dt_s=round(dt, 2), presents=p1 - p0, xr_frames=x1 - x0,
                presents_per_s=round((p1 - p0) / dt, 1), xr_frames_per_s=round((x1 - x0) / dt, 1),
                xrOk=st.get("xrOk"), mode=st.get("mode"))
    return "PASS", (f"presents {data['presents_per_s']}/s, xrFrames {data['xr_frames_per_s']}/s "
                    f"over {dt:.1f}s (xrOk={st.get('xrOk')} mode={st.get('mode')})")


def check_exit(ctx, data):
    g = ctx.g
    if g.attached:
        raise CheckSkip("attached to a running game: not quitting it")
    if not g.launched:
        raise CheckSkip("game not launched")
    if ctx.pipe_stuck:
        g.kill()
        g.launched = False
        raise CheckFail("pipe stuck after an earlier timeout: killed instead of qqq")
    ctx.phase = "qqq -> waiting for process exit"
    t = g.quit(grace=15)
    data.update(g.exit_info or {})
    if t is None:
        return "FAIL", "no exit within 15s of qqq: killed"
    return "PASS", f"qqq -> process exit {t:.1f}s"


def halves_stats(path, mask=None, band=64, step=2):
    """Back buffer [left | right]: share of sampled pixels whose halves differ by <= 1 / <= 4 (max channel), outside a
    `band`-px column band at the seam and the outer edges (scaled to the width). mask: a GBuffer RT0 capture of the same
    view (GBuffer cleared): only pixels with geometry in both halves count (QASmoke has no sky; its sky pixels keep
    stale history in mono too)."""
    w, h, bpx, rows = read_bmp(path)
    mrows, mbpx = (read_bmp(mask)[3], read_bmp(mask)[2]) if mask else (None, 0)
    half = w // 2
    band = int(band * w / 2880 + 0.5)
    n = n1 = n4 = 0
    for y in range(0, h, step):
        r = rows[y]
        for x in range(band, half - band, step):
            if mrows is not None:
                m = mrows[y]
                if not (any(m[x * mbpx:x * mbpx + 3]) and any(m[(x + half) * mbpx:(x + half) * mbpx + 3])):
                    continue
            a, b = x * bpx, (x + half) * bpx
            d = max(abs(r[a] - r[b]), abs(r[a + 1] - r[b + 1]), abs(r[a + 2] - r[b + 2]))
            n += 1
            n1 += d <= 1
            n4 += d <= 4
    return 100.0 * n1 / max(n, 1), 100.0 * n4 / max(n, 1)


def halves_disparity(path, blk=8, eye_px=344):
    """Block disparities between the back-buffer halves [left | right] (pure python). The image is point-sampled to
    ~eye_px grey columns per eye; each textured left-eye block (blk x blk samples) is matched along its row in the
    right eye over +-1/4 of the eye width (as far as the window stays inside the right eye). s > 0 = crossed parallax (right[x - s] ~ left[x], nearer = larger); far
    content and sky give 0. Only blocks with a clear minimum (best error < 0.4 x the median over shifts) count.
    -> dict(n, pos, neg, zero (shares of blocks with s > 0 / < 0 / = 0), med_px, p90_px, step_px)"""
    w, h, bpx, rows = read_bmp(path)
    half = w // 2
    sub = max(1, half // eye_px)
    hw = half // sub
    g = []
    for y in range(0, h, sub):
        r = rows[y]
        g.append([r[x * bpx] + r[x * bpx + 1] + r[x * bpx + 2] for x in range(0, w, sub)][:2 * hw])  # grey x3
    D = hw // 4
    out = []
    for y0 in range(0, len(g) - blk + 1, blk):
        band = g[y0:y0 + blk]
        for x0 in range(hw // 16, hw - blk - hw // 16 + 1, blk):
            L = [row[x0:x0 + blk] for row in band]
            flat = [v for r in L for v in r]
            m = sum(flat) / len(flat)
            if sum((v - m) ** 2 for v in flat) / len(flat) < 12.0 ** 2:  # grey x3: std >= 4 levels
                continue
            errs = []
            s_lo, s_hi = max(-D, x0 + blk - hw), min(D, x0)  # window inside the right eye: hw <= hw + x0 - s <= 2 hw - blk
            for s in range(s_lo, s_hi + 1):
                xr = hw + x0 - s
                e = 0
                for lr, row in zip(L, band):
                    e += sum(map(abs, map(int.__sub__, lr, row[xr:xr + blk])))
                errs.append(e)
            if len(errs) < 8:
                continue
            best = min(errs)
            med = sorted(errs)[len(errs) // 2]
            if best >= 0.4 * med:
                continue
            out.append((errs.index(best) + s_lo) * sub)
    n = len(out)
    out.sort()
    res = dict(n=n, step_px=sub)
    if n:
        res.update(pos=round(sum(s > 0 for s in out) / n, 3), neg=round(sum(s < 0 for s in out) / n, 3),
                   zero=round(sum(s == 0 for s in out) / n, 3), med_px=out[n // 2], p90_px=out[int(n * 0.9)])
    return res


def check_stereo(ctx, data):
    """Single-pass stereo (if the hooks are installed): test eyes at IPD 0 -> the back-buffer halves agree outside
    the seam band; IPD 0.5 m -> crossed parallax (right half shifted left). Restores the mode and test knobs."""
    st0 = parse_kv(ctx.cmd("stereostat", expect_ok=False))
    if "mode" not in st0 or st0.get("hooks") != "1":
        raise CheckSkip("stereo hooks not installed ([Stereo] iMode=0, bInstallHooks=0)")
    ctx.ensure_world()
    mode0, ipd0, test0 = st0.get("mode", "0"), st0.get("ipd", "0.064m").rstrip("m"), st0.get("testEyes", "0")
    st, _ = ctx.status()
    xr = truthy(st.get("xrOk"))
    try:
        if not xr:
            ctx.cmd("fovreq 127.8")  # union of two 90 deg test eyes in 2 x (1440x1620)
        for c in ("console tm", "stereotest 1", "stereofov 45", "stereoipd 0", "stereoclear 0", "stereo 3"):
            ctx.cmd(c)
        ctx.sleep(6.0, "stereo + cuts settle (SAO history)")
        s = parse_kv(ctx.cmd("stereostat"))
        data["stereostat"] = {k: s.get(k) for k in ("mode", "split", "b12", "stereoFrames", "saoPerEye")}
        if s.get("stereoFrames") != "1":
            raise CheckFail(f"no stereo frames rendered (stereostat {data['stereostat']})")
        # Full frame, no GBuffer clear and no GBuffer mask (15 Nov): "stereoclear 1" clears at the first group-8 GBuffer
        # draw, which in exterior cells comes after most geometry (terrain, objects: albedo/normals wiped -> dark,
        # desaturated frame, decals black) and its RT0 capture covered 0.2 % of the frame, so the old masked number
        # measured a few foliage pixels of a broken frame. The sky is rendered every frame in vrtest_sanctuary.
        p0 = ctx.capture("stereo_ipd0")
        same1, same4 = halves_stats(p0)
        data.update(halves_within1=round(same1, 3), halves_within4=round(same4, 3))
        ctx.cmd("stereoipd 0.5")
        ctx.sleep(1.5, "IPD 0.5 settle")
        p5 = ctx.capture("stereo_ipd0.5")
        d5 = halves_disparity(p5)
        d0 = halves_disparity(p0)
        data.update(disp_ipd05=d5, disp_ipd0=d0)
    finally:
        for c in (f"stereoipd {ipd0}", f"stereotest {test0}", "stereoclear 0", f"stereo {mode0}", "console tm"):
            ctx.cmd(c, expect_ok=False)
        if not xr:
            ctx.cmd("fovreq 0", expect_ok=False)
    if not ctx.args.keep_bmp:  # 63 MB each at 5504x2880: keep 1/4 PNG previews only
        for n in ("stereo_ipd0", "stereo_ipd0.5"):
            p = os.path.join(ctx.outdir, n + ".bmp")
            if os.path.exists(p):
                bmp_to_png_small(p, p[:-4] + "_q.png")
                os.remove(p)
    # disparity: at IPD 0 the halves match block for block (shift 0); at IPD 0.5 m near blocks shift crossed (s > 0)
    # and almost none uncrossed (a swapped or mirrored eye would give s < 0). Sky / far blocks stay at 0.
    ok_d0 = d0.get("n", 0) >= 50 and d0.get("zero", 0) >= 0.95
    ok_d5 = d5.get("n", 0) >= 50 and d5.get("pos", 0) >= 0.25 and d5.get("neg", 1) <= 0.05
    ok = same4 >= 99.0 and ok_d0 and ok_d5
    summary = (f"IPD 0 halves (full frame, 64-px seam/outer bands excluded) {same1:.2f}% within 1, {same4:.2f}% within 4 "
               f"(want within 4 >= 99); block disparity IPD 0: {d0.get('zero', 0) * 100:.0f}% of {d0.get('n', 0)} blocks at 0 "
               f"(want >= 95%); IPD 0.5 m: crossed {d5.get('pos', 0) * 100:.0f}% / uncrossed {d5.get('neg', 0) * 100:.0f}% "
               f"of {d5.get('n', 0)} blocks, median {d5.get('med_px', 0):+d} px, p90 {d5.get('p90_px', 0):+d} px "
               f"(want crossed >= 25%, uncrossed <= 5%)")
    return ("PASS" if ok else "FAIL"), summary


CHECKS = [  # name, function, hard timeout (s), needs a live pipe
    ("smoke", check_smoke, 120),
    ("headpose", check_headpose, 150),
    ("fov", check_fov, 40),
    ("camstate", check_camstate, 60),
    ("vr_head", check_vr_head, 40),
    ("vr_move", check_vr_move, 40),
    ("vr_buttons", check_vr_buttons, 40),
    ("vr_comfort", check_vr_comfort, 45),
    ("vr_roomscale", check_vr_roomscale, 40),
    ("vr_gun", check_vr_gun, 50),
    ("stereo", check_stereo, 120),
    ("timing", check_timing, 25),
    ("exit", check_exit, 40),
]


def run_check(ctx, name, fn, timeout):
    data, out = {}, {}

    def target():
        try:
            out["r"] = fn(ctx, data)
        except CheckSkip as e:
            out["r"] = ("SKIP", str(e))
        except CheckFail as e:
            out["r"] = ("FAIL", str(e))
        except BaseException as e:  # SystemExit from fo4test waits, pipe errors, bugs
            out["r"] = ("FAIL", f"{type(e).__name__}: {e}"[:300])

    ctx.phase, ctx.cmd_started = "start", None
    say(f"{name}: start (timeout {timeout}s)")
    t0 = last = time.time()
    th = threading.Thread(target=target, name=name, daemon=True)
    th.start()
    while th.is_alive():
        th.join(0.25)
        now = time.time()
        if th.is_alive() and now - t0 > timeout:
            ctx.pipe_stuck = True  # the worker may be blocked inside a pipe read
            out["r"] = ("FAIL", f"hard timeout after {timeout}s during: {ctx.phase}")
            break
        cs = ctx.cmd_started
        if th.is_alive() and cs is not None and now - cs > CMD_STUCK:
            ctx.pipe_stuck = True
            out["r"] = ("FAIL", f"no pipe reply within {CMD_STUCK:.0f}s to: {ctx.phase[2:]}")
            break
        if now - last >= PROGRESS_EVERY:
            say(f"  {name} {now - t0:.0f}s: {ctx.phase}")
            last = now
    status, summary = out["r"]
    dur = round(time.time() - t0, 1)
    say(f"{name}: {status} ({dur}s) {summary}")
    return {"check": name, "status": status, "summary": summary, "duration_s": dur, "data": dict(data)}


# ==== main ====================================================================================
def print_table(rows, wall):
    w = max(len(r["check"]) for r in rows)
    print()
    print(f"{'check'.ljust(w)} | result | s     | key numbers")
    print(f"{'-' * w}-+--------+-------+------------")
    for r in rows:
        print(f"{r['check'].ljust(w)} | {r['status']:<6} | {r['duration_s']:5.1f} | {r['summary']}")
    print(f"total wall time {wall:.1f}s")


def run_suite(args):
    names = [n for n, _, _ in CHECKS]
    sel = names if not args.only else [s.strip() for s in args.only.split(",") if s.strip()]
    bad = [s for s in sel if s not in names]
    if bad:
        raise SystemExit(f"unknown check(s): {', '.join(bad)} (have {', '.join(names)})")
    t_start = time.time()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(SUITE_DIR, stamp)
    os.makedirs(outdir, exist_ok=True)
    report = {"started": stamp, "args": vars(args), "checks_selected": sel, "dir": outdir, "results": []}
    rows = report["results"]
    say(f"suite: {','.join(sel)} | no_xr={args.no_xr} attach={args.attach} | out {outdir}")

    g = Game(runtime_json=args.runtime_json, no_xr=args.no_xr, attach=args.attach, quiet=True)
    ctx = Ctx(g, outdir, args)
    try:
        t0 = time.time()
        try:
            g.__enter__()
            rows.append({"check": "launch", "status": "PASS", "duration_s": round(time.time() - t0, 1),
                         "summary": "attached to running game" if g.attached else "launched; pipe + renderer/XR init up",
                         "data": {}})
        except BaseException as e:
            rows.append({"check": "launch", "status": "FAIL", "duration_s": round(time.time() - t0, 1),
                         "summary": f"{type(e).__name__}: {e}"[:300], "data": {}})
            if isinstance(e, KeyboardInterrupt):
                raise
            return rows, report, outdir, t_start
        for name, fn, timeout in CHECKS:
            if name not in sel:
                continue
            if name != "exit":
                if ctx.pipe_stuck:
                    rows.append({"check": name, "status": "SKIP", "duration_s": 0.0,
                                 "summary": "pipe stuck after an earlier timeout", "data": {}})
                    continue
                if g.launched and not fo4test.game_pid():
                    rows.append({"check": name, "status": "SKIP", "duration_s": 0.0,
                                 "summary": "game process gone (crash?)", "data": {}})
                    continue
            rows.append(run_check(ctx, name, fn, timeout))
    finally:
        if getattr(args, "vremu", False):
            report["vremu_cleanup"] = vremu_cleanup()
            say("vremu: " + report["vremu_cleanup"])
        # teardown: never leave a launched game running
        if g.launched:
            if ctx.pipe_stuck:
                g.kill()
                g.launched = False
            else:
                g.quit()
        report["wall_s"] = round(time.time() - t_start, 1)
        with open(os.path.join(outdir, "report.json"), "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=1, default=str)
    return rows, report, outdir, t_start


# ==== offline self-test =======================================================================
def selftest():
    fails = []

    def expect(label, cond, info=""):
        print(f"  {'ok ' if cond else 'BAD'} {label}{(' - ' + str(info)) if info != '' else ''}")
        if not cond:
            fails.append(label)

    print("parsers")
    st = parse_kv("presents=1831 xrInit=true xrOk=false state=0 xrFrames=0 submitting=false mode=cinema "
                  "fov=0.0x0.0 runtime='' err='runtime json not found: C:\\nonexistent\\x.json'")
    expect("status presents", st.get("presents") == "1831")
    expect("status xrOk false", truthy(st.get("xrOk")) is False)
    expect("status quoted err with spaces", st.get("err", "").startswith("runtime json not found"), st.get("err"))
    expect("status runtime empty", st.get("runtime") == "")
    st = parse_kv("presents=901 xrInit=true xrOk=true state=5 xrFrames=900 submitting=true mode=mono "
                  "fov=129.5x100.0 runtime='Meta XR Simulator' err=''")
    expect("status xrOk true / runtime with space", truthy(st["xrOk"]) and st["runtime"] == "Meta XR Simulator")
    rd = parse_kv("ingame=1 loading=0 mainmenu=0 cell=QASmoke camstate=0")
    expect("ready", rd == {"ingame": "1", "loading": "0", "mainmenu": "0", "cell": "QASmoke", "camstate": "0"})
    expect("ready on error reply", parse_kv("error: renderer not ready") == {})
    cam = ("fp update=10 getRot=10 getPos=10 applied=false state=0 root 'Camera Root' localRot=[1.000 0.000 0.000 | "
           "0.000 1.000 0.000 | 0.000 0.000 1.000] worldRot=[...] worldPos=(1.0,2.0,3.0) children=1 child 'WorldRoot "
           "Camera' rtti=NiCamera worldRot=[...] frustum l=-2.1187 r=2.1187 t=1.1918 b=-1.1918 n=15.00 f=353840 "
           "ortho=false port=(0.00,1.00,1.00,0.00) worldFOV=129.47 firstPersonFOV=129.47")
    f = parse_frustum(cam)
    expect("frustum parse", f == {"l": -2.1187, "r": 2.1187, "t": 1.1918, "b": -1.1918}, f)
    expect("hfov r=2.1187 -> 129.47 (README)", abs(hfov_deg(f["r"]) - 129.47) < 0.02, round(hfov_deg(f["r"]), 3))
    expect("hfov r=tan(50deg) -> 100", abs(hfov_deg(math.tan(math.radians(50))) - 100) < 1e-6)
    expect("no frustum -> None", parse_frustum("no PlayerCamera") is None)

    print("cam rotation -> yaw/pitch")

    def cam_str(rot, pos="(1.0,2.0,3.0)"):
        return ("fp update=1 root 'Camera Root' localRot=[1 0 0 | 0 1 0 | 0 0 1] worldRot=[" +
                " | ".join(" ".join(f"{x:.3f}" for x in rot[i:i + 3]) for i in (0, 3, 6)) +
                f"] worldPos={pos} children=1 child 'C' worldRot=[9 9 9 | 9 9 9 | 9 9 9]")

    # Row convention (measured): row i = world image of camera-local axis i (X right, Y forward, Z up).
    def rz(deg):  # yaw right (clockwise seen from +Z): forward (0,1,0) -> (sin, cos, 0), right -> (cos, -sin, 0)
        c, s_ = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        return [c, -s_, 0, s_, c, 0, 0, 0, 1]

    def rx(deg):  # pitch up: forward (0,1,0) -> (0, cos, sin), up -> (0, -sin, cos)
        c, s_ = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        return [1, 0, 0, 0, c, s_, 0, -s_, c]

    ident = [1, 0, 0, 0, 1, 0, 0, 0, 1]
    fw = cam_forward(cam_str(ident))
    expect("identity forward (0,1,0)", fw is not None and all(abs(a - b) < 1e-6 for a, b in zip(fw, (0, 1, 0))), fw)
    yp = cam_yaw_pitch(cam_str(ident))
    expect("identity yaw 90, fz 0", abs(yp[0] - 90) < 1e-6 and abs(yp[1]) < 1e-6, yp)
    yp = cam_yaw_pitch(cam_str(rz(30)))
    expect("30deg right -> yaw 60", abs(yp[0] - 60) < 0.01, yp)
    expect("30deg right: delta = -30", abs(wrap180(yp[0] - 90) + 30) < 0.01)
    yp = cam_yaw_pitch(cam_str(rz(120)))
    expect("120deg right -> yaw -30 (wraps; delta -120)", abs(yp[0] + 30) < 0.01 and abs(wrap180(yp[0] - 90) + 120) < 0.01, yp)
    yp = cam_yaw_pitch(cam_str(rx(20)))
    expect("pitch 20 up -> fz sin(20)", abs(yp[1] - math.sin(math.radians(20))) < 1e-3 and abs(yp[0] - 90) < 0.01, yp)
    # in-game sample (aim work): engine pick dir == row 1 = (-0.7071,-0.7071,0); column 1 = (0.7071,-0.7071,0)
    game = [-0.7071, 0.7071, 0, -0.7071, -0.7071, 0, 0, 0, 1]  # heading -135: right = (-0.7071, 0.7071, 0)
    fw = cam_forward(cam_str(game))
    expect("forward = row 1 (engine pick dir), not column 1", fw is not None and
           all(abs(a - b) < 1e-3 for a, b in zip(fw, (-0.7071, -0.7071, 0))), fw)
    yp = cam_yaw_pitch(cam_str(game))
    expect("in-game sample -> yaw atan2(fy,fx) = -135", abs(yp[0] + 135) < 0.01, yp)
    expect("worldRot=[...] placeholder -> None", cam_yaw_pitch("worldRot=[...] x") is None)
    expect("no worldRot -> None", cam_yaw_pitch("no PlayerCamera") is None)
    expect("worldPos parse", parse_cam_pos(cam_str(ident)) == (1.0, 2.0, 3.0), parse_cam_pos(cam_str(ident)))
    expect("worldPos missing -> None", parse_cam_pos("nothing") is None)
    expect("pad polls regex", re.search(r"polls so far (\d+)", "pad override for 0 ms (polls so far 1234)").group(1) == "1234")

    print(f"head pose metric on {SHOTS}")
    names = ["base", "yaw30", "yaw30_again", "pitch20", "up30cm", "right30cm", "roll15"]
    have = [n for n in names if os.path.exists(os.path.join(SHOTS, n + ".bmp"))]
    if "base" not in have:
        expect("captures present", False, "base.bmp missing")
    else:
        t0 = time.time()
        greys = {n: grey_small(os.path.join(SHOTS, n + ".bmp")) for n in have}
        t1 = time.time()
        mean, std = image_stats(greys["base"])
        shifts = {}
        for n in have[1:]:
            shifts[n] = best_shift(greys["base"], greys[n])
            s = shifts[n]
            print(f"    {n:12s} dx={s['dx']:+3d} dy={s['dy']:+3d} res={s['res']:5.1f} (no-shift res {s['res0']:5.1f})"
                  f"{' EDGE' if s['edge'] else ''}")
        t2 = time.time()
        print(f"    base {len(greys['base'][0])}x{len(greys['base'])} mean {mean:.1f} std {std:.1f}; "
              f"load {t1 - t0:.1f}s, match {t2 - t1:.1f}s")
        ok, crit = evaluate_headpose(shifts, std)
        for c, o, v in crit:
            expect(f"criterion {c}", o, v)
        hf = implied_hfov(shifts, len(greys["base"][0]) * 8)
        expect("implied hFOV yaw vs pitch agree within 5 deg", len(hf) == 2 and abs(hf["yaw30"] - hf["pitch20"]) < 5, hf)
        print("    summary: " + headpose_summary(shifts, hf))
        self_shift = best_shift(greys["base"], greys["base"])
        expect("base vs itself -> 0,0 res 0", (self_shift["dx"], self_shift["dy"], self_shift["res"]) == (0, 0, 0))
        ok_neg, _ = evaluate_headpose({n: self_shift for n in shifts}, std)
        expect("all-zero shifts -> evaluate FAIL", not ok_neg)
        flat = [[50.0] * 160 for _ in range(90)]
        ok_flat, _ = evaluate_headpose(shifts, image_stats(flat)[1])
        expect("flat base -> evaluate FAIL", not ok_flat)
        # synthetic shift: move base content 7 left / 5 down -> expect dx=-7 dy=+5
        H, W = len(greys["base"]), len(greys["base"][0])
        syn = [[greys["base"][min(max(y - 5, 0), H - 1)][min(max(x + 7, 0), W - 1)] for x in range(W)] for y in range(H)]
        s = best_shift(greys["base"], syn)
        expect("synthetic shift dx=-7 dy=+5 (odd -> refinement)", (s["dx"], s["dy"]) == (-7, 5), (s["dx"], s["dy"]))

        fd, tmp = tempfile.mkstemp(suffix=".png")
        os.close(fd)
        try:
            bmp_to_png(os.path.join(SHOTS, "base.bmp"), tmp)
            with open(tmp, "rb") as fh:
                d = fh.read()
            w, h = struct.unpack(">II", d[16:24])
            idat = d[d.index(b"IDAT") + 4: d.index(b"IEND") - 8]
            expect("png header/size", d[:8] == b"\x89PNG\r\n\x1a\n" and (w, h) == (1280, 720) and
                   len(zlib.decompress(idat)) == h * (1 + 3 * w), f"{w}x{h}, {len(d) // 1024} KiB")
        finally:
            os.remove(tmp)
        expect("bmp_complete on finished capture", bmp_complete(os.path.join(SHOTS, "base.bmp")))
        expect("bmp_complete on missing file", not bmp_complete(os.path.join(SHOTS, "nope.bmp")))
    print("selftest: " + ("PASS" if not fails else f"FAIL ({len(fails)}): " + "; ".join(fails)))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-xr", action="store_true", help="plugin gets a nonexistent OpenXR runtime json (flat)")
    ap.add_argument("--runtime-json", help="force this OpenXR runtime json for the launch (e.g. the simulator)")
    ap.add_argument("--only", help="comma list of: " + ",".join(n for n, _, _ in CHECKS))
    ap.add_argument("--attach", action="store_true", help="use an already-running game (never quit it)")
    ap.add_argument("--cell", default="QASmoke", help="cell to coc into (default QASmoke)")
    ap.add_argument("--load", help="load this save instead of coc --cell (e.g. vrtest_sanctuary; preferred over QASmoke)")
    ap.add_argument("--keep-bmp", action="store_true", help="keep BMP captures next to the PNGs")
    ap.add_argument("--vremu", action="store_true",
                    help="SteamVR + vremu emulator: implies --runtime-json <steamxr_win64.json>, enables vr_* checks")
    ap.add_argument("--selftest", action="store_true", help="offline parser/metric tests, no game")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    if args.vremu:
        if args.no_xr:
            raise SystemExit("--vremu and --no-xr are exclusive")
        if not args.runtime_json:
            args.runtime_json = STEAMVR_RUNTIME_JSON
    if args.no_xr and args.runtime_json:
        raise SystemExit("--no-xr and --runtime-json are exclusive")
    rows, report, outdir, t_start = run_suite(args)
    print_table(rows, report.get("wall_s", time.time() - t_start))
    print(f"report: {os.path.join(outdir, 'report.json')}")
    sys.exit(1 if any(r["status"] == "FAIL" for r in rows) else 0)


if __name__ == "__main__":
    main()
