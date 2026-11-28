#include "plugin/Plugin.h"

#include "plugin/CameraDirector.h"
#include "plugin/CameraTweaks.h"
#include "plugin/CommandChannel.h"
#include "plugin/Comfort.h"
#include "plugin/ConfigUI.h"
#include "plugin/CrashLog.h"
#include "plugin/ModsResetGuard.h"
#include "plugin/PanelHost.h"
#include "plugin/ConfigStore.h"
#include "plugin/ComfortEffects.h"
#include "plugin/ControllerBindings.h"
#include "plugin/QuickMenu.h"
#include "plugin/Dialogue.h"
#include "plugin/GameBridge.h"
#include "plugin/GameHud.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/GamePipboy.h"
#include "plugin/PipboyLayer.h"
#include "plugin/HudLayer.h"
#include "plugin/MenuPointer.h"
#include "plugin/PointerBeam.h"
#include "plugin/GameWindow.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/FaceDiag.h"
#include "plugin/Dlss.h"
#include "plugin/HeadPose.h"
#include "plugin/Presenter.h"
#include "plugin/RenderSize.h"
#include "plugin/Roomscale.h"
#include "plugin/VirtualPad.h"
#include "plugin/Settings.h"
#include "plugin/Scope.h"
#include "plugin/StereoPoC.h"
#include "plugin/VatsWindow.h"
#include "plugin/ThreadQueues.h"
#include "plugin/WorkshopHand.h"
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <string>

#include <F4SE/F4SE.h>
#include <REX/FModule.h>

namespace fo4flatvr
{
    namespace
    {
        Settings g_settings;

        // ---- 20 Nov (live settings, ConfigStore.h; analysis/vr-config-ui.md section 9) ----
        // The keys of Settings.h whose module has a runtime setter are re-applied when they change (any layer: fo4flatvr.ini,
        // MCM, a VR-panel preview). Only changed keys / sections are applied, so a pipe override of another key stays.
        // Keys that need a restart are named in the log when they change (the reason is also in config_spec.py / the panel).
        struct RestartKey
        {
            const char* sec;
            const char* key;
            const char* why;
        };
        constexpr RestartKey kRestartKeys[] = {
            { "General", "bEnabled", "the plugin installs nothing when off" },
            { "General", "bMono", "start-up view mode (switch at runtime with the pipe 'mode')" },
            { "General", "bAFR", "start-up view mode" },
            { "General", "bPoseSync", "head-path timing (pose sync between the game's job threads and Present)" },
            { "General", "fPoseSyncTimeoutMs", "head-path timing" },
            { "Debug", "bPoseTrace", "diagnostic trace writer started once (the pipe 'htrace' arms one at runtime)" },
            { "Debug", "iPoseTraceFrames", "diagnostic trace writer" },
            { "Debug", "iPoseTraceFiles", "diagnostic trace writer" },
            { "Render", "iWidth", "the engine sizes its render targets from the swapchain once (Renderer::Init)" },
            { "Render", "iHeight", "the engine sizes its render targets from the swapchain once (Renderer::Init)" },
            { "Render", "fRenderScale", "the engine sizes its render targets from the swapchain once (Renderer::Init)" },
            { "OpenXR", "sRuntimeJson", "XR_RUNTIME_JSON is read when the OpenXR instance is created" },
            { "OpenXR", "bPreferStageSpace", "the reference space is created with the OpenXR session" },
            { "Roomscale", "bHeadAuthority", "first-person camera source (head path)" },
            { "Roomscale", "fEyeHeightSmoothS", "first-person eye-height filter (head path)" },
            { "Stereo", "bInstallHooks", "device / context hooks go in before the engine creates its shaders" },
            { "Stereo", "sShaderDir", "the stereo shader twins are built when the engine creates its shaders" },
            { "Stereo", "iSeamAlign", "part of the render size" },
            { "Stereo", "iSeamGuard", "part of the render size" },
            { "Camera", "bCameraDirector", "the director hook on TESCamera::Update (head-path pose wait) is installed at load" },
            { "Camera", "bOwnHeading", "first-person yaw owner (head path; A/B with the pipe 'rs cfg own')" },
            { "Camera", "bGateVats", "VATS start/stop detours are installed in pairs at load" },
            { "Camera", "bVatsKeepCamera", "VATS ChangeState detour installed at load" },
            { "Menus", "bVatsLiveWorld", "VATS gates installed at load" },
            { "Menus", "bVatsCameraWindow", "VATS window call sites and gates installed at load" },
            { "Menus", "bVatsNoImod", "VATS ChangeState detour installed at load" },
            { "Weapon", "bFloatingGun", "W1 / A1 patches at load" },
            { "Weapon", "bHideArms", "A1 patch at load" },
            { "Weapon", "bStableRig", "first-person rig fit (head path)" },
            { "Weapon", "sWeaponOffsetsDir", "weapon offset JSONs loaded once" },
            { "Weapon", "sControllerModelRight", "controller model loaded once" },
            { "Weapon", "sControllerModelLeft", "controller model loaded once" },
            { "FRIK", "bEnable", "FRIK.dll installs its hooks and patches at its load" },
            { "FRIK", "bBody", "FRIK.dll installs its body hooks / patches and builds its skeleton at load" },
        };

