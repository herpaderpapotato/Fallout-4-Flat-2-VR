// VATS camera window, D3D11 side (VatsWindow.h): GPU timer around the engine render, mask copy, and at Present the
// window image = tone-mapped HDR camera view (+ VATS highlight from the mask) with the VATSMenu copy on top.
#include <REX/LOG.h>
#include "plugin/VatsWindow.h"
#include "plugin/VatsWindowInternal.h"

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

namespace fo4flatvr::vatswin
{
    using Microsoft::WRL::ComPtr;
    using detail::S;

    namespace
    {
        // ---- GPU timer (render thread) ----
        struct TimerSlot
        {
            ComPtr<ID3D11Query> disjoint, t0, t1;
            bool                pending{ false };
        };
        constexpr int kSlots = 4;
        TimerSlot     g_timer[kSlots];
        int           g_timerNext = 0;
        bool          g_timerTried = false, g_timerOk = false;
        std::atomic<double> g_gpuMsAvg{ 0.0 }, g_gpuMsMax{ 0.0 }, g_gpuMsLast{ 0.0 };
        std::atomic<std::uint64_t> g_gpuSamples{ 0 };

        void ReadTimers(ID3D11DeviceContext* a_ctx)
        {
            for (auto& s : g_timer) {
                if (!s.pending) {
                    continue;
                }
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                UINT64                              a = 0, b = 0;
                if (a_ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                    a_ctx->GetData(s.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                    a_ctx->GetData(s.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                    continue;
                }
                s.pending = false;
                if (dj.Disjoint || !dj.Frequency || b < a) {
                    continue;
                }
                const double ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
                g_gpuMsLast.store(ms);
                const auto n = g_gpuSamples.fetch_add(1);
                g_gpuMsAvg.store(n == 0 ? ms : g_gpuMsAvg.load() * 0.9 + ms * 0.1);
                g_gpuMsMax.store((std::max)(g_gpuMsMax.load() * 0.995, ms));
            }
        }

        TimerSlot* g_active = nullptr;

        // ---- compose (Present thread) ----
        constexpr char kHlsl[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> menu  : register(t1);
Texture2D<float4> mask  : register(t2);
Texture2D<float4> albedo : register(t3);
SamplerState      lin   : register(s0);
cbuffer C : register(b0) { float4 p0; float4 p1; };  // p0: exposure, premul, maskOn, maskStrength; p1: src texel (x,y), 0, 0
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 Tone(float3 c) { return 1.0 - exp(-max(c, 0.0) * p0.x); }
// HDR + fill light: albedo x ambient where geometry was drawn (the window view gets no sun / world ambient)
float3 Px(float2 uv) { return Tone(scene.SampleLevel(lin, uv, 0).rgb + albedo.SampleLevel(lin, uv, 0).rgb * p1.z); }
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // 4 bilinear taps over the output pixel's footprint (downscale without heavy aliasing)
    float2 d = p1.xy;
    float3 c = Px(uv + float2(-d.x, -d.y)) + Px(uv + float2(d.x, -d.y)) + Px(uv + float2(-d.x, d.y)) + Px(uv + float2(d.x, d.y));
    c *= 0.25;
    if (p0.z > 0.5) {
        float3 m = mask.SampleLevel(lin, uv, 0).rgb;
        c = c + m * p0.w * (1.0 - c);
    }
    c = pow(saturate(c), 1.0 / 2.2);
    float4 m = menu.SampleLevel(lin, uv, 0);
    c = p0.y > 0.5 ? c * (1.0 - m.a) + m.rgb : lerp(c, m.rgb, m.a);
    return float4(c, 1.0);
}
)";

        struct Res
        {
            bool                          tried{ false }, ok{ false };
            ComPtr<ID3D11VertexShader>    vs;
            ComPtr<ID3D11PixelShader>     ps;
            ComPtr<ID3D11Buffer>          cb;
            ComPtr<ID3D11SamplerState>    smp;
            ComPtr<ID3D11RasterizerState> rs;
            ComPtr<ID3D11BlendState>      bs;
            ComPtr<ID3D11DepthStencilState> dss;
            ComPtr<ID3D11Texture2D>       out;
            ComPtr<ID3D11RenderTargetView> outRtv;
            UINT                          outW{ 0 }, outH{ 0 };
            // SRV caches keyed by the texture pointer
            ID3D11Texture2D*              sceneKey{ nullptr };
            ComPtr<ID3D11ShaderResourceView> sceneSrv;
            ID3D11Texture2D*              menuKey{ nullptr };
            ComPtr<ID3D11ShaderResourceView> menuSrv;
            ComPtr<ID3D11Texture2D>       blankTex;
            ComPtr<ID3D11ShaderResourceView> blankSrv;
            ComPtr<ID3D11Texture2D>       maskTex;
            ComPtr<ID3D11ShaderResourceView> maskSrv;
            ComPtr<ID3D11Texture2D>       albTex;
            ComPtr<ID3D11ShaderResourceView> albSrv;
        };
        Res        g_r;
        std::mutex g_capLock;
        std::string g_capPath;
        std::string g_lastErr;

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

        bool MakeSrv(ID3D11Device* a_dev, ID3D11Texture2D* a_tex, ComPtr<ID3D11ShaderResourceView>& a_out)
        {
            D3D11_TEXTURE2D_DESC td{};
            a_tex->GetDesc(&td);
            if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = ViewFormat(td.Format);
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            a_out.Reset();
            return SUCCEEDED(a_dev->CreateShaderResourceView(a_tex, &sd, &a_out));
        }

        bool CreateRes(ID3D11Device* a_dev)
        {
            if (g_r.tried) {
                return g_r.ok;
            }
            g_r.tried = true;
            ComPtr<ID3DBlob> vsb, psb, err;
            if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "vatswin", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
                FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "vatswin", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
                REX::ERROR("vatswin: shader compile failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
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
            const std::uint32_t black = 0;
            D3D11_TEXTURE2D_DESC btd{};
            btd.Width = btd.Height = 1;
            btd.MipLevels = btd.ArraySize = 1;
            btd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            btd.SampleDesc.Count = 1;
            btd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ &black, 4, 4 };
            if (FAILED(a_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_r.vs)) ||
                FAILED(a_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_r.ps)) ||
                FAILED(a_dev->CreateBuffer(&bd, nullptr, &g_r.cb)) || FAILED(a_dev->CreateSamplerState(&smd, &g_r.smp)) ||
                FAILED(a_dev->CreateRasterizerState(&rd, &g_r.rs)) || FAILED(a_dev->CreateBlendState(&bld, &g_r.bs)) ||
                FAILED(a_dev->CreateDepthStencilState(&dsd, &g_r.dss)) || FAILED(a_dev->CreateTexture2D(&btd, &init, &g_r.blankTex)) ||
                FAILED(a_dev->CreateShaderResourceView(g_r.blankTex.Get(), nullptr, &g_r.blankSrv))) {
                REX::ERROR("vatswin: D3D resource creation failed");
                return false;
            }
            g_r.ok = true;
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
            ID3D11ShaderResourceView* srv[4]{};
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
                ctx->PSGetShaderResources(0, 4, srv);
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
                ID3D11ShaderResourceView* nulls[4]{};
                ctx->PSSetShaderResources(0, 4, nulls);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
                ctx->OMSetDepthStencilState(dss, sref);
                ctx->OMSetBlendState(bs, bf, sm);
                ctx->RSSetViewports(nvp, vps);
                ctx->RSSetState(rs);
                ctx->PSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetSamplers(0, 1, &smp);
                ctx->PSSetShaderResources(0, 4, srv);
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

