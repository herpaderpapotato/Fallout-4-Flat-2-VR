// VR gestures, VRIK style. See Gestures.h and analysis/vr-gestures.md.
#include "plugin/Gestures.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/CameraDirector.h"
#include "plugin/ConfigUI.h"
#include "plugin/FaceDiag.h"
#include "plugin/FrikBridge.h"
#include "plugin/GameBridge.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/Haptics.h"
#include "plugin/Settings.h"
#include "xr/XrRuntime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fo4flatvr::gestures
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int kSlots = 13;
        constexpr const char* kSlotNames[kSlots] = { "Tap", "Up", "Down", "Left", "Right", "Back", "Fwd", "UpDown", "DownUp", "LeftRight",
            "RightLeft", "BackFwd", "FwdBack" };
        constexpr const char* kHandKey[2] = { "L", "R" };
        enum Action
        {
            kNone = 0,
            kKey = 1,
            kRecentre = 2,
            kPov = 3,
            kCycle = 4,
            kConfig = 5,  // 20 Nov: the VR settings panel (ConfigUI.h), default on right Up+Down
            kActions
        };
        constexpr const char* kActionNames[kActions] = { "none", "key", "recentre", "pov", "cycle", "config" };

        // Key list of the MCM dropdowns (index -> virtual-key code). APPEND ONLY: the MCM ini stores the index.
        // plugin/tools/gen_mcm_config.py reads the names between the markers for config.json.
        struct KeyDef
        {
            const char* name;
            int         vk;
        };
        // MCM-KEYS-BEGIN
        constexpr KeyDef kKeys[] = {
            { "(none)", 0 },
            { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 }, { "F5", 0x74 }, { "F6", 0x75 },
            { "F7", 0x76 }, { "F8", 0x77 }, { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
            { "1", 0x31 }, { "2", 0x32 }, { "3", 0x33 }, { "4", 0x34 }, { "5", 0x35 }, { "6", 0x36 }, { "7", 0x37 },
            { "8", 0x38 }, { "9", 0x39 }, { "0", 0x30 },
            { "A", 0x41 }, { "B", 0x42 }, { "C", 0x43 }, { "D", 0x44 }, { "E", 0x45 }, { "F", 0x46 }, { "G", 0x47 },
            { "H", 0x48 }, { "I", 0x49 }, { "J", 0x4A }, { "K", 0x4B }, { "L", 0x4C }, { "M", 0x4D }, { "N", 0x4E },
            { "O", 0x4F }, { "P", 0x50 }, { "Q", 0x51 }, { "R", 0x52 }, { "S", 0x53 }, { "T", 0x54 }, { "U", 0x55 },
            { "V", 0x56 }, { "W", 0x57 }, { "X", 0x58 }, { "Y", 0x59 }, { "Z", 0x5A },
            { "Num 0", 0x60 }, { "Num 1", 0x61 }, { "Num 2", 0x62 }, { "Num 3", 0x63 }, { "Num 4", 0x64 }, { "Num 5", 0x65 },
            { "Num 6", 0x66 }, { "Num 7", 0x67 }, { "Num 8", 0x68 }, { "Num 9", 0x69 },
            { "Num *", 0x6A }, { "Num +", 0x6B }, { "Num -", 0x6D }, { "Num .", 0x6E }, { "Num /", 0x6F },
            { "Insert", 0x2D }, { "Delete", 0x2E }, { "Home", 0x24 }, { "End", 0x23 }, { "Page Up", 0x21 }, { "Page Down", 0x22 },
            { "Up arrow", 0x26 }, { "Down arrow", 0x28 }, { "Left arrow", 0x25 }, { "Right arrow", 0x27 },
            { "Space", 0x20 }, { "Enter", 0x0D }, { "Tab", 0x09 }, { "Backspace", 0x08 }, { "Escape", 0x1B },
            { "; :", 0xBA }, { "= +", 0xBB }, { ", <", 0xBC }, { "- _", 0xBD }, { ". >", 0xBE }, { "/ ?", 0xBF },
            { "` ~ (console)", 0xC0 }, { "[ {", 0xDB }, { "\\ |", 0xDC }, { "] }", 0xDD }, { "' \"", 0xDE },
        };
        // MCM-KEYS-END
        constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
        constexpr const char* kModNames[4] = { "none", "Shift", "Ctrl", "Alt" };
        constexpr int         kModVk[4] = { 0, VK_LSHIFT, VK_LCONTROL, VK_LMENU };

        struct Slot
        {
            int action{ kNone };
            int key{ 0 };  // kKeys index
            int mod{ 0 };  // kModNames index
        };

        struct Cfg
        {
            bool  enabled{ true };            // [Gestures] bEnabled
            bool  hand[2]{ false, true };     // bLeftHand (L3 = sprint: off by default) / bRightHand
            Slot  slot[2][kSlots]{};          // i<L|R><Slot>Action / Key / Mod
            float moveCm{ 11.0f };            // fMoveCm (VRIK 8 u = 11.4 cm)
            float dominance{ 1.5f };          // fDominance (VRIK: none)
            float maxHoldS{ 2.0f };           // fMaxHoldS
            float cooldownS{ 0.3f };          // fCooldownS (VRIK 0.1)
            float keyHoldMs{ 100.0f };        // fKeyHoldMs
            float buttonDelayS{ 0.5f };       // fButtonDelayS (VRIK 0.5): a game-used click reaches the game after this, held still
            bool  allViews{ true };           // bAllViews (20 Nov): every camera view, not only the first-person world view
            bool  haptics{ true };            // bHaptics
            float hapticAmp{ 0.5f };          // fHapticAmp
            float hapticS{ 0.06f };           // fHapticS
            // [Selfie]
            bool  selfie{ true };             // bEnabled
            bool  selfieKeepInMenus{ true };  // bKeepInMenus: pause, Pip-Boy and menus keep it (19 Nov, user: to screenshot it)
            int   selfieHands{ 2 };           // iHands: 0 off hand, 1 main hand, 2 both hands (VRIK selfieModeHands)
            float handsUpCm{ 25.0f };         // fHandsUpCm (VRIK 18 u = 26 cm)
            float holdS{ 0.5f };              // fHoldS (VRIK 4 frames)
            float distanceM{ 1.4f };          // fDistanceM (VRIK 100 u = 1.4 m)
            float rollGain{ 1.0f };           // fRollGain (VRIK: the body follows the hand's heading 1:1)
            float rollDeadzoneDeg{ 0.0f };    // fRollDeadzoneDeg (VRIK: none)
        };

        struct Press
        {
            bool              down{ false };
            bool              ignored{ false };   // started in cooldown / outside gameplay / no pose: never a gesture
            bool              aborted{ false };
            bool              passed{ false };    // button delay passed: this press belongs to the game
            Clock::time_point t0{};
            float             fwd[3]{}, right[3]{};  // head yaw frame at press (OpenXR, horizontal)
            float             start[3]{}, mn[3]{}, mx[3]{}, last[3]{};  // hand rel. head: x right, y forward, z up (m)
            float             maxDist{ 0.0f };    // largest single-axis excursion from the start (VRIK maxDist)
            bool              latch{ false };     // stick held at zero until back in the dead zone
        };

        struct KeyJob
        {
            int               vk{ 0 }, modVk{ 0 };
            Clock::time_point up{};
        };

        struct State
        {
            Cfg               cfg;
            bool              runtimeOff{ false };  // pipe "gesture off"
            Press             press[2];
            Clock::time_point cooldownUntil{};
            std::vector<KeyJob> keys;
            bool              recentreReq{ false };
            Clock::time_point lastRecentre{}, lastNotGameplay{};
            // config file watch
            Clock::time_point nextPoll{};
            std::filesystem::file_time_type ourTime{}, mcmTime{};
            bool              ourExists{ false }, mcmExists{ false };
            bool              loaded{ false };
            unsigned          reloads{ 0 };
            // selfie
            bool              upLatched{ false };
            int               upFrames{ 0 };
            Clock::time_point upSince{};
            bool              refValid{ false };
            float             ref[3]{};             // steering axis at toggle (OpenXR, horizontal unit)
            bool              noFrikLogged{ false };
            // stats
            unsigned          fired[2][kSlots]{}, unassigned{ 0 }, rejected{ 0 }, aborts{ 0 }, keySends{ 0 }, keyNoFocus{ 0 },
                delayPasses{ 0 }, selfieOn{ 0 }, selfieOff{ 0 }, selfieAutoOff{ 0 };
            std::string       last{ "none" };
            bool              viewGameplay{ false }, viewAny{ false };  // last frame's Frame::gameplay / anyView (status)
            int               viewCam{ -1 };
        };
        State g_s;

        std::mutex g_selfieLock;
        Selfie     g_selfie;

        // ---- math (OpenXR: +X right, +Y up, -Z forward) ---------------------------------------------------------------
        void Rot(const xr::Quat& q, const float v[3], float o[3])
        {
            const float ux = q.x, uy = q.y, uz = q.z;
            const float cx = uy * v[2] - uz * v[1], cy = uz * v[0] - ux * v[2], cz = ux * v[1] - uy * v[0];
            const float tx = 2 * cx, ty = 2 * cy, tz = 2 * cz;
            const float c2x = uy * tz - uz * ty, c2y = uz * tx - ux * tz, c2z = ux * ty - uy * tx;
            o[0] = v[0] + q.w * tx + c2x, o[1] = v[1] + q.w * ty + c2y, o[2] = v[2] + q.w * tz + c2z;
        }
        bool Horizontal(const float v[3], float o[3], float a_minLen)
        {
            const float l = std::sqrt(v[0] * v[0] + v[2] * v[2]);
            if (!(l > a_minLen)) return false;
            o[0] = v[0] / l, o[1] = 0.0f, o[2] = v[2] / l;
            return true;
        }
        void YawFrame(const xr::Pose& a_head, float a_fwd[3], float a_right[3])
        {
            const float nz[3]{ 0.0f, 0.0f, -1.0f };
            float       f[3];
            Rot(a_head.orientation, nz, f);
            if (!Horizontal(f, a_fwd, 0.05f)) a_fwd[0] = 0.0f, a_fwd[1] = 0.0f, a_fwd[2] = -1.0f;
            a_right[0] = -a_fwd[2], a_right[1] = 0.0f, a_right[2] = a_fwd[0];
        }
        void Local(const Press& p, const xr::Pose& a_hand, const xr::Pose& a_head, float o[3])
        {
            const float r[3]{ a_hand.position.x - a_head.position.x, a_hand.position.y - a_head.position.y, a_hand.position.z - a_head.position.z };
            o[0] = r[0] * p.right[0] + r[2] * p.right[2];  // right
            o[1] = r[0] * p.fwd[0] + r[2] * p.fwd[2];      // forward
            o[2] = r[1];                                   // up
        }

        // ---- settings -----------------------------------------------------------------------------------------------
        std::string McmPath() { return std::filesystem::absolute("Data/MCM/Settings/fo4flatvr.ini").string(); }

        struct Reader
        {
            std::string a, b;  // our ini, MCM user ini (b overrides a)
            int I(const char* s, const char* k, int d) const
            {
                d = static_cast<int>(::GetPrivateProfileIntA(s, k, d, a.c_str()));
                return b.empty() ? d : static_cast<int>(::GetPrivateProfileIntA(s, k, d, b.c_str()));
            }
            float F(const char* s, const char* k, float d) const
            {
                for (const auto* p : { &a, &b }) {
                    if (p->empty()) continue;  // (an empty path would be win.ini)
                    char buf[64]{};
                    ::GetPrivateProfileStringA(s, k, "", buf, sizeof(buf), p->c_str());
                    if (buf[0]) {
                        char*       e = nullptr;
                        const float v = std::strtof(buf, &e);
                        if (e != buf && std::isfinite(v)) d = v;
                    }
                }
                return d;
            }
        };

        void Load(State& s)
        {
            // 20 Nov: Settings::Path() is the merged file (fo4flatvr.ini < MCM < VR-panel previews, ConfigStore.h); reading MCM's
            // file on top again would hide an unsaved panel preview of a key MCM also has
            const Reader r{ Settings::Path(), std::string{} };
            Cfg          c;
            c.enabled = r.I("Gestures", "bEnabled", 1) != 0;
            c.hand[0] = r.I("Gestures", "bLeftHand", 0) != 0;
            c.hand[1] = r.I("Gestures", "bRightHand", 1) != 0;
            for (int h = 0; h < 2; ++h) {
                for (int i = 0; i < kSlots; ++i) {
                    const std::string base = std::string("i") + kHandKey[h] + kSlotNames[i];
                    auto& sl = c.slot[h][i];
                    // 20 Nov: right Up+Down opens the VR settings panel unless assigned otherwise
                    const int def = (h == 1 && std::string(kSlotNames[i]) == "UpDown") ? kConfig : kNone;
                    sl.action = std::clamp(r.I("Gestures", (base + "Action").c_str(), def), 0, kActions - 1);
                    sl.key = std::clamp(r.I("Gestures", (base + "Key").c_str(), 0), 0, kKeyCount - 1);
                    sl.mod = std::clamp(r.I("Gestures", (base + "Mod").c_str(), 0), 0, 3);
                }
            }
            c.moveCm = std::clamp(r.F("Gestures", "fMoveCm", c.moveCm), 3.0f, 60.0f);
            c.dominance = std::clamp(r.F("Gestures", "fDominance", c.dominance), 1.0f, 5.0f);
            c.maxHoldS = std::clamp(r.F("Gestures", "fMaxHoldS", c.maxHoldS), 0.3f, 10.0f);
            c.cooldownS = std::clamp(r.F("Gestures", "fCooldownS", c.cooldownS), 0.0f, 3.0f);
            c.keyHoldMs = std::clamp(r.F("Gestures", "fKeyHoldMs", c.keyHoldMs), 20.0f, 1000.0f);
            c.buttonDelayS = std::clamp(r.F("Gestures", "fButtonDelayS", c.buttonDelayS), 0.1f, 3.0f);
            c.allViews = r.I("Gestures", "bAllViews", 1) != 0;
            c.haptics = r.I("Gestures", "bHaptics", 1) != 0;
            c.hapticAmp = std::clamp(r.F("Gestures", "fHapticAmp", c.hapticAmp), 0.0f, 1.0f);
            c.hapticS = std::clamp(r.F("Gestures", "fHapticS", c.hapticS), 0.0f, 1.0f);
            c.selfie = r.I("Selfie", "bEnabled", 1) != 0;
            c.selfieKeepInMenus = r.I("Selfie", "bKeepInMenus", 1) != 0;
            c.selfieHands = std::clamp(r.I("Selfie", "iHands", 2), 0, 2);
            c.handsUpCm = std::clamp(r.F("Selfie", "fHandsUpCm", c.handsUpCm), 5.0f, 80.0f);
            c.holdS = std::clamp(r.F("Selfie", "fHoldS", c.holdS), 0.0f, 5.0f);
            c.distanceM = std::clamp(r.F("Selfie", "fDistanceM", c.distanceM), 0.5f, 5.0f);
            c.rollGain = std::clamp(r.F("Selfie", "fRollGain", c.rollGain), -5.0f, 5.0f);
            c.rollDeadzoneDeg = std::clamp(r.F("Selfie", "fRollDeadzoneDeg", c.rollDeadzoneDeg), 0.0f, 45.0f);
            s.cfg = c;
            ++s.reloads;
            int assigned = 0;
            for (const auto& hs : c.slot)
                for (const auto& sl : hs) assigned += sl.action != kNone;
            REX::INFO("gestures: settings (re)loaded #{}: enabled={} hands L={} R={} assigned={} move={:.1f}cm dom={:.2f} selfie={} hands={} "
                      "up={:.0f}cm hold={:.2f}s dist={:.2f}m gain={:.2f} (fo4flatvr.ini [Gestures]/[Selfie] < {})",
                s.reloads, c.enabled ? 1 : 0, c.hand[0] ? 1 : 0, c.hand[1] ? 1 : 0, assigned, c.moveCm, c.dominance, c.selfie ? 1 : 0, c.selfieHands,
                c.handsUpCm, c.holdS, c.distanceM, c.rollGain, McmPath());
        }

        bool Stamp(const std::string& a_path, std::filesystem::file_time_type& a_t, bool& a_exists)
        {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(a_path, ec);
            const bool ex = !ec;
            const bool changed = ex != a_exists || (ex && t != a_t);
            a_exists = ex;
            if (ex) a_t = t;
            return changed;
        }

        void Poll(State& s, Clock::time_point a_now)
        {
            if (s.loaded && a_now < s.nextPoll) return;
            s.nextPoll = a_now + std::chrono::milliseconds(1000);
            const bool a = Stamp(Settings::Path(), s.ourTime, s.ourExists);
            const bool b = Stamp(McmPath(), s.mcmTime, s.mcmExists);
            if (!s.loaded || a || b) {
                s.loaded = true;
                Load(s);
            }
        }

        // ---- actions --------------------------------------------------------------------------------------------------
        bool GameFocused()
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
            return pid == ::GetCurrentProcessId();
        }
        bool ExtendedVk(int vk)
        {
            return (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E || vk == 0x6F || vk == VK_RCONTROL || vk == VK_RMENU;
        }
        void SendKey(int vk, bool a_down)
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (ExtendedVk(vk) ? KEYEVENTF_EXTENDEDKEY : 0) | (a_down ? 0 : KEYEVENTF_KEYUP);
            if (!in.ki.wScan) {  // no scan code: send the virtual key
                in.ki.wVk = static_cast<WORD>(vk);
                in.ki.dwFlags &= ~static_cast<DWORD>(KEYEVENTF_SCANCODE);
            }
            ::SendInput(1, &in, sizeof(in));
        }
        void PressKey(State& s, const Slot& a_sl, Clock::time_point a_now)
        {
            const int vk = kKeys[a_sl.key].vk;
            if (!vk) {
                REX::INFO("gestures: key action without a key");
                return;
            }
            if (!GameFocused()) {
                ++s.keyNoFocus;
                REX::WARN("gestures: key {} not sent: the game window does not have the focus (it would go to another program)", kKeys[a_sl.key].name);
                return;
            }
            const int mvk = kModVk[a_sl.mod];
            if (mvk) SendKey(mvk, true);
            SendKey(vk, true);
            s.keys.push_back({ vk, mvk, a_now + std::chrono::milliseconds(static_cast<int>(s.cfg.keyHoldMs)) });
            ++s.keySends;
        }
        void ReleaseKeys(State& s, Clock::time_point a_now, bool a_all)
        {
            for (auto it = s.keys.begin(); it != s.keys.end();) {
                if (a_all || a_now >= it->up) {
                    SendKey(it->vk, false);
                    if (it->modVk) SendKey(it->modVk, false);
                    it = s.keys.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::string SlotText(const Slot& sl)
        {
            if (sl.action != kKey) return kActionNames[sl.action];
            return std::format("key {}{}", sl.mod ? std::string(kModNames[sl.mod]) + "+" : std::string(), kKeys[sl.key].name);
        }
        void Run(State& s, int a_hand, int a_slot, Clock::time_point a_now)
        {
            const Slot& sl = s.cfg.slot[a_hand][a_slot];
            if (sl.action == kNone) {
                ++s.unassigned;
                return;
            }
            ++s.fired[a_hand][a_slot];
            s.cooldownUntil = a_now + std::chrono::milliseconds(static_cast<int>(s.cfg.cooldownS * 1000.0f));
            if (s.cfg.haptics) haptics::Request(a_hand, s.cfg.hapticAmp, s.cfg.hapticS);
            REX::INFO("gestures: {} {} -> {}", kHandKey[a_hand], kSlotNames[a_slot], SlotText(sl));
            switch (sl.action) {
            case kKey: PressKey(s, sl, a_now); break;
            case kRecentre: s.recentreReq = true; break;
            case kPov:
            case kCycle: {
                // 20 Nov: gestures run everywhere; only a camera change waits while a load / the main menu is up (no player
                // camera to switch there)
                if (auto* ui = RE::UI::GetSingleton(); ui && (ui->GetMenuOpen("LoadingMenu") || ui->GetMenuOpen("MainMenu"))) {
                    REX::INFO("gestures: camera action skipped (loading / main menu)");
                    break;
                }
                const std::string r = camdir::Command(sl.action == kPov ? "pov" : "cycle");
                REX::INFO("gestures: camdir {} -> {}", sl.action == kPov ? "pov" : "cycle", r);
                if (camdir::CallerShouldShow(r)) game::HudMessage("VR: " + r);  // else camdir::PollNotify announces it
                break;
            }
            case kConfig: configui::RequestToggle("gesture"); break;
            default: break;
            }
        }

        // VRIK GestureMonitor::update classification on release. -1 = no gesture (diagonal / ambiguous).
        int Classify(const State& s, const Press& p, std::string& a_why)
        {
            const float m = s.cfg.moveCm / 100.0f;
            const float d[3]{ p.last[0] - p.start[0], p.last[1] - p.start[1], p.last[2] - p.start[2] };
            const float net = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (net >= m) {
                // single direction: dominant axis of the net move. Axis 0 right (+4 Right / -3 Left), 1 forward (+6 / -5 Back),
                // 2 up (+1 Up / -2 Down)
                const float a[3]{ std::abs(d[0]), std::abs(d[1]), std::abs(d[2]) };
                const int   i = a[0] >= a[1] && a[0] >= a[2] ? 0 : (a[1] >= a[2] ? 1 : 2);
                const float second = std::max(a[(i + 1) % 3], a[(i + 2) % 3]);
                a_why = std::format("net {:.1f}cm d=({:.1f},{:.1f},{:.1f})", net * 100, d[0] * 100, d[1] * 100, d[2] * 100);
                if (a[i] < s.cfg.dominance * second) return -1;
                if (i == 0) return d[0] > 0 ? 4 : 3;
                if (i == 1) return d[1] > 0 ? 6 : 5;
                return d[2] > 0 ? 1 : 2;
            }
            if (p.maxDist > m) {
                // out and back: largest excursion from the end point toward an axis' min or max
                const float e[6]{ p.last[2] - p.mn[2], p.mx[2] - p.last[2], p.last[0] - p.mn[0], p.mx[0] - p.last[0], p.last[1] - p.mn[1],
                    p.mx[1] - p.last[1] };
                // e: 0 Down+Up, 1 Up+Down, 2 Left+Right, 3 Right+Left, 4 Back+Fwd, 5 Fwd+Back
                constexpr int kSlot[6] = { 8, 7, 9, 10, 11, 12 };
                int           best = 0;
                for (int i = 1; i < 6; ++i)
                    if (e[i] > e[best]) best = i;
                float second = 0.0f;
                for (int i = 0; i < 6; ++i)
                    if (i != best) second = std::max(second, e[i]);
                a_why = std::format("out-and-back max {:.1f}cm net {:.1f}cm", e[best] * 100, net * 100);
                if (e[best] < s.cfg.dominance * second) return -1;
                return kSlot[best];
            }
            a_why = std::format("tap net {:.1f}cm max {:.1f}cm", net * 100, p.maxDist * 100);
            return 0;
        }

        // ---- per-hand stick click ---------------------------------------------------------------------------------------
        void HandUpdate(State& s, int h, const Frame& f, bool a_click, bool a_grip, bool a_otherClick, float a_sx, float a_sy, bool a_poseOk,
            const xr::Pose& a_hand, Clock::time_point a_now)
        {
            Press&     p = s.press[h];
            const bool on = s.cfg.enabled && !s.runtimeOff && s.cfg.hand[h];
            const int  offHand = f.leftHanded ? 1 : 0;  // the hand whose click the game uses (sprint)
            // 20 Nov (user: "Gestures shouldn't be locked out in a menu or in any scenario"): bAllViews = every context and
            // camera view, menus included; else first-person gameplay only (as before)
            const bool allowed = f.gameplay || s.cfg.allViews;
            const bool delayHand = h == offHand || f.primaryClickToGame;  // this hand's click is also the game's button
            if (!on) {
                p = Press{};
                return;
            }
            if (a_click && !p.down) {  // press
                p = Press{};
                p.down = true;
                p.t0 = a_now;
                p.latch = true;
                p.ignored = !allowed || !a_poseOk || !f.head || a_now < s.cooldownUntil;
                if (!p.ignored) {
                    YawFrame(*f.head, p.fwd, p.right);
                    Local(p, a_hand, *f.head, p.start);
                    for (int i = 0; i < 3; ++i) p.mn[i] = p.mx[i] = p.last[i] = p.start[i];
                }
                return;
            }
            if (p.down && a_click) {  // held
                if (p.ignored || p.aborted || p.passed) return;
                std::string why;
                if (!allowed) why = s.cfg.allViews ? "menu / loading" : "left gameplay";
                else if (a_grip) why = "grip";
                else if (a_otherClick) why = "other stick click";
                else if (!a_poseOk || !f.head) why = "pose lost";
                else if (std::chrono::duration<float>(a_now - p.t0).count() > s.cfg.maxHoldS) why = "held too long";
                if (!why.empty()) {
                    p.aborted = true;
                    ++s.aborts;
                    s.last = std::format("{} aborted ({})", kHandKey[h], why);
                    return;
                }
                Local(p, a_hand, *f.head, p.last);
                for (int i = 0; i < 3; ++i) {
                    p.mn[i] = std::min(p.mn[i], p.last[i]);
                    p.mx[i] = std::max(p.mx[i], p.last[i]);
                    p.maxDist = std::max(p.maxDist, std::abs(p.last[i] - p.start[i]));
                }
                // VRIK button delay: the off hand's click (sprint) goes to the game once held still long enough
                if (delayHand && std::chrono::duration<float>(a_now - p.t0).count() >= s.cfg.buttonDelayS && p.maxDist <= 0.07f) {
                    p.passed = true;
                    p.latch = false;  // the press is the game's now (sprint): the stick moves again
                    ++s.delayPasses;
                    if (s.cfg.haptics) haptics::Request(h, s.cfg.hapticAmp * 0.6f, 0.03f);
                }
                return;
            }
            if (p.down && !a_click) {  // release
                p.down = false;
                if (p.ignored || p.aborted || p.passed) return;
                std::string why;
                const int   slot = Classify(s, p, why);
                if (slot < 0) {
                    ++s.rejected;
                    s.last = std::format("{} rejected ({})", kHandKey[h], why);
                    REX::INFO("gestures: {} no gesture: {}", kHandKey[h], why);
                    return;
                }
                s.last = std::format("{} {} ({}) -> {}", kHandKey[h], kSlotNames[slot], why, SlotText(s.cfg.slot[h][slot]));
                Run(s, h, slot, a_now);
                return;
            }
            // released: the stick stays held at zero until it is back in the dead zone
            if (p.latch && std::sqrt(a_sx * a_sx + a_sy * a_sy) < 0.35f) p.latch = false;
        }

        // ---- selfie (both hands up) -------------------------------------------------------------------------------------
        void SetSelfie(bool a_on, const char* a_why)
        {
            std::scoped_lock l(g_selfieLock);
            if (g_selfie.active == a_on) return;
            g_selfie.active = a_on;
            g_selfie.follow = false;
            g_selfie.yawRad = 0.0f;
            ++g_selfie.seq;
            REX::INFO("gestures: selfie {} ({})", a_on ? "on" : "off", a_why);
            if (a_on) {
                facediag::AutoDump("selfie on");  // 19 Nov black-face report: face textures / shader state in the headset session
            }
        }
        bool SteerAxis(const xr::Pose& a_grip, float o[3])
        {
            const float x[3]{ 1.0f, 0.0f, 0.0f };  // grip +X: normal to the palm; horizontal when the arm points up
            float       v[3];
            Rot(a_grip.orientation, x, v);
            return Horizontal(v, o, 0.3f);
        }

        void SelfieUpdate(State& s, const Frame& f, Clock::time_point a_now)
        {
            const auto& c = *f.c;
            const Cfg&  cfg = s.cfg;
            bool        active;
            {
                std::scoped_lock l(g_selfieLock);
                active = g_selfie.active;
                g_selfie.distanceM = cfg.distanceM;
            }
            const bool frik = frikbridge::BodyActive();
            // automatic exit: a camera other than first person, a load (LoadingMenu / MainMenu), FRIK gone in gameplay, or selfie
            // switched off. Pause, Pip-Boy and other menus keep it (19 Nov, user: "the selfie view disappears when I pause or
            // load a menu, so it's hard to capture"); FRIK keeps placing the body while paused ([FRIK] bPausedFrame).
            // [Selfie] bKeepInMenus=0: any menu ends it, as before.
            bool loading = false;
            if (active) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    loading = ui->GetMenuOpen("LoadingMenu") || ui->GetMenuOpen("MainMenu");
                }
            }
            const bool menuExit = !f.gameplay && !cfg.selfieKeepInMenus;
            if (active && (loading || menuExit || f.camState != 0 || (!frik && f.gameplay) || !cfg.selfie || s.runtimeOff)) {
                SetSelfie(false, loading ? "load" : menuExit ? "menu" : f.camState != 0 ? "camera change" : !frik ? "FRIK body inactive" : "switched off");
                ++s.selfieAutoOff;
                active = false;
                s.upLatched = false;
            }
            if (!cfg.selfie || s.runtimeOff || !c.active || !f.head) {
                s.upFrames = 0;
                return;
            }
            const int   main = f.leftHanded ? 0 : 1, off = 1 - main;
            const bool  valid[2]{ c.leftGripPoseValid, c.rightGripPoseValid };
            const float hy[2]{ c.leftGripPose.position.y, c.rightGripPose.position.y };
            const float head = f.head->position.y, thr = head + cfg.handsUpCm / 100.0f;
            auto        upOf = [&](int h) { return valid[h] && hy[h] > thr; };
            auto        aboveHmd = [&](int h) { return valid[h] && hy[h] > head; };
            bool        up, above;
            if (cfg.selfieHands == 2) up = upOf(0) && upOf(1), above = aboveHmd(0) && aboveHmd(1);
            else {
                const int h = cfg.selfieHands == 0 ? off : main;
                up = upOf(h), above = aboveHmd(h);
            }
            const int steerHand = cfg.selfieHands == 0 ? off : main;  // VRIK: off hand for 0, main hand for 1 and 2
            const xr::Pose& steer = steerHand == 0 ? c.leftGripPose : c.rightGripPose;

            if (!s.upLatched) {
                // 20 Nov: with bAllViews also in menus / Pip-Boy / dialogue (no lockout); still needs FRIK's body and the
                // first-person camera (the selfie IS FRIK's mirror body in first person, it ends in any other camera)
                const bool ctxOk = cfg.allViews || (f.gameplay && a_now - s.lastNotGameplay > std::chrono::seconds(2));
                const bool gate = ctxOk && f.camState == 0 && frik && a_now - s.lastRecentre > std::chrono::seconds(2) && a_now >= s.cooldownUntil;
                if (up && gate) {
                    if (s.upFrames++ == 0) s.upSince = a_now;
                    if (std::chrono::duration<float>(a_now - s.upSince).count() >= cfg.holdS && s.upFrames > 3) {
                        s.upLatched = true;
                        s.upFrames = 0;
                        SetSelfie(!active, "hands up");
                        active = !active;
                        active ? ++s.selfieOn : ++s.selfieOff;
                        s.refValid = active && SteerAxis(steer, s.ref);
                        if (cfg.haptics) {
                            for (int h = 0; h < 2; ++h)
                                if (cfg.selfieHands == 2 || h == steerHand) haptics::Request(h, cfg.hapticAmp, active ? 0.08f : 0.04f);
                        }
                        s.last = std::format("selfie {}", active ? "on" : "off");
                    }
                } else {
                    s.upFrames = 0;
                    if (up && !frik && !s.noFrikLogged) {
                        s.noFrikLogged = true;
                        REX::INFO("gestures: hands up, but FRIK-flat's body is not active ([FRIK] bBody / FRIK.dll): no selfie");
                    }
                }
            } else if (!above) {
                s.upLatched = false;  // re-arm only below the HMD (VRIK)
            }
            // while the hands stay up after turning it on: place in front of the head and steer by the wrist roll
            const bool follow = active && s.upLatched && above;
            float      yaw = 0.0f;
            bool       yawNew = false;
            if (follow) {
                float v[3];
                if (SteerAxis(steer, v)) {
                    if (!s.refValid) s.refValid = true, s.ref[0] = v[0], s.ref[1] = v[1], s.ref[2] = v[2];
                    // signed angle ref -> v about +Y (counter-clockwise seen from above = positive)
                    const float cr = s.ref[2] * v[0] - s.ref[0] * v[2];
                    const float dt = s.ref[0] * v[0] + s.ref[2] * v[2];
                    float       a = std::atan2(cr, dt);
                    const float dz = cfg.rollDeadzoneDeg * 0.0174533f;
                    a = std::abs(a) <= dz ? 0.0f : a - std::copysign(dz, a);
                    yaw = cfg.rollGain * a;
                    yawNew = true;
                }
            }
            std::scoped_lock l(g_selfieLock);
            g_selfie.follow = follow;
            if (yawNew) g_selfie.yawRad = yaw;
        }
    }

    void Update(const Frame& a_f)
    {
        State&     s = g_s;
        const auto now = Clock::now();
        Poll(s, now);
        ReleaseKeys(s, now, false);
        if (!a_f.gameplay) s.lastNotGameplay = now;
        s.viewGameplay = a_f.gameplay, s.viewAny = a_f.anyView, s.viewCam = a_f.camState;
        if (!a_f.c) return;
        const auto& c = *a_f.c;
        const bool  ok = c.active;
        HandUpdate(s, 0, a_f, ok && c.leftStickClick, ok && c.leftGrip > 0.6f, ok && c.rightStickClick, c.leftStickX, c.leftStickY,
            ok && c.leftGripPoseValid, c.leftGripPose, now);
        HandUpdate(s, 1, a_f, ok && c.rightStickClick, ok && c.rightGrip > 0.6f, ok && c.leftStickClick, c.rightStickX, c.rightStickY,
            ok && c.rightGripPoseValid, c.rightGripPose, now);
        SelfieUpdate(s, a_f, now);
    }

    void AbortPress(int a_hand, const char* a_why)
    {
        if (a_hand < 0 || a_hand > 1) return;
        State& s = g_s;
        Press& p = s.press[a_hand];
        if (!p.down || p.ignored || p.aborted || p.passed) return;
        p.aborted = true;
        ++s.aborts;
        s.last = std::format("{} aborted ({})", kHandKey[a_hand], a_why ? a_why : "claimed");
    }

    bool StickHeld(int a_hand)
    {
        if (a_hand < 0 || a_hand > 1) return false;
        const auto& p = g_s.press[a_hand];
        return (p.down && !p.passed) || p.latch;
    }

    bool ClickToGame(int a_hand, bool a_raw)
    {
        if (a_hand < 0 || a_hand > 1) return a_raw;
        const auto& s = g_s;
        if (!s.cfg.enabled || s.runtimeOff || !s.cfg.hand[a_hand]) return a_raw;
        const auto& p = s.press[a_hand];
        return a_raw && p.down && (p.passed || p.ignored);
    }

    bool TakeRecentre() { return std::exchange(g_s.recentreReq, false); }

    void NoteRecentre() { g_s.lastRecentre = Clock::now(); }

    Selfie SelfieState()
    {
        std::scoped_lock l(g_selfieLock);
        return g_selfie;
    }

    std::string Command(const std::string& a_arg)
    {
        std::istringstream is(a_arg);
        std::string        v;
        is >> v;
        State& s = g_s;
        if (v == "on" || v == "off") {
            s.runtimeOff = v == "off";
            if (s.runtimeOff) ReleaseKeys(s, Clock::now(), true);
            return std::string("gesture ") + v;
        }
        if (v == "reload") {
            Load(s);
            return "gesture reloaded";
        }
        if (v == "test") {  // gesture test R 3: run that slot's action as if performed
            std::string hand;
            int         slot = -1;
            is >> hand >> slot;
            const int h = hand == "L" || hand == "l" ? 0 : 1;
            if (slot < 0 || slot >= kSlots) return "error: gesture test <R|L> <slot 0-12>";
            Run(s, h, slot, Clock::now());
            return std::format("gesture test {} {} -> {}", kHandKey[h], kSlotNames[slot], SlotText(s.cfg.slot[h][slot]));
        }
        if (v == "selfie") {  // gesture selfie on|off (manual, for tests)
            std::string w;
            is >> w;
            SetSelfie(w != "off", "pipe");
            return std::format("gesture selfie {}", SelfieState().active ? "on" : "off");
        }
        std::string assigned;
        for (int h = 0; h < 2; ++h)
            for (int i = 0; i < kSlots; ++i)
                if (s.cfg.slot[h][i].action != kNone) assigned += std::format(" {}{}={}", kHandKey[h], kSlotNames[i], SlotText(s.cfg.slot[h][i]));
        unsigned fired = 0;
        for (const auto& hs : s.fired)
            for (unsigned n : hs) fired += n;
        const auto sf = SelfieState();
        return std::format("gesture enabled={} runtimeOff={} allViews={} (now: gameplay={} anyView={} cam={}) hands L={} R={} move={:.1f}cm dom={:.2f} maxHold={:.2f}s cooldown={:.2f}s "
                           "reloads={} mcmIni={} ({}) | fired={} unassigned={} rejected={} aborts={} keySends={} keyNoFocus={} delayPasses={} "
                           "| last: {} | assigned:{} | selfie cfg={} hands={} active={} follow={} yaw={:.1f}deg dist={:.2f}m on={} off={} autoOff={} "
                           "frikBody={} | {}",
            s.cfg.enabled ? 1 : 0, s.runtimeOff ? 1 : 0, s.cfg.allViews ? 1 : 0, s.viewGameplay ? 1 : 0, s.viewAny ? 1 : 0, s.viewCam,
            s.cfg.hand[0] ? 1 : 0, s.cfg.hand[1] ? 1 : 0, s.cfg.moveCm, s.cfg.dominance, s.cfg.maxHoldS,
            s.cfg.cooldownS, s.reloads, s.mcmExists ? "present" : "absent", McmPath(), fired, s.unassigned, s.rejected, s.aborts, s.keySends,
            s.keyNoFocus, s.delayPasses, s.last, assigned.empty() ? " none" : assigned, s.cfg.selfie ? 1 : 0, s.cfg.selfieHands, sf.active ? 1 : 0,
            sf.follow ? 1 : 0, sf.yawRad * 57.2958f, sf.distanceM, s.selfieOn, s.selfieOff, s.selfieAutoOff, frikbridge::BodyActive() ? 1 : 0,
            haptics::Status());
    }
}
