// VR dialogue, game side (Dialogue.h): D-look call-site patches, UI / MenuTopicManager polling, weapon cull, haptic
// edge, primary stick -> option button.
#include "plugin/Dialogue.h"

#include "plugin/ControllerBindings.h"
#include "plugin/Settings.h"
#include "plugin/VirtualPad.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <utility>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

namespace fo4flatvr::dlg
{
    namespace
    {
        // 1.11.240 (analysis/vr-vats-dialogue.md 3.4 / 6.1; verified in Ghidra 14 Nov: each site is
        // `mov rcx,[30E6E58] ; mov edx,0xC ; call PlayerCamera::QCameraEquals 102B5C0` in MenuTopicManager::UpdateSpeaker
        // BAF9C0, and BB035D reads [30E6E50]+0x4C right before the third).
        constexpr std::uintptr_t kQCameraEquals = 0x102B5C0;
        constexpr std::uintptr_t kSites[3] = { 0xBB0039, 0xBB0251, 0xBB0376 };  // looking / show buttons / keep menu
        constexpr std::uintptr_t kMenuTopicManager = 0x30E6E50;                 // MenuTopicManager* (singleton pointer)
        constexpr std::size_t    kMenuOpen = 0x4C;                               // bool menuOpen
        constexpr std::size_t    kBlockInput = 0x55;                             // input ignored while set (HandleEvent, TryToInterrupt...)
        // DialogueMenu (1.11 DialogueMenu::ProcessMessage 1043460 / SetIsLookingAtSpeaker 1043E50 / HandleEvent 1043720)
        constexpr std::size_t kDmAcceptInput = 0x154;  // HandleEvent returns early when 0
        constexpr std::size_t kDmLooking = 0x164;      // EnableMenu (1) / DisableMenu (0)
        constexpr std::size_t kDmButtons = 0x165;      // ShowButtonHelp (1) / HideButtonHelp (0)
        constexpr std::uintptr_t kMenuStackLock = 0x3DBD368;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        Config     g_cfg;
        std::mutex g_cfgLock;

        using QCam_t = bool (*)(void*, std::uint32_t);
        QCam_t                     g_orig[3]{};
        bool                       g_patched[3]{};
        std::atomic<bool>          g_look{ true }, g_hideWeapon{ true }, g_haptics{ true }, g_stick{ true };
        std::atomic<std::uint64_t> g_calls[3]{}, g_forced[3]{};

        bool MenuOpenFlag()
        {
            const auto* mtm = *reinterpret_cast<std::uint8_t* const*>(Base() + kMenuTopicManager);
            return mtm && mtm[kMenuOpen] != 0;
        }

        template <int I>
        bool HkQCameraEquals(void* a_cam, std::uint32_t a_id)
        {
            ++g_calls[I];
            if (g_orig[I](a_cam, a_id)) {
                return true;
            }
            if (g_look.load(std::memory_order_relaxed) && MenuOpenFlag()) {
                ++g_forced[I];
                return true;
            }
            return false;
        }
        constexpr QCam_t kThunks[3] = { &HkQCameraEquals<0>, &HkQCameraEquals<1>, &HkQCameraEquals<2> };

        // Main thread (Tick)
        struct Game
        {
            bool          open{ false }, menuOpen{ false }, accept{ false }, looking{ false }, buttons{ false }, block{ false };
            bool          flatExclusive{ false };
            std::string   otherFlat;  // other FlatScreenModel menus (names) when not exclusive
            bool          weaponCulled{ false };  // we culled WEAPON and have not restored it
            std::uint64_t ticks{ 0 }, openTicks{ 0 }, lookingTicks{ 0 }, notLookingTicks{ 0 }, buttonEdges{ 0 }, opens{ 0 };
            std::uint64_t weaponCulls{ 0 }, weaponRecull{ 0 }, weaponRestores{ 0 }, weaponMissing{ 0 };
            std::uint64_t lookOffEdges{ 0 };  // looking 1 -> 0 while open
            // stick -> button
            int           stickBtn{ -1 };  // 0 A, 1 B, 2 X, 3 Y; -1 none
            std::uint64_t stickPresses[4]{};
            std::string   lastStick{ "none" };
        };
        Game              g_g;
        std::mutex        g_gLock;  // g_g strings for the pipe thread
        std::atomic<bool> g_open{ false }, g_flatExclusive{ false }, g_hapticReq{ false }, g_weaponHidden{ false };
        std::atomic<bool> g_canPick{ false };  // options shown + enabled + input accepted

