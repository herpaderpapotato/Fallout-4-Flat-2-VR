// GpuProf.inl - included by StereoDraw.cpp inside its anonymous namespace (after CtxState / g_s, StereoShadow.inl).
//
// "gpuprof <frames>" (perf pass, 26 Nov): GPU time per render pass. While armed, a timestamp query closes a segment at
// every render-target change, at the first compute dispatch after draws (and back), and at the named marks of the
// present path (stereo::GpuProfMark). A segment's key = its colour target (size, format) or depth target, plus the
// pixel-shader groups drawn into it. Per key: mean and median GPU ms per frame over the recorded frames.
// "gpuprof" prints the table (also written to the log). Nothing runs while not armed (one atomic load per pass).
// Note (memory: GPU timing in VR): the VR compositor's own GPU work lands inside whichever segment is open at that
// moment; the per-key median leaves most of it out, the mean keeps it.

constexpr int kGpRing = 8;
constexpr int kGpMaxSeg = 768;
struct GpFrame
{
    ID3D11Query*              disjoint{ nullptr };
    ID3D11Query*              t0{ nullptr };
    std::vector<ID3D11Query*> q;
    std::vector<std::string>  keys;
    std::vector<std::uint32_t> draws;
    int                       n{ 0 };
    bool                      pending{ false }, open{ false };
};
struct GpCur
{
    std::string   target{ "(no target)" };
    std::uint16_t groups[4]{};
    int           ng{ 0 };
    std::uint32_t draws{ 0 }, dispatches{ 0 };
    bool          cs{ false };
};
struct GpAgg
{
    double             sum{ 0 }, cur{ 0 };
    std::vector<float> perFrame;
    std::uint64_t      draws{ 0 }, curDraws{ 0 };
    double             orderSum{ 0 };
    int                orderN{ 0 };
    bool               touched{ false };
};
GpFrame                                g_gp[kGpRing];
int                                    g_gpHead = 0;
std::atomic<int>                       g_gpArm{ 0 };       // frames still to record
std::atomic<bool>                      g_gpActive{ false };
GpCur                                  g_gpCur;
std::string                            g_gpPrefix;         // render thread
std::unordered_map<void*, std::string> g_gpTargetNames;    // render thread
std::mutex                             g_gpLock;           // g_gpAgg, g_gpFrames, g_gpTotal
std::map<std::string, GpAgg>           g_gpAgg;
std::vector<float>                     g_gpTotal;
int                                    g_gpFrames = 0, g_gpDropped = 0;

const char* GpGroupName(std::uint16_t a_g)
{
    switch (a_g) {
        case 3: return "sky";
        case 9: return "light";
        case 10: return "composite";
        case 12: return "tiled";
        case 125: return "saoRaw";
        case 136: return "motionblur";
        case 137: return "taa";
        case 144: return "ssrTrace";
        case 151: return "flare";
        case 155: return "flareVis";
        case 0xFFFF: return "ps?";
        default: return nullptr;
    }
}

std::string GpTargetName(ID3D11RenderTargetView* a_rtv, ID3D11DepthStencilView* a_dsv, UINT a_n)
{
    void* const k = a_rtv ? static_cast<void*>(a_rtv) : static_cast<void*>(a_dsv);
    if (!k) {
        return "(no target)";
    }
    if (const auto it = g_gpTargetNames.find(k); it != g_gpTargetNames.end()) {
        return it->second;
    }
    std::string     s = "?";
    ID3D11Resource* res = nullptr;
    if (a_rtv) {
        a_rtv->GetResource(&res);
    } else {
        a_dsv->GetResource(&res);
    }
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        UINT slice = 0;
        if (!a_rtv) {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            a_dsv->GetDesc(&vd);
            slice = vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? vd.Texture2DArray.FirstArraySlice : 0;
            s = std::format("depth-only {}x{} fmt {}{}", d.Width, d.Height, static_cast<int>(d.Format),
                d.ArraySize > 1 ? std::format(" slice {}/{}", slice, d.ArraySize) : std::string());
        } else {
            s = std::format("rt {}x{} fmt {}{}", d.Width, d.Height, static_cast<int>(d.Format), a_n > 1 ? std::format(" x{} targets", a_n) : std::string());
        }
        tex->Release();
    }
    if (res) {
        res->Release();
    }
    g_gpTargetNames.emplace(k, s);
    return s;
}

