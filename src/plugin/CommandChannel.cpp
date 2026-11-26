#include "plugin/Dlss.h"
#include "plugin/CommandChannel.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/ControllerBindings.h"
#include "plugin/Gestures.h"
#include "plugin/FaceDiag.h"

#include "plugin/CameraDirector.h"
#include "plugin/CameraTweaks.h"
#include "plugin/ConfigStore.h"
#include "plugin/ConfigUI.h"
#include "plugin/PanelHost.h"
#include "plugin/ModsResetGuard.h"
#include "plugin/Comfort.h"
#include "plugin/ComfortEffects.h"
#include "plugin/Dialogue.h"
#include "plugin/GameBridge.h"
#include "plugin/GameHud.h"
#include "plugin/HudLayer.h"
#include "plugin/MenuPointer.h"
#include "plugin/PointerBeam.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/PipboyLayer.h"
#include "plugin/GameWindow.h"
#include "plugin/HeadPose.h"
#include "plugin/Haptics.h"
#include "plugin/VirtualPad.h"
#include "plugin/Presenter.h"
#include "plugin/Roomscale.h"
#include "plugin/StereoPoC.h"
#include "plugin/VatsWindow.h"
#include "plugin/ThreadQueues.h"
#include "plugin/WorkshopHand.h"

#include <REX/LOG.h>
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR  // wingdi.h macro collides with REX::ERROR
#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>

namespace fo4flatvr::command
{
    namespace
    {
        constexpr const char* kPipeName = R"(\\.\pipe\fo4flatvr)";

