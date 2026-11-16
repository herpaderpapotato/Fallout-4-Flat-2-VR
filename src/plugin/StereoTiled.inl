// StereoTiled.inl - included by StereoDraw.cpp inside its anonymous namespace (after StereoPost.inl).
//
// Per-eye tiled deferred lighting (fxp g012 BSDFTiledLighting; stereo-lighting.md item 7e). The engine runs three
// compute dispatches over the double-wide target, one group per 8x8-pixel tile:
//  1. tile depth bounds: eye view z per tile (depth was written with the eye's projection and the mono near/far, so the
//     mono inverse-projection z rows give the eye's z). With eye regions a multiple of 8 px no tile straddles the seam:
//     runs unchanged.
//  2. light culling: tile frusta from mono tangents (cb0[1].x/.w + tile * cb0[3].z/.w), lights in head view space.
//  3. shading: view position from the mono inverse projection (cb12 rows 20-27) of the absolute pixel.
// Culling and shading are dispatched once per eye region (half the groups each) with their StereoCS.pack twin
// (proto/stereo/tools/gen_stereo_cs.py) and a per-eye cb13: group offset, mono -> eye tile tangent map, head -> eye view
// transform (culling: light position into the eye's view space, tile planes through the eye's origin). Shading binds
// the eye's b12 copy (rows 20-27 map the eye's double-wide NDC to head view space, where the lights are). Tile
// indices, the per-tile light lists and the output pixels stay absolute, so the composite reads them unchanged.
// If per-eye tiled lighting cannot run (no twins, eye region not a multiple of 8 px, tangent convention mismatch,
// "stereotiled 0"), TiledPerEyeAvailable() is false and StereoCuts keeps the tiled-lighting cut (volume path).

constexpr std::uint16_t kGroupTiled = 12;
constexpr std::uint8_t  kTiledCull = 1, kTiledShade = 2;
struct TiledCs
{
    std::uint8_t         kind{ 0 };
    ID3D11ComputeShader* twin{ nullptr };
};
struct CsPackEntry
{
    std::uint8_t              kind{ 0 };
    std::uint32_t             tech{ 0 };
    std::vector<std::uint8_t> blob;
};
std::unordered_map<dxbc::Key, CsPackEntry, KeyHash>  g_csPack;    // StereoCS.pack
std::unordered_map<ID3D11ComputeShader*, TiledCs>    g_tiledCs;   // engine CS -> twin (under g_regLock)
std::atomic<int>  g_tiledTwins{ 0 }, g_tiledTwinFails{ 0 };
std::atomic<bool> g_tiledEye{ true };   // pipe "stereotiled 0|1" (A/B: 0 = keep the cut, volume path)
std::atomic<bool> g_tiledMonoTest{ false };  // pipe "stereotiled mono": mono tiled dispatches in stereo (A/B, wrong)
std::atomic<int>  g_tiledConv{ -1 };    // engine tile-tangent convention: -1 not yet checked, 1 matches, 0 mismatch
ID3D11Buffer*     g_tiledCb[2]{};
std::uint64_t     g_tiledSplit[2]{}, g_tiledMono = 0, g_tiledFail = 0;
int               g_tiledLogs = 0, g_tiledConvLogs = 0;

void LoadCsPack()
{
    const std::string path = g_cfg.shaderDir + "\\StereoCS.pack";
    std::ifstream     f(path, std::ios::binary);
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "FCSP", 4) != 0) {
        REX::WARN("stereo: {} missing or invalid - tiled lighting stays cut in stereo (volume path)", path);
        return;
    }
    std::uint32_t ver = 0, n = 0;
    std::memcpy(&ver, d.data() + 4, 4);
    std::memcpy(&n, d.data() + 8, 4);
    if (ver != 1) {
        REX::ERROR("stereo: {} version {}", path, ver);
        return;
    }
    std::size_t q = 12;
    for (std::uint32_t i = 0; i < n && q + 32 <= d.size(); ++i) {
        dxbc::Key k;
        std::memcpy(k.b, d.data() + q, 16);
        CsPackEntry e;
        e.kind = d[q + 18];
        std::memcpy(&e.tech, d.data() + q + 20, 4);
        std::uint32_t sz = 0;
        std::memcpy(&sz, d.data() + q + 28, 4);
        q += 32;
        if (q + sz > d.size()) {
            break;
        }
        e.blob.assign(d.begin() + static_cast<std::ptrdiff_t>(q), d.begin() + static_cast<std::ptrdiff_t>(q + sz));
        q += (sz + 3) & ~std::size_t{ 3 };
        g_csPack.emplace(k, std::move(e));
    }
    REX::INFO("stereo: loaded {} tiled-lighting CS twins from {}", g_csPack.size(), path);
}

