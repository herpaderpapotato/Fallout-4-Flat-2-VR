#pragma once
// Data/F4SE/Plugins/fo4flatvr.ini. Plain Win32 profile API: no extra dependency, re-readable.

#include <array>
#include <map>
#include <string>

namespace fo4flatvr
{
    struct Settings
    {
        bool        enabled{ true };
        bool        headTracking{ true };   // Phase 1: HMD pose onto the game camera
        bool        positionTracking{ true };
        bool        mono{ true };           // start in mono world view instead of cinema
        bool        afr{ false };            // alternate-eye stereo world view (overrides mono)
        float       unitsPerMeter{ 70.0f };
        float       maxRenderFov{ 0.0f };     // cap on horizontal render FOV (deg) for sharpness; 0 = none
        float       heightOffsetCm{ 0.0f };   // added to head height (negative = lower camera)
        bool        headAuthority{ true };    // [Roomscale] bHeadAuthority: 1st-person camera = capsule + eye height + head (not the animated camera node)
        float       eyeHeightTauS{ 0.6f };    // [Roomscale] fEyeHeightSmoothS: low-pass of the game's eye height (removes bob/sway)
        bool        sneakHeightStep{ true };  // [Roomscale] bSneakHeightStep: eye height steps at once on a sneak toggle (FO4VR), 20 Nov
        bool        poseSync{ true };         // [General] bPoseSync: game job threads wait for this frame's head pose (HeadPose.h)
        float       poseSyncTimeoutMs{ 50.0f };  // [General] fPoseSyncTimeoutMs: upper bound of that wait
        bool        gpuQueueLimit{ true };    // [General] bGpuQueueLimit: wait for the GPU before the head pose is predicted (Presenter GpuQueueLimit), 20 Nov
        int         gpuQueueFrames{ 1 };      // [General] iGpuQueueFrames: frames left unfinished on the GPU at that point (0-2)
        bool        poseTrace{ false };       // [Debug] bPoseTrace: continuous per-event pose trace CSVs (HeadPose.h htrace)
        int         poseTraceFrames{ 3000 };  // [Debug] iPoseTraceFrames: frames per CSV file
        int         poseTraceFiles{ 20 };     // [Debug] iPoseTraceFiles: stop after this many files
        int         windowWidth{ 1280 };       // game window client size (0 = the game's own)
        int         windowHeight{ 720 };
        int         renderWidth{ 2880 };        // swapchain/render resolution; 0 = game decides (window size)
        int         renderHeight{ 1620 };
        float       renderScale{ 1.0f };        // [Render] fRenderScale: multiplies the per-eye size (iWidth/2 x iHeight), like FO4VR's fRenderTargetSizeMultiplier
        int         stereoSeamAlign{ 128 };     // [Stereo] iSeamAlign: stereo eye region rounded up to a multiple (guard px on both sides of each eye, not submitted); 0/1 = off. 128: a 1/128-res buffer misaligned at 64 (43-px stripes, s5d)
        int         stereoSeamGuard{ 32 };      // [Stereo] iSeamGuard: minimum guard px per eye before the alignment (32: run5 all-on 99.03 -> 99.36 %, +2.4 % px)  // game units per real metre (head position scale)
        std::string xrRuntimeJson;          // optional: forces XR_RUNTIME_JSON for this process
        bool        preferStageSpace{ false };
        bool        recentreOnPoseJump{ true };  // [OpenXR] bRecentreOnPoseJump: re-anchor on a start-up placeholder-pose jump (HeadPose.cpp)
        float       quadWidthMeters{ 3.0f };
        float       quadDistanceMeters{ 2.5f };
        float       quadHeightMeters{ 0.0f };
        int         recenterKey{ 0x77 };    // VK_F8 (round 8: was F9 = the game's Quickload)
        int         toggleKey{ 0x79 };      // VK_F10: pause/resume VR submission
        // Round 8 hotkeys (keyboard only; only while the game window has focus). Checked free against Fallout 4's ControlMap
        // (F5 Quicksave, F9 Quickload, F1/F4/F6/F12 in debug/menu contexts), Steam (F12), AAF (Home/Delete/End/PgUp/PgDn,
        // F11 quickscene), OCBP/LPCE (F7), SAM (;), MCM keybinds in this MO2 setup (numpad, ;, \, ').
        int         cameraCycleKey{ 0x71 };  // [Keys] iCameraCycle (VK_F2): cycle this camera state's policy SCENE/TRACK/CINEMA
        int         povToggleKey{ 0x72 };    // [Keys] iPovToggle (VK_F3): first <-> third person (Force1st/3rdPerson)
        bool        aimWithController{ true };  // [Controllers] fire + activation ray from the aim-hand controller
        // 26 Nov: [Controllers] sNoHandPickMenus: while one of these menus is open the activation pick is the game's own
        // (camera / cursor), not the hand ray. Default empty: the new-game crash reproduced with and without the hand-ray
        // pick in LooksMenu (it is not the cause), and the ray is how a Nate / Nora choice is made in VR.
        std::string noHandPickMenus;
        bool        leftHanded{ false };        // [Controllers] aim hand = left controller
        bool        handLineOfFire{ true };       // [Controllers] bHandLineOfFire: gun lowering / iron-sight tests from the hand
        bool        suppressCameraRecoil{ true }; // [Controllers] bSuppressCameraRecoil: recoil does not turn the view
        bool        handAutoAimOverride{ true };  // [Controllers] bHandAutoAimOverride: hand pick beats the camera auto-aim actor
        bool        clampHandToCamera{ true };    // [Controllers] bClampHandToCamera: P not behind walls seen from the camera
        bool        aimThrowsWithController{ true };  // [Controllers] bAimThrowsWithController: grenades/mines fly where the hand points
        bool        noSpread{ true };             // [Weapon] bNoSpread: no cone of fire for the player's shots outside VATS
        bool        noSpreadPellets{ false };     // [Weapon] bNoSpreadPellets: also for multi-projectile weapons (shotguns)
        bool        separateHud{ true };        // [HUD] bSeparateHud: HUD on its own head-locked layer in world view
        float       hudDistanceMeters{ 1.2f };  // [HUD] fDistanceMeters
        float       hudWidthMeters{ 1.0f };     // [HUD] fWidthMeters
        float       hudHeightMeters{ -0.25f };  // [HUD] fHeightMeters (centre offset from eye level)
        bool        hudHideCrosshair{ true };   // [HUD] bHideCrosshair
        int         hudSourceTarget{ 0x3E };    // [HUD] iSourceTarget (62 = 0x3E glass result, 63 = 0x3F raw movie)
        bool        hudPremultiplied{ true };   // [HUD] bPremultiplied
        bool        hudClearTargets{ true };    // [HUD] bClearTargets: clear 0x3E/0x3F after the HUD draw (no lingering elements)
        // [Pipboy] wrist Pip-Boy (D3 stage 1; PipboyLayer.h / GamePipboy.h)
        bool        wristPipboy{ true };        // [Pipboy] bWristPipboy
        float       pipboyWidthMeters{ 0.20f }; // [Pipboy] fWidthMeters
        float       pipboyOffset[3]{ -0.05f, 0.0f, 0.13f };  // [Pipboy] fOffsetX/Y/Z (left-hand grip space, m)
        float       pipboyRotDeg[3]{ 0.0f, 0.0f, 0.0f };     // [Pipboy] fRotXDeg/fRotYDeg/fRotZDeg
        int         pipboySourceTarget{ 0x3D }; // [Pipboy] iSourceTarget (61 = 0x3D measured source; 60 = 0x3C)
        bool        pipboyStopImod{ true };     // [Pipboy] bStopImod
        bool        pipboyCursorDot{ true };    // [Pipboy] bCursorDot
        bool        pipboyTint{ true };         // [Pipboy] bTint (screen tinted with fPipboyEffectColorR/G/B:Pipboy)
        // [Menus] menu laser pointer (D2; MenuPointer.h / GameMenuPointer.h)
        bool        menuPointer{ true };        // [Menus] bPointer
        bool        menuCursorDot{ true };      // [Menus] bCursorDot
        bool        menuPanel{ true };          // [Menus] bPanel: Pause/Favorites/MessageBox over the live world view (FO4VR)
        bool        menuDialoguePanel{ true };  // [Menus] bDialoguePanel: dialogue options on the menu panel (+ look-anywhere, hide weapon, haptics)
        bool        menuNoBlur{ true };         // [Menus] bNoMenuBlur: no frozen blurred background behind panels / VATS (UI+0x1F8 held at 0)
        bool        menuClearTargets{ true };   // [Menus] bClearMenuTargets: clear 0x3E/0x3F after FlatScreenModel (pause-menu HUD decal)
        int         menuPanelSource{ 0x3E };    // [Menus] iPanelSource (62 = 0x3E, 63 = 0x3F FlatScreenModel target)
        bool        menuAnchor{ true };         // [Menus] bAnchorToView: menus / flat fallback appear where the user looks
        float       menuReanchorDeg{ 50.0f };   // [Menus] fReanchorDeg: a new menu in the same episode moves the quad if the head turned this far
        bool        menuBackdrop{ true };       // [Menus] bBackdrop: last world frame stays behind the flat fallback (not black)
        float       menuBackdropDim{ 0.5f };    // [Menus] fBackdropDim: black over that frozen backdrop (0..1)
        bool        menuBackdropPanorama{ true }; // [Menus] bBackdropPanorama: the backdrop is a world panorama (cube) filled from the
                                                  // head-tracked frames as you look around, world-locked, everywhere (never black)
        float       menuPanoramaHoleDim{ 0.6f };  // [Menus] fPanoramaHoleBrightness: extra scale where nothing was captured yet
        bool        menuPanelPreview{ true };   // [Menus] bPanelPreview: container/barter/crafting/workshop/lockpicking/book menus on the
                                                //   panel over the live world, their 3D item preview captured onto it
        bool        menuFollowWorkshop{ true }; // [Menus] bFollowWorkshop: the WorkshopMenu panel re-anchors when the head turns past fReanchorDeg
        // Round 6 (FO4VR workbenches, analysis/vr-workbenches.md): crafting menus (Cooking/Examine/PowerArmorMod/RobotMod) on
        // FO4VR's workbench panel geometry (world_projectedMenu.nif on UprightHmdLagNode: a curved screen 47 u = 0.68 m from
        // the head, ~1.21 m arc x 0.66 m, centre 0.08 m below eye level); the cinema geometry ([Cinema]) otherwise.
        bool        menuWorkbenchPanel{ true };          // [Menus] bWorkbenchPanelFO4VR
        float       menuWorkbenchWidthMeters{ 1.21f };   // [Menus] fWorkbenchWidthMeters
        float       menuWorkbenchDistanceMeters{ 0.68f }; // [Menus] fWorkbenchDistanceMeters
        float       menuWorkbenchHeightMeters{ -0.08f }; // [Menus] fWorkbenchHeightMeters
        // FO4VR workshop mode: the WorkshopMenu movie on the off-hand controller (WSWorkshopModel, WS_InvWand.nif on the
        // secondary wand, ~0.40 m), not in front of the head. 22 Nov: placement in the off hand's FO4VR wand frame (the
        // OpenXR grip . [Weapon] fGripRot / fGripOffset, the frame the floating gun and FRIK-flat use; -60 deg X = the aim
        // pose), offsets and pitch = WS_InvWand.nif's shape transform under SecondaryUIOffsetNode (identity on Touch).
        bool        menuWorkshopOnHand{ true };          // [Menus] bWorkshopOnHand
        float       menuWorkshopHandWidthMeters{ 0.40f }; // [Menus] fWorkshopHandWidthMeters
        float       menuWorkshopHandUpMeters{ -0.004f }; // [Menus] fWorkshopHandUpMeters (along the wand's up, before the pitch)
        float       menuWorkshopHandBackMeters{ 0.024f }; // [Menus] fWorkshopHandBackMeters (along the wand's back, toward the user)
        float       menuWorkshopHandPitchDeg{ -26.8f };  // [Menus] fWorkshopHandPitchDeg (about the wand's X; negative = face tilted up toward the eyes)
        bool        menuWorkshopHandCrop{ true };        // [Menus] bWorkshopHandCrop: hand panel = the Workshop stage only (no side bands)
        float       menuWorkshopCropTop{ 0.0f };         // [Menus] fWorkshopCropTop: the shown area starts this far down the 16:9 stage (0..0.9)
        float       wandGripRotDeg[3]{ -60.0f, 0.0f, 0.0f };  // [Weapon] (else [Hands]) fGripRotX/Y/ZDeg: OpenXR grip -> FO4VR wand
        float       wandGripOffsetCm[3]{ 0.0f, 0.0f, 0.0f };  // [Weapon] (else [Hands]) fGripOffsetX/Y/Z (right, forward, up; cm)
        float       menuVatsLabelScale{ 2.0f }; // [Menus] fVatsLabelScale: VATS overlay magnified about the selected target (1 = off)
        bool        menuVatsNoVignette{ true }; // [Menus] bVatsNoVignette: no flat VATS VignetteMenu over the live world
        bool        pipboyFloating{ false };    // [Pipboy] bFloating: FO4VR projected Pip-Boy in front of the head instead of the wrist
        // Round 7 (analysis/vr-terminals-pipboy.md): FO4VR's projected Pip-Boy (bAlwaysUseProjectedPipboy:VR) is what
        // bWristPipboy=0 gives now: the PA-dash screen mesh (Interface/Objects/PADashPipboyScreen.nif + world_projectedPipBoyScreen.nif
        // backdrop) on UprightHmdLagNode, 0.575 m wide, 0.65 m ahead, centre 0.07 m below the eyes, yaw dragged along only past a
        // 45 deg cone (fHmdRotationLagMaxDistance:VRUI), no scanlines. 0 = the game's own flat Pip-Boy (flat window).
        bool        pipboyProjected{ true };           // [Pipboy] bProjected
        float       pipboyProjWidthMeters{ 0.575f };   // [Pipboy] fProjectedWidthMeters
        float       pipboyProjDistanceMeters{ 0.65f }; // [Pipboy] fProjectedDistanceMeters
        float       pipboyProjHeightMeters{ -0.07f };  // [Pipboy] fProjectedHeightMeters
        float       pipboyProjLagConeDeg{ 45.0f };     // [Pipboy] fProjectedLagConeDeg (0 = old smooth follow, [PowerArmor] fPipboyFollowSeconds)
        bool        pipboyProjNoScanlines{ true };     // [Pipboy] bProjectedNoScanlines (FO4VR: EffectControl+0x40 = 0 when projected)
        // Round 7: terminals on their own monitor in the live world (FO4VR TerminalMenu::InitMenu), pointer via the engine pick.
        bool        menuTerminalInWorld{ true };       // [Menus] bTerminalInWorld (0 = the flat window with the panorama)
        // Round 7: every panel menu on its FO4VR world-space model's geometry (Pause, Container/Barter, LevelUp, Lockpicking,
        // SleepWait, SPECIAL, Book); 0 = only the crafting menus (bWorkbenchPanelFO4VR), the rest on [Cinema].
        bool        menuPanelFO4VR{ true };            // [Menus] bPanelGeometryFO4VR
        float       menuPauseWidthMeters{ 1.48f };     // [Menus] fPauseWidthMeters (world_projected_MainPause.nif arc)
        float       menuPauseDistanceMeters{ 0.78f };  // [Menus] fPauseDistanceMeters
        float       menuPauseHeightMeters{ -0.045f };  // [Menus] fPauseHeightMeters
        // Round 8: per-menu panel geometry override, [MenuGeometry] <MenuName>=width,distance,height (metres; height = centre
        // offset from eye level). Wins over every built-in geometry (FO4VR models, [Cinema]). Keys are menu names as the log
        // prints them (PauseMenu, DialogueMenu, ContainerMenu, LooksMenu, ...); lower-cased here.
        std::map<std::string, std::array<float, 3>> menuGeometry;
        // Round 8: the dialogue options panel (DialogueMenu, EDI) on FO4VR's dialogue band scale (world_Dialogue.nif on
        // UprightHmdLagNode shows the options strip u 0.18..0.83, v 0.69..0.83 of the movie on a 0.85 m arc 0.55 m ahead,
        // 0.18 m below the eyes -> the whole 16:9 movie 1.32 m wide at 0.55 m, centred at eye level).
        float       menuDialogueWidthMeters{ 1.32f };    // [MenuGeometry] DialogueMenu (code default)
        float       menuDialogueDistanceMeters{ 0.55f };
        float       menuDialogueHeightMeters{ 0.0f };
        // Round 8: menus forced onto the flat window (explicit opt-in), comma list, e.g. "LooksMenu,TerminalMenu".
        std::string menuWindowMenus;                     // [Menus] sWindowMenus
        // [Stereo] single-pass stereo PoC (StereoPoC.h; analysis/stereo-poc-plan.md). iMode: 0 off, 1 census,
        // 2 debug-PS stereo, 3 faithful stereo, 4 identity (diagnostic). Hooks are installed iff iMode != 0
        // or bInstallHooks=1 (lets the pipe switch modes at runtime).
        int         stereoMode{ 3 };            // [Stereo] iMode: 3 = single-pass stereo, ON by default (user rule: features on by default)
        bool        stereoInstallHooks{ true };  // [Stereo] bInstallHooks (needed by iMode 3)
        bool        stereoSkipUnconverted{ false };  // [Stereo] bSkipUnconverted
        bool        stereoOwnDepth{ true };     // [Stereo] bOwnDepth (stereo modes: no world z-prepass, GBuffer writes depth)
        bool        stereoPrepass{ true };      // [Stereo] bStereoPrepass (z-prepass converted too; preferred over bOwnDepth)
        bool        stereoFirstPerson{ true };  // [Stereo] bFirstPerson (1st-person camera = stereo camera too)
        int         stereoScope{ 1 };           // [Stereo] iScope (0 GBuffer writers + prepass, 1 every camera draw with a twin)
        bool        stereoTestEyes{ false };    // [Stereo] bTestEyes
        float       stereoTestIpdMeters{ 0.064f };  // [Stereo] fTestIpdMeters
        float       stereoTestHalfFovDeg{ 45.0f };  // [Stereo] fTestHalfFovDeg
        int         stereoLogEveryFrames{ 600 };    // [Stereo] iLogEveryFrames
        std::string stereoShaderDir;            // [Stereo] sShaderDir (default Data/F4SE/Plugins/fo4flatvr_stereo)
        // [Stereo] lighting/post round (stereo-poc-plan.md section 14): split draws + per-eye b12, cut list (each cut
        // is applied only while frames are rendered in stereo)
        bool        stereoSplit{ true };        // [Stereo] bSplitDraws
        bool        stereoSaoPerEye{ true };    // [Stereo] bSaoPerEye
        bool        stereoTaaPerEye{ true };    // [Stereo] bTaaPerEye (per-eye motion vectors + jitter for TAA)
        bool        stereoHiddenAreaMask{ true };  // [Stereo] bHiddenAreaMask (22 Nov: lens-hidden pixels stamped into depth)
        bool        stereoCutTaa{ false };      // [Stereo] bCutTaa (TAA off, FXAA on; off by default: per-eye TAA)
        bool        stereoCutMotionBlur{ false }; // [Stereo] bCutMotionBlur (per-eye motion blur since session 3)
        bool        stereoCutDof{ false };      // [Stereo] bCutDof (DOF runs per eye: blur kernels only)
        bool        stereoCutLensFlare{ false }; // [Stereo] bCutLensFlare (per-eye flare since session 3)
        bool        stereoCutGodrays{ true };   // [Stereo] bCutGodrays
        bool        stereoCutSsr{ false };      // [Stereo] bCutSsr (per-eye SSR: split prepass + eye-bounded ray march twin)
        bool        stereoCutHbao{ true };      // [Stereo] bCutHbao
        bool        stereoCutSao{ true };       // [Stereo] bCutSao: SAO off in stereo by default, as FO4VR forces AO off (0 = per-eye SAO)
        bool        stereoCutTiled{ false };    // [Stereo] bCutTiledLighting (tiled lighting per eye: StereoCS.pack twins, StereoTiled.inl)
        bool        stereoSaoPixelShader{ true }; // [Stereo] bSaoPixelShader
        float       stereoEyeFarScale{ 2.0f };
        bool        stereoKeepMonoDepthClip{ true }; // [Stereo] bKeepMonoDepthClip    // [Stereo] fEyeFarScale (eye far plane x mono far; 1 = clip z=w sky)
        // [Camera] step 0 (CameraTweaks.h)
        bool        camNoAimZoom{ true };          // [Camera] bNoAimZoom
        bool        camFirstPersonDialogue{ true }; // [Camera] bFirstPersonDialogue
        bool        camNoKillCams{ true };         // [Camera] bNoKillCams
        bool        camNoIdleOrbit{ true };        // [Camera] bNoIdleOrbit
        // [PowerArmor] (analysis/vr-power-armor.md; HudLayer.h, PipboyLayer.h)
        bool        paHudOnLayer{ false };          // [PowerArmor] bHudOnLayer: PA dash on the HUD layer, not in the world image
        bool        paHideVisorRain{ false };       // [PowerArmor] bHideVisorRain: no visor rain in the world view
        bool        paProjectedPipboy{ false };     // [PowerArmor] bProjectedPipboy: projected Pip-Boy in PA (pointer, no camera)
        float       paPipboyDistanceMeters{ 0.60f }; // [PowerArmor] fPipboyDistanceMeters
        float       paPipboyHeightMeters{ -0.12f };  // [PowerArmor] fPipboyHeightMeters
        float       paPipboyWidthMeters{ 0.50f };    // [PowerArmor] fPipboyWidthMeters
        float       paPipboyFollowSeconds{ 0.30f };  // [PowerArmor] fPipboyFollowSeconds (0 = head-locked)

        static Settings Load();
        // 20 Nov: the merged settings file every module reads (ConfigStore.h: fo4flatvr.ini < MCM < VR-panel previews)
        static std::string Path();
        static std::string UserPath();  // the user's fo4flatvr.ini itself (never written)
    };
}
