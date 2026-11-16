#pragma once
// VR controllers -> the game's gamepad. The game reads the pad through xinput1_3 imports
// (1.11.240 IAT: ordinal 2 XInputGetState @ rva 2440F10, ordinal 4 XInputGetCapabilities @ 2440F08).
// We patch those IAT slots: pad 0 reports the VR controllers merged with any real pad.

#include <cstdint>

namespace fo4flatvr::pad
{
    struct State
    {
        bool  active{ false };
        float lx{}, ly{}, rx{}, ry{};   // -1..1, +y up
        float lt{}, rt{};               // 0..1
        bool  lb{}, rb{};
        bool  a{}, b{}, x{}, y{};
        bool  ls{}, rs{};               // stick clicks
        bool  start{}, back{};
        bool  dpadUp{}, dpadDown{}, dpadLeft{}, dpadRight{};
    };

    // Patch the IAT (call from F4SEPlugin_Load). Idempotent; logs and does nothing if the slots
    // don't hold the real xinput1_3 exports.
    void Install();

    // Thread-safe; the latest state wins. Pass active=false to stop injecting.
    void Publish(const State& a_state);

    // Test override (pipe "pad"): replaces the VR controller state for a_ms milliseconds.
    void Override(const State& a_state, int a_ms);

    // For diagnostics: last published state + how many GetState calls were served.
    State         Last();
    std::uint64_t Polls();
}