        std::mutex                    g_pipLock;
        std::optional<pipboy::Config> g_pipPending;  // a Pip-Boy mode / geometry change waits until no Pip-Boy is open

        pipboy::Config PipboyConfigOf(const Settings& a_s)
        {
            pipboy::Config pc;
            pc.enabled = a_s.wristPipboy;
            pc.widthMeters = a_s.pipboyWidthMeters;
            for (int i = 0; i < 3; ++i) {
                pc.offset[i] = a_s.pipboyOffset[i];
                pc.rotDeg[i] = a_s.pipboyRotDeg[i];
            }
            pc.leftHanded = a_s.leftHanded;
            pc.source = a_s.pipboySourceTarget;
            pc.stopImod = a_s.pipboyStopImod;
            pc.cursorDot = a_s.pipboyCursorDot;
            pc.tint = a_s.pipboyTint;
            pc.projected = a_s.paProjectedPipboy;  // [PowerArmor] projected Pip-Boy uses the same hooks
            pc.projDistanceMeters = a_s.paPipboyDistanceMeters;
            pc.projHeightMeters = a_s.paPipboyHeightMeters;
            pc.projWidthMeters = a_s.paPipboyWidthMeters;
            pc.projFollowSeconds = a_s.paPipboyFollowSeconds;
            pc.floating = a_s.pipboyFloating;  // [Pipboy] bFloating, or bWristPipboy=0 + bProjected=1 (round 7)
            pc.floatWidthMeters = a_s.pipboyProjWidthMeters;
            pc.floatDistanceMeters = a_s.pipboyProjDistanceMeters;
            pc.floatHeightMeters = a_s.pipboyProjHeightMeters;
            pc.lagConeDeg = a_s.pipboyProjLagConeDeg;
            pc.noScanlines = a_s.pipboyProjNoScanlines;
            return pc;
        }

        hud::Config HudConfigOf(const Settings& a_s)
        {
            hud::Config hc;
            hc.enabled = a_s.separateHud;
            hc.distanceMeters = a_s.hudDistanceMeters;
            hc.widthMeters = a_s.hudWidthMeters;
            hc.heightMeters = a_s.hudHeightMeters;
            hc.hideCrosshair = a_s.hudHideCrosshair;
            hc.source = a_s.hudSourceTarget;
            hc.premultiplied = a_s.hudPremultiplied;
            return hc;
        }

        // main tick: a Pip-Boy change is applied once no Pip-Boy is open (each open picks wrist / projected / the game's path)
        void PollPendingLive()
        {
            std::optional<pipboy::Config> pc;
            {
                std::scoped_lock l(g_pipLock);
                if (!g_pipPending || game::MenuOpen("PipboyMenu")) return;
                pc.swap(g_pipPending);
            }
            pipboy::Configure(*pc);
            REX::INFO("Pip-Boy: settings applied (live): wrist={} floating={} projectedPA={} width {:.3f} m", pc->enabled ? 1 : 0, pc->floating ? 1 : 0,
                pc->projected ? 1 : 0, pc->widthMeters);
        }

