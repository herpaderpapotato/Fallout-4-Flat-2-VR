"""Phase 1 head-rotation check through the command pipe. Run with the game in-world.

Captures: base (tracking off), yaw+30, yaw+30 again 3 s later (accumulation check), pitch+20,
roll+15. Writes BMP+PNG to work/shots/headpose/ and prints simple image metrics so an agent can
judge direction without looking: horizontal/vertical shift of the image vs base (phase-free
block matching on a downscaled grey image).
"""
import os
import struct
import sys
import time
import zlib

PIPE = r"\\.\pipe\fo4flatvr"
OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "work", "shots", "headpose"))


class Pipe:
    def __init__(self):
        deadline = time.time() + 10
        while True:
            try:
                self.f = open(PIPE, "r+b", buffering=0)
                return
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)

    def cmd(self, c):
        self.f.write((c + "\n").encode())
        r = b""
        while not r.endswith(b"\n"):
            r += self.f.read(4096)
        r = r.decode().strip()
        print(f"> {c}\n  {r}")
        return r


def read_bmp(p):
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    top = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top else h - 1 - y
        r = d[off + sy * w * 4: off + (sy + 1) * w * 4]
        rows.append(r)
    return w, h, rows


def to_png(p, dst):
    w, h, rows = read_bmp(p)
    raw = []
    for r in rows:
        px = bytearray(w * 3)
        px[0::3] = r[2::4]
        px[1::3] = r[1::4]
        px[2::3] = r[0::4]
        raw.append(b"\0" + bytes(px))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(b"".join(raw), 6)) + chunk(b"IEND", b""))


def grey_small(p, step=8):
    w, h, rows = read_bmp(p)
    g = []
    for y in range(0, h, step):
        r = rows[y]
        g.append([(r[x * 4] + r[x * 4 + 1] + r[x * 4 + 2]) / 3 for x in range(0, w, step)])
    return g


def best_shift(a, b, maxs=40):
    """Shift (dx, dy) in downscaled pixels minimising mean abs diff of b vs a, centre region."""
    H, W = len(a), len(a[0])
    best = None
    for dy in range(-maxs // 2, maxs // 2 + 1, 2):
        for dx in range(-maxs, maxs + 1, 2):
            s = n = 0
            for y in range(H // 4, 3 * H // 4, 2):
                yy = y + dy
                if not 0 <= yy < H:
                    continue
                for x in range(W // 4, 3 * W // 4, 2):
                    xx = x + dx
                    if 0 <= xx < W:
                        s += abs(a[y][x] - b[yy][xx])
                        n += 1
            if n and (best is None or s / n < best[0]):
                best = (s / n, dx, dy)
    return best


def main():
    os.makedirs(OUT, exist_ok=True)
    p = Pipe()
    p.cmd("status")
    shots = [("base", ["track off", "pose off"]),
             ("yaw30", ["track on", "pose 30 0 0"]),
             ("yaw30_again", []),
             ("pitch20", ["pose 0 20 0"]),
             ("roll15", ["pose 0 0 15"]),
             ("up30cm", ["pose off", "posepos 0 0 30"]),
             ("right30cm", ["pose off", "posepos 30 0 0"])]
    for name, cmds in shots:
        for c in cmds:
            p.cmd(c)
        time.sleep(3.0 if name == "yaw30_again" else 1.0)
        bmp = os.path.join(OUT, name + ".bmp").replace("\\", "/")
        p.cmd("capture " + bmp)
        time.sleep(1.0)
        to_png(bmp, bmp[:-4] + ".png")
    p.cmd("pose off")
    p.cmd("track off")

    base = grey_small(os.path.join(OUT, "base.bmp"))
    for name, _ in shots[1:]:
        m = best_shift(base, grey_small(os.path.join(OUT, name + ".bmp")))
        print(f"{name:12s} best match of base content at dx={m[1]:+d} dy={m[2]:+d} (x8 px), residual {m[0]:.1f}")


if __name__ == "__main__":
    main()
