#pragma once
// Dlss.h - per-eye DLSS (DLAA) and DLSS neural rendering (NGX feature 18) inside fo4flatvr (20 Nov, DLSS agent;
// analysis/dlssnr-vr.md section 5, analysis/dlss-in-fo4flatvr.md).
//
// Phase 1: NGX on the game's own D3D11 device (no ReShade, no loader, no Streamline). One DLSS feature per eye (or one
// shared feature over both eyes, [DLSS] bPerEye=0) reading sub-rects of the double-wide image; it is evaluated in place of
// the engine's main-view TAA draw (StereoDraw.cpp TAA branch, after RunMvFix) and the engine's TAA is skipped.
// Phase 2: the neural pass per eye on a private D3D12 device (NR has no D3D11 path): shared textures + one shared fence,
// GPU-side waits only. The SDR composition is RenoDX's (work/mod-research/renodx).
// Render thread only (the draw hook and the Present hook are both on the game's render thread).

#include <atomic>
#include <string>

struct ID3D11DeviceContext;

namespace fo4flatvr::dlss
{
    // Registers the [DLSS] / [NeuralRendering] settings reload (cfgstore::Subscribe) and reads them. Plugin load.
    void Install();

    // StereoCuts / the camera hook: DLSS replaces the engine's TAA, so the TAA effect has to run (the [Stereo] bCutTaa cut is
    // lifted, BSGraphics::State+0xAC forced to 1, FXAA off) while DLSS is on and usable in a stereo mode.
    bool WantsTaa();
    // With WantsTaa: whether the engine's sub-pixel jitter stays on ([DLSS] bJitter). Off = the camera hook builds the camera
    // without jitter (SetCameraData's jitter argument), TAA itself stays active.
    bool WantsJitter();

    // The main view's TAA resolve of a stereo frame, after the per-eye motion-vector pass. The draw's PS SRVs are bound
    // (t0 colour, t2 motion vectors, t3 depth, t4 mask), RT0 is the TAA output. True = DLSS/NR wrote the frame into RT0 and the
    // engine's TAA draw must be skipped.
    struct TaaDraw
    {
        ID3D11DeviceContext* ctx{ nullptr };
        double               seamPad{ 0.0 };  // [Stereo] seam guard per eye region (px), RenderSize plan
        double               fullWidth{ 0.0 };  // double-wide render width
        double               jitterX{ 0.0 }, jitterY{ 0.0 };  // engine TAA jitter of the frame, clip-space translation
        bool                 mvFixed{ false };  // the per-eye motion-vector pass ran this frame
        int                  mvSign{ 0 };
    };
    bool OnTaaDraw(const TaaDraw& a_draw);
    // NR without DLSS ([DLSS] bEnabled=0, [NeuralRendering] bEnabled=1): the engine's TAA draws, then the neural pass runs on
    // its output. Called from StereoDraw's AfterDraw for the TAA draw that OnTaaDraw left to the engine.
    void OnAfterTaaDraw(ID3D11DeviceContext* a_ctx);

    // Present hook, render thread: frame counter, GPU timer read-back, the NR toggle key, the periodic cost line.
    void OnPresent();

    // StereoDraw's context hooks pass straight through while we drive the immediate context (NGX evaluates, our compute
    // passes): no shadow-state tracking, no stereo conversion.
    inline std::atomic<int> g_bypass{ 0 };
    inline bool Bypass() { return g_bypass.load(std::memory_order_relaxed) != 0; }

    // Pipe "dlss [status|reset|rebuild|nr retry|nrstereo|census]".
    std::string Command(const std::string& a_arg);
    std::string Status();
}
