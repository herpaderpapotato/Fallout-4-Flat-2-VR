#include "plugin/Settings.h"

#include <Windows.h>  // NOMINMAX / WIN32_LEAN_AND_MEAN come from xrcore's public defines

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fo4flatvr
{
    namespace
    {
        std::string ReadString(const std::string& a_path, const char* a_section, const char* a_key, const char* a_default)
        {
            char buf[1024]{};
            ::GetPrivateProfileStringA(a_section, a_key, a_default, buf, sizeof(buf), a_path.c_str());
            return buf;
        }

        float ReadFloat(const std::string& a_path, const char* a_section, const char* a_key, float a_default)
        {
            const auto s = ReadString(a_path, a_section, a_key, "");
            if (s.empty()) {
                return a_default;
            }
            char*       end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            return end != s.c_str() ? v : a_default;
        }

        int ReadInt(const std::string& a_path, const char* a_section, const char* a_key, int a_default)
        {
            return static_cast<int>(::GetPrivateProfileIntA(a_section, a_key, a_default, a_path.c_str()));
        }
    }

    std::string Settings::Path()
    {
        // Relative to the game's working directory (the install root), like F4SE's own paths.
        return std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr.ini").string();
    }

    Settings Settings::Load()
    {
        Settings   s;
        const auto p = Path();
        s.enabled = ReadInt(p, "General", "bEnabled", 1) != 0;
        s.headTracking = ReadInt(p, "General", "bHeadTracking", 1) != 0;
        s.positionTracking = ReadInt(p, "General", "bPositionTracking", 1) != 0;
        s.mono = ReadInt(p, "General", "bMono", 1) != 0;
        s.afr = ReadInt(p, "General", "bAFR", 0) != 0;
        s.renderWidth = ReadInt(p, "Render", "iWidth", 2880);
        s.windowWidth = ReadInt(p, "Window", "iWidth", s.windowWidth);
        s.windowHeight = ReadInt(p, "Window", "iHeight", s.windowHeight);
        s.maxRenderFov = ReadFloat(p, "Render", "fMaxFov", 0.0f);
        s.heightOffsetCm = ReadFloat(p, "General", "fHeightOffsetCm", 0.0f);
        s.headAuthority = ReadFloat(p, "Roomscale", "bHeadAuthority", 1.0f) != 0.0f;
        s.eyeHeightTauS = ReadFloat(p, "Roomscale", "fEyeHeightSmoothS", s.eyeHeightTauS);
        s.poseSync = ReadInt(p, "General", "bPoseSync", 1) != 0;
        s.poseSyncTimeoutMs = ReadFloat(p, "General", "fPoseSyncTimeoutMs", s.poseSyncTimeoutMs);
        s.poseTrace = ReadInt(p, "Debug", "bPoseTrace", 0) != 0;
        s.poseTraceFrames = ReadInt(p, "Debug", "iPoseTraceFrames", s.poseTraceFrames);
        s.poseTraceFiles = ReadInt(p, "Debug", "iPoseTraceFiles", s.poseTraceFiles);
        s.renderHeight = ReadInt(p, "Render", "iHeight", 1620);
        s.renderScale = ReadFloat(p, "Render", "fRenderScale", s.renderScale);
        s.stereoSeamAlign = ReadInt(p, "Stereo", "iSeamAlign", s.stereoSeamAlign);
        s.stereoSeamGuard = ReadInt(p, "Stereo", "iSeamGuard", s.stereoSeamGuard);
        s.unitsPerMeter = ReadFloat(p, "General", "fUnitsPerMeter", s.unitsPerMeter);
        s.xrRuntimeJson = ReadString(p, "OpenXR", "sRuntimeJson", "");
        s.preferStageSpace = ReadInt(p, "OpenXR", "bPreferStageSpace", 0) != 0;
        s.quadWidthMeters = ReadFloat(p, "Cinema", "fWidthMeters", s.quadWidthMeters);
        s.quadDistanceMeters = ReadFloat(p, "Cinema", "fDistanceMeters", s.quadDistanceMeters);
        s.quadHeightMeters = ReadFloat(p, "Cinema", "fHeightMeters", s.quadHeightMeters);
        s.recenterKey = ReadInt(p, "Keys", "iRecenter", s.recenterKey);
        s.toggleKey = ReadInt(p, "Keys", "iToggle", s.toggleKey);
        s.cameraCycleKey = ReadInt(p, "Keys", "iCameraCycle", s.cameraCycleKey);
        s.povToggleKey = ReadInt(p, "Keys", "iPovToggle", s.povToggleKey);
        s.aimWithController = ReadInt(p, "Controllers", "bAimWithController", s.aimWithController ? 1 : 0) != 0;
        s.noSpread = ReadInt(p, "Weapon", "bNoSpread", s.noSpread ? 1 : 0) != 0;
        s.noSpreadPellets = ReadInt(p, "Weapon", "bNoSpreadPellets", s.noSpreadPellets ? 1 : 0) != 0;
        s.leftHanded = ReadInt(p, "Controllers", "bLeftHanded", s.leftHanded ? 1 : 0) != 0;
        s.handLineOfFire = ReadInt(p, "Controllers", "bHandLineOfFire", s.handLineOfFire ? 1 : 0) != 0;
        s.suppressCameraRecoil = ReadInt(p, "Controllers", "bSuppressCameraRecoil", s.suppressCameraRecoil ? 1 : 0) != 0;
        s.handAutoAimOverride = ReadInt(p, "Controllers", "bHandAutoAimOverride", s.handAutoAimOverride ? 1 : 0) != 0;
        s.clampHandToCamera = ReadInt(p, "Controllers", "bClampHandToCamera", s.clampHandToCamera ? 1 : 0) != 0;
        s.separateHud = ReadInt(p, "HUD", "bSeparateHud", s.separateHud ? 1 : 0) != 0;
        s.hudDistanceMeters = ReadFloat(p, "HUD", "fDistanceMeters", s.hudDistanceMeters);
        s.hudWidthMeters = ReadFloat(p, "HUD", "fWidthMeters", s.hudWidthMeters);
        s.hudHeightMeters = ReadFloat(p, "HUD", "fHeightMeters", s.hudHeightMeters);
        s.hudHideCrosshair = ReadInt(p, "HUD", "bHideCrosshair", s.hudHideCrosshair ? 1 : 0) != 0;
        s.hudSourceTarget = ReadInt(p, "HUD", "iSourceTarget", s.hudSourceTarget);
        s.hudPremultiplied = ReadInt(p, "HUD", "bPremultiplied", s.hudPremultiplied ? 1 : 0) != 0;
        s.hudClearTargets = ReadInt(p, "HUD", "bClearTargets", s.hudClearTargets ? 1 : 0) != 0;
        s.wristPipboy = ReadInt(p, "Pipboy", "bWristPipboy", s.wristPipboy ? 1 : 0) != 0;
        s.pipboyWidthMeters = ReadFloat(p, "Pipboy", "fWidthMeters", s.pipboyWidthMeters);
        s.pipboyOffset[0] = ReadFloat(p, "Pipboy", "fOffsetX", s.pipboyOffset[0]);
        s.pipboyOffset[1] = ReadFloat(p, "Pipboy", "fOffsetY", s.pipboyOffset[1]);
        s.pipboyOffset[2] = ReadFloat(p, "Pipboy", "fOffsetZ", s.pipboyOffset[2]);
        s.pipboyRotDeg[0] = ReadFloat(p, "Pipboy", "fRotXDeg", s.pipboyRotDeg[0]);
        s.pipboyRotDeg[1] = ReadFloat(p, "Pipboy", "fRotYDeg", s.pipboyRotDeg[1]);
        s.pipboyRotDeg[2] = ReadFloat(p, "Pipboy", "fRotZDeg", s.pipboyRotDeg[2]);
        s.pipboySourceTarget = ReadInt(p, "Pipboy", "iSourceTarget", s.pipboySourceTarget);
        s.pipboyStopImod = ReadInt(p, "Pipboy", "bStopImod", s.pipboyStopImod ? 1 : 0) != 0;
        s.pipboyCursorDot = ReadInt(p, "Pipboy", "bCursorDot", s.pipboyCursorDot ? 1 : 0) != 0;
        s.pipboyTint = ReadInt(p, "Pipboy", "bTint", s.pipboyTint ? 1 : 0) != 0;
        s.menuPointer = ReadInt(p, "Menus", "bPointer", s.menuPointer ? 1 : 0) != 0;
        s.menuCursorDot = ReadInt(p, "Menus", "bCursorDot", s.menuCursorDot ? 1 : 0) != 0;
        s.menuPanel = ReadInt(p, "Menus", "bPanel", s.menuPanel ? 1 : 0) != 0;
        s.menuDialoguePanel = ReadInt(p, "Menus", "bDialoguePanel", s.menuDialoguePanel ? 1 : 0) != 0;
        s.menuNoBlur = ReadInt(p, "Menus", "bNoMenuBlur", s.menuNoBlur ? 1 : 0) != 0;
        s.menuClearTargets = ReadInt(p, "Menus", "bClearMenuTargets", s.menuClearTargets ? 1 : 0) != 0;
        s.menuPanelSource = ReadInt(p, "Menus", "iPanelSource", s.menuPanelSource);
        s.menuAnchor = ReadInt(p, "Menus", "bAnchorToView", s.menuAnchor ? 1 : 0) != 0;
        s.menuReanchorDeg = ReadFloat(p, "Menus", "fReanchorDeg", s.menuReanchorDeg);
        s.menuBackdrop = ReadInt(p, "Menus", "bBackdrop", s.menuBackdrop ? 1 : 0) != 0;
        s.menuBackdropDim = ReadFloat(p, "Menus", "fBackdropDim", s.menuBackdropDim);
        s.menuBackdropPanorama = ReadInt(p, "Menus", "bBackdropPanorama", s.menuBackdropPanorama ? 1 : 0) != 0;
        s.menuPanoramaHoleDim = ReadFloat(p, "Menus", "fPanoramaHoleBrightness", s.menuPanoramaHoleDim);
        s.menuPanelPreview = ReadInt(p, "Menus", "bPanelPreview", s.menuPanelPreview ? 1 : 0) != 0;
        s.menuFollowWorkshop = ReadInt(p, "Menus", "bFollowWorkshop", s.menuFollowWorkshop ? 1 : 0) != 0;
        s.menuWorkbenchPanel = ReadInt(p, "Menus", "bWorkbenchPanelFO4VR", s.menuWorkbenchPanel ? 1 : 0) != 0;
        s.menuWorkbenchWidthMeters = ReadFloat(p, "Menus", "fWorkbenchWidthMeters", s.menuWorkbenchWidthMeters);
        s.menuWorkbenchDistanceMeters = ReadFloat(p, "Menus", "fWorkbenchDistanceMeters", s.menuWorkbenchDistanceMeters);
        s.menuWorkbenchHeightMeters = ReadFloat(p, "Menus", "fWorkbenchHeightMeters", s.menuWorkbenchHeightMeters);
        s.menuWorkshopOnHand = ReadInt(p, "Menus", "bWorkshopOnHand", s.menuWorkshopOnHand ? 1 : 0) != 0;
        s.menuWorkshopHandWidthMeters = ReadFloat(p, "Menus", "fWorkshopHandWidthMeters", s.menuWorkshopHandWidthMeters);
        s.menuWorkshopHandUpMeters = ReadFloat(p, "Menus", "fWorkshopHandUpMeters", s.menuWorkshopHandUpMeters);
        s.menuWorkshopHandBackMeters = ReadFloat(p, "Menus", "fWorkshopHandBackMeters", s.menuWorkshopHandBackMeters);
        s.menuWorkshopHandPitchDeg = ReadFloat(p, "Menus", "fWorkshopHandPitchDeg", s.menuWorkshopHandPitchDeg);
        s.menuVatsLabelScale = ReadFloat(p, "Menus", "fVatsLabelScale", s.menuVatsLabelScale);
        s.menuVatsNoVignette = ReadInt(p, "Menus", "bVatsNoVignette", s.menuVatsNoVignette ? 1 : 0) != 0;
        s.pipboyFloating = ReadInt(p, "Pipboy", "bFloating", s.pipboyFloating ? 1 : 0) != 0;
        s.pipboyProjected = ReadInt(p, "Pipboy", "bProjected", s.pipboyProjected ? 1 : 0) != 0;
        s.pipboyProjWidthMeters = ReadFloat(p, "Pipboy", "fProjectedWidthMeters", s.pipboyProjWidthMeters);
        s.pipboyProjDistanceMeters = ReadFloat(p, "Pipboy", "fProjectedDistanceMeters", s.pipboyProjDistanceMeters);
        s.pipboyProjHeightMeters = ReadFloat(p, "Pipboy", "fProjectedHeightMeters", s.pipboyProjHeightMeters);
        s.pipboyProjLagConeDeg = ReadFloat(p, "Pipboy", "fProjectedLagConeDeg", s.pipboyProjLagConeDeg);
        s.pipboyProjNoScanlines = ReadInt(p, "Pipboy", "bProjectedNoScanlines", s.pipboyProjNoScanlines ? 1 : 0) != 0;
        s.menuTerminalInWorld = ReadInt(p, "Menus", "bTerminalInWorld", s.menuTerminalInWorld ? 1 : 0) != 0;
        s.menuPanelFO4VR = ReadInt(p, "Menus", "bPanelGeometryFO4VR", s.menuPanelFO4VR ? 1 : 0) != 0;
        s.menuPauseWidthMeters = ReadFloat(p, "Menus", "fPauseWidthMeters", s.menuPauseWidthMeters);
        s.menuPauseDistanceMeters = ReadFloat(p, "Menus", "fPauseDistanceMeters", s.menuPauseDistanceMeters);
        s.menuPauseHeightMeters = ReadFloat(p, "Menus", "fPauseHeightMeters", s.menuPauseHeightMeters);
        s.menuWindowMenus = ReadString(p, "Menus", "sWindowMenus", "");
        {
            // [MenuGeometry] <MenuName>=width,distance,height
            std::string buf(32768, '\0');
            const DWORD n = ::GetPrivateProfileSectionA("MenuGeometry", buf.data(), static_cast<DWORD>(buf.size()), p.c_str());
            for (std::size_t i = 0; i < n && buf[i];) {
                const std::string line(buf.c_str() + i);
                i += line.size() + 1;
                const auto eq = line.find('=');
                if (eq == std::string::npos || line[0] == ';') {
                    continue;
                }
                std::string key = line.substr(0, eq);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                float v[3]{};
                if (std::sscanf(line.c_str() + eq + 1, " %f , %f , %f", &v[0], &v[1], &v[2]) == 3 && v[0] > 0.01f && v[1] > 0.01f) {
                    s.menuGeometry[key] = { v[0], v[1], v[2] };
                }
            }
            if (const auto it = s.menuGeometry.find("dialoguemenu"); it != s.menuGeometry.end()) {
                s.menuDialogueWidthMeters = it->second[0], s.menuDialogueDistanceMeters = it->second[1], s.menuDialogueHeightMeters = it->second[2];
            }
        }
        s.stereoMode = ReadInt(p, "Stereo", "iMode", s.stereoMode);
        s.stereoInstallHooks = ReadInt(p, "Stereo", "bInstallHooks", s.stereoInstallHooks ? 1 : 0) != 0;
        s.stereoSkipUnconverted = ReadInt(p, "Stereo", "bSkipUnconverted", s.stereoSkipUnconverted ? 1 : 0) != 0;
        s.stereoOwnDepth = ReadInt(p, "Stereo", "bOwnDepth", s.stereoOwnDepth ? 1 : 0) != 0;
        s.stereoPrepass = ReadInt(p, "Stereo", "bStereoPrepass", s.stereoPrepass ? 1 : 0) != 0;
        s.stereoFirstPerson = ReadInt(p, "Stereo", "bFirstPerson", s.stereoFirstPerson ? 1 : 0) != 0;
        s.stereoScope = ReadInt(p, "Stereo", "iScope", s.stereoScope);
        s.stereoTestEyes = ReadInt(p, "Stereo", "bTestEyes", s.stereoTestEyes ? 1 : 0) != 0;
        s.stereoTestIpdMeters = ReadFloat(p, "Stereo", "fTestIpdMeters", s.stereoTestIpdMeters);
        s.stereoTestHalfFovDeg = ReadFloat(p, "Stereo", "fTestHalfFovDeg", s.stereoTestHalfFovDeg);
        s.stereoLogEveryFrames = ReadInt(p, "Stereo", "iLogEveryFrames", s.stereoLogEveryFrames);
        {
            const auto rb = [&](const char* k, bool& v) { v = ReadInt(p, "Stereo", k, v ? 1 : 0) != 0; };
            rb("bSplitDraws", s.stereoSplit);
            rb("bSaoPerEye", s.stereoSaoPerEye);
            rb("bTaaPerEye", s.stereoTaaPerEye);
            rb("bCutTaa", s.stereoCutTaa);
            rb("bCutMotionBlur", s.stereoCutMotionBlur);
            rb("bCutDof", s.stereoCutDof);
            rb("bCutLensFlare", s.stereoCutLensFlare);
            rb("bCutGodrays", s.stereoCutGodrays);
            rb("bCutSsr", s.stereoCutSsr);
            rb("bCutHbao", s.stereoCutHbao);
            rb("bCutSao", s.stereoCutSao);
            rb("bCutTiledLighting", s.stereoCutTiled);
            rb("bSaoPixelShader", s.stereoSaoPixelShader);
            rb("bKeepMonoDepthClip", s.stereoKeepMonoDepthClip);
        }
        s.stereoEyeFarScale = ReadFloat(p, "Stereo", "fEyeFarScale", s.stereoEyeFarScale);
        s.stereoShaderDir = ReadString(p, "Stereo", "sShaderDir", "");
        if (s.stereoShaderDir.empty()) {
            s.stereoShaderDir = std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr_stereo").string();
        }
        s.camNoAimZoom = ReadInt(p, "Camera", "bNoAimZoom", s.camNoAimZoom ? 1 : 0) != 0;
        s.camFirstPersonDialogue = ReadInt(p, "Camera", "bFirstPersonDialogue", s.camFirstPersonDialogue ? 1 : 0) != 0;
        s.camNoKillCams = ReadInt(p, "Camera", "bNoKillCams", s.camNoKillCams ? 1 : 0) != 0;
        s.camNoIdleOrbit = ReadInt(p, "Camera", "bNoIdleOrbit", s.camNoIdleOrbit ? 1 : 0) != 0;
        s.paHudOnLayer = ReadInt(p, "PowerArmor", "bHudOnLayer", s.paHudOnLayer ? 1 : 0) != 0;
        s.paHideVisorRain = ReadInt(p, "PowerArmor", "bHideVisorRain", s.paHideVisorRain ? 1 : 0) != 0;
        s.paProjectedPipboy = ReadInt(p, "PowerArmor", "bProjectedPipboy", s.paProjectedPipboy ? 1 : 0) != 0;
        s.paPipboyDistanceMeters = ReadFloat(p, "PowerArmor", "fPipboyDistanceMeters", s.paPipboyDistanceMeters);
        s.paPipboyHeightMeters = ReadFloat(p, "PowerArmor", "fPipboyHeightMeters", s.paPipboyHeightMeters);
        s.paPipboyWidthMeters = ReadFloat(p, "PowerArmor", "fPipboyWidthMeters", s.paPipboyWidthMeters);
        s.paPipboyFollowSeconds = ReadFloat(p, "PowerArmor", "fPipboyFollowSeconds", s.paPipboyFollowSeconds);
        return s;
    }
}
