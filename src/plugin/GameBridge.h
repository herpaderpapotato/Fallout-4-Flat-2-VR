#pragma once
// The only place that touches CommonLibF4 RE types. Hands raw COM pointers / plain types to the
// Win32/D3D-side code so TUs including <Windows.h>/<d3d11.h> never need the RE headers.

#include <cstdint>
#include <string>

namespace fo4flatvr::game
{
    struct RenderHandles
    {
        void* device{ nullptr };     // ID3D11Device*
        void* context{ nullptr };    // ID3D11DeviceContext* (immediate)
        void* swapChain{ nullptr };  // IDXGISwapChain* of the main window
        void* hwnd{ nullptr };
        int   width{ 0 };
        int   height{ 0 };
    };

    // Reads BSGraphics::RendererData (Address Library id 2704429). Returns false until the
    // renderer is initialised.
    bool GetRenderHandles(RenderHandles& a_out);

    // Queue a console command on the game's main thread (F4SE task interface ->
    // Console::ExecuteCommand, id 2248537). Safe to call from any thread.
    bool QueueConsoleCommand(std::string a_command);

    // Hooks FirstPersonState's GetRotation/GetTranslation (vtable entries) and composes the head
    // pose onto what the state returns. Other camera states (VATS, dialogue, furniture, third
    // person) are untouched, so the game keeps control of the camera there. Idempotent.
    bool InstallCameraHook();
    void SetUnitsPerMeter(float a_units);  // head position scale, default 70
    void SetHeightOffsetCm(float a_cm);    // added to camera height while head tracking is on
    // 20 Nov (camera agent, [Camera] bBodyCamera): the first-person eye above the capsule as head authority last had it
    // (low-passed camera node z - capsule z, units; < 0 = never sampled) and the height offset in units.
    void FirstPersonEye(float& a_eyeHeightUnits, float& a_heightOffsetUnits);
    // Head authority (user design 14 Nov): in first person the camera is built from the capsule (data.location),
    // a low-passed game eye height and the head offset, instead of the game's animated camera node, so body
    // turns, animation bob and idle sway cannot move the view. a_tauS = eye-height smoothing time constant.
    void SetHeadAuthority(bool a_on, float a_tauS);
    bool HeadAuthority();
    // 20 Nov: with head authority, the eye height steps at once when the player's sneak flag flips (FO4VR) instead of
    // following the game's gradual sneak camera blend + the low-pass. [Roomscale] bSneakHeightStep; pipe "sneakstep [on|off]".
    void        SetSneakHeightStep(bool a_on);
    std::string SneakStepCommand(const std::string& a_arg);
    // Head authority: the first-person rig fit (PlayerCharacter::UpdateActor3DPosition D7C430 -> Get1stPersonCameraPosition,
    // caller D7C9CD) gets the raw (vanilla) translation instead of the head-authority one. The engine sets the rig root from
    // (camera - Camera bone world of the previous frame) without the previous root, so a camera that does not follow the
    // Camera bone makes the root alternate between two positions every frame (measured: 5 u, undamped). The camera itself
    // is not changed. Gun pipe "gun cfg rigfit 0|1"; on by default.
    void SetRigFitRaw(bool a_on);
    bool RigFitRaw();
    float HeadAuthorityTau();

    // PlayerCamera's active state id (RE::CameraState: 0 first person, 2 VATS, 4 iron sights,
    // 8 third person, 9 furniture, 12 dialogue, ...), or -1 if unavailable. Main thread.
    int CurrentCameraState();
    // FirstPersonState::Update calls so far (our vtable hook). Menu panels: detects a game-paused camera.
    std::uint64_t FirstPersonUpdates();

    // Name of the first open menu that takes over the view (Pip-Boy, inventory/container, workshop,
    // terminal, main/pause/loading, LooksMenu ...), or nullptr. These render a zoomed, blurred or
    // flat view that must not be world-locked. Main thread.
    // a_ignore: one menu name not to count (the wrist Pip-Boy keeps the world view with "PipboyMenu" open).
    // a_skip: optional predicate; menus it returns true for are not counted either (menu panels over the world view).
    const char* ViewTakingMenu(const char* a_ignore = nullptr, bool (*a_skip)(const char*) = nullptr);

    // Applies head::TargetFov() to PlayerCamera (or restores the game's FOV when it is 0). Called
    // from the first-person hook and from the presenter each frame. Main thread.
    void ApplyTargetFov();
    // Same, without sampling the FOV-adjust for the gain loop (camera director, after a non-first-person
    // state's Update rewrote the FOV). Main thread.
    void ApplyTargetFovNoSample();
    // A/B for the worldFOV gain loop fix (pipe "fovloop old|new")
    void SetFovLoopStable(bool a_on);

