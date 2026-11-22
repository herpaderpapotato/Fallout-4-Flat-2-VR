// StereoCuts.cpp - the cut list of the first stereo-default build (analysis/stereo-lighting-post.md section 5,
// stereo-poc-plan.md section 14): effects that are mono by construction (screen-space history, mono screen positions,
// closed NVIDIA libraries, compute passes over the whole double-wide image) are switched off while frames are
// rendered in stereo, and restored as soon as they are not (mono keeps TAA, SSR, ...). Every cut has its own
// [Stereo] switch. Data writes only (no code patches), all on the render thread from the Present hook.
//
// Verified in 1.11.240 (Ghidra, this round), every value is read per frame by the engine:
//  - Setting objects {vtbl, value, key}: value address below, key pointer at value + 8 is checked against the name.
//    bComputeShaderDeferredTiledLighting:Display 2F97BC8 (ShadowSceneNode::UpdateLightList 21BE181 -> DrawWorld flag
//    3E62254 every frame), bMBEnable:Imagespace 2F986B8 (motion blur IsActive 2211EA0), bDoDepthOfField:Imagespace
//    2F99700 (ImageSpaceEffectDepthOfField*::IsActive 2257E8C), bLensFlare:ImageSpace 2F97998 (DeferredLightsImpl
//    21F7900, QueueLight), bVolumetricLightingEnable:Display 2F98918 (NVGodrays::IsEnabled 22189D0),
//    bEnable:NVHBAO 2F4D270 (NVHBAO::QEnabled 21F021A).
//  - TAA: BSGraphics::State+0xAC (3D8724C) = "sAntiAliasing == taa" (BSShaderManager::Initialize 218179A); TAA's
//    IsActive (2214FC0) and the jitter (BuildCameraStateData) need it = 1. The camera hook forces it to 0 (same
//    mechanism as "stereojitter 0"). FXAA: ImageSpaceManager effect[17] (BSImagespaceShaderFXAA, vtbl 290A508)
//    +8 = "sAntiAliasing == fxaa" (218174D).
//  - SSR: runtime flag 3E5DFC1 (copied from bScreenSpaceReflections:LightingShader at init; DeferredComposite
//    21F830F) + effect[71] (ImageSpaceEffectScalableAmbientObscuranceCS, vtbl 2915A68) +0x120 SSR, +0x121 SAO,
//    +8 active = SSR || SAO; Script::ToggleSSR (5E6A00) also calls BSShaderManager::ClearAllRenderPasses 21836F0.
//  - SAO PS variant: effect[70] (ImageSpaceEffectScalableAmbientObscurance, vtbl 2915DD0), IsActive = bSAOEnable &&
//    +8 (2213820); the console "sao cs" swaps the two the same way.

#include <REX/LOG.h>

#include "StereoPoC.h"
#include "StereoInternal.h"
#include "plugin/ThreadQueues.h"
#include "plugin/Dlss.h"