        std::string Handle(const std::string& a_line)
        {
            const auto sp = a_line.find(' ');
            const std::string verb = a_line.substr(0, sp);
            const std::string arg = sp == std::string::npos ? std::string{} : a_line.substr(sp + 1);

            if (verb == "ping") {
                return "pong";
            }
            if (verb == "status") {
                return present::Status();
            }
            if (verb == "pad") {
                // pad key=value ... ms=<duration>; keys: lx ly rx ry lt rt (floats), a b x y lb rb ls rs
                // start back up down left right (0/1)
                pad::State st;
                int        ms = 500;
                std::size_t pos = 0;
                while (pos < arg.size()) {
                    const auto end = arg.find(' ', pos);
                    const std::string kv = arg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    pos = end == std::string::npos ? arg.size() : end + 1;
                    const auto eq = kv.find('=');
                    if (eq == std::string::npos) {
                        continue;
                    }
                    const std::string k = kv.substr(0, eq);
                    const float       v = std::strtof(kv.c_str() + eq + 1, nullptr);
                    const bool        b = v != 0.0f;
                    if (k == "ms") ms = static_cast<int>(v);
                    else if (k == "lx") st.lx = v;
                    else if (k == "ly") st.ly = v;
                    else if (k == "rx") st.rx = v;
                    else if (k == "ry") st.ry = v;
                    else if (k == "lt") st.lt = v;
                    else if (k == "rt") st.rt = v;
                    else if (k == "a") st.a = b;
                    else if (k == "b") st.b = b;
                    else if (k == "x") st.x = b;
                    else if (k == "y") st.y = b;
                    else if (k == "lb") st.lb = b;
                    else if (k == "rb") st.rb = b;
                    else if (k == "ls") st.ls = b;
                    else if (k == "rs") st.rs = b;
                    else if (k == "start") st.start = b;
                    else if (k == "back") st.back = b;
                    else if (k == "up") st.dpadUp = b;
                    else if (k == "down") st.dpadDown = b;
                    else if (k == "left") st.dpadLeft = b;
                    else if (k == "right") st.dpadRight = b;
                    else return "error: unknown pad key '" + k + "'";
                }
                pad::Override(st, ms);
                return std::format("pad override for {} ms (polls so far {})", ms, pad::Polls());
            }
            if (verb == "counters") {
                return present::Counters();
            }
            if (verb == "ready") {
                return game::ReadyReport();
            }
            if (verb == "background") {
                game::RenderHandles rh;
                if (!game::GetRenderHandles(rh)) {
                    return "error: renderer not ready";
                }
                window::SendToBack(rh.hwnd);
                return "sent to back";
            }
            if (verb == "rt") {
                return game::RenderTargetReport();
            }
            if (verb == "htrace") {  // head-timing trace: htrace arm <frames> | htrace dump <path> | htrace
                return head::TraceCommand(arg);
            }
            if (verb == "cam") {
                return game::CameraReport();
            }
            if (verb == "console") {
                if (arg.empty()) {
                    return "error: console needs a command";
                }
                return game::QueueConsoleCommand(arg) ? "queued" : "error: no task interface";
            }
            if (verb == "minidump") {
                // hang diagnostics: write a minidump of this process (all thread stacks) from the pipe thread
                if (arg.empty()) {
                    return "error: minidump <path>";
                }
                HANDLE f = ::CreateFileA(arg.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f == INVALID_HANDLE_VALUE) {
                    return "error: cannot create " + arg;
                }
                using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
                HMODULE dbg = ::LoadLibraryA("dbghelp.dll");
                auto* write = dbg ? reinterpret_cast<Fn>(::GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
                // MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory
                const BOOL ok = write && write(::GetCurrentProcess(), ::GetCurrentProcessId(), f, 0x1 | 0x4 | 0x1000 | 0x40, nullptr, nullptr, nullptr);
                ::CloseHandle(f);
                REX::INFO("minidump: {} -> {}", arg, ok ? "ok" : "failed");
                return ok ? "written" : "error: MiniDumpWriteDump failed";
            }
            if (verb == "fovloop") {
                game::SetFovLoopStable(arg != "old");
                return arg == "old" ? "fov loop old" : "fov loop new";
            }
            if (verb == "cropburst") {
                int      n = 0;
                char     path[400]{};
                unsigned x0 = 0, x1 = 0, y = 0, w = 0, h = 0, step = 1;
                if (std::sscanf(arg.c_str(), "%d %399s %u %u %u %u %u %u", &n, path, &x0, &x1, &y, &w, &h, &step) != 8 || n <= 0) {
                    return "error: cropburst <n> <file> <x0> <x1> <y> <w> <h> <step>";
                }
                present::RequestCropBurst(n, path, x0, x1, y, w, h, step);
                return "requested";
            }
            if (verb == "captureburst") {
                // captureburst <n> <stem>
                const auto spc = arg.find(' ');
                const int  n = std::atoi(arg.c_str());
                if (spc == std::string::npos || n <= 0) {
                    return "error: captureburst <n> <stem>";
                }
                present::RequestCaptureBurst(n, arg.substr(spc + 1));
                return "requested";
            }
            if (verb == "capture") {
                if (arg.empty()) {
                    return "error: capture needs a path";
                }
                present::RequestCapture(arg);
                return "requested";
            }
            if (verb == "pose") {
                if (arg == "off") {
                    head::ClearOverride();
                    return "override off";
                }
                float y = 0, p = 0, r = 0;
                if (sscanf_s(arg.c_str(), "%f %f %f", &y, &p, &r) < 1) {
                    return "error: pose <yawDeg> [pitchDeg] [rollDeg] | pose off";
                }
                head::SetOverride(y, p, r);
                return std::format("override yaw={} pitch={} roll={}", y, p, r);
            }
            if (verb == "posepos") {
                float x = 0, y = 0, z = 0;
                if (sscanf_s(arg.c_str(), "%f %f %f", &x, &y, &z) < 1) {
                    return "error: posepos <rightCm> [forwardCm] [upCm]";
                }
                head::SetPositionOverride(x, y, z);
                return std::format("position override right={}cm forward={}cm up={}cm", x, y, z);
            }
            if (verb == "fovreq") {
                float d = 0;
                if (sscanf_s(arg.c_str(), "%f", &d) < 1) {
                    return "error: fovreq <horizontalDeg> (0 = game default); only sticks while XR is not driving the FOV";
                }
                head::SetTargetFov(d);
                return std::format("requested worldFOV {}", d);
            }
            if (verb == "track") {
                head::SetTracking(arg != "off");
                return head::Tracking() ? "tracking on" : "tracking off";
            }
            if (verb == "posemode") {
                head::SetConvention(arg == "1" ? 1 : 0);  // 0 = base*head (default), 1 = head*base
                return std::format("convention {}", head::Convention());
            }
            if (verb == "mode") {
                if (arg != "mono" && arg != "cinema" && arg != "afr") {
                    return "error: mode cinema|mono|afr";
                }
                present::RequestMode(arg == "afr" ? 2 : arg == "mono" ? 1 : 0);
                return "requested " + arg;
            }
            if (verb == "haptic") {
                // 22 Nov (FO4VR parity suite, diagnostic): one pulse through the haptics queue (Haptics.h), so a test can show
                // its haptic detector (vremu's per-hand counter) sees a pulse the plugin sends. haptic <L|R> [amplitude] [seconds]
                char  hand = 'R';
                float amp = 0.3f, sec = 0.01f;
                if (sscanf_s(arg.c_str(), " %c %f %f", &hand, 1u, &amp, &sec) < 1 || (hand != 'L' && hand != 'R' && hand != 'l' && hand != 'r')) {
                    return "error: haptic <L|R> [amplitude] [seconds]";
                }
                const int h = (hand == 'L' || hand == 'l') ? 0 : 1;
                haptics::Request(h, amp, sec);
                return std::format("haptic queued hand={} amp={:.2f} sec={:.3f}", h ? "R" : "L", amp, sec);
            }
            if (verb == "aim" && arg == "weapon") {
                // 22 Nov (FO4VR parity suite, diagnostic, read-only): the equipped weapon instance - form, instance flags
                // (WEAPON_FLAGS; kHasScope 0x200000 = what FO4VR EF7B00 tests for raise-to-eye scoping), zoom data, ammo
                // count; and whether ScopeMenu is open. Read on the main thread. Lets a test confirm its fixed test data.
                return tq::RunOnMainWait([]() -> std::string {
                    auto* p = RE::PlayerCharacter::GetSingleton();
                    auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
                    std::string out = "weapon";
                    int         n = 0;
                    if (mh) {
                        for (auto& it : mh->equippedItems) {
                            auto* obj = it.item.object;
                            if (!obj || !obj->IsWeapon()) continue;
                            auto* inst = static_cast<RE::TESObjectWEAP::InstanceData*>(it.item.instanceData.get());
                            auto* base = static_cast<RE::TESObjectWEAP*>(obj);
                            const auto* d = inst ? inst : static_cast<RE::TESObjectWEAP::InstanceData*>(&base->weaponData);
                            const auto flags = d->flags.underlying();
                            const std::string nm(RE::TESFullName::GetFullName(*obj));
                            out += std::format(" | form={:08X} name='{}' instance={} flags=0x{:08X} hasScope={} zoom={:08X}", obj->GetFormID(),
                                nm, inst ? 1 : 0, flags, (flags & 0x00200000u) ? 1 : 0, d->zoomData ? d->zoomData->GetFormID() : 0u);
                            ++n;
                        }
                    }
                    auto* ui = RE::UI::GetSingleton();
                    out += std::format(" | weapons={} scopeMenu={}", n, ui && ui->GetMenuOpen("ScopeMenu") ? 1 : 0);
                    return out;
                }, std::chrono::milliseconds(1000), "aim weapon: main thread busy");
            }
            if (verb == "aim") {
                // aim            -> report (hand ray P/D, camera, engine pick ray, pick ref, fire/projectile)
                // aim on|off     -> runtime switch (hooks must have been installed at load)
                // aim conv 0|1   -> debug: camera-local -> world via matrix rows (0, engine) or columns (1)
                if (arg == "on" || arg == "off") {
                    game::SetAimEnabled(arg == "on");
                    return std::format("aim {}", game::AimEnabled() ? "on" : "off (or hooks not installed)");
                }
                if (arg == "conv 0" || arg == "conv 1") {
                    game::SetAimConvention(arg.back() - '0');
                    return "aim " + arg;
                }
                // aim lof|recoil|autoaim|clamp|throw on|off -> runtime switches of the follow-ups; aim probe -> wall distances
                for (const char* f : { "lof", "recoil", "autoaim", "clamp", "throw", "vatsshot" }) {
                    const std::string pre = std::string(f) + " ";
                    if (arg == pre + "on" || arg == pre + "off") {
                        return game::SetAimFeature(f, arg == pre + "on");
                    }
                }
                if (arg == "probe") {
                    game::RequestAimProbe();
                    return "aim probe requested (see probe= in 'aim')";
                }
                if (arg.rfind("equip ", 0) == 0) {  // test helper: equip + draw <hexFormID> (must be in inventory)
                    const auto id = static_cast<std::uint32_t>(std::strtoul(arg.c_str() + 6, nullptr, 16));
                    game::RequestEquip(id);
                    return std::format("aim equip {:08X} requested", id);
                }
                if (arg == "pickhand 0" || arg == "pickhand 1") {  // test aid: which hand's ray the pick uses (bindings switch it per press)
                    game::SetPickHand(arg.back() - '0');
                    return "aim " + arg;
                }
                // aim equip0 <id>: equip through the equip manager without drawing (draw with the controller binding);
                // aim unequip <id>: unequip through the equip manager
                if (arg.rfind("equip0 ", 0) == 0 || arg.rfind("unequip ", 0) == 0) {
                    const bool un = arg[0] == 'u';
                    const auto id = static_cast<std::uint32_t>(std::strtoul(arg.c_str() + (un ? 8 : 7), nullptr, 16));
                    game::RequestEquip(id, un ? 2 : 1);
                    return std::format("aim {} {:08X} requested", un ? "unequip" : "equip0", id);
                }
                if (!arg.empty()) {
                    return "error: aim [on|off|conv 0|conv 1|equip <hexFormID>|lof|recoil|autoaim|clamp on|off|probe]";
                }
                return game::AimReport();
            }
            if (verb == "hud") {
                // hud                -> status (state, counters, hook stats, last hudcap statistics)
                // hud on|off         -> HUD separation (world view only; needs the hooks from load)
                // hud src 3e|3f      -> engine target shown on the layer; hud premul 0|1 -> layer alpha mode
                if (arg == "on" || arg == "off") {
                    hud::SetEnabled(arg == "on");
                    return std::format("hud {}", hud::Enabled() ? "on" : arg == "on" ? "off (hooks not installed)" : "off");
                }
                if (arg == "src 3e" || arg == "src 3f") {
                    hud::SetSource(arg.back() == 'e' ? 0x3E : 0x3F);
                    return "hud " + arg;
                }
                if (arg == "premul 0" || arg == "premul 1") {
                    hud::SetPremultiplied(arg.back() == '1');
                    return "hud " + arg;
                }
                if (arg == "crosshair 0" || arg == "crosshair 1") {
                    hud::SetHideCrosshair(arg.back() == '1');
                    return "hud " + arg;
                }
                if (arg == "fw") {  // HUDFramework widget guard: matrices, widget stage bounds, layer angles
                    return game::HudFrameworkStatus();
                }
                if (arg == "clear on" || arg == "clear off") {  // [HUD] bClearTargets (lingering HUD elements)
                    hud::SetClearHudTargets(arg == "clear on");
                    return "hud " + arg;
                }
                if (arg == "split") {  // 20 Nov: FO4VR HUD split (hand HUD, chin compass): config, clip rectangles, counters
                    return hud::SplitStatus();
                }
                if (arg.rfind("set ", 0) == 0) {  // hud set <[HUD] key> <value>: live (config panel / MCM apply)
                    const std::string kv = arg.substr(4);
                    const auto        kvSp = kv.find(' ');
                    if (kvSp == std::string::npos) {
                        return "error: hud set <key> <value>";
                    }
                    return hud::SetSplitValue(kv.substr(0, kvSp), kv.substr(kvSp + 1));
                }
                if (!arg.empty()) {
                    return "error: hud [on|off|src 3e|src 3f|premul 0|premul 1|crosshair 0|crosshair 1|clear on|off|split|set <key> <value>]";
                }
                return hud::Status() + " | " + hud::SplitStatus();
            }
            if (verb == "pipboy") {
                // pipboy             -> status (state, capture counts, quad pose, last pointer hit uv, game side)
                // pipboy on|off      -> wrist Pip-Boy for the next open (hooks from load); pipboy src 3c|3d
                if (arg == "on" || arg == "off") {
                    pipboy::SetEnabled(arg == "on");
                    return std::format("pipboy {}", pipboy::Enabled() ? "on" : arg == "on" ? "off (hooks not installed)" : "off");
                }
                if (arg == "src 3c" || arg == "src 3d") {
                    pipboy::SetSource(arg.back() == 'c' ? 0x3C : 0x3D);
                    return "pipboy " + arg;
                }
                if (arg == "tint on" || arg == "tint off") {  // [Pipboy] bTint at runtime
                    pipboy::SetTint(arg == "tint on");
                    return "pipboy " + arg;
                }
                if (arg == "floating on" || arg == "floating off") {  // [Pipboy] bFloating at runtime (next open)
                    pipboy::SetFloatingEnabled(arg == "floating on");
                    return std::format("pipboy floating {}", pipboy::FloatingEnabled() ? "on" : arg == "floating on" ? "off (hooks not installed)" : "off");
                }
                if (!arg.empty()) {
                    return "error: pipboy [on|off|src 3c|src 3d]";
                }
                return pipboy::Status();
            }
            if (verb == "pa") {
                // pa                       -> power armour: PA HUD capture (HUD layer) + projected Pip-Boy state
                // pa hud|rain|pipboy on|off -> [PowerArmor] bHudOnLayer / bHideVisorRain / bProjectedPipboy at runtime
                if (arg == "hud on" || arg == "hud off") {
                    hud::SetPaHudOnLayer(arg == "hud on");
                    return "pa " + arg;
                }
                if (arg == "rain on" || arg == "rain off") {  // "rain off" = rain hidden
                    hud::SetHideVisorRain(arg == "rain off");
                    return "pa " + arg;
                }
                if (arg == "pipboy on" || arg == "pipboy off") {
                    pipboy::SetProjectedEnabled(arg == "pipboy on");
                    return std::format("pa pipboy {}", pipboy::ProjectedEnabled() ? "on" : arg == "pipboy on" ? "off (hooks not installed)" : "off");
                }
                if (!arg.empty()) {
                    return "error: pa [hud on|off|rain on|off|pipboy on|off]";
                }
                return hud::PaStatus() + " | " + game::HudHookStats() + " | pipboy " + pipboy::Status();
            }
            if (verb == "pipcap") {
                if (arg.empty()) {
                    return "error: pipcap needs a path";
                }
                pipboy::RequestCapture(arg);
                return "requested";
            }
            if (verb == "hudcap") {
                if (arg.empty()) {
                    return "error: hudcap needs a path";
                }
                hud::RequestCapture(arg);
                return "requested";
            }
            if (verb == "pointer") {
                // pointer         -> menu laser pointer (D2): target menu, uv, quad/aim, movie viewport, last event
                // pointer on|off  -> runtime switch ([Menus] bPointer)
                if (arg == "on" || arg == "off") {
                    menuptr::SetEnabled(arg == "on");
                    return std::format("pointer {}", menuptr::Enabled() ? "on" : "off");
                }
                if (arg.rfind("findref ", 0) == 0 || arg.rfind("count ", 0) == 0 || arg == "bar" || arg == "probe" || arg.rfind("tree ", 0) == 0 || arg.rfind("hints ", 0) == 0) {  // test helpers; bar = button-hint dump, probe = AS3 objects under the pointer
                    return game::MenuTestCommand(arg);
                }
                if (!arg.empty()) {
                    return "error: pointer [on|off|findref <hexBase>|count <hexForm> [hexRef]]";
                }
                return menuptr::Status();
            }
            if (verb == "wshand") {
                // wshand [on|off|ini] -> workshop placement / highlight along the hand ray (WorkshopHand.h)
                return wshand::Command(arg);
            }
            if (verb == "beam") {
                // beam         -> pointer beam (PointerBeam.h): offers per source, world mode, last beam per hand
                // beam on|off  -> runtime switch ([Beam] bBeam)
                if (arg == "on" || arg == "off") {
                    beam::SetEnabled(arg == "on");
                    return std::format("beam {}", beam::Enabled() ? "on" : "off");
                }
                if (!arg.empty()) {
                    return "error: beam [on|off]";
                }
                return beam::Status();
            }
            if (verb == "menus") {
                return game::MenusReport();
            }
            if (verb == "menupanel") {
                // menupanel [on|off] -> [Menus] bPanel: Pause/Favorites/MessageBox over the live world view
                if (arg == "clear on" || arg == "clear off") {  // [Menus] bClearMenuTargets (pause-menu decal fix)
                    hud::SetClearMenuTargets(arg == "clear on");
                    return "menupanel " + arg;
                }
                if (arg == "modmenus on" || arg == "modmenus off" || arg == "modmenus ini" || arg == "modmenus 1" || arg == "modmenus 0") {
                    // 20 Nov: [Menus] bPanelModMenus live (mod / F4SE menus on the anchored panel; "ini" = back to the file)
                    game::SetModPanelMenus(arg == "modmenus ini" ? -1 : (arg == "modmenus on" || arg == "modmenus 1") ? 1 : 0);
                    return "menupanel " + arg;
                }
                if (arg == "on" || arg == "off") {
                    hud::SetMenuPanelEnabled(arg == "on");
                    return std::format("menupanel {}", hud::MenuPanelEnabled() ? "on" : arg == "on" ? "off (hooks not installed)" : "off");
                }
                return hud::MenuPanelStatus() + " | " + game::PausedCameraReport() + " | " + game::MenuBlurReport();
            }
            if (verb == "uimsg") {
                // test aid: "uimsg <MenuName> show|hide" -> UIMessageQueue::AddMessage on the game's task queue
                // (opens the pause menu etc. without input focus)
                const auto spc = arg.find(' ');
                const std::string menu = arg.substr(0, spc);
                const std::string what = spc == std::string::npos ? "show" : arg.substr(spc + 1);
                if (menu.empty() || (what != "show" && what != "hide")) {
                    return "error: uimsg <MenuName> show|hide";
                }
                tq::RunOnMain([menu, show = what == "show"]() {  // main-idle queue (ThreadQueues.h)
                    if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                        REX::INFO("uimsg: {} {}", menu, show ? "show" : "hide");
                        q->AddMessage(RE::BSFixedString(menu.c_str()), show ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide);
                    }
                });
                return "queued";
            }
            if (verb == "threads") {
                return tq::Status();  // 20 Nov: main-idle / render queues (ThreadQueues.h, analysis/threading.md)
            }
            if (verb == "camdir") {
                // camera step 1: camdir [on|off|table|policy <state> <policy>|fadetest [holdMs]]
                return camdir::Command(arg);
            }
            if (verb == "facediag") {
                return facediag::Command(arg);  // player's FaceGen head: geometry, shader flags, textures (GPU read-back)
            }
            if (verb == "sneakstep") {
                return game::SneakStepCommand(arg);  // 20 Nov: sneak eye height step [on|off] + learned levels
            }
            if (verb == "gesture") {
                return gestures::Command(arg);  // VR gestures + selfie: gesture [status|reload|on|off|test <R|L> <slot>|selfie on|off]
            }
            if (verb == "bindings") {
                return bindings::Command(arg);  // FO4VR controller bindings: bindings [on|off] (readout: context + last pad)
            }
            if (verb == "gun") {
                return gun::Command(arg);  // floating gun: gun [on|off|arms [on|off]|dump|cfg <k> <v>]
            }
            if (verb == "nospread") {
                return game::NoSpreadCommand(arg);  // [Weapon] bNoSpread: nospread [status|on|off|pellets on|off|reset|hist]
            }
            if (verb == "frik") {
                return frikbridge::Command(arg);  // FRIK-flat bridge: frik [status|smooth on|off|reset]
            }
            if (verb == "hands") {
                return "error: hands retired (FRIK/ROCK scope); the FO4VR-baseline floating gun is 'gun'";
            }
            if (verb == "dlg") {
                // VR dialogue: dlg [status|panel|pointer|look|weapon|haptics|stick on|off|cap <path>|cfg <k> <v>]
                return dlg::Command(arg);
            }
            if (verb == "comfort") {
                // C8: comfort [cfg [<k> <v>]|turn snap|smooth|off|move head|hand|body|vignette on|off|stats reset]
                return comfort::Command(arg);
            }
            if (verb == "vatswin") {
                // VATS camera window: vatswin [status|on|off|every n|scene 0|1|fov d|exposure x|mask 0|1|cap <bmp>]
                return vatswin::Command(arg);
            }
            if (verb == "comfx") {
                // comfort effects: comfx [status | shake|radial|double|blur|dof 0|1]
                return comfx::Command(arg);
            }
            if (verb == "rs") {
                return rs::Command(arg);  // roomscale + body yaw: rs [on|off|reset|hist|cfg <k> <v>]
            }
            if (verb.rfind("stereo", 0) == 0) {
                // stereo [0..4] | stereoskip 0|1 | stereoipd <m> | stereotest 0|1 | stereofov <halfDeg> |
                // stereojitter 0|1 | stereocap <path.bmp> | stereostat | stereoprepass|stereo1st|stereoclear 0|1 |
                // stereoscope 0|1 | stereogbcap <stem> | stereocov <stem>
                auto c = stereo::CurrentConfig();
                const float v = std::strtof(arg.c_str(), nullptr);
                if (verb == "stereo" || verb == "stereostat") {
                    if (verb == "stereo" && !arg.empty()) {
                        const int m = std::atoi(arg.c_str());
                        if (m < 0 || m > 4) {
                            return "error: stereo 0 off | 1 census | 2 debug stereo | 3 faithful stereo | 4 identity";
                        }
                        if (!stereo::HooksEnabled()) {
                            return "error: stereo hooks not installed at load ([Stereo] iMode=0 and bInstallHooks=0)";
                        }
                        stereo::SetMode(static_cast<stereo::Mode>(m));
                    }
                    return stereo::Status();
                }
                if (verb == "stereoskip") {
                    c.skipUnconverted = v != 0.0f;
                } else if (verb == "stereoipd") {
                    c.testIpdMeters = v;
                } else if (verb == "stereotest") {
                    c.testEyes = v != 0.0f;
                } else if (verb == "stereofov") {
                    if (v < 5.0f || v > 85.0f) {
                        return "error: stereofov <horizontal half FOV of each test eye, 5..85 deg>";
                    }
                    c.testHalfFovDeg = v;
                } else if (verb == "stereodump") {
                    stereo::RequestDrawDump();
                    return "requested (log: stereodump lines)";
                } else if (verb == "stereodepth") {
                    c.ownDepth = v != 0.0f;
                } else if (verb == "stereoprepass") {
                    c.stereoPrepass = v != 0.0f;  // z-prepass in stereo (early-Z) vs bOwnDepth fallback
                } else if (verb == "stereo1st") {
                    c.firstPerson = v != 0.0f;
                } else if (verb == "stereoscope") {
                    c.scope = static_cast<int>(v);
                } else if (verb == "stereoclear") {
                    c.clearGBuffer = v != 0.0f;  // measurement: clear the GBuffer targets at the frame's first GBuffer draw
                } else if (verb == "stereogbcap" || verb == "stereocov") {
                    if (arg.empty()) {
                        return "error: " + verb + " needs a path stem";
                    }
                    stereo::RequestGBufferCapture(arg, verb == "stereocov");
                    return "requested (log: stereogb / stereocov lines)";
                } else if (verb == "stereojitter") {
                    c.noJitter = v == 0.0f;  // "stereojitter 0" = force TAA jitter off
                } else if (verb == "stereoeyecap") {
                    if (arg.empty()) {
                        return "error: stereoeyecap needs a path stem";
                    }
                    present::RequestStereoEyeCapture(arg);
                    return "requested";
                } else if (verb == "stereocap") {
                    if (arg.empty()) {
                        return "error: stereocap needs a path";
                    }
                    stereo::RequestCapture(arg);
                    return "requested";
                } else if (auto r = stereo::Command(verb, arg); !r.empty()) {
                    return r;  // lighting round: stereosplit|stereosao|stereocut|stereocuts|stereostagecap
                } else {
                    return "error: unknown stereo command '" + verb + "'";
                }
                stereo::Configure(c);
                return stereo::Status();
            }
            if (verb == "camcfg") {
                // camcfg -> [Camera] step-0 switches, the settings' values, PlayerCamera FOV-adjust fields
                // camcfg noaimzoom|fpdialogue|nokillcams|noidleorbit 0|1 ; camcfg aimzoom on|off (= noaimzoom 0|1)
                return game::camtweak::Command(arg);
            }
            if (verb == "recenter") {
                present::RequestRecenter();
                return "requested";
            }
            if (verb == "gpuprof") {
                return stereo::Command(verb, arg);  // perf pass: GPU ms per render pass (GpuProf.inl): gpuprof <frames> | gpuprof
            }
            if (verb == "dlss") {
                return dlss::Command(arg);  // 20 Nov: DLSS / neural rendering (Dlss.h)
            }
            if (verb == "modsreset") {
                return modsreset::Status();  // 26 Nov: mod-change reset prompt guard (ModsResetGuard.h)
            }
            if (verb == "panels") {
                return panelhost::Command(arg);  // 26 Nov: other plugins' VR panels (PanelHost.h)
            }
            if (verb == "cfgui") {
                return configui::Command(arg);  // 20 Nov: VR settings panel (ConfigUI.h)
            }
            if (verb == "cfg") {
                if (arg == "subs") return cfgstore::SubscribersStatus();  // 20 Nov: live-settings reloads (ConfigStore.h)
                return cfgstore::Status();  // 20 Nov: settings layers (ConfigStore.h)
            }
            return "error: unknown command '" + verb + "'";
        }