        void WriteBmpHalf(const std::string& a_path, const std::uint8_t* a_rows, UINT a_pitch, UINT a_w, UINT a_h)
        {
            const UINT w = a_w / 2, h = a_h / 2, rowBytes = w * 4;
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
            if (f == INVALID_HANDLE_VALUE) {
                REX::ERROR("vatswin cap: cannot open {}", a_path);
                return;
            }
            DWORD wr = 0;
            ::WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
            ::WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
            std::vector<std::uint8_t> row(rowBytes);
            for (UINT y = 0; y < h; ++y) {
                const std::uint8_t* src = a_rows + static_cast<std::size_t>(y * 2) * a_pitch;
                for (UINT x = 0; x < w; ++x) {
                    const std::uint8_t* p = src + x * 8;
                    row[x * 4 + 0] = p[2], row[x * 4 + 1] = p[1], row[x * 4 + 2] = p[0], row[x * 4 + 3] = 255;
                }
                ::WriteFile(f, row.data(), rowBytes, &wr, nullptr);
            }
            ::CloseHandle(f);
            REX::INFO("vatswin cap: wrote {} ({}x{})", a_path, w, h);
        }

        void Capture(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex, const std::string& a_path)
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
            if (SUCCEEDED(a_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                WriteBmpHalf(a_path, static_cast<const std::uint8_t*>(m.pData), m.RowPitch, d.Width, d.Height);
                a_ctx->Unmap(st.Get(), 0);
            }
        }
    }

