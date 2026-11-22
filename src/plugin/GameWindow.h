#pragma once
// Game window helpers (Win32 only; no RE types).

#include <string>

namespace fo4flatvr::window
{
    // Put the game window at the bottom of the z-order without activating anything, so the user's
    // other windows stay visible during unattended tests. The game keeps rendering (bAlwaysActive).
    void SendToBack(void* a_hwnd);

    // Force the game's main window to a normal (captioned, non-topmost) window with the given client
    // size, whatever Fallout4Prefs.ini says (iSize/bBorderless/bFull Screen). Patches the
    // CreateWindowExA IAT slot (1.11.240 rva 2440B48); call before the renderer initialises.
    // 0 = leave the game's own size.
    void InstallSmallWindow(int a_clientWidth, int a_clientHeight);

    // Engine render resolution independent of the window. Renderer::Init (aid 2276814) writes the
    // engine's screen-size globals (aids 2704649/2704650 and 2787932/2787933, rvas 3D87218..3D87224)
    // from its window properties, then calls CreateWindowExA, then creates the swapchain and the
    // render targets from those globals. The CreateWindowExA hook rewrites them to this size, so the
    // scene targets and the camera aspect follow the render size. 0 = leave.
    void SetEngineRenderSize(int a_width, int a_height);

    // 20 Nov (live settings): [Window] iWidth / iHeight while the game runs. The window is only a desktop mirror (the render
    // size is the swapchain's, kept by the ResizeBuffers hook; GetClientRect reports the render size), so its client area
    // can change at any time: SetWindowPos (async, no activation). The game window is the one CreateWindowExA made, else the
    // swapchain's output window (SetGameWindow). 0 = leave. Any thread.
    void SetGameWindow(void* a_hwnd);
    std::string ResizeClient(int a_clientWidth, int a_clientHeight);

}
