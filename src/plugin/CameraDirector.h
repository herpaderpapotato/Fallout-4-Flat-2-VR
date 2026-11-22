#pragma once
// Camera step 1 (analysis/vr-camera-states.md sections 4 and 8): the camera director.
//
// Code patch: call site 1026035 in PlayerCamera::UpdateCamera (1025E20) -> TESCamera::Update (827C80),
// bytes E8 46 1C 80 FF, target prologue 40 53 48 83 EC 20 48 8B D9 (byte-checked; mismatch = not patched).
// After the original (the active state's Begin/Update/End ran and wrote the camera root), the director
// reads the state id, looks up its policy and, per policy, replaces the root pose:
//   TRACK      rotation Rz(ownYaw) x head, translation = the game's (+ head offset). First person keeps its
//              FirstPersonState hooks (GameBridge); other states are rewritten here.
//   FADE_SNAP  fade to black (fFadeMs), hold the last good pose (+ head motion), snap when the policy leaves
//              FADE_SNAP and the view is black, fade in. fFadeTimeoutS -> CINEMA.
//   LOCK       translation frozen at the last good pose (+ head motion), rotation as TRACK.
//   CINEMA     untouched: the presenter shows the flat-screen fallback (opt-in per state only, 15 Nov).
//   SCENE      (15 Nov, user: "no flat panel rendering a scene unless we opt in") the AAF free-camera method for every
//              other game camera: translation = the game camera's + head motion since entry, rotation = Rz(game camera
//              yaw) x head (game pitch dropped), blinks on cuts. Internally TRACK + a per-state scene flag; default for
//              every state without its own handling (third person, auto-vanity, free, transition, tween, animated /
//              furniture-animation, mount, bleedout, dialogue camera). Controls keep what they had on the flat screen.
// Policies are data: [Camera] sPolicy<StateName>=TRACK|SCENE|FADE_SNAP|LOCK|CINEMA, or pipe "camdir policy".
// The owned heading itself (ownYaw) lives in Roomscale ([Camera] bOwnHeading).
// Free-camera scene view ([Camera] bFreeCamScene, default 1; analysis/mod-aaf-vr.md): the free camera (AAF scenes, tfc)
// updates through the thunk 1026290 (E9 -> 827C80, byte-checked), which is redirected to the same post-tick; state 3 is
// then TRACK with translation = the free camera's + head offset, rotation = Rz(free-camera yaw) x head (pitch dropped),
// blinks on position/yaw cuts, a black hold over game-driven glides, and a fade grace for the brief third-person state.

#include <cstdint>
#include <string>

namespace fo4flatvr::camdir
{
    enum class Policy : int
    {
        kTrack = 0,
        kFadeSnap = 1,
        kLock = 2,
        kCinema = 3,
        kScene = 4,  // config only: a SCENE state reports kTrack from ViewPolicy (SceneView() tells them apart)
    };

    // Reads [Camera] bCameraDirector, sPolicy*, fFadeMs, fFadeTimeoutS and patches the call site. Once, at
    // plugin load. Returns a summary for the log.
    std::string Install();

    // Presenter (main thread): the policy in effect for the camera state it sees. Without the director (switch
    // off or patch failed) this is the old rule: first person = TRACK, anything else = CINEMA.
    Policy ViewPolicy(int a_camState);
    // TRACK, LOCK or FADE_SNAP: the head-tracked world view (the presenter keeps world view + fade).
    bool WorldView(int a_camState);
    // SCENE state (head-tracked game camera, AAF method): the presenter keeps the flat-screen input mapping there.
    bool SceneView(int a_camState);

    // Camera step 2: the VATS camera gate is active, so VATSMenu keeps the head-tracked world view (GameBridge's
    // view-taking menu list skips it).
    bool VatsWorldView();
    // Furniture gate + workbench: the third-person body shown for the furniture animation goes back once the player's
    // sit state is normal again (TaskQueue Show1stPerson(true)). Main thread, every frame.
    void PollWorkbenchBody();
    // POV head hide ([Camera] bPovHeadHide, 15 Nov, user: "FRIK removes the player head ... both FRIK and AAF POV views take
    // that into account"): in the free-camera scene view (state Free = AAF POV/TFC, SCENE policy) whose game camera sits at
    // an actor's Head bone (on the player or on an NPC; within fPovHeadRadius for 2 finds in a row, kept out to 1.5x), that
    // actor's whole head is culled: its BSFaceGenNiNodeSkinned node (every head part) + the armour in FRIK's head slots + head
    // gear by FRIK's face-list names in its other armour; without a FaceGen node nothing is hidden. Never in first person /
    // iron sights / selfie / third person / other states (17 Nov fix); shown again as soon as the view leaves that head.
    // First person stays FRIK's (C35: FRIK shows its own culls again outside first person). Main thread, every frame.
    void PollHeadHide();