        // BSUtilities::GetObjectByName(root, name, deep, flag) 17A0F50 (prologue checked; same use as GameMenuPointer).
        // The cull is the NiAVObject app-culled flag (flags +0x108 bit 0, what SetAppCulled writes), set directly so no
        // virtual slot numbering is assumed.
        constexpr std::uintptr_t kGetObjectByName = 0x17A0F50;
        using GetObjectByName_t = RE::NiAVObject* (*)(RE::NiAVObject*, const RE::BSFixedString&, bool, bool);
        int g_gobOk = -1;

        RE::NiAVObject* WeaponNode()
        {
            if (g_gobOk < 0) {
                static constexpr std::uint8_t kGob[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 };
                g_gobOk = std::memcmp(reinterpret_cast<const void*>(Base() + kGetObjectByName), kGob, sizeof(kGob)) == 0 ? 1 : 0;
                if (!g_gobOk) {
                    REX::ERROR("dialogue: GetObjectByName {:X} prologue differs; weapon hiding off", kGetObjectByName);
                }
            }
            if (g_gobOk != 1) {
                return nullptr;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* root = player ? player->Get3D(true) : nullptr;
            if (!root) {
                return nullptr;
            }
            static const RE::BSFixedString kWeapon("WEAPON");
            return reinterpret_cast<GetObjectByName_t>(Base() + kGetObjectByName)(root, kWeapon, true, false);
        }
        bool Culled(const RE::NiAVObject* a_o) { return (a_o->flags.flags & 1) != 0; }
        void SetCulled(RE::NiAVObject* a_o, bool a_on)
        {
            if (a_on) {
                a_o->flags.flags |= 1;
            } else {
                a_o->flags.flags &= ~static_cast<std::uint64_t>(1);
            }
        }

        void UpdateWeapon(Game& g, bool a_want)
        {
            if (a_want) {
                RE::NiAVObject* w = WeaponNode();
                if (!w) {
                    ++g.weaponMissing;
                    return;
                }
                if (!g.weaponCulled) {
                    if (Culled(w)) {
                        return;  // already hidden by the game (holstered / something else): leave it alone
                    }
                    SetCulled(w, true);
                    g.weaponCulled = true;
                    ++g.weaponCulls;
                    REX::INFO("dialogue: first-person WEAPON node culled");
                } else if (!Culled(w)) {
                    SetCulled(w, true);  // the game un-culled it (weapon swap / 3D reload)
                    ++g.weaponRecull;
                }
            } else if (g.weaponCulled) {
                if (RE::NiAVObject* w = WeaponNode()) {
                    SetCulled(w, false);
                }
                g.weaponCulled = false;
                ++g.weaponRestores;
                REX::INFO("dialogue: first-person WEAPON node restored");
            }
            g_weaponHidden = g.weaponCulled;
        }

        std::string CfgString(const Config& c)
        {
            return std::format("look={} panel={} hideWeapon={} haptics={} pointer={} stick={} dist={:.2f} down={:.2f} width={:.2f} pitch={:.1f} "
                               "lag={:.2f}s deadzone={:.1f} crop=[{:.3f},{:.3f}..{:.3f},{:.3f}] src={:X} premul={} haptic={:.2f}/{:.2f}s leftHanded={}",
                c.lookAnywhere ? 1 : 0, c.panel ? 1 : 0, c.hideWeapon ? 1 : 0, c.haptics ? 1 : 0, c.pointer ? 1 : 0, c.stickOptions ? 1 : 0,
                c.distanceMeters, c.downMeters, c.widthMeters, c.pitchDeg, c.lagSeconds, c.deadzoneDeg, c.cropLeft, c.cropTop, c.cropRight,
                c.cropBottom, c.source, c.premultiplied ? 1 : 0, c.hapticAmplitude, c.hapticSeconds, c.leftHanded ? 1 : 0);
        }
    }

