#pragma once
// Byte check for a CALL rel32 site that more than one fo4flatvr module hooks (C351F3: floating gun W1 + FRIK-flat
// bridge). A site is hookable when it is `E8 <rel32>` and its target is either the original callee or a trampoline
// stub of this DLL (`FF 25 00 00 00 00 <abs64>` jumping into fo4flatvr.dll), i.e. another fo4flatvr hook that itself
// calls on to the original. write_call<5> then returns the current target, so each hook calls the next one.
// Header-only; include after the CommonLib / Windows headers.

#include <cstdint>
#include <cstring>
#include <string>

namespace fo4flatvr::callchain
{
    inline bool InThisModule(std::uintptr_t a_addr)
    {
        HMODULE self = nullptr, at = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&InThisModule), &self)) {
            return false;
        }
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(a_addr), &at)) {
            return false;
        }
        return at == self;
    }

    // a_site / a_original: absolute addresses. a_why: "pristine", "chained" or the reason for refusing.
    inline bool Hookable(std::uintptr_t a_site, std::uintptr_t a_original, std::string& a_why)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(a_site);
        if (p[0] != 0xE8) {
            a_why = "not a CALL rel32";
            return false;
        }
        std::int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        const std::uintptr_t target = a_site + 5 + static_cast<std::intptr_t>(rel);
        if (target == a_original) {
            a_why = "pristine";
            return true;
        }
        const auto* t = reinterpret_cast<const std::uint8_t*>(target);
        static constexpr std::uint8_t kJmpAbs[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        if (::IsBadReadPtr(t, 14) == 0 && std::memcmp(t, kJmpAbs, 6) == 0) {
            std::uintptr_t abs;
            std::memcpy(&abs, t + 6, 8);
            if (InThisModule(abs)) {
                a_why = "chained (fo4flatvr hook)";
                return true;
            }
        }
        a_why = "call target is neither the original nor a fo4flatvr hook (another mod?)";
        return false;
    }
}
