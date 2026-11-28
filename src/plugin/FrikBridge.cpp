// FRIK-flat bridge: see FrikBridge.h / FrikBridgeApi.h. RE: only RE::UI::menuMode (paused-frame test).
#include <F4SE/F4SE.h>  // CommonLib before any Windows header
#include <RE/Fallout.h>

#include "plugin/ConfigStore.h"
#include "plugin/ControllerBindings.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/ThreadQueues.h"  // v9 RunOnMain
#include "plugin/FrikBridgeApi.h"
#include "plugin/Gestures.h"
#include "plugin/Haptics.h"
#include "plugin/HeadPose.h"
#include "plugin/Settings.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR

#include "plugin/CallChain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace fo4flatvr::frikbridge
{
    namespace
    {
        std::mutex                            g_lock;
        FO4FlatVR_Frame                       g_frame{};
        std::chrono::steady_clock::time_point g_t{};
        bool                                  g_have = false;
        std::atomic<bool>                     g_frikActive{ false };
        std::atomic<bool>                     g_installed{ false };
        std::atomic<unsigned>                 g_vibrateRequests{ 0 };
        std::string                           g_iniPath;
        float                                 g_unitsPerMeter = 70.0f;

        // ---- own per-frame hook: call C351F3 (Main::OnIdle_UpdatePlayer -> PlayerUpdateEvent Notify C403B0) ----
        constexpr std::uintptr_t                kNotifyCallSite = 0xC351F3;
        constexpr std::uintptr_t                kNotify = 0xC403B0;
        using Notify_t = int (*)(void*, void*);
        Notify_t                                g_origNotify = nullptr;  // the next hook in the chain (floating gun) or Notify
        bool                                    g_hooked = false;
        std::string                             g_hookWhy = "not installed";
        std::atomic<FO4FlatVR_FrameCallback>    g_frameCb{ nullptr };
        std::atomic<void*>                      g_frameCtx{ nullptr };
        std::atomic<std::uint64_t>              g_frameCalls{ 0 };
        std::atomic<FO4FlatVR_FrameCallback>    g_preCb{ nullptr };   // v11: before the floating gun (graph output)
        std::atomic<void*>                      g_preCtx{ nullptr };
        std::atomic<std::uint64_t>              g_preCalls{ 0 };

        // ---- 16 Nov (user: at a terminal "the hands end up freezing in the air"): pause-time frame ----
        // Main::OnIdle (C339D0) runs every frame; the player update (OnIdle_UpdatePlayer -> C351F3) only runs on the job threads it
        // starts when UI::menuMode (+0x1E0) is 0 (OnIdle copies it to Main+0x1D0 at entry and then skips StartThreads), so with a
        // pausing menu (terminal, Pause, crafting, projected Pip-Boy) FRIK-flat got no frame. FO4VR runs FRIK from OnIdle +0x27E
        // (VR D8405E = flat C33E62, call BGSDynamicPersistenceManager::DemoteQueuedActors 338630), paused or not. Here that call
        // site runs FRIK's frame only while paused: main thread, before this frame's StartThreads and after the previous
        // frame's WaitForThreads (no job thread can be in FRIK's C351F3 frame), never in a frame whose player update runs
        // (menuMode != 0 at that point = no StartThreads in this OnIdle) and only if C351F3 did not run since the last check.
        // [FRIK] bPausedFrame=0 = old behaviour (no FRIK frame while paused).
        constexpr std::uintptr_t   kIdleCallSite = 0xC33E62;
        constexpr std::uintptr_t   kDemoteQueuedActors = 0x338630;
        using Demote_t = void (*)(void*);
        Demote_t                   g_origDemote = nullptr;
        bool                       g_idleHooked = false;
        std::string                g_idleWhy = "not installed";
        std::atomic<bool>          g_pausedFrame{ true };
        std::atomic<std::uint64_t> g_pausedCalls{ 0 }, g_pausedSkipStall{ 0 };
        std::uint64_t              g_lastSeenFrameCalls = 0;  // main thread only

        // ---- v3: camera offset (FRIK smooth movement) ----
        std::atomic<FO4FlatVR_CameraOffsetCallback> g_camCb{ nullptr };
        std::atomic<void*>                          g_camCtx{ nullptr };
        std::atomic<bool>                           g_smooth{ true };   // [FRIK] bSmoothMovement / pipe "frik smooth on|off"
        std::atomic<bool>                           g_body{ false };    // [FRIK] bBody
        // [FRIK] bSmoothHorizontal (default 0): FRIK smooths x/y too, but on flat roomscale moves the capsule after a physical
        // step/lean and the x/y smoothing then lags that move: vremu 20 cm head step -> camera error up to 10 u (0.001 without
        // smoothing), plus a 39-51 u trail behind the capsule while running (15 Nov 00:17). 0 = height only.
        std::atomic<bool>                           g_smoothXY{ false };
        std::atomic<bool>                           g_camValid{ false };
        std::atomic<float>                          g_camX{ 0.0f }, g_camY{ 0.0f }, g_camZ{ 0.0f };
        std::atomic<std::uint64_t>                  g_camSteps{ 0 }, g_camValidSteps{ 0 };
        std::atomic<float>                          g_camMaxAbsZ{ 0.0f }, g_camMaxAbsXY{ 0.0f };

        // ---- perf (pipe "frik perf"): game frame interval at this hook and FRIK.dll's frame callback time, per window ----
        // "frik cb off" skips the callback (body frozen) so the frame time can be compared body on/off in one session.
        std::atomic<bool> g_cbSkip{ false };
        struct Perf
        {
            std::mutex    lock;
            std::uint64_t n = 0, nCb = 0;
            double        sumIntMs = 0, maxIntMs = 0, sumCbUs = 0, maxCbUs = 0;
            std::uint32_t hist[64]{};  // frame interval histogram, 0.5 ms bins (last bin = 31.5+ ms)
            long long     last = 0;
        };
        Perf g_perf;
        double QpcFreq()
        {
            static const double f = [] { LARGE_INTEGER q; ::QueryPerformanceFrequency(&q); return static_cast<double>(q.QuadPart); }();
            return f;
        }
        long long Qpc()
        {
            LARGE_INTEGER q;
            ::QueryPerformanceCounter(&q);
            return q.QuadPart;
        }

        int HkNotify(void* a_src, void* a_evt)
        {
            // 15 Nov: the floating gun (next in the chain) and the notify run FIRST, then FRIK-flat's frame: FRIK's primary hand
            // takes the gun's WEAPON of this frame as its grip target (frik-flat C27), so the hand stays on the gun with no lag.
            if (const auto pre = g_preCb.load(std::memory_order_acquire); pre && !g_cbSkip.load(std::memory_order_relaxed)) {
                ++g_preCalls;
                pre(g_preCtx.load(std::memory_order_relaxed));  // v11: the engine's graph output, before the gun moves the rig
            }
            const int r = g_origNotify(a_src, a_evt);
            const long long t0 = Qpc();
            double cbUs = -1.0;
            if (const auto cb = g_frameCb.load(std::memory_order_acquire); cb && !g_cbSkip.load(std::memory_order_relaxed)) {
                ++g_frameCalls;
                cb(g_frameCtx.load(std::memory_order_relaxed));  // FRIK-flat's frame (it catches its own exceptions)
                cbUs = (Qpc() - t0) * 1.0e6 / QpcFreq();
            }
            {
                std::scoped_lock l(g_perf.lock);
                if (g_perf.last) {
                    const double ms = (t0 - g_perf.last) * 1.0e3 / QpcFreq();
                    if (ms < 1000.0) {  // not across a load / pause
                        ++g_perf.n;
                        g_perf.sumIntMs += ms;
                        g_perf.maxIntMs = (std::max)(g_perf.maxIntMs, ms);
                        ++g_perf.hist[(std::min)(63, static_cast<int>(ms * 2.0))];
                    }
                }
                g_perf.last = t0;
                if (cbUs >= 0.0) {
                    ++g_perf.nCb;
                    g_perf.sumCbUs += cbUs;
                    g_perf.maxCbUs = (std::max)(g_perf.maxCbUs, cbUs);
                }
            }
            return r;
        }

        void HkIdleDemote(void* a_mgr)
        {
            g_origDemote(a_mgr);
            const std::uint64_t calls = g_frameCalls.load(std::memory_order_acquire);
            const bool          stalled = calls == g_lastSeenFrameCalls;  // no C351F3 frame since the previous OnIdle
            g_lastSeenFrameCalls = calls;
            if (!g_pausedFrame.load(std::memory_order_relaxed) || g_cbSkip.load(std::memory_order_relaxed)) {
                return;
            }
            const auto cb = g_frameCb.load(std::memory_order_acquire);
            auto* ui = cb ? RE::UI::GetSingleton() : nullptr;
            if (!ui || ui->menuMode == 0) {
                return;  // not paused: this frame's player update calls FRIK at C351F3
            }
            if (!stalled) {
                ++g_pausedSkipStall;  // first paused frame: the previous OnIdle still ran the player update
                return;
            }
            // not while a game loads (the player's 3D is rebuilt then; FRIK never ran there on flat) or at the main menu
            static const RE::BSFixedString s_loading("LoadingMenu"), s_main("MainMenu");
            if (ui->GetMenuOpen(s_loading) || ui->GetMenuOpen(s_main)) {
                return;
            }
            ++g_pausedCalls;
            cb(g_frameCtx.load(std::memory_order_relaxed));  // FRIK-flat's frame (it catches its own exceptions)
        }

        // window report + reset: frames, interval mean/p50/p95/max ms, callback mean/max us
        std::string PerfReport()
        {
            std::scoped_lock l(g_perf.lock);
            auto pct = [&](double p) {
                const std::uint64_t want = static_cast<std::uint64_t>(p * g_perf.n);
                std::uint64_t       acc = 0;
                for (int i = 0; i < 64; ++i) {
                    acc += g_perf.hist[i];
                    if (acc > want) return (i + 1) * 0.5;
                }
                return 32.0;
            };
            const auto s = std::format("perf frames={} int_mean={:.2f} p50<={:.1f} p95<={:.1f} max={:.2f} cb={} cb_mean_us={:.0f} cb_max_us={:.0f} cbskip={}",
                g_perf.n, g_perf.n ? g_perf.sumIntMs / g_perf.n : 0.0, pct(0.5), pct(0.95), g_perf.maxIntMs, g_perf.nCb,
                g_perf.nCb ? g_perf.sumCbUs / g_perf.nCb : 0.0, g_perf.maxCbUs, g_cbSkip.load() ? 1 : 0);
            g_perf.n = g_perf.nCb = 0;
            g_perf.sumIntMs = g_perf.maxIntMs = g_perf.sumCbUs = g_perf.maxCbUs = 0;
            std::memset(g_perf.hist, 0, sizeof(g_perf.hist));
            return s;
        }

        const std::string& IniPath()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                g_iniPath = Settings::Path();
                char buf[64]{};
                ::GetPrivateProfileStringA("General", "fUnitsPerMeter", "70", buf, sizeof(buf), g_iniPath.c_str());
                const float u = std::strtof(buf, nullptr);
                g_unitsPerMeter = u > 1.0f ? u : 70.0f;
            });
            return g_iniPath;
        }

        bool Enabled()
        {
            return ::GetPrivateProfileIntA("FRIK", "bEnable", 1, IniPath().c_str()) != 0;
        }

        // v3: body ON by default since 15 Nov (verified visible under vremu); [FRIK] bBody=0 = smooth movement only
        // test-only override (frik-flat/tools/frik_session.py): environment FO4FLATVR_FRIK_BODY=1, so body tests never touch the
        // user's ini
        bool IniBody()
        {
            char env[8]{};
            if (::GetEnvironmentVariableA("FO4FLATVR_FRIK_BODY", env, sizeof(env)) && env[0] == '1') return true;
            // or a flag file next to the ini (an elevated launch may not inherit the environment)
            std::string flag = IniPath();
            if (const auto slash = flag.find_last_of("\\/"); slash != std::string::npos) flag.resize(slash + 1);
            flag += "fo4flatvr_frik_body.test";
            if (::GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
            return ::GetPrivateProfileIntA("FRIK", "bBody", 1, IniPath().c_str()) != 0;
        }
        bool IniSmooth() { return ::GetPrivateProfileIntA("FRIK", "bSmoothMovement", 1, IniPath().c_str()) != 0; }
        bool IniSmoothXY() { return ::GetPrivateProfileIntA("FRIK", "bSmoothHorizontal", 0, IniPath().c_str()) != 0; }
        // FRIK.dll may call before Install (plugin load order): read the ini then
        bool Body() { return g_installed.load() ? g_body.load() : (Enabled() && IniBody()); }

        // --- same math as FloatingGunInput.cpp (head-relative grip in the camera basis)
        xr::Vec3 Cross(const xr::Vec3& a, const xr::Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            const xr::Vec3 u{ q.x, q.y, q.z };
            const xr::Vec3 c = Cross(u, v);
            const xr::Vec3 t{ 2 * c.x, 2 * c.y, 2 * c.z };
            const xr::Vec3 c2 = Cross(u, t);
            return { v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z };
        }
        void ToGame(const xr::Vec3& v, float o[3]) { o[0] = v.x, o[1] = -v.z, o[2] = v.y; }

        void HeadRel(const xr::Pose& a_grip, bool a_valid, const xr::Pose& a_head, bool a_headValid, FO4FlatVR_HeadRel& a_out)
        {
            a_out = {};
            a_out.valid = a_valid && a_headValid;
            if (!a_out.valid) return;
            const auto&    hq = a_head.orientation;
            const xr::Quat inv{ -hq.x, -hq.y, -hq.z, hq.w };
            ToGame(Rot(inv, { a_grip.position.x - a_head.position.x, a_grip.position.y - a_head.position.y, a_grip.position.z - a_head.position.z }),
                a_out.pos);
            const xr::Vec3 axes[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } };
            for (int c = 0; c < 3; ++c) {
                float g[3];
                ToGame(Rot(inv, Rot(a_grip.orientation, axes[c])), g);
                for (int r = 0; r < 3; ++r) a_out.rot[r][c] = g[r];
            }
        }

        void Copy(const xr::Pose& a_in, FO4FlatVR_Pose& a_out)
        {
            a_out.q[0] = a_in.orientation.x, a_out.q[1] = a_in.orientation.y, a_out.q[2] = a_in.orientation.z, a_out.q[3] = a_in.orientation.w;
            a_out.p[0] = a_in.position.x, a_out.p[1] = a_in.position.y, a_out.p[2] = a_in.position.z;
        }

        // --- C ABI
        std::uint8_t GetFrame(FO4FlatVR_Frame* a_out)
        {
            if (!a_out || a_out->size < sizeof(FO4FlatVR_Frame)) return 0;
            std::scoped_lock l(g_lock);
            if (!g_have) return 0;
            *a_out = g_frame;
            a_out->size = sizeof(FO4FlatVR_Frame);
            a_out->ageSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t).count();
            return 1;
        }

        void Vibrate(std::int32_t a_hand, float a_amplitude, float a_seconds)
        {
            // 16 Nov: played through the haptics queue (Haptics.h; the Presenter drains it on the XR thread)
            ++g_vibrateRequests;
            haptics::RequestFrik(a_hand, a_amplitude, a_seconds);
        }

        // v4: VRIK-style selfie request from the gestures (Gestures.h)
        std::uint8_t GetSelfie(FO4FlatVR_Selfie* a_out)
        {
            if (!a_out || a_out->size < sizeof(FO4FlatVR_Selfie)) return 0;
            const auto s = gestures::SelfieState();
            a_out->size = sizeof(FO4FlatVR_Selfie);
            a_out->seq = s.seq;
            a_out->active = s.active ? 1 : 0;
            a_out->follow = s.follow ? 1 : 0;
            a_out->pad[0] = a_out->pad[1] = 0;
            a_out->distanceM = s.distanceM;
            a_out->yawRad = s.yawRad;
            return 1;
        }

        // What the floating gun does while FRIK-flat's body is active (15 Nov, measured): on flat the first-person rig is a
        // flattened bone tree in player-relative space with no NiNode chain above WEAPON / RArm_Hand (FO4VR's Update1StPersonArm
        // has nothing to move), so FRIK-flat drives its arms from stand-in hand nodes (FRIK's own PrimaryWeaponOffsetNode .
        // glue^-1, absolute world) and the WEAPON node stays with the floating gun, which already places it with FRIK's glue and
        // per-weapon JSONs (FloatingGun.h W1). The gun therefore keeps placing the weapon; only its controller stand-in models
        // go (FO4VR+FRIK shows hands, not controllers) and come back with the ini value when FRIK goes inactive.
        // [FRIK] bGunStepsAside=1 restores the old full step-aside (gun::StepAside: no weapon placement at all).
        void ApplyGunRole(bool a_frikActive)
        {
            if (::GetPrivateProfileIntA("FRIK", "bGunStepsAside", 0, IniPath().c_str()) != 0) {
                gun::StepAside(a_frikActive);
                return;
            }
            const int ctrl = a_frikActive ? 0 : (::GetPrivateProfileIntA("Weapon", "bControllerModels", 1, IniPath().c_str()) != 0 ? 1 : 0);
            const auto r = gun::Command(std::format("cfg ctrl {}", ctrl));
            REX::INFO("[FRIK] floating gun keeps the weapon; controller models {} ({})", ctrl ? "back" : "hidden while FRIK's body is active", r);
        }

        void SetFrikActive(std::uint8_t a_active)
        {
            const bool on = a_active != 0;
            if (on && !Body()) {
                // v3 guard: without the body FRIK.dll must never switch the floating gun off (smooth-movement-only mode)
                REX::WARN("[FRIK] FRIK-flat asked to go active with bBody=0 - refused (floating gun stays)");
                return;
            }
            if (g_frikActive.exchange(on) == on) return;
            REX::INFO("[FRIK] FRIK-flat reports {}", on ? "active" : "inactive");
            ApplyGunRole(on);
        }

        const char* IniPathC() { return IniPath().c_str(); }

        std::uint8_t SetFrameCallback(FO4FlatVR_FrameCallback a_cb, void* a_ctx)
        {
            if (a_cb && !Body()) {
                REX::WARN("[FRIK] frame callback refused: bBody=0 (smooth-movement-only mode)");
                return 0;
            }
            g_frameCtx.store(a_ctx, std::memory_order_relaxed);
            g_frameCb.store(a_cb, std::memory_order_release);
            REX::INFO("[FRIK] frame callback {} (C351F3 hook {}: {})", a_cb ? "registered" : "cleared", g_hooked ? "installed" : "NOT installed", g_hookWhy);
            return g_hooked ? 1 : 0;
        }

        std::uint8_t SetPreFrameCallbackC(FO4FlatVR_FrameCallback a_cb, void* a_ctx)
        {
            g_preCtx.store(a_ctx, std::memory_order_relaxed);
            g_preCb.store(a_cb, std::memory_order_release);
            REX::INFO("[FRIK] pre-frame (graph output) callback {} (C351F3 hook {})", a_cb ? "registered" : "cleared", g_hooked ? "installed" : "NOT installed");
            return g_hooked ? 1 : 0;
        }

        std::uint8_t BodyEnabled() { return Body() ? 1 : 0; }

        std::uint8_t SetCameraOffsetCallback(FO4FlatVR_CameraOffsetCallback a_cb, void* a_ctx)
        {
            g_camCtx.store(a_ctx, std::memory_order_relaxed);
            g_camCb.store(a_cb, std::memory_order_release);
            g_camValid = false;
            REX::INFO("[FRIK] camera offset (smooth movement) callback {}; bSmoothMovement={}", a_cb ? "registered" : "cleared", g_smooth.load() ? 1 : 0);
            return g_smooth.load() ? 1 : 0;
        }

        // v5: recentre counter (FRIK-flat C36 calibrate-only body height)
        std::uint32_t RecentreCount() { return head::RecenterCount(); }

        // v6: [General] bNoScaling, re-read at most once a second (any thread)
        std::atomic<int>                     g_noScaling{ -1 };
        std::atomic<std::int64_t>            g_noScalingNext{ 0 };
        std::uint8_t NoScalingC()
        {
            const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            if (g_noScaling.load(std::memory_order_relaxed) < 0 || now >= g_noScalingNext.load(std::memory_order_relaxed)) {
                g_noScalingNext.store(now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count(),
                    std::memory_order_relaxed);
                const int v = ::GetPrivateProfileIntA("General", "bNoScaling", 0, IniPath().c_str()) != 0 ? 1 : 0;
                if (g_noScaling.exchange(v) != v) {
                    REX::INFO("[FRIK] [General] bNoScaling={}: {}", v,
                        v ? "the game's default scale (FRIK body scale 1, no PlayerHeight / auto body height / arm-length scaling; 70 units per metre)"
                          : "our scaling on (FRIK PlayerHeight + C36 calibrated body height, fUnitsPerMeter)");
                }
            }
            return static_cast<std::uint8_t>(g_noScaling.load(std::memory_order_relaxed) > 0 ? 1 : 0);
        }

        // v6: FRIK.dll's config table (VR settings panel)
        std::atomic<const FO4FlatVR_FrikConfigApi*> g_cfgApi{ nullptr };
        void SetFrikConfigApiC(const FO4FlatVR_FrikConfigApi* a_api)
        {
            g_cfgApi.store(a_api && a_api->size >= sizeof(FO4FlatVR_FrikConfigApi) ? a_api : nullptr, std::memory_order_release);
            REX::INFO("[FRIK] FRIK.dll config table {} (VR settings panel FRIK pages)", g_cfgApi.load() ? "registered" : a_api ? "REFUSED (size)" : "cleared");
        }

        // v7: settings revision (ConfigStore.h)
        std::uint64_t ConfigRevisionC() { return cfgstore::Revision(); }

        // v8: input claims (ROCK-flat; ControllerBindings.h SetClaims)
        std::uint8_t SetInputClaimsC(std::uint32_t a_owner, std::uint32_t a_left, std::uint32_t a_right)
        {
            return bindings::SetClaims(a_owner, a_left, a_right) ? 1 : 0;
        }

        // v10: input adds (ROCK-flat left-hand fire / button layout; ControllerBindings.h SetAdds)
        std::uint8_t SetInputAddsC(std::uint32_t a_owner, std::uint32_t a_left, std::uint32_t a_right)
        {
            return bindings::SetAdds(a_owner, a_left, a_right) ? 1 : 0;
        }

        // v9: main-thread queue for FRIK-flat (engine cull calls)
        std::uint8_t RunOnMainC(void (*a_fn)(void*), void* a_ctx)
        {
            if (!a_fn) return 0;
            return tq::RunOnMain([a_fn, a_ctx] { a_fn(a_ctx); }) ? 1 : 0;
        }

        // v12 (28 Nov, ROCK-flat R35): the floating gun's kick through FRIK's recoil controllers (FO4FlatVR_KickCallback).
        // [ROCK] bRecoilControl=1 (live): 0 = the gun never calls it and always kicks on its own (the pre-R35 behaviour).
        std::atomic<FO4FlatVR_KickCallback> g_kickCb{ nullptr };
        std::atomic<void*>                  g_kickCtx{ nullptr };
        std::atomic<bool>                   g_kickOn{ true };
        std::atomic<std::uint64_t>          g_kickCalls{ 0 }, g_kickTaken{ 0 }, g_kickDropped{ 0 };
        bool IniRecoilControl() { return ::GetPrivateProfileIntA("ROCK", "bRecoilControl", 1, IniPath().c_str()) != 0; }
        std::uint8_t GetInputContextC() { return bindings::InputContext(); }  // v13
        std::uint8_t SetKickCallbackC(FO4FlatVR_KickCallback a_cb, void* a_ctx)
        {
            g_kickCtx.store(a_ctx, std::memory_order_relaxed);
            g_kickCb.store(a_cb, std::memory_order_release);
            REX::INFO("[FRIK] kick callback {} ([ROCK] bRecoilControl={})", a_cb ? "registered" : "cleared", g_kickOn.load() ? 1 : 0);
            return g_kickOn.load() ? 1 : 0;
        }

        constexpr FO4FlatVR_FrikBridge kBridge{ FO4FLATVR_FRIK_BRIDGE_VERSION, 0, &GetFrame, &Vibrate, &SetFrikActive, &IniPathC, &SetFrameCallback,
            &BodyEnabled, &SetCameraOffsetCallback, &GetSelfie, &RecentreCount, &NoScalingC, &SetFrikConfigApiC, &ConfigRevisionC, &SetInputClaimsC,
            &RunOnMainC, &SetInputAddsC, &SetPreFrameCallbackC, &SetKickCallbackC, &GetInputContextC };
    }

    bool FilterGunKick(const RE::NiTransform& a_kick, RE::NiTransform& a_out)
    {
        static_assert(sizeof(RE::NiTransform) == 16 * sizeof(float), "bridge v12 kick layout");
        const auto cb = g_kickOn.load(std::memory_order_relaxed) && !g_cbSkip.load(std::memory_order_relaxed) ? g_kickCb.load(std::memory_order_acquire) : nullptr;
        if (!cb) return false;
        ++g_kickCalls;
        float in[16], out[16];
        std::memcpy(in, &a_kick, sizeof(in));
        std::memcpy(out, &a_kick, sizeof(out));
        if (cb(g_kickCtx.load(std::memory_order_relaxed), in, out) == 0) return false;
        std::memcpy(&a_out, out, sizeof(out));
        ++g_kickTaken;
        const auto& r = a_out.rotate.entry;
        if (a_out.translate.x == 0.0f && a_out.translate.y == 0.0f && a_out.translate.z == 0.0f && r[0][0] == 1.0f && r[1][1] == 1.0f && r[2][2] == 1.0f) {
            ++g_kickDropped;
        }
        return true;
    }

    bool BodyActive() { return g_frikActive.load(); }
    const FO4FlatVR_FrikConfigApi* ConfigApi() { return g_cfgApi.load(std::memory_order_acquire); }
    bool NoScaling() { return NoScalingC() != 0; }

    void Install()
    {
        // 20 Nov (live settings): fUnitsPerMeter goes to FRIK-flat in every frame snapshot (a change applies at the next frame);
        // bPausedFrame gates the paused-frame hook; bGunStepsAside re-decides the floating gun's role while FRIK is active.
        // FRIK-flat's own [FRIK] switches follow the bridge v7 settings revision (vrshim iniIntLive).
        g_kickOn = IniRecoilControl();
        cfgstore::Subscribe("frikbridge", "General/fUnitsPerMeter,FRIK/bPausedFrame,FRIK/bGunStepsAside,ROCK/bRecoilControl", [](const cfgstore::Changes& a_ch) {
            if (a_ch.Has("ROCK", "bRecoilControl")) {
                g_kickOn = IniRecoilControl();
                REX::INFO("[FRIK] [ROCK] bRecoilControl={} (live): the floating gun's kick {}", g_kickOn.load() ? 1 : 0,
                    g_kickOn.load() ? "goes through FRIK's recoil controllers (ROCK)" : "stays the gun's own");
            }
            if (a_ch.Has("General", "fUnitsPerMeter")) {
                char buf[64]{};
                ::GetPrivateProfileStringA("General", "fUnitsPerMeter", "70", buf, sizeof(buf), IniPath().c_str());
                const float u = std::strtof(buf, nullptr);
                g_unitsPerMeter = u > 1.0f ? u : 70.0f;
                REX::INFO("[FRIK] unitsPerMeter -> {} (live)", g_unitsPerMeter);
            }
            if (a_ch.Has("FRIK", "bPausedFrame")) {
                g_pausedFrame = ::GetPrivateProfileIntA("FRIK", "bPausedFrame", 1, IniPath().c_str()) != 0;
                REX::INFO("[FRIK] paused frame {} (live{})", g_pausedFrame.load() ? "on" : "off", g_idleHooked ? "" : "; C33E62 not hooked");
            }
            if (a_ch.Has("FRIK", "bGunStepsAside") && g_frikActive.load()) {
                gun::StepAside(false);  // undo a full step-aside first; ApplyGunRole sets the role the switch asks for
                ApplyGunRole(true);
            }
        });
        g_installed = true;
        const bool en = Enabled();
        g_body = en && IniBody();
        g_smooth = IniSmooth();
        g_smoothXY = IniSmoothXY();
        if (en && g_body.load()) {
            // own hook, chained: accepts the original call or another fo4flatvr hook there (the floating gun's W1)
            const std::uintptr_t base = REL::Offset(0).address();
            if (callchain::Hookable(base + kNotifyCallSite, base + kNotify, g_hookWhy)) {
                auto& tramp = REL::GetTrampoline();
                if (tramp.empty()) tramp.create(64);
                g_origNotify = reinterpret_cast<Notify_t>(tramp.write_call<5>(base + kNotifyCallSite, &HkNotify));
                g_hooked = true;
            } else {
                REX::ERROR("[FRIK] C351F3 not hookable: {} - FRIK-flat would get no frame", g_hookWhy);
            }
            // pause-time frame (FO4VR's OnIdle slot): the same callback while the game is paused
            // 20 Nov (live settings): hooked whatever bPausedFrame says (HkIdleDemote passes through while it is off), so the
            // switch ("frik paused", the VR panel, the ini) applies live
            g_pausedFrame = ::GetPrivateProfileIntA("FRIK", "bPausedFrame", 1, IniPath().c_str()) != 0;
            if (callchain::Hookable(base + kIdleCallSite, base + kDemoteQueuedActors, g_idleWhy)) {
                auto& tramp = REL::GetTrampoline();
                if (tramp.empty()) tramp.create(64);
                g_origDemote = reinterpret_cast<Demote_t>(tramp.write_call<5>(base + kIdleCallSite, &HkIdleDemote));
                g_idleHooked = true;
            } else {
                REX::ERROR("[FRIK] C33E62 not hookable: {} - FRIK-flat gets no frame while the game is paused", g_idleWhy);
            }
        } else {
            g_hookWhy = en ? "bBody=0 (smooth movement only)" : "bEnable=0";
        }
        REX::INFO("[FRIK] bridge v{}: bEnable={} bBody={} bSmoothMovement={} bSmoothHorizontal={} (FRIK.dll {}), unitsPerMeter={}, frame hook C351F3 {} ({})",
            FO4FLATVR_FRIK_BRIDGE_VERSION, en ? 1 : 0, g_body.load() ? 1 : 0, g_smooth.load() ? 1 : 0, g_smoothXY.load() ? 1 : 0,
            ::GetModuleHandleW(L"FRIK.dll") ? "loaded" : "not loaded", g_unitsPerMeter, g_hooked ? "ok" : "not installed", g_hookWhy);
        REX::INFO("[FRIK] paused frame (C33E62, FO4VR's OnIdle slot; [FRIK] bPausedFrame): {} ({})", g_idleHooked ? "on" : "off", g_idleWhy);
        if (g_frikActive.load()) {
            ApplyGunRole(true);
        }
    }

    void PublishXr(const xr::ControllerState& a_c, const xr::Pose& a_head, bool a_headValid, bool a_worldView, bool a_leftHanded)
    {
        {  // HeadPose.cpp start-up pose jump re-anchor (logged here: HeadPose has no logger)
            static unsigned s_seen = 0;
            float           jm = 0.0f;
            if (const unsigned n = head::PoseJumpRecentres(&jm); n != s_seen) {
                s_seen = n;
                REX::INFO("head: re-anchored on a {:.2f} m head pose jump before any recentre (#{}; runtime placeholder pose; [OpenXR] "
                          "bRecentreOnPoseJump)",
                    jm, n);
            }
        }
        FO4FlatVR_Frame f{};
        f.size = sizeof(f);
        f.active = a_c.active;
        f.worldView = a_worldView;
        f.leftHanded = a_leftHanded;
        f.headValid = a_headValid;
        IniPath();
        f.unitsPerMeter = g_unitsPerMeter;
        Copy(a_head, f.head);
        for (int side = 0; side < 2; ++side) {
            const bool left = side == 0;
            auto&      h = f.hands[side];
            h.gripValid = a_c.active && (left ? a_c.leftGripPoseValid : a_c.rightGripPoseValid);
            h.aimValid = a_c.active && (left ? a_c.leftAimValid : a_c.rightAimValid);
            h.a = left ? a_c.x : a_c.a;
            h.b = left ? a_c.y : a_c.b;
            h.stickClick = left ? a_c.leftStickClick : a_c.rightStickClick;
            h.menu = left ? a_c.menu : 0;
            h.stickX = left ? a_c.leftStickX : a_c.rightStickX;
            h.stickY = left ? a_c.leftStickY : a_c.rightStickY;
            h.trigger = left ? a_c.leftTrigger : a_c.rightTrigger;
            h.grip = left ? a_c.leftGrip : a_c.rightGrip;
            Copy(left ? a_c.leftGripPose : a_c.rightGripPose, h.gripPose);
            Copy(left ? a_c.leftAim : a_c.rightAim, h.aimPose);
            HeadRel(left ? a_c.leftGripPose : a_c.rightGripPose, h.gripValid, a_head, a_headValid, h.gripInHead);
        }
        std::scoped_lock l(g_lock);
        f.seq = g_frame.seq + 1;
        g_frame = f;
        g_t = std::chrono::steady_clock::now();
        g_have = true;
    }
}

