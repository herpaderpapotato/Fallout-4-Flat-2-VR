// VR dialogue, game side (Dialogue.h): D-look call-site patches, UI / MenuTopicManager polling, weapon cull, haptic
// edge, primary stick -> option button.
#include "plugin/Dialogue.h"
#include "plugin/EngineCull.h"

#include "plugin/ControllerBindings.h"
#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"
#include "plugin/ThreadQueues.h"
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
        // +0x55: a player choice is pending (22 Nov, OnMenuButtonPress BAF7C0: 0 -> TryToInterruptDialogueLine, set -> the
        // response is taken + HideButtons + cleared; DialogueMenu::HandleEvent 1043720 returns early while it is set so the
        // movie's option buttons pick). Before 22 Nov it was read as "input blocked" and CanPick required it clear.
        constexpr std::size_t    kChoicePending = 0x55;
        // MenuTopicManager::OnMenuButtonPress(SCENE_ACTION_PLAYER_RESPONSE_TYPE) (FO4VR CEA480, called by the VR thumbstick
        // handler 12ED530): 0 Positive, 1 Negative, 2 Neutral, 3 Question.
        constexpr std::uintptr_t kOnMenuButtonPress = 0xBAF7C0;
        constexpr std::uint8_t   kOmbpBytes[10] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
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
        std::atomic<bool>          g_look{ true }, g_hideWeapon{ true }, g_haptics{ true }, g_stick{ true }, g_listScroll{ true };
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
            bool          open{ false }, menuOpen{ false }, accept{ false }, looking{ false }, buttons{ false }, pending{ false };
            bool          flatExclusive{ false };
            std::string   otherFlat;  // other FlatScreenModel menus (names) when not exclusive
            bool          weaponCulled{ false };  // we culled WEAPON and have not restored it
            std::uint64_t ticks{ 0 }, openTicks{ 0 }, lookingTicks{ 0 }, notLookingTicks{ 0 }, buttonEdges{ 0 }, opens{ 0 };
            std::uint64_t weaponCulls{ 0 }, weaponRecull{ 0 }, weaponRestores{ 0 }, weaponMissing{ 0 };
            std::uint64_t lookOffEdges{ 0 };  // looking 1 -> 0 while open
            // stick -> response (presenter thread)
            bool          stickArmed{ false };  // the stick was in neutral since the last flick / since the menu opened
            int           lastDir{ 0 };
            std::uint64_t stickFlicks[5]{};     // per FO4VR direction 1 up, 2 right, 3 down, 4 left
            std::uint64_t scrollSteps{ 0 };
            int           scrollDir{ 0 };
            std::chrono::steady_clock::time_point nextScroll{};
            std::string   lastStick{ "none" };
        };

        // ---- FO4VR stick direction (BSOpenVRControllerDevice stick, VR 1BC0B10 + 1BBFDD0) ---------------------------
        // magnitude rescaled with fLThumbDeadzone 0.25 .. fLThumbDeadzoneMax 0.94 (VR 3882650 / 3882698, used for both hands),
        // each component below fDirectionalDeadzone 0.5 (38826C8) zeroed, then the dominant axis: |x| >= cos 45 -> right /
        // left. DIRECTION_VAL: 0 none, 1 up, 2 right, 3 down, 4 left.
        constexpr float kThumbDead = 0.25f, kThumbDeadMax = 0.94f, kDirDead = 0.5f;
        int Fo4vrDir(float a_x, float a_y)
        {
            const float r = std::sqrt(a_x * a_x + a_y * a_y);
            if (r <= 0.0f) {
                return 0;
            }
            const float s = std::clamp((r - kThumbDead) / (kThumbDeadMax - kThumbDead), 0.0f, 1.0f) / r;
            float x = a_x * s, y = a_y * s;
            if (std::abs(x) < kDirDead) x = 0.0f;
            if (std::abs(y) < kDirDead) y = 0.0f;
            if (x == 0.0f && y == 0.0f) {
                return 0;
            }
            const float inv = 1.0f / std::sqrt(x * x + y * y);
            if (std::abs(x * inv) >= 0.70710677f) {
                return x < 0.0f ? 4 : 2;
            }
            return y < 0.0f ? 3 : 1;
        }
        // VR 12ED530: direction -> SCENE_ACTION_PLAYER_RESPONSE_TYPE (0 Positive, 1 Negative, 2 Neutral, 3 Question)
        constexpr int kDirToType[5] = { -1, 3, 1, 0, 2 };
        constexpr const char* kDirName[5] = { "none", "up", "right", "down", "left" };
        constexpr const char* kTypeName[4] = { "Positive", "Negative", "Neutral", "Question" };

        // ---- MenuTopicManager::OnMenuButtonPress probe + call -------------------------------------------------------
        using Ombp_t = void (*)(void*, std::int32_t);
        Ombp_t                     g_ombpOrig{ nullptr };   // trampoline stub (detoured) or the function itself
        std::string                g_ombpInstall{ "not installed" };
        std::atomic<std::uint64_t> g_ombpCalls{ 0 }, g_ombpOurs{ 0 };
        std::atomic<int>           g_ombpLastType{ -1 };
        thread_local bool          t_ombpFromUs{ false };
        void HkOnMenuButtonPress(void* a_mtm, std::int32_t a_type)
        {
            ++g_ombpCalls;
            if (t_ombpFromUs) ++g_ombpOurs;
            g_ombpLastType = a_type;
            REX::INFO("dialogue: MenuTopicManager::OnMenuButtonPress({} {}){}", a_type, a_type >= 0 && a_type < 4 ? kTypeName[a_type] : "?",
                t_ombpFromUs ? " (stick, direct)" : "");
            g_ombpOrig(a_mtm, a_type);
        }

        // ---- Extended Dialogue Interface list (main thread: Tick writes, presenter reads) --------------------------------
        enum XdiMode : int
        {
            kNoXdi = 0,      // XDI.dll not loaded or no List_mc (vanilla DialogueMenu.swf)
            kXdiSlots = 1,   // XDI list showing <= 4 rows whose optionIDs are the 4 response slots
            kXdiLong = 2,    // XDI list with more rows / other optionIDs (framework options): stick scrolls
            kXdiHidden = 3,  // XDI list not shown (between lines)
        };
        std::atomic<int> g_xdiDll{ -1 };
        std::atomic<int> g_xdiMode{ kNoXdi };
        std::mutex       g_xdiLock;
        std::string      g_xdiDesc{ "-" };  // under g_xdiLock
        bool XdiDll()
        {
            int e = g_xdiDll.load();
            if (e < 0) {
                e = ::GetModuleHandleW(L"XDI.dll") ? 1 : 0;
                g_xdiDll = e;
            }
            return e != 0;
        }
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
        // 22 Nov (VATS head agent): through the engine's SetAppCulled (EngineCull.h: bit 0 now, engine mark on the main thread)
        void SetCulled(RE::NiAVObject* a_o, bool a_on) { enginecull::Set(a_o, a_on); }

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
            return std::format("look={} panel={} hideWeapon={} haptics={} pointer={} stickResponses={} dist={:.2f} down={:.2f} width={:.2f} pitch={:.1f} "
                               "lag={:.2f}s deadzone={:.1f} crop=[{:.3f},{:.3f}..{:.3f},{:.3f}] src={:X} premul={} haptic={:.2f}/{:.2f}s leftHanded={}",
                c.lookAnywhere ? 1 : 0, c.panel ? 1 : 0, c.hideWeapon ? 1 : 0, c.haptics ? 1 : 0, c.pointer ? 1 : 0, c.stickResponses ? 1 : 0,
                c.distanceMeters, c.downMeters, c.widthMeters, c.pitchDeg, c.lagSeconds, c.deadzoneDeg, c.cropLeft, c.cropTop, c.cropRight,
                c.cropBottom, c.source, c.premultiplied ? 1 : 0, c.hapticAmplitude, c.hapticSeconds, c.leftHanded ? 1 : 0);
        }
    }

    namespace
    {
        Config ReadCfg()
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
        c.stickResponses = rb("bStickResponses", c.stickResponses);
        c.listScroll = rd("Controllers", "bDialogueListNav", 1.0f) != 0.0f;
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
            return c;
        }

        void ApplyCfg(const Config& c)
        {
            {
                std::scoped_lock l(g_cfgLock);
                g_cfg = c;
            }
            g_look = c.lookAnywhere;
            g_hideWeapon = c.hideWeapon;
            g_haptics = c.haptics;
            g_stick = c.stickResponses;
            g_listScroll = c.listScroll;
        }
    }

    std::string Install()
    {
        const Config c = ReadCfg();
        ApplyCfg(c);
        // 20 Nov (live settings, ConfigStore.h): [Dialogue] and [Menus] bDialoguePanel are re-read on a change; the D-look call
        // sites are patched whatever bLookAnywhere says at load (each passes through while g_look is off)
        cfgstore::Subscribe("dialogue", "Dialogue,Menus/bDialoguePanel,Controllers/bLeftHanded,Controllers/bDialogueListNav", [](const cfgstore::Changes&) {
            const Config n = ReadCfg();
            ApplyCfg(n);
            REX::INFO("dialogue: settings re-read (live): {}", CfgString(n));
        });

        // D-look: byte-check each call site (mov edx,0xC ; call rel32 -> QCameraEquals), patch independently.
        std::string sites;
        if (true) {
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
        // 22 Nov: OnMenuButtonPress pass-through probe (records the response type the engine receives, from the stick, a
        // movie button or XDI); the stick calls the function through it. Bytes checked; not detoured if they differ.
        {
            const auto addr = Base() + kOnMenuButtonPress;
            if (std::memcmp(reinterpret_cast<const void*>(addr), kOmbpBytes, sizeof(kOmbpBytes)) == 0) {
                static REL::Trampoline tramp{ "fo4flatvr dialogue pick" };
                tramp.create(64);
                auto* stub = static_cast<std::uint8_t*>(tramp.allocate(5 + 14));
                std::memcpy(stub, reinterpret_cast<const void*>(addr), 5);  // mov [rsp+10h],rbx: position independent
                stub[5] = 0xFF;
                stub[6] = 0x25;
                std::memset(stub + 7, 0, 4);
                const std::uint64_t back = addr + 5;
                std::memcpy(stub + 11, &back, 8);
                g_ombpOrig = reinterpret_cast<Ombp_t>(stub);
                tramp.write_jmp<5>(addr, reinterpret_cast<void*>(&HkOnMenuButtonPress));
                g_ombpInstall = "detoured (pass-through probe)";
            } else {
                g_ombpOrig = reinterpret_cast<Ombp_t>(addr);  // still callable; no probe
                g_ombpInstall = "bytes differ (another mod hooks it?): no probe, called directly";
            }
            REX::INFO("dialogue: MenuTopicManager::OnMenuButtonPress {:X} {}", kOnMenuButtonPress, g_ombpInstall);
        }
        const auto s = CfgString(c) + " | look sites" + sites + " | OnMenuButtonPress " + g_ombpInstall;
        REX::INFO("dialogue: {}", s);
        return s;
    }

    const Config& GetConfig()
    {
        return g_cfg;
    }

    namespace
    {
        using GValue = Scaleform::GFx::Value;
        std::atomic<bool> g_canFlick{ false };  // DialogueMenu open, accepting input, enabled (looking)

        double GNum(const GValue& a_v, double a_def)
        {
            switch (a_v.GetType()) {
            case GValue::ValueType::kNumber: return a_v.GetNumber();
            case GValue::ValueType::kInt: return a_v.GetInt();
            case GValue::ValueType::kUInt: return a_v.GetUInt();
            default: return a_def;
            }
        }
        double GMem(const GValue& a_o, const char* a_n, double a_def)
        {
            GValue v;
            return a_o.IsObject() && a_o.GetMember(a_n, &v) ? GNum(v, a_def) : a_def;
        }

        // Main thread. XDI's movie: root1 = MainTimeline, root1.List_mc = its BSScrollingList (entryList rows carry optionID =
        // the response slot 0..3 for the vanilla options; XDI framework options have their own ids).
        void UpdateXdi(RE::IMenu* a_m, bool a_shown)
        {
            int         mode = kNoXdi;
            std::string desc = "-";
            GValue      list;
            if (a_m && XdiDll() && a_m->uiMovie && a_m->uiMovie->GetVariable(&list, "root1.List_mc") && list.IsObject()) {
                mode = kXdiHidden;
                if (a_shown) {
                    GValue      arr;
                    const double alpha = GMem(list, "alpha", 0.0);
                    const int    sel = static_cast<int>(GMem(list, "selectedIndex", -1.0));
                    std::uint32_t n = 0;
                    bool          slots = true;
                    unsigned      seen = 0;
                    std::string   rows;
                    if (list.GetMember("entryList", &arr) && arr.IsArray()) {
                        n = arr.GetArraySize();
                        for (std::uint32_t i = 0; i < n; ++i) {
                            GValue e, t;
                            arr.GetElement(i, &e);
                            const int id = static_cast<int>(GMem(e, "optionID", -1.0));
                            if (id < 0 || id > 3 || (seen & (1u << id))) {
                                slots = false;
                            } else {
                                seen |= 1u << id;
                            }
                            std::string txt = e.IsObject() && e.GetMember("text", &t) && t.IsString() ? std::string(t.GetString()) : std::string();
                            if (txt.size() > 24) txt.resize(24);
                            std::replace(txt.begin(), txt.end(), '\'', '`');
                            rows += std::format("{}{}:{}\"{}\"", i ? " " : "", i, id, txt);
                        }
                    }
                    if (alpha > 0.0 && n > 0) {
                        mode = slots && n <= 4 ? kXdiSlots : kXdiLong;
                    }
                    desc = std::format("rows={} alpha={:.2f} sel={} [{}]", n, alpha, sel, rows);
                }
            }
            const int prev = g_xdiMode.exchange(mode);
            // log on a mode change or new rows (not on a selection / fade change)
            static std::string s_key;
            const auto         b = desc.find(" [");
            const std::string  key = b == std::string::npos ? std::string() : desc.substr(b);
            if (mode != prev || ((mode == kXdiSlots || mode == kXdiLong) && key != s_key)) {
                static constexpr const char* kM[4] = { "no XDI list", "XDI slots (stick = the 4 responses)", "XDI long list (stick scrolls)",
                    "XDI list hidden" };
                if (mode != kNoXdi || prev != kNoXdi) {
                    REX::INFO("dialogue: {} {}", kM[mode], desc);
                }
            }
            if (mode == kXdiSlots || mode == kXdiLong) {
                s_key = key;
            }
            std::scoped_lock l(g_xdiLock);
            if (mode != kXdiHidden || g_xdiDesc == "-") {
                g_xdiDesc = desc;
            }
        }

        Scaleform::Ptr<RE::IMenu> DialogueMenuPtr()
        {
            auto* ui = RE::UI::GetSingleton();
            static const RE::BSFixedString kDialogue("DialogueMenu");
            return ui ? ui->GetMenu(kDialogue) : nullptr;
        }

        // Main thread: the response `a_type` as FO4VR's stick does (12ED530 -> OnMenuButtonPress). Under XDI with a choice
        // pending, XDI's own row for that slot is picked (its click handler), so XDI's result / framework logic runs.
        void PickOnMain(int a_type, int a_dir)
        {
            auto        menu = DialogueMenuPtr();
            const auto* mtm = *reinterpret_cast<std::uint8_t* const*>(Base() + kMenuTopicManager);
            if (!menu || !mtm || !g_ombpOrig) {
                return;
            }
            const auto* raw = reinterpret_cast<const std::uint8_t*>(menu.get());
            if (!raw[kDmAcceptInput]) {
                return;
            }
            const bool pending = mtm[kChoicePending] != 0;
            GValue     list, root;
            if (XdiDll() && menu->uiMovie && menu->uiMovie->GetVariable(&list, "root1.List_mc") && list.IsObject() &&
                menu->uiMovie->GetVariable(&root, "root1") && root.IsObject()) {
                if (pending && GMem(list, "alpha", 0.0) > 0.0) {
                    GValue arr;
                    int    idx = -1;
                    if (list.GetMember("entryList", &arr) && arr.IsArray()) {
                        for (std::uint32_t i = 0; i < arr.GetArraySize(); ++i) {
                            GValue e;
                            arr.GetElement(i, &e);
                            if (static_cast<int>(GMem(e, "optionID", -1.0)) == a_type) {
                                idx = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (idx < 0) {
                        REX::INFO("dialogue: stick {} -> {} ({}): XDI shows no row for that response; nothing picked", kDirName[a_dir], a_type,
                            kTypeName[a_type]);
                        return;
                    }
                    list.SetMember("selectedIndex", GValue(static_cast<std::int32_t>(idx)));
                    GValue nul(nullptr);
                    const bool ok = root.Invoke("onItemPress", nullptr, &nul, 1);
                    REX::INFO("dialogue: stick {} -> {} ({}): XDI row {} picked ({})", kDirName[a_dir], a_type, kTypeName[a_type], idx,
                        ok ? "onItemPress" : "onItemPress FAILED");
                    return;
                }
            }
            REX::INFO("dialogue: stick {} -> {} ({}): OnMenuButtonPress ({})", kDirName[a_dir], a_type, kTypeName[a_type],
                pending ? "choice pending: the response" : "no choice pending: interrupts the line, as FO4VR");
            t_ombpFromUs = true;
            g_ombpOrig(const_cast<std::uint8_t*>(mtm), a_type);
            t_ombpFromUs = false;
        }

        // Main thread: one scroll step in XDI's list (its own gamepad handlers: DialogueMenu.ProcessUserEvent QuickkeyUp/Down
        // -> List_mc.moveSelectionUp/Down, QuickkeyLeft/Right -> MainTimeline.listmcspindown/up for value rows).
        void ScrollOnMain(int a_dir)
        {
            auto        menu = DialogueMenuPtr();
            const auto* mtm = *reinterpret_cast<std::uint8_t* const*>(Base() + kMenuTopicManager);
            GValue      list, root;
            if (!menu || !mtm || !mtm[kChoicePending] || !menu->uiMovie || !menu->uiMovie->GetVariable(&list, "root1.List_mc") ||
                !list.IsObject() || GMem(list, "alpha", 0.0) <= 0.0 || !menu->uiMovie->GetVariable(&root, "root1") || !root.IsObject()) {
                return;
            }
            bool ok = false;
            switch (a_dir) {
            case 1: ok = list.Invoke("moveSelectionUp"); break;
            case 3: ok = list.Invoke("moveSelectionDown"); break;
            case 4: ok = root.Invoke("listmcspindown"); break;
            case 2: ok = root.Invoke("listmcspinup"); break;
            default: break;
            }
            if (!ok) {
                REX::WARN("dialogue: XDI scroll {} failed", kDirName[a_dir]);
            }
        }
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
        Scaleform::Ptr<RE::IMenu> dlgMenu;  // 22 Nov: XDI list poll (main thread, after the stack walk)
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
                    dlgMenu = m;
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
        g.pending = mtm && mtm[kChoicePending] != 0;
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
        g_canPick = g.open && g.buttons && g.looking && g.accept && g.pending;
        g_canFlick = g.open && g.looking && g.accept;
        UpdateXdi(dlgMenu.get(), g.open && g.buttons);
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
        if (!g_open.load()) {
            g.stickArmed = false;  // a stick held when the menu opens must come back to neutral first
            g.lastDir = g.scrollDir = 0;
            return;
        }
        const int  mode = g_xdiMode.load();
        const bool xdi = mode != kNoXdi;
        const bool responses = g_stick.load();
        if (!responses && !(xdi && g_listScroll.load())) {
            return;  // both off: the stick is passed on as before 22 Nov
        }
        // The primary stick belongs to the options while the menu is open (no turning / wheel from it). Under XDI the
        // bindings' D-pad pulses (bDialogueListNav) are dropped: the list is driven through its movie below.
        if (g_cfg.leftHanded) {
            a_p.lx = a_p.ly = 0.0f;
        } else {
            a_p.rx = a_p.ry = 0.0f;
        }
        if (xdi) {
            a_p.dpadUp = a_p.dpadDown = a_p.dpadLeft = a_p.dpadRight = false;
        }
        const int  dir = Fo4vrDir(a_x, a_y);
        const auto now = std::chrono::steady_clock::now();
        const bool scroll = xdi && g_listScroll.load() && (mode == kXdiLong || (mode == kXdiSlots && !responses));
        if (scroll) {
            // XDI long list: one step per push, repeating while held (the quick-loot pulser timing: 450 ms, then 170 ms)
            if (dir == 0) {
                g.scrollDir = 0;
                g.stickArmed = true;
                return;
            }
            g.stickArmed = false;
            if (!g_canPick.load()) {
                return;
            }
            if (dir != g.scrollDir || now >= g.nextScroll) {
                g.nextScroll = now + std::chrono::milliseconds(dir != g.scrollDir ? 450 : 170);
                g.scrollDir = dir;
                ++g.scrollSteps;
                tq::RunOnMain([dir]() { ScrollOnMain(dir); });
            }
            return;
        }
        g.scrollDir = 0;
        if (dir == 0) {
            g.stickArmed = true;
            g.lastDir = 0;
            return;
        }
        if (!g.stickArmed || !responses) {
            return;  // FO4VR fires on the move out of neutral only (ThumbstickEvent prev == none)
        }
        g.stickArmed = false;
        g.lastDir = dir;
        if (!g_canFlick.load()) {
            return;
        }
        const int type = kDirToType[dir];
        ++g.stickFlicks[dir];
        {
            std::scoped_lock l(g_gLock);
            g.lastStick = std::format("{} -> {} {} at stick {:.2f},{:.2f} ({})", kDirName[dir], type, kTypeName[type], a_x, a_y,
                mode == kXdiSlots ? "XDI row" : mode == kNoXdi ? "OnMenuButtonPress" : "between lines: OnMenuButtonPress");
        }
        tq::RunOnMain([type, dir]() { PickOnMain(type, dir); });
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
        std::string xdi;
        {
            std::scoped_lock l(g_xdiLock);
            xdi = g_xdiDesc;
        }
        static constexpr const char* kM[4] = { "none", "slots", "long", "hidden" };
        return std::format("open={} menuOpen={} looking={} buttons={} accept={} pending={} exclusive={} otherFlat='{}' opens={} ticks={} openTicks={} "
                           "lookingTicks={} notLookingTicks={} lookOffEdges={} buttonEdges={} | look={} patched={}{}{} calls={}/{}/{} forced={}/{}/{} | "
                           "weapon hide={} hidden={} culls={} recull={} restores={} missing={} | stick responses={} listScroll={} canPick={} canFlick={} "
                           "flicks up={} right={} down={} left={} scrollSteps={} last='{}' | engine picks={} ours={} lastType={} probe='{}' | "
                           "xdiDll={} xdiMode={} xdi='{}' | haptics={} | cfg {}",
            g.open ? 1 : 0, g.menuOpen ? 1 : 0, g.looking ? 1 : 0, g.buttons ? 1 : 0, g.accept ? 1 : 0, g.pending ? 1 : 0, g.flatExclusive ? 1 : 0,
            other, g.opens, g.ticks, g.openTicks, g.lookingTicks, g.notLookingTicks, g.lookOffEdges, g.buttonEdges, g_look.load() ? 1 : 0,
            g_patched[0] ? 1 : 0, g_patched[1] ? 1 : 0, g_patched[2] ? 1 : 0, g_calls[0].load(), g_calls[1].load(), g_calls[2].load(),
            g_forced[0].load(), g_forced[1].load(), g_forced[2].load(), g_hideWeapon.load() ? 1 : 0, g_weaponHidden.load() ? 1 : 0, g.weaponCulls,
            g.weaponRecull, g.weaponRestores, g.weaponMissing, g_stick.load() ? 1 : 0, g_listScroll.load() ? 1 : 0, g_canPick.load() ? 1 : 0,
            g_canFlick.load() ? 1 : 0, g.stickFlicks[1], g.stickFlicks[2], g.stickFlicks[3], g.stickFlicks[4], g.scrollSteps, stick,
            g_ombpCalls.load(), g_ombpOurs.load(), g_ombpLastType.load(), g_ombpInstall, g_xdiDll.load(), kM[std::clamp(g_xdiMode.load(), 0, 3)],
            xdi, g_haptics.load() ? 1 : 0, CfgString(c));
    }

    std::string GameCommand(const std::string& a_arg)
    {
        if (a_arg == "npcs") {
            // Test helper: actors in the player's cell (ref id, name, distance in units), nearest first.
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            const bool queued = tq::RunOnMain([done]() {  // main-idle queue (ThreadQueues.h): walks the cell's references
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
                    // 22 Nov: bearing (game heading convention: 0 = +Y, clockwise, degrees) and height difference, so a test can
                    // point a hand at the actor directly
                    const float brg = std::atan2(p.x - pp.x, p.y - pp.y) * 57.29578f;
                    v.emplace_back(d, std::format("{:08X}/{:08X} '{}' {:.0f} b={:.1f} dz={:.0f}", r->GetFormID(), base ? base->GetFormID() : 0,
                                          nm ? nm : "", d, brg, p.z - pp.z));
                }
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::string s = std::format("n={}", v.size());
                for (std::size_t i = 0; i < v.size() && i < 40; ++i) {
                    s += " | " + v[i].second;
                }
                done->set_value(std::move(s));
            });
            if (!queued) {
                return "error: no main-thread queue";
            }
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
        const Flag flags[] = { { "look", &g_look }, { "weapon", &g_hideWeapon }, { "haptics", &g_haptics }, { "stick", &g_stick },
            { "scroll", &g_listScroll } };
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
