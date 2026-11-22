// fo4flatvr engine cull (EngineCull.h): bit 0 at once, the engine's SetAppCulled on the main thread.
#include "plugin/EngineCull.h"
#include "plugin/ConfigStore.h"
#include "plugin/ThreadQueues.h"

#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace fo4flatvr::enginecull
{
    namespace
    {
        struct Pending
        {
            RE::NiPointer<RE::NiAVObject> node;
            bool                          culled;
        };
        std::mutex                 g_lock;
        std::vector<Pending>       g_pending;
        bool                       g_drainQueued = false;
        std::atomic<std::uint64_t> g_sets{ 0 }, g_drains{ 0 }, g_applied{ 0 }, g_skipped{ 0 }, g_direct{ 0 };
        std::atomic<std::uint64_t> g_rev{ ~0ull };
        std::atomic<bool>          g_on{ true };

        void DirectBit(RE::NiAVObject* a_node, bool a_culled)
        {
            if (a_culled) {
                a_node->flags.flags |= 1;
            } else {
                a_node->flags.flags &= ~static_cast<std::uint64_t>(1);
            }
        }

        void Drain()
        {
            std::vector<Pending> work;
            {
                std::scoped_lock l(g_lock);
                work.swap(g_pending);
                g_drainQueued = false;
            }
            ++g_drains;
            for (auto& p : work) {
                auto* n = p.node.get();
                if (!n) continue;
                if (((n->flags.flags & 1) != 0) != p.culled) {
                    ++g_skipped;
                    continue;
                }
                n->SetAppCulled(!p.culled);  // the engine call no-ops on an unchanged state: step through the other one
                n->SetAppCulled(p.culled);
                ++g_applied;
            }
        }
    }

    bool On()
    {
        const auto rev = cfgstore::Revision();
        if (g_rev.load() != rev) {
            std::string v;
            g_on = !(cfgstore::EffectiveValue("FRIK", "bEngineCullFlag", v) && std::atoi(v.c_str()) == 0);
            g_rev = rev;
        }
        return g_on.load();
    }

    void Set(RE::NiAVObject* a_node, bool a_culled)
    {
        if (!a_node) return;
        ++g_sets;
        const bool changed = ((a_node->flags.flags & 1) != 0) != a_culled;
        DirectBit(a_node, a_culled);
        if (!changed || !On()) {
            return;
        }
        bool queue = false;
        {
            std::scoped_lock l(g_lock);
            g_pending.push_back({ RE::NiPointer<RE::NiAVObject>(a_node), a_culled });
            if (!g_drainQueued) g_drainQueued = queue = true;
        }
        if (queue && !tq::RunOnMain([] { Drain(); })) {
            std::scoped_lock l(g_lock);
            g_direct += g_pending.size();
            g_pending.clear();
            g_drainQueued = false;
        }
    }

    std::string Status()
    {
        return std::format("enginecull on={} sets={} drains={} applied={} skipped(changed again)={} direct(no queue)={}", On() ? 1 : 0,
            g_sets.load(), g_drains.load(), g_applied.load(), g_skipped.load(), g_direct.load());
    }
}
