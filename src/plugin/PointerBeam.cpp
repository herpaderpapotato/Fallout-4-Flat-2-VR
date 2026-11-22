// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/PointerBeam.h"
#include "plugin/GameBridge.h"
#include "plugin/GameMenuPointer.h"  // game::PipboyScreenColor
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>

namespace fo4flatvr::beam
{
    namespace
    {
        Config            g_cfg;
        std::mutex        g_lock;  // g_cfg writes, status line
        std::atomic<bool> g_enabled{ true };

        // Presenter thread
        struct Slot
        {
            bool valid{ false };
            Ray  ray;
        };
        Slot     g_offer[2];
        bool     g_headValid = false;
        xr::Pose g_head{};
        bool     g_worldAllowed = false;
        bool     g_frameOpen = false;

        std::atomic<std::uint64_t> g_frames{ 0 }, g_beams{ 0 }, g_dots{ 0 }, g_offers[static_cast<int>(Source::kCount)]{}, g_worldQueries{ 0 },
            g_worldHits{ 0 };
        std::string g_last[2]{ "none", "none" };
        int         g_worldMode = 0;
        // activation preview fade clock (FO4VR: +0x38 of the preview objects, reset when they are rebuilt)
        float                                 g_previewS = 0.0f;
        bool                                  g_previewShown = false;
        std::chrono::steady_clock::time_point g_previewLast{};
        std::atomic<std::uint64_t>            g_previewFrames{ 0 }, g_previewTargets{ 0 };

