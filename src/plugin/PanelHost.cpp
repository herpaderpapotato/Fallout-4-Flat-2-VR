// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/PanelHost.h"

#include "plugin/Haptics.h"
#include "plugin/PanelHostApi.h"
#include "plugin/PointerBeam.h"
#include "plugin/Settings.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fo4flatvr::panelhost
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int kMaxPanels = xr::Runtime::kMaxWorldQuads - xr::Runtime::kTopWorldQuad - 1;  // slots after the settings panel

        struct Panel
        {
            std::string          id, title;
            FO4FlatVR_PanelDesc  desc{};
            int                  slot{ 0 };
            bool                 open{ false };
            bool                 wantAnchor{ false };
            int                  below{ -1 };
            float                belowGap{ 0.02f };
            xr::Pose             pose;
            bool                 poseValid{ false };
            bool                 visible{ false };
            ID3D11Texture2D*     tex{ nullptr };
            bool                 rendered{ false };
            Clock::time_point    lastDraw{};
            // input
            int                  owner{ -1 };
            bool                 clickHeld{ false };
            bool                 armed[2]{ true, true };
            bool                 closeWas[2]{ false, false };
            int                  stepDir{ 0 };
            Clock::time_point    nextStep{};
            float                u{ 0 }, v{ 0 }, dist{ 0 };
            bool                 simAim{ false };
            float                simU{ 0 }, simV{ 0 };
            bool                 simDown{ false };
            // stats
            std::uint64_t        draws{ 0 }, submits{ 0 }, opens{ 0 }, clicks{ 0 };
        };

        std::recursive_mutex                  g_lock;  // panels, frame work
        std::vector<std::unique_ptr<Panel>>   g_panels;
        std::array<std::atomic<std::uint8_t>, 8> g_open{};  // mirrors Panel::open for IsOpen without the frame lock
        std::mutex                            g_reqLock;
        std::vector<std::function<void()>>    g_reqs;
        std::atomic<bool>                     g_enabled{ true };
        std::atomic<bool>                     g_xrRunning{ false };
        std::atomic<bool>                     g_owns{ false };
        std::atomic<int>                      g_maskBits{ 0 };  // bit 0 left, bit 1 right
        std::atomic<int>                      g_gestureReq{ 0 };
        bool                                  g_installed{ false };

        void Queue(std::function<void()> a_fn)
        {
            std::scoped_lock l(g_reqLock);
            g_reqs.push_back(std::move(a_fn));
        }

        Panel* Get(int a_h) { return a_h >= 0 && a_h < static_cast<int>(g_panels.size()) ? g_panels[a_h].get() : nullptr; }

        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            const float ux = q.x, uy = q.y, uz = q.z;
            const float cx = uy * v.z - uz * v.y, cy = uz * v.x - ux * v.z, cz = ux * v.y - uy * v.x;
            const float tx = 2 * cx, ty = 2 * cy, tz = 2 * cz;
            const float c2x = uy * tz - uz * ty, c2y = uz * tx - ux * tz, c2z = ux * ty - uy * tx;
            return { v.x + q.w * tx + c2x, v.y + q.w * ty + c2y, v.z + q.w * tz + c2z };
        }
        xr::Quat Mul(const xr::Quat& a, const xr::Quat& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }
        xr::Quat FromYpr(const float a_deg[3])  // yaw about +Y, pitch about +X, roll about -Z (OpenXR axes), in that order
        {
            const float d = 3.14159265f / 180.0f;
            auto axis = [](float a, float x, float y, float z) {
                return xr::Quat{ x * std::sin(a * 0.5f), y * std::sin(a * 0.5f), z * std::sin(a * 0.5f), std::cos(a * 0.5f) };
            };
            return Mul(Mul(axis(a_deg[0] * d, 0, 1, 0), axis(a_deg[1] * d, 1, 0, 0)), axis(a_deg[2] * d, 0, 0, -1));
        }

        float HeightM(const Panel& p) { return p.desc.widthM * static_cast<float>(p.desc.texH) / static_cast<float>((std::max)(1u, p.desc.texW)); }

        void Anchor(Panel& p, const xr::Pose& a_head)
        {
            const xr::Vec3 f = Rot(a_head.orientation, { 0, 0, -1 });
            float          a = 0.0f;
            if (f.x * f.x + f.z * f.z > 1e-4f) a = std::atan2(-f.x, -f.z);
            const xr::Quat q{ 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
            const xr::Vec3 off = Rot(q, { p.desc.pos[0], p.desc.pos[1], p.desc.pos[2] });
            p.pose = { q, { a_head.position.x + off.x, a_head.position.y + off.y, a_head.position.z + off.z } };
            p.poseValid = true;
            REX::INFO("panels: '{}' anchored ({:.2f}, {:.2f}, {:.2f}) m from the head, {:.2f} m wide (head yaw {:.1f} deg)", p.id,
                p.desc.pos[0], p.desc.pos[1], p.desc.pos[2], p.desc.widthM, a * 57.2957795f);
        }

        bool HitTest(const Panel& p, const xr::Pose& a_aim, bool a_held, float& u, float& v, float& t)
        {
            if (!p.poseValid) return false;
            const xr::Vec3 d = Rot(a_aim.orientation, { 0, 0, -1 });
            const xr::Vec3 n = Rot(p.pose.orientation, { 0, 0, 1 });
            const float    dn = d.x * n.x + d.y * n.y + d.z * n.z;
            if (dn > -1e-3f) return false;
            const xr::Vec3 po{ p.pose.position.x - a_aim.position.x, p.pose.position.y - a_aim.position.y,
                p.pose.position.z - a_aim.position.z };
            t = (po.x * n.x + po.y * n.y + po.z * n.z) / dn;
            if (!(t > 0.02f) || t > 20.0f) return false;
            const xr::Vec3 hp{ a_aim.position.x + d.x * t - p.pose.position.x, a_aim.position.y + d.y * t - p.pose.position.y,
                a_aim.position.z + d.z * t - p.pose.position.z };
            const xr::Vec3 right = Rot(p.pose.orientation, { 1, 0, 0 }), up = Rot(p.pose.orientation, { 0, 1, 0 });
            u = (hp.x * right.x + hp.y * right.y + hp.z * right.z) / p.desc.widthM + 0.5f;
            v = 0.5f - (hp.x * up.x + hp.y * up.y + hp.z * up.z) / HeightM(p);
            const float m = a_held ? 0.25f : 0.0f;
            return !(u < -m || u > 1 + m || v < -m || v > 1 + m);
        }

        void SetOpenLocked(Panel& p, int a_h, bool a_open, const char* a_why)
        {
            if (p.open == a_open) return;
            p.open = a_open;
            g_open[a_h] = a_open ? 1 : 0;
            if (a_open) {
                p.wantAnchor = p.desc.placement == FO4FLATVR_PANEL_WORLD;
                p.armed[0] = p.armed[1] = false;  // a trigger already held stays the game's until released
                p.rendered = false;
                ++p.opens;
            } else {
                p.owner = -1;
                p.clickHeld = false;
                p.visible = false;
            }
            REX::INFO("panels: '{}' {} ({})", p.id, a_open ? "open" : "closed", a_why);
            if (p.desc.onEvent) p.desc.onEvent(p.desc.user, a_h, a_open ? 1u : 2u);
        }

        void CopyPlacement(Panel& p, const FO4FlatVR_PanelDesc& d)
        {
            p.desc.placement = d.placement;
            for (int i = 0; i < 3; ++i) p.desc.pos[i] = d.pos[i], p.desc.ypr[i] = d.ypr[i];
            if (d.widthM > 0.01f) p.desc.widthM = d.widthM;
            p.desc.flags = d.flags;
        }

        // ---- the C API ----
        int32_t ApiRegister(const FO4FlatVR_PanelDesc* d)
        {
            if (!g_enabled) {
                REX::WARN("panels: registration refused: [PanelHost] bEnabled=0");
                return -1;
            }
            if (!d || d->size < sizeof(FO4FlatVR_PanelDesc) || !d->id || !d->draw || d->texW < 16 || d->texH < 16 || d->texW > 4096 ||
                d->texH > 4096 || !(d->widthM > 0.02f)) {
                REX::WARN("panels: registration refused: bad descriptor");
                return -1;
            }
            std::scoped_lock l(g_lock);
            for (std::size_t i = 0; i < g_panels.size(); ++i)
                if (g_panels[i]->id == d->id) {
                    REX::WARN("panels: '{}' registered again: the first registration is kept", d->id);
                    return static_cast<int32_t>(i);
                }
            if (static_cast<int>(g_panels.size()) >= kMaxPanels) {
                REX::WARN("panels: '{}' refused: all {} panel slots are taken", d->id, kMaxPanels);
                return -1;
            }
            auto p = std::make_unique<Panel>();
            p->id = d->id;
            p->title = d->title ? d->title : d->id;
            p->desc = *d;
            p->desc.id = nullptr;
            p->desc.title = nullptr;
            p->slot = xr::Runtime::kTopWorldQuad + 1 + static_cast<int>(g_panels.size());
            const int h = static_cast<int>(g_panels.size());
            REX::INFO("panels: '{}' ({}) registered as panel {} (quad slot {}): {}x{} px, {:.2f} m wide, placement {}, flags 0x{:X}", p->id,
                p->title, h, p->slot, d->texW, d->texH, d->widthM, d->placement, d->flags);
            g_panels.push_back(std::move(p));
            return h;
        }
        void ApiSetOpen(int32_t h, uint8_t open)
        {
            Queue([h, open] {
                if (auto* p = Get(h)) SetOpenLocked(*p, h, open != 0, "client");
            });
        }
        void ApiToggle(int32_t h)
        {
            Queue([h] {
                if (auto* p = Get(h)) SetOpenLocked(*p, h, !p->open, "client toggle");
            });
        }
        uint8_t ApiIsOpen(int32_t h) { return h >= 0 && h < static_cast<int32_t>(g_open.size()) ? g_open[h].load() : 0; }
        void    ApiReanchor(int32_t h)
        {
            Queue([h] {
                if (auto* p = Get(h); p && p->desc.placement == FO4FLATVR_PANEL_WORLD) {
                    p->wantAnchor = true;
                    p->below = -1;
                }
            });
        }
        void ApiSetPlacement(int32_t h, const FO4FlatVR_PanelDesc* d)
        {
            if (!d) return;
            const FO4FlatVR_PanelDesc c = *d;
            Queue([h, c] {
                if (auto* p = Get(h)) {
                    CopyPlacement(*p, c);
                    p->wantAnchor = p->open && c.placement == FO4FLATVR_PANEL_WORLD;
                    p->poseValid = p->poseValid && c.placement == FO4FLATVR_PANEL_WORLD;
                }
            });
        }
        void ApiPlaceBelow(int32_t h, int32_t other, float gap)
        {
            Queue([h, other, gap] {
                if (auto* p = Get(h)) {
                    p->below = other;
                    p->belowGap = gap;
                }
            });
        }
        uint8_t ApiXrRunning() { return g_xrRunning ? 1 : 0; }
        void    ApiHaptic(int32_t hand, float amp, float sec)
        {
            if (hand == 0 || hand == 1) haptics::Request(hand, std::clamp(amp, 0.0f, 1.0f), std::clamp(sec, 0.0f, 0.5f));
        }

        const FO4FlatVR_PanelHost kApi{ FO4FLATVR_PANEL_HOST_VERSION, sizeof(FO4FlatVR_PanelHost), &ApiRegister, &ApiSetOpen, &ApiToggle,
            &ApiIsOpen, &ApiReanchor, &ApiSetPlacement, &ApiPlaceBelow, &ApiXrRunning, &ApiHaptic };
    }

    void Install()
    {
        std::scoped_lock l(g_lock);
        if (g_installed) return;
        g_installed = true;
        g_enabled = ::GetPrivateProfileIntA("PanelHost", "bEnabled", 1, Settings::Path().c_str()) != 0;
        REX::INFO("panels: other plugins' VR panels {} ([PanelHost] bEnabled), {} slots", g_enabled ? "on" : "OFF", kMaxPanels);
    }

    void GestureToggle() { g_gestureReq.fetch_add(1); }
    bool OwnsPointer() { return g_owns.load(); }

    void UpdateFrame(xr::Runtime& a_rt, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, bool a_began, const xr::Pose* a_head,
        int a_reservedHand)
    {
        g_xrRunning = a_rt.running();
        if (!g_enabled) return;
        std::vector<std::function<void()>> reqs;
        {
            std::scoped_lock rl(g_reqLock);
            reqs.swap(g_reqs);
        }
        std::scoped_lock l(g_lock);
        for (auto& r : reqs) r();
        for (int n = g_gestureReq.exchange(0); n > 0; --n) {
            bool any = false;
            for (std::size_t i = 0; i < g_panels.size(); ++i) {
                auto& p = *g_panels[i];
                if (p.desc.flags & FO4FLATVR_PANEL_GESTURE_TOGGLE) {
                    SetOpenLocked(p, static_cast<int>(i), !p.open, "gesture");
                    any = true;
                }
            }
            if (!any) REX::INFO("panels: the 'Mod panel' action ran, but no panel asked for it");
        }
        const auto now = Clock::now();
        const auto& c = a_rt.Controllers();
        const bool  active = a_began && c.active;

        // ---- poses / visibility ----
        for (std::size_t i = 0; i < g_panels.size(); ++i) {
            auto& p = *g_panels[i];
            p.visible = false;
            if (!p.open) continue;
            if (p.desc.placement == FO4FLATVR_PANEL_WORLD) {
                if (p.below >= 0) {
                    const Panel* o = Get(p.below);
                    if (o && o->open && o->poseValid && o->desc.placement == FO4FLATVR_PANEL_WORLD) {
                        const xr::Vec3 up = Rot(o->pose.orientation, { 0, 1, 0 });
                        const float    dy = HeightM(*o) * 0.5f + p.belowGap + HeightM(p) * 0.5f;
                        p.pose = { o->pose.orientation, { o->pose.position.x - up.x * dy, o->pose.position.y - up.y * dy,
                                                            o->pose.position.z - up.z * dy } };
                        p.poseValid = true;
                        p.wantAnchor = false;
                    }
                }
                if (p.wantAnchor && a_head) {
                    Anchor(p, *a_head);
                    p.wantAnchor = false;
                }
                p.visible = p.poseValid && !p.wantAnchor;
            } else {
                const bool      left = p.desc.placement == FO4FLATVR_PANEL_LEFT_HAND;
                const bool      valid = active && (left ? c.leftGripPoseValid : c.rightGripPoseValid);
                const xr::Pose& g = left ? c.leftGripPose : c.rightGripPose;
                if (valid) {
                    const xr::Vec3 o = Rot(g.orientation, { p.desc.pos[0], p.desc.pos[1], p.desc.pos[2] });
                    p.pose = { Mul(g.orientation, FromYpr(p.desc.ypr)), { g.position.x + o.x, g.position.y + o.y, g.position.z + o.z } };
                    p.poseValid = true;
                    bool show = true;
                    if ((p.desc.flags & FO4FLATVR_PANEL_SHOW_WHEN_FACING) && a_head) {
                        const xr::Vec3 n = Rot(p.pose.orientation, { 0, 0, 1 });
                        xr::Vec3       to{ a_head->position.x - p.pose.position.x, a_head->position.y - p.pose.position.y,
                            a_head->position.z - p.pose.position.z };
                        const float    len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
                        show = len > 1e-3f && (n.x * to.x + n.y * to.y + n.z * to.z) / len > 0.64f;  // within ~50 deg
                    }
                    p.visible = show;
                } else {
                    p.poseValid = false;
                }
            }
        }

        // ---- rays: each hand on the nearest visible panel it hits (not the settings panel's hand, not its own panel) ----
        int   handPanel[2]{ -1, -1 };
        float hu[2]{}, hv[2]{}, ht[2]{};
        for (int h = 0; h < 2; ++h) {
            if (!active || h == a_reservedHand || !(h ? c.rightAimValid : c.leftAimValid)) continue;
            const xr::Pose& aim = h ? c.rightAim : c.leftAim;
            float           best = 1e9f;
            for (std::size_t i = 0; i < g_panels.size(); ++i) {
                auto& p = *g_panels[i];
                if (!p.visible) continue;
                if ((p.desc.placement == FO4FLATVR_PANEL_LEFT_HAND && h == 0) || (p.desc.placement == FO4FLATVR_PANEL_RIGHT_HAND && h == 1))
                    continue;
                float u, v, t;
                const bool held = p.clickHeld && p.owner == h;
                if (HitTest(p, aim, held, u, v, t) && (held || t < best)) {
                    best = held ? -1.0f : t;
                    handPanel[h] = static_cast<int>(i);
                    hu[h] = u, hv[h] = v, ht[h] = t;
                }
            }
        }

        int  mask = 0;
        bool owns = false;
        for (std::size_t i = 0; i < g_panels.size(); ++i) {
            auto& p = *g_panels[i];
            if (!p.open) continue;
            int owner = -1;
            if (p.owner >= 0 && handPanel[p.owner] == static_cast<int>(i)) owner = p.owner;
            else if (handPanel[beam::Primary()] == static_cast<int>(i)) owner = beam::Primary();
            else if (handPanel[1 - beam::Primary()] == static_cast<int>(i)) owner = 1 - beam::Primary();
            if (owner != p.owner) p.clickHeld = false;
            p.owner = owner;

            FO4FlatVR_PanelFrame in{};
            in.size = sizeof(in);
            in.device = a_dev;
            in.context = a_ctx;
            in.dt = p.lastDraw.time_since_epoch().count() ? std::chrono::duration<float>(now - p.lastDraw).count() : 1.0f / 72.0f;
            in.visible = p.visible ? 1 : 0;
            in.input.hand = -1;
            bool down = false;
            if (p.simAim) {
                in.input.hover = 1;
                in.input.x = p.simU * static_cast<float>(p.desc.texW);
                in.input.y = p.simV * static_cast<float>(p.desc.texH);
                down = p.simDown;
            } else if (owner >= 0) {
                in.input.hover = 1;
                in.input.hand = owner;
                in.input.x = std::clamp(hu[owner], 0.0f, 1.0f) * static_cast<float>(p.desc.texW);
                in.input.y = std::clamp(hv[owner], 0.0f, 1.0f) * static_cast<float>(p.desc.texH);
                const float tr = owner ? c.rightTrigger : c.leftTrigger;
                if (tr < 0.35f) p.armed[owner] = true;
                down = p.armed[owner] && (p.clickHeld ? tr > 0.35f : tr > 0.6f);
                const float sx = owner ? c.rightStickX : c.leftStickX, sy = owner ? c.rightStickY : c.leftStickY;
                if (std::abs(sy) > 0.25f && std::abs(sy) > std::abs(sx)) in.input.wheel = sy * in.dt * 5.0f;
                const int dir = sx > 0.6f ? 1 : sx < -0.6f ? -1 : 0;
                if (dir == 0) {
                    p.stepDir = 0;
                } else if (dir != p.stepDir || now >= p.nextStep) {
                    p.nextStep = now + std::chrono::milliseconds(dir != p.stepDir ? 450 : 150);
                    p.stepDir = dir;
                    in.input.step = dir;
                }
                const bool cb = owner ? c.b : c.y;
                in.input.closeButton = (cb && !p.closeWas[owner]) ? 1 : 0;
                p.closeWas[owner] = cb;
                mask |= 1 << owner;
                beam::Ray ray;
                ray.src = beam::Source::kExternal;
                ray.hit = true;
                ray.length = ht[owner];
                ray.pressed = down;
                beam::Offer(owner, ray);
            } else {
                p.stepDir = 0;
            }
            if (down && !p.clickHeld) {
                ++p.clicks;
                if (owner >= 0) haptics::Request(owner, 0.35f, 0.025f);
            }
            p.clickHeld = down;
            in.input.down = down ? 1 : 0;
            owns = owns || owner >= 0 || p.clickHeld || p.simAim;

            if (!p.visible) continue;  // a hand panel looked away from: not drawn, not submitted
            FO4FlatVR_PanelOutput out{};
            p.lastDraw = now;
            p.desc.draw(p.desc.user, &in, &out);
            ++p.draws;
            p.tex = out.texture;
            p.rendered = out.texture != nullptr;
            if (out.requestClose || (in.input.closeButton && (p.desc.flags & FO4FLATVR_PANEL_B_CLOSES))) {
                SetOpenLocked(p, static_cast<int>(i), false, out.requestClose ? "the panel asked" : "B / Y");
            }
        }
        g_maskBits = mask;
        g_owns = owns;
    }

    void MaskControllers(xr::ControllerState& a_c)
    {
        const int m = g_maskBits.load();
        if (m & 2) {
            a_c.rightTrigger = 0.0f;
            a_c.rightStickX = a_c.rightStickY = 0.0f;
            a_c.a = a_c.b = false;
        }
        if (m & 1) {
            a_c.leftTrigger = 0.0f;
            a_c.leftStickX = a_c.leftStickY = 0.0f;
            a_c.x = a_c.y = false;
        }
    }

    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx)
    {
        if (!g_enabled) return;
        std::scoped_lock l(g_lock);
        for (auto& pp : g_panels) {
            auto& p = *pp;
            if (!p.open || !p.visible || !p.rendered || !p.tex) continue;
            if (a_rt.SubmitWorldQuad(a_ctx, p.tex, p.pose, p.desc.widthM, true, (p.desc.flags & FO4FLATVR_PANEL_PREMULTIPLIED) != 0, p.slot))
                ++p.submits;
        }
    }

    std::string Command(const std::string& a_arg)
    {
        std::scoped_lock l(g_lock);
        std::string verb = a_arg, rest;
        if (const auto sp = a_arg.find(' '); sp != std::string::npos) verb = a_arg.substr(0, sp), rest = a_arg.substr(sp + 1);
        auto find = [&](const std::string& a_id) -> int {
            for (std::size_t i = 0; i < g_panels.size(); ++i)
                if (g_panels[i]->id == a_id) return static_cast<int>(i);
            return -1;
        };
        if (verb.empty() || verb == "status") {
            std::string s = std::format("panels enabled={} xr={} owns={} mask={} count={}", g_enabled.load(), g_xrRunning.load(), g_owns.load(),
                g_maskBits.load(), g_panels.size());
            for (std::size_t i = 0; i < g_panels.size(); ++i) {
                const auto& p = *g_panels[i];
                s += std::format(" | {} '{}' slot={} open={} visible={} owner={} draws={} submits={} opens={} clicks={}", i, p.id, p.slot,
                    p.open, p.visible, p.owner, p.draws, p.submits, p.opens, p.clicks);
            }
            return s;
        }
        if (verb == "open" || verb == "close" || verb == "toggle") {
            const int h = find(rest);
            if (h < 0) return "error: no panel " + rest;
            auto& p = *g_panels[h];
            SetOpenLocked(p, h, verb == "open" ? true : verb == "close" ? false : !p.open, "pipe");
            return std::format("{} {}", p.id, p.open ? "open" : "closed");
        }
        if (verb == "aim") {
            if (rest == "off") {
                for (auto& p : g_panels) p->simAim = false, p->simDown = false;
                return "aim off";
            }
            char  id[128]{}, act[16]{};
            float u = 0, v = 0;
            if (sscanf_s(rest.c_str(), "%127s %f %f %15s", id, static_cast<unsigned>(sizeof(id)), &u, &v, act, static_cast<unsigned>(sizeof(act))) < 3) return "error: aim <id> <u> <v> [down|up]";
            const int h = find(id);
            if (h < 0) return std::string("error: no panel ") + id;
            auto& p = *g_panels[h];
            p.simAim = true, p.simU = u, p.simV = v;
            if (std::string(act) == "down") p.simDown = true;
            if (std::string(act) == "up") p.simDown = false;
            return std::format("aim {} {:.3f} {:.3f} down={}", p.id, u, v, p.simDown);
        }
        return "error: panels [status|open <id>|close <id>|toggle <id>|aim <id> <u> <v> [down|up]|aim off]";
    }
}

extern "C" __declspec(dllexport) const FO4FlatVR_PanelHost* FO4FLATVR_GetPanelHost(uint32_t a_version)
{
    if (a_version == 0 || a_version > FO4FLATVR_PANEL_HOST_VERSION) return nullptr;
    return &fo4flatvr::panelhost::kApi;
}
