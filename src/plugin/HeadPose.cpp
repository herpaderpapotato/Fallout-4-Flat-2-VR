#include "plugin/HeadPose.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace fo4flatvr::head
{
    namespace
    {
        struct Q { float x{}, y{}, z{}, w{ 1.0f }; };

        Q Mul(const Q& a, const Q& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }

        Q Conj(const Q& q) { return { -q.x, -q.y, -q.z, q.w }; }

        Q AxisAngle(float ax, float ay, float az, float rad)
        {
            const float s = std::sin(rad * 0.5f);
            return { ax * s, ay * s, az * s, std::cos(rad * 0.5f) };
        }

        Vec3 Rotate(const Q& q, const Vec3& v)
        {
            const Q p = Mul(Mul(q, Q{ v.x, v.y, v.z, 0.0f }), Conj(q));
            return { p.x, p.y, p.z };
        }

        Mat3 ToMat(const Q& q)
        {
            const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            return { { { 1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy) },
                       { 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx) },
                       { 2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy) } } };
        }

        // Yaw-only part of q (rotation about game Z).
        Q YawOf(const Q& q)
        {
            const Vec3  f = Rotate(q, { 0.0f, 1.0f, 0.0f });
            const float yaw = std::atan2(-f.x, f.y);  // + = turned left (about +Z)
            return AxisAngle(0, 0, 1, yaw);
        }

        std::mutex g_lock;
        Q          g_xrQ;          // game basis
        Vec3       g_xrP;          // game basis, metres
        bool       g_xrValid{ false };
        bool       g_haveNeutral{ false };
        Q          g_neutralYaw;
        Vec3       g_neutralP;
        bool       g_override{ false };
        Q          g_overrideQ;
        Vec3       g_overrideP;    // metres
        bool       g_tracking{ true };
        bool       g_posTracking{ true };
        int        g_convention{ 0 };
        float      g_targetFov{ 0.0f };
        std::atomic<unsigned> g_recenterCount{ 0 };  // bumped on every re-anchor (roomscale resets on change)
        unsigned long long g_seq{ 0 };   // PublishXr count (g_lock)

        void RecenterLocked()
        {
            g_neutralYaw = YawOf(g_xrQ);
            g_neutralP = g_xrP;
            g_haveNeutral = true;
            ++g_recenterCount;
        }
    }

    unsigned RecenterCount()
    {
        return g_recenterCount.load();
    }

    void PublishXr(float a_qx, float a_qy, float a_qz, float a_qw, float a_px, float a_py, float a_pz, bool a_valid)
    {
        // OpenXR (X right, Y up, -Z forward) -> game (X right, Y forward, Z up): game = (x, -z, y).
        // A rotation quaternion's vector part maps the same way; w is unchanged.
        std::scoped_lock l(g_lock);
        g_xrQ = { a_qx, -a_qz, a_qy, a_qw };
        g_xrP = { a_px, -a_pz, a_py };
        g_xrValid = a_valid;
        ++g_seq;
        if (a_valid && !g_haveNeutral) {
            RecenterLocked();
        }
    }

    void Recenter()
    {
        std::scoped_lock l(g_lock);
        if (g_xrValid) {
            RecenterLocked();
        }
    }

    void SetOverride(float a_yawDeg, float a_pitchDeg, float a_rollDeg)
    {
        constexpr float d2r = 3.14159265358979f / 180.0f;
        const Q yaw = AxisAngle(0, 0, 1, -a_yawDeg * d2r);   // +Z turns left, so yaw-right is negative
        const Q pitch = AxisAngle(1, 0, 0, a_pitchDeg * d2r); // +X tips forward (Y) toward up (Z)
        const Q roll = AxisAngle(0, 1, 0, a_rollDeg * d2r);   // about forward: + tilts right
        std::scoped_lock l(g_lock);
        g_overrideQ = Mul(Mul(yaw, pitch), roll);
        g_override = true;
    }

    void SetPositionOverride(float a_xCm, float a_yCm, float a_zCm)
    {
        std::scoped_lock l(g_lock);
        g_overrideP = { a_xCm / 100.0f, a_yCm / 100.0f, a_zCm / 100.0f };
        if (!g_override) {
            g_overrideQ = {};
            g_override = true;
        }
    }

    void ClearOverride()
    {
        std::scoped_lock l(g_lock);
        g_override = false;
        g_overrideQ = {};
        g_overrideP = {};
    }

    void SetTracking(bool a_on)
    {
        std::scoped_lock l(g_lock);
        g_tracking = a_on;
    }

    bool Tracking()
    {
        std::scoped_lock l(g_lock);
        return g_tracking;
    }

    bool OverrideActive()
    {
        std::scoped_lock l(g_lock);
        return g_override;
    }

    void SetPositionTracking(bool a_on)
    {
        std::scoped_lock l(g_lock);
        g_posTracking = a_on;
    }

    bool Get(Mat3& a_out)
    {
        unsigned long long seq;
        return Get(a_out, seq);
    }

    bool GetOffset(Vec3& a_out)
    {
        unsigned long long seq;
        return GetOffset(a_out, seq);
    }

    unsigned long long Seq()
    {
        std::scoped_lock l(g_lock);
        return g_seq;
    }

    bool Get(Mat3& a_out, unsigned long long& a_seq)
    {
        Q q;
        {
            std::scoped_lock l(g_lock);
            a_seq = g_seq;
            if (!g_tracking) {
                return false;
            }
            if (g_override) {
                q = g_overrideQ;
            } else if (g_xrValid && g_haveNeutral) {
                q = Mul(Conj(g_neutralYaw), g_xrQ);
            } else {
                return false;
            }
        }
        a_out = ToMat(q);
        return true;
    }

    bool GetOffset(Vec3& a_out, unsigned long long& a_seq)
    {
        std::scoped_lock l(g_lock);
        a_seq = g_seq;
        if (!g_tracking || !g_posTracking) {
            return false;
        }
        if (g_override) {
            a_out = g_overrideP;
            return true;
        }
        if (!g_xrValid || !g_haveNeutral) {
            return false;
        }
        const Vec3 d{ g_xrP.x - g_neutralP.x, g_xrP.y - g_neutralP.y, g_xrP.z - g_neutralP.z };
        a_out = Rotate(Conj(g_neutralYaw), d);
        return true;
    }

    void SetTargetFov(float a_hDeg)
    {
        std::scoped_lock l(g_lock);
        g_targetFov = a_hDeg;
    }

    float TargetFov()
    {
        std::scoped_lock l(g_lock);
        return g_targetFov;
    }

    void SetConvention(int a_mode)
    {
        std::scoped_lock l(g_lock);
        g_convention = a_mode;
    }

    int Convention()
    {
        std::scoped_lock l(g_lock);
        return g_convention;
    }

    // ---- pose sync ----
    namespace
    {
        std::mutex              g_syncLock;
        std::condition_variable g_syncCv;
        std::atomic<bool>       g_syncOn{ true };
        std::atomic<float>      g_syncTimeoutMs{ 50.0f };
        std::atomic<bool>       g_windowOpen{ false };
        std::atomic<DWORD>      g_windowThread{ 0 };
        struct SyncStats
        {
            std::uint64_t calls{ 0 }, waits{ 0 }, timeouts{ 0 };
            double        waitMsSum{ 0.0 }, waitMsMax{ 0.0 };
        };
        SyncStats g_sync;  // g_syncLock
    }

    void SetPoseSync(bool a_on, float a_timeoutMs)
    {
        g_syncTimeoutMs = (std::max)(1.0f, a_timeoutMs);
        g_syncOn = a_on;
        if (!a_on) {
            ClosePublishWindow();
        }
    }

    bool PoseSync()
    {
        return g_syncOn.load();
    }

    void OpenPublishWindow()
    {
        if (!g_syncOn.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock l(g_syncLock);
        g_windowThread = ::GetCurrentThreadId();
        g_windowOpen = true;
    }

    void ClosePublishWindow()
    {
        if (!g_windowOpen.load()) {
            return;
        }
        {
            std::scoped_lock l(g_syncLock);
            g_windowOpen = false;
        }
        g_syncCv.notify_all();
    }

    void WaitForPublish(int a_site)
    {
        if (!g_windowOpen.load(std::memory_order_acquire) || ::GetCurrentThreadId() == g_windowThread.load(std::memory_order_relaxed)) {
            return;
        }
        LARGE_INTEGER t0{}, t1{}, fq{};
        ::QueryPerformanceCounter(&t0);
        bool timedOut = false;
        {
            std::unique_lock l(g_syncLock);
            ++g_sync.calls;
            const auto timeout = std::chrono::microseconds(static_cast<long long>(g_syncTimeoutMs.load() * 1000.0f));
            timedOut = !g_syncCv.wait_for(l, timeout, [] { return !g_windowOpen.load(); });
            ::QueryPerformanceCounter(&t1);
            ::QueryPerformanceFrequency(&fq);
            const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
            ++g_sync.waits;
            g_sync.timeouts += timedOut ? 1 : 0;
            g_sync.waitMsSum += ms;
            g_sync.waitMsMax = (std::max)(g_sync.waitMsMax, ms);
            if (TraceArmed()) {
                TraceRec r{ 'W' };
                r.seq = g_seq;  // not under g_lock: diagnostic only
                r.v[0] = static_cast<float>(ms);
                r.v[1] = timedOut ? 1.0f : 0.0f;
                r.v[2] = static_cast<float>(a_site);
                Trace(r);
            }
        }
    }

    std::string PoseSyncStats(bool a_reset)
    {
        std::scoped_lock l(g_syncLock);
        const auto s = std::format("posesync={} waits={} avg={:.2f}ms max={:.2f}ms timeouts={}", g_syncOn.load() ? 1 : 0, g_sync.waits,
            g_sync.waits ? g_sync.waitMsSum / static_cast<double>(g_sync.waits) : 0.0, g_sync.waitMsMax, g_sync.timeouts);
        if (a_reset) {
            g_sync = SyncStats{};
        }
        return s;
    }

    // ---- htrace ----
    namespace
    {
        // Two fixed buffers (allocated once, never freed), lock-free append (fetch_add). A writer that read the
        // buffer index just before a flip writes into the other (still allocated) buffer: at worst one torn row.
        constexpr std::size_t           kTraceCap = 1u << 17;
        TraceRec*                       g_buf[2]{ nullptr, nullptr };
        std::atomic<int>                g_cur{ 0 };
        std::atomic<std::size_t>        g_traceN{ 0 };
        std::atomic<bool>               g_traceArmed{ false };
        std::atomic<unsigned long long> g_traceFrame{ 0 };
        std::atomic<unsigned long long> g_traceEnd{ 0 };  // stop when frame reaches this
        std::mutex                      g_traceCmdLock;
        // continuous mode ([Debug] bPoseTrace): files of a_framesPerFile frames, written by a worker thread
        bool                            g_contOn{ false };
        int                             g_contFrames{ 3000 }, g_contMaxFiles{ 20 }, g_contFiles{ 0 };
        std::string                     g_contDir;
        std::atomic<bool>               g_writing{ false };

        bool Alloc()
        {
            for (auto& b : g_buf) {
                if (!b) {
                    b = static_cast<TraceRec*>(::VirtualAlloc(nullptr, kTraceCap * sizeof(TraceRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                }
            }
            return g_buf[0] && g_buf[1];
        }

        std::size_t WriteCsv(const std::string& a_path, const TraceRec* a_recs, std::size_t a_n)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, a_path.c_str(), "w") != 0 || !f) {
                return 0;
            }
            LARGE_INTEGER fq{};
            ::QueryPerformanceFrequency(&fq);
            std::fprintf(f, "# qpcfreq %lld\nkind,tid,qpc,frame,seq,xrTime", static_cast<long long>(fq.QuadPart));
            for (int k = 0; k < 16; ++k) {
                std::fprintf(f, ",v%d", k);
            }
            std::fprintf(f, "\n");
            for (std::size_t i = 0; i < a_n; ++i) {
                const auto& r = a_recs[i];
                std::fprintf(f, "%c,%u,%lld,%llu,%llu,%lld", r.kind ? r.kind : '?', r.tid, r.qpc, r.frame, r.seq, r.xrTime);
                for (float x : r.v) {
                    std::fprintf(f, ",%.9g", x);
                }
                std::fprintf(f, "\n");
            }
            std::fclose(f);
            return a_n;
        }

        void ArmLocked(int a_frames)
        {
            g_traceArmed.store(false);
            g_traceN.store(0);
            g_traceEnd.store(g_traceFrame.load() + static_cast<unsigned long long>(a_frames));
            g_traceArmed.store(true);
        }
    }

    bool TraceArmed()
    {
        return g_traceArmed.load(std::memory_order_relaxed);
    }

    void TraceConfigure(bool a_continuous, int a_framesPerFile, int a_maxFiles, const std::string& a_dir)
    {
        std::scoped_lock cl(g_traceCmdLock);
        g_contOn = a_continuous && Alloc();
        g_contFrames = (std::max)(60, a_framesPerFile);
        g_contMaxFiles = (std::max)(1, a_maxFiles);
        g_contDir = a_dir;
        if (g_contOn) {
            ArmLocked(g_contFrames);
        }
    }

    void TraceSetFrame(unsigned long long a_frame)
    {
        g_traceFrame.store(a_frame, std::memory_order_relaxed);
        if (!g_traceArmed.load(std::memory_order_relaxed) || a_frame < g_traceEnd.load(std::memory_order_relaxed)) {
            return;
        }
        g_traceArmed.store(false);
        if (!g_contOn || g_writing.load()) {
            return;
        }
        // continuous: flip buffers, re-arm at once, write the full one on a worker thread
        std::unique_lock cl(g_traceCmdLock, std::try_to_lock);
        if (!cl.owns_lock() || g_contFiles >= g_contMaxFiles) {
            return;
        }
        const int         full = g_cur.load();
        const std::size_t n = (std::min)(g_traceN.load(), kTraceCap);
        const std::string path = std::format("{}\\fo4flatvr_posetrace_{:03}.csv", g_contDir, g_contFiles++);
        g_cur.store(1 - full);
        ArmLocked(g_contFrames);
        g_writing.store(true);
        std::thread([path, full, n] {
            WriteCsv(path, g_buf[full], n);
            g_writing.store(false);
        }).detach();
    }

    void Trace(TraceRec& a_rec)
    {
        if (!g_traceArmed.load(std::memory_order_relaxed)) {
            return;
        }
        TraceRec* const   buf = g_buf[g_cur.load(std::memory_order_relaxed)];
        const std::size_t i = g_traceN.fetch_add(1, std::memory_order_relaxed);
        if (!buf || i >= kTraceCap) {
            return;
        }
        LARGE_INTEGER c{};
        ::QueryPerformanceCounter(&c);
        a_rec.qpc = c.QuadPart;
        a_rec.tid = ::GetCurrentThreadId();
        a_rec.frame = g_traceFrame.load(std::memory_order_relaxed);
        buf[i] = a_rec;
    }

    std::string TraceCommand(const std::string& a_arg)
    {
        if (a_arg == "sync on" || a_arg == "sync off") {  // runtime A/B of [General] bPoseSync
            SetPoseSync(a_arg == "sync on", g_syncTimeoutMs.load());
            return PoseSyncStats(false);
        }
        if (a_arg == "sync") {
            return PoseSyncStats(false);
        }
        if (a_arg.rfind("continuous ", 0) == 0) {  // htrace continuous <frames> <files> <dir>: the [Debug] bPoseTrace path
            int  frames = 0, files = 0;
            char dir[512]{};
            if (sscanf_s(a_arg.c_str() + 11, "%d %d %511[^\n]", &frames, &files, dir, static_cast<unsigned>(sizeof(dir))) != 3) {
                return "error: htrace continuous <frames> <files> <dir>";
            }
            TraceConfigure(true, frames, files, dir);
            return std::format("htrace continuous {} frames x {} files -> {}", frames, files, dir);
        }
        std::scoped_lock cl(g_traceCmdLock);
        if (a_arg.rfind("arm", 0) == 0) {
            const int n = std::atoi(a_arg.c_str() + 3);
            if (n <= 0) {
                return "error: htrace arm <frames>";
            }
            if (!Alloc()) {
                return "error: trace buffer allocation failed";
            }
            g_contOn = false;  // a manual arm ends continuous mode
            ArmLocked(n);
            return std::format("htrace armed for {} frames (from frame {})", n, g_traceFrame.load());
        }
        if (a_arg.rfind("dump ", 0) == 0) {
            if (g_traceArmed.load()) {
                return "error: still recording";
            }
            const std::size_t n = (std::min)(g_traceN.load(), kTraceCap);
            const std::string path = a_arg.substr(5);
            if (!g_buf[g_cur.load()]) {
                return "error: nothing recorded";
            }
            WriteCsv(path, g_buf[g_cur.load()], n);
            return std::format("htrace wrote {} records to {}", n, path);
        }
        return std::format("htrace armed={} recs={} frame={} end={} continuous={} files={}", g_traceArmed.load() ? 1 : 0, g_traceN.load(),
            g_traceFrame.load(), g_traceEnd.load(), g_contOn ? 1 : 0, g_contFiles);
    }
}
