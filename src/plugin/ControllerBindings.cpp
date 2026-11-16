// FO4VR default controller bindings -> virtual gamepad. See ControllerBindings.h and analysis/vr-controls.md.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/ControllerBindings.h"
#include "plugin/GameBridge.h"
#include "plugin/Settings.h"
#include "xr/XrRuntime.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

        bool MenuOpen(RE::UI* a_ui, const char* a_name) { return a_ui && a_ui->GetMenuOpen(a_name); }

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

    void Map(const xr::ControllerState& c, const Context& a, pad::State& p)
    {
        if (!Enabled()) {
            MapGeneric(c, a, p);
            return;
        }
        const auto now = Clock::now();
        const bool L = a.leftHanded;
        // roles: primary (weapon) hand P, off hand O; lower face button (A/X) and upper (B/Y) per hand
        const float pTrig = L ? c.leftTrigger : c.rightTrigger, oTrig = L ? c.rightTrigger : c.leftTrigger;
        const float pGripV = L ? c.leftGrip : c.rightGrip, oGripV = L ? c.rightGrip : c.leftGrip;
        const float psx = L ? c.leftStickX : c.rightStickX, psy = L ? c.leftStickY : c.rightStickY;
        const float osx = L ? c.rightStickX : c.leftStickX, osy = L ? c.rightStickY : c.leftStickY;
        const bool  psClick = L ? c.leftStickClick : c.rightStickClick, osClick = L ? c.rightStickClick : c.leftStickClick;
        const bool  pLower = L ? c.x : c.a, pUpper = L ? c.y : c.b;  // A, B (right-handed)
        const bool  oLower = L ? c.a : c.x, oUpper = L ? c.b : c.y;  // X, Y (right-handed)
        const bool  trigOn = pTrig > 0.6f, oTrigOn = oTrig > 0.6f;

        std::scoped_lock l(g_lock);
        State&     s = g_s;
        QcEnsureSink(now);
        const Ctx  ctx = Detect(a);
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
        }
        p.start = c.menu;  // Touch left menu button (FO4VR: SteamVR dashboard) -> pause, as before

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
            p.b = oTrigOn;                                  // Pip-Boy
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
            // primary stick click + primary grip = holster (hold X); stick click alone = favorites (D-pad)
            // (favorites fires on the click's release unless the grip was pressed during it)
            if (pClickPressed) s.clickUsedForHolster = false;
            if (pGripPressed && s.pStickClick.down) s.holster = true, s.clickUsedForHolster = true;
            if (s.holster) s.pGrip.heldFired = true;  // the grip release after a holster is not a reload
            if (!s.pGrip.down) s.holster = false;
            if (pClickReleased && !s.clickUsedForHolster) s.favorites.Fire(now, 150);
            p.x = s.holster || s.transfer.On(now);
            p.dpadUp = s.favorites.On(now) || qlUp;
            // primary grip: tap = reload (X tap), hold >= 350 ms = grenade (RB held until release)
            if (!s.holster) {
                if (s.pGrip.down && !s.pGrip.heldFired && s.pGrip.HeldMs(now) >= 350.0) s.pGrip.heldFired = true;
                if (pGripReleased && !s.pGrip.heldFired) s.reload.Fire(now, 120);
                p.rb = s.pGrip.down && s.pGrip.heldFired;
                p.x = p.x || s.reload.On(now);
            }
            // off grip held >= 500 ms: Pip-Boy light (hold B 1.2 s)
            if (s.oGrip.down && !s.oGrip.heldFired && s.oGrip.HeldMs(now) >= 500.0) {
                s.oGrip.heldFired = true;
                s.light.Fire(now, 1200);
            }
            (void)oGripPressed;
            p.b = p.b || s.light.On(now);
            // Y: tap = pause (Start), hold >= 600 ms = workshop mode (hold Back)
            if (s.offUpper.down && s.offUpper.HeldMs(now) >= 600.0) s.offUpper.heldFired = true;
            if (offUpperReleased && !s.offUpper.heldFired) s.pause.Fire(now, 150);
            p.back = s.offUpper.down && s.offUpper.heldFired;
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
            p.a = oTrigOn || (trigOn && !a.pipPointerOnQuad);  // select / equip (the primary trigger clicks via the pointer when on the screen)
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
            p.lx = psx, p.ly = psy;      // part / target
            p.rt = trigOn ? 1.0f : pTrig;  // queue / fire
            p.lb = pUpper;               // exit V.A.T.S. (B)
            p.x = pLower;                // critical (A)
            p.a = oTrigOn;
            p.b = s.pGrip.down || s.oGrip.down;
            break;
        }
        case Ctx::Workshop: {
            p.lx = osx, p.ly = osy;      // categories / objects
            p.rx = psx;                  // turn
            p.a = trigOn;                // build / select
            p.x = pLower;                // scrap (A)
            p.y = pUpper;                // store (B)
            p.back = oUpper;             // hold Y: leave the workshop
            p.b = oLower || s.pGrip.down || s.oGrip.down;  // toggle menu (X) / back
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
            break;
        }
        case Ctx::Menu:
        default: {
            // containers, barter, terminals, workbenches, perk chart, pause menu ...: both sticks navigate
            const float mx = std::clamp(psx + osx, -1.0f, 1.0f), my = std::clamp(psy + osy, -1.0f, 1.0f);
            p.lx = mx, p.ly = my;
            p.rx = psx, p.ry = psy;
            const bool trigClickByPointer = a.menuPointerOnQuad;
            p.a = (trigOn || oTrigOn) && !trigClickByPointer;  // select (the pointer clicks when it is on the quad)
            p.b = s.pGrip.down || s.oGrip.down || pUpper;  // exit / back (grip; B as on the flat pad)
            p.x = oLower;                // take all / tag / accept deal
            p.y = pLower;                // sort / scrap (A)
            p.start = p.start || (oUpper && !s.offUpper.heldFired);
            break;
        }
        }
        if (!InWorld(ctx) && s.actHand != 0) {  // the off-hand pick ray only lasts while activating in gameplay
            s.actHand = 0, s.actPending = false;
            game::SetPickHand(0);
        }
        if (ctx != Ctx::Pipboy) p.lt = 0.0f;  // FO4VR: no left-trigger aim (aim down sights = raise the gun to the eye)
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

    std::string Command(const std::string& a_arg)
    {
        std::istringstream is(a_arg);
        std::string        v;
        is >> v;
        if (v == "on" || v == "off") {
            g_enabled = v == "on" ? 1 : 0;
            return "bindings " + v;
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
            QuickLootText());
    }
}
