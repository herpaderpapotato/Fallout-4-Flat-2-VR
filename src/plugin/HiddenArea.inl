// HiddenArea.inl - included by StereoDraw.cpp inside its anonymous namespace (after StereoTiled.inl).
//
// 22 Nov (hidden-area agent; work/handoff/stereo-lighting.md "Hidden-area mask"). FO4VR bUseHiddenAreaMesh:VR=1 port:
// the pixels the headset lenses never show are stamped into the scene depth at the near plane (z = 0) before the frame's
// first stereo z-prepass / GBuffer draw, so early-Z rejects them for the prepass, the GBuffer and the depth-tested light
// volumes. The mesh is the runtime's XR_KHR_visibility_mask hidden-triangle mesh per eye (view-space tangents on the
// z = -1 plane; Presenter hands it over), mapped into each eye's region of the double-wide target with that eye's padded
// frustum tangents (g_eyeTan: the region also holds the seam guard bands, which stay unmasked).
//
// FO4VR reference (Fallout4VR.exe 1.2.72): FUN_141d97650 draws the OpenVR hidden-area mesh (FUN_141db5100: both eyes in one
// double-wide tri shape, z = 0) at the start of the z-prepass (FUN_141d955e0, flat DoZPrePass) with render target 0
// unbound, DSS table entry 146234038 = depth mode 2 (write, test always) / stencil mode 1, stencil ref 0xFF. GBuffer and
// image-space passes then use stencil mode 0x29 / 0xB with ref 0x80 (FUN_14291aef0, BSUtilityShader::CreateCommandBuffer
// 1428F48BD, ImageSpaceEffectHDR::Render 142883A70 binds the depth-stencil target for it). We stamp depth only: flat's
// deferred passes have no hidden-area stencil states (that would need DSS twins for every full-screen pass: not done).
//
// Re-stamped after any ClearDepthStencilView of the stamped depth (HkClearDSV); at most once per depth target and clear.

struct HamState
{
    // hand-off (Presenter thread -> render thread; the same thread in practice, locked anyway)
    std::mutex                 lock;
    bool                       ext{ false };       // XR_KHR_visibility_mask offered by the runtime
    std::uint64_t              serial{ 0 };        // mesh version (0 = none)
    std::vector<float>         xy[2];              // tangent-space vertices
    std::vector<std::uint32_t> idx[2];
    float                      synthetic{ 0.0f };  // test aid ("stereoham synth <f>"): corner triangles, legs f of the region
    std::uint64_t              fetches{ 0 };
    // render thread
    std::uint64_t            builtSerial{ ~0ull };
    float                    builtSynthetic{ -1.0f };
    float                    builtTan[2][4]{};
    std::vector<float>       ndc;                  // DW NDC triangle list, both eyes
    UINT                     verts[2]{};           // vertices per eye in ndc (eye 0 first)
    float                    probeNdc[2][2]{};     // centroid of each eye's largest triangle (DW NDC)
    ID3D11Buffer*            vb{ nullptr };
    UINT                     vbBytes{ 0 };
    bool                     vbDirty{ true };
    bool                     initTried{ false };
    ID3D11VertexShader*      vs{ nullptr };
    ID3D11InputLayout*       il{ nullptr };
    ID3D11DepthStencilState* dss{ nullptr };
    ID3D11RasterizerState*   rs{ nullptr };
    ID3D11BlendState*        bs{ nullptr };
    ID3D11Resource*          stampedRes{ nullptr };  // not referenced (the engine's depth target)
    bool                     stampedValid{ false };  // stampedRes holds this frame's stamp (no clear since)
    // stats
    std::uint64_t frameStamps{ 0 }, lastFrameStamps{ 0 }, totalStamps{ 0 }, framesStamped{ 0 }, clearsAfterStamp{ 0 };
    std::uint64_t skipNoDsv{ 0 }, skipSize{ 0 }, skipNoTan{ 0 }, failures{ 0 };
    float         areaShare[2]{};                   // stamped share of each eye region (NDC area)
    // readback probe ("stereoham probe"): copy of the stamped depth at the frame's first deferred light / composite draw
    int           probeState{ 0 };                  // 0 idle, 1 armed
    std::string   probeResult{ "not run" };
    float         lastVp[4]{};                      // viewport of the last stamp (x, y, w, h)
};
HamState      g_ham;
std::atomic<bool> g_hamOn{ true };  // [Stereo] bHiddenAreaMask (Configure)

