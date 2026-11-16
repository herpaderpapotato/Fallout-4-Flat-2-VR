#include "plugin/Plugin.h"

#include "plugin/CameraDirector.h"
#include "plugin/CameraTweaks.h"
#include "plugin/CommandChannel.h"
#include "plugin/Comfort.h"
#include "plugin/ComfortEffects.h"
#include "plugin/Dialogue.h"
#include "plugin/GameBridge.h"
#include "plugin/GameHud.h"
#include "plugin/GamePipboy.h"
#include "plugin/PipboyLayer.h"
#include "plugin/HudLayer.h"
#include "plugin/MenuPointer.h"
#include "plugin/GameWindow.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/HeadPose.h"
#include "plugin/Presenter.h"
#include "plugin/RenderSize.h"
#include "plugin/Roomscale.h"
#include "plugin/VirtualPad.h"
#include "plugin/Settings.h"
#include "plugin/StereoPoC.h"
#include "plugin/VatsWindow.h"
#include <cstdlib>
#include <string>

#include <F4SE/F4SE.h>
#include <REX/FModule.h>

namespace fo4flatvr
{
    namespace
    {
        Settings g_settings;

        void TryInstallPresenter(const char* a_when)
        {
            game::RenderHandles h;
            if (!game::GetRenderHandles(h)) {
                REX::WARN("{}: renderer not ready yet", a_when);
                return;
            }
            REX::INFO("{}: renderer {}x{} device {} swapchain {}", a_when, h.width, h.height, h.device, h.swapChain);
            present::Install(h.swapChain, h.device, h.context, g_settings);
            rendersize::HookResize(h.swapChain);
        }

        void OnMessage(F4SE::MessagingInterface::Message* a_msg)
        {
            switch (a_msg->type) {
            case F4SE::MessagingInterface::kPostLoad:
                TryInstallPresenter("PostLoad");
                break;
            case F4SE::MessagingInterface::kGameDataReady:
                TryInstallPresenter("GameDataReady");
                game::InstallCameraHook();
                head::SetTracking(g_settings.headTracking);
                head::SetPositionTracking(g_settings.positionTracking);
                game::SetUnitsPerMeter(g_settings.unitsPerMeter);
                game::SetHeightOffsetCm(g_settings.heightOffsetCm);
                game::SetHeadAuthority(g_settings.headAuthority, g_settings.eyeHeightTauS);
                head::SetPoseSync(g_settings.poseSync, g_settings.poseSyncTimeoutMs);
                REX::INFO("pose sync: {} (timeout {:.0f} ms)", g_settings.poseSync ? "on" : "off", g_settings.poseSyncTimeoutMs);
                if (g_settings.poseTrace) {  // [Debug] bPoseTrace: CSVs next to the plugin log
                    char*       up = nullptr;
                    std::size_t upn = 0;
                    std::string docs = ".";
                    if (_dupenv_s(&up, &upn, "USERPROFILE") == 0 && up) {
                        docs = std::string(up) + "\\Documents\\My Games\\Fallout4\\F4SE";
                        std::free(up);
                    }
                    head::TraceConfigure(true, g_settings.poseTraceFrames, g_settings.poseTraceFiles, docs);
                    REX::INFO("pose trace: continuous, {} frames per file, max {} files -> {}\\fo4flatvr_posetrace_NNN.csv",
                        g_settings.poseTraceFrames, g_settings.poseTraceFiles, docs);
                }
                game::camtweak::ApplySettings("GameDataReady");  // [Camera] step 0 (GMSTs are loaded now)
                break;
            case F4SE::MessagingInterface::kPostLoadGame:
            case F4SE::MessagingInterface::kNewGame:
                game::camtweak::ApplySettings(a_msg->type == F4SE::MessagingInterface::kNewGame ? "NewGame" : "PostLoadGame");
                comfx::OnGameLoaded();  // a camera shake restored from the save
                break;
            default:
                break;
            }
        }
    }

