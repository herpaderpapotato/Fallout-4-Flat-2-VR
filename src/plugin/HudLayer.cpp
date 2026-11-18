// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/HudLayer.h"
#include "plugin/GameHud.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
