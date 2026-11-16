#include "plugin/CameraTweaks.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

namespace fo4flatvr::game::camtweak
{
    namespace
    {
        std::atomic<bool> g_noAimZoom{ true }, g_fpDialogue{ true }, g_noKillCams{ true }, g_noIdleOrbit{ true };
        std::atomic<bool> g_settingsDirty{ true };
        std::atomic<bool> g_dataReady{ false };  // GMSTs come from the ESM: apply only after kGameDataReady
        std::mutex        g_lock;  // tweak table (main thread writes, pipe thread reports)

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        std::uintptr_t ImageSize()
        {
            const auto b = Base();
            const auto nt = b + *reinterpret_cast<const std::int32_t*>(b + 0x3C);
            return *reinterpret_cast<const std::uint32_t*>(nt + 0x50);  // OptionalHeader.SizeOfImage
        }

        enum class Group { kDialogue, kKillCams, kIdleOrbit };
        enum class Coll { kIni, kGmst };

        struct Tweak
        {
            const char*   name;
            std::uintptr_t rva;     // verified value address (RE::Setting::_value = setting + 8)
            Coll          coll;
            Group         group;
            float         want;     // bool settings: 0/1
            // runtime
            bool          resolved{ false };
            bool          failed{ false };
            const char*   how{ "-" };   // "setting" | "setting@rva" | "raw"
            void*         value{ nullptr };
            bool          haveOrig{ false };
            float         orig{ 0.0f };
            int           writes{ 0 };
        };

        Tweak g_tweaks[] = {
            { "bDialogueCameraEnable:Interface", 0x2F34A80, Coll::kIni, Group::kDialogue, 0.0f },
            { "fKillCamBaseOdds", 0x2F1C920, Coll::kGmst, Group::kKillCams, 0.0f },
            { "fKillCamLevelMaxBias", 0x2F1C950, Coll::kGmst, Group::kKillCams, 0.0f },
            { "bVATSPlayerDeath:VATS", 0x2F37278, Coll::kIni, Group::kKillCams, 0.0f },
            { "bDisableAutoVanityMode:Camera", 0x2F391B0, Coll::kIni, Group::kIdleOrbit, 1.0f },
        };

        bool IsBool(const Tweak& t) { return t.name[0] == 'b'; }

        bool GroupOn(Group g)
        {
            switch (g) {
            case Group::kDialogue: return g_fpDialogue.load();
            case Group::kKillCams: return g_noKillCams.load();
            case Group::kIdleOrbit: return g_noIdleOrbit.load();
            }
            return false;
        }

        float Read(const Tweak& t)
        {
            return IsBool(t) ? static_cast<float>(*static_cast<const std::uint8_t*>(t.value))
                             : *static_cast<const float*>(t.value);
        }

        void Write(Tweak& t, float v)
        {
            if (IsBool(t)) {
                *static_cast<std::uint8_t*>(t.value) = v != 0.0f ? 1 : 0;
            } else {
                *static_cast<float*>(t.value) = v;
            }
        }

        bool Plausible(const Tweak& t, const void* a_value)
        {
            if (IsBool(t)) {
                return *static_cast<const std::uint8_t*>(a_value) <= 1;
            }
            const float f = *static_cast<const float*>(a_value);
            return std::isfinite(f) && std::abs(f) <= 1000.0f;
        }

        RE::Setting* Lookup(const Tweak& t)
        {
            if (t.coll == Coll::kIni) {
                return RE::GetINISetting(t.name);
            }
            auto* gmst = RE::GameSettingCollection::GetSingleton();
            if (!gmst) {
                return nullptr;
            }
            const auto it = gmst->settings.find(RE::BSFixedString(t.name));
            return it != gmst->settings.end() ? it->second : nullptr;
        }

        // Main thread. Finds the value of a setting: the engine Setting object (preferred, its value
        // address checked against the verified RVA), else the RVA if the Setting there carries the right
        // key and a plausible value.
        void Resolve(Tweak& t)
        {
            if (t.resolved || t.failed) {
                return;
            }
            const auto base = Base();
            void*      expect = reinterpret_cast<void*>(base + t.rva);
            if (auto* s = Lookup(t)) {
                void* v = reinterpret_cast<std::uint8_t*>(s) + 8;  // Setting::_value
                const char want = t.name[0];
                if (s->GetKey().empty() || s->GetKey()[0] != want) {
                    REX::ERROR("camera tweak {}: setting has key '{}' (unexpected type); skipped", t.name, s->GetKey());
                    t.failed = true;
                    return;
                }
                if (!Plausible(t, v)) {
                    REX::ERROR("camera tweak {}: implausible current value; skipped", t.name);
                    t.failed = true;
                    return;
                }
                t.value = v;
                t.how = v == expect ? "setting@rva" : "setting";
                t.resolved = true;
                REX::INFO("camera tweak {}: engine setting found, value at rva {:X} ({} verified rva {:X}), current {}", t.name,
                    reinterpret_cast<std::uintptr_t>(v) - base, v == expect ? "==" : "!=", t.rva, Read(t));
                return;
            }
            // raw fallback: Setting{vtbl, value, key} with value at rva -> key pointer at rva + 8
            const auto  key = *reinterpret_cast<const char* const*>(base + t.rva + 8);
            const auto  k = reinterpret_cast<std::uintptr_t>(key);
            const bool  keyOk = k >= base && k < base + ImageSize() && std::strcmp(key, t.name) == 0;
            if (!keyOk || !Plausible(t, expect)) {
                REX::ERROR("camera tweak {}: no engine setting and rva {:X} does not hold it (key {}, value plausible {}); skipped",
                    t.name, t.rva, keyOk ? "ok" : "mismatch", Plausible(t, expect));
                t.failed = true;
                return;
            }
            t.value = expect;
            t.how = "raw";
            t.resolved = true;
            REX::INFO("camera tweak {}: not in the setting collections; using rva {:X} (key verified), current {}", t.name,
                t.rva, Read(t));
        }