std::string GpKey()
{
    if (!g_gpPrefix.empty()) {
        return g_gpPrefix;
    }
    std::string s = g_gpCur.cs ? "compute after " + g_gpCur.target : g_gpCur.target;
    if (!g_gpCur.cs) {
        if (!g_gpCur.draws) {
            return s + " | no draws (clears / copies / state)";
        }
        s += " |";
        for (int i = 0; i < g_gpCur.ng; ++i) {
            const char* n = GpGroupName(g_gpCur.groups[i]);
            s += n ? std::format(" {}", n) : std::format(" g{}", g_gpCur.groups[i]);
        }
        if (g_gpCur.ng >= 4) {
            s += " ..";
        }
    }
    return s;
}

// closes the open segment (timestamp) under its key
void GpClose()
{
    GpFrame& f = g_gp[g_gpHead];
    if (!f.open || f.n >= kGpMaxSeg) {
        return;
    }
    if (f.n >= static_cast<int>(f.q.size())) {
        D3D11_QUERY_DESC t{ D3D11_QUERY_TIMESTAMP, 0 };
        ID3D11Query*     q = nullptr;
        if (FAILED(g_device->CreateQuery(&t, &q))) {
            return;
        }
        f.q.push_back(q);
        f.keys.emplace_back();
        f.draws.push_back(0);
    }
    g_ctx->End(f.q[f.n]);
    f.keys[f.n] = GpKey();
    f.draws[f.n] = g_gpCur.draws + g_gpCur.dispatches;
    ++f.n;
    g_gpCur.draws = g_gpCur.dispatches = 0;
    g_gpCur.ng = 0;
}

void GpCollect(bool a_flush)
{
    for (auto& f : g_gp) {
        if (!f.pending) {
            continue;
        }
        const UINT                          fl = a_flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64                              last = 0;
        if (g_ctx->GetData(f.disjoint, &dj, sizeof(dj), fl) != S_OK || (f.n && g_ctx->GetData(f.q[f.n - 1], &last, sizeof(last), fl) != S_OK)) {
            continue;
        }
        f.pending = false;
        UINT64 prev = 0;
        if (dj.Disjoint || !dj.Frequency || g_ctx->GetData(f.t0, &prev, sizeof(prev), 0) != S_OK) {
            ++g_gpDropped;
            continue;
        }
        const UINT64     first = prev;
        std::scoped_lock l(g_gpLock);
        bool             ok = true;
        for (int i = 0; i < f.n && ok; ++i) {
            UINT64 t = 0;
            if (g_ctx->GetData(f.q[i], &t, sizeof(t), 0) != S_OK) {
                ok = false;
                break;
            }
            GpAgg& a = g_gpAgg[f.keys[i]];
            a.cur += t > prev ? static_cast<double>(t - prev) * 1000.0 / static_cast<double>(dj.Frequency) : 0.0;
            a.curDraws += f.draws[i];
            if (!a.touched) {
                a.touched = true;
                a.orderSum += i;
                ++a.orderN;
            }
            prev = t;
        }
        for (auto& [k, a] : g_gpAgg) {
            if (ok) {
                a.sum += a.cur;
                a.draws += a.curDraws;
                a.perFrame.push_back(static_cast<float>(a.cur));
            }
            a.cur = 0;
            a.curDraws = 0;
            a.touched = false;
        }
        if (ok) {
            ++g_gpFrames;
            g_gpTotal.push_back(static_cast<float>(static_cast<double>(prev - first) * 1000.0 / static_cast<double>(dj.Frequency)));
        } else {
            ++g_gpDropped;
        }
    }
}

