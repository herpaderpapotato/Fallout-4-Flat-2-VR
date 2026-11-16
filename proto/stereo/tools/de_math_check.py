"""Numerical check of the per-eye transforms used by the stereo PoC (PROTOTYPE TOOL, stdlib only).

Conventions are the engine's (verified in 1.11.240 BSGraphics::State::CalculateCameraViewProj 182C130):
row vectors (p' = p * M), left-handed view space x right / y up / z forward, D3D depth 0..1,
  P = [[2/(r-l), 0, 0, 0], [0, 2/(t-b), 0, 0], [-(l+r)/(r-l), -(t+b)/(t-b), f/(f-n), 1], [0, 0, -n f/(f-n), 0]]
with l,r,t,b the NiFrustum tangents (l<0<r, b<0<t).

Checks, for a mono camera and two eyes (IPD offset, optional cant, asymmetric FOV):
 1. category A path: clip_e = p_rel * (V * E_e * P_e)                        (exact by construction)
 2. category B path: clip_e = (p_rel * V * P_m) * D_e,  D_e = inv(V * P_m) * (V * E_e * P_e)
    evaluated in float32 like the GPU (WVP premultiplied in float32 by the CPU, D_e rounded to float32,
    dp4 in float32), compared with the double-precision truth in pixels and in depth.
usage: python -I de_math_check.py
"""
import math, random, struct


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def vec_mul(v, m, fp32=False):
    if fp32:
        out = []
        for j in range(4):
            acc = 0.0
            for k in range(4):  # dp4: sequential float32 multiply-add
                acc = f32(acc + f32(v[k] * m[k][j]))
            out.append(acc)
        return out
    return [sum(v[k] * m[k][j] for k in range(4)) for j in range(4)]


def inverse(m):
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(m)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        piv = a[c][c]
        a[c] = [x / piv for x in a[c]]
        for r in range(4):
            if r != c:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[4:] for row in a]


def proj(l, r, t, b, n, f):
    return [[2 / (r - l), 0, 0, 0], [0, 2 / (t - b), 0, 0],
            [-(l + r) / (r - l), -(t + b) / (t - b), f / (f - n), 1], [0, 0, -n * f / (f - n), 0]]


def rot_y(a):  # yaw about the view-space up axis (row-vector form)
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, -s, 0], [0, 1, 0, 0], [s, 0, c, 0], [0, 0, 0, 1]]


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0, 0], [0, c, s, 0], [0, -s, c, 0], [0, 0, 0, 1]]


def trans(x, y, z):
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [x, y, z, 1]]


def eye_matrix(offset_x, cant):
    # E_e = T(-o) * R ; R columns = eye axes in head space. cant: eye yaw relative to head.
    return mat_mul(trans(-offset_x, 0, 0), rot_y(-cant))


def run(near, far, ipd_units, cant_deg, W, seed=1):
    random.seed(seed)
    V = mat_mul(rot_y(0.7), rot_x(-0.2))  # rotation-only view (engine renders camera-relative)
    # union mono frustum, aspect 2W/H (double-wide), eyes asymmetric like a typical HMD
    eyes = [(-1.39, 0.84, 0.97, -1.19), (-0.84, 1.39, 0.97, -1.19)]  # l, r, t, b tangents
    Pm = proj(-2.4, 2.4, 1.2, -1.2, near, far)
    VPm = mat_mul(V, Pm)
    inv_vpm = inverse(VPm)
    worst = {}
    invV = inverse(V)
    wvp32 = [[f32(x) for x in row] for row in VPm]
    for e, (l, r, t, b) in enumerate(eyes):
        Pe = proj(l, r, t, b, near, far)
        Ee = eye_matrix((-0.5 if e == 0 else 0.5) * ipd_units, math.radians(cant_deg) * (1 if e == 0 else -1))
        VPe = mat_mul(mat_mul(V, Ee), Pe)
        De = mat_mul(inv_vpm, VPe)
        De32 = [[f32(x) for x in row] for row in De]
        vpe32 = [[f32(x) for x in row] for row in VPe]
        for _ in range(4000):
            # random world point in front of the eye, distance log-uniform near..far
            d = math.exp(random.uniform(math.log(near * 1.5), math.log(far * 0.9)))
            dirv = [random.uniform(-1, 1), random.uniform(-0.8, 0.8), 1.0]
            nrm = math.sqrt(sum(x * x for x in dirv))
            pv = [x / nrm * d for x in dirv]  # view-space point
            # back to camera-relative world: p_rel = pv * inv(V)
            prel = vec_mul(pv + [1.0], invV)
            prel[3] = 1.0
            truth = vec_mul(prel, VPe)
            if truth[3] <= near:
                continue
            # category B on the GPU: WVP premultiplied in float32 (the CPU writes a float32 matrix),
            # then clip_m in float32, then clip_e = clip_m * D_e in float32
            p32 = [f32(x) for x in prel]
            clip_m = vec_mul(p32, wvp32, fp32=True)
            clip_e = vec_mul(clip_m, De32, fp32=True)
            # category A on the GPU: VP_e in float32 directly
            clip_a = vec_mul(p32, vpe32, fp32=True)
            for name, c in (('B', clip_e), ('A', clip_a)):
                sx = (c[0] / c[3] - truth[0] / truth[3]) * 0.5 * W  # pixel error in an eye of width W
                dz = abs(c[2] / c[3] - truth[2] / truth[3])
                key = name
                worst.setdefault(key + '_px', 0.0)
                worst.setdefault(key + '_dz', 0.0)
                worst[key + '_px'] = max(worst[key + '_px'], abs(sx))
                worst[key + '_dz'] = max(worst[key + '_dz'], dz)
    return worst


if __name__ == '__main__':
    W = 2000  # pixels per eye
    print('worst-case errors vs double precision truth (eye width %d px):' % W)
    print('%-8s %-9s %-6s %-5s | %-12s %-12s | %-12s %-12s' % ('near', 'far', 'ipd', 'cant', 'A px', 'A dz', 'B px', 'B dz'))
    for near, far, ipd, cant in ((15, 353840, 4.48, 0), (5, 1e6, 4.48, 0), (15, 353840, 4.48, 5), (1, 1e6, 4.48, 10),
                                 (15, 353840, 70.0, 0)):
        w = run(near, far, ipd, cant, W)
        print('%-8g %-9g %-6g %-5g | %-12.3e %-12.3e | %-12.3e %-12.3e' % (
            near, far, ipd, cant, w['A_px'], w['A_dz'], w['B_px'], w['B_dz']))
