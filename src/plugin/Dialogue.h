#pragma once
// VR dialogue (D5 stages D1 + D2; analysis/vr-vats-dialogue.md sections 3 and 6). [Dialogue] in the plugin ini.
//
// Game side (GameDialogue.cpp, CommonLibF4 types):
//  D-look  MenuTopicManager::UpdateSpeaker (BAF9C0) decides each frame whether the player "looks at the speaker"; flat
//          ties it to PlayerCamera::QCameraEquals(0xC) (dialogue camera) or the ViewCaster pick (= our hand ray). The three
//          QCameraEquals calls BB0039 / BB0251 / BB0376 (`BA 0C 00 00 00 E8 rel32 -> 102B5C0`, byte-checked) go through a
//          thunk that also returns true while MenuTopicManager ([30E6E50]) +0x4C menuOpen is set: FO4VR's rule (VR CEA600),
//          so once a conversation is open the options stay enabled and the dialogue input context stays, wherever the
//          head or hand points.
//  D-wpn   while DialogueMenu is open the first-person "WEAPON" node is app-culled (FO4VR EF9E80 culls the weapon bone),
//          restored when it closes. WeaponHidden() is the query for other features (Hands).
//  D-rumble rising edge of DialogueMenu +0x165 (set by ProcessMessage on DialogueShowButtons = "ShowButtonHelp") ->
//          one haptic pulse on the primary controller (FO4VR fDialogueRumble*).
//  D-stick 22 Nov (FO4VR DialogueMenu::HandleEvent(ThumbstickEvent) VR 12ED530; evidence/dialogue.md): a primary-stick
//          flick from neutral picks a response at once, by FO4VR's fixed directions: up = Question, right = Negative
//          (sarcastic), down = Positive, left = Neutral; FO4VR's direction rule (fLThumbDeadzone 0.25 / Max 0.94 rescale,
//          fDirectionalDeadzone 0.5 per axis, dominant axis at 45 deg). Vanilla DialogueMenu.swf: MenuTopicManager::
//          OnMenuButtonPress(type) (BAF7C0), the call FO4VR makes. Extended Dialogue Interface (XDI.dll): its list row
//          whose optionID is that slot is picked through XDI's own handler (List_mc.selectedIndex + root.onItemPress),
//          so XDI's framework / result logic runs as on a click. XDI lists that are not the 4 slots (more than 4 rows or
//          other optionIDs): the stick scrolls (List_mc.moveSelectionUp/Down, repeat while held; left/right = XDI value
//          rows), A / the trigger picks the highlighted row. Primary stick zeroed while DialogueMenu is open.
// Render / XR side (DialogueLayer.cpp, D3D11):
//  D-cap   GameHud H1 (DrawPostFX call A3D3C5) copies the "FlatScreenModel" renderer's 0x3E/0x3F (crop box) while
//          DialogueMenu is the only open FlatScreenModel menu; H2 (RenderMain call A3CD30) skips that renderer's
//          screen mesh, so the options leave the world image.
//  D-panel the crop on world quad slot 1 (xr::Runtime::SubmitWorldQuad): position = HMD, yaw = lagged HMD yaw (dead
//          zone, then exponential follow; FO4VR UprightHmdLagNode), no pitch/roll; fDistance ahead, fDown below, fWidth.
//  D-ptr   the menu laser pointer (MenuPointer.*) hit-tests the panel quad and sends Scaleform mouse events to
//          DialogueMenu's movie (crop uv -> movie uv).

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
}

namespace fo4flatvr::pad
{
    struct State;
}

