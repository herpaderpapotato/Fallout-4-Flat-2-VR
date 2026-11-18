// Controller haptics queue. See Haptics.h.
#include "plugin/Haptics.h"

#include "xr/XrRuntime.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

namespace fo4flatvr::haptics
{
    namespace
    {
        constexpr float kFrikPulseS = 0.025f;   // one FRIK micro pulse is held this long (> one frame at 45-90 Hz)
        constexpr float kOpenVrMaxMicros = 4000.0f;

        struct Pending
        {
            bool  on{ false };
            float amp{ 0.0f };
            float seconds{ 0.0f };
        };
        std::mutex            g_lock;
        Pending               g_pending[2];
        std::atomic<unsigned> g_requests{ 0 }, g_frikRequests{ 0 }, g_played{ 0 };

        void Queue(int a_hand, float a_amp, float a_seconds)
        {
            if (a_hand < 0 || a_hand > 1 || !(a_amp > 0.0f)) return;
            std::scoped_lock l(g_lock);
            auto& p = g_pending[a_hand];
            if (!p.on) {
                p = { true, a_amp, a_seconds };
            } else {
                p.amp = std::max(p.amp, a_amp);
                p.seconds = std::max(p.seconds, a_seconds);
            }
        }
    }

    void Request(int a_hand, float a_amplitude, float a_seconds)
    {
        ++g_requests;
        Queue(a_hand, std::clamp(a_amplitude, 0.0f, 1.0f), a_seconds);
    }

    void RequestFrik(int a_hand, float a_amplitude, float a_seconds)
    {
        ++g_frikRequests;
        float amp = std::clamp(a_amplitude, 0.0f, 1.0f), secs = a_seconds;
        if (secs > 0.0f && secs <= kOpenVrMaxMicros * 1e-6f * 1.01f) {  // an OpenVR-style micro pulse (vrshim)
            amp *= std::clamp(secs / (kOpenVrMaxMicros * 1e-6f), 0.05f, 1.0f);
            secs = kFrikPulseS;
        }
        Queue(a_hand, amp, secs);
    }

    void Drain(xr::Runtime& a_rt)
    {
        Pending p[2];
        {
            std::scoped_lock l(g_lock);
            p[0] = g_pending[0], p[1] = g_pending[1];
            g_pending[0] = {}, g_pending[1] = {};
        }
        for (int h = 0; h < 2; ++h) {
            if (!p[h].on) continue;
            a_rt.Vibrate(h, p[h].amp, p[h].seconds);
            ++g_played;
        }
    }

    std::string Status()
    {
        return std::format("haptics requests={} frikRequests={} played={}", g_requests.load(), g_frikRequests.load(), g_played.load());
    }
}
