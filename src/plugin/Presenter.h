#pragma once
// D3D11 side of the plugin: hooks the game's IDXGISwapChain::Present and drives xr::Runtime from it.

#include "plugin/Settings.h"

namespace fo4flatvr::present
{
    // a_swapChain / a_device / a_context are the game's (IDXGISwapChain*, ID3D11Device*,
    // ID3D11DeviceContext*). Idempotent.
    bool Install(void* a_swapChain, void* a_device, void* a_context, const Settings& a_settings);

    // Thread-safe. One-line status: XR state, frame counters, last error.
    std::string Status();

    // Thread-safe, live (not the 30-frame snapshot): "presents=<n> xrFrames=<n> t=<qpc seconds>".
    std::string Counters();

    // Thread-safe. Writes the next presented backbuffer to a_path as a 32-bit BMP.
    void RequestCapture(std::string a_path);
    // back buffer of a_frames consecutive presents (GPU copies, written after the last): <stem>_00.bmp ...
    void RequestCaptureBurst(int a_frames, std::string a_stem);
    // cropburst <n> <file> <x0> <x1> <y> <w> <h> <step>: two w x h rects of n consecutive frames, decimated, into one raw file
    void RequestCropBurst(int a_frames, std::string a_path, unsigned a_x0, unsigned a_x1, unsigned a_y, unsigned a_w, unsigned a_h,
        unsigned a_step);

    // Thread-safe. Stereo PoC (E7): at the next stereo submission write <stem>_bb.bmp (back buffer) and, after
    // EndFrame, <stem>_eye0.bmp / <stem>_eye1.bmp (the finished XR eye swapchain images of that frame).
    void RequestStereoEyeCapture(std::string a_stem);

    // Thread-safe. Applied on the next frame.
    void RequestRecenter();
    void RequestMode(int a_mode);  // 0 cinema quad, 1 mono world view, 2 alternate-eye stereo
}
