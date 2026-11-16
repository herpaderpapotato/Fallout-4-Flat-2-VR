#pragma once
// DxbcSig.h - Minimal DXBC reader for the draw hook:
//  - Key(): the 16-byte header checksum at blob offset 4 (the identity used by StereoTargets.inl;
//    analysis/tools/shaders/dxbc_checksum.py reproduces it for all 11,690 shipped blobs, so it is a
//    reliable content key and costs nothing to read);
//  - ReadSignature(): ISGN/OSGN elements (name, index, system value, component type, register, mask);
//  - SameInputLayoutContract(): our replacement's ISGN must equal the original's for every non-system
//    element, in order (the engine's input layout was created against the original bytecode).
// Same layout as fxp.py parse_signature (24-byte elements, SM5 ISGN/OSGN; no ISG1/OSG5 in the flat set).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fo4flatvr::stereo::dxbc
{
    struct Key
    {
        std::uint8_t b[16]{};
        bool operator==(const Key&) const = default;
    };

    struct Element
    {
        std::string   name;   // upper-cased: D3D11 linkage is case-insensitive
        std::uint32_t index{};
        std::uint32_t sysValue{};   // D3D_NAME: 0 none, 1 position, 2 clip distance, 6 vertex id, 8 instance id
        std::uint32_t compType{};   // D3D_REGISTER_COMPONENT_TYPE
        std::uint32_t reg{};
        std::uint8_t  mask{};
        bool operator==(const Element&) const = default;
    };

    inline bool GetKey(const void* a_blob, std::size_t a_size, Key& a_out)
    {
        if (!a_blob || a_size < 32 || std::memcmp(a_blob, "DXBC", 4) != 0) {
            return false;
        }
        std::memcpy(a_out.b, static_cast<const std::uint8_t*>(a_blob) + 4, 16);
        return true;
    }

    inline bool ReadSignature(const void* a_blob, std::size_t a_size, const char a_fourcc[4], std::vector<Element>& a_out)
    {
        a_out.clear();
        const auto* b = static_cast<const std::uint8_t*>(a_blob);
        if (!b || a_size < 32 || std::memcmp(b, "DXBC", 4) != 0) {
            return false;
        }
        auto u32 = [&](std::size_t o) -> std::uint32_t {
            std::uint32_t v = 0;
            if (o + 4 <= a_size) {
                std::memcpy(&v, b + o, 4);
            }
            return v;
        };
        const std::uint32_t chunks = u32(28);
        for (std::uint32_t k = 0; k < chunks && 32 + 4 * k + 4 <= a_size; ++k) {
            const std::uint32_t o = u32(32 + 4 * k);
            if (o + 8 > a_size || std::memcmp(b + o, a_fourcc, 4) != 0) {
                continue;
            }
            const std::size_t base = o + 8;
            const std::uint32_t n = u32(base);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::size_t e = base + 8 + 24 * static_cast<std::size_t>(i);
                if (e + 24 > a_size) {
                    return false;
                }
                Element el;
                const std::uint32_t nameOff = u32(e);
                for (std::size_t c = base + nameOff; c < a_size && b[c]; ++c) {
                    const char ch = static_cast<char>(b[c]);
                    el.name.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 32) : ch);
                }
                el.index = u32(e + 4);
                el.sysValue = u32(e + 8);
                el.compType = u32(e + 12);
                el.reg = u32(e + 16);
                el.mask = b[e + 20];
                a_out.push_back(std::move(el));
            }
            return true;
        }
        return false;
    }

    // Non-system ISGN elements identical (order, register, mask, type); ours may add system values
    // (SV_InstanceID) which the IA generates without the input layout.
    inline bool SameInputLayoutContract(const std::vector<Element>& a_original, const std::vector<Element>& a_ours)
    {
        std::vector<Element> o, n;
        for (const auto& e : a_original) {
            if (e.sysValue == 0) {
                o.push_back(e);
            }
        }
        for (const auto& e : a_ours) {
            if (e.sysValue == 0) {
                n.push_back(e);
            }
        }
        return o == n;
    }

    // Faithful mode: the original OSGN must be a register-exact prefix of ours (engine PS links unchanged).
    inline bool OutputsArePrefix(const std::vector<Element>& a_original, const std::vector<Element>& a_ours)
    {
        if (a_ours.size() < a_original.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_original.size(); ++i) {
            if (!(a_original[i] == a_ours[i])) {
                return false;
            }
        }
        return true;
    }
}