namespace fo4flatvr::dlg
{
    // Defaults OFF while paused (14 Nov): built, not yet verified in game. Flip to true + ini 1 after the 6.4 tests.
    struct Config
    {
        bool  lookAnywhere{ false };   // [Dialogue] bLookAnywhere (D-look patches; off = not installed)
        bool  panel{ false };          // [Dialogue] bPanel (capture + option panel quad)
        bool  hideWeapon{ false };     // [Dialogue] bHideWeapon
        bool  haptics{ false };        // [Dialogue] bHaptics
        bool  pointer{ false };        // [Dialogue] bPointer (menu laser pointer on the panel)
        bool  stickResponses{ true };  // [Dialogue] bStickResponses (22 Nov; FO4VR primary-stick directions -> the 4 responses).
                                       // The old [Dialogue] bStickOptions (off by default, pad buttons) is no longer read.
        bool  listScroll{ true };      // [Controllers] bDialogueListNav: XDI long lists scroll with the primary stick
        float distanceMeters{ 0.6f }; // [Dialogue] fDistanceMeters (ahead of the HMD, horizontal)
        float downMeters{ 0.2f };     // [Dialogue] fDownMeters (below the HMD)
        float widthMeters{ 0.6f };    // [Dialogue] fWidthMeters (height from the crop's aspect)
        float pitchDeg{ 0.0f };       // [Dialogue] fPitchDeg (+ = top tilted away; 0 = upright/level)
        float lagSeconds{ 0.3f };     // [Dialogue] fLagSeconds (follow time constant)
        float deadzoneDeg{ 20.0f };   // [Dialogue] fDeadzoneDeg (head yaw vs panel yaw before it follows)
        float cropLeft{ 0.0f }, cropTop{ 0.0f }, cropRight{ 1.0f }, cropBottom{ 1.0f };  // [Dialogue] fCrop* (0..1 of the movie)
        int   source{ 0x3E };         // [Dialogue] iSourceTarget (62 = 0x3E glass result, as the HUD; 63 = 0x3F movie)
        bool  premultiplied{ true };  // [Dialogue] bPremultiplied
        float hapticAmplitude{ 0.3f };   // [Dialogue] fHapticAmplitude
        float hapticSeconds{ 0.12f };    // [Dialogue] fHapticSeconds
        bool  leftHanded{ false };    // [Controllers] bLeftHanded: primary hand = left
    };

    // Reads [Dialogue] (+ [Controllers] bLeftHanded) and installs the D-look call-site patches. Once, at plugin load.
    std::string Install();
    const Config& GetConfig();
    Config        ConfigCopy();                                 // any thread
    bool          SetCfgValue(const std::string& a_key, float a_v);  // pipe "dlg cfg <key> <value>"

    // ---- game side (GameDialogue.cpp), main thread --------------------------------------------------------------
    // Every Present, before the XR submission: polls UI / MenuTopicManager, weapon cull, haptic edge.
    void Tick();
    bool Open();                 // DialogueMenu open (last Tick)
    bool FlatExclusive();        // ... and no other FlatScreenModel menu is open (the capture holds only the options)
    bool TakeHapticRequest();    // true once per options-shown edge (bHaptics)
    bool WeaponHidden();         // the first-person weapon is culled by us right now (query for Hands)
    bool CanPick();              // options shown + enabled + input accepted + a choice pending (last Tick; any thread)
    // Presenter, before comfort::FilterPad: primary stick direction -> pad face button while options can be picked.
    void FilterPad(pad::State& a_p, float a_stickX, float a_stickY);
    std::string GameStatus();
    std::string GameCommand(const std::string& a_arg);  // look/weapon/haptics/stick/scroll on|off, npcs

    // ---- render / XR side (DialogueLayer.cpp) --------------------------------------------------------------------
    bool CaptureWanted();                              // GameHud H1, renderer "FlatScreenModel"
    void OnFlatTargets(void* a_tex3E, void* a_tex3F);  // GameHud H1: copy the crop (ID3D11Texture2D*)
    bool SkipWorldFlat();                              // GameHud H2, renderer "FlatScreenModel"
    void NoteWorldFlatSkipped();
    // Presenter, world view, between BeginFrame and EndFrame: lag pose from a_head (reference space, the pose the frame
    // was rendered with), cursor dot, world quad slot 1; publishes the panel to the menu pointer. False = no layer.
    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx, const xr::Pose& a_head, bool a_headValid);
    void OnPresent(bool a_layerNext);  // every Present: decide the next frame's H2 skip, service dlgcap
    // Presenter: fire the haptic pulse if requested (needs the session).
    void UpdateXr(xr::Runtime& a_rt, bool a_began);
    std::string Command(const std::string& a_arg);     // pipe "dlg ..."
}
