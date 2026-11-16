"""Headless image metrics for the stereo PoC (PROTOTYPE TOOL, stdlib only).

Inputs are BMPs written by the plugin ("stereocap" = render target 0 after the converted draws, with a
.json sidecar; or the Presenter "capture" of the back buffer) - both double-wide [left | right].

  python -I stereo_metrics.py halves  <cap.bmp>                 # E3: IPD 0 -> halves must be identical
  python -I stereo_metrics.py disparity <cap.bmp> [--json f]    # E4: debug-PS capture, IPD > 0, test eyes
  python -I stereo_metrics.py compare <a.bmp> <b.bmp>           # E2: identity VS vs engine VS (mono)
  python -I stereo_metrics.py selftest                          # synthetic image with known disparity
  python -I stereo_metrics.py halvesband <bb.bmp> [band] [tol]  # lighting round: halves at IPD 0 without a seam band
  python -I stereo_metrics.py litdisp <lit.bmp> <debug.bmp>     # lighting round E4: lit image, depth from debug capture

disparity: the debug PS writes R = log depth band, G = shading * cell hash, B = hash of the 64-unit world cell.
For every textured tile in the left half, the expected horizontal shift to the right half is
  d = ipdUnits * W / (2 * tanHalfH * z),  z = 16 * 2^(15 * R_linear)     (parallel symmetric test eyes)
(left eye at -ipd/2 sees a point further right than the right eye does: x_L - x_R = d > 0).
The tool measures d by SAD block matching on G and B and compares per depth band.
Exit code 0 = pass.
"""
import json, math, os, random, struct, sys


