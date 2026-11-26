// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/ModsResetGuard.h"

#include "plugin/GameMenuPointer.h"
#include "plugin/Settings.h"
#include "plugin/ThreadQueues.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <REL/Relocation.h>

#include "plugin/CallChain.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace fo4flatvr::modsreset
{
    namespace
    {
        // MessageMenuManager::CreateOKMessage(manager, title, text, callback, priority, flag) (1.11.240 141079460)
        using CreateOK_t = void* (*)(void*, const char*, const char*, void*, std::uint32_t, bool);
        constexpr std::uintptr_t kCreateOK = 0x1079460;
        constexpr std::uintptr_t kSites[2] = { 0xA91B48, 0xA87EE5 };   // CloseModManager, InitialGetBlacklistCallback
        constexpr const char*    kSiteNames[2] = { "ModManager::CloseModManager (the installed / loaded mod set changed)",
               "StartMenuBase::InitialGetBlacklistCallback (a loaded plugin is on Bethesda.net's blacklist: it was unchecked)" };

        CreateOK_t        g_orig[2]{};
        // ModsChanged_ConfirmResetCallback::Run(self, answer) (vtable 2539850 slot 1 = 2539858 -> A98DA0): answer false frees
        // the CharacterCreation data (A7EC70) and sets Main::resetGame / fullReset
        using Run_t = void (*)(void*, bool);
        constexpr std::uintptr_t kRunSlot = 0x2539858;
        constexpr std::uintptr_t kRunFn = 0xA98DA0;
        constexpr std::uintptr_t kFreeCharGen = 0xA7EC70;
        Run_t             g_origRun = nullptr;
        // Papyrus Game.PrecacheCharGenClear (native thunk 1129F50: jmp LooksMenuUtils::ClearCachedCharGenData A85D50): nulls the
        // CharacterCreation pointers LooksMenu::AdvanceMovie (A80A40) reads without a null check (crash A8107C)
        constexpr std::uintptr_t kClearThunk = 0x1129F50;
        constexpr std::uintptr_t kClearFn = 0xA85D50;
        std::atomic<int>  g_clears{ 0 }, g_clearsDeferred{ 0 };
        std::atomic<int>  g_runs{ 0 }, g_runsDeferred{ 0 };
        bool              g_enabled = true;
        std::atomic<int>  g_requests{ 0 }, g_deferred{ 0 }, g_replayed{ 0 };
        std::mutex        g_lock;
        std::string       g_last = "-";

        struct Pending
        {
            int           site;
            void*         mgr;
            const char*   title;
            const char*   text;
            void*         cb;
            std::uint32_t prio;
            bool          flag;
        };

        std::string Where(void* a_addr)
        {
            HMODULE m = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_addr), &m) && m) {
                char name[MAX_PATH]{};
                ::GetModuleFileNameA(m, name, MAX_PATH);
                const char* b = std::strrchr(name, '\\');
                return std::format("{}+{:X}", b ? b + 1 : name, reinterpret_cast<std::uintptr_t>(a_addr) - reinterpret_cast<std::uintptr_t>(m));
            }
            return std::format("{}", a_addr);
        }

        void Replay(Pending p)
        {
            // main thread, once per frame until character creation has closed
            if (game::MenuOpen("LooksMenu")) {
                tq::RunOnMain([p] { Replay(p); });
                return;
            }
            ++g_replayed;
            REX::WARN("modsreset: LooksMenu closed - the held mod-change reset prompt is shown now (from {})", kSiteNames[p.site]);
            g_orig[p.site](p.mgr, p.title, p.text, p.cb, p.prio, p.flag);
        }

        void ResetNow(const char* a_why)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            reinterpret_cast<void (*)()>(base + kFreeCharGen)();   // what the callback does with answer false
            if (auto* m = RE::Main::GetSingleton()) {
                m->resetGame = true;
                m->fullReset = true;
            }
            REX::WARN("modsreset: the held mod-change reset runs now ({})", a_why);
        }

        void ReplayReset()
        {
            if (game::MenuOpen("LooksMenu")) {
                tq::RunOnMain([] { ReplayReset(); });
                return;
            }
            ++g_replayed;
            ResetNow("LooksMenu closed");
        }

        void ClearReplay()
        {
            if (game::MenuOpen("LooksMenu")) {
                tq::RunOnMain([] { ClearReplay(); });
                return;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            REX::WARN("modsreset: LooksMenu closed - the held PrecacheCharGenClear runs now");
            reinterpret_cast<void (*)()>(base + kClearFn)();
        }

        void HkClear()
        {
            ++g_clears;
            const bool looks = game::MenuOpen("LooksMenu");
            REX::WARN("modsreset: Papyrus Game.PrecacheCharGenClear() (LooksMenu {})", looks ? "OPEN" : "closed");
            if (looks && g_enabled) {
                ++g_clearsDeferred;
                REX::WARN("modsreset: PrecacheCharGenClear held back until character creation (LooksMenu) closes: clearing now nulls "
                          "the CharacterCreation data LooksMenu is still using (crash at Fallout4.exe+A8107C)");
                tq::RunOnMain([] { ClearReplay(); });
                return;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            reinterpret_cast<void (*)()>(base + kClearFn)();
        }

        void HkRun(void* a_self, bool a_answer)
        {
            ++g_runs;
            void* frames[12]{};
            const auto n = ::RtlCaptureStackBackTrace(1, 12, frames, nullptr);
            std::string stack;
            for (USHORT i = 0; i < n; ++i) stack += " < " + Where(frames[i]);
            const bool looks = game::MenuOpen("LooksMenu");
            REX::WARN("modsreset: ModsChanged_ConfirmResetCallback answer {} (LooksMenu {}); caller{}", a_answer, looks ? "OPEN" : "closed", stack);
            if (!a_answer && looks && g_enabled) {
                ++g_runsDeferred;
                REX::WARN("modsreset: reset held back until character creation (LooksMenu) closes: running it now frees the "
                          "CharacterCreation data LooksMenu is still using (crash at Fallout4.exe+A8107C)");
                tq::RunOnMain([] { ReplayReset(); });
                return;
            }
            g_origRun(a_self, a_answer);
        }

        template <int I>
        void* Hk(void* a_mgr, const char* a_title, const char* a_text, void* a_cb, std::uint32_t a_prio, bool a_flag)
        {
            ++g_requests;
            void* frames[12]{};
            const auto n = ::RtlCaptureStackBackTrace(1, 12, frames, nullptr);
            std::string stack;
            for (USHORT i = 0; i < n; ++i) stack += " < " + Where(frames[i]);
            const bool looks = game::MenuOpen("LooksMenu");
            {
                std::scoped_lock l(g_lock);
                g_last = std::format("{} ({}){}", kSiteNames[I], a_text ? a_text : "?", looks ? " during character creation" : "");
            }
            REX::WARN("modsreset: the game asks for a reset: {} - text '{}', LooksMenu {}; caller{}", kSiteNames[I], a_text ? a_text : "?",
                looks ? "OPEN" : "closed", stack);
            if (g_enabled && looks) {
                ++g_deferred;
                REX::WARN("modsreset: held back until character creation (LooksMenu) closes: answering it now frees the "
                          "CharacterCreation data LooksMenu is still using (crash at Fallout4.exe+A8107C)");
                const Pending p{ I, a_mgr, a_title, a_text, a_cb, a_prio, a_flag };
                tq::RunOnMain([p] { Replay(p); });
                return nullptr;   // both callers ignore the result
            }
            return g_orig[I](a_mgr, a_title, a_text, a_cb, a_prio, a_flag);
        }
    }

    void Install()
    {
        g_enabled = ::GetPrivateProfileIntA("General", "bModsResetGuard", 1, Settings::Path().c_str()) != 0;
        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        auto&      tramp = REL::GetTrampoline();
        std::string res;
        for (int i = 0; i < 2; ++i) {
            const auto  site = base + kSites[i];
            std::string why;
            if (!callchain::Hookable(site, base + kCreateOK, why)) {
                res += std::format(" | {:X}: {} (not hooked)", kSites[i], why);
                continue;
            }
            g_orig[i] = reinterpret_cast<CreateOK_t>(tramp.write_call<5>(site, i == 0 ? reinterpret_cast<void*>(&Hk<0>) : reinterpret_cast<void*>(&Hk<1>)));
            res += std::format(" | {:X}: {}", kSites[i], why);
        }
        // the Papyrus native thunk of Game.PrecacheCharGenClear: E9 rel32 -> A85D50
        {
            auto*      p = reinterpret_cast<std::uint8_t*>(base + kClearThunk);
            const auto tgt = p[0] == 0xE9 ? reinterpret_cast<std::uintptr_t>(p) + 5 + *reinterpret_cast<std::int32_t*>(p + 1) : 0;
            if (tgt == base + kClearFn) {
                tramp.write_jmp<5>(base + kClearThunk, reinterpret_cast<std::uintptr_t>(&HkClear));
                res += " | PrecacheCharGenClear thunk 1129F50: hooked";
            } else {
                res += std::format(" | PrecacheCharGenClear thunk: unexpected bytes {:02X} (not hooked)", p[0]);
            }
        }
        // the callback's Run slot (covers every place that queues the prompt)
        auto* slot = reinterpret_cast<std::uintptr_t*>(base + kRunSlot);
        if (*slot == base + kRunFn) {
            DWORD old = 0;
            if (::VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
                g_origRun = reinterpret_cast<Run_t>(*slot);
                *slot = reinterpret_cast<std::uintptr_t>(&HkRun);
                ::VirtualProtect(slot, sizeof(*slot), old, &old);
                res += " | Run slot 2539858: hooked";
            } else {
                res += " | Run slot: VirtualProtect failed";
            }
        } else {
            res += std::format(" | Run slot holds {} (not hooked)", Where(reinterpret_cast<void*>(*slot)));
        }
        REX::INFO("modsreset: mod-change reset prompt guard {} ([General] bModsResetGuard){}", g_enabled ? "on" : "OFF (logging only)", res);
    }

    std::string Status()
    {
        std::scoped_lock l(g_lock);
        return std::format("modsreset enabled={} requests={} deferred={} runs={} runsDeferred={} replayed={} chargenClears={} clearsDeferred={} last='{}'",
            g_enabled, g_requests.load(), g_deferred.load(), g_runs.load(), g_runsDeferred.load(), g_replayed.load(), g_clears.load(),
            g_clearsDeferred.load(), g_last);
    }
}