        void ApplyStereoLive(const Settings& a_s, const cfgstore::Changes& a_ch)
        {
            auto        c = stereo::CurrentConfig();  // keeps the render plan, the shader dir and the pipe-only diagnostics
            std::string what;
            auto B = [&](const char* k, bool& v, bool n) {
                if (a_ch.Has("Stereo", k)) v = n, what += std::format(" {}={}", k, n ? 1 : 0);
            };
            auto F = [&](const char* k, float& v, float n) {
                if (a_ch.Has("Stereo", k)) v = n, what += std::format(" {}={}", k, n);
            };
            auto I = [&](const char* k, int& v, int n) {
                if (a_ch.Has("Stereo", k)) v = n, what += std::format(" {}={}", k, n);
            };
            B("bSkipUnconverted", c.skipUnconverted, a_s.stereoSkipUnconverted);
            B("bOwnDepth", c.ownDepth, a_s.stereoOwnDepth);
            B("bStereoPrepass", c.stereoPrepass, a_s.stereoPrepass);
            B("bFirstPerson", c.firstPerson, a_s.stereoFirstPerson);
            I("iScope", c.scope, a_s.stereoScope);
            B("bTestEyes", c.testEyes, a_s.stereoTestEyes);
            F("fTestIpdMeters", c.testIpdMeters, a_s.stereoTestIpdMeters);
            F("fTestHalfFovDeg", c.testHalfFovDeg, a_s.stereoTestHalfFovDeg);
            I("iLogEveryFrames", c.logEveryFrames, a_s.stereoLogEveryFrames);
            B("bSplitDraws", c.split, a_s.stereoSplit);
            B("bSaoPerEye", c.saoPerEye, a_s.stereoSaoPerEye);
            B("bTaaPerEye", c.taaPerEye, a_s.stereoTaaPerEye);
            B("bHiddenAreaMask", c.hiddenAreaMask, a_s.stereoHiddenAreaMask);
            B("bCutTaa", c.cutTaa, a_s.stereoCutTaa);
            B("bCutMotionBlur", c.cutMotionBlur, a_s.stereoCutMotionBlur);
            B("bCutDof", c.cutDof, a_s.stereoCutDof);
            B("bCutLensFlare", c.cutLensFlare, a_s.stereoCutLensFlare);
            B("bCutGodrays", c.cutGodrays, a_s.stereoCutGodrays);
            B("bCutSsr", c.cutSsr, a_s.stereoCutSsr);  // the SSR flag + ClearAllRenderPasses go to the main-idle queue (StereoCuts)
            B("bCutHbao", c.cutHbao, a_s.stereoCutHbao);
            B("bCutSao", c.cutSao, a_s.stereoCutSao);
            B("bCutTiledLighting", c.cutTiled, a_s.stereoCutTiled);
            B("bSaoPixelShader", c.saoPixelShader, a_s.stereoSaoPixelShader);
            B("bKeepMonoDepthClip", c.keepMonoDepthClip, a_s.stereoKeepMonoDepthClip);
            if (a_ch.Has("Stereo", "fEyeFarScale")) {
                c.eyeFarScale = a_s.stereoEyeFarScale;
                what += std::format(" fEyeFarScale={}", a_s.stereoEyeFarScale);
            }
            if (a_ch.Has("General", "fUnitsPerMeter")) {
                c.unitsPerMeter = a_s.unitsPerMeter;
                what += std::format(" unitsPerMeter={}", a_s.unitsPerMeter);
            }
            if (a_ch.Has("Stereo", "iMode")) {
                const int m = (a_s.stereoMode >= 0 && a_s.stereoMode <= 4) ? a_s.stereoMode : 0;
                if (stereo::HooksEnabled()) {
                    c.mode = static_cast<stereo::Mode>(m);
                    what += std::format(" iMode={}", m);
                } else {
                    REX::WARN("stereo: [Stereo] iMode={} needs a restart: the stereo hooks were not installed at load (iMode=0, bInstallHooks=0)", m);
                }
            }
            if (what.empty()) return;
            stereo::Configure(c);
            REX::INFO("stereo: settings applied (live):{}", what);
        }

