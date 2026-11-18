#include "plugin/GameHud.h"
#include "plugin/HudLayer.h"
#include "plugin/Dialogue.h"
#include "plugin/PipboyLayer.h"
#include "plugin/Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <mutex>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <Scaleform/G/GFx_Viewport.h>

#include <Windows.h>
#undef ERROR

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

    // ---- HUDFramework guard (analysis/mod-edi-hudframework.md; [HUD] bHudFrameworkFix, default 1) ----------------------
    // HUDFramework (mod 20309) parents its widget container under HUDMenu root1.BottomCenterGroup_mc.CompassWidget_mc.
    // CompassBar_mc and sets the container matrix ONCE to the inverse of CompassBar_mc's transform.concatenatedMatrix, so
    // widget x/y (set through globalToLocal) are 1280x720 HUD stage coordinates and widgets draw at their own scale. That
    // only holds if concatenatedMatrix is the pure display-list chain. Here, once a second on the UI thread, the chain is
    // rebuilt from each ancestor's transform.matrix (never touched by the viewport). If chain x container is not the
    // identity, the container gets the true inverse and each widget's x/y is moved so its anchor stays where it landed
    // (globalToLocal already used the display-list chain): only a wrong scale / rotation is removed. Power armour
    // (HUDFramework moves the widgets into the crosshair clips) is left alone. Every change of the widget layout is
    // logged ("hudfw:"): viewport, matrices, each widget's stage bounds and its angle on the HUD layer.
    namespace
    {
        using HValue = Scaleform::GFx::Value;
        struct M2
        {
            double a{ 1 }, b{ 0 }, c{ 0 }, d{ 1 }, tx{ 0 }, ty{ 0 };  // flash.geom.Matrix: x' = a x + c y + tx, y' = b x + d y + ty
        };
        M2 Mul(const M2& p, const M2& q)  // q first, then p
        {
            return { p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d, p.a * q.tx + p.c * q.ty + p.tx,
                p.b * q.tx + p.d * q.ty + p.ty };
        }
        bool Inv(const M2& m, M2& o)
        {
            const double det = m.a * m.d - m.b * m.c;
            if (std::abs(det) < 1e-12) {
                return false;
            }
            o.a = m.d / det, o.b = -m.b / det, o.c = -m.c / det, o.d = m.a / det;
            o.tx = -(o.a * m.tx + o.c * m.ty), o.ty = -(o.b * m.tx + o.d * m.ty);
            return true;
        }
        double HNum(const HValue& a_v, double a_def = 0.0)
        {
            switch (a_v.GetType()) {
            case HValue::ValueType::kNumber: return a_v.GetNumber();
            case HValue::ValueType::kInt: return a_v.GetInt();
            case HValue::ValueType::kUInt: return a_v.GetUInt();
            case HValue::ValueType::kBoolean: return a_v.GetBoolean() ? 1.0 : 0.0;
            default: return a_def;
            }
        }
        double HMem(const HValue& a_o, const char* a_n, double a_def = 0.0)
        {
            HValue v;
            return a_o.IsObject() && a_o.GetMember(a_n, &v) ? HNum(v, a_def) : a_def;
        }
        std::string HStr(const HValue& a_o, const char* a_n)
        {
            HValue v;
            return a_o.IsObject() && a_o.GetMember(a_n, &v) && v.IsString() ? std::string(v.GetString()) : std::string("-");
        }
        bool MatrixOf(const HValue& a_m, M2& a_out)
        {
            if (!a_m.IsObject()) {
                return false;
            }
            a_out = { HMem(a_m, "a", 1), HMem(a_m, "b"), HMem(a_m, "c"), HMem(a_m, "d", 1), HMem(a_m, "tx"), HMem(a_m, "ty") };
            return true;
        }
        bool TransformMatrix(const HValue& a_obj, const char* a_which, M2& a_out)
        {
            HValue t, m;
            return a_obj.IsObject() && a_obj.GetMember("transform", &t) && t.IsObject() && t.GetMember(a_which, &m) && MatrixOf(m, a_out);
        }
        // Display-list chain of a_obj up to (not including) the stage: what concatenatedMatrix should be.
        bool Chain(const HValue& a_obj, M2& a_out)
        {
            M2     acc;
            HValue o = a_obj;
            for (int i = 0; i < 32; ++i) {
                HValue p;
                if (!o.GetMember("parent", &p) || !p.IsObject()) {
                    a_out = acc;
                    return true;  // o is the stage
                }
                M2 m;
                if (!TransformMatrix(o, "matrix", m)) {
                    return false;
                }
                acc = Mul(m, acc);
                o = p;
            }
            return false;
        }
        std::string MStr(const M2& m) { return std::format("[{:.4f} {:.4f} {:.4f} {:.4f} {:.1f} {:.1f}]", m.a, m.b, m.c, m.d, m.tx, m.ty); }
        double      LinDev(const M2& m) { return (std::max)({ std::abs(m.a - 1), std::abs(m.d - 1), std::abs(m.b), std::abs(m.c) }); }

        std::atomic<int>                      g_hfOn{ -1 };
        std::atomic<bool>                     g_hfPending{ false };
        std::chrono::steady_clock::time_point g_hfNext{};
        std::atomic<std::uint64_t>            g_hfChecks{ 0 }, g_hfFixes{ 0 };
        std::string                           g_hfLastLog;  // UI thread
        std::mutex                            g_hfLock;
        std::string                           g_hfStatus{ "not checked" };  // under g_hfLock

        void HfCheck()  // F4SE UI task (main thread)
        {
            g_hfPending = false;
            ++g_hfChecks;
            auto*                         ui = RE::UI::GetSingleton();
            static const RE::BSFixedString kHud("HUDMenu");
            Scaleform::Ptr<RE::IMenu>     menu = ui ? ui->GetMenu(kHud) : nullptr;
            if (!menu || !menu->uiMovie) {
                return;
            }
            auto*  mv = menu->uiMovie.get();
            HValue hf;
            if (!mv->GetVariable(&hf, "root1.HUDFramework") || !hf.IsObject()) {
                std::scoped_lock l(g_hfLock);
                g_hfStatus = "HUDFramework not in HUDMenu";
                return;
            }
            HValue pa;
            if (hf.GetMember("_isInPA", &pa) && pa.IsBoolean() && pa.GetBoolean()) {
                std::scoped_lock l(g_hfLock);
                g_hfStatus = "power armour (widgets on the crosshair clips; not checked)";
                return;
            }
            HValue bar, cont;
            if (!mv->GetVariable(&bar, "root1.BottomCenterGroup_mc.CompassWidget_mc.CompassBar_mc") || !bar.IsObject()) {
                return;
            }
            if (!hf.GetMember("widgetContainer", &cont) || !cont.IsObject()) {  // not reachable by name: CompassBar_mc's last child
                const int n = static_cast<int>(HMem(bar, "numChildren"));
                HValue    idx(n - 1);
                if (n <= 0 || !bar.Invoke("getChildAt", &cont, &idx, 1) || !cont.IsObject()) {
                    return;
                }
            }
            M2 S, C, K;
            if (!Chain(bar, S) || !TransformMatrix(cont, "matrix", C)) {
                return;
            }
            const bool hasK = TransformMatrix(bar, "concatenatedMatrix", K);
            const M2   P = Mul(S, C);  // container -> HUD stage
            const bool off = LinDev(P) > 0.002 || std::abs(P.tx) > 0.5 || std::abs(P.ty) > 0.5;
            bool       fixed = false;
            M2         Cn;
            if (off && g_hfOn.load() == 1 && Inv(S, Cn)) {
                HValue args[6] = { HValue(Cn.a), HValue(Cn.b), HValue(Cn.c), HValue(Cn.d), HValue(Cn.tx), HValue(Cn.ty) };
                HValue nm, t;
                mv->CreateObject(&nm, "flash.geom.Matrix", args, 6);
                if (nm.IsObject() && cont.GetMember("transform", &t) && t.IsObject() && t.SetMember("matrix", nm)) {
                    const int n = static_cast<int>(HMem(cont, "numChildren"));
                    for (int i = 0; i < n && i < 64; ++i) {  // keep each anchor where it landed: new local = P * old local
                        HValue w, idx(i);
                        if (!cont.Invoke("getChildAt", &w, &idx, 1) || !w.IsObject()) {
                            continue;
                        }
                        const double x = HMem(w, "x"), y = HMem(w, "y");
                        w.SetMember("x", HValue(P.a * x + P.c * y + P.tx));
                        w.SetMember("y", HValue(P.b * x + P.d * y + P.ty));
                    }
                    fixed = true;
                    ++g_hfFixes;
                }
            }
            // layout line: viewport, matrices, each widget's stage bounds and its angle on the HUD layer
            Scaleform::GFx::Viewport vp{};
            mv->GetViewport(&vp);
            HValue root;
            mv->GetVariable(&root, "root1");
            const auto&  hc = hud::GetConfig();
            const double bw = vp.bufferWidth > 0 ? vp.bufferWidth : vp.width, bh = vp.bufferHeight > 0 ? vp.bufferHeight : vp.height;
            const double qw = hc.widthMeters, qh = bw > 0 ? qw * bh / bw : qw * 9.0 / 16.0, qd = hc.distanceMeters > 0.05f ? hc.distanceMeters : 1.0;
            std::string  s = std::format("viewport buf {}x{} rect {},{} {}x{} | chain {} container {} -> stage {}{}{}", vp.bufferWidth, vp.bufferHeight,
                 vp.left, vp.top, vp.width, vp.height, MStr(S), MStr(C), MStr(P), hasK ? " concatenated " + MStr(K) : std::string{},
                 fixed ? " | FIXED: container set to the chain inverse" : off ? " | off (bHudFrameworkFix=0 or not invertible)" : " | ok");
            const int n = static_cast<int>(HMem(cont, "numChildren"));
            s += std::format(" | widgets={}", n);
            for (int i = 0; i < n && i < 24; ++i) {
                HValue w, idx(i), r;
                if (!cont.Invoke("getChildAt", &w, &idx, 1) || !w.IsObject()) {
                    continue;
                }
                double bx = 0, by = 0, bwid = 0, bhei = 0;
                if (root.IsObject() && w.Invoke("getBounds", &r, &root, 1) && r.IsObject()) {
                    bx = HMem(r, "x"), by = HMem(r, "y"), bwid = HMem(r, "width"), bhei = HMem(r, "height");
                }
                // ExactFit HUD (HUDMenu LoadMovie scale mode 2): HUD stage 1280x720 -> the whole layer quad
                const double u = (bx + bwid * 0.5) / 1280.0, v = (by + bhei * 0.5) / 720.0;
                const double yaw = std::atan2((u - 0.5) * qw, qd) * 57.29578, pitch = std::atan2(hc.heightMeters + (0.5 - v) * qh, qd) * 57.29578;
                s += std::format(" | #{} '{}' x={:.0f} y={:.0f} scale={:.2f}x{:.2f} vis={} stage bounds {:.0f},{:.0f} {:.0f}x{:.0f} -> layer {:+.1f}/{:+.1f} deg",
                    i, HStr(w, "name"), HMem(w, "x"), HMem(w, "y"), HMem(w, "scaleX", 1), HMem(w, "scaleY", 1), HMem(w, "visible", 1) != 0 ? 1 : 0, bx, by,
                    bwid, bhei, yaw, pitch);
            }
            {
                std::scoped_lock l(g_hfLock);
                g_hfStatus = s;
            }
            if (s != g_hfLastLog) {  // log on change only
                g_hfLastLog = s;
                REX::INFO("hudfw: {}", s);
            }
        }
    }

    void HudFrameworkTick()
    {
        int on = g_hfOn.load();
        if (on < 0) {
            on = ::GetPrivateProfileIntA("HUD", "bHudFrameworkFix", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
            g_hfOn = on;
            REX::INFO("hudfw: HUDFramework widget guard {} ([HUD] bHudFrameworkFix; the layout is logged either way)", on ? "on" : "off");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < g_hfNext || g_hfPending.exchange(true)) {
            return;
        }
        g_hfNext = now + std::chrono::seconds(1);
        if (const auto tasks = F4SE::GetTaskInterface()) {
            tasks->AddUITask([]() { HfCheck(); });
        } else {
            g_hfPending = false;
        }
    }

    std::string HudFrameworkStatus()
    {
        std::scoped_lock l(g_hfLock);
        return std::format("hudfw on={} checks={} fixes={} | {}", g_hfOn.load(), g_hfChecks.load(), g_hfFixes.load(), g_hfStatus);
    }
}
