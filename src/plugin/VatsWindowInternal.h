#pragma once
// Shared state between VatsWindow.cpp (game side: hooks, engine render; CommonLibF4) and VatsWindowGpu.cpp (D3D11 side:
// GPU timer, tone map + compose at Present). Split because no translation unit in this plugin mixes RE/Fallout.h with
// the Windows D3D11 headers.

#include <atomic>
#include <cstdint>
#include <string>

namespace fo4flatvr::vatswin::detail
{
    struct Shared
    {
        std::atomic<float>         exposure{ 1.0f };      // [Menus] fVatsWindowExposure (tone map: 1 - exp(-c * e))
        std::atomic<int>           width{ 2048 };         // [Menus] iVatsWindowWidth: window texture width (height from the scene aspect)
        std::atomic<bool>          mask{ true };          // [Menus] bVatsWindowMask: VATS target highlight from our mask copy
        std::atomic<float>         maskStrength{ 0.25f }; // tint strength of the highlight ([Menus] fVatsWindowMaskStrength)
        std::atomic<void*>         sceneTex{ nullptr };   // ID3D11Texture2D* of our engine render target (HDR result)
        std::atomic<bool>          maskValid{ false };    // a mask copy for the last render exists
        std::atomic<bool>          albedoValid{ false };  // an albedo (GBuffer RT 0x1A) copy for the last render exists
        std::atomic<float>         ambient{ 0.6f };       // [Menus] fVatsWindowAmbient: fill light = albedo x this (0 = off)
        std::atomic<std::uint64_t> renders{ 0 };          // completed renders since install
        std::atomic<std::uint64_t> episodeRenders{ 0 };   // completed renders since the window became active
        std::atomic<std::uint64_t> composes{ 0 };
    };
    Shared& S();

    // Render thread (inside the R1 hook; raw D3D query calls only, no state changes)
    void GpuTimerBegin(void* a_ctx);
    void GpuTimerEnd(void* a_ctx);
    // Render thread: copy the engine's VATS mask target (RT 0xF, after our RenderMask call) into our own texture
    bool MaskCopy(void* a_ctx, void* a_srcTex);
    // Render thread: copy our render's albedo (GBuffer RT 0x1A) for the fill light in the tone map
    bool AlbedoCopy(void* a_ctx, void* a_srcTex);
    std::string GpuStatus();
    // Pipe: write the next composed window image as a BMP (and the raw-exposure scene alone as <path>_scene.bmp)
    void RequestCapture(const std::string& a_path);
}