namespace fo4flatvr::frikbridge
{
    void StepCameraOffset()
    {
        const auto cb = g_camCb.load(std::memory_order_acquire);
        if (!cb || !g_smooth.load(std::memory_order_relaxed)) {
            g_camValid.store(false, std::memory_order_relaxed);
            return;
        }
        ++g_camSteps;
        float o[3]{};
        const bool ok = cb(g_camCtx.load(std::memory_order_relaxed), o) != 0;
        if (ok) {
            ++g_camValidSteps;
            g_camX.store(o[0], std::memory_order_relaxed);
            g_camY.store(o[1], std::memory_order_relaxed);
            g_camZ.store(o[2], std::memory_order_relaxed);
            const float az = std::abs(o[2]), axy = std::sqrt(o[0] * o[0] + o[1] * o[1]);
            if (az > g_camMaxAbsZ.load(std::memory_order_relaxed)) g_camMaxAbsZ.store(az, std::memory_order_relaxed);
            if (axy > g_camMaxAbsXY.load(std::memory_order_relaxed)) g_camMaxAbsXY.store(axy, std::memory_order_relaxed);
        }
        g_camValid.store(ok, std::memory_order_release);
    }

    bool CameraOffset(float& a_x, float& a_y, float& a_z)
    {
        if (!g_camValid.load(std::memory_order_acquire) || !g_smooth.load(std::memory_order_relaxed)) {
            return false;
        }
        const bool xy = g_smoothXY.load(std::memory_order_relaxed);
        a_x = xy ? g_camX.load(std::memory_order_relaxed) : 0.0f;
        a_y = xy ? g_camY.load(std::memory_order_relaxed) : 0.0f;
        a_z = g_camZ.load(std::memory_order_relaxed);
        return true;
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg == "smooth on" || a_arg == "smooth off") {
            g_smooth = a_arg == "smooth on";
            g_camValid = false;
            REX::INFO("[FRIK] pipe: smooth movement {}", g_smooth.load() ? "on" : "off");
        } else if (a_arg == "smooth xy on" || a_arg == "smooth xy off") {
            g_smoothXY = a_arg == "smooth xy on";
        } else if (a_arg == "smooth reset" || a_arg == "reset") {
            g_camSteps = 0, g_camValidSteps = 0, g_camMaxAbsZ = 0.0f, g_camMaxAbsXY = 0.0f;
        } else if (a_arg == "perf") {
            return PerfReport();
        } else if (a_arg == "cb off" || a_arg == "cb on") {
            g_cbSkip = a_arg == "cb off";
            REX::INFO("[FRIK] pipe: FRIK.dll frame callback {}", g_cbSkip.load() ? "skipped (body frozen; perf A/B)" : "running");
        } else if (a_arg == "paused on" || a_arg == "paused off") {
            g_pausedFrame = a_arg == "paused on";
            REX::INFO("[FRIK] pipe: paused frame {}", g_pausedFrame.load() ? "on" : "off");
        } else if (a_arg == "noscale") {  // [General] bNoScaling (the VR panel's live row; FRIK re-reads it within 1 s)
            g_noScalingNext = 0;
            return std::format("noscale={} (FRIK body: {}; units per metre {}, live)", NoScaling() ? 1 : 0,
                NoScaling() ? "the game's default scale" : "FRIK PlayerHeight / C36", g_unitsPerMeter);
        } else if (a_arg == "reread") {  // a [FRIK] key FRIK-flat re-reads by itself (bFrikPipboy): nothing to do here
            return "FRIK-flat re-reads it within 1 s";
        } else if (a_arg == "cfg" || a_arg.rfind("cfg ", 0) == 0) {  // FRIK.dll's config table (VR panel FRIK pages)
            const auto* api = ConfigApi();
            if (!api) return "frikcfg: none (FRIK.dll not loaded or older than bridge v6)";
            if (a_arg.size() > 4) {
                char sec[96]{}, key[96]{};
                if (sscanf_s(a_arg.c_str() + 4, "%95s %95s", sec, static_cast<unsigned>(sizeof(sec)), key, static_cast<unsigned>(sizeof(key))) == 2) {
                    char v[256]{};
                    const bool have = api->Get(sec, key, v, sizeof(v)) != 0;
                    return std::format("frikcfg {}.{} = {}", sec, key, have ? v : "(not in the file)");
                }
            }
            char st[512]{};
            api->Status(st, sizeof(st));
            return st;
        } else if (!a_arg.empty() && a_arg != "status") {
            return "error: frik [status|smooth on|off|smooth xy on|off|reset|perf|cb on|off|paused on|off|noscale|cfg [<Section> <Key>]]";
        }
        return std::format("frik v={} enable={} body={} smooth={} xy={} frikdll={} camcb={} steps={} valid={} off=({:.3f},{:.3f},{:.3f}) maxz={:.3f} "
                           "maxxy={:.3f} framecb={} frames={} active={} paused={}/{} pausedFrames={} pausedSkipFirst={} noscale={} frikcfg={} "
                           "kick cb={} on={} calls={} taken={} dropped={}",
            FO4FLATVR_FRIK_BRIDGE_VERSION, Enabled() ? 1 : 0, g_body.load() ? 1 : 0, g_smooth.load() ? 1 : 0, g_smoothXY.load() ? 1 : 0,
            ::GetModuleHandleW(L"FRIK.dll") ? 1 : 0, g_camCb.load() ? 1 : 0, g_camSteps.load(), g_camValidSteps.load(),
            g_camX.load(), g_camY.load(), g_camZ.load(), g_camMaxAbsZ.load(), g_camMaxAbsXY.load(), g_frameCb.load() ? 1 : 0,
            g_frameCalls.load(), g_frikActive.load() ? 1 : 0, g_idleHooked ? 1 : 0, g_pausedFrame.load() ? 1 : 0, g_pausedCalls.load(),
            g_pausedSkipStall.load(), NoScaling() ? 1 : 0, ConfigApi() ? 1 : 0, g_kickCb.load() ? 1 : 0, g_kickOn.load() ? 1 : 0,
            g_kickCalls.load(), g_kickTaken.load(), g_kickDropped.load());
    }
}

extern "C" __declspec(dllexport) const FO4FlatVR_FrikBridge* FO4FLATVR_GetFrikBridge(std::uint32_t a_version)
{
    using namespace fo4flatvr::frikbridge;
    // v4 appends GetSelfie, v5 RecentreCount, v6 NoScaling + SetFrikConfigApi, v7 ConfigRevision, v8 SetInputClaims, v9 RunOnMain, v10 SetInputAdds, v11 SetPreFrameCallback, v12 SetKickCallback: an older FRIK.dll gets the same table (it never
    // reads past its own fields)
    if (a_version < FO4FLATVR_FRIK_BRIDGE_MIN_VERSION || a_version > FO4FLATVR_FRIK_BRIDGE_VERSION) {
        REX::WARN("[FRIK] FRIK.dll asked for bridge v{}, have v{}..v{}: FRIK-flat stays dormant", a_version, FO4FLATVR_FRIK_BRIDGE_MIN_VERSION,
            FO4FLATVR_FRIK_BRIDGE_VERSION);
        return nullptr;
    }
    if (!Enabled()) {
        REX::INFO("[FRIK] bEnable=0: FRIK-flat stays dormant");
        return nullptr;
    }
    return &kBridge;
}
