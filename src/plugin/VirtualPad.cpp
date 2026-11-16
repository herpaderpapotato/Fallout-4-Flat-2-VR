#include <REX/LOG.h>

#include "plugin/VirtualPad.h"

#include <Windows.h>
#undef ERROR
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace fo4flatvr::pad
{
    namespace
    {
        constexpr std::uintptr_t kIatGetState = 0x2440F10;         // xinput1_3 #2
        constexpr std::uintptr_t kIatGetCapabilities = 0x2440F08;  // xinput1_3 #4

        using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        using GetCaps_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

        GetState_t g_getState = nullptr;
        GetCaps_t  g_getCaps = nullptr;

        std::mutex                 g_lock;
        State                      g_state;
        std::atomic<std::uint64_t> g_polls{ 0 };
        DWORD                      g_packet = 0;
        State                      g_override;
        ULONGLONG                  g_overrideUntil = 0;

        SHORT Axis(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return static_cast<SHORT>(std::lround(v * 32767.0f));
        }

        BYTE Trigger(float v)
        {
            return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }

        SHORT Merge(SHORT real, SHORT vr)
        {
            return std::abs(static_cast<int>(vr)) > std::abs(static_cast<int>(real)) ? vr : real;
        }

        DWORD WINAPI HkGetState(DWORD a_user, XINPUT_STATE* a_out)
        {
            XINPUT_STATE real{};
            const DWORD  rc = g_getState ? g_getState(a_user, &real) : ERROR_DEVICE_NOT_CONNECTED;
            if (a_user != 0 || !a_out) {
                if (a_out && rc == ERROR_SUCCESS) {
                    *a_out = real;
                }
                return rc;
            }
            State s;
            {
                std::scoped_lock l(g_lock);
                s = ::GetTickCount64() < g_overrideUntil ? g_override : g_state;
            }
            if (!s.active) {
                if (rc == ERROR_SUCCESS) {
                    *a_out = real;
                }
                return rc;
            }
            ++g_polls;
            XINPUT_GAMEPAD& g = a_out->Gamepad;
            g = rc == ERROR_SUCCESS ? real.Gamepad : XINPUT_GAMEPAD{};
            WORD btn = 0;
            if (s.a) btn |= XINPUT_GAMEPAD_A;
            if (s.b) btn |= XINPUT_GAMEPAD_B;
            if (s.x) btn |= XINPUT_GAMEPAD_X;
            if (s.y) btn |= XINPUT_GAMEPAD_Y;
            if (s.lb) btn |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (s.rb) btn |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (s.ls) btn |= XINPUT_GAMEPAD_LEFT_THUMB;
            if (s.rs) btn |= XINPUT_GAMEPAD_RIGHT_THUMB;
            if (s.start) btn |= XINPUT_GAMEPAD_START;
            if (s.back) btn |= XINPUT_GAMEPAD_BACK;
            if (s.dpadUp) btn |= XINPUT_GAMEPAD_DPAD_UP;
            if (s.dpadDown) btn |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (s.dpadLeft) btn |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (s.dpadRight) btn |= XINPUT_GAMEPAD_DPAD_RIGHT;
            g.wButtons |= btn;
            g.sThumbLX = Merge(g.sThumbLX, Axis(s.lx));
            g.sThumbLY = Merge(g.sThumbLY, Axis(s.ly));
            g.sThumbRX = Merge(g.sThumbRX, Axis(s.rx));
            g.sThumbRY = Merge(g.sThumbRY, Axis(s.ry));
            g.bLeftTrigger = (std::max)(g.bLeftTrigger, Trigger(s.lt));
            g.bRightTrigger = (std::max)(g.bRightTrigger, Trigger(s.rt));
            a_out->dwPacketNumber = ++g_packet;  // always changes; the game re-reads every poll anyway
            return ERROR_SUCCESS;
        }

        DWORD WINAPI HkGetCapabilities(DWORD a_user, DWORD a_flags, XINPUT_CAPABILITIES* a_out)
        {
            const DWORD rc = g_getCaps ? g_getCaps(a_user, a_flags, a_out) : ERROR_DEVICE_NOT_CONNECTED;
            if (a_user == 0 && rc != ERROR_SUCCESS && a_out) {
                bool active;
                {
                    std::scoped_lock l(g_lock);
                    active = g_state.active || ::GetTickCount64() < g_overrideUntil;
                }
                if (active) {
                    *a_out = {};
                    a_out->Type = XINPUT_DEVTYPE_GAMEPAD;
                    a_out->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
                    a_out->Gamepad.wButtons = 0xF3FF;
                    a_out->Gamepad.bLeftTrigger = a_out->Gamepad.bRightTrigger = 0xFF;
                    a_out->Gamepad.sThumbLX = a_out->Gamepad.sThumbLY = a_out->Gamepad.sThumbRX = a_out->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
                    return ERROR_SUCCESS;
                }
            }
            return rc;
        }

        bool PatchSlot(std::uintptr_t a_rva, FARPROC a_expected, void* a_new, void** a_old, const char* a_what)
        {
            auto** slot = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr)) + a_rva);
            if (!a_expected || *slot != reinterpret_cast<void*>(a_expected)) {
                REX::ERROR("virtual pad: IAT slot for {} does not hold the xinput1_3 export ({} vs {}); not patching",
                    a_what, *slot, reinterpret_cast<void*>(a_expected));
                return false;
            }
            DWORD old = 0;
            if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                return false;
            }
            *a_old = *slot;
            *slot = a_new;
            ::VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }
    }

    void Install()
    {
        static bool done = false;
        if (done) {
            return;
        }
        done = true;
        const HMODULE xi = ::GetModuleHandleW(L"xinput1_3.dll");
        if (!xi) {
            REX::ERROR("virtual pad: xinput1_3.dll not loaded");
            return;
        }
        void* old = nullptr;
        if (PatchSlot(kIatGetState, ::GetProcAddress(xi, MAKEINTRESOURCEA(2)), reinterpret_cast<void*>(&HkGetState), &old, "XInputGetState")) {
            g_getState = reinterpret_cast<GetState_t>(old);
        }
        if (PatchSlot(kIatGetCapabilities, ::GetProcAddress(xi, MAKEINTRESOURCEA(4)), reinterpret_cast<void*>(&HkGetCapabilities), &old, "XInputGetCapabilities")) {
            g_getCaps = reinterpret_cast<GetCaps_t>(old);
        }
        REX::INFO("virtual pad: XInputGetState {} / GetCapabilities {}", g_getState ? "hooked" : "NOT hooked",
            g_getCaps ? "hooked" : "NOT hooked");
    }

    void Publish(const State& a_state)
    {
        std::scoped_lock l(g_lock);
        g_state = a_state;
    }

    void Override(const State& a_state, int a_ms)
    {
        std::scoped_lock l(g_lock);
        g_override = a_state;
        g_override.active = true;
        g_overrideUntil = ::GetTickCount64() + static_cast<ULONGLONG>((std::max)(a_ms, 0));
    }

    State Last()
    {
        std::scoped_lock l(g_lock);
        return g_state;
    }

    std::uint64_t Polls()
    {
        return g_polls.load();
    }
}
