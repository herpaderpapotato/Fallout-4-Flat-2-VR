#include "plugin/GameMenuPointer.h"
#include "plugin/CameraDirector.h"
#include "plugin/GameBridge.h"
#include "plugin/HudLayer.h"
#include "plugin/Settings.h"
#include "plugin/ThreadQueues.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <future>
#include <memory>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <vector>
#include <format>
#include <mutex>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <Scaleform/G/GFx_MovieDef.h>
#include <Scaleform/G/GFx_Viewport.h>

#include <Windows.h>
#undef ERROR

namespace fo4flatvr::game
{
    namespace
    {
        // 1.11.240 (analysis/vr-hud.md section 3; UI::GetTopMenu 141A80500 read in Ghidra for this feature).
        constexpr std::uintptr_t kMenuStackLock = 0x3DBD368;  // BSReadWriteLock taken by GetTopMenu / ProcessMessages
        constexpr int            kTopMenuDepthLimit = 0x15;   // GetTopMenu(ui, out, 0x15) for "TopMenu" messages
        constexpr std::uint32_t  kUsesCursor = 0x4, kUsesMenuContext = 0x8, kModal = 0x10;
        // Pip-Boy screen colour (ImageSpaceEffectPipboyScreen::UpdateParams 2233B40).
        constexpr std::uintptr_t kPipColorValue[3] = { 0x2F98BD8, 0x2F98BF0, 0x2F98C08 };  // Setting value (+8)
        constexpr std::uintptr_t kPipBrightness = 0x2F98BC4;                               // cb2[1].w

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        RE::BSReadWriteLock& StackLock() { return *reinterpret_cast<RE::BSReadWriteLock*>(Base() + kMenuStackLock); }

        bool Excluded(const char* a_name)
        {
            // ButtonBarMenu: the button-hint bar sits above every menu (depth 16) and carries kUsesCursor, but only
            // passes events on. PipboyMenu (flat fallback): its movie is drawn onto a 3D screen mesh, so uv over the
            // back buffer does not map to the movie. CursorMenu: the mouse cursor itself.
            static constexpr const char* kNames[] = { "ButtonBarMenu", "PipboyMenu", "CursorMenu" };
            for (const char* n : kNames) {
                if (_stricmp(a_name, n) == 0) {
                    return true;
                }
            }
            return false;
        }

        // ---- any-menu pass ([Menus] bPointerAnyMenu, default 1) ----------------------------------------------------
        // An interactive menu the stack walk below cannot see by kUsesMenuContext / kUsesCursor (gamepad mode). Taken:
        // a menu that can own a cursor or the input even with a pad (kAssignCursorToRenderer 0x10000, kUpdateUsesCursor
        // 0x400, kModal 0x10, kPausesGame 0x1), or a mod / F4SE menu (IMenu vtable outside Fallout4.exe) with
        // kCustomRendering. Never: HUD-only and non-interactive overlays (HUDMenu, FaderMenu, VignetteMenu, LoadingMenu,
        // PromptMenu) and VATSMenu (its own window / hand targeting; a mouse click would pick limbs and the trigger
        // fires), plus Excluded() above.
        std::atomic<int> g_anyMenu{ -1 };
        std::mutex       g_anyLock;
        std::string      g_anySeen;  // under g_anyLock: |name| list, logged once each

        bool AnyMenuPass()
        {
            int e = g_anyMenu.load();
            if (e < 0) {
                e = ::GetPrivateProfileIntA("Menus", "bPointerAnyMenu", 1, Settings::Path().c_str()) != 0 ? 1 : 0;
                g_anyMenu = e;
                REX::INFO("pointer: any-menu pass {} ([Menus] bPointerAnyMenu)", e ? "on" : "off");
            }
            return e != 0;
        }

        bool OutsideExe(const void* a_menu)
        {
            static std::uintptr_t lo = 0, hi = 0;
            if (!lo) {
                const auto  base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                lo = base, hi = base + nt->OptionalHeader.SizeOfImage;
            }
            const auto vt = *reinterpret_cast<const std::uintptr_t*>(a_menu);
            return vt < lo || vt >= hi;
        }

        bool AnyMenuInteractive(const void* a_menu, const char* a_name, std::uint32_t a_flags)
        {
            static constexpr const char* kNot[] = { "HUDMenu", "FaderMenu", "VignetteMenu", "LoadingMenu", "PromptMenu", "VATSMenu" };
            for (const char* n : kNot) {
                if (_stricmp(a_name, n) == 0) {
                    return false;
                }
            }
            if (Excluded(a_name)) {
                return false;
            }
            constexpr std::uint32_t kOwnsInput = 0x10000 | 0x400 | 0x10 | 0x1;  // AssignCursorToRenderer, UpdateUsesCursor, Modal, PausesGame
            return (a_flags & kOwnsInput) != 0 || ((a_flags & 0x8000) && OutsideExe(a_menu));
        }