        void ApplyOne(Tweak& t, const char* a_when, bool a_log)
        {
            Resolve(t);
            if (!t.resolved) {
                return;
            }
            const float cur = Read(t);
            if (GroupOn(t.group)) {
                if (!t.haveOrig) {
                    t.orig = cur;
                    t.haveOrig = true;
                    REX::INFO("camera tweak {}: original value {}", t.name, cur);
                }
                if (cur != t.want) {
                    Write(t, t.want);
                    ++t.writes;
                    if (a_log || t.writes <= 20) {
                        REX::INFO("camera tweak {}: {} -> {} ({}, via {})", t.name, cur, t.want, a_when, t.how);
                    }
                }
            } else if (t.haveOrig && cur == t.want && t.orig != t.want) {
                Write(t, t.orig);
                REX::INFO("camera tweak {}: switched off, restored {} ({})", t.name, t.orig, a_when);
            }
        }

        // ---- per-frame PlayerCamera fields -----------------------------------------------------------
        bool          g_zoomCleared = false;          // we cleared adjustFOV and owe the game a 1
        std::uint64_t g_zoomClears = 0, g_zoomRestores = 0, g_vanityClears = 0, g_ticks = 0;
        std::atomic<float> g_lastAdjusted{ 0.0f };   // engine GetAdjustedWorldFOV at the last Present
        std::atomic<bool>  g_lastWorldView{ false };

        using GetAdjustedFov_t = float (*)(RE::PlayerCamera*);
        constexpr std::uintptr_t kGetAdjustedWorldFov = 0x1029EB0;
        int g_adjFnState = 0;  // 0 unchecked, 1 ok, -1 bytes mismatch

        float ManualAdjusted(const RE::PlayerCamera* c)
        {
            return c->adjustFOV ? c->worldFOV + c->fovAdjustCurrent + c->fovAnimatorAdjust : c->worldFOV;
        }
    }

    void Configure(const Config& a_cfg)
    {
        g_noAimZoom = a_cfg.noAimZoom;
        g_fpDialogue = a_cfg.firstPersonDialogue;
        g_noKillCams = a_cfg.noKillCams;
        g_noIdleOrbit = a_cfg.noIdleOrbit;
        g_settingsDirty = true;
        REX::INFO("camera tweaks: bNoAimZoom={} bFirstPersonDialogue={} bNoKillCams={} bNoIdleOrbit={}", a_cfg.noAimZoom ? 1 : 0,
            a_cfg.firstPersonDialogue ? 1 : 0, a_cfg.noKillCams ? 1 : 0, a_cfg.noIdleOrbit ? 1 : 0);
    }

    Config Current()
    {
        return { g_noAimZoom.load(), g_fpDialogue.load(), g_noKillCams.load(), g_noIdleOrbit.load() };
    }

    void ApplySettings(const char* a_when)
    {
        g_dataReady = true;
        std::scoped_lock l(g_lock);
        for (auto& t : g_tweaks) {
            ApplyOne(t, a_when, true);
        }
    }

