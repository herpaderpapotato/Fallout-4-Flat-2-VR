// Scopes, game side (Scope.h): the drawn weapon's scope data and the first-person WEAPON node cull, on the main thread
// (main tick after the main queue, ThreadQueues.h). Split from Scope.cpp: no translation unit here mixes RE/Fallout.h with
// the D3D11 headers.
#include <REX/LOG.h>
#include <RE/Fallout.h>

#include "plugin/EngineCull.h"
#include "plugin/Scope.h"

#include <atomic>
#include <cstring>
#include <cmath>
#include <format>
#include <mutex>

namespace fo4flatvr::scope::detail
{
    namespace
    {
        std::mutex        g_lock;
        WeaponInfo        g_info;
        std::atomic<bool> g_wantCull{ false };
        bool              g_culled = false;          // main thread: we culled g_node
        RE::NiAVObject*   g_node = nullptr;          // the WEAPON node we culled (pointer kept only to un-cull the same node)
        void*             g_nodeRoot = nullptr;      // its first-person root at the time
        std::uint64_t     g_culls = 0, g_uncullls = 0, g_ticks = 0;
        std::string       g_lastErr;
        std::string       g_menuInfo;               // ScopeMenu renderer / flags / depth, the last time it was open

        float DefaultFov()
        {
            static float s_v = 0.0f;
            if (s_v > 1.0f) {
                return s_v;
            }
            if (auto* s = RE::GetINISetting("fDefaultWorldFOV:Display"); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
                const float v = s->GetFloat();
                if (std::isfinite(v) && v > 20.0f && v < 170.0f) {
                    s_v = v;
                    REX::INFO("scope: fDefaultWorldFOV:Display = {:.1f}", v);
                }
            }
            return s_v;
        }

        // FO4VR FUN_140f0b220: the scope point's offset by the weapon's Pip-Boy icon id (VR PipboyInventoryData::GetItemIconID
        // C0C4A0 with no stack; flat 1.11 B41520, prologue byte-checked). Default fWeaponScopeOffset (0, 0, 8).
        int IconId(RE::TESForm* a_form)
        {
            static int s_ok = 0;  // 0 unchecked, 1 ok, -1 bytes differ
            constexpr std::uintptr_t kGetItemIconId = 0xB41520;
            if (s_ok == 0) {
                static constexpr std::uint8_t kPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9 };
                s_ok = std::memcmp(reinterpret_cast<const void*>(REL::Offset(kGetItemIconId).address()), kPro, sizeof(kPro)) == 0 ? 1 : -1;
                if (s_ok < 0) REX::ERROR("scope: GetItemIconID bytes differ at rva {:X}; scope point = FO4VR default (0, 0, 8)", kGetItemIconId);
            }
            if (s_ok < 0 || !a_form) return -1;
            using Fn = std::uint32_t (*)(RE::TESForm*, void*);
            return static_cast<int>(reinterpret_cast<Fn>(REL::Offset(kGetItemIconId).address())(a_form, nullptr));
        }

        void ScopeOffset(int a_icon, float& a_x, float& a_z)
        {
            a_x = 0.0f, a_z = 8.0f;
            switch (a_icon) {
            case 2: a_z = 5.0f; break;
            case 3: case 10: case 0x1b: a_z = 7.0f; break;
            case 4: case 0xb: case 0x18: a_z = 8.0f; break;
            case 5: case 6: a_z = 4.0f; break;
            case 7: a_z = 9.0f; break;
            case 8: case 9: a_z = 10.0f; break;
            case 0xf: a_x = -8.5f, a_z = 14.5f; break;
            default: break;
            }
        }

