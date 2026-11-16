// StereoDebugPosPS.hlsl - debug-mode pixel shader for the bytecode twins (StereoVS.pack). It reads only
// SV_Position, so it links to any vertex shader's outputs. Deterministic per pixel depth, identical in both
// halves at IPD 0 (the halves-identity measurement E3); writes every GBuffer target (6 MRTs, measured).
//   R = depth, G = depth^2, B = 0.5   (z = NDC depth of the eye's clip position). Smooth in z on
//   purpose: a frac(z * 65536) code amplified 1-ulp interpolation noise between the halves into full-range jumps.

struct PSOut
{
    float4 t0 : SV_Target0;
    float4 t1 : SV_Target1;
    float4 t2 : SV_Target2;
    float4 t3 : SV_Target3;
    float4 t4 : SV_Target4;
    float4 t5 : SV_Target5;
};

PSOut main(float4 pos : SV_Position)
{
    const float z = pos.z;
    const float4 c = float4(z, z * z, 0.5f, 1.0f);
    PSOut o;
    o.t0 = c;
    o.t1 = c.gbra;
    o.t2 = c.brga;
    o.t3 = c.rbga;
    o.t4 = c.grba;
    o.t5 = c.bgra;
    return o;
}
