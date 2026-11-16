#pragma once
// Roomscale (B4) and body yaw (B3). analysis/vr-roomscale.md.
//
// B4: PlayerCharacter vtable slot 0x125 (ModifyMovementData, vtable entry rva 256D260, original
// D5AB70) is replaced. After the original runs, the head's horizontal displacement from the capsule
// (closed loop on the measured data.location, deadzone + hysteresis + clamp) is added to moveDelta
// (actor-local), so the engine moves the character controller with collision, like stick input.
// The camera then only carries the residual (head - capsule) instead of the whole head offset.
// B3: the body heading follows the head yaw (beyond a threshold, or continuously while the stick
// moves) through angleDelta.z in the same hook; the camera hooks rotate the head relative to the body
// so the view never turns twice. Stick/mouse turns rotate the room.
//
// Coordinates: "room" = the head-pose frame handed out by head:: (recentre origin, X right, Y forward,
// Z up, metres). Yaw angles use the engine heading convention (radians, + = turn right, 0 = +Y).

#include <cstdint>
#include <string>

namespace fo4flatvr::head
{
    struct Mat3;
    struct Vec3;
}

namespace fo4flatvr::rs
{
    // Reads [Roomscale] from the plugin ini and patches the vtable slot (byte-checked first). Call
    // once at plugin load. Returns a short summary for the log.
    std::string Install();

    // FirstPersonState hooks (GameBridge). a_baseYaw = heading of the game's camera rotation (radians,
    // + = right). Both are no-ops while roomscale/body-yaw are inactive.
    //  - rotation: pre-rotates the head rotation by the body-relative yaw correction;
    //  - offset:   replaces the horizontal head offset with the residual (head - capsule).
    void AdjustHeadRotation(head::Mat3& a_head, float a_baseYaw);
    void AdjustHeadOffset(head::Vec3& a_offsetM, float a_baseYaw);

    // GetTranslation feedback mode for GameBridge (see the comment there): 0 = old (2x), 1 = half-add
    // (default), 2 = exact with alternating change ("rs cfg tfix 0|1|2"). NoteCameraBase records the raw
    // value and last frame's change for the "rs" readout.
    // Camera step 1 ([Camera] bOwnHeading=1): the owned room yaw (radians, heading convention). The world-view
    // camera is Rz(ownYaw) x head; only user turn input (the game's angleDelta.z in the primary movement update)
    // and resets (start, teleport, recenter) change it, so game-driven player rotation never turns the view.
    // False while unavailable (no head pose yet, hook not installed, switch off).
    bool OwnedYaw(float& a_yaw);
    bool OwnHeadingEnabled();
    // The one entry point for feature-driven turning (snap/smooth turn, ...): queues a_radians (heading
    // convention, + = right) that the next primary ModifyMovementData adds to angleDelta.z. It is then the
    // user's turn input like the stick: the body turns (engine ApplyMovementDelta) and, with bOwnHeading, ownYaw
    // and the view turn by the same amount. Any thread; accumulates until consumed (not consumed while the
    // primary movement update does not run, e.g. menus). Pipe "rs turn <deg>".
    void UserTurn(float a_radians);
    // Primary movement updates of the player so far (each one consumes UserTurn). Comfort (C8) only queues turns
    // while this advances, so nothing piles up while the game is paused. Any thread.
    std::uint64_t PrimaryUpdates();

    // Camera step 2 (large game-driven rotations): TakeGameTurn returns (and clears) the largest heading change in one
    // primary movement update that neither the user's turn input nor our body-follow explains (setangle, scripts), radians.
    // SnapViewToHeading (director, while the view is black): owned yaw := heading - head yaw, so the view faces the
    // body's current heading and the body stays where the game put it. Returns a summary for the log.
    bool        TakeGameTurn(float& a_radians);
    std::string SnapViewToHeading();
    // While a heading snap is pending (fade-out), B3 must not turn the body back toward the old view (the snap would
    // then land between the two headings). Set by the director, cleared by it (and by SnapViewToHeading).
    void FreezeBodyFollow(bool a_freeze);
    // The director's heading-snap threshold (radians, 0 = snaps off): a game turn at least this large freezes body-follow
    // in the same update that sees it (the director's tick comes after the movement update).
    void SetHeadingSnapThreshold(float a_radians);

    int  TranslationFeedbackMode();
    void NoteCameraBase(float a_x, float a_y, float a_z, float a_prevAddX, float a_prevAddY);
    // After each FirstPersonState::Update (GameBridge): camera root world position, the full horizontal head offset
    // added (world units), the change actually applied (feedback mode) and GetTranslation call counts during /
    // outside the update. Kept in a ring for "rs camhist [n]" (overshoot diagnostics).
    void NoteCameraFrame(float a_x, float a_y, float a_z, float a_addX, float a_addY, float a_dX, float a_dY, int a_getPosIn,
        int a_getPosOut);

    // Pipe "rs [on|off|reset|hist|cfg <key> <value>]".
    std::string Command(const std::string& a_arg);
}
