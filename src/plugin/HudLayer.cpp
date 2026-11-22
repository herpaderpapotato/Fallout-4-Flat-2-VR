// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/HudLayer.h"
#include "plugin/GameHud.h"
#include "plugin/Settings.h"
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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace fo4flatvr::hud
{
    namespace
    {
        // Copies of the engine targets, refreshed by H1 during the game's render (same thread as Present).
        struct Copy
        {
            ComPtr<ID3D11Texture2D> tex;
            D3D11_TEXTURE2D_DESC    desc{};
            bool                    fresh{ false };  // copied since the last Present
        };

        Config                     g_cfg;
        std::mutex                 g_lock;           // g_cfg geometry, capture path, last capture stats
        std::atomic<bool>          g_enabled{ false };
        std::atomic<bool>          g_layerActive{ false };
        std::atomic<int>           g_source{ 0x3E };
        std::atomic<bool>          g_hideCrosshair{ true };
        std::atomic<bool>          g_premultiplied{ true };
        std::atomic<std::uint64_t> g_copies{ 0 }, g_submits{ 0 }, g_presents{ 0 }, g_skippedFrames{ 0 };
        bool                       g_skippedThisFrame{ false };
        Copy                       g_copy[2];  // [0] = 0x3E, [1] = 0x3F
        ComPtr<ID3D11Device>        g_dev;
        ComPtr<ID3D11DeviceContext> g_ctx;
        std::string                g_capturePath;
        std::string                g_lastCapture{ "none" };

        // Power armour dash (PowerArmorRenderer composite) over black / over white, + the saved destination.
        std::atomic<bool>          g_paOn{ false }, g_rainOff{ false };
        Copy                       g_paB, g_paW;
        ComPtr<ID3D11Texture2D>    g_paSave;
        D3D11_TEXTURE2D_DESC       g_paSaveDesc{};
        std::atomic<std::uint64_t> g_paCaptures{ 0 }, g_paComposites{ 0 }, g_paFailed{ 0 };
        std::string                g_paDestLine{ "none" };  // render thread writes once per format change (under g_lock)

        // Composite (HUD over PA dash) -> g_comp, submitted instead of the HUD copy while a PA capture is fresh.
        struct CompRes
        {
            ComPtr<ID3D11VertexShader>    vs;
            ComPtr<ID3D11PixelShader>     ps;
            ComPtr<ID3D11Buffer>          cb;
            ComPtr<ID3D11RasterizerState> rs;
            bool                          tried{ false };
            bool                          ok{ false };
        };
        CompRes                        g_compRes;
        ComPtr<ID3D11Texture2D>        g_comp;
        ComPtr<ID3D11RenderTargetView> g_compRtv;
        D3D11_TEXTURE2D_DESC           g_compDesc{};
        bool                           g_compValid = false;  // g_comp holds this frame's composite (for hudcap)

        // Menu panel: copies of the FlatScreenModel targets ([0] = 0x3E, [1] = 0x3F) taken in H1.
        std::atomic<bool>          g_menuOn{ false };       // [Menus] bPanel / pipe
        std::atomic<bool>          g_menuActive{ false };   // this frame: panel shown -> H2 skips FlatScreenModel
        std::atomic<int>           g_menuSource{ 0x3E };
        std::atomic<bool>          g_menuPremul{ true };
        Copy                       g_menuCopy[2];
        std::uint64_t              g_menuCopyPresent[2]{};  // g_presents value of the last copy (staleness)
        std::atomic<std::uint64_t> g_menuCopies{ 0 }, g_menuSkips{ 0 }, g_menuSkipFrames{ 0 }, g_menuShown{ 0 };
        bool                       g_menuSkippedThisFrame{ false };

        // Menu panel 3D item previews (Container3D, WorkbenchItem3D, ExamineMenuItem3D, LockpickingMenu3D, ...: their own
        // Interface3D renderers, composited screen-space in RenderMain): captured over black / over white into g_pvB/g_pvW
        // (several renderers in one frame accumulate in draw order), left out of the world image, and composited with the
        // FlatScreenModel copy into g_menuComp for the panel. [Menus] bPanelPreview.
        std::atomic<bool>              g_pvOn{ true };
        Copy                           g_pvB, g_pvW;
        ComPtr<ID3D11Texture2D>        g_pvSave;
        D3D11_TEXTURE2D_DESC           g_pvSaveDesc{};
        bool                           g_pvThisFrame{ false };       // render thread: a preview was captured since the last Present
        bool                           g_menuDrawnThisFrame{ false };  // FlatScreenModel went to the panel earlier in this frame
        bool                           g_pvOnTop{ false };            // the first preview of this frame was drawn after the menu
        std::atomic<std::uint64_t>     g_pvCaptures{ 0 }, g_pvFailed{ 0 }, g_pvComposites{ 0 }, g_pvFrames{ 0 };
        std::string                    g_pvNames;                     // renderer names captured (under g_lock)
        ComPtr<ID3D11Texture2D>        g_menuComp;
        ComPtr<ID3D11RenderTargetView> g_menuCompRtv;
        D3D11_TEXTURE2D_DESC           g_menuCompDesc{};
        bool                           g_menuCompValid{ false };      // g_menuComp holds this frame's panel (hudcap)

        // premultiplied PA = (black result, 1 - mean(white - black)); out = hud + (1 - hud.a) * pa. Sizes may differ:
        // every source is addressed by its own pixel grid (Load at uv x size).
        constexpr char kCompHlsl[] = R"(
Texture2D<float4> hud : register(t0);
Texture2D<float4> pab : register(t1);
Texture2D<float4> paw : register(t2);
cbuffer C : register(b0) { float4 outSize; float4 hudSize; float4 paSize; float4 flags; };  // flags.w: pa over hud
void vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy / outSize.xy;
    float4 h = 0;
    if (flags.x > 0.5) {
        h = hud.Load(int3(uv * hudSize.xy, 0));
        if (flags.z > 0.5) h.rgb *= h.a;  // straight-alpha HUD source -> premultiplied
    }
    float4 pa = 0;
    if (flags.y > 0.5) {
        int3 p = int3(uv * paSize.xy, 0);
        float3 b = pab.Load(p).rgb;
        float3 w = paw.Load(p).rgb;
        float a = saturate(1.0 - dot(w - b, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
        pa = float4(min(b, a), a);
    }
    if (flags.w > 0.5) return pa + (1.0 - pa.a) * h;
    return h + (1.0 - h.a) * pa;
}
)";

        int Index(int a_rt) { return a_rt == 0x3E ? 0 : 1; }

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

        ID3D11ShaderResourceView* Srv(const Copy& a_c, ComPtr<ID3D11ShaderResourceView>& a_srv)
        {
            if (!a_c.tex) {
                return nullptr;
            }
            if (!a_srv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = Typed(a_c.desc.Format);
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(g_dev->CreateShaderResourceView(a_c.tex.Get(), &vd, &a_srv))) {
                    REX::ERROR("hud: SRV creation failed (fmt {})", static_cast<int>(vd.Format));
                    return nullptr;
                }
            }
            return a_srv.Get();
        }
        ComPtr<ID3D11ShaderResourceView> g_srvHud[2], g_srvPaB, g_srvPaW;
        ID3D11Texture2D* g_srvFor[4]{};  // texture each SRV was made for (recreated when the copy is reallocated)

        bool CreateCompRes()
        {
            auto& r = g_compRes;
            if (r.tried) {
                return r.ok;
            }
            r.tried = true;
            ComPtr<ID3DBlob> vsb, psb, err;
            if (FAILED(D3DCompile(kCompHlsl, sizeof(kCompHlsl) - 1, "pa_comp", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
                FAILED(D3DCompile(kCompHlsl, sizeof(kCompHlsl) - 1, "pa_comp", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
                REX::ERROR("hud pa: shader compile failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                return false;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 64;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &r.vs)) ||
                FAILED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &r.ps)) ||
                FAILED(g_dev->CreateBuffer(&bd, nullptr, &r.cb)) || FAILED(g_dev->CreateRasterizerState(&rd, &r.rs))) {
                REX::ERROR("hud pa: resource creation failed");
                return false;
            }
            r.ok = true;
            REX::INFO("hud pa: composite shaders ready");
            return true;
        }

        // Saves exactly the immediate-context bindings the composite draw changes and restores them.
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
            ID3D11ShaderResourceView* srv[3]{};
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
                ctx->PSGetShaderResources(0, 3, srv);
                ctx->PSGetConstantBuffers(0, 1, &cb);
                ctx->RSGetState(&rs);
                ctx->RSGetViewports(&nvp, vps);
                ctx->OMGetBlendState(&bs, bf, &sm);
                ctx->OMGetDepthStencilState(&dss, &sref);
                ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
            }
            ~SavedState()
            {
                ID3D11ShaderResourceView* nullSrv[3]{};
                ctx->PSSetShaderResources(0, 3, nullSrv);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
                ctx->OMSetDepthStencilState(dss, sref);
                ctx->OMSetBlendState(bs, bf, sm);
                ctx->RSSetViewports(nvp, vps);
                ctx->RSSetState(rs);
                ctx->PSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetShaderResources(0, 3, srv);
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
                rel(il), rel(vs), rel(hs), rel(ds), rel(gs), rel(ps), rel(cb), rel(rs), rel(bs), rel(dss), rel(dsv);
                for (auto& s : srv) {
                    rel(s);
                }
                for (auto& r : rtv) {
                    rel(r);
                }
            }
            SavedState(const SavedState&) = delete;
            SavedState& operator=(const SavedState&) = delete;
        };

        // a_top (may be null; straight alpha if !a_topPremul) and the over-black/over-white capture a_b/a_w (null = none)
        // -> a_out (RGBA8 premultiplied, a_top's size, else a_b's). a_bOnTop: the capture goes over a_top instead of under.
        // a_srvBase: first of three SRV cache slots (top, b, w). Returns a_out or null.
        ComPtr<ID3D11ShaderResourceView> g_srvCache[8];
        ID3D11Texture2D*                 g_srvCacheFor[8]{};

        ID3D11Texture2D* CompositeInto(ID3D11DeviceContext* a_ctx, const Copy* a_top, bool a_topPremul, const Copy* a_b, const Copy* a_w,
            bool a_bOnTop, ComPtr<ID3D11Texture2D>& a_out, ComPtr<ID3D11RenderTargetView>& a_outRtv, D3D11_TEXTURE2D_DESC& a_outDesc,
            int a_srvBase, const char* a_what)
        {
            if (!g_dev || !CreateCompRes()) {
                return nullptr;
            }
            const bool  haveB = a_b && a_w && a_b->tex && a_w->tex;
            const Copy* sizeSrc = a_top && a_top->tex ? a_top : haveB ? a_b : nullptr;
            if (!sizeSrc) {
                return nullptr;
            }
            const UINT w = sizeSrc->desc.Width, h = sizeSrc->desc.Height;
            if (!w || !h) {
                return nullptr;
            }
            if (!a_out || a_outDesc.Width != w || a_outDesc.Height != h) {
                a_out.Reset();
                a_outRtv.Reset();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w;
                td.Height = h;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc = { 1, 0 };
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &a_out)) || FAILED(g_dev->CreateRenderTargetView(a_out.Get(), nullptr, &a_outRtv))) {
                    REX::ERROR("hud {}: composite texture {}x{} creation failed", a_what, w, h);
                    a_out.Reset();
                    a_outRtv.Reset();
                    a_outDesc = {};
                    return nullptr;
                }
                a_outDesc = td;
                REX::INFO("hud {}: composite texture {}x{}", a_what, w, h);
            }
            auto srvFor = [](int a_slot, const Copy* a_c) -> ID3D11ShaderResourceView* {
                if (!a_c || !a_c->tex) {
                    return nullptr;
                }
                if (g_srvCacheFor[a_slot] != a_c->tex.Get()) {
                    g_srvCache[a_slot].Reset();
                    g_srvCacheFor[a_slot] = a_c->tex.Get();
                }
                return Srv(*a_c, g_srvCache[a_slot]);
            };
            ID3D11ShaderResourceView* srvs[3] = { srvFor(a_srvBase, a_top), haveB ? srvFor(a_srvBase + 1, a_b) : nullptr,
                haveB ? srvFor(a_srvBase + 2, a_w) : nullptr };
            const float cbData[16] = { static_cast<float>(w), static_cast<float>(h), 0, 0,
                a_top ? static_cast<float>(a_top->desc.Width) : 0.0f, a_top ? static_cast<float>(a_top->desc.Height) : 0.0f, 0, 0,
                haveB ? static_cast<float>(a_b->desc.Width) : 0.0f, haveB ? static_cast<float>(a_b->desc.Height) : 0.0f, 0, 0,
                srvs[0] ? 1.0f : 0.0f, (srvs[1] && srvs[2]) ? 1.0f : 0.0f, a_topPremul ? 0.0f : 1.0f, a_bOnTop ? 1.0f : 0.0f };
            a_ctx->UpdateSubresource(g_compRes.cb.Get(), 0, nullptr, cbData, 0, 0);
            {
                SavedState              saved(a_ctx);
                ID3D11RenderTargetView* rtv = a_outRtv.Get();
                a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                a_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                a_ctx->OMSetDepthStencilState(nullptr, 0);
                const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
                a_ctx->RSSetViewports(1, &vp);
                a_ctx->RSSetState(g_compRes.rs.Get());
                a_ctx->IASetInputLayout(nullptr);
                a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                a_ctx->VSSetShader(g_compRes.vs.Get(), nullptr, 0);
                a_ctx->HSSetShader(nullptr, nullptr, 0);
                a_ctx->DSSetShader(nullptr, nullptr, 0);
                a_ctx->GSSetShader(nullptr, nullptr, 0);
                a_ctx->PSSetShader(g_compRes.ps.Get(), nullptr, 0);
                ID3D11Buffer* cb = g_compRes.cb.Get();
                a_ctx->PSSetConstantBuffers(0, 1, &cb);
                a_ctx->PSSetShaderResources(0, 3, srvs);
                a_ctx->Draw(3, 0);
            }
            return a_out.Get();
        }

        // HUD copy (may be null) over the PA capture (may be stale = not used) -> g_comp. Returns g_comp or null.
        ID3D11Texture2D* Composite(ID3D11DeviceContext* a_ctx, const Copy* a_hud, bool a_pa, bool a_hudPremul)
        {
            if (!g_dev || !CreateCompRes()) {
                return nullptr;
            }
            const Copy* sizeSrc = a_hud ? a_hud : &g_paB;
            const UINT  w = sizeSrc->desc.Width, h = sizeSrc->desc.Height;
            if (!w || !h) {
                return nullptr;
            }
            if (!g_comp || g_compDesc.Width != w || g_compDesc.Height != h) {
                g_comp.Reset();
                g_compRtv.Reset();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w;
                td.Height = h;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc = { 1, 0 };
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_comp)) || FAILED(g_dev->CreateRenderTargetView(g_comp.Get(), nullptr, &g_compRtv))) {
                    REX::ERROR("hud pa: composite texture {}x{} creation failed", w, h);
                    g_comp.Reset();
                    g_compRtv.Reset();
                    g_compDesc = {};
                    return nullptr;
                }
                g_compDesc = td;
                REX::INFO("hud pa: composite texture {}x{}", w, h);
            }
            // SRVs follow their textures (copies are reallocated on size/format change)
            auto srvFor = [](int a_slot, const Copy& a_c, ComPtr<ID3D11ShaderResourceView>& a_srv) -> ID3D11ShaderResourceView* {
                if (g_srvFor[a_slot] != a_c.tex.Get()) {
                    a_srv.Reset();
                    g_srvFor[a_slot] = a_c.tex.Get();
                }
                return Srv(a_c, a_srv);
            };
            ID3D11ShaderResourceView* srvs[3] = { a_hud ? srvFor(a_hud == &g_copy[0] ? 0 : 1, *a_hud, g_srvHud[a_hud == &g_copy[0] ? 0 : 1]) : nullptr,
                a_pa ? srvFor(2, g_paB, g_srvPaB) : nullptr, a_pa ? srvFor(3, g_paW, g_srvPaW) : nullptr };
            const float cbData[16] = { static_cast<float>(w), static_cast<float>(h), 0, 0,
                a_hud ? static_cast<float>(a_hud->desc.Width) : 0.0f, a_hud ? static_cast<float>(a_hud->desc.Height) : 0.0f, 0, 0,
                static_cast<float>(g_paB.desc.Width), static_cast<float>(g_paB.desc.Height), 0, 0,
                srvs[0] ? 1.0f : 0.0f, (srvs[1] && srvs[2]) ? 1.0f : 0.0f, a_hudPremul ? 0.0f : 1.0f, 0 };
            a_ctx->UpdateSubresource(g_compRes.cb.Get(), 0, nullptr, cbData, 0, 0);
            {
                SavedState              saved(a_ctx);
                ID3D11RenderTargetView* rtv = g_compRtv.Get();
                a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                a_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                a_ctx->OMSetDepthStencilState(nullptr, 0);
                const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
                a_ctx->RSSetViewports(1, &vp);
                a_ctx->RSSetState(g_compRes.rs.Get());
                a_ctx->IASetInputLayout(nullptr);
                a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                a_ctx->VSSetShader(g_compRes.vs.Get(), nullptr, 0);
                a_ctx->HSSetShader(nullptr, nullptr, 0);
                a_ctx->DSSetShader(nullptr, nullptr, 0);
                a_ctx->GSSetShader(nullptr, nullptr, 0);
                a_ctx->PSSetShader(g_compRes.ps.Get(), nullptr, 0);
                ID3D11Buffer* cb = g_compRes.cb.Get();
                a_ctx->PSSetConstantBuffers(0, 1, &cb);
                a_ctx->PSSetShaderResources(0, 3, srvs);
                a_ctx->Draw(3, 0);
            }
            ++g_paComposites;
            g_compValid = true;
            return g_comp.Get();
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
                D3D11_TEXTURE2D_DESC td = sd;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // PA composite source
                td.CPUAccessFlags = 0;
                td.MiscFlags = 0;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &a_dst.tex))) {
                    REX::ERROR("hud: CreateTexture2D({}x{} fmt {}) failed", sd.Width, sd.Height, static_cast<int>(sd.Format));
                    a_dst.desc = {};
                    return;
                }
                a_dst.desc = td;
                REX::INFO("hud: copy target {}x{} format {}", sd.Width, sd.Height, static_cast<int>(sd.Format));
            }
            g_ctx->CopySubresourceRegion(a_dst.tex.Get(), 0, 0, 0, 0, a_src, 0, nullptr);
            a_dst.fresh = true;
        }

        float HalfToFloat(std::uint16_t h)
        {
            const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
            float v;
            if (e == 0) {
                v = std::ldexp(static_cast<float>(m), -24);
            } else if (e == 31) {
                v = m ? 0.0f : 65504.0f;
            } else {
                v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
            }
            return s ? -v : v;
        }

        std::uint8_t ToByte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

        // Copy -> BGRA8 (alpha kept) + statistics. Returns the stats line ("" if the format is unsupported).
        std::string ReadBack(const Copy& a_copy, std::vector<std::uint8_t>& a_bgra)
        {
            const auto& d = a_copy.desc;
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
                            r = ToByte(HalfToFloat(v[0])), g = ToByte(HalfToFloat(v[1])), b = ToByte(HalfToFloat(v[2])), a = ToByte(HalfToFloat(v[3]));
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
            g_ctx->Unmap(staging.Get(), 0);
            if (!ok) {
                return std::format("unsupported format {}", static_cast<int>(d.Format));
            }

            // Statistics: coverage, bounding box, a 3x3 grid of covered pixels (rows top/mid/bottom, columns
            // left/centre/right), and the premultiplication test (premultiplied => no channel above alpha).
            std::size_t vis = 0, opaque = 0, overAlpha = 0, colourNoAlpha = 0, sumA = 0;
            std::size_t grid[9]{};
            UINT        x0 = w, y0 = h, x1 = 0, y1 = 0;
            for (UINT y = 0; y < h; ++y) {
                for (UINT x = 0; x < w; ++x) {
                    const std::uint8_t* p = a_bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                    const int           mx = (std::max)({ p[0], p[1], p[2] });
                    const int           a = p[3];
                    if (a == 0) {
                        colourNoAlpha += mx > 4;
                        continue;
                    }
                    ++vis;
                    sumA += a;
                    opaque += a == 255;
                    overAlpha += mx > a + 4;
                    x0 = (std::min)(x0, x), y0 = (std::min)(y0, y), x1 = (std::max)(x1, x), y1 = (std::max)(y1, y);
                    ++grid[(y * 3 / h) * 3 + x * 3 / w];
                }
            }
            return std::format("{}x{} fmt {} fresh={} alpha>0 {} ({:.2f}%) opaque {} meanA {:.0f} bbox [{},{}..{},{}] grid T[{} {} {}] M[{} {} {}] B[{} {} {}] rgb>a+4 {} colourAtAlpha0 {}",
                w, h, static_cast<int>(d.Format), a_copy.fresh ? 1 : 0, vis, 100.0 * static_cast<double>(vis) / (static_cast<double>(w) * h), opaque,
                vis ? static_cast<double>(sumA) / static_cast<double>(vis) : 0.0, vis ? x0 : 0, vis ? y0 : 0, x1, y1, grid[0], grid[1], grid[2],
                grid[3], grid[4], grid[5], grid[6], grid[7], grid[8], overAlpha, colourNoAlpha);
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
            ih.biHeight = -static_cast<LONG>(a_h);  // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;  // B,G,R,A; the 4th byte is the alpha
            ih.biSizeImage = imageSize;
            HANDLE f = ::CreateFileA(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                REX::ERROR("hudcap: cannot open {} ({})", a_path, ::GetLastError());
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
            const int src = g_source.load();
            for (int rt : { 0x3E, 0x3F }) {
                const Copy& c = g_copy[Index(rt)];
                if (!c.tex || !g_dev) {
                    summary += std::format(" {:X}: no copy yet;", rt);
                    continue;
                }
                std::vector<std::uint8_t> px;
                const std::string         stats = ReadBack(c, px);
                summary += std::format(" {:X}: {};", rt, stats);
                if (px.empty()) {
                    continue;
                }
                WriteBmp(std::format("{}_{:x}.bmp", stem, rt), px, c.desc.Width, c.desc.Height);
                if (rt == src) {
                    WriteBmp(a_path, px, c.desc.Width, c.desc.Height);
                }
            }
            // Menu panel copies (FlatScreenModel), when one exists.
            for (int rt : { 0x3E, 0x3F }) {
                const Copy& c = g_menuCopy[Index(rt)];
                if (!c.tex || !g_dev) {
                    continue;
                }
                std::vector<std::uint8_t> px;
                const std::string         stats = ReadBack(c, px);
                summary += std::format(" menu{:X} (age {} presents): {};", rt, g_presents.load() - g_menuCopyPresent[Index(rt)], stats);
                if (!px.empty()) {
                    WriteBmp(std::format("{}_menu{:x}.bmp", stem, rt), px, c.desc.Width, c.desc.Height);
                }
            }
            // Power armour dash: premultiplied colour (over black) + alpha (1 - (white - black)) as one BGRA image.
            if (g_paB.tex && g_paW.tex && g_dev) {
                std::vector<std::uint8_t> pb, pw;
                const std::string         sb = ReadBack(g_paB, pb);
                ReadBack(g_paW, pw);
                if (!pb.empty() && pb.size() == pw.size()) {
                    const UINT w = g_paB.desc.Width, h = g_paB.desc.Height;
                    std::vector<std::uint8_t> pa(pb.size());
                    std::size_t vis = 0, opaque = 0, grid[9]{};
                    double      sumA = 0;
                    UINT        x0 = w, y0 = h, x1 = 0, y1 = 0;
                    for (UINT y = 0; y < h; ++y) {
                        for (UINT x = 0; x < w; ++x) {
                            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                            const int d = (static_cast<int>(pw[i]) - pb[i] + pw[i + 1] - pb[i + 1] + pw[i + 2] - pb[i + 2]) / 3;
                            const int a = std::clamp(255 - d, 0, 255);
                            pa[i] = (std::min)(pb[i], static_cast<std::uint8_t>(a));
                            pa[i + 1] = (std::min)(pb[i + 1], static_cast<std::uint8_t>(a));
                            pa[i + 2] = (std::min)(pb[i + 2], static_cast<std::uint8_t>(a));
                            pa[i + 3] = static_cast<std::uint8_t>(a);
                            if (a > 4) {
                                ++vis;
                                sumA += a;
                                opaque += a >= 250;
                                x0 = (std::min)(x0, x), y0 = (std::min)(y0, y), x1 = (std::max)(x1, x), y1 = (std::max)(y1, y);
                                ++grid[(y * 3 / h) * 3 + x * 3 / w];
                            }
                        }
                    }
                    WriteBmp(stem + "_pa.bmp", pa, w, h);
                    summary += std::format(" pa: {}x{} fmt {} fresh={} captures={} alpha>4 {} ({:.2f}%) opaque {} meanA {:.0f} bbox [{},{}..{},{}] grid T[{} {} {}] M[{} {} {}] B[{} {} {}] (black: {});",
                        w, h, static_cast<int>(g_paB.desc.Format), g_paB.fresh ? 1 : 0, g_paCaptures.load(), vis,
                        100.0 * static_cast<double>(vis) / (static_cast<double>(w) * h), opaque, vis ? sumA / static_cast<double>(vis) : 0.0, vis ? x0 : 0,
                        vis ? y0 : 0, x1, y1, grid[0], grid[1], grid[2], grid[3], grid[4], grid[5], grid[6], grid[7], grid[8], sb);
                }
            } else {
                summary += " pa: no capture yet;";
            }
            if (g_comp && g_compValid) {
                Copy c;
                c.tex = g_comp;
                c.desc = g_compDesc;
                std::vector<std::uint8_t> px;
                summary += std::format(" shown: {};", ReadBack(c, px));
                if (!px.empty()) {
                    WriteBmp(stem + "_shown.bmp", px, g_compDesc.Width, g_compDesc.Height);
                }
            }
            // Menu panel with its 3D item preview (what the panel quad shows this frame)
            if (g_menuComp && g_menuCompValid) {
                Copy c;
                c.tex = g_menuComp;
                c.desc = g_menuCompDesc;
                std::vector<std::uint8_t> px;
                summary += std::format(" panel (preview {}): {};", g_pvOnTop ? "over menu" : "under menu", ReadBack(c, px));
                if (!px.empty()) {
                    WriteBmp(stem + "_panel.bmp", px, g_menuCompDesc.Width, g_menuCompDesc.Height);
                }
            }
            REX::INFO("hudcap {}:{}", a_path, summary);
            std::scoped_lock l(g_lock);
            g_lastCapture = a_path + ":" + summary;
        }
    }


    // ---- FO4VR HUD split (HudLayer.h SplitConfig) -------------------------------------------------------------------
    namespace
    {
        enum Piece : int { kPieceHp = 0, kPieceRight = 1, kPieceCompass = 2, kPieceCount = 3 };
        struct Rect01
        {
            float u0{ 0 }, v0{ 0 }, u1{ 0 }, v1{ 0 };
            bool  valid{ false };
        };
        std::mutex  g_splitLock;  // g_split, g_rect01, g_head*, g_splitLine
        SplitConfig g_split;
        Rect01      g_rect01[kPieceCount];
        xr::Pose    g_head{};
        bool        g_headValid{ false };
        float       g_aniso{ 1.0f };  // stage -> target sx / sy (game::HudSplitTick)

        // present thread only
        struct PxRect
        {
            int x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
        };
        ComPtr<ID3D11Texture2D> g_atlas, g_zero;
        D3D11_TEXTURE2D_DESC    g_atlasDesc{}, g_zeroDesc{};
        float                   g_slideU = 0.0f;  // VRIK compassOffset.z (game units, 0 = shown .. -50 = hidden)
        bool                    g_compassWant = true;
        std::chrono::steady_clock::time_point g_lastFrame{};
        std::atomic<std::uint64_t> g_pieceFrames{ 0 }, g_handFrames{ 0 }, g_compassFrames{ 0 }, g_atlasBuilds{ 0 }, g_compassToggles{ 0 };
        std::string             g_splitLine{ "no frame yet" };

        UINT BytesPerPixel(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
            default: return 4;
            }
        }

        // a_w x a_h in a_fmt, zero-filled (transparent black), default usage, SRV (the XR stage copy reads it)
        bool MakeZeroed(ComPtr<ID3D11Texture2D>& a_tex, D3D11_TEXTURE2D_DESC& a_desc, UINT a_w, UINT a_h, DXGI_FORMAT a_fmt, const char* a_what)
        {
            if (a_tex && a_desc.Width == a_w && a_desc.Height == a_h && a_desc.Format == a_fmt) {
                return true;
            }
            a_tex.Reset();
            a_desc = {};
            D3D11_TEXTURE2D_DESC td{};
            td.Width = a_w;
            td.Height = a_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = a_fmt;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const UINT                bpp = BytesPerPixel(a_fmt);
            std::vector<std::uint8_t> zeros(static_cast<std::size_t>(a_w) * a_h * bpp, 0);
            D3D11_SUBRESOURCE_DATA    init{ zeros.data(), a_w * bpp, 0 };
            if (!g_dev || FAILED(g_dev->CreateTexture2D(&td, &init, &a_tex))) {
                REX::ERROR("hud split: {} texture {}x{} fmt {} creation failed", a_what, a_w, a_h, static_cast<int>(a_fmt));
                a_tex.Reset();
                return false;
            }
            a_desc = td;
            REX::INFO("hud split: {} texture {}x{} fmt {}", a_what, a_w, a_h, static_cast<int>(a_fmt));
            return true;
        }

        xr::Quat QMul(const xr::Quat& a, const xr::Quat& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }
        xr::Vec3 QRot(const xr::Quat& q, const xr::Vec3& v)
        {
            const xr::Quat p{ v.x, v.y, v.z, 0.0f };
            const xr::Quat r = QMul(QMul(q, p), { -q.x, -q.y, -q.z, q.w });
            return { r.x, r.y, r.z };
        }

        // Per hand. Mode 3 = VRIK's rule (VrikBody::setupState 18000F4C0, hideCompass 2/3): the palm normal's world-up component
        // > 0.65, no height, no timing. OpenXR grip: +X is out of the left palm and into the right palm -> palm normal +X (left),
        // -X (right). Mode 2 (default, the user's spec "raises hands, not high"): the grip between fCompassRaiseHighM and
        // fCompassRaiseLowM below the eyes.
        bool HandShows(const SplitConfig& a_c, const xr::Pose& a_head, const xr::Pose& a_grip, bool a_left)
        {
            if (a_c.compassShow == 3) {
                const xr::Vec3 n = QRot(a_grip.orientation, { a_left ? 1.0f : -1.0f, 0.0f, 0.0f });
                return n.y > a_c.palmUpDot;
            }
            const float below = a_head.position.y - a_grip.position.y;  // the reference space is gravity-aligned (+Y up)
            return below <= a_c.raiseLowM && below >= a_c.raiseHighM;
        }

        // Upright head frame (FO4VR UprightHmdNode): head position, yaw only. Looking far down, the yaw comes from the head's
        // up vector, blended in as VRIK does for its HMD direction (factor clamp(-4 f.y - 2.5, 0, 1)).
        xr::Quat UprightYaw(const xr::Pose& a_head)
        {
            const xr::Vec3 f = QRot(a_head.orientation, { 0.0f, 0.0f, -1.0f });
            const xr::Vec3 u = QRot(a_head.orientation, { 0.0f, 1.0f, 0.0f });
            const float    t = std::clamp(-f.y * 4.0f - 2.5f, 0.0f, 1.0f);
            const float    dx = f.x * (1.0f - t) + u.x * t, dz = f.z * (1.0f - t) + u.z * t;
            const float    yaw = (std::abs(dx) + std::abs(dz) > 1e-5f) ? std::atan2(-dx, -dz) : 0.0f;
            return { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        }

        bool  ReadIniBool(const char* a_k, bool a_d) { return ::GetPrivateProfileIntA("HUD", a_k, a_d ? 1 : 0, Settings::Path().c_str()) != 0; }
        int   ReadIniInt(const char* a_k, int a_d) { return static_cast<int>(::GetPrivateProfileIntA("HUD", a_k, a_d, Settings::Path().c_str())); }
        float ReadIniFloat(const char* a_k, float a_d)
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("HUD", a_k, "", buf, sizeof(buf), Settings::Path().c_str());
            if (!buf[0]) {
                return a_d;
            }
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : a_d;
        }

        // ini key -> field; the start-up load and the live "hud set" share it
        struct SplitKey
        {
            const char* name;
            int         kind;  // 0 bool, 1 int, 2 float
            std::size_t off;
        };
        const SplitKey kSplitKeys[] = {
            { "bHandHud", 0, offsetof(SplitConfig, handHud) },
            { "fHandHudWidthM", 2, offsetof(SplitConfig, handWidthM) },
            { "fHandHudX", 2, offsetof(SplitConfig, handX) },
            { "fHandHudY", 2, offsetof(SplitConfig, handY) },
            { "fHandHudZ", 2, offsetof(SplitConfig, handZ) },
            { "fHandHudPitchDeg", 2, offsetof(SplitConfig, handPitchDeg) },
            { "bCompassChin", 0, offsetof(SplitConfig, compassSplit) },
            { "fCompassDistanceM", 2, offsetof(SplitConfig, compassDistM) },
            { "fCompassHeightM", 2, offsetof(SplitConfig, compassHeightM) },
            { "fCompassWidthM", 2, offsetof(SplitConfig, compassWidthM) },
            { "iCompassShow", 1, offsetof(SplitConfig, compassShow) },
            { "iCompassHand", 1, offsetof(SplitConfig, compassHand) },
            { "fCompassRaiseLowM", 2, offsetof(SplitConfig, raiseLowM) },
            { "fCompassRaiseHighM", 2, offsetof(SplitConfig, raiseHighM) },
            { "fCompassPalmUpDot", 2, offsetof(SplitConfig, palmUpDot) },
            { "fCompassSlideM", 2, offsetof(SplitConfig, slideM) },
            { "fCompassSlideRate", 2, offsetof(SplitConfig, slideRate) },
        };

        std::string SplitConfigLine(const SplitConfig& c)
        {
            return std::format("hand={} w={:.3f} off={:.3f},{:.3f},{:.3f} pitch={:.0f} | compass chin={} d={:.2f} h={:.2f} w={:.2f} show={} hand={} "
                               "raise {:.2f}..{:.2f} palm>{:.2f} slide {:.2f} m rate {:.1f}",
                c.handHud ? 1 : 0, c.handWidthM, c.handX, c.handY, c.handZ, c.handPitchDeg, c.compassSplit ? 1 : 0, c.compassDistM,
                c.compassHeightM, c.compassWidthM, c.compassShow, c.compassHand, c.raiseHighM, c.raiseLowM, c.palmUpDot, c.slideM,
                c.slideRate);
        }
    }

    void ConfigureSplit(const SplitConfig& a_cfg)
    {
        std::scoped_lock l(g_splitLock);
        g_split = a_cfg;
    }

    SplitConfig GetSplitConfig()
    {
        std::scoped_lock l(g_splitLock);
        return g_split;
    }

    void LoadSplitConfig()
    {
        SplitConfig c;
        for (const auto& k : kSplitKeys) {
            auto* p = reinterpret_cast<std::uint8_t*>(&c) + k.off;
            if (k.kind == 0) {
                *reinterpret_cast<bool*>(p) = ReadIniBool(k.name, *reinterpret_cast<bool*>(p));
            } else if (k.kind == 1) {
                *reinterpret_cast<int*>(p) = ReadIniInt(k.name, *reinterpret_cast<int*>(p));
            } else {
                *reinterpret_cast<float*>(p) = ReadIniFloat(k.name, *reinterpret_cast<float*>(p));
            }
        }
        c.leftHanded = ::GetPrivateProfileIntA("Controllers", "bLeftHanded", 0, Settings::Path().c_str()) != 0;
        ConfigureSplit(c);
        REX::INFO("hud split (FO4VR HUD placement): {} leftHanded={}", SplitConfigLine(c), c.leftHanded ? 1 : 0);
    }

    std::string SetSplitValue(const std::string& a_key, const std::string& a_value)
    {
        for (const auto& k : kSplitKeys) {
            if (_stricmp(k.name, a_key.c_str()) != 0) {
                continue;
            }
            float v = 0.0f;
            if (_stricmp(a_value.c_str(), "on") == 0 || _stricmp(a_value.c_str(), "true") == 0) {
                v = 1.0f;
            } else if (_stricmp(a_value.c_str(), "off") == 0 || _stricmp(a_value.c_str(), "false") == 0) {
                v = 0.0f;
            } else {
                char* end = nullptr;
                v = std::strtof(a_value.c_str(), &end);
                if (end == a_value.c_str()) {
                    return "error: hud set " + a_key + ": not a value: '" + a_value + "'";
                }
            }
            SplitConfig c = GetSplitConfig();
            auto*       p = reinterpret_cast<std::uint8_t*>(&c) + k.off;
            if (k.kind == 0) {
                *reinterpret_cast<bool*>(p) = v != 0.0f;
            } else if (k.kind == 1) {
                *reinterpret_cast<int*>(p) = static_cast<int>(std::lround(v));
            } else {
                *reinterpret_cast<float*>(p) = v;
            }
            ConfigureSplit(c);
            REX::INFO("hud split: {} = {} (live)", k.name, a_value);
            return "hud split " + SplitConfigLine(c);
        }
        std::string keys;
        for (const auto& k : kSplitKeys) {
            keys += std::string(keys.empty() ? "" : " ") + k.name;
        }
        return "error: hud set: unknown key '" + a_key + "' (keys: " + keys + ")";
    }

    void SetPieceRect(int a_piece, float a_u0, float a_v0, float a_u1, float a_v1, bool a_valid)
    {
        if (a_piece < 0 || a_piece >= kPieceCount) {
            return;
        }
        std::scoped_lock l(g_splitLock);
        g_rect01[a_piece] = { a_u0, a_v0, a_u1, a_v1, a_valid && a_u1 > a_u0 && a_v1 > a_v0 };
    }

    void SetPieceAniso(float a_sxOverSy)
    {
        std::scoped_lock l(g_splitLock);
        g_aniso = a_sxOverSy > 0.1f && a_sxOverSy < 10.0f ? a_sxOverSy : 1.0f;
    }

    bool SplitWanted()
    {
        const SplitConfig c = GetSplitConfig();
        return g_enabled.load() && (c.handHud || c.compassSplit || c.compassShow != 0);
    }

    void UpdateFrame(xr::Runtime& a_rt, bool a_began, const xr::Pose* a_head)
    {
        (void)a_rt;
        std::scoped_lock l(g_splitLock);
        g_headValid = a_began && a_head;
        if (g_headValid) {
            g_head = *a_head;
        }
    }

    namespace
    {
        // Cuts this frame's pieces out of the HUD copy a_hud into g_atlas (their rectangles are then cleared in a_hud, so the
        // head-locked HUD quad no longer shows them) and queues them. Present thread, before the main HUD quad is submitted.
        // Returns the number of piece layers queued.
        int SubmitSplit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx, Copy& a_hud, bool a_premul)
        {
            SplitConfig c;
            Rect01      r01[kPieceCount];
            xr::Pose    head;
            bool        headOk;
            float       aniso = 1.0f;
            {
                std::scoped_lock l(g_splitLock);
                c = g_split;
                for (int i = 0; i < kPieceCount; ++i) {
                    r01[i] = g_rect01[i];
                }
                head = g_head;
                headOk = g_headValid;
                aniso = g_aniso;
            }
            const auto  now = std::chrono::steady_clock::now();
            const float dt = g_lastFrame.time_since_epoch().count() ? std::clamp(std::chrono::duration<float>(now - g_lastFrame).count(), 0.0f, 0.05f)
                                                                    : 0.0f;  // VRIK clamps its frame time to 0.05 s too
            g_lastFrame = now;
            const auto& ctl = a_rt.Controllers();

            // compass visibility (VRIK: decided every frame, no debounce; the slide smooths it)
            bool want = g_compassWant;
            if (c.compassShow == 0) {
                want = true;
            } else if (c.compassShow == 1) {
                want = false;
            } else if (headOk && ctl.active) {
                bool any = false, seen = false;
                if (c.compassHand != 2 && ctl.leftGripPoseValid) {
                    seen = true;
                    any = any || HandShows(c, head, ctl.leftGripPose, true);
                }
                if (c.compassHand != 1 && ctl.rightGripPoseValid) {
                    seen = true;
                    any = any || HandShows(c, head, ctl.rightGripPose, false);
                }
                if (seen) {
                    want = any;  // VRIK: hands not tracked keep the last state
                }
            }
            if (want != g_compassWant) {
                ++g_compassToggles;
            }
            g_compassWant = want;
            // VRIK VrikBody::showHideControllers 18000EFB0: z += dt * 10 * (z - 1) * (hidden ? +1 : -1), clamped to [-50, 0]
            g_slideU = std::clamp(g_slideU + dt * c.slideRate * (g_slideU - 1.0f) * (want ? -1.0f : 1.0f), -50.0f, 0.0f);
            if (c.compassShow == 0) {
                g_slideU = 0.0f;
            } else if (c.compassShow == 1) {
                g_slideU = -50.0f;
            }
            const bool compassVisible = g_slideU > -50.0f;

            const UINT W = a_hud.desc.Width, H = a_hud.desc.Height;
            auto       px = [&](const Rect01& r) {
                PxRect p;
                if (!r.valid || !W || !H) {
                    return p;
                }
                const int x0 = std::clamp(static_cast<int>(std::floor(r.u0 * W)), 0, static_cast<int>(W));
                const int y0 = std::clamp(static_cast<int>(std::floor(r.v0 * H)), 0, static_cast<int>(H));
                const int x1 = std::clamp(static_cast<int>(std::ceil(r.u1 * W)), 0, static_cast<int>(W));
                const int y1 = std::clamp(static_cast<int>(std::ceil(r.v1 * H)), 0, static_cast<int>(H));
                p = { x0, y0, x1 - x0, y1 - y0 };
                return p;
            };
            const PxRect hp = px(r01[kPieceHp]), rt = px(r01[kPieceRight]), cp = px(r01[kPieceCompass]);

            // hand: the aim hand's aim pose (FO4VR: PrimaryUIAttachNode under PrimaryWandNode)
            const bool  leftAim = c.leftHanded;
            const bool  aimOk = ctl.active && (leftAim ? ctl.leftAimValid : ctl.rightAimValid);
            const auto& aim = leftAim ? ctl.leftAim : ctl.rightAim;
            const bool  handOn = c.handHud && aimOk && (hp.w > 0 || rt.w > 0);
            const bool  compassMask = cp.w > 0 && (c.compassSplit || !compassVisible);
            const bool  compassOn = c.compassSplit && cp.w > 0 && compassVisible && headOk;
            if (!handOn && !compassMask) {
                return 0;
            }

            // atlas layout: row 0 = [HP | right meters], bottom-aligned, 4 px apart; row 1 = compass
            const int gap = 4;
            const int hpW = handOn ? hp.w : 0, rtW = handOn ? rt.w : 0;
            const int row0H = handOn ? (std::max)(hpW ? hp.h : 0, rtW ? rt.h : 0) : 0;
            const int row0W = hpW + rtW + (hpW && rtW ? gap : 0);
            const int dyC = row0H + (row0H && compassOn ? gap : 0);
            const int aw = (std::max)({ row0W, compassOn ? cp.w : 0, 1 });
            const int ah = (std::max)(dyC + (compassOn ? cp.h : 0), 1);
            // the atlas only grows (64 px steps), so small layout changes do not reallocate it
            if (!g_atlas || g_atlasDesc.Format != a_hud.desc.Format || g_atlasDesc.Width < static_cast<UINT>(aw) ||
                g_atlasDesc.Height < static_cast<UINT>(ah)) {
                const UINT nw = (std::max)(static_cast<UINT>((aw + 63) & ~63), g_atlasDesc.Format == a_hud.desc.Format ? g_atlasDesc.Width : 0u);
                const UINT nh = (std::max)(static_cast<UINT>((ah + 63) & ~63), g_atlasDesc.Format == a_hud.desc.Format ? g_atlasDesc.Height : 0u);
                if (!MakeZeroed(g_atlas, g_atlasDesc, nw, nh, a_hud.desc.Format, "piece atlas")) {
                    return 0;
                }
                ++g_atlasBuilds;
            }
            const int maskW = (std::max)({ hp.w, rt.w, cp.w, 1 }), maskH = (std::max)({ hp.h, rt.h, cp.h, 1 });
            if (!g_zero || g_zeroDesc.Format != a_hud.desc.Format || g_zeroDesc.Width < static_cast<UINT>(maskW) ||
                g_zeroDesc.Height < static_cast<UINT>(maskH)) {
                if (!MakeZeroed(g_zero, g_zeroDesc, static_cast<UINT>((maskW + 63) & ~63), static_cast<UINT>((maskH + 63) & ~63), a_hud.desc.Format,
                        "mask")) {
                    return 0;
                }
            }
            // clear the used atlas area (pieces change size: no stale pixels in the gaps)
            {
                const UINT zw = (std::min)(g_zeroDesc.Width, g_atlasDesc.Width), zh = (std::min)(g_zeroDesc.Height, g_atlasDesc.Height);
                for (UINT y = 0; y < static_cast<UINT>(ah); y += zh) {
                    for (UINT x = 0; x < static_cast<UINT>(aw); x += zw) {
                        const D3D11_BOX zb{ 0, 0, 0, (std::min)(zw, g_atlasDesc.Width - x), (std::min)(zh, g_atlasDesc.Height - y), 1 };
                        a_ctx->CopySubresourceRegion(g_atlas.Get(), 0, x, y, 0, g_zero.Get(), 0, &zb);
                    }
                }
            }
            auto cut = [&](const PxRect& r, int a_dx, int a_dy) {
                if (r.w <= 0 || r.h <= 0) {
                    return;
                }
                const D3D11_BOX b{ static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, static_cast<UINT>(r.x + r.w), static_cast<UINT>(r.y + r.h), 1 };
                if (a_dx >= 0) {
                    a_ctx->CopySubresourceRegion(g_atlas.Get(), 0, static_cast<UINT>(a_dx), static_cast<UINT>(a_dy), 0, a_hud.tex.Get(), 0, &b);
                }
                const D3D11_BOX zb{ 0, 0, 0, static_cast<UINT>(r.w), static_cast<UINT>(r.h), 1 };
                a_ctx->CopySubresourceRegion(a_hud.tex.Get(), 0, static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, g_zero.Get(), 0, &zb);
            };
            xr::Runtime::HudPiece pieces[2];
            int                   n = 0;
            // stage px -> target px may be anisotropic (ExactFit over a non-16:9 viewport): the quad height undoes it (stage aspect)
            const float stretch = aniso;
            if (handOn) {
                cut(hp, 0, row0H - hp.h);
                cut(rt, hpW + (hpW && rtW ? gap : 0), row0H - rt.h);
                auto& p = pieces[n++];
                p.x = 0, p.y = 0, p.w = row0W, p.h = row0H;
                p.inView = false;
                p.widthMeters = c.handWidthM;
                p.heightMeters = c.handWidthM * static_cast<float>(row0H) / static_cast<float>((std::max)(row0W, 1)) * stretch;
                // aim pose: -Z along the ray, +Y up, +X right; the quad shows its +Z side = back toward the user
                const float    half = c.handPitchDeg * 0.5f * 0.0174532925f;
                const xr::Quat tilt{ -std::sin(half), 0.0f, 0.0f, std::cos(half) };
                const xr::Vec3 off = QRot(aim.orientation, { leftAim ? -c.handX : c.handX, c.handY, c.handZ });
                p.pose.orientation = QMul(aim.orientation, tilt);
                p.pose.position = { aim.position.x + off.x, aim.position.y + off.y, aim.position.z + off.z };
                ++g_handFrames;
            }
            if (compassMask) {
                cut(cp, compassOn ? 0 : -1, dyC);
                if (compassOn) {
                    auto& p = pieces[n++];
                    p.x = 0, p.y = dyC, p.w = cp.w, p.h = cp.h;
                    p.inView = false;
                    p.widthMeters = c.compassWidthM;
                    p.heightMeters = c.compassWidthM * static_cast<float>(cp.h) / static_cast<float>(cp.w) * stretch;
                    // FO4VR world_HMD_status.nif compass_panel on UprightHmdNode, facing the eye; VRIK's slide moves it down
                    const xr::Quat qy = UprightYaw(head);
                    const float    h = c.compassHeightM, d = c.compassDistM;
                    const float    tilt = std::atan2(h, d);
                    const xr::Quat qx{ std::sin(tilt * 0.5f), 0.0f, 0.0f, std::cos(tilt * 0.5f) };
                    const float    slide = g_slideU / 50.0f * c.slideM;  // 0 .. -slideM
                    const xr::Vec3 o = QRot(qy, { 0.0f, h + slide, -d });
                    p.pose.orientation = QMul(qy, qx);
                    p.pose.position = { head.position.x + o.x, head.position.y + o.y, head.position.z + o.z };
                    ++g_compassFrames;
                }
            }
            if (n > 0 && a_rt.SubmitHudPieces(a_ctx, g_atlas.Get(), pieces, n, a_premul)) {
                ++g_pieceFrames;
            }
            {
                std::scoped_lock l(g_splitLock);
                g_splitLine = std::format("this frame: hand {} compass {} | px hp {}x{}@{},{} right {}x{}@{},{} compass {}x{}@{},{} (HUD copy {}x{}) atlas {}x{}",
                    handOn ? 1 : 0, compassOn ? "1" : compassMask ? "masked" : "0", hp.w, hp.h, hp.x, hp.y, rt.w, rt.h, rt.x, rt.y, cp.w, cp.h, cp.x, cp.y, W,
                    H, g_atlasDesc.Width, g_atlasDesc.Height);
            }
            return n;
        }
    }

    std::string SplitStatus()
    {
        const SplitConfig c = GetSplitConfig();
        std::string       line;
        Rect01            r[kPieceCount];
        {
            std::scoped_lock l(g_splitLock);
            line = g_splitLine;
            for (int i = 0; i < kPieceCount; ++i) {
                r[i] = g_rect01[i];
            }
        }
        std::string                  rects;
        static constexpr const char* kNames[] = { "hp", "right", "compass" };
        for (int i = 0; i < kPieceCount; ++i) {
            rects += r[i].valid ? std::format(" {}=[{:.3f},{:.3f}..{:.3f},{:.3f}]", kNames[i], r[i].u0, r[i].v0, r[i].u1, r[i].v1)
                                : std::format(" {}=none", kNames[i]);
        }
        return std::format("split {} | rects{} | pieceFrames={} handFrames={} compassFrames={} compassWant={} slide={:.1f}u toggles={} atlasBuilds={} | {}",
            SplitConfigLine(c), rects, g_pieceFrames.load(), g_handFrames.load(), g_compassFrames.load(), g_compassWant ? 1 : 0, g_slideU,
            g_compassToggles.load(), g_atlasBuilds.load(), line);
    }

    void Configure(const Config& a_cfg)
    {
        {
            std::scoped_lock l(g_lock);
            g_cfg = a_cfg;
        }
        g_enabled = a_cfg.enabled;
        g_source = a_cfg.source == 0x3F ? 0x3F : 0x3E;
        g_premultiplied = a_cfg.premultiplied;
        g_hideCrosshair = a_cfg.hideCrosshair;
    }

    const Config& GetConfig()
    {
        return g_cfg;
    }

    void SetEnabled(bool a_on)
    {
        g_enabled = a_on;
        if (!a_on) {
            g_layerActive = false;
        }
    }

    bool Enabled()
    {
        return g_enabled.load() && game::HudHooksInstalled();
    }

    void SetSource(int a_rt)
    {
        g_source = a_rt == 0x3E ? 0x3E : 0x3F;
    }

    void SetPremultiplied(bool a_on)
    {
        g_premultiplied = a_on;
    }

    void SetHideCrosshair(bool a_on)
    {
        g_hideCrosshair = a_on;
    }

    void OnHudTargets(void* a_tex3E, void* a_tex3F)
    {
        bool want = g_enabled.load();
        if (!want) {
            std::scoped_lock l(g_lock);
            want = !g_capturePath.empty();
        }
        if (!want) {
            return;
        }
        if (a_tex3E) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3E), g_copy[0]);
        }
        if (a_tex3F) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3F), g_copy[1]);
        }
        ++g_copies;
    }

    bool SkipWorldHud()
    {
        return g_enabled.load(std::memory_order_relaxed) && g_layerActive.load(std::memory_order_relaxed);
    }

    bool HideCrosshair()
    {
        return g_hideCrosshair.load(std::memory_order_relaxed) && SkipWorldHud();
    }

    void NoteWorldHudSkipped()
    {
        g_skippedThisFrame = true;
    }

    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx)
    {
        if (!Enabled()) {
            return false;
        }
        const Copy& c = g_copy[Index(g_source.load())];
        const bool  hudFresh = c.fresh && c.tex;
        const bool  paFresh = g_paB.fresh && g_paW.fresh && g_paB.tex && g_paW.tex;
        if (!hudFresh && !paFresh) {
            return false;  // no HUD drawn this frame (HUD closed, Interface3D off, ...): no layer
        }
        float d, w, h;
        {
            std::scoped_lock l(g_lock);
            d = g_cfg.distanceMeters, w = g_cfg.widthMeters, h = g_cfg.heightMeters;
        }
        // FO4VR HUD split (meters on the aim hand, compass on the chin panel): cut out of this frame's copy before it is shown.
        // Not in power armour (FO4VR's PA helmet HUD carries them there; the PA dash composite keeps the whole HUD).
        if (hudFresh && !paFresh) {
            SubmitSplit(a_rt, a_ctx, g_copy[Index(g_source.load())], g_premultiplied.load());
        }
        // Power armour: the dash captured this frame goes under the HUD in one premultiplied texture.
        ID3D11Texture2D* shown = hudFresh ? c.tex.Get() : nullptr;
        bool             premul = g_premultiplied.load();
        if (paFresh) {
            if (ID3D11Texture2D* comp = Composite(a_ctx, hudFresh ? &c : nullptr, true, premul)) {
                shown = comp;
                premul = true;
            }
        }
        if (!shown) {
            return false;
        }
        // Head-locked: centre (0, h, -d) in VIEW space, tilted about X so the quad faces the eye.
        const float    tilt = std::atan2(h, d);
        const xr::Pose pose{ { std::sin(tilt * 0.5f), 0.0f, 0.0f, std::cos(tilt * 0.5f) }, { 0.0f, h, -d } };
        if (!a_rt.SubmitHud(a_ctx, shown, pose, w, premul)) {
            return false;
        }
        ++g_submits;
        return true;
    }

    void OnPresent(bool a_layerNext)
    {
        ++g_presents;
        std::string path;
        {
            std::scoped_lock l(g_lock);
            path.swap(g_capturePath);
        }
        if (!path.empty()) {
            ServiceCapture(path);
        }
        if (g_skippedThisFrame) {
            ++g_skippedFrames;
        }
        g_skippedThisFrame = false;
        g_copy[0].fresh = g_copy[1].fresh = false;
        g_paB.fresh = g_paW.fresh = false;
        g_menuCopy[0].fresh = g_menuCopy[1].fresh = false;
        if (g_menuSkippedThisFrame) {
            ++g_menuSkipFrames;
        }
        g_menuSkippedThisFrame = false;
        g_compValid = false;
        if (g_pvThisFrame) {
            ++g_pvFrames;
        }
        g_pvThisFrame = false;
        g_menuDrawnThisFrame = false;
        g_menuCompValid = false;
        g_pvB.fresh = g_pvW.fresh = false;
        g_layerActive = a_layerNext && g_enabled.load() && game::HudHooksInstalled();
        game::HudFrameworkTick();  // HUDFramework widget guard / layout log (no-op without HUDFramework)
        game::HudSplitTick();      // FO4VR HUD split: clip rectangles (no-op with every piece off)
    }

    void ConfigurePowerArmor(bool a_hudOnLayer, bool a_hideRain)
    {
        g_paOn = a_hudOnLayer;
        g_rainOff = a_hideRain;
    }
    void SetPaHudOnLayer(bool a_on) { g_paOn = a_on; }
    void SetHideVisorRain(bool a_on) { g_rainOff = a_on; }
    bool PaHudOnLayer() { return g_paOn.load(std::memory_order_relaxed) && SkipWorldHud(); }
    bool HideVisorRain() { return g_rainOff.load(std::memory_order_relaxed) && SkipWorldHud(); }

    bool CapturePaOverlay(void* a_destTex, void* a_destRtv, void (*a_draw)(void*), void* a_ctx)
    {
        auto* tex = static_cast<ID3D11Texture2D*>(a_destTex);
        auto* rtv = static_cast<ID3D11RenderTargetView*>(a_destRtv);
        if (!tex || !rtv || !a_draw) {
            ++g_paFailed;
            return false;
        }
        D3D11_TEXTURE2D_DESC sd{};
        tex->GetDesc(&sd);
        if (sd.SampleDesc.Count > 1) {
            ++g_paFailed;
            return false;
        }
        if (!g_dev) {
            tex->GetDevice(&g_dev);
            g_dev->GetImmediateContext(&g_ctx);
        }
        if (!g_paSave || g_paSaveDesc.Width != sd.Width || g_paSaveDesc.Height != sd.Height || g_paSaveDesc.Format != sd.Format) {
            g_paSave.Reset();
            D3D11_TEXTURE2D_DESC td = sd;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = 0;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_paSave))) {
                REX::ERROR("hud pa: save texture {}x{} fmt {} failed", sd.Width, sd.Height, static_cast<int>(sd.Format));
                g_paSaveDesc = {};
                ++g_paFailed;
                return false;
            }
            g_paSaveDesc = td;
            REX::INFO("hud pa: destination {}x{} format {} (bind 0x{:X})", sd.Width, sd.Height, static_cast<int>(sd.Format), sd.BindFlags);
            std::scoped_lock l(g_lock);
            g_paDestLine = std::format("{}x{} fmt {}", sd.Width, sd.Height, static_cast<int>(sd.Format));
        }
        g_ctx->CopySubresourceRegion(g_paSave.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        static constexpr float kBlack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        g_ctx->ClearRenderTargetView(rtv, kBlack);
        a_draw(a_ctx);
        CopyTarget(tex, g_paB);
        g_ctx->ClearRenderTargetView(rtv, kWhite);
        a_draw(a_ctx);
        CopyTarget(tex, g_paW);
        g_ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, g_paSave.Get(), 0, nullptr);
        ++g_paCaptures;
        return true;
    }

    std::string PaStatus()
    {
        std::string dest;
        {
            std::scoped_lock l(g_lock);
            dest = g_paDestLine;
        }
        return std::format("paHud={} hideRain={} layerActive={} captures={} composites={} failed={} dest {} paTex {}x{} fmt {}", g_paOn.load() ? 1 : 0,
            g_rainOff.load() ? 1 : 0, SkipWorldHud() ? 1 : 0, g_paCaptures.load(), g_paComposites.load(), g_paFailed.load(), dest, g_paB.desc.Width,
            g_paB.desc.Height, static_cast<int>(g_paB.desc.Format));
    }

    void ConfigureMenuPanel(bool a_enabled, int a_source, bool a_premultiplied)
    {
        g_menuOn = a_enabled;
        g_menuSource = a_source == 0x3F ? 0x3F : 0x3E;
        g_menuPremul = a_premultiplied;
    }

    bool MenuPanelEnabled() { return g_menuOn.load() && game::HudHooksInstalled(); }

    void SetMenuPanelEnabled(bool a_on)
    {
        g_menuOn = a_on;
        if (!a_on) {
            g_menuActive = false;
        }
    }

    bool MenuPanelWanted()
    {
        // Copy whenever FlatScreenModel draws while the switch is on (also the first frame of a menu, before the
        // Presenter has seen it open), or when a hudcap is pending.
        if (g_menuOn.load(std::memory_order_relaxed)) {
            return true;
        }
        std::scoped_lock l(g_lock);
        return !g_capturePath.empty();
    }

    void OnMenuTargets(void* a_tex3E, void* a_tex3F)
    {
        bool both = false;
        {
            std::scoped_lock l(g_lock);
            both = !g_capturePath.empty();
        }
        const int src = g_menuSource.load();
        if (a_tex3E && (both || src == 0x3E)) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3E), g_menuCopy[0]);
            g_menuCopyPresent[0] = g_presents.load();
        }
        if (a_tex3F && (both || src == 0x3F)) {
            CopyTarget(static_cast<ID3D11Texture2D*>(a_tex3F), g_menuCopy[1]);
            g_menuCopyPresent[1] = g_presents.load();
        }
        ++g_menuCopies;
    }

    bool SkipWorldMenu() { return g_menuActive.load(std::memory_order_relaxed); }

    void NoteWorldMenuSkipped()
    {
        ++g_menuSkips;
        g_menuSkippedThisFrame = true;
        g_menuDrawnThisFrame = true;
    }

    void SetMenuPanelNext(bool a_on) { g_menuActive = a_on && MenuPanelEnabled(); }

    void SetMenuPreviewEnabled(bool a_on) { g_pvOn = a_on; }
    bool MenuPreviewEnabled() { return g_pvOn.load(); }
    bool MenuPreviewWanted() { return g_pvOn.load(std::memory_order_relaxed) && g_menuActive.load(std::memory_order_relaxed); }

    bool CaptureMenuPreview(const char* a_name, void* a_destTex, void* a_destRtv, void (*a_draw)(void*), void* a_ctx)
    {
        auto* tex = static_cast<ID3D11Texture2D*>(a_destTex);
        auto* rtv = static_cast<ID3D11RenderTargetView*>(a_destRtv);
        if (!tex || !rtv || !a_draw) {
            ++g_pvFailed;
            return false;
        }
        D3D11_TEXTURE2D_DESC sd{};
        tex->GetDesc(&sd);
        if (sd.SampleDesc.Count > 1) {
            ++g_pvFailed;
            return false;
        }
        if (!g_dev) {
            tex->GetDevice(&g_dev);
            g_dev->GetImmediateContext(&g_ctx);
        }
        if (!g_pvSave || g_pvSaveDesc.Width != sd.Width || g_pvSaveDesc.Height != sd.Height || g_pvSaveDesc.Format != sd.Format) {
            g_pvSave.Reset();
            D3D11_TEXTURE2D_DESC td = sd;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = 0;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_pvSave))) {
                REX::ERROR("hud preview: save texture {}x{} fmt {} failed", sd.Width, sd.Height, static_cast<int>(sd.Format));
                g_pvSaveDesc = {};
                ++g_pvFailed;
                return false;
            }
            g_pvSaveDesc = td;
            REX::INFO("hud preview: destination {}x{} format {} (bind 0x{:X})", sd.Width, sd.Height, static_cast<int>(sd.Format), sd.BindFlags);
        }
        // A second renderer in the same frame draws over the first one's result (the game's own order and blending).
        const auto same = [&](const Copy& c) {
            return c.tex && c.desc.Width == sd.Width && c.desc.Height == sd.Height && c.desc.Format == sd.Format;
        };
        const bool accumulate = g_pvThisFrame && same(g_pvB) && same(g_pvW);
        g_ctx->CopySubresourceRegion(g_pvSave.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        static constexpr float kBlack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (accumulate) {
            g_ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, g_pvB.tex.Get(), 0, nullptr);
        } else {
            g_ctx->ClearRenderTargetView(rtv, kBlack);
        }
        a_draw(a_ctx);
        CopyTarget(tex, g_pvB);
        if (accumulate) {
            g_ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, g_pvW.tex.Get(), 0, nullptr);
        } else {
            g_ctx->ClearRenderTargetView(rtv, kWhite);
        }
        a_draw(a_ctx);
        CopyTarget(tex, g_pvW);
        g_ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, g_pvSave.Get(), 0, nullptr);
        if (!g_pvThisFrame) {
            g_pvOnTop = g_menuDrawnThisFrame;  // drawn after the menu -> over it on the panel
        }
        g_pvThisFrame = true;
        if (++g_pvCaptures == 1 || a_name) {
            std::scoped_lock l(g_lock);
            if (a_name && g_pvNames.find(a_name) == std::string::npos) {
                g_pvNames += g_pvNames.empty() ? a_name : std::string(",") + a_name;
                REX::INFO("hud preview: renderer '{}' captured onto the menu panel ({} the menu)", a_name, g_pvOnTop ? "drawn after" : "drawn before");
            }
        }
        return true;
    }

    void* MenuPanelTexture(bool& a_premultiplied)
    {
        a_premultiplied = g_menuPremul.load();
        const int   i = Index(g_menuSource.load());
        const Copy& c = g_menuCopy[i];
        // This frame's copy, or the last one while it is recent (a frame where the renderer did not run).
        const bool menuOk = c.tex && (c.fresh || g_presents.load() - g_menuCopyPresent[i] <= 30);
        // 3D item preview captured this frame: composited with the menu (premultiplied result)
        if (g_pvThisFrame && g_pvB.fresh && g_pvW.fresh && g_ctx) {
            if (CompositeInto(g_ctx.Get(), menuOk ? &c : nullptr, a_premultiplied, &g_pvB, &g_pvW, g_pvOnTop, g_menuComp, g_menuCompRtv,
                    g_menuCompDesc, 4, "preview")) {
                ++g_pvComposites;
                ++g_menuShown;
                g_menuCompValid = true;
                a_premultiplied = true;
                return g_menuComp.Get();
            }
        }
        if (!menuOk) {
            return nullptr;
        }
        ++g_menuShown;
        return c.tex.Get();
    }

    namespace
    {
        std::atomic<bool>          g_clearMenu{ true };
        std::atomic<std::uint64_t> g_menuClears{ 0 };
    }

    bool ClearMenuTargetsWanted() { return g_clearMenu.load(std::memory_order_relaxed); }
    namespace
    {
        std::atomic<bool> g_clearHud{ true };
    }
    bool ClearHudTargetsWanted() { return g_clearHud.load(std::memory_order_relaxed); }
    void SetClearHudTargets(bool a_on) { g_clearHud = a_on; }
    void SetClearMenuTargets(bool a_on) { g_clearMenu = a_on; }

    void ClearMenuTargets(void* a_rtv3E, void* a_rtv3F)
    {
        static constexpr float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (void* p : { a_rtv3E, a_rtv3F }) {
            auto* rtv = static_cast<ID3D11RenderTargetView*>(p);
            if (!rtv) {
                continue;
            }
            if (!g_ctx) {
                rtv->GetDevice(&g_dev);
                g_dev->GetImmediateContext(&g_ctx);
            }
            g_ctx->ClearRenderTargetView(rtv, kZero);
        }
        ++g_menuClears;
    }

    std::string MenuPanelStatus()
    {
        return std::format("clearTargets={} clears={} ", g_clearMenu.load() ? 1 : 0, g_menuClears.load()) +
               std::format("panel on={} hooks={} active={} src={:X} premul={} copies={} h2Skips={} skipFrames={} shown={} copy {}x{} fmt {} age {}",
            g_menuOn.load() ? 1 : 0, game::HudHooksInstalled() ? 1 : 0, g_menuActive.load() ? 1 : 0, g_menuSource.load(), g_menuPremul.load() ? 1 : 0,
            g_menuCopies.load(), g_menuSkips.load(), g_menuSkipFrames.load(), g_menuShown.load(), g_menuCopy[Index(g_menuSource.load())].desc.Width,
            g_menuCopy[Index(g_menuSource.load())].desc.Height, static_cast<int>(g_menuCopy[Index(g_menuSource.load())].desc.Format),
            g_presents.load() - g_menuCopyPresent[Index(g_menuSource.load())]) +
               [] {
                   std::scoped_lock l(g_lock);
                   return std::format(" preview on={} captures={} frames={} composites={} failed={} onTop={} dest {}x{} fmt {} renderers '{}'",
                       g_pvOn.load() ? 1 : 0, g_pvCaptures.load(), g_pvFrames.load(), g_pvComposites.load(), g_pvFailed.load(), g_pvOnTop ? 1 : 0,
                       g_pvSaveDesc.Width, g_pvSaveDesc.Height, static_cast<int>(g_pvSaveDesc.Format), g_pvNames);
               }();
    }

    void RequestCapture(std::string a_path)
    {
        std::scoped_lock l(g_lock);
        g_capturePath = std::move(a_path);
    }

    std::string Status()
    {
        std::string last;
        Config      c;
        {
            std::scoped_lock l(g_lock);
            last = g_lastCapture;
            c = g_cfg;
        }
        return std::format("enabled={} hooks={} layer={} src={:X} premul={} dist={:.2f} width={:.2f} height={:.2f} hideCrosshair={} "
                           "copies={} submits={} worldHudSkippedFrames={} presents={} {} | last hudcap {}",
            g_enabled.load() ? 1 : 0, game::HudHooksInstalled() ? 1 : 0, g_layerActive.load() ? 1 : 0, g_source.load(),
            g_premultiplied.load() ? 1 : 0, c.distanceMeters, c.widthMeters, c.heightMeters, g_hideCrosshair.load() ? 1 : 0, g_copies.load(),
            g_submits.load(), g_skippedFrames.load(), g_presents.load(), game::HudHookStats(), last) + " | " + PaStatus();
    }
}
