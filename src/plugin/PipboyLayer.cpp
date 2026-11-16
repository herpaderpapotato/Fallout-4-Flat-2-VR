// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/PipboyLayer.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/GamePipboy.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace fo4flatvr::pipboy
{
    namespace
    {
        struct Copy
        {
            ComPtr<ID3D11Texture2D> tex;
            mutable ComPtr<ID3D11ShaderResourceView> srv;  // for the tint pass (created on first use)
            D3D11_TEXTURE2D_DESC    desc{};
            bool                    fresh{ false };  // copied since the last Present
            std::uint64_t           count{ 0 };
        };

        Config                      g_cfg;
        std::mutex                  g_lock;  // g_cfg, capture path/result, status strings
        std::atomic<bool>           g_enabled{ true };
        std::atomic<bool>           g_projEnabled{ false };  // [PowerArmor] bProjectedPipboy
        std::atomic<bool>           g_floatEnabled{ false }; // [Pipboy] bFloating
        std::atomic<int>            g_source{ 0x3D };
        std::atomic<bool>           g_wrist{ false };      // wrist Pip-Boy open (game side), updated every Present
        std::atomic<bool>           g_capturePending{ false };
        bool                        g_armed = false;      // last H1 was renderer "PipboyMenu" (render thread)
        Copy                        g_copy3C, g_copy3D;
        ComPtr<ID3D11Texture2D>     g_disp;               // shown copy (+ cursor dot)
        ComPtr<ID3D11RenderTargetView> g_dispRtv;
        D3D11_TEXTURE2D_DESC        g_dispDesc{};
        ComPtr<ID3D11Device>        g_dev;
        ComPtr<ID3D11DeviceContext> g_ctx;
        std::string                 g_capturePath;
        std::string                 g_lastCapture{ "none" };

        // Per-frame placement / pointer (Presenter thread; Status() copies under g_lock)
        bool          g_poseValid = false;
        xr::Pose      g_pose;            // quad centre/orientation in the reference space
        float         g_quadW = 0.2f, g_quadH = 0.16f;
        bool          g_onQuad = false;
        float         g_u = 0.0f, g_v = 0.0f, g_hitDist = 0.0f;
        bool          g_trigDown = false;
        int           g_wheelDir = 0;
        std::chrono::steady_clock::time_point g_nextWheel{};
        std::atomic<std::uint64_t> g_submits{ 0 }, g_frames{ 0 }, g_hits{ 0 }, g_clicks{ 0 }, g_wheels{ 0 };
        std::string   g_poseLine{ "none" };
        std::string   g_lastHitLine{ "none" };
        std::string   g_aimLine{ "none" };

        // Tint pass ([Pipboy] bTint): the shown texture = luminance(source) x Pip-Boy colour x brightness, exactly the
        // last two instructions of the game's PipboyScreen image-space shader (fxp group 100: dp3 (0.3, 0.59, 0.11),
        // mul cb2[3].xyz, mul cb2[1].w). Its scanline/blur terms (0.6 x src + 0.016 x 5x5 box) average to 1 and are
        // not reproduced. Colour from game::PipboyScreenColor (main thread), refreshed every 30 frames.
        struct TintRes
        {
            ComPtr<ID3D11VertexShader>    vs;
            ComPtr<ID3D11PixelShader>     ps;
            ComPtr<ID3D11Buffer>          cb;
            ComPtr<ID3D11RasterizerState> rs;
            bool                          tried{ false };
            bool                          ok{ false };
        };
        TintRes           g_tintRes;
        std::atomic<bool> g_tintOn{ true };
        float             g_tintRgb[3]{ 0.08f, 1.0f, 0.09f };  // the game's defaults until read
        float             g_tintBright = 1.0f;
        std::atomic<float> g_tintBrightRender{ 1.0f };  // H1 for renderer "PipboyMenu" (see game::PipboyScreenBrightness)
        bool              g_tintFromGame = false;
        float             g_tintCbRgb[3]{ -1.0f, -1.0f, -1.0f };  // colour last written to the constant buffer
        std::atomic<std::uint64_t> g_tintDraws{ 0 };

        constexpr char kTintHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer C : register(b0) { float4 tint; };
void vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float4 c = src.Load(int3(pos.xy, 0));
    float  l = dot(c.rgb, float3(0.3, 0.59, 0.11));
    return float4(l * tint.rgb, c.a);
}
)";

        bool CreateTintRes(ID3D11Device* a_dev)
        {
            auto& t = g_tintRes;
            if (t.tried) {
                return t.ok;
            }
            t.tried = true;
            ComPtr<ID3DBlob> vsb, psb, err;
            if (FAILED(D3DCompile(kTintHlsl, sizeof(kTintHlsl) - 1, "pipboy_tint", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
                FAILED(D3DCompile(kTintHlsl, sizeof(kTintHlsl) - 1, "pipboy_tint", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
                REX::ERROR("pipboy tint: shader compile failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                return false;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            if (FAILED(a_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &t.vs)) ||
                FAILED(a_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &t.ps)) ||
                FAILED(a_dev->CreateBuffer(&bd, nullptr, &t.cb)) || FAILED(a_dev->CreateRasterizerState(&rd, &t.rs))) {
                REX::ERROR("pipboy tint: resource creation failed");
                return false;
            }
            t.ok = true;
            REX::INFO("pipboy tint: shaders ready");
            return true;
        }

        // Saves exactly the immediate-context bindings the tint draw changes and puts them back (the engine's
        // state cache assumes its own bindings).
        struct SavedState
        {
            ID3D11DeviceContext*      ctx;
            ID3D11InputLayout*        il{};
            D3D11_PRIMITIVE_TOPOLOGY  topo{};
            ID3D11VertexShader*       vs{};
            ID3D11HullShader*         hs{};
            ID3D11DomainShader*       ds{};
            ID3D11GeometryShader*     gs{};
            ID3D11PixelShader*        ps{};
            ID3D11ShaderResourceView* srv{};
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

            explicit SavedState(ID3D11DeviceContext* a) :
                ctx(a)
            {
                ctx->IAGetInputLayout(&il);
                ctx->IAGetPrimitiveTopology(&topo);
                ctx->VSGetShader(&vs, nullptr, nullptr);
                ctx->HSGetShader(&hs, nullptr, nullptr);
                ctx->DSGetShader(&ds, nullptr, nullptr);
                ctx->GSGetShader(&gs, nullptr, nullptr);
                ctx->PSGetShader(&ps, nullptr, nullptr);
                ctx->PSGetShaderResources(0, 1, &srv);
                ctx->PSGetConstantBuffers(0, 1, &cb);
                ctx->RSGetState(&rs);
                ctx->RSGetViewports(&nvp, vps);
                ctx->OMGetBlendState(&bs, bf, &sm);
                ctx->OMGetDepthStencilState(&dss, &sref);
                ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
            }
            ~SavedState()
            {
                ID3D11ShaderResourceView* nullSrv = nullptr;
                ctx->PSSetShaderResources(0, 1, &nullSrv);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
                ctx->OMSetDepthStencilState(dss, sref);
                ctx->OMSetBlendState(bs, bf, sm);
                ctx->RSSetViewports(nvp, vps);
                ctx->RSSetState(rs);
                ctx->PSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetShaderResources(0, 1, &srv);
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
                rel(il), rel(vs), rel(hs), rel(ds), rel(gs), rel(ps), rel(srv), rel(cb), rel(rs), rel(bs), rel(dss), rel(dsv);
                for (auto& r : rtv) {
                    rel(r);
                }
            }
            SavedState(const SavedState&) = delete;
            SavedState& operator=(const SavedState&) = delete;
        };

        // Draws the tinted source into g_disp. False = not done (caller copies untinted).
        bool DrawTinted(ID3D11DeviceContext* a_ctx, const Copy& a_src);

        // ---- math (OpenXR conventions) ------------------------------------------------------------
        xr::Quat Mul(const xr::Quat& a, const xr::Quat& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }
        xr::Quat Conj(const xr::Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            const xr::Quat p{ v.x, v.y, v.z, 0.0f };
            const xr::Quat r = Mul(Mul(q, p), Conj(q));
            return { r.x, r.y, r.z };
        }
        xr::Quat AxisAngle(float x, float y, float z, float deg)
        {
            const float h = deg * 0.5f * 3.14159265f / 180.0f, s = std::sin(h);
            return { x * s, y * s, z * s, std::cos(h) };
        }
        float Dot(const xr::Vec3& a, const xr::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        xr::Vec3 Sub(const xr::Vec3& a, const xr::Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

        // ---- projected Pip-Boy (power armour) -----------------------------------------------------
        // FO4VR (analysis/vr-power-armor.md): in power armour the Pip-Boy screen hangs on UprightHmdLagNode, an
        // upright node that follows the HMD with lag. Here: yaw-only (upright) frame from the head, the quad at
        // (0, h, -d) in it, tilted about X to face the eye; position and yaw follow the head with time constant
        // fPipboyFollowSeconds (0 = head-locked). The anchor snaps to the head when the session starts.
        std::atomic<bool> g_projected{ false };   // projected session open (Presenter thread; read by H2)
        bool              g_projValid = false;    // g_projYaw/g_projPos initialised
        float             g_projYaw = 0.0f;       // radians about +Y (OpenXR: -Z forward)
        xr::Vec3          g_projPos{};            // smoothed head position
        std::chrono::steady_clock::time_point g_projLast{};
        std::atomic<std::uint64_t> g_projFrames{ 0 };

        bool ProjectedPose(const xr::Pose& a_head, const Config& a_cfg, bool a_smooth)
        {
            const xr::Vec3 f = Rot(a_head.orientation, { 0.0f, 0.0f, -1.0f });
            if (std::abs(f.x) + std::abs(f.z) < 1e-4f) {
                return g_projValid;  // looking straight up/down: keep the last yaw
            }
            const float yaw = std::atan2(-f.x, -f.z);
            const auto  now = std::chrono::steady_clock::now();
            if (!g_projValid || !a_smooth || a_cfg.projFollowSeconds <= 0.0f) {
                g_projYaw = yaw;
                g_projPos = a_head.position;
            } else {
                const float dt = std::clamp(std::chrono::duration<float>(now - g_projLast).count(), 0.0f, 0.25f);
                const float k = 1.0f - std::exp(-dt / a_cfg.projFollowSeconds);
                float       dy = yaw - g_projYaw;
                while (dy > 3.14159265f) {
                    dy -= 6.28318531f;
                }
                while (dy < -3.14159265f) {
                    dy += 6.28318531f;
                }
                g_projYaw += k * dy;
                g_projPos = { g_projPos.x + k * (a_head.position.x - g_projPos.x), g_projPos.y + k * (a_head.position.y - g_projPos.y),
                    g_projPos.z + k * (a_head.position.z - g_projPos.z) };
            }
            g_projLast = now;
            g_projValid = true;
            const float    h = a_cfg.projHeightMeters, d = a_cfg.projDistanceMeters;
            const xr::Quat qy{ 0.0f, std::sin(g_projYaw * 0.5f), 0.0f, std::cos(g_projYaw * 0.5f) };
            const float    tilt = std::atan2(h, d);
            const xr::Quat qx{ std::sin(tilt * 0.5f), 0.0f, 0.0f, std::cos(tilt * 0.5f) };
            const xr::Vec3 o = Rot(qy, { 0.0f, h, -d });
            g_pose.orientation = Mul(qy, qx);
            g_pose.position = { g_projPos.x + o.x, g_projPos.y + o.y, g_projPos.z + o.z };
            ++g_projFrames;
            return true;
        }

        // The configured source if it has been copied, else the other one (0x3C is only written when the
        // renderer runs its screen effect; 0x3D always holds the drawn menu).
        const Copy& Source()
        {
            const bool want3D = g_source.load() == 0x3D;
            const Copy& a = want3D ? g_copy3D : g_copy3C;
            const Copy& b = want3D ? g_copy3C : g_copy3D;
            return a.tex ? a : b;
        }

        DXGI_FORMAT Typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }

        void CopyTarget(ID3D11Texture2D* a_src, Copy& a_dst)
        {
            D3D11_TEXTURE2D_DESC sd{};
            a_src->GetDesc(&sd);
            if (sd.SampleDesc.Count > 1) {
                return;
            }
            if (!g_dev) {
                a_src->GetDevice(&g_dev);
                g_dev->GetImmediateContext(&g_ctx);
            }
            if (!a_dst.tex || a_dst.desc.Width != sd.Width || a_dst.desc.Height != sd.Height || a_dst.desc.Format != sd.Format) {
                a_dst.tex.Reset();
                a_dst.srv.Reset();
                D3D11_TEXTURE2D_DESC td = sd;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // tint pass source
                td.CPUAccessFlags = 0;
                td.MiscFlags = 0;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &a_dst.tex))) {
                    REX::ERROR("pipboy: CreateTexture2D({}x{} fmt {}) failed", sd.Width, sd.Height, static_cast<int>(sd.Format));
                    a_dst.desc = {};
                    return;
                }
                a_dst.desc = td;
                REX::INFO("pipboy: copy target {}x{} format {}", sd.Width, sd.Height, static_cast<int>(sd.Format));
            }
            g_ctx->CopySubresourceRegion(a_dst.tex.Get(), 0, 0, 0, 0, a_src, 0, nullptr);
            a_dst.fresh = true;
            ++a_dst.count;
        }

        // The displayed texture: copy of the source + cursor dot.
        ID3D11Texture2D* PrepareDisplay(ID3D11DeviceContext* a_ctx, const Copy& a_src, bool a_dot, float a_u, float a_v)
        {
            const DXGI_FORMAT fmt = Typed(a_src.desc.Format);
            if (!g_disp || g_dispDesc.Width != a_src.desc.Width || g_dispDesc.Height != a_src.desc.Height || g_dispDesc.Format != fmt) {
                g_disp.Reset();
                g_dispRtv.Reset();
                D3D11_TEXTURE2D_DESC td = a_src.desc;
                td.Format = fmt;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_disp)) || FAILED(g_dev->CreateRenderTargetView(g_disp.Get(), nullptr, &g_dispRtv))) {
                    REX::ERROR("pipboy: display texture {}x{} fmt {} creation failed", td.Width, td.Height, static_cast<int>(fmt));
                    g_disp.Reset();
                    g_dispRtv.Reset();
                    g_dispDesc = {};
                    return a_src.tex.Get();
                }
                g_dispDesc = td;
            }
            if (!(g_tintOn.load() && DrawTinted(a_ctx, a_src))) {
                a_ctx->CopyResource(g_disp.Get(), a_src.tex.Get());
            }
            if (a_dot) {
                ComPtr<ID3D11DeviceContext1> c1;
                if (SUCCEEDED(a_ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
                    const LONG cx = static_cast<LONG>(a_u * static_cast<float>(g_dispDesc.Width));
                    const LONG cy = static_cast<LONG>(a_v * static_cast<float>(g_dispDesc.Height));
                    const LONG r = (std::max)(3L, static_cast<LONG>(g_dispDesc.Width / 160));
                    const float outer[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    const float inner[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    const D3D11_RECT ro{ cx - r - 1, cy - r - 1, cx + r + 2, cy + r + 2 };
                    const D3D11_RECT ri{ cx - r, cy - r, cx + r + 1, cy + r + 1 };
                    c1->ClearView(g_dispRtv.Get(), outer, &ro, 1);
                    c1->ClearView(g_dispRtv.Get(), inner, &ri, 1);
                }
            }
            return g_disp.Get();
        }

        bool DrawTinted(ID3D11DeviceContext* a_ctx, const Copy& a_src)
        {
            if (!g_dispRtv || !g_dev || !CreateTintRes(g_dev.Get())) {
                return false;
            }
            if (!a_src.srv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = Typed(a_src.desc.Format);
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(g_dev->CreateShaderResourceView(a_src.tex.Get(), &vd, a_src.srv.ReleaseAndGetAddressOf()))) {
                    REX::ERROR("pipboy tint: SRV creation failed (fmt {}); showing untinted", static_cast<int>(vd.Format));
                    g_tintOn = false;
                    return false;
                }
            }
            g_tintBright = g_tintBrightRender.load();
            const float rgb[3] = { g_tintRgb[0] * g_tintBright, g_tintRgb[1] * g_tintBright, g_tintRgb[2] * g_tintBright };
            if (std::memcmp(rgb, g_tintCbRgb, sizeof(rgb)) != 0) {
                const float c[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
                a_ctx->UpdateSubresource(g_tintRes.cb.Get(), 0, nullptr, c, 0, 0);
                std::memcpy(g_tintCbRgb, rgb, sizeof(rgb));
            }
            SavedState              saved(a_ctx);
            ID3D11RenderTargetView* rtv = g_dispRtv.Get();
            a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            a_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            a_ctx->OMSetDepthStencilState(nullptr, 0);
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(g_dispDesc.Width), static_cast<float>(g_dispDesc.Height), 0.0f, 1.0f };
            a_ctx->RSSetViewports(1, &vp);
            a_ctx->RSSetState(g_tintRes.rs.Get());
            a_ctx->IASetInputLayout(nullptr);
            a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            a_ctx->VSSetShader(g_tintRes.vs.Get(), nullptr, 0);
            a_ctx->HSSetShader(nullptr, nullptr, 0);
            a_ctx->DSSetShader(nullptr, nullptr, 0);
            a_ctx->GSSetShader(nullptr, nullptr, 0);
            a_ctx->PSSetShader(g_tintRes.ps.Get(), nullptr, 0);
            ID3D11Buffer* cb = g_tintRes.cb.Get();
            a_ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* srv = a_src.srv.Get();
            a_ctx->PSSetShaderResources(0, 1, &srv);
            a_ctx->Draw(3, 0);
            ++g_tintDraws;
            return true;
        }

        float HalfToFloat(std::uint16_t h)
        {
            const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
            float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
            return s ? -v : v;
        }
        std::uint8_t ToByte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

        std::string ReadBack(const Copy& a_copy, std::vector<std::uint8_t>& a_bgra)
        {
            const auto&          d = a_copy.desc;
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &staging))) {
                return "staging creation failed";
            }
            g_ctx->CopyResource(staging.Get(), a_copy.tex.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                return "map failed";
            }
            const UINT w = d.Width, h = d.Height;
            a_bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
            bool ok = true;
            for (UINT y = 0; y < h && ok; ++y) {
                const auto*   row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
                std::uint8_t* out = a_bgra.data() + static_cast<std::size_t>(y) * w * 4;
                for (UINT x = 0; x < w; ++x) {
                    std::uint8_t r = 0, g = 0, b = 0, a = 0;
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
                            r = ToByte(HalfToFloat(v[0])), g = ToByte(HalfToFloat(v[1])), b = ToByte(HalfToFloat(v[2])), a = ToByte(HalfToFloat(v[3]));
                        }
                        break;
                    default:
                        ok = false;
                        break;
                    }
                    out[x * 4] = b, out[x * 4 + 1] = g, out[x * 4 + 2] = r, out[x * 4 + 3] = a;
                }
            }
            g_ctx->Unmap(staging.Get(), 0);
            if (!ok) {
                return std::format("unsupported format {}", static_cast<int>(d.Format));
            }
            // Statistics: lit pixels (max channel > 16), mean of R/G/B over the image, bbox of lit pixels, FNV-1a hash.
            std::size_t lit = 0;
            double      sum[3]{};
            UINT        x0 = w, y0 = h, x1 = 0, y1 = 0;
            std::uint64_t hash = 1469598103934665603ull;
            for (UINT y = 0; y < h; ++y) {
                for (UINT x = 0; x < w; ++x) {
                    const std::uint8_t* p = a_bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                    sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
                    for (int k = 0; k < 3; ++k) {
                        hash = (hash ^ p[k]) * 1099511628211ull;
                    }
                    if ((std::max)({ p[0], p[1], p[2] }) > 16) {
                        ++lit;
                        x0 = (std::min)(x0, x), y0 = (std::min)(y0, y), x1 = (std::max)(x1, x), y1 = (std::max)(y1, y);
                    }
                }
            }
            const double n = static_cast<double>(w) * h;
            return std::format("{}x{} fmt {} fresh={} copies={} lit {} ({:.2f}%) meanRGB {:.1f},{:.1f},{:.1f} bbox [{},{}..{},{}] hash {:016X}", w, h,
                static_cast<int>(d.Format), a_copy.fresh ? 1 : 0, a_copy.count, lit, 100.0 * static_cast<double>(lit) / n, sum[0] / n, sum[1] / n,
                sum[2] / n, lit ? x0 : 0, lit ? y0 : 0, x1, y1, hash);
        }

        void WriteBmp(const std::string& a_path, const std::vector<std::uint8_t>& a_bgra, UINT a_w, UINT a_h)
        {
            const std::uint32_t imageSize = a_w * a_h * 4;
            BITMAPFILEHEADER    fh{};
            BITMAPINFOHEADER    ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + imageSize;
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(a_w);
            ih.biHeight = -static_cast<LONG>(a_h);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = imageSize;
            HANDLE f = ::CreateFileA(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                REX::ERROR("pipcap: cannot open {} ({})", a_path, ::GetLastError());
                return;
            }
            DWORD written = 0;
            ::WriteFile(f, &fh, sizeof(fh), &written, nullptr);
            ::WriteFile(f, &ih, sizeof(ih), &written, nullptr);
            ::WriteFile(f, a_bgra.data(), imageSize, &written, nullptr);
            ::CloseHandle(f);
        }

        void ServiceCapture(const std::string& a_path)
        {
            std::string summary;
            std::string stem = a_path;
            if (stem.size() > 4 && _stricmp(stem.c_str() + stem.size() - 4, ".bmp") == 0) {
                stem.resize(stem.size() - 4);
            }
            const int src = &Source() == &g_copy3D ? 0x3D : 0x3C;
            for (int rt : { 0x3C, 0x3D }) {
                const Copy& c = rt == 0x3C ? g_copy3C : g_copy3D;
                if (!c.tex || !g_dev) {
                    summary += std::format(" {:X}: no copy yet;", rt);
                    continue;
                }
                std::vector<std::uint8_t> px;
                summary += std::format(" {:X}: {};", rt, ReadBack(c, px));
                if (px.empty()) {
                    continue;
                }
                WriteBmp(std::format("{}_{:x}.bmp", stem, rt), px, c.desc.Width, c.desc.Height);
                if (rt == src) {
                    WriteBmp(a_path, px, c.desc.Width, c.desc.Height);
                }
            }
            if (g_disp && g_dev) {  // the texture the quad shows (tint + cursor), as last prepared
                Copy shown;
                shown.tex = g_disp;
                shown.desc = g_dispDesc;
                shown.count = g_submits.load();
                std::vector<std::uint8_t> px;
                summary += std::format(" shown: {};", ReadBack(shown, px));
                if (!px.empty()) {
                    WriteBmp(stem + "_shown.bmp", px, g_dispDesc.Width, g_dispDesc.Height);
                }
            }
            REX::INFO("pipcap {}:{}", a_path, summary);
            std::scoped_lock l(g_lock);
            g_lastCapture = a_path + ":" + summary;
        }
    }

    void Configure(const Config& a_cfg)
    {
        {
            std::scoped_lock l(g_lock);
            g_cfg = a_cfg;
        }
        g_enabled = a_cfg.enabled;
        g_projEnabled = a_cfg.projected;
        g_floatEnabled = a_cfg.floating;
        g_source = a_cfg.source == 0x3C ? 0x3C : 0x3D;
        g_tintOn = a_cfg.tint;
    }

    void SetTint(bool a_on) { g_tintOn = a_on; }

    const Config& GetConfig() { return g_cfg; }

    void SetEnabled(bool a_on) { g_enabled = a_on; }

    bool Enabled() { return g_enabled.load() && game::PipboyHooksInstalled(); }

    void SetSource(int a_rt) { g_source = a_rt == 0x3D ? 0x3D : 0x3C; }

    bool WantsTargets() { return g_wrist.load(std::memory_order_relaxed) || g_capturePending.load(std::memory_order_relaxed); }

    void OnRendererPostFX(bool a_isPipboy, void* a_tex3D)
    {
        g_armed = a_isPipboy;
        if (a_isPipboy) {
            g_tintBrightRender = game::PipboyScreenBrightness();  // the Pip-Boy renderer's value, just written by DrawPostFX
        }
        if (a_isPipboy && a_tex3D) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3D), g_copy3D);
        }
    }

    void OnFinalTarget(void* a_tex3C)
    {
        if (!g_armed) {
            return;  // RenderEffect 0xC into 0x3C for some other renderer
        }
        g_armed = false;
        if (a_tex3C) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3C), g_copy3C);
        }
    }

    bool UpdateFrame(xr::Runtime& a_rt, bool a_began, const xr::Pose* a_head)
    {
        ++g_frames;
        const bool wrist = game::PipboyWristOpen();
        g_wrist = wrist;
        // Power armour: projected screen (FO4VR bUIPowerArmorGeometry_UseProjectedPipboy) instead of the wrist.
        const bool proj = wrist && game::PipboyProjected();
        const bool projChanged = proj != g_projected.load();
        g_projected = proj;
        if (g_tintOn.load() && (g_frames.load() % 30 == 1 || !g_tintFromGame || projChanged)) {
            float rgb[3], bright = 1.0f;
            if (game::PipboyScreenColor(rgb, bright, proj && game::PipboyProjectedInPowerArmor())) {  // in power armour: fPAEffectColorR/G/B like the game
                if (!g_tintFromGame || std::memcmp(rgb, g_tintRgb, sizeof(rgb)) != 0) {  // brightness animates: not logged
                    REX::INFO("pipboy tint: colour {:.3f},{:.3f},{:.3f} (brightness now {:.3f})", rgb[0], rgb[1], rgb[2], bright);
                }
                std::memcpy(g_tintRgb, rgb, sizeof(rgb));
                g_tintFromGame = true;
            }
        }
        const auto& c = a_rt.Controllers();
        Config      cfg;
        {
            std::scoped_lock l(g_lock);
            cfg = g_cfg;
        }
        const bool  rightHand = cfg.leftHanded;  // Pip-Boy hand
        const bool  gripOk = a_began && c.active && (rightHand ? c.rightGripPoseValid : c.leftGripPoseValid);
        const auto& grip = rightHand ? c.rightGripPose : c.leftGripPose;

        // Quad aspect from the shown source.
        const Copy& src = Source();
        g_quadW = cfg.widthMeters;
        g_quadH = src.desc.Width ? cfg.widthMeters * static_cast<float>(src.desc.Height) / static_cast<float>(src.desc.Width) : cfg.widthMeters * 0.8f;

        g_poseValid = gripOk;
        if (gripOk) {
            // Base wrist pose in grip space (OpenXR grip: +X normal to the palm pointing right for both hands,
            // -Z little finger -> thumb, +Y by the right-hand rule): screen normal out of the back of the hand,
            // image right toward the hand (left) / toward the elbow (right), image top away from the user with
            // the forearm held across the body.
            const xr::Quat base = rightHand ? xr::Quat{ 0.70710678f, 0.0f, 0.70710678f, 0.0f } : xr::Quat{ 0.70710678f, 0.0f, -0.70710678f, 0.0f };
            const xr::Quat user = Mul(Mul(AxisAngle(0, 1, 0, cfg.rotDeg[1]), AxisAngle(1, 0, 0, cfg.rotDeg[0])), AxisAngle(0, 0, 1, cfg.rotDeg[2]));
            xr::Vec3 off{ rightHand ? -cfg.offset[0] : cfg.offset[0], cfg.offset[1], cfg.offset[2] };
            const xr::Vec3 wo = Rot(grip.orientation, off);
            g_pose.orientation = Mul(Mul(grip.orientation, base), user);
            g_pose.position = { grip.position.x + wo.x, grip.position.y + wo.y, grip.position.z + wo.z };
        }
        if (proj) {
            g_quadW = cfg.projWidthMeters;
            g_quadH = src.desc.Width ? g_quadW * static_cast<float>(src.desc.Height) / static_cast<float>(src.desc.Width) : g_quadW * 0.8f;
            g_poseValid = a_began && a_head && ProjectedPose(*a_head, cfg, !projChanged);
        } else {
            g_projValid = false;
        }

        // Pointer: aim-hand ray against the quad.
        const bool  aimOk = a_began && c.active && (rightHand ? c.leftAimValid : c.rightAimValid);
        const auto& aim = rightHand ? c.leftAim : c.rightAim;
        const float trig = rightHand ? c.leftTrigger : c.rightTrigger;
        const float stickY = rightHand ? c.leftStickY : c.rightStickY;
        bool        on = false;
        if (wrist && g_poseValid && aimOk) {
            const xr::Vec3 d = Rot(aim.orientation, { 0.0f, 0.0f, -1.0f });
            const xr::Vec3 n = Rot(g_pose.orientation, { 0.0f, 0.0f, 1.0f });
            const float    den = Dot(d, n);
            if (std::abs(den) > 1e-4f) {
                const float t = Dot(Sub(g_pose.position, aim.position), n) / den;
                if (t > 0.0f && t < 3.0f) {
                    const xr::Vec3 hit{ aim.position.x + d.x * t, aim.position.y + d.y * t, aim.position.z + d.z * t };
                    const xr::Vec3 l = Rot(Conj(g_pose.orientation), Sub(hit, g_pose.position));
                    const float    u = l.x / g_quadW + 0.5f, v = 0.5f - l.y / g_quadH;
                    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                        on = true;
                        g_u = u, g_v = v, g_hitDist = t;
                        ++g_hits;
                    }
                }
            }
        }
        g_onQuad = on;
        // Trigger with hysteresis; a click needs the press to start on the quad.
        const bool wasDown = g_trigDown;
        if (!g_trigDown && trig > 0.6f && on) {
            g_trigDown = true;
            ++g_clicks;
        } else if (g_trigDown && trig < 0.4f) {
            g_trigDown = false;
        }
        if (!wrist) {
            g_trigDown = false;
        }
        // Thumbstick scroll: one wheel step per 150 ms while held past 0.5.
        int        wheel = 0;
        const auto now = std::chrono::steady_clock::now();
        const int  dir = stickY > 0.5f ? 1 : stickY < -0.5f ? -1 : 0;
        if (on && dir != 0 && (dir != g_wheelDir || now >= g_nextWheel)) {
            wheel = dir;
            g_nextWheel = now + std::chrono::milliseconds(150);
            ++g_wheels;
        }
        g_wheelDir = dir;
        if (wrist) {
            game::PipboyPointer p;
            p.onQuad = on;
            p.u = g_u;
            p.v = g_v;
            p.down = g_trigDown;
            p.wheel = wheel;
            game::SetPipboyPointer(p);
        }
        {
            std::scoped_lock l(g_lock);
            if (g_poseValid) {
                const auto& o = g_pose.orientation;
                const auto& p = g_pose.position;
                g_poseLine = std::format("pos {:.4f},{:.4f},{:.4f} quat {:.4f},{:.4f},{:.4f},{:.4f} size {:.3f}x{:.3f} grip {:.4f},{:.4f},{:.4f}", p.x, p.y, p.z,
                    o.x, o.y, o.z, o.w, g_quadW, g_quadH, grip.position.x, grip.position.y, grip.position.z);
            }
            if (aimOk) {
                const xr::Vec3 d = Rot(aim.orientation, { 0.0f, 0.0f, -1.0f });
                g_aimLine = std::format("pos {:.4f},{:.4f},{:.4f} dir {:.4f},{:.4f},{:.4f}", aim.position.x, aim.position.y, aim.position.z, d.x, d.y, d.z);
            }
            if (on || wasDown != g_trigDown) {
                g_lastHitLine = std::format("u={:.4f} v={:.4f} dist={:.3f} trigger={:.2f} down={}", g_u, g_v, g_hitDist, trig, g_trigDown ? 1 : 0);
            }
        }
        return wrist && (on || g_trigDown);
    }

    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx)
    {
        if (!g_wrist.load() || !g_poseValid || !g_dev) {
            return false;
        }
        const Copy& src = Source();
        if (!src.tex) {
            return false;
        }
        ID3D11Texture2D* shown = PrepareDisplay(a_ctx, src, g_cfg.cursorDot && g_onQuad, g_u, g_v);
        if (!a_rt.SubmitWorldQuad(a_ctx, shown, g_pose, g_quadW, false)) {
            return false;
        }
        ++g_submits;
        return true;
    }

    void OnPresent()
    {
        std::string path;
        {
            std::scoped_lock l(g_lock);
            path.swap(g_capturePath);
        }
        if (!path.empty()) {
            ServiceCapture(path);
            g_capturePending = false;
        }
        g_copy3C.fresh = g_copy3D.fresh = false;
    }

    bool PointerOnQuad() { return g_wrist.load() && (g_onQuad || g_trigDown); }

    void RequestCapture(std::string a_path)
    {
        std::scoped_lock l(g_lock);
        g_capturePath = std::move(a_path);
        g_capturePending = true;
    }

    std::string Status()
    {
        std::string pose, hit, cap, aim;
        Config      c;
        {
            std::scoped_lock l(g_lock);
            pose = g_poseLine;
            hit = g_lastHitLine;
            aim = g_aimLine;
            cap = g_lastCapture;
            c = g_cfg;
        }
        return std::format("enabled={} wrist={} src={:X} shown={:X} width={:.2f} offset={:.3f},{:.3f},{:.3f} rot={:.0f},{:.0f},{:.0f} leftHanded={} "
                           "copies3C={} copies3D={} submits={} frames={} onQuad={} hits={} clicks={} wheels={} | quad {} | pointer {} | aim {} | game {} | last pipcap {}",
            Enabled() ? 1 : 0, g_wrist.load() ? 1 : 0, g_source.load(), &Source() == &g_copy3D ? 0x3D : 0x3C, c.widthMeters, c.offset[0], c.offset[1], c.offset[2], c.rotDeg[0],
            c.rotDeg[1], c.rotDeg[2], c.leftHanded ? 1 : 0, g_copy3C.count, g_copy3D.count, g_submits.load(), g_frames.load(), g_onQuad ? 1 : 0,
            g_hits.load(), g_clicks.load(), g_wheels.load(), pose, hit, aim, game::PipboyGameReport(), cap) +
               std::format(" | tint on={} rgb={:.3f},{:.3f},{:.3f} brightness={:.3f} fromGame={} draws={}", g_tintOn.load() ? 1 : 0, g_tintRgb[0], g_tintRgb[1],
                   g_tintRgb[2], g_tintBright, g_tintFromGame ? 1 : 0, g_tintDraws.load()) +
               std::format(" | projected enabled={} open={} frames={} dist={:.2f} height={:.2f} width={:.2f} follow={:.2f}s yaw={:.2f}deg", ProjectedEnabled() ? 1 : 0,
                   g_projected.load() ? 1 : 0, g_projFrames.load(), c.projDistanceMeters, c.projHeightMeters, c.projWidthMeters, c.projFollowSeconds,
                   g_projYaw * 57.2957795f) +
               std::format(" floating={} inPA={}", FloatingEnabled() ? 1 : 0, game::PipboyProjectedInPowerArmor() ? 1 : 0);
    }

    bool ProjectedEnabled() { return g_projEnabled.load() && game::PipboyHooksInstalled(); }
    void SetProjectedEnabled(bool a_on) { g_projEnabled = a_on; }
    bool FloatingEnabled() { return g_floatEnabled.load() && game::PipboyHooksInstalled(); }
    void SetFloatingEnabled(bool a_on) { g_floatEnabled = a_on; }
    bool SkipWorldScreen() { return g_projected.load(std::memory_order_relaxed);
    }
}
