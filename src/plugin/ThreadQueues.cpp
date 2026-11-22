// ThreadQueues.cpp - see ThreadQueues.h (20 Nov, thread-safety audit; analysis/threading.md)
#include "plugin/ThreadQueues.h"
#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"

#include <REX/LOG.h>
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <REL/Relocation.h>

#include "plugin/CallChain.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace fo4flatvr::tq
{
    namespace
    {
        // OnIdle C339D0 +0xDC4: E8 E7 20 40 00 = call Console::ExecuteQueuedCommand (1036880, void(void)); the only code
        // caller (Ghidra 1.11.240). Reached every OnIdle, paused or not, after WaitForThreads / ProcessMessages / AdvanceMenus.
        constexpr std::uintptr_t kIdleCallSite = 0xC34794;
        constexpr std::uintptr_t kExecuteQueuedCommand = 0x1036880;

        using Void_t = void (*)();
        Void_t      g_origExec = nullptr;
        bool        g_hooked = false;  // written at load only
        // [General] bMainThreadQueue, live since 20 Nov: the C34794 hook is installed whatever it says at load; off = tasks go
        // to F4SE AddTask and the ticks run at Present (the old behaviour), the hook only drains what was queued before
        std::atomic<bool> g_enabled{ true };
        std::string g_why = "not installed";

        struct Tick
        {
            void (*fn)();
            const char* name;
        };
        std::vector<Tick> g_ticks;  // registered at load (single-threaded), read-only afterwards

        std::mutex                         g_mainLock, g_renderLock;
        std::vector<std::function<void()>> g_main, g_render;
        std::atomic<std::uint32_t>         g_mainTid{ 0 };
        std::atomic<std::uint64_t>         g_mainQueued{ 0 }, g_mainRun{ 0 }, g_renderQueued{ 0 }, g_renderRun{ 0 }, g_idles{ 0 },
            g_fallbackTasks{ 0 }, g_waitTimeouts{ 0 }, g_inline{ 0 }, g_exceptions{ 0 };
        std::atomic<std::uint64_t>         g_maxDrainUs{ 0 };

        double QpcUsPerTick()
        {
            static const double f = [] { LARGE_INTEGER q; ::QueryPerformanceFrequency(&q); return 1.0e6 / static_cast<double>(q.QuadPart); }();
            return f;
        }

        void RunGuarded(const std::function<void()>& a_fn, const char* a_where)
        {
            try {
                a_fn();
            } catch (const std::exception& e) {
                if (g_exceptions.fetch_add(1) < 10) REX::ERROR("tq: {} task threw: {}", a_where, e.what());
            } catch (...) {
                if (g_exceptions.fetch_add(1) < 10) REX::ERROR("tq: {} task threw (unknown exception)", a_where);
            }
        }

        void DrainMain()
        {
            const auto tid = ::GetCurrentThreadId();
            if (const auto prev = g_mainTid.exchange(tid); prev != tid) {
                REX::INFO("tq: main-idle queue drained on thread {} (was {}; OnIdle C34794 = main thread, after WaitForThreads / UI::AdvanceMenus)",
                    tid, prev);
            }
            ++g_idles;
            LARGE_INTEGER t0;
            ::QueryPerformanceCounter(&t0);
            std::vector<std::function<void()>> q;
            {
                std::scoped_lock l(g_mainLock);
                q.swap(g_main);
            }
            for (auto& fn : q) {
                RunGuarded(fn, "main");
                ++g_mainRun;
            }
            if (g_enabled.load(std::memory_order_relaxed)) {
                for (const auto& t : g_ticks) {
                    t.fn();
                }
            }
            LARGE_INTEGER t1;
            ::QueryPerformanceCounter(&t1);
            const auto us = static_cast<std::uint64_t>((t1.QuadPart - t0.QuadPart) * QpcUsPerTick());
            for (auto m = g_maxDrainUs.load(); us > m && !g_maxDrainUs.compare_exchange_weak(m, us);) {}
        }

        void HkExecuteQueuedCommand()
        {
            DrainMain();
            g_origExec();  // the console's queued commands, as before (after ours: a QueueConsoleCommand task runs it directly)
        }
    }

    void Install()
    {
        g_enabled = ::GetPrivateProfileIntA("General", "bMainThreadQueue", 1, Settings::Path().c_str()) != 0;
        cfgstore::Subscribe("tq", "General/bMainThreadQueue", [](const cfgstore::Changes&) {
            const bool on = ::GetPrivateProfileIntA("General", "bMainThreadQueue", 1, Settings::Path().c_str()) != 0;
            if (g_enabled.exchange(on) != on) {
                REX::INFO("tq: main-thread queue {} (live [General] bMainThreadQueue{})", on ? "on" : "off: F4SE AddTask / AddUITask, polls at Present",
                    g_hooked ? "" : "; the C34794 hook is not installed, so the fallback stays");
            }
        });
        {
            const std::uintptr_t base = REL::Offset(0).address();
            if (callchain::Hookable(base + kIdleCallSite, base + kExecuteQueuedCommand, g_why)) {
                auto& tramp = REL::GetTrampoline();
                if (tramp.empty()) tramp.create(64);
                g_origExec = reinterpret_cast<Void_t>(tramp.write_call<5>(base + kIdleCallSite, &HkExecuteQueuedCommand));
                g_hooked = true;
                g_why += std::format("; trampoline {}B used of {}B", tramp.allocated_size(), tramp.capacity());
            } else {
                g_why = "C34794 not hookable (" + g_why + "): F4SE AddTask fallback, polls at Present";
            }
        }
        REX::INFO("tq: main-thread queue {} ({}{}); main ticks: {}", g_hooked && g_enabled ? "on" : "off", g_why,
            g_enabled ? "" : "; [General] bMainThreadQueue=0: hook passes through, live switch", [] {
            std::string s;
            for (const auto& t : g_ticks) s += std::format("{}{}", s.empty() ? "" : ", ", t.name);
            return s.empty() ? std::string("none yet") : s;
        }());
    }

    bool MainHooked() { return g_hooked; }

    bool RunOnMain(std::function<void()> a_fn)
    {
        if (!a_fn) return false;
        if (g_hooked && g_enabled.load(std::memory_order_relaxed)) {
            std::scoped_lock l(g_mainLock);
            g_main.push_back(std::move(a_fn));
            ++g_mainQueued;
            return true;
        }
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks) return false;
        ++g_fallbackTasks;
        tasks->AddTask(std::move(a_fn));
        return true;
    }

    std::string RunOnMainWait(std::function<std::string()> a_fn, std::chrono::milliseconds a_timeout, const char* a_timeoutText)
    {
        if (!a_fn) return a_timeoutText;
        if (g_hooked && g_enabled.load(std::memory_order_relaxed) && IsMainThread()) {
            ++g_inline;
            return a_fn();
        }
        auto done = std::make_shared<std::promise<std::string>>();
        auto fut = done->get_future();
        const bool queued = RunOnMain([done, fn = std::move(a_fn)]() {
            std::string r;
            try {
                r = fn();
            } catch (const std::exception& e) {
                r = std::string("error: task threw: ") + e.what();
            } catch (...) {
                r = "error: task threw";
            }
            done->set_value(std::move(r));
        });
        if (!queued) return "error: no main-thread queue";
        if (fut.wait_for(a_timeout) != std::future_status::ready) {
            ++g_waitTimeouts;
            return a_timeoutText;
        }
        return fut.get();
    }

    bool RunOnRender(std::function<void()> a_fn)
    {
        if (!a_fn) return false;
        std::scoped_lock l(g_renderLock);
        g_render.push_back(std::move(a_fn));
        ++g_renderQueued;
        return true;
    }

    void AddMainTick(void (*a_fn)(), const char* a_name)
    {
        if (a_fn) g_ticks.push_back({ a_fn, a_name });
    }

    void OnPresent()
    {
        std::vector<std::function<void()>> q;
        {
            std::scoped_lock l(g_renderLock);
            if (g_render.empty()) return;
            q.swap(g_render);
        }
        for (auto& fn : q) {
            RunGuarded(fn, "render");
            ++g_renderRun;
        }
    }

    void PresentFallbackTicks()
    {
        if (g_hooked && g_enabled.load(std::memory_order_relaxed)) return;
        for (const auto& t : g_ticks) {
            t.fn();
        }
    }

    bool IsMainThread()
    {
        const auto t = g_mainTid.load(std::memory_order_relaxed);
        return t != 0 && t == ::GetCurrentThreadId();
    }

    std::string Status()
    {
        std::size_t pendMain = 0, pendRender = 0;
        {
            std::scoped_lock l(g_mainLock);
            pendMain = g_main.size();
        }
        {
            std::scoped_lock l(g_renderLock);
            pendRender = g_render.size();
        }
        std::string ticks;
        for (const auto& t : g_ticks) ticks += std::format("{}{}", ticks.empty() ? "" : ",", t.name);
        return std::format("main queue {} ({}) thread={} idles={} queued={} run={} pending={} maxDrainUs={} inlineWaits={} waitTimeouts={} "
                           "f4seFallbackTasks={} exceptions={} | render queue queued={} run={} pending={} | ticks [{}] {}",
            g_hooked && g_enabled.load() ? "on" : "off", g_why, g_mainTid.load(), g_idles.load(), g_mainQueued.load(), g_mainRun.load(), pendMain, g_maxDrainUs.load(),
            g_inline.load(), g_waitTimeouts.load(), g_fallbackTasks.load(), g_exceptions.load(), g_renderQueued.load(), g_renderRun.load(), pendRender,
            ticks, g_hooked && g_enabled.load() ? "at OnIdle C34794" : "at Present (fallback)");
    }
}