        void Serve(HANDLE a_pipe)
        {
            std::string buffer;
            char        chunk[512];
            for (;;) {
                DWORD read = 0;
                if (!::ReadFile(a_pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
                    return;  // client went away
                }
                buffer.append(chunk, read);
                std::size_t nl;
                while ((nl = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.empty()) {
                        continue;
                    }
                    REX::INFO("cmd: {}", line);
                    std::string reply = Handle(line) + "\n";
                    DWORD       written = 0;
                    if (!::WriteFile(a_pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr)) {
                        return;
                    }
                }
            }
        }

        void Run()
        {
            // The game may run elevated; the default pipe DACL then gives other users read-only
            // access. Grant read/write to authenticated local users (remote clients are rejected).
            SECURITY_ATTRIBUTES  sa{ sizeof(sa), nullptr, FALSE };
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (::ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
                    SDDL_REVISION_1, &sd, nullptr)) {
                sa.lpSecurityDescriptor = sd;
            } else {
                REX::WARN("command pipe: SDDL conversion failed ({}); using default security", ::GetLastError());
            }
            for (;;) {
                HANDLE pipe = ::CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 4096, 4096, 0, sd ? &sa : nullptr);
                if (pipe == INVALID_HANDLE_VALUE) {
                    REX::ERROR("command pipe: CreateNamedPipe failed ({})", ::GetLastError());
                    return;
                }
                const bool connected = ::ConnectNamedPipe(pipe, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
                if (connected) {
                    Serve(pipe);
                }
                ::DisconnectNamedPipe(pipe);
                ::CloseHandle(pipe);
            }
        }
    }

    std::string Execute(const std::string& a_line)
    {
        return Handle(a_line);
    }

    void Start()
    {
        std::thread(Run).detach();
        REX::INFO("command pipe listening on {}", kPipeName);
    }
}
