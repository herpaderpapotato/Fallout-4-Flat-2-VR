#pragma once
// Weapon in hand (B5/C3 stage 0 + 1; analysis/vr-hands-weapon.md).
//
// Code patches (byte-checked; mismatch = logged, not patched):
//  W1  call C351F3 in Main::OnIdle_UpdatePlayer (E8 B8 B1 00 00 -> BSTEventSource<PlayerUpdateEvent>::Notify
//      C403B0). The hook runs HandsFrame() and then the original Notify. At this point the first-person rig
//      (player+0xB78) has its final animated pose and this frame's camera root is known.
//      HandsFrame: target WEAPON = H(primary grip) . C(calibration) . T_wto(0,-4,1) . G^-1(FRIK hand glue) .
//      J(FRIK weapon JSON or skeleton bind; auto-align of the fire node to the controller aim without a JSON),
//      rigid correction D = target . WEAPON^-1 applied to COM (never Root / Camera), then the engine's own
//      first-person update (D9F7D0 with Q1stPersonCamera) is re-run.
//  W2  call C346AC in Main::OnIdle (E8 9F EB FF FF -> Main::Swap C33250): probe only (WEAPON world pre-Swap).
// Controller aim: while the feature is active, GameBridge's hand ray takes P/D from the weapon's fire node
// computed from the same controller snapshot (AimFromWeapon); the clamp still applies afterwards.
// Stage 2 ([Hands] bArmIK=1, same W1 hook): COM placed under the HMD with the player heading's yaw only, then FRIK's
// arm solver (solveArmToHandWorldTarget) on both first-person arms: gun hand = WEAPON* . (animated WEAPON local)^-1,
// off hand = left grip . mirror(C . T_wto . G^-1), or the animation's support-hand pose on the gun when near it
// (bTwoHanded: barrel along off - gun hand); kept recoil (bKeepRecoil), free off-hand fingers (bFingers, FRIK open/
// closed tables into FlattenedBone.local), PipboyBone for the wrist Pip-Boy quad (bPipboyOnBone).

#include <string>

namespace xr
{
    struct ControllerState;
    struct Pose;
}

namespace fo4flatvr::hands
{
    // Reads [Hands] from the plugin ini, loads the weapon offset JSONs and patches W1/W2. Once, at plugin load.
    std::string Install();

    // Presenter (main thread), after BeginFrame: the primary controller's grip/aim relative to the head pose just
    // handed to the camera (a_newHead: the next game frame's camera) and relative to the head pose the camera now on
    // screen was rendered with (a_oldHead: the hand ray / P-D frame).
    void PublishXr(const xr::ControllerState& a_c, const xr::Pose& a_newHead, bool a_newValid, const xr::Pose& a_oldHead,
        bool a_oldValid, bool a_worldView, bool a_leftHanded);

    // GameBridge::UpdateHandRay (main thread, Present): camera root world (rows = NiMatrix3 storage, translate) ->
    // muzzle P (world) and barrel D. False when the feature is not active (then the aim-pose ray is used).
    bool AimFromWeapon(const float a_camRows[9], const float a_camPos[3], float a_P[3], float a_D[3]);

    // F3 (GetLineOfFireData): while the weapon is in the hand and [Hands] bNoGunLowering=1, the gun-blocked test
    // must not lower the (now physical) gun.
    bool SuppressGunLowering();

    // Stage 2 ([Hands] bPipboyOnBone=1), wrist Pip-Boy (PipboyLayer, main thread): where PipboyBone (left forearm) sat
    // in the left controller's OpenXR grip frame (metres, lifted [Hands] fPipboyLiftCm off the back of the hand) at
    // the last hook frame. False unless the last hook call ran the arm IK with the left arm solved; the value is
    // kept while the game is paused (the Pip-Boy menu stops the update, and with it the hook).
    bool PipboyBoneOffset(float a_offGrip[3]);

    // Presenter, every Present (main thread): render-time probes.
    void OnPresent();

    // Pipe "hands [on|off|probe [frames]|dump|cfg <key> <value>]".
    std::string Command(const std::string& a_arg);
}
