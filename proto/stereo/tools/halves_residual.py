"""Classify the pixels that differ between the halves of an IPD-0 capture (stdlib; numeric only).

    python -I halves_residual.py <cap.bmp>

For each covered pixel (non-black in either half) whose left/right max-channel difference is > 1:
 - 'edge'     : the right-half value matches (<= 1) a left-half neighbour within +-1 px, i.e. the two eyes
                rasterised the same surface with a sub-pixel offset (squeeze rounding at a high-contrast edge);
 - 'other'    : not explained by a 1-px offset (real content difference).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stereo_metrics as sm  # noqa: E402


def main():
    w, h, px = sm.read_bmp(sys.argv[1])
    half = w // 2
    cov = diff = edge = 0
    for y in range(1, h - 1):
        r = px[y]
        for x in range(1, half - 1):
            a = r[x * 4:x * 4 + 3]
            b = r[(x + half) * 4:(x + half) * 4 + 3]
            if not (any(a) or any(b)):
                continue
            cov += 1
            if max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) <= 1:
                continue
            diff += 1
            hit = False
            for dy in (-1, 0, 1):
                rr = px[y + dy]
                for dx in (-1, 0, 1):
                    c = rr[(x + dx) * 4:(x + dx) * 4 + 3]
                    if max(abs(c[0] - b[0]), abs(c[1] - b[1]), abs(c[2] - b[2])) <= 1:
                        hit = True
                        break
                if hit:
                    break
            edge += hit
    print('covered %d px, differing %d (%.3f%% of covered): 1-px-offset edges %d (%.1f%%), other %d (%.4f%% of covered)' % (
        cov, diff, 100 * diff / max(cov, 1), edge, 100 * edge / max(diff, 1), diff - edge, 100 * (diff - edge) / max(cov, 1)))


if __name__ == '__main__':
    main()
