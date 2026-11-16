// StereoCommon.hlsli - PROTOTYPE (unbuilt, stereo PoC). Shared by StereoVS.hlsl / StereoDebugPS.hlsl.
//
// Single-pass instanced stereo into a double-wide target, the Fallout 4 VR idiom (measured in
// analysis/stereo-single-pass-evidence.md 2.2): every converted draw is issued with 2x the instance
// count, eye = SV_InstanceID & 1, clip-space x is squeezed into the eye's half of the target and
// SV_ClipDistance0 cuts triangles at the seam. One viewport covers both halves.
//
// Matrix convention: the engine's (row vectors, p' = mul(p, M), left-handed view space, D3D depth),
// so the CPU uploads its double-precision results as plain row-major float4x4 (no transpose).

#ifndef STEREO_COMMON_HLSLI
#define STEREO_COMMON_HLSLI

// b5: unused by every flat 1.11.240 shader (evidence doc 1: VS uses 0,1,2,9-13; PS 0,1,2,12,13).
cbuffer StereoCB : register(b5)
{
    // Category A shaders (position = ViewProj(cb12[8..11]) * camera-relative world):
    // camera-relative world -> eye clip.  VP_e = V_mono * E_e * P_e  (E_e = eye-from-head, P_e = eye frustum).
    row_major float4x4 gEyeViewProj[2];
    // Category B shaders (CPU-premultiplied WorldViewProj in cb2[0..3]): mono clip -> eye clip.
    // D_e = inverse(VP_mono) * VP_e, built in double precision (tools/de_math_check.py: < 2e-3 px).
    row_major float4x4 gEyeFromMonoClip[2];
    // x: 1 = squeeze into the eye's half + seam clip (stereo), 0 = full-width output (identity tests)
    // y: debug flags (bit 0: tint eye 1 in the debug PS)
    // z: eye half width in pixels (debug PS), w: unused
    float4 gStereoParams;
};

static const float kEyeSign[2] = { -1.0f, 1.0f };  // eye 0 = left half, eye 1 = right half

struct StereoClip
{
    float4 pos;   // SV_Position
    float  clip;  // SV_ClipDistance0
};

// Squeeze x into [-1,0] (eye 0) or [0,1] (eye 1):  x' = 0.5 x + sign * 0.5 w.
// Seam clip:  eye 0 keeps x <= w (its own right edge), eye 1 keeps x >= -w.  ClipDistance = w + sign * x.
// The outer edges (x = -w for eye 0, x = +w for eye 1) land on the viewport edges and clip normally.
StereoClip StereoFinish(float4 clipEye, uint eye)
{
    StereoClip o;
    const float s = gStereoParams.x;
    o.pos = clipEye;
    o.pos.x = (s > 0.0f) ? (0.5f * clipEye.x + kEyeSign[eye] * 0.5f * clipEye.w) : clipEye.x;
    o.clip = (s > 0.0f) ? (clipEye.w + kEyeSign[eye] * clipEye.x) : 1.0f;
    return o;
}

#endif
