// StereoPassCap.inl - included by StereoDraw.cpp inside its anonymous namespace (after Decode/Fmt).
//
// "stereopasscap <tag>": for one frame, every render pass's output is read back the moment the pass ends (its RT0
// is about to be unbound; for compute: UAV 0 right after the Dispatch) and the halves of that target are compared
// on the CPU (IPD-0 test: both eyes must be equal). One log line per pass ("stereopass:"), so the first pass where
// the halves diverge, and how (interior / seam band / outer band, alignment of the seam on that target's texel
// grid), is visible in one frame. Stalls the GPU for every pass of that frame (diagnostic only).
//
// Layout (seam padding, [Stereo] iSeamAlign): each eye region is Wp = fullW/2 px; the left eye's real pixels are
// [0, Wp - pad), the right eye's [Wp + pad, 2 Wp). On a target of width w the same map is scaled by w / fullW.

struct PassCapState
{
    int                     state{ 0 };  // 0 idle, 1 requested, 2 recording this frame (stem/state under g_capLock)
    std::string             tag;
    ID3D11RenderTargetView* rtv{ nullptr };  // RT0 of the open pass (AddRef'd)
    int                     draws{ 0 }, splits{ 0 }, stereo{ 0 }, gate0{ 0 }, post{ 0 };
    std::vector<int>        bmpGroups;  // write <dir>/<tag>_<idx>_g<grp>.bmp for passes whose PS group is listed (-2: all)
    std::string             bmpDir;
    int                     psGroup{ -1 }, vsGroup{ -1 };
    ID3D11PixelShader*      ps{ nullptr };
    int                     index{ 0 };
    std::vector<ID3D11Texture2D*> pool;
    int                     cs{ 0 };
};
PassCapState g_pc;
std::unordered_map<ID3D11PixelShader*, dxbc::Key>   g_psKeys;  // every PS of g_device (diagnostics), under g_regLock
std::unordered_map<ID3D11ComputeShader*, dxbc::Key> g_csKeys;  // every CS of g_device (diagnostics), under g_regLock

std::string KeyHex(const dxbc::Key& a_k, int a_bytes = 6)
{
    std::string s;
    for (int i = 0; i < a_bytes; ++i) {
        s += std::format("{:02x}", a_k.b[i]);
    }
    return s;
}

ID3D11Texture2D* PassStaging(UINT a_w, UINT a_h, DXGI_FORMAT a_fmt)
{
    for (auto* t : g_pc.pool) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        if (d.Width == a_w && d.Height == a_h && d.Format == a_fmt) {
            return t;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = a_w;
    d.Height = a_h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = a_fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &t))) {
        return nullptr;
    }
    g_pc.pool.push_back(t);
    return t;
}

