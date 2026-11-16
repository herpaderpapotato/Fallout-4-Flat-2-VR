// StereoCamera.cpp - camera gate and per-eye constants for the single-pass stereo PoC.
// Hooks BSGraphics::State::SetCameraData so the per-eye constant buffer (b5) always matches the camera
// the engine has just applied, and so the draw hook knows whether the current camera is the world camera
// (the only camera rendered in stereo in the PoC; shadow, cube, local-map, Interface3D cameras stay mono).
//
// Verified (1.11.240, Ghidra, this session):
//  - SetCameraData 182BC80 (aid_2277087): copies ViewData + posAdjust into the TLS BSGraphics::Context
//    (view +0x2290, proj +0x22D0, viewProj +0x2310, viewProjUnjittered +0x2350, posAdjust +0x2220), calls
//    SetCameraViewPort(camera+0x184) and Renderer::SetPerFrameConstants (b12). 50 call sites, so an entry
//    detour rather than call-site patches. Prologue = 15 bytes of position-independent code:
//      48 89 5C 24 08 | 48 89 74 24 10 | 57 | 48 83 EC 30
//  - Every Renderer::SetContext call (9 sites) passes a null Context, and the only Context is created in
//    Renderer::Init with deferredContext = 0, so all threads use the default Context *(38E1398) and the
//    immediate ID3D11DeviceContext *(38E13B8). (No deferred contexts in this build.)
//  - World camera = DrawWorld::spCamera *(3E62210); 1st-person camera *(3E622D0) (DeferredPrePass 21F38B0
//    applies 1st-person, then world, before the GBuffer batches).
//  - Projection layout = BSGraphics::State::CalculateCameraViewProj 182C130 (see StereoMath.h).
// Inferred: NiCamera frustum at +0x160 {l, r, t, b, n, f, bool ortho @ +0x18} (CommonLibF4 NiCamera +
// the viewport at +0x184 passed to SetCameraViewPort). Self-checked at runtime below.

#include <REX/LOG.h>

