// FO4VR default controller bindings -> virtual gamepad. See ControllerBindings.h and analysis/vr-controls.md.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/ControllerBindings.h"
#include "plugin/FrikBridgeApi.h"
#include "plugin/ConfigStore.h"
#include "plugin/ConfigUI.h"
#include "plugin/Dialogue.h"
#include "plugin/Haptics.h"
#include "plugin/GameBridge.h"
#include "plugin/GamePipboy.h"
#include "plugin/Gestures.h"
#include "plugin/Settings.h"
#include "plugin/ThreadQueues.h"
#include "plugin/UiSoundDiag.h"
#include "xr/XrRuntime.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <string>

namespace fo4flatvr::bindings
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        enum class Ctx
        {
            Gameplay,
            QuickLoot,  // gameplay while the HUD quick-container list of a lootable container is up (FO4VR quick menu)
            Pipboy,
            Favorites,
            Vats,
            Workshop,
            Lockpick,
            Dialogue,
            Menu
        };
        const char* CtxName(Ctx c)
        {
            switch (c) {
            case Ctx::Gameplay: return "gameplay";
            case Ctx::QuickLoot: return "quickloot";
            case Ctx::Pipboy: return "pipboy";
            case Ctx::Favorites: return "favorites";
            case Ctx::Vats: return "vats";
            case Ctx::Workshop: return "workshop";
            case Ctx::Lockpick: return "lockpick";
            case Ctx::Dialogue: return "dialogue";
            default: return "menu";
            }
        }

        // One physical input with press time (tap / hold logic).
        struct Btn
        {
            bool              down{ false };
            Clock::time_point t0{};
            bool              heldFired{ false };  // the hold action started
            bool Update(bool a_now, Clock::time_point a_t, bool& a_pressed, bool& a_released)
            {
                a_pressed = a_now && !down;
                a_released = !a_now && down;
                if (a_pressed) t0 = a_t, heldFired = false;
                down = a_now;
                return down;
            }
            double HeldMs(Clock::time_point a_t) const { return down ? std::chrono::duration<double, std::milli>(a_t - t0).count() : 0.0; }
        };

        struct Pulse  // a game button held for a fixed time (tap emulation)
        {
            Clock::time_point until{};
            void Fire(Clock::time_point a_t, int a_ms) { until = a_t + std::chrono::milliseconds(a_ms); }
            bool On(Clock::time_point a_t) const { return a_t < until; }
        };

        struct StickDir  // stick deflection as a button with hysteresis
        {
            bool on{ false };
            bool Update(float a_v, float a_other)
            {
                if (!on && a_v > 0.75f && a_v > std::abs(a_other)) on = true;
                else if (on && a_v < 0.5f) on = false;
                return on;
            }
        };

        // ---- quick container ("quick loot") detection ---------------------------------------------------------------
        // Flat 1.11.240: the HUD quick-container list follows QuickContainerStateEvent (BSTValueEvent<QuickContainerStateData>,
        // value at +0, has-value flag at +0xB8), broadcast by BSTGlobalEvent::EventSource<QuickContainerStateEvent> (singleton
        // pointer rva 3278E58 = NG id 4803314, created lazily by the HUD rollover model; its vtable rva 2532770; the
        // BSTEventSource sits at +0x10). PlayerControls::ProcessEvent(QuickContainerStateEvent) (DDA3A0) copies "has a value"
        // into every PlayerInputHandler::inQuickContainer (+0x18).
        // FO4VR 1.2.72 (PlayerControls sink FC0170) keeps one flag per wand (+0x18 / +0x19, data +0xC0 / +0xC1) and sets it only
        // in loot or steal mode (data +0xAC == 0 or 6, test at A91AA0); its player handlers then leave that wand's stick to the
        // list (help text HelpContainerVR_VW: R stick up/down select, Trigger take, B transfer menu). We use the same test.
        constexpr std::uintptr_t kQcSourcePtrRva = 0x3278E58;
        constexpr std::uintptr_t kQcSourceVtblRva = 0x2532770;

        struct QcInfo
        {
            std::atomic<bool>          has{ false };
            std::atomic<int>           mode{ -1 }, selected{ -1 }, items{ 0 };
            std::atomic<std::uint32_t> containerHandle{ 0 };
            std::atomic<std::uint64_t> events{ 0 };
        };
        QcInfo g_qc;

        class QcSink final : public RE::BSTEventSink<RE::QuickContainerStateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::QuickContainerStateEvent& a_event, RE::BSTEventSource<RE::QuickContainerStateEvent>*) override
            {
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&a_event);
                const bool  has = raw[0xB8] != 0;
                if (has) {
                    const auto& d = *reinterpret_cast<const RE::QuickContainerStateData*>(raw);
                    g_qc.mode = *reinterpret_cast<const std::int32_t*>(raw + 0xAC);
                    g_qc.selected = *reinterpret_cast<const std::int32_t*>(raw + 0xA8);
                    g_qc.items = static_cast<int>(d.itemData.size());
                    g_qc.containerHandle = *reinterpret_cast<const std::uint32_t*>(raw + 0x88);
                } else {
                    g_qc.mode = -1, g_qc.selected = -1, g_qc.items = 0, g_qc.containerHandle = 0;
                }
                g_qc.has = has;
                ++g_qc.events;
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        QcSink            g_qcSink;
        std::atomic<int>  g_qcReg{ 0 };  // 0 not yet, 1 registered, -1 gave up (layout check failed)
        Clock::time_point g_qcNextTry{};
        std::atomic<int>  g_quickLoot{ -1 };  // [Controllers] bQuickLootStick, -1 = not read yet

        bool QuickLootEnabled()
        {
            int e = g_quickLoot.load();
            if (e < 0) {
                e = ::GetPrivateProfileIntA("Controllers", "bQuickLootStick", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
                g_quickLoot = e;
                REX::INFO("bindings: quick-loot stick {} ([Controllers] bQuickLootStick)", e ? "on" : "off");
            }
            return e != 0;
        }

        // Register our sink on the game's QuickContainerStateEvent source once it exists (it is created with the HUD).
        void QcEnsureSink(Clock::time_point a_now)
        {
            if (g_qcReg.load() != 0 || a_now < g_qcNextTry) return;
            g_qcNextTry = a_now + std::chrono::seconds(1);
            const std::uintptr_t base = REL::Offset(0).address();
            const auto           src = *reinterpret_cast<std::uintptr_t*>(base + kQcSourcePtrRva);
            if (!src) return;
            const std::uintptr_t vt = *reinterpret_cast<std::uintptr_t*>(src);
            if (vt != base + kQcSourceVtblRva) {
                g_qcReg = -1;
                REX::WARN("bindings: QuickContainerStateEvent source vtable {:X} != expected {:X}: quick-loot detection falls back to "
                          "PlayerControls handler flag (no loot-mode filter)", vt - base, kQcSourceVtblRva);
                return;
            }
            reinterpret_cast<RE::BSTEventSource<RE::QuickContainerStateEvent>*>(src + 0x10)->RegisterSink(&g_qcSink);
            g_qcReg = 1;
            REX::INFO("bindings: quick-container sink registered (source {:X}, id 4803314 -> {:X})", src - base,
                REL::ID(4803314).address() - base);
        }

        // The engine's own "quick container is up" state: PlayerInputHandler::inQuickContainer of the movement handler.
        bool QcEngineFlag()
        {
            auto* pc = RE::PlayerControls::GetSingleton();
            auto* h = pc ? reinterpret_cast<RE::PlayerInputHandler*>(pc->movementHandler) : nullptr;
            return h && h->inQuickContainer;
        }

        // The quick-container list of a lootable container (loot / steal mode, as FO4VR) is up.
        bool QuickLootOpen()
        {
            if (!QuickLootEnabled() || !QcEngineFlag()) return false;
            if (g_qcReg.load() != 1) return g_qcReg.load() < 0;  // no event data: engine flag only (if the sink is impossible)
            const int m = g_qc.mode.load();
            return g_qc.has.load() && (m == 0 || m == 6);
        }

        // Stick up/down -> D-pad pulses for the list (QuickContainerUp/Down = gamepad D-pad up/down in the flat ControlMap;
        // HUDQuickContainerDataModel::HandleEvent A088E0 steps on release, so each step is a short press + release).
        struct QlNav
        {
            int               dir{ 0 };       // held direction: +1 up, -1 down, 0 neutral
            int               pulseDir{ 0 };  // direction of the current pulse
            Clock::time_point next{};
            Pulse             pulse;
            unsigned          up{ 0 }, down{ 0 };
            static constexpr float kOn = 0.6f, kOff = 0.35f;  // dead zone with hysteresis
            static constexpr int   kPressMs = 60, kFirstRepeatMs = 450, kRepeatMs = 170;
            void Update(bool a_active, float a_y, float a_x, Clock::time_point a_t)
            {
                int want = 0;
                if (a_active) {
                    if (dir == 0) want = (std::abs(a_y) > kOn && std::abs(a_y) > std::abs(a_x)) ? (a_y > 0 ? 1 : -1) : 0;
                    else want = dir * a_y > kOff ? dir : 0;
                }
                if (want != dir) {
                    dir = want;
                    if (dir) Fire(a_t), next = a_t + std::chrono::milliseconds(kFirstRepeatMs);
                } else if (dir && a_t >= next) {
                    Fire(a_t), next = a_t + std::chrono::milliseconds(kRepeatMs);
                }
                if (!a_active) pulseDir = 0;
            }
            void Fire(Clock::time_point a_t)
            {
                pulseDir = dir;
                pulse.Fire(a_t, kPressMs);
                ++(dir > 0 ? up : down);
            }
            bool Up(Clock::time_point a_t) const { return pulseDir > 0 && pulse.On(a_t); }
            bool Down(Clock::time_point a_t) const { return pulseDir < 0 && pulse.On(a_t); }
        };

        struct State
        {
            Btn      pGrip, oGrip, offUpper, pStickClick, pAct, oAct, pUpperBtn;
            // quick loot: list navigation, B -> transfer (pad X), stick latched per hand until it is back in the dead zone
            QlNav    ql;
            Pulse    transfer;
            bool     upperForTransfer{ false };
            bool     qlLatch[2]{};
            int      qlHand{ 0 };
            unsigned qlTransfers{ 0 }, qlEntries{ 0 };
            // per-hand activation: the hand whose activate button (A right / X left) went down last picks with its ray;
            // its press reaches the game once a pick update has used that ray (or after kActWaitMs)
            int               actHand{ 0 };          // 0 primary, 1 off
            bool              actPending{ false };
            std::uint64_t     actRuns0{ 0 };
            Clock::time_point actReleased{};
            unsigned          actByHand[2]{}, actWaitTimeouts{ 0 };
            double            actLastWaitMs{ 0 };
            Pulse    reload, pause, light, sneak, favorites;
            // 20 Nov (user: "Left trigger hold is pip boy light. This also has left grip hold as pip boy light"): the off
            // trigger opens the Pip-Boy with a pad B tap (PipboyHandler: B held >= 0.3 s = light), so only the off grip hold
            // (FO4VR) toggles the light
            Btn      oTrigBtn;
            Pulse    pipboyTap;
            unsigned pipboyTaps{ 0 }, lightHolds{ 0 }, lightDirect{ 0 };
            // 20 Nov: input carried across a context change (generalised "no tap/hold carried" of the buttons): a stick held
            // when the context changes drives nothing in the new one until it is back in the dead zone; the off trigger
            // (Pip-Boy open -> Pip-Boy select) likewise until released
            bool     stickCarry[2]{}, oTrigCarry{ false };
            unsigned stickCarries{ 0 }, trigCarries{ 0 };
            Btn      menuBtn;   // 20 Nov: left menu button, tap = Pause (on release), hold = VR settings panel (ConfigUI.h)
            Pulse    menuTap;
            unsigned menuHolds{ 0 }, menuTaps{ 0 };
            QlNav    dlgY, dlgX;  // EDI dialogue list: primary stick Y -> D-pad up/down, X -> D-pad right/left
            // 20 Nov workshop (FO4VR): X toggles the build menu (minimised = walk around, menu panel hidden), the off stick
            // steps the menu (D-pad pulses) while it is shown, L stick click held = settlement stats (panel shown, enlarged)
            QlNav             wsY, wsX;
            Btn               wsToggle;
            bool              wsMin{ false }, wsStats{ false }, wsGripCancel{ false }, wsYCarried{ false };
            Clock::time_point wsNextHold{};
            unsigned          wsToggles{ 0 }, wsHoldSteps{ 0 }, wsEnters{ 0 };
            bool     clickUsedForHolster{ false };
            bool     holster{ false };
            StickDir up, down;
            Ctx      ctx{ Ctx::Gameplay };
            pad::State last;
            std::string lastText;
            // rising-edge counters of the emitted pad inputs (verification)
            unsigned n[16]{};
            pad::State prev;
        };

        std::mutex        g_lock;
        State             g_s;
        std::atomic<int>  g_enabled{ -1 };  // -1 = not read yet

        // 20 Nov switches ([Controllers]): read once
        int IniOnce(std::atomic<int>& a_v, const char* a_section, const char* a_key, int a_def)
        {
            int e = a_v.load();
            if (e == INT_MIN) {
                e = static_cast<int>(::GetPrivateProfileIntA(a_section, a_key, a_def, Settings::Path().c_str()));
                a_v = e;
                REX::INFO("bindings: [{}] {}={}", a_section, a_key, e);
            }
            return e;
        }
        std::atomic<int> g_wsFO4VR{ INT_MIN }, g_glyphRemap{ INT_MIN }, g_wsUpFarther{ INT_MIN }, g_lightDirect{ INT_MIN };
        // 1 = the off grip hold toggles the Pip-Boy light through the engine (game::RequestTogglePipboyLight); 0 = pad B held
        // 1.2 s (PipboyHandler's hold), as before
        bool LightDirect() { return IniOnce(g_lightDirect, "Controllers", "bPipboyLightDirect", 1) != 0; }
        std::atomic<float> g_wsStatsScale{ -1.0f };
        // 1 = workshop mode the FO4VR way (hold Y enters through the engine, X minimises the menu, stick menu navigation ...)
        bool WorkshopFO4VR() { return IniOnce(g_wsFO4VR, "Controllers", "bWorkshopFO4VR", 1) != 0; }
        // 1 = every button glyph the game shows names the Touch button that does it under these bindings
        bool GlyphRemapOn() { return IniOnce(g_glyphRemap, "Controllers", "bGlyphRemap", 1) != 0; }
        // 1 = primary stick up pushes a held workshop object away (down pulls it closer); 0 = the reverse
        bool WorkshopUpFarther() { return IniOnce(g_wsUpFarther, "Controllers", "bWorkshopStickUpFarther", 1) != 0; }
        float WorkshopStatsScale()
        {
            float v = g_wsStatsScale.load();
            if (v < 0.0f) {
                char buf[32]{};
                ::GetPrivateProfileStringA("Menus", "fWorkshopStatsScale", "1.75", buf, sizeof(buf), Settings::Path().c_str());
                v = std::clamp(static_cast<float>(std::atof(buf)), 1.0f, 4.0f);
                g_wsStatsScale = v;
            }
            return v;
        }
        std::atomic<bool> g_wsPanelHidden{ false }, g_wsStatsShown{ false };

        bool Enabled()
        {
            int e = g_enabled.load();
            if (e < 0) {
                e = ::GetPrivateProfileIntA("Controllers", "bFO4VRBindings", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
                g_enabled = e;
                REX::INFO("bindings: FO4VR default controller bindings {}", e ? "on" : "off ([Controllers] bFO4VRBindings=0: generic Xbox layout)");
            }
            return e != 0;
        }

        // [Controllers] bDialogueFaceButtons (default 1): in DialogueMenu the Touch face buttons A/B/X/Y are sent as pad
        // A/B/X/Y (the four options as the menu labels them); 0 = the earlier mapping (A/X -> A, grip -> B).
        std::atomic<int> g_dlgFace{ -1 };
        bool DialogueFaceButtons()
        {
            int e = g_dlgFace.load();
            if (e < 0) {
                e = ::GetPrivateProfileIntA("Controllers", "bDialogueFaceButtons", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
                g_dlgFace = e;
                REX::INFO("bindings: dialogue face buttons {}", e ? "A/B/X/Y -> pad A/B/X/Y (the 4 options as labelled)"
                                                                   : "off ([Controllers] bDialogueFaceButtons=0: A/X -> A, grip -> B)");
            }
            return e != 0;
        }

        // [Controllers] bDialogueListNav (default 1), only while Extended Dialogue Interface (XDI.dll) is loaded: EDI
        // replaces DialogueMenu.swf with a scrolling option list. Its gamepad mode (DialogueMenu.ProcessUserEvent) moves
        // the highlight on QuickkeyUp/Down (pad D-pad up/down; QuickkeyLeft/Right step value entries) and picks the
        // highlighted option on MultiActivateA release; B/X/Y are not options there. See analysis/mod-edi-hudframework.md.
        std::atomic<int>  g_dlgListNav{ -1 };
        std::atomic<int>  g_edi{ -1 };  // XDI.dll loaded (-1 = not checked yet)
        std::atomic<bool> g_dlgListNavActive{ false };
        bool DialogueListNavOn()
        {
            int e = g_dlgListNav.load();
            if (e < 0) {
                e = ::GetPrivateProfileIntA("Controllers", "bDialogueListNav", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
                g_dlgListNav = e;
            }
            return e != 0;
        }
        bool EdiLoaded()
        {
            int e = g_edi.load();
            if (e < 0) {  // F4SE plugins are loaded before the first conversation; checked once, on the first one
                e = ::GetModuleHandleW(L"XDI.dll") ? 1 : 0;
                g_edi = e;
                REX::INFO("bindings: Extended Dialogue Interface (XDI.dll) {}; dialogue list navigation {}", e ? "loaded" : "not loaded",
                    !e ? "not used" : DialogueListNavOn() ? "on (primary stick -> D-pad steps, trigger off the panel = A, pointer targets DialogueMenu)"
                                                          : "off ([Controllers] bDialogueListNav=0)");
            }
            return e != 0;
        }

        bool MenuOpen(RE::UI* a_ui, const char* a_name) { return a_ui && a_ui->GetMenuOpen(a_name); }

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // ---- FO4VR workshop controls: engine calls (flat 1.11.240; analysis/vr-controls.md section 6) ---------------------
        // Hold Y: FO4VR feeds its Y hold into the same TogglePOVHandler as flat (both exes carry it and fEnterWorkshopDelay).
        // Flat's handler (DE0730) enters workshop mode only when the TogglePOV button itself is held fEnterWorkshopDelay
        // (1.5 s) from its own first event, otherwise its release toggles first / third person. Our old mapping pressed pad
        // Back only after a 600 ms Y hold, so a 1-2 s Y hold gave the game a < 1.5 s Back press = POV toggle (user report
        // 20 Nov). Now the hold starts the workshop directly, the way the handler does it once its timer passes: input
        // layers 0x100 and 1 enabled, player state vfunc (player+0x128)[0x24] == 0, FindNearbyWorkshop status 0, then
        // DialogueMenuUtils::CloseMenu + PlayerCharacter::StopDialogueCamera + Workshop::StartNearbyWorkshop (DE08A0..DE08E4).
        namespace eng
        {
            constexpr std::uintptr_t kFindNearbyWorkshop = 0x37D5C0;     // Workshop::FindNearbyWorkshop(status&) -> ref
            constexpr std::uintptr_t kStartNearbyWorkshop = 0x37D640;    // Workshop::StartNearbyWorkshop(status&) -> ref
            constexpr std::uintptr_t kDialogueCloseMenu = 0x1083C50;      // DialogueMenuUtils::CloseMenu()
            constexpr std::uintptr_t kStopDialogueCamera = 0xD5FD30;      // PlayerCharacter::StopDialogueCamera(player, 0, 0)
            constexpr std::uintptr_t kUserEventEnabled = 0x1668C20;       // BSInputEnableManager: user events enabled (mgr, flags)
            constexpr std::uintptr_t kInputEnableMgr = 0x30E88B0;         // BSInputEnableManager* singleton
            constexpr std::uintptr_t kPlayer = 0x32DD370;                 // PlayerCharacter* singleton
            constexpr std::uintptr_t kEnterWorkshopDelay = 0x2F35FB0;     // fEnterWorkshopDelay:Controls value (default 1.5)
            constexpr std::uintptr_t kHasActiveEditItem = 0x3973D0;       // Workshop::HasActiveEditItem() (globals only)
            constexpr std::uintptr_t kQNewItemMode = 0x3A4040;            // Workshop::QNewItemMode() (globals only)
            constexpr std::uintptr_t kContextData = 0x3897F0;             // Workshop::ContextData::ContextData(ctx, player)
            constexpr std::uintptr_t kUpdateItemHoldDistance = 0x396DA0;  // Workshop::UpdateItemHoldDistance(ctx, farther, scale)

            struct Check
            {
                std::uintptr_t rva;
                std::uint8_t   b[8];
            };
            constexpr Check kChecks[] = {
                { kFindNearbyWorkshop, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 } },
                { kStartNearbyWorkshop, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 } },
                { kDialogueCloseMenu, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 } },
                { kStopDialogueCamera, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 } },
                { kUserEventEnabled, { 0x40, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28 } },
                { kContextData, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 } },
                { kUpdateItemHoldDistance, { 0x48, 0x83, 0xEC, 0x38, 0xF3, 0x0F, 0x10, 0x0D } },
                { kHasActiveEditItem, { 0x8B, 0x05, 0xAA, 0xDE, 0xD4, 0x02, 0x39, 0x05 } },
                { kQNewItemMode, { 0x80, 0x3D, 0xF1, 0xAE, 0xB3, 0x02, 0x00, 0x0F } },
            };
            std::atomic<int> g_ok{ -1 };
            bool Ok()
            {
                int ok = g_ok.load();
                if (ok < 0) {
                    ok = 1;
                    for (const auto& c : kChecks) {
                        if (std::memcmp(reinterpret_cast<const void*>(Base() + c.rva), c.b, sizeof(c.b)) != 0) {
                            REX::WARN("bindings: workshop engine function {:X} bytes differ: FO4VR workshop calls off", c.rva);
                            ok = 0;
                        }
                    }
                    g_ok = ok;
                }
                return ok != 0;
            }
            void* Player() { return *reinterpret_cast<void**>(Base() + kPlayer); }
            float EnterWorkshopDelay()
            {
                const float v = *reinterpret_cast<const float*>(Base() + kEnterWorkshopDelay);
                return v > 0.05f && v < 10.0f ? v : 1.5f;
            }
            // presenter thread: pure reads of the workshop globals
            bool ItemHeld()
            {
                if (!Ok()) return false;
                using B_t = bool (*)();
                return reinterpret_cast<B_t>(Base() + kHasActiveEditItem)() || reinterpret_cast<B_t>(Base() + kQNewItemMode)();
            }

            std::atomic<std::uint64_t> g_enterReq{ 0 }, g_entered{ 0 }, g_holdReq{ 0 };
            std::atomic<int>           g_lastStatus{ -1 };  // StartWorkshopStatus of the last hold (0 ok, 1 none nearby, 2 locked, 3 blocked)
            std::string                g_lastWhy{ "-" };    // main thread writes, Command reads under g_lock (best effort)

            void RequestEnterWorkshop()
            {
                if (!Ok()) return;
                ++g_enterReq;
                tq::RunOnMain([]() {  // main-idle queue (ThreadQueues.h; requested from the Present thread)
                    void* pl = Player();
                    if (!pl) return;
                    const auto b = Base();
                    using En_t = bool (*)(void*, std::uint32_t);
                    void* mgr = *reinterpret_cast<void**>(b + kInputEnableMgr);
                    const bool inputs = mgr && reinterpret_cast<En_t>(b + kUserEventEnabled)(mgr, 0x100) &&
                                        reinterpret_cast<En_t>(b + kUserEventEnabled)(mgr, 1);
                    char st = 1;
                    using Find_t = void* (*)(char*);
                    void* ws = reinterpret_cast<Find_t>(b + kFindNearbyWorkshop)(&st);
                    g_lastStatus = ws ? st : 1;
                    auto* sub = reinterpret_cast<std::uint8_t*>(pl) + 0x128;
                    using V_t = int (*)(void*);
                    const int pst = reinterpret_cast<V_t>((*reinterpret_cast<std::uintptr_t**>(sub))[0x120 / 8])(sub);
                    if (!ws || st != 0 || !inputs || pst != 0) {
                        g_lastWhy = std::format("no workshop start: ref={} status={} inputs={} playerState={}", ws ? 1 : 0, static_cast<int>(st),
                            inputs ? 1 : 0, pst);
                        REX::INFO("bindings: hold Y: {}", g_lastWhy);
                        return;
                    }
                    reinterpret_cast<void (*)()>(b + kDialogueCloseMenu)();
                    reinterpret_cast<void (*)(void*, std::uint32_t, std::uint32_t)>(b + kStopDialogueCamera)(pl, 0, 0);
                    st = 1;
                    void* r = reinterpret_cast<Find_t>(b + kStartNearbyWorkshop)(&st);
                    ++g_entered;
                    g_lastWhy = std::format("StartNearbyWorkshop ref={} status={}", r ? 1 : 0, static_cast<int>(st));
                    REX::INFO("bindings: hold Y: {}", g_lastWhy);
                });
            }

            // R stick up / down while an item is held: flat's mouse-wheel path (WorkshopMenu::HandleEvent ZoomIn/ZoomOut ->
            // UpdateItemHoldDistance(ctx, zoomIn, 1.0)); flat has no gamepad input for it.
            void RequestHoldDistance(bool a_farther, float a_scale)
            {
                if (!Ok()) return;
                ++g_holdReq;
                tq::RunOnMain([a_farther, a_scale]() {  // main-idle queue (ThreadQueues.h)
                    void* pl = Player();
                    if (!pl || !RE::UI::GetSingleton() || !RE::UI::GetSingleton()->GetMenuOpen("WorkshopMenu")) return;
                    alignas(16) std::uint8_t ctx[0x40]{};
                    const auto b = Base();
                    reinterpret_cast<void* (*)(void*, void*)>(b + kContextData)(ctx, pl);
                    reinterpret_cast<void (*)(void*, bool, float)>(b + kUpdateItemHoldDistance)(ctx, a_farther, a_scale);
                });
            }
        }

        // 20 Nov: the Touch left menu button. With the VR settings panel on ([ConfigUI] bMenuHold, fMenuHoldS): held that long =
        // open / close the panel (haptic on the left hand), released before = Pause (a 120 ms Start pulse on release).
        // Without it: Start while held, as before. Lock held.
        Clock::time_point g_menuTestUntil{};  // pipe "bindings menu <ms>" (test aid), under g_lock

        bool MenuStart(State& s, bool a_menu, Clock::time_point a_now)
        {
            a_menu = a_menu || a_now < g_menuTestUntil;
            const float hold = configui::MenuHoldSeconds();
            bool        pr, rl;
            s.menuBtn.Update(a_menu, a_now, pr, rl);
            if (hold <= 0.0f) {
                return a_menu;
            }
            if (s.menuBtn.down && !s.menuBtn.heldFired && s.menuBtn.HeldMs(a_now) >= hold * 1000.0) {
                s.menuBtn.heldFired = true;
                ++s.menuHolds;
                configui::RequestToggle("left menu button held");
                haptics::Request(0, 0.5f, 0.05f);
            }
            if (rl && !s.menuBtn.heldFired) {
                s.menuTap.Fire(a_now, 120);
                ++s.menuTaps;
            }
            return s.menuTap.On(a_now);
        }

        Ctx Detect(const Context& a_ctx)
        {
            auto* ui = RE::UI::GetSingleton();
            if (a_ctx.pipboyWrist) return Ctx::Pipboy;
            if (MenuOpen(ui, "DialogueMenu")) return Ctx::Dialogue;
            if (MenuOpen(ui, "FavoritesMenu")) return Ctx::Favorites;
            if (MenuOpen(ui, "VATSMenu")) return Ctx::Vats;
            if (MenuOpen(ui, "LockpickingMenu")) return Ctx::Lockpick;
            if (MenuOpen(ui, "WorkshopMenu")) return Ctx::Workshop;
            if (!a_ctx.worldView) return Ctx::Menu;
            return QuickLootOpen() ? Ctx::QuickLoot : Ctx::Gameplay;
        }
        bool InWorld(Ctx c) { return c == Ctx::Gameplay || c == Ctx::QuickLoot; }

        // ---- 21 Nov, bridge v8 input claims (ROCK-flat phase 1; ControllerBindings.h SetClaims) --------------------------
        // A physics-hands client (ROCK.dll) claims inputs per physical hand. Applied only in gameplay / quick loot, to a copy
        // of the controller state at the top of Map(), so gestures, the pad mapping and the tap/hold timers all see the claim.
        // Two edge rules keep a claim from producing game input of its own:
        //  - onset while held: the input goes neutral without its release action (tap/hold state voided as on a context
        //    change: no reload tap from a grip, no pause tap from Y; a held stick-click gesture press is aborted);
        //  - end while held: the input stays masked until it is physically released (no fresh press = no grenade hold),
        //    also when the context changes to a menu while it is held (as the stale-stick rule of a context change).
        std::atomic<std::uint32_t> g_claimL[FO4FLATVR_CLAIM_OWNER_MAX + 1]{}, g_claimR[FO4FLATVR_CLAIM_OWNER_MAX + 1]{};
        std::atomic<int>           g_claimsOn{ INT_MIN };
        bool ClaimsOn() { return IniOnce(g_claimsOn, "ROCK", "bInputClaims", 1) != 0; }
        std::atomic<std::uint32_t> g_effMask[2]{};  // physical hand: bits masked by the last Map() (claimed or latched)
        std::atomic<std::uint64_t> g_claimSets{ 0 }, g_claimChanges{ 0 }, g_claimOnsetsHeld{ 0 }, g_claimLatchFrames{ 0 }, g_claimMaskedFrames{ 0 };
        std::uint32_t              g_claimLatched[2]{}, g_claimPrevEff[2]{};  // Presenter thread (Map)
        bool                       g_claimOnsetThisFrame = false;              // Presenter thread (Map)
        std::atomic<int>           g_claimLogged{ 0 };

        std::string ClaimBits(std::uint32_t a_m)
        {
            if (!a_m) return "-";
            std::string r;
            if (a_m & FO4FLATVR_CLAIM_GRIP) r += "grip ";
            if (a_m & FO4FLATVR_CLAIM_TRIGGER) r += "trigger ";
            if (a_m & FO4FLATVR_CLAIM_LOWER) r += "lower ";
            if (a_m & FO4FLATVR_CLAIM_UPPER) r += "upper ";
            if (a_m & FO4FLATVR_CLAIM_STICKCLICK) r += "click ";
            r.pop_back();
            return r;
        }

        void ClaimedMasks(std::uint32_t& a_l, std::uint32_t& a_r)
        {
            a_l = a_r = 0;
            if (!ClaimsOn()) return;
            for (std::uint32_t o = 1; o <= FO4FLATVR_CLAIM_OWNER_MAX; ++o) {
                a_l |= g_claimL[o].load(std::memory_order_relaxed);
                a_r |= g_claimR[o].load(std::memory_order_relaxed);
            }
        }

        // the claimed copy of a_c for this frame (a_c itself when nothing is masked); a_world = gameplay / quick loot
        const xr::ControllerState& ApplyClaims(const xr::ControllerState& a_c, bool a_world, xr::ControllerState& a_tmp)
        {
            std::uint32_t cl[2];
            ClaimedMasks(cl[0], cl[1]);
            if (!a_world) cl[0] = cl[1] = 0;
            g_claimOnsetThisFrame = false;
            std::uint32_t eff[2];
            for (int h = 0; h < 2; ++h) {
                const bool    left = h == 0;
                std::uint32_t down = 0;
                if ((left ? a_c.leftGrip : a_c.rightGrip) > 0.6f) down |= FO4FLATVR_CLAIM_GRIP;
                if ((left ? a_c.leftTrigger : a_c.rightTrigger) > 0.05f) down |= FO4FLATVR_CLAIM_TRIGGER;
                if (left ? a_c.x : a_c.a) down |= FO4FLATVR_CLAIM_LOWER;
                if (left ? a_c.y : a_c.b) down |= FO4FLATVR_CLAIM_UPPER;
                if (left ? a_c.leftStickClick : a_c.rightStickClick) down |= FO4FLATVR_CLAIM_STICKCLICK;
                if (!a_c.active) down = 0;
                g_claimLatched[h] = (g_claimLatched[h] | (cl[h] & down)) & down;  // held when claimed: masked until released
                eff[h] = cl[h] | g_claimLatched[h];
                const std::uint32_t onsetHeld = eff[h] & ~g_claimPrevEff[h] & down;
                if (onsetHeld) {
                    g_claimOnsetThisFrame = true;
                    ++g_claimOnsetsHeld;
                    if (onsetHeld & FO4FLATVR_CLAIM_STICKCLICK) gestures::AbortPress(h, "stick click claimed");
                }
                if (g_claimLatched[h] & ~cl[h]) ++g_claimLatchFrames;
                g_claimPrevEff[h] = eff[h];
                g_effMask[h].store(eff[h], std::memory_order_relaxed);
            }
            if (!eff[0] && !eff[1]) return a_c;
            ++g_claimMaskedFrames;
            a_tmp = a_c;
            for (int h = 0; h < 2; ++h) {
                const bool left = h == 0;
                const auto m = eff[h];
                if (m & FO4FLATVR_CLAIM_GRIP) (left ? a_tmp.leftGrip : a_tmp.rightGrip) = 0.0f;
                if (m & FO4FLATVR_CLAIM_TRIGGER) (left ? a_tmp.leftTrigger : a_tmp.rightTrigger) = 0.0f;
                if (m & FO4FLATVR_CLAIM_LOWER) (left ? a_tmp.x : a_tmp.a) = false;
                if (m & FO4FLATVR_CLAIM_UPPER) (left ? a_tmp.y : a_tmp.b) = false;
                if (m & FO4FLATVR_CLAIM_STICKCLICK) (left ? a_tmp.leftStickClick : a_tmp.rightStickClick) = false;
            }
            return a_tmp;
        }

        std::string ClaimsText()
        {
            std::string owners;
            for (std::uint32_t o = 1; o <= FO4FLATVR_CLAIM_OWNER_MAX; ++o) {
                const auto l = g_claimL[o].load(), r = g_claimR[o].load();
                if (l || r) owners += std::format(" owner{}{}: L[{}] R[{}]", o, o == FO4FLATVR_CLAIM_OWNER_ROCK ? "(ROCK)" : "", ClaimBits(l), ClaimBits(r));
            }
            return std::format("claims on={} masked now L[{}] R[{}] |{} | sets={} changes={} onsetsWhileHeld={} latchFrames={} maskedFrames={}",
                ClaimsOn() ? 1 : 0, ClaimBits(g_effMask[0].load()), ClaimBits(g_effMask[1].load()), owners.empty() ? " none" : owners,
                g_claimSets.load(), g_claimChanges.load(), g_claimOnsetsHeld.load(), g_claimLatchFrames.load(), g_claimMaskedFrames.load());
        }

        // ---- glyphs (20 Nov, user: "the glyphs say the wrong thing") ----------------------------------------------------
        // Flat shows gamepad glyphs from button NAMES ("Xenon_A" ... ; name -> character of the "$Controller_Buttons" font):
        //  (1) menus: each menu's AS3 BSButtonHintData carries a hard-coded XenonButton name; flat 1.11 draws every menu's
        //      hints in the shared ButtonBarMenu through ButtonHintDataWithClone::CreateClone (A50840), which builds the
        //      clone's BSButtonHintData from the source's cached strings (+0x48 XenonButton, +0x60 secondary). Hook: the call at
        //      A504CC; the names are swapped for the call only.
        //  (2) the engine's user-event lookups: ControlMap::GetButtonNameFromUserEvent (166FCF0) for the gamepad (device 2) -
        //      HUD rollover / quick-container prompts (UIUtils::GetAllButtonsForUserEvent B71E84), every "[UserEvent]" token in
        //      HUD messages, prompts and tutorial / message texts (UIUtils::GetButtonHTMLFromUserEvent B71C52, the
        //      AS3 callbacks of HUDMenu / PromptMenu, MESG GetConvertedDescription), GetButtonFromUserEvent (B71EE5/B71F06),
        //      B72213/B72238. Hook: those call sites; the returned name is replaced.
        // The replacement names the Touch control that sends that pad button in the current context (reverse of Map below):
        // Touch A/B/X/Y keep their letters, trigger -> RT/LT glyph, grip -> RB/LB, stick -> LS/RS, stick click -> L3/R3
        // (right-handed; left-handed mirrors). FO4VR's own Touch art ("Controller VR" font in its fonts_en.swf) is not
        // used: the menu hints are drawn by flat AS3 in the "$Controller_Buttons" font, see vr-controls.md section 7.
        enum class Role
        {
            None,
            PLower, PUpper, OLower, OUpper, PTrig, OTrig, PGrip, OGrip, PStick, OStick, PClick, OClick, Menu
        };
        enum class GCtx
        {
            Gameplay, QuickLoot, Pipboy, Favorites, Vats, Workshop, Lockpick, Dialogue, Menu, Workbench, LevelUp
        };
        std::atomic<int>  g_lastCtx{ 0 };      // Ctx of the last Map()
        std::atomic<bool> g_pipWrist{ false };  // the last Map() saw the wrist Pip-Boy
        std::atomic<bool> g_leftHanded{ false };
        std::atomic<std::uint64_t> g_glyphNames{ 0 }, g_glyphSwaps{ 0 }, g_glyphClones{ 0 }, g_glyphCloneSwaps{ 0 };
        std::mutex  g_glyphLogLock;
        std::string g_glyphLast{ "-" };
        int         g_glyphLogged{ 0 };

        bool IsWorkbenchMenu(RE::UI* a_ui)
        {
            return MenuOpen(a_ui, "ExamineMenu") || MenuOpen(a_ui, "CookingMenu") || MenuOpen(a_ui, "PowerArmorModMenu") ||
                   MenuOpen(a_ui, "RobotModMenu");
        }

        GCtx GlyphCtx()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return GCtx::Gameplay;
            if (MenuOpen(ui, "DialogueMenu")) return GCtx::Dialogue;
            if (MenuOpen(ui, "FavoritesMenu")) return GCtx::Favorites;
            if (MenuOpen(ui, "VATSMenu")) return GCtx::Vats;
            if (MenuOpen(ui, "LockpickingMenu")) return GCtx::Lockpick;
            if (MenuOpen(ui, "WorkshopMenu")) return GCtx::Workshop;
            if (MenuOpen(ui, "LevelUpMenu")) return GCtx::LevelUp;
            if (IsWorkbenchMenu(ui)) return GCtx::Workbench;
            if (MenuOpen(ui, "PipboyMenu")) return g_pipWrist.load() ? GCtx::Pipboy : GCtx::Menu;
            static constexpr const char* kMenus[] = { "ContainerMenu", "BarterMenu", "TerminalMenu", "PauseMenu", "BookMenu",
                "SleepWaitMenu", "LooksMenu", "SPECIALMenu", "Workshop_CaravanMenu", "ExamineConfirmMenu", "TerminalHolotapeMenu",
                "PipboyHolotapeMenu", "CreditsMenu", "MainMenu" };
            for (const char* m : kMenus)
                if (MenuOpen(ui, m)) return GCtx::Menu;
            if (static_cast<Ctx>(g_lastCtx.load()) == Ctx::Menu) return GCtx::Menu;
            return QuickLootOpen() ? GCtx::QuickLoot : GCtx::Gameplay;
        }

        const char* GCtxName(GCtx c)
        {
            static constexpr const char* k[] = { "gameplay", "quickloot", "pipboy", "favorites", "vats", "workshop", "lockpick",
                "dialogue", "menu", "workbench", "levelup" };
            return k[static_cast<int>(c)];
        }

        // pad button name -> the Touch control (hand role) that sends it in context c (mirrors Map()).
        Role RoleFor(GCtx c, std::string_view n)
        {
            const bool dpad = n.starts_with("DPad_");
            switch (c) {
            case GCtx::Gameplay:
            case GCtx::QuickLoot:
                if (n == "Xenon_A") return Role::PLower;
                if (n == "Xenon_B") return Role::OTrig;                                         // Pip-Boy
                if (n == "Xenon_X") return c == GCtx::QuickLoot ? Role::PUpper : Role::PGrip;  // transfer / reload, holster
                if (n == "Xenon_Y" || n == "Xenon_R3" || n == "Xenon_RS" || dpad) return Role::PStick;  // jump, sneak, look, lists
                if (n == "Xenon_L1") return Role::PUpper;                                       // V.A.T.S.
                if (n == "Xenon_R1") return Role::PGrip;                                        // grenade / bash
                if (n == "Xenon_R2") return Role::PTrig;
                if (n == "Xenon_L3") return Role::OClick;                                       // sprint
                if (n == "Xenon_LS") return Role::OStick;
                if (n == "Xenon_Select" || n == "Xenon_Start") return Role::OUpper;            // hold Y workshop / tap pause
                return Role::None;
            case GCtx::Pipboy:
                // STAT page: the off trigger is the perk chart (pad Y, FO4VR "L trigger: perk chart on the status screen");
                // select there = the primary trigger (pointer, or off the screen -> A)
                if (game::PipboyPage() == 0) {
                    if (n == "Xenon_Y") return Role::OTrig;
                    if (n == "Xenon_A") return Role::PTrig;
                }
                if (n == "Xenon_A") return Role::OTrig;
                if (n == "Xenon_B") return Role::OGrip;
                if (n == "Xenon_L2") return Role::OLower;
                if (n == "Xenon_R2") return Role::OUpper;
                if (n == "Xenon_LS" || dpad) return Role::OStick;
                if (n == "Xenon_RS") return Role::PStick;
                return Role::None;
            case GCtx::Favorites:
                if (n == "Xenon_A") return Role::PTrig;
                if (n == "Xenon_B") return Role::PGrip;
                if (n == "Xenon_LS" || dpad) return Role::PStick;
                return Role::None;
            case GCtx::Vats:
                if (n == "Xenon_R2") return Role::PTrig;
                if (n == "Xenon_L1") return Role::PUpper;
                if (n == "Xenon_X") return Role::PLower;
                if (n == "Xenon_A") return Role::OTrig;
                if (n == "Xenon_B") return Role::PGrip;
                if (n == "Xenon_LS") return Role::PStick;  // body part
                if (n == "Xenon_RS") return Role::OStick;  // target (20 Nov)
                return Role::None;
            case GCtx::Workshop:
                if (!WorkshopFO4VR()) return Role::None;
                if (n == "Xenon_A") return Role::PTrig;    // build / select / place
                if (n == "Xenon_X") return Role::PLower;   // scrap (A)
                if (n == "Xenon_B") return Role::PUpper;   // store / back (B)
                if (n == "Xenon_Y") return Role::PGrip;    // wire (R grip)
                if (n == "Xenon_Select") return Role::OUpper;  // exit (Y)
                if (n == "Xenon_R1") return Role::PClick;  // tag for search / supply line (hold R stick click)
                if (n == "Xenon_L2" || n == "Xenon_R2" || n == "Xenon_RS") return Role::PStick;  // rotate
                if (n == "Xenon_LS" || dpad) return Role::OStick;  // menu navigation / walk
                if (n == "Xenon_L3") return Role::OClick;
                return Role::None;
            case GCtx::Lockpick:
                if (n == "Xenon_B") return Role::PGrip;
                if (n == "Xenon_LS") return Role::OStick;
                if (n == "Xenon_RS") return Role::PStick;
                return Role::None;
            case GCtx::Dialogue: return Role::None;  // the face buttons are sent by label (bDialogueFaceButtons)
            case GCtx::Menu:
            case GCtx::Workbench:
            case GCtx::LevelUp:
                if (c == GCtx::Menu && n == "Xenon_Y" && game::PipboyPage() == 0) return Role::OTrig;  // flat-window Pip-Boy STAT: perk chart
                if (n == "Xenon_A") return Role::PTrig;  // select / take (the pointer's trigger)
                if (n == "Xenon_B") return c == GCtx::LevelUp ? Role::PGrip : Role::PUpper;
                if (n == "Xenon_X") return Role::OLower;
                if (n == "Xenon_Y") return c == GCtx::Workbench ? Role::OUpper : Role::PLower;  // scrap (Y) / sort (A)
                if (n == "Xenon_Start") return Role::OUpper;
                if (c == GCtx::LevelUp && n == "Xenon_R1") return Role::PUpper;  // next rank (B)
                if (c == GCtx::LevelUp && n == "Xenon_L1") return Role::OUpper;  // previous rank (Y)
                if (n == "Xenon_LS" || n == "Xenon_RS" || dpad) return Role::PStick;
                return Role::None;
            }
            return Role::None;
        }

        // user events whose button differs from the plain pad mapping (quick-container prompts)
        Role RoleForEvent(GCtx c, std::string_view ev)
        {
            if (ev == "QCOpenTransferMenu") return Role::PUpper;  // B (FO4VR quick menu)
            if (ev == "QCTakeItem") return Role::PLower;
            if (ev == "QuickContainerUp" || ev == "QuickContainerDown") return Role::PStick;
            (void)c;
            return Role::None;
        }

        const char* RoleGlyph(Role r, bool a_left)
        {
            switch (r) {
            case Role::PLower: return a_left ? "Xenon_X" : "Xenon_A";
            case Role::PUpper: return a_left ? "Xenon_Y" : "Xenon_B";
            case Role::OLower: return a_left ? "Xenon_A" : "Xenon_X";
            case Role::OUpper: return a_left ? "Xenon_B" : "Xenon_Y";
            case Role::PTrig: return a_left ? "Xenon_L2" : "Xenon_R2";
            case Role::OTrig: return a_left ? "Xenon_R2" : "Xenon_L2";
            case Role::PGrip: return a_left ? "Xenon_L1" : "Xenon_R1";
            case Role::OGrip: return a_left ? "Xenon_R1" : "Xenon_L1";
            case Role::PStick: return a_left ? "Xenon_LS" : "Xenon_RS";
            case Role::OStick: return a_left ? "Xenon_RS" : "Xenon_LS";
            case Role::PClick: return a_left ? "Xenon_L3" : "Xenon_R3";
            case Role::OClick: return a_left ? "Xenon_R3" : "Xenon_L3";
            case Role::Menu: return "Xenon_Start";
            default: return nullptr;
            }
        }

        // 20 Nov (user: "Looking at someone you can talk to shows RT to talk ... A is the talk button"): user events that exist
        // only in the main gameplay input context (flat embedded ControlMap; the quick-container aliases SpecialActivate /
        // SecondaryActivate / QuickContainerPerk = !Activate / !ReadyWeapon / !Jump) always name their gameplay button.
        // The lookups pass context -1 (any; UIUtils B71E50..), and the HUD caches its rollover prompts ("Talk", "Open",
        // "Take", "Sit" ...) from a lookup made while loading (GlyphCtx = menu: A -> RT, Jump Y -> A in the 23:05 log), so
        // resolving them by the current menu context showed the menu's buttons on gameplay prompts. Jump is also a Workshop
        // context event (wire) and keeps the workshop mapping there.
        bool GameplayEvent(std::string_view ev, GCtx c)
        {
            static constexpr std::string_view k[] = { "Activate", "SpecialActivate", "SecondaryActivate", "ReadyWeapon", "Pipboy", "PrimaryAttack",
                "SecondaryAttack", "Melee", "TogglePOV", "Sprint", "VATS", "Sneak", "QuickContainerPerk", "Pause" };
            if (ev == "Jump") return c != GCtx::Workshop;
            for (const auto& e : k)
                if (ev == e) return true;
            return false;
        }

        // bridge v8: the role's physical control is masked now (claim or release latch) and the glyph is for a gameplay action
        bool RoleClaimed(Role a_r, GCtx a_c, std::string_view a_event)
        {
            const bool gameplay = a_c == GCtx::Gameplay || a_c == GCtx::QuickLoot || (!a_event.empty() && GameplayEvent(a_event, a_c));
            if (!gameplay) return false;
            const bool    L = g_leftHanded.load();
            int           hand = -1;  // physical
            std::uint32_t bit = 0;
            switch (a_r) {
            case Role::PLower: hand = L ? 0 : 1, bit = FO4FLATVR_CLAIM_LOWER; break;
            case Role::PUpper: hand = L ? 0 : 1, bit = FO4FLATVR_CLAIM_UPPER; break;
            case Role::PTrig: hand = L ? 0 : 1, bit = FO4FLATVR_CLAIM_TRIGGER; break;
            case Role::PGrip: hand = L ? 0 : 1, bit = FO4FLATVR_CLAIM_GRIP; break;
            case Role::PClick: hand = L ? 0 : 1, bit = FO4FLATVR_CLAIM_STICKCLICK; break;
            case Role::OLower: hand = L ? 1 : 0, bit = FO4FLATVR_CLAIM_LOWER; break;
            case Role::OUpper: hand = L ? 1 : 0, bit = FO4FLATVR_CLAIM_UPPER; break;
            case Role::OTrig: hand = L ? 1 : 0, bit = FO4FLATVR_CLAIM_TRIGGER; break;
            case Role::OGrip: hand = L ? 1 : 0, bit = FO4FLATVR_CLAIM_GRIP; break;
            case Role::OClick: hand = L ? 1 : 0, bit = FO4FLATVR_CLAIM_STICKCLICK; break;
            default: return false;
            }
            std::uint32_t cl[2];
            ClaimedMasks(cl[0], cl[1]);
            return ((cl[hand] | g_effMask[hand].load(std::memory_order_relaxed)) & bit) != 0;
        }

        // a_name: the game's gamepad button name; a_event: the user event (or empty). nullptr = keep.
        const char* RemapGlyph(std::string_view a_name, std::string_view a_event, const char* a_path)
        {
            if (!GlyphRemapOn() || !Enabled() || a_name.empty()) return nullptr;
            const GCtx c = GlyphCtx();
            Role       r = a_event.empty() ? Role::None : RoleForEvent(c, a_event);
            if (r == Role::None && !a_event.empty() && GameplayEvent(a_event, c)) {
                r = RoleFor(GCtx::Gameplay, a_name);
                if (r == Role::None) return nullptr;  // no Touch control sends it in gameplay (e.g. LT aim): keep
            }
            if (r == Role::None) r = RoleFor(c, a_name);
            if (r != Role::None && RoleClaimed(r, c, a_event)) return nullptr;  // bridge v8: claimed in gameplay - not named
            const char* g = RoleGlyph(r, g_leftHanded.load());
            if (!g || a_name == g) return nullptr;
            std::scoped_lock l(g_glyphLogLock);
            g_glyphLast = std::format("{} {} {}{}{} -> {}", a_path, GCtxName(c), a_event, a_event.empty() ? "" : ":", a_name, g);
            if (g_glyphLogged < 60) {
                ++g_glyphLogged;
                REX::INFO("glyphs: {}", g_glyphLast);
            }
            return g;
        }

        // hook (2): ControlMap::GetButtonNameFromUserEvent(controlMap, userEvent, device, context, out) call sites
        using GetBtnName_t = bool (*)(void*, const RE::BSFixedString&, std::int32_t, std::uint32_t, RE::BSFixedStringCS&);
        GetBtnName_t g_origGetBtnName = nullptr;
        bool HkGetBtnName(void* a_map, const RE::BSFixedString& a_event, std::int32_t a_device, std::uint32_t a_context, RE::BSFixedStringCS& a_out)
        {
            const bool r = g_origGetBtnName(a_map, a_event, a_device, a_context, a_out);
            if (r && a_device == 2) {
                ++g_glyphNames;
                const char* ev = a_event.c_str();
                const char* nm = a_out.c_str();
                if (const char* g = RemapGlyph(nm ? nm : "", ev ? ev : "", "event")) {
                    a_out = g;
                    ++g_glyphSwaps;
                }
            }
            return r;
        }

        // hook (1): ButtonHintDataWithClone::CreateClone(source, out, movie); source +0x48 / +0x60 = Xenon / secondary Xenon
        using CreateClone_t = void* (*)(void*, void*, void*);
        CreateClone_t g_origCreateClone = nullptr;
        void* HkCreateClone(void* a_src, void* a_out, void* a_movie)
        {
            ++g_glyphClones;
            auto* xen = reinterpret_cast<RE::BSFixedStringCS*>(reinterpret_cast<std::uint8_t*>(a_src) + 0x48);
            auto* xen2 = reinterpret_cast<RE::BSFixedStringCS*>(reinterpret_cast<std::uint8_t*>(a_src) + 0x60);
            const RE::BSFixedStringCS keep1 = *xen, keep2 = *xen2;
            bool swapped = false;
            if (const char* g = RemapGlyph(keep1.c_str() ? keep1.c_str() : "", "", "hint")) *xen = g, swapped = true;
            if (const char* g = RemapGlyph(keep2.c_str() ? keep2.c_str() : "", "", "hint2")) *xen2 = g, swapped = true;
            void* r = g_origCreateClone(a_src, a_out, a_movie);
            if (swapped) {
                *xen = keep1, *xen2 = keep2;
                ++g_glyphCloneSwaps;
            }
            return r;
        }

        REL::Trampoline g_glyphTramp{ "fo4flatvr glyphs" };
        std::string     g_glyphInstall{ "not installed" };

        // The previous generic layout (bFO4VRBindings=0), unchanged from Presenter.cpp before 14 Nov.
        void MapGeneric(const xr::ControllerState& c, const Context& a, pad::State& p)
        {
            p.lx = c.leftStickX;
            p.ly = c.leftStickY;
            p.rx = c.rightStickX;
            p.ry = 0.0f;
            p.lt = (a.menuPointerOnQuad && a.menuPointerLeftHand) ? 0.0f : c.leftTrigger;
            p.rt = (a.pipPointerOnQuad || (a.menuPointerOnQuad && !a.menuPointerLeftHand)) ? 0.0f : c.rightTrigger;
            p.lb = c.leftGrip > 0.6f;
            p.rb = c.rightGrip > 0.6f;
            p.a = c.a;
            p.b = c.b;
            p.x = c.x;
            p.y = c.y;
            p.ls = c.leftStickClick;
            p.rs = c.rightStickClick;
            p.start = c.menu;
        }
    }

    void Map(const xr::ControllerState& a_raw, const Context& a, pad::State& p)
    {
        // VR gestures (Gestures.h, 16 Nov): stick click + hand motion / both hands up, before the sticks are mapped
        const Ctx  ctx0 = Enabled() ? Detect(a) : Ctx::Gameplay;
        // 21 Nov, bridge v8: inputs claimed by a physics-hands client (ROCK-flat) read as released from here on, in gameplay /
        // quick loot only (the generic layout: its world view without a menu)
        xr::ControllerState        claimedCopy;
        const xr::ControllerState& c = ApplyClaims(a_raw, InWorld(Enabled() ? ctx0 : Detect(a)), claimedCopy);
        const bool gestureWorld = Enabled() ? InWorld(ctx0) : a.worldView;
        // 20 Nov (user: "Gestures shouldn't be locked out in a menu or in any scenario. It's not like they're triggerable
        // accidentally"): gestures run in every context - menus, Pip-Boy, dialogue, VATS, workshop, any camera view and
        // policy. Only the camera actions (first/third person, view cycle) wait while a load / the main menu is up
        // (Gestures.cpp: no player camera to change there). [Gestures] bAllViews=0 = first-person gameplay only.
        // workshop: the primary stick click is the game's too (R stick click held still = tag / supply line), so it gets the
        // same VRIK button delay as the off hand's sprint click
        gestures::Update({ &c, a.head, gestureWorld, a.camState, a.leftHanded, true,
            Enabled() && ctx0 == Ctx::Workshop && WorkshopFO4VR() });
        if (!Enabled()) {
            MapGeneric(c, a, p);
            std::scoped_lock l(g_lock);
            p.start = MenuStart(g_s, c.menu, Clock::now());  // 20 Nov: tap = Pause, hold = VR settings panel
            return;
        }
        const auto now = Clock::now();
        const bool L = a.leftHanded;
        // roles: primary (weapon) hand P, off hand O; lower face button (A/X) and upper (B/Y) per hand
        const float pTrig = L ? c.leftTrigger : c.rightTrigger, oTrig = L ? c.rightTrigger : c.leftTrigger;
        const float pGripV = L ? c.leftGrip : c.rightGrip, oGripV = L ? c.rightGrip : c.leftGrip;
        float       psx = L ? c.leftStickX : c.rightStickX, psy = L ? c.leftStickY : c.rightStickY;
        float       osx = L ? c.rightStickX : c.leftStickX, osy = L ? c.rightStickY : c.leftStickY;
        const bool  psClick = L ? c.leftStickClick : c.rightStickClick;
        bool        osClick = L ? c.rightStickClick : c.leftStickClick;
        const bool  osClickRaw = osClick;
        const bool  pLower = L ? c.x : c.a, pUpper = L ? c.y : c.b;  // A, B (right-handed)
        const bool  oLower = L ? c.a : c.x, oUpper = L ? c.b : c.y;  // X, Y (right-handed)
        const bool  trigOn = pTrig > 0.6f, oTrigOn = oTrig > 0.6f;

        std::scoped_lock l(g_lock);
        State&     s = g_s;
        QcEnsureSink(now);
        const Ctx  ctx = Detect(a);
        g_lastCtx = static_cast<int>(ctx), g_pipWrist = a.pipboyWrist, g_leftHanded = L;
        uisound::SetContextName(CtxName(ctx));
        const int pPhys = L ? 0 : 1, oPhys = 1 - pPhys;
        {
            // gestures: a gesture stick's own jobs (turn / jump / sneak, move, menu navigation) are held while its click is
            // down and until it is back in the dead zone, in every context (20 Nov); the off hand's click (sprint) reaches the
            // game only after the VRIK button delay
            if (gestures::StickHeld(pPhys)) psx = psy = 0.0f;
            if (gestures::StickHeld(oPhys)) osx = osy = 0.0f;
            if (InWorld(ctx)) osClick = gestures::ClickToGame(oPhys, osClick);
        }
        const bool ctxChanged = ctx != s.ctx && !(InWorld(ctx) && InWorld(s.ctx));  // gameplay <-> quick loot keeps tap/hold state
        if (ctx == Ctx::QuickLoot && s.ctx != Ctx::QuickLoot) ++s.qlEntries;
        s.ctx = ctx;
        bool pr, rl;
        s.pGrip.Update(pGripV > 0.6f, now, pr, rl);
        const bool pGripPressed = pr, pGripReleased = rl;
        s.oGrip.Update(oGripV > 0.6f, now, pr, rl);
        const bool oGripPressed = pr;
        s.offUpper.Update(oUpper, now, pr, rl);
        const bool offUpperReleased = rl;
        s.pStickClick.Update(psClick, now, pr, rl);
        const bool pClickPressed = pr, pClickReleased = rl;
        if (ctxChanged) {
            s.pGrip.heldFired = s.oGrip.heldFired = s.offUpper.heldFired = true;  // no tap/hold carried across a context change
            s.holster = false;
            // 20 Nov: nor a stick / the off trigger. Each context routes the sticks differently (V.A.T.S.: primary stick = pad
            // LS; gameplay: off stick = LS ...), so a stick held over the change would jump straight into the new context's
            // job (turn / walk / scroll / part select) and the game's handlers saw the switch as one stick event.
            s.stickCarry[0] = std::hypot(psx, psy) > 0.35f;
            s.stickCarry[1] = std::hypot(osx, osy) > 0.35f;
            s.oTrigCarry = oTrigOn;
            s.stickCarries += (s.stickCarry[0] ? 1u : 0u) + (s.stickCarry[1] ? 1u : 0u);
            s.trigCarries += s.oTrigCarry ? 1u : 0u;
        }
        if (g_claimOnsetThisFrame && !ctxChanged) {
            // bridge v8: a held input was claimed this frame - its release is not a tap (reload / pause) and no hold carries on
            s.pGrip.heldFired = s.oGrip.heldFired = s.offUpper.heldFired = true;
            s.holster = false;
        }
        if (s.stickCarry[0] && std::hypot(psx, psy) < 0.35f) s.stickCarry[0] = false;
        if (s.stickCarry[1] && std::hypot(osx, osy) < 0.35f) s.stickCarry[1] = false;
        if (s.stickCarry[0]) psx = psy = 0.0f;
        if (s.stickCarry[1]) osx = osy = 0.0f;
        if (!oTrigOn) s.oTrigCarry = false;
        const bool oTrigUse = oTrigOn && !s.oTrigCarry;
        // Touch left menu button (FO4VR: SteamVR dashboard) -> pause; 20 Nov: on release, a 1 s hold opens the VR settings panel
        p.start = MenuStart(s, c.menu, now);
        const bool listNav = ctx == Ctx::Dialogue && DialogueListNavOn() && EdiLoaded();
        if (!listNav) {
            s.dlgY.Update(false, 0.0f, 0.0f, now);
            s.dlgX.Update(false, 0.0f, 0.0f, now);
        }
        if (listNav != g_dlgListNavActive.load()) {
            g_dlgListNavActive = listNav;
            REX::INFO("bindings: EDI dialogue list navigation {}", listNav ? "active" : "inactive");
        }

        switch (ctx) {
        case Ctx::Gameplay:
        case Ctx::QuickLoot: {
            // quick loot (FO4VR quick menu): the stick of the hand whose ray picks the container (s.actHand: primary, or the
            // off hand after X) steps through the list with D-pad pulses; that stick's Y axis neither jumps / sneaks
            // (primary) nor moves (off) until it is back in the dead zone after the list closes.
            const bool ql = ctx == Ctx::QuickLoot;
            if (ql) s.qlHand = s.actHand;
            {
                const float hy[2] = { psy, osy };
                for (int h = 0; h < 2; ++h) {
                    if (ql && s.qlHand == h) s.qlLatch[h] = true;
                    else if (s.qlLatch[h] && std::abs(hy[h]) < QlNav::kOff) s.qlLatch[h] = false;
                }
                s.ql.Update(ql, s.qlHand == 1 ? osy : psy, s.qlHand == 1 ? osx : psx, now);
            }
            p.lx = osx, p.ly = s.qlLatch[1] ? 0.0f : osy;  // move (comfort applies the direction mode afterwards)
            p.ls = osClick;          // sprint (direct movement)
            p.rx = psx;              // turn (comfort: snap/smooth)
            p.ry = 0.0f;
            const float jumpY = s.qlLatch[0] ? 0.0f : psy;
            const bool up = s.up.Update(jumpY, psx), down = s.down.Update(-jumpY, psx);
            p.y = up;                                       // jump
            if (down && !s.sneak.On(now) && s.sneak.until < now - std::chrono::milliseconds(300)) s.sneak.Fire(now, 120);
            p.rs = s.sneak.On(now);                         // toggle sneak (R3)
            p.rt = trigOn ? 1.0f : pTrig;                   // fire
            // Pip-Boy: a pad B tap at the off trigger's press. Before 20 Nov B followed the trigger, and PipboyHandler
            // (1077010) toggles the light once B is held 0.3 s, so a held trigger was a second light binding.
            {
                bool tpr, trl;
                s.oTrigBtn.Update(oTrigUse, now, tpr, trl);
                if (tpr) s.pipboyTap.Fire(now, 120), ++s.pipboyTaps;
                (void)trl;
            }
            p.b = s.pipboyTap.On(now);                      // Pip-Boy
            // activate (hold = grab), per hand: A picks what the primary controller points at, X the off controller
            {
                constexpr double kActWaitMs = 150.0;
                bool apr, arl, opr, orl;
                s.pAct.Update(pLower, now, apr, arl);
                s.oAct.Update(oLower, now, opr, orl);
                if (apr || opr) {
                    const int h = opr && !apr ? 1 : 0;
                    s.actHand = h;
                    game::SetPickHand(h);
                    s.actRuns0 = game::PickRuns(h);
                    s.actPending = true;
                    ++s.actByHand[h];
                }
                if (arl || orl) s.actReleased = now;
                const bool anyDown = s.pAct.down || s.oAct.down;
                if (s.actPending) {
                    const double waited = std::chrono::duration<double, std::milli>(now - (s.actHand == 1 ? s.oAct.t0 : s.pAct.t0)).count();
                    if (game::PickRuns(s.actHand) >= s.actRuns0 + 1) {
                        s.actPending = false, s.actLastWaitMs = waited;
                    } else if (waited >= kActWaitMs) {
                        s.actPending = false, s.actLastWaitMs = waited, ++s.actWaitTimeouts;
                    }
                }
                // back to the primary hand's ray 300 ms after the off hand's button is released (kept while the off ray's
                // quick-loot list is up, so the off stick can step through it and X takes the next item)
                if (s.actHand == 1 && !anyDown && !s.actPending && !ql && now - s.actReleased > std::chrono::milliseconds(300)) {
                    s.actHand = 0;
                    game::SetPickHand(0);
                }
                p.a = anyDown && !s.actPending;
            }
            // B: V.A.T.S.; while a quick-loot list is up B opens the transfer menu (FO4VR) = QCOpenTransferMenu (!ReadyWeapon =
            // pad X in the flat ControlMap). A press that started as "transfer" never turns into V.A.T.S.
            {
                bool bpr, brl;
                s.pUpperBtn.Update(pUpper, now, bpr, brl);
                if (bpr) {
                    s.upperForTransfer = ql;
                    if (ql) s.transfer.Fire(now, 120), ++s.qlTransfers;
                }
                p.lb = pUpper && !s.upperForTransfer;       // V.A.T.S.
            }
            p.dpadDown = s.ql.Down(now);                    // quick-loot list down / up
            const bool qlUp = s.ql.Up(now);
            // primary stick click + primary grip = holster (hold X). 16 Nov (user): the stick click alone no longer opens
            // FavoritesMenu (D-pad up); the click is the gesture button now (Gestures.h)
            if (pClickPressed) s.clickUsedForHolster = false;
            if (pGripPressed && s.pStickClick.down) s.holster = true, s.clickUsedForHolster = true;
            if (s.holster) s.pGrip.heldFired = true;  // the grip release after a holster is not a reload
            if (!s.pGrip.down) s.holster = false;
            (void)pClickReleased;
            p.x = s.holster || s.transfer.On(now);
            p.dpadUp = s.favorites.On(now) || qlUp;
            // primary grip: tap = reload (X tap), hold >= 350 ms = grenade (RB held until release)
            if (!s.holster) {
                if (s.pGrip.down && !s.pGrip.heldFired && s.pGrip.HeldMs(now) >= 350.0) s.pGrip.heldFired = true;
                if (pGripReleased && !s.pGrip.heldFired) s.reload.Fire(now, 120);
                p.rb = s.pGrip.down && s.pGrip.heldFired;
                p.x = p.x || s.reload.On(now);
            }
            // off grip held >= 500 ms: Pip-Boy light (FO4VR "hold L grip"). 20 Nov: through the engine's TogglePipboyLight
            // (GamePipboy.h; [Controllers] bPipboyLightDirect=1), else / if its bytes differ: hold B 1.2 s (PipboyHandler)
            if (s.oGrip.down && !s.oGrip.heldFired && s.oGrip.HeldMs(now) >= 500.0) {
                s.oGrip.heldFired = true;
                ++s.lightHolds;
                if (LightDirect() && game::RequestTogglePipboyLight("off grip held")) {
                    ++s.lightDirect;
                    haptics::Request(oPhys, 0.3f, 0.03f);
                } else {
                    s.light.Fire(now, 1200);
                }
            }
            (void)oGripPressed;
            p.b = p.b || s.light.On(now);
            if (WorkshopFO4VR() && eng::Ok()) {
                // Y (FO4VR): held fEnterWorkshopDelay (the game's own 1.5 s) in a build area = workshop mode, started through
                // the engine (no pad Back: flat's TogglePOV handler toggled first / third person on that); tap (< 0.5 s) =
                // pause; a release in between does nothing. Outside a build area the hold does nothing (status in the log).
                const double enterMs = eng::EnterWorkshopDelay() * 1000.0;
                if (s.offUpper.down && !s.offUpper.heldFired && s.offUpper.HeldMs(now) >= enterMs) {
                    s.offUpper.heldFired = true;
                    ++s.wsEnters;
                    eng::RequestEnterWorkshop();
                    haptics::Request(oPhys, 0.4f, 0.04f);
                }
                if (offUpperReleased && !s.offUpper.heldFired && now - s.offUpper.t0 < std::chrono::milliseconds(500)) s.pause.Fire(now, 150);
                p.back = false;
            } else {
                // Y: tap = pause (Start), hold >= 600 ms = workshop mode (hold Back) - the pre-20 Nov mapping
                if (s.offUpper.down && s.offUpper.HeldMs(now) >= 600.0) s.offUpper.heldFired = true;
                if (offUpperReleased && !s.offUpper.heldFired) s.pause.Fire(now, 150);
                p.back = s.offUpper.down && s.offUpper.heldFired;
            }
            p.start = p.start || s.pause.On(now);
            break;
        }
        case Ctx::Pipboy: {
            p.lx = osx, p.ly = osy;      // sections (left/right), selection (up/down)
            p.rx = psx, p.ry = psy;      // map scroll / perk chart
            // tabs: X previous / Y next (A / B left-handed). The flat Pip-Boy switches its top tabs with LT / RT
            // (measured 14 Nov: pad rt -> page 0->1; lb / rb change nothing)
            p.lt = oLower ? 1.0f : 0.0f;
            p.rt = oUpper ? 1.0f : 0.0f;
            // 20 Nov (user: "In the pipboy, theres no button that levels you up"): FO4VR HelpPipBoyControlsVR_VW "[L trigger] -
            // Open Perk chart while on the status screen". Flat: PipboyMenu.swf YButton -> ShowPerksMenu (pad Y; keyboard T),
            // which nothing sent before. On the STAT page the off trigger is that; select stays on the primary trigger.
            const bool statPage = game::PipboyPage() == 0;
            p.y = statPage && oTrigUse;                          // perk chart / level up
            p.a = (oTrigUse && !statPage) || (trigOn && !a.pipPointerOnQuad);  // select / equip (the primary trigger clicks via the pointer when on the screen)
            p.b = s.pGrip.down || s.oGrip.down;                 // close (FO4VR: lower the arm / grip)
            break;
        }
        case Ctx::Favorites: {
            p.lx = psx, p.ly = psy;      // highlight with the primary stick
            p.a = trigOn || pLower;      // use
            p.b = s.pGrip.down || s.oGrip.down || pUpper;
            break;
        }
        case Ctx::Vats: {
            // 20 Nov (user: "VATS can't cycle targets ... push the other stick to the side to cycle to another enemy"): flat
            // VATSMenu (VatsMenuObj::HandleEvent B1B690) reads two sticks: ChangePart (pad LS) = body part
            // (ProcessPartSelectionInput), Look (pad RS) = target by stick direction (UpdateState B1E865, fVATSThumbstick-
            // SelectionAngle). The off stick was not sent here.
            p.lx = psx, p.ly = psy;      // body part
            p.rx = osx, p.ry = osy;      // target
            p.rt = trigOn ? 1.0f : pTrig;  // queue / fire
            p.lb = pUpper;               // exit V.A.T.S. (B)
            p.x = pLower;                // critical (A)
            p.a = oTrigUse;
            p.b = s.pGrip.down || s.oGrip.down;
            break;
        }
        case Ctx::Workshop: {
            if (!WorkshopFO4VR()) {  // the pre-20 Nov mapping
                p.lx = osx, p.ly = osy;
                p.rx = psx;
                p.a = trigOn;
                p.x = pLower;
                p.y = pUpper;
                p.back = oUpper;
                p.b = oLower || s.pGrip.down || s.oGrip.down;
                break;
            }
            // FO4VR workshop (HelpWorkshopMenuVR_VW, WorkshopTutorialMessage09_1_Start, HelpWorkshopPowerVR_VW,
            // HelpWorkshopWorkersVR_VW) -> flat WorkshopMenu::HandleEvent (B2C1D0) user events, Workshop input context:
            //   X                toggle the build menu: minimised = the panel is hidden and the off stick walks (ours: flat
            //                    has no minimised state; WorkshopMenu stays open, workshop mode stays on)
            //   off stick        menu shown: left/right = category / item row (Left/Right = D-pad), up = into the selected
            //                    entry (Up), down = back out (Down); minimised: move (+ click = sprint)
            //   off stick click  held = settlement stats (menu shown: the panel is enlarged so its stat bar reads)
            //   R trigger        build / select / place (Accept = A; hold > 0.25 s = flat's adjust mode, > 1 s multi-select)
            //   A                scrap; on a settler: move to another settlement (XButton = X)
            //   B                store (Cancel = B: store / cancel the edit / back out)
            //   R grip           wire (YButton = Y); while an object is held: cancel the move / build (Cancel = B)
            //   R stick L/R      held object: rotate (RTrigger / LTrigger analog); else turn
            //   R stick up/down  held object: farther / closer (UpdateItemHoldDistance, flat's mouse wheel)
            //   R stick click    held still: tag for search; on a settler: supply line (RShoulder = RB)
            //   Y                leave workshop mode (CloseMenu = Back, acts on release)
            if (ctxChanged) {
                s.wsMin = false, s.wsStats = false;
                s.wsToggle.down = oLower;  // a press carried in does not toggle
                s.wsYCarried = oUpper;     // the Y hold that entered workshop mode must not leave it on its release
            }
            if (!oUpper) s.wsYCarried = false;
            bool tpr, trl;
            s.wsToggle.Update(oLower, now, tpr, trl);
            if (tpr) {
                s.wsMin = !s.wsMin;
                ++s.wsToggles;
                haptics::Request(oPhys, 0.3f, 0.03f);
            }
            const bool held = eng::ItemHeld();
            const bool oClickGame = gestures::ClickToGame(oPhys, osClickRaw);
            s.wsStats = !s.wsMin && oClickGame;
            {
                const unsigned steps0 = s.wsY.up + s.wsY.down + s.wsX.up + s.wsX.down;
                s.wsY.Update(!s.wsMin, osy, osx, now);
                s.wsX.Update(!s.wsMin, osx, osy, now);
                // 22 Nov (workshop agent): FO4VR's secondary-stick menu handler (VR 140C01530 / 140C024D0) pulses that
                // controller on every step: FUN_141BA9BF0(device, 0.3, 0.01 s)
                if (s.wsY.up + s.wsY.down + s.wsX.up + s.wsX.down != steps0) haptics::Request(oPhys, 0.3f, 0.01f);
            }
            if (s.wsMin) {
                p.lx = osx, p.ly = osy;  // walk (comfort applies the direction mode afterwards)
                p.ls = oClickGame;       // sprint
            } else {
                p.dpadUp = s.wsY.Up(now), p.dpadDown = s.wsY.Down(now);
                p.dpadRight = s.wsX.Up(now), p.dpadLeft = s.wsX.Down(now);
            }
            if (held) {
                constexpr float dz = 0.2f;
                // 21 Nov (workshop agent): FO4VR's primary-stick handler (VR 140C01530) does ONE of the two per stick event:
                // the dominant axis wins - |x| > |y| = UpdateItemZRot (rotate), else UpdateItemHoldDistance (push / pull).
                const bool rotAxis = std::abs(psx) > std::abs(psy);
                p.rt = rotAxis && psx > dz ? (psx - dz) / (1.0f - dz) : 0.0f;
                p.lt = rotAxis && psx < -dz ? (-psx - dz) / (1.0f - dz) : 0.0f;
                p.rx = 0.0f;
                if (!rotAxis && std::abs(psy) > 0.35f && now >= s.wsNextHold) {
                    s.wsNextHold = now + std::chrono::milliseconds(60);
                    eng::RequestHoldDistance((psy > 0.0f) == WorkshopUpFarther(), (std::abs(psy) - 0.35f) / 0.65f);
                    ++s.wsHoldSteps;
                }
            } else {
                p.rx = psx;  // turn
            }
            if (pGripPressed) s.wsGripCancel = held;  // the grip's job is chosen when it goes down
            p.a = trigOn;
            p.x = pLower;
            // 22 Nov (workshop agent): FO4VR's Workshop input context maps Cancel to the grip of either controller (ControlMap
            // row "Cancel 0x02 0x02 0x02"); the primary grip is taken by the wire handler (WandGrip), so the off-hand grip is
            // the plain Cancel: back out of a category / store / cancel the edit (flat B)
            p.b = pUpper || (s.pGrip.down && s.wsGripCancel) || s.oGrip.down;
            p.y = s.pGrip.down && !s.wsGripCancel;
            p.back = oUpper && !s.wsYCarried;
            p.rb = gestures::ClickToGame(pPhys, psClick);
            break;
        }
        case Ctx::Lockpick: {
            p.lx = osx, p.ly = osy;      // bobby pin
            p.rx = psx, p.ry = psy;      // screwdriver
            p.b = s.pGrip.down || s.oGrip.down;  // exit
            break;
        }
        case Ctx::Dialogue: {
            // the dialogue filter (GameDialogue FilterPad) turns the primary stick into the 4 options afterwards
            p.rx = psx, p.ry = psy;
            p.lx = osx, p.ly = osy;
            if (DialogueFaceButtons()) {
                // 15 Nov (user: "buttons don't correspond to the displayed buttons"): the flat DialogueMenu shows the gamepad
                // glyphs of the four options (Y top, X left, B right, A bottom). Touch face buttons carry the same labels, so
                // each physical button is sent as that pad button, by label (not by hand role; left-handed too). Nothing
                // else sends B: on the pad B is an option, so the old grip -> B picked the right-hand option.
                p.a = c.a;
                p.b = c.b;
                p.x = c.x;
                p.y = c.y;
            } else {
                p.a = pLower || oLower;
                p.b = s.pGrip.down || s.oGrip.down;
            }
            if (listNav) {
                // EDI: the primary stick steps the option list (one D-pad pulse per push, repeat while held) instead of
                // looking around; the trigger accepts the highlighted option when the pointer is not on the panel (on
                // the panel it is the pointer's click: hover highlights, click picks). Steps only while the options are
                // up: between lines EDI hides its list and re-enables favorites, where the D-pad would be a quick key.
                const bool pick = dlg::CanPick();
                s.dlgY.Update(pick, psy, psx, now);
                s.dlgX.Update(pick, psx, psy, now);
                p.dpadUp = s.dlgY.Up(now);
                p.dpadDown = s.dlgY.Down(now);
                p.dpadRight = s.dlgX.Up(now);
                p.dpadLeft = s.dlgX.Down(now);
                p.rx = p.ry = 0.0f;
                p.a = p.a || (trigOn && !a.menuPointerOnQuad);
            }
            break;
        }
        case Ctx::Menu:
        default: {
            // containers, barter, terminals, workbenches, perk chart, pause menu ...: both sticks navigate
            const float mx = std::clamp(psx + osx, -1.0f, 1.0f), my = std::clamp(psy + osy, -1.0f, 1.0f);
            p.lx = mx, p.ly = my;
            p.rx = psx, p.ry = psy;
            const bool trigClickByPointer = a.menuPointerOnQuad;
            p.a = (trigOn || oTrigUse) && !trigClickByPointer;  // select (the pointer clicks when it is on the quad)
            p.b = s.pGrip.down || s.oGrip.down || pUpper;  // exit / back (grip; B as on the flat pad)
            p.x = oLower;                // take all / tag / accept deal
            p.y = pLower;                // sort / scrap (A)
            {
                auto* ui = RE::UI::GetSingleton();
                if (MenuOpen(ui, "LevelUpMenu")) {
                    // 20 Nov, FO4VR perk chart (HelpPerksVR_VW): B next rank, Y previous rank, grip exit (flat NextPerk RB /
                    // PrevPerk LB / Cancel B)
                    p.b = s.pGrip.down || s.oGrip.down;
                    p.rb = pUpper;
                    p.lb = oUpper;
                } else if (game::PipboyPage() == 0) {
                    // Pip-Boy shown on the flat window (not the wrist / projected one): STAT page, off trigger = perk chart
                    p.y = oTrigUse;
                    p.a = trigOn && !trigClickByPointer;
                    p.start = p.start || (oUpper && !s.offUpper.heldFired);
                } else if (IsWorkbenchMenu(ui)) {
                    // 20 Nov, FO4VR workbenches: Y scrap (HelpCraftingVR / modding station texts) = flat YButton; X rename /
                    // tag / repair = flat XButton (above)
                    p.y = p.y || oUpper;
                } else {
                    p.start = p.start || (oUpper && !s.offUpper.heldFired);
                }
            }
            break;
        }
        }
        if (!InWorld(ctx) && s.actHand != 0) {  // the off-hand pick ray only lasts while activating in gameplay
            s.actHand = 0, s.actPending = false;
            game::SetPickHand(0);
        }
        // FO4VR: no left-trigger aim (aim down sights = raise the gun to the eye); the workshop's LT = object rotation
        if (ctx != Ctx::Pipboy && ctx != Ctx::Workshop) p.lt = 0.0f;
        {
            const bool ws = ctx == Ctx::Workshop && WorkshopFO4VR();
            g_wsPanelHidden = ws && s.wsMin;
            g_wsStatsShown = ws && s.wsStats;
            if (!ws) s.wsMin = false, s.wsStats = false;
        }
        if (InWorld(ctx) && a.menuPointerOnQuad && !a.menuPointerLeftHand) p.rt = 0.0f;
        {
            const bool now16[16] = { p.a, p.b, p.x, p.y, p.lb, p.rb, p.ls, p.rs, p.start, p.back, p.dpadUp, p.rt > 0.5f, p.ly > 0.5f, p.ly < -0.5f, p.lx > 0.5f, p.lx < -0.5f };
            const bool was16[16] = { s.prev.a, s.prev.b, s.prev.x, s.prev.y, s.prev.lb, s.prev.rb, s.prev.ls, s.prev.rs, s.prev.start, s.prev.back, s.prev.dpadUp,
                s.prev.rt > 0.5f, s.prev.ly > 0.5f, s.prev.ly < -0.5f, s.prev.lx > 0.5f, s.prev.lx < -0.5f };
            for (int i = 0; i < 16; ++i) if (now16[i] && !was16[i]) ++s.n[i];
            s.prev = p;
        }
        s.last = p;
        s.lastText = std::format(
            "ctx={} lx={:.2f} ly={:.2f} rx={:.2f} ry={:.2f} rt={:.2f} lb={} rb={} a={} b={} x={} y={} ls={} rs={} start={} back={} dpadUp={} dpadDown={}",
            CtxName(ctx), p.lx, p.ly, p.rx, p.ry, p.rt, p.lb ? 1 : 0, p.rb ? 1 : 0, p.a ? 1 : 0, p.b ? 1 : 0, p.x ? 1 : 0, p.y ? 1 : 0,
            p.ls ? 1 : 0, p.rs ? 1 : 0, p.start ? 1 : 0, p.back ? 1 : 0, p.dpadUp ? 1 : 0, p.dpadDown ? 1 : 0);
    }

    namespace
    {
        std::string QuickLootText()  // g_lock held
        {
            const auto& s = g_s;
            return std::format("| quickloot on={} sink={} engineFlag={} qcEvents={} qcHas={} qcMode={} qcItems={} qcSelected={} qcContainer={:08X} "
                               "qlHand={} navUp={} navDown={} transfers={} entries={} latch={}{}",
                QuickLootEnabled() ? 1 : 0, g_qcReg.load(), QcEngineFlag() ? 1 : 0, g_qc.events.load(), g_qc.has.load() ? 1 : 0, g_qc.mode.load(),
                g_qc.items.load(), g_qc.selected.load(), g_qc.containerHandle.load(), s.qlHand == 1 ? "off" : "primary", s.ql.up, s.ql.down,
                s.qlTransfers, s.qlEntries, s.qlLatch[0] ? 1 : 0, s.qlLatch[1] ? 1 : 0);
        }
    }

    bool SetClaims(std::uint32_t a_owner, std::uint32_t a_left, std::uint32_t a_right)
    {
        ++g_claimSets;
        if (a_owner < 1 || a_owner > FO4FLATVR_CLAIM_OWNER_MAX) {
            if (g_claimLogged.fetch_add(1) < 50) REX::WARN("bindings: input claim from owner {} refused (1..{})", a_owner, FO4FLATVR_CLAIM_OWNER_MAX);
            return false;
        }
        a_left &= FO4FLATVR_CLAIM_ALL, a_right &= FO4FLATVR_CLAIM_ALL;
        const auto pl = g_claimL[a_owner].exchange(a_left), pr = g_claimR[a_owner].exchange(a_right);
        if (pl != a_left || pr != a_right) {
            const auto n = ++g_claimChanges;
            // logged on change; a client that toggles per frame is only counted after the first 200 lines (pipe "bindings claims")
            if (g_claimLogged.fetch_add(1) < 200)
                REX::INFO("bindings: input claims owner {}{}: L[{}] R[{}] (#{}; honoured {}, gameplay / quick loot only)", a_owner,
                    a_owner == FO4FLATVR_CLAIM_OWNER_ROCK ? " (ROCK)" : "", ClaimBits(a_left), ClaimBits(a_right), n,
                    ClaimsOn() ? "yes" : "no: [ROCK] bInputClaims=0");
        }
        return ClaimsOn();
    }

    void Claims(std::uint32_t& a_left, std::uint32_t& a_right) { ClaimedMasks(a_left, a_right); }

    bool DialogueListNavActive() { return g_dlgListNavActive.load(std::memory_order_relaxed) && dlg::Open(); }

    bool  WorkshopPanelHidden() { return g_wsPanelHidden.load(std::memory_order_relaxed); }
    float WorkshopPanelScale() { return g_wsStatsShown.load(std::memory_order_relaxed) ? WorkshopStatsScale() : 1.0f; }

    void InstallGlyphs()
    {
        // 20 Nov (live settings, ConfigStore.h): the bindings' read-once switches are read again after a change ([Controllers]
        // bFO4VRBindings / bDialogueFaceButtons / bDialogueListNav / bQuickLootStick / bWorkshopFO4VR / bWorkshopStickUpFarther /
        // bGlyphRemap, [Menus] fWorkshopStatsScale): the caches go back to "not read".
        static std::once_flag s_sub;
        std::call_once(s_sub, [] {
            cfgstore::Subscribe("bindings claims", "ROCK/bInputClaims", [](const cfgstore::Changes&) {
                g_claimsOn = INT_MIN;
                REX::INFO("bindings: [ROCK] bInputClaims={} (live)", ClaimsOn() ? 1 : 0);
            });
            cfgstore::Subscribe("bindings", "Controllers,Menus/fWorkshopStatsScale", [](const cfgstore::Changes&) {
                g_enabled = -1;
                g_dlgFace = -1;
                g_dlgListNav = -1;
                g_quickLoot = -1;
                g_wsFO4VR = INT_MIN;
                g_wsUpFarther = INT_MIN;
                g_lightDirect = INT_MIN;
                g_glyphRemap = INT_MIN;
                g_wsStatsScale = -1.0f;
                REX::INFO("bindings: [Controllers] switches re-read (live): FO4VR bindings {} workshop FO4VR {} stick up farther {} glyphs {} "
                          "stats scale {:.2f}",
                    Enabled() ? 1 : 0, WorkshopFO4VR() ? 1 : 0, WorkshopUpFarther() ? 1 : 0, GlyphRemapOn() ? 1 : 0, WorkshopStatsScale());
            });
        });
        // the glyph call sites are patched whatever bGlyphRemap says at load (the lookups pass through while it is off: live)
        if (!GlyphRemapOn()) {
            REX::INFO("glyphs: [Controllers] bGlyphRemap=0 at load: call sites patched anyway, glyphs pass through until it is on");
        }
        const auto b = Base();
        // call sites of ControlMap::GetButtonNameFromUserEvent (166FCF0) and ButtonHintDataWithClone::CreateClone (A50840)
        static constexpr std::uintptr_t kNameSites[] = { 0xB71C2C, 0xB71C52, 0xB71E84, 0xB71EE5, 0xB71F06, 0xB72213, 0xB72238 };
        constexpr std::uintptr_t        kGetBtnName = 0x166FCF0, kCloneSite = 0xA504CC, kCreateClone = 0xA50840;
        auto target = [&](std::uintptr_t a_site) -> std::uintptr_t {
            const auto* p = reinterpret_cast<const std::uint8_t*>(b + a_site);
            if (p[0] != 0xE8) return 0;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            return b + a_site + 5 + rel - b;
        };
        int names = 0, bad = 0;
        g_glyphTramp.create(16 * 14);
        for (const auto site : kNameSites) {
            if (target(site) != kGetBtnName) {
                ++bad;
                REX::WARN("glyphs: call site {:X} is not a call to {:X}: skipped", site, kGetBtnName);
                continue;
            }
            const auto orig = g_glyphTramp.write_call<5>(b + site, &HkGetBtnName);
            if (!g_origGetBtnName) g_origGetBtnName = reinterpret_cast<GetBtnName_t>(orig);
            ++names;
        }
        bool clone = false;
        if (target(kCloneSite) == kCreateClone) {
            g_origCreateClone = reinterpret_cast<CreateClone_t>(g_glyphTramp.write_call<5>(b + kCloneSite, &HkCreateClone));
            clone = true;
        } else {
            ++bad;
            REX::WARN("glyphs: call site {:X} is not a call to CreateClone {:X}: menu hints keep the flat glyphs", kCloneSite, kCreateClone);
        }
        uisound::Install();  // 20 Nov: UI sound play counter (ticking report; UiSoundDiag.h)
        g_glyphInstall = std::format("installed: user-event name sites {}/{}, menu hint clone {}{}", names, std::size(kNameSites), clone ? 1 : 0,
            bad ? std::format(" ({} skipped)", bad) : "");
        REX::INFO("glyphs: {}", g_glyphInstall);
    }

    std::string Command(const std::string& a_arg)
    {
        std::istringstream is(a_arg);
        std::string        v;
        is >> v;
        if (v == "on" || v == "off") {
            g_enabled = v == "on" ? 1 : 0;
            return "bindings " + v;
        }
        if (v == "menu") {  // 20 Nov test aid: "bindings menu <ms>" = the left menu button held for ms (SteamVR's Touch binding
                            // does not pass the menu button to apps), then the menu hold / tap counters
            int ms = 0;
            is >> ms;
            std::scoped_lock l(g_lock);
            if (ms > 0) g_menuTestUntil = Clock::now() + std::chrono::milliseconds((std::min)(ms, 10000));
            return std::format("menu holdS={:.2f} holds={} taps={}{}", configui::MenuHoldSeconds(), g_s.menuHolds, g_s.menuTaps,
                ms > 0 ? std::format(" (held for {} ms from now)", ms) : "");
        }
        if (v == "ws") {  // 20 Nov: "bindings ws [enter]": workshop state; enter = what the Y hold does (test aid)
            std::string w;
            is >> w;
            if (w == "enter") eng::RequestEnterWorkshop();
            std::scoped_lock l(g_lock);
            return std::format("bindings ws fo4vr={} engine={} enterDelay={:.2f}s itemHeld={} min={} stats={} panelHidden={} panelScale={:.2f} "
                               "toggles={} holdSteps={} holdReq={} yHolds={} enterReq={} started={} lastStatus={} last: {}",
                WorkshopFO4VR() ? 1 : 0, eng::Ok() ? 1 : 0, eng::Ok() ? eng::EnterWorkshopDelay() : 0.0f, eng::ItemHeld() ? 1 : 0, g_s.wsMin ? 1 : 0,
                g_s.wsStats ? 1 : 0, WorkshopPanelHidden() ? 1 : 0, WorkshopPanelScale(), g_s.wsToggles, g_s.wsHoldSteps, eng::g_holdReq.load(),
                g_s.wsEnters, eng::g_enterReq.load(), eng::g_entered.load(), eng::g_lastStatus.load(), eng::g_lastWhy);
        }
        if (v == "glyphs") {  // 20 Nov: "bindings glyphs [<XenonName> [event]]": glyph remap state / what a name maps to now
            std::string n, ev;
            is >> n >> ev;
            std::string probe;
            if (!n.empty()) {
                const char* g = RemapGlyph(n, ev, "probe");
                probe = std::format(" | probe {} {} -> {} (ctx {})", n, ev, g ? g : "(kept)", GCtxName(GlyphCtx()));
            }
            std::scoped_lock l(g_glyphLogLock);
            return std::format("bindings glyphs on={} {} ctx={} nameLookups={} nameSwaps={} hintClones={} hintSwaps={} last: {}{}", GlyphRemapOn() ? 1 : 0,
                g_glyphInstall, GCtxName(GlyphCtx()), g_glyphNames.load(), g_glyphSwaps.load(), g_glyphClones.load(), g_glyphCloneSwaps.load(),
                g_glyphLast, probe);
        }
        if (v == "claims") {  // 21 Nov: "bindings claims [on|off]": bridge v8 input claims (ROCK-flat) and what Map() masks now
            std::string w;
            is >> w;
            if (w == "on" || w == "off") g_claimsOn = w == "on" ? 1 : 0;
            return "bindings " + ClaimsText();
        }
        if (v == "sounds") return uisound::Report();  // 20 Nov: "bindings sounds": UI sounds played (ticking report)
        if (v == "light") {  // 20 Nov: "bindings light [toggle]": Pip-Boy light state; toggle = what the off grip hold does (test aid)
            std::string w;
            is >> w;
            bool queued = false;
            if (w == "toggle") queued = game::RequestTogglePipboyLight("pipe");
            std::scoped_lock l(g_lock);
            return std::format("bindings light direct={} holds={} viaEngine={} pipboyTaps={}{} | {}", LightDirect() ? 1 : 0, g_s.lightHolds, g_s.lightDirect,
                g_s.pipboyTaps, w == "toggle" ? (queued ? " (toggle queued)" : " (engine calls unavailable)") : "", game::PipboyLightReport());
        }
        if (v == "ql") {  // "bindings ql [on|off]": quick-loot stick ([Controllers] bQuickLootStick) + quick-container state
            std::string w;
            is >> w;
            if (w == "on" || w == "off") g_quickLoot = w == "on" ? 1 : 0;
            std::scoped_lock l(g_lock);
            return "bindings ql " + QuickLootText();
        }
        std::scoped_lock l(g_lock);
        const auto& n = g_s.n;
        return std::format("bindings fo4vr={} {} | edges a={} b={} x={} y={} lb={} rb={} ls={} rs={} start={} back={} dpadUp={} rt={} lsUp={} lsDown={} lsRight={} lsLeft={} "
                           "| activate hand={} pressesPrimary={} pressesOff={} pending={} lastWaitMs={:.1f} waitTimeouts={} {}",
            Enabled() ? 1 : 0, g_s.lastText, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[10], n[11], n[12], n[13], n[14], n[15],
            g_s.actHand == 1 ? "off" : "primary", g_s.actByHand[0], g_s.actByHand[1], g_s.actPending ? 1 : 0, g_s.actLastWaitMs, g_s.actWaitTimeouts,
            QuickLootText()) +
               std::format(" | dialogue edi={} listNavSwitch={} listNav={} steps up={} down={} right={} left={}", g_edi.load(), g_dlgListNav.load(),
                   g_dlgListNavActive.load() ? 1 : 0, g_s.dlgY.up, g_s.dlgY.down, g_s.dlgX.up, g_s.dlgX.down) +
               std::format(" | carried over a context change: sticks={} (now primary={} off={}) offTrigger={} (now {}) | pipboyTaps={} lightHolds={}",
                   g_s.stickCarries, g_s.stickCarry[0] ? 1 : 0, g_s.stickCarry[1] ? 1 : 0, g_s.trigCarries, g_s.oTrigCarry ? 1 : 0, g_s.pipboyTaps,
                   g_s.lightHolds);
    }
}
