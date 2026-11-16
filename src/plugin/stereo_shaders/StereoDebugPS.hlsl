// StereoDebugPS.hlsl - PROTOTYPE (unbuilt, stereo PoC). Replaces the engine pixel shader for converted
// draws in DEBUG mode (STEREO_FAITHFUL=0 vertex shaders). Cheap and machine-readable rather than pretty:
//
//   R = depth band   : saturate(log2(viewZ / 16) / 15)   16 .. 524288 game units, log-spaced
//   G = shading      : Lambert against a fixed world light * (0.6 + 0.4 * cell hash)
//   B = cell hash    : integer hash of the 64-unit world cell - world-locked, identical in both eyes and
//                      APERIODIC (a parity checker makes block matching ambiguous by one period; the
//                      metrics self-test caught exactly that)
//
// tools/stereo_metrics.py decodes R (after undoing sRGB if the target is _SRGB) to predict the expected
// disparity of every tile and compares it with the measured left/right shift. SV_Target0 carries the code;
// targets 1..5 get swizzled copies so that all six GBuffer targets are deterministic per pixel.

#include "StereoCommon.hlsli"

struct PSIn
{
    float4 pos   : SV_POSITION;
    float3 nrmW  : TEXCOORD0;
    float3 world : TEXCOORD1;
    float  viewZ : TEXCOORD2;
    nointerpolation uint eye : TEXCOORD3;
};

struct PSOut
{
    float4 t0 : SV_Target0;
    float4 t1 : SV_Target1;  // the other GBuffer targets get deterministic copies (halves identity on all targets)
    float4 t2 : SV_Target2;
    float4 t3 : SV_Target3;
    float4 t4 : SV_Target4;
    float4 t5 : SV_Target5;
};

PSOut Fill(float4 c)
{
    PSOut o;
    o.t0 = c;
    o.t1 = c.gbra;
    o.t2 = c.brga;
    o.t3 = c.rbga;
    o.t4 = c.grba;
    o.t5 = c.bgra;
    return o;
}

PSOut main(PSIn i)
{
    const float band = saturate(log2(max(i.viewZ, 1.0f) / 16.0f) / 15.0f);
    const float3 n = normalize(i.nrmW + 1e-6f);
    const float lambert = 0.3f + 0.7f * saturate(dot(n, normalize(float3(0.4f, 0.3f, 0.85f))));
    const int3  cell = (int3)floor(i.world / 64.0f);
    const uint  h = ((uint)cell.x * 73856093u) ^ ((uint)cell.y * 19349663u) ^ ((uint)cell.z * 83492791u);
    const float hash = (float)((h >> 8) & 255u) / 255.0f;
    float3 c = float3(band, lambert * (0.6f + 0.4f * hash), hash);
    if (((uint)gStereoParams.y & 1u) && i.eye == 1u) {
        c.b = 1.0f - c.b;  // optional visual eye tint (off for measurements)
    }
    return Fill(float4(c, 1.0f));
}
