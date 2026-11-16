#include <REX/LOG.h>

#include "plugin/GameWindow.h"

#include <Windows.h>
#undef ERROR

#include <cstdint>


namespace fo4flatvr::window
{
    namespace
    {
        constexpr std::uintptr_t kIatCreateWindowExA = 0x2440B48;
        using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
        CreateWindowExA_t g_createWindowExA = nullptr;
        int               g_clientW = 0, g_clientH = 0;
        bool              g_done = false;
        int               g_renderW = 0, g_renderH = 0;
        constexpr std::uintptr_t kScreenGlobals[4] = { 0x3D87218, 0x3D8721C, 0x3D87220, 0x3D87224 };  // w,h,w,h

        HWND WINAPI HkCreateWindowExA(DWORD a_ex, LPCSTR a_class, LPCSTR a_name, DWORD a_style, int a_x, int a_y,
            int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_inst, LPVOID a_param)
        {
            // The renderer creates exactly one top-level window (class = window name = the app name).
            if (!g_done && !a_parent && g_clientW > 0 && g_clientH > 0) {
                g_done = true;
                if (g_renderW > 0 && g_renderH > 0) {
                    auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
                    auto* g = reinterpret_cast<std::uint32_t*>(base + kScreenGlobals[0]);
                    REX::INFO("engine screen size globals {}x{} / {}x{} -> {}x{}", g[0], g[1], g[2], g[3], g_renderW, g_renderH);
                    // Sanity: both pairs must hold the same plausible size the renderer was given.
                    if (g[0] == g[2] && g[1] == g[3] && g[0] >= 320 && g[0] <= 16384 && g[1] >= 200 && g[1] <= 16384) {
                        g[0] = g[2] = static_cast<std::uint32_t>(g_renderW);
                        g[1] = g[3] = static_cast<std::uint32_t>(g_renderH);
                    } else {
                        REX::ERROR("engine screen size globals look wrong; not changing them");
                    }
                }
                const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
                RECT        rc{ 0, 0, g_clientW, g_clientH };
                ::AdjustWindowRectEx(&rc, style, FALSE, a_ex & ~WS_EX_TOPMOST);
                REX::INFO("game window: '{}' asked {}x{} style {:X}; forcing client {}x{} style {:X}",
                    a_name ? a_name : "?", a_w, a_h, a_style, g_clientW, g_clientH, style);
                return g_createWindowExA(a_ex & ~WS_EX_TOPMOST, a_class, a_name, style, a_x < 0 ? 0 : a_x,
                    a_y < 0 ? 0 : a_y, rc.right - rc.left, rc.bottom - rc.top, a_parent, a_menu, a_inst, a_param);
            }
            return g_createWindowExA(a_ex, a_class, a_name, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_inst, a_param);
        }
    }

    void InstallSmallWindow(int a_clientWidth, int a_clientHeight)
    {
        if (a_clientWidth <= 0 || a_clientHeight <= 0 || g_createWindowExA) {
            return;
        }
        auto**      slot = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr)) + kIatCreateWindowExA);
        const void* expected = reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "CreateWindowExA"));
        if (!expected || *slot != expected) {
            REX::ERROR("small window: IAT slot does not hold CreateWindowExA; not patching");
            return;
        }
        DWORD old = 0;
        if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            g_createWindowExA = reinterpret_cast<CreateWindowExA_t>(*slot);
            *slot = reinterpret_cast<void*>(&HkCreateWindowExA);
            ::VirtualProtect(slot, sizeof(void*), old, &old);
            g_clientW = a_clientWidth;
            g_clientH = a_clientHeight;
            REX::INFO("small window: forcing game window client {}x{}", a_clientWidth, a_clientHeight);
        }
    }

    void SetEngineRenderSize(int a_width, int a_height)
    {
        g_renderW = a_width;
        g_renderH = a_height;
    }

    void SendToBack(void* a_hwnd)
    {
        const HWND hwnd = static_cast<HWND>(a_hwnd);
        if (hwnd && ::IsWindow(hwnd)) {
            ::SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}