        // Caller holds no lock. Returns the target menu (AddRef'd via Ptr) or null.
        // Measured (14 Nov, gamepad active): MainMenu 0x80C8AC9 and PauseMenu 0x84C8E59 do NOT carry kUsesCursor
        // (0x4) while a gamepad is the input device, ButtonBarMenu 0x84CD44 does. So the first pass looks for
        // kUsesMenuContext (0x8: the menu pushes the menu input context, i.e. it is the one being navigated), the
        // second for kUsesCursor; both from the top, below depth 0x15, stopping at a modal menu.
        Scaleform::Ptr<RE::IMenu> FindTarget(MenuPointerTarget* a_info)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return nullptr;
            }
            Scaleform::Ptr<RE::IMenu> found;
            auto&                     lock = StackLock();
            lock.lock_read();
            const auto& stack = ui->menuStack;
            for (const std::uint32_t want : { kUsesMenuContext, kUsesCursor }) {
                for (std::uint32_t i = stack.size(); i-- > 0;) {
                    const auto& m = stack[i];
                    if (!m) {
                        continue;
                    }
                    const auto* raw = reinterpret_cast<const std::uint8_t*>(m.get());
                    const int   depth = raw[0x64];  // a byte in 1.11.240 (see MenusReport)
                    if (depth >= kTopMenuDepthLimit) {
                        continue;
                    }
                    const std::uint32_t flags = m->menuFlags.underlying();
                    const char*         name = m->menuName.c_str();
                    if (!name) {
                        name = "";
                    }
                    if ((flags & want) && m->uiMovie && !Excluded(name)) {
                        found = m;
                        if (a_info) {
                            a_info->valid = true;
                            a_info->name = name;
                            a_info->flags = flags;
                            a_info->depth = depth;
                            if (_stricmp(name, "TerminalMenu") == 0) {
                                a_info->terminalMode = *reinterpret_cast<const int*>(raw + 0x240);
                            }
                        }
                        break;
                    }
                    if (flags & kModal) {
                        break;  // the engine does not pass events below a modal menu
                    }
                }
                if (found) {
                    break;
                }
            }
            // Third pass (analysis/mod-edi-hudframework.md section 4): in gamepad mode some interactive menus carry
            // neither flag, e.g. DialogueMenu 0x18800 (kUsesCursor only without a pad, UpdateInputContext 1043FE0).
            std::string anyNew;
            if (!found && AnyMenuPass()) {
                for (std::uint32_t i = stack.size(); i-- > 0;) {
                    const auto& m = stack[i];
                    if (!m) {
                        continue;
                    }
                    const auto* raw = reinterpret_cast<const std::uint8_t*>(m.get());
                    const int   depth = raw[0x64];
                    if (depth >= kTopMenuDepthLimit) {
                        continue;
                    }
                    const std::uint32_t flags = m->menuFlags.underlying();
                    const char*         name = m->menuName.c_str();
                    if (!name) {
                        name = "";
                    }
                    if (m->uiMovie && AnyMenuInteractive(m.get(), name, flags)) {
                        found = m;
                        if (a_info) {
                            a_info->valid = true;
                            a_info->name = name;
                            a_info->flags = flags;
                            a_info->depth = depth;
                        }
                        std::scoped_lock l(g_anyLock);
                        if (g_anySeen.find(std::format("|{}|", name)) == std::string::npos) {
                            g_anySeen += std::format("|{}|", name);
                            anyNew = std::format("'{}' flags=0x{:X} depth={}", name, flags, depth);
                        }
                        break;
                    }
                    if (flags & kModal) {
                        break;
                    }
                }
            }
            lock.unlock_read();
            if (!anyNew.empty()) {
                REX::INFO("pointer: {} targeted by the any-menu pass (no kUsesMenuContext/kUsesCursor; [Menus] bPointerAnyMenu)", anyNew);
            }
            return found;
        }

        // ---- 3D-screen menus (TerminalMenu): back-buffer uv -> movie uv -------------------------------------
        // TerminalMenu's movie (renderer "TerminalMenu", viewport 876x700) is drawn offscreen and shown on the terminal's
        // "Screen:0" mesh. The engine maps a mouse event the same way in TerminalMenu::ProcessMessage (B0C480, case
        // kScaleformEvent, mouse types 1..4, only without a gamepad): UIUtils::SetScreenGeometry("Screen:0" of the
        // terminal ref's GetCurrent3D) + UIUtils::PickScreen(world camera, 0, 0, 0) -> the hit's texture uv x viewport
        // buffer size. PickScreen (1087930) reads the cursor from MenuCursor (+0x24/+0x28 ints, render-size pixels
        // 3D87220/24) and needs MenuCursor +0x50 != 0; the uv lands in 2F3B920/24. We set the cursor to the pointer's
        // pixel for the call and restore every field afterwards.
        constexpr std::uintptr_t kTerminalRef = 0x328FC68;      // TESObjectREFR* of the terminal in use (TerminalMenu)
        constexpr std::uintptr_t kWorldRootPtr = 0x32DD338;     // Main::WorldRootNode; camera at +0x140
        constexpr std::uintptr_t kMenuCursorPtr = 0x327A568;    // MenuCursor*
        constexpr std::uintptr_t kRenderW = 0x3D87220, kRenderH = 0x3D87224;
        constexpr std::uintptr_t kScreenGeom = 0x3313688;       // UIUtils screen geometry (SetScreenGeometry 1087920 stores here)
        constexpr std::uintptr_t kPickU = 0x2F3B920, kPickV = 0x2F3B924;
        constexpr std::uintptr_t kPickScreen = 0x1087930;
        constexpr std::uintptr_t kGetObjectByName = 0x17A0F50; // BSUtilities::GetObjectByName(root, name, deep, flag)
        constexpr std::uintptr_t kSetScreenGeometry = 0x1087920;
        using PickScreen_t = bool (*)(void*, bool, float, float);
        using GetObjectByName_t = RE::NiAVObject* (*)(RE::NiAVObject*, const RE::BSFixedString&, bool, bool);

        int  g_pickOk = -1;  // -1 unchecked, 0 bytes differ (no 3D-screen mapping), 1 ok
        std::atomic<std::uint64_t> g_picks{ 0 }, g_pickHits{ 0 };

        bool PickCodeOk()
        {
            if (g_pickOk < 0) {
                const auto* b = reinterpret_cast<const std::uint8_t*>(Base());
                static constexpr std::uint8_t kPick[] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08 };
                static constexpr std::uint8_t kGob[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 };
                static constexpr std::uint8_t kSsg[] = { 0x48, 0x89, 0x0D };
                std::int32_t rel = 0;
                std::memcpy(&rel, b + kSetScreenGeometry + 3, 4);
                const bool ok = std::memcmp(b + kPickScreen, kPick, sizeof(kPick)) == 0 &&
                                std::memcmp(b + kGetObjectByName, kGob, sizeof(kGob)) == 0 &&
                                std::memcmp(b + kSetScreenGeometry, kSsg, sizeof(kSsg)) == 0 &&
                                kSetScreenGeometry + 7 + rel == kScreenGeom;
                g_pickOk = ok ? 1 : 0;
                if (ok) {
                    REX::INFO("pointer: 3D-screen pick (TerminalMenu) code verified");
                } else {
                    REX::ERROR("pointer: PickScreen/GetObjectByName/SetScreenGeometry bytes differ; TerminalMenu uses flat uv");
                }
            }
            return g_pickOk == 1;
        }

        // Main thread. true + movie uv if the pointer hits the terminal's screen mesh.
        bool PickTerminalScreen(float a_u, float a_v, float& a_mu, float& a_mv)
        {
            if (!PickCodeOk()) {
                return false;
            }
            const auto base = Base();
            auto*      term = *reinterpret_cast<RE::TESObjectREFR**>(base + kTerminalRef);
            auto*      world = *reinterpret_cast<std::uint8_t**>(base + kWorldRootPtr);
            auto*      cursor = *reinterpret_cast<std::uint8_t**>(base + kMenuCursorPtr);
            if (!term || !world || !cursor) {
                return false;
            }
            void* camera = *reinterpret_cast<void**>(world + 0x140);
            RE::NiAVObject* root = term->GetCurrent3D();
            if (!camera || !root) {
                return false;
            }
            static const RE::BSFixedString kScreen("Screen:0");
            RE::NiAVObject* screen = reinterpret_cast<GetObjectByName_t>(base + kGetObjectByName)(root, kScreen, true, false);
            if (!screen) {
                return false;
            }
            const int w = *reinterpret_cast<const int*>(base + kRenderW), h = *reinterpret_cast<const int*>(base + kRenderH);
            if (w <= 0 || h <= 0) {
                return false;
            }
            auto& geom = *reinterpret_cast<void**>(base + kScreenGeom);
            auto& cx = *reinterpret_cast<int*>(cursor + 0x24);
            auto& cy = *reinterpret_cast<int*>(cursor + 0x28);
            auto& cOn = *reinterpret_cast<int*>(cursor + 0x50);
            void* const oldGeom = geom;
            const int   ox = cx, oy = cy, oOn = cOn;
            geom = screen;
            cx = static_cast<int>(a_u * static_cast<float>(w));
            cy = static_cast<int>(a_v * static_cast<float>(h));
            if (cOn == 0) {
                cOn = 1;
            }
            const bool hit = reinterpret_cast<PickScreen_t>(base + kPickScreen)(camera, false, 0.0f, 0.0f);
            cx = ox, cy = oy, cOn = oOn;
            geom = oldGeom;
            ++g_picks;
            if (!hit) {
                return false;
            }
            ++g_pickHits;
            a_mu = *reinterpret_cast<const float*>(base + kPickU);
            a_mv = *reinterpret_cast<const float*>(base + kPickV);
            return std::isfinite(a_mu) && std::isfinite(a_mv);
        }

        std::string g_lastPick{ "none" };  // under g_lock (declared before use below)

        // Scaleform 4 GFx::MouseEvent (BSGFxEventBuffer::GetMouseEvent 1B1E1E0 fills the same layout).
        struct GfxMouseEvent
        {
            std::uint32_t type;  // 1 MouseMove, 2 MouseDown, 3 MouseUp, 4 MouseWheel
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
            float         u, v, wheel;
            bool          movieUv{ false };  // world-screen pick: already movie uv
        };
        bool g_queueMovieUv = false;  // main thread: movieUv of the events SetMenuPointer queues now
        std::atomic<bool> g_vpFix{ true };  // 28 Nov: [Menus] bPointerViewportFix (events relative to the movie's viewport)
        // 28 Nov (PA/LooksMenu agent): [Menus] bLooksMenuPointerCursor. FO4VR moves the face-sculpt cursor only from the menu
        // cursor (VR LooksMenu::HandleEvent(MouseMoveEvent) 140B61270 is MoveSculptCursor's only caller; the wand drives the
        // cursor). Our pointer talks to the movie directly (GFx mouse events), so the engine's LooksMenu never heard it and the
        // cursor stayed at 550,360. Each pointer move over LooksMenu now sets the menu cursor (MenuCursor singleton 327A568,
        // +0x24/+0x28 screen px) and runs flat's own handler (LooksMenu::HandleEvent(MouseMoveEvent) A81A20 on the menu's
        // BSInputEventUser at +0x10): ShouldMoveCursor -> MoveSculptCursor(x, y, mouse) with its letterbox rule, the pick, and
        // the mouse sculpt / rotate branches exactly as for a real mouse.
        std::atomic<bool>          g_looksCursor{ true };
        std::atomic<std::uint64_t> g_looksMoves{ 0 };
        int                        g_looksOk = -1;  // prologue checked (-1 unknown)
        constexpr std::uintptr_t   kMenuCursorSingleton = 0x327A568;  // MenuCursor* (LooksMenu mouse handler A81A3F)
        constexpr std::uintptr_t   kScreenW = 0x3D87218, kScreenH = 0x3D8721C;  // screen px (MoveSculptCursor A85B81 / A85BC9)
        constexpr std::uintptr_t   kLooksMouseMove = 0xA81A20;     // LooksMenu::HandleEvent(MouseMoveEvent) (this = menu + 0x10)
        bool CallLooksMouseMove(void* a_user, void* a_ev)
        {
            using Fn = void (*)(void*, void*);
            __try {
                reinterpret_cast<Fn>(REL::Offset(kLooksMouseMove).address())(a_user, a_ev);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        void LooksMenuCursor(RE::IMenu* a_menu, float a_u, float a_v)  // UI task (main thread)
        {
            const auto b = REL::Offset(0).address();
            if (g_looksOk < 0) {
                static constexpr std::uint8_t kPro[] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49, 0x89, 0x73, 0x10 };
                g_looksOk = std::memcmp(reinterpret_cast<const void*>(b + kLooksMouseMove), kPro, sizeof(kPro)) == 0 ? 1 : 0;
                REX::INFO("menu pointer: LooksMenu sculpt cursor via the game's mouse handler {}", g_looksOk ? "ready" : "OFF (prologue mismatch)");
            }
            if (g_looksOk != 1 || !a_menu) return;
            auto* mc = *reinterpret_cast<std::uint8_t**>(b + kMenuCursorSingleton);
            const auto w = *reinterpret_cast<const std::uint32_t*>(b + kScreenW), h = *reinterpret_cast<const std::uint32_t*>(b + kScreenH);
            if (!mc || w == 0 || h == 0) return;
            *reinterpret_cast<std::int32_t*>(mc + 0x24) = static_cast<std::int32_t>(std::clamp(a_u, 0.0f, 1.0f) * static_cast<float>(w));
            *reinterpret_cast<std::int32_t*>(mc + 0x28) = static_cast<std::int32_t>(std::clamp(a_v, 0.0f, 1.0f) * static_cast<float>(h));
            alignas(16) std::uint8_t ev[0x60]{};  // the handler only writes +0x24 (handled) in its rotate branch
            if (CallLooksMouseMove(reinterpret_cast<std::uint8_t*>(a_menu) + 0x10, ev)) {
                ++g_looksMoves;
            } else {
                g_looksOk = 0;
                REX::WARN("menu pointer: LooksMenu mouse handler faulted - sculpt cursor drive off");
            }
        }

        std::mutex                 g_lock;  // g_events, g_downMenu, report strings
        std::deque<QueuedEvent>    g_events;
        std::atomic<bool>          g_taskPending{ false };
        MenuPointerInput           g_last;
        bool                       g_lastValid = false;
        bool                       g_down = false;      // a MouseDown was queued and no MouseUp yet
        std::string                g_downMenu;          // menu that received the MouseDown (UI task)
        std::string                g_lastTarget{ "-" };  // main thread
        MenuPointerTarget          g_lastInfo;           // under g_lock: target seen by the last SetMenuPointer
        std::atomic<std::uint64_t> g_sent[5]{};
        std::atomic<std::uint64_t> g_dropped{ 0 }, g_targetChanges{ 0 };
        std::string                g_lastEvent{ "none" };
        std::string                g_lastViewport{ "none" };
        std::string                g_forced;  // under g_lock: menu named by a world panel (dialogue), else empty

        // The forced menu (SetMenuPointer a_forceMenu) if set and open, else the stack walk.
        Scaleform::Ptr<RE::IMenu> ResolveTarget(MenuPointerTarget* a_info)
        {
            std::string forced;
            {
                std::scoped_lock l(g_lock);
                forced = g_forced;
            }
            if (forced.empty()) {
                return FindTarget(a_info);
            }
            auto* ui = RE::UI::GetSingleton();
            Scaleform::Ptr<RE::IMenu> m = ui ? ui->GetMenu(RE::BSFixedString(forced.c_str())) : nullptr;
            if (!m || !m->OnStack() || !m->uiMovie) {
                return nullptr;
            }
            if (a_info) {
                a_info->valid = true;
                a_info->name = forced;
                a_info->flags = m->menuFlags.underlying();
                a_info->depth = reinterpret_cast<const std::uint8_t*>(m.get())[0x64];
            }
            return m;
        }

        // The button-hint bar (ButtonBarMenu: depth 16 over every menu, kUsesCursor) holds the menus' bottom buttons
        // (ACCEPT / BACK / ...). The engine's mouse path ("TopMenu" = GetTopMenu) offers every mouse event to it first
        // and passes it down when not consumed; here the bar's movie gets the same events as the target menu (its
        // clickable clips are only the bar's buttons, so the two never both react to one click).
        std::atomic<std::uint64_t> g_barSent{ 0 };
        bool                       g_barDown = false;  // UI task: the bar got the MouseDown
        std::string                g_barLast{ "none" };  // under g_lock

        // ---- button-bar hints (AS3) -----------------------------------------------------------------------------
        // ButtonBarMenu (1.10 ctor B05A80): movie "ButtonBarMenu", object path "root1", bar clip
        // "root1.ButtonBarHolder_mc.ButtonHintBar_mc"; each hint is a clone whose AS3 onTextClick calls the owning menu's
        // hint (ButtonHintDataWithClone B05270). With a gamepad as the input device the hints get no click from a mouse
        // event [measured 14 Nov: clicks on the bar change nothing]. So after a pointer click has been delivered, the hint
        // under the stage mouse is found with hitTestPoint and gets a synthetic flash MouseEvent "click" (the same AS3
        // listener chain as a real click), unless the bar is in the keyboard/mouse platform (0), where the real one works.
        using GValue = Scaleform::GFx::Value;

        double Num(const GValue& a_v, double a_def = 0.0)
        {
            switch (a_v.GetType()) {
            case GValue::ValueType::kNumber: return a_v.GetNumber();
            case GValue::ValueType::kInt: return a_v.GetInt();
            case GValue::ValueType::kUInt: return a_v.GetUInt();
            case GValue::ValueType::kBoolean: return a_v.GetBoolean() ? 1.0 : 0.0;
            default: return a_def;
            }
        }

        std::string Str(const GValue& a_v)
        {
            return a_v.IsString() ? std::string(a_v.GetString()) : a_v.IsUndefined() ? std::string("undef") : std::format("{}", Num(a_v, -1.0));
        }

        double MemberNum(const GValue& a_obj, const char* a_name, double a_def = -1.0)
        {
            GValue v;
            return a_obj.IsObject() && a_obj.GetMember(a_name, &v) ? Num(v, a_def) : a_def;
        }

        std::string MemberStr(const GValue& a_obj, const char* a_name)
        {
            GValue v;
            return a_obj.IsObject() && a_obj.GetMember(a_name, &v) ? Str(v) : std::string("-");
        }

        // 28 Nov (menus agent): a movie's geometry for the pipe probe "pointer movie [<Menu>]": viewport, scale mode,
        // alignment, visible frame rect (stage units), the SWF's own size, the AS3 stage, root1, and where the stage
        // corners land in viewport pixels (TranslateToScreen).
        std::string MovieGeometry(const char* a_name, Scaleform::GFx::Movie* a_mv)
        {
            Scaleform::GFx::Viewport vp{};
            a_mv->GetViewport(&vp);
            const auto  vis = a_mv->GetVisibleFrameRect();
            const auto  safe = a_mv->GetSafeRect();
            float       dw = -1.0f, dh = -1.0f;
            if (auto* def = a_mv->GetMovieDef()) {
                dw = def->GetWidth(), dh = def->GetHeight();
            }
            Scaleform::Render::Point<float> p0{ 0.0f, 0.0f }, p1{ dw, dh };
            const auto s0 = a_mv->TranslateToScreen(p0, nullptr);
            const auto s1 = a_mv->TranslateToScreen(p1, nullptr);
            float mx = 0.0f, my = 0.0f;
            std::uint32_t mb = 0;
            a_mv->GetMouseState(0, &mx, &my, &mb);
            GValue root, stage;
            std::string st = "no root1";
            if (a_mv->GetVariable(&root, "root1") && root.IsObject()) {
                st = std::format("root1 x={} y={} sx={} sy={} w={} h={}", MemberNum(root, "x"), MemberNum(root, "y"), MemberNum(root, "scaleX"),
                    MemberNum(root, "scaleY"), MemberNum(root, "width"), MemberNum(root, "height"));
                if (root.GetMember("stage", &stage) && stage.IsObject()) {
                    st += std::format(" stage {}x{} scaleMode={} align='{}' mouse {:.1f},{:.1f}", MemberNum(stage, "stageWidth"),
                        MemberNum(stage, "stageHeight"), MemberStr(stage, "scaleMode"), MemberStr(stage, "align"), MemberNum(stage, "mouseX"),
                        MemberNum(stage, "mouseY"));
                }
            }
            return std::format("{}: vp buf {}x{} rect {},{} {}x{} sc {},{} {}x{} flags 0x{:X} scale {:.3f} aspect {:.3f} | mode {} align {} | def {}x{} "
                               "vis {:.1f},{:.1f}..{:.1f},{:.1f} safe {:.1f},{:.1f}..{:.1f},{:.1f} | stage(0,0)->{:.1f},{:.1f} stage(def)->{:.1f},{:.1f} "
                               "| gfxMouse {:.1f},{:.1f} | {}",
                a_name, vp.bufferWidth, vp.bufferHeight, vp.left, vp.top, vp.width, vp.height, vp.scissorLeft, vp.scissorTop, vp.scissorWidth,
                vp.scissorHeight, vp.flags.underlying(), vp.scale, vp.aspectRatio, static_cast<int>(a_mv->GetViewScaleMode()),
                static_cast<int>(a_mv->GetViewAlignment()), dw, dh, vis.x1, vis.y1, vis.x2, vis.y2, safe.x1, safe.y1, safe.x2, safe.y2, s0.x,
                s0.y, s1.x, s1.y, mx, my, st);
        }

        // Walks the bar's children; a_report: one entry per child. Returns the index of the visible child under the stage
        // mouse (hitTestPoint, shape) or -1; a_hit receives it.
        int BarHints(Scaleform::GFx::Movie* a_mv, std::string* a_report, GValue* a_hit, double* a_platform)
        {
            GValue bar, stage, root;
            if (!a_mv || !a_mv->GetVariable(&bar, "root1.ButtonBarHolder_mc.ButtonHintBar_mc") || !bar.IsObject()) {
                if (a_report) *a_report = "no ButtonHintBar_mc";
                return -1;
            }
            double mx = -1, my = -1;
            if (a_mv->GetVariable(&root, "root1") && root.IsObject() && root.GetMember("stage", &stage) && stage.IsObject()) {
                mx = MemberNum(stage, "mouseX"), my = MemberNum(stage, "mouseY");
            }
            if (a_platform) {
                *a_platform = MemberNum(bar, "uiPlatform", MemberNum(bar, "_uiPlatform", -1.0));
            }
            const int n = static_cast<int>(MemberNum(bar, "numChildren", 0.0));
            if (a_report) {
                *a_report = std::format("stage mouse {:.1f},{:.1f} bar x={:.1f} y={:.1f} w={:.1f} h={:.1f} visible={} mouseEnabled={} mouseChildren={} platform={} children={}",
                    mx, my, MemberNum(bar, "x"), MemberNum(bar, "y"), MemberNum(bar, "width"), MemberNum(bar, "height"), MemberStr(bar, "visible"),
                    MemberStr(bar, "mouseEnabled"), MemberStr(bar, "mouseChildren"), a_platform ? *a_platform : -1.0, n);
            }
            int hitIdx = -1;
            for (int i = 0; i < n && i < 40; ++i) {
                GValue child, idx(i);
                if (!bar.Invoke("getChildAt", &child, &idx, 1) || !child.IsObject()) {
                    continue;
                }
                bool hit = false;
                if (mx >= 0) {
                    GValue args[3] = { GValue(mx), GValue(my), GValue(true) }, r;
                    hit = child.Invoke("hitTestPoint", &r, args, 3) && r.IsBoolean() && r.GetBoolean();
                }
                const std::string vis = MemberStr(child, "visible");
                if (hit && vis == "1" && hitIdx < 0) {
                    hitIdx = i;
                    if (a_hit) *a_hit = child;
                }
                if (a_report) {
                    *a_report += std::format(" | #{} '{}' text='{}' vis={} enabled={} x={:.1f} y={:.1f} w={:.1f} h={:.1f} mouseEnabled={} hit={}", i,
                        MemberStr(child, "name"), MemberStr(child, "ButtonText"), vis, MemberStr(child, "ButtonEnabled"), MemberNum(child, "x"),
                        MemberNum(child, "y"), MemberNum(child, "width"), MemberNum(child, "height"), MemberStr(child, "mouseEnabled"), hit ? 1 : 0);
                }
            }
            return hitIdx;
        }

        // Display objects under the stage mouse of a movie (AS3 stage.getObjectsUnderPoint), each with its parent chain:
        // name, ButtonText, has ButtonHintData, mouseEnabled. a_hintData: the first ButtonHintData object found on a chain
        // (a BSButtonHint under the pointer).
        std::string ObjectsUnderMouse(Scaleform::GFx::Movie* a_mv, GValue* a_hintData, GValue* a_hint)
        {
            GValue root, stage;
            if (!a_mv || !a_mv->GetVariable(&root, "root1") || !root.IsObject() || !root.GetMember("stage", &stage) || !stage.IsObject()) {
                return "no root1.stage";
            }
            const double mx = MemberNum(stage, "mouseX"), my = MemberNum(stage, "mouseY");
            GValue pt;
            GValue pargs[2] = { GValue(mx), GValue(my) };
            a_mv->CreateObject(&pt, "flash.geom.Point", pargs, 2);
            GValue arr;
            if (!pt.IsObject() || !stage.Invoke("getObjectsUnderPoint", &arr, &pt, 1) || !arr.IsArray()) {
                return std::format("stage mouse {:.1f},{:.1f}: getObjectsUnderPoint failed", mx, my);
            }
            std::string s = std::format("stage mouse {:.1f},{:.1f} objects={}", mx, my, arr.GetArraySize());
            bool found = false;
            for (std::uint32_t i = 0; i < arr.GetArraySize() && i < 12; ++i) {
                GValue o;
                arr.GetElement(i, &o);
                s += std::format(" | [{}]", i);
                for (int depth = 0; depth < 10 && o.IsObject(); ++depth) {
                    const bool hasData = o.HasMember("ButtonHintData");
                    s += std::format(" < {}{}", MemberStr(o, "name"), hasData ? std::format("(hint '{}' en={} me={})", MemberStr(o, "ButtonText"),
                                                                                    MemberStr(o, "ButtonEnabled"), MemberStr(o, "mouseEnabled"))
                                                                              : std::string{});
                    if (hasData && !found) {
                        GValue data;
                        if (o.GetMember("ButtonHintData", &data) && data.IsObject()) {
                            found = true;
                            if (a_hintData) *a_hintData = data;
                            if (a_hint) *a_hint = o;
                        }
                    }
                    GValue p;
                    if (!o.GetMember("parent", &p)) {
                        break;
                    }
                    o = p;
                }
            }
            return s;
        }

        // Display tree below an object: name, visible, x/y/w/h, ButtonText if any; depth-limited.
        void TreeDump(const GValue& a_obj, int a_depth, int a_max, std::string& a_out)
        {
            const int n = static_cast<int>(MemberNum(a_obj, "numChildren", 0.0));
            for (int i = 0; i < n && i < 30 && a_out.size() < 6000; ++i) {
                GValue child, idx(i);
                if (!const_cast<GValue&>(a_obj).Invoke("getChildAt", &child, &idx, 1) || !child.IsObject()) {
                    continue;
                }
                a_out += std::format(" |{}{} vis={} x={:.0f} y={:.0f} w={:.0f} h={:.0f}{}", std::string(a_depth, '.'), MemberStr(child, "name"),
                    MemberStr(child, "visible"), MemberNum(child, "x"), MemberNum(child, "y"), MemberNum(child, "width"), MemberNum(child, "height"),
                    child.HasMember("ButtonText") ? " text='" + MemberStr(child, "ButtonText") + "'" : std::string{});
                if (a_depth < a_max) {
                    TreeDump(child, a_depth + 1, a_max, a_out);
                }
            }
        }

        std::atomic<std::uint64_t> g_barSynthClicks{ 0 };
        std::string                g_barHintLast{ "none" };  // under g_lock

        // UI task, right after a MouseUp went to a menu movie (the target or the button bar). Button hints (BSButtonHint:
        // the ACCEPT / CANCEL / BACK prompts of confirm panels and button bars) get no click from a mouse event while the
        // gamepad is the input device [measured 14 Nov: clicks on hints change nothing; list rows work]. If a hint is under
        // the stage mouse (stage.getObjectsUnderPoint, parent chain with a ButtonHintData member), its data object's
        // onTextClick() is called - the function a real click on the hint text calls (1.10 ButtonHintDataWithClone
        // B05270 registers it), i.e. the hint's own action.
        // Measured 14 Nov: real pointer clicks on hints DO work once the bar movie gets the events (CANCEL / CONFIRM in the
        // pause menu's load panel), so this fallback is off by default (it would double the action); diagnostic only.
        std::atomic<bool> g_hintFallback{ false };  // pipe "pointer hints on|off"

        void HintClickFallback(RE::IMenu* a_menu, const char* a_menuName)
        {
            if (!g_hintFallback.load() || !a_menu || !a_menu->uiMovie) {
                return;
            }
            GValue      data, hint;
            const auto  rep = ObjectsUnderMouse(a_menu->uiMovie.get(), &data, &hint);
            if (!data.IsObject()) {
                return;
            }
            GValue     r;
            const bool ok = data.HasMember("onTextClick") && data.Invoke("onTextClick", &r);
            ++g_barSynthClicks;
            const std::string what = std::format("menu '{}' hint '{}' (enabled={}) onTextClick called={} result={}", a_menuName, MemberStr(hint, "ButtonText"),
                MemberStr(hint, "ButtonEnabled"), ok ? 1 : 0, Str(r));
            REX::INFO("pointer: button hint click: {}", what);
            std::scoped_lock l(g_lock);
            g_barHintLast = what;
        }

        void BarClickFallback(RE::IMenu* a_bar) { HintClickFallback(a_bar, "ButtonBarMenu"); }

        void SendToBar(const QueuedEvent& a_e)
        {
            if (a_e.type == 3 && !g_barDown) {
                return;
            }
            auto* ui = RE::UI::GetSingleton();
            static const RE::BSFixedString kBar("ButtonBarMenu");
            Scaleform::Ptr<RE::IMenu> bar = ui ? ui->GetMenu(kBar) : nullptr;
            if (!bar || !bar->OnStack() || !bar->uiMovie) {
                g_barDown = false;
                return;
            }
            Scaleform::GFx::Viewport vp{};
            bar->uiMovie->GetViewport(&vp);
            const float bw = static_cast<float>(vp.bufferWidth > 0 ? vp.bufferWidth : vp.width);
            const float bh = static_cast<float>(vp.bufferHeight > 0 ? vp.bufferHeight : vp.height);
            GfxMouseEvent ev{};
            ev.type = a_e.type;
            ev.x = a_e.u * bw - (g_vpFix.load() ? static_cast<float>(vp.left) : 0.0f);  // 28 Nov: viewport-relative (see DrainEvents)
            ev.y = a_e.v * bh - (g_vpFix.load() ? static_cast<float>(vp.top) : 0.0f);
            ev.scrollDelta = a_e.wheel;
            const auto rc = bar->uiMovie->HandleEvent(*reinterpret_cast<const Scaleform::GFx::Event*>(&ev));
            if (a_e.type == 3 && g_barDown) {
                BarClickFallback(bar.get());
            }
            g_barDown = a_e.type == 2 ? true : a_e.type == 3 ? false : g_barDown;
            ++g_barSent;
            if (a_e.type != 1) {
                std::scoped_lock l(g_lock);
                g_barLast = std::format("type {} x={:.1f} y={:.1f} rc={} buf {}x{}", a_e.type, ev.x, ev.y, static_cast<int>(rc), vp.bufferWidth, vp.bufferHeight);
            }
        }

        // 28 Nov: the mouse-button half of LooksMenu's engine input (flat LooksMenu::HandleEvent(ButtonEvent) A81590, mouse
        // branch; FO4VR 140B60B70 the same): left button -> +0x1F9 on the menu's BSInputEventUser (+0x10) = held (the mouse
        // move handler sculpts with it: SculptModeLStickMouse(delta)), and in face mode (menuMode 1) Invoke SculptMode (the
        // feature under the cursor). Our pointer's trigger = that left button. UI task (main thread).
        std::atomic<std::uint64_t> g_looksButtons{ 0 }, g_looksSculptCalls{ 0 };
        std::atomic<std::uint64_t> g_looksHintSkips{ 0 };
        void LooksMenuButton(RE::IMenu* a_menu, bool a_down)
        {
            if (g_looksOk != 1 || !a_menu) return;
            // a click on a button hint (the pointer clicks those through the movie: HintClickFallback) is not a face click:
            // without this, a hint click in face mode also started sculpting the feature under the cursor (measured)
            if (a_down && a_menu->uiMovie) {
                GValue data;
                const auto rep = ObjectsUnderMouse(a_menu->uiMovie.get(), &data, nullptr);
                // a hint clip under the mouse (its ButtonHintData member is not always readable as an object)
                const bool hintHit = data.IsObject() || rep.find("(hint '") != std::string::npos;
                static int logged = 0;
                if (logged < 12) {
                    ++logged;
                    REX::INFO("menu pointer: LooksMenu click -> {}: {}", hintHit ? "hint (no face click)" : "face click", rep.substr(0, 300));
                }
                if (hintHit) {
                    ++g_looksHintSkips;
                    return;
                }
            }
            *(reinterpret_cast<std::uint8_t*>(a_menu) + 0x10 + 0x1F9) = a_down ? 1 : 0;
            ++g_looksButtons;
            // the release only clears the held flag: the press already started sculpting (menuMode 1 -> 3), and a release
            // after a hint click (press skipped above) must not start it either
            if (!a_down) return;
            GValue mode;
            if (a_menu->menuObj.IsObject() && a_menu->menuObj.GetMember("menuMode", &mode) && static_cast<int>(Num(mode, -1.0)) == 1) {
                a_menu->menuObj.Invoke("SculptMode");
                ++g_looksSculptCalls;
            }
        }

        void DrainEvents()  // F4SE UI task (main thread)
        {
            std::deque<QueuedEvent> evs;
            bool                    forced = false;
            {
                std::scoped_lock l(g_lock);
                evs.swap(g_events);
                forced = !g_forced.empty();
            }
            g_taskPending = false;
            if (evs.empty()) {
                return;
            }
            MenuPointerTarget         info;
            Scaleform::Ptr<RE::IMenu> menu = ResolveTarget(&info);
            const bool                bar = !forced && (!info.valid || _stricmp(info.name.c_str(), "TerminalMenu") != 0);
            for (const auto& e : evs) {
                if (bar) {
                    SendToBar(e);
                }
                Scaleform::Ptr<RE::IMenu> dst = menu;
                std::string               dstName = info.name;
                if (e.type == 3) {  // up: only to the menu that got the down
                    std::string downMenu;
                    {
                        std::scoped_lock l(g_lock);
                        downMenu = g_downMenu;
                        g_downMenu.clear();
                    }
                    if (downMenu.empty()) {
                        ++g_dropped;
                        continue;
                    }
                    if (!menu || downMenu != info.name) {
                        auto* ui = RE::UI::GetSingleton();
                        dst = ui ? ui->GetMenu(RE::BSFixedString(downMenu)) : nullptr;
                        dstName = downMenu;
                        if (!dst || !dst->OnStack()) {
                            ++g_dropped;
                            continue;
                        }
                    }
                }
                if (!dst || !dst->uiMovie) {
                    ++g_dropped;
                    continue;
                }
                Scaleform::GFx::Viewport vp{};
                dst->uiMovie->GetViewport(&vp);
                const float bw = static_cast<float>(vp.bufferWidth > 0 ? vp.bufferWidth : vp.width);
                const float bh = static_cast<float>(vp.bufferHeight > 0 ? vp.bufferHeight : vp.height);
                float mu = e.u, mv = e.v;
                if (_stricmp(dstName.c_str(), "TerminalMenu") == 0 && !e.movieUv) {
                    float pu = 0.0f, pv = 0.0f;
                    const bool hit = PickTerminalScreen(e.u, e.v, pu, pv);
                    {
                        std::scoped_lock l(g_lock);
                        g_lastPick = hit ? std::format("screen uv {:.4f},{:.4f} from {:.4f},{:.4f}", pu, pv, e.u, e.v) :
                                           std::format("miss at {:.4f},{:.4f}", e.u, e.v);
                    }
                    if (hit) {
                        mu = pu, mv = pv;
                    } else if (e.type != 3) {
                        ++g_dropped;  // off the terminal screen: nothing to point at (an up is still delivered)
                        continue;
                    }
                }
                GfxMouseEvent ev{};
                ev.type = e.type;
                // 28 Nov (menus agent; user: "the pointing visual is offset to the left of where it's actually pointing"):
                // GFx takes mouse coordinates relative to the movie's viewport origin (measured: PauseMenu viewport 171,0
                // 4778x2688 in a 5120x2688 buffer; event x 3227.7 -> stage.mouseX 864.7 = 3227.7 * 1280 / 4778). The game
                // letterboxes 16:9 menus inside wider screens (InitMovieViewport 141B1BB30: left = (W - H*16/9) / 2), so the
                // buffer position u*W must lose the viewport's left/top, or the hit lands 'left' px right of the drawn dot.
                // [Menus] bPointerViewportFix.
                ev.x = mu * bw - (g_vpFix.load() ? static_cast<float>(vp.left) : 0.0f);
                ev.y = mv * bh - (g_vpFix.load() ? static_cast<float>(vp.top) : 0.0f);
                ev.scrollDelta = e.wheel;
                const auto rc = dst->uiMovie->HandleEvent(*reinterpret_cast<const Scaleform::GFx::Event*>(&ev));
                if (g_looksCursor.load() && _stricmp(dstName.c_str(), "LooksMenu") == 0) {
                    if (e.type == 1 || e.type == 2) LooksMenuCursor(dst.get(), mu, mv);  // the button acts at the cursor
                    if (e.type == 2 || e.type == 3) LooksMenuButton(dst.get(), e.type == 2);
                }
                if (e.type < 5) {
                    ++g_sent[e.type];
                }
                if (e.type == 3 && _stricmp(dstName.c_str(), "TerminalMenu") != 0) {
                    HintClickFallback(dst.get(), dstName.c_str());  // ACCEPT / CANCEL prompts (button hints) under the pointer
                }
                std::scoped_lock l(g_lock);
                if (e.type == 2) {
                    g_downMenu = dstName;
                }
                static constexpr const char* kType[] = { "?", "move", "down", "up", "wheel" };
                g_lastEvent = std::format("{} {} x={:.1f} y={:.1f}{} rc={}", kType[e.type < 5 ? e.type : 0], dstName, ev.x, ev.y,
                    e.type == 4 ? std::format(" delta={:.0f}", e.wheel) : std::string{}, static_cast<int>(rc));
                g_lastViewport = std::format("buf {}x{} rect {},{} {}x{} scale {:.3f} aspect {:.3f}", vp.bufferWidth, vp.bufferHeight, vp.left,
                    vp.top, vp.width, vp.height, vp.scale, vp.aspectRatio);
            }
        }

        void Queue(std::uint32_t a_type, float a_u, float a_v, float a_wheel = 0.0f)
        {
            std::scoped_lock l(g_lock);
            if (g_events.size() >= 64) {
                g_events.pop_front();
                ++g_dropped;
            }
            g_events.push_back({ a_type, a_u, a_v, a_wheel, g_queueMovieUv });
        }
    }

    namespace
    {
        constexpr std::uintptr_t kPlayerUpdateCamera = 0xD5A670;  // PlayerCharacter::UpdateCamera(PlayerCharacter*)
        int                        g_pucOk = -1;
        std::uint64_t              g_pucLastFp = 0;
        bool                       g_pucTracking = false;
        std::atomic<std::uint64_t> g_pucCalls{ 0 }, g_pucFrames{ 0 };
    }

    bool DriveCameraWhilePaused(bool a_active, std::uint64_t (*a_counter)())
    {
        const std::uint64_t a_fpUpdates = a_counter ? a_counter() : 0;
        if (!a_active) {
            g_pucTracking = false;
            return false;
        }
        ++g_pucFrames;
        if (g_pucOk < 0) {
            static constexpr std::uint8_t kPro[] = { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x1D };
            g_pucOk = std::memcmp(reinterpret_cast<const void*>(Base() + kPlayerUpdateCamera), kPro, sizeof(kPro)) == 0 ? 1 : 0;
            if (g_pucOk) {
                REX::INFO("menu panel: PlayerCharacter::UpdateCamera prologue verified (paused-camera driver available)");
            } else {
                REX::ERROR("menu panel: PlayerCharacter::UpdateCamera bytes differ; the world stays frozen behind pausing menus");
            }
        }
        // First frame of a panel: just remember the count (the game may still be updating it itself).
        if (!g_pucTracking) {
            g_pucTracking = true;
            g_pucLastFp = a_fpUpdates;
            return false;
        }
        const bool stalled = a_fpUpdates == g_pucLastFp;
        bool       ran = false;
        auto*      player = RE::PlayerCharacter::GetSingleton();
        if (stalled && g_pucOk == 1 && player && player->Get3D()) {
            using Fn = void (*)(RE::PlayerCharacter*);
            reinterpret_cast<Fn>(Base() + kPlayerUpdateCamera)(player);
            ++g_pucCalls;
            ran = true;
        }
        g_pucLastFp = ran && a_counter ? a_counter() : a_fpUpdates;
        return ran;
    }

    namespace
    {
        constexpr std::size_t kUiBlurCount = 0x1F8;
        std::int32_t          g_blurHeld = 0;
        std::uint64_t         g_blurFrames = 0;
    }

    void HoldMenuBlur(bool a_active)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        auto& cnt = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(ui) + kUiBlurCount);
        if (a_active) {
            if (cnt != 0) {
                g_blurHeld += cnt;
                cnt = 0;
            }
            ++g_blurFrames;
        } else if (g_blurHeld != 0) {
            cnt += g_blurHeld;  // the game decremented from 0 for menus that closed meanwhile: this rebalances
            g_blurHeld = 0;
        }
    }

    std::string MenuBlurReport()
    {
        auto* ui = RE::UI::GetSingleton();
        const int now = ui ? *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(ui) + kUiBlurCount) : -999;
        return std::format("blur ui1F8={} held={} heldFrames={}", now, g_blurHeld, g_blurFrames);
    }

    std::string FlatOverlayMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return {};
        }
        std::string found;
        auto&       lock = StackLock();
        lock.lock_read();
        const auto& stack = ui->menuStack;
        for (std::uint32_t i = stack.size(); i-- > 0 && found.empty();) {
            const auto& m = stack[i];
            if (!m || !(m->menuFlags.underlying() & 0x8000)) {
                continue;
            }
            const char* r = m->customRendererName.c_str();
            const char* n = m->menuName.c_str();
            if (r && n && _stricmp(r, "FlatScreenModel") == 0 && _stricmp(n, "ButtonBarMenu") != 0) {
                found = n;
            }
        }
        lock.unlock_read();
        return found;
    }

    // 20 Nov (Silhouette's picker: "offset to right and in right eye"): the generic mod-menu panel rule (analysis/mod-sam-vr.md
    // option A2, never built before). A mod / F4SE menu (IMenu vtable outside Fallout4.exe) drawn by FlatScreenModel
    // (kCustomRendering) that takes input - the any-menu pointer pass's test: kUsesCursor / kUsesMenuContext, or
    // kAssignCursorToRenderer / kUpdateUsesCursor / kModal / kPausesGame (Silhouette 0x80184D2 has neither 0x4 nor 0x8) - goes on
    // the anchored menu panel with the pointer, instead of the head-locked overlay that stretches its 16:9 layout over the
    // 140 deg render frustum (a right-hand window lands 50-65 deg right: only the right eye sees it). [Menus] sPanelMenus adds
    // vanilla-vtable names, sNoPanelMenus excludes names (comma lists), bPanelModMenus=0 turns the rule off.
    namespace
    {
        struct ModPanelCfg
        {
            bool        loaded{ false };
            bool        on{ true };
            std::string add, no;  // ",name,name," lower case
        };
        ModPanelCfg      g_modPanel;
        std::mutex       g_modPanelLock;
        std::string      g_modPanelSeen;  // |name| list, logged once each
        std::atomic<int> g_modPanelLive{ -1 };  // pipe override (-1 = ini)

        std::string LowerList(const char* a_key)
        {
            char buf[1024]{};
            ::GetPrivateProfileStringA("Menus", a_key, "", buf, sizeof(buf), Settings::Path().c_str());
            std::string out = ",";
            for (const char* p = buf; *p; ++p) {
                if (*p != ' ' && *p != '\t') out += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            }
            return out + ",";
        }
        bool InList(const std::string& a_list, const char* a_name)
        {
            std::string k = "," + std::string(a_name) + ",";
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return a_list.size() > 2 && a_list.find(k) != std::string::npos;
        }
        std::string OwnerModule(const void* a_menu)
        {
            HMODULE mod = nullptr;
            const auto vt = *reinterpret_cast<const void* const*>(a_menu);
            char       path[MAX_PATH]{};
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(vt), &mod) &&
                ::GetModuleFileNameA(mod, path, MAX_PATH)) {
                const char* b = std::strrchr(path, '\\');
                return b ? b + 1 : path;
            }
            return "?";
        }
    }

    void SetModPanelMenus(int a_on) { g_modPanelLive = a_on; }

    // ---- 28 Nov (menus agent): mod menus at the aspect of the user's flat display ---------------------------------------
    // User: "The right 1/3rd edge of some menus is cut off, such as the screen archer menu." Measured (work/ui/menufix.py):
    // the render buffer is the double-wide 5120x2688 (1.905:1). The game letterboxes its own menus to 16:9 (InitMovieViewport
    // 141B1BB30 with LoadMovie's last argument), but a mod menu like SAM gets the whole buffer with showAll, so its visible
    // stage is 720 x 1.905 = 1371 wide (-45.7..1325.7). SAM lays its slider list at stage 1143..~1450 and its notification
    // at about -190: made for the user's 3440x1440 monitor (visible -220..1500), so both edges are cut on our buffer.
    // Fix: while such a menu (viewport = the whole buffer, not exactFit) is on the menu panel, its viewport gets the aspect
    // of the user's flat display (Fallout4Prefs.ini [Display] iSize W/H, or [Menus] fModMenuAspect): wider -> the full
    // width and a centred band of the height (letterbox), narrower -> a centred column. Put back when it leaves the panel.
    // The pointer maps through the viewport (bPointerViewportFix). [Menus] bModMenuFlatAspect.
    namespace
    {
        std::atomic<bool> g_fitLoaded{ false };
        bool              g_fitOn = true;      // main thread
        float             g_fitAspect = 0.0f;  // main thread: 0 = none known
        std::mutex        g_fitLock;
        std::string       g_fitWant;  // the Presenter's panel menu
        std::string       g_fitReport{ "none" };
        struct Fitted
        {
            Scaleform::GFx::Movie*   movie{ nullptr };
            Scaleform::GFx::Viewport orig{};
            Scaleform::GFx::Viewport ours{};
        };
        std::vector<std::pair<std::string, Fitted>> g_fitted;  // main thread

        float FlatDisplayAspect()
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("Menus", "fModMenuAspect", "0", buf, sizeof(buf), Settings::Path().c_str());
            const float a = static_cast<float>(std::atof(buf));
            if (a > 0.5f) {
                return a;
            }
            // the game's own display size (MO2 maps the profile's Fallout4Prefs.ini onto this path for the game process)
            char path[MAX_PATH]{};
            char* up = nullptr;
            std::size_t n = 0;
            if (_dupenv_s(&up, &n, "USERPROFILE") == 0 && up) {
                std::snprintf(path, sizeof(path), "%s\\Documents\\My Games\\Fallout4\\Fallout4Prefs.ini", up);
                std::free(up);
            }
            const int w = ::GetPrivateProfileIntA("Display", "iSize W", 0, path);
            const int h = ::GetPrivateProfileIntA("Display", "iSize H", 0, path);
            return w > 0 && h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
        }

        Scaleform::GFx::Movie* MovieOf(const std::string& a_name)
        {
            auto* ui = RE::UI::GetSingleton();
            Scaleform::Ptr<RE::IMenu> m = ui ? ui->GetMenu(RE::BSFixedString(a_name.c_str())) : nullptr;
            return m && m->OnStack() && m->uiMovie ? m->uiMovie.get() : nullptr;
        }

        bool SameVp(const Scaleform::GFx::Viewport& a, const Scaleform::GFx::Viewport& b)
        {
            return a.bufferWidth == b.bufferWidth && a.bufferHeight == b.bufferHeight && a.left == b.left && a.top == b.top && a.width == b.width &&
                   a.height == b.height;
        }
    }

    // ---- 28 Nov (menus agent): the menu / HUD glass mesh over the whole render buffer ------------------------------------
    // User (screenshot, main menu): "the green backdrop/shading is generally misaligned". The green shading is the HUD-glass
    // pass: FlatScreenModel / HUDScreenModel (ctors aid 2219859 / 2219872) draw their offscreen target through a screen-
    // attached glass mesh picked by the screen aspect (1.6 -> HUDGlassFlat16x10.nif, >= 3 SUW, >= 2 UW, else HUDGlassFlat.nif
    // = 16:9). Our render buffer is 5120x2688 = 1.905 -> the 16:9 mesh, which covers only the centred 16:9 part of the screen
    // but maps the WHOLE target (uv 0..1) onto it: the glass shading lands at x' = 171 + x * 4778/5120 (measured: backdrop mask
    // x 1264..1858 -> tint 1372..1870, centre 1628 = predicted), while the menu itself is drawn 1:1. Fix: stretch the mesh's
    // horizontal axis by screen aspect / mesh aspect (renderer +0x80 = screen-attached root, local rotate column), so its uv
    // span covers the buffer 1:1 again. [Menus] bGlassFitScreen; pipe "pointer glass [k] [axis]".
    namespace
    {
        constexpr std::uintptr_t kRendererGetByNameRva = 0xA3A770;  // Interface3D::Renderer::GetByName (byte-checked in GamePipboy)
        constexpr std::uintptr_t kRendererScreenAttached = 0x80;    // Interface3D::Renderer screen-attached 3D root (MainScreen_SetScreenAttached3D A3A990)
        struct GlassFit
        {
            RE::NiAVObject* node{ nullptr };
            RE::NiMatrix3   orig{};
            float           k{ 1.0f };
            int             axis{ 0 };
        };
        constexpr int    kGlassRenderers = 5;
        GlassFit         g_glass[kGlassRenderers];
        bool             g_glassOn = true;      // main thread ([Menus] bGlassFitScreen)
        float            g_glassOverrideK = 0;  // pipe: 0 = computed
        int              g_glassAxis = 0;       // pipe: mesh axis that is screen-horizontal
        std::atomic<int> g_glassApplied{ 0 };
        std::string      g_glassReport{ "none" };  // under g_fitLock

        float MeshAspect(float a_screen)  // the game's own pick (ctors above)
        {
            if (std::abs(1.6f - a_screen) <= 0.0001f) return 1.6f;
            if (a_screen >= 3.0f) return 32.0f / 9.0f;
            if (a_screen >= 2.0f) return 21.0f / 9.0f;
            return 16.0f / 9.0f;
        }

        void GlassTick()
        {
            static int ok = -1;
            if (ok < 0) {
                static constexpr std::uint8_t kGbn[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30 };
                ok = std::memcmp(reinterpret_cast<const void*>(Base() + kRendererGetByNameRva), kGbn, sizeof(kGbn)) == 0 ? 1 : 0;
                if (!ok) REX::ERROR("menu glass: Interface3D::Renderer::GetByName bytes differ; glass fit off");
            }
            if (!ok) return;
            const auto W = *reinterpret_cast<const std::uint32_t*>(Base() + 0x3D87218);  // screen size (InitMovieViewport)
            const auto H = *reinterpret_cast<const std::uint32_t*>(Base() + 0x3D8721C);
            if (W == 0 || H == 0) return;
            const float screen = static_cast<float>(W) / static_cast<float>(H);
            const float want = !g_glassOn ? 1.0f : g_glassOverrideK > 0.0f ? g_glassOverrideK : screen / MeshAspect(screen);
            using GetByName_t = std::uint8_t* (*)(const RE::BSFixedString&);
            // 28 Nov (audit round 2): every Interface3D renderer whose screen-attached model the game picks by the same aspect buckets:
            // menus / HUD glass (aid 2219859 / 2219872), loading screen (BackgroundScreenModel 14099A250 -> LoadScreen02 + Translucent,
            // HUDGlass material), power-armour dash (PowerArmorGeometry::InitializePAHUDGeom 1052C90, PADash 16:9 / UW / SUW),
            // ScopeMenu (140AFC190: the same HUDGlassFlat* meshes)
            static const RE::BSFixedString kNames[kGlassRenderers] = { RE::BSFixedString("FlatScreenModel"), RE::BSFixedString("HUDScreenModel"),
                RE::BSFixedString("BackgroundScreenModel"), RE::BSFixedString("PowerArmorRenderer"), RE::BSFixedString("ScopeMenu") };
            std::string rep;
            for (int i = 0; i < kGlassRenderers; ++i) {
                auto*       r = reinterpret_cast<GetByName_t>(Base() + kRendererGetByNameRva)(kNames[i]);
                auto*       node = r ? *reinterpret_cast<RE::NiAVObject**>(r + kRendererScreenAttached) : nullptr;
                GlassFit&   g = g_glass[i];
                if (node != g.node) {  // new (or no) mesh: remember its own rotate
                    g = GlassFit{};
                    g.node = node;
                    if (node) g.orig = node->local.rotate;
                }
                if (!node) continue;
                if (std::abs(g.k - want) > 1e-4f || g.axis != g_glassAxis) {
                    RE::NiMatrix3 m = g.orig;
                    for (int row = 0; row < 3; ++row) m.entry[row][g_glassAxis] *= want;
                    node->local.rotate = m;
                    RE::NiUpdateData ud{};
                    node->Update(ud);
                    g.k = want;
                    g.axis = g_glassAxis;
                    ++g_glassApplied;
                    REX::INFO("menu glass: '{}' mesh axis {} x{:.4f} (screen {}x{} = {:.3f}, mesh {:.3f})", kNames[i].c_str(), g_glassAxis, want, W, H, screen,
                        MeshAspect(screen));
                }
                rep += std::format("{} k={:.4f} axis={} ", kNames[i].c_str(), g.k, g.axis);
            }
            std::scoped_lock l(g_fitLock);
            g_glassReport = rep.empty() ? std::string("no renderers") : rep;
        }
    }

    std::string GlassCommand(const std::string& a_arg)  // pipe "pointer glass [k] [axis]" (k 0 = computed)
    {
        float k = 0.0f;
        int   axis = g_glassAxis;
        if (std::sscanf(a_arg.c_str(), "%f %d", &k, &axis) >= 1) {
            g_glassOverrideK = k;
            g_glassAxis = std::clamp(axis, 0, 2);
        }
        std::scoped_lock l(g_fitLock);
        return std::format("glass on={} override={:.4f} axis={} applied={} | {}", g_glassOn ? 1 : 0, g_glassOverrideK, g_glassAxis, g_glassApplied.load(),
            g_glassReport);
    }

    // ---- 28 Nov (menus agent): aspect audit - the "aspect gap" (work/handoff/ui-menus.md "Aspect audit") -----------------
    // The engine sorts the screen into buckets (Ghidra, every reader of the screen-size globals 3D87218/3D8721C): 16:10
    // (|a-1.6| < 1e-4), "16:9" (a < 2.0), UW (2..3), SUW (>= 3), and only its UW/SUW paths know that the Scaleform menus are
    // letterboxed. But BSScaleformManager::InitMovieViewport (141B1BB30) letterboxes every menu above 16/9 = 1.778. Our
    // buffer is 5120x2688 = 1.905: in the gap. Every 16:9-bucket path that maps between the whole screen and a menu's stage
    // is off by the letterbox (171 px each side): input (pointer, fixed in DrainEvents), screen-texture meshes (glass,
    // GlassTick), world points projected into menus (HUD floating markers, VATS part labels: below), full-screen fills (the
    // fader: FitFullScreenMenus). One rule for all of them: map through the letterbox rect, which for a world point is exactly
    // the engine's own UW formula u' = (a * 9/16 * (2u - 1) + 1) / 2 (CalculateFloatingMarkerPositionAndVisiblity 1409CC840).
    // [Menus] bAspectGapFix (markers + VATS labels + fader); off = the engine's 16:9-bucket behaviour.
    namespace
    {
        std::atomic<bool>          g_gapOn{ true };
        std::atomic<std::uint64_t> g_gapMarkers{ 0 }, g_gapVats{ 0 };
        std::atomic<float>         g_gapLastU{ -1.0f }, g_gapLastU2{ -1.0f };
        std::atomic<float>         g_gapVatsU{ -1.0f }, g_gapVatsU2{ -1.0f };
        float                      g_gapVatsRing[16]{};  // audit: last raw part x values
        std::atomic<std::uint32_t> g_gapVatsN{ 0 };
        std::string                g_gapInstall{ "not installed" };

        float ScreenAspect()
        {
            const auto W = *reinterpret_cast<const std::uint32_t*>(Base() + 0x3D87218);
            const auto H = *reinterpret_cast<const std::uint32_t*>(Base() + 0x3D8721C);
            return W && H ? static_cast<float>(W) / static_cast<float>(H) : 0.0f;
        }
        bool InGap(float a) { return a > 16.0f / 9.0f + 1e-3f && a < 2.0f; }  // letterboxed, but not the engine's UW bucket
        float GapRemap(float u, float a) { return (a * 0.5625f * (2.0f * u - 1.0f) + 1.0f) * 0.5f; }

        // HUDMarkerData::CalculateFloatingMarkerPositionAndVisiblity(marker, float rect[4]) -> visible; writes x/y at +0x10/+0x14
        using Marker_t = bool (*)(void*, float*);
        Marker_t g_origMarker = nullptr;
        bool HkMarker(void* a_marker, float* a_rect)
        {
            const bool vis = g_origMarker(a_marker, a_rect);
            const float a = ScreenAspect();
            if (!g_gapOn.load(std::memory_order_relaxed) || !InGap(a) || !a_marker || !a_rect) {
                return vis;
            }
            float* xy = reinterpret_cast<float*>(static_cast<std::uint8_t*>(a_marker) + 0x10);
            const float w = a_rect[2] - a_rect[0];
            if (w <= 0.0f) return vis;
            const float u = (xy[0] - a_rect[0]) / w;
            const float u2 = GapRemap(u, a);
            xy[0] = a_rect[0] + w * u2;
            ++g_gapMarkers;
            g_gapLastU = u, g_gapLastU2 = u2;
            return vis && xy[0] >= a_rect[0] && xy[0] <= a_rect[2];  // the engine's own on-screen test, with the remapped x
        }

        // VATSMenu::UpdateState part labels: NiCamera project (1416D2270)(cam, pt, &x, &y, &z, eps) -> x 0..1 of the screen,
        // passed to AS3 UpdatePartPositions over the letterboxed stage
        using Project_t = std::uint64_t (*)(void*, float*, float*, float*, float*, float);
        Project_t g_origProject = nullptr;
        std::uint64_t HkVatsProject(void* a_cam, float* a_pt, float* a_x, float* a_y, float* a_z, float a_eps)
        {
            const auto r = g_origProject(a_cam, a_pt, a_x, a_y, a_z, a_eps);
            const float a = ScreenAspect();
            if ((r & 0xFF) && a_x && g_gapOn.load(std::memory_order_relaxed) && InGap(a)) {
                g_gapVatsRing[g_gapVatsN++ % 16] = *a_x;
                g_gapVatsU = *a_x;
                *a_x = GapRemap(*a_x, a);
                g_gapVatsU2 = *a_x;
                ++g_gapVats;
            }
            return r;
        }

        bool CallSiteOk(std::uintptr_t a_site, std::uintptr_t a_target, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_site);
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            if (p[0] != 0xE8 || a_site + 5 + rel != a_target) {
                REX::ERROR("aspect gap: {} call at {:X} is not E8 -> {:X}; not patching", a_what, a_site, a_target);
                return false;
            }
            return true;
        }

        // full-screen fill menus the game letterboxes: their viewport goes back to the whole buffer (FaderMenu: exactFit
        // solid fade, letterboxed it leaves 171 px strips unfaded at both sides)
        std::uint64_t g_fullFits = 0;
        void FitFullScreenMenus()
        {
            if (!g_gapOn.load(std::memory_order_relaxed)) return;
            static const RE::BSFixedString kFader("FaderMenu");
            auto* ui = RE::UI::GetSingleton();
            Scaleform::Ptr<RE::IMenu> m = ui ? ui->GetMenu(kFader) : nullptr;
            if (!m || !m->uiMovie) return;
            Scaleform::GFx::Viewport vp{};
            m->uiMovie->GetViewport(&vp);
            if (vp.bufferWidth > 0 && vp.bufferHeight > 0 && (vp.left != 0 || vp.top != 0 || vp.width != vp.bufferWidth || vp.height != vp.bufferHeight)) {
                vp.left = vp.top = 0;
                vp.width = vp.bufferWidth;
                vp.height = vp.bufferHeight;
                m->uiMovie->SetViewport(vp);
                if (++g_fullFits <= 3) REX::INFO("aspect gap: FaderMenu viewport -> whole buffer {}x{}", vp.bufferWidth, vp.bufferHeight);
            }
        }
    }

    void InstallAspectGapHooks()
    {
        g_gapOn = ::GetPrivateProfileIntA("Menus", "bAspectGapFix", 1, Settings::Path().c_str()) != 0;
        auto&                    tramp = REL::GetTrampoline();
        constexpr std::uintptr_t kMarkerSite = 0x9C87DA, kMarker = 0x9CC840;  // aid_2220476 -> CalculateFloatingMarkerPositionAndVisiblity
        constexpr std::uintptr_t kVatsSite = 0xB1EAA5, kProject = 0x16D2270;   // VATSMenu::UpdateState part positions -> project
        std::string s;
        if (CallSiteOk(kMarkerSite, kMarker, "HUD floating marker")) {
            g_origMarker = reinterpret_cast<Marker_t>(tramp.write_call<5>(Base() + kMarkerSite, &HkMarker));
            s += "markers ";
        }
        if (CallSiteOk(kVatsSite, kProject, "VATS part labels")) {
            g_origProject = reinterpret_cast<Project_t>(tramp.write_call<5>(Base() + kVatsSite, &HkVatsProject));
            s += "vatsLabels ";
        }
        g_gapInstall = s.empty() ? std::string("none") : s;
        REX::INFO("aspect gap: hooks {} ([Menus] bAspectGapFix={}; screen aspect {:.3f}, in gap {})", g_gapInstall, g_gapOn.load() ? 1 : 0,
            ScreenAspect(), InGap(ScreenAspect()) ? 1 : 0);
    }

    std::string AspectGapReport()
    {
        // LooksMenu sculpt cursor (screen px, MoveSculptCursor 140A85B50 globals 2F2CB30 / 2F2CB34): audit check against the pointer
        const float sx = *reinterpret_cast<const float*>(Base() + 0x2F2CB30), sy = *reinterpret_cast<const float*>(Base() + 0x2F2CB34);
        return std::format("aspectGap on={} hooks={} aspect={:.3f} inGap={} markers={} lastU {:.4f}->{:.4f} vatsLabels={} lastVats {:.4f}->{:.4f} faderFits={} "
                           "sculpt {:.1f},{:.1f} looksCursor on={} ok={} moves={} buttons={} sculptCalls={} hintSkips={}",
            g_gapOn.load() ? 1 : 0, g_gapInstall, ScreenAspect(), InGap(ScreenAspect()) ? 1 : 0, g_gapMarkers.load(), g_gapLastU.load(), g_gapLastU2.load(),
            g_gapVats.load(), g_gapVatsU.load(), g_gapVatsU2.load(), g_fullFits, sx, sy, g_looksCursor.load() ? 1 : 0, g_looksOk, g_looksMoves.load(),
            g_looksButtons.load(), g_looksSculptCalls.load(), g_looksHintSkips.load());
    }

    void SetPanelMenu(const std::string& a_name)
    {
        std::scoped_lock l(g_fitLock);
        if (g_fitWant != a_name) {
            g_fitWant = a_name;
        }
    }

    void FitPanelViewportTick()  // main tick
    {
        if (!g_fitLoaded.exchange(true)) {
            g_fitOn = ::GetPrivateProfileIntA("Menus", "bModMenuFlatAspect", 1, Settings::Path().c_str()) != 0;
            g_vpFix = ::GetPrivateProfileIntA("Menus", "bPointerViewportFix", 1, Settings::Path().c_str()) != 0;
            g_looksCursor = ::GetPrivateProfileIntA("Menus", "bLooksMenuPointerCursor", 1, Settings::Path().c_str()) != 0;
            g_fitAspect = FlatDisplayAspect();
            g_glassOn = ::GetPrivateProfileIntA("Menus", "bGlassFitScreen", 1, Settings::Path().c_str()) != 0;
            g_gapOn = ::GetPrivateProfileIntA("Menus", "bAspectGapFix", 1, Settings::Path().c_str()) != 0;
            REX::INFO("menu viewport: flat-display aspect {} ([Menus] bModMenuFlatAspect; aspect {:.3f}{})", g_fitOn ? "on" : "off", g_fitAspect,
                g_fitAspect > 0.0f ? "" : " - unknown: nothing changed");
        }
        GlassTick();  // 28 Nov: glass mesh over the whole buffer (main menu backdrop)
        FitFullScreenMenus();  // 28 Nov: aspect gap - full-screen fills (FaderMenu) over the whole buffer
        std::string want;
        {
            std::scoped_lock l(g_fitLock);
            want = g_fitWant;
        }
        // put back what left the panel (or everything when off)
        for (auto it = g_fitted.begin(); it != g_fitted.end();) {
            if (g_fitOn && it->first == want) {
                ++it;
                continue;
            }
            if (auto* mv = MovieOf(it->first); mv && mv == it->second.movie) {
                Scaleform::GFx::Viewport cur{};
                mv->GetViewport(&cur);
                if (SameVp(cur, it->second.ours)) {
                    mv->SetViewport(it->second.orig);
                }
            }
            REX::INFO("menu viewport: '{}' put back", it->first);
            it = g_fitted.erase(it);
        }
        if (!g_fitOn || want.empty() || g_fitAspect <= 0.0f) {
            return;
        }
        auto* mv = MovieOf(want);
        if (!mv) {
            return;
        }
        Scaleform::GFx::Viewport cur{};
        mv->GetViewport(&cur);
        for (auto& [name, f] : g_fitted) {
            if (name != want) continue;
            if (f.movie != mv) {  // menu re-created: start over below
                g_fitted.clear();
                break;
            }
            if (!SameVp(cur, f.ours) && SameVp(cur, f.orig)) {
                mv->SetViewport(f.ours);  // the game re-initialised it (InitMovieViewport): ours again
            }
            return;
        }
        // only menus the game gave the whole buffer (not its 16:9 letterbox) and that keep their aspect
        const auto mode = mv->GetViewScaleMode();
        if (cur.left != 0 || cur.top != 0 || cur.width != cur.bufferWidth || cur.height != cur.bufferHeight || cur.width <= 0 || cur.height <= 0 ||
            mode == Scaleform::GFx::Movie::ScaleModeType::kExactFit) {
            return;
        }
        const float bufAspect = static_cast<float>(cur.width) / static_cast<float>(cur.height);
        Fitted f;
        f.movie = mv;
        f.orig = cur;
        f.ours = cur;
        if (std::abs(g_fitAspect - bufAspect) > 0.01f) {
            if (g_fitAspect > bufAspect) {
                f.ours.height = static_cast<std::int32_t>(std::lround(static_cast<float>(cur.width) / g_fitAspect));
                f.ours.top = (cur.height - f.ours.height) / 2;
            } else {
                f.ours.width = static_cast<std::int32_t>(std::lround(static_cast<float>(cur.height) * g_fitAspect));
                f.ours.left = (cur.width - f.ours.width) / 2;
            }
            mv->SetViewport(f.ours);
        }
        const auto        vis = mv->GetVisibleFrameRect();
        const std::string rep = std::format("'{}' viewport {},{} {}x{} in {}x{} (aspect {:.3f} -> {:.3f}), visible stage {:.1f}..{:.1f} x {:.1f}..{:.1f}",
            want, f.ours.left, f.ours.top, f.ours.width, f.ours.height, cur.bufferWidth, cur.bufferHeight, bufAspect, g_fitAspect, vis.x1, vis.x2, vis.y1,
            vis.y2);
        REX::INFO("menu viewport: {}", rep);
        {
            std::scoped_lock l(g_fitLock);
            g_fitReport = rep;
        }
        g_fitted.emplace_back(want, f);
    }

    std::string FitReport()
    {
        std::string want, rep;
        {
            std::scoped_lock l(g_fitLock);
            want = g_fitWant;
            rep = g_fitReport;
        }
        return std::format("flatAspect on={} aspect={:.3f} vpFix={} want='{}' last {} | {}", g_fitOn ? 1 : 0, g_fitAspect, g_vpFix.load() ? 1 : 0, want, rep,
            AspectGapReport());
    }

    void LooksMenuCall(const char* a_fn)
    {
        tq::RunOnMain([fn = std::string(a_fn)]() {
            auto* ui = RE::UI::GetSingleton();
            Scaleform::Ptr<RE::IMenu> m = ui ? ui->GetMenu(RE::BSFixedString("LooksMenu")) : nullptr;
            if (m && m->OnStack() && m->menuObj.IsObject()) {
                const bool ok = m->menuObj.Invoke(fn.c_str());
                REX::INFO("menu pointer: LooksMenu {} -> {}", fn, ok ? "invoked" : "failed");
            }
        });
    }

    void ReloadPointerSettings()
    {
        g_vpFix = ::GetPrivateProfileIntA("Menus", "bPointerViewportFix", 1, Settings::Path().c_str()) != 0;
        g_looksCursor = ::GetPrivateProfileIntA("Menus", "bLooksMenuPointerCursor", 1, Settings::Path().c_str()) != 0;
        g_fitLoaded = false;
        g_anyMenu = -1;
        std::scoped_lock l(g_modPanelLock);
        g_modPanel.loaded = false;
    }

    std::string ModPanelMenu()
    {
        {
            std::scoped_lock l(g_modPanelLock);
            if (!g_modPanel.loaded) {
                g_modPanel.loaded = true;
                g_modPanel.on = ::GetPrivateProfileIntA("Menus", "bPanelModMenus", 1, Settings::Path().c_str()) != 0;
                g_modPanel.add = LowerList("sPanelMenus");
                g_modPanel.no = LowerList("sNoPanelMenus");
                REX::INFO("menu panel: mod-menu rule {} ([Menus] bPanelModMenus; sPanelMenus '{}', sNoPanelMenus '{}')", g_modPanel.on ? "on" : "off",
                    g_modPanel.add, g_modPanel.no);
            }
        }
        const int live = g_modPanelLive.load();
        const bool on = live >= 0 ? live != 0 : g_modPanel.on;
        auto* ui = RE::UI::GetSingleton();
        if (!ui || (!on && g_modPanel.add.size() <= 2)) {
            return {};
        }
        static constexpr const char* kNever[] = { "ButtonBarMenu", "CursorMenu", "VATSMenu", "DialogueMenu", "HUDMenu", "PipboyMenu",
            "FaderMenu", "VignetteMenu", "LoadingMenu", "PromptMenu" };
        std::string found, logLine;
        auto&       lock = StackLock();
        lock.lock_read();
        const auto& stack = ui->menuStack;
        for (std::uint32_t i = stack.size(); i-- > 0 && found.empty();) {
            const auto& m = stack[i];
            if (!m) {
                continue;
            }
            const std::uint32_t flags = m->menuFlags.underlying();
            const char*         r = m->customRendererName.c_str();
            const char*         n = m->menuName.c_str();
            if (!(flags & 0x8000) || !r || !n || !*n || _stricmp(r, "FlatScreenModel") != 0) {
                continue;
            }
            bool never = false;
            for (const char* x : kNever) {
                never = never || _stricmp(n, x) == 0;
            }
            if (never || InList(g_modPanel.no, n)) {
                continue;
            }
            const bool listed = InList(g_modPanel.add, n);
            const bool mod = OutsideExe(m.get());
            const bool interactive = (flags & (0x4 | 0x8)) != 0 || AnyMenuInteractive(m.get(), n, flags);
            if (!interactive || !(listed || (on && mod))) {
                continue;
            }
            found = n;
            std::scoped_lock l(g_anyLock);
            const std::string tag = "|" + found + "|";
            if (g_modPanelSeen.find(tag) == std::string::npos) {
                g_modPanelSeen += tag;
                logLine = std::format("menu panel: '{}' flags=0x{:X} depth={} module {} -> anchored panel ({})", found, flags,
                    static_cast<int>(reinterpret_cast<const std::uint8_t*>(m.get())[0x64]), mod ? OwnerModule(m.get()) : std::string("Fallout4.exe"),
                    listed ? "[Menus] sPanelMenus" : "mod menu rule, [Menus] bPanelModMenus");
            }
        }
        lock.unlock_read();
        if (!logLine.empty()) {
            REX::INFO("{}", logLine);
        }
        return found;
    }

    bool MenuOpen(const char* a_name)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !a_name) {
            return false;
        }
        // perf pass 26 Nov: callers ask every frame with literal names; the interned name is kept per thread, keyed by the
        // pointer and checked against the text (a reused buffer with another name just re-interns)
        struct Entry
        {
            std::string       text;
            RE::BSFixedString name;
        };
        thread_local std::unordered_map<const char*, Entry> cache;
        auto                                                it = cache.find(a_name);
        if (it == cache.end() || it->second.text != a_name) {
            if (cache.size() > 256) {
                cache.clear();
            }
            it = cache.insert_or_assign(a_name, Entry{ a_name, RE::BSFixedString(a_name) }).first;
        }
        return ui->GetMenuOpen(it->second.name);
    }

    bool VatsTargetScreen(float& a_u, float& a_v)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString("VATSMenu"));
        if (!m) {
            return false;
        }
        const auto* obj = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(m.get()) + 0xE8);
        if (!obj) {
            return false;
        }
        const std::uint32_t idx = *reinterpret_cast<const std::uint32_t*>(obj + 0x1230);
        const std::uint32_t cnt = *reinterpret_cast<const std::uint32_t*>(obj + 0x248);
        const auto*         order = *reinterpret_cast<const std::uint8_t* const*>(obj + 0x238);
        const auto*         refs = *reinterpret_cast<RE::TESObjectREFR* const* const*>(obj + 0x220);
        if (idx >= cnt || cnt > 256 || !order || !refs) {
            return false;
        }
        const std::uint32_t k = *reinterpret_cast<const std::uint32_t*>(order + static_cast<std::size_t>(idx) * 8);
        const RE::TESObjectREFR* ref = refs[k];
        if (!ref) {
            return false;
        }
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* root = cam ? cam->cameraRoot.get() : nullptr;
        if (!root) {
            return false;
        }
        for (auto& c : root->children) {
            if (auto* nc = ::netimmerse_cast<RE::NiCamera*>(c.get())) {
                const auto& w = nc->worldToCam;
                const float p[4] = { ref->data.location.x, ref->data.location.y, ref->data.location.z + 70.0f, 1.0f };
                float       r[4]{};
                for (int i = 0; i < 4; ++i) {
                    r[i] = w[i][0] * p[0] + w[i][1] * p[1] + w[i][2] * p[2] + w[i][3] * p[3];
                }
                if (!(r[3] > 1e-3f)) {
                    return false;  // behind the camera
                }
                a_u = 0.5f + 0.5f * r[0] / r[3];
                a_v = 0.5f - 0.5f * r[1] / r[3];
                return true;
            }
        }
        return false;
    }

    void HideMenuIfOpen(const char* a_name)
    {
        static std::int64_t last = 0;
        const std::int64_t  now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!MenuOpen(a_name) || now - last < 500) {
            return;
        }
        last = now;
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::BSFixedString(a_name), RE::UI_MESSAGE_TYPE::kHide);
            REX::INFO("menu: {} hidden (not used over the live world)", a_name);
        }
    }

    bool PanelEligible(const char* a_name)
    {
        if (!a_name) {
            return false;
        }
        // No world behind (main menu, loading, credits), the camera is taken (LooksMenu face camera, scope zoom), or the
        // movie is on a 3D screen (Pip-Boy fallback, terminal). Menus with a 3D item preview (Container/Barter "Container3D",
        // Examine/Cooking/Workbench "WorkbenchItem3D"/"ExamineMenuItem3D", Workshop, PowerArmorMod, Lockpicking, Book) are
        // panels too: the preview renderer is captured onto the panel (hud::CaptureMenuPreview); [Menus] bPanelPreview=0
        // keeps them on the flat fallback.
        // round 8: LooksMenu (character creation / face camera) is a panel over the SCENE view of the game's camera now
        // ("camera takeover yes", user 16 Nov); the flat window stays for no-world menus, the scope zoom and VATS
        static constexpr const char* kNo[] = { "MainMenu", "LoadingMenu", "CreditsMenu", "ScopeMenu", "PipboyMenu",
            "PipboyHolotapeMenu", "TerminalMenu", "TerminalHolotapeMenu",
            "VATSMenu" };  // VATS: head-locked overlay over the live world with the camera gate (Presenter), else flat fallback
        static constexpr const char* kPreview[] = { "ContainerMenu", "BarterMenu", "ExamineMenu", "WorkshopMenu", "CookingMenu",
            "PowerArmorModMenu", "LockpickingMenu", "BookMenu" };
        if (!hud::MenuPreviewEnabled()) {
            for (const char* n : kPreview) {
                if (_stricmp(a_name, n) == 0) {
                    return false;
                }
            }
        }
        for (const char* n : kNo) {
            if (_stricmp(a_name, n) == 0) {
                return false;
            }
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString(a_name));
        if (!m) {
            return false;
        }
        const char* r = m->customRendererName.c_str();
        return (m->menuFlags.underlying() & 0x8000) && r && _stricmp(r, "FlatScreenModel") == 0;
    }

    // ---- Round 7: terminals on their own monitor in the world (FO4VR) ------------------------------------------------
    namespace
    {
        constexpr std::uintptr_t kNiPickCtor = 0x16FD560;     // NiPick::NiPick(NiPick*, u16 initialSize, u16 growBy)
        constexpr std::uintptr_t kNiPickObjects = 0x16FD660;  // NiPick::PickObjects(NiPick*, const NiPoint3& o, const NiPoint3& d, bool append)
        constexpr std::uintptr_t kNiPickDtor = 0x16FD610;     // NiPick::~NiPick(NiPick*)
        std::atomic<bool>          g_worldScreenOn{ true };
        int                        g_niPickOk = -1;
        std::atomic<std::uint64_t> g_wsPicks{ 0 }, g_wsHits{ 0 };
        std::string                g_wsLast{ "none" };  // under g_lock

        bool NiPickOk()
        {
            if (g_niPickOk < 0) {
                const auto* b = reinterpret_cast<const std::uint8_t*>(Base());
                static constexpr std::uint8_t kCtor[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF };
                static constexpr std::uint8_t kPick[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF };
                static constexpr std::uint8_t kDtor[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1, 0x20 };
                const bool ok = std::memcmp(b + kNiPickCtor, kCtor, sizeof(kCtor)) == 0 && std::memcmp(b + kNiPickObjects, kPick, sizeof(kPick)) == 0 &&
                                std::memcmp(b + kNiPickDtor, kDtor, sizeof(kDtor)) == 0 && PickCodeOk();
                g_niPickOk = ok ? 1 : 0;
                if (ok) {
                    REX::INFO("terminal in world: NiPick ctor/PickObjects/dtor verified (hand-ray pick on the terminal screen)");
                } else {
                    REX::ERROR("terminal in world: NiPick bytes differ; no pointer on the terminal monitor");
                }
            }
            return g_niPickOk == 1;
        }

        RE::NiAVObject* TerminalScreen()
        {
            const auto base = Base();
            auto*      term = *reinterpret_cast<RE::TESObjectREFR**>(base + kTerminalRef);
            RE::NiAVObject* root = term ? term->GetCurrent3D() : nullptr;
            if (!root) {
                return nullptr;
            }
            static const RE::BSFixedString kScreen("Screen:0");
            return reinterpret_cast<GetObjectByName_t>(base + kGetObjectByName)(root, kScreen, true, false);
        }
    }

    bool MenuUsesRenderer(const char* a_menu, const char* a_renderer)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !a_menu || !a_renderer || !ui->GetMenuOpen(a_menu)) {
            return false;
        }
        Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString(a_menu));
        const char*               r = m ? m->customRendererName.c_str() : nullptr;
        return r && _stricmp(r, a_renderer) == 0;
    }

    void HudMessage(const std::string& a_text)
    {
        constexpr std::uintptr_t kShowHudMessage = 0xA37270;  // SendHUDMessage::ShowHUDMessage(msg, sound, throttle, warning)
        static const bool ok = [] {
            static constexpr std::uint8_t kPro[] = { 0x48, 0x83, 0xEC, 0x58, 0xC6, 0x44, 0x24, 0x28, 0x00 };
            const bool m = std::memcmp(reinterpret_cast<const void*>(Base() + kShowHudMessage), kPro, sizeof(kPro)) == 0;
            if (!m) REX::ERROR("HUD message: ShowHUDMessage bytes differ; no notifications");
            return m;
        }();
        if (!ok) {
            return;
        }
        tq::RunOnMain([t = a_text]() {  // main-idle queue (ThreadQueues.h; HUD data model owned by the main thread there)
            using Fn = void (*)(const char*, const char*, bool, bool);
            // 21 Nov: sound nullptr, as the game's own silent callers (VATS ShowHUDMessage(msg, 0, ..)). An empty "" sound name
            // became a BSFixedString entry that HUDMessages::AddQueuedMessage (9D2730) passed to PlayMenuSound, which resolved it
            // to UITerminalCharScrollLP (SNDR 000980FB): a loop nobody stops. Every notification (camera state changes in VATS /
            // AAF scenes, Pip-Boy notices, gesture toggles) started one more: the user's stacking "keypress" tick.
            reinterpret_cast<Fn>(Base() + kShowHudMessage)(t.c_str(), nullptr, false, false);
        });
    }

    void SetWorldScreenEnabled(bool a_on) { g_worldScreenOn = a_on; }
    bool WorldScreenEnabled() { return g_worldScreenOn.load(); }

    bool WorldScreenMenu(const char* a_name)
    {
        if (!a_name || !g_worldScreenOn.load() || (_stricmp(a_name, "TerminalMenu") != 0 && _stricmp(a_name, "TerminalHolotapeMenu") != 0)) {
            return false;
        }
        // round 8 (camera takeover generalised): any camera state whose policy is a live world view (TRACK / LOCK, SCENE reports
        // TRACK) - with the game's terminal camera (furniture gate off) the monitor is seen through that camera + the head;
        // FADE_SNAP (fading to black) and CINEMA keep the flat window
        if (const auto pol = camdir::ViewPolicy(CurrentCameraState()); (pol != camdir::Policy::kTrack && pol != camdir::Policy::kLock) || !PickCodeOk()) {
            return false;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->GetMenuOpen(a_name)) {
            return false;
        }
        Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString(a_name));
        const char* r = m ? m->customRendererName.c_str() : nullptr;
        // no terminal ref (a holotape terminal from the Pip-Boy): the movie is on the Pip-Boy renderer instead
        return r && _stricmp(r, "TerminalMenu") == 0 && TerminalScreen() != nullptr;
    }

    bool PickWorldScreen(const float a_lp[3], const float a_ld[3], float a_upm, float& a_u, float& a_v, float& a_dist)
    {
        if (!NiPickOk()) {
            return false;
        }
        RE::NiAVObject* screen = TerminalScreen();
        auto*           cam = RE::PlayerCamera::GetSingleton();
        auto*           root = cam ? cam->cameraRoot.get() : nullptr;
        if (!screen || !root) {
            return false;
        }
        // camera-local (X right, Y forward, Z up; rows of the root's world rotation = world images of the local axes)
        const auto& m = root->world.rotate;
        auto        toWorld = [&](const float* l) {
            return RE::NiPoint3{ l[0] * m.entry[0].x + l[1] * m.entry[1].x + l[2] * m.entry[2].x,
                l[0] * m.entry[0].y + l[1] * m.entry[1].y + l[2] * m.entry[2].y, l[0] * m.entry[0].z + l[1] * m.entry[1].z + l[2] * m.entry[2].z };
        };
        const RE::NiPoint3 off = toWorld(a_lp);
        RE::NiPoint3       d = toWorld(a_ld);
        const float        len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (!(len > 0.5f)) {
            return false;
        }
        d = { d.x / len, d.y / len, d.z / len };
        const RE::NiPoint3 o{ root->world.translate.x + off.x * a_upm, root->world.translate.y + off.y * a_upm, root->world.translate.z + off.z * a_upm };
        // NiPick as UIUtils::PickScreen sets it up: ctor(0, 8); +4 = 0; +8 = 1 (u64); target +0x18 (NiPointer); +0x48 = 0x101
        alignas(16) std::uint8_t pick[0x80]{};
        using Ctor_t = void* (*)(void*, std::uint16_t, std::uint16_t);
        using Pick_t = bool (*)(void*, const RE::NiPoint3&, const RE::NiPoint3&, bool);
        using Dtor_t = void (*)(void*);
        const auto base = Base();
        reinterpret_cast<Ctor_t>(base + kNiPickCtor)(pick, 0, 8);
        *reinterpret_cast<std::uint32_t*>(pick + 4) = 0;
        *reinterpret_cast<std::uint64_t*>(pick + 8) = 1;
        screen->IncRefCount();  // released by the dtor
        *reinterpret_cast<RE::NiAVObject**>(pick + 0x18) = screen;
        *reinterpret_cast<std::uint32_t*>(pick + 0x48) = 0x101;
        const bool    ok = reinterpret_cast<Pick_t>(base + kNiPickObjects)(pick, o, d, false);
        const auto    n = *reinterpret_cast<const std::uint32_t*>(pick + 0x38);
        const auto*   recs = *reinterpret_cast<std::uint8_t* const* const*>(pick + 0x28);
        bool          hit = false;
        if (ok && n > 0 && recs && recs[0]) {
            a_u = *reinterpret_cast<const float*>(recs[0] + 0x24);
            a_v = *reinterpret_cast<const float*>(recs[0] + 0x28);
            a_dist = *reinterpret_cast<const float*>(recs[0] + 0x20);
            hit = std::isfinite(a_u) && std::isfinite(a_v) && a_u >= -0.01f && a_u <= 1.01f && a_v >= -0.01f && a_v <= 1.01f;
        }
        reinterpret_cast<Dtor_t>(base + kNiPickDtor)(pick);
        ++g_wsPicks;
        if (hit) {
            ++g_wsHits;
        }
        {
            std::scoped_lock l(g_lock);
            g_wsLast = hit ? std::format("hit uv {:.4f},{:.4f} dist {:.1f} u from ({:.1f},{:.1f},{:.1f}) dir ({:.3f},{:.3f},{:.3f})", a_u, a_v, a_dist, o.x, o.y,
                                 o.z, d.x, d.y, d.z)
                           : std::format("miss (results {}) from ({:.1f},{:.1f},{:.1f}) dir ({:.3f},{:.3f},{:.3f})", n, o.x, o.y, o.z, d.x, d.y, d.z);
        }
        return hit;
    }

    std::string WorldScreenReport()
    {
        std::string last;
        {
            std::scoped_lock l(g_lock);
            last = g_wsLast;
        }
        return std::format("terminalInWorld on={} nipick={} picks={} hits={} last {}", g_worldScreenOn.load() ? 1 : 0, g_niPickOk, g_wsPicks.load(),
            g_wsHits.load(), last);
    }

    std::string PausedCameraReport()
    {
        return std::format("pausedCamera ok={} calls={} frames={}", g_pucOk, g_pucCalls.load(), g_pucFrames.load());
    }

    bool FindPointerMenu(MenuPointerTarget& a_out)
    {
        a_out = {};
        return static_cast<bool>(FindTarget(&a_out));
    }

    void SetMenuPointer(const MenuPointerInput& a_in, bool a_active, const char* a_forceMenu)
    {
        {
            std::scoped_lock l(g_lock);
            g_forced = a_forceMenu ? a_forceMenu : "";
        }
        g_queueMovieUv = a_active ? a_in.movieUv : g_last.movieUv;  // a release after the pointer left keeps the last mode
        bool queued = false;
        if (!a_active) {
            if (g_down) {  // release a held button when the pointer goes away (cinema hidden, switch off)
                Queue(3, g_last.u, g_last.v);
                g_down = false;
                queued = true;
            }
            g_lastValid = false;
            std::scoped_lock l(g_lock);
            g_lastInfo = {};
        } else {
            MenuPointerTarget t;
            ResolveTarget(&t);
            const std::string tname = t.valid ? t.name : "-";
            {
                std::scoped_lock l(g_lock);
                g_lastInfo = t;
            }
            if (tname != g_lastTarget) {
                ++g_targetChanges;
                g_lastTarget = tname;
                g_lastValid = false;  // a new movie gets a move first
            }
            if (a_in.onQuad) {
                const bool moved = !g_lastValid || std::abs(a_in.u - g_last.u) > 0.0005f || std::abs(a_in.v - g_last.v) > 0.0005f;
                if (moved) {
                    Queue(1, a_in.u, a_in.v);
                    queued = true;
                }
                if (a_in.down && !g_down) {
                    Queue(2, a_in.u, a_in.v);
                    g_down = true;
                    queued = true;
                }
                for (int i = 0; i < std::abs(a_in.wheel); ++i) {
                    Queue(4, a_in.u, a_in.v, a_in.wheel > 0 ? 1.0f : -1.0f);
                    queued = true;
                }
            }
            if (!a_in.down && g_down) {  // release at the current (or last on-quad) position
                Queue(3, a_in.onQuad ? a_in.u : g_last.u, a_in.onQuad ? a_in.v : g_last.v);
                g_down = false;
                queued = true;
            }
            if (a_in.onQuad) {
                g_last = a_in;
                g_lastValid = true;
            }
        }
        if (queued && !g_taskPending.exchange(true)) {
            // 20 Nov (threading audit): main-idle queue = the UI thread after UI::AdvanceMenus, before Swap draws the menus
            // (ThreadQueues.h); the F4SE UI task ran on job threads too, while the menus rendered on the main thread
            if (!tq::RunOnUI([]() { DrainEvents(); })) {
                g_taskPending = false;
            }
        }
    }

    std::string MenuPointerGameReport()
    {
        MenuPointerTarget t;  // as seen by the main thread at the last Present (no stack access from the pipe thread)
        std::string       ev, vp, down;
        {
            std::scoped_lock l(g_lock);
            t = g_lastInfo;
            ev = g_lastEvent;
            vp = g_lastViewport;
            down = g_downMenu;
        }
        std::string pick, bar;
        {
            std::scoped_lock l(g_lock);
            pick = g_lastPick;
            bar = g_barLast;
        }
        return std::format("menu='{}' flags=0x{:X} depth={} termMode={} sent move={} down={} up={} wheel={} dropped={} targetChanges={} held='{}' | last {} | viewport {} | screenPick picks={} hits={} last {} | buttonBar sent={} last {}",
            t.valid ? t.name : "-", t.flags, t.depth, t.terminalMode, g_sent[1].load(), g_sent[2].load(), g_sent[3].load(), g_sent[4].load(), g_dropped.load(),
            g_targetChanges.load(), down, ev, vp, g_picks.load(), g_pickHits.load(), pick, g_barSent.load(), bar);
    }

    bool PipboyScreenColor(float a_rgb[3], float& a_brightness, bool a_powerArmor)
    {
        static constexpr const char* kPipKeys[3] = { "fPipboyEffectColorR:Pipboy", "fPipboyEffectColorG:Pipboy", "fPipboyEffectColorB:Pipboy" };
        static constexpr const char* kPaKeys[3] = { "fPAEffectColorR:Pipboy", "fPAEffectColorG:Pipboy", "fPAEffectColorB:Pipboy" };
        static constexpr std::uintptr_t kPaColorValue[3] = { 0x2F98C20, 0x2F98C38, 0x2F98C50 };
        const char* const* kKeys = a_powerArmor ? kPaKeys : kPipKeys;
        const std::uintptr_t* kValues = a_powerArmor ? kPaColorValue : kPipColorValue;
        const auto base = Base();
        float      rgb[3];
        for (int i = 0; i < 3; ++i) {
            const RE::Setting* s = RE::GetINISetting(kKeys[i]);
            if (s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
                rgb[i] = s->GetFloat();
            } else {
                // Fallback: the Setting object at the verified RVA, if it carries the right key.
                const auto* obj = reinterpret_cast<const RE::Setting*>(base + kValues[i] - 8);
                if (obj->GetKey() != kKeys[i]) {
                    return false;
                }
                rgb[i] = *reinterpret_cast<const float*>(base + kValues[i]);
            }
            if (!std::isfinite(rgb[i])) {
                return false;
            }
        }
        const float b = PipboyScreenBrightness();
        for (int i = 0; i < 3; ++i) {
            a_rgb[i] = std::clamp(rgb[i], 0.0f, 4.0f);
        }
        a_brightness = b;
        return true;
    }
}

