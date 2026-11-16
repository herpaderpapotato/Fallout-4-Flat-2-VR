// StereoShadow.inl - included by StereoDraw.cpp inside its anonymous namespace (after CtxState / g_s).
//
// "stereoshadow <n>": for the next n frames, one log line per frame ("stereoshadow:") with the signature of every
// depth-only pass (no colour target bound, a depth view bound): target size / array slice, viewport, draw count,
// primitive-index total and an order-independent hash of (VS, index count, instances) over its draws. Directional
// shadow cascades are such passes, so a change of the shadow-caster set (culling) or of the cascade layout between
// frames, or between stereo and mono frames at the same pose, shows here without reading GPU memory.
// Costs one RSGetViewports per depth-only draw while active; nothing otherwise.

struct ShadowSigKey
{
    ID3D11DepthStencilView* dsv;
    int                     vx, vy, vw, vh;
    bool operator==(const ShadowSigKey&) const = default;
};
struct ShadowSigKeyHash
{
    std::size_t operator()(const ShadowSigKey& k) const noexcept
    {
        return std::hash<void*>()(k.dsv) ^ (static_cast<std::size_t>(k.vx) << 3) ^ (static_cast<std::size_t>(k.vy) << 17) ^
               (static_cast<std::size_t>(k.vw) << 29) ^ (static_cast<std::size_t>(k.vh) << 41);
    }
};
struct ShadowSigVal
{
    int           order{ 0 };  // first draw's position in the frame
    std::uint32_t draws{ 0 };
    std::uint64_t prims{ 0 };
    std::uint64_t hash{ 0 };
};
struct ShadowDsvInfo
{
    UINT w{ 0 }, h{ 0 }, slice{ 0 }, arr{ 0 };
};
std::atomic<int>                                                     g_shadowFrames{ 0 };  // frames left to log
std::atomic<int>                                                     g_shadowDump{ 0 };    // 1 requested, 2 dumping this frame
std::vector<std::string>                                             g_shadowDumpLines;
std::unordered_map<ShadowSigKey, ShadowSigVal, ShadowSigKeyHash>     g_shadowSig;
std::unordered_map<ID3D11DepthStencilView*, ShadowDsvInfo>           g_shadowDsv;
int                                                                  g_shadowOrder{ 0 };
ID3D11DepthStencilView*                                              g_curDsv{ nullptr };  // bound DSV of g_ctx (not referenced)

const ShadowDsvInfo& ShadowDsv(ID3D11DepthStencilView* a_dsv)
{
    auto [it, fresh] = g_shadowDsv.try_emplace(a_dsv);
    if (fresh) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};
        a_dsv->GetDesc(&d);
        if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
            it->second.slice = d.Texture2DArray.FirstArraySlice;
            it->second.arr = d.Texture2DArray.ArraySize;
        }
        ID3D11Resource* r = nullptr;
        a_dsv->GetResource(&r);
        if (r) {
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
                D3D11_TEXTURE2D_DESC td{};
                t->GetDesc(&td);
                it->second.w = td.Width;
                it->second.h = td.Height;
                t->Release();
            }
            r->Release();
        }
    }
    return it->second;
}

void ShadowSigOnDraw(ID3D11DeviceContext* a_ctx, UINT a_instances, UINT a_count)
{
    if (g_shadowFrames.load(std::memory_order_relaxed) <= 0 || !g_curDsv) {
        return;
    }
    for (UINT i = 0; i < g_s.numRTV && i < 8; ++i) {
        if (g_s.rtv[i]) {
            return;  // colour pass
        }
    }
    UINT          n = 1;
    D3D11_VIEWPORT vp{};
    a_ctx->RSGetViewports(&n, &vp);
    const ShadowSigKey k{ g_curDsv, static_cast<int>(vp.TopLeftX), static_cast<int>(vp.TopLeftY), static_cast<int>(vp.Width),
        static_cast<int>(vp.Height) };
    if (g_shadowDump.load(std::memory_order_relaxed) == 2 && g_shadowDumpLines.size() < 4000) {
        const auto& di = ShadowDsv(g_curDsv);
        g_shadowDumpLines.push_back(std::format("{}x{}s{} vp {},{},{}x{} vs {} g{} n {} i {}", di.w, di.h, di.slice, k.vx, k.vy, k.vw, k.vh,
            static_cast<void*>(g_s.engineVS), g_s.info ? static_cast<int>(g_s.info->group) : -1, a_count, a_instances));
    }
    auto [it, fresh] = g_shadowSig.try_emplace(k);
    if (fresh) {
        it->second.order = g_shadowOrder++;
    }
    ++it->second.draws;
    it->second.prims += static_cast<std::uint64_t>(a_count) * a_instances;
    // order-independent: sum of a mixed per-draw value
    std::uint64_t x = reinterpret_cast<std::uintptr_t>(g_s.engineVS) ^ (static_cast<std::uint64_t>(a_count) << 20) ^
                      (static_cast<std::uint64_t>(a_instances) << 52);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    it->second.hash += x;
}

void ShadowSigOnPresent(bool a_stereoFrame)
{
    if (const int ds = g_shadowDump.load(std::memory_order_relaxed); ds == 2) {
        for (std::size_t i = 0; i < g_shadowDumpLines.size(); ++i) {
            REX::INFO("stereoshadowdump: st {} #{} {}", a_stereoFrame ? 1 : 0, i, g_shadowDumpLines[i]);
        }
        REX::INFO("stereoshadowdump: done {} draws", g_shadowDumpLines.size());
        g_shadowDumpLines.clear();
        g_shadowDump.store(0);
    } else if (ds == 1) {
        g_shadowDumpLines.clear();
        g_shadowDump.store(2);
        if (g_shadowFrames.load(std::memory_order_relaxed) <= 0) {
            g_shadowFrames.store(1);  // the draw hook records only while signatures are on
        }
    }
    const int left = g_shadowFrames.load(std::memory_order_relaxed);
    if (left <= 0) {
        return;
    }
    std::vector<std::pair<ShadowSigKey, ShadowSigVal>> v(g_shadowSig.begin(), g_shadowSig.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.order < b.second.order; });
    std::string s;
    for (const auto& [k, val] : v) {
        const auto& di = ShadowDsv(k.dsv);
        s += std::format(" | {}x{}s{} vp {},{},{}x{} d {} p {} h {:016x}", di.w, di.h, di.slice, k.vx, k.vy, k.vw, k.vh, val.draws, val.prims,
            val.hash);
    }
    REX::INFO("stereoshadow: f {} st {} n {}{}", g_stats.frames, a_stereoFrame ? 1 : 0, v.size(), s);
    g_shadowSig.clear();
    g_shadowOrder = 0;
    g_shadowFrames.store(left - 1, std::memory_order_relaxed);
}