// Renderer::Begin (HkClearState): the previous frame's last segment ends here, the next recorded frame starts
void GpFrameMark()
{
    if (!g_gpActive.load(std::memory_order_relaxed) && g_gpArm.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    if (!g_device || !g_ctx) {
        return;
    }
    GpFrame& f = g_gp[g_gpHead];
    if (f.open) {
        g_gpPrefix = "frame tail: after Present until the next Renderer::Begin (game update; other GPU users)";
        GpClose();
        g_ctx->End(f.disjoint);
        f.open = false;
        f.pending = true;
        g_gpHead = (g_gpHead + 1) % kGpRing;
    }
    g_gpPrefix.clear();
    const bool last = g_gpArm.load(std::memory_order_relaxed) <= 0;
    GpCollect(last);
    if (last) {
        g_gpActive.store(false);
        return;
    }
    GpFrame& n = g_gp[g_gpHead];
    if (n.pending) {
        return;  // ring full: skip this frame
    }
    if (!n.disjoint) {
        D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC t{ D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(g_device->CreateQuery(&d, &n.disjoint)) || FAILED(g_device->CreateQuery(&t, &n.t0))) {
            return;
        }
    }
    --g_gpArm;
    g_gpActive.store(true);
    n.n = 0;
    n.open = true;
    g_gpCur = GpCur{};
    g_gpCur.target = "frame start (before the first target)";
    g_ctx->Begin(n.disjoint);
    g_ctx->End(n.t0);
}

inline void GpOnTarget(ID3D11RenderTargetView* a_rtv, ID3D11DepthStencilView* a_dsv, UINT a_n)
{
    if (!g_gpActive.load(std::memory_order_relaxed)) {
        return;
    }
    GpClose();
    g_gpCur.cs = false;
    g_gpCur.target = GpTargetName(a_rtv, a_dsv, a_n);
}

inline void GpOnDraw(std::uint16_t a_group)
{
    if (!g_gpActive.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_gpCur.cs) {
        GpClose();
        g_gpCur.cs = false;
    }
    ++g_gpCur.draws;
    for (int i = 0; i < g_gpCur.ng; ++i) {
        if (g_gpCur.groups[i] == a_group) {
            return;
        }
    }
    if (g_gpCur.ng < 4) {
        g_gpCur.groups[g_gpCur.ng++] = a_group;
    }
}

inline void GpOnDispatch()
{
    if (!g_gpActive.load(std::memory_order_relaxed)) {
        return;
    }
    if (!g_gpCur.cs) {
        GpClose();
        g_gpCur.cs = true;
    }
    ++g_gpCur.dispatches;
}

// named mark from the present path / DLSS: everything until the next mark (or "" = back to per-target keys) is one key
void GpMark(const char* a_name)
{
    if (!g_gpActive.load(std::memory_order_relaxed)) {
        return;
    }
    GpClose();
    g_gpPrefix = a_name ? a_name : "";
}

std::string GpCommand(std::string_view a_arg)
{
    if (!a_arg.empty()) {
        const int n = (std::max)(1, (std::min)(2000, std::atoi(std::string(a_arg).c_str())));
        {
            std::scoped_lock l(g_gpLock);
            g_gpAgg.clear();
            g_gpTotal.clear();
            g_gpFrames = g_gpDropped = 0;
        }
        g_gpArm.store(n);
        return std::format("gpuprof: recording {} frames; 'gpuprof' prints the table", n);
    }
    std::scoped_lock l(g_gpLock);
    if (!g_gpFrames) {
        return std::format("gpuprof: no frames recorded yet (armed {}, dropped {})", g_gpArm.load(), g_gpDropped);
    }
    struct Row
    {
        std::string key;
        double      mean, med, order, draws;
    };
    const auto median = [](std::vector<float> v) {
        if (v.empty()) {
            return 0.0;
        }
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return static_cast<double>(v[v.size() / 2]);
    };
    std::vector<Row> rows;
    double           meanSum = 0, medSum = 0;
    for (auto& [k, a] : g_gpAgg) {
        Row r{ k, a.sum / g_gpFrames, median(a.perFrame), a.orderN ? a.orderSum / a.orderN : 0.0, static_cast<double>(a.draws) / g_gpFrames };
        meanSum += r.mean;
        medSum += r.med;
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.order < b.order; });
    std::string out = std::format("gpuprof: {} frames ({} dropped), {} keys; frame GPU span mean {:.3f} ms median {:.3f} ms; sum of key medians {:.3f} ms\n"
                                  "  mean ms | median ms | draws+dispatches per frame | pass (in frame order)\n",
        g_gpFrames, g_gpDropped, rows.size(), meanSum, median(g_gpTotal), medSum);
    for (const auto& r : rows) {
        if (r.mean < 0.004 && r.med < 0.004) {
            continue;
        }
        out += std::format("  {:7.3f} | {:7.3f} | {:7.1f} | {}\n", r.mean, r.med, r.draws, r.key);
    }
    REX::INFO("{}", out);
    return out;
}