constexpr const char* kHamHlsl = "float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.0, 1.0); }";

bool HamInit()
{
    if (g_ham.initTried) {
        return g_ham.vs && g_ham.il && g_ham.dss && g_ham.rs && g_ham.bs;
    }
    g_ham.initTried = true;
    ID3DBlob*     code = nullptr;
    ID3DBlob*     err = nullptr;
    const HRESULT hr = ::D3DCompile(kHamHlsl, std::strlen(kHamHlsl), "HiddenAreaVS", nullptr, nullptr, "main", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
        REX::WARN("stereo ham: VS compile: {}", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    if (SUCCEEDED(hr) && code) {
        o_createVS(g_device, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ham.vs);
        const D3D11_INPUT_ELEMENT_DESC el{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        o_createIL(g_device, &el, 1, code->GetBufferPointer(), code->GetBufferSize(), &g_ham.il);
        code->Release();
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;  // FO4VR depth mode 2: write, test always
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;  // the engine's stencil bits stay as they are
    g_device->CreateDepthStencilState(&dd, &g_ham.dss);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rd, &g_ham.rs);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = 0;  // depth only, whatever targets the engine has bound
    g_device->CreateBlendState(&bd, &g_ham.bs);
    REX::INFO("stereo ham: stamp pipeline VS {} IL {} DSS {} RS {} BS {}", static_cast<void*>(g_ham.vs), static_cast<void*>(g_ham.il),
        static_cast<void*>(g_ham.dss), static_cast<void*>(g_ham.rs), static_cast<void*>(g_ham.bs));
    return g_ham.vs && g_ham.il && g_ham.dss && g_ham.rs && g_ham.bs;
}

// Tangent-space mesh -> double-wide NDC triangle list with the eyes' current (padded) frustum tangents.
void HamBuild()
{
    std::vector<float>         xy[2];
    std::vector<std::uint32_t> idx[2];
    float                      synth = 0.0f;
    std::uint64_t              serial = 0;
    {
        std::scoped_lock l(g_ham.lock);
        serial = g_ham.serial;
        synth = g_ham.synthetic;
        if (synth <= 0.0f) {
            for (int e = 0; e < 2; ++e) {
                xy[e] = g_ham.xy[e];
                idx[e] = g_ham.idx[e];
            }
        }
    }
    g_ham.ndc.clear();
    for (int e = 0; e < 2; ++e) {
        const double l = g_eyeTan[e][0], r = g_eyeTan[e][1], t = g_eyeTan[e][2], b = g_eyeTan[e][3];
        if (synth > 0.0f) {
            // test aid: a triangle in each corner of the region, legs = synth x the region's tangent extent
            const double dx = (r - l) * synth, dy = (t - b) * synth;
            const double tri[4][6] = { { l, t, l + dx, t, l, t - dy }, { r, t, r, t - dy, r - dx, t }, { l, b, l, b + dy, l + dx, b },
                { r, b, r - dx, b, r, b + dy } };
            xy[e].clear();
            idx[e].clear();
            for (int c = 0; c < 4; ++c) {
                for (int k = 0; k < 3; ++k) {
                    idx[e].push_back(static_cast<std::uint32_t>(xy[e].size() / 2));
                    xy[e].push_back(static_cast<float>(tri[c][2 * k]));
                    xy[e].push_back(static_cast<float>(tri[c][2 * k + 1]));
                }
            }
        }
        const std::size_t before = g_ham.ndc.size();
        double            area = 0.0, best = -1.0;
        const std::size_t nv = xy[e].size() / 2;
        for (std::size_t i = 0; i + 2 < idx[e].size(); i += 3) {
            double p[3][2]{};
            bool   ok = true;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t v = idx[e][i + k];
                if (v >= nv) {
                    ok = false;
                    break;
                }
                // eye NDC (x right, y up), then the eye's half of the double-wide NDC
                const double ex = (2.0 * xy[e][2 * v] - (l + r)) / (r - l);
                const double ey = (2.0 * xy[e][2 * v + 1] - (t + b)) / (t - b);
                p[k][0] = 0.5 * ex + (e ? 0.5 : -0.5);
                p[k][1] = ey;
            }
            if (!ok) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                g_ham.ndc.push_back(static_cast<float>(p[k][0]));
                g_ham.ndc.push_back(static_cast<float>(p[k][1]));
            }
            const double a = std::abs((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1])) * 0.5;
            area += a;
            if (a > best) {
                best = a;
                g_ham.probeNdc[e][0] = static_cast<float>((p[0][0] + p[1][0] + p[2][0]) / 3.0);
                g_ham.probeNdc[e][1] = static_cast<float>((p[0][1] + p[1][1] + p[2][1]) / 3.0);
            }
        }
        g_ham.verts[e] = static_cast<UINT>((g_ham.ndc.size() - before) / 2);
        g_ham.areaShare[e] = static_cast<float>(area / 2.0);  // an eye region is 1 x 2 in DW NDC
        std::memcpy(g_ham.builtTan[e], g_eyeTan[e], sizeof(g_ham.builtTan[e]));
    }
    g_ham.builtSerial = serial;
    g_ham.builtSynthetic = synth;
    g_ham.vbDirty = true;
    REX::INFO("stereo ham: mesh {} built: triangles L {} R {} | stamped share of the eye region L {:.1f} % R {:.1f} % | tangents L ({:.4f} {:.4f} "
              "{:.4f} {:.4f}) R ({:.4f} {:.4f} {:.4f} {:.4f}){}",
        serial, g_ham.verts[0] / 3, g_ham.verts[1] / 3, 100.0 * g_ham.areaShare[0], 100.0 * g_ham.areaShare[1], g_eyeTan[0][0], g_eyeTan[0][1],
        g_eyeTan[0][2], g_eyeTan[0][3], g_eyeTan[1][0], g_eyeTan[1][1], g_eyeTan[1][2], g_eyeTan[1][3],
        synth > 0.0f ? std::format(" (SYNTHETIC test mask, legs {:.2f})", synth) : std::string());
}