// HkCreateCS: an engine g012 culling/shading CS -> create its twin
void TiledNoteCS(ID3D11Device* a_dev, ID3D11ComputeShader* a_cs, const dxbc::Key& a_key)
{
    const auto it = g_csPack.find(a_key);
    if (it == g_csPack.end()) {
        return;
    }
    TiledCs t{ it->second.kind, nullptr };
    if (FAILED(o_createCS(a_dev, it->second.blob.data(), it->second.blob.size(), nullptr, &t.twin))) {
        t.twin = nullptr;
    }
    if (t.twin) {
        ++g_tiledTwins;
    } else if (++g_tiledTwinFails <= 4) {
        REX::WARN("stereo: tiled CS twin (tech {:x}, kind {}) rejected by CreateComputeShader", it->second.tech, t.kind);
    }
    std::unique_lock l(g_regLock);
    g_tiledCs[a_cs] = t;
}

// eye region width a multiple of the 8-px tile (no tile straddles the seam)
bool TiledAligned()
{
    const double half = g_seamFullW * 0.5;
    return half >= 8.0 && std::fmod(half, 8.0) == 0.0;
}

bool TiledAvailable()
{
    return g_tiledEye.load(std::memory_order_relaxed) && g_tiledTwins.load(std::memory_order_relaxed) > 0 && TiledAligned() &&
           g_tiledConv.load(std::memory_order_relaxed) != 0 && g_splitOn.load(std::memory_order_relaxed);
}

// The engine's mono tile tangents (tracked cb0 content) against the mono frustum: tan_x(tile 0) = l, tan_x(tilesX) = r,
// tan_y(row 0 from the bottom) = b, rising towards t. The per-eye map assumes this convention.
void TiledCheckConvention(ID3D11DeviceContext* a_ctx, UINT a_x, UINT a_y)
{
    ID3D11Buffer* b = nullptr;
    a_ctx->CSGetConstantBuffers(0, 1, &b);
    Tracked* t = TrackPoolBuffer(b);
    if (!t && b && g_tiledConvLogs < 1) {
        ++g_tiledConvLogs;
        D3D11_BUFFER_DESC bd{};
        b->GetDesc(&bd);
        REX::INFO("stereo tiled: compute cb0 {} ({} bytes, usage {}) - tangent convention checked from its next Map", static_cast<void*>(b),
            bd.ByteWidth, static_cast<int>(bd.Usage));
    }
    if (b) {
        b->Release();
    }
    if (!t || t->writes == 0 || t->size < 64) {
        return;  // content known from the next write on
    }
    const float* f = reinterpret_cast<const float*>(t->data);
    const double sx = f[4], sy = f[7], dx = f[14], dy = f[15], tx = f[10], ty = f[11];
    const double l = g_monoTan[0], r = g_monoTan[1], tt = g_monoTan[2], bb = g_monoTan[3];
    const double tolx = 0.02 * std::abs(r - l), toly = std::abs(dy) * 1.01 + 0.002 * std::abs(tt - bb);
    const bool okx = std::abs(sx - l) <= tolx && std::abs(sx + dx * a_x - r) <= tolx;
    const bool oky = dy > 0 && std::abs(sy - bb) <= toly && std::abs(sy + dy * a_y - tt) <= toly + std::abs(dy);
    const bool okn = tx == static_cast<float>(a_x) && ty == static_cast<float>(a_y);
    const int  v = okx && oky && okn ? 1 : 0;
    if (g_tiledConvLogs < 3 || v != g_tiledConv.load()) {
        ++g_tiledConvLogs;
        REX::INFO("stereo tiled: engine tile tangents x {:.5f} + {:.6f} * i, y {:.5f} + {:.6f} * j, tiles {}x{} (cb0) / dispatch {}x{} | "
                  "mono frustum l {:.5f} r {:.5f} t {:.5f} b {:.5f} -> x {} y {} tiles {} => {}",
            sx, dx, sy, dy, tx, ty, a_x, a_y, l, r, tt, bb, okx ? "ok" : "MISMATCH", oky ? "ok" : "MISMATCH", okn ? "ok" : "MISMATCH",
            v ? "per-eye tiled lighting OK" : "MISMATCH: per-eye tiled lighting disabled (cut -> volume path)");
    }
    g_tiledConv.store(v);
}

