#pragma once
// C8: VR locomotion comfort options (analysis/vr-feature-map.md C8). [Comfort] in the plugin ini.
//
// Turning (iTurnMode 0 smooth / 1 snap / 2 game): while the head-tracked world view is up and the player's movement
// update accepts turn input, the right stick X no longer reaches the virtual pad (rx = 0, so the engine's own
// look-speed yaw never double-applies); instead our turn goes through rs::UserTurn, which the Roomscale
// ModifyMovementData hook adds to angleDelta.z (the same path as the body-follow yaw). There it counts as user turn
// input, so it turns the owned heading ([Camera] bOwnHeading, rs::OwnedYaw) and the body together: one owner of the
// heading. Turns are only queued while rs::PrimaryUpdates() advances (game not paused).
// Direction (iMoveDirection 0 head / 1 off-hand aim / 2 body): the left-stick vector is rotated before it is
// published to the pad, by (wanted yaw - body heading), so the game moves the body in the chosen direction.
// Vignette (bVignette): head-locked radial black quad (xr::Runtime::SubmitVignette) above the world, below the
// HUD, faded in/out over fVignetteFadeMs while stick locomotion or turning happens.
//
// Angles use the engine heading convention (radians unless named *Deg, + = turn right, 0 = +Y).

#include <cstdint>
#include <string>

namespace fo4flatvr::comfort
{
    // Reads [Comfort] (and [Controllers] bLeftHanded). Once at plugin load. Returns a summary for the log.
    std::string Install();

    struct PadFrame
    {
        float lx{}, ly{}, rx{};       // in/out: what goes to the virtual pad (-1..1, +y = forward / up)
        bool  worldView{ false };     // head-tracked world view shown, no view-taking menu, wrist Pip-Boy closed
        bool  handYawValid{ false };
        float handYawRel{ 0.0f };     // off-hand aim yaw minus the head yaw the camera was rendered with (radians)
    };

    // Main thread (Presenter), once per XR frame, before pad::Publish: snap flick detection / smooth stick, rx zeroed
    // while we own turning, left stick rotated for the movement direction, vignette target.
    void FilterPad(PadFrame& a_f);

    // Main thread (Presenter, every frame): vignette opacity now (advances the fade) and its shape. False = nothing
    // to submit (switch off or fully faded out).
    bool Vignette(float& a_alpha, float& a_innerDeg, float& a_outerDeg);
    // Main thread, after EndFrame: whether a vignette layer went out with this frame and the frame's layer count.
    void NoteFrame(bool a_vignetteSubmitted, std::uint32_t a_layerCount, std::uint64_t a_uploads);

    // Pipe "comfort [cfg [<key> <value>]|turn snap|smooth|off|move head|hand|body|vignette on|off|stats reset]".
    std::string Command(const std::string& a_arg);
}