        void ReadWeapon(RE::PlayerCharacter* a_p, WeaponInfo& a_out)
        {
            a_out.defaultFov = DefaultFov();
            if (!a_p || !a_p->GetWeaponMagicDrawn()) {
                return;
            }
            auto* mh = a_p->currentProcess ? a_p->currentProcess->middleHigh : nullptr;
            if (!mh) {
                return;
            }
            for (auto& it : mh->equippedItems) {
                auto* obj = it.item.object;
                if (!obj || !obj->IsWeapon()) {
                    continue;
                }
                auto*       inst = static_cast<RE::TESObjectWEAP::InstanceData*>(it.item.instanceData.get());
                auto*       base = static_cast<RE::TESObjectWEAP*>(obj);
                const auto* d = inst ? inst : static_cast<RE::TESObjectWEAP::InstanceData*>(&base->weaponData);
                const auto  flags = d->flags.underlying();
                a_out.form = obj->GetFormID();
                {
                    static std::uint32_t s_form = 0;  // main thread: the icon lookup only when the weapon changes
                    static int           s_icon = -1;
                    if (s_form != a_out.form) {
                        s_form = a_out.form;
                        s_icon = IconId(obj);
                        float x, z;
                        ScopeOffset(s_icon, x, z);
                        REX::INFO("scope: weapon {:08X} icon id {} -> scope point offset ({:.1f}, 0, {:.1f}) (FO4VR FUN_140f0b220)", s_form, s_icon, x, z);
                    }
                    a_out.iconId = s_icon;
                    ScopeOffset(s_icon, a_out.scopeOffX, a_out.scopeOffZ);
                }
                a_out.scopedGun = d->type.get() == RE::WEAPON_TYPE::kGun && (flags & 0x00200000u) != 0;  // FO4VR EF7B00: type 9 + bit 21
                if (d->zoomData) {
                    const float m = d->zoomData->zoomData.fovMult;
                    a_out.rawFovMult = m;
                    // measured 27 Nov (CompMcCreadyHuntingRifle, short scope): fovMult 4.0 and, while scoped, PlayerCamera
                    // fovAdjustTarget -56.31 = B / 4 - B with B = 75.08 (the game's own world FOV): the zoomed FOV = B / fovMult
                    a_out.fovMult = (std::isfinite(m) && m >= 1.0f && m <= 40.0f) ? m : 0.0f;
                    if (a_out.fovMult > 1.01f) {
                        static float s_base = 0.0f;  // main thread
                        auto* cam = RE::PlayerCamera::GetSingleton();
                        if (cam && cam->fovAdjustTarget < -0.5f && a_p->currentProcess) {
                            const float b = -cam->fovAdjustTarget * a_out.fovMult / (a_out.fovMult - 1.0f);
                            if (std::isfinite(b) && b > 30.0f && b < 170.0f) {
                                if (std::abs(b - s_base) > 0.5f) REX::INFO("scope: the game's ADS base FOV {:.2f} (from fovAdjustTarget {:.2f}, zoom {:.2f})", b, cam->fovAdjustTarget, a_out.fovMult);
                                s_base = b;
                            }
                        }
                        const float adsBase = s_base > 1.0f ? s_base : (a_out.defaultFov > 1.0f ? a_out.defaultFov : 70.0f);
                        a_out.zoomHDeg = adsBase / a_out.fovMult;
                    }
                }
                return;  // the first weapon (the gun hand's)
            }
        }

        void ApplyCull(RE::PlayerCharacter* a_p)
        {
            const bool want = g_wantCull.load();
            RE::NiAVObject* root = a_p ? a_p->Get3D(true) : nullptr;
            if (g_culled && (g_nodeRoot != root || !want)) {
                // un-cull the node we culled, if it still belongs to the same first-person 3D
                if (g_nodeRoot == root && g_node) {
                    enginecull::Set(g_node, false);
                }
                ++g_uncullls;
                g_culled = false;
                g_node = nullptr;
                g_nodeRoot = nullptr;
            }
            if (want && !g_culled && root) {
                static const RE::BSFixedString kWeapon("WEAPON");
                auto* w = root->GetObjectByName(kWeapon);
                if (!w) {
                    g_lastErr = "no WEAPON node in the first-person 3D";
                    return;
                }
                if (w->flags.flags & 1) {
                    return;  // already hidden by someone else: not ours to show again
                }
                enginecull::Set(w, true);
                g_culled = true;
                g_node = w;
                g_nodeRoot = root;
                if (++g_culls <= 5) {
                    REX::INFO("scope: first-person WEAPON culled while scoped");
                }
            }
        }
    }

    void MainTick()
    {
        ++g_ticks;
        auto*      p = RE::PlayerCharacter::GetSingleton();
        WeaponInfo w;
        if (p && p->Get3D()) {
            ReadWeapon(p, w);
        }
        {
            std::scoped_lock l(g_lock);
            g_info = w;
        }
        ApplyCull(p);
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen("ScopeMenu")) {
            Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString("ScopeMenu"));
            if (m) {
                const auto* raw = reinterpret_cast<const std::uint8_t*>(m.get());
                auto info = std::format("renderer='{}' flags=0x{:X} depth={} movie={} canBeVisible={} fader={}", m->customRendererName.c_str(),
                    m->menuFlags.underlying(), static_cast<int>(m->depthPriority), m->uiMovie ? 1 : 0, raw[0x61],
                    ui->GetMenuOpen("FaderMenu") ? 1 : 0);
                std::scoped_lock l(g_lock);
                if (info != g_menuInfo) {
                    REX::INFO("scope: ScopeMenu {}", info);
                    g_menuInfo = info;
                }
            }
        }
    }

    WeaponInfo Weapon()
    {
        std::scoped_lock l(g_lock);
        return g_info;
    }

    void SetWantCull(bool a_cull)
    {
        g_wantCull = a_cull;
    }

    std::string GameStatus()
    {
        const WeaponInfo w = Weapon();
        auto* cam = RE::PlayerCamera::GetSingleton();
        return std::format("form={:08X} icon={} scopeOff=({:.1f},{:.1f}) scopedGun={} zoom={:.3f} zoomHDeg={:.2f} rawFovMult={:.4f} camFovAdjustTarget={:.2f} camWorldFov={:.2f} defaultFov={:.1f} cullWanted={} culled={} culls={} uncullls={} ticks={}{}",
            w.form, w.iconId, w.scopeOffX, w.scopeOffZ, w.scopedGun ? 1 : 0, w.fovMult, w.zoomHDeg, w.rawFovMult, cam ? cam->fovAdjustTarget : 0.0f, cam ? cam->worldFOV : 0.0f, w.defaultFov, g_wantCull.load() ? 1 : 0, g_culled ? 1 : 0, g_culls, g_uncullls, g_ticks,
            g_lastErr.empty() ? "" : " cullErr='" + g_lastErr + "'") + " scopeMenu: " + [] { std::scoped_lock l(g_lock); return g_menuInfo; }();
    }
}