namespace fo4flatvr::game
{
    float PipboyScreenBrightness()
    {
        const float b = *reinterpret_cast<const float*>(REL::Offset(0).address() + kPipBrightness);
        return std::isfinite(b) && b > 0.05f && b < 4.0f ? b : 1.0f;
    }
}

namespace fo4flatvr::game
{
    namespace
    {
        std::uint32_t CountIn(RE::TESObjectREFR* a_ref, std::uint32_t a_form)
        {
            if (!a_ref || !a_ref->inventoryList) {
                return 0;
            }
            std::uint32_t n = 0;
            for (const auto& item : a_ref->inventoryList->data) {
                if (item.object && item.object->GetFormID() == a_form) {
                    for (auto* s = item.stackData.get(); s; s = s->nextStack.get()) {
                        n += s->count;
                    }
                }
            }
            return n;
        }
    }

    std::string MenuTestCommand(const std::string& a_arg)
    {
        // 20 Nov: every task below goes to the main-idle queue (ThreadQueues.h: UI thread, no menu advancing / rendering)
        if (a_arg == "hints on" || a_arg == "hints off") {  // button-hint click fallback switch
            g_hintFallback = a_arg == "hints on";
            return "pointer " + a_arg;
        }
        if (a_arg.rfind("tree ", 0) == 0) {  // pointer tree <MenuName>: display tree of root1 (4 levels, UI task)
            const std::string name = a_arg.substr(5);
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            const bool queued = tq::RunOnUI([done, name]() {
                auto* ui = RE::UI::GetSingleton();
                Scaleform::Ptr<RE::IMenu> m = ui ? ui->GetMenu(RE::BSFixedString(name.c_str())) : nullptr;
                GValue root;
                if (!m || !m->uiMovie || !m->uiMovie->GetVariable(&root, "root1") || !root.IsObject()) {
                    done->set_value("no such menu / root1");
                    return;
                }
                std::string s = name + ":";
                TreeDump(root, 0, 3, s);
                done->set_value(s);
            });
            if (!queued) return "error: no main-thread queue";
            if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                return "error: UI task did not run within 2 s";
            }
            return fut.get();
        }
        if (a_arg == "gap ring") {  // last 16 raw VATS part x (0..1 of the screen) as the engine projected them
            std::string r = std::format("n={} raw:", g_gapVatsN.load());
            for (float v : g_gapVatsRing) r += std::format(" {:.4f}", v);
            return r;
        }
        if (a_arg == "gap on" || a_arg == "gap off") {  // 28 Nov: pointer gap on|off (runtime [Menus] bAspectGapFix, A/B tests)
            g_gapOn = a_arg == "gap on";
            return AspectGapReport();
        }
        if (a_arg.rfind("glass", 0) == 0) {  // 28 Nov: pointer glass [k] [axis]
            return GlassCommand(a_arg.size() > 6 ? a_arg.substr(6) : std::string{});
        }
        if (a_arg.rfind("movie", 0) == 0) {  // pointer movie [<MenuName>]: movie geometry (viewport, scale mode, stage) per open menu
            const std::string name = a_arg.size() > 6 ? a_arg.substr(6) : std::string{};
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            const bool queued = tq::RunOnUI([done, name]() {
                auto* ui = RE::UI::GetSingleton();
                std::string s;
                if (!ui) {
                    done->set_value("no UI");
                    return;
                }
                for (const auto& m : ui->menuStack) {
                    if (!m || !m->uiMovie || (!name.empty() && _stricmp(name.c_str(), m->menuName.c_str()) != 0)) continue;
                    s += MovieGeometry(m->menuName.c_str(), m->uiMovie.get());
                    s += " || ";
                }
                done->set_value(s.empty() ? std::string("no such menu with a movie") : s);
            });
            if (!queued) return "error: no main-thread queue";
            if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                return "error: UI task did not run within 2 s";
            }
            return fut.get();
        }
        if (a_arg == "probe") {  // pointer probe: AS3 objects under the pointer in the target menu's movie (UI task)
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            const bool queued = tq::RunOnUI([done]() {
                MenuPointerTarget         info;
                Scaleform::Ptr<RE::IMenu> m = ResolveTarget(&info);
                if (!m || !m->uiMovie) {
                    done->set_value("no target menu");
                    return;
                }
                std::string s = std::format("menu '{}': {}", info.name, ObjectsUnderMouse(m->uiMovie.get(), nullptr, nullptr));
                auto* ui = RE::UI::GetSingleton();
                Scaleform::Ptr<RE::IMenu> bar = ui ? ui->GetMenu(RE::BSFixedString("ButtonBarMenu")) : nullptr;
                if (bar && bar->uiMovie) {
                    s += " || ButtonBarMenu: " + ObjectsUnderMouse(bar->uiMovie.get(), nullptr, nullptr);
                }
                done->set_value(s);
            });
            if (!queued) return "error: no main-thread queue";
            if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                return "error: UI task did not run within 2 s";
            }
            return fut.get();
        }
        if (a_arg == "bar") {  // pointer bar: the button-hint bar's AS3 children (UI task)
            auto done = std::make_shared<std::promise<std::string>>();
            auto fut = done->get_future();
            const bool queued = tq::RunOnUI([done]() {
                auto* ui = RE::UI::GetSingleton();
                Scaleform::Ptr<RE::IMenu> bar = ui ? ui->GetMenu(RE::BSFixedString("ButtonBarMenu")) : nullptr;
                if (!bar || !bar->uiMovie) {
                    done->set_value("no ButtonBarMenu");
                    return;
                }
                std::string rep;
                double      platform = -1.0;
                BarHints(bar->uiMovie.get(), &rep, nullptr, &platform);
                std::string last;
                {
                    std::scoped_lock l(g_lock);
                    last = g_barHintLast;
                }
                done->set_value(std::format("onStack={} synthClicks={} lastClick {} | {}", bar->OnStack() ? 1 : 0, g_barSynthClicks.load(), last, rep));
            });
            if (!queued) return "error: no main-thread queue";
            if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                return "error: UI task did not run within 2 s";
            }
            return fut.get();
        }
        auto done = std::make_shared<std::promise<std::string>>();
        auto fut = done->get_future();
        const bool queued = tq::RunOnMain([done, a_arg]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                done->set_value("error: no player");
                return;
            }
            unsigned a = 0, b = 0;
            if (sscanf_s(a_arg.c_str(), "findref %x", &a) == 1) {
                // Test helper: newest reference of base form a in the player's cell (e.g. one just placed with placeatme).
                auto*         cell = player->GetParentCell();
                std::uint32_t best = 0;
                std::size_t   n = 0;
                if (cell) {
                    for (const auto& r : cell->references) {
                        const auto* base = r ? r->GetObjectReference() : nullptr;
                        if (base && base->GetFormID() == a) {
                            ++n;
                            best = (std::max)(best, r->GetFormID());
                        }
                    }
                }
                done->set_value(std::format("ref={:08X} matches={}", best, n));
                return;
            }
            const int k = sscanf_s(a_arg.c_str(), "count %x %x", &a, &b);
            if (k >= 1) {
                RE::TESObjectREFR* other = nullptr;
                if (k == 2) {
                    other = RE::TESForm::GetFormByID<RE::TESObjectREFR>(b);
                }
                done->set_value(std::format("player={} ref={}", CountIn(player, a), other ? static_cast<int>(CountIn(other, a)) : -1));
                return;
            }
            done->set_value("error: findref <hexBase> | count <hexForm> [hexRef]");
        });
        if (!queued) return "error: no main-thread queue";
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            return "error: main thread did not run the task within 2 s";
        }
        return fut.get();
    }
}