bool HamUploadVb(ID3D11DeviceContext* a_ctx)
{
    if (!g_ham.vbDirty) {
        return g_ham.vb != nullptr;
    }
    const UINT bytes = static_cast<UINT>(g_ham.ndc.size() * sizeof(float));
    if (!bytes) {
        return false;
    }
    if (!g_ham.vb || g_ham.vbBytes < bytes) {
        if (g_ham.vb) {
            g_ham.vb->Release();
            g_ham.vb = nullptr;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_ham.vb))) {
            g_ham.vb = nullptr;
            return false;
        }
        g_ham.vbBytes = bytes;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(o_map(a_ctx, g_ham.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        return false;
    }
    std::memcpy(m.pData, g_ham.ndc.data(), bytes);
    o_unmap(a_ctx, g_ham.vb, 0);
    g_ham.vbDirty = false;
    return true;
}

// Render thread, from PrepareImpl right before a converted stereo z-prepass / GBuffer draw is issued.
void HamMaybeStamp(ID3D11DeviceContext* a_ctx)
{
    if (!g_hamOn.load(std::memory_order_relaxed) || g_secondCam.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::scoped_lock l(g_ham.lock);
        if (g_ham.serial == 0 && g_ham.synthetic <= 0.0f) {
            return;  // no mesh (runtime without the extension, or not fetched yet)
        }
    }
    ID3D11DepthStencilView* dsv = nullptr;
    a_ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) {
        ++g_ham.skipNoDsv;
        return;
    }
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
        res->Release();  // the engine keeps it alive; identity only
    }
    if (g_ham.stampedValid && res == g_ham.stampedRes) {
        dsv->Release();
        return;  // already stamped since this target's last clear
    }
    if (!g_tanValid || g_tanIdentity) {
        ++g_ham.skipNoTan;
        dsv->Release();
        return;
    }
    UINT           nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    a_ctx->RSGetViewports(&nvp, oVp);
    D3D11_TEXTURE2D_DESC td{};
    if (ID3D11Texture2D* t = nullptr; res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) {
        t->GetDesc(&td);
        t->Release();
    }
    // the double-wide main view only (viewport = the whole depth target, the planned render width when known)
    if (nvp == 0 || oVp[0].Width < 64.0f || std::abs(oVp[0].Width - static_cast<float>(td.Width)) > 0.5f ||
        std::abs(oVp[0].Height - static_cast<float>(td.Height)) > 0.5f || (g_seamFullW > 0.0 && std::abs(td.Width - g_seamFullW) > 0.5)) {
        ++g_ham.skipSize;
        dsv->Release();
        return;
    }
    bool rebuild = false;
    {
        std::scoped_lock l(g_ham.lock);
        rebuild = g_ham.builtSerial != g_ham.serial || g_ham.builtSynthetic != g_ham.synthetic;
    }
    for (int e = 0; e < 2 && !rebuild; ++e) {
        for (int k = 0; k < 4; ++k) {
            rebuild |= std::abs(g_ham.builtTan[e][k] - g_eyeTan[e][k]) > 1e-5f;
        }
    }
    if (rebuild) {
        HamBuild();
    }
    if (!HamInit() || !HamUploadVb(a_ctx)) {
        ++g_ham.failures;
        dsv->Release();
        return;
    }

    // save (hooked slots through the originals: the engine's shadow state never sees the stamp)
    ID3D11VertexShader* oVs = nullptr;
    ID3D11PixelShader*  oPs = nullptr;
    ID3D11GeometryShader* oGs = nullptr;
    ID3D11HullShader*   oHs = nullptr;
    ID3D11DomainShader* oDs = nullptr;
    a_ctx->VSGetShader(&oVs, nullptr, nullptr);
    a_ctx->PSGetShader(&oPs, nullptr, nullptr);
    a_ctx->GSGetShader(&oGs, nullptr, nullptr);
    a_ctx->HSGetShader(&oHs, nullptr, nullptr);
    a_ctx->DSGetShader(&oDs, nullptr, nullptr);
    ID3D11InputLayout* oIl = nullptr;
    a_ctx->IAGetInputLayout(&oIl);
    D3D11_PRIMITIVE_TOPOLOGY oTopo{};
    a_ctx->IAGetPrimitiveTopology(&oTopo);
    ID3D11Buffer* oVb = nullptr;
    UINT          oStride = 0, oOffset = 0;
    a_ctx->IAGetVertexBuffers(0, 1, &oVb, &oStride, &oOffset);
    ID3D11RasterizerState* oRs = nullptr;
    a_ctx->RSGetState(&oRs);
    ID3D11BlendState* oBs = nullptr;
    float             oBf[4]{};
    UINT              oMask = 0;
    a_ctx->OMGetBlendState(&oBs, oBf, &oMask);
    ID3D11DepthStencilState* oDss = nullptr;
    UINT                     oRef = 0;
    a_ctx->OMGetDepthStencilState(&oDss, &oRef);

    // stamp: both eyes in one draw, z = 0 (near) over the whole viewport
    const UINT stride = 8, offset = 0;
    o_iaSetIL(a_ctx, g_ham.il);
    a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_ctx->IASetVertexBuffers(0, 1, &g_ham.vb, &stride, &offset);
    o_vsSet(a_ctx, g_ham.vs, nullptr, 0);
    o_psSet(a_ctx, nullptr, nullptr, 0);
    a_ctx->GSSetShader(nullptr, nullptr, 0);
    a_ctx->HSSetShader(nullptr, nullptr, 0);
    a_ctx->DSSetShader(nullptr, nullptr, 0);
    a_ctx->RSSetState(g_ham.rs);
    D3D11_VIEWPORT vp = oVp[0];
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 0.0f;
    a_ctx->RSSetViewports(1, &vp);
    const float bf[4]{};
    a_ctx->OMSetBlendState(g_ham.bs, bf, 0xFFFFFFFF);
    o_omSetDSS(a_ctx, g_ham.dss, 0);
    o_draw(a_ctx, g_ham.verts[0] + g_ham.verts[1], 0);

    // restore
    o_iaSetIL(a_ctx, oIl);
    a_ctx->IASetPrimitiveTopology(oTopo);
    a_ctx->IASetVertexBuffers(0, 1, &oVb, &oStride, &oOffset);
    o_vsSet(a_ctx, oVs, nullptr, 0);
    o_psSet(a_ctx, oPs, nullptr, 0);
    a_ctx->GSSetShader(oGs, nullptr, 0);
    a_ctx->HSSetShader(oHs, nullptr, 0);
    a_ctx->DSSetShader(oDs, nullptr, 0);
    a_ctx->RSSetState(oRs);
    a_ctx->RSSetViewports(nvp, oVp);
    a_ctx->OMSetBlendState(oBs, oBf, oMask);
    o_omSetDSS(a_ctx, oDss, oRef);
    for (IUnknown* u : { static_cast<IUnknown*>(oVs), static_cast<IUnknown*>(oPs), static_cast<IUnknown*>(oGs), static_cast<IUnknown*>(oHs),
             static_cast<IUnknown*>(oDs), static_cast<IUnknown*>(oIl), static_cast<IUnknown*>(oVb), static_cast<IUnknown*>(oRs),
             static_cast<IUnknown*>(oBs), static_cast<IUnknown*>(oDss), static_cast<IUnknown*>(dsv) }) {
        if (u) {
            u->Release();
        }
    }
    g_ham.stampedRes = res;
    g_ham.stampedValid = true;
    g_ham.lastVp[0] = vp.TopLeftX, g_ham.lastVp[1] = vp.TopLeftY, g_ham.lastVp[2] = vp.Width, g_ham.lastVp[3] = vp.Height;
    if (g_ham.frameStamps++ == 0) {
        ++g_ham.framesStamped;
    }
    ++g_ham.totalStamps;
}

