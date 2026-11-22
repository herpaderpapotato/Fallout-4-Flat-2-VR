// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/MenuPointer.h"
#include "plugin/ControllerBindings.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/PointerBeam.h"
#include "plugin/VirtualPad.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace fo4flatvr::menuptr
{
    namespace
    {
        Config            g_cfg;
        std::mutex        g_lock;  // g_cfg copy, status strings
        std::atomic<bool> g_enabled{ true };

        // Presenter thread
        bool          g_active = false;  // cinema shown and a target menu exists
        bool          g_onQuad = false;
        bool          g_onPanel = false;
        Panel         g_panel;  // Presenter thread (Dialogue.h sets it before UpdateFrame)
        float         g_u = 0.0f, g_v = 0.0f, g_dist = 0.0f;
        bool          g_trigDown = false;
        int           g_wheelDir = 0;
        std::chrono::steady_clock::time_point g_nextWheel{};
        std::atomic<std::uint64_t> g_frames{ 0 }, g_hits{ 0 }, g_clicks{ 0 }, g_wheels{ 0 }, g_cursorFrames{ 0 };
        std::string   g_target{ "-" };
        std::string   g_quadLine{ "none" }, g_aimLine{ "none" }, g_hitLine{ "none" };
        // world-screen terminal (SetWorldScreen)
        struct WorldScreen
        {
            bool        active{ false };
            std::string menu;
            float       pos[3]{}, dir[3]{};
            float       upm{ 70.0f };
        };
        WorldScreen   g_ws;
        bool          g_onWorld = false;
        std::atomic<std::uint64_t> g_worldHits{ 0 };

        // Cursor copy of the back buffer
        ComPtr<ID3D11Texture2D>        g_disp;
        ComPtr<ID3D11RenderTargetView> g_dispRtv;
        D3D11_TEXTURE2D_DESC           g_dispDesc{};

        xr::Quat Mul(const xr::Quat& a, const xr::Quat& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }
        xr::Quat Conj(const xr::Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            const xr::Quat r = Mul(Mul(q, { v.x, v.y, v.z, 0.0f }), Conj(q));
            return { r.x, r.y, r.z };
        }
        float    Dot(const xr::Vec3& a, const xr::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        xr::Vec3 Sub(const xr::Vec3& a, const xr::Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

        DXGI_FORMAT Typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
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

    const Config& GetConfig() { return g_cfg; }
    void          SetEnabled(bool a_on) { g_enabled = a_on; }
    bool          Enabled() { return g_enabled.load(); }
    bool          LeftHand() { return g_cfg.leftHanded; }

    void SetPanel(const Panel& a_panel)
    {
        g_panel = a_panel;
    }

    void SetWorldScreen(bool a_active, const char* a_menu, const float a_lp[3], const float a_ld[3], float a_upm)
    {
        g_ws.active = a_active && a_menu && a_lp && a_ld;
        if (g_ws.active) {
            g_ws.menu = a_menu;
            for (int i = 0; i < 3; ++i) {
                g_ws.pos[i] = a_lp[i], g_ws.dir[i] = a_ld[i];
            }
            g_ws.upm = a_upm;
        }
    }

    bool WorldScreenOn() { return g_onWorld; }

    bool PanelCursor(float& a_u, float& a_v, bool& a_down)
    {
        if (!g_onPanel) {
            return false;
        }
        a_u = g_u, a_v = g_v, a_down = g_trigDown;
        return true;
    }

    bool UpdateFrame(xr::Runtime& a_rt, bool a_began, bool a_cinema)
    {
        ++g_frames;
        const bool panel = g_panel.valid && !a_cinema;  // dialogue option panel (world quad) instead of the cinema quad
        const bool world = g_ws.active && !a_cinema && !panel;  // round 7: terminal monitor in the game world
        const bool on = g_enabled.load() && (a_cinema || panel || world) && a_began;
        game::MenuPointerTarget target;
        if (on) {
            if (panel) {
                target.valid = true;
                target.name = g_panel.menu;
            } else if (world) {
                target.valid = true;
                target.name = g_ws.menu;
            } else {
                game::FindPointerMenu(target);  // incl. the any-menu pass (DialogueMenu / EDI in pad mode, mod menus)
            }
        }
        g_active = on && target.valid;

        const auto& c = a_rt.Controllers();
        const bool  left = g_cfg.leftHanded;
        const bool  aimOk = c.active && (left ? c.leftAimValid : c.rightAimValid);
        const auto& aim = left ? c.leftAim : c.rightAim;
        const float trig = left ? c.leftTrigger : c.rightTrigger;
        const float stickY = left ? c.leftStickY : c.rightStickY;

        xr::Pose quad;
        float    qw = 0.0f, qh = 0.0f;
        if (panel) {
            quad = { { g_panel.quat[0], g_panel.quat[1], g_panel.quat[2], g_panel.quat[3] }, { g_panel.pos[0], g_panel.pos[1], g_panel.pos[2] } };
            qw = g_panel.width, qh = g_panel.height;
        } else {
            a_rt.CinemaQuadPose(quad, qw, qh);
        }
        bool hit = false;
        if (world) {
            float u = 0.0f, v = 0.0f, dist = 0.0f;
            if (g_active && game::PickWorldScreen(g_ws.pos, g_ws.dir, g_ws.upm, u, v, dist)) {
                hit = true;
                g_u = std::clamp(u, 0.0f, 1.0f), g_v = std::clamp(v, 0.0f, 1.0f), g_dist = dist / (g_ws.upm > 0.0f ? g_ws.upm : 70.0f);
                ++g_hits;
                ++g_worldHits;
            }
        } else if (g_active && aimOk && qw > 0.0f && qh > 0.0f) {
            const xr::Vec3 d = Rot(aim.orientation, { 0.0f, 0.0f, -1.0f });
            const xr::Vec3 n = Rot(quad.orientation, { 0.0f, 0.0f, 1.0f });
            const float    den = Dot(d, n);
            if (den < -1e-4f) {  // ray hits the visible (front) side
                const float t = Dot(Sub(quad.position, aim.position), n) / den;
                if (t > 0.0f && t < 50.0f) {
                    const xr::Vec3 p{ aim.position.x + d.x * t, aim.position.y + d.y * t, aim.position.z + d.z * t };
                    const xr::Vec3 l = Rot(Conj(quad.orientation), Sub(p, quad.position));
                    const float    u = l.x / qw + 0.5f, v = 0.5f - l.y / qh;
                    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                        hit = true;
                        g_u = u, g_v = v, g_dist = t;
                        // 21 Nov (workshop agent): the cinema quad may show a sub-rectangle of its image (the workshop build
                        // area on the off hand, XrRuntime::SetCinemaQuadCrop): quad uv -> image uv
                        float crop[4];
                        if (!panel && a_rt.CinemaQuadCrop(crop)) {
                            g_u = crop[0] + u * (crop[2] - crop[0]);
                            g_v = crop[1] + v * (crop[3] - crop[1]);
                        }
                        ++g_hits;
                    }
                }
            }
        }
        g_onQuad = hit && !panel && !world;  // PrepareCinema draws the dot only on the cinema image
        g_onPanel = hit && panel;
        // Pointer beam (PointerBeam.h): while the pointer is live, from the pointing hand to the hit (end dot) or out to the
        // beam's full length. A terminal on its monitor: the engine pick distance along the same aim ray.
        if (g_active && (aimOk || world)) {
            beam::Ray br;
            br.src = beam::Source::kMenu;
            br.hit = hit;
            br.length = g_dist;
            br.pressed = g_trigDown;
            beam::Offer(left ? 0 : 1, br);
        }
        // Trigger with hysteresis; a click must start on the quad.
        const bool wasDown = g_trigDown;
        if (!g_trigDown && trig > 0.6f && hit) {
            g_trigDown = true;
            ++g_clicks;
        } else if (g_trigDown && trig < 0.4f) {
            g_trigDown = false;
        }
        if (!g_active) {
            g_trigDown = false;
        }
        // Thumbstick Y -> wheel: one step per 150 ms while past 0.5.
        int        wheel = 0;
        const auto now = std::chrono::steady_clock::now();
        const int  dir = stickY > 0.5f ? 1 : stickY < -0.5f ? -1 : 0;
        // (not while the EDI dialogue list owns the stick: its steps are D-pad pulses, ControllerBindings)
        if (hit && !panel && dir != 0 && !bindings::DialogueListNavActive() && (dir != g_wheelDir || now >= g_nextWheel)) {
            wheel = dir;
            g_nextWheel = now + std::chrono::milliseconds(150);
            ++g_wheels;
        }
        g_wheelDir = dir;

        game::MenuPointerInput in;
        in.onQuad = hit;
        in.u = panel ? g_panel.u0 + g_u * (g_panel.u1 - g_panel.u0) : g_u;
        in.v = panel ? g_panel.v0 + g_v * (g_panel.v1 - g_panel.v0) : g_v;
        in.down = g_trigDown;
        in.wheel = wheel;
        in.movieUv = world;
        game::SetMenuPointer(in, g_active, panel ? g_panel.menu.c_str() : world ? g_ws.menu.c_str() : nullptr);
        g_onWorld = world && g_active && (hit || g_trigDown);

        {
            std::scoped_lock l(g_lock);
            g_target = target.valid ? target.name : "-";
            const auto& o = quad.orientation;
            const auto& p = quad.position;
            g_quadLine = std::format("pos {:.4f},{:.4f},{:.4f} quat {:.4f},{:.4f},{:.4f},{:.4f} size {:.3f}x{:.3f}", p.x, p.y, p.z, o.x, o.y, o.z,
                o.w, qw, qh);
            if (aimOk) {
                const xr::Vec3 d = Rot(aim.orientation, { 0.0f, 0.0f, -1.0f });
                g_aimLine = std::format("pos {:.4f},{:.4f},{:.4f} dir {:.4f},{:.4f},{:.4f}", aim.position.x, aim.position.y, aim.position.z, d.x, d.y, d.z);
            }
            if (hit || wasDown != g_trigDown) {
                g_hitLine = std::format("u={:.4f} v={:.4f} dist={:.3f} trigger={:.2f} down={}", g_u, g_v, g_dist, trig, g_trigDown ? 1 : 0);
            }
        }
        return g_active && (hit || g_trigDown);
    }

    ID3D11Texture2D* PrepareCinema(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_bb)
    {
        // (the pointer beam's end dot replaces this dot, and saves the full-size copy)
        if (!a_bb || !a_ctx || !g_active || !g_onQuad || !g_cfg.cursorDot || beam::DotReplacesCursor(beam::Source::kMenu)) {
            return a_bb;
        }
        D3D11_TEXTURE2D_DESC sd{};
        a_bb->GetDesc(&sd);
        if (sd.SampleDesc.Count > 1) {
            return a_bb;
        }
        const DXGI_FORMAT fmt = Typed(sd.Format);
        if (!g_disp || g_dispDesc.Width != sd.Width || g_dispDesc.Height != sd.Height || g_dispDesc.Format != fmt) {
            g_disp.Reset();
            g_dispRtv.Reset();
            g_dispDesc = {};
            ComPtr<ID3D11Device> dev;
            a_bb->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width;
            td.Height = sd.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (!dev || FAILED(dev->CreateTexture2D(&td, nullptr, &g_disp)) || FAILED(dev->CreateRenderTargetView(g_disp.Get(), nullptr, &g_dispRtv))) {
                REX::ERROR("pointer: cursor texture {}x{} fmt {} creation failed", td.Width, td.Height, static_cast<int>(fmt));
                g_disp.Reset();
                g_dispRtv.Reset();
                return a_bb;
            }
            g_dispDesc = td;
        }
        a_ctx->CopySubresourceRegion(g_disp.Get(), 0, 0, 0, 0, a_bb, 0, nullptr);
        ComPtr<ID3D11DeviceContext1> c1;
        if (SUCCEEDED(a_ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
            const LONG cx = static_cast<LONG>(g_u * static_cast<float>(sd.Width));
            const LONG cy = static_cast<LONG>(g_v * static_cast<float>(sd.Height));
            const LONG r = (std::max)(4L, static_cast<LONG>(sd.Width / 240));
            const LONG b = (std::max)(2L, r / 3);
            const float outer[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            const float inner[4] = { g_trigDown ? 0.3f : 1.0f, 1.0f, g_trigDown ? 0.3f : 1.0f, 1.0f };  // green while held
            const D3D11_RECT ro{ cx - r - b, cy - r - b, cx + r + b + 1, cy + r + b + 1 };
            const D3D11_RECT ri{ cx - r, cy - r, cx + r + 1, cy + r + 1 };
            c1->ClearView(g_dispRtv.Get(), outer, &ro, 1);
            c1->ClearView(g_dispRtv.Get(), inner, &ri, 1);
        }
        ++g_cursorFrames;
        return g_disp.Get();
    }

    std::string Status()
    {
        std::string target, quad, aim, hit;
        {
            std::scoped_lock l(g_lock);
            target = g_target;
            quad = g_quadLine;
            aim = g_aimLine;
            hit = g_hitLine;
        }
        const auto pad = pad::Last();  // what the virtual pad got last (trigger suppression check)
        return std::format("enabled={} active={} target='{}' onQuad={} u={:.4f} v={:.4f} down={} leftHanded={} frames={} hits={} clicks={} wheels={} cursorFrames={} padLT={:.2f} padRT={:.2f} | quad {} | aim {} | pointer {} | game {}",
                   g_enabled.load() ? 1 : 0, g_active ? 1 : 0, target, g_onQuad ? 1 : 0, g_u, g_v, g_trigDown ? 1 : 0, g_cfg.leftHanded ? 1 : 0, g_frames.load(),
                   g_hits.load(), g_clicks.load(), g_wheels.load(), g_cursorFrames.load(), pad.lt, pad.rt, quad, aim, hit, game::MenuPointerGameReport()) +
               std::format(" | world active={} on={} hits={} {}", g_ws.active ? 1 : 0, g_onWorld ? 1 : 0, g_worldHits.load(), game::WorldScreenReport());
    }
}
