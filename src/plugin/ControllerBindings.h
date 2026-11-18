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
//               primary stick click + primary grip holster (the click alone = gesture button, Gestures.h); A or X activate; B V.A.T.S.; Y tap pause / hold workshop;
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

}