void PassAnalyze(ID3D11DeviceContext* a_ctx, ID3D11Resource* a_res, UINT a_mip, UINT a_slice, const std::string& a_label, int a_group = -1)
{
    ID3D11Texture2D* tex = nullptr;
    if (!a_res || FAILED(a_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const int idx = g_pc.index++;
    const UINT w = (std::max)(1u, d.Width >> a_mip), h = (std::max)(1u, d.Height >> a_mip);
    FmtInfo    f = Fmt(d.Format);
    if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
        f = { 4, 5, 3 };
    }
    DXGI_FORMAT sf = d.Format;  // typeless targets: copy as is, decode by family
    if (d.SampleDesc.Count != 1 || !f.bpp) {
        REX::INFO("stereopass: {} #{:03} {} {}x{} fmt {} samples {} - not analysable", g_pc.tag, idx, a_label, w, h,
            static_cast<int>(d.Format), d.SampleDesc.Count);
        tex->Release();
        return;
    }
    ID3D11Texture2D* st = PassStaging(w, h, sf);
    if (!st) {
        tex->Release();
        return;
    }
    a_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, D3D11CalcSubresource(a_mip, a_slice, d.MipLevels), nullptr);
    tex->Release();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(a_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        return;
    }
    if (w * h <= 16 || w < 8) {
        float v[4]{};
        Decode(static_cast<const std::uint8_t*>(m.pData), f, d.Format, v);
        a_ctx->Unmap(st, 0);
        REX::INFO("stereopass: {} #{:03} {} {}x{} fmt {} | texel0 {:.5g} {:.5g} {:.5g} {:.5g}", g_pc.tag, idx, a_label, w, h,
            static_cast<int>(d.Format), v[0], v[1], v[2], v[3]);
        return;
    }
    // eye geometry on this target
    const double fullW = g_seamFullW > 0 ? g_seamFullW : static_cast<double>(w);
    const double sc = w / fullW;
    const double pad = g_seamPad;
    // left eye's real texels [x0, x0 + eW), right eye's the same + offR (one region width)
    const double rStart = fullW * 0.5 * sc;  // region width on this target (fractional = misaligned)
    const double eyeW = (fullW * 0.5 - pad) * sc;
    const long   offR = std::lround(rStart);
    const long   x0 = std::lround(std::floor(pad / 2) * sc);
    const long   eW = static_cast<long>(std::floor(eyeW + 1e-6));
    const long   band = (std::max)(1L, std::lround(64.0 * sc));
    const double frac = std::abs(rStart - std::round(rStart));
    std::uint64_t n = 0, same = 0, n1 = 0, n4 = 0, sb = 0, sb1 = 0, ob = 0, ob1 = 0;
    double        sumAbs = 0, sumL = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        for (long x = 0; x < eW && x0 + x + offR < static_cast<long>(w); ++x) {
            const std::uint8_t* a = row + static_cast<std::size_t>(x0 + x) * f.bpp;
            const std::uint8_t* b = row + static_cast<std::size_t>(x0 + x + offR) * f.bpp;
            float fa[4]{}, fb[4]{};
            Decode(a, f, d.Format, fa);
            Decode(b, f, d.Format, fb);
            bool  w1 = true, w4 = true;
            float dmax = 0;
            for (int c = 0; c < (std::min)(f.channels, 3); ++c) {
                const float da = std::fabs(fa[c] - fb[c]), mx = (std::max)(std::fabs(fa[c]), std::fabs(fb[c]));
                if (!(da == da)) {
                    continue;
                }
                const float tol = f.kind == 1 || f.kind == 2 || f.kind == 6 ? 1.0f / 255.0f : (std::max)(1.0f / 255.0f, 0.01f * mx);
                w1 = w1 && da <= tol * 1.001f;
                w4 = w4 && da <= 4.0f * tol * 1.001f;
                dmax = (std::max)(dmax, da);
            }
            if (x < band) {
                ++ob;
                ob1 += w1;
            } else if (x >= eW - band) {
                ++sb;
                sb1 += w1;
            } else {
                ++n;
                same += std::memcmp(a, b, f.bpp) == 0;
                n1 += w1;
                n4 += w4;
                sumAbs += dmax;
                sumL += fa[0];
            }
        }
    }
    if (!g_pc.bmpDir.empty() && std::find_if(g_pc.bmpGroups.begin(), g_pc.bmpGroups.end(), [&](int g) { return g == a_group || g == -2; }) !=
                                     g_pc.bmpGroups.end()) {
        WriteBmp(std::format("{}/{}_{:03}_g{:03}.bmp", g_pc.bmpDir, g_pc.tag, idx, a_group < 0 ? 999 : a_group), st, m);
    }
    a_ctx->Unmap(st, 0);
    const double N = n ? static_cast<double>(n) : 1.0;
    REX::INFO("stereopass: {} #{:03} {} {}x{} fmt {} | interior within1 {:.3f}% within4 {:.3f}% identical {:.2f}% meanAbs {:.3g} mean {:.4g} | "
              "seam band within1 {:.2f}% | outer band within1 {:.2f}% | right start {:.2f} ({})",
        g_pc.tag, idx, a_label, w, h, static_cast<int>(d.Format), 100.0 * n1 / N, 100.0 * n4 / N, 100.0 * same / N, sumAbs / N,
        sumL / N, sb ? 100.0 * sb1 / sb : 0.0, ob ? 100.0 * ob1 / ob : 0.0, rStart, frac < 1e-3 ? "aligned" : "MISALIGNED");
}

void PassFlush(ID3D11DeviceContext* a_ctx)
{
    if (!g_pc.rtv) {
        return;
    }
    ID3D11Resource* res = nullptr;
    g_pc.rtv->GetResource(&res);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    g_pc.rtv->GetDesc(&rd);
    UINT mip = 0, slice = 0;
    if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
        mip = rd.Texture2D.MipSlice;
    } else if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        mip = rd.Texture2DArray.MipSlice;
        slice = rd.Texture2DArray.FirstArraySlice;
    }
    std::string psk = "-";
    if (g_pc.ps) {
        std::shared_lock l(g_regLock);
        if (const auto it = g_psKeys.find(g_pc.ps); it != g_psKeys.end()) {
            psk = KeyHex(it->second);
        }
    }
    const std::string label = std::format("RT ps g{:03} ({}) vs g{:03} draws {} split {} stereo {} post {} gate0 {}", g_pc.psGroup < 0 ? 999 : g_pc.psGroup,
        psk, g_pc.vsGroup < 0 ? 999 : g_pc.vsGroup, g_pc.draws, g_pc.splits, g_pc.stereo, g_pc.post, g_pc.gate0);
    PassAnalyze(a_ctx, res, mip, slice, label, g_pc.psGroup);
    if (res) {
        res->Release();
    }
    g_pc.rtv->Release();
    g_pc.rtv = nullptr;
    g_pc.draws = g_pc.splits = g_pc.stereo = g_pc.gate0 = g_pc.post = 0;
    g_pc.psGroup = g_pc.vsGroup = -1;
    g_pc.ps = nullptr;
}

