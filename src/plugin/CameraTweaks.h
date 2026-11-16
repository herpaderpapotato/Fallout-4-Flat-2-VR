#pragma once
// Camera step 0 (analysis/vr-camera-states.md section 8): VR comfort fixes with no code hooks, only
// engine data writes. [Camera] keys in the ini, each ON by default:
//  bNoAimZoom          while the presenter shows the head-tracked world view, PlayerCamera::adjustFOV
//                      (+0x1A9) is cleared, so GetAdjustedWorldFOV (1029EB0) / GetAdjusted1stPersonFOV
//                      (1029F80) return the plain worldFOV / firstPersonFOV: ADS no longer zooms. Put
//                      back to 1 when the world view ends (scopes / menus / other camera states).
//  bFirstPersonDialogue bDialogueCameraEnable:Interface = 0 (TryDialogueCamera D5F510 returns at once).
//  bNoKillCams         fKillCamBaseOdds = 0 and fKillCamLevelMaxBias = 0 (GetVATSKillCamera 6CC560:
//                      odds = min(levelBias, maxBias) + baseOdds), bVATSPlayerDeath:VATS = 0.
//  bNoIdleOrbit        bDisableAutoVanityMode:Camera = 1 (SetAllowAutoVanityMode 1027110 then never sets
//                      allowAutoVanityMode) and PlayerCamera::allowAutoVanityMode (+0x1A4) kept 0 (the idle
//                      event handler PlayerCamera::ProcessEvent 1026500 starts AutoVanity only if it is 1).
// Settings are found through the engine's setting collections; the value address is cross-checked
// against the verified RVA. Without a collection hit, the raw RVA is used only if the Setting's key
// string there matches and the value is plausible. Switching an item off restores the original value.

#include <string>

namespace fo4flatvr::game::camtweak
{
    struct Config
    {
        bool noAimZoom{ true };
        bool firstPersonDialogue{ true };
        bool noKillCams{ true };
        bool noIdleOrbit{ true };
    };

    void   Configure(const Config& a_cfg);  // any thread; settings are (re)applied on the next Tick
    Config Current();

    // Main thread. Writes the engine settings for the current config (logs every change).
    void ApplySettings(const char* a_when);

    // Main thread, every Present. a_worldView = the presenter is showing the head-tracked world view.
    void Tick(bool a_worldView);

    // PlayerCamera::GetAdjustedWorldFOV (1029EB0, prologue byte-checked) on the main thread, or a manual
    // equivalent if the bytes do not match. What the world NiCamera frustum is built from.
    float AdjustedWorldFov();

    // Pipe "camcfg [<item> 0|1 | aimzoom on|off]": the four switches, the settings' current values and
    // PlayerCamera's FOV-adjust fields.
    std::string Command(const std::string& a_arg);
}
