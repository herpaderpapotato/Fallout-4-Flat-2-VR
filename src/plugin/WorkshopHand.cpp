// Workshop placement / selection along the primary hand's pointer (FO4VR). See WorkshopHand.h.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/WorkshopHand.h"
#include "plugin/ConfigStore.h"
#include "plugin/GameBridge.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

namespace fo4flatvr::wshand
{
    namespace
    {
        constexpr std::uintptr_t kContextDataCtor = 0x3897F0;  // Workshop::ContextData::ContextData(ContextData*, Actor&)
        constexpr std::uintptr_t kQEnabled = 0x37E430;         // Workshop::QEnabled(): movzx eax, byte [rip+x]; ret
        struct Site
        {
            std::uintptr_t rva;
            std::uint8_t   bytes[5];
            const char*    what;
        };
        // E8 rel32 -> 3897F0
        constexpr Site kSites[2] = {
            { 0xBD56AC, { 0xE8, 0x3F, 0x41, 0x7B, 0xFF }, "MainRenderAEnd -> Workshop::Update (placement)" },
            { 0xBD5999, { 0xE8, 0x52, 0x3E, 0x7B, 0xFF }, "AutoAim_PickRef -> UpdateForegroundReferenceJob (highlight)" },
        };

        using Ctor_t = void* (*)(void*, void*);
        Ctor_t g_orig[2]{};
        bool   g_hooked[2]{};

        std::atomic<bool>          g_on{ true };
        std::atomic<int>           g_live{ -1 };  // pipe override (-1 = ini)
        std::atomic<std::uint64_t> g_calls[2]{}, g_applied[2]{}, g_noRay[2]{};
        std::atomic<int>           g_state{ -1 };  // last logged: 1 hand ray, 0 camera ray (no fresh hand ray)
        std::mutex                 g_lock;
        float                      g_lastEye[6]{}, g_lastHand[6]{};

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        bool On()
        {
            const int l = g_live.load(std::memory_order_relaxed);
            return l >= 0 ? l != 0 : g_on.load(std::memory_order_relaxed);
        }

        void ReadIni()
        {
            std::string v;
            const bool  on = !cfgstore::EffectiveValue("Controllers", "bWorkshopHandPointer", v) || std::atoi(v.c_str()) != 0;
            if (on != g_on.exchange(on)) {
                REX::INFO("workshop hand pointer {} ([Controllers] bWorkshopHandPointer)", on ? "on" : "off");
            }
        }

        // ContextData layout (both exes): +0x00 origin (NiPoint3), +0x0C direction (NiPoint3), +0x18 player yaw, ...
        void Apply(void* a_ctx, int a_site)
        {
            g_calls[a_site].fetch_add(1, std::memory_order_relaxed);
            if (!a_ctx || !On()) {
                return;
            }
            float o[3], d[3];
            auto* f = static_cast<float*>(a_ctx);
            const bool ok = game::HandRayWorld(o, d) && std::isfinite(o[0]) && std::isfinite(d[0]) &&
                            (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > 0.25f;
            if (!ok) {
                g_noRay[a_site].fetch_add(1, std::memory_order_relaxed);
                if (a_site == 0 && g_state.exchange(0) != 0) {
                    REX::INFO("workshop hand pointer: no fresh hand ray -> the camera ray (flat) until it returns");
                }
                return;
            }
            {
                std::scoped_lock l(g_lock);
                std::memcpy(g_lastEye, f, sizeof(g_lastEye));
                g_lastHand[0] = o[0], g_lastHand[1] = o[1], g_lastHand[2] = o[2];
                g_lastHand[3] = d[0], g_lastHand[4] = d[1], g_lastHand[5] = d[2];
            }
            f[0] = o[0], f[1] = o[1], f[2] = o[2];
            f[3] = d[0], f[4] = d[1], f[5] = d[2];
            g_applied[a_site].fetch_add(1, std::memory_order_relaxed);
            if (a_site == 0 && g_state.exchange(1) != 1) {
                REX::INFO("workshop hand pointer: placement + highlight follow the primary hand ray (origin {:.0f},{:.0f},{:.0f} dir {:.2f},{:.2f},{:.2f})",
                    o[0], o[1], o[2], d[0], d[1], d[2]);
            }
        }

        void* HkMain(void* a_ctx, void* a_actor)
        {
            void* r = g_orig[0](a_ctx, a_actor);
            Apply(a_ctx, 0);
            return r;
        }

        void* HkPick(void* a_ctx, void* a_actor)
        {
            void* r = g_orig[1](a_ctx, a_actor);
            Apply(a_ctx, 1);
            return r;
        }

        bool QEnabled()
        {
            static const std::uint8_t* s_flag = [] {
                const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + kQEnabled);
                if (p[0] != 0x0F || p[1] != 0xB6 || p[2] != 0x05 || p[7] != 0xC3) {
                    return static_cast<const std::uint8_t*>(nullptr);
                }
                std::int32_t rel;
                std::memcpy(&rel, p + 3, sizeof(rel));
                return p + 7 + rel;
            }();
            return s_flag && *s_flag;
        }
    }

