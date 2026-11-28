// Scopes like FO4VR (Scope.h): the raise-to-eye test, the scope eyes, the round scope image on its quad.
// Present thread (the D3D11 immediate-context owner) except Install / Command.
#include <REX/LOG.h>

#include "plugin/ConfigStore.h"
#include "plugin/Dlss.h"
#include "plugin/Scope.h"
#include "plugin/StereoPoC.h"
#include "plugin/ThreadQueues.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fo4flatvr::scope
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        // ---- FO4VR 1.2.72 settings (evidence/scope.md C3: values read from the exe's data) ----
        constexpr float kHmdConeEnter = 25.0f, kHmdConeFade = 30.0f, kHmdConeExit = 35.0f;  // fHmdToWeaponAngle*ScopeCone/FadeDegrees
        constexpr float kWpnConeEnter = 7.0f, kWpnConeFade = 10.0f, kWpnConeExit = 15.0f;   // fWeaponAngle*Scope*Degrees
        constexpr float kDistEnter = 38.0f, kDistFade = 42.0f, kDistExit = 40.0f;           // fWeaponDist*Scope (game units)
        constexpr float kWidening = 60.0f, kWideningExp = 2.0f;                             // fScopeWeaponAngleWideningFactor / Exponent
        constexpr float kHmdOffsetBack = 16.0f;   // fHMDScopeOffsetY -16 (HMD node Y = forward -> 16 u behind the eyes)
        constexpr float kWeaponOffsetUp = 8.0f;   // fWeaponScopeOffsetZ 8 (weapon node Z = up)
        constexpr double kRad = 3.14159265358979 / 180.0;

        struct Cfg
        {
            bool  enabled{ true };
            float viewDeg{ 0.0f };        // > 0: override - angular diameter of the scope image as seen (0 = FO4VR's lens on the gun)
            float viewDistance{ 0.0f };   // > 0 with fViewDeg: metres from the eye along the aim ray
            float lensScale{ 1.0f };      // FO4VR fScopeScale:VR (world_scope.nif render_circle radius 7.852 u)
            float lensUp{ 10.0f };        // FO4VR fScopeOffsetZ:VR (ScopeParent above PrimaryUIAttachNode, game units)
            int   overlaySource{ 1 };     // the game's reticle: 1 ScopeMenu glass target 0x3E, 2 movie target 0x3F, 0 menu panel copy
            float fadeMagnitude{ 1.25f }; // FO4VR fScopeFadeMagnitude
            float zoomScale{ 1.0f };      // x the flat ADS zoom
            float overlayScale{ 1.0f };   // ScopeMenu overlay: circle diameter / overlay height
            bool  holdBreathGrip{ true };
            int   reticle{ 1 };           // our duplex crosshair: 0 never, 1 only while the game's overlay is missing, 2 always
        };
        std::mutex g_cfgLock;
        Cfg        g_cfg;
        Cfg CurCfg()
        {
            std::scoped_lock l(g_cfgLock);
            return g_cfg;
        }

        std::string Get(const std::map<std::string, std::string>& a_m, const char* a_sec, const char* a_key)
        {
            std::string k = std::string(a_sec) + "\n" + a_key;
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto it = a_m.find(k);
            if (it == a_m.end()) return {};
            std::string v = it->second;
            if (const auto sc = v.find_first_of(";#"); sc != std::string::npos) v.erase(sc);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            return v;
        }

        Cfg LoadCfg()
        {
            Cfg        c;
            const auto m = cfgstore::MergedValues();
            auto       b = [&](const char* k, bool& v) {
                const auto t = Get(m, "Scope", k);
                if (!t.empty()) v = std::strtol(t.c_str(), nullptr, 0) != 0;
            };
            auto f = [&](const char* k, float& v, float lo, float hi) {
                const auto t = Get(m, "Scope", k);
                if (!t.empty()) v = std::clamp(std::strtof(t.c_str(), nullptr), lo, hi);
            };
            b("bEnabled", c.enabled);
            f("fViewDeg", c.viewDeg, 0.0f, 90.0f);
            f("fViewDistance", c.viewDistance, 0.0f, 5.0f);
            f("fLensScale", c.lensScale, 0.25f, 4.0f);
            f("fLensOffsetZ", c.lensUp, -20.0f, 40.0f);
            {
                const auto t = Get(m, "Scope", "iOverlaySource");
                if (!t.empty()) c.overlaySource = std::clamp(static_cast<int>(std::strtol(t.c_str(), nullptr, 0)), 0, 2);
            }
            f("fFadeMagnitude", c.fadeMagnitude, 0.0f, 2.0f);
            f("fZoomScale", c.zoomScale, 0.25f, 4.0f);
            f("fOverlayScale", c.overlayScale, 0.5f, 2.0f);
            b("bHoldBreathGrip", c.holdBreathGrip);
            {
                const auto t = Get(m, "Scope", "iReticle");
                if (!t.empty()) c.reticle = std::clamp(static_cast<int>(std::strtol(t.c_str(), nullptr, 0)), 0, 2);
            }
            return c;
        }

        // ---- vector maths (OpenXR space) ----
        struct V3 { float x, y, z; };
        V3    Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        V3    Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        V3    Mul(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Len(V3 a) { return std::sqrt(Dot(a, a)); }
        V3    Rot(const xr::Quat& q, V3 v)  // q * v * q^-1
        {
            const V3 u{ q.x, q.y, q.z };
            const V3 t = Mul({ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x }, 2.0f);
            const V3 c{ u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x };
            return Add(Add(v, Mul(t, q.w)), c);
        }
        V3    P(const xr::Vec3& v) { return { v.x, v.y, v.z }; }
        float AngleDeg(V3 a, V3 b) { return static_cast<float>(std::acos(std::clamp(Dot(a, b), -1.0f, 1.0f)) / kRad); }
        float Ramp(float x, float enter, float fade) { return std::clamp((fade - x) / (std::max)(fade - enter, 1e-4f), 0.0f, 1.0f); }

        // ---- state (Present thread unless atomic) ----
        std::atomic<bool>  g_raised{ false };       // FO4VR "scoped" decision (enter / exit test)
        std::atomic<bool>  g_viewActive{ false };   // raised and ScopeMenu open: next frame renders the scope view
        std::atomic<bool>  g_menuInWorld{ false };
        std::atomic<bool>  g_menuCopy{ false };
        std::atomic<bool>  g_noCopy{ false };
        std::atomic<bool>  g_ovDiag{ false };
        std::atomic<bool>  g_noEyes{ false };       // pipe `scope eyes 0` (diagnostic): keep the headset eyes while scoped       // pipe `scope ovdiag 1`: copy the ScopeMenu overlay whatever the state
        std::string        g_ovCapPath;             // pipe `scope ovcap <bmp>` (under g_capLock)       // pipe `scope copy 0` (diagnostic): ScopeMenu stays in the stereo frame
        std::atomic<float> g_fade{ 0.0f };
        std::atomic<bool>  g_stereo{ false };
        using Clock = std::chrono::steady_clock;
        Clock::time_point  g_exitAt{};
        float              g_last[4]{};             // last test values: dist (u), hmd angle, weapon angle, weapon limit
        std::uint64_t      g_enters = 0, g_exits = 0, g_scopeFrames = 0, g_submits = 0, g_submitFails = 0, g_noMenuFrames = 0;
        std::string        g_lastErr;
        int                g_force = 0;             // pipe: 1 forced raised, -1 forced off, 0 test

        struct Pub
        {
            bool     scope{ false };
            xr::Pose head{}, cam{}, aim{};
            float    upm{ 70.0f };
            float    fovH{ 0.0f };                  // circle angular diameter in the world (rad)
        };
        xr::Pose g_aimForEyes{};  // primary aim pose of the frame being begun (Update)
        float    g_upm = 70.0f;
        // the game's scope overlay (ScopeMenu renderer targets after its post FX; GameHud H1)
        struct Overlay
        {
            ComPtr<ID3D11Texture2D>          tex[2];
            ComPtr<ID3D11ShaderResourceView> srv[2];
            std::uint64_t                    frame[2]{};
            std::uint64_t                    copies{ 0 };
        };
        Overlay                    g_ov;
        std::atomic<std::uint64_t> g_frames{ 0 };  // frame begins (FillEyes)
        std::uint64_t              g_ovUsed = 0, g_ovMissing = 0;
        Pub  g_pubNow;      // published at the last frame begin = the frame presented at the next submission
        bool g_presentedScope = false;
        bool g_lastSubmitScope = false;

        // FO4VR Main::Swap scope pass (VR D831F0): the scope camera's FOV = TESObjectWEAP::CalcSightsAdjustedFOV(70) =
        // 2 atan(tan(35 deg) / fovMult) (flat 478B00, same maths); world_scope.nif render_circle shows UV 0.042..0.958 of that
        // render = the circle's angular diameter.
        float ScopeFovRad(const Cfg& c)
        {
            const auto   w = detail::Weapon();
            const double m = w.fovMult > 0.0f ? w.fovMult : 1.0;
            const double t = std::tan(35.0 * kRad) / m * 0.916 / c.zoomScale;
            return static_cast<float>(std::clamp(2.0 * std::atan(t), 0.5 * kRad, 90.0 * kRad));
        }

        // ---- GPU: round mask + ScopeMenu overlay ----
        constexpr char kHlsl[] = R"(
cbuffer C : register(b0) { float4 g_menu; float4 g_opt; };
Texture2D    t_scene : register(t0);
Texture2D    t_menu  : register(t1);
SamplerState s_lin   : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VO i) : SV_Target
{
    float2 c = i.uv - 0.5;
    float  r = length(c) * 2.0;
    float3 col = t_scene.Sample(s_lin, i.uv).rgb;
    if (g_opt.y > 0.5) {
        float2 muv = float2(0.5, 0.5) + c * g_menu.xy;
        float4 m = t_menu.Sample(s_lin, muv);
        if (any(muv < 0.0) || any(muv > 1.0)) m = float4(0, 0, 0, 0);
        col = g_opt.x > 0.5 ? col * (1.0 - m.a) + m.rgb : lerp(col, m.rgb, m.a);
    }
    if (g_opt.w > 0.5) {
        // duplex crosshair (the game's ScopeMenu overlay draws nothing here yet): thin near the centre, heavy outside
        float2 a = abs(c) * 2.0;                   // 0 centre .. 1 rim
        float  thin = 0.0028, heavy = 0.016;
        float  wx = a.y < 0.45 ? thin : heavy;     // vertical line half-width at this height
        float  wy = a.x < 0.45 ? thin : heavy;
        if (a.x < wx || a.y < wy) col = float3(0, 0, 0);
    }
    float mask = saturate((1.0 - r) / max(g_opt.z, 1e-4));
    return float4(col * mask, 1.0);
}
)";

        struct Gpu
        {
            bool                             tried{ false }, ok{ false };
            ComPtr<ID3D11VertexShader>       vs;
            ComPtr<ID3D11PixelShader>        ps;
            ComPtr<ID3D11Buffer>             cb;
            ComPtr<ID3D11SamplerState>       smp;
            ComPtr<ID3D11RasterizerState>    rs;
            ComPtr<ID3D11BlendState>         bs;
            ComPtr<ID3D11DepthStencilState>  dss;
            ComPtr<ID3D11Texture2D>          sq;     // the eye's centre square (back-buffer format)
            ComPtr<ID3D11ShaderResourceView> sqSrv;
            UINT                             sqSide{ 0 };
            DXGI_FORMAT                      sqFmt{ DXGI_FORMAT_UNKNOWN };
            ComPtr<ID3D11Texture2D>          out;
            ComPtr<ID3D11RenderTargetView>   outRtv;
            UINT                             outSide{ 0 };
            ID3D11Texture2D*                 menuKey{ nullptr };
            ComPtr<ID3D11ShaderResourceView> menuSrv;
        };
        Gpu         g_gpu;
        std::mutex  g_capLock;
        std::string g_capPath;

        DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            default: return f;
            }
        }

        bool CreateGpu(ID3D11Device* a_dev)
        {
            if (g_gpu.tried) {
                return g_gpu.ok;
            }
            g_gpu.tried = true;
            ComPtr<ID3DBlob> vsb, psb, err;
            if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "scope", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
                FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "scope", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
                g_lastErr = std::string("shader compile: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                REX::ERROR("scope: {}", g_lastErr);
                return false;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 32;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SAMPLER_DESC smd{};
            smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            smd.MaxLOD = D3D11_FLOAT32_MAX;
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            D3D11_BLEND_DESC bld{};
            bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            D3D11_DEPTH_STENCIL_DESC dsd{};
            if (FAILED(a_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_gpu.vs)) ||
                FAILED(a_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_gpu.ps)) ||
                FAILED(a_dev->CreateBuffer(&bd, nullptr, &g_gpu.cb)) || FAILED(a_dev->CreateSamplerState(&smd, &g_gpu.smp)) ||
                FAILED(a_dev->CreateRasterizerState(&rd, &g_gpu.rs)) || FAILED(a_dev->CreateBlendState(&bld, &g_gpu.bs)) ||
                FAILED(a_dev->CreateDepthStencilState(&dsd, &g_gpu.dss))) {
                g_lastErr = "D3D resource creation";
                REX::ERROR("scope: {}", g_lastErr);
                return false;
            }
            g_gpu.ok = true;
            return true;
        }

        // the immediate-context bindings this pass changes, put back afterwards (the engine's state cache assumes them)
        struct Saved
        {
            ID3D11DeviceContext*      ctx;
            ID3D11InputLayout*        il{};
            D3D11_PRIMITIVE_TOPOLOGY  topo{};
            ID3D11VertexShader*       vs{};
            ID3D11HullShader*         hs{};
            ID3D11DomainShader*       ds{};
            ID3D11GeometryShader*     gs{};
            ID3D11PixelShader*        ps{};
            ID3D11ShaderResourceView* srv[2]{};
            ID3D11SamplerState*       smp{};
            ID3D11Buffer*             cb{};
            ID3D11RasterizerState*    rs{};
            D3D11_VIEWPORT            vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                      nvp{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
            ID3D11BlendState*         bs{};
            FLOAT                     bf[4]{};
            UINT                      sm{};
            ID3D11DepthStencilState*  dss{};
            UINT                      sref{};
            ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView*   dsv{};

            explicit Saved(ID3D11DeviceContext* a) :
                ctx(a)
            {
                ctx->IAGetInputLayout(&il);
                ctx->IAGetPrimitiveTopology(&topo);
                ctx->VSGetShader(&vs, nullptr, nullptr);
                ctx->HSGetShader(&hs, nullptr, nullptr);
                ctx->DSGetShader(&ds, nullptr, nullptr);
                ctx->GSGetShader(&gs, nullptr, nullptr);
                ctx->PSGetShader(&ps, nullptr, nullptr);
                ctx->PSGetShaderResources(0, 2, srv);
                ctx->PSGetSamplers(0, 1, &smp);
                ctx->PSGetConstantBuffers(0, 1, &cb);
                ctx->RSGetState(&rs);
                ctx->RSGetViewports(&nvp, vps);
                ctx->OMGetBlendState(&bs, bf, &sm);
                ctx->OMGetDepthStencilState(&dss, &sref);
                ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
            }
            ~Saved()
            {
                ID3D11ShaderResourceView* nulls[2]{};
                ctx->PSSetShaderResources(0, 2, nulls);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
                ctx->OMSetDepthStencilState(dss, sref);
                ctx->OMSetBlendState(bs, bf, sm);
                ctx->RSSetViewports(nvp, vps);
                ctx->RSSetState(rs);
                ctx->PSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetSamplers(0, 1, &smp);
                ctx->PSSetShaderResources(0, 2, srv);
                ctx->PSSetShader(ps, nullptr, 0);
                ctx->GSSetShader(gs, nullptr, 0);
                ctx->DSSetShader(ds, nullptr, 0);
                ctx->HSSetShader(hs, nullptr, 0);
                ctx->VSSetShader(vs, nullptr, 0);
                ctx->IASetPrimitiveTopology(topo);
                ctx->IASetInputLayout(il);
                auto rel = [](auto*& p) {
                    if (p) {
                        p->Release();
                        p = nullptr;
                    }
                };
                rel(il), rel(vs), rel(hs), rel(ds), rel(gs), rel(ps), rel(smp), rel(cb), rel(rs), rel(bs), rel(dss), rel(dsv);
                for (auto& s : srv) rel(s);
                for (auto& r : rtv) rel(r);
            }
            Saved(const Saved&) = delete;
            Saved& operator=(const Saved&) = delete;
        };

        void WriteBmp(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex, const std::string& a_path, bool a_alpha = false)
        {
            D3D11_TEXTURE2D_DESC d{};
            a_tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(a_dev->CreateTexture2D(&d, nullptr, &st))) {
                return;
            }
            a_ctx->CopyResource(st.Get(), a_tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(a_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                return;
            }
            const UINT       w = d.Width, h = d.Height, rowBytes = w * 4;
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + rowBytes * h;
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(w);
            ih.biHeight = -static_cast<LONG>(h);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            HANDLE f = ::CreateFileA(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                ::WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
                ::WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
                std::vector<std::uint8_t> row(rowBytes);
                for (UINT y = 0; y < h; ++y) {
                    const auto* src = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) {
                        if (a_alpha) {
                            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = src[x * 4 + 3];
                        } else {
                            row[x * 4 + 0] = src[x * 4 + 2], row[x * 4 + 1] = src[x * 4 + 1], row[x * 4 + 2] = src[x * 4 + 0];
                        }
                        row[x * 4 + 3] = 255;
                    }
                    ::WriteFile(f, row.data(), rowBytes, &wr, nullptr);
                }
                ::CloseHandle(f);
                REX::INFO("scope cap: wrote {} ({}x{})", a_path, w, h);
            }
            a_ctx->Unmap(st.Get(), 0);
        }

        // The round scope image: centre square of the eye region, ScopeMenu overlay on top, black outside the circle.
        ID3D11Texture2D* Compose(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_bb, unsigned a_x, unsigned a_w, unsigned a_h, ID3D11Texture2D* a_menu,
            bool a_premul, const Cfg& c)
        {
            ComPtr<ID3D11Device> dev;
            a_ctx->GetDevice(&dev);
            if (!CreateGpu(dev.Get())) {
                return nullptr;
            }
            D3D11_TEXTURE2D_DESC bd{};
            a_bb->GetDesc(&bd);
            const UINT side = (std::min)(a_w, a_h);
            if (side < 16 || bd.SampleDesc.Count != 1) {
                g_lastErr = "eye region / back buffer unusable";
                return nullptr;
            }
            if (!g_gpu.sq || g_gpu.sqSide != side || g_gpu.sqFmt != bd.Format) {
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = sd.Height = side;
                sd.MipLevels = sd.ArraySize = 1;
                sd.Format = bd.Format;
                sd.SampleDesc.Count = 1;
                sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                g_gpu.sq.Reset();
                g_gpu.sqSrv.Reset();
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = ViewFormat(bd.Format);
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_gpu.sq)) || FAILED(dev->CreateShaderResourceView(g_gpu.sq.Get(), &vd, &g_gpu.sqSrv))) {
                    g_lastErr = "square texture";
                    g_gpu.sq.Reset();
                    return nullptr;
                }
                g_gpu.sqSide = side;
                g_gpu.sqFmt = bd.Format;
                REX::INFO("scope: eye square {}x{} format {}", side, side, static_cast<int>(bd.Format));
            }
            const UINT out = (std::min)(side, 2048u);
            if (!g_gpu.out || g_gpu.outSide != out) {
                D3D11_TEXTURE2D_DESC od{};
                od.Width = od.Height = out;
                od.MipLevels = od.ArraySize = 1;
                od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                od.SampleDesc.Count = 1;
                od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                g_gpu.out.Reset();
                g_gpu.outRtv.Reset();
                if (FAILED(dev->CreateTexture2D(&od, nullptr, &g_gpu.out)) || FAILED(dev->CreateRenderTargetView(g_gpu.out.Get(), nullptr, &g_gpu.outRtv))) {
                    g_lastErr = "output texture";
                    g_gpu.out.Reset();
                    return nullptr;
                }
                g_gpu.outSide = out;
            }
            const D3D11_BOX box{ a_x + (a_w - side) / 2, (a_h - side) / 2, 0, a_x + (a_w - side) / 2 + side, (a_h - side) / 2 + side, 1 };
            a_ctx->CopySubresourceRegion(g_gpu.sq.Get(), 0, 0, 0, 0, a_bb, 0, &box);
            ID3D11ShaderResourceView* menuSrv = nullptr;
            float                     mu = 1.0f, mv = 1.0f;
            bool                      gameOverlay = false;
            if (c.overlaySource > 0) {
                // the game's own scope overlay (ScopeMenu renderer target, copied at H1 this frame or a recent one)
                const int i = c.overlaySource - 1;
                if (g_ov.srv[i] && g_frames.load() - g_ov.frame[i] <= 3) {
                    D3D11_TEXTURE2D_DESC md{};
                    g_ov.tex[i]->GetDesc(&md);
                    menuSrv = g_ov.srv[i].Get();
                    mv = c.overlayScale;
                    mu = c.overlayScale * static_cast<float>(md.Height) / static_cast<float>((std::max)(1u, md.Width));
                    gameOverlay = true;
                    a_premul = true;
                    ++g_ovUsed;
                } else {
                    ++g_ovMissing;
                }
                a_menu = nullptr;
            }
            if (a_menu) {
                if (a_menu != g_gpu.menuKey) {
                    g_gpu.menuKey = nullptr;
                    D3D11_TEXTURE2D_DESC md{};
                    a_menu->GetDesc(&md);
                    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                    vd.Format = ViewFormat(md.Format);
                    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    vd.Texture2D.MipLevels = 1;
                    g_gpu.menuSrv.Reset();
                    if ((md.BindFlags & D3D11_BIND_SHADER_RESOURCE) && SUCCEEDED(dev->CreateShaderResourceView(a_menu, &vd, &g_gpu.menuSrv))) {
                        g_gpu.menuKey = a_menu;
                    }
                }
                if (g_gpu.menuKey) {
                    D3D11_TEXTURE2D_DESC md{};
                    a_menu->GetDesc(&md);
                    menuSrv = g_gpu.menuSrv.Get();
                    // the ScopeMenu circle: centred, diameter = overlay height x fOverlayScale
                    mv = c.overlayScale;
                    mu = c.overlayScale * static_cast<float>(md.Height) / static_cast<float>((std::max)(1u, md.Width));
                }
            }
            const bool  crosshair = c.reticle == 2 || (c.reticle == 1 && !gameOverlay);
            const float cbv[8] = { mu, mv, 0.0f, 0.0f, a_premul ? 1.0f : 0.0f, menuSrv ? 1.0f : 0.0f, 0.01f, crosshair ? 1.0f : 0.0f };
            {
                Saved sv(a_ctx);
                a_ctx->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, cbv, 0, 0);
                ID3D11RenderTargetView* rtv = g_gpu.outRtv.Get();
                a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                a_ctx->OMSetBlendState(g_gpu.bs.Get(), nullptr, 0xFFFFFFFF);
                a_ctx->OMSetDepthStencilState(g_gpu.dss.Get(), 0);
                const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(out), static_cast<float>(out), 0.0f, 1.0f };
                a_ctx->RSSetViewports(1, &vp);
                a_ctx->RSSetState(g_gpu.rs.Get());
                a_ctx->IASetInputLayout(nullptr);
                a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                a_ctx->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
                a_ctx->HSSetShader(nullptr, nullptr, 0);
                a_ctx->DSSetShader(nullptr, nullptr, 0);
                a_ctx->GSSetShader(nullptr, nullptr, 0);
                a_ctx->PSSetShader(g_gpu.ps.Get(), nullptr, 0);
                ID3D11ShaderResourceView* srvs[2] = { g_gpu.sqSrv.Get(), menuSrv };
                a_ctx->PSSetShaderResources(0, 2, srvs);
                ID3D11SamplerState* smp = g_gpu.smp.Get();
                a_ctx->PSSetSamplers(0, 1, &smp);
                ID3D11Buffer* cb = g_gpu.cb.Get();
                a_ctx->PSSetConstantBuffers(0, 1, &cb);
                a_ctx->Draw(3, 0);
            }
            std::string cap;
            {
                std::scoped_lock l(g_capLock);
                cap.swap(g_capPath);
            }
            if (!cap.empty()) {
                WriteBmp(dev.Get(), a_ctx, g_gpu.out.Get(), cap);
                if (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    WriteBmp(dev.Get(), a_ctx, a_bb, cap + "_bb.bmp");  // the whole stereo frame (is the overlay drawn into it?)
                }
                for (int i = 0; i < 2; ++i) {
                    if (g_ov.tex[i]) {
                        WriteBmp(dev.Get(), a_ctx, g_ov.tex[i].Get(), cap + (i ? "_ov3F.bmp" : "_ov3E.bmp"));
                        WriteBmp(dev.Get(), a_ctx, g_ov.tex[i].Get(), cap + (i ? "_ov3Falpha.bmp" : "_ov3Ealpha.bmp"), true);
                    }
                }
                if (a_menu) {
                    // the overlay copy alone, for checking the circle's size (fOverlayScale)
                    D3D11_TEXTURE2D_DESC md{};
                    a_menu->GetDesc(&md);
                    if (md.Format == DXGI_FORMAT_R8G8B8A8_UNORM || md.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || md.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                        WriteBmp(dev.Get(), a_ctx, a_menu, cap + "_menu.bmp");
                        WriteBmp(dev.Get(), a_ctx, a_menu, cap + "_menualpha.bmp", true);
                    }
                }
            }
            return g_gpu.out.Get();
        }
    }

    void Install()
    {
        {
            std::scoped_lock l(g_cfgLock);
            g_cfg = LoadCfg();
        }
        cfgstore::Subscribe("scope", "Scope", [](const cfgstore::Changes&) {
            const Cfg c = LoadCfg();
            {
                std::scoped_lock l(g_cfgLock);
                g_cfg = c;
            }
            REX::INFO("scope: settings reloaded: bEnabled={} view {:.0f} deg at {:.2f} m, fade x{:.2f}, zoom x{:.2f}, overlay x{:.2f}, hold breath grip {}",
                c.enabled ? 1 : 0, c.viewDeg, c.viewDistance, c.fadeMagnitude, c.zoomScale, c.overlayScale, c.holdBreathGrip ? 1 : 0);
        });
        tq::AddMainTick(&detail::MainTick, "scope::MainTick");
        const Cfg c = CurCfg();
        REX::INFO("scope: installed ([Scope] bEnabled={}): FO4VR raise-to-eye scope, view {:.0f} deg at {:.2f} m, fade x{:.2f}", c.enabled ? 1 : 0,
            c.viewDeg, c.viewDistance, c.fadeMagnitude);
    }

    bool Enabled()
    {
        return CurCfg().enabled;
    }

    void Update(const xr::Pose& a_head, bool a_headValid, const xr::Pose& a_aim, bool a_aimValid, bool a_gameplay, bool a_blocked,
        float a_unitsPerMeter, bool a_scopeMenuOpen, bool a_stereo)
    {
        const Cfg  c = CurCfg();
        const auto w = detail::Weapon();
        g_stereo = a_stereo;
        if (a_aimValid) {
            g_aimForEyes = a_aim;
        }
        g_upm = a_unitsPerMeter > 1.0f ? a_unitsPerMeter : 70.0f;
        bool raised = g_raised.load();
        float fade = 0.0f;
        const bool gate = c.enabled && w.scopedGun && a_gameplay && !a_blocked && g_stereo.load();
        if (gate && !(a_headValid && a_aimValid)) {
            fade = g_fade.load() / (std::max)(c.fadeMagnitude, 1e-3f);  // a frame without poses (no xrBeginFrame): keep the state
        } else if (gate) {
            const float upm = a_unitsPerMeter > 1.0f ? a_unitsPerMeter : 70.0f;
            const V3 hp = P(a_head.position);
            const V3 hf = Rot(a_head.orientation, { 0.0f, 0.0f, -1.0f });
            const V3 eye = Add(hp, Rot(a_head.orientation, { 0.0f, 0.0f, kHmdOffsetBack / upm }));     // fHMDScopeOffset
            const V3 af = Rot(a_aim.orientation, { 0.0f, 0.0f, -1.0f });
            // fWeaponScopeOffset (0, 0, 8), per weapon by FUN_140f0b220 (weapon frame X right, Z up -> aim pose X, Y)
            const V3 sp = Add(P(a_aim.position), Rot(a_aim.orientation, { w.scopeOffX / upm, w.scopeOffZ / upm, 0.0f }));
            const V3 d = Sub(sp, eye);
            const float distU = Len(d) * upm;
            const V3 dir = Len(d) > 1e-5f ? Mul(d, 1.0f / Len(d)) : hf;
            const float widen = std::pow(kWidening / (std::max)(distU, 1.0f), kWideningExp);
            const float aH = AngleDeg(dir, hf), aW = AngleDeg(dir, af);
            g_last[0] = distU, g_last[1] = aH, g_last[2] = aW, g_last[3] = (raised ? kWpnConeExit : kWpnConeEnter) * widen;
            const bool in = aH < (raised ? kHmdConeExit : kHmdConeEnter) && aW < (raised ? kWpnConeExit : kWpnConeEnter) * widen &&
                            distU < (raised ? kDistExit : kDistEnter);
            raised = in;
            fade = (std::min)({ Ramp(aH, kHmdConeEnter, kHmdConeFade), Ramp(aW, kWpnConeEnter * widen, kWpnConeFade * widen),
                Ramp(distU, kDistEnter, kDistFade) });
        } else {
            raised = false;
        }
        if (g_force != 0) {
            raised = g_force > 0 && c.enabled && w.scopedGun;
            fade = raised ? 1.0f : 0.0f;
        }
        if (raised != g_raised.load()) {
            g_raised = raised;
            if (raised) {
                ++g_enters;
                REX::INFO("scope: raised to the eye (dist {:.1f} u, hmd {:.1f} deg, weapon {:.1f} / {:.1f} deg) -> aim held, weapon {:08X} zoom x{:.2f}",
                    g_last[0], g_last[1], g_last[2], g_last[3], w.form, w.fovMult);
            } else {
                ++g_exits;
                g_exitAt = Clock::now();
                REX::INFO("scope: lowered -> aim released");
            }
        }
        // not scoped (the game has not opened ScopeMenu yet / refused the aim): never a fully black world
        const bool view = raised && a_scopeMenuOpen && a_stereo;
        g_fade = std::clamp(fade * c.fadeMagnitude, 0.0f, view ? 1.0f : 0.85f);
        if (view != g_viewActive.load()) {
            g_viewActive = view;
            REX::INFO("scope: view {} (ScopeMenu {})", view ? "on: the stereo render is the scope camera" : "off", a_scopeMenuOpen ? "open" : "closed");
        }
        const bool inWorld = c.enabled && a_stereo &&
                             (raised || (a_scopeMenuOpen && Clock::now() - g_exitAt < std::chrono::milliseconds(1500)));
        g_menuInWorld = inWorld;
        g_menuCopy = inWorld && a_scopeMenuOpen;
        detail::SetWantCull(raised);
    }

    void NoteOverlayRenderer(int a_target, int a_mode60, int a_mode68, int a_mode6C, int a_rt1C0, int a_rt1C4)
    {
        static std::string s_last;  // render thread
        auto line = std::format("target 0x{:X} (+60 {} +68 {} +6C {} +1C0 {} +1C4 {})", a_target, a_mode60, a_mode68, a_mode6C, a_rt1C0, a_rt1C4);
        if (line != s_last) {
            REX::INFO("scope: ScopeMenu renderer {}", line);
            s_last = line;
        }
    }

    bool SkipScopeMenuDraw()
    {
        return g_menuInWorld.load(std::memory_order_relaxed) && CurCfg().overlaySource > 0;
    }

    bool WantOverlayTargets()
    {
        return g_ovDiag.load(std::memory_order_relaxed) || (g_menuInWorld.load(std::memory_order_relaxed) && CurCfg().overlaySource > 0);
    }

    void OnOverlayTargets(void* a_glassTex, void* a_movieTex)
    {
        void* src[2] = { a_glassTex, a_movieTex };
        for (int i = 0; i < 2; ++i) {
            auto* t = static_cast<ID3D11Texture2D*>(src[i]);
            if (!t) continue;
            D3D11_TEXTURE2D_DESC sd{};
            t->GetDesc(&sd);
            if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1 || sd.MipLevels != 1) continue;
            ComPtr<ID3D11Device> dev;
            t->GetDevice(&dev);
            ComPtr<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC md{};
            if (g_ov.tex[i]) g_ov.tex[i]->GetDesc(&md);
            if (!g_ov.tex[i] || md.Width != sd.Width || md.Height != sd.Height || md.Format != sd.Format) {
                D3D11_TEXTURE2D_DESC nd = sd;
                nd.Usage = D3D11_USAGE_DEFAULT;
                nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                nd.CPUAccessFlags = 0;
                nd.MiscFlags = 0;
                g_ov.tex[i].Reset();
                g_ov.srv[i].Reset();
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = ViewFormat(sd.Format);
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateTexture2D(&nd, nullptr, &g_ov.tex[i])) || FAILED(dev->CreateShaderResourceView(g_ov.tex[i].Get(), &vd, &g_ov.srv[i]))) {
                    g_ov.tex[i].Reset();
                    g_ov.srv[i].Reset();
                    continue;
                }
                REX::INFO("scope: game overlay copy {} {}x{} format {}", i ? "0x3F" : "0x3E", sd.Width, sd.Height, static_cast<int>(sd.Format));
            }
            ctx->CopyResource(g_ov.tex[i].Get(), t);
            g_ov.frame[i] = g_frames.load();
            std::string cap;
            {
                std::scoped_lock l(g_capLock);
                if (i == 1) cap.swap(g_ovCapPath);
            }
            if (!cap.empty() && (sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
                WriteBmp(dev.Get(), ctx.Get(), g_ov.tex[i].Get(), cap);
                WriteBmp(dev.Get(), ctx.Get(), g_ov.tex[i].Get(), cap + "_alpha.bmp", true);
            }
        }
        ++g_ov.copies;
    }

    bool HoldAim()
    {
        return g_raised.load();
    }

    bool GripHoldsBreath()
    {
        return g_raised.load() && CurCfg().holdBreathGrip;
    }

    bool ScopeMenuInWorld()
    {
        return g_menuInWorld.load();
    }

    bool ViewActive()
    {
        return g_viewActive.load();
    }

    bool WantMenuCopy()
    {
        return g_menuCopy.load() && !g_noCopy.load();
    }

    float Fade()
    {
        return g_viewActive.load() ? 0.0f : g_fade.load();
    }

    void FillEyes(stereo::EyeFrame& a_ef)
    {
        Pub p;
        if (a_ef.valid && g_viewActive.load() && !g_noEyes.load()) {
            const Cfg c = CurCfg();
            const float fov = ScopeFovRad(c);
            const double t = std::tan(fov * 0.5);
            p.scope = true;
            p.fovH = fov;
            p.head.orientation = { a_ef.headQ[0], a_ef.headQ[1], a_ef.headQ[2], a_ef.headQ[3] };
            p.head.position = { a_ef.headP[0], a_ef.headP[1], a_ef.headP[2] };
            p.cam.orientation = g_aimForEyes.orientation;
            p.aim = g_aimForEyes;
            p.upm = g_upm;
            p.cam.position = p.head.position;
            for (int e = 0; e < 2; ++e) {
                // keep the eye region's pixel aspect: tan range ratio of the headset eye
                const double w = std::tan(a_ef.fov[e][1]) - std::tan(a_ef.fov[e][0]);
                const double h = std::tan(a_ef.fov[e][2]) - std::tan(a_ef.fov[e][3]);
                const double r = w > 1e-6 ? h / w : 1.0;
                const double th = r >= 1.0 ? t : t / r, tv = r >= 1.0 ? t * r : t;  // the circle spans the shorter side
                a_ef.fov[e][0] = static_cast<float>(-std::atan(th));
                a_ef.fov[e][1] = static_cast<float>(std::atan(th));
                a_ef.fov[e][2] = static_cast<float>(std::atan(tv));
                a_ef.fov[e][3] = static_cast<float>(-std::atan(tv));
                a_ef.eyeQ[e][0] = p.cam.orientation.x, a_ef.eyeQ[e][1] = p.cam.orientation.y;
                a_ef.eyeQ[e][2] = p.cam.orientation.z, a_ef.eyeQ[e][3] = p.cam.orientation.w;
                a_ef.eyeP[e][0] = p.cam.position.x, a_ef.eyeP[e][1] = p.cam.position.y, a_ef.eyeP[e][2] = p.cam.position.z;
            }
            ++g_scopeFrames;
        }
        g_frames.fetch_add(1);
        if (p.scope != g_pubNow.scope) {
            dlss::Command("reset");  // the eyes jump: no DLSS history across the cut (no-op when DLSS is off)
        }
        g_pubNow = p;
    }

    bool PresentedIsScope()
    {
        return g_pubNow.scope;
    }

    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_bb, unsigned a_x, unsigned a_w, unsigned a_h, ID3D11Texture2D* a_menu,
        bool a_menuPremultiplied)
    {
        const Pub p = g_pubNow;
        if (!p.scope || !a_ctx || !a_bb) {
            return false;
        }
        const Cfg c = CurCfg();
        if (!a_menu) {
            ++g_noMenuFrames;
        }
        ID3D11Texture2D* img = Compose(a_ctx, a_bb, a_x, a_w, a_h, a_menu, a_menuPremultiplied, c);
        if (!img) {
            ++g_submitFails;
            return false;
        }
        V3    pos{};
        float width = 0.0f;
        if (c.viewDeg > 0.0f && c.viewDistance > 0.0f) {
            // override: fixed angular size along the aim ray from the eye
            const V3 f = Rot(p.cam.orientation, { 0.0f, 0.0f, -1.0f });
            pos = Add(P(p.head.position), Mul(f, c.viewDistance));
            width = 2.0f * c.viewDistance * static_cast<float>(std::tan(c.viewDeg * 0.5 * kRad));
        } else {
            // FO4VR: world_scope.nif under ScopeParent = PrimaryUIAttachNode + (0, 0, fScopeOffsetZ 10), rotation 0, fScopeScale 1
            // (VR EF3A9C / EF5AC0); its render_circle (radius 7.852 u) faces along the gun: the lens on the gun at the hand
            pos = Add(P(p.aim.position), Rot(p.aim.orientation, { 0.0f, c.lensUp / p.upm, 0.0f }));
            width = 2.0f * 7.852f * c.lensScale / p.upm;
        }
        const xr::Pose qp{ p.cam.orientation, { pos.x, pos.y, pos.z } };
        if (!a_rt.SubmitWorldQuad(a_ctx, img, qp, width, false, true, 3)) {
            ++g_submitFails;
            g_lastErr = "SubmitWorldQuad: " + a_rt.lastError();
            return false;
        }
        ++g_submits;
        return true;
    }

    std::string Status()
    {
        const Cfg c = CurCfg();
        return std::format("scope enabled={} raised={} viewActive={} menuInWorld={} menuCopy={} stereo={} force={} fade={:.2f} test dist={:.1f}u hmd={:.1f}deg "
                           "weapon={:.1f}/{:.1f}deg | enters={} exits={} scopeFrames={} submits={} submitFails={} noMenuFrames={} scopeFovDeg={:.1f} "
                           "view={:.0f}deg@{:.2f}m (0 = FO4VR lens) overlay src={} used={} missing={} copies={} | {}{}",
            c.enabled ? 1 : 0, g_raised.load() ? 1 : 0, g_viewActive.load() ? 1 : 0, g_menuInWorld.load() ? 1 : 0, g_menuCopy.load() ? 1 : 0,
            g_stereo.load() ? 1 : 0, g_force, g_fade.load(), g_last[0], g_last[1], g_last[2], g_last[3], g_enters, g_exits, g_scopeFrames, g_submits,
            g_submitFails, g_noMenuFrames, ScopeFovRad(c) / kRad, c.viewDeg, c.viewDistance, c.overlaySource, g_ovUsed, g_ovMissing, g_ov.copies, detail::GameStatus(),
            g_lastErr.empty() ? "" : " err='" + g_lastErr + "'");
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg == "on" || a_arg == "off" || a_arg == "test") {
            g_force = a_arg == "on" ? 1 : a_arg == "off" ? -1 : 0;
            return "scope force " + a_arg + " | " + Status();
        }
        if (a_arg == "eyes 0" || a_arg == "eyes 1") {
            g_noEyes = a_arg == "eyes 0";
            return Status();
        }
        if (a_arg == "ovdiag 0" || a_arg == "ovdiag 1") {
            g_ovDiag = a_arg == "ovdiag 1";
            return Status();
        }
        if (a_arg.rfind("ovcap ", 0) == 0) {
            std::scoped_lock l(g_capLock);
            g_ovCapPath = a_arg.substr(6);
            return "scope ovcap: next ScopeMenu overlay copy -> " + g_ovCapPath;
        }
        if (a_arg == "copy 0" || a_arg == "copy 1") {
            g_noCopy = a_arg == "copy 0";
            return Status();
        }
        if (a_arg.rfind("cap ", 0) == 0) {
            std::scoped_lock l(g_capLock);
            g_capPath = a_arg.substr(4);
            return "scope cap: next scope frame -> " + g_capPath;
        }
        return Status();
    }
}