#include "StereoPoC.h"
#include "StereoMath.h"
#include "plugin/HeadPose.h"
#include "StereoInternal.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace fo4flatvr::stereo
{
    namespace
    {
        constexpr std::uintptr_t kSetCameraData = 0x182BC80;
        constexpr std::uint8_t   kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
            0x57, 0x48, 0x83, 0xEC, 0x30 };
        constexpr std::uintptr_t kWorldCamera = 0x3E62210;     // DrawWorld::spCamera
        constexpr std::uintptr_t kFirstPersonCamera = 0x3E622D0;  // DrawWorld 1st-person camera (DeferredPrePass 21F38B0)
        constexpr std::uintptr_t kDefaultContext = 0x38E1398;  // BSGraphics::Context*
        constexpr std::size_t    kCtxPosAdjust = 0x2220;
        constexpr std::size_t    kCtxView = 0x2290;
        constexpr std::size_t    kCtxProj = 0x22D0;
        constexpr std::size_t    kCtxViewProj = 0x2310;
        constexpr std::size_t    kCtxViewProjUnjittered = 0x2350;
        // BSGraphics::State+0xAC: BuildCameraStateData (182D750, decompiled in 1.11.240) applies the TAA
        // jitter to proj/viewProj only if this int is non-zero and the camera asks for jitter.
        constexpr std::size_t    kStateJitterEnable = 0xAC;
        constexpr std::size_t    kCamFrustum = 0x160;

        using SetCameraData_t = void (*)(void* a_state, void* a_camera, bool a_jitter, float a_x, float a_y);
        SetCameraData_t g_original = nullptr;

        std::mutex g_eyeLock;
        EyeFrame   g_published;    // from the Presenter, for the frame being rendered
        EyeFrame   g_used;         // what the last stereo world camera was built from
        bool       g_usedValid = false;
        int        g_selfChecks[3]{};
        int        g_savedJitter = -1;  // State+0xAC before noJitter forced it to 0 (-1 = not forced)

        std::uint8_t* Base() { return reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr)); }

        // "stereotrace <n>": log the world camera's view matrix, posAdjust and the eyes for the next n applications
        // (per-frame variation check under a static headset pose)
        std::atomic<int> g_trace{ 0 };

        void TraceCamera(const math::M4& a_v, const float* a_posAdjust, const math::EyeRel* a_eyes, const EyeFrame& a_used)
        {
            if (g_trace.load(std::memory_order_relaxed) <= 0) {
                return;
            }
            const int left = g_trace.fetch_sub(1) - 1;
            REX::INFO("stereotrace[{}]: V r0 ({:.9f} {:.9f} {:.9f}) r1 ({:.9f} {:.9f} {:.9f}) r2 ({:.9f} {:.9f} {:.9f}) t ({:.6f} {:.6f} {:.6f}) "
                      "posAdj ({:.6f} {:.6f} {:.6f}) | eyeL pos ({:.6f} {:.6f} {:.6f}) tan ({:.6f} {:.6f} {:.6f} {:.6f}) eyeR pos ({:.6f} {:.6f} {:.6f}) "
                      "| headQ ({:.9f} {:.9f} {:.9f} {:.9f}) headP ({:.6f} {:.6f} {:.6f})",
                left, a_v.m[0][0], a_v.m[0][1], a_v.m[0][2], a_v.m[1][0], a_v.m[1][1], a_v.m[1][2], a_v.m[2][0], a_v.m[2][1], a_v.m[2][2],
                a_v.m[3][0], a_v.m[3][1], a_v.m[3][2], a_posAdjust[0], a_posAdjust[1], a_posAdjust[2], a_eyes[0].pos[0], a_eyes[0].pos[1],
                a_eyes[0].pos[2], a_eyes[0].tanL, a_eyes[0].tanR, a_eyes[0].tanT, a_eyes[0].tanB, a_eyes[1].pos[0], a_eyes[1].pos[1],
                a_eyes[1].pos[2], a_used.headQ[0], a_used.headQ[1], a_used.headQ[2], a_used.headQ[3], a_used.headP[0], a_used.headP[1],
                a_used.headP[2]);
        }

        // Guard bands (RenderSize.h, [Stereo] iSeamAlign): each eye's region is eyeWidth + seamPad px; seamPad / 2 extra
        // pixels on its left, the rest on its right, the same layout for both eyes (so the real images start exactly one
        // region width apart: every reduced-resolution level and every pixel-parity pattern lines up between the eyes).
        // They extend the eye's FOV at the same angular pixel pitch. Only the rendering uses the widened eyes; the
        // submitted image is cropped back to eyeWidth and claims the eye's real FOV (a_used).
        void PadEyes(const Config& c, math::EyeRel a_eyes[2])
        {
            if (c.seamPad <= 0 || c.eyeWidth <= 0) {
                return;
            }
            const int gL = c.seamPad / 2, gR = c.seamPad - gL;
            for (int e = 0; e < 2; ++e) {
                const double perPx = (a_eyes[e].tanR - a_eyes[e].tanL) / c.eyeWidth;
                a_eyes[e].tanL -= perPx * gL;
                a_eyes[e].tanR += perPx * gR;
            }
        }

        // Test aid (pipe "stereocant <yaw> <pitch>" deg): rotate both eyes by a small extra yaw/pitch relative to the head,
        // like canted HMD eyes or pose noise between the head and the eye views (reproduces far-plane clipping of
        // z = w geometry under symmetric emulator eyes).
        void ApplyCant(math::EyeRel a_eyes[2])
        {
            const double y = internal::g_cantDeg[0].load() * 3.14159265358979 / 180.0, p = internal::g_cantDeg[1].load() * 3.14159265358979 / 180.0;
            if (y == 0.0 && p == 0.0) {
                return;
            }
            const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
            const double Ry[3][3] = { { cy, 0, -sy }, { 0, 1, 0 }, { sy, 0, cy } };
            const double Rx[3][3] = { { 1, 0, 0 }, { 0, cp, sp }, { 0, -sp, cp } };
            double       Rc[3][3]{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k) Rc[i][j] += Ry[i][k] * Rx[k][j];
            for (int e = 0; e < 2; ++e) {
                double r[3][3]{};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k) r[i][j] += a_eyes[e].rot[i][k] * Rc[k][j];
                std::memcpy(a_eyes[e].rot, r, sizeof(r));
            }
        }

        void TestEyes(const Config& c, math::EyeRel a_out[2], EyeFrame& a_used)
        {
            const double t = std::tan(c.testHalfFovDeg * 3.14159265358979 / 180.0);
            const double aspect = (c.eyeWidth > 0 && c.eyeHeight > 0) ? double(c.eyeWidth) / c.eyeHeight : 1.0;
            const double tv = t / aspect;
            for (int e = 0; e < 2; ++e) {
                a_out[e] = math::EyeRel{};
                a_out[e].pos[0] = (e == 0 ? -0.5 : 0.5) * c.testIpdMeters * c.unitsPerMeter;
                a_out[e].tanL = -t;
                a_out[e].tanR = t;
                a_out[e].tanT = tv;
                a_out[e].tanB = -tv;
            }
            ApplyCant(a_out);
            PadEyes(c, a_out);
            // what to claim at submission: head pose +- ipd/2 along head x, symmetric fov
            std::scoped_lock l(g_eyeLock);
            a_used = g_published;
            for (int e = 0; e < 2; ++e) {
                std::memcpy(a_used.eyeQ[e], g_published.headQ, sizeof(a_used.eyeQ[e]));
                // eye position = head position + rotate(headQ, (+-ipd/2, 0, 0)) (OpenXR space)
                const float* q = g_published.headQ;
                const double off = (e == 0 ? -0.5 : 0.5) * c.testIpdMeters;
                const double x = q[0], y = q[1], z = q[2], w = q[3];
                const double rx = 1 - 2 * (y * y + z * z), ry = 2 * (x * y + z * w), rz = 2 * (x * z - y * w);
                a_used.eyeP[e][0] = static_cast<float>(g_published.headP[0] + rx * off);
                a_used.eyeP[e][1] = static_cast<float>(g_published.headP[1] + ry * off);
                a_used.eyeP[e][2] = static_cast<float>(g_published.headP[2] + rz * off);
                const float h = static_cast<float>(std::atan(t)), v = static_cast<float>(std::atan(tv));
                a_used.fov[e][0] = -h;
                a_used.fov[e][1] = h;
                a_used.fov[e][2] = v;
                a_used.fov[e][3] = -v;
            }
        }

        bool HeadsetEyes(const Config& c, math::EyeRel a_out[2], EyeFrame& a_used)
        {
            std::scoped_lock l(g_eyeLock);
            if (!g_published.valid) {
                return false;
            }
            a_used = g_published;
            for (int e = 0; e < 2; ++e) {
                double hq[4], hp[3], eq[4], ep[3], rq[4], rp[3];
                for (int i = 0; i < 4; ++i) {
                    hq[i] = g_published.headQ[i];
                    eq[i] = g_published.eyeQ[e][i];
                }
                for (int i = 0; i < 3; ++i) {
                    hp[i] = g_published.headP[i];
                    ep[i] = g_published.eyeP[e][i];
                }
                math::RelativePose(hq, hp, eq, ep, rq, rp);
                math::FromOpenXr(rq, rp, c.unitsPerMeter, a_out[e]);
                a_out[e].tanL = std::tan(g_published.fov[e][0]);
                a_out[e].tanR = std::tan(g_published.fov[e][1]);
                a_out[e].tanT = std::tan(g_published.fov[e][2]);
                a_out[e].tanB = std::tan(g_published.fov[e][3]);
            }
            ApplyCant(a_out);
            PadEyes(c, a_out);
            return true;
        }

        void OnCameraApplied(void* a_camera)
        {
            const Config c = CurrentConfig();
            int gate = 0;
            auto* const base = Base();
            void* const worldCam = *reinterpret_cast<void**>(base + kWorldCamera);
            void* const firstCam = *reinterpret_cast<void**>(base + kFirstPersonCamera);
            // The 1st-person camera (arms, weapon; DeferredPrePass applies it before the world camera) gets its own
            // eye matrices from its own View/Proj (own near/far, posAdjust): D_e = VP_1st^-1 * VP_1st,e.
            const int which = !a_camera ? 0 : a_camera == worldCam ? 1 : (c.firstPerson && a_camera == firstCam) ? 2 : 0;
            if (c.mode >= Mode::kCensus && which) {
                const auto* fr = reinterpret_cast<const float*>(static_cast<std::uint8_t*>(a_camera) + kCamFrustum);
                const bool  ortho = *(reinterpret_cast<const std::uint8_t*>(fr) + 0x18) != 0;
                auto* const ctx = *reinterpret_cast<std::uint8_t**>(base + kDefaultContext);
                if (!ortho && ctx) {
                    const auto V = math::M4::FromFloats(reinterpret_cast<const float*>(ctx + kCtxView));
                    const auto P = math::M4::FromFloats(reinterpret_cast<const float*>(ctx + kCtxProj));
                    const auto VP = math::M4::FromFloats(reinterpret_cast<const float*>(ctx + kCtxViewProj));
                    const double n = fr[4], f = fr[5];
                    const auto   VPunj = math::M4::FromFloats(reinterpret_cast<const float*>(ctx + kCtxViewProjUnjittered));
                    internal::NoteJitter(math::MaxAbsDiff(VP, VPunj));
                    // TAA jitter of this application: J = VPunj^-1 * VP is a clip-space translation (x, y += j * w).
                    // The eyes get the same pixel offset (StereoMath EyeJitter), so TAA keeps working per eye.
                    double jx = 0.0, jy = 0.0;
                    {
                        math::M4 inv;
                        if (math::Inverse(VPunj, inv)) {
                            const auto J = math::Mul(inv, VP);
                            double off = 0.0;
                            for (int i = 0; i < 4; ++i) {
                                for (int k = 0; k < 4; ++k) {
                                    if (!(i == 3 && k < 2)) {
                                        off = (std::max)(off, std::abs(J.m[i][k] - (i == k ? 1.0 : 0.0)));
                                    }
                                }
                            }
                            if (off < 1e-4 && std::abs(J.m[3][0]) < 0.01 && std::abs(J.m[3][1]) < 0.01) {
                                jx = J.m[3][0];
                                jy = J.m[3][1];
                            }
                            internal::NoteJitterForm(off, J.m[3][0], J.m[3][1]);
                        }
                    }
                    if (g_selfChecks[which] < 5) {
                        // Convention self-check (expected ~1e-6 with TAA off; jitter shows up as a VP-vs-V*P gap).
                        ++g_selfChecks[which];
                        const auto Pf = math::Projection(fr[0], fr[1], fr[2], fr[3], n, f);
                        REX::INFO("stereo: {} camera frustum l{:.4f} r{:.4f} t{:.4f} b{:.4f} n{:.2f} f{:.0f}; "
                                  "|P_ctx - P(frustum)| = {:.3g}, |VP_ctx - V*P| = {:.3g}, posAdjust ({:.1f} {:.1f} {:.1f})",
                            which == 1 ? "world" : "1st-person", fr[0], fr[1], fr[2], fr[3], n, f, math::MaxAbsDiff(P, Pf),
                            math::MaxAbsDiff(VP, math::Mul(V, P)),
                            reinterpret_cast<const float*>(ctx + kCtxPosAdjust)[0],
                            reinterpret_cast<const float*>(ctx + kCtxPosAdjust)[1],
                            reinterpret_cast<const float*>(ctx + kCtxPosAdjust)[2]);
                    }
                    internal::StereoConstants k{};
                    bool ok = false;
                    if (c.mode == Mode::kCensus) {
                        ok = true;  // gate only: census counts what *would* be converted
                    } else if (c.mode == Mode::kIdentity) {
                        // Engine's own matrix, no squeeze: the replacement VS must reproduce the mono image. Converted
                        // draws are doubled like in stereo (native instancing needs id >> 1); eye 1 is discarded by a
                        // "kill" matrix (clip = (0, 0, 0, -w): every vertex behind the w = 0 plane).
                        math::M4 kill{};
                        kill.m[3][3] = -1.0;
                        VP.ToFloats(k.eyeViewProj[0]);
                        kill.ToFloats(k.eyeViewProj[1]);
                        math::M4::Identity().ToFloats(k.eyeFromMonoClip[0]);
                        kill.ToFloats(k.eyeFromMonoClip[1]);
                        k.params[0] = 0.0f;
                        ok = true;
                        math::EyeRel none[2];
                        internal::BuildEyeFrameBuffers(which, V, P, VP, none, fr, true, 0.0, 0.0,
                            reinterpret_cast<const float*>(ctx + kCtxPosAdjust));
                    } else {
                        math::EyeRel eyes[2];
                        EyeFrame     used;
                        ok = c.testEyes ? (TestEyes(c, eyes, used), true) : HeadsetEyes(c, eyes, used);
                        if (ok) {
                            const auto m = math::BuildEyes(V, VP, n, f, eyes, jx, jy);
                            ok = m.valid;
                            for (int e = 0; ok && e < 2; ++e) {
                                m.viewProj[e].ToFloats(k.eyeViewProj[e]);
                                m.fromMonoClip[e].ToFloats(k.eyeFromMonoClip[e]);
                            }
                            k.params[0] = 1.0f;
                            k.params[2] = static_cast<float>(c.eyeWidth);
                            k.params[3] = c.keepMonoDepthClip ? 1.0f : 0.0f;  // twins: keep mono's depth-clip status
                            if (ok) {
                                std::scoped_lock l(g_eyeLock);
                                g_used = used;
                                g_usedValid = true;
                            }
                            if (ok && which == 1) {
                                TraceCamera(V, reinterpret_cast<const float*>(ctx + kCtxPosAdjust), eyes, used);
                            }
                            if (ok && head::TraceArmed()) {  // htrace E (trace only): eye matrices built: head used + game V rotation
                                head::TraceRec r{ 'E' };
                                r.seq = head::Seq();
                                for (int i = 0; i < 4; ++i) r.v[i] = used.headQ[i];
                                for (int i = 0; i < 3; ++i)
                                    for (int j = 0; j < 3; ++j) r.v[4 + i * 3 + j] = static_cast<float>(V.m[i][j]);
                                r.v[13] = static_cast<float>(which);
                                r.v[14] = static_cast<float>(V.m[3][0]), r.v[15] = static_cast<float>(V.m[3][1]);
                                head::Trace(r);
                            }
                            if (ok) {
                                // per-eye b12 copies for the split draws (lighting, composite, water, ...)
                                internal::BuildEyeFrameBuffers(which, V, P, VP, eyes, fr, false, jx, jy,
                                    reinterpret_cast<const float*>(ctx + kCtxPosAdjust));
                            }
                        }
                    }
                    if (ok && c.mode != Mode::kCensus) {
                        ok = internal::UploadConstants(k);
                        // sky: the same eyes without their translation (infinitely far content)
                        internal::StereoConstants ks = k;
                        if (c.mode != Mode::kIdentity) {
                            math::EyeRel eyes[2];
                            EyeFrame     unused;
                            if (c.testEyes ? (TestEyes(c, eyes, unused), true) : HeadsetEyes(c, eyes, unused)) {
                                for (auto& e : eyes) {
                                    e.pos[0] = e.pos[1] = e.pos[2] = 0.0;
                                }
                                const auto m = math::BuildEyes(V, VP, n, f, eyes, jx, jy);
                                for (int e = 0; m.valid && e < 2; ++e) {
                                    m.viewProj[e].ToFloats(ks.eyeViewProj[e]);
                                    m.fromMonoClip[e].ToFloats(ks.eyeFromMonoClip[e]);
                                }
                            }
                        }
                        internal::UploadSkyConstants(ks);
                    }
                    gate = ok ? which : 0;
                }
            }
            internal::SetCameraGate(gate);
        }

        void HkSetCameraData(void* a_state, void* a_camera, bool a_jitter, float a_x, float a_y)
        {
            // noJitter (diagnostic, pipe "stereojitter 0"): force the State's jitter switch off before the
            // camera data is (re)built; restore the engine's value when the switch is released.
            if (a_state) {
                auto* const flag = reinterpret_cast<int*>(static_cast<std::uint8_t*>(a_state) + kStateJitterEnable);
                // The TAA cut (stereo frames, [Stereo] bCutTaa) uses the same switch: 0 = no TAA and no jitter.
                const bool  force = CurrentConfig().noJitter || internal::TaaCutActive();
                if (force && g_savedJitter < 0) {
                    g_savedJitter = *flag;
                    REX::INFO("stereo: jitter forced off (State+0xAC was {})", g_savedJitter);
                }
                if (force) {
                    *flag = 0;
                } else if (g_savedJitter >= 0) {
                    *flag = g_savedJitter;
                    g_savedJitter = -1;
                    REX::INFO("stereo: jitter switch restored");
                }
            }
            internal::BeginCameraApply();
            g_original(a_state, a_camera, a_jitter, a_x, a_y);
            internal::EndCameraApply();
            OnCameraApplied(a_camera);
        }
    }

    bool InstallCameraHook()
    {
        if (g_original) {
            return true;
        }
        auto* const entry = Base() + kSetCameraData;
        if (std::memcmp(entry, kPrologue, sizeof(kPrologue)) != 0) {
            REX::ERROR("stereo: SetCameraData prologue mismatch (another hook or wrong build); camera hook not installed");
            return false;
        }
        // Stub: stolen prologue + jmp [rip+0] -> entry+15
        auto* stub = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stub) {
            return false;
        }
        std::memcpy(stub, kPrologue, sizeof(kPrologue));
        const std::uint8_t jmpAbs[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(stub + 15, jmpAbs, 6);
        const std::uint64_t back = reinterpret_cast<std::uint64_t>(entry + 15);
        std::memcpy(stub + 21, &back, 8);
        DWORD old = 0;
        ::VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old);
        g_original = reinterpret_cast<SetCameraData_t>(stub);

        // Entry: jmp [rip+0] -> HkSetCameraData (14 bytes) + nop (15th byte)
        std::uint8_t patch[15];
        std::memcpy(patch, jmpAbs, 6);
        const std::uint64_t hook = reinterpret_cast<std::uint64_t>(&HkSetCameraData);
        std::memcpy(patch + 6, &hook, 8);
        patch[14] = 0x90;
        if (!::VirtualProtect(entry, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
            g_original = nullptr;
            return false;
        }
        std::memcpy(entry, patch, sizeof(patch));
        ::VirtualProtect(entry, sizeof(patch), old, &old);
        ::FlushInstructionCache(::GetCurrentProcess(), entry, sizeof(patch));
        REX::INFO("stereo: SetCameraData detour installed (stub {})", static_cast<void*>(stub));
        return true;
    }

    namespace internal
    {
        void RequestTrace(int a_frames) { g_trace.store(a_frames); }
    }

    float TestEyesRenderHFovDeg(float a_aspect)
    {
        const Config c = CurrentConfig();
        if (!c.testEyes || (c.mode != Mode::kStereoDebug && c.mode != Mode::kStereoFaithful) || a_aspect <= 0.0f) {
            return 0.0f;
        }
        const double t = std::tan(c.testHalfFovDeg * 3.14159265358979 / 180.0);
        const double eyeAspect = (c.eyeWidth > 0 && c.eyeHeight > 0) ? double(c.eyeWidth) / c.eyeHeight : 1.0;
        const double tanH = (std::max)(t, t / eyeAspect * a_aspect);  // tanV_eye * (2W/H)
        return static_cast<float>(2.0 * std::atan(tanH) * 180.0 / 3.14159265358979);
    }

    void PublishEyes(const EyeFrame& a_eyes)
    {
        std::scoped_lock l(g_eyeLock);
        g_published = a_eyes;
    }

    bool UsedEyes(EyeFrame& a_out)
    {
        std::scoped_lock l(g_eyeLock);
        a_out = g_used;
        return g_usedValid;
    }
}
