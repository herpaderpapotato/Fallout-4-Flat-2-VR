#pragma once
// StereoInternal.h - Shared between StereoCamera.cpp, StereoDraw.cpp and StereoCuts.cpp.

#include "StereoMath.h"

#include <atomic>
#include <string>

namespace fo4flatvr::stereo::internal
{
    // CPU mirror of cbuffer StereoCB (shaders/StereoCommon.hlsli), 272 bytes, row-major float4x4.
    struct StereoConstants
    {
        float eyeViewProj[2][16];
        float eyeFromMonoClip[2][16];
        float params[4];  // x squeeze on/off, y debug flags, z eye width px, w > 0: twins keep mono depth-clip status
    };
    static_assert(sizeof(StereoConstants) == 272);

    // Map(WRITE_DISCARD) the b5 buffer on the immediate context. Called from the SetCameraData hook,
    // i.e. on the thread that is about to issue the camera's draws.
    bool UploadConstants(const StereoConstants& a_k);
    // The same for sky draws (fxp g003: dome, clouds, sun, moon, stars): eye rotation + FOV but no eye translation.
    // The engine draws the sky as a small dome around the camera forced to the far plane (z = w); with the eye
    // offset it would get near-field parallax (measured: clouds 6 px at IPD 64 mm = ~7.7 m away).
    bool UploadSkyConstants(const StereoConstants& a_k);

    // Which stereo camera was applied last and b5 holds valid eye data for: 0 none (mono camera: shadow, cube,
    // local map, Interface3D ...), 1 the world camera, 2 the 1st-person camera.
    void SetCameraGate(int a_camera);

    // max |ViewProj - ViewProjUnjittered| of the last world-camera application (0 = no TAA jitter).
    void NoteJitter(double a_maxAbs);

    // Brackets the engine's SetCameraData (its SetPerFrameConstants maps the per-frame b12 buffer in between: the
    // Map hook identifies that buffer and keeps a CPU copy of what the engine wrote).
    void BeginCameraApply();
    void EndCameraApply();

    // Per-eye copies of the engine's per-frame buffer b12 for split draws (stereo-lighting-post.md M1): rows 4-11
    // and 16-27 hold the eye's double-wide projection P_dw,e = E_e * P_e * S_e (mono view space -> eye clip ->
    // squeezed into half e); everything else (View, posAdjust, fog, motion-vector rows) stays mono. a_identity:
    // both copies = the engine's buffer unchanged (mode 4: the split path must reproduce mono exactly).
    // a_frustum = the mono camera's NiFrustum l, r, t, b, n, f (camera+0x160).
    // a_jx/a_jy: the camera's TAA jitter (clip-space translation, see StereoMath EyeJitter); a_posAdjust: ctx+0x2220
    // (camera-relative origin; the per-eye motion-vector pass needs the previous frame's).
    bool BuildEyeFrameBuffers(int a_camera, const math::M4& a_view, const math::M4& a_proj, const math::M4& a_viewProj,
        const math::EyeRel a_eyes[2], const float a_frustum[6], bool a_identity, double a_jx, double a_jy, const float a_posAdjust[3]);

    // Jitter form check of the last world/1st-person application (J = VPunj^-1 VP; off-translation residue, jx, jy).
    void NoteJitterForm(double a_offTranslation, double a_jx, double a_jy);

    // StereoCamera.cpp: log the world camera + eyes for the next n stereo applications ("stereotrace <n>")
    void RequestTrace(int a_frames);

    // TAA cut active (the camera hook forces BSGraphics::State+0xAC = 0 while it is).
    bool TaaCutActive();

    // StereoCuts.cpp: apply/restore the cut list; called once per Present on the render thread.
    void UpdateCuts(bool a_stereoFrames);
    std::string CutsStatus();
    std::string ForceSetting(const std::string& a_tag, int a_value);

    // StereoDraw.cpp (StereoTiled.inl): per-eye tiled lighting can run (twins loaded, eye regions a multiple of 8 px, tile
    // tangent convention verified or not yet checked, "stereotiled" on). False: StereoCuts keeps the tiled-lighting cut.
    bool TiledPerEyeAvailable();

    // StereoCamera.cpp test aid: extra eye yaw/pitch (deg) relative to the head, pipe "stereocant <yaw> <pitch>"
    inline std::atomic<double> g_cantDeg[2]{ 0.0, 0.0 };
}