bool TiledEyeCbs(ID3D11DeviceContext* a_ctx, UINT a_half)
{
    if (!g_tiledCb[0]) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (auto*& b : g_tiledCb) {
            if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) {
                b = nullptr;
                return false;
            }
        }
    }
    const double l = g_monoTan[0], r = g_monoTan[1], t = g_monoTan[2], b = g_monoTan[3];
    if (!(r - l > 1e-6) || !(t - b > 1e-6)) {
        return false;
    }
    for (int e = 0; e < 2; ++e) {
        const math::EyeRel& eye = g_eyeRel[e];
        const double        le = eye.tanL, re = eye.tanR, te = eye.tanT, be = eye.tanB;
        alignas(16) float   k[20]{};
        const std::int32_t  off = static_cast<std::int32_t>(e * a_half);
        std::memcpy(&k[0], &off, 4);
        // same pixel column: mono tangent -> pixel fraction over the double-wide image -> eye tangent over its region
        const double ax = 2.0 * (re - le) / (r - l);
        const double ay = (te - be) / (t - b);
        k[4] = static_cast<float>(ax);
        k[5] = static_cast<float>(le - e * (re - le) - l * ax);
        k[6] = static_cast<float>(ay);
        k[7] = static_cast<float>(be - b * ay);
        // p_eye = (p_head - o) * R  (rows: dp4 with (p, 1))
        for (int j = 0; j < 3; ++j) {
            double w = 0.0;
            for (int i = 0; i < 3; ++i) {
                k[8 + 4 * j + i] = static_cast<float>(eye.rot[i][j]);
                w -= eye.pos[i] * eye.rot[i][j];
            }
            k[8 + 4 * j + 3] = static_cast<float>(w);
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(a_ctx->Map(g_tiledCb[e], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            return false;
        }
        std::memcpy(m.pData, k, sizeof(k));
        a_ctx->Unmap(g_tiledCb[e], 0);
    }
    return true;
}

// HkDispatch: true = handled (the engine's dispatch was replaced by the two eye dispatches)
bool TiledDispatch(ID3D11DeviceContext* a_ctx, UINT a_x, UINT a_y, UINT a_z)
{
    if (g_tiledTwins.load(std::memory_order_relaxed) == 0) {
        return false;  // no twins (StereoCS.pack missing): TiledAvailable() is false, the cut keeps tiled lighting off
    }
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != static_cast<int>(Mode::kStereoDebug) && mode != static_cast<int>(Mode::kStereoFaithful)) {
        return false;
    }
    ID3D11ComputeShader* cs = nullptr;
    a_ctx->CSGetShader(&cs, nullptr, nullptr);
    if (!cs) {
        return false;
    }
    TiledCs info;
    bool    found = false;
    {
        std::shared_lock l(g_regLock);
        if (const auto it = g_tiledCs.find(cs); it != g_tiledCs.end()) {
            info = it->second;
            found = true;
        }
    }
    if (!found) {
        cs->Release();
        return false;
    }
    const char* why = nullptr;
    if (g_tiledMonoTest.load(std::memory_order_relaxed)) {
        why = "mono test";
    } else if (!g_frameStereoCam.load(std::memory_order_relaxed) || !g_tanValid || g_tanIdentity) {
        why = "no stereo world camera this frame";
    } else if (!info.twin) {
        why = "no twin";
    } else if (!TiledAvailable()) {
        why = "per-eye tiled lighting unavailable";
    } else if (!g_b12EyeValid || g_b12In.cam != 1 || !g_b12Eye[0] || !g_b12Eye[1]) {
        why = "no per-eye b12 of the world camera";
    } else if (a_z != 1 || (a_x & 1) || std::abs(a_x * 8.0 - g_seamFullW) > 0.5) {
        why = "dispatch width does not match the double-wide render width";
    }
    if (!why && info.kind == kTiledCull) {
        TiledCheckConvention(a_ctx, a_x, a_y);
        if (g_tiledConv.load() == 0) {
            why = "tile tangent convention mismatch";
        }
    }
    if (!why && !TiledEyeCbs(a_ctx, a_x / 2)) {
        why = "per-eye cb13";
    }
    if (why) {
        cs->Release();
        if (std::string_view(why) == "mono test") {
            ++g_tiledMono;
        } else {
            ++g_tiledFail;
            if (g_tiledLogs < 8) {
                ++g_tiledLogs;
                REX::WARN("stereo tiled: kind {} dispatch {}x{}x{} runs mono: {} (b12 eye valid {} cam {}, render width {})", info.kind, a_x,
                    a_y, a_z, why, g_b12EyeValid, g_b12In.cam, g_seamFullW);
            }
        }
        return false;
    }
    ID3D11Buffer* old[2]{};
    a_ctx->CSGetConstantBuffers(12, 2, old);
    a_ctx->CSSetShader(info.twin, nullptr, 0);
    const UINT half = a_x / 2;
    for (int e = 0; e < 2; ++e) {
        ID3D11Buffer* const cb[2] = { info.kind == kTiledShade ? g_b12Eye[e] : old[0], g_tiledCb[e] };
        a_ctx->CSSetConstantBuffers(12, 2, cb);
        o_dispatch(a_ctx, half, a_y, a_z);
        ++g_tiledSplit[info.kind == kTiledShade ? 1 : 0];
    }
    a_ctx->CSSetShader(cs, nullptr, 0);
    a_ctx->CSSetConstantBuffers(12, 2, old);
    for (auto* b : old) {
        if (b) {
            b->Release();
        }
    }
    cs->Release();
    return true;
}
