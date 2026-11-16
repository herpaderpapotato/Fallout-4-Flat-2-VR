#pragma once
// StereoMath.h - Per-eye matrices for single-pass stereo, in double.
//
// Engine conventions (verified in 1.11.240, BSGraphics::State::CalculateCameraViewProj 182C130 and the
// g008 VS bytecode): row vectors (p' = p * M), matrices stored row-major (XMMATRIX), view space is
// left-handed x right / y up / z forward, rotation only (rendering is camera-relative: positions arrive
// as World - PosAdjust), D3D depth 0..1, not reversed. NiFrustum tangents: l < 0 < r, b < 0 < t.
//
//   P(l,r,t,b,n,f) = | 2/(r-l)        0              0             0 |
//                    | 0              2/(t-b)        0             0 |
//                    | -(l+r)/(r-l)   -(t+b)/(t-b)   f/(f-n)       1 |
//                    | 0              0              -n f/(f-n)    0 |
//
// Per eye (e = 0 left, 1 right):
//   E_e  = T(-o_e) * R_e         eye-from-head in view space (o_e eye offset, R_e columns = eye axes)
//   VP_e = V_mono * E_e * P_e    category A: camera-relative world -> eye clip (uploaded to b5)
//   D_e  = inv(VP_mono) * VP_e   category B: mono clip -> eye clip (premultiplied-WVP shaders)
// tools/de_math_check.py: float32 GPU evaluation of both paths stays < 2e-3 px and ~2.4e-7 depth from
// the double-precision truth for near 1..15, far 3.5e5..1e6, IPD 4.5..70 units, cant 0..10 deg.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace fo4flatvr::stereo::math
{
    struct M4
    {
        double m[4][4]{};

        static M4 Identity()
        {
            M4 r;
            for (int i = 0; i < 4; ++i) {
                r.m[i][i] = 1.0;
            }
            return r;
        }

        // From the engine's row-major float matrix (e.g. Context+0x2310 ViewProj).
        static M4 FromFloats(const float* a_f)
        {
            M4 r;
            for (int i = 0; i < 16; ++i) {
                r.m[i / 4][i % 4] = a_f[i];
            }
            return r;
        }

        void ToFloats(float* a_out) const
        {
            for (int i = 0; i < 16; ++i) {
                a_out[i] = static_cast<float>(m[i / 4][i % 4]);
            }
        }
    };

    inline M4 Mul(const M4& a, const M4& b)
    {
        M4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                double s = 0.0;
                for (int k = 0; k < 4; ++k) {
                    s += a.m[i][k] * b.m[k][j];
                }
                r.m[i][j] = s;
            }
        }
        return r;
    }

    // Gauss-Jordan with partial pivoting. Returns false if singular (ortho/degenerate camera: caller
    // falls back to mono for that camera).
    inline bool Inverse(const M4& a_in, M4& a_out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                a[i][j] = a_in.m[i][j];
                a[i][j + 4] = (i == j) ? 1.0 : 0.0;
            }
        }
        for (int c = 0; c < 4; ++c) {
            int p = c;
            for (int r = c + 1; r < 4; ++r) {
                if (std::abs(a[r][c]) > std::abs(a[p][c])) {
                    p = r;
                }
            }
            if (std::abs(a[p][c]) < 1e-300) {
                return false;
            }
            if (p != c) {
                for (int j = 0; j < 8; ++j) {
                    const double t = a[c][j];
                    a[c][j] = a[p][j];
                    a[p][j] = t;
                }
            }
            const double inv = 1.0 / a[c][c];
            for (int j = 0; j < 8; ++j) {
                a[c][j] *= inv;
            }
            for (int r = 0; r < 4; ++r) {
                if (r != c && a[r][c] != 0.0) {
                    const double f = a[r][c];
                    for (int j = 0; j < 8; ++j) {
                        a[r][j] -= f * a[c][j];
                    }
                }
            }
        }
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                a_out.m[i][j] = a[i][j + 4];
            }
        }
        return true;
    }

    inline double MaxAbsDiff(const M4& a, const M4& b)
    {
        double d = 0.0;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                d = (std::max)(d, std::abs(a.m[i][j] - b.m[i][j]));
            }
        }
        return d;
    }

    // The engine's perspective projection (CalculateCameraViewProj, non-ortho branch).
    inline M4 Projection(double l, double r, double t, double b, double n, double f)
    {
        M4 p;
        p.m[0][0] = 2.0 / (r - l);
        p.m[1][1] = 2.0 / (t - b);
        p.m[2][0] = -(l + r) / (r - l);
        p.m[2][1] = -(t + b) / (t - b);
        p.m[2][2] = f / (f - n);
        p.m[2][3] = 1.0;
        p.m[3][2] = -n * f / (f - n);
        return p;
    }

    // Eye projections use a far plane g_eyeFarScale x the mono far plane. The engine pins sky/cloud geometry (and
    // fullscreen far-plane passes) to z = w in MONO clip space; re-expressed in an eye whose orientation differs even
    // slightly from the mono camera (HMD eye canting, pose noise) or through float rounding of the per-eye matrices,
    // part of that geometry lands at z > w and the far plane clips it along a straight line: hard-edged holes in the
    // clouds that move with the head (sky flicker), and a missing triangle of far-plane-drawn lighting. With the eye
    // far plane further out those points stay inside (z/w = 1 - ~n/(2f)). Depth of near geometry is unchanged to
    // ~n/f (4e-5). [Stereo] fEyeFarScale / pipe "stereofar <x>" (1 = old behaviour).
    inline std::atomic<double> g_eyeFarScale{ 2.0 };

    // One eye relative to the head (= the mono camera), already in the engine's view space and units.
    struct EyeRel
    {
        double pos[3]{};                                              // eye origin in head view space (game units)
        double rot[3][3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };    // columns = eye x/y/z axes in head space
        double tanL{ -1 }, tanR{ 1 }, tanT{ 1 }, tanB{ -1 };          // eye frustum tangents (l<0<r, b<0<t)
    };

    // E_e = T(-o) * R  (row vectors: p_eye = (p_head - o) * R).
    inline M4 EyeFromHead(const EyeRel& e)
    {
        M4 t = M4::Identity();
        t.m[3][0] = -e.pos[0];
        t.m[3][1] = -e.pos[1];
        t.m[3][2] = -e.pos[2];
        M4 r = M4::Identity();
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.m[i][j] = e.rot[i][j];
            }
        }
        return Mul(t, r);
    }

    // OpenXR (right-handed, -Z forward, metres) relative eye pose -> engine view space (left-handed,
    // +Z forward, game units). S = diag(1,1,-1):  o_game = S o_xr * upm,  R_game = S R_xr S.
    // a_q = eye orientation relative to the head (x,y,z,w), a_t = eye position relative to the head.
    inline void FromOpenXr(const double a_q[4], const double a_t[3], double a_unitsPerMeter, EyeRel& a_out)
    {
        const double x = a_q[0], y = a_q[1], z = a_q[2], w = a_q[3];
        const double R[3][3] = {
            { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
        };
        const double s[3] = { 1, 1, -1 };
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                a_out.rot[i][j] = s[i] * R[i][j] * s[j];
            }
            a_out.pos[i] = s[i] * a_t[i] * a_unitsPerMeter;
        }
    }

    // Relative pose eye-in-head from two poses in the same reference space: rel = inv(head) * eye.
    inline void RelativePose(const double hq[4], const double hp[3], const double eq[4], const double ep[3],
        double a_outQ[4], double a_outP[3])
    {
        // q_rel = conj(hq) * eq
        const double cx = -hq[0], cy = -hq[1], cz = -hq[2], cw = hq[3];
        a_outQ[0] = cw * eq[0] + cx * eq[3] + cy * eq[2] - cz * eq[1];
        a_outQ[1] = cw * eq[1] - cx * eq[2] + cy * eq[3] + cz * eq[0];
        a_outQ[2] = cw * eq[2] + cx * eq[1] - cy * eq[0] + cz * eq[3];
        a_outQ[3] = cw * eq[3] - cx * eq[0] - cy * eq[1] - cz * eq[2];
        // p_rel = conj(hq) * (ep - hp) * hq  (rotate the world delta into head space)
        const double d[3] = { ep[0] - hp[0], ep[1] - hp[1], ep[2] - hp[2] };
        const double x = cx, y = cy, z = cz, w = cw;  // rotation by conj(hq)
        const double R[3][3] = {
            { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
        };
        for (int i = 0; i < 3; ++i) {
            a_outP[i] = R[i][0] * d[0] + R[i][1] * d[1] + R[i][2] * d[2];
        }
    }

    struct EyeMatrices
    {
        M4   viewProj[2];       // VP_e
        M4   fromMonoClip[2];   // D_e
        bool valid{ false };
    };

    // a_view / a_viewProj: the engine's matrices for the camera being applied (Context+0x2290 / +0x2310),
    // a_near / a_far: that camera's NiFrustum near/far (camera+0x160+0x10/+0x14).
    // TAA jitter for one eye, applied in the eye's clip space before the double-wide squeeze: the mono camera's
    // jitter (a_jx, a_jy) is a translation of its double-wide clip space (clip.xy += j * w); the squeeze halves x,
    // so the same pixel offset needs 2 * a_jx before it.
    inline M4 EyeJitter(double a_jx, double a_jy)
    {
        M4 j = M4::Identity();
        j.m[3][0] = 2.0 * a_jx;
        j.m[3][1] = a_jy;
        return j;
    }

    inline EyeMatrices BuildEyes(const M4& a_view, const M4& a_viewProj, double a_near, double a_far, const EyeRel a_eyes[2],
        double a_jx = 0.0, double a_jy = 0.0)
    {
        EyeMatrices out;
        M4 invVP;
        if (!Inverse(a_viewProj, invVP)) {
            return out;
        }
        for (int e = 0; e < 2; ++e) {
            const EyeRel& eye = a_eyes[e];
            const M4 P = Mul(Projection(eye.tanL, eye.tanR, eye.tanT, eye.tanB, a_near, a_far * g_eyeFarScale.load(std::memory_order_relaxed)),
                EyeJitter(a_jx, a_jy));
            out.viewProj[e] = Mul(Mul(a_view, EyeFromHead(eye)), P);
            out.fromMonoClip[e] = Mul(invVP, out.viewProj[e]);
        }
        out.valid = true;
        return out;
    }
}
