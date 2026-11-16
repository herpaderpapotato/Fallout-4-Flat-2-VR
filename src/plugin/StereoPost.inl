// StereoPost.inl - included by StereoDraw.cpp inside its anonymous namespace (after RunSplit).
//
// Image-space passes that were on the stereo cut list, made per eye (stereo-lighting.md session 3, item 7). Each pass
// reads a screen position or a centre point from its own cb2 (a pool buffer: content tracked through the Map hook,
// g_trPool); the pass is drawn once per eye with a per-eye cb2 copy, restricted to that eye's region.
//  - motion blur g136 (PS cb2[1] = (max u, max v, centre u, centre v)): the radial weight is measured from the eye's
//    own optical centre, the tap clamp keeps the left eye's taps in its region; scissor per eye. The motion vectors it
//    reads are the per-eye ones (the MV pass runs before the first MV consumer, now including g136).
//  - lens flare visibility g155 (PS cb2[i] = (light u, light v, depth ref, -)): the light's mono screen position is
//    moved to where that direction lies in the left eye (or the right eye if it is outside the left), so the occlusion
//    taps read that eye's depth.
//  - lens flare sprites g151 (VS cb2[1] = sprite basis, cb2[2] = (light u, light v, -, axis t)): drawn once per eye
//    with the viewport on the eye region, the light at its eye-local position and the basis x doubled (the viewport is
//    half as wide), so sprites keep their pixel size and line up through the eye's own centre.
// Directions only (sun, distant lights): the eye translation is ignored for the flare position.

constexpr std::uint16_t kGroupMotionBlur = 136, kGroupFlare = 151, kGroupFlareVis = 155;
std::atomic<bool>       g_postEye{ true };  // pipe "stereopost 0|1" (A/B)
int                     g_postGroup = -1;   // group of the draw PrepareImpl routed to RunPost
std::uint64_t           g_postDraws[3]{}, g_postFail = 0;

