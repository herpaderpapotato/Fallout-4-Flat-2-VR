#pragma once
// Shared between FloatingGunInput.cpp (xr side, no RE types) and FloatingGun.cpp (game side).

#include <chrono>

namespace fo4flatvr::gun::detail
{
    // One controller snapshot in the game's camera-local basis (X right, Y forward, Z up). Rotations are column-vector
    // matrices r[row][col] mapping the grip frame (game basis: X = grip +X, Y = grip -Z (through the fist), Z = grip +Y)
    // into the head frame.
    struct HandRel
    {
        bool  valid{ false };
        float pos[3]{};     // grip position, metres
        float rot[3][3]{};  // grip orientation
        float aimDir[3]{};  // aim pose -Z (pointing direction), unit
        float aimPos[3]{};  // aim pose position, metres
    };

    struct Snap
    {
        bool    worldView{ false };
        bool    leftHanded{ false };
        HandRel rNew;  // primary (weapon) hand relative to the head handed to the camera at this Present (next game frame)
        HandRel rOld;  // primary hand relative to the head the current camera root was rendered with (P/D at Present)
        HandRel oNew;  // off hand (controller model, off-hand activation ray), same two head frames
        HandRel oOld;
        std::chrono::steady_clock::time_point t{};
        unsigned long long                    seq{ 0 };
    };

    void SetSnapshot(const Snap& a_s);
}