        // Present thread (cfgstore::DispatchChanges at the start of the Presenter's frame)
        void ApplyLive(const cfgstore::Changes& a_ch)
        {
            Settings s = Settings::Load();
            if (!s.wristPipboy && s.pipboyProjected) {
                s.pipboyFloating = true;  // as at load (round 7)
            }
            std::string what;
            auto has = [&](const char* a_sec, const char* a_key) { return a_ch.Has(a_sec, a_key); };
            auto any = [&](const char* a_sec) { return a_ch.Any(a_sec); };
            const bool hand = has("Controllers", "bLeftHanded");
            if (has("General", "bHeadTracking")) head::SetTracking(s.headTracking), what += " headTracking";
            if (has("General", "bPositionTracking")) head::SetPositionTracking(s.positionTracking), what += " positionTracking";
            if (has("OpenXR", "bRecentreOnPoseJump")) head::SetRecentreOnPoseJump(s.recentreOnPoseJump ? 0.5f : 0.0f), what += " recentreOnPoseJump";
            if (has("General", "fUnitsPerMeter")) {
                // head offset scale (GameBridge reads it per camera update, as fHeightOffsetCm): when the head is read is unchanged
                game::SetUnitsPerMeter(s.unitsPerMeter);
                what += std::format(" unitsPerMeter={}", s.unitsPerMeter);
            }
            if (has("Roomscale", "bSneakHeightStep")) game::SetSneakHeightStep(s.sneakHeightStep), what += " sneakHeightStep";
            if (any("Stereo") || has("General", "fUnitsPerMeter")) ApplyStereoLive(s, a_ch);
            if (has("Camera", "bNoAimZoom") || has("Camera", "bFirstPersonDialogue") || has("Camera", "bNoKillCams") || has("Camera", "bNoIdleOrbit")) {
                game::camtweak::Configure({ s.camNoAimZoom, s.camFirstPersonDialogue, s.camNoKillCams, s.camNoIdleOrbit });
                what += " cameraTweaks";
            }
            if (has("Controllers", "bAimWithController")) {
                game::SetAimEnabled(s.aimWithController);
                what += std::format(" aim={}", s.aimWithController ? 1 : 0);
            }
            if (has("Controllers", "bHandLineOfFire") || has("Controllers", "bSuppressCameraRecoil") || has("Controllers", "bHandAutoAimOverride") ||
                has("Controllers", "bClampHandToCamera")) {
                game::ConfigureAimExtras(s.handLineOfFire, s.suppressCameraRecoil, s.handAutoAimOverride, s.clampHandToCamera);
                what += " aimExtras";
            }
            if (has("Controllers", "bAimThrowsWithController")) game::ConfigureAimThrows(s.aimThrowsWithController), what += " aimThrows";
            if (has("Weapon", "bNoSpread") || has("Weapon", "bNoSpreadPellets")) game::SetNoSpread(s.noSpread, s.noSpreadPellets), what += " noSpread";
            if (any("HUD") || hand) {
                hud::Configure(HudConfigOf(s));
                hud::SetClearHudTargets(s.hudClearTargets);
                hud::LoadSplitConfig();  // FO4VR HUD split (incl. its handedness)
                game::ReloadHudSettings();
                what += " hud";
            }
            if (any("PowerArmor")) hud::ConfigurePowerArmor(s.paHudOnLayer, s.paHideVisorRain), what += " powerArmorHud";
            if (has("Menus", "bPanel") || has("Menus", "iPanelSource")) hud::ConfigureMenuPanel(s.menuPanel, s.menuPanelSource, true), what += " menuPanel";
            if (has("Menus", "bPanelPreview")) hud::SetMenuPreviewEnabled(s.menuPanelPreview), what += " panelPreview";
            if (has("Menus", "bClearMenuTargets")) hud::SetClearMenuTargets(s.menuClearTargets), what += " clearMenuTargets";
            if (has("Menus", "bTerminalInWorld")) game::SetWorldScreenEnabled(s.menuTerminalInWorld), what += " terminalInWorld";
            if (has("Menus", "bPointer") || has("Menus", "bCursorDot") || hand) {
                menuptr::Config mc;
                mc.enabled = s.menuPointer;
                mc.leftHanded = s.leftHanded;
                mc.cursorDot = s.menuCursorDot;
                menuptr::Configure(mc);
                what += " menuPointer";
            }
            if (any("Menus")) game::ReloadPointerSettings();
            if (any("Pipboy") || any("PowerArmor") || hand) {
                std::scoped_lock l(g_pipLock);
                g_pipPending = PipboyConfigOf(s);  // applied by the main tick once no Pip-Boy is open
                what += " pipboy (when closed)";
            }
            if (any("Beam") || hand) beam::LoadAndConfigure(Settings::Path(), s.leftHanded), what += " beam";
            if (any("Window")) what += " " + window::ResizeClient(s.windowWidth, s.windowHeight);
            if (any("Cinema") || any("Keys") || any("Menus") || any("MenuGeometry") || any("Controllers") || has("General", "fUnitsPerMeter") ||
                has("Render", "fMaxFov")) {
                present::ApplySettings(s);
                what += " presenter";
            }
            for (const auto& r : kRestartKeys) {
                if (has(r.sec, r.key)) REX::WARN("settings: [{}] {} changed: applies after a restart ({})", r.sec, r.key, r.why);
            }
            if (!what.empty()) REX::INFO("settings: applied live:{}", what);
        }