    float AdjustedWorldFov()
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) {
            return 0.0f;
        }
        if (g_adjFnState == 0) {
            // push rdi; sub rsp,30h; cmp byte ptr [rcx+1A9h],0
            static constexpr std::uint8_t kPrologue[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9, 0xA9, 0x01, 0x00, 0x00, 0x00 };
            g_adjFnState = std::memcmp(reinterpret_cast<const void*>(Base() + kGetAdjustedWorldFov), kPrologue, sizeof(kPrologue)) == 0 ? 1 : -1;
            if (g_adjFnState < 0) {
                REX::ERROR("camera tweaks: GetAdjustedWorldFOV prologue mismatch at rva {:X}; using the field sum", kGetAdjustedWorldFov);
            }
        }
        if (g_adjFnState > 0) {
            return reinterpret_cast<GetAdjustedFov_t>(Base() + kGetAdjustedWorldFov)(cam);
        }
        return ManualAdjusted(cam);
    }

    void Tick(bool a_worldView)
    {
        ++g_ticks;
        if (g_dataReady.load() && (g_settingsDirty.exchange(false) || g_ticks % 120 == 0)) {
            // periodic: the in-game settings menu (OnSettingsValueChange) can switch the dialogue camera back on
            std::scoped_lock l(g_lock);
            for (auto& t : g_tweaks) {
                ApplyOne(t, "tick", false);
            }
        }
        g_lastWorldView = a_worldView;
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) {
            return;
        }
        // Aim zoom: GetAdjustedWorldFOV = worldFOV + fovAdjustCurrent (+0x170) + fovAnimatorAdjust (+0x17C)
        // only while adjustFOV (+0x1A9) is set. Engine writers of +0x1A9: PlayerCamera ctor/Revert/
        // DialogueCameraState::ClearData (1), VATSMenu GameState::ChangeState 2 (0) / 8 (1),
        // DialogueCameraState::Begin. None of them runs in the world view (VATS/dialogue cam are not world view).
        if (a_worldView && g_noAimZoom.load()) {
            if (cam->adjustFOV) {
                cam->adjustFOV = false;
                g_zoomCleared = true;
                if (++g_zoomClears <= 5) {
                    REX::INFO("camera tweaks: adjustFOV cleared (world view; fovAdjustCurrent {:.2f} animator {:.2f})",
                        cam->fovAdjustCurrent, cam->fovAnimatorAdjust);
                }
            }
        } else if (g_zoomCleared) {
            if (!cam->adjustFOV) {
                cam->adjustFOV = true;
            }
            g_zoomCleared = false;
            if (++g_zoomRestores <= 5) {
                REX::INFO("camera tweaks: adjustFOV restored ({})", a_worldView ? "bNoAimZoom off" : "left world view");
            }
        }
        if (g_noIdleOrbit.load() && cam->allowAutoVanityMode) {
            cam->allowAutoVanityMode = false;  // StopDialogue/Furniture/Mount/AnimatedMode and LoadGame put it back
            if (++g_vanityClears <= 5) {
                REX::INFO("camera tweaks: allowAutoVanityMode cleared");
            }
        }
        g_lastAdjusted = AdjustedWorldFov();
    }

    std::string Command(const std::string& a_arg)
    {
        if (!a_arg.empty()) {
            auto set = [&](std::atomic<bool>& a_flag, bool a_on) {
                a_flag = a_on;
                g_settingsDirty = true;
            };
            const auto sp = a_arg.find(' ');
            const std::string k = a_arg.substr(0, sp);
            const std::string v = sp == std::string::npos ? std::string{} : a_arg.substr(sp + 1);
            const bool on = v == "1" || v == "on";
            if (v != "0" && v != "1" && v != "on" && v != "off") {
                return "error: camcfg [noaimzoom|fpdialogue|nokillcams|noidleorbit 0|1 | aimzoom on|off]";
            }
            if (k == "aimzoom") {
                set(g_noAimZoom, !on);  // aimzoom on = the game's ADS zoom (bNoAimZoom=0)
            } else if (k == "noaimzoom") {
                set(g_noAimZoom, on);
            } else if (k == "fpdialogue") {
                set(g_fpDialogue, on);
            } else if (k == "nokillcams") {
                set(g_noKillCams, on);
            } else if (k == "noidleorbit") {
                set(g_noIdleOrbit, on);
            } else {
                return "error: unknown camcfg item '" + k + "'";
            }
            REX::INFO("camcfg {}", a_arg);
        }
        std::string s = std::format("noAimZoom={} fpDialogue={} noKillCams={} noIdleOrbit={} worldView={}", g_noAimZoom.load() ? 1 : 0,
            g_fpDialogue.load() ? 1 : 0, g_noKillCams.load() ? 1 : 0, g_noIdleOrbit.load() ? 1 : 0, g_lastWorldView.load() ? 1 : 0);
        {
            std::scoped_lock l(g_lock);
            for (const auto& t : g_tweaks) {
                // short key: the setting name up to ':'
                std::string key(t.name, std::strcspn(t.name, ":"));
                if (t.resolved) {
                    s += std::format(" {}={} {}_orig={} {}_via={}", key, Read(t), key, t.haveOrig ? std::format("{}", t.orig) : "-", key, t.how);
                } else {
                    s += std::format(" {}={} ", key, t.failed ? "unresolved" : "pending");
                }
            }
        }
        if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            s += std::format(" worldFOV={:.2f} firstPersonFOV={:.2f} fovAdjustCurrent={:.3f} fovAdjustTarget={:.3f} fovAdjustPerSec={:.3f}"
                             " fovAnimatorAdjust={:.3f} adjustFOV={} allowAutoVanityMode={} adjustedWorldFOV={:.2f} zoomClears={} zoomRestores={} vanityClears={}",
                cam->worldFOV, cam->firstPersonFOV, cam->fovAdjustCurrent, cam->fovAdjustTarget, cam->fovAdjustPerSec, cam->fovAnimatorAdjust,
                cam->adjustFOV ? 1 : 0, cam->allowAutoVanityMode ? 1 : 0, g_lastAdjusted.load(), g_zoomClears, g_zoomRestores, g_vanityClears);
        }
        return s;
    }
}
