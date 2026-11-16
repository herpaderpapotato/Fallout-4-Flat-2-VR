// StereoMarkPS.hlsl - coverage markers (pipe "stereocov"): bound for one capture frame in place of the engine's
// GBuffer pixel shader. No inputs, so it links to any vertex shader. STEREO_MARK selects the colour:
//   0 green  = draw converted to stereo
//   1 red    = stereo camera applied, draw not converted
//   2 blue   = GBuffer draw while no stereo camera is applied
// Caveat: alpha-tested materials (foliage cards) lose their discard and cover their whole quads.
#ifndef STEREO_MARK
#define STEREO_MARK 0
#endif

float4 main() : SV_Target0
{
#if STEREO_MARK == 0
    return float4(0, 1, 0, 1);
#elif STEREO_MARK == 1
    return float4(1, 0, 0, 1);
#else
    return float4(0, 0, 1, 1);
#endif
}