Tracked* TrackPoolBuffer(ID3D11Buffer* a_b)
{
    if (!a_b) {
        return nullptr;
    }
    if (Tracked* t = TrackedFor(a_b)) {
        return t;
    }
    D3D11_BUFFER_DESC bd{};
    a_b->GetDesc(&bd);
    if (g_trPoolN >= kTrPool || bd.ByteWidth > sizeof(Tracked::data) || !(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ||
        bd.Usage != D3D11_USAGE_DYNAMIC) {
        return nullptr;
    }
    Tracked& t = g_trPool[g_trPoolN++];
    t.res = a_b;
    t.size = bd.ByteWidth;
    REX::INFO("stereo post: tracking cb2 pool buffer {} ({} bytes) from the next Map", static_cast<void*>(a_b), bd.ByteWidth);
    return nullptr;  // content known from the next write on
}

// mono double-wide UV -> double-wide UV of the same view direction in eye e's region (guard band included)
bool MonoUvToEye(double a_u, double a_v, int a_e, double& a_ue, double& a_ve)
{
    const double xv = g_monoTan[0] + a_u * (g_monoTan[1] - g_monoTan[0]);
    const double yv = g_monoTan[2] - a_v * (g_monoTan[2] - g_monoTan[3]);
    const auto&  R = g_eyeRel[a_e].rot;  // p_eye = p_head * R (row vector; columns = eye axes)
    double       d[3]{};
    for (int j = 0; j < 3; ++j) {
        d[j] = xv * R[0][j] + yv * R[1][j] + 1.0 * R[2][j];
    }
    if (d[2] <= 1e-6) {
        return false;
    }
    const double tx = d[0] / d[2], ty = d[1] / d[2];
    const double le = g_eyeTan[a_e][0], re = g_eyeTan[a_e][1], te = g_eyeTan[a_e][2], be = g_eyeTan[a_e][3];
    const double ul = (tx - le) / (re - le), vl = (te - ty) / (te - be);
    a_ue = (a_e + ul) * 0.5;
    a_ve = vl;
    return true;
}

struct EyeCbPair
{
    ID3D11Buffer* b[2]{};
    UINT          size{ 0 };
};
std::unordered_map<int, EyeCbPair> g_postCb;  // key: group

// per-eye copies of the tracked buffer `a_src`, rows patched by a_patch(float* rows, eye); false = not ready
template <class F>
bool MakeEyeCbs(ID3D11DeviceContext* a_ctx, int a_key, const Tracked& a_src, int a_eyes, F&& a_patch)
{
    EyeCbPair& p = g_postCb[a_key];
    if (!p.b[0] || p.size != a_src.size) {
        for (auto*& b : p.b) {
            if (b) {
                b->Release();
                b = nullptr;
            }
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = a_src.size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (auto*& b : p.b) {
            if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) {
                b = nullptr;
                return false;
            }
        }
        p.size = a_src.size;
    }
    for (int e = 0; e < a_eyes; ++e) {
        alignas(16) std::uint8_t tmp[sizeof(Tracked::data)];
        std::memcpy(tmp, a_src.data, a_src.size);
        a_patch(reinterpret_cast<float*>(tmp), e);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(a_ctx->Map(p.b[e], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            return false;
        }
        std::memcpy(m.pData, tmp, a_src.size);
        a_ctx->Unmap(p.b[e], 0);
    }
    return true;
}

// the tracked content of cb2 bound to the stage (0 VS, 1 PS), or nullptr (registers it for the next frames)
const Tracked* BoundCb2(ID3D11DeviceContext* a_ctx, int a_stage)
{
    ID3D11Buffer* b = nullptr;
    if (a_stage == 0) {
        a_ctx->VSGetConstantBuffers(2, 1, &b);
    } else {
        a_ctx->PSGetConstantBuffers(2, 1, &b);
    }
    Tracked* t = TrackPoolBuffer(b);
    if (b) {
        b->Release();
    }
    return (t && t->writes > 0) ? t : nullptr;
}

double EyeCentreU(int a_e)
{
    const double le = g_eyeTan[a_e][0], re = g_eyeTan[a_e][1];
    return (a_e + (0.0 - le) / (re - le)) * 0.5;
}

double EyeCentreV(int a_e)
{
    const double te = g_eyeTan[a_e][2], be = g_eyeTan[a_e][3];
    return te / (te - be);
}

template <class F>
void RunPost(ID3D11DeviceContext* a_ctx, F&& a_draw)
{
    const int grp = g_postGroup;
    g_postGroup = -1;
    if (grp == kGroupFlareVis) {
        const Tracked* t = BoundCb2(a_ctx, 1);
        if (!t || !MakeEyeCbs(a_ctx, grp, *t, 1, [](float* f, int) {
                for (int i = 0; i < 128 && (i + 1) * 16 <= static_cast<int>(sizeof(Tracked::data)); ++i) {
                    float* r = f + 4 * i;
                    if (!(r[0] == r[0]) || std::fabs(r[0]) > 4.0f || std::fabs(r[1]) > 4.0f) {
                        continue;
                    }
                    double u = 0, v = 0;
                    if (MonoUvToEye(r[0], r[1], 0, u, v) && u >= 0.0 && u < 0.5) {
                        r[0] = static_cast<float>(u), r[1] = static_cast<float>(v);
                    } else if (MonoUvToEye(r[0], r[1], 1, u, v)) {
                        r[0] = static_cast<float>(u), r[1] = static_cast<float>(v);
                    }
                }
            })) {
            ++g_postFail;
            a_draw();
            return;
        }
        ID3D11Buffer* old = nullptr;
        a_ctx->PSGetConstantBuffers(2, 1, &old);
        a_ctx->PSSetConstantBuffers(2, 1, &g_postCb[grp].b[0]);
        a_draw();
        a_ctx->PSSetConstantBuffers(2, 1, &old);
        if (old) {
            old->Release();
        }
        ++g_postDraws[2];
        return;
    }
    UINT           nvp = 1;
    D3D11_VIEWPORT vp{};
    a_ctx->RSGetViewports(&nvp, &vp);
    if (nvp == 0 || vp.Width < 2.0f) {
        ++g_postFail;
        a_draw();
        return;
    }
    const double fullW = vp.Width;
    const bool     flare = grp == kGroupFlare;
    const Tracked* t = BoundCb2(a_ctx, flare ? 0 : 1);
    const bool     ok = t && MakeEyeCbs(a_ctx, grp, *t, 2, [&](float* f, int e) {
        if (flare) {
            double u = 0, v = 0;
            if (MonoUvToEye(f[8], f[9], e, u, v)) {
                f[8] = static_cast<float>(u * 2.0 - e);  // eye-local UV (the viewport is the eye region)
                f[9] = static_cast<float>(v);
            } else {
                f[8] = f[9] = -100.0f;  // behind the eye: off screen
            }
            f[4] *= 2.0f;
            f[6] *= 2.0f;
        } else {
            f[6] = static_cast<float>(EyeCentreU(e));
            f[7] = static_cast<float>(EyeCentreV(e));
            if (e == 0) {
                f[4] = (std::min)(f[4], static_cast<float>(0.5 - 0.5 / fullW));
            }
        }
    });
    if (!ok) {
        ++g_postFail;
        a_draw();
        return;
    }
    ID3D11Buffer* old = nullptr;
    if (flare) {
        a_ctx->VSGetConstantBuffers(2, 1, &old);
    } else {
        a_ctx->PSGetConstantBuffers(2, 1, &old);
    }
    ID3D11RasterizerState* rs = nullptr;
    a_ctx->RSGetState(&rs);
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT       nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    a_ctx->RSGetScissorRects(&nsc, sc);
    bool                         engineScissor = false;
    ID3D11RasterizerState* const clone = ScissorClone(rs, engineScissor);
    const float                  hw = vp.Width * 0.5f;
    for (int e = 0; e < 2; ++e) {
        if (flare) {
            a_ctx->VSSetConstantBuffers(2, 1, &g_postCb[grp].b[e]);
            D3D11_VIEWPORT v2 = vp;
            v2.TopLeftX = vp.TopLeftX + e * hw;
            v2.Width = hw;
            a_ctx->RSSetViewports(1, &v2);
        } else {
            a_ctx->PSSetConstantBuffers(2, 1, &g_postCb[grp].b[e]);
        }
        if (clone) {
            a_ctx->RSSetState(clone);
            const D3D11_RECT r = EyeHalfScissor(vp, e, engineScissor && nsc > 0 ? &sc[0] : nullptr, grp);
            a_ctx->RSSetScissorRects(1, &r);
        }
        a_draw();
    }
    if (flare) {
        a_ctx->VSSetConstantBuffers(2, 1, &old);
        a_ctx->RSSetViewports(1, &vp);
    } else {
        a_ctx->PSSetConstantBuffers(2, 1, &old);
    }
    a_ctx->RSSetState(rs);
    a_ctx->RSSetScissorRects(nsc, nsc ? sc : nullptr);
    for (IUnknown* u : { static_cast<IUnknown*>(old), static_cast<IUnknown*>(rs) }) {
        if (u) {
            u->Release();
        }
    }
    ++g_postDraws[flare ? 1 : 0];
}
