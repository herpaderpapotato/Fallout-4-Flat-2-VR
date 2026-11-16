#pragma once
// Render resolution independent of the window.
//
// Measured in 1.11.240: Renderer::Init (aid 2276814) creates the device/swapchain through the
// imported D3D11CreateDeviceAndSwapChain (IAT rva 2441758, called from aid 4492363), then
// CreateSwapChainRenderTarget (aid 2277019) reads the back buffer's size into the engine's
// screen-size globals (aid 2787932/2787933), from which the RenderTargetManager sizes everything.
// So forcing the swapchain buffer size is enough: the window stays small and DXGI scales the
// desktop mirror.

namespace fo4flatvr::rendersize
{
    // The render (swapchain) size actually used, from [Render] iWidth/iHeight (the double-wide total in stereo:
    // per-eye = iWidth/2 x iHeight), [Render] fRenderScale (multiplies the per-eye size, like FO4VR's
    // fRenderTargetSizeMultiplier) and, in single-pass stereo, guard bands: each eye's region is widened to a
    // multiple of a_align (>= eye + a_guard px), half of the extra pixels on each side of the eye, the same layout for
    // both eyes. The reduced-resolution post chains (1/2 .. 1/64) then put the seam on a texel boundary, both eyes'
    // images start on the same texel phase at every level, and blur kernels bleed into guard pixels instead of the
    // other eye. The guards are rendered with extra FOV and not submitted (stereo::SplitDoubleWide crops them).
    struct Plan
    {
        int width{ 0 }, height{ 0 };        // swapchain / back buffer (0 = the game decides)
        int eyeWidth{ 0 }, eyeHeight{ 0 };  // submitted per-eye image
        int seamPad{ 0 };                   // guard band per eye (stereo), px
        float scale{ 1.0f };
    };
    Plan Resolve(int a_width, int a_height, float a_scale, bool a_stereo, int a_align, int a_guard);

    // Patch the IAT entry before the renderer initialises (call from F4SEPlugin_Load).
    // a_width/a_height 0 = leave the game's own size.
    void Install(int a_width, int a_height);

    // Hook IDXGISwapChain::ResizeBuffers on the live swapchain so window resizes keep the size.
    void HookResize(void* a_swapChain);
}