    // Frustum (tangents: left<0, right>0, top>0, bottom<0) of the world NiCamera as the game last
    // used it - i.e. what the image about to be presented was rendered with. Main thread.
    bool GetWorldFrustum(float& a_left, float& a_right, float& a_top, float& a_bottom);

    // One-line readiness probe for test scripts: "ingame=<0|1> loading=<0|1> mainmenu=<0|1>
    // cell=<name|-> camstate=<n>". In-game = player has a parent cell and no loading menu is open.
    std::string ReadyReport();

    // Make the engine size its render targets and camera aspect for a_w x a_h: sets the in-memory
    // "iSize W:Display"/"iSize H:Display" before the renderer initialises (they feed Renderer::Init
    // and render target 0, which the camera aspect is derived from). RestoreDisplaySize() puts the
    // originals back after init so the game does not save the render size into Fallout4Prefs.ini.
    void OverrideDisplaySize(int a_w, int a_h);
    void RestoreDisplaySize();

    // Diagnostics: engine render target sizes (RenderTargetManager properties for targets 0..N and
    // the D3D texture sizes in RendererData).
    std::string RenderTargetReport();

    // Diagnostics for the pipe "cam" command. Reads live game objects from the pipe thread:
    // values may be torn, which is acceptable for debugging.
    std::string CameraReport();

    // htrace (HeadPose.h): record the world NiCamera's world transform as it stands now (call at Present: what the
    // frame being presented was rendered with) as a 'C' record. No-op unless the trace is armed.
    void TraceWorldCamera();

    // ---- Controller aim ([Controllers] bAimWithController) -----------------------------------
    // Code patches, installed once at plugin load (before the game runs), each byte-verified first
    // (mismatch = logged and skipped):
    //  F1  detour of Projectile::SetOriginAndDirection (rva E42EA0): player shots (not VATS) get origin =
    //      hand P and angles from hand D, useOrigin = 1. Grenades/mines (WEAP type 10/11) get the angles
    //      only (game throw origin kept), while ConfigureAimThrows is on.
    //  throw preview: calls D8265A/D82668/D828B3 in PerkUtilities::CalculateGrenadeTrajectory (D825F0)
    //      return the hand's pitch/heading (same switch), so the perk arc matches the throw.
    //  A1  call D70D0D (PlayerCharacter::UpdatePickRef -> ViewCaster::UpdatePickRef 994B80): the
    //      activation ray (origin, dir) is replaced with (P, D).
    //  diag call 47A096 (TESObjectWEAP::Fire -> Projectile::Launch E421E0): records the player's
    //      last projectile handle + launch data so "aim" can report where it actually flew.
    // Every hook passes through unchanged while aim is disabled or no fresh hand ray exists.
    // Returns a short summary ("fire=ok pick=ok launch=ok").
    std::string InstallAimHooks();
    void        SetAimEnabled(bool a_on);
    // [Weapon] bNoSpread (default 1): the player's shots outside VATS get no cone of fire (ProjectileLaunchData
    // coneOfFireRadiusMult = 0 in F1, read by TESObjectWEAP::Fire -> AimModel::CalculateFireVecs). bNoSpreadPellets (default 0):
    // also for multi-projectile weapons (shotguns). Installs F1 if controller aim did not. Plugin load, once.
    std::string InstallNoSpread(bool a_on, bool a_pellets);
    // pipe: nospread [status|on|off|pellets on|off|reset|hist] (hist = per projectile: deviation deg, pellets, zeroed)
    std::string NoSpreadCommand(const std::string& a_arg);
    void        SetNoSpread(bool a_on, bool a_pellets);  // 20 Nov (live settings): [Weapon] bNoSpread / bNoSpreadPellets
    bool        AimEnabled();
    // Debug knob: how a camera-local vector is taken to world. 0 = local axis i is NiMatrix3 row i
    // (what the engine itself does: SetOriginAndDirection reads camera-root row 1 as forward), 1 = column i.
    void SetAimConvention(int a_conv);
    // Aim follow-ups ([Controllers] bHandLineOfFire, bSuppressCameraRecoil, bHandAutoAimOverride,
    // bClampHandToCamera). Since 20 Nov every follow-up is installed with the aim hooks and gated at runtime (live).
    //  F3  detour GetLineOfFireData (DA6640): gun-blocked lowering / cover iron-sight tests use the hand ray.
    //  F4  call D5AC41 (ModifyMovementData -> AimModel::ApplyAimAdjust): recoil no longer turns the
    //      player's view while the hand ray is live (spread / cone growth unchanged).
    //  A2  calls 9967DA (GetAutoAimActor) and 9956C0 (IsCameraLookingAtTeammate) inside
    //      ViewCaster::UpdatePickRef: a hand pick is not replaced by the camera's auto-aim actor, and a
    //      teammate the hand ray hits is the target.
    //  clamp: no patch; camera -> P segment test with CalculatePickHitFraction (DA6400) every frame,
    //      P moved to just before the hit.
    void        ConfigureAimExtras(bool a_lineOfFire, bool a_suppressRecoil, bool a_autoAimOverride, bool a_clamp);
    // [Controllers] bAimThrowsWithController (default 1): grenade/mine throws follow the primary controller's pointing
    // direction (pitch included). Runtime: pipe "aim throw on|off".
    void        ConfigureAimThrows(bool a_on);
    // Pipe "aim lof|recoil|autoaim|clamp|throw on|off" (runtime; only if installed at load).
    std::string SetAimFeature(const std::string& a_name, bool a_on);
    // Pipe "aim probe": next AimTick casts 24 horizontal rays from the camera (report: probe='...').
    void        RequestAimProbe();

