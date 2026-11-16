#include "plugin/GameHud.h"
#include "plugin/HudLayer.h"
#include "plugin/Dialogue.h"
#include "plugin/PipboyLayer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <mutex>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

namespace fo4flatvr::game
{
    namespace
    {
        // 1.11.240 RVAs (analysis/vr-hud.md).
        constexpr std::uintptr_t kDrawPostFXCallSite = 0xA3D3C5;   // in RenderPrepassesAndMenus (A3CE30)
        constexpr std::uintptr_t kDrawPostFX = 0xA3D920;
        constexpr std::uintptr_t kRenderMainCallSite = 0xA3CD30;   // in RenderAll (A3CBE0)
        constexpr std::uintptr_t kRenderMain = 0xA3D450;
        constexpr std::uintptr_t kHudCrosshairVtbl = 0x252F2C8;
        constexpr std::size_t    kUpdateDisplayObjectSlot = 4;
        constexpr std::uintptr_t kUpdateDisplayObject = 0x9BD280;
        constexpr std::uintptr_t kCrosshairSetting = 0x2F2E058;    // Setting: vtbl, value (+8), key (+0x10)
        constexpr std::uintptr_t kRendererNameOffset = 0x208;     // Interface3D::Renderer name (BSFixedString)
        constexpr std::uintptr_t kRtHandleOffset = 0xDF4;         // RenderTargetManager: u32 handle per RT id
        constexpr int            kRtGlass = 0x3E;
        constexpr int            kRtMovie = 0x3F;

        using DrawPostFX_t = void (*)(void*);
        using RenderMain_t = void (*)(void*, std::uint32_t);
        using UpdateDisplayObject_t = void (*)(void*);
        DrawPostFX_t          g_origDrawPostFX = nullptr;
        RenderMain_t          g_origRenderMain = nullptr;
        UpdateDisplayObject_t g_origCrosshairUpdate = nullptr;
        bool                  g_h1 = false, g_h2 = false, g_c1 = false;
        std::uint8_t*         g_crosshairByte = nullptr;

        std::atomic<std::uint64_t> g_h1Calls{ 0 }, g_h1Hud{ 0 }, g_h2Calls{ 0 }, g_h2Skips{ 0 }, g_c1Calls{ 0 }, g_c1Hidden{ 0 };

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        bool IsHudRenderer(const void* a_renderer)
        {
            if (!a_renderer) {
                return false;
            }
            const auto* name = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(a_renderer) + kRendererNameOffset);
            const char* s = name->c_str();
            return s && _stricmp(s, "HUDScreenModel") == 0;
        }

