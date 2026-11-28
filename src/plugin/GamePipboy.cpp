#include "plugin/GamePipboy.h"
#include "plugin/GameBridge.h"
#include "plugin/GameHud.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/PipboyLayer.h"
#include "plugin/ThreadQueues.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <Scaleform/G/GFx_Viewport.h>

namespace fo4flatvr::game
{
    namespace
    {
        // 1.11.240 RVAs (analysis/vr-pipboy.md; flow re-read in Ghidra for this feature).
        constexpr std::uintptr_t kPipboyManagerPtr = 0x31F0888;   // PipboyManager* (id 4799238)
        constexpr std::uintptr_t kPlayOpenAnim = 0xB4F980;        // PipboyManager::PlayPipboyOpenAnim(BSFixedString&)
        constexpr std::uintptr_t kPlayCloseAnim = 0xB50580;       // PipboyManager::PlayPipboyCloseAnim(bool immediate)
        constexpr std::uintptr_t kPlayHolotapeAnim = 0xB4FB60;    // PipboyManager::PlayPipboyLoadHolotapeAnim(BGSNote*, bool immediate)
        constexpr std::uintptr_t kOnOpenAnim = 0xB4FEE0;          // InitPipboy; OnPipboyOpened if waitEvent == ""; show PipboyMenu
        constexpr std::uintptr_t kOnCloseAnim = 0xB506B0;         // teardown; OnPipboyClosed if closeEvent is PA/force close
        constexpr std::uintptr_t kStartAnimGraphListening = 0xB52440;
        constexpr std::uintptr_t kQPipboyActive = 0xB50C50;
        constexpr std::uintptr_t kActorInPowerArmor = 0x9893F0;   // PowerArmor::ActorInPowerArmor(Actor*)
        constexpr std::uintptr_t kEmptyFixedString = 0x3153E0;    // returns const BSFixedString& ""
        constexpr std::uintptr_t kFixedStringAssign = 0x167C380;  // BSFixedString::operator=(const BSFixedString&)
        constexpr std::uintptr_t kImodStop = 0x4ACED0;            // ImageSpaceModifierInstanceForm::Stop(const BSFixedString&)
        constexpr std::uintptr_t kStrImodPipboy = 0x32901B8;      // "PipboyMenuImod" name used by InitPipboy / OnPipboyCloseAnim
        constexpr std::uintptr_t kStrForceClose = 0x32901C0;      // strForceClose
        constexpr std::uintptr_t kStrPowerArmorClose = 0x32901C8; // strPowerArmorClose
        constexpr std::uintptr_t kEffectCallSite = 0xA3D3EC;      // RenderPrepassesAndMenus: call RenderEffect(0xC, 0x3D, 0x3C)
        constexpr std::uintptr_t kRenderEffect = 0x2185610;
        constexpr std::uintptr_t kPageIndex = 0x3287638;          // PipboyMenu current page (0 STAT .. 4 RADIO)
        constexpr std::uintptr_t kTabPerPage = 0x3287640;         // u32[5]: current tab per page
        constexpr std::uintptr_t kRendererGetByName = 0xA3A770;   // Interface3D::Renderer::GetByName(const BSFixedString&)
        bool                     g_getByNameOk = false;

        // PipboyManager fields (flat 1.11.240; same as 1.10 decompiles)
        constexpr std::size_t kMenuToOpen = 0x190, kOpenAnimEvent = 0x198, kCloseAnimEvent = 0x1A0;
        constexpr std::size_t kOpening = 0x1C6, kClosing = 0x1C7, kActive = 0x1D0;

        using Open_t = void (*)(void*, void*);
        using Close_t = void (*)(void*, bool);
        using Holotape_t = void (*)(void*, void*, bool);
        using MgrFn_t = void (*)(void*);
        using MgrBool_t = bool (*)(void*);
        using ActorBool_t = bool (*)(void*);
        using EmptyFs_t = const void* (*)();
        using FsAssign_t = void* (*)(void*, const void*);
        using ImodStop_t = void (*)(const void*);
        using Effect_t = std::uint64_t (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t);

        Open_t   g_origOpen = nullptr;
        Close_t  g_origClose = nullptr;
        Holotape_t g_origHolotape = nullptr;
        bool       g_o3 = false;
        std::atomic<std::uint64_t> g_holotapeCalls{ 0 }, g_holotapeForced{ 0 };
        Effect_t g_origEffect = nullptr;
        bool     g_o1 = false, g_o2 = false, g_h3 = false;