    namespace detail
    {
        void GpuTimerBegin(void* a_ctx)
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(a_ctx);
            if (!g_timerTried) {
                g_timerTried = true;
                ComPtr<ID3D11Device> dev;
                ctx->GetDevice(&dev);
                D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts{ D3D11_QUERY_TIMESTAMP, 0 };
                g_timerOk = true;
                for (auto& s : g_timer) {
                    g_timerOk = g_timerOk && SUCCEEDED(dev->CreateQuery(&dj, &s.disjoint)) && SUCCEEDED(dev->CreateQuery(&ts, &s.t0)) &&
                                SUCCEEDED(dev->CreateQuery(&ts, &s.t1));
                }
            }
            g_active = nullptr;
            if (!g_timerOk) {
                return;
            }
            ReadTimers(ctx);
            auto& s = g_timer[g_timerNext];
            if (s.pending) {
                return;  // still in flight: skip this sample
            }
            g_timerNext = (g_timerNext + 1) % kSlots;
            ctx->Begin(s.disjoint.Get());
            ctx->End(s.t0.Get());
            g_active = &s;
        }

        void GpuTimerEnd(void* a_ctx)
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(a_ctx);
            if (!g_active) {
                return;
            }
            ctx->End(g_active->t1.Get());
            ctx->End(g_active->disjoint.Get());
            g_active->pending = true;
            g_active = nullptr;
        }

        bool CopyInto(void* a_ctx, void* a_srcTex, ComPtr<ID3D11Texture2D>& a_tex, ComPtr<ID3D11ShaderResourceView>& a_srv, const char* a_what)
        {
            auto* ctx = static_cast<ID3D11DeviceContext*>(a_ctx);
            auto* src = static_cast<ID3D11Texture2D*>(a_srcTex);
            if (!ctx || !src) {
                return false;
            }
            D3D11_TEXTURE2D_DESC sd{};
            src->GetDesc(&sd);
            D3D11_TEXTURE2D_DESC md{};
            if (a_tex) {
                a_tex->GetDesc(&md);
            }
            if (!a_tex || md.Width != sd.Width || md.Height != sd.Height || md.Format != sd.Format) {
                ComPtr<ID3D11Device> dev;
                ctx->GetDevice(&dev);
                D3D11_TEXTURE2D_DESC nd = sd;
                nd.MipLevels = 1;
                nd.ArraySize = 1;
                nd.Usage = D3D11_USAGE_DEFAULT;
                nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                nd.CPUAccessFlags = 0;
                nd.MiscFlags = 0;
                a_tex.Reset();
                a_srv.Reset();
                if (sd.MipLevels != 1 || sd.ArraySize != 1 || FAILED(dev->CreateTexture2D(&nd, nullptr, &a_tex)) ||
                    !MakeSrv(dev.Get(), a_tex.Get(), a_srv)) {
                    a_tex.Reset();
                    return false;
                }
                REX::INFO("vatswin: {} copy {}x{} format {}", a_what, sd.Width, sd.Height, static_cast<int>(sd.Format));
            }
            ctx->CopyResource(a_tex.Get(), src);
            return true;
        }

        bool MaskCopy(void* a_ctx, void* a_srcTex) { return CopyInto(a_ctx, a_srcTex, g_r.maskTex, g_r.maskSrv, "mask"); }
        bool AlbedoCopy(void* a_ctx, void* a_srcTex) { return CopyInto(a_ctx, a_srcTex, g_r.albTex, g_r.albSrv, "albedo"); }

        std::string GpuStatus()
        {
            return std::format("gpuAvg={:.3f}ms gpuLast={:.3f}ms gpuMax={:.3f}ms gpuSamples={} out={}x{} composeErr='{}'", g_gpuMsAvg.load(),
                g_gpuMsLast.load(), g_gpuMsMax.load(), g_gpuSamples.load(), g_r.outW, g_r.outH, g_lastErr);
        }

        void RequestCapture(const std::string& a_path)
        {
            std::scoped_lock l(g_capLock);
            g_capPath = a_path;
        }
    }

    ID3D11Texture2D* Compose(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_menu, bool a_premultiplied)
    {
        auto* scene = static_cast<ID3D11Texture2D*>(S().sceneTex.load());
        if (!a_ctx || !scene || S().episodeRenders.load() == 0) {
            return nullptr;
        }
        ComPtr<ID3D11Device> dev;
        a_ctx->GetDevice(&dev);
        if (!CreateRes(dev.Get())) {
            return nullptr;
        }
        if (scene != g_r.sceneKey) {
            if (!MakeSrv(dev.Get(), scene, g_r.sceneSrv)) {
                g_lastErr = "scene SRV";
                return nullptr;
            }
            g_r.sceneKey = scene;
        }
        ID3D11ShaderResourceView* menuSrv = g_r.blankSrv.Get();
        if (a_menu) {
            if (a_menu != g_r.menuKey) {
                g_r.menuKey = MakeSrv(dev.Get(), a_menu, g_r.menuSrv) ? a_menu : nullptr;
            }
            if (g_r.menuKey) {
                menuSrv = g_r.menuSrv.Get();
            }
        }
        D3D11_TEXTURE2D_DESC sd{};
        scene->GetDesc(&sd);
        const UINT w = static_cast<UINT>((std::min)(static_cast<int>(sd.Width), S().width.load()));
        const UINT h = (std::max)(1u, static_cast<UINT>(static_cast<std::uint64_t>(w) * sd.Height / (std::max)(1u, sd.Width)));
        if (!g_r.out || g_r.outW != w || g_r.outH != h) {
            D3D11_TEXTURE2D_DESC od{};
            od.Width = w;
            od.Height = h;
            od.MipLevels = od.ArraySize = 1;
            od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            od.SampleDesc.Count = 1;
            od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            g_r.out.Reset();
            g_r.outRtv.Reset();
            if (FAILED(dev->CreateTexture2D(&od, nullptr, &g_r.out)) || FAILED(dev->CreateRenderTargetView(g_r.out.Get(), nullptr, &g_r.outRtv))) {
                g_lastErr = "output texture";
                g_r.out.Reset();
                return nullptr;
            }
            g_r.outW = w, g_r.outH = h;
            REX::INFO("vatswin: window texture {}x{} (scene {}x{} format {})", w, h, sd.Width, sd.Height, static_cast<int>(sd.Format));
        }
        const bool maskOn = S().mask.load() && S().maskValid.load() && g_r.maskSrv;
        const bool albOn = S().ambient.load() > 0.0f && S().albedoValid.load() && g_r.albSrv;
        const float cbv[8] = { S().exposure.load(), a_premultiplied ? 1.0f : 0.0f, maskOn ? 1.0f : 0.0f, S().maskStrength.load(),
            0.25f * static_cast<float>(sd.Width) / static_cast<float>(w) / static_cast<float>(sd.Width),
            0.25f * static_cast<float>(sd.Height) / static_cast<float>(h) / static_cast<float>(sd.Height), albOn ? S().ambient.load() : 0.0f, 0.0f };
        {
            Saved sv(a_ctx);
            a_ctx->UpdateSubresource(g_r.cb.Get(), 0, nullptr, cbv, 0, 0);
            ID3D11RenderTargetView* rtv = g_r.outRtv.Get();
            a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            a_ctx->OMSetBlendState(g_r.bs.Get(), nullptr, 0xFFFFFFFF);
            a_ctx->OMSetDepthStencilState(g_r.dss.Get(), 0);
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
            a_ctx->RSSetViewports(1, &vp);
            a_ctx->RSSetState(g_r.rs.Get());
            a_ctx->IASetInputLayout(nullptr);
            a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            a_ctx->VSSetShader(g_r.vs.Get(), nullptr, 0);
            a_ctx->HSSetShader(nullptr, nullptr, 0);
            a_ctx->DSSetShader(nullptr, nullptr, 0);
            a_ctx->GSSetShader(nullptr, nullptr, 0);
            a_ctx->PSSetShader(g_r.ps.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[4] = { g_r.sceneSrv.Get(), menuSrv, maskOn ? g_r.maskSrv.Get() : g_r.blankSrv.Get(),
                albOn ? g_r.albSrv.Get() : g_r.blankSrv.Get() };
            a_ctx->PSSetShaderResources(0, 4, srvs);
            ID3D11SamplerState* smp = g_r.smp.Get();
            a_ctx->PSSetSamplers(0, 1, &smp);
            ID3D11Buffer* cb = g_r.cb.Get();
            a_ctx->PSSetConstantBuffers(0, 1, &cb);
            a_ctx->Draw(3, 0);
        }
        S().composes.fetch_add(1);
        std::string cap;
        {
            std::scoped_lock l(g_capLock);
            cap.swap(g_capPath);
        }
        if (!cap.empty()) {
            Capture(dev.Get(), a_ctx, g_r.out.Get(), cap);
        }
        return g_r.out.Get();
    }
}