// ClearDepthStencilView on g_ctx: a depth clear of the stamped target removes the stamp (re-stamped at the next draw).
void HamOnClearDepth(ID3D11DepthStencilView* a_dsv, UINT a_flags)
{
    if (!a_dsv || !(a_flags & D3D11_CLEAR_DEPTH) || !g_ham.stampedValid) {
        return;
    }
    ID3D11Resource* res = nullptr;
    a_dsv->GetResource(&res);
    if (res) {
        res->Release();
    }
    if (res == g_ham.stampedRes) {
        g_ham.stampedValid = false;
        ++g_ham.clearsAfterStamp;
    }
}

float HamDecodeDepth(const std::uint8_t* a_px, DXGI_FORMAT a_fmt)
{
    switch (a_fmt) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        {
            std::uint32_t v = 0;
            std::memcpy(&v, a_px, 4);
            return static_cast<float>(v & 0xFFFFFFu) / 16777215.0f;
        }
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        {
            float f = 0;
            std::memcpy(&f, a_px, 4);
            return f;
        }
    default:
        return -1.0f;
    }
}

// Render thread, at the frame's first deferred light / composite draw after a stamp, when armed: full copy of the stamped
// depth (D3D11 copies depth-stencil resources whole only), one blocking Map, then per eye: the depth at the centroid of the
// mask's largest triangle (expect 0 = stamped), at the eye region's centre (expect > 0), and the share of the region at
// exactly 0 against the mask's area share.
void HamProbe(ID3D11DeviceContext* a_ctx)
{
    if (g_ham.probeState != 1 || !g_ham.stampedValid || !g_ham.stampedRes) {
        return;
    }
    g_ham.probeState = 0;
    ID3D11Texture2D* src = nullptr;
    if (FAILED(g_ham.stampedRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src)))) {
        g_ham.probeResult = "probe failed: depth is not a 2D texture";
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count != 1 || d.MipLevels != 1 || d.ArraySize != 1 || FAILED(g_device->CreateTexture2D(&sd, nullptr, &st))) {
        g_ham.probeResult = std::format("probe failed: staging copy of the depth ({}x{} fmt {} samples {} mips {} array {})", d.Width, d.Height,
            static_cast<int>(d.Format), d.SampleDesc.Count, d.MipLevels, d.ArraySize);
        src->Release();
        return;
    }
    a_ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(o_map(a_ctx, st, 0, D3D11_MAP_READ, 0, &m))) {
        g_ham.probeResult = "probe failed: Map";
        st->Release();
        src->Release();
        return;
    }
    const UINT bpp = (d.Format == DXGI_FORMAT_R32G8X24_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) ? 8u : 4u;
    auto at = [&](int x, int y) {
        x = (std::clamp)(x, 0, static_cast<int>(d.Width) - 1);
        y = (std::clamp)(y, 0, static_cast<int>(d.Height) - 1);
        return HamDecodeDepth(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch + static_cast<std::size_t>(x) * bpp,
            d.Format);
    };
    std::string out = std::format("depth {}x{} fmt {} |", d.Width, d.Height, static_cast<int>(d.Format));
    const UINT hw = d.Width / 2;
    for (int e = 0; e < 2; ++e) {
        const int px = static_cast<int>((g_ham.probeNdc[e][0] * 0.5f + 0.5f) * d.Width);
        const int py = static_cast<int>((0.5f - g_ham.probeNdc[e][1] * 0.5f) * d.Height);
        const int cx = static_cast<int>(e * hw + hw / 2), cy = static_cast<int>(d.Height / 2);
        std::uint64_t zero = 0, total = 0;
        for (UINT y = 0; y < d.Height; y += 2) {
            for (UINT x = e * hw; x < (e + 1) * hw; x += 2) {
                ++total;
                zero += at(static_cast<int>(x), static_cast<int>(y)) == 0.0f;
            }
        }
        out += std::format(" eye {}: masked px ({},{}) depth {:.6f}, centre px ({},{}) depth {:.6f}, region at 0: {:.2f} % (mask {:.2f} %) |", e, px,
            py, at(px, py), cx, cy, at(cx, cy), total ? 100.0 * zero / total : 0.0, 100.0 * g_ham.areaShare[e]);
    }
    o_unmap(a_ctx, st, 0);
    st->Release();
    src->Release();
    g_ham.probeResult = out;
    REX::INFO("stereo ham probe: {}", out);
}

