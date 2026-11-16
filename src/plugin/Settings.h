#pragma once
// Data/F4SE/Plugins/fo4flatvr.ini. Plain Win32 profile API: no extra dependency, re-readable.

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
        bool        poseSync{ true };         // [General] bPoseSync: game job threads wait for this frame's head pose (HeadPose.h)
        float       poseSyncTimeoutMs{ 50.0f };  // [General] fPoseSyncTimeoutMs: upper bound of that wait
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
        float       quadWidthMeters{ 3.0f };
        float       quadDistanceMeters{ 2.5f };
        float       quadHeightMeters{ 0.0f };
        int         recenterKey{ 0x78 };    // VK_F9
        int         toggleKey{ 0x79 };      // VK_F10: pause/resume VR submission
        bool        aimWithController{ true };  // [Controllers] fire + activation ray from the aim-hand controller
        bool        leftHanded{ false };        // [Controllers] aim hand = left controller
        bool        handLineOfFire{ true };       // [Controllers] bHandLineOfFire: gun lowering / iron-sight tests from the hand
        bool        suppressCameraRecoil{ true }; // [Controllers] bSuppressCameraRecoil: recoil does not turn the view
        bool        handAutoAimOverride{ true };  // [Controllers] bHandAutoAimOverride: hand pick beats the camera auto-aim actor
        bool        clampHandToCamera{ true };    // [Controllers] bClampHandToCamera: P not behind walls seen from the camera
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
        // secondary wand, ~0.40 m), not in front of the head. Placement in the off-hand grip frame.
        bool        menuWorkshopOnHand{ true };          // [Menus] bWorkshopOnHand
        float       menuWorkshopHandWidthMeters{ 0.40f }; // [Menus] fWorkshopHandWidthMeters
        float       menuWorkshopHandUpMeters{ 0.13f };   // [Menus] fWorkshopHandUpMeters (along the grip's +Y)
        float       menuWorkshopHandBackMeters{ 0.03f }; // [Menus] fWorkshopHandBackMeters (along the grip's +Z, toward the user)
        float       menuWorkshopHandPitchDeg{ -25.0f };  // [Menus] fWorkshopHandPitchDeg (negative = face tilted up toward the eyes)
        float       menuVatsLabelScale{ 2.0f }; // [Menus] fVatsLabelScale: VATS overlay magnified about the selected target (1 = off)
        bool        menuVatsNoVignette{ true }; // [Menus] bVatsNoVignette: no flat VATS VignetteMenu over the live world
        bool        pipboyFloating{ false };    // [Pipboy] bFloating: FO4VR projected Pip-Boy in front of the head instead of the wrist
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
        static std::string Path();
    };
}