    // The aim hand relative to the head pose the current camera root was rendered with, in the game's
    // camera-local basis (X right, Y forward, Z up): position in metres, direction unit length.
    // Main thread (Presenter, every frame). Computes and publishes the world ray P/D from the camera
    // root's world transform; a_valid = false invalidates it.
    void UpdateHandRay(const float a_posM[3], const float a_dir[3], bool a_valid);
    // Per-hand activation (FO4VR: A picks what the right controller points at, X what the left points at):
    // ViewCaster::UpdatePickRef uses the primary hand ray (0) or the off hand's aim ray (1, gun::OffHandAim).
    // PickRuns: how many pick updates have used that hand's ray (ControllerBindings waits for one before activating).
    void SetPickHand(int a_hand);
    // Pointer beam (PointerBeam.h), Present thread: distance (m) along the primary hand ray to the first collision
    // (CalculatePickHitFraction, the wall-clamp test), at most a_maxM; a_hit = something within a_maxM. False when there is
    // no fresh hand ray, no game world or the test is unavailable.
    bool HandRayWorldHit(float a_maxM, float& a_distM, bool& a_hit);
    // 21 Nov (workshop agent, WorkshopHand.h): the primary hand ray in world units (origin = the wall-clamped hand point the
    // pick and the shots use, unit direction). Any thread; false when there is no fresh, valid ray (no XR frame within 200 ms,
    // not first person / iron sights, hand > 1 m from the camera).
    bool HandRayWorld(float a_origin[3], float a_dir[3]);
    // FO4VR activation preview geometry for the primary hand ray: length (m) = FO4VR's reach a_reachM (longer when pointing
    // down, a_floorMult), clamped at the first hit; a_target = the hand-ray activation pick has a ref (then the ray runs to
    // its hit, up to a_maxM). False without a fresh hand ray / game world.
    bool ActivatePreview(float a_reachM, float a_floorMult, float a_maxM, float& a_lenM, bool& a_target);
    // FO4VR's wand-laser modes: 1 = workshop mode (Workshop::QEnabled), 2 = commanding a companion
    // (PlayerCharacter::GetActorDoingPlayerCommand), 0 = neither.
    int BeamWorldMode();
    // Floating gun (FO4VR kickback / recoil on the wand chain): the player's AimModel recoil target (+0x68/+0x6C) and
    // spring offset (+0x70/+0x74), radians, as of the last ApplyAimAdjust (F4 hook; false if not hooked or stale);
    // the number of player projectile launches so far (TESObjectWEAP::Fire -> Projectile::Launch).
    bool PlayerAimModel(float a_out[4]);
    std::uint64_t PlayerLaunches();
    std::uint64_t PickRuns(int a_hand);
    // Main thread, every Present: follows the player's last launched projectile (diagnostics) and
    // runs a pending RequestEquip.
    void AimTick();
    // Test helper (pipe "aim equip <hexFormID>"): equip + draw an item the player carries, on the main thread.
    // a_mode: 0 equip + draw (DrawWeaponMagicHands), 1 equip only (draw with the controller), 2 unequip (ActorEquipManager).
    void RequestEquip(std::uint32_t a_formID, int a_mode = 0);
    // Pipe "aim": hand ray, camera, engine's own pick ray, current pick ref, fire/projectile stats.
    std::string AimReport();
}
