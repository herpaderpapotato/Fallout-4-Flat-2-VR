// VR dialogue, render / XR side (Dialogue.h): FlatScreenModel crop copy (H1), world-image skip (H2), lag-follow option
// panel on world quad slot 1, cursor dot, haptic pulse, "dlg" pipe command.
// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/Dialogue.h"
#include "plugin/MenuPointer.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace fo4flatvr::dlg
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kD2R = kPi / 180.0f;
        constexpr float kR2D = 180.0f / kPi;
        using Clock = std::chrono::steady_clock;

        std::atomic<bool> g_panelOn{ true }, g_pointerOn{ true };
        std::atomic<bool> g_layerActive{ false };  // this frame's H2 may skip FlatScreenModel (decided at the last Present)
        bool              g_skippedThisFrame = false;
        std::atomic<std::uint64_t> g_h1Copies{ 0 }, g_h2Skips{ 0 }, g_skipFrames{ 0 }, g_submits{ 0 }, g_haptics{ 0 }, g_presents{ 0 };
        bool              g_installedFlags = false;

        ComPtr<ID3D11Device>           g_dev;
        ComPtr<ID3D11DeviceContext>    g_ctx;
        ComPtr<ID3D11Texture2D>        g_crop;
        ComPtr<ID3D11RenderTargetView> g_cropRtv;
        D3D11_TEXTURE2D_DESC           g_cropDesc{};
        bool                           g_cropFresh = false;
        float                          g_cropUv[4]{ 0, 0, 1, 1 };  // movie uv of the copied box (for the pointer)
        UINT                           g_srcW = 0, g_srcH = 0;

        // dlgcap: full copies of 0x3E / 0x3F taken in H1 on request, written at the next Present
        std::mutex              g_lock;  // capture path, status lines
        std::string             g_capPath;
        bool                    g_capArmed = false;  // render thread: take full copies at the next H1
        ComPtr<ID3D11Texture2D> g_full[2];
        bool                    g_fullFresh = false;
        std::string             g_lastCap{ "none" };
        std::string             g_poseLine{ "none" };

        // lag-follow yaw (reference space, radians, rotation about +Y; 0 = looking down -Z)
        float             g_yaw = 0.0f;
        bool              g_yawValid = false, g_following = false;
        Clock::time_point g_lastSubmit{};
        float             g_headYaw = 0.0f, g_headPitch = 0.0f;
        std::uint64_t     g_followStarts = 0;

        DXGI_FORMAT Typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }

        float Wrap(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)  // q v q^-1
        {
            const float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z), tz = 2.0f * (q.x * v.y - q.y * v.x);
            return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx) };
        }

        void EnsureDevice(ID3D11Texture2D* a_src)
        {
            if (!g_dev) {
                a_src->GetDevice(&g_dev);
                g_dev->GetImmediateContext(&g_ctx);
            }
        }

        bool EnsureTex(ComPtr<ID3D11Texture2D>& a_tex, D3D11_TEXTURE2D_DESC* a_have, UINT a_w, UINT a_h, DXGI_FORMAT a_fmt, UINT a_bind,
            ComPtr<ID3D11RenderTargetView>* a_rtv)
        {
            D3D11_TEXTURE2D_DESC cur{};
            if (a_tex) {
                a_tex->GetDesc(&cur);
            }
            if (a_tex && cur.Width == a_w && cur.Height == a_h && cur.Format == a_fmt) {
                return true;
            }
            a_tex.Reset();
            if (a_rtv) {
                a_rtv->Reset();
            }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = a_w;
            td.Height = a_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = a_fmt;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = a_bind;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &a_tex))) {
                // RTV binding may not be allowed for this format: retry as a plain shader resource
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &a_tex))) {
                    REX::ERROR("dialogue: texture {}x{} fmt {} creation failed", a_w, a_h, static_cast<int>(a_fmt));
                    return false;
                }
            }
            if (a_rtv && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = Typed(a_fmt);
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                if (FAILED(g_dev->CreateRenderTargetView(a_tex.Get(), &rd, a_rtv->GetAddressOf()))) {
                    a_rtv->Reset();
                }
            }
            if (a_have) {
                *a_have = td;
            }
            REX::INFO("dialogue: texture {}x{} fmt {}", a_w, a_h, static_cast<int>(a_fmt));
            return true;
        }

        float HalfToFloat(std::uint16_t h)
        {
            const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
            float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
            return s ? -v : v;
        }

        // texture -> BGRA8 + stats line (alpha>0 count, bbox in fractions, 3x3 grid, hash)
        std::string ReadBack(ID3D11Texture2D* a_tex, std::vector<std::uint8_t>& a_bgra, UINT& a_w, UINT& a_h)
        {
            D3D11_TEXTURE2D_DESC d{};
            a_tex->GetDesc(&d);
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) {
                return "staging failed";
            }
            g_ctx->CopyResource(st.Get(), a_tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                return "map failed";
            }
            const UINT w = d.Width, h = d.Height;
            a_w = w, a_h = h;
            a_bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
            bool ok = true;
            auto b8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            for (UINT y = 0; y < h && ok; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
                std::uint8_t* out = a_bgra.data() + static_cast<std::size_t>(y) * w * 4;
                for (UINT x = 0; x < w; ++x) {
                    std::uint8_t r, g, b, a;
                    switch (d.Format) {
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                        r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2], a = row[x * 4 + 3];
                        break;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                        b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
                        break;
                    case DXGI_FORMAT_R10G10B10A2_UNORM:
                    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                        {
                            std::uint32_t v;
                            std::memcpy(&v, row + x * 4, 4);
                            r = static_cast<std::uint8_t>((v & 0x3FF) >> 2), g = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) >> 2);
                            b = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) >> 2), a = static_cast<std::uint8_t>((v >> 30) * 85);
                        }
                        break;
                    case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                        {
                            std::uint16_t v[4];
                            std::memcpy(v, row + x * 8, 8);
                            r = b8(HalfToFloat(v[0])), g = b8(HalfToFloat(v[1])), b = b8(HalfToFloat(v[2])), a = b8(HalfToFloat(v[3]));
                        }
                        break;
                    default:
                        ok = false;
                        r = g = b = a = 0;
                        break;
                    }
                    out[x * 4] = b, out[x * 4 + 1] = g, out[x * 4 + 2] = r, out[x * 4 + 3] = a;
                }
            }
            g_ctx->Unmap(st.Get(), 0);
            if (!ok) {
                return std::format("unsupported format {}", static_cast<int>(d.Format));
            }
            std::size_t vis = 0, overA = 0, grid[9]{};
            UINT        x0 = w, y0 = h, x1 = 0, y1 = 0;
            std::uint64_t hash = 1469598103934665603ull;
            for (UINT y = 0; y < h; ++y) {
                for (UINT x = 0; x < w; ++x) {
                    const std::uint8_t* p = a_bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                    for (int k = 0; k < 4; ++k) {
                        hash = (hash ^ p[k]) * 1099511628211ull;
                    }
                    if (p[3] == 0) {
                        continue;
                    }
                    ++vis;
                    overA += (std::max)({ p[0], p[1], p[2] }) > p[3] + 4;
                    x0 = (std::min)(x0, x), y0 = (std::min)(y0, y), x1 = (std::max)(x1, x), y1 = (std::max)(y1, y);
                    ++grid[(y * 3 / h) * 3 + x * 3 / w];
                }
            }
            return std::format("{}x{} fmt {} alpha>0 {} ({:.2f}%) bbox [{:.3f},{:.3f}..{:.3f},{:.3f}] grid T[{} {} {}] M[{} {} {}] B[{} {} {}] rgb>a {} hash {:016X}",
                w, h, static_cast<int>(d.Format), vis, 100.0 * static_cast<double>(vis) / (static_cast<double>(w) * h),
                vis ? static_cast<double>(x0) / w : 0.0, vis ? static_cast<double>(y0) / h : 0.0, vis ? static_cast<double>(x1 + 1) / w : 0.0,
                vis ? static_cast<double>(y1 + 1) / h : 0.0, grid[0], grid[1], grid[2], grid[3], grid[4], grid[5], grid[6], grid[7], grid[8], overA, hash);
        }

        void WriteBmp(const std::string& a_path, const std::vector<std::uint8_t>& a_bgra, UINT a_w, UINT a_h)
        {
            const std::uint32_t size = a_w * a_h * 4;
            BITMAPFILEHEADER    fh{};
            BITMAPINFOHEADER    ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + size;
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(a_w);
            ih.biHeight = -static_cast<LONG>(a_h);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = size;
            HANDLE f = ::CreateFileA(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                return;
            }
            DWORD n = 0;
            ::WriteFile(f, &fh, sizeof(fh), &n, nullptr);
            ::WriteFile(f, &ih, sizeof(ih), &n, nullptr);
            ::WriteFile(f, a_bgra.data(), size, &n, nullptr);
            ::CloseHandle(f);
        }

        void ServiceCapture(const std::string& a_path)
        {
            std::string stem = a_path;
            if (stem.size() > 4 && _stricmp(stem.c_str() + stem.size() - 4, ".bmp") == 0) {
                stem.resize(stem.size() - 4);
            }
            std::string summary;
            if (g_fullFresh) {
                for (int i = 0; i < 2; ++i) {
                    if (!g_full[i]) {
                        continue;
                    }
                    std::vector<std::uint8_t> px;
                    UINT                      w = 0, h = 0;
                    const std::string         s = ReadBack(g_full[i].Get(), px, w, h);
                    summary += std::format(" {:X}: {};", i ? 0x3F : 0x3E, s);
                    if (!px.empty()) {
                        WriteBmp(std::format("{}_{:x}.bmp", stem, i ? 0x3F : 0x3E), px, w, h);
                    }
                }
            } else {
                summary += " full: no FlatScreenModel draw captured (no dialogue / not exclusive);";
            }
            if (g_crop && g_cropFresh) {
                std::vector<std::uint8_t> px;
                UINT                      w = 0, h = 0;
                summary += std::format(" panel: {};", ReadBack(g_crop.Get(), px, w, h));
                if (!px.empty()) {
                    WriteBmp(stem + "_panel.bmp", px, w, h);
                }
            } else {
                summary += " panel: none this frame;";
            }
            REX::INFO("dlgcap {}:{}", a_path, summary);
            std::scoped_lock l(g_lock);
            g_lastCap = a_path + ":" + summary;
        }
    }

    bool CaptureWanted()
    {
        if (!FlatExclusive()) {
            return false;
        }
        if (g_panelOn.load(std::memory_order_relaxed) && g_layerActive.load(std::memory_order_relaxed)) {
            return true;
        }
        std::scoped_lock l(g_lock);
        return g_capArmed;
    }

    void OnFlatTargets(void* a_tex3E, void* a_tex3F)
    {
        const Config c = ConfigCopy();
        auto* src = static_cast<ID3D11Texture2D*>(c.source == 0x3E ? a_tex3E : a_tex3F);
        bool  arm = false;
        {
            std::scoped_lock l(g_lock);
            arm = g_capArmed;
            g_capArmed = false;
        }
        if (arm) {
            ID3D11Texture2D* srcs[2] = { static_cast<ID3D11Texture2D*>(a_tex3E), static_cast<ID3D11Texture2D*>(a_tex3F) };
            for (int i = 0; i < 2; ++i) {
                if (!srcs[i]) {
                    continue;
                }
                EnsureDevice(srcs[i]);
                D3D11_TEXTURE2D_DESC sd{};
                srcs[i]->GetDesc(&sd);
                if (sd.SampleDesc.Count == 1 && EnsureTex(g_full[i], nullptr, sd.Width, sd.Height, sd.Format, D3D11_BIND_SHADER_RESOURCE, nullptr)) {
                    g_ctx->CopyResource(g_full[i].Get(), srcs[i]);
                    g_fullFresh = true;
                }
            }
        }
        if (!src) {
            return;
        }
        EnsureDevice(src);
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.SampleDesc.Count > 1 || !sd.Width || !sd.Height) {
            return;
        }
        const UINT l = static_cast<UINT>(std::floor(c.cropLeft * sd.Width)), t = static_cast<UINT>(std::floor(c.cropTop * sd.Height));
        const UINT r = (std::max)(l + 2, (std::min)(sd.Width, static_cast<UINT>(std::ceil(c.cropRight * sd.Width))));
        const UINT b = (std::max)(t + 2, (std::min)(sd.Height, static_cast<UINT>(std::ceil(c.cropBottom * sd.Height))));
        if (r > sd.Width || b > sd.Height) {
            return;
        }
        if (!EnsureTex(g_crop, &g_cropDesc, r - l, b - t, sd.Format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &g_cropRtv)) {
            return;
        }
        const D3D11_BOX box{ l, t, 0, r, b, 1 };
        g_ctx->CopySubresourceRegion(g_crop.Get(), 0, 0, 0, 0, src, 0, &box);
        g_cropUv[0] = static_cast<float>(l) / sd.Width, g_cropUv[1] = static_cast<float>(t) / sd.Height;
        g_cropUv[2] = static_cast<float>(r) / sd.Width, g_cropUv[3] = static_cast<float>(b) / sd.Height;
        g_srcW = sd.Width, g_srcH = sd.Height;
        g_cropFresh = true;
        ++g_h1Copies;
    }

    bool SkipWorldFlat()
    {
        return g_panelOn.load(std::memory_order_relaxed) && g_layerActive.load(std::memory_order_relaxed) && FlatExclusive();
    }

    void NoteWorldFlatSkipped()
    {
        ++g_h2Skips;
        g_skippedThisFrame = true;
    }

    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx, const xr::Pose& a_head, bool a_headValid)
    {
        menuptr::Panel noPanel;
        if (!g_panelOn.load() || !g_layerActive.load() || !g_cropFresh || !g_crop || !a_headValid) {
            menuptr::SetPanel(noPanel);
            g_yawValid = g_yawValid && (Clock::now() - g_lastSubmit) < std::chrono::milliseconds(250);
            return false;
        }
        const Config c = ConfigCopy();
        // head yaw / pitch from the forward (-Z) axis
        const xr::Vec3 f = Rot(a_head.orientation, { 0.0f, 0.0f, -1.0f });
        const float    hy = std::atan2(-f.x, -f.z);
        g_headYaw = hy;
        g_headPitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
        const auto now = Clock::now();
        float      dt = std::chrono::duration<float>(now - g_lastSubmit).count();
        if (!g_yawValid || dt > 0.25f) {
            g_yaw = hy;  // (re)opened: straight ahead
            g_yawValid = true;
            g_following = false;
            dt = 0.0f;
        }
        g_lastSubmit = now;
        const float err = Wrap(hy - g_yaw);
        if (!g_following && std::abs(err) > c.deadzoneDeg * kD2R) {
            g_following = true;
            ++g_followStarts;
        }
        if (g_following) {
            g_yaw = Wrap(g_yaw + err * (1.0f - std::exp(-dt / c.lagSeconds)));
            if (std::abs(Wrap(hy - g_yaw)) < 1.0f * kD2R) {
                g_following = false;
            }
        }
        const float    s = std::sin(g_yaw), co = std::cos(g_yaw);
        const xr::Vec3 pos{ a_head.position.x - c.distanceMeters * s, a_head.position.y - c.downMeters, a_head.position.z - c.distanceMeters * co };
        // orientation = Ry(yaw) * Rx(-pitch): + pitch turns the face up toward the eyes
        const float    hyw = 0.5f * g_yaw, hp = -0.5f * c.pitchDeg * kD2R;
        const xr::Quat qy{ 0.0f, std::sin(hyw), 0.0f, std::cos(hyw) }, qx{ std::sin(hp), 0.0f, 0.0f, std::cos(hp) };
        const xr::Quat q{ qy.w * qx.x + qy.x * qx.w + qy.y * qx.z - qy.z * qx.y, qy.w * qx.y - qy.x * qx.z + qy.y * qx.w + qy.z * qx.x,
            qy.w * qx.z + qy.x * qx.y - qy.y * qx.x + qy.z * qx.w, qy.w * qx.w - qy.x * qx.x - qy.y * qx.y - qy.z * qx.z };
        const xr::Pose pose{ q, pos };
        const float    w = c.widthMeters;
        const float    h = w * static_cast<float>(g_cropDesc.Height) / static_cast<float>((std::max)(1u, g_cropDesc.Width));

        // cursor dot from the pointer's last frame on this panel
        float pu = 0.0f, pv = 0.0f;
        bool  down = false;
        if (g_cropRtv && menuptr::PanelCursor(pu, pv, down)) {
            ComPtr<ID3D11DeviceContext1> c1;
            if (SUCCEEDED(a_ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
                const LONG cx = static_cast<LONG>(pu * g_cropDesc.Width), cy = static_cast<LONG>(pv * g_cropDesc.Height);
                const LONG r = (std::max)(4L, static_cast<LONG>(g_cropDesc.Width / 120)), bd = (std::max)(2L, r / 3);
                const float outer[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                const float inner[4] = { down ? 0.3f : 1.0f, 1.0f, down ? 0.3f : 1.0f, 1.0f };
                const D3D11_RECT ro{ cx - r - bd, cy - r - bd, cx + r + bd + 1, cy + r + bd + 1 };
                const D3D11_RECT ri{ cx - r, cy - r, cx + r + 1, cy + r + 1 };
                c1->ClearView(g_cropRtv.Get(), outer, &ro, 1);
                c1->ClearView(g_cropRtv.Get(), inner, &ri, 1);
            }
        }
        if (!a_rt.SubmitWorldQuad(a_ctx, g_crop.Get(), pose, w, true, c.premultiplied, 1)) {
            menuptr::SetPanel(noPanel);
            return false;
        }
        ++g_submits;
        menuptr::Panel p;
        p.valid = g_pointerOn.load() && GetConfig().pointer;
        p.pos[0] = pos.x, p.pos[1] = pos.y, p.pos[2] = pos.z;
        p.quat[0] = q.x, p.quat[1] = q.y, p.quat[2] = q.z, p.quat[3] = q.w;
        p.width = w;
        p.height = h;
        p.u0 = g_cropUv[0], p.v0 = g_cropUv[1], p.u1 = g_cropUv[2], p.v1 = g_cropUv[3];
        p.menu = "DialogueMenu";
        menuptr::SetPanel(p.valid ? p : noPanel);
        {
            std::scoped_lock l(g_lock);
            g_poseLine = std::format("panelYaw={:.2f} headYaw={:.2f} headPitch={:.2f} err={:.2f} following={} followStarts={} pos {:.4f},{:.4f},{:.4f} "
                                     "quat {:.4f},{:.4f},{:.4f},{:.4f} size {:.3f}x{:.3f} head {:.4f},{:.4f},{:.4f} layerFrames={}",
                g_yaw * kR2D, hy * kR2D, g_headPitch * kR2D, Wrap(hy - g_yaw) * kR2D, g_following ? 1 : 0, g_followStarts, pos.x, pos.y, pos.z, q.x, q.y,
                q.z, q.w, w, h, a_head.position.x, a_head.position.y, a_head.position.z, a_rt.WorldQuadsSubmitted(1));
        }
        return true;
    }

    void UpdateXr(xr::Runtime& a_rt, bool a_began)
    {
        if (!a_began || !TakeHapticRequest()) {
            return;
        }
        const Config c = ConfigCopy();
        a_rt.Vibrate(c.leftHanded ? 0 : 1, c.hapticAmplitude, c.hapticSeconds);
        ++g_haptics;
        REX::INFO("dialogue: haptic pulse {:.2f} for {:.2f} s on the {} controller (options shown)", c.hapticAmplitude, c.hapticSeconds,
            c.leftHanded ? "left" : "right");
    }

    void OnPresent(bool a_layerNext)
    {
        if (!g_installedFlags) {
            g_installedFlags = true;
            g_panelOn = GetConfig().panel;
            g_pointerOn = GetConfig().pointer;
        }
        ++g_presents;
        std::string path;
        {
            static int waited = 0;
            std::scoped_lock l(g_lock);
            if (!g_capPath.empty() && (!g_capArmed || ++waited > 60)) {  // no FlatScreenModel draw within 60 frames: report that
                path.swap(g_capPath);
                g_capArmed = false;
                waited = 0;
            }
        }
        if (!path.empty()) {
            ServiceCapture(path);
        }
        if (g_skippedThisFrame) {
            ++g_skipFrames;
        }
        g_skippedThisFrame = false;
        menuptr::SetPanel({});  // re-published by Submit in the next frame if the panel is shown again
        g_cropFresh = false;
        g_fullFresh = false;
        g_layerActive = a_layerNext && g_panelOn.load() && Open();
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg.empty() || a_arg == "status") {
            std::string pose, cap;
            {
                std::scoped_lock l(g_lock);
                pose = g_poseLine;
                cap = g_lastCap;
            }
            return std::format("panel={} pointer={} layerActive={} h1Copies={} h2Skips={} skipFrames={} submits={} haptics={} presents={} crop {}x{} "
                               "uv [{:.3f},{:.3f}..{:.3f},{:.3f}] of {}x{} | {} | {} | last dlgcap {}",
                g_panelOn.load() ? 1 : 0, g_pointerOn.load() ? 1 : 0, g_layerActive.load() ? 1 : 0, g_h1Copies.load(), g_h2Skips.load(), g_skipFrames.load(),
                g_submits.load(), g_haptics.load(), g_presents.load(), g_cropDesc.Width, g_cropDesc.Height, g_cropUv[0], g_cropUv[1], g_cropUv[2],
                g_cropUv[3], g_srcW, g_srcH, pose, GameStatus(), cap);
        }
        if (a_arg == "panel on" || a_arg == "panel off") {
            g_panelOn = a_arg == "panel on";
            return std::format("dlg panel {}", g_panelOn.load() ? "on" : "off");
        }
        if (a_arg == "pointer on" || a_arg == "pointer off") {
            g_pointerOn = a_arg == "pointer on";
            return std::format("dlg pointer {}", g_pointerOn.load() ? "on" : "off");
        }
        if (a_arg.rfind("cap ", 0) == 0) {
            std::scoped_lock l(g_lock);
            g_capPath = a_arg.substr(4);
            g_capArmed = true;
            return "dlgcap queued (next FlatScreenModel draw while DialogueMenu is exclusive)";
        }
        if (a_arg.rfind("cfg ", 0) == 0) {
            std::istringstream is(a_arg.substr(4));
            std::string        k;
            float              v = 0.0f;
            if (is >> k >> v && SetCfgValue(k, v)) {
                return std::format("dlg cfg {} = {}", k, v);
            }
            return "error: dlg cfg dist|down|width|pitch|lag|deadzone|cropl|cropt|cropr|cropb|src|premul|amp|hsec <value>";
        }
        if (auto r = GameCommand(a_arg); !r.empty()) {
            return r;
        }
        return "error: dlg [status|panel on|off|pointer on|off|look on|off|weapon on|off|haptics on|off|stick on|off|cap <path>|cfg <key> <value>]";
    }
}
