"""Where do the two halves of a double-wide capture differ? (stdlib; numeric only)

    python -I halves_map.py <cap.bmp> [cols rows] [--shift]

Prints a cols x rows grid over one half: per tile the share of pixels whose left/right max-channel
difference is <= 1, as a digit 0-9 ('#' = 100%). With --shift it also reports, per tile row, the
horizontal offset (-64..64 px, step 1) that best aligns right to left (a seam/offset bug shows as a
constant non-zero shift; mono-stretched content shows as a large, position-dependent one).
Also prints the share of exactly black pixels per half (RT0 never written).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stereo_metrics as sm  # noqa: E402


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    path = a[0]
    cols = int(a[1]) if len(a) > 1 else 24
    rows = int(a[2]) if len(a) > 2 else 12
    w, h, px = sm.read_bmp(path)
    half = w // 2
    tw, th = half // cols, h // rows
    black = [0, 0]
    n = 0
    print('%s: %dx%d, grid %dx%d over each half (digit = tenths of identical pixels, # = all)' % (os.path.basename(path), w, h, cols, rows))
    for ty in range(rows):
        line = ''
        for tx in range(cols):
            same = tot = 0
            for y in range(ty * th, (ty + 1) * th, 3):
                r = px[y]
                for x in range(tx * tw, (tx + 1) * tw, 3):
                    o1, o2 = x * 4, (x + half) * 4
                    d = max(abs(r[o1] - r[o2]), abs(r[o1 + 1] - r[o2 + 1]), abs(r[o1 + 2] - r[o2 + 2]))
                    same += d <= 1
                    tot += 1
            f = same / tot
            line += '#' if f >= 0.999 else str(min(9, int(f * 10)))
        print('  ' + line)
    for y in range(0, h, 4):
        r = px[y]
        for x in range(0, half, 4):
            n += 1
            black[0] += r[x * 4] == r[x * 4 + 1] == r[x * 4 + 2] == 0
            black[1] += r[(x + half) * 4] == r[(x + half) * 4 + 1] == r[(x + half) * 4 + 2] == 0
    print('black pixels: left %.1f%% right %.1f%%' % (100 * black[0] / n, 100 * black[1] / n))
    if '--shift' in sys.argv:
        for ty in range(rows):
            y0 = ty * th + th // 2
            best = None
            for s in range(-64, 65):
                e = 0
                for y in range(y0 - 4, y0 + 5, 2):
                    r = px[y]
                    for x in range(80, half - 80, 4):
                        o1, o2 = x * 4, (x + half + s) * 4
                        e += abs(r[o1] - r[o2]) + abs(r[o1 + 1] - r[o2 + 1]) + abs(r[o1 + 2] - r[o2 + 2])
                if best is None or e < best[0]:
                    best = (e, s)
            print('  row %2d (y %4d): best right-vs-left shift %+d px' % (ty, y0, best[1]))


if __name__ == '__main__':
    main()