        std::atomic<bool> g_worldViewAvailable{ false };
        bool              g_direct = false;  // main thread: the current Pip-Boy session was opened directly
        bool              g_projected = false;  // main thread: ... projected screen (power armour, or [Pipboy] bFloating)
        bool              g_projectedPa = false;  // main thread: ... and in power armour
        std::atomic<std::uint64_t> g_projectedOpens{ 0 };
        std::atomic<std::uint64_t> g_openCalls{ 0 }, g_directOpens{ 0 }, g_closeCalls{ 0 }, g_directCloses{ 0 }, g_h3Calls{ 0 }, g_h3Pip{ 0 };
        std::atomic<std::uint64_t> g_h3Last{ 0 };  // effect << 32 | src << 16 | dst of the last RenderEffect(0xC) call
        std::string g_lastDecision{ "none" };  // under g_evLock
        std::mutex  g_evLock;                  // g_lastDecision, g_events, g_lastInject

        std::uintptr_t Base() { return REL::Offset(0).address(); }
        template <class T>
        T Fn(std::uintptr_t a_rva) { return reinterpret_cast<T>(Base() + a_rva); }
        std::uint8_t* Mgr() { return *reinterpret_cast<std::uint8_t**>(Base() + kPipboyManagerPtr); }

        const char* FsText(const void* a_fs)
        {
            const char* s = a_fs ? reinterpret_cast<const RE::BSFixedString*>(a_fs)->c_str() : nullptr;
            return s ? s : "";
        }

        bool BytesMatch(std::uintptr_t a_rva, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_rva);
            if (std::memcmp(p, a_expect, a_n) == 0) {
                return true;
            }
            std::string got, want;
            for (std::size_t i = 0; i < a_n; ++i) {
                got += std::format("{:02X} ", p[i]);
                want += std::format("{:02X} ", a_expect[i]);
            }
            REX::ERROR("pipboy: {} at {:X} has bytes [{}], expected [{}]; not patching", a_what, a_rva, got, want);
            return false;
        }

        // Why a wrist/projected open is (not) used; "" = use it. a_projected: in power armour (FO4VR shows the
        // Pip-Boy projected there: bUIPowerArmorGeometry_UseProjectedPipboy:VRUI, PipboyManager C325E0/C34EC0).
        std::string Blocker(const void* a_menuName, bool& a_projected)
        {
            a_projected = false;
            if (!pipboy::Enabled() && !pipboy::ProjectedEnabled() && !pipboy::FloatingEnabled()) {
                return "switch off";
            }
            if (!g_worldViewAvailable.load()) {
                return "no XR world view";
            }
            if (_stricmp(FsText(a_menuName), "PipboyMenu") != 0) {
                return std::format("menu '{}'", FsText(a_menuName));
            }
            if (const int cs = CurrentCameraState(); cs != 0) {
                return std::format("camera state {}", cs);
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return "no player";
            }
            if (Fn<ActorBool_t>(kActorInPowerArmor)(player)) {
                if (!pipboy::ProjectedEnabled()) {
                    return "power armour (projected Pip-Boy off)";
                }
                a_projected = true;
                return {};
            }
            // [Pipboy] bFloating: FO4VR bAlwaysUseProjectedPipboy - the projected screen outside power armour as well.
            if (pipboy::FloatingEnabled()) {
                a_projected = true;
                return {};
            }
            if (!pipboy::Enabled()) {
                return "wrist switch off";
            }
            return {};
        }

