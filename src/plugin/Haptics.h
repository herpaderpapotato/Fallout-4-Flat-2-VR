#pragma once
// Controller haptics queue (16 Nov, gestures; analysis/vr-gestures.md).
//
// Any thread queues a pulse; the Presenter plays the queue on the XR thread once per frame (xr::Runtime::Vibrate needs the
// session thread). Users: the gesture module (a pulse per recognised gesture) and the FRIK-flat bridge's Vibrate (FRIK's own
// config / Pip-Boy / torch haptics), which before this was only counted.
//
// FRIK path: FRIK's vrcf engine re-fires a short OpenVR TriggerHapticPulse every frame, using the microseconds (<= ~4 ms) as
// the perceived intensity; the vrshim turns that into Vibrate(hand, 1.0, micros * 1e-6). OpenXR has a real amplitude and
// duration, so such a micro pulse is played as amplitude = micros / 4000 (x the requested amplitude) for kFrikPulseS (a bit
// over one frame): while FRIK keeps re-firing it, the buzz is continuous; when it stops, the buzz ends within that time.

#include <string>

namespace xr
{
    class Runtime;
}

namespace fo4flatvr::haptics
{
    // hand 0 = left, 1 = right (physical); amplitude 0..1; seconds <= 0 = the shortest pulse. Coalesced per hand per frame
    // (strongest amplitude, longest duration).
    void Request(int a_hand, float a_amplitude, float a_seconds);

    // FRIK-flat bridge Vibrate: OpenVR-style micro pulses mapped as described above.
    void RequestFrik(int a_hand, float a_amplitude, float a_seconds);

    // Presenter thread, frame begun: play what is queued.
    void Drain(xr::Runtime& a_rt);

    // pipe readout (part of "gesture status")
    std::string Status();
}