std::string HamStatus()
{
    bool          ext = false;
    std::uint64_t serial = 0, fetches = 0;
    float         synth = 0.0f;
    std::size_t   tris[2]{};
    {
        std::scoped_lock l(g_ham.lock);
        ext = g_ham.ext;
        serial = g_ham.serial;
        fetches = g_ham.fetches;
        synth = g_ham.synthetic;
        tris[0] = g_ham.idx[0].size() / 3;
        tris[1] = g_ham.idx[1].size() / 3;
    }
    return std::format("hidden-area mask: on {} | extension {} | mesh serial {} fetches {} triangles L {} R {}{} | stamped L {} R {} tris, share "
                       "L {:.1f} % R {:.1f} % | stamps last frame {} total {} frames {} | clears after stamp {} | skips no-dsv {} size {} no-tan {} "
                       "fail {} | probe: {}",
        g_hamOn.load() ? 1 : 0, ext ? "present" : "absent", serial, fetches, tris[0], tris[1],
        synth > 0.0f ? std::format(" SYNTHETIC {:.2f}", synth) : std::string(), g_ham.verts[0] / 3, g_ham.verts[1] / 3,
        100.0 * g_ham.areaShare[0], 100.0 * g_ham.areaShare[1], g_ham.lastFrameStamps, g_ham.totalStamps, g_ham.framesStamped,
        g_ham.clearsAfterStamp, g_ham.skipNoDsv, g_ham.skipSize, g_ham.skipNoTan, g_ham.failures, g_ham.probeResult);
}
