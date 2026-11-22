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
#include <deque>
#include <format>
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
            ev.x = a_e.u * bw;
            ev.y = a_e.v * bh;
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
                ev.x = mu * bw;
                ev.y = mv * bh;
                ev.scrollDelta = e.wheel;
                const auto rc = dst->uiMovie->HandleEvent(*reinterpret_cast<const Scaleform::GFx::Event*>(&ev));
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

    void ReloadPointerSettings()
    {
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
        return ui && a_name && ui->GetMenuOpen(a_name);
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