        void TryInstallPresenter(const char* a_when)
        {
            game::RenderHandles h;
            if (!game::GetRenderHandles(h)) {
                REX::WARN("{}: renderer not ready yet", a_when);
                return;
            }
            REX::INFO("{}: renderer {}x{} device {} swapchain {}", a_when, h.width, h.height, h.device, h.swapChain);
            present::Install(h.swapChain, h.device, h.context, g_settings);
            window::SetGameWindow(h.hwnd);  // 20 Nov: live [Window] size
            window::InstallFocusKeeper(h.hwnd);  // 26 Nov perf pass: no out-of-focus frame-rate cap while in VR
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
                crashlog::Install("game data ready");  // again: the game / other plugins may have set their own filter since
                game::InstallCameraHook();
                head::SetTracking(g_settings.headTracking);
                head::SetPositionTracking(g_settings.positionTracking);
                head::SetRecentreOnPoseJump(g_settings.recentreOnPoseJump ? 0.5f : 0.0f);
                REX::INFO("head: start-up pose jump re-anchor {} ([OpenXR] bRecentreOnPoseJump; > 0.5 m between valid poses, until the first recentre)",
                    g_settings.recentreOnPoseJump ? "on" : "off");
                game::SetUnitsPerMeter(g_settings.unitsPerMeter);
                game::SetHeightOffsetCm(g_settings.heightOffsetCm);
                game::SetHeadAuthority(g_settings.headAuthority, g_settings.eyeHeightTauS);
                game::SetSneakHeightStep(g_settings.sneakHeightStep);
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
            sc.hiddenAreaMask = g_settings.stereoHiddenAreaMask;
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
        quickmenu::Install();  // 28 Nov: FO4VR quick menu movie (FavoritesMenu LoadMovie call 104994D; QuickMenu.h)
        bindings::InstallGlyphs();  // 20 Nov: glyphs name the Touch control that does each action (call sites B71C2C..B72238, A504CC; byte-checked)
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
        modsreset::Install();  // 26 Nov: hold the mod-change reset prompt while character creation is open ([General] bModsResetGuard)
        facediag::Install();  // FaceGen customization render guard: call sites 6DF69B/6EC07E (byte-checked; [Stereo] bFaceGenOffscreen)
        dlss::Install();      // 20 Nov: per-eye DLSS + neural rendering at the TAA draw ([DLSS], [NeuralRendering]; Dlss.h)
        scope::Install();     // 27 Nov: FO4VR raise-to-eye scopes ([Scope]; Scope.h): no code patches, a main tick
        // Code patches go in now, before the game's main loop runs (no other thread can be inside them).
        // 20 Nov (live settings): the aim hooks are installed whatever bAimWithController says and pass through while it is
        // off (every hook checks the aim switch), so it and the follow-up switches apply live
        game::ConfigureAimExtras(g_settings.handLineOfFire, g_settings.suppressCameraRecoil, g_settings.handAutoAimOverride,
            g_settings.clampHandToCamera);
        game::ConfigureAimThrows(g_settings.aimThrowsWithController);  // grenades/mines follow the hand (F1 + throw preview)
        REX::INFO("aim hooks: {}", game::InstallAimHooks());
        game::SetAimEnabled(g_settings.aimWithController);
        wshand::Install();  // 21 Nov: workshop placement / highlight along the primary hand ray (FO4VR; [Controllers] bWorkshopHandPointer)
        if (!g_settings.aimWithController) {
            REX::INFO("controller aim off (bAimWithController=0): aim hooks pass through (live switch)");
        }
        game::InstallNoSpread(g_settings.noSpread, g_settings.noSpreadPellets);  // [Weapon] bNoSpread: no cone of fire outside VATS (F1, shared with controller aim)
        // Separated HUD (D1): render-thread call-site hooks + crosshair vtable entry; off-switch = no patches.
        {
            hud::Configure(HudConfigOf(g_settings));
            hud::LoadSplitConfig();  // 20 Nov: FO4VR HUD split (meters on the aim hand, compass on the chin panel), [HUD] keys
            // [PowerArmor] PA dash on the HUD layer / no visor rain: same H2 call site, only while the HUD layer is active.
            hud::ConfigurePowerArmor(g_settings.paHudOnLayer, g_settings.paHideVisorRain);
            REX::INFO("power armour: hudOnLayer={} hideVisorRain={} projectedPipboy={}", g_settings.paHudOnLayer, g_settings.paHideVisorRain,
                g_settings.paProjectedPipboy);
            // [Menus] bPanel: flat menus over the live world view (FlatScreenModel copied in H1, skipped in H2).
            hud::ConfigureMenuPanel(g_settings.menuPanel, g_settings.menuPanelSource, true);
            hud::SetMenuPreviewEnabled(g_settings.menuPanelPreview);  // crafting / container menus: 3D preview onto the panel
            hud::SetClearMenuTargets(g_settings.menuClearTargets);  // pause-menu decal on the HUD layer
            hud::SetClearHudTargets(g_settings.hudClearTargets);    // lingering HUD elements (pickup text, HIDDEN)
            // round 7: bWristPipboy=0 + [Pipboy] bProjected=1 = FO4VR's projected Pip-Boy (same capture/hooks as bFloating)
            if (!g_settings.wristPipboy && g_settings.pipboyProjected) {
                g_settings.pipboyFloating = true;
            }
            // H1 is shared with the wrist Pip-Boy; with every user off the HUD hooks only pass through. 20 Nov (live settings):
            // installed always, so bSeparateHud / bPanel / the Pip-Boy modes / the dialogue panel can be switched on while running.
            game::InstallHudHooks();
        }
        game::InstallAspectGapHooks();  // 28 Nov (menus agent): aspect gap - HUD markers / VATS labels through the letterbox ([Menus] bAspectGapFix)
        // Wrist Pip-Boy (D3 stage 1): open/close detours + RenderEffect(0xC) call site; off-switch = no patches.
        {
            pipboy::Configure(PipboyConfigOf(g_settings));
            // 20 Nov (live settings): installed always; each open picks wrist / projected / the game's own path from the config
            game::InstallPipboyHooks();
            if (g_settings.wristPipboy || g_settings.paProjectedPipboy || g_settings.pipboyFloating) {
                REX::INFO("Pip-Boy: {} (bWristPipboy={} bProjected={} bFloating effective={})",
                    g_settings.pipboyFloating ? "FO4VR projected screen in front of you" : "wrist", g_settings.wristPipboy ? 1 : 0,
                    g_settings.pipboyProjected ? 1 : 0, g_settings.pipboyFloating ? 1 : 0);
            } else {
                REX::INFO("Pip-Boy: the game's own (bWristPipboy=0, bProjected=0): hooks installed, the game's path until a mode is on");
            }
            game::SetWorldScreenEnabled(g_settings.menuTerminalInWorld);
            REX::INFO("terminals: {}", g_settings.menuTerminalInWorld ? "on their own monitor in the live world (FO4VR; [Menus] bTerminalInWorld=1)"
                                                                       : "flat window ([Menus] bTerminalInWorld=0)");
        }

        // Menu laser pointer (D2): no code patches (Scaleform events from an F4SE UI task); off-switch = inactive.
        {
            menuptr::Config mc;
            mc.enabled = g_settings.menuPointer;
            mc.leftHanded = g_settings.leftHanded;
            mc.cursorDot = g_settings.menuCursorDot;
            menuptr::Configure(mc);
            // Pointer beam ([Beam]): the visible laser + end dot for every pointer (menus, Pip-Boy, FO4VR's workshop/command laser)
            beam::LoadAndConfigure(Settings::Path(), g_settings.leftHanded);
            REX::INFO("menu pointer {} (hand {}, cursor {})", mc.enabled ? "on" : "off", mc.leftHanded ? "left" : "right", mc.cursorDot ? 1 : 0);
        }
        // 20 Nov: VR settings panel ([ConfigUI] bEnabled; ConfigUI.h) - no hooks, drawn and submitted from the Presenter
        crashlog::Install("load");  // 26 Nov: crash report + minidump ([General] bCrashLog; CrashLog.h)
        configui::Install();
        panelhost::Install();  // 26 Nov: other plugins' VR panels ([PanelHost] bEnabled; PanelHost.h, export FO4FLATVR_GetPanelHost)
        // 20 Nov (threading audit, analysis/threading.md): our own main-thread queue at OnIdle C34794 (Console::ExecuteQueuedCommand
        // call, after WaitForThreads / UI::AdvanceMenus) replaces F4SE AddTask / AddUITask (which run on whichever thread drains
        // them, job threads included). The polls below changed scene-graph / actor state at Present while the job threads ran
        // the player update; they run there now. [General] bMainThreadQueue=0 = the old behaviour.
        tq::AddMainTick(&dlg::Tick, "dlg::Tick");
        tq::AddMainTick(&camdir::PollWorkbenchBody, "camdir::PollWorkbenchBody");
        tq::AddMainTick(&camdir::PollHeadHide, "camdir::PollHeadHide");
        tq::AddMainTick(&game::AimTick, "game::AimTick");
        tq::AddMainTick(&game::FitPanelViewportTick, "game::FitPanelViewportTick");  // 28 Nov: panel mod menus at the flat display's aspect
        tq::AddMainTick(&PollPendingLive, "live settings");  // 20 Nov: deferred live changes (Pip-Boy mode while it is closed)
        tq::Install();
        // 20 Nov (live settings, ConfigStore.h): the Settings.h keys with a runtime setter (others subscribe in their modules)
        cfgstore::Subscribe("settings",
            "General,Debug,Render,OpenXR,Cinema,Keys,Controllers,Weapon/bNoSpread,Weapon/bNoSpreadPellets,Weapon/bFloatingGun,Weapon/bHideArms,"
            "Weapon/bStableRig,Weapon/sWeaponOffsetsDir,Weapon/sControllerModelRight,Weapon/sControllerModelLeft,HUD,Pipboy,Menus,MenuGeometry,"
            "Stereo,Camera/bNoAimZoom,Camera/bFirstPersonDialogue,Camera/bNoKillCams,Camera/bNoIdleOrbit,Camera/bCameraDirector,Camera/bOwnHeading,"
            "Camera/bGateVats,Camera/bVatsKeepCamera,Roomscale/bSneakHeightStep,Roomscale/bHeadAuthority,Roomscale/fEyeHeightSmoothS,PowerArmor,"
            "Beam,Window,FRIK/bEnable,FRIK/bBody",
            &ApplyLive);

        const auto messaging = F4SE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(OnMessage)) {
            REX::ERROR("could not register F4SE messaging listener");
            return true;
        }
        command::Start();
        return true;
    }
}
