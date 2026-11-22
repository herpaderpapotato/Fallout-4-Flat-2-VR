// UI sound diagnostic + crosshair focus tick filter. See UiSoundDiag.h.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/UiSoundDiag.h"
#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"
#include "plugin/ThreadQueues.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace fo4flatvr::uisound
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr std::uintptr_t kPlayMenuSound = 0x1086570;  // UIUtils::PlayMenuSound(const char*) (descriptor by editor id)
        // mov [rsp+8],rbx | push rdi | sub rsp,50 | mov rbx,rcx
        constexpr std::uint8_t kPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xD9 };
        // 21 Nov: a steady slow tick (1 / s) never reached 12 plays in 3 s: 5 plays within 10 s now count as repeating
        constexpr double       kWindowS = 10.0;
        constexpr unsigned     kRepeatPlays = 5;
        constexpr double       kRepeatLogEveryS = 30.0;  // REPEATING line: at most once per 30 s per sound (no total cap)
        constexpr std::uintptr_t kMenuStackLock = 0x3DBD368;  // BSReadWriteLock of UI::menuStack (GameMenuPointer.cpp)

        // ---- crosshair focus tick: PlayerCharacter::UpdatePickRef (D70BE0) ------------------------------------------------
        // The engine plays "UIItemFocus" whenever this frame's ViewCaster pick (handle +0, +8, +0xC) differs from the last
        // frame's and is not empty (D70E95..D70EE0). Designed for a mouse / stick crosshair; our hand ray (GameBridge A1) and
        // the head-tracked camera ray move every frame, so the pick flickers between neighbouring refs (user log 21 Nov
        // 01:42: Couch / Scribe Haylen / Chair / none every 15-80 ms) and every flicker ticks. FO4VR's exe has no code
        // reference to "UIItemFocus".
        constexpr std::uintptr_t kPickDataSite = 0xD70D19;      // call ViewCasterBase::GetViewCasterData: the pick after the update
        constexpr std::uintptr_t kGetViewCasterData = 0x994460;
        constexpr std::uintptr_t kFocusLeaSite = 0xD70ED9;      // lea rcx,"UIItemFocus"
        constexpr std::uintptr_t kFocusSoundSite = 0xD70EE0;    // call UIUtils::PlayMenuSound
        constexpr std::uintptr_t kPickRefFn = 0xD70BE0;         // PlayerCharacter::UpdatePickRef
        constexpr std::uintptr_t kInvalidHandle = 0x30E5280;    // the handle manager's empty handle value (id 4795988)
        constexpr std::uint8_t   kFocusLea[] = { 0x48, 0x8D, 0x0D, 0x98, 0xC9, 0x7F, 0x01 };

        using Play_t = void (*)(const char*);
        using GetData_t = void* (*)(void*, void*);
        Play_t    g_orig = nullptr;           // PlayMenuSound past our entry jmp
        Play_t    g_origFocusPlay = nullptr;  // the focus call's original target (PlayMenuSound's entry)
        GetData_t g_origGetData = nullptr;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        struct Count
        {
            std::uint64_t     total{ 0 };
            unsigned          window{ 0 };
            Clock::time_point windowStart{};
            bool              repeatLogged{ false };
            Clock::time_point lastRepeatLog{};
            std::uint64_t     repeatLogs{ 0 };
            std::map<std::uintptr_t, unsigned> callers;  // this window's callers (return address) -> plays
        };
        std::mutex                   g_lock;
        std::map<std::string, Count> g_counts;  // under g_lock
        std::atomic<const char*>     g_ctx{ "-" };
        std::atomic<std::uint64_t>   g_calls{ 0 };
        std::string                  g_install{ "not installed" };
        std::string                  g_focusInstall{ "not installed" };
        thread_local std::uintptr_t  t_callerOverride = 0;  // set by our own call sites: the engine call they stand in for
        thread_local const char*     t_menuSoundName = nullptr;  // PlayMenuSound's editor id while it runs (names its descriptor)

        // ---- caller resolution -----------------------------------------------------------------------------------------
        // Start of the function containing a_addr (x64 unwind table, chained entries followed), 0 if none.
        std::uintptr_t FunctionStart(std::uintptr_t a_addr, std::uintptr_t& a_imageBase)
        {
            DWORD64 img = 0;
            auto*   rf = ::RtlLookupFunctionEntry(a_addr, &img, nullptr);
            for (int i = 0; rf && i < 8; ++i) {
                const auto* ui = reinterpret_cast<const std::uint8_t*>(img + rf->UnwindData);
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                const unsigned codes = ui[2];
                rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<std::uint8_t*>(ui) + 4 + ((codes + 1) & ~1u) * 2);
            }
            a_imageBase = static_cast<std::uintptr_t>(img);
            return rf ? static_cast<std::uintptr_t>(img + rf->BeginAddress) : 0;
        }

        const char* KnownGameFunction(std::uintptr_t a_fnRva)
        {
            switch (a_fnRva) {
            case kPickRefFn:
                return "PlayerCharacter::UpdatePickRef (crosshair / hand-ray pick changed)";
            default:
                return nullptr;
            }
        }

        // "Fallout4.exe+D70EE5 in fn D70BE0 PlayerCharacter::UpdatePickRef (...)"
        std::string Where(std::uintptr_t a_ret)
        {
            if (!a_ret) return "?";
            HMODULE mod = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_ret), &mod) || !mod) {
                return std::format("{:X} (no module: JIT / trampoline)", a_ret);
            }
            char path[MAX_PATH]{};
            ::GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            name = name ? name + 1 : path;
            const auto     mb = reinterpret_cast<std::uintptr_t>(mod);
            std::uintptr_t img = 0;
            const auto     fn = FunctionStart(a_ret, img);
            std::string    s = std::format("{}+{:X}", name, a_ret - mb);
            if (fn && img == mb) {
                s += std::format(" in fn {:X}", fn - mb);
                if (mod == ::GetModuleHandleW(nullptr)) {
                    if (const char* k = KnownGameFunction(fn - mb)) s += std::format(" {}", k);
                }
            }
            return s;
        }

        // the open menus, bottom -> top (main-idle queue: no menu message processing in flight)
        void LogMenuStackLater(std::string a_sound, const char* a_what = "ui sound: '{}' REPEATING")
        {
            tq::RunOnMain([sound = std::move(a_sound), a_what]() {
                auto* ui = RE::UI::GetSingleton();
                if (!ui) return;
                std::string s;
                auto&       lock = *reinterpret_cast<RE::BSReadWriteLock*>(Base() + kMenuStackLock);
                lock.lock_read();
                for (const auto& m : ui->menuStack) {
                    if (!m) continue;
                    const char* n = m->menuName.c_str();
                    s += std::format("{}{}", s.empty() ? "" : ", ", n && *n ? n : "?");
                }
                lock.unlock_read();
                REX::INFO("{}: open menus (bottom -> top): {}", std::vformat(a_what, std::make_format_args(sound)), s.empty() ? "none" : s);
            });
        }

        void HkPlayMenuSound(const char* a_name)
        {
            // reached by a jmp from PlayMenuSound's entry: the return address is the caller's
            const std::uintptr_t caller = t_callerOverride ? t_callerOverride : reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            ++g_calls;
            if (a_name) {
                const char* key = *a_name ? a_name : "(empty name)";  // 21 Nov: "" resolved to UITerminalCharScrollLP (a loop)
                const auto  now = Clock::now();
                std::string log;
                bool        repeat = false;
                std::vector<std::pair<unsigned, std::uintptr_t>> callers;
                std::uint64_t total = 0;
                unsigned      window = 0;
                {
                    std::scoped_lock l(g_lock);
                    auto& c = g_counts[key];
                    total = ++c.total;
                    if (std::chrono::duration<double>(now - c.windowStart).count() > kWindowS) {
                        c.windowStart = now;
                        c.window = 0;
                        c.callers.clear();
                    }
                    window = ++c.window;
                    if (c.callers.size() < 16 || c.callers.count(caller)) ++c.callers[caller];
                    if (c.total <= 2) {
                        callers.emplace_back(1, caller);
                    } else if (c.window >= kRepeatPlays &&
                               (!c.repeatLogged || std::chrono::duration<double>(now - c.lastRepeatLog).count() >= kRepeatLogEveryS)) {
                        c.repeatLogged = true;
                        c.lastRepeatLog = now;
                        ++c.repeatLogs;
                        repeat = true;
                        for (const auto& [a, n] : c.callers) callers.emplace_back(n, a);
                    }
                }
                if (total <= 2) {
                    log = std::format("ui sound: '{}' played (#{}; bindings context {}; caller {})", key, total, g_ctx.load(),
                        Where(caller));
                } else if (repeat) {
                    std::sort(callers.rbegin(), callers.rend());
                    std::string who;
                    for (std::size_t i = 0; i < callers.size() && i < 4; ++i)
                        who += std::format("{}{} x{}", i ? " | " : "", Where(callers[i].second), callers[i].first);
                    log = std::format("ui sound: '{}' REPEATING: {} plays within {:.0f} s ({} in total; bindings context {}) callers: {}", key,
                        window, kWindowS, total, g_ctx.load(), who);
                }
                if (!log.empty()) REX::INFO("{}", log);
                if (repeat) LogMenuStackLater(key);
            }
            t_menuSoundName = a_name;
            g_orig(a_name);
            t_menuSoundName = nullptr;
        }

        // ---- focus tick filter -----------------------------------------------------------------------------------------
        struct PickKey
        {
            std::uint32_t h0{}, h8{}, hC{};
            bool operator==(const PickKey&) const = default;
        };
        struct Seen
        {
            PickKey           k;
            Clock::time_point t;
        };
        std::mutex                  g_pickLock;
        std::vector<Seen>           g_seen;  // under g_pickLock: refs under the pick recently, last frame each was there
        thread_local bool           t_pickValid = false;   // set by HkPickData, consumed by HkFocusSound (same call, same thread)
        thread_local bool           t_pickRecent = false;  // this frame's pick was already under the pick within the window
        std::atomic<int>            g_focusMode{ INT_MIN };
        std::atomic<float>          g_focusRepeatS{ -1.0f };
        std::atomic<std::uint64_t>  g_focusPlayed{ 0 }, g_focusSuppressed{ 0 }, g_focusFrames{ 0 };
        std::atomic<std::uint64_t>  g_focusSuppressLogs{ 0 };
        std::atomic<long long>      g_focusLastSummaryMs{ 0 };

        // [Controllers] iPickFocusSound: 0 = no focus tick (FO4VR), 1 = once per newly focused ref (default), 2 = the
        // engine's (every pick change)
        int FocusMode()
        {
            int m = g_focusMode.load();
            if (m == INT_MIN) {
                m = static_cast<int>(::GetPrivateProfileIntA("Controllers", "iPickFocusSound", 0, Settings::Path().c_str()));
                m = std::clamp(m, 0, 2);
                g_focusMode = m;
                REX::INFO("ui sound: [Controllers] iPickFocusSound={} ({})", m,
                    m == 0 ? "no crosshair focus tick" : m == 1 ? "focus tick once per newly focused ref" : "engine focus tick");
            }
            return m;
        }
        float FocusRepeatS()
        {
            float s = g_focusRepeatS.load();
            if (s < 0.0f) {
                char buf[32]{};
                ::GetPrivateProfileStringA("Controllers", "fPickFocusSoundRepeatS", "1.5", buf, sizeof(buf), Settings::Path().c_str());
                s = std::clamp(std::strtof(buf, nullptr), 0.0f, 30.0f);
                g_focusRepeatS = s;
                REX::INFO("ui sound: [Controllers] fPickFocusSoundRepeatS={:.2f}", s);
            }
            return s;
        }

        void* HkPickData(void* a_vc, void* a_out)
        {
            void* r = g_origGetData(a_vc, a_out);
            t_pickValid = false;
            t_pickRecent = false;
            if (!a_out) return r;
            ++g_focusFrames;
            const auto*         d = static_cast<const std::uint32_t*>(a_out);
            const PickKey       k{ d[0], d[2], d[3] };
            const std::uint32_t none = *reinterpret_cast<const std::uint32_t*>(Base() + kInvalidHandle);
            t_pickValid = true;
            if (k.h0 == none && k.h8 == none && k.hC == none) return r;  // empty pick: the engine plays nothing
            const auto  now = Clock::now();
            const float window = FocusRepeatS();
            std::scoped_lock l(g_pickLock);
            for (auto& s : g_seen) {
                if (s.k == k) {
                    t_pickRecent = window > 0.0f && std::chrono::duration<float>(now - s.t).count() < window;
                    s.t = now;
                    return r;
                }
            }
            if (g_seen.size() >= 64) {
                g_seen.erase(std::min_element(g_seen.begin(), g_seen.end(), [](const Seen& a, const Seen& b) { return a.t < b.t; }));
            }
            g_seen.push_back({ k, now });
            return r;
        }

        void HkFocusSound(const char* a_name)
        {
            const int  mode = FocusMode();
            const bool play = mode >= 2 || !t_pickValid || (mode == 1 && !t_pickRecent);
            t_pickValid = false;
            if (play) {
                ++g_focusPlayed;
                t_callerOverride = Base() + kFocusSoundSite + 5;  // the counter detour reports the engine site, not ours
                g_origFocusPlay(a_name);
                t_callerOverride = 0;
                return;
            }
            const auto n = ++g_focusSuppressed;
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            long long       last = g_focusLastSummaryMs.load();
            if (g_focusSuppressLogs < 2) {
                ++g_focusSuppressLogs;
                REX::INFO("ui sound: crosshair focus tick suppressed (#{}: {})", n, mode == 0 ? "iPickFocusSound=0" : "ref focused again within the window");
            } else if (ms - last >= 30000 && g_focusLastSummaryMs.compare_exchange_strong(last, ms)) {
                REX::INFO("ui sound: crosshair focus ticks played {} suppressed {} (iPickFocusSound={}, window {:.2f} s)", g_focusPlayed.load(), n,
                    mode, FocusRepeatS());
            }
        }

        bool CallTo(std::uintptr_t a_site, std::uintptr_t a_target)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_site);
            if (p[0] != 0xE8) return false;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            return Base() + a_site + 5 + rel == Base() + a_target;
        }

        void InstallFocusFilter(REL::Trampoline& a_tramp)
        {
            const auto b = Base();
            if (!CallTo(kPickDataSite, kGetViewCasterData) || !CallTo(kFocusSoundSite, kPlayMenuSound) ||
                std::memcmp(reinterpret_cast<const void*>(b + kFocusLeaSite), kFocusLea, sizeof(kFocusLea)) != 0) {
                g_focusInstall = "UpdatePickRef bytes differ: focus tick filter not installed (engine ticks on every pick change)";
                REX::WARN("ui sound: {}", g_focusInstall);
                return;
            }
            g_origGetData = reinterpret_cast<GetData_t>(a_tramp.write_call<5>(b + kPickDataSite, &HkPickData));
            g_origFocusPlay = reinterpret_cast<Play_t>(a_tramp.write_call<5>(b + kFocusSoundSite, &HkFocusSound));
            g_focusInstall = "focus tick filter on UpdatePickRef D70EE0";
            REX::INFO("ui sound: {} (iPickFocusSound={}, fPickFocusSoundRepeatS={:.2f})", g_focusInstall, FocusMode(), FocusRepeatS());
        }

        // ---- every engine sound start (21 Nov; user: "Ticking again after vats", "its like a keypress") -----------------
        // The 02:33 session's tick did not pass UIUtils::PlayMenuSound. All game sounds funnel through BSAudioManager
        // (flat 1.11.240, labels from the address library): GetSoundHandle creates a handle from a descriptor (SNDR forms,
        // sound markers, actor / weapon / UI sounds; GetSoundHandleByName = PlayMenuSound's path calls it), GetSoundHandleByFile
        // from a file (dialogue / raw wav), and the handle starts through Play / PlayAfter / FadeInPlay (BSSoundHandle's
        // wrappers call these; inlined copies too). Per sound: starts, who created and who started it (first two frames
        // outside the audio code), handles still playing (polled with BSSoundHandle::IsPlaying every 0.5 s).
        // Log lines: first two starts per sound; REPEATING (>= 5 starts within 10 s, once per 30 s per sound); STACKING
        // (more handles of one sound playing at once than ever before, from 2 up); LONG (one handle playing > 10 s:
        // looping, never stopped; two per sound). Pass-through only. [Controllers] bSoundDiag=0 = not installed.
        constexpr std::uintptr_t kGetHandle = 0x1630310;        // BSAudioManager::GetSoundHandle(mgr, handle*, BSISoundDescriptor*, float, flags, extra)
        constexpr std::uintptr_t kGetHandleByFile = 0x1630160;  // BSAudioManager::GetSoundHandleByFile(mgr, handle*, const BSResource::ID&, flags, prio)
        constexpr std::uintptr_t kMgrPlay = 0x1630DB0;          // BSAudioManager::Play(mgr, id)            <- BSSoundHandle::Play 162DD50
        constexpr std::uintptr_t kMgrPlayAfter = 0x1630E70;     // BSAudioManager::PlayAfter(mgr, id, delay) <- BSSoundHandle::PlayAfter 162DD90
        constexpr std::uintptr_t kMgrFadeInPlay = 0x1631270;    // BSAudioManager::FadeInPlay(mgr, id, ms)  <- BSSoundHandle::FadeInPlay 162E710
        constexpr std::uintptr_t kMgrStop = 0x1631060;          // BSAudioManager::Stop(mgr, id)            <- BSSoundHandle::Stop 162DE20
        constexpr std::uintptr_t kHandleIsPlaying = 0x162DE60;  // BSSoundHandle::IsPlaying(handle*)
        constexpr std::uintptr_t kSndrSubVtbl = 0x24AE780;      // BGSSoundDescriptorForm's BSISoundDescriptor vtable (form + 0x20)
        constexpr std::uintptr_t kAudioLo = 0x162D000, kAudioHi = 0x1646000;  // BSSoundHandle / BSAudioManager code (skipped as callers)
        constexpr double         kSndWindowS = 10.0, kSndRepeatLogS = 30.0, kSndLongS = 10.0;
        constexpr unsigned       kSndRepeat = 5;

        struct EntryHook
        {
            const char*         name;
            std::uintptr_t      rva;
            std::uint8_t        bytes[10];
            std::size_t         copy;  // leading bytes that are whole position-independent instructions (relocated)
        };
        // mov [rsp+8|10h],rbx first in each: 5 bytes relocated; the rest only checked
        constexpr EntryHook kSndHooks[] = {
            { "GetSoundHandle", kGetHandle, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54 }, 5 },
            { "GetSoundHandleByFile", kGetHandleByFile, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 }, 5 },
            { "Play", kMgrPlay, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 }, 5 },
            { "PlayAfter", kMgrPlayAfter, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 }, 5 },
            { "FadeInPlay", kMgrFadeInPlay, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10 }, 5 },
            { "Stop", kMgrStop, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 }, 5 },
        };

        struct Handle
        {
            std::uint32_t id;
            std::uint8_t  assumeSuccess, state;
            std::uint16_t pad;
        };
        using GetHandle_t = std::uint64_t (*)(void*, Handle*, const void*, float, std::uint64_t, void*);
        using ByFile_t = std::uint64_t (*)(void*, Handle*, const std::uint32_t*, std::uint64_t, std::uint64_t);
        using Mgr2_t = std::uint64_t (*)(void*, std::uint64_t);
        using Mgr3_t = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t);
        using IsPlaying_t = bool (*)(const Handle*);
        GetHandle_t g_origGetHandle = nullptr;
        ByFile_t    g_origByFile = nullptr;
        Mgr2_t      g_origMgrPlay = nullptr, g_origMgrStop = nullptr;
        Mgr3_t      g_origMgrPlayAfter = nullptr, g_origMgrFadeInPlay = nullptr;
        std::string g_sndInstall{ "not installed" };

        struct Callers
        {
            std::uintptr_t a{ 0 }, b{ 0 };  // first two frames outside the audio code / PlayMenuSound / this plugin
            auto operator<=>(const Callers&) const = default;
        };
        struct SoundId
        {
            std::string       name;
            Callers           created, played;
            Clock::time_point start{};
            bool              playing{ false }, longLogged{ false };
        };
        struct SoundStat
        {
            std::uint64_t     starts{ 0 }, handles{ 0 }, stops{ 0 }, ended{ 0 };
            unsigned          window{ 0 };
            Clock::time_point windowStart{}, lastRepeatLog{};
            bool              repeatLogged{ false };
            int               live{ 0 }, liveLogged{ 1 }, maxLive{ 0 };
            unsigned          longLogs{ 0 };
            std::map<Callers, unsigned> played;   // this window's starters
            std::map<Callers, unsigned> created;  // creators (first 16)
        };
        std::mutex                                       g_sndLock;
        std::unordered_map<std::uint32_t, SoundId>       g_ids;        // under g_sndLock
        std::map<std::string, SoundStat>                 g_snd;        // under g_sndLock
        std::unordered_map<const void*, std::string>     g_descNames;  // under g_sndLock: descriptor -> name
        std::unordered_map<const void*, std::string>     g_edid;       // under g_sndLock: SNDR form -> editor id
        std::atomic<bool>                                g_wantEdid{ false };
        std::atomic<int>                                 g_edidTries{ 0 };
        std::atomic<int>                                 g_camState{ -1 };
        std::atomic<std::uint64_t>                       g_sndStarts{ 0 }, g_sndUnknown{ 0 };
        std::uintptr_t                                   g_selfLo = 0, g_selfHi = 0, g_gameHi = 0;

        void ModuleRange(std::uintptr_t a_base, std::uintptr_t& a_hi)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
            a_hi = a_base + nt->OptionalHeader.SizeOfImage;
        }

        Callers WalkCallers()
        {
            void*          fr[24];
            const USHORT   n = ::RtlCaptureStackBackTrace(1, 24, fr, nullptr);
            const auto     b = Base();
            Callers        c;
            int            got = 0;
            for (USHORT i = 0; i < n && got < 2; ++i) {
                const auto a = reinterpret_cast<std::uintptr_t>(fr[i]);
                if (a >= g_selfLo && a < g_selfHi) continue;
                if (a >= b && a < g_gameHi) {
                    const auto rva = a - b;
                    if (rva >= kAudioLo && rva < kAudioHi) continue;
                    std::uintptr_t img = 0;
                    if (FunctionStart(a, img) == b + kPlayMenuSound) continue;
                }
                (got == 0 ? c.a : c.b) = a;
                ++got;
            }
            return c;
        }

        std::string Who(const Callers& a_c)
        {
            if (!a_c.a) return "?";
            return a_c.b ? std::format("{} <- {}", Where(a_c.a), Where(a_c.b)) : Where(a_c.a);
        }

        std::string TopCallers(const std::map<Callers, unsigned>& a_m)
        {
            std::vector<std::pair<unsigned, Callers>> v;
            for (const auto& [k, n] : a_m) v.emplace_back(n, k);
            std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            std::string s;
            for (std::size_t i = 0; i < v.size() && i < 3; ++i) s += std::format("{}{} x{}", i ? " | " : "", Who(v[i].second), v[i].first);
            return s.empty() ? "?" : s;
        }

        // under g_sndLock
        std::string DescName(const void* a_desc)
        {
            if (!a_desc) return "(no descriptor)";
            if (auto it = g_descNames.find(a_desc); it != g_descNames.end()) return it->second;
            const auto  vt = *reinterpret_cast<const std::uintptr_t*>(a_desc);
            std::string n;
            bool        cache = true;
            if (vt == Base() + kSndrSubVtbl) {
                const auto* form = reinterpret_cast<const RE::TESForm*>(static_cast<const std::uint8_t*>(a_desc) - 0x20);
                const auto  fid = form->GetFormID();
                if (t_menuSoundName && *t_menuSoundName) {
                    n = std::format("{} [{:08X}]", t_menuSoundName, fid);
                } else if (auto e = g_edid.find(form); e != g_edid.end()) {
                    n = std::format("{} [{:08X}]", e->second, fid);
                } else {
                    n = std::format("SNDR {:08X}", fid);
                    cache = g_edidTries.load() >= 3;  // the editor id map may not be read yet
                    g_wantEdid = true;
                }
            } else {
                n = std::format("descriptor vtbl {:X}", vt - Base());
            }
            if (cache) g_descNames[a_desc] = n;
            return n;
        }

        // SNDR editor ids from the game's editor-id map (main thread; PlayMenuSound looks sounds up by these names)
        void BuildEdidMap()
        {
            std::unordered_map<const void*, std::string> m;
            const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
            if (!map) return;
            {
                RE::BSAutoReadLock l{ lock };
                for (const auto& [k, f] : *map) {
                    if (f && f->GetFormType() == RE::ENUM_FORM_ID::kSNDR && k.c_str()) m.emplace(f, k.c_str());
                }
            }
            const auto count = m.size();
            {
                std::scoped_lock l(g_sndLock);
                g_edid = std::move(m);
            }
            REX::INFO("engine sound: {} sound descriptor editor ids read (try {})", count, g_edidTries.load());
        }

        void Created(std::uint32_t a_id, std::string a_name, const Callers& a_c)
        {
            std::scoped_lock l(g_sndLock);
            if (g_ids.size() >= 4096) {
                for (auto it = g_ids.begin(); it != g_ids.end();) it = it->second.playing ? std::next(it) : g_ids.erase(it);
            }
            auto& st = g_snd[a_name];
            ++st.handles;
            if (st.created.size() < 16 || st.created.count(a_c)) ++st.created[a_c];
            auto& s = g_ids[a_id];
            s = SoundId{};
            s.name = std::move(a_name);
            s.created = a_c;
        }

        void Started(std::uint64_t a_id, const char* a_how)
        {
            const auto id = static_cast<std::uint32_t>(a_id);
            if (id == 0xFFFFFFFF) return;
            ++g_sndStarts;
            const Callers c = WalkCallers();
            const auto    now = Clock::now();
            std::string   log, stackFor;
            {
                std::scoped_lock l(g_sndLock);
                auto it = g_ids.find(id);
                if (it == g_ids.end()) {
                    ++g_sndUnknown;
                    it = g_ids.emplace(id, SoundId{ "handle made before the hook / unhooked path", {}, {}, {}, false, false }).first;
                }
                auto& s = it->second;
                auto& st = g_snd[s.name];
                if (!s.playing) {
                    s.playing = true;
                    ++st.live;
                    st.maxLive = std::max(st.maxLive, st.live);
                }
                s.start = now;
                s.longLogged = false;
                s.played = c;
                ++st.starts;
                if (std::chrono::duration<double>(now - st.windowStart).count() > kSndWindowS) {
                    st.windowStart = now;
                    st.window = 0;
                    st.played.clear();
                }
                ++st.window;
                if (st.played.size() < 16 || st.played.count(c)) ++st.played[c];
                if (st.starts <= 2) {
                    log = std::format("engine sound: '{}' {} (#{}; id {}; {} playing; created by {}; started by {}; bindings context {}; camera state {})",
                        s.name, a_how, st.starts, id, st.live, Who(s.created), Who(c), g_ctx.load(), g_camState.load());
                } else if (st.window >= kSndRepeat &&
                           (!st.repeatLogged || std::chrono::duration<double>(now - st.lastRepeatLog).count() >= kSndRepeatLogS)) {
                    st.repeatLogged = true;
                    st.lastRepeatLog = now;
                    log = std::format("engine sound: '{}' REPEATING: {} starts within {:.0f} s ({} in total, {} handles made, {} playing now, max {}; "
                                      "bindings context {}; camera state {}) started by: {} ; created by: {}",
                        s.name, st.window, kSndWindowS, st.starts, st.handles, st.live, st.maxLive, g_ctx.load(), g_camState.load(),
                        TopCallers(st.played), TopCallers(st.created));
                    stackFor = s.name;
                }
            }
            if (!log.empty()) REX::INFO("{}", log);
            if (!stackFor.empty()) LogMenuStackLater(stackFor, "engine sound: '{}' REPEATING");
        }

        void Stopped(std::uint64_t a_id)
        {
            const auto id = static_cast<std::uint32_t>(a_id);
            std::scoped_lock l(g_sndLock);
            auto it = g_ids.find(id);
            if (it == g_ids.end() || !it->second.playing) return;
            it->second.playing = false;
            auto& st = g_snd[it->second.name];
            --st.live;
            ++st.stops;
        }

        std::uint64_t HkGetHandle(void* a_mgr, Handle* a_h, const void* a_desc, float a_dist, std::uint64_t a_flags, void* a_extra)
        {
            const auto r = g_origGetHandle(a_mgr, a_h, a_desc, a_dist, a_flags, a_extra);
            if ((r & 0xFF) && a_h && a_h->id != 0xFFFFFFFF) {
                std::string n;
                {
                    std::scoped_lock l(g_sndLock);
                    n = DescName(a_desc);
                }
                Created(a_h->id, std::move(n), WalkCallers());
            }
            return r;
        }

        std::uint64_t HkByFile(void* a_mgr, Handle* a_h, const std::uint32_t* a_file, std::uint64_t a_flags, std::uint64_t a_prio)
        {
            const auto r = g_origByFile(a_mgr, a_h, a_file, a_flags, a_prio);
            if (a_h && a_h->id != 0xFFFFFFFF) {
                // BSResource::ID: file hash, extension, directory hash
                std::string n = "file ?";
                if (a_file) {
                    const char* ext = reinterpret_cast<const char*>(a_file + 1);
                    n = std::format("file {:08X}/{:08X}.{}", a_file[2], a_file[0], std::string_view(ext, ::strnlen(ext, 4)));
                }
                Created(a_h->id, std::move(n), WalkCallers());
            }
            return r;
        }

        std::uint64_t HkMgrPlay(void* a_mgr, std::uint64_t a_id)
        {
            Started(a_id, "Play");
            return g_origMgrPlay(a_mgr, a_id);
        }
        std::uint64_t HkMgrPlayAfter(void* a_mgr, std::uint64_t a_id, std::uint64_t a_delay)
        {
            Started(a_id, "PlayAfter");
            return g_origMgrPlayAfter(a_mgr, a_id, a_delay);
        }
        std::uint64_t HkMgrFadeInPlay(void* a_mgr, std::uint64_t a_id, std::uint64_t a_ms)
        {
            Started(a_id, "FadeInPlay");
            return g_origMgrFadeInPlay(a_mgr, a_id, a_ms);
        }
        std::uint64_t HkMgrStop(void* a_mgr, std::uint64_t a_id)
        {
            Stopped(a_id);
            return g_origMgrStop(a_mgr, a_id);
        }

        // main tick: camera state, editor ids, which handles still play (every 0.5 s)
        void SoundTick()
        {
            if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                g_camState = pc->currentState ? static_cast<int>(pc->currentState->id.get()) : -1;
            }
            static Clock::time_point lastPoll{}, lastEdid{};
            const auto               now = Clock::now();
            if (g_wantEdid.load() && g_edidTries.load() < 3 && now - lastEdid > std::chrono::seconds(20)) {
                lastEdid = now;
                g_wantEdid = false;
                ++g_edidTries;
                BuildEdidMap();
                std::scoped_lock l(g_sndLock);
                g_descNames.clear();  // re-name with the editor ids
            }
            if (now - lastPoll < std::chrono::milliseconds(500)) return;
            lastPoll = now;
            std::vector<std::uint32_t> ids;
            {
                std::scoped_lock l(g_sndLock);
                for (const auto& [id, s] : g_ids)
                    if (s.playing) ids.push_back(id);
            }
            const auto isPlaying = reinterpret_cast<IsPlaying_t>(Base() + kHandleIsPlaying);
            std::vector<std::uint32_t> ended;
            for (const auto id : ids) {
                const Handle h{ id, 0, 1, 0 };
                if (!isPlaying(&h)) ended.push_back(id);
            }
            std::vector<std::string> logs, stacks;
            {
                std::scoped_lock l(g_sndLock);
                for (const auto id : ended) {
                    auto it = g_ids.find(id);
                    if (it == g_ids.end() || !it->second.playing) continue;
                    it->second.playing = false;
                    auto& st = g_snd[it->second.name];
                    --st.live;
                    ++st.ended;
                }
                for (auto& [id, s] : g_ids) {
                    if (!s.playing || s.longLogged || std::chrono::duration<double>(now - s.start).count() < kSndLongS) continue;
                    s.longLogged = true;
                    auto& st = g_snd[s.name];
                    if (st.longLogs >= 2) continue;
                    ++st.longLogs;
                    logs.push_back(std::format("engine sound: '{}' LONG: handle {} playing {:.0f} s since its start (looping / never stopped; {} of it "
                                               "playing; created by {}; started by {}; bindings context {}; camera state {})",
                        s.name, id, std::chrono::duration<double>(now - s.start).count(), st.live, Who(s.created), Who(s.played), g_ctx.load(),
                        g_camState.load()));
                    stacks.push_back(s.name);
                }
                for (auto& [n, st] : g_snd) {
                    if (st.live <= 0) {
                        st.live = 0;
                        st.liveLogged = 1;
                        continue;
                    }
                    if (st.live > st.liveLogged) {
                        st.liveLogged = st.live;
                        logs.push_back(std::format("engine sound: '{}' STACKING: {} handles playing at once ({} starts, {} handles made, {} stopped, "
                                                   "{} ended; bindings context {}; camera state {}) started by: {} ; created by: {}",
                            n, st.live, st.starts, st.handles, st.stops, st.ended, g_ctx.load(), g_camState.load(), TopCallers(st.played),
                            TopCallers(st.created)));
                        stacks.push_back(n);
                    }
                }
            }
            for (const auto& s : logs) REX::INFO("{}", s);
            for (auto& n : stacks) LogMenuStackLater(std::move(n), "engine sound: '{}'");
        }

        void InstallEngineSounds(REL::Trampoline& a_tramp)
        {
            if (::GetPrivateProfileIntA("Controllers", "bSoundDiag", 1, Settings::Path().c_str()) == 0) {
                g_sndInstall = "off ([Controllers] bSoundDiag=0)";
                REX::INFO("engine sound: {}", g_sndInstall);
                return;
            }
            const auto b = Base();
            ModuleRange(b, g_gameHi);
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&WalkCallers), &self);
            if (self) {
                g_selfLo = reinterpret_cast<std::uintptr_t>(self);
                ModuleRange(g_selfLo, g_selfHi);
            }
            for (const auto& h : kSndHooks) {
                if (std::memcmp(reinterpret_cast<const void*>(b + h.rva), h.bytes, sizeof(h.bytes)) != 0) {
                    g_sndInstall = std::format("BSAudioManager::{} ({:X}) bytes differ: not installed", h.name, h.rva);
                    REX::WARN("engine sound: {}", g_sndInstall);
                    return;
                }
            }
            if (*reinterpret_cast<const std::uint8_t*>(b + kHandleIsPlaying) != 0x48 && *reinterpret_cast<const std::uint8_t*>(b + kHandleIsPlaying) != 0x40) {
                g_sndInstall = "BSSoundHandle::IsPlaying prologue unexpected: not installed";
                REX::WARN("engine sound: {}", g_sndInstall);
                return;
            }
            void* const hooks[] = { reinterpret_cast<void*>(&HkGetHandle), reinterpret_cast<void*>(&HkByFile), reinterpret_cast<void*>(&HkMgrPlay),
                reinterpret_cast<void*>(&HkMgrPlayAfter), reinterpret_cast<void*>(&HkMgrFadeInPlay), reinterpret_cast<void*>(&HkMgrStop) };
            void* origs[std::size(kSndHooks)]{};
            for (std::size_t i = 0; i < std::size(kSndHooks); ++i) {
                const auto& h = kSndHooks[i];
                auto*       stub = static_cast<std::uint8_t*>(a_tramp.allocate(h.copy + 14));
                std::memcpy(stub, reinterpret_cast<const void*>(b + h.rva), h.copy);
                stub[h.copy] = 0xFF;
                stub[h.copy + 1] = 0x25;
                std::memset(stub + h.copy + 2, 0, 4);
                const std::uint64_t back = b + h.rva + h.copy;
                std::memcpy(stub + h.copy + 6, &back, 8);
                origs[i] = stub;
            }
            g_origGetHandle = reinterpret_cast<GetHandle_t>(origs[0]);
            g_origByFile = reinterpret_cast<ByFile_t>(origs[1]);
            g_origMgrPlay = reinterpret_cast<Mgr2_t>(origs[2]);
            g_origMgrPlayAfter = reinterpret_cast<Mgr3_t>(origs[3]);
            g_origMgrFadeInPlay = reinterpret_cast<Mgr3_t>(origs[4]);
            g_origMgrStop = reinterpret_cast<Mgr2_t>(origs[5]);
            for (std::size_t i = 0; i < std::size(kSndHooks); ++i) a_tramp.write_jmp<5>(b + kSndHooks[i].rva, hooks[i]);
            tq::AddMainTick(&SoundTick, "uisound::SoundTick");
            g_sndInstall = "BSAudioManager GetSoundHandle / ByFile / Play / PlayAfter / FadeInPlay / Stop detoured (pass-through counter)";
            REX::INFO("engine sound: {} ([Controllers] bSoundDiag=1)", g_sndInstall);
        }
    }

    void SetContextName(const char* a_ctx) { g_ctx = a_ctx ? a_ctx : "-"; }

    std::string Install()
    {
        static bool done = false;
        if (done) return g_install;
        done = true;
        const auto  base = REL::Offset(0).address();
        static REL::Trampoline tramp{ "fo4flatvr ui sound" };  // own trampoline (the shared one is ~80 % used at load)
        tramp.create(512);
        cfgstore::Subscribe("uisound", "Controllers/iPickFocusSound,Controllers/fPickFocusSoundRepeatS", [](const cfgstore::Changes&) {
            g_focusMode = INT_MIN;
            g_focusRepeatS = -1.0f;
            REX::INFO("ui sound: focus tick settings re-read (live): iPickFocusSound={} fPickFocusSoundRepeatS={:.2f}", FocusMode(),
                FocusRepeatS());
        });
        InstallFocusFilter(tramp);
        InstallEngineSounds(tramp);  // 21 Nov: every engine sound start (not only PlayMenuSound)
        const auto* p = reinterpret_cast<const std::uint8_t*>(base + kPlayMenuSound);
        if (std::memcmp(p, kPrologue, sizeof(kPrologue)) != 0) {
            g_install = "PlayMenuSound bytes differ: not installed";
            REX::WARN("ui sound: {}", g_install);
            return g_install;
        }
        // the first 5 bytes are one position-independent instruction (mov [rsp+8],rbx): relocated into a stub
        auto* stub = static_cast<std::uint8_t*>(tramp.allocate(5 + 14));
        std::memcpy(stub, p, 5);
        stub[5] = 0xFF;
        stub[6] = 0x25;
        std::memset(stub + 7, 0, 4);
        const std::uint64_t back = base + kPlayMenuSound + 5;
        std::memcpy(stub + 11, &back, 8);
        g_orig = reinterpret_cast<Play_t>(stub);
        tramp.write_jmp<5>(base + kPlayMenuSound, reinterpret_cast<void*>(&HkPlayMenuSound));
        g_install = "PlayMenuSound detoured (pass-through counter)";
        REX::INFO("ui sound: {}", g_install);
        return g_install;
    }

    std::string Report()
    {
        std::vector<std::pair<std::uint64_t, std::string>> top;
        {
            std::scoped_lock l(g_lock);
            for (const auto& [n, c] : g_counts) top.emplace_back(c.total, n);
        }
        std::sort(top.rbegin(), top.rend());
        std::string s = std::format("ui sounds: {} | calls={} distinct={} | most played:", g_install, g_calls.load(), top.size());
        for (std::size_t i = 0; i < top.size() && i < 12; ++i) s += std::format(" {}={}", top[i].second, top[i].first);
        s += std::format(" | {}: iPickFocusSound={} window {:.2f} s, pick frames {} ticks played {} suppressed {}", g_focusInstall,
            FocusMode(), FocusRepeatS(), g_focusFrames.load(), g_focusPlayed.load(), g_focusSuppressed.load());
        // engine sounds: most started, and every sound with handles playing now
        std::vector<std::tuple<std::uint64_t, std::string, int, int>> eng;
        std::size_t ids = 0;
        {
            std::scoped_lock l(g_sndLock);
            ids = g_ids.size();
            for (const auto& [n, st] : g_snd) eng.emplace_back(st.starts, n, st.live, st.maxLive);
        }
        std::sort(eng.rbegin(), eng.rend());
        s += std::format(" | engine sounds: {} | starts={} (unknown handle {}) distinct={} ids={} | most started (playing now / max):", g_sndInstall,
            g_sndStarts.load(), g_sndUnknown.load(), eng.size(), ids);
        for (std::size_t i = 0; i < eng.size() && i < 15; ++i)
            s += std::format(" '{}'={} ({}/{})", std::get<1>(eng[i]), std::get<0>(eng[i]), std::get<2>(eng[i]), std::get<3>(eng[i]));
        s += " | playing now:";
        for (const auto& e : eng)
            if (std::get<2>(e) > 0) s += std::format(" '{}' x{}", std::get<1>(e), std::get<2>(e));
        return s;
    }
}