    std::string Install()
    {
        const auto path = Settings::Path();
        auto rd = [&](const char* sec, const char* k, float d) {
            char buf[64]{};
            ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), path.c_str());
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : d;
        };
        Config c;
        auto rb = [&](const char* k, bool d) { return rd("Dialogue", k, d ? 1.0f : 0.0f) != 0.0f; };
        c.lookAnywhere = rb("bLookAnywhere", c.lookAnywhere);
        c.panel = rb("bPanel", c.panel);
        c.hideWeapon = rb("bHideWeapon", c.hideWeapon);
        c.haptics = rb("bHaptics", c.haptics);
        c.pointer = rb("bPointer", c.pointer);
        c.stickOptions = rb("bStickOptions", c.stickOptions);
        // [Menus] bDialoguePanel (default 1, ui-menus): the options go on the menu panel (Presenter, FlatScreenModel capture,
        // live world) instead of into the stereo world image; options stay selectable wherever the head / hand points
        // (FO4VR rule), weapon hidden, haptic pulse. This module's own crop panel ([Dialogue] bPanel) stays as configured.
        if (::GetPrivateProfileIntA("Menus", "bDialoguePanel", 1, path.c_str()) != 0) {
            c.lookAnywhere = true;
            c.hideWeapon = true;
            c.haptics = true;
        }
        c.distanceMeters = rd("Dialogue", "fDistanceMeters", c.distanceMeters);
        c.downMeters = rd("Dialogue", "fDownMeters", c.downMeters);
        c.widthMeters = rd("Dialogue", "fWidthMeters", c.widthMeters);
        c.pitchDeg = rd("Dialogue", "fPitchDeg", c.pitchDeg);
        c.lagSeconds = (std::max)(0.01f, rd("Dialogue", "fLagSeconds", c.lagSeconds));
        c.deadzoneDeg = std::clamp(rd("Dialogue", "fDeadzoneDeg", c.deadzoneDeg), 0.0f, 90.0f);
        c.cropLeft = std::clamp(rd("Dialogue", "fCropLeft", c.cropLeft), 0.0f, 1.0f);
        c.cropTop = std::clamp(rd("Dialogue", "fCropTop", c.cropTop), 0.0f, 1.0f);
        c.cropRight = std::clamp(rd("Dialogue", "fCropRight", c.cropRight), c.cropLeft + 0.01f, 1.0f);
        c.cropBottom = std::clamp(rd("Dialogue", "fCropBottom", c.cropBottom), c.cropTop + 0.01f, 1.0f);
        c.source = static_cast<int>(rd("Dialogue", "iSourceTarget", static_cast<float>(c.source))) == 0x3E ? 0x3E : 0x3F;
        c.premultiplied = rb("bPremultiplied", c.premultiplied);
        c.hapticAmplitude = std::clamp(rd("Dialogue", "fHapticAmplitude", c.hapticAmplitude), 0.0f, 1.0f);
        c.hapticSeconds = std::clamp(rd("Dialogue", "fHapticSeconds", c.hapticSeconds), 0.0f, 2.0f);
        c.leftHanded = rd("Controllers", "bLeftHanded", 0.0f) != 0.0f;
        {
            std::scoped_lock l(g_cfgLock);
            g_cfg = c;
        }
        g_look = c.lookAnywhere;
        g_hideWeapon = c.hideWeapon;
        g_haptics = c.haptics;
        g_stick = c.stickOptions;