    void Install()
    {
        static bool s_done = false;
        if (s_done) {
            return;
        }
        s_done = true;
        ReadIni();
        cfgstore::Subscribe("workshop hand pointer", "Controllers/bWorkshopHandPointer", [](const cfgstore::Changes&) { ReadIni(); });
        static REL::Trampoline tramp{ "fo4flatvr workshop hand" };
        tramp.create(64);
        const auto b = Base();
        void* const hooks[2] = { reinterpret_cast<void*>(&HkMain), reinterpret_cast<void*>(&HkPick) };
        for (int i = 0; i < 2; ++i) {
            const auto& s = kSites[i];
            if (std::memcmp(reinterpret_cast<const void*>(b + s.rva), s.bytes, 5) != 0) {
                const auto* p = reinterpret_cast<const std::uint8_t*>(b + s.rva);
                REX::ERROR("workshop hand pointer: {} call at {:X} has bytes {:02X} {:02X} {:02X} {:02X} {:02X}; not hooked", s.what, s.rva, p[0], p[1],
                    p[2], p[3], p[4]);
                continue;
            }
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(b + s.rva + 1), 4);
            if (s.rva + 5 + rel != kContextDataCtor) {
                REX::ERROR("workshop hand pointer: {} call at {:X} targets {:X}, expected {:X}; not hooked", s.what, s.rva, s.rva + 5 + rel,
                    kContextDataCtor);
                continue;
            }
            g_orig[i] = reinterpret_cast<Ctor_t>(tramp.write_call<5>(b + s.rva, hooks[i]));
            g_hooked[i] = true;
        }
        REX::INFO("workshop hand pointer ([Controllers] bWorkshopHandPointer={}): ContextData calls hooked: placement {} (BD56AC), highlight {} (BD5999)",
            g_on.load() ? 1 : 0, g_hooked[0] ? "yes" : "NO", g_hooked[1] ? "yes" : "NO");
    }

    std::string Status()
    {
        float e[6], h[6];
        {
            std::scoped_lock l(g_lock);
            std::memcpy(e, g_lastEye, sizeof(e));
            std::memcpy(h, g_lastHand, sizeof(h));
        }
        const float dot = e[3] * h[3] + e[4] * h[4] + e[5] * h[5];
        return std::format(
            "wshand on={} (ini {} pipe {}) workshop={} hooks placement={} highlight={} | placement calls={} hand={} noRay={} | highlight calls={} hand={} "
            "noRay={} | last eye {:.1f},{:.1f},{:.1f} dir {:.3f},{:.3f},{:.3f} -> hand {:.1f},{:.1f},{:.1f} dir {:.3f},{:.3f},{:.3f} (angle {:.1f} deg)",
            On() ? 1 : 0, g_on.load() ? 1 : 0, g_live.load(), QEnabled() ? 1 : 0, g_hooked[0] ? 1 : 0, g_hooked[1] ? 1 : 0, g_calls[0].load(),
            g_applied[0].load(), g_noRay[0].load(), g_calls[1].load(), g_applied[1].load(), g_noRay[1].load(), e[0], e[1], e[2], e[3], e[4], e[5], h[0],
            h[1], h[2], h[3], h[4], h[5], std::acos(std::fmax(-1.0f, std::fmin(1.0f, dot))) * 57.2957795f);
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg == "on" || a_arg == "off") {
            g_live = a_arg == "on" ? 1 : 0;
        } else if (a_arg == "ini") {
            g_live = -1;
        } else if (!a_arg.empty()) {
            return "error: wshand [on|off|ini]";
        }
        return Status();
    }
}
