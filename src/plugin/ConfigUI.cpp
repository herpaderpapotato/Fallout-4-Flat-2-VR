// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/ConfigUI.h"

#include "plugin/CameraDirector.h"
#include "plugin/CommandChannel.h"
#include "plugin/ConfigStore.h"
#include "plugin/FrikBridge.h"
#include "plugin/FrikBridgeApi.h"
#include "plugin/GameBridge.h"
#include "plugin/Haptics.h"
#include "plugin/PointerBeam.h"
#include "plugin/Presenter.h"
#include "plugin/Settings.h"
#include "plugin/SpecPages.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"  // ImGui::GetHoveredID (hover haptics)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace fo4flatvr::configui
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

#include "plugin/ConfigFont.gen.inl"

        enum class RowType
        {
            kBool,
            kInt,
            kFloat,
            kEnum,
            kPolicy,
            kVk
        };
        struct Row
        {
            const char* page;
            const char* group;
            const char* section;
            const char* key;
            RowType     type;
            const char* def;
            float       mn, mx, step;
            const char* options;  // '|' separated; "@keys" = the gesture key table; "@vk" = kVkList
            const char* label;
            const char* help;
            const char* apply;    // "" = after a restart
            bool        mcm;
        };
        constexpr Row kRows[] = {
#include "plugin/ConfigSpec.gen.inl"
        };
        constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
        const char* const kKeyNames[] = {
#include "plugin/ConfigKeys.gen.inl"
        };
        constexpr int kKeyNameCount = static_cast<int>(sizeof(kKeyNames) / sizeof(kKeyNames[0]));
        constexpr const char* kPages[] = { "Quick", "Camera", "Comfort", "Render", "Height", "Gestures", "Selfie", "FRIK", "Keys" };
        constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));
        constexpr const char* kStateNames[] = { "FirstPerson", "AutoVanity", "VATS", "Free", "IronSights", "Transition", "Tween",
            "Animated", "ThirdPerson", "Furniture", "Mount", "Bleedout", "Dialogue" };
        struct VkDef
        {
            const char* name;
            int         vk;
        };
        constexpr VkDef kVkList[] = { { "(none)", 0 }, { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 }, { "F5", 0x74 },
            { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 }, { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
            { "Insert", 0x2D }, { "Delete", 0x2E }, { "Home", 0x24 }, { "End", 0x23 }, { "Page Up", 0x21 }, { "Page Down", 0x22 },
            { "Num 0", 0x60 }, { "Num 1", 0x61 }, { "Num 2", 0x62 }, { "Num 3", 0x63 }, { "Num 4", 0x64 }, { "Num 5", 0x65 },
            { "Num 6", 0x66 }, { "Num 7", 0x67 }, { "Num 8", 0x68 }, { "Num 9", 0x69 }, { "Pause", 0x13 }, { "Scroll Lock", 0x91 } };
        constexpr int kVkCount = static_cast<int>(sizeof(kVkList) / sizeof(kVkList[0]));

        // Mods Config FO4 VR spec.json pages (SpecPages.h): FRIK's under the FRIK tab, other mods' under "Mods"
        constexpr int kSpecHoverBase = 100000;  // State::hoverRow == this: a spec row (specMod / specRow)

        constexpr int kTexW = 1280, kTexH = 896;

        struct OwnCfg
        {
            bool  enabled{ true };     // [ConfigUI] bEnabled (load only)
            bool  menuHold{ true };    // [ConfigUI] bMenuHold
            float menuHoldS{ 1.0f };   // [ConfigUI] fMenuHoldS
            float distM{ 0.7f }, widthM{ 0.9f }, heightM{ -0.12f };
            bool  haptics{ true };
            int   keyVk{ 0x71 };       // [Keys] iConfigMenu
            int   keyMod{ 2 };         // [Keys] iConfigMenuMod: 0 none, 1 Shift, 2 Ctrl, 3 Alt
        };

        struct HandHit
        {
            bool  hit{ false };
            float u{ 0 }, v{ 0 }, dist{ 0 };
        };

        struct State
        {
            OwnCfg cfg;
            bool   installed{ false };
            // panel
            bool        open{ false };
            bool        wantAnchor{ false };
            xr::Pose    pose;            // panel centre in the reference space
            float       widthM{ 0.9f };
            int         page{ 0 };
            bool        confirmClose{ false };
            std::string toast;
            Clock::time_point toastAt{};
            // values
            std::map<std::string, std::string> pending;   // "sec\nkey" lower -> value (previews)
            std::set<std::string>              dirty;     // previews not yet flushed to the merged file / applied
            Clock::time_point                  nextFlush{};
            std::map<std::string, std::string> lastApplied;  // live keys: value last applied
            std::map<std::string, std::string> start;        // value in effect at load (restart markers)
            bool                               appliedInit{ false };
            // spec.json pages (SpecPages.h)
            int                                frikPage{ 0 };    // FRIK tab sub-page: 0 = fo4flatvr's FRIK rows, n = FRIK spec page n-1
            int                                modSel{ 0 };      // Mods tab: mod (index among the non-FRIK specs) and page
            int                                modPage{ 0 };
            int                                specMod{ -1 };    // the spec row under the ray (hoverRow == kSpecHoverBase)
            const specpages::Row*              specRow{ nullptr };
            int                                specModNext{ -1 };
            const specpages::Row*              specRowNext{ nullptr };
            // input
            int        owner{ -1 };     // hand whose ray is on the panel (0 left, 1 right)
            bool       clickHeld{ false };
            bool       armed[2]{ true, true };
            bool       closeBtnWas[2]{ false, false };
            HandHit    hits[2];
            xr::Vec3   aimPos[2], aimDir[2];  // last aim rays (reference space; status readout)
            bool       aimValid[2]{};
            float      wheel{ 0 };
            int        hoverRow{ -1 }, hoverRowNext{ -1 };
            ImGuiID    lastHoverId{ 0 };
            int        stepDir{ 0 };
            Clock::time_point nextStep{};
            bool       simAim{ false };
            float      simU{ 0 }, simV{ 0 };
            bool       simDown{ false };
            std::string help;
            bool       keyWas{ false };
            Clock::time_point lastFrame{};
            // render
            bool                     imguiOk{ false };
            bool                     imguiFailed{ false };
            ID3D11Texture2D*         tex{ nullptr };
            ID3D11RenderTargetView*  rtv{ nullptr };
            bool                     rendered{ false };
            // stats
            std::uint64_t frames{ 0 }, submits{ 0 }, opens{ 0 }, applies{ 0 }, saves{ 0 }, clicks{ 0 };
            std::string   lastApply{ "-" };
        };

        std::recursive_mutex g_lock;
        State                g_s;
        std::atomic<int>     g_toggleReq{ 0 };
        std::atomic<float>   g_menuHoldS{ 1.0f };
        std::atomic<bool>    g_ownsPointer{ false };
        std::atomic<bool>    g_enabled{ false };
        std::atomic<int>     g_maskHand{ -1 };

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        std::string KeyOf(const Row& r) { return Lower(r.section) + "\n" + Lower(r.key); }

        bool SameValue(const std::string& a, const std::string& b)
        {
            char *ea = nullptr, *eb = nullptr;
            const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
            if (ea != a.c_str() && eb != b.c_str() && *ea == 0 && *eb == 0) return std::abs(x - y) < 1e-4;
            return Lower(a) == Lower(b);
        }

        std::vector<std::string> Split(const char* a_s, char a_d)
        {
            std::vector<std::string> out;
            std::string              cur;
            for (const char* p = a_s; *p; ++p) {
                if (*p == a_d) {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur += *p;
                }
            }
            if (!cur.empty() || !out.empty()) out.push_back(cur);
            return out;
        }

        std::vector<std::string> Options(const Row& r)
        {
            if (std::strcmp(r.options, "@keys") == 0) return std::vector<std::string>(kKeyNames, kKeyNames + kKeyNameCount);
            if (std::strcmp(r.options, "@vk") == 0) {
                std::vector<std::string> v;
                for (const auto& k : kVkList) v.emplace_back(k.name);
                return v;
            }
            return Split(r.options, '|');
        }

        // ---- values ----
        std::string Base(const Row& r)
        {
            std::string v;
            return cfgstore::UserValue(r.section, r.key, v) ? v : std::string(r.def);
        }
        std::string Saved(const Row& r)
        {
            std::string v;
            return cfgstore::McmValue(r.section, r.key, v) ? v : Base(r);
        }
        std::string Current(const State& s, const Row& r)
        {
            if (const auto it = s.pending.find(KeyOf(r)); it != s.pending.end()) return it->second;
            return Saved(r);
        }
        std::string Effective(const Row& r)  // what the merged file holds (previews included)
        {
            std::string v;
            return cfgstore::EffectiveValue(r.section, r.key, v) ? v : std::string(r.def);
        }
        bool IsLive(const Row& r) { return r.apply && r.apply[0]; }

        float AsFloat(const std::string& v) { return std::strtof(v.c_str(), nullptr); }
        int   AsInt(const std::string& v) { return static_cast<int>(std::strtol(v.c_str(), nullptr, 0)); }

        std::string Fmt(const Row& r, float v)
        {
            if (r.type == RowType::kInt) return std::to_string(static_cast<int>(std::lround(v)));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return buf;
        }

        int Decimals(const Row& r)
        {
            if (r.type == RowType::kInt || r.step >= 1.0f) return 0;
            if (r.step >= 0.1f) return 1;
            if (r.step >= 0.01f) return 2;
            return 3;
        }

        void Toast(State& s, std::string a_msg)
        {
            REX::INFO("cfgui: {}", a_msg);
            s.toast = std::move(a_msg);
            s.toastAt = Clock::now();
        }

        // ---- own settings ----
        void LoadOwnCfg(State& s)
        {
            const auto p = Settings::Path();
            auto rf = [&](const char* sec, const char* k, float d) {
                char buf[64]{};
                ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), p.c_str());
                char*       e = nullptr;
                const float v = std::strtof(buf, &e);
                return e != buf && std::isfinite(v) ? v : d;
            };
            auto& c = s.cfg;
            c.menuHold = ::GetPrivateProfileIntA("ConfigUI", "bMenuHold", 1, p.c_str()) != 0;
            c.menuHoldS = std::clamp(rf("ConfigUI", "fMenuHoldS", 1.0f), 0.3f, 5.0f);
            c.distM = std::clamp(rf("ConfigUI", "fDistanceM", 0.7f), 0.3f, 3.0f);
            c.widthM = std::clamp(rf("ConfigUI", "fWidthM", 0.9f), 0.3f, 3.0f);
            c.heightM = std::clamp(rf("ConfigUI", "fHeightM", -0.12f), -1.0f, 1.0f);
            c.haptics = ::GetPrivateProfileIntA("ConfigUI", "bHaptics", 1, p.c_str()) != 0;
            c.keyVk = static_cast<int>(::GetPrivateProfileIntA("Keys", "iConfigMenu", 0x71, p.c_str()));
            c.keyMod = std::clamp(static_cast<int>(::GetPrivateProfileIntA("Keys", "iConfigMenuMod", 2, p.c_str())), 0, 3);
            g_menuHoldS = (c.enabled && c.menuHold) ? c.menuHoldS : 0.0f;
        }

        // ---- apply ----
        std::string Expand(const std::string& a_tmpl, const std::string& a_v)
        {
            std::string out;
            for (std::size_t i = 0; i < a_tmpl.size();) {
                if (a_tmpl[i] == '{') {
                    const auto e = a_tmpl.find('}', i);
                    if (e != std::string::npos) {
                        const std::string tok = a_tmpl.substr(i + 1, e - i - 1);
                        if (tok == "v") {
                            out += a_v;
                        } else if (tok == "onoff") {
                            out += (a_v != "0" && Lower(a_v) != "false") ? "on" : "off";
                        } else if (tok.rfind("enum:", 0) == 0) {
                            const auto words = Split(tok.c_str() + 5, ',');
                            const int  ix = AsInt(a_v);
                            out += (ix >= 0 && ix < static_cast<int>(words.size())) ? words[ix] : a_v;
                        } else {
                            out += a_tmpl.substr(i, e - i + 1);
                        }
                        i = e + 1;
                        continue;
                    }
                }
                out += a_tmpl[i++];
            }
            return out;
        }

        void Apply(State& s, const Row& r, const std::string& a_v)
        {
            if (!IsLive(r)) return;
            const std::string tmpl = r.apply;
            std::string       res;
            if (tmpl == "@height") {
                game::SetHeightOffsetCm(AsFloat(a_v));
                res = std::format("height offset {} cm", a_v);
            } else if (tmpl == "@present") {
                present::ReloadHotkeys();
                const bool en = s.cfg.enabled;
                LoadOwnCfg(s);  // the panel's own key ([Keys] iConfigMenu) too
                s.cfg.enabled = en;
                res = "hotkeys re-read";
            } else if (tmpl == "@reload") {
                // 20 Nov (live settings): the module that owns the key re-reads it from the merged file (cfgstore::Subscribe);
                // the preview / save already rebuilt that file, so the reload runs at the next DispatchChanges (this frame)
                res = "re-read by its module";
            } else if (tmpl == "@configui") {
                const bool en = s.cfg.enabled;
                LoadOwnCfg(s);
                s.cfg.enabled = en;
                g_menuHoldS = (s.cfg.enabled && s.cfg.menuHold) ? s.cfg.menuHoldS : 0.0f;
                res = "panel settings re-read";
            } else {
                const std::string cmd = Expand(tmpl, a_v);
                res = cmd + " -> " + command::Execute(cmd);
            }
            ++s.applies;
            s.lastApply = std::format("{}:{}={} ({})", r.key, r.section, a_v, res.size() > 160 ? res.substr(0, 160) + "..." : res);
            s.lastApplied[KeyOf(r)] = a_v;
            REX::INFO("cfgui: apply {}", s.lastApply);
        }

        const Row* FirstRow(const std::string& a_key)
        {
            for (const auto& r : kRows)
                if (KeyOf(r) == a_key) return &r;
            return nullptr;
        }

        // previews into the merged file + live apply, at most every 100 ms (slider drags)
        void Flush(State& s, bool a_force)
        {
            if (s.dirty.empty()) return;
            const auto now = Clock::now();
            if (!a_force && now < s.nextFlush) return;
            s.nextFlush = now + std::chrono::milliseconds(100);
            for (const auto& k : s.dirty) {
                const Row* r = FirstRow(k);
                if (!r) continue;
                if (const auto it = s.pending.find(k); it != s.pending.end()) {
                    cfgstore::SetPreview(r->section, r->key, it->second);
                } else {
                    cfgstore::ClearPreview(r->section, r->key);
                }
            }
            cfgstore::Rebuild();
            for (const auto& k : s.dirty) {
                const Row* r = FirstRow(k);
                if (!r || !IsLive(*r)) continue;
                const std::string v = Effective(*r);
                if (const auto it = s.lastApplied.find(k); it == s.lastApplied.end() || !SameValue(it->second, v)) {
                    Apply(s, *r, v);
                }
            }
            s.dirty.clear();
        }

        void SetValue(State& s, const Row& r, const std::string& a_v)
        {
            const auto k = KeyOf(r);
            if (SameValue(a_v, Saved(r))) {
                s.pending.erase(k);
            } else {
                s.pending[k] = a_v;
            }
            s.dirty.insert(k);
        }

        cfgstore::VType VTypeOf(const Row& r)
        {
            switch (r.type) {
            case RowType::kBool: return cfgstore::VType::kBool;
            case RowType::kFloat: return cfgstore::VType::kFloat;
            case RowType::kPolicy: return cfgstore::VType::kString;
            default: return cfgstore::VType::kInt;
            }
        }

        int RestartCount(State& s)
        {
            int                   n = 0;
            std::set<std::string> seen;
            for (const auto& r : kRows) {
                const auto k = KeyOf(r);
                if (IsLive(r) || !seen.insert(k).second) continue;
                const auto it = s.start.find(k);
                if (it != s.start.end() && !SameValue(it->second, Current(s, r))) ++n;
            }
            return n;
        }

        std::size_t PendingCount(const State& s) { return s.pending.size() + specpages::PendingCount(); }

        void Save(State& s)
        {
            Flush(s, true);
            std::string specHow;
            const int   specN = specpages::Save(specHow);
            int         n = 0, fails = 0;
            std::string how;
            for (const auto& [k, v] : std::map<std::string, std::string>(s.pending)) {
                const Row* r = FirstRow(k);
                if (!r) continue;
                std::string h;
                bool        ok = false;
                if (SameValue(v, Base(*r)) && cfgstore::RemoveOverride(r->section, r->key)) {
                    ok = true, h = "line removed (back to fo4flatvr.ini / default)";
                } else {
                    ok = cfgstore::Save(r->section, r->key, v, VTypeOf(*r), h);
                }
                REX::INFO("cfgui: save {}:{}={} -> {} ({})", r->key, r->section, v, ok ? "ok" : "FAILED", h);
                if (ok) {
                    ++n;
                    cfgstore::ClearPreview(r->section, r->key);
                    s.pending.erase(k);
                    how = h;
                } else {
                    ++fails;
                }
            }
            cfgstore::Rebuild();
            ++s.saves;
            const int restart = RestartCount(s);
            Toast(s, std::format("Saved {} setting{}{}{}{}{}", n, n == 1 ? "" : "s", how.empty() ? "" : " (" + how + ")",
                fails ? std::format(", {} FAILED", fails) : "", specN ? std::format(", {} mod setting{} (FRIK.toml / spec files){}", specN, specN == 1 ? "" : "s", specHow.empty() ? "" : " " + specHow) : "",
                restart ? std::format(". Restart the game for {} of them.", restart) : ""));
        }

        void Discard(State& s)
        {
            const auto keys = s.pending;
            s.pending.clear();
            for (const auto& [k, v] : keys) s.dirty.insert(k);
            Flush(s, true);
            const auto frikN = specpages::Discard();
            const auto total = keys.size() + frikN;
            Toast(s, std::format("Discarded {} change{}", total, total == 1 ? "" : "s"));
        }

        void Close(State& s, const char* a_why)
        {
            s.open = false;
            s.confirmClose = false;
            s.owner = -1;
            s.clickHeld = false;
            g_ownsPointer = false;
            g_maskHand = -1;
            REX::INFO("cfgui: closed ({})", a_why);
        }

        void RequestClose(State& s, const char* a_why)
        {
            if (PendingCount(s) != 0) {
                s.confirmClose = true;
                return;
            }
            Close(s, a_why);
        }

        // ---- live re-apply of changes made elsewhere (MCM menu, editor) ----
        void ReapplyChanged(State& s)
        {
            std::set<std::string> seen;
            for (const auto& r : kRows) {
                const auto k = KeyOf(r);
                if (!IsLive(r) || !seen.insert(k).second) continue;
                const std::string v = Effective(r);
                const auto        it = s.lastApplied.find(k);
                if (it == s.lastApplied.end()) {
                    s.lastApplied[k] = v;
                } else if (!SameValue(it->second, v)) {
                    Apply(s, r, v);
                }
            }
        }

        // ---- math ----
        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)
        {
            const float ux = q.x, uy = q.y, uz = q.z;
            const float cx = uy * v.z - uz * v.y, cy = uz * v.x - ux * v.z, cz = ux * v.y - uy * v.x;
            const float tx = 2 * cx, ty = 2 * cy, tz = 2 * cz;
            const float c2x = uy * tz - uz * ty, c2y = uz * tx - ux * tz, c2z = ux * ty - uy * tx;
            return { v.x + q.w * tx + c2x, v.y + q.w * ty + c2y, v.z + q.w * tz + c2z };
        }

        void Anchor(State& s, const xr::Pose& a_head)
        {
            const xr::Vec3 f = Rot(a_head.orientation, { 0, 0, -1 });
            float          a = 0.0f;
            if (f.x * f.x + f.z * f.z > 1e-4f) a = std::atan2(-f.x, -f.z);
            const xr::Quat q{ 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
            const xr::Vec3 off = Rot(q, { 0.0f, s.cfg.heightM, -s.cfg.distM });
            s.pose = { q, { a_head.position.x + off.x, a_head.position.y + off.y, a_head.position.z + off.z } };
            s.widthM = s.cfg.widthM;
            REX::INFO("cfgui: panel anchored {:.2f} m ahead, {:.2f} m, {:.2f} m wide (head yaw {:.1f} deg)", s.cfg.distM, s.cfg.heightM, s.widthM,
                a * 57.2957795f);
        }

        HandHit HitTest(const State& s, const xr::Pose& a_aim, bool a_held)
        {
            HandHit        h;
            const xr::Vec3 d = Rot(a_aim.orientation, { 0, 0, -1 });
            const xr::Vec3 n = Rot(s.pose.orientation, { 0, 0, 1 });
            const float    dn = d.x * n.x + d.y * n.y + d.z * n.z;
            if (dn > -1e-3f) return h;  // parallel or from behind
            const xr::Vec3 po{ s.pose.position.x - a_aim.position.x, s.pose.position.y - a_aim.position.y, s.pose.position.z - a_aim.position.z };
            const float    t = (po.x * n.x + po.y * n.y + po.z * n.z) / dn;
            if (!(t > 0.02f) || t > 20.0f) return h;
            const xr::Vec3 hp{ a_aim.position.x + d.x * t - s.pose.position.x, a_aim.position.y + d.y * t - s.pose.position.y,
                a_aim.position.z + d.z * t - s.pose.position.z };
            const xr::Vec3 right = Rot(s.pose.orientation, { 1, 0, 0 }), up = Rot(s.pose.orientation, { 0, 1, 0 });
            const float    w = s.widthM, hgt = s.widthM * kTexH / kTexW;
            const float    u = (hp.x * right.x + hp.y * right.y + hp.z * right.z) / w + 0.5f;
            const float    v = 0.5f - (hp.x * up.x + hp.y * up.y + hp.z * up.z) / hgt;
            const float    m = a_held ? 0.25f : 0.0f;
            if (u < -m || u > 1 + m || v < -m || v > 1 + m) return h;
            h.hit = true, h.u = u, h.v = v, h.dist = t;
            return h;
        }

        void Pulse(const State& s, int a_hand, float a_amp, float a_sec)
        {
            if (s.cfg.haptics && a_hand >= 0) haptics::Request(a_hand, a_amp, a_sec);
        }

        // ---- ImGui ----
        void Style()
        {
            ImGuiStyle& st = ImGui::GetStyle();
            st.WindowRounding = 10.0f;
            st.FrameRounding = 6.0f;
            st.GrabRounding = 6.0f;
            st.ScrollbarSize = 26.0f;
            st.ScrollbarRounding = 8.0f;
            st.FramePadding = { 14.0f, 9.0f };
            st.ItemSpacing = { 12.0f, 10.0f };
            st.WindowPadding = { 22.0f, 18.0f };
            st.GrabMinSize = 26.0f;
            ImVec4* c = st.Colors;
            const ImVec4 green{ 0.55f, 1.0f, 0.62f, 1.0f };
            c[ImGuiCol_Text] = green;
            c[ImGuiCol_TextDisabled] = { 0.35f, 0.62f, 0.40f, 1.0f };
            c[ImGuiCol_WindowBg] = { 0.02f, 0.06f, 0.035f, 0.95f };
            c[ImGuiCol_ChildBg] = { 0.0f, 0.0f, 0.0f, 0.0f };
            c[ImGuiCol_PopupBg] = { 0.03f, 0.09f, 0.05f, 0.98f };
            c[ImGuiCol_Border] = { 0.25f, 0.6f, 0.32f, 0.8f };
            c[ImGuiCol_FrameBg] = { 0.06f, 0.18f, 0.09f, 1.0f };
            c[ImGuiCol_FrameBgHovered] = { 0.12f, 0.32f, 0.17f, 1.0f };
            c[ImGuiCol_FrameBgActive] = { 0.18f, 0.45f, 0.24f, 1.0f };
            c[ImGuiCol_Button] = { 0.08f, 0.24f, 0.12f, 1.0f };
            c[ImGuiCol_ButtonHovered] = { 0.16f, 0.45f, 0.23f, 1.0f };
            c[ImGuiCol_ButtonActive] = { 0.26f, 0.65f, 0.33f, 1.0f };
            c[ImGuiCol_SliderGrab] = { 0.4f, 0.9f, 0.5f, 1.0f };
            c[ImGuiCol_SliderGrabActive] = { 0.6f, 1.0f, 0.65f, 1.0f };
            c[ImGuiCol_Header] = { 0.08f, 0.24f, 0.12f, 1.0f };
            c[ImGuiCol_HeaderHovered] = { 0.16f, 0.45f, 0.23f, 1.0f };
            c[ImGuiCol_HeaderActive] = { 0.26f, 0.65f, 0.33f, 1.0f };
            c[ImGuiCol_Separator] = { 0.25f, 0.6f, 0.32f, 0.8f };
            c[ImGuiCol_ScrollbarBg] = { 0.02f, 0.08f, 0.04f, 1.0f };
            c[ImGuiCol_ScrollbarGrab] = { 0.2f, 0.5f, 0.27f, 1.0f };
            c[ImGuiCol_ScrollbarGrabHovered] = { 0.3f, 0.7f, 0.38f, 1.0f };
            c[ImGuiCol_ScrollbarGrabActive] = { 0.4f, 0.9f, 0.5f, 1.0f };
        }

        bool InitImGui(State& s, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx)
        {
            if (s.imguiOk) return true;
            if (s.imguiFailed || !a_dev || !a_ctx) return false;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kTexW;
            td.Height = kTexH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &s.tex)) || FAILED(a_dev->CreateRenderTargetView(s.tex, nullptr, &s.rtv))) {
                REX::ERROR("cfgui: panel texture could not be created; the panel stays off");
                s.imguiFailed = true;
                return false;
            }
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            io.DisplaySize = { static_cast<float>(kTexW), static_cast<float>(kTexH) };
            ImFontConfig fc;
            fc.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoMedium), static_cast<int>(kRobotoMediumSize), 30.0f, &fc);
            Style();
            if (!ImGui_ImplDX11_Init(a_dev, a_ctx)) {
                REX::ERROR("cfgui: ImGui DX11 backend init failed; the panel stays off");
                ImGui::DestroyContext();
                s.imguiFailed = true;
                return false;
            }
            s.imguiOk = true;
            REX::INFO("cfgui: ImGui {} ready, panel texture {}x{}", IMGUI_VERSION, kTexW, kTexH);
            return true;
        }

        const ImVec4 kYellow{ 1.0f, 0.86f, 0.35f, 1.0f };
        const ImVec4 kGrey{ 0.4f, 0.62f, 0.45f, 1.0f };

        bool ArrowsControl(const char* a_label, float a_width, int& a_index, int a_count)
        {
            bool changed = false;
            if (ImGui::Button("<", { 48, 0 })) {
                a_index = (a_index + a_count - 1) % a_count;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(a_label, { a_width, 0 })) {
                a_index = (a_index + 1) % a_count;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(">", { 48, 0 })) {
                a_index = (a_index + 1) % a_count;
                changed = true;
            }
            return changed;
        }

        int VkIndex(int a_vk)
        {
            for (int i = 0; i < kVkCount; ++i)
                if (kVkList[i].vk == a_vk) return i;
            return -1;
        }

        void StepRow(State& s, const Row& r, int a_dir)
        {
            const std::string cur = Current(s, r);
            switch (r.type) {
            case RowType::kBool: SetValue(s, r, AsInt(cur) ? "0" : "1"); break;
            case RowType::kInt:
            case RowType::kFloat: {
                float v = AsFloat(cur) + a_dir * r.step;
                v = std::clamp(v, r.mn, r.mx);
                if (r.step > 0) v = std::round(v / r.step) * r.step;
                SetValue(s, r, Fmt(r, v));
                break;
            }
            case RowType::kEnum: {
                const int n = static_cast<int>(Options(r).size());
                if (n > 0) SetValue(s, r, std::to_string(((AsInt(cur) + a_dir) % n + n) % n));
                break;
            }
            case RowType::kPolicy: {
                const auto o = Options(r);
                int        i = 0;
                for (int j = 0; j < static_cast<int>(o.size()); ++j)
                    if (Lower(o[j]) == Lower(cur)) i = j;
                if (!o.empty()) SetValue(s, r, o[((i + a_dir) % static_cast<int>(o.size()) + static_cast<int>(o.size())) % static_cast<int>(o.size())]);
                break;
            }
            case RowType::kVk: {
                int i = VkIndex(AsInt(cur));
                i = i < 0 ? 0 : ((i + a_dir) % kVkCount + kVkCount) % kVkCount;
                SetValue(s, r, std::to_string(kVkList[i].vk));
                break;
            }
            }
        }

        void DrawRow(State& s, int a_i, const char* a_labelOverride = nullptr, float a_indent = 0.0f)
        {
            const Row&        r = kRows[a_i];
            const std::string cur = Current(s, r), base = Base(r);
            const bool        pending = s.pending.contains(KeyOf(r));
            const float       W = ImGui::GetContentRegionAvail().x;
            const float       labelW = W * 0.44f, resetW = 110.0f, ctrlW = W - labelW - resetW - 24.0f;
            ImGui::PushID(a_i);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            if (a_indent > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + a_indent);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pending ? kYellow : ImGui::GetStyle().Colors[ImGuiCol_Text], "%s", a_labelOverride ? a_labelOverride : r.label);
            if (!IsLive(r)) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "(restart)");
            }
            ImGui::SameLine(labelW);
            switch (r.type) {
            case RowType::kBool: {
                const bool on = AsInt(cur) != 0;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.18f, 0.5f, 0.25f, 1.0f });
                if (ImGui::Button(on ? "ON" : "OFF", { 140, 0 })) SetValue(s, r, on ? "0" : "1");
                if (on) ImGui::PopStyleColor();
                break;
            }
            case RowType::kInt:
            case RowType::kFloat: {
                if (ImGui::Button("-", { 48, 0 })) StepRow(s, r, -1);
                ImGui::SameLine();
                float v = AsFloat(cur);
                ImGui::SetNextItemWidth(ctrlW - 2 * 48 - 2 * ImGui::GetStyle().ItemSpacing.x);
                char fmt[16];
                std::snprintf(fmt, sizeof(fmt), "%%.%df", Decimals(r));
                if (ImGui::SliderFloat("##v", &v, r.mn, r.mx, fmt, ImGuiSliderFlags_AlwaysClamp)) {
                    if (r.step > 0) v = std::round(v / r.step) * r.step;
                    SetValue(s, r, Fmt(r, v));
                }
                ImGui::SameLine();
                if (ImGui::Button("+", { 48, 0 })) StepRow(s, r, +1);
                break;
            }
            case RowType::kEnum:
            case RowType::kPolicy:
            case RowType::kVk: {
                const auto opts = Options(r);
                int        ix = 0, n = static_cast<int>(opts.size());
                std::string shown;
                if (r.type == RowType::kEnum) {
                    ix = std::clamp(AsInt(cur), 0, (std::max)(n - 1, 0));
                    shown = n ? opts[ix] : cur;
                } else if (r.type == RowType::kPolicy) {
                    for (int j = 0; j < n; ++j)
                        if (Lower(opts[j]) == Lower(cur)) ix = j;
                    shown = cur;
                } else {
                    const int vk = AsInt(cur);
                    ix = VkIndex(vk);
                    shown = ix >= 0 ? kVkList[ix].name : std::format("VK 0x{:X}", vk);
                    if (ix < 0) ix = 0;
                }
                const int before = ix;
                if (n > 0 && ArrowsControl(std::format("{}##e", shown).c_str(), ctrlW - 2 * 48 - 2 * ImGui::GetStyle().ItemSpacing.x, ix, n) &&
                    ix != before) {
                    if (r.type == RowType::kEnum) SetValue(s, r, std::to_string(ix));
                    else if (r.type == RowType::kPolicy) SetValue(s, r, opts[ix]);
                    else SetValue(s, r, std::to_string(kVkList[ix].vk));
                }
                break;
            }
            }
            ImGui::SameLine(W - resetW + ImGui::GetStyle().WindowPadding.x * 0.0f);
            if (!SameValue(cur, base)) {
                if (ImGui::Button("Reset", { resetW, 0 })) SetValue(s, r, base);
            } else {
                ImGui::Dummy({ resetW, 1 });
            }
            const ImVec2 rowMax{ rowMin.x + W, ImGui::GetItemRectMax().y };
            if (ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                s.hoverRowNext = a_i;
                if (r.help && r.help[0]) s.help = std::format("{}: {}", a_labelOverride ? a_labelOverride : r.label, r.help);
            }
            ImGui::PopID();
        }

        std::string SlotActionKey(const std::string& a_actionKey)  // "iRUpDownKey" -> "iRUpDownAction"
        {
            for (const char* suf : { "Key", "Mod" }) {
                const std::size_t n = std::strlen(suf);
                if (a_actionKey.size() > n && a_actionKey.compare(a_actionKey.size() - n, n, suf) == 0)
                    return a_actionKey.substr(0, a_actionKey.size() - n) + "Action";
            }
            return {};
        }

        void DrawQuick(State& s)
        {
            const int   st = game::CurrentCameraState();
            const auto  pol = camdir::ViewPolicy(st);
            const char* stName = (st >= 0 && st < 13) ? kStateNames[st] : "?";
            ImGui::SeparatorText("Actions");
            if (ImGui::Button("Recentre view", { 300, 0 })) {
                present::RequestRecenter();
                Toast(s, "Recentred");
            }
            ImGui::SameLine();
            if (ImGui::Button("Cycle camera view", { 300, 0 })) {
                Toast(s, "Camera: " + camdir::Command("cycle"));
            }
            ImGui::SameLine();
            if (ImGui::Button("First / third person", { 300, 0 })) {
                Toast(s, "Camera: " + camdir::Command("pov"));
            }
            ImGui::TextColored(kGrey, "Camera now: %s, view %s%s", stName, camdir::SceneView(st) ? "SCENE" : camdir::PolicyName(pol),
                st == 0 ? "" : "  (Cycle changes this state's view; the Camera page has all states)");
        }

        // ---- Mods Config spec.json pages (SpecPages.h) ----
        int RowIndexOf(const char* a_sec, const char* a_key)
        {
            for (int i = 0; i < kRowCount; ++i)
                if (_stricmp(kRows[i].section, a_sec) == 0 && _stricmp(kRows[i].key, a_key) == 0) return i;
            return -1;
        }
        bool        SpecBool(const std::string& v) { return Lower(v) == "true" || v == "1"; }
        std::string SpecFmt(const specpages::Row& r, float v)
        {
            if (r.type == specpages::Type::kInt) return std::to_string(static_cast<int>(std::lround(v)));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return buf;
        }
        int SpecDecimals(const specpages::Row& r)
        {
            if (r.decimals >= 0) return r.decimals;
            if (r.type == specpages::Type::kInt || r.step >= 1.0f) return 0;
            if (r.step >= 0.1f || r.step <= 0.0f) return r.step <= 0.0f ? 2 : 1;
            if (r.step >= 0.01f) return 2;
            return 3;
        }
        int SpecChoice(int a_mod, const specpages::Row& r)
        {
            const std::string cur = specpages::Current(a_mod, r);
            for (int i = 0; i < static_cast<int>(r.options.size()); ++i) {
                const std::string ov = Lower(r.options[i].value);
                const bool        boolOpt = ov == "true" || ov == "false";
                if (boolOpt ? (ov == "true") == SpecBool(cur) : SameValue(r.options[i].value, cur)) return i;
            }
            return 0;
        }
        void StepSpecRow(int a_mod, const specpages::Row& r, int a_dir)
        {
            const std::string cur = specpages::Current(a_mod, r);
            switch (r.type) {
            case specpages::Type::kBool: specpages::Set(a_mod, r, SpecBool(cur) ? "false" : "true"); break;
            case specpages::Type::kInt:
            case specpages::Type::kFloat: {
                const float st = r.step > 0 ? r.step : (r.mx - r.mn) / 100.0f;
                float       v = std::clamp(AsFloat(cur) + a_dir * st, r.mn, r.mx);
                if (st > 0) v = std::round(v / st) * st;
                specpages::Set(a_mod, r, SpecFmt(r, v));
                break;
            }
            case specpages::Type::kChoice: {
                const int n = static_cast<int>(r.options.size());
                if (n > 0) specpages::Set(a_mod, r, r.options[((SpecChoice(a_mod, r) + a_dir) % n + n) % n].value);
                break;
            }
            default: break;
            }
        }

        void DrawSpecRow(State& s, int a_mod, const specpages::Row& r, bool a_enabled)
        {
            const bool        action = r.type == specpages::Type::kAction;
            const std::string cur = action ? std::string() : specpages::Current(a_mod, r);
            const bool        pending = !action && specpages::IsPending(r);
            const float       W = ImGui::GetContentRegionAvail().x;
            const float       labelW = W * 0.44f, resetW = 130.0f, ctrlW = W - labelW - resetW - 24.0f;
            ImGui::PushID(r.id.c_str());
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(!a_enabled || action);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pending ? kYellow : ImGui::GetStyle().Colors[ImGuiCol_Text], "%s%s", r.label.c_str(), r.flatNote.empty() ? "" : " *");
            if (!action && !specpages::Live(a_mod, r)) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, r.restart ? "(restart)" : "(on save)");
            }
            ImGui::SameLine(labelW);
            switch (r.type) {
            case specpages::Type::kBool: {
                const bool on = SpecBool(cur);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.18f, 0.5f, 0.25f, 1.0f });
                if (ImGui::Button(on ? "ON" : "OFF", { 140, 0 })) specpages::Set(a_mod, r, on ? "false" : "true");
                if (on) ImGui::PopStyleColor();
                break;
            }
            case specpages::Type::kInt:
            case specpages::Type::kFloat: {
                if (ImGui::Button("-", { 48, 0 })) StepSpecRow(a_mod, r, -1);
                ImGui::SameLine();
                float v = AsFloat(cur);
                ImGui::SetNextItemWidth(ctrlW - 2 * 48 - 2 * ImGui::GetStyle().ItemSpacing.x);
                char fmt[24];
                std::snprintf(fmt, sizeof(fmt), "%%.%df%s", SpecDecimals(r), r.unit == "%" ? "%%" : r.unit.c_str());
                if (ImGui::SliderFloat("##v", &v, r.mn, r.mx, fmt, ImGuiSliderFlags_AlwaysClamp)) {
                    if (r.step > 0) v = std::round(v / r.step) * r.step;
                    specpages::Set(a_mod, r, SpecFmt(r, v));
                }
                ImGui::SameLine();
                if (ImGui::Button("+", { 48, 0 })) StepSpecRow(a_mod, r, +1);
                break;
            }
            case specpages::Type::kChoice: {
                int       ix = SpecChoice(a_mod, r);
                const int n = static_cast<int>(r.options.size()), before = ix;
                if (n > 0 && ArrowsControl(std::format("{}##e", r.options[ix].text).c_str(), ctrlW - 2 * 48 - 2 * ImGui::GetStyle().ItemSpacing.x, ix, n) &&
                    ix != before) {
                    specpages::Set(a_mod, r, r.options[ix].value);
                }
                break;
            }
            case specpages::Type::kAction:
                ImGui::Button("Open", { 140, 0 });
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "in-world tool: not on flat yet");
                break;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(W - resetW);
            if (!action && a_enabled && !r.def.empty() && !SameValue(cur, r.def) && !(SpecBool(cur) && SpecBool(r.def))) {
                if (ImGui::Button("Default", { resetW, 0 })) specpages::Set(a_mod, r, r.def);
            } else {
                ImGui::Dummy({ resetW, 1 });
            }
            const ImVec2 rowMax{ rowMin.x + W, ImGui::GetItemRectMax().y };
            if (ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                if (!action && a_enabled) {
                    s.hoverRowNext = kSpecHoverBase;
                    s.specModNext = a_mod;
                    s.specRowNext = &r;
                }
                std::string h = r.help;
                if (r.type == specpages::Type::kChoice) {
                    const int ix = SpecChoice(a_mod, r);
                    if (ix < static_cast<int>(r.options.size()) && !r.options[ix].help.empty()) h += (h.empty() ? "" : " ") + r.options[ix].help;
                }
                if (action) h += (h.empty() ? "" : " ") + std::string("Needs FRIK's in-world config UI, which does not render on flat yet.");
                if (!r.flatNote.empty()) h += " * " + r.flatNote;
                s.help = std::format("{}: {}", r.label, h);
            }
            ImGui::PopID();
        }

        void DrawSpecPage(State& s, int a_mod, int a_page)
        {
            const auto& mods = specpages::Mods();
            if (a_mod < 0 || a_mod >= static_cast<int>(mods.size()) || a_page < 0 || a_page >= static_cast<int>(mods[a_mod].pages.size())) return;
            const auto& m = mods[a_mod];
            const auto& pg = m.pages[a_page];
            if (m.isFrik && !frikbridge::ConfigApi()) {
                ImGui::TextColored(kYellow, "FRIK.dll's config table is not there (FRIK.dll not loaded or older): changes are written to FRIK.toml on Save.");
            }
            if (!pg.help.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kGrey, "%s", pg.help.c_str());
                ImGui::PopTextWrapPos();
            }
            std::string group = "\x01";
            bool        open = true;
            for (const auto& r : pg.rows) {
                if (!specpages::Cond(a_mod, r.visible)) continue;
                if (r.group != group) {
                    group = r.group;
                    open = true;
                    if (!group.empty()) {
                        ImGui::Spacing();
                        if (r.collapsible) {
                            open = ImGui::CollapsingHeader(std::format("{}##{}", group, pg.name).c_str());
                        } else {
                            ImGui::SeparatorText(group.c_str());
                        }
                    }
                }
                if (!open) continue;
                DrawSpecRow(s, a_mod, r, specpages::Cond(a_mod, r.enabled));
            }
        }

        // a mod's pages that have a row visible on flat (e.g. FRIK's Fallout London VR page only with that .esp)
        std::vector<int> VisiblePages(int a_mod)
        {
            std::vector<int> v;
            const auto&      pages = specpages::Mods()[a_mod].pages;
            for (int i = 0; i < static_cast<int>(pages.size()); ++i)
                for (const auto& r : pages[i].rows)
                    if (specpages::Cond(a_mod, r.visible)) {
                        v.push_back(i);
                        break;
                    }
            return v;
        }

        std::vector<int> OtherMods()  // spec mods shown under "Mods" (all but FRIK)
        {
            std::vector<int> v;
            const auto&      mods = specpages::Mods();
            for (int i = 0; i < static_cast<int>(mods.size()); ++i)
                if (!mods[i].isFrik) v.push_back(i);
            return v;
        }

        void SubTabs(const std::vector<std::string>& a_names, int& a_sel, const char* a_id)
        {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::PushID(a_id);
            const int   n = static_cast<int>(a_names.size());
            const float w = (std::min)(260.0f, (io.DisplaySize.x - 44.0f - 6.0f * (n - 1)) / (std::max)(n, 1));
            for (int i = 0; i < n; ++i) {
                if (i) ImGui::SameLine(0, 6);
                const bool sel = i == a_sel;
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.18f, 0.5f, 0.25f, 1.0f });
                if (ImGui::Button(std::format("{}##{}", a_names[i], i).c_str(), { w, 44 })) a_sel = i;
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }

        // sub-tabs under the page tabs: FRIK (fo4flatvr's rows + FRIK's spec pages) and Mods (other specs)
        void DrawSubTabs(State& s)
        {
            if (s.page < kPageCount && std::strcmp(kPages[s.page], "FRIK") == 0) {
                std::vector<std::string> names{ "fo4flatvr" };
                if (const int fi = specpages::FrikIndex(); fi >= 0)
                    for (const int pi : VisiblePages(fi)) names.push_back(specpages::Mods()[fi].pages[pi].name);
                s.frikPage = std::clamp(s.frikPage, 0, static_cast<int>(names.size()) - 1);
                SubTabs(names, s.frikPage, "frik");
            } else if (s.page == kPageCount) {
                const auto mods = OtherMods();
                if (mods.empty()) return;
                std::vector<std::string> names;
                for (const int i : mods) names.push_back(specpages::Mods()[i].display);
                s.modSel = std::clamp(s.modSel, 0, static_cast<int>(names.size()) - 1);
                const int before = s.modSel;
                SubTabs(names, s.modSel, "mods");
                if (s.modSel != before) s.modPage = 0;
                std::vector<std::string> pages;
                for (const int pi : VisiblePages(mods[s.modSel])) pages.push_back(specpages::Mods()[mods[s.modSel]].pages[pi].name);
                if (!pages.empty()) {
                    s.modPage = std::clamp(s.modPage, 0, static_cast<int>(pages.size()) - 1);
                    SubTabs(pages, s.modPage, "modpages");
                }
            }
        }

        void DrawPage(State& s)
        {
            if (s.page == kPageCount) {  // Mods
                const auto mods = OtherMods();
                if (mods.empty()) {
                    ImGui::TextColored(kGrey, "No other mod ships a Mods Config spec (Data\\ModsConfigFO4\\Config\\<mod>\\spec.json).");
                    return;
                }
                const int  mi = mods[std::clamp(s.modSel, 0, static_cast<int>(mods.size()) - 1)];
                const auto vp = VisiblePages(mi);
                if (!vp.empty()) DrawSpecPage(s, mi, vp[std::clamp(s.modPage, 0, static_cast<int>(vp.size()) - 1)]);
                return;
            }
            if (std::strcmp(kPages[s.page], "FRIK") == 0 && s.frikPage > 0 && specpages::FrikIndex() >= 0) {
                const int   fi = specpages::FrikIndex();
                const auto  vp = VisiblePages(fi);
                const int   pi = vp.empty() ? 0 : vp[std::clamp(s.frikPage - 1, 0, static_cast<int>(vp.size()) - 1)];
                const auto& name = specpages::Mods()[fi].pages[pi].name;
                // fo4flatvr's own rows that belong with FRIK's page
                for (const auto& [sec, key, onPage] : { std::tuple{ "General", "bNoScaling", "Body" }, { "FRIK", "bFrikPipboy", "Pip-Boy" },
                         { "FRIK", "bSmoothMovement", "Smooth movement" }, { "FRIK", "bSmoothHorizontal", "Smooth movement" } }) {
                    if (name == onPage)
                        if (const int i = RowIndexOf(sec, key); i >= 0) DrawRow(s, i);
                }
                DrawSpecPage(s, fi, pi);
                return;
            }
            const char* page = kPages[s.page];
            if (s.page == 0) DrawQuick(s);
            std::string group;
            std::string handHeader;
            bool        handOpen = true;
            for (int i = 0; i < kRowCount; ++i) {
                const Row& r = kRows[i];
                if (std::strcmp(r.page, page) != 0) continue;
                const std::string g = r.group;
                const bool slot = g.rfind("Right stick:", 0) == 0 || g.rfind("Left stick:", 0) == 0;
                if (slot) {
                    const std::string hh = g.rfind("Right", 0) == 0 ? "Right stick gestures" : "Left stick gestures";
                    if (hh != handHeader) {
                        handHeader = hh;
                        ImGui::Spacing();
                        handOpen = ImGui::CollapsingHeader(hh.c_str(), hh[0] == 'R' ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                    }
                    group = g;
                    if (!handOpen) continue;
                    const std::string key = r.key;
                    const std::string slotLabel = g.substr(g.find(':') + 2);
                    if (key.size() > 6 && key.compare(key.size() - 6, 6, "Action") == 0) {
                        DrawRow(s, i, slotLabel.c_str());
                    } else {
                        std::string a;
                        if (!cfgstore::EffectiveValue(r.section, SlotActionKey(key), a)) {
                            for (const auto& rr : kRows)
                                if (std::string(rr.key) == SlotActionKey(key)) a = rr.def;
                        }
                        if (const auto it = s.pending.find(Lower(r.section) + "\n" + Lower(SlotActionKey(key))); it != s.pending.end()) a = it->second;
                        if (AsInt(a) == 1) DrawRow(s, i, nullptr, 40.0f);
                    }
                    continue;
                }
                if (g != group) {
                    group = g;
                    handHeader.clear();
                    ImGui::Spacing();
                    ImGui::SeparatorText(g.c_str());
                }
                DrawRow(s, i);
            }
        }

        void Draw(State& s)
        {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("fo4flatvr settings", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoScrollWithMouse);
            s.help.clear();
            s.hoverRowNext = -1;
            s.specModNext = -1;
            s.specRowNext = nullptr;
            // title
            ImGui::PushFont(nullptr, 38.0f);
            ImGui::TextUnformatted("VR SETTINGS");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "fo4flatvr");
            ImGui::SameLine(io.DisplaySize.x - 190.0f);
            if (ImGui::Button("Close", { 150, 0 })) RequestClose(s, "close button");
            // tabs (+ "Mods" when another mod ships a Mods Config spec)
            const int tabs = kPageCount + (OtherMods().empty() ? 0 : 1);
            if (s.page >= tabs) s.page = 0;
            for (int p = 0; p < tabs; ++p) {
                if (p) ImGui::SameLine(0, 6);
                const bool sel = p == s.page;
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.26f, 0.65f, 0.33f, 1.0f }), ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.02f, 0.08f, 0.03f, 1.0f });
                if (ImGui::Button(p < kPageCount ? kPages[p] : "Mods", { (io.DisplaySize.x - 44.0f - 6.0f * (tabs - 1)) / tabs, 52 })) {
                    s.page = p;
                }
                if (sel) ImGui::PopStyleColor(2);
            }
            DrawSubTabs(s);
            ImGui::Separator();
            // rows
            const float footerH = 128.0f;
            ImGui::BeginChild("rows", { 0, -footerH }, ImGuiChildFlags_None, ImGuiWindowFlags_None);
            DrawPage(s);
            ImGui::EndChild();
            // footer
            ImGui::Separator();
            {
                const int  restart = RestartCount(s) + specpages::PendingRestartCount();
                const bool mcm = cfgstore::McmLoaded();
                const auto total = PendingCount(s);
                std::string line;
                if (!s.toast.empty() && Clock::now() - s.toastAt < std::chrono::seconds(6)) {
                    line = s.toast;
                } else if (!s.help.empty()) {
                    line = s.help;
                } else {
                    line = std::format("{} unsaved{}. Saves go to {} (FRIK: FRIK.toml). Trigger = click, stick = scroll / change, B = close.", total,
                        restart ? std::format(", {} need a restart", restart) : "", mcm ? "MCM" : "the MCM settings file");
                }
                ImGui::PushTextWrapPos(io.DisplaySize.x - 520.0f);
                ImGui::TextColored(total == 0 ? ImGui::GetStyle().Colors[ImGuiCol_Text] : kYellow, "%s", line.c_str());
                ImGui::PopTextWrapPos();
                ImGui::SameLine(io.DisplaySize.x - 500.0f);
                ImGui::BeginDisabled(total == 0);
                if (ImGui::Button("Save", { 220, 64 })) Save(s);
                ImGui::SameLine();
                if (ImGui::Button("Discard", { 220, 64 })) Discard(s);
                ImGui::EndDisabled();
            }
            // close with unsaved changes
            if (s.confirmClose) {
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                dl->AddRectFilled({ 0, 0 }, io.DisplaySize, IM_COL32(0, 0, 0, 170));
                ImGui::SetNextWindowPos({ io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f }, ImGuiCond_Always, { 0.5f, 0.5f });
                ImGui::SetNextWindowSize({ 820, 0 });
                ImGui::Begin("##confirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextColored(kYellow, "%zu unsaved change%s", PendingCount(s), PendingCount(s) == 1 ? "" : "s");
                ImGui::Spacing();
                if (ImGui::Button("Save and close", { 250, 64 })) {
                    Save(s);
                    Close(s, "save and close");
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard and close", { 270, 64 })) {
                    Discard(s);
                    Close(s, "discard and close");
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep editing", { 230, 64 })) s.confirmClose = false;
                ImGui::End();
            }
            ImGui::End();
            // cursor dot when the beam's dot is off
            if ((s.owner >= 0 || s.simAim) && !beam::DotReplacesCursor(beam::Source::kExternal)) {
                ImGui::GetForegroundDrawList()->AddCircleFilled(io.MousePos, 9.0f, s.clickHeld ? IM_COL32(120, 255, 140, 255) : IM_COL32(255, 230, 90, 255));
            }
        }

        void RenderPanel(State& s, ID3D11DeviceContext* a_ctx, float a_dt)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = (std::max)(a_dt, 1.0f / 500.0f);
            io.DisplaySize = { static_cast<float>(kTexW), static_cast<float>(kTexH) };
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            Draw(s);
            ImGui::Render();
            ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* oldDsv = nullptr;
            a_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
            const float clear[4]{ 0, 0, 0, 0 };
            a_ctx->ClearRenderTargetView(s.rtv, clear);
            a_ctx->OMSetRenderTargets(1, &s.rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            a_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
            for (auto*& v : oldRtv)
                if (v) v->Release();
            if (oldDsv) oldDsv->Release();
            s.rendered = true;
            ++s.frames;
        }

        int PageIndex(const std::string& a_name)
        {
            for (int p = 0; p < kPageCount; ++p)
                if (Lower(kPages[p]) == Lower(a_name)) return p;
            return -1;
        }

        void OpenPanel(State& s, const char* a_why)
        {
            s.open = true;
            s.wantAnchor = true;
            s.confirmClose = false;
            s.armed[0] = s.armed[1] = false;  // a trigger already held stays the game's until released
            ++s.opens;
            REX::INFO("cfgui: open ({})", a_why);
        }
    }

    void Install()
    {
        std::scoped_lock l(g_lock);
        auto& s = g_s;
        if (s.installed) return;
        s.installed = true;
        LoadOwnCfg(s);
        s.cfg.enabled = ::GetPrivateProfileIntA("ConfigUI", "bEnabled", 1, Settings::Path().c_str()) != 0;
        g_enabled = s.cfg.enabled;
        g_menuHoldS = (s.cfg.enabled && s.cfg.menuHold) ? s.cfg.menuHoldS : 0.0f;
        std::set<std::string> seen;
        for (const auto& r : kRows) {
            const auto k = KeyOf(r);
            if (seen.insert(k).second) s.start[k] = Effective(r);
        }
        // 20 Nov (live settings): [ConfigUI] bEnabled switches the panel on / off while the game runs (ImGui starts lazily)
        cfgstore::Subscribe("configui", "ConfigUI/bEnabled", [](const cfgstore::Changes&) {
            const bool on = ::GetPrivateProfileIntA("ConfigUI", "bEnabled", 1, Settings::Path().c_str()) != 0;
            std::scoped_lock l(g_lock);
            auto& st = g_s;
            if (st.cfg.enabled == on) return;
            if (!on && st.open) Close(st, "[ConfigUI] bEnabled=0");
            st.cfg.enabled = on;
            g_enabled = on;
            g_menuHoldS = (on && st.cfg.menuHold) ? st.cfg.menuHoldS : 0.0f;
            REX::INFO("cfgui: VR settings panel {} ([ConfigUI] bEnabled, live)", on ? "on" : "OFF");
        });
        REX::INFO("cfgui: VR settings panel {} ([ConfigUI] bEnabled): {} rows; open with the gesture action, the left menu button held "
                  "{:.1f} s{}, or {}+VK 0x{:X}",
            s.cfg.enabled ? "on" : "OFF", kRowCount, s.cfg.menuHoldS, s.cfg.menuHold ? "" : " (off)",
            s.cfg.keyMod == 1 ? "Shift" : s.cfg.keyMod == 2 ? "Ctrl" : s.cfg.keyMod == 3 ? "Alt" : "no modifier", s.cfg.keyVk);
    }

    bool  Enabled() { return g_enabled.load(); }
    float MenuHoldSeconds() { return g_menuHoldS.load(); }
    bool  OwnsPointer() { return g_ownsPointer.load(); }

    void RequestToggle(const char* a_why)
    {
        if (!g_enabled) return;
        g_toggleReq.fetch_add(1);
        REX::INFO("cfgui: toggle requested ({})", a_why ? a_why : "?");
    }

    bool IsOpen()
    {
        std::scoped_lock l(g_lock);
        return g_s.open;
    }

    bool HotkeyClaims(int a_vk)
    {
        if (!g_enabled) return false;
        std::scoped_lock l(g_lock);
        const auto& c = g_s.cfg;
        if (a_vk == 0 || a_vk != c.keyVk) return false;
        const int mod = c.keyMod == 1 ? VK_SHIFT : c.keyMod == 2 ? VK_CONTROL : c.keyMod == 3 ? VK_MENU : 0;
        return mod == 0 || (::GetAsyncKeyState(mod) & 0x8000) != 0;
    }

    void Tick()
    {
        if (!g_enabled) {
            cfgstore::Poll();  // still keep the merged file current (MCM / editor changes)
            cfgstore::DispatchChanges();  // 20 Nov: modules re-read changed keys (live settings), also with the panel off
            return;
        }
        {
            std::scoped_lock l(g_lock);
            auto& s = g_s;
            const bool changed = cfgstore::Poll();
            if (!s.appliedInit) {
                s.appliedInit = true;
                ReapplyChanged(s);  // records the values in effect, applies nothing
            } else if (changed) {
                ReapplyChanged(s);
            }
            Flush(s, false);
            if (s.open || specpages::PendingCount() != 0) {
                specpages::Tick();  // saved values 1 Hz, FRIK previews 10 Hz
            }
        }
        // 20 Nov (live settings): every module subscribed to a changed key re-reads it (ConfigStore.h); outside our lock, a
        // reload may call back into the panel (bEnabled)
        cfgstore::DispatchChanges();
    }

    void UpdateFrame(xr::Runtime& a_rt, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, bool a_began, const xr::Pose* a_head, bool a_focused)
    {
        if (!g_enabled) return;
        std::scoped_lock l(g_lock);
        auto&      s = g_s;
        const auto now = Clock::now();
        const float dt = s.lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - s.lastFrame).count() : 1.0f / 72.0f;
        s.lastFrame = now;
        // hotkey (game window focused)
        {
            const int  mod = s.cfg.keyMod == 1 ? VK_SHIFT : s.cfg.keyMod == 2 ? VK_CONTROL : s.cfg.keyMod == 3 ? VK_MENU : 0;
            const bool down = a_focused && s.cfg.keyVk != 0 && (::GetAsyncKeyState(s.cfg.keyVk) & 0x8000) != 0 &&
                              (mod == 0 || (::GetAsyncKeyState(mod) & 0x8000) != 0);
            if (down && !s.keyWas) g_toggleReq.fetch_add(1);
            s.keyWas = down;
        }
        for (int n = g_toggleReq.exchange(0); n > 0; --n) {
            if (s.open) {
                RequestClose(s, "toggle");
            } else {
                OpenPanel(s, "toggle");
            }
        }
        if (!s.open) {
            g_ownsPointer = false;
            g_maskHand = -1;
            return;
        }
        if (s.wantAnchor && a_head) {
            Anchor(s, *a_head);
            s.wantAnchor = false;
        }
        if (!InitImGui(s, a_dev, a_ctx)) {
            Close(s, "no renderer");
            return;
        }
        // ---- pointer ----
        const auto& c = a_rt.Controllers();
        const bool  active = a_began && c.active && !s.wantAnchor;
        const float trig[2]{ c.leftTrigger, c.rightTrigger };
        for (int h = 0; h < 2; ++h) {
            const bool valid = active && (h ? c.rightAimValid : c.leftAimValid);
            s.hits[h] = valid ? HitTest(s, h ? c.rightAim : c.leftAim, s.clickHeld && s.owner == h) : HandHit{};
            s.aimValid[h] = valid;
            if (valid) {
                const xr::Pose& a = h ? c.rightAim : c.leftAim;
                s.aimPos[h] = a.position;
                s.aimDir[h] = Rot(a.orientation, { 0, 0, -1 });
            }
        }
        int owner = -1;
        if (s.clickHeld && s.owner >= 0 && s.hits[s.owner].hit) {
            owner = s.owner;
        } else {
            const int prim = beam::Primary();
            if (s.hits[prim].hit) owner = prim;
            else if (s.hits[1 - prim].hit) owner = 1 - prim;
        }
        if (owner != s.owner && s.owner >= 0) {
            s.clickHeld = false;
        }
        s.owner = owner;
        ImGuiIO& io = ImGui::GetIO();
        bool     down = false;
        if (s.simAim) {
            io.AddMousePosEvent(s.simU * kTexW, s.simV * kTexH);
            down = s.simDown;
        } else if (owner >= 0) {
            const auto& hh = s.hits[owner];
            io.AddMousePosEvent(std::clamp(hh.u, 0.0f, 1.0f) * kTexW, std::clamp(hh.v, 0.0f, 1.0f) * kTexH);
            const float t = trig[owner];
            if (t < 0.35f) s.armed[owner] = true;
            down = s.armed[owner] && (s.clickHeld ? t > 0.35f : t > 0.6f);
            // stick: scroll (Y), change the row under the ray (X)
            const float sx = owner ? c.rightStickX : c.leftStickX, sy = owner ? c.rightStickY : c.leftStickY;
            if (std::abs(sy) > 0.25f && std::abs(sy) > std::abs(sx)) io.AddMouseWheelEvent(0.0f, sy * dt * 5.0f);
            const int dir = sx > 0.6f ? 1 : sx < -0.6f ? -1 : 0;
            if (dir == 0) {
                s.stepDir = 0;
            } else if (s.hoverRow >= 0 && (dir != s.stepDir || now >= s.nextStep)) {
                s.nextStep = now + std::chrono::milliseconds(dir != s.stepDir ? 450 : 150);
                s.stepDir = dir;
                if (s.hoverRow == kSpecHoverBase) {
                    if (s.specRow) StepSpecRow(s.specMod, *s.specRow, dir);
                } else {
                    StepRow(s, kRows[s.hoverRow], dir);
                }
                Pulse(s, owner, 0.2f, 0.015f);
            }
            // B (right) / Y (left): close
            const bool closeBtn = owner ? c.b : c.y;
            if (closeBtn && !s.closeBtnWas[owner]) {
                if (s.confirmClose) s.confirmClose = false;
                else RequestClose(s, "B / Y");
            }
            s.closeBtnWas[owner] = closeBtn;
        } else {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            s.stepDir = 0;
        }
        if (down != s.clickHeld) {
            io.AddMouseButtonEvent(0, down);
            if (down) {
                ++s.clicks;
                Pulse(s, owner, 0.35f, 0.025f);
            }
            s.clickHeld = down;
        }
        g_ownsPointer = owner >= 0 || s.clickHeld || s.simAim;
        g_maskHand = owner;
        if (owner >= 0) {
            beam::Ray ray;
            ray.src = beam::Source::kExternal;
            ray.hit = true;
            ray.length = s.hits[owner].dist;
            ray.pressed = s.clickHeld;
            beam::Offer(owner, ray);
        }
        // ---- frame ----
        RenderPanel(s, a_ctx, dt);
        s.hoverRow = s.hoverRowNext;
        s.specMod = s.specModNext;
        s.specRow = s.specRowNext;
        if (const ImGuiID hid = ImGui::GetHoveredID(); hid != s.lastHoverId) {
            if (hid != 0) Pulse(s, owner, 0.12f, 0.012f);
            s.lastHoverId = hid;
        }
        Flush(s, false);
    }

    void MaskControllers(xr::ControllerState& a_c)
    {
        const int h = g_maskHand.load();
        if (h == 1) {
            a_c.rightTrigger = 0.0f;
            a_c.rightStickX = a_c.rightStickY = 0.0f;
            a_c.a = a_c.b = false;
        } else if (h == 0) {
            a_c.leftTrigger = 0.0f;
            a_c.leftStickX = a_c.leftStickY = 0.0f;
            a_c.x = a_c.y = false;
        }
    }

    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx)
    {
        if (!g_enabled) return;
        std::scoped_lock l(g_lock);
        auto& s = g_s;
        if (!s.open || !s.rendered || !s.tex || s.wantAnchor) return;
        if (a_rt.SubmitWorldQuad(a_ctx, s.tex, s.pose, s.widthM, true, true, xr::Runtime::kTopWorldQuad)) {
            ++s.submits;
        }
    }

    std::string Command(const std::string& a_arg)
    {
        std::scoped_lock l(g_lock);
        auto& s = g_s;
        if (a_arg == "store") return cfgstore::Status();
        if (!g_enabled && a_arg != "" && a_arg != "status") return "error: [ConfigUI] bEnabled=0";
        if (a_arg == "open") {
            if (!s.open) OpenPanel(s, "pipe");
            return "open";
        }
        if (a_arg == "close") {
            if (s.open) {
                if (PendingCount(s) != 0) Discard(s);
                Close(s, "pipe");
            }
            return "closed";
        }
        if (a_arg == "toggle") {
            g_toggleReq.fetch_add(1);
            return "toggle queued";
        }
        if (a_arg.rfind("page ", 0) == 0) {
            const int p = Lower(a_arg.substr(5)) == "mods" ? kPageCount : PageIndex(a_arg.substr(5));
            if (p < 0) return "error: page Quick|Camera|Comfort|Render|Height|Gestures|Selfie|FRIK|Keys|Mods";
            s.page = p;
            if (p == kPageCount) return "page Mods";
            s.page = p;
            return std::format("page {}", kPages[p]);
        }
        if (a_arg.rfind("set ", 0) == 0 || a_arg.rfind("reset ", 0) == 0) {
            const bool reset = a_arg[0] == 'r';
            char       sec[64]{}, key[64]{}, val[128]{};
            const int  n = sscanf_s(a_arg.c_str() + (reset ? 6 : 4), "%63s %63s %127s", sec, static_cast<unsigned>(sizeof(sec)), key,
                 static_cast<unsigned>(sizeof(key)), val, static_cast<unsigned>(sizeof(val)));
            if (n < (reset ? 2 : 3)) return "error: cfgui set <Section> <Key> <value> | reset <Section> <Key>";
            const Row* r = FirstRow(Lower(sec) + "\n" + Lower(key));
            if (!r) return std::format("error: {}:{} is not a panel row", key, sec);
            SetValue(s, *r, reset ? Base(*r) : std::string(val));
            Flush(s, true);
            return std::format("{}:{} current={} saved={} base={} pending={} lastApply={}", r->key, r->section, Current(s, *r), Saved(*r), Base(*r),
                s.pending.size(), s.lastApply);
        }
        if (a_arg == "save") {
            Save(s);
            return s.toast;
        }
        if (a_arg == "spec" || a_arg.rfind("spec ", 0) == 0) {  // Mods Config spec pages: spec [status|reload|rows <mod> [page]|get|set ...]
            return specpages::Command(a_arg.size() > 5 ? a_arg.substr(5) : std::string());
        }
        if (a_arg.rfind("frikpage ", 0) == 0) {
            s.page = PageIndex("FRIK");
            s.frikPage = std::atoi(a_arg.c_str() + 9);
            return std::format("FRIK sub-page {}", s.frikPage);
        }
        if (a_arg == "discard") {
            Discard(s);
            return s.toast;
        }
        if (a_arg == "rows") {
            std::string out = std::format("page {}:", kPages[s.page]);
            for (const auto& r : kRows) {
                if (std::strcmp(r.page, kPages[s.page]) != 0) continue;
                out += std::format(" | {}:{}={}{} (saved {}, base {}{})", r.key, r.section, Current(s, r), s.pending.contains(KeyOf(r)) ? "*" : "",
                    Saved(r), Base(r), IsLive(r) ? "" : ", restart");
            }
            return out;
        }
        if (a_arg.rfind("aim ", 0) == 0) {
            if (a_arg == "aim off") {
                s.simAim = false;
                s.simDown = false;
                return "aim off";
            }
            float u = 0, v = 0;
            char  st[16]{};
            const int n = sscanf_s(a_arg.c_str() + 4, "%f %f %15s", &u, &v, st, static_cast<unsigned>(sizeof(st)));
            if (n < 2) return "error: cfgui aim <u> <v> [down|up] | aim off";
            s.simAim = true, s.simU = u, s.simV = v;
            if (n == 3) s.simDown = std::strcmp(st, "down") == 0;
            return std::format("aim {:.3f} {:.3f} {}", u, v, s.simDown ? "down" : "up");
        }
        if (!a_arg.empty() && a_arg != "status" && a_arg != "ray") {
            return "error: cfgui [status|ray|open|close|toggle|page <name>|frikpage <n>|set <S> <K> <v>|reset <S> <K>|save|discard|rows|spec ...|aim <u> <v> [down|up]|aim off|store]";
        }
        const int restart = RestartCount(s);
        if (a_arg == "ray") {
            std::string out = std::format("panel={:.3f},{:.3f},{:.3f} yawq={:.3f},{:.3f} width={:.2f}", s.pose.position.x, s.pose.position.y,
                s.pose.position.z, s.pose.orientation.y, s.pose.orientation.w, s.widthM);
            for (int h = 0; h < 2; ++h) {
                out += std::format(" | {} valid={} pos={:.3f},{:.3f},{:.3f} dir={:.3f},{:.3f},{:.3f} hit={} u={:.3f} v={:.3f} d={:.3f}", h ? "right" : "left",
                    s.aimValid[h] ? 1 : 0, s.aimPos[h].x, s.aimPos[h].y, s.aimPos[h].z, s.aimDir[h].x, s.aimDir[h].y, s.aimDir[h].z,
                    s.hits[h].hit ? 1 : 0, s.hits[h].u, s.hits[h].v, s.hits[h].dist);
            }
            return out;
        }
        return std::format("enabled={} open={} page={} anchored={} owner={} click={} hoverRow={} pending={} restartNeeded={} frames={} submits={} "
                           "opens={} clicks={} applies={} saves={} imgui={} menuHoldS={:.2f} key=0x{:X}/mod{} mcm={} lastApply={} toast='{}' | store: {}",
            g_enabled.load() ? 1 : 0, s.open ? 1 : 0, kPages[s.page], s.wantAnchor ? 0 : 1, s.owner, s.clickHeld ? 1 : 0,
            s.hoverRow == kSpecHoverBase && s.specRow ? s.specRow->key.c_str() : s.hoverRow >= 0 && s.hoverRow < kRowCount ? kRows[s.hoverRow].key : "-", s.pending.size(), restart, s.frames, s.submits, s.opens, s.clicks, s.applies, s.saves,
            s.imguiOk ? "ok" : s.imguiFailed ? "FAILED" : "not yet", g_menuHoldS.load(), s.cfg.keyVk, s.cfg.keyMod,
            cfgstore::McmLoaded() ? 1 : 0, s.lastApply, s.toast, cfgstore::Status());
    }
}