#include <F4SE/F4SE.h>

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace fo4flatvr::stereo::internal
{
    namespace
    {
        std::uint8_t* Base() { return reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr)); }

        struct ByteCut
        {
            const char*    tag;
            std::uintptr_t valueRva;
            const char*    setting;
            bool Config::*sw;
            int            saved{ -1 };     // engine value while applied (-1 = not applied)
            int            verified{ -1 };  // -1 unchecked, 0 key mismatch (cut disabled), 1 ok
        };
        ByteCut g_byteCuts[] = {
            { "tiled", 0x2F97BC8, "bComputeShaderDeferredTiledLighting:Display", &Config::cutTiled },
            { "mb", 0x2F986B8, "bMBEnable:Imagespace", &Config::cutMotionBlur },
            { "dof", 0x2F99700, "bDoDepthOfField:Imagespace", &Config::cutDof },
            { "flare", 0x2F97998, "bLensFlare:ImageSpace", &Config::cutLensFlare },
            { "godrays", 0x2F98918, "bVolumetricLightingEnable:Display", &Config::cutGodrays },
            { "hbao", 0x2F4D270, "bEnable:NVHBAO", &Config::cutHbao },
        };

        constexpr std::uintptr_t kImageSpaceManager = 0x3E5E4B0;
        constexpr std::uintptr_t kSsrFlag = 0x3E5DFC1;
        constexpr std::uintptr_t kClearAllRenderPasses = 0x21836F0;
        constexpr std::uintptr_t kShadowSceneNode = 0x3E5DF60;  // the root ClearAllRenderPasses traverses (aid_2712479 ptr)
        constexpr std::uintptr_t kVtblSaoPs = 0x2915DD0, kVtblSaoCs = 0x2915A68;
        constexpr int            kFxaaIndex = 17, kSaoPsIndex = 70, kSaoCsIndex = 71;

        std::atomic<bool> g_taaApplied{ false };
        int               g_fxaaSaved = -1;
        int               g_fxaaVerified = -1;

        // SSR + SAO variant share effect[71]: one saved snapshot for both
        struct EffState
        {
            std::uint8_t ssrFlag, e71ssr, e71sao, e71active, e70active;
        };
        bool     g_effSaved = false;
        EffState g_eff{};
        int      g_effVerified = -1;

        bool g_want = false;
        int  g_logs = 0;

        bool VerifySetting(ByteCut& a_c)
        {
            if (a_c.verified < 0) {
                const char* key = *reinterpret_cast<const char* const*>(Base() + a_c.valueRva + 8);
                a_c.verified = (key && std::strcmp(key, a_c.setting) == 0) ? 1 : 0;
                if (!a_c.verified) {
                    REX::ERROR("stereo cuts: {} - Setting at {:X} is not '{}'; cut disabled", a_c.tag, a_c.valueRva, a_c.setting);
                }
            }
            return a_c.verified == 1;
        }

        int g_effectNullLogs = 0;

        std::uint8_t* Effect(int a_index)
        {
            // The effect pointer array is at ImageSpaceManager+0x18; the engine itself reads entries 70/71 without a bound
            // check (Script::ToggleSSR, the "sao" console command), so indices <= 71 are safe. (+0x28 is not a count.)
            auto* const ism = *reinterpret_cast<std::uint8_t**>(Base() + kImageSpaceManager);
            auto* const data = ism ? *reinterpret_cast<std::uint8_t***>(ism + 0x18) : nullptr;
            std::uint8_t* const e = (data && a_index >= 0 && a_index <= kSaoCsIndex) ? data[a_index] : nullptr;
            if (!e && g_effectNullLogs < 6) {
                ++g_effectNullLogs;
                REX::WARN("stereo cuts: ImageSpaceManager effect[{}] not available (manager {}, array {})", a_index,
                    static_cast<void*>(ism), static_cast<void*>(data));
            }
            return e;
        }

        std::uintptr_t VtblRva(const std::uint8_t* a_obj)
        {
            return a_obj ? *reinterpret_cast<const std::uintptr_t*>(a_obj) - reinterpret_cast<std::uintptr_t>(Base()) : 0;
        }

        std::atomic<int> g_ssrPending{ -1 };  // SSR flag value queued to the main thread (-1 none)
        std::atomic<std::uint64_t> g_ssrSwitches{ 0 };

        void ClearAllRenderPasses();

        // The SSR runtime flag is baked into cached render passes, so a change needs BSShaderManager::ClearAllRenderPasses
        // (a scene-graph walk). Called from the Present hook it HUNG the game (reproduced 14 Nov 15:00:45, SanctuaryExt,
        // "ClearAllRenderPasses begin" without "end"; the user's pause-menu hang at 14:16 followed the same restore).
        // The engine itself only does it from Script::ToggleSSR (console/script on the main thread), so the flag write
        // and the clear go to the F4SE task queue (main thread, the same place console commands run).
        void SetSsrFlag(std::uint8_t a_want)
        {
            auto* const ssr = Base() + kSsrFlag;
            if (*ssr == a_want || g_ssrPending.load() == a_want) {
                return;
            }
            // 20 Nov (threading audit): our main-idle queue (ThreadQueues.h; where console commands run, after WaitForThreads),
            // not an F4SE task (those run on whichever thread drains them, job threads included)
            g_ssrPending.store(a_want);
            const bool queued = tq::RunOnMain([a_want]() {
                auto* const flag = Base() + kSsrFlag;
                const std::uint8_t old = *flag;
                *flag = a_want;
                if (old != a_want) {
                    REX::INFO("stereo cuts: SSR flag {} -> {} (main-thread task); ClearAllRenderPasses begin", old, a_want);
                    ClearAllRenderPasses();
                    REX::INFO("stereo cuts: ClearAllRenderPasses end");
                    g_ssrSwitches.fetch_add(1);
                }
                g_ssrPending.store(-1);
            });
            if (!queued) {
                g_ssrPending.store(-1);
                REX::WARN("stereo cuts: no main-thread queue - SSR flag left at {}", *ssr);
            }
        }

        void ClearAllRenderPasses()
        {
            // nothing to clear (and nothing to traverse) before the scene graph exists (main menu / startup)
            if (!*reinterpret_cast<void**>(Base() + kShadowSceneNode)) {
                REX::INFO("stereo cuts: no ShadowSceneNode yet - ClearAllRenderPasses skipped");
                return;
            }
            using Fn = void (*)();
            reinterpret_cast<Fn>(Base() + kClearAllRenderPasses)();
        }

        void ApplyBytes(const Config& c, bool a_want)
        {
            for (auto& b : g_byteCuts) {
                auto* const v = Base() + b.valueRva;
                const bool  on = a_want && (c.*(b.sw)) && VerifySetting(b);
                if (on) {
                    if (b.saved < 0) {
                        b.saved = *v;
                    }
                    if (*v != 0) {
                        *v = 0;
                    }
                } else if (b.saved >= 0) {
                    *v = static_cast<std::uint8_t>(b.saved);
                    b.saved = -1;
                }
            }
        }

        void ApplyTaa(const Config& c, bool a_want)
        {
            // 20 Nov (DLSS agent): DLSS / neural rendering replace the engine's TAA draw (Dlss.h), so the TAA effect must run:
            // the cut is lifted and FXAA forced off while they are on and usable (WantsTaa); lifted again when they fail.
            const bool dlssTaa = a_want && dlss::WantsTaa();
            const bool on = a_want && c.cutTaa && !dlssTaa;
            g_taaApplied.store(on);
            std::uint8_t* const fx = Effect(kFxaaIndex);
            if (g_fxaaVerified < 0 && fx) {
                // effect[17]+8 is set from "sAntiAliasing == fxaa" in BSShaderManager::Initialize; the object is a
                // BSImagespaceShaderFXAA (vtbl 290A508, referenced only by its constructor 2198919)
                const auto rva = VtblRva(fx);
                g_fxaaVerified = (rva == 0x290A508) ? 1 : 0;
                REX::INFO("stereo cuts: effect[{}] vtbl rva {:X} active {} -> {}", kFxaaIndex, rva, fx[8],
                    g_fxaaVerified ? "BSImagespaceShaderFXAA ok" : "unexpected class, FXAA not forced");
            }
            if (g_fxaaVerified != 1 || !fx) {
                return;
            }
            if (on || dlssTaa) {
                if (g_fxaaSaved < 0) {
                    g_fxaaSaved = fx[8];
                }
                fx[8] = on ? 1 : 0;  // TAA cut: FXAA instead; DLSS: no FXAA over its result
            } else if (g_fxaaSaved >= 0) {
                fx[8] = static_cast<std::uint8_t>(g_fxaaSaved);
                g_fxaaSaved = -1;
            }
        }

        void ApplyEffects(const Config& c, bool a_want)
        {
            std::uint8_t* const e70 = Effect(kSaoPsIndex);
            std::uint8_t* const e71 = Effect(kSaoCsIndex);
            if (g_effVerified < 0 && e70 && e71) {
                g_effVerified = (VtblRva(e70) == kVtblSaoPs && VtblRva(e71) == kVtblSaoCs) ? 1 : 0;
                REX::INFO("stereo cuts: effect[70] vtbl {:X} active {} | effect[71] vtbl {:X} active {} ssr {} sao {} | ssr flag {} -> {}",
                    VtblRva(e70), e70[8], VtblRva(e71), e71[8], e71[0x120], e71[0x121], *(Base() + kSsrFlag),
                    g_effVerified ? "ok" : "unexpected classes, SSR/SAO-PS cuts disabled");
            }
            if (g_effVerified != 1) {
                return;
            }
            auto* const ssr = Base() + kSsrFlag;
            const bool  cutSsr = a_want && c.cutSsr, saoPs = a_want && c.saoPixelShader;
            const bool  saoOff = a_want && c.cutSao;  // diagnostic only (not in the default cut list)
            if (!cutSsr && !saoPs && !saoOff) {
                if (g_effSaved) {
                    SetSsrFlag(g_eff.ssrFlag);
                    e71[0x120] = g_eff.e71ssr;
                    e71[0x121] = g_eff.e71sao;
                    e71[8] = g_eff.e71active;
                    e70[8] = g_eff.e70active;
                    g_effSaved = false;
                }
                return;
            }
            if (!g_effSaved) {
                g_eff = { *ssr, e71[0x120], e71[0x121], e71[8], e70[8] };
                g_effSaved = true;
            }
            const std::uint8_t ssrWant = cutSsr ? 0 : g_eff.ssrFlag;
            const std::uint8_t e71ssr = cutSsr ? 0 : g_eff.e71ssr;
            // SAO on at all = the PS effect or the CS effect's SAO part; with saoPs it runs through the PS effect only
            const bool         saoOn = g_eff.e70active || (g_eff.e71active && g_eff.e71sao);
            const std::uint8_t e71sao = (saoPs || saoOff) ? 0 : g_eff.e71sao;
            const std::uint8_t e70v = saoOff ? 0 : saoPs ? (saoOn ? 1 : 0) : g_eff.e70active;
            const std::uint8_t e71v = (e71ssr || e71sao) ? 1 : 0;  // as ToggleSSR: active = SSR || SAO part
            SetSsrFlag(ssrWant);
            e71[0x120] = e71ssr;
            e71[0x121] = e71sao;
            e71[8] = e71v;
            e70[8] = e70v;
        }
    }

    bool TaaCutActive() { return g_taaApplied.load(std::memory_order_relaxed); }

    // Test aid ("stereosetting <tag> <0|1>"): write one of the cut list's engine settings (e.g. force motion blur on to
    // measure it although the user's prefs have it off). While the cut is applied the value goes to the saved copy, so it
    // takes effect when the cut is lifted.
    std::string ForceSetting(const std::string& a_tag, int a_value)
    {
        for (auto& b : g_byteCuts) {
            if (a_tag == b.tag && VerifySetting(b)) {
                auto* const v = Base() + b.valueRva;
                if (b.saved >= 0) {
                    b.saved = a_value ? 1 : 0;
                } else {
                    *v = static_cast<std::uint8_t>(a_value ? 1 : 0);
                }
                return std::format("{} ({}) = {} (cut {})", b.tag, b.setting, a_value ? 1 : 0, b.saved >= 0 ? "applied" : "off");
            }
        }
        if (a_tag == "tiledmin") {
            // iTiledLightingMinLights:Display (2F97BE0, u32; ShadowSceneNode::UpdateLightList 21BE15D): tiled lighting runs
            // only with at least this many lights in the frame (test aid: lower it to force the tiled path)
            auto* const v = reinterpret_cast<std::uint32_t*>(Base() + 0x2F97BE0);
            const char* key = *reinterpret_cast<const char* const*>(Base() + 0x2F97BE0 + 8);
            if (!key || std::strcmp(key, "iTiledLightingMinLights:Display") != 0) {
                return "error: iTiledLightingMinLights setting not found";
            }
            const std::uint32_t old = *v;
            *v = static_cast<std::uint32_t>((std::max)(0, a_value));
            return std::format("tiledmin (iTiledLightingMinLights) {} -> {}", old, *v);
        }
        return "error: stereosetting tiled|mb|dof|flare|godrays|hbao <0|1> | tiledmin <n>";
    }

    void UpdateCuts(bool a_stereoFrames)
    {
        Config c = CurrentConfig();
        // tiled lighting runs per eye (StereoTiled.inl) unless that is unavailable: then the cut stays (volume path)
        c.cutTiled = c.cutTiled || !TiledPerEyeAvailable();
        bool         want = false;
        if (c.forceCuts > 0) {
            want = true;
        } else if (c.forceCuts == 0) {
            // Keyed on the configured MODE, not on the frames: menus, the flat fallback and loading screens render mono
            // frames in a stereo mode, and toggling the cut list there (BSShaderManager::ClearAllRenderPasses for SSR,
            // HBAO/DOF/flare/TAA switches) is what preceded the pause-menu hang (user log 14 Nov 14:16: hang right after
            // "stereo cuts: restored" in the pause menu). Now the list is applied once (first Present in a stereo mode,
            // i.e. at the main menu, before any world render passes exist) and restored only when the mode changes.
            (void)a_stereoFrames;
            want = c.mode == Mode::kStereoDebug || c.mode == Mode::kStereoFaithful;
        }
        const bool changed = want != g_want;
        const bool log = changed && g_logs < 40;
        if (log) {
            ++g_logs;
            REX::INFO("stereo cuts: {} (force {}) - begin", want ? "applying" : "restoring", c.forceCuts);
        }
        g_want = want;
        ApplyBytes(c, want);
        if (log) {
            REX::INFO("stereo cuts: settings done");
        }
        ApplyTaa(c, want);
        if (log) {
            REX::INFO("stereo cuts: taa/fxaa done");
        }
        ApplyEffects(c, want);
        if (log) {
            REX::INFO("stereo cuts: {} (force {})", want ? "applied" : "restored", c.forceCuts);
        }
    }

    std::string CutsStatus()
    {
        std::string s = std::format("cuts={}", g_want ? "on" : "off");
        for (const auto& b : g_byteCuts) {
            s += std::format(" {}={}", b.tag, b.saved >= 0 ? 1 : 0);
        }
        s += std::format(" taa={} fxaa={} ssr={} saops={}", g_taaApplied.load() ? 1 : 0, g_fxaaSaved >= 0 ? 1 : 0,
            g_effSaved && *(Base() + kSsrFlag) == 0 && g_eff.ssrFlag ? 1 : 0,
            g_effSaved && Effect(kSaoPsIndex) && Effect(kSaoPsIndex)[8] && !g_eff.e70active ? 1 : 0);
        return s;
    }
}