def read_bmp(path):
    data = open(path, 'rb').read()
    off, = struct.unpack_from('<I', data, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', data, 18)
    assert bpp == 32, 'expect 32-bit BMP'
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        src = y if top_down else h - 1 - y
        o = off + src * w * 4
        rows.append(data[o:o + w * 4])
    return w, h, rows  # BGRX bytes per row


def write_bmp(path, w, h, px):  # px[y][x] = (r,g,b)
    rowb = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[y][x]
            rowb += bytes((b, g, r, 255))
    hdr = struct.pack('<2sIHHI', b'BM', 54 + len(rowb), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 32, 0, len(rowb), 0, 0, 0, 0)
    open(path, 'wb').write(hdr + info + rowb)


def chan(rows, x, y, c):  # c: 0=R 1=G 2=B
    return rows[y][x * 4 + (2 - c)]


def srgb_to_linear(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def cmd_halves(path):
    w, h, rows = read_bmp(path)
    half = w // 2
    diff = same = n = 0
    for y in range(0, h, 2):
        r = rows[y]
        for x in range(0, half, 2):
            a = r[x * 4:x * 4 + 3]
            b = r[(x + half) * 4:(x + half) * 4 + 3]
            d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
            diff += d
            same += d <= 1
            n += 1
    mean, frac = diff / n, same / n
    ok = frac >= 0.999 and mean < 0.05
    print('halves: mean max-channel diff %.4f, identical(<=1) %.4f%% -> %s' % (mean, 100 * frac, 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def cmd_compare(a, b):
    wa, ha, ra = read_bmp(a)
    wb, hb, rb = read_bmp(b)
    assert (wa, ha) == (wb, hb), 'size mismatch'
    se = n = big = 0
    for y in range(ha):
        for i in range(0, wa * 4, 4):
            for c in range(3):
                d = ra[y][i + c] - rb[y][i + c]
                se += d * d
                big += abs(d) > 4
                n += 1
    mse = se / n
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    ok = psnr >= 45 and big / n < 1e-3
    print('compare: PSNR %.2f dB, channels off by >4: %.5f%% -> %s' % (psnr, 100 * big / n, 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def measure_disparity(w, h, rows, meta, tile=16, stride=32, seed=7):
    half = w // 2
    srgb = meta.get('srgb', False)
    ipd, tanh = meta['ipdUnits'], meta['tanHalfH']
    lin = srgb_to_linear if srgb else (lambda v: v / 255.0)
    results = []
    rnd = random.Random(seed)
    for ty in range(tile, h - 2 * tile, stride):
        for tx in range(tile, half - 2 * tile, stride):
            g = [chan(rows, tx + i, ty + j, 1) for j in range(tile) for i in range(tile)]
            bb = [chan(rows, tx + i, ty + j, 2) for j in range(tile) for i in range(tile)]
            rr = sorted(chan(rows, tx + i, ty + j, 0) for j in range(tile) for i in range(tile))
            if rr[len(rr) // 2] == 0 or max(rr) - min(rr) > 8:  # empty (sky/skipped) or a depth edge
                continue
            if max(bb) - min(bb) < 32 and max(g) - min(g) < 16:  # untextured: unmatchable
                continue
            z = 16.0 * 2 ** (15.0 * lin(rr[len(rr) // 2]))
            pred = ipd * half / (2.0 * tanh * z)
            full = rnd.random() < 0.1  # unguided search on 10% of tiles to keep the guided one honest
            lo, hi = (-8, int(pred * 2 + 16)) if full else (int(pred * 0.6) - 6, int(pred * 1.4) + 6)
            best, bestd = None, None
            for d in range(lo, hi + 1):
                x0 = tx + half - d
                if x0 < half or x0 + tile > w:
                    continue
                s = 0
                for j in range(tile):
                    row = rows[ty + j]
                    for i in range(tile):
                        o = (x0 + i) * 4
                        s += abs(row[o + 1] - g[j * tile + i]) + abs(row[o] - bb[j * tile + i])
                if best is None or s < best:
                    best, bestd = s, d
            if bestd is not None:
                results.append(dict(z=z, pred=pred, meas=bestd, full=full, sad=best / (tile * tile)))
    return results


def cmd_disparity(path, meta_path=None):
    w, h, rows = read_bmp(path)
    meta = json.load(open(meta_path or path + '.json'))
    res = measure_disparity(w, h, rows, meta)
    if not res:
        print('disparity: no usable tiles -> FAIL')
        return 1
    bands = {}
    for r in res:
        bands.setdefault(int(math.log2(r['z'])), []).append(r)
    ok = True
    print('band z(units)      tiles  pred(px)  meas(px)  err(px)')
    for k in sorted(bands):
        v = bands[k]
        pred = sorted(x['pred'] for x in v)[len(v) // 2]
        meas = sorted(x['meas'] for x in v)[len(v) // 2]
        err = meas - pred
        good = abs(err) <= max(1.5, 0.1 * pred)
        ok = ok and (good or len(v) < 5)
        print('  2^%-2d %-10.0f %5d  %8.2f  %8.2f  %+7.2f %s' % (k, 2 ** k, len(v), pred, meas, err, '' if good else '<-'))
    full = [r for r in res if r['full']]
    agree = sum(abs(r['meas'] - r['pred']) <= max(1.5, 0.15 * r['pred']) for r in full)
    sign = sum(r['meas'] >= -1 for r in res) / len(res)
    print('unguided tiles agreeing with prediction: %d/%d; non-negative disparity: %.1f%%' % (agree, len(full), 100 * sign))
    for r in full:
        if abs(r['meas'] - r['pred']) > max(1.5, 0.15 * r['pred']):
            print('  unguided disagreement: z %.0f pred %.2f meas %d sad %.1f' % (r['z'], r['pred'], r['meas'], r['sad']))
    ok = ok and sign > 0.98 and (not full or agree / len(full) > 0.8)
    print('disparity ->', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def cmd_selftest():
    # Synthetic double-wide image: planes at known depths with the debug-PS encoding, shifted by the
    # predicted disparity, plus a far background with ~0 disparity.
    W, H = 640, 400
    meta = dict(ipdUnits=70.0, tanHalfH=1.0, srgb=False)
    half = W // 2

    def scene(xh, y):  # returns z and world-locked texture coordinate for eye-centred column xh
        if 60 <= y < 220 and 50 <= xh < 210:
            return 300.0
        if 250 <= y < 380:
            return 1200.0
        return 20000.0

    px = [[(0, 0, 0)] * W for _ in range(H)]
    for eye in (0, 1):
        for y in range(H):
            for x in range(half):
                # find the scene column: iterate depths (z is piecewise constant, so solve per plane)
                for z in (300.0, 1200.0, 20000.0):
                    d = meta['ipdUnits'] * half / (2 * meta['tanHalfH'] * z)
                    xc = x - d / 2 if eye == 0 else x + d / 2  # centre-eye column
                    if scene(int(xc), y) == z:
                        break
                band = math.log2(z / 16.0) / 15.0
                hcell = (((int(xc // 6) * 73856093) ^ ((y // 6) * 19349663)) >> 8) & 255  # aperiodic, like the PS
                px[y][x + eye * half] = (int(band * 255 + 0.5), 80 + (120 * hcell) // 255, hcell)
    path = os.path.join(os.environ.get('TEMP', '.'), 'stereo_metrics_selftest.bmp')
    write_bmp(path, W, H, px)
    json.dump(meta, open(path + '.json', 'w'))
    print('selftest image', path)
    return cmd_disparity(path)


def cmd_halves_band(path, band=64, tol=1):
    """Lighting round: halves identity of a lit image at IPD 0, full and without a `band`-px column band at the
    seam and the outer edges (screen-space kernels - bloom, SAO blur, FXAA - legitimately see across the seam
    of a double-wide image). Pass = >= 99.9% of pixels within `tol` (max channel) outside the band."""
    w, h, rows = read_bmp(path)
    half = w // 2
    band = int(band * w / 2880.0 + 0.5)
    n = same = nb = sameb = n4 = n4b = 0
    diff = 0
    for y in range(h):
        r = rows[y]
        for x in range(half):
            a = r[x * 4:x * 4 + 3]
            b = r[(x + half) * 4:(x + half) * 4 + 3]
            d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
            n += 1
            diff += d
            same += d <= tol
            n4 += d <= 4
            if band <= x < half - band:
                nb += 1
                sameb += d <= tol
                n4b += d <= 4
    ok = nb and sameb / nb >= 0.999
    print('halves-band: full %.4f%% within %d (%.4f%% within 4), mean %.4f | without %d-px seam/edge band %.4f%% within %d '
          '(%.4f%% within 4) -> %s' % (100.0 * same / n, tol, 100.0 * n4 / n, diff / n, band, 100.0 * sameb / max(nb, 1), tol,
                                       100.0 * n4b / max(nb, 1), 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def cmd_litdisp(lit_path, depth_path, meta_path=None):
    """E4 on a lit image: depth per tile from a debug-mode capture of the same view and IPD (R = log depth band),
    texture from the lit back buffer; block-match left tiles in the right half like `disparity`."""
    w, h, lit = read_bmp(lit_path)
    w2, h2, dep = read_bmp(depth_path)
    if (w, h) != (w2, h2):
        print('litdisp: size %dx%d vs %dx%d -> FAIL' % (w, h, w2, h2))
        return 1
    meta = json.load(open(meta_path or depth_path + '.json'))
    rows = []
    for y in range(h):
        a, b = lit[y], dep[y]
        r = bytearray(a)
        r[2::4] = b[2::4]  # R (depth band) from the debug capture, G/B texture from the lit image
        rows.append(bytes(r))
    res = measure_disparity(w, h, rows, meta)
    if not res:
        print('litdisp: no usable tiles -> FAIL')
        return 1
    bands = {}
    for r in res:
        bands.setdefault(int(math.log2(r['z'])), []).append(r)
    ok = True
    print('band z(units)      tiles  pred(px)  meas(px)  err(px)')
    for k in sorted(bands):
        v = bands[k]
        pred = sorted(x['pred'] for x in v)[len(v) // 2]
        meas = sorted(x['meas'] for x in v)[len(v) // 2]
        good = abs(meas - pred) <= max(1.5, 0.1 * pred)
        ok = ok and (good or len(v) < 5)
        print('  2^%-2d %-10.0f %5d  %8.2f  %8.2f  %+7.2f %s' % (k, 2 ** k, len(v), pred, meas, meas - pred, '' if good else '<-'))
    # Scored: tiles with real parallax (pred >= 1 px). Far tiles (pred < 1 px: distant LOD, sky) are reported apart:
    # their median |measured| must stay small (sky at infinity, not at a few metres).
    near = [r for r in res if r['pred'] >= 1.0]
    far = [r for r in res if r['pred'] < 1.0]
    ok = True
    for k in sorted(bands):
        v = [r for r in bands[k] if r['pred'] >= 1.0]
        if len(v) >= 5:
            pred = sorted(x['pred'] for x in v)[len(v) // 2]
            meas = sorted(x['meas'] for x in v)[len(v) // 2]
            ok = ok and abs(meas - pred) <= max(1.5, 0.1 * pred)
    within = sum(abs(r['meas'] - r['pred']) <= max(1.5, 0.15 * r['pred']) for r in near) / max(len(near), 1)
    sign = sum(r['meas'] >= -1 for r in near) / max(len(near), 1)
    farmed = sorted(abs(r['meas']) for r in far)[len(far) // 2] if far else 0
    print('tiles with parallax (pred >= 1 px) %d: within max(1.5 px, 15%%) of prediction %.1f%%; non-negative %.1f%% | far tiles %d: '
          'median |measured| %.1f px' % (len(near), 100 * within, 100 * sign, len(far), farmed))
    # far tiles are informational: only the PoC's two HLSL families write the log-depth band in debug mode (the others,
    # e.g. ground/terrain, use a position-only PS), so a "far" R there is not a depth. The sky check is `skydisp`.
    ok = ok and len(near) >= 20 and sign > 0.95 and within > 0.8
    print('litdisp ->', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def cmd_skydisp(path, y0=0.06, y1=0.30, x0=0.14, x1=0.83, rng=12):
    """Sky at infinity: global horizontal shift of the sky band (rows y0..y1, columns x0..x1 of a half, fractions)
    between the halves of a lit back buffer at IPD > 0. Pass = |shift| <= 1 px (a sky dome drawn with the eye
    translation shows near-field parallax: 6 px at 64 mm before the fix)."""
    w, h, rows = read_bmp(path)
    half = w // 2
    ya, yb, xa, xb = int(y0 * h), int(y1 * h), int(x0 * half), int(x1 * half)
    res = []
    for s in range(-rng, rng + 1):
        e = n = 0
        for y in range(ya, yb, 2):
            r = rows[y]
            for x in range(xa, xb, 2):
                e += abs(r[x * 4 + 1] - r[(x + half - s) * 4 + 1]) + abs(r[x * 4] - r[(x + half - s) * 4])
                n += 1
        res.append((e / n, s))
    res.sort()
    best, second = res[0], res[1]
    ok = abs(best[1]) <= 1
    print('skydisp: best shift %+d px (SAD %.3f; next %+d px SAD %.3f) -> %s' % (best[1], best[0], second[1], second[0], 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def cmd_halves_masked(path, mask_path, band=64):
    """Halves identity of `path` on geometry pixels only: pixels that are non-zero in both halves of `mask_path` (a
    GBuffer RT0 capture of the same static view with the GBuffer cleared each frame). Stale history in pixels nothing
    draws (no sky in QASmoke; the engine's temporal AO keeps it) is excluded that way."""
    w, h, r = read_bmp(path)
    w2, h2, m = read_bmp(mask_path)
    if (w, h) != (w2, h2):
        print('halves-masked: size mismatch -> FAIL')
        return 1
    half = w // 2
    band = int(band * w / 2880.0 + 0.5)
    n = n1 = n4 = 0
    for y in range(h):
        a, g = r[y], m[y]
        for x in range(band, half - band):
            if not (any(g[x * 4:x * 4 + 3]) and any(g[(x + half) * 4:(x + half) * 4 + 3])):
                continue
            d = max(abs(a[x * 4 + c] - a[(x + half) * 4 + c]) for c in range(3))
            n += 1
            n1 += d <= 1
            n4 += d <= 4
    ok = n and n4 / n >= 0.99
    print('halves-masked: geometry px %d (%.1f%%): %.4f%% within 1, %.4f%% within 4 -> %s' % (
        n, 100.0 * n / (h * (half - 2 * band)), 100.0 * n1 / max(n, 1), 100.0 * n4 / max(n, 1), 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    if a[0] == 'halves':
        sys.exit(cmd_halves(a[1]))
    if a[0] == 'compare':
        sys.exit(cmd_compare(a[1], a[2]))
    if a[0] == 'disparity':
        sys.exit(cmd_disparity(a[1], a[3] if len(a) > 3 and a[2] == '--json' else None))
    if a[0] == 'selftest':
        sys.exit(cmd_selftest())
    if a[0] == 'halvesband':
        sys.exit(cmd_halves_band(a[1], int(a[2]) if len(a) > 2 else 64, int(a[3]) if len(a) > 3 else 1))
    if a[0] == 'skydisp':
        sys.exit(cmd_skydisp(a[1]))
    if a[0] == 'halvesmasked':
        sys.exit(cmd_halves_masked(a[1], a[2], int(a[3]) if len(a) > 3 else 64))
    if a[0] == 'litdisp':
        sys.exit(cmd_litdisp(a[1], a[2], a[3] if len(a) > 3 else None))
    print(__doc__)
    sys.exit(2)
