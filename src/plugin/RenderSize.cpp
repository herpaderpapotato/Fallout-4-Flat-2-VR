#include <REX/LOG.h>

#include "plugin/RenderSize.h"
#include "plugin/StereoPoC.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <Windows.h>
#undef ERROR
#include <d3d11.h>
#include <dxgi.h>

namespace fo4flatvr::rendersize
{
    namespace
    {
        constexpr std::uintptr_t kIatD3D11CreateDeviceAndSwapChain = 0x2441758;  // 1.11.240
        constexpr std::size_t    kResizeBuffersSlot = 13;  // IDXGISwapChain vtable

        using Create_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
            UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        using Resize_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

        Create_t g_create = nullptr;
        HWND     g_gameHwnd = nullptr;  // the game window (swapchain OutputWindow)
        Resize_t g_resize = nullptr;
        UINT     g_width = 0, g_height = 0;

        HRESULT WINAPI HkCreate(IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_type, HMODULE a_sw, UINT a_flags,
            const D3D_FEATURE_LEVEL* a_levels, UINT a_numLevels, UINT a_sdk, const DXGI_SWAP_CHAIN_DESC* a_desc,
            IDXGISwapChain** a_swap, ID3D11Device** a_device, D3D_FEATURE_LEVEL* a_level, ID3D11DeviceContext** a_ctx)
        {
            if (a_desc && g_width && g_height) {
                DXGI_SWAP_CHAIN_DESC d = *a_desc;
                REX::INFO("swapchain create: game asked {}x{}, forcing {}x{}", d.BufferDesc.Width, d.BufferDesc.Height,
                    g_width, g_height);
                d.BufferDesc.Width = g_width;
                d.BufferDesc.Height = g_height;
                d.Windowed = TRUE;  // never exclusive fullscreen; the window is only a mirror
                g_gameHwnd = d.OutputWindow;
                const HRESULT hr = g_create(a_adapter, a_type, a_sw, a_flags, a_levels, a_numLevels, a_sdk, &d, a_swap,
                    a_device, a_level, a_ctx);
                // The engine's first Renderer::Begin calls ResizeBuffers(0,0) (pending window-size change),
                // which would shrink the buffer to the client area and size every render target from it.
                // Hook it now, before that can happen.
                if (SUCCEEDED(hr) && a_swap && *a_swap) {
                    HookResize(*a_swap);
                }
                // Stereo PoC: device/context vtable hooks before the engine creates its shaders.
                if (SUCCEEDED(hr) && a_device && *a_device && a_ctx && *a_ctx) {
                    stereo::OnDeviceCreated(*a_device, *a_ctx);
                }
                return hr;
            }
            const HRESULT hr = g_create(a_adapter, a_type, a_sw, a_flags, a_levels, a_numLevels, a_sdk, a_desc, a_swap, a_device,
                a_level, a_ctx);
            if (SUCCEEDED(hr) && a_device && *a_device && a_ctx && *a_ctx) {
                stereo::OnDeviceCreated(*a_device, *a_ctx);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE HkResize(IDXGISwapChain* a_this, UINT a_count, UINT a_w, UINT a_h, DXGI_FORMAT a_fmt,
            UINT a_flags)
        {
            REX::INFO("ResizeBuffers {}x{} -> keeping {}x{}", a_w, a_h, g_width, g_height);
            return g_resize(a_this, a_count, g_width, g_height, a_fmt, a_flags);
        }

        // Renderer::Begin feeds GetClientRect(hwnd) into RenderTargetManager::SetFrameBufferProperties
        // every frame (renderTargetData[0], used for camera aspect and swapchain-target viewports).
        // For the game window report the render size instead of the (small) window's client area.
        constexpr std::uintptr_t kIatGetClientRect = 0x2440B78;
        using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
        GetClientRect_t g_getClientRect = nullptr;

        BOOL WINAPI HkGetClientRect(HWND a_hwnd, LPRECT a_rect)
        {
            if (a_hwnd && a_hwnd == g_gameHwnd && a_rect && g_width && g_height) {
                *a_rect = { 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
                return TRUE;
            }
            return g_getClientRect(a_hwnd, a_rect);
        }

        bool PatchPointer(void** a_slot, void* a_new, void** a_old)
        {
            DWORD old = 0;
            if (!::VirtualProtect(a_slot, sizeof(void*), PAGE_READWRITE, &old)) {
                return false;
            }
            *a_old = *a_slot;
            *a_slot = a_new;
            ::VirtualProtect(a_slot, sizeof(void*), old, &old);
            return true;
        }
    }

    Plan Resolve(int a_width, int a_height, float a_scale, bool a_stereo, int a_align, int a_guard)
    {
        Plan p;
        if (a_width <= 0 || a_height <= 0) {
            return p;  // the game decides (window size); no scale, no guard band
        }
        p.scale = (a_scale > 0.0f && std::isfinite(a_scale)) ? std::clamp(a_scale, 0.25f, 4.0f) : 1.0f;
        p.eyeWidth = (std::max)(16, static_cast<int>(std::lround(a_width * 0.5 * p.scale)));
        p.eyeHeight = (std::max)(16, static_cast<int>(std::lround(a_height * static_cast<double>(p.scale))));
        int region = p.eyeWidth;
        if (a_stereo) {
            region += (std::max)(0, a_guard);
            if (a_align > 1) {
                region = (region + a_align - 1) / a_align * a_align;
            }
        }
        p.seamPad = region - p.eyeWidth;
        p.width = 2 * region;
        p.height = p.eyeHeight;
        REX::INFO("render size: [Render] {}x{} x scale {:.3f} -> eye {}x{}{} -> render {}x{}", a_width, a_height, p.scale, p.eyeWidth,
            p.eyeHeight, a_stereo ? std::format(" + seam guard {} px (align {}, min {})", p.seamPad, a_align, a_guard) : std::string{},
            p.width, p.height);
        return p;
    }

    namespace
    {
        // SteamVR's OpenXR runtime cannot share projection-layer textures with a device whose adapter came from a
        // DXGI 1.0 factory: xrclient log "Failed to create sync texture. Ensure application was built using DXGI 1.1
        // or later (i.e. Call CreateDXGIFactory1)" and every frame "ComposeLayerProjection: failed to submit view N:
        // VRCompositorError_SharedTexturesNotSupported" -> the 3D world is never shown (quad layers still are, as
        // SteamVR overlays). Fallout4.exe imports only dxgi!CreateDXGIFactory, so route it to CreateDXGIFactory1
        // (same signature; the 1.1 factory also answers IID_IDXGIFactory). VDXR tolerated the 1.0 factory.
        using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
        CreateFactory_t g_createFactory1 = nullptr;
        std::atomic<int> g_factoryCalls{ 0 };

        HRESULT WINAPI HkCreateDXGIFactory(REFIID a_riid, void** a_out)
        {
            const HRESULT hr = g_createFactory1(a_riid, a_out);
            if (g_factoryCalls.fetch_add(1) < 4) {
                REX::INFO("dxgi: CreateDXGIFactory -> CreateDXGIFactory1 hr {:08X}", static_cast<std::uint32_t>(hr));
            }
            return hr;
        }

        void** FindIatSlot(const char* a_dll, const char* a_func)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) {
                return nullptr;
            }
            for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
                if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) != 0 || !imp->OriginalFirstThunk) {
                    continue;
                }
                auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
                auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++iat) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                        continue;
                    }
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), a_func) == 0) {
                        return reinterpret_cast<void**>(&iat->u1.Function);
                    }
                }
            }
            return nullptr;
        }

        void InstallDxgiFactoryUpgrade()
        {
            HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
            if (!dxgi) {
                dxgi = ::LoadLibraryW(L"dxgi.dll");
            }
            const auto* real = dxgi ? reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory")) : nullptr;
            g_createFactory1 = dxgi ? reinterpret_cast<CreateFactory_t>(::GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
            void** slot = FindIatSlot("dxgi.dll", "CreateDXGIFactory");
            if (!slot || !real || !g_createFactory1 || *slot != real) {
                REX::ERROR("dxgi: CreateDXGIFactory IAT slot not found or not the real export ({} / {}); SteamVR may not show the 3D world",
                    slot ? *slot : nullptr, real);
                return;
            }
            void* old = nullptr;
            if (PatchPointer(slot, reinterpret_cast<void*>(&HkCreateDXGIFactory), &old)) {
                REX::INFO("dxgi: CreateDXGIFactory routed to CreateDXGIFactory1 (IAT {})", static_cast<void*>(slot));
            }
        }
    }

    void Install(int a_width, int a_height)
    {
        InstallDxgiFactoryUpgrade();
        const bool sizeForced = a_width > 0 && a_height > 0;
        if (!sizeForced && !stereo::HooksEnabled()) {
            return;  // the stereo PoC also needs the IAT hook (device creation), even at the game's own size
        }
        g_width = sizeForced ? static_cast<UINT>(a_width) : 0;
        g_height = sizeForced ? static_cast<UINT>(a_height) : 0;
        auto*       base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
        auto**      slot = reinterpret_cast<void**>(base + kIatD3D11CreateDeviceAndSwapChain);
        const auto* expected = reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
        // Byte-check equivalent for an IAT slot: it must currently point at the real export.
        if (!expected || *slot != expected) {
            REX::ERROR("render size: IAT slot does not hold D3D11CreateDeviceAndSwapChain ({} vs {}); not patching",
                *slot, expected);
            return;
        }
        void* old = nullptr;
        if (PatchPointer(slot, reinterpret_cast<void*>(&HkCreate), &old)) {
            g_create = reinterpret_cast<Create_t>(old);
            REX::INFO("render size: forcing swapchain {}x{} (IAT hook installed)", g_width, g_height);
        }
        auto**      gcrSlot = reinterpret_cast<void**>(base + kIatGetClientRect);
        const auto* gcrExpected = reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetClientRect"));
        if (!gcrExpected || *gcrSlot != gcrExpected) {
            REX::ERROR("render size: IAT slot does not hold GetClientRect; not patching (camera aspect/viewports will follow the window)");
        } else if (PatchPointer(gcrSlot, reinterpret_cast<void*>(&HkGetClientRect), &old)) {
            g_getClientRect = reinterpret_cast<GetClientRect_t>(old);
            REX::INFO("render size: GetClientRect for the game window reports {}x{}", g_width, g_height);
        }
    }

    void HookResize(void* a_swapChain)
    {
        if (!g_width || g_resize || !a_swapChain) {
            return;
        }
        auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
        void*  old = nullptr;
        DWORD  prot = 0;
        if (::VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) {
            old = vtbl[kResizeBuffersSlot];
            vtbl[kResizeBuffersSlot] = reinterpret_cast<void*>(&HkResize);
            ::VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), prot, &prot);
            g_resize = reinterpret_cast<Resize_t>(old);
            REX::INFO("render size: ResizeBuffers hooked");
        }
    }
}