        // D-look: byte-check each call site (mov edx,0xC ; call rel32 -> QCameraEquals), patch independently.
        std::string sites;
        if (c.lookAnywhere) {
            auto& tramp = REL::GetTrampoline();
            if (tramp.empty()) {
                REX::WARN("dialogue: F4SE trampoline not set up; creating a local one");
                tramp.create(64);
            }
            const auto base = Base();
            for (int i = 0; i < 3; ++i) {
                const auto* p = reinterpret_cast<const std::uint8_t*>(base + kSites[i] - 5);
                static constexpr std::uint8_t kMovEdx[6] = { 0xBA, 0x0C, 0x00, 0x00, 0x00, 0xE8 };
                std::int32_t rel = 0;
                std::memcpy(&rel, p + 6, 4);
                const std::uintptr_t target = kSites[i] + 5 + static_cast<std::intptr_t>(rel);
                if (std::memcmp(p, kMovEdx, sizeof(kMovEdx)) != 0 || target != kQCameraEquals) {
                    REX::ERROR("dialogue D-look: site {:X} bytes {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} -> {:X} (expected BA 0C 00 00 00 E8 -> {:X}); "
                               "not patched",
                        kSites[i], p[0], p[1], p[2], p[3], p[4], p[5], target, kQCameraEquals);
                    sites += std::format(" {:X}=MISMATCH", kSites[i]);
                    continue;
                }
                g_orig[i] = reinterpret_cast<QCam_t>(tramp.write_call<5>(base + kSites[i], kThunks[i]));
                g_patched[i] = true;
                sites += std::format(" {:X}=ok", kSites[i]);
            }
            REX::INFO("dialogue D-look: QCameraEquals(0xC) call sites{}", sites);
        } else {
            sites = " not installed (bLookAnywhere=0)";
        }
        const auto s = CfgString(c) + " | look sites" + sites;
        REX::INFO("dialogue: {}", s);
        return s;
    }

    const Config& GetConfig()
    {
        return g_cfg;
    }

    void Tick()
    {
        Game& g = g_g;
        ++g.ticks;
        auto* ui = RE::UI::GetSingleton();
        const bool wasOpen = g.open, wasButtons = g.buttons, wasLooking = g.looking;
        g.open = g.accept = g.looking = g.buttons = false;
        g.flatExclusive = false;
        std::string other;
        if (ui) {
            static const RE::BSFixedString kDialogue("DialogueMenu");
            auto& lock = *reinterpret_cast<RE::BSReadWriteLock*>(Base() + kMenuStackLock);
            lock.lock_read();
            for (const auto& m : ui->menuStack) {
                if (!m) {
                    continue;
                }
                const char* name = m->menuName.c_str();
                const bool  isDialogue = name && std::strcmp(name, "DialogueMenu") == 0;
                if (isDialogue) {
                    const auto* raw = reinterpret_cast<const std::uint8_t*>(m.get());
                    g.open = true;
                    g.accept = raw[kDmAcceptInput] != 0;
                    g.looking = raw[kDmLooking] != 0;
                    g.buttons = raw[kDmButtons] != 0;
                    continue;
                }
                const auto  flags = m->menuFlags.underlying();
                const char* rend = m->customRendererName.c_str();
                // ButtonBarMenu (button hints, depth 16) and CursorMenu also draw through FlatScreenModel (measured at the
                // main menu: ButtonBarMenu 0x84CD40 renderer FlatScreenModel); their output belongs with the dialogue UI.
                const bool benign = name && (std::strcmp(name, "ButtonBarMenu") == 0 || std::strcmp(name, "CursorMenu") == 0);
                if (!benign && (flags & 0x8000) && rend && std::strcmp(rend, "FlatScreenModel") == 0) {
                    other += std::format("{},", name ? name : "?");
                }
            }
            lock.unlock_read();
        }
        g.flatExclusive = g.open && other.empty();
        const auto* mtm = *reinterpret_cast<std::uint8_t* const*>(Base() + kMenuTopicManager);
        g.menuOpen = mtm && mtm[kMenuOpen] != 0;
        g.block = mtm && mtm[kBlockInput] != 0;
        if (g.open) {
            ++g.openTicks;
            ++(g.looking ? g.lookingTicks : g.notLookingTicks);
            if (wasLooking && !g.looking && wasOpen) {
                ++g.lookOffEdges;
            }
        }
        if (g.open && !wasOpen) {
            ++g.opens;
            REX::INFO("dialogue: DialogueMenu opened (menuOpen={} looking={} buttons={} exclusive={} other='{}')", g.menuOpen ? 1 : 0,
                g.looking ? 1 : 0, g.buttons ? 1 : 0, g.flatExclusive ? 1 : 0, other);
        } else if (!g.open && wasOpen) {
            REX::INFO("dialogue: DialogueMenu closed");
        }
        if (g.open && g.buttons && !wasButtons) {
            ++g.buttonEdges;
            if (g_haptics.load()) {
                g_hapticReq = true;
            }
        }
        UpdateWeapon(g, g_hideWeapon.load() && g.open);
        g_open = g.open;
        g_flatExclusive = g.flatExclusive;
        g_canPick = g.open && g.buttons && g.looking && g.accept && !g.block;
        std::scoped_lock l(g_gLock);
        g.otherFlat = other;
    }

    bool Open() { return g_open.load(std::memory_order_relaxed); }
    bool FlatExclusive() { return g_flatExclusive.load(std::memory_order_relaxed); }
    bool TakeHapticRequest() { return g_hapticReq.exchange(false); }
    bool WeaponHidden() { return g_weaponHidden.load(std::memory_order_relaxed); }
    bool CanPick() { return g_canPick.load(std::memory_order_relaxed); }

    void FilterPad(pad::State& a_p, float a_x, float a_y)
    {
        Game& g = g_g;
        // EDI list navigation owns the primary stick (ControllerBindings: D-pad steps); no stick -> option button then
        if (!g_stick.load() || !g_open.load() || bindings::DialogueListNavActive()) {
            g.stickBtn = -1;
            return;
        }
        const bool left = g_cfg.leftHanded;
        // The primary stick belongs to the options while the menu is open (no turning / wheel from it).
        if (left) {
            a_p.lx = a_p.ly = 0.0f;
        } else {
            a_p.rx = a_p.ry = 0.0f;
        }
        const float ax = std::abs(a_x), ay = std::abs(a_y);
        const float m = (std::max)(ax, ay);
        if (g.stickBtn >= 0 && m < 0.35f) {
            g.stickBtn = -1;  // released: re-armed
        } else if (g.stickBtn < 0 && m > 0.7f && g_canPick.load()) {
            // flat gamepad layout of the 4 options: Y top, X left, B right, A bottom
            g.stickBtn = ay >= ax ? (a_y > 0.0f ? 3 : 0) : (a_x > 0.0f ? 1 : 2);
            ++g.stickPresses[g.stickBtn];
            static constexpr const char* kN[4] = { "A(down)", "B(right)", "X(left)", "Y(up)" };
            std::scoped_lock l(g_gLock);
            g.lastStick = std::format("{} at stick {:.2f},{:.2f}", kN[g.stickBtn], a_x, a_y);
            REX::INFO("dialogue: stick -> {}", g.lastStick);
        }
        switch (g.stickBtn) {
        case 0: a_p.a = true; break;
        case 1: a_p.b = true; break;
        case 2: a_p.x = true; break;
        case 3: a_p.y = true; break;
        default: break;
        }
    }

    std::string GameStatus()
    {
        const Game& g = g_g;
        std::string other, stick;
        {
            std::scoped_lock l(g_gLock);
            other = g.otherFlat;
            stick = g.lastStick;
        }
        Config c;
        {
            std::scoped_lock l(g_cfgLock);
            c = g_cfg;
        }
        return std::format("open={} menuOpen={} looking={} buttons={} accept={} block={} exclusive={} otherFlat='{}' opens={} ticks={} openTicks={} "
                           "lookingTicks={} notLookingTicks={} lookOffEdges={} buttonEdges={} | look={} patched={}{}{} calls={}/{}/{} forced={}/{}/{} | "
                           "weapon hide={} hidden={} culls={} recull={} restores={} missing={} | stick on={} canPick={} presses A={} B={} X={} Y={} last {} | "
                           "haptics={} | cfg {}",
            g.open ? 1 : 0, g.menuOpen ? 1 : 0, g.looking ? 1 : 0, g.buttons ? 1 : 0, g.accept ? 1 : 0, g.block ? 1 : 0, g.flatExclusive ? 1 : 0, other,
            g.opens, g.ticks, g.openTicks, g.lookingTicks, g.notLookingTicks, g.lookOffEdges, g.buttonEdges, g_look.load() ? 1 : 0,
            g_patched[0] ? 1 : 0, g_patched[1] ? 1 : 0, g_patched[2] ? 1 : 0, g_calls[0].load(), g_calls[1].load(), g_calls[2].load(),
            g_forced[0].load(), g_forced[1].load(), g_forced[2].load(), g_hideWeapon.load() ? 1 : 0, g_weaponHidden.load() ? 1 : 0, g.weaponCulls,
            g.weaponRecull, g.weaponRestores, g.weaponMissing, g_stick.load() ? 1 : 0, g_canPick.load() ? 1 : 0, g.stickPresses[0], g.stickPresses[1],
            g.stickPresses[2], g.stickPresses[3], stick, g_haptics.load() ? 1 : 0, CfgString(c));
    }

    std::string GameCommand(const std::string& a_arg)
    {
        if (a_arg == "npcs") {
            // Test helper: actors in the player's cell (ref id, name, distance in units), nearest first.
            const auto tasks = F4SE::GetTaskInterface();
            if (!tasks) {
                return "error: no task interface";
            }
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            tasks->AddTask([done]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* cell = player ? player->GetParentCell() : nullptr;
                if (!cell) {
                    done->set_value("error: no cell");
                    return;
                }
                std::vector<std::pair<float, std::string>> v;
                const auto& pp = player->data.location;
                for (const auto& r : cell->references) {
                    if (!r || r.get() == player || !r->IsActor() || r->IsDisabled() || r->IsDeleted()) {
                        continue;
                    }
                    const auto&  p = r->data.location;
                    const float  d = std::sqrt((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) + (p.z - pp.z) * (p.z - pp.z));
                    const char*  nm = r->GetDisplayFullName();
                    const auto*  base = r->GetObjectReference();
                    v.emplace_back(d, std::format("{:08X}/{:08X} '{}' {:.0f}", r->GetFormID(), base ? base->GetFormID() : 0, nm ? nm : "", d));
                }
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::string s = std::format("n={}", v.size());
                for (std::size_t i = 0; i < v.size() && i < 40; ++i) {
                    s += " | " + v[i].second;
                }
                done->set_value(std::move(s));
            });
            if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                return "error: main thread did not run the task within 2 s";
            }
            return fut.get();
        }
        auto onoff = [&](const char* key, std::atomic<bool>& flag) -> std::string {
            const std::string k(key);
            if (a_arg == k + " on" || a_arg == k + " off") {
                flag = a_arg == k + " on";
                return std::format("dlg {} {}", key, flag.load() ? "on" : "off");
            }
            return {};
        };
        using Flag = std::pair<const char*, std::atomic<bool>*>;
        const Flag flags[] = { { "look", &g_look }, { "weapon", &g_hideWeapon }, { "haptics", &g_haptics }, { "stick", &g_stick } };
        for (const auto& [k, f] : flags) {
            if (auto r = onoff(k, *f); !r.empty()) {
                return r;
            }
        }
        return {};
    }

    // Geometry/crop changes from the pipe ("dlg cfg <key> <value>"); DialogueLayer copies the config under this lock.
    bool SetCfgValue(const std::string& a_key, float a_v)
    {
        std::scoped_lock l(g_cfgLock);
        Config& c = g_cfg;
        if (a_key == "dist") c.distanceMeters = a_v;
        else if (a_key == "down") c.downMeters = a_v;
        else if (a_key == "width") c.widthMeters = a_v;
        else if (a_key == "pitch") c.pitchDeg = a_v;
        else if (a_key == "lag") c.lagSeconds = (std::max)(0.01f, a_v);
        else if (a_key == "deadzone") c.deadzoneDeg = a_v;
        else if (a_key == "cropl") c.cropLeft = std::clamp(a_v, 0.0f, 0.99f);
        else if (a_key == "cropt") c.cropTop = std::clamp(a_v, 0.0f, 0.99f);
        else if (a_key == "cropr") c.cropRight = std::clamp(a_v, 0.01f, 1.0f);
        else if (a_key == "cropb") c.cropBottom = std::clamp(a_v, 0.01f, 1.0f);
        else if (a_key == "src") c.source = static_cast<int>(a_v) == 0x3E ? 0x3E : 0x3F;
        else if (a_key == "premul") c.premultiplied = a_v != 0.0f;
        else if (a_key == "amp") c.hapticAmplitude = std::clamp(a_v, 0.0f, 1.0f);
        else if (a_key == "hsec") c.hapticSeconds = std::clamp(a_v, 0.0f, 2.0f);
        else return false;
        return true;
    }

    Config ConfigCopy()
    {
        std::scoped_lock l(g_cfgLock);
        return g_cfg;
    }
}
