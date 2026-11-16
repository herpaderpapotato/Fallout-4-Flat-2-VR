#pragma once
// FRIK-flat bridge (export API for FRIK.dll built by G:\Fallout4ToVR\frik-flat; C ABI in FrikBridgeApi.h).
//
// fo4flatvr.ini [FRIK]:
//   bEnable=1   FRIK.dll (if deployed) gets the bridge and runs; 0 = FRIK.dll stays dormant (no hooks, no patches) and
//               fo4flatvr installs no bridge hook.
//   bBody=1     (v3, default 1 since 15 Nov) 0 = FRIK.dll runs FRIK's smooth movement only: no hooks/patches/skeleton/frame on its side, and its
//               SetFrikActive / frame callback are refused here, so the floating gun, hidden arms, controller models and
//               per-hand activation stay exactly as without FRIK.dll. 1 = full FRIK-flat: FRIK's body, arms (IK to the
//               controllers) and fingers. Test override: env FO4FLATVR_FRIK_BODY=1 or the flag file fo4flatvr_frik_body.test.
//   bSmoothMovement=1  (v3) FRIK's SmoothMovementVR (FRIK_Config/FRIK.toml [SmoothMovementVR] knobs) offsets the camera:
//               stepped once per FirstPersonState::Update (GameBridge.cpp), offset added to the first-person camera position.
//               Needs FRIK.dll deployed (any bBody). 0 = no offset. Pipe: frik [smooth on|off|smooth xy on|off|reset]
//   bSmoothHorizontal=0  height only (default): FRIK's x/y smoothing lags roomscale capsule moves (measured 15 Nov). 1 = x/y too.
// While FRIK.dll reports itself active (body on): the floating gun KEEPS placing the drawn weapon (flat's first-person rig
// is a player-relative flattened bone tree FRIK's arm placement cannot drive; the gun already uses FRIK's glue + per-weapon
// JSONs) and its controller models are hidden (FRIK's hands instead); FRIK's body arms reach the same grip. [FRIK]
// bGunStepsAside=1 = the old full step-aside (gun::StepAside). The gun is the FO4VR baseline without the body.
// Pipe: frik perf (game frame interval + FRIK frame time window), frik cb off|on (skip FRIK's frame; perf A/B).
// The bridge does not depend on the gun code:
//   - own per-frame hook: call site C351F3 (PlayerUpdateEvent notify, byte-checked with plugin/CallChain.h, chained with
//     the gun's W1 hook in either install order); it calls FRIK.dll's registered frame callback (API v2), then the next hook;
//   - own pose feed: Presenter -> frikbridge::PublishXr (controller state + head pose handed to the camera) -> snapshot ->
//     FRIK.dll reads it from its frame callback through FO4FLATVR_GetFrikBridge()->GetFrame.

#include <string>

namespace xr
{
    struct ControllerState;
    struct Pose;
}

namespace fo4flatvr::frikbridge
{
    // Plugin load (Plugin.cpp): reads [FRIK]; with bEnable=1 hooks C351F3 (chained); re-applies the gun step-aside if FRIK.dll
    // already said it is active.
    void Install();

    // Presenter thread, once per XR frame (Presenter.cpp, next to gun::PublishXr): the controller state and the head pose
    // handed to the camera for the next game frame.
    void PublishXr(const xr::ControllerState& a_c, const xr::Pose& a_head, bool a_headValid, bool a_worldView, bool a_leftHanded);

    // v3 FRIK smooth movement (GameBridge.cpp camera hooks). StepCameraOffset: once per FirstPersonState::Update, before the
    // game's update (calls FRIK.dll's callback). CameraOffset: the offset to add to the first-person camera position (game
    // units, world axes); false = none (smoothing off, no FRIK.dll, or FRIK returned none).
    void StepCameraOffset();
    bool CameraOffset(float& a_x, float& a_y, float& a_z);

    // pipe: frik [status|smooth on|off|reset]
    std::string Command(const std::string& a_arg);
}