        // O1
        void HkPlayOpenAnim(void* a_mgr, void* a_menuName)
        {
            ++g_openCalls;
            bool              projected = false;
            const std::string why = Blocker(a_menuName, projected);
            if (!why.empty()) {
                g_direct = false;
                g_projected = false;
                {
                    std::scoped_lock l(g_evLock);
                    g_lastDecision = "original (" + why + ")";
                }
                REX::INFO("pipboy: open '{}' -> original path ({})", FsText(a_menuName), why);
                g_origOpen(a_mgr, a_menuName);
                return;
            }
            auto* m = static_cast<std::uint8_t*>(a_mgr);
            if (m[kOpening]) {
                return;  // as the original: an open is already in progress
            }
            const auto assign = Fn<FsAssign_t>(kFixedStringAssign);
            m[kOpening] = 1;
            assign(m + kMenuToOpen, a_menuName);
            assign(m + kOpenAnimEvent, Fn<EmptyFs_t>(kEmptyFixedString)());  // "" -> OnPipboyOpenAnim calls OnPipboyOpened now
            Fn<MgrFn_t>(kStartAnimGraphListening)(a_mgr);
            g_direct = true;
            g_projected = projected;
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                g_projectedPa = projected && player && Fn<ActorBool_t>(kActorInPowerArmor)(player);
            }
            // InitPipboy (in power armour EnablePipboyShader shows the PA dash Pip-Boy geometry, screen-attached; H2 then
            // skips that RenderMain), OnPipboyOpened, UIMessageQueue::AddMessage(PipboyMenu, kShow)
            Fn<MgrFn_t>(kOnOpenAnim)(a_mgr);
            if (pipboy::GetConfig().stopImod) {
                Fn<ImodStop_t>(kImodStop)(reinterpret_cast<const void*>(Base() + kStrImodPipboy));
            }
            // round 7: FO4VR's projected screen has no scanlines (VR EnablePipboyShader C34EC0: QPipboyEffectControl()+0x40 = 0
            // when projected; flat B51F00 sets it = !inPowerArmor). Renderer "PipboyMenu" +0xBC = effect control (1.11 A3B420).
            if (projected && !g_projectedPa && pipboy::GetConfig().noScanlines && g_getByNameOk) {
                using GetByName_t = std::uint8_t* (*)(const RE::BSFixedString&);
                static const RE::BSFixedString kPip("PipboyMenu");
                if (auto* r = Fn<GetByName_t>(kRendererGetByName)(kPip)) {
                    r[0xBC + 0x40] = 0;
                    REX::INFO("pipboy: projected screen without scanlines (FO4VR)");
                }
            }
            ++g_directOpens;
            if (projected) {
                ++g_projectedOpens;
            }
            {
                std::scoped_lock l(g_evLock);
                g_lastDecision = projected ? (g_projectedPa ? "projected" : "floating") : "wrist";
            }
            REX::INFO("pipboy: open '{}' -> {} (direct: no StartPipboyMode, no arm animation{})", FsText(a_menuName),
                projected ? (g_projectedPa ? "projected (power armour)" : "floating (projected, [Pipboy] bFloating)") : "wrist",
                pipboy::GetConfig().stopImod ? ", imod stopped" : "");
        }

        // O2
        void HkPlayCloseAnim(void* a_mgr, bool a_immediate)
        {
            ++g_closeCalls;
            if (!g_direct) {
                g_origClose(a_mgr, a_immediate);
                return;
            }
            auto* m = static_cast<std::uint8_t*>(a_mgr);
            if (m[kClosing] == 0 && Fn<MgrBool_t>(kQPipboyActive)(a_mgr)) {
                m[kClosing] = 1;
                Fn<FsAssign_t>(kFixedStringAssign)(m + kCloseAnimEvent,
                    reinterpret_cast<const void*>(Base() + (a_immediate ? kStrForceClose : kStrPowerArmorClose)));
                Fn<MgrFn_t>(kOnCloseAnim)(a_mgr);  // teardown + OnPipboyClosed; Update hides the menu next frame
                ++g_directCloses;
                REX::INFO("pipboy: close (immediate={}) -> direct (no close animation)", a_immediate);
            }
        }

        // O3: a holotape loaded from a wrist-opened Pip-Boy's inventory would wait for the ActionPipboyLoadHolotape
        // arm animation (never played: the arm is not raised) -> use the game's own immediate path instead.
        void HkPlayHolotapeAnim(void* a_mgr, void* a_note, bool a_immediate)
        {
            ++g_holotapeCalls;
            if (g_direct && !a_immediate) {
                ++g_holotapeForced;
                REX::INFO("pipboy: holotape load from a wrist Pip-Boy -> immediate (no load animation)");
                a_immediate = true;
            }
            g_origHolotape(a_mgr, a_note, a_immediate);
        }

        // H3
        std::uint64_t HkRenderEffect(void* a_ism, std::uint32_t a_effect, std::uint32_t a_src, std::uint32_t a_dst, std::uint64_t a_5)
        {
            const auto r = g_origEffect(a_ism, a_effect, a_src, a_dst, a_5);
            ++g_h3Calls;
            g_h3Last = (static_cast<std::uint64_t>(a_effect) << 32) | (static_cast<std::uint64_t>(a_src & 0xFFFF) << 16) | (a_dst & 0xFFFF);
            if (a_dst == 0x3C && pipboy::WantsTargets()) {
                ++g_h3Pip;
                pipboy::OnFinalTarget(RenderTargetTexture(0x3C));
            }
            return r;
        }

        // ---- pointer -> Scaleform -------------------------------------------------------------
        // Scaleform 4 GFx::MouseEvent as the game reads it (PipboyMenu::ProcessMessage writes x/y at +8/+C).
        struct GfxMouseEvent
        {
            std::uint32_t type;       // 1 MouseMove, 2 MouseDown, 3 MouseUp, 4 MouseWheel
            std::uint32_t modifiers;
            float         x, y;
            float         scrollDelta;
            std::uint32_t button;
            std::uint32_t mouseIndex;
        };
        static_assert(sizeof(GfxMouseEvent) == 28);

        struct QueuedEvent
        {
            std::uint32_t type;
            float         u, v;
            float         wheel;
        };

        std::deque<QueuedEvent> g_events;
        std::atomic<bool>       g_taskPending{ false };
        PipboyPointer           g_last;
        bool                    g_lastValid = false;
        std::atomic<std::uint64_t> g_sent[5]{};
        std::atomic<std::uint64_t> g_dropped{ 0 };
        std::string             g_lastInject{ "none" };  // under g_evLock

        void DrainEvents()  // UI task
        {
            std::deque<QueuedEvent> evs;
            {
                std::scoped_lock l(g_evLock);
                evs.swap(g_events);
            }
            g_taskPending = false;
            auto* ui = RE::UI::GetSingleton();
            if (!ui || evs.empty()) {
                return;
            }
            // round 7: a terminal without a terminal ref (holotape terminal) is drawn on the Pip-Boy screen (TerminalMenu::InitMenu
            // renderer = PipboyManager::Get3DRendererName; FO4VR VR_ShowTerminalOnPipboy C33C60): the pointer goes to it then
            const bool toTerminal = MenuUsesRenderer("TerminalMenu", "PipboyMenu");
            const auto menu = ui->GetMenu(toTerminal ? "TerminalMenu" : "PipboyMenu");
            if (!menu || !menu->uiMovie) {
                g_dropped += evs.size();
                return;
            }
            Scaleform::GFx::Viewport vp{};
            menu->uiMovie->GetViewport(&vp);
            const float bw = static_cast<float>(vp.bufferWidth > 0 ? vp.bufferWidth : vp.width);
            const float bh = static_cast<float>(vp.bufferHeight > 0 ? vp.bufferHeight : vp.height);
            std::uint32_t rc = 0;
            float         x = 0, y = 0;
            for (const auto& e : evs) {
                GfxMouseEvent ev{};
                ev.type = e.type;
                ev.x = x = e.u * bw;
                ev.y = y = e.v * bh;
                ev.scrollDelta = e.wheel;
                rc = menu->uiMovie->HandleEvent(*reinterpret_cast<const Scaleform::GFx::Event*>(&ev));
                if (e.type < 5) {
                    ++g_sent[e.type];
                }
            }
            std::scoped_lock l(g_evLock);
            g_lastInject = std::format("type={} x={:.1f} y={:.1f} rc={} viewport buf {}x{} rect {},{} {}x{}", evs.back().type, x, y, rc,
                vp.bufferWidth, vp.bufferHeight, vp.left, vp.top, vp.width, vp.height);
        }

        void Queue(std::uint32_t a_type, float a_u, float a_v, float a_wheel = 0.0f)
        {
            std::scoped_lock l(g_evLock);
            if (g_events.size() >= 64) {
                g_events.pop_front();
                ++g_dropped;
            }
            g_events.push_back({ a_type, a_u, a_v, a_wheel });
        }
    }

    std::string InstallPipboyHooks()
    {
        static std::string summary;
        if (!summary.empty()) {
            return summary;
        }
        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) {
            REX::WARN("pipboy: F4SE trampoline not set up; creating a local one");
            tramp.create(128);
        }
        const auto base = Base();
        auto detour = [&](std::uintptr_t a_rva, const std::uint8_t (&a_prologue)[10], void* a_hook, const char* a_what) -> void* {
            // The first 5 bytes are one whole, position-independent instruction (mov [rsp+8|10],rbx): relocated into a stub.
            if (!BytesMatch(a_rva, a_prologue, sizeof(a_prologue), a_what)) {
                return nullptr;
            }
            auto* stub = static_cast<std::uint8_t*>(tramp.allocate(5 + 14));
            std::memcpy(stub, reinterpret_cast<const void*>(base + a_rva), 5);
            stub[5] = 0xFF;
            stub[6] = 0x25;
            std::memset(stub + 7, 0, 4);
            const std::uint64_t back = base + a_rva + 5;
            std::memcpy(stub + 11, &back, 8);
            tramp.write_jmp<5>(base + a_rva, a_hook);
            REX::INFO("pipboy: detoured {} at rva {:X} (original via stub {})", a_what, a_rva, static_cast<void*>(stub));
            return stub;
        };
        // O1: mov [rsp+8],rbx | push rdi | sub rsp,20 | mov rax,gs:[58]
        static constexpr std::uint8_t kOpenPrologue[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
        if (auto* s = detour(kPlayOpenAnim, kOpenPrologue, reinterpret_cast<void*>(&HkPlayOpenAnim), "PipboyManager::PlayPipboyOpenAnim")) {
            g_origOpen = reinterpret_cast<Open_t>(s);
            g_o1 = true;
        }
        // O2: mov [rsp+10],rbx | push rsi | sub rsp,20
        static constexpr std::uint8_t kClosePrologue[10] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20 };
        if (g_o1) {  // close only matters for wrist opens
            if (auto* s = detour(kPlayCloseAnim, kClosePrologue, reinterpret_cast<void*>(&HkPlayCloseAnim), "PipboyManager::PlayPipboyCloseAnim")) {
                g_origClose = reinterpret_cast<Close_t>(s);
                g_o2 = true;
            }
        }
        // O3: mov [rsp+18],r8b | mov [rsp+8],rcx | push r15
        static constexpr std::uint8_t kHoloPrologue[10] = { 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08 };
        if (g_o1) {
            if (auto* s = detour(kPlayHolotapeAnim, kHoloPrologue, reinterpret_cast<void*>(&HkPlayHolotapeAnim),
                    "PipboyManager::PlayPipboyLoadHolotapeAnim")) {
                g_origHolotape = reinterpret_cast<Holotape_t>(s);
                g_o3 = true;
            }
        }
        // Functions called directly: their first bytes must be what was read in Ghidra.
        struct Check { std::uintptr_t rva; std::uint8_t b[6]; const char* what; };
        static constexpr Check kCalled[] = {
            { kOnOpenAnim, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "OnPipboyOpenAnim" },
            { kOnCloseAnim, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 }, "OnPipboyCloseAnim" },
            { kStartAnimGraphListening, { 0x48, 0x83, 0xEC, 0x38, 0x48, 0x89 }, "StartAnimationGraphListening" },
            { kQPipboyActive, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 }, "QPipboyActive" },
            { kActorInPowerArmor, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 }, "ActorInPowerArmor" },
            { kEmptyFixedString, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "BSFixedString empty getter" },
            { kFixedStringAssign, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "BSFixedString::operator=" },
            { kImodStop, { 0x40, 0x53, 0x41, 0x54, 0x41, 0x56 }, "ImageSpaceModifierInstanceForm::Stop" },
        };
        bool calledOk = true;
        for (const auto& c : kCalled) {
            calledOk = BytesMatch(c.rva, c.b, sizeof(c.b), c.what) && calledOk;
        }
        {
            static constexpr std::uint8_t kGbn[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30 };
            g_getByNameOk = BytesMatch(kRendererGetByName, kGbn, sizeof(kGbn), "Interface3D::Renderer::GetByName");  // projected: no scanlines
        }
        if (!calledOk && (g_o1 || g_o2)) {
            // Should not happen with matching prologues; the hooks would call into unknown code. Leave them
            // installed but disabled: every call goes to the original.
            REX::ERROR("pipboy: called-function check failed; wrist and projected open disabled");
            pipboy::SetEnabled(false);
            pipboy::SetProjectedEnabled(false);
        }
        // H3: mov edx,0C ; call RenderEffect
        static constexpr std::uint8_t kH3[10] = { 0xBA, 0x0C, 0x00, 0x00, 0x00, 0xE8, 0x1F, 0x82, 0x74, 0x01 };
        if (BytesMatch(kEffectCallSite - 5, kH3, sizeof(kH3), "RenderEffect(0xC) call")) {
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(base + kEffectCallSite + 1), 4);
            if (kEffectCallSite + 5 + rel != kRenderEffect) {
                REX::ERROR("pipboy: H3 call targets {:X}, expected {:X}; not patching", kEffectCallSite + 5 + rel, kRenderEffect);
            } else {
                g_origEffect = reinterpret_cast<Effect_t>(tramp.write_call<5>(base + kEffectCallSite, &HkRenderEffect));
                g_h3 = true;
                REX::INFO("pipboy H3: call at rva {:X} -> hook (original {:X})", kEffectCallSite,
                    reinterpret_cast<std::uintptr_t>(g_origEffect) - base);
            }
        }
        REX::INFO("pipboy: strings forceClose='{}' paClose='{}' imod='{}'", FsText(reinterpret_cast<const void*>(base + kStrForceClose)),
            FsText(reinterpret_cast<const void*>(base + kStrPowerArmorClose)), FsText(reinterpret_cast<const void*>(base + kStrImodPipboy)));
        summary = std::format("open={} close={} holotape={} h3={} h1={}", g_o1 ? "ok" : "FAILED", g_o2 ? "ok" : "FAILED",
            g_o3 ? "ok" : "FAILED", g_h3 ? "ok" : "FAILED", H1Installed() ? "ok" : "MISSING");
        REX::INFO("pipboy hooks: {} (trampoline {}B used of {}B)", summary, tramp.allocated_size(), tramp.capacity());
        return summary;
    }

    bool PipboyHooksInstalled()
    {
        return g_o1 && g_o2 && g_h3 && H1Installed();
    }

    void SetPipboyWorldViewAvailable(bool a_on)
    {
        g_worldViewAvailable = a_on;
    }

    bool PipboyWristOpen()
    {
        if (!g_direct) {
            return false;
        }
        auto* ui = RE::UI::GetSingleton();
        const bool open = ui && ui->GetMenuOpen("PipboyMenu");
        const auto* m = Mgr();
        const bool busy = m && (m[kActive] || m[kOpening]);
        if (!open && !busy) {
            g_direct = false;
            g_projected = false;
            g_lastValid = false;
            return false;
        }
        return true;
    }

    bool PipboyProjected()
    {
        return g_direct && g_projected;
    }

    bool PipboyProjectedInPowerArmor()
    {
        return g_direct && g_projected && g_projectedPa;
    }

    void SetPipboyPointer(const PipboyPointer& a_p)
    {
        if (!g_direct) {
            return;
        }
        bool queued = false;
        if (a_p.onQuad) {
            const bool moved = !g_lastValid || std::abs(a_p.u - g_last.u) > 0.0005f || std::abs(a_p.v - g_last.v) > 0.0005f;
            if (moved) {
                Queue(1, a_p.u, a_p.v);
                queued = true;
            }
            if (a_p.down && !(g_lastValid && g_last.down)) {
                Queue(2, a_p.u, a_p.v);
                queued = true;
            }
            for (int i = 0; i < std::abs(a_p.wheel); ++i) {
                Queue(4, a_p.u, a_p.v, a_p.wheel > 0 ? 1.0f : -1.0f);
                queued = true;
            }
        }
        if (!a_p.down && g_lastValid && g_last.down) {  // release wherever the ray is now (at the last on-quad position)
            Queue(3, a_p.onQuad ? a_p.u : g_last.u, a_p.onQuad ? a_p.v : g_last.v);
            queued = true;
        }
        PipboyPointer keep = a_p;
        if (!a_p.onQuad && g_lastValid) {  // remember the last on-quad position
            keep.u = g_last.u;
            keep.v = g_last.v;
        }
        g_last = keep;
        g_lastValid = a_p.onQuad || g_lastValid;
        if (queued && !g_taskPending.exchange(true)) {
            // 20 Nov (threading audit): main-idle queue (ThreadQueues.h: UI thread after UI::AdvanceMenus, before Swap renders
            // the Pip-Boy); the F4SE UI task ran on job threads too
            if (!tq::RunOnUI([]() { DrainEvents(); })) {
                g_taskPending = false;
            }
        }
    }

    int PipboyPage()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->GetMenuOpen("PipboyMenu")) {
            return -1;
        }
        const std::uint32_t page = *reinterpret_cast<const std::uint32_t*>(Base() + kPageIndex);
        return page < 5 ? static_cast<int>(page) : -1;
    }

    int PipboyTab()
    {
        const int page = PipboyPage();
        if (page < 0) return -1;
        const std::uint32_t tab = *reinterpret_cast<const std::uint32_t*>(Base() + kTabPerPage + 4 * static_cast<std::uintptr_t>(page));
        return tab < 16 ? static_cast<int>(tab) : -1;
    }

    namespace
    {
        // Pip-Boy light (flat 1.11.240, read in Ghidra 20 Nov). PipboyHandler::HandleEvent (1077010): Pipboy button held
        // >= 0.3 s (2F3B0F8) with fighting controls enabled (1668D20(mgr, 0x40)) and, in power armour, the light-override
        // keyword worn -> TaskQueueInterface::QueueTogglePipboyLight (task 0x67) -> PlayerCharacter::TogglePipboyLight.
        constexpr std::uintptr_t kTogglePipboyLight = 0xD76930;   // PlayerCharacter::TogglePipboyLight(player)
        constexpr std::uintptr_t kIsPipboyLightOn = 0xD769A0;     // PlayerCharacter::IsPipboyLightOn(player): player+0xC38 != 0
        constexpr std::uintptr_t kInputEnabled2 = 0x1668D20;      // BSInputEnableManager (2268249)(mgr, flags)
        constexpr std::uintptr_t kInputEnableMgr = 0x30E88B0;     // BSInputEnableManager*
        constexpr std::uintptr_t kLightOverrideKw = 0x374850;     // PowerArmor::GetPipboyLightOverrideKeyword()
        constexpr std::uintptr_t kWornHasKeyword = 0x507940;      // TESObjectREFR::WornHasKeyword(ref, kw)
        struct LightCheck
        {
            std::uintptr_t rva;
            std::uint8_t   b[8];
            const char*    what;
        };
        constexpr LightCheck kLightChecks[] = {
            { kTogglePipboyLight, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B }, "TogglePipboyLight" },
            { kIsPipboyLightOn, { 0x48, 0x83, 0xB9, 0x38, 0x0C, 0x00, 0x00, 0x00 }, "IsPipboyLightOn" },
            { kInputEnabled2, { 0x40, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28 }, "BSInputEnableManager check" },
            { kLightOverrideKw, { 0x48, 0x8B, 0x05, 0x41, 0x1A, 0xD8, 0x02, 0x48 }, "GetPipboyLightOverrideKeyword" },
            { kWornHasKeyword, { 0x48, 0x83, 0xEC, 0x58, 0x33, 0xC0, 0x48, 0x89 }, "WornHasKeyword" },
            { kActorInPowerArmor, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 }, "ActorInPowerArmor" },
        };
        std::atomic<int>           g_lightOk{ -1 };
        std::atomic<std::uint64_t> g_lightReq{ 0 }, g_lightToggles{ 0 }, g_lightRefused{ 0 };
        std::mutex                 g_lightLock;
        std::string                g_lightLast{ "-" };  // under g_lightLock

        bool LightFnsOk()
        {
            int ok = g_lightOk.load();
            if (ok < 0) {
                ok = 1;
                for (const auto& c : kLightChecks) {
                    if (!BytesMatch(c.rva, c.b, sizeof(c.b), c.what)) ok = 0;
                }
                g_lightOk = ok;
                REX::INFO("pipboy light: engine calls {}", ok ? "verified (direct toggle)" : "differ: the pad B hold is used");
            }
            return ok != 0;
        }
    }

    bool RequestTogglePipboyLight(const char* a_why)
    {
        if (!LightFnsOk()) {
            return false;
        }
        ++g_lightReq;
        tq::RunOnMain([why = std::string(a_why ? a_why : "")]() {  // main-idle queue: the engine runs it as a main-thread task too
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            const auto b = Base();
            void*      mgr = *reinterpret_cast<void**>(b + kInputEnableMgr);
            using En_t = bool (*)(void*, std::uint32_t);
            const bool fighting = mgr && reinterpret_cast<En_t>(b + kInputEnabled2)(mgr, 0x40);
            const bool pa = Fn<ActorBool_t>(kActorInPowerArmor)(player);
            bool       paKw = true;
            if (pa) {
                void* kw = reinterpret_cast<void* (*)()>(b + kLightOverrideKw)();
                paKw = kw && reinterpret_cast<bool (*)(void*, void*)>(b + kWornHasKeyword)(player, kw);
            }
            using On_t = bool (*)(void*);
            const bool before = reinterpret_cast<On_t>(b + kIsPipboyLightOn)(player);
            std::string line;
            if (!fighting || !paKw) {
                ++g_lightRefused;
                line = std::format("{}: not toggled (fighting controls {}, power armour {} light keyword {}) light {}", why, fighting ? "on" : "off",
                    pa ? 1 : 0, paKw ? 1 : 0, before ? "on" : "off");
            } else {
                reinterpret_cast<void (*)(void*)>(b + kTogglePipboyLight)(player);
                ++g_lightToggles;
                const bool after = reinterpret_cast<On_t>(b + kIsPipboyLightOn)(player);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(player);
                line = std::format("{}: TogglePipboyLight light {} -> {} (BSLight {} node {}){}", why, before ? "on" : "off", after ? "on" : "off",
                    *reinterpret_cast<void* const*>(raw + 0xC38), *reinterpret_cast<void* const*>(raw + 0xC40),
                    before == after ? " - NO CHANGE: the light form / 3D was not found (ShowPipboyLight made no light)" : "");
            }
            REX::INFO("pipboy light: {}", line);
            std::scoped_lock l(g_lightLock);
            g_lightLast = std::move(line);
        });
        return true;
    }

    std::string PipboyLightReport()
    {
        bool on = false;
        if (LightFnsOk()) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                on = reinterpret_cast<bool (*)(void*)>(Base() + kIsPipboyLightOn)(player);
            }
        }
        std::scoped_lock l(g_lightLock);
        return std::format("light engine={} on={} requests={} toggles={} refused={} last: {}", g_lightOk.load(), on ? 1 : 0, g_lightReq.load(),
            g_lightToggles.load(), g_lightRefused.load(), g_lightLast);
    }

    std::string PipboyGameReport()
    {
        const auto base = Base();
        const std::uint32_t page = *reinterpret_cast<const std::uint32_t*>(base + kPageIndex);
        const std::uint32_t tab = page < 5 ? *reinterpret_cast<const std::uint32_t*>(base + kTabPerPage + 4 * page) : 0xFFFFFFFF;
        const auto*         m = Mgr();
        std::string         last, decision;
        {
            std::scoped_lock l(g_evLock);
            last = g_lastInject;
            decision = g_lastDecision;
        }
        return std::format("hooks={} wrist={} projected={} projectedOpens={} decision='{}' page={} tab={} mgr opening={} closing={} active={} opens={} wristOpens={} "
                           "closes={} wristCloses={} holotapes={} holotapesImmediate={} h3Calls={} h3Copies={} h3Last=eff{:X}/src{:X}/dst{:X} sent move={} down={} up={} wheel={} dropped={} lastInject {}",
            PipboyHooksInstalled() ? 1 : 0, g_direct ? 1 : 0, g_projected ? 1 : 0, g_projectedOpens.load(), decision, page, tab, m ? m[kOpening] : -1, m ? m[kClosing] : -1,
            m ? m[kActive] : -1, g_openCalls.load(), g_directOpens.load(), g_closeCalls.load(), g_directCloses.load(), g_holotapeCalls.load(), g_holotapeForced.load(), g_h3Calls.load(),
            g_h3Pip.load(), g_h3Last.load() >> 32, (g_h3Last.load() >> 16) & 0xFFFF, g_h3Last.load() & 0xFFFF, g_sent[1].load(), g_sent[2].load(), g_sent[3].load(), g_sent[4].load(), g_dropped.load(), last);
    }
}