        const char* SrcName(Source s)
        {
            switch (s) {
            case Source::kWorld: return "world";
            case Source::kActivate: return "activate";
            case Source::kPipboy: return "pipboy";
            case Source::kMenu: return "menu";
            case Source::kExternal: return "external";
            default: return "?";
            }
        }

        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            // v' = v + 2w (q x v) + 2 q x (q x v)
            const xr::Vec3 u{ q.x, q.y, q.z };
            const xr::Vec3 t{ 2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x) };
            return { v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z), v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
        }

        bool SourceOn(const Config& c, Source s)
        {
            switch (s) {
            case Source::kMenu: return c.menus;
            case Source::kPipboy: return c.pipboy;
            case Source::kWorld: return c.workshop;
            case Source::kActivate: return c.activate;
            default: return true;
            }
        }

        // A hit beats a miss; then the priority (callers' explicit one, else by source: menu > pipboy > external > world > activate).
        int Rank(const Ray& r)
        {
            static constexpr int kBySource[] = { 1, 0, 3, 4, 2 };
            const int base = r.priority != 0 ? r.priority : kBySource[static_cast<int>(r.src)];
            return (r.hit ? 1000 : 0) + base;
        }

        float ReadFloat(const std::string& a_path, const char* a_key, float a_default)
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("Beam", a_key, "", buf, sizeof(buf), a_path.c_str());
            if (!buf[0]) {
                return a_default;
            }
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : a_default;
        }
        bool ReadBool(const std::string& a_path, const char* a_key, bool a_default)
        {
            return ::GetPrivateProfileIntA("Beam", a_key, a_default ? 1 : 0, a_path.c_str()) != 0;
        }
    }

    void Configure(const Config& a_cfg)
    {
        {
            std::scoped_lock l(g_lock);
            g_cfg = a_cfg;
        }
        g_enabled = a_cfg.enabled;
    }

    void LoadAndConfigure(const std::string& a_iniPath, bool a_leftHanded)
    {
        Config c;
        c.enabled = ReadBool(a_iniPath, "bBeam", c.enabled);
        c.dot = ReadBool(a_iniPath, "bDot", c.dot);
        c.rgba[0] = std::clamp(ReadFloat(a_iniPath, "fColorR", c.rgba[0]), 0.0f, 1.0f);
        c.rgba[1] = std::clamp(ReadFloat(a_iniPath, "fColorG", c.rgba[1]), 0.0f, 1.0f);
        c.rgba[2] = std::clamp(ReadFloat(a_iniPath, "fColorB", c.rgba[2]), 0.0f, 1.0f);
        c.rgba[3] = std::clamp(ReadFloat(a_iniPath, "fAlpha", c.rgba[3]), 0.0f, 1.0f);
        c.widthCm = std::clamp(ReadFloat(a_iniPath, "fWidthCm", c.widthCm), 0.05f, 10.0f);
        c.maxLengthM = std::clamp(ReadFloat(a_iniPath, "fMaxLengthM", c.maxLengthM), 0.1f, 200.0f);
        c.dotSizeDeg = std::clamp(ReadFloat(a_iniPath, "fDotSizeDeg", c.dotSizeDeg), 0.05f, 10.0f);
        c.dotMinCm = std::clamp(ReadFloat(a_iniPath, "fDotMinCm", c.dotMinCm), 0.05f, 20.0f);
        c.startCm = std::clamp(ReadFloat(a_iniPath, "fStartCm", c.startCm), 0.0f, 50.0f);
        c.menus = ReadBool(a_iniPath, "bMenus", c.menus);
        c.pipboy = ReadBool(a_iniPath, "bPipboy", c.pipboy);
        c.workshop = ReadBool(a_iniPath, "bWorkshop", c.workshop);
        c.activate = ReadBool(a_iniPath, "bActivate", c.activate);
        c.activateReachM = std::clamp(ReadFloat(a_iniPath, "fActivateReachM", c.activateReachM), 0.1f, 50.0f);
        c.activateFloorMult = std::clamp(ReadFloat(a_iniPath, "fActivateFloorMult", c.activateFloorMult), 0.0f, 10.0f);
        c.activateFadeStartS = std::clamp(ReadFloat(a_iniPath, "fActivateFadeStartS", c.activateFadeStartS), 0.0f, 3600.0f);
        c.activateFadeS = std::clamp(ReadFloat(a_iniPath, "fActivateFadeS", c.activateFadeS), 0.0f, 3600.0f);
        c.activateMarkerCm = std::clamp(ReadFloat(a_iniPath, "fActivateMarkerCm", c.activateMarkerCm), 0.0f, 200.0f);
        c.activatePipboyColor = ReadBool(a_iniPath, "bActivatePipboyColor", c.activatePipboyColor);
        c.leftHanded = a_leftHanded;
        Configure(c);
        REX::INFO("pointer beam {} (dot {}, colour {:.2f},{:.2f},{:.2f} a {:.2f}, width {:.2f} cm, max {:.1f} m; menus {} pipboy {} workshop/command {} "
                  "activate {}; {} hand)",
            c.enabled ? "on" : "off", c.dot ? 1 : 0, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3], c.widthCm, c.maxLengthM, c.menus ? 1 : 0,
            c.pipboy ? 1 : 0, c.workshop ? 1 : 0, c.activate ? 1 : 0, c.leftHanded ? "left" : "right");
    }

    const Config& GetConfig() { return g_cfg; }
    void          SetEnabled(bool a_on) { g_enabled = a_on; }
    bool          Enabled() { return g_enabled.load(); }
    int           Primary() { return g_cfg.leftHanded ? 0 : 1; }

    bool DotReplacesCursor(Source a_src)
    {
        return g_enabled.load() && g_cfg.dot && SourceOn(g_cfg, a_src);
    }

    void BeginFrame(const xr::Pose* a_head, bool a_worldAllowed)
    {
        g_offer[0] = g_offer[1] = Slot{};
        g_headValid = a_head != nullptr;
        if (a_head) {
            g_head = *a_head;
        }
        g_worldAllowed = a_worldAllowed;
        g_frameOpen = true;
    }

    void Offer(int a_hand, const Ray& a_ray)
    {
        if (!g_frameOpen || a_hand < 0 || a_hand > 1 || a_ray.src >= Source::kCount || !g_enabled.load() || !SourceOn(g_cfg, a_ray.src)) {
            return;
        }
        ++g_offers[static_cast<int>(a_ray.src)];
        auto& s = g_offer[a_hand];
        if (!s.valid || Rank(a_ray) > Rank(s.ray)) {
            s.valid = true;
            s.ray = a_ray;
        }
    }

    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx)
    {
        if (!g_frameOpen) {
            return;
        }
        g_frameOpen = false;
        if (!g_enabled.load() || !g_headValid || !a_ctx) {
            return;
        }
        const Config c = g_cfg;
        ++g_frames;
        // FO4VR's laser: workshop mode / commanding a companion (bWorkshop); the activation ray otherwise (bActivate, opt-in)
        g_worldMode = 0;
        if (g_worldAllowed && (c.workshop || c.activate)) {
            const int mode = c.workshop ? game::BeamWorldMode() : 0;
            g_worldMode = mode;
            float dist = 0.0f;
            bool  hit = false, target = false;
            Ray   r;
            bool  have = false;
            ++g_worldQueries;
            if (mode != 0) {
                if (game::HandRayWorldHit(c.maxLengthM, dist, hit)) {
                    r.src = Source::kWorld;
                    r.hit = hit;
                    r.length = dist;
                    have = true;
                }
            } else if (game::ActivatePreview(c.activateReachM, c.activateFloorMult, c.maxLengthM, dist, target)) {
                // FO4VR activation preview: always drawn to its length with an end marker; pressed = has a target
                r.src = Source::kActivate;
                r.hit = true;
                r.length = dist;
                r.pressed = target;
                have = true;
            }
            if (have) {
                if (r.hit) ++g_worldHits;
                g_frameOpen = true;  // Offer() takes offers only inside a frame
                Offer(Primary(), r);
                g_frameOpen = false;
            }
        }
        // preview fade clock: runs while the preview is drawn every frame, restarts after any frame without it
        const auto now = std::chrono::steady_clock::now();
        const bool previewNow = g_offer[Primary()].valid && g_offer[Primary()].ray.src == Source::kActivate;
        if (previewNow && g_previewShown) {
            g_previewS += (std::min)(0.25f, std::chrono::duration<float>(now - g_previewLast).count());
        } else {
            g_previewS = 0.0f;
        }
        g_previewShown = previewNow;
        g_previewLast = now;
        const auto& ctl = a_rt.Controllers();
        for (int hand = 0; hand < 2; ++hand) {
            const auto& s = g_offer[hand];
            if (!s.valid) {
                continue;
            }
            const bool aimOk = ctl.active && (hand == 0 ? ctl.leftAimValid : ctl.rightAimValid);
            if (!aimOk) {
                continue;
            }
            const xr::Pose& aim = hand == 0 ? ctl.leftAim : ctl.rightAim;
            const xr::Vec3  d = Rot(aim.orientation, { 0.0f, 0.0f, -1.0f });
            if (s.ray.src == Source::kActivate) {
                // FO4VR look (VR 140EF0F30): world_targetIndicator_ray.nif (a 72.4 u cone, base 1.79 u across) at ref scale
                // len / 96 u from the wand -> the cone covers 75.4 % of the length, base width 1.87 % of it; marker
                // activateArc_Target (target, green) / activateArc_noTarget (white) at the full length. Colour: white, green
                // with a target (143884D20 / 143884D50), or the Pip-Boy colour. The cone fades after fActivationPreviewFadeTime1
                // over FadeTime2; the marker does not fade.
                const float len = s.ray.length;
                if (!(len > 0.01f)) {
                    continue;
                }
                float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                if (s.ray.pressed) {
                    rgba[0] = 0.0f, rgba[2] = 0.0f;
                    ++g_previewTargets;
                }
                if (c.activatePipboyColor) {
                    float rgb[3], bright = 1.0f;
                    if (game::PipboyScreenColor(rgb, bright, false)) {
                        rgba[0] = rgb[0], rgba[1] = rgb[1], rgba[2] = rgb[2];
                    }
                }
                float fade = 1.0f;
                if (c.activateFadeStartS > 0.0f || c.activateFadeS > 0.0f) {
                    if (g_previewS >= c.activateFadeStartS + c.activateFadeS) {
                        fade = 0.0f;
                    } else if (g_previewS > c.activateFadeStartS) {
                        fade = 1.0f - (g_previewS - c.activateFadeStartS) / (std::max)(c.activateFadeS, 1e-3f);
                    }
                }
                rgba[3] = std::round(std::clamp(fade, 0.0f, 1.0f) * 32.0f) / 32.0f;  // quantised: few cone re-uploads
                const float    coneLen = len * (72.4f / 96.0f);
                const xr::Vec3 from = aim.position;
                const xr::Vec3 to{ aim.position.x + d.x * coneLen, aim.position.y + d.y * coneLen, aim.position.z + d.z * coneLen };
                const xr::Vec3 end{ aim.position.x + d.x * len, aim.position.y + d.y * len, aim.position.z + d.z * len };
                const float marker = c.dot ? c.activateMarkerCm * 0.01f : 0.0f;
                // cone hidden once faded out (alpha 0); the marker stays at the full length
                if (a_rt.SubmitBeam(a_ctx, hand, from, to, g_head.position, 0.0187f * len, rgba, marker, s.ray.pressed, xr::Runtime::kBeamPreview,
                        &end)) {
                    ++g_beams;
                    ++g_previewFrames;
                    if (marker > 0.0f) ++g_dots;
                    std::scoped_lock l(g_lock);
                    g_last[hand] = std::format("activate-preview {} len={:.3f}m fade={:.2f} t={:.1f}s", s.ray.pressed ? "target" : "no-target", len,
                        rgba[3], g_previewS);
                }
                continue;
            }
            const float     len = s.ray.hit ? s.ray.length : c.maxLengthM;
            const float     start = (std::min)(c.startCm * 0.01f, len * 0.5f);
            if (!(len > start + 0.002f)) {
                continue;
            }
            const xr::Vec3 from{ aim.position.x + d.x * start, aim.position.y + d.y * start, aim.position.z + d.z * start };
            const xr::Vec3 to{ aim.position.x + d.x * len, aim.position.y + d.y * len, aim.position.z + d.z * len };
            float dot = 0.0f;
            if (s.ray.hit && c.dot) {
                dot = (std::max)(c.dotMinCm * 0.01f, 2.0f * len * std::tan(c.dotSizeDeg * 0.5f * 0.0174532925f));
            }
            if (a_rt.SubmitBeam(a_ctx, hand, from, to, g_head.position, c.widthCm * 0.01f, c.rgba, dot, s.ray.pressed)) {
                ++g_beams;
                if (dot > 0.0f) ++g_dots;
                std::scoped_lock l(g_lock);
                g_last[hand] = std::format("{} {} len={:.3f}m{}", SrcName(s.ray.src), s.ray.hit ? "hit" : "miss", len, s.ray.pressed ? " pressed" : "");
            }
        }
    }

    std::string Status()
    {
        std::string l0, l1;
        {
            std::scoped_lock l(g_lock);
            l0 = g_last[0];
            l1 = g_last[1];
        }
        return std::format("enabled={} dot={} frames={} beams={} dots={} offers menu={} pipboy={} world={} activate={} external={} worldQueries={} "
                           "worldHits={} worldMode={} | left: {} | right: {}",
            g_enabled.load() ? 1 : 0, g_cfg.dot ? 1 : 0, g_frames.load(), g_beams.load(), g_dots.load(), g_offers[static_cast<int>(Source::kMenu)].load(),
            g_offers[static_cast<int>(Source::kPipboy)].load(), g_offers[static_cast<int>(Source::kWorld)].load(),
            g_offers[static_cast<int>(Source::kActivate)].load(), g_offers[static_cast<int>(Source::kExternal)].load(), g_worldQueries.load(),
            g_worldHits.load(), g_worldMode, l0, l1);
    }
}
