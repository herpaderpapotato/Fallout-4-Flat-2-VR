#pragma once
// FO4VR default controller bindings (analysis/vr-controls.md) -> the flat game's gamepad (VirtualPad).
//
// Evidence: Fallout4_VR.esm help/tutorial messages, Touch ("<<OCULUS>>") sections, e.g. HelpControlsVR_VW,
// HelpPipBoyControlsVR_VW, plus the FO4VR exe's embedded ControlMap (VR columns: Wand*, Pause, CloseMenu, Cancel).
// FO4VR handles most VR input in code, not in the ControlMap, so the defaults are taken from what the game tells the
// player. Each FO4VR action is expressed as the flat gamepad input that triggers the same game action (flat
// embedded ControlMap gamepad column), per context:
//   gameplay  - off stick move, off stick click sprint, primary stick X turn (comfort), up jump, down sneak (click =
//               gesture button, no longer favorites); primary trigger fire; off trigger Pip-Boy; primary grip tap reload / hold grenade;
//               primary stick click + primary grip holster (the click alone = gesture button, Gestures.h); A or X activate; B V.A.T.S.; Y tap pause / hold
//               (fEnterWorkshopDelay, 1.5 s) workshop mode, started through the engine (20 Nov);
//               off grip hold Pip-Boy light
//   Pip-Boy   - off stick sections/selection; X/Y (A/B left-handed) tabs; off trigger select; grip close;
//               primary trigger = the wrist pointer click (PipboyLayer); primary stick = right stick
//   quick loot - gameplay while the HUD quick-container list of a lootable container is up (FO4VR quick menu): the
//               stick of the hand whose ray picks the container steps through the list (D-pad pulses, repeat on hold)
//               instead of jump/sneak (primary) or forward/back (off); A / X take; B opens the transfer menu (pad X).
//               [Controllers] bQuickLootStick=1 (0 = off); pipe "bindings ql [on|off]".
//   favorites, VATS, workshop, lockpicking, dialogue, other menus: see ControllerBindings.cpp / vr-controls.md.
// [Controllers] bFO4VRBindings=1 (0 = the previous generic Xbox layout).

#include "plugin/VirtualPad.h"

#include <cstdint>
#include <string>

namespace xr
{
    struct ControllerState;
    struct Pose;
}

namespace fo4flatvr::bindings
{
    struct Context
    {
        bool worldView{ false };         // head-tracked first-person world view (not the cinema quad)
        bool pipboyWrist{ false };       // wrist Pip-Boy open (game::PipboyWristOpen)
        bool pipPointerOnQuad{ false };  // the wrist Pip-Boy pointer is on the screen (its trigger clicks)
        bool menuPointerOnQuad{ false }; // the menu pointer is on the cinema quad (its trigger clicks)
        bool menuPointerLeftHand{ false };
        bool leftHanded{ false };
        const xr::Pose* head{ nullptr };  // this frame's head pose (reference space), null = invalid (gestures)
        int  camState{ -1 };              // game camera state (0 = first person; gestures / selfie)
        // 20 Nov: no menu that takes the view / the pointer is open (no view-taking menu, menu panel, terminal screen) and
        // nothing is loading, in ANY camera view (CINEMA, third person, SCENE ...). Gestures run on it ([Gestures] bAllViews).
        bool noMenu{ false };
    };

    // Presenter (main thread), once per XR frame: controller state -> virtual pad (before dialogue / comfort filters).
    void Map(const xr::ControllerState& a_c, const Context& a_ctx, pad::State& a_out);

    // Pipe "bindings [on|off]": context and the last mapping.
    std::string Command(const std::string& a_arg);

    // Extended Dialogue Interface (XDI.dll loaded; analysis/mod-edi-hudframework.md), [Controllers] bDialogueListNav=1:
    // while DialogueMenu is open the primary stick steps EDI's option list (D-pad pulses = QuickkeyUp/Down/Left/Right,
    // what its gamepad mode reads), the trigger off the panel accepts (pad A), and the menu pointer targets DialogueMenu
    // (it has neither kUsesMenuContext nor kUsesCursor while a pad is in use). Any thread; set by the last Map().
    bool DialogueListNavActive();

    // 20 Nov, FO4VR workshop mode ([Controllers] bWorkshopFO4VR=1): X minimised the build menu -> the Presenter hides the
    // WorkshopMenu panel (workshop mode stays on, the off stick walks); off stick click held = settlement stats -> the panel
    // is drawn this much larger ([Menus] fWorkshopStatsScale). Any thread; set by the last Map().
    // 28 Nov: [Controllers] bStickCarryOver (default 1): a stick held over a context change drives the new context at once.
    bool  StickCarryOver();
    bool  WorkshopPanelHidden();
    float WorkshopPanelScale();

    // 20 Nov ([Controllers] bGlyphRemap=1): the gamepad glyphs the game shows (menu button bars, HUD prompts, "[UserEvent]"
    // tokens in messages) name the Touch control that does that action under these bindings (call-site hooks, byte-checked).
    // Plugin load.
    void InstallGlyphs();

    // 21 Nov, bridge v8 (ROCK-flat phase 1; FrikBridgeApi.h FO4FLATVR_CLAIM_*): input claims of a physics-hands client, per
    // physical hand (a_left / a_right bit masks: grip, trigger, lower face A/X, upper face B/Y, stick click). Honoured only in
    // gameplay / quick loot: Map() then treats the claimed inputs as released before the gestures and the pad mapping run
    // (no pad output, no gesture, no tap/hold), and the glyph remap does not name a claimed control for a gameplay action.
    // Menus, the wrist Pip-Boy, dialogue, VATS, workshop and lockpicking keep every input (menus keep the grip).
    // fo4flatvr.ini [ROCK] bInputClaims=1 (live). Returns true when claims are honoured now. Any thread. Logs on change.
    bool SetClaims(std::uint32_t a_owner, std::uint32_t a_left, std::uint32_t a_right);
    // the ORed masks of every owner as Map() applies them (0, 0 while [ROCK] bInputClaims=0). Any thread.
    void Claims(std::uint32_t& a_left, std::uint32_t& a_right);
    // 28 Nov, bridge v10: input adds of a client (same bits / owners / contexts / switch as the claims): inputs Map() sees
    // pressed on that physical hand although they are not (ROCK's left-hand fire = right trigger added while the left trigger
    // is claimed). Applied after the claims, before the gestures and the pad mapping. Any thread. Logs on change.
    bool SetAdds(std::uint32_t a_owner, std::uint32_t a_left, std::uint32_t a_right);
    // 28 Nov, bridge v13 (ROCK-flat R50): the input context of the last Map(): 0 gameplay, 1 quick loot, 2 Pip-Boy,
    // 3 favourites / quick menu, 4 V.A.T.S., 5 workshop, 6 lockpick, 7 dialogue, 8 other menu. Any thread.
    std::uint8_t InputContext();

}