        bool IsPipboyRenderer(const void* a_renderer)
        {
            if (!a_renderer) {
                return false;
            }
            const auto* name = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(a_renderer) + kRendererNameOffset);
            const char* s = name->c_str();
            return s && _stricmp(s, "PipboyMenu") == 0;
        }

        const char* RendererName(const void* a_renderer)
        {
            if (!a_renderer) {
                return "";
            }
            const char* s = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(a_renderer) + kRendererNameOffset)->c_str();
            return s ? s : "";
        }

        // Menu 3D item previews and other menu-owned Interface3D renderers (vr-hud.md: Container3D, WorkbenchItem3D,
        // ExamineMenuItem3D, WorkshopMenu3D, LockpickingMenu3D, book/scope models): every renderer that is not the HUD,
        // the flat menu, the Pip-Boy, the power-armour dash / rain or the loading-screen background.
        bool IsPreviewRenderer(const char* a_name)
        {
            static constexpr const char* kNot[] = { "HUDScreenModel", "FlatScreenModel", "PipboyMenu", "PowerArmorRenderer", "HUDRainRenderer",
                "BackgroundScreenModel" };
            if (!a_name || !*a_name) {
                return false;
            }
            for (const char* n : kNot) {
                if (_stricmp(a_name, n) == 0) {
                    return false;
                }
            }
            return true;
        }

        // Power armour (analysis/vr-power-armor.md): renderers set up by PowerArmorGeometry::InitializePAHUDGeom
        // (1052C90: screen-attached PA dash, HUD glass, kFullFrame) and the visor rain renderer.
        std::atomic<std::uint64_t> g_h2Pa{ 0 }, g_h2PaCaptured{ 0 }, g_h2Rain{ 0 }, g_h2RainSkips{ 0 }, g_h2PipSkips{ 0 };
        std::mutex                 g_seenLock;
        std::string                g_seen;  // renderer names seen at H2 (render order of the first frame each appeared)

        void NoteRenderer(const char* a_name)
        {
            std::scoped_lock l(g_seenLock);
            const std::string tag = std::string(" ") + a_name + " ";
            if ((" " + g_seen + " ").find(tag) == std::string::npos && g_seen.size() < 600) {
                g_seen += (g_seen.empty() ? "" : " ") + std::string(a_name);
                REX::INFO("hud H2: renderer '{}' seen", a_name);
            }
        }

        // ID3D11Texture2D* / ID3D11RenderTargetView* of an engine render target id (any target the handle table maps).
        bool RtViews(std::uint32_t a_id, void*& a_tex, void*& a_rtv)
        {
            auto* rtm = RE::BSGraphics::RenderTargetManager::GetSingleton();
            auto* data = RE::BSGraphics::GetRendererData();
            if (!rtm || !data || a_id >= 101) {
                return false;
            }
            const auto handle = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(rtm) + kRtHandleOffset + 4 * a_id);
            if (handle >= 101) {
                return false;
            }
            a_tex = data->renderTargets[handle].texture;
            a_rtv = data->renderTargets[handle].rtView;
            return a_tex && a_rtv;
        }

        void* g_flatRtv[2]{};  // render thread: RTVs of 0x3E / 0x3F during the last FlatScreenModel draw (H1)
        void* g_hudRtv[2]{};   // ... during the last HUDScreenModel draw (H1)

        // HUD elements that disappear (pickup text, HIDDEN/CAUTION) must not linger in the HUD glass feedback: the next
        // frame's HUD starts from RenderEffect 0x43 on the pooled targets instead of a clear (A3CE30).
        void ClearHudTargets()
        {
            if (hud::ClearHudTargetsWanted() && (g_hudRtv[0] || g_hudRtv[1])) {
                hud::ClearMenuTargets(g_hudRtv[0], g_hudRtv[1]);
            }
            g_hudRtv[0] = g_hudRtv[1] = nullptr;
        }

        void ClearFlatTargets()
        {
            if (hud::ClearMenuTargetsWanted() && (g_flatRtv[0] || g_flatRtv[1])) {
                hud::ClearMenuTargets(g_flatRtv[0], g_flatRtv[1]);
            }
            g_flatRtv[0] = g_flatRtv[1] = nullptr;
        }

        struct PaDrawCtx
        {
            void*         renderer;
            std::uint32_t dest;
        };

        void* RtTexture(int a_id)
        {
            auto* rtm = RE::BSGraphics::RenderTargetManager::GetSingleton();
            auto* data = RE::BSGraphics::GetRendererData();
            if (!rtm || !data) {
                return nullptr;
            }
            const auto handle = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(rtm) + kRtHandleOffset + 4 * a_id);
            if (handle >= 101) {  // -1: pooled and not acquired
                return nullptr;
            }
            return data->renderTargets[handle].texture;
        }

        // H1
        void HkDrawPostFX(void* a_renderer)
        {
            g_origDrawPostFX(a_renderer);
            ++g_h1Calls;
            // VR dialogue (D-cap): the FlatScreenModel draw while DialogueMenu is its only menu = the option panel
            if (_stricmp(RendererName(a_renderer), "FlatScreenModel") == 0) {
                // the pooled glass targets this menu draw used: cleared after its RenderMain (pause-menu decal, HudLayer.h)
                void* tex = nullptr;
                g_flatRtv[0] = RtViews(kRtGlass, tex, g_flatRtv[0]) ? g_flatRtv[0] : nullptr;
                g_flatRtv[1] = RtViews(kRtMovie, tex, g_flatRtv[1]) ? g_flatRtv[1] : nullptr;
                if (dlg::CaptureWanted()) {
                    dlg::OnFlatTargets(RtTexture(kRtGlass), RtTexture(kRtMovie));
                }
                // Menu panel (flat 2D menus over the live world view): every FlatScreenModel draw while it is on.
                if (hud::MenuPanelWanted()) {
                    hud::OnMenuTargets(RtTexture(kRtGlass), RtTexture(kRtMovie));
                }
            }
            if (IsHudRenderer(a_renderer)) {
                ++g_h1Hud;
                void* tex = nullptr;
                g_hudRtv[0] = RtViews(kRtGlass, tex, g_hudRtv[0]) ? g_hudRtv[0] : nullptr;
                g_hudRtv[1] = RtViews(kRtMovie, tex, g_hudRtv[1]) ? g_hudRtv[1] : nullptr;
                hud::OnHudTargets(RtTexture(kRtGlass), RtTexture(kRtMovie));
            } else if (pipboy::WantsTargets()) {
                // Wrist Pip-Boy (D3): for renderer "PipboyMenu" the offscreen 0x3D holds the menu + post FX now; the
                // final 0x3C is written after this (RenderEffect 0xC, call A3D3EC, hooked in GamePipboy.cpp), which
                // only copies it when this H1 said the renderer was "PipboyMenu".
                const bool isPip = IsPipboyRenderer(a_renderer);
                pipboy::OnRendererPostFX(isPip, isPip ? RtTexture(0x3D) : nullptr);
            }
        }

        // H2
        void HkRenderMain(void* a_renderer, std::uint32_t a_dest)
        {
            ++g_h2Calls;
            if (IsHudRenderer(a_renderer)) {
                if (hud::SkipWorldHud()) {
                    ++g_h2Skips;
                    hud::NoteWorldHudSkipped();
                } else {
                    g_origRenderMain(a_renderer, a_dest);
                }
                ClearHudTargets();
                return;
            }
            const char* name = RendererName(a_renderer);
            NoteRenderer(name);
            if (_stricmp(name, "FlatScreenModel") == 0) {
                if (dlg::SkipWorldFlat()) {  // dialogue options on their own panel
                    dlg::NoteWorldFlatSkipped();
                } else if (hud::SkipWorldMenu()) {  // menu panel: the menu is on its own quad
                    hud::NoteWorldMenuSkipped();
                } else {
                    g_origRenderMain(a_renderer, a_dest);
                }
                ClearFlatTargets();  // nothing of this menu may reach the HUD's glass targets (pause-menu decal)
                return;
            }
            // 3D item preview of a panel menu (Container3D, WorkbenchItem3D, ExamineMenuItem3D, LockpickingMenu3D, ...):
            // onto the panel with the menu, not screen-space into the (double-wide stereo) world image.
            if (hud::MenuPreviewWanted() && IsPreviewRenderer(name)) {
                void*     tex = nullptr;
                void*     rtv = nullptr;
                PaDrawCtx ctx{ a_renderer, a_dest };
                if (RtViews(a_dest, tex, rtv) && hud::CaptureMenuPreview(name, tex, rtv, [](void* p) {
                        const auto* c = static_cast<const PaDrawCtx*>(p);
                        g_origRenderMain(c->renderer, c->dest);
                    }, &ctx)) {
                    return;
                }
            }
            if (_stricmp(name, "PowerArmorRenderer") == 0) {
                ++g_h2Pa;
                if (hud::PaHudOnLayer()) {
                    void *tex = nullptr, *rtv = nullptr;
                    PaDrawCtx ctx{ a_renderer, a_dest };
                    if (RtViews(a_dest, tex, rtv) &&
                        hud::CapturePaOverlay(tex, rtv, [](void* p) {
                            const auto* c = static_cast<const PaDrawCtx*>(p);
                            g_origRenderMain(c->renderer, c->dest);
                        }, &ctx)) {
                        ++g_h2PaCaptured;
                        return;  // drawn twice into the (restored) destination: not in the world image
                    }
                }
            } else if (_stricmp(name, "HUDRainRenderer") == 0) {
                ++g_h2Rain;
                if (hud::HideVisorRain()) {
                    ++g_h2RainSkips;
                    return;
                }
            } else if (_stricmp(name, "PipboyMenu") == 0 && pipboy::SkipWorldScreen()) {
                ++g_h2PipSkips;  // projected Pip-Boy in power armour: its screen is on our quad, not on the PA dash
                return;
            }
            g_origRenderMain(a_renderer, a_dest);
        }

        // C1
        void HkCrosshairUpdate(void* a_this)
        {
            ++g_c1Calls;
            if (g_crosshairByte && hud::HideCrosshair()) {
                ++g_c1Hidden;
                const std::uint8_t saved = *g_crosshairByte;
                *g_crosshairByte = 0;
                g_origCrosshairUpdate(a_this);
                *g_crosshairByte = saved;
                return;
            }
            g_origCrosshairUpdate(a_this);
        }

        bool CallMatches(std::uintptr_t a_site, std::uintptr_t a_target, const std::uint8_t (&a_bytes)[8], const char* a_what)
        {
            // 3 bytes of context before the E8 (mov rcx,...) + the 5-byte call
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_site - 3);
            if (std::memcmp(p, a_bytes, sizeof(a_bytes)) != 0) {
                std::string got, want;
                for (std::size_t i = 0; i < sizeof(a_bytes); ++i) {
                    got += std::format("{:02X} ", p[i]);
                    want += std::format("{:02X} ", a_bytes[i]);
                }
                REX::ERROR("hud: {} at {:X} has bytes [{}], expected [{}]; not patching", a_what, a_site - 3, got, want);
                return false;
            }
            std::int32_t rel;
            std::memcpy(&rel, p + 4, 4);
            if (a_site + 5 + rel != a_target) {
                REX::ERROR("hud: {} call at {:X} targets {:X}, expected {:X}; not patching", a_what, a_site, a_site + 5 + rel, a_target);
                return false;
            }
            return true;
        }
    }

    std::string InstallHudHooks()
    {
        static std::string summary;
        if (!summary.empty()) {
            return summary;
        }
        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) {
            REX::WARN("hud: F4SE trampoline not set up; creating a local one");
            tramp.create(128);
        }
        const auto base = Base();

        // H1: mov rcx,r14 ; call DrawPostFX
        static constexpr std::uint8_t kH1[8] = { 0x49, 0x8B, 0xCE, 0xE8, 0x56, 0x05, 0x00, 0x00 };
        if (CallMatches(kDrawPostFXCallSite, kDrawPostFX, kH1, "Interface3D::Renderer::DrawPostFX")) {
            g_origDrawPostFX = reinterpret_cast<DrawPostFX_t>(tramp.write_call<5>(base + kDrawPostFXCallSite, &HkDrawPostFX));
            g_h1 = true;
            REX::INFO("hud H1: call at rva {:X} -> hook (original {:X})", kDrawPostFXCallSite,
                reinterpret_cast<std::uintptr_t>(g_origDrawPostFX) - base);
        }
        // H2: mov rcx,rdi ; call RenderMain
        static constexpr std::uint8_t kH2[8] = { 0x48, 0x8B, 0xCF, 0xE8, 0x1B, 0x07, 0x00, 0x00 };
        if (CallMatches(kRenderMainCallSite, kRenderMain, kH2, "Interface3D::Renderer::RenderMain")) {
            g_origRenderMain = reinterpret_cast<RenderMain_t>(tramp.write_call<5>(base + kRenderMainCallSite, &HkRenderMain));
            g_h2 = true;
            REX::INFO("hud H2: call at rva {:X} -> hook (original {:X})", kRenderMainCallSite,
                reinterpret_cast<std::uintptr_t>(g_origRenderMain) - base);
        }
        // C1: HUDCrosshair::UpdateDisplayObject vtable slot; the setting object must carry the expected key.
        {
            auto*       slot =reinterpret_cast<std::uintptr_t*>(base + kHudCrosshairVtbl) + kUpdateDisplayObjectSlot;
            const auto* setting = reinterpret_cast<const std::uint8_t*>(base + kCrosshairSetting);
            const char* key = *reinterpret_cast<const char* const*>(setting + 0x10);
            if (*slot != base + kUpdateDisplayObject) {
                REX::ERROR("hud C1: HUDCrosshair vtable slot {} holds {:X}, expected rva {:X}; not patching", kUpdateDisplayObjectSlot,
                    *slot - base, kUpdateDisplayObject);
            } else if (!key || std::strncmp(key, "bCrosshairEnabled", 17) != 0) {
                REX::ERROR("hud C1: setting at rva {:X} has key '{}', expected bCrosshairEnabled; not patching", kCrosshairSetting,
                    key ? key : "(null)");
            } else {
                g_crosshairByte = const_cast<std::uint8_t*>(setting + 8);
                REL::Relocation<std::uintptr_t> vtbl{ REL::Offset(kHudCrosshairVtbl) };
                g_origCrosshairUpdate = reinterpret_cast<UpdateDisplayObject_t>(vtbl.write_vfunc(kUpdateDisplayObjectSlot, &HkCrosshairUpdate));
                g_c1 = true;
                REX::INFO("hud C1: HUDCrosshair vtable slot {} -> hook; setting '{}' value {}", kUpdateDisplayObjectSlot, key,
                    static_cast<int>(*g_crosshairByte));
            }
        }
        summary = std::format("h1={} h2={} crosshair={}", g_h1 ? "ok" : "FAILED", g_h2 ? "ok" : "FAILED", g_c1 ? "ok" : "FAILED");
        REX::INFO("hud hooks: {} (trampoline {}B used of {}B)", summary, tramp.allocated_size(), tramp.capacity());
        return summary;
    }

    void* RenderTargetTexture(int a_id)
    {
        return RtTexture(a_id);
    }

    bool H1Installed()
    {
        return g_h1;
    }

    bool HudHooksInstalled()
    {
        return g_h1 && g_h2;
    }

    std::string HudHookStats()
    {
        std::string seen;
        {
            std::scoped_lock l(g_seenLock);
            seen = g_seen;
        }
        return std::format("h1Calls={} h1Hud={} h2Calls={} h2Skips={} crosshairCalls={} crosshairHidden={} crosshairPref={} "
                           "h2PowerArmor={} paCaptured={} h2Rain={} rainSkips={} pipScreenSkips={} renderers=[{}]",
            g_h1Calls.load(), g_h1Hud.load(), g_h2Calls.load(), g_h2Skips.load(), g_c1Calls.load(), g_c1Hidden.load(),
            g_crosshairByte ? static_cast<int>(*g_crosshairByte) : -1, g_h2Pa.load(), g_h2PaCaptured.load(), g_h2Rain.load(),
            g_h2RainSkips.load(), g_h2PipSkips.load(), seen);
    }

    std::string MenusReport()
    {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            return "error: no task interface";
        }
        auto done = std::make_shared<std::promise<std::string>>();
        auto fut = done->get_future();
        tasks->AddTask([done]() {
            std::string s;
            auto*       ui = RE::UI::GetSingleton();
            if (!ui) {
                done->set_value("error: no UI");
                return;
            }
            // Main thread: the render passes walk this same array, in this order, on this thread.
            const auto& stack = ui->menuStack;
            // UI counters (1.11.240): +0x1E0 menu mode [I: FO4VR UpdateVRUI freezes the UI anchor while != 0],
            // +0x1EC freeze-frame menu background (UseFreezeFrameMenuBackground 1A80480), +0x1F0 freeze-frame pause
            // (UseFreezeFrameDuringGamePause 1A804A0).
            const auto* u = reinterpret_cast<const std::uint8_t*>(ui);
            const auto  ctr = [u](std::size_t o) { return *reinterpret_cast<const std::int32_t*>(u + o); };
            s = std::format("n={} ui1E0={} ui1E4={} ui1E8={} ui1EC={} ui1F0={} ui1F4={} ui1F8={}", stack.size(), ctr(0x1E0), ctr(0x1E4),
                ctr(0x1E8), ctr(0x1EC), ctr(0x1F0), ctr(0x1F4), ctr(0x1F8));
            for (const auto& m : stack) {
                if (!m) {
                    s += " | (null)";
                    continue;
                }
                const auto* raw = reinterpret_cast<const std::uint8_t*>(m.get());
                const auto  flags = m->menuFlags.underlying();
                const char* name = m->menuName.c_str();
                const char* rend = m->customRendererName.c_str();
                s += std::format(" | {} flags=0x{:X} custom={} renderer={} depth={} onStack={} advanced={}", name && *name ? name : "?",
                    flags, (flags & 0x8000) ? 1 : 0, rend && *rend ? rend : "-", static_cast<int>(raw[0x64]),  // a byte in 1.11.240 (the enum's upper bytes are garbage)
                    raw[0x60], raw[0x63]);
            }
            done->set_value(std::move(s));
        });
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            return "error: main thread did not run the task within 2 s";
        }
        return fut.get();
    }
}