    // Fade opacity now (0..1), advancing the fade clock. Main thread (Presenter, every Present).
    float FadeAlpha();

    // Presenter: a view-taking menu is shown in the flat window (crafting menu on the game's workbench camera, ...).
    // While set, FADE_SNAP counts as CINEMA (no black hold behind / over the window; any running fade episode ends at
    // once). Round 4: the workbench camera's Transition/Animated states held 5 s of black over the cooking menu.
    void SetMenuWindow(bool a_on);
    // Director post-ticks so far (every TESCamera::Update of the player camera, any state). UI round 8: the paused-camera
    // driver's stall test outside first person (a game-pausing menu over a SCENE / third-person / furniture camera).
    std::uint64_t Ticks();

    // ---- 20 Nov (user: "completely ignore any calls to move the camera away from our first person camera (single exception
    // is VATS) ... from its position on the player body"; "a HUD notification" on camera changes; inputs must not change with
    // the view mode). analysis/third-person-vr.md section 8.
    // [Camera] bLockFirstPerson (pipe "camdir lock on|off"): every engine request to leave first person is refused except
    //   VATS: Set3rdPerson / Force3rdPerson (POV toggle, wheel zoom, scripts, AAF), ToggleFreeCameraMode (tfc / AAF free
    //   camera, entering only), StartFurnitureMode (chairs, beds, workbenches, terminals, power armour: HUD mode only),
    //   StartAnimatedMode (PA enter/exit, scripted cams), StartBleedoutMode, StartMountMode; the dialogue camera, kill cams,
    //   the VATS death cam and the idle orbit through camtweak (forced on); any other non-first-person state that still
    //   appears (a save loaded in third person, a mod's own PushState) -> PlayerCamera::Force1stPerson. F3 to third refused.
    // [Camera] bBodyCamera (pipe "camdir body on|off"): in every camera state except first person / iron sights (already
    //   there) and VATS, the camera position is the first-person eye on the player body (capsule + first-person eye height +
    //   the head offset, as head authority builds it), rotation Rz(owned heading) x head; the policy table is ignored
    //   (TRACK); the player's head is hidden (the third-person body is shown in those states).
    //   21 Nov: bBodyCameraFollowsAnimation (default 1): while the body is animated away from the capsule (scene / furniture /
    //   bleedout signal, or the animated eye > 24 u from the capsule eye) the eye is the third-person Head bone +
    //   fBodyCameraEyeForward along the face (read in the director tick: after UpdateAnimation + UpdateScenegraph, this
    //   frame's pose). bBodyCameraYieldsToPov (default 1): the free camera at ANOTHER actor's head (AAF POV on an NPC) is
    //   shown as SCENE. bFreeCamStatePosition (default 1): the free camera's position comes from the state (after AAF's write).
    // 20 Nov late (analysis/mod-aaf-vr.md section 13): [Camera] bLockAllowScenes (AAF / defeat scenes keep their cameras under
    //   the lock; defeat window fDefeatWindowS after a bleedout), bOrbitStatesScene (TRACK on an orbit camera = SCENE),
    //   bFreeCamOwnStickYaw (free-camera stick turn kept as our own yaw; AAF POV auto mode no longer pulls it back),
    //   bSceneAftercare (PipboyHandler hooked: refused Pip-Boy presses diagnosed, scene leftovers released; pipe
    //   "camdir aftercare").
    bool BodyView(int a_camState);      // bBodyCamera applies to this state
    bool LockFirstPerson();
    // [Camera] bSameInputAllViews (default 1): controls keep the first-person gameplay mapping in every camera view
    // (SCENE, CINEMA, third person, free camera ...) while no menu is open. 0 = the earlier flat-screen (menu) mapping there.
    bool SameInputAllViews();
    // The game camera's yaw (heading convention) the last director tick saw before it rewrote the root (the frame the game
    // moves the player in outside first person). False before a tick or outside a world-view state.
    bool GameCameraYaw(float& a_yaw);
    // [Camera] bCameraHudNotify (default 1): a HUD message on every camera state change and every view mode change
    // (SCENE/TRACK/CINEMA/LOCK/FADE_SNAP, by F2, gesture, panel, pipe or an automatic rule) and on a refused camera request.
    // Main thread, every frame (Presenter).
    void PollNotify();
    // the F2 / F3 / gesture result text a caller would show itself; false when PollNotify will announce it anyway
    bool CallerShouldShow(const std::string& a_result);

    // Pipe "camdir [on|off|policy <state> <policy>|fadetest [holdMs]|table]".
    std::string Command(const std::string& a_arg);

    const char* PolicyName(Policy a_p);
}