    bool OnLoad()
    {
        const auto ver = REX::FModule::GetExecutingModule().GetFileVersion();
        REX::INFO("fo4flatvr {} loading; game {}", F4SE::GetPluginVersion().string("."), ver.string("."));
        if (ver != F4SE::RUNTIME_1_11_240) {
            REX::ERROR("unsupported runtime {} (built for 1.11.240); staying inactive", ver.string("."));
            return true;  // load, do nothing: never take the game down over a version mismatch
        }

        g_settings = Settings::Load();
        REX::INFO("settings: {} enabled={} runtimeJson='{}' quad {}m @ {}m aimWithController={} leftHanded={}", Settings::Path(),
            g_settings.enabled, g_settings.xrRuntimeJson, g_settings.quadWidthMeters, g_settings.quadDistanceMeters,
            g_settings.aimWithController, g_settings.leftHanded);
        if (!g_settings.enabled) {
            return true;
        }

        // Render size: [Render] iWidth/iHeight x fRenderScale, plus the stereo seam guard band (RenderSize.h).
        const bool stereoAtLoad = g_settings.stereoMode == 2 || g_settings.stereoMode == 3;
        const auto renderPlan = rendersize::Resolve(g_settings.renderWidth, g_settings.renderHeight, g_settings.renderScale, stereoAtLoad,
            g_settings.stereoSeamAlign, g_settings.stereoSeamGuard);
        // Single-pass stereo PoC: configure before the renderer exists (the device hooks are installed from
        // the D3D11CreateDeviceAndSwapChain IAT hook, before the engine creates its shaders).
        {
            stereo::Config sc;
            sc.mode = static_cast<stereo::Mode>((g_settings.stereoMode >= 0 && g_settings.stereoMode <= 4) ? g_settings.stereoMode : 0);
            sc.skipUnconverted = g_settings.stereoSkipUnconverted;
            sc.ownDepth = g_settings.stereoOwnDepth;
            sc.stereoPrepass = g_settings.stereoPrepass;
            sc.firstPerson = g_settings.stereoFirstPerson;
            sc.scope = g_settings.stereoScope;
            sc.testEyes = g_settings.stereoTestEyes;
            sc.testIpdMeters = g_settings.stereoTestIpdMeters;
            sc.testHalfFovDeg = g_settings.stereoTestHalfFovDeg;
            sc.unitsPerMeter = g_settings.unitsPerMeter;
            sc.eyeWidth = renderPlan.eyeWidth;
            sc.eyeHeight = renderPlan.eyeHeight;
            sc.seamPad = renderPlan.seamPad;
            sc.renderWidth = renderPlan.width;
            sc.shaderDir = g_settings.stereoShaderDir;
            sc.logEveryFrames = g_settings.stereoLogEveryFrames;
            sc.split = g_settings.stereoSplit;
            sc.saoPerEye = g_settings.stereoSaoPerEye;
            sc.taaPerEye = g_settings.stereoTaaPerEye;
            sc.cutTaa = g_settings.stereoCutTaa;
            sc.cutMotionBlur = g_settings.stereoCutMotionBlur;
            sc.cutDof = g_settings.stereoCutDof;
            sc.cutLensFlare = g_settings.stereoCutLensFlare;
            sc.cutGodrays = g_settings.stereoCutGodrays;
            sc.cutSsr = g_settings.stereoCutSsr;
            sc.cutHbao = g_settings.stereoCutHbao;
            sc.cutSao = g_settings.stereoCutSao;
            sc.cutTiled = g_settings.stereoCutTiled;
            sc.saoPixelShader = g_settings.stereoSaoPixelShader;
            sc.eyeFarScale = g_settings.stereoEyeFarScale;
            sc.keepMonoDepthClip = g_settings.stereoKeepMonoDepthClip;
            stereo::Configure(sc);
            const bool hooks = sc.mode != stereo::Mode::kOff || g_settings.stereoInstallHooks;
            stereo::SetHooksEnabled(hooks);
            if (hooks) {
                stereo::InstallCameraHook();
            } else {
                REX::INFO("stereo off ([Stereo] iMode=0, bInstallHooks=0): no stereo hooks installed");
            }
        }
        // Must happen before the renderer initialises (it does so after plugin load).
        rendersize::Install(renderPlan.width, renderPlan.height);
        window::SetEngineRenderSize(renderPlan.width, renderPlan.height);
        window::InstallSmallWindow(g_settings.windowWidth, g_settings.windowHeight);
        pad::Install();
        game::camtweak::Configure({ g_settings.camNoAimZoom, g_settings.camFirstPersonDialogue, g_settings.camNoKillCams,
            g_settings.camNoIdleOrbit });  // [Camera] step 0: data writes only, applied from GameDataReady / Present
        rs::Install();  // roomscale/body yaw: PlayerCharacter vtable slot 0x125 (byte-checked; [Roomscale] keys)
        camdir::Install();  // camera step 1: director post-hook on TESCamera::Update (call 1026035, byte-checked; [Camera] keys)
        vatswin::Install();  // VATS camera window ([Menus] bVatsCameraWindow): call sites C383F6/B1E319/B1E9C1 (byte-checked)
        comfort::Install();  // C8 locomotion comfort ([Comfort] keys): data only, turns go through rs::UserTurn
        comfx::Install();    // no camera shake / view-warping imod effects ([Comfort] bNoCameraShake ...; byte-checked)
        gun::Install();     // floating gun (FO4VR baseline): call site C351F3 (byte-checked, chainable; [Weapon] keys)
        frikbridge::Install();  // FRIK-flat bridge: own chained hook at C351F3 + export API ([FRIK] bEnable); FRIK.dll active -> gun placement steps aside
        dlg::Install();     // VR dialogue: D-look call sites BB0039/BB0251/BB0376 (byte-checked; [Dialogue] keys)
        // Code patches go in now, before the game's main loop runs (no other thread can be inside them).
        if (g_settings.aimWithController) {
            game::ConfigureAimExtras(g_settings.handLineOfFire, g_settings.suppressCameraRecoil, g_settings.handAutoAimOverride,
                g_settings.clampHandToCamera);
            REX::INFO("aim hooks: {}", game::InstallAimHooks());
        } else {
            REX::INFO("controller aim off (bAimWithController=0): no aim hooks installed");
        }
        game::InstallNoSpread(g_settings.noSpread, g_settings.noSpreadPellets);  // [Weapon] bNoSpread: no cone of fire outside VATS (F1, shared with controller aim)
        // Separated HUD (D1): render-thread call-site hooks + crosshair vtable entry; off-switch = no patches.
        {
            hud::Config hc;
            hc.enabled = g_settings.separateHud;
            hc.distanceMeters = g_settings.hudDistanceMeters;
            hc.widthMeters = g_settings.hudWidthMeters;
            hc.heightMeters = g_settings.hudHeightMeters;
            hc.hideCrosshair = g_settings.hudHideCrosshair;
            hc.source = g_settings.hudSourceTarget;
            hc.premultiplied = g_settings.hudPremultiplied;
            hud::Configure(hc);
            // [PowerArmor] PA dash on the HUD layer / no visor rain: same H2 call site, only while the HUD layer is active.
            hud::ConfigurePowerArmor(g_settings.paHudOnLayer, g_settings.paHideVisorRain);
            REX::INFO("power armour: hudOnLayer={} hideVisorRain={} projectedPipboy={}", g_settings.paHudOnLayer, g_settings.paHideVisorRain,
                g_settings.paProjectedPipboy);
            // [Menus] bPanel: flat menus over the live world view (FlatScreenModel copied in H1, skipped in H2).
            hud::ConfigureMenuPanel(g_settings.menuPanel, g_settings.menuPanelSource, true);
            hud::SetMenuPreviewEnabled(g_settings.menuPanelPreview);  // crafting / container menus: 3D preview onto the panel
            hud::SetClearMenuTargets(g_settings.menuClearTargets);  // pause-menu decal on the HUD layer
            hud::SetClearHudTargets(g_settings.hudClearTargets);    // lingering HUD elements (pickup text, HIDDEN)
            if (g_settings.separateHud || g_settings.wristPipboy || g_settings.paProjectedPipboy || dlg::GetConfig().panel ||
                g_settings.menuPanel || g_settings.pipboyFloating) {
                // H1 is shared with the wrist Pip-Boy; with bSeparateHud=0 the HUD hooks only pass through.
                game::InstallHudHooks();
            } else {
                REX::INFO("separated HUD off (bSeparateHud=0): no HUD hooks installed");
            }
        }
        // Wrist Pip-Boy (D3 stage 1): open/close detours + RenderEffect(0xC) call site; off-switch = no patches.
        {
            pipboy::Config pc;
            pc.enabled = g_settings.wristPipboy;
            pc.widthMeters = g_settings.pipboyWidthMeters;
            for (int i = 0; i < 3; ++i) {
                pc.offset[i] = g_settings.pipboyOffset[i];
                pc.rotDeg[i] = g_settings.pipboyRotDeg[i];
            }
            pc.leftHanded = g_settings.leftHanded;
            pc.source = g_settings.pipboySourceTarget;
            pc.stopImod = g_settings.pipboyStopImod;
            pc.cursorDot = g_settings.pipboyCursorDot;
            pc.tint = g_settings.pipboyTint;
            pc.projected = g_settings.paProjectedPipboy;  // [PowerArmor] projected Pip-Boy uses the same hooks
            pc.projDistanceMeters = g_settings.paPipboyDistanceMeters;
            pc.projHeightMeters = g_settings.paPipboyHeightMeters;
            pc.projWidthMeters = g_settings.paPipboyWidthMeters;
            pc.projFollowSeconds = g_settings.paPipboyFollowSeconds;
            pc.floating = g_settings.pipboyFloating;  // [Pipboy] bFloating: FO4VR projected Pip-Boy outside power armour too
            pipboy::Configure(pc);
            if (g_settings.wristPipboy || g_settings.paProjectedPipboy || g_settings.pipboyFloating) {
                game::InstallPipboyHooks();
            } else {
                REX::INFO("wrist Pip-Boy off (bWristPipboy=0): no Pip-Boy hooks installed");
            }
        }

        // Menu laser pointer (D2): no code patches (Scaleform events from an F4SE UI task); off-switch = inactive.
        {
            menuptr::Config mc;
            mc.enabled = g_settings.menuPointer;
            mc.leftHanded = g_settings.leftHanded;
            mc.cursorDot = g_settings.menuCursorDot;
            menuptr::Configure(mc);
            REX::INFO("menu pointer {} (hand {}, cursor {})", mc.enabled ? "on" : "off", mc.leftHanded ? "left" : "right", mc.cursorDot ? 1 : 0);
        }

        const auto messaging = F4SE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(OnMessage)) {
            REX::ERROR("could not register F4SE messaging listener");
            return true;
        }
        command::Start();
        return true;
    }
}
