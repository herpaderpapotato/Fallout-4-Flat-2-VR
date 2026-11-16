#pragma once
// Floating gun: the Fallout 4 VR 1.2.72 baseline for the weapon (analysis/vr-hands-weapon.md, "FO4VR baseline").
//
// FO4VR places the first-person WEAPON node on the wand's PrimaryWeaponOffsetNode every frame (Update1stPersonArms,
// VR EF5BE0 -> EF6280) and shows no first-person arms. Body, arms, IK, fingers and two-handed grip are FRIK / ROCK
// features and are not done here (the earlier hands code is in plugin/src/plugin/_retired/, not built).
//
// fo4flatvr.ini [Weapon] (the calibration keys are also read from the old [Hands] section when [Weapon] lacks them):
//   bFloatingGun=1   0 = no patches, no arm hiding
//   bHideArms=1      first-person body/arm/hand/Pip-Boy geometry culled while a headset feeds the plugin (gun floats)
//   bControllerModels=1  FO4VR's controller stand-ins: a model on each controller (off hand always, primary hand while no
//                    weapon model is placed); sControllerModelRight/Left (Data-relative NIF paths, default
//                    Data/Meshes/fo4flatvr/ControllerR.nif / ControllerL.nif = FO4VR's Vive_Wand.nif fallback, installed
//                    by tools/install_controller_models.py), fControllerModelOffsetX/Y/Z (cm), fControllerModelRotX/Y/ZDeg,
//                    fControllerModelScale - all in the FO4VR wand frame (grip . fGripRot)
//   fGripOffsetX/Y/Z, fGripRotX/Y/ZDeg, iAlign, bAimFromMuzzle, bNoGunLowering, sWeaponOffsetsDir (see the dist ini)
//
// Code patch (byte-checked; mismatch = logged, not patched):
//   W1  call C351F3 in Main::OnIdle_UpdatePlayer (E8 B8 B1 00 00 -> BSTEventSource<PlayerUpdateEvent>::Notify C403B0),
//       chained: the FRIK-flat bridge (FrikBridge.cpp) hooks the same call and calls through to this one.
//       The hook writes WEAPON's own local so that WEAPON.world = H(primary grip) . C . T_wto(0,-4,1) . G^-1 . J
//       (FRIK glue + per-weapon JSON, or auto-align of the fire node to the controller aim), then re-runs the engine's
//       first-person update (D9F7D0 with Q1stPersonCamera). Nothing above WEAPON is moved.
//   A1  call 21F2051 in DrawWorld::Add1stPersonGeomToCuller (E8 DA DF 5E FF -> BSCullingGroup::Add 17E0030), only with
//       bHideArms=1: a first-person root child with AppCulled set is not added, as in FO4VR's own Add1stPersonGeomToCuller
//       (VR 142842BC0). Flat adds every root child regardless of the bit, so culling the arm models alone does nothing.
// Controller aim: GameBridge's hand ray takes P/D from the fire node computed from the same controller snapshot.

#include <string>

namespace xr
{
    struct ControllerState;
    struct Pose;
}

namespace fo4flatvr::gun
{
    // Reads [Weapon] (+ [Hands] fallback), loads the weapon offset JSONs, patches W1. Once, at plugin load, before
    // frikbridge::Install (the bridge chains onto W1).
    std::string Install();

    // Presenter (main thread), after BeginFrame: the primary controller's grip/aim relative to the head pose just handed
    // to the camera (a_newHead: the next game frame's camera) and to the head the camera on screen was rendered with
    // (a_oldHead: the hand ray's P/D frame).
    void PublishXr(const xr::ControllerState& a_c, const xr::Pose& a_newHead, bool a_newValid, const xr::Pose& a_oldHead,
        bool a_oldValid, bool a_worldView, bool a_leftHanded);

    // GameBridge::UpdateHandRay (main thread, Present): camera root world (rows = NiMatrix3 storage, translate) ->
    // muzzle P (world) and barrel D. False when the gun is not placed (then the aim-pose ray is used).
    bool AimFromWeapon(const float a_camRows[9], const float a_camPos[3], float a_P[3], float a_D[3]);

    // GameBridge::UpdateHandRay (main thread, Present): the OFF hand's aim pose relative to the head the camera on screen
    // was rendered with, camera-local game basis (X right, Y forward, Z up), position in metres, direction unit.
    // False when there is no fresh snapshot (per-hand activation: X on the left controller picks with this ray).
    bool OffHandAim(float a_posM[3], float a_dir[3]);

    // F3 (GetLineOfFireData): while the gun is placed and bNoGunLowering=1 the gun-blocked test must not lower it.
    bool SuppressGunLowering();

    // FRIK-flat bridge: while FRIK.dll owns the weapon our placement steps aside (arm hiding stays: FO4VR+FRIK shows
    // FRIK's body, not the first-person arms).
    void StepAside(bool a_aside);

    // Presenter, every Present (main thread): P vs fire node bookkeeping.
    void OnPresent();

    // Pipe "gun [on|off|arms on|off|dump|cfg <key> <value>|race <ms>|trace <frames> <csv>]" (race/trace: diagnostics, README).
    std::string Command(const std::string& a_arg);
}
