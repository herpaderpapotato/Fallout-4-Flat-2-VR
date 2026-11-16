#pragma once
// Shared between HandsInput.cpp (xr side, no RE types) and Hands.cpp (game side).

#include <chrono>

namespace fo4flatvr::hands::detail
{
    // One controller snapshot in the game's camera-local basis (X right, Y forward, Z up). Rotations are
    // column-vector matrices r[row][col] mapping the grip frame (game basis: X = grip +X, Y = grip -Z (through
    // the fist), Z = grip +Y) into the head frame.
    struct HandRel
    {
        bool  valid{ false };
        float pos[3]{};        // grip position, metres
        float rot[3][3]{};     // grip orientation
        float aimDir[3]{};     // aim pose -Z (pointing direction), unit
        float aimPos[3]{};     // aim pose position, metres
    };

    // Stage 2 finger input of one controller (what xr::ControllerState exposes: no touch states).
    struct HandInput
    {
        float trigger{ 0.0f };  // 0..1
        float grip{ 0.0f };     // 0..1
        float thumb{ 0.0f };    // 0..1: face button / stick click held = 1, stick deflection beyond 0.3 = 0.6
    };

    struct Snap
    {
        bool     worldView{ false };
        bool     leftHanded{ false };
        HandRel  rNew;   // primary (weapon) hand relative to the head handed to the camera at this Present (next game frame)
        HandRel  rOld;   // primary hand relative to the head the current camera root was rendered with (P/D at Present)
        HandRel  lNew;   // off hand, same frames (stage 2)
        HandRel  lOld;
        HandInput inPrimary, inOff;
        bool     headNewValid{ false };
        float    headNewQ[4]{ 0, 0, 0, 1 };  // head pose handed to the camera (reference space, x y z w)
        float    headNewP[3]{};
        std::chrono::steady_clock::time_point t{};
        unsigned long long seq{ 0 };
    };

    void SetSnapshot(const Snap& a_s);
}