void PassCapOnRT(ID3D11DeviceContext* a_ctx, ID3D11RenderTargetView* a_next)
{
    if (g_pc.state == 2 && g_pc.rtv && a_next != g_pc.rtv) {
        PassFlush(a_ctx);
    }
}

void PassCapOnDraw(ID3D11DeviceContext* a_ctx, int a_action)
{
    if (g_pc.state != 2 || !g_s.rtv[0]) {
        return;
    }
    if (g_pc.rtv != g_s.rtv[0]) {
        PassFlush(a_ctx);
        g_pc.rtv = g_s.rtv[0];
        g_pc.rtv->AddRef();
    }
    ++g_pc.draws;
    g_pc.splits += a_action == 3;
    g_pc.stereo += a_action == 1;
    g_pc.post += a_action == 4;
    g_pc.gate0 += g_gateCam.load(std::memory_order_relaxed) == 0;
    g_pc.psGroup = g_s.psInfo ? g_s.psInfo->group : -1;
    g_pc.vsGroup = g_s.info ? g_s.info->group : -1;
    g_pc.ps = g_s.enginePS;
}

void PassCapOnDispatch(ID3D11DeviceContext* a_ctx, UINT a_x, UINT a_y, UINT a_z)
{
    if (g_pc.state != 2) {
        return;
    }
    ID3D11UnorderedAccessView* uav = nullptr;
    a_ctx->CSGetUnorderedAccessViews(0, 1, &uav);
    ID3D11ComputeShader* cs = nullptr;
    a_ctx->CSGetShader(&cs, nullptr, nullptr);
    std::string csk = "-";
    if (cs) {
        std::shared_lock l(g_regLock);
        if (const auto it = g_csKeys.find(cs); it != g_csKeys.end()) {
            csk = KeyHex(it->second);
        }
    }
    if (cs) {
        cs->Release();
    }
    ++g_pc.cs;
    if (!uav) {
        REX::INFO("stereopass: {} #{:03} CS ({}) groups {}x{}x{} - no UAV 0", g_pc.tag, g_pc.index++, csk, a_x, a_y, a_z);
        return;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    uav->GetDesc(&ud);
    ID3D11Resource* res = nullptr;
    uav->GetResource(&res);
    UINT mip = 0, slice = 0;
    if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) {
        mip = ud.Texture2D.MipSlice;
    } else if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
        mip = ud.Texture2DArray.MipSlice;
        slice = ud.Texture2DArray.FirstArraySlice;
    }
    if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D || ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
        PassAnalyze(a_ctx, res, mip, slice, std::format("CS ({}) groups {}x{}x{}", csk, a_x, a_y, a_z));
    } else {
        REX::INFO("stereopass: {} #{:03} CS ({}) groups {}x{}x{} - UAV dim {}", g_pc.tag, g_pc.index++, csk, a_x, a_y, a_z,
            static_cast<int>(ud.ViewDimension));
    }
    if (res) {
        res->Release();
    }
    uav->Release();
}

// OnPresent (under g_capLock)
void PassCapOnPresent(ID3D11DeviceContext* a_ctx)
{
    if (g_pc.state == 2) {
        PassFlush(a_ctx);
        REX::INFO("stereopass: {} done ({} passes, {} dispatches)", g_pc.tag, g_pc.index, g_pc.cs);
        g_pc.state = 0;
        g_pc.tag.clear();
        for (auto* t : g_pc.pool) {
            t->Release();
        }
        g_pc.pool.clear();
    } else if (g_pc.state == 1) {
        g_pc.state = 2;
        g_pc.index = 0;
        g_pc.cs = 0;
    }
}

void PassCapNoteShader(ID3D11PixelShader* a_ps, ID3D11ComputeShader* a_cs, const dxbc::Key& a_key)
{
    std::unique_lock l(g_regLock);
    if (a_ps) {
        g_psKeys[a_ps] = a_key;
    }
    if (a_cs) {
        g_csKeys[a_cs] = a_key;
    }
}
