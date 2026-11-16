#include "plugin/ComfortEffects.h"

#include "plugin/Settings.h"

#include <REX/LOG.h>
#include <F4SE/F4SE.h>

#include <Windows.h>
#undef ERROR

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <utility>
#include <string>

namespace fo4flatvr::comfx
{
    namespace
    {
        std::uintptr_t Base() { return REL::Offset(0).address(); }

        std::string Hex(const std::uint8_t* a_p, std::size_t a_n)
        {
            std::string s;
            for (std::size_t i = 0; i < a_n; ++i) {
                s += std::format("{}{:02X}", i ? " " : "", a_p[i]);
            }
            return s;
        }

        bool BytesAt(std::uintptr_t a_rva, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_rva);
            if (std::memcmp(p, a_expect, a_n) == 0) {
                return true;
            }
            REX::ERROR("comfx: {} at {:X} has bytes [{}], expected [{}]; skipped", a_what, a_rva, Hex(p, a_n), Hex(a_expect, a_n));
            return false;
        }

        bool WriteCode(std::uintptr_t a_addr, const void* a_data, std::size_t a_n)
        {
            DWORD old = 0;
            if (!::VirtualProtect(reinterpret_cast<void*>(a_addr), a_n, PAGE_EXECUTE_READWRITE, &old)) {
                REX::ERROR("comfx: VirtualProtect {:X} failed ({})", a_addr - Base(), ::GetLastError());
                return false;
            }
            std::memcpy(reinterpret_cast<void*>(a_addr), a_data, a_n);
            ::VirtualProtect(reinterpret_cast<void*>(a_addr), a_n, old, &old);
            ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(a_addr), a_n);
            return true;
        }

        bool WriteSlot(std::uintptr_t a_slot, std::uintptr_t a_value)
        {
            DWORD old = 0;
            if (!::VirtualProtect(reinterpret_cast<void*>(a_slot), sizeof(a_value), PAGE_READWRITE, &old)) {
                REX::ERROR("comfx: VirtualProtect slot {:X} failed ({})", a_slot - Base(), ::GetLastError());
                return false;
            }
            *reinterpret_cast<std::uintptr_t*>(a_slot) = a_value;
            ::VirtualProtect(reinterpret_cast<void*>(a_slot), sizeof(a_value), old, &old);
            return true;
        }

        std::mutex g_lock;  // install / toggles (load thread, pipe thread)

        // ---- camera shake ------------------------------------------------------------------------------------
        // ShakeCamera(float strength, NiPoint3& origin, float time, float radiusMult) 827370:
        //   sub rsp,68h ; mov rax,[rip+..] ; movaps xmm4,xmm0 ; movaps [rsp+50h],xmm6 ...
        // writes the shake state at 827479 (movss [326D360],xmm1 = strength) and 827483 (movss [326D364],xmm7 = time).
        constexpr std::uintptr_t kShakeCamera = 0x827370;
        constexpr std::uint8_t   kShakePrologue[] = { 0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0x05, 0xF5, 0x5F, 0xA4, 0x02, 0x0F, 0x28, 0xE0, 0x0F, 0x29 };
        constexpr std::uintptr_t kShakeStrengthStore = 0x827479;  // F3 0F 11 0D rel32
        constexpr std::uintptr_t kShakeTimeStore = 0x827483;      // F3 0F 11 3D rel32
        constexpr std::uintptr_t kShakeStrength = 0x326D360;
        constexpr std::uintptr_t kShakeTime = 0x326D364;

        std::atomic<bool> g_noShake{ true };
        int               g_shakeState = 0;  // 0 not checked, 1 verified, -1 bytes mismatch
        bool              g_shakePatched = false;
        std::uint64_t     g_shakeClears = 0;

        bool VerifyShake()
        {
            if (g_shakeState != 0) {
                return g_shakeState > 0;
            }
            bool ok = BytesAt(kShakeCamera, kShakePrologue, sizeof(kShakePrologue), "ShakeCamera prologue");
            auto store = [&](std::uintptr_t a_site, std::uint8_t a_modrm, std::uintptr_t a_target, const char* a_what) {
                const std::uint8_t op[4] = { 0xF3, 0x0F, 0x11, a_modrm };
                if (!BytesAt(a_site, op, 4, a_what)) {
                    return false;
                }
                const auto disp = *reinterpret_cast<const std::int32_t*>(Base() + a_site + 4);
                const auto tgt = a_site + 8 + static_cast<std::intptr_t>(disp);
                if (tgt != a_target) {
                    REX::ERROR("comfx: {} at {:X} stores to {:X}, expected {:X}; skipped", a_what, a_site, tgt, a_target);
                    return false;
                }
                return true;
            };
            ok = ok && store(kShakeStrengthStore, 0x0D, kShakeStrength, "ShakeCamera strength store");
            ok = ok && store(kShakeTimeStore, 0x3D, kShakeTime, "ShakeCamera time store");
            g_shakeState = ok ? 1 : -1;
            return ok;
        }

        float* ShakeStrength() { return reinterpret_cast<float*>(Base() + kShakeStrength); }
        float* ShakeTime() { return reinterpret_cast<float*>(Base() + kShakeTime); }

        void ClearShake(const char* a_when)
        {
            if (g_shakeState <= 0 || !g_noShake.load()) {
                return;
            }
            const float s = *ShakeStrength(), t = *ShakeTime();
            if (s != 0.0f || t != 0.0f) {
                *ShakeStrength() = 0.0f;
                *ShakeTime() = 0.0f;
                if (++g_shakeClears <= 20) {
                    REX::INFO("comfx: camera shake cleared ({}; strength {:.3f} time {:.3f})", a_when, s, t);
                }
            }
        }

        void ApplyShake()
        {
            if (!VerifyShake()) {
                return;
            }
            const bool want = g_noShake.load();
            if (want == g_shakePatched) {
                return;
            }
            const std::uint8_t b = want ? 0xC3 : kShakePrologue[0];  // ret / original first byte (sub rsp,68h)
            if (WriteCode(Base() + kShakeCamera, &b, 1)) {
                g_shakePatched = want;
                REX::INFO("comfx: ShakeCamera {:X} {}", kShakeCamera, want ? "-> ret (no camera shake)" : "restored");
                ClearShake("switched on");
            }
        }

        // ---- image-space effect IsActive wrappers --------------------------------------------------------------
        using IsActive_t = bool (*)(void*);

        struct Fx
        {
            const char*          tag;        // pipe item
            const char*          key;        // ini key ([Comfort])
            const char*          what;       // log name
            std::atomic<bool>*   sw;
            std::uintptr_t       slotRva;    // vtable slot 8 (IsActive)
            std::uintptr_t       fnRva;      // the engine IsActive it must hold
            std::uint8_t         bytes[16];  // function bytes
            std::size_t          n;
            // runtime
            int                  state{ 0 };     // 0 not installed, 1 wrapped, -1 mismatch
            IsActive_t           orig{ nullptr };
            std::atomic<std::uint64_t> suppressed{ 0 };  // calls where the engine said active and we returned false
            std::atomic<std::uint64_t> calls{ 0 };
        };

        std::atomic<bool> g_noRadial{ true }, g_noDouble{ true }, g_noBlur{ true }, g_noDof{ false };

        Fx g_fx[] = {
            { "radial", "bNoRadialBlur", "ImageSpaceEffectRadialBlur::IsActive", &g_noRadial, 0x29177F8, 0x22379E0,
                { 0x80, 0x3D, 0x99, 0xA0, 0xC3, 0x01, 0x00, 0x75, 0x1F, 0x48, 0x8B, 0x05, 0xC0, 0x6A, 0xC2, 0x01 }, 16 },
            { "double", "bNoDoubleVision", "ImageSpaceEffectGetHit::IsActive", &g_noDouble, 0x2917748, 0x2237470,
                { 0x80, 0x3D, 0xFD, 0xA5, 0xC3, 0x01, 0x00, 0x75, 0x16, 0x48, 0x8B, 0x05, 0x30, 0x70, 0xC2, 0x01 }, 16 },
            { "blur", "bNoImodBlur", "ImageSpaceEffectFullScreenBlur::IsActive", &g_noBlur, 0x2917688, 0x2235BB0,
                { 0x80, 0x3D, 0x8C, 0xBE, 0xC3, 0x01, 0x00, 0x75, 0x16, 0x48, 0x8B, 0x05, 0xF0, 0x88, 0xC2, 0x01 }, 16 },
            { "dof", "bNoDepthOfField", "ImageSpaceEffectDepthOfField::IsActive", &g_noDof, 0x291A6E8, 0x22566F0,
                { 0x80, 0x3D, 0xC9, 0x20, 0xD4, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0xB2, 0x7D, 0xC0, 0x01 }, 14 },
            { "dof", "bNoDepthOfField", "ImageSpaceEffectDepthOfFieldSplitScreen::IsActive", &g_noDof, 0x291A750, 0x22579E0,
                { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x8B, 0x05, 0xD1, 0x66, 0xC0, 0x01, 0x0F, 0x57, 0xC0, 0xFF, 0xC8 }, 16 },
            { "dof", "bNoDepthOfField", "ImageSpaceEffectBokehDepthOfField::IsActive", &g_noDof, 0x2916000, 0x2215B90,
                { 0x8B, 0x05, 0x26, 0x85, 0xC4, 0x01, 0x0F, 0x57, 0xC0, 0xFF, 0xC8, 0x83, 0xF8, 0x01 }, 14 },
        };
        constexpr std::size_t kFxCount = sizeof(g_fx) / sizeof(g_fx[0]);

        // Depth of field: the manager's imod-blended DOF strength (ImageSpaceManager +0x10C, read by DOF IsActive at
        // 2256730) is > 0 every frame at rest in Sanctuary (measured 15 Nov), so imod "pulses" cannot be told apart from
        // the world's own look there. The DOF item therefore removes all depth of field and is OFF by default.
        template <std::size_t I>
        bool HkIsActive(void* a_this)
        {
            Fx&        f = g_fx[I];
            const bool active = f.orig(a_this);
            f.calls.fetch_add(1, std::memory_order_relaxed);
            if (active && f.sw->load(std::memory_order_relaxed)) {
                if (f.suppressed.fetch_add(1, std::memory_order_relaxed) == 0) {
                    REX::INFO("comfx: {} suppressed (first time)", f.what);
                }
                return false;
            }
            return active;
        }

        template <std::size_t... I>
        constexpr auto MakeHooks(std::index_sequence<I...>)
        {
            return std::array<IsActive_t, sizeof...(I)>{ &HkIsActive<I>... };
        }
        const auto g_hooks = MakeHooks(std::make_index_sequence<kFxCount>{});

        void InstallFx(std::size_t a_i)
        {
            Fx& f = g_fx[a_i];
            if (f.state != 0) {
                return;
            }
            const auto base = Base();
            const auto slot = base + f.slotRva;
            const auto cur = *reinterpret_cast<const std::uintptr_t*>(slot);
            if (cur != base + f.fnRva) {
                REX::ERROR("comfx: {} vtable slot {:X} holds {:X}, expected {:X}; skipped", f.what, f.slotRva,
                    cur >= base ? cur - base : cur, f.fnRva);
                f.state = -1;
                return;
            }
            if (!BytesAt(f.fnRva, f.bytes, f.n, f.what)) {
                f.state = -1;
                return;
            }
            f.orig = reinterpret_cast<IsActive_t>(cur);
            if (!WriteSlot(slot, reinterpret_cast<std::uintptr_t>(g_hooks[a_i]))) {
                f.state = -1;
                return;
            }
            f.state = 1;
            REX::INFO("comfx: {} wrapped (slot {:X}, fn {:X}); [Comfort] {}={}", f.what, f.slotRva, f.fnRva, f.key, f.sw->load() ? 1 : 0);
        }

        std::string StatusString()
        {
            std::string s = std::format("shake={} shakeSite={} shakePatched={} strength={:.3f} time={:.3f} clears={}", g_noShake.load() ? 1 : 0,
                g_shakeState > 0 ? "ok" : (g_shakeState < 0 ? "mismatch" : "-"), g_shakePatched ? 1 : 0,
                g_shakeState > 0 ? *ShakeStrength() : -1.0f, g_shakeState > 0 ? *ShakeTime() : -1.0f, g_shakeClears);
            s += std::format(" radial={} double={} blur={} dof={}", g_noRadial.load() ? 1 : 0, g_noDouble.load() ? 1 : 0,
                g_noBlur.load() ? 1 : 0, g_noDof.load() ? 1 : 0);
            for (const auto& f : g_fx) {
                s += std::format(" | {}:{} calls={} suppressed={}", f.what, f.state > 0 ? "wrapped" : (f.state < 0 ? "mismatch" : "-"),
                    f.calls.load(), f.suppressed.load());
            }
            return s;
        }
    }

    std::string Install()
    {
        std::scoped_lock l(g_lock);
        const auto path = Settings::Path();
        auto rd = [&](const char* a_key, int a_def = 1) { return ::GetPrivateProfileIntA("Comfort", a_key, a_def, path.c_str()) != 0; };
        g_noShake = rd("bNoCameraShake");
        g_noRadial = rd("bNoRadialBlur");
        g_noDouble = rd("bNoDoubleVision");
        g_noBlur = rd("bNoImodBlur");
        g_noDof = rd("bNoDepthOfField", 0);  // default off: also removes the world's own DOF (see the Fx table)
        // Off at load = nothing patched (the switch can still be turned on from the pipe for the image-space items,
        // which then installs them).
        if (g_noShake.load()) {
            ApplyShake();
        }
        for (std::size_t i = 0; i < kFxCount; ++i) {
            if (g_fx[i].sw->load()) {
                InstallFx(i);
            }
        }
        const auto s = StatusString();
        REX::INFO("comfx: {}", s);
        return s;
    }

    void OnGameLoaded()
    {
        std::scoped_lock l(g_lock);
        ClearShake("game loaded");
    }

    std::string Command(const std::string& a_arg)
    {
        std::scoped_lock l(g_lock);
        if (!a_arg.empty() && a_arg != "status") {
            const auto        sp = a_arg.find(' ');
            const std::string k = a_arg.substr(0, sp);
            const std::string v = sp == std::string::npos ? std::string{} : a_arg.substr(sp + 1);
            if (v != "0" && v != "1" && v != "on" && v != "off") {
                return "error: comfx [status | shake|radial|double|blur|dof 0|1]";
            }
            const bool on = v == "1" || v == "on";
            if (k == "shake") {
                g_noShake = on;
                ApplyShake();
            } else if (k == "radial" || k == "double" || k == "blur" || k == "dof") {
                std::atomic<bool>* sw = k == "radial" ? &g_noRadial : k == "double" ? &g_noDouble : k == "blur" ? &g_noBlur : &g_noDof;
                *sw = on;
                if (on) {
                    for (std::size_t i = 0; i < kFxCount; ++i) {
                        if (g_fx[i].sw == sw) {
                            InstallFx(i);
                        }
                    }
                }
            } else {
                return "error: unknown comfx item '" + k + "'";
            }
            REX::INFO("comfx {}", a_arg);
        }
        return StatusString();
    }
}
