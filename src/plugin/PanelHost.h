#pragma once
// Panel host (26 Nov): other plugins' VR panels (PanelHostApi.h), handled like the VR settings panel (ConfigUI.h).
//
// Up to kMaxPanels panels, one OpenXR quad layer each (world-quad slots after the settings panel's, layered with it after
// the cinema quad, below the beams). World panels are anchored ahead of the head when they open; hand panels ride on a
// controller's grip pose (optionally only while they face the head, like a watch). Either controller's aim ray points at
// them (the hand the settings panel owns is left to it; a hand cannot point at the panel it carries): trigger = click,
// stick up/down = scroll, stick left/right = step, B / Y = close for panels that ask for it. The pointing hand's trigger,
// stick and face buttons are kept from the game, the shared beam is offered (beam::Offer, Source::kExternal), haptic ticks
// on click. The client draws on the Presenter thread into its own texture (FO4FlatVR_PanelDraw_t).
// Gesture / binding action "Mod panel" (Gestures.h action 6) toggles every panel registered with
// FO4FLATVR_PANEL_GESTURE_TOGGLE. Off switch: [PanelHost] bEnabled=0 (the API refuses registration, nothing is drawn).
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
    struct ControllerState;
}

namespace fo4flatvr::panelhost
{
    void Install();
    // Presenter thread, right after the settings panel's UpdateFrame: requests, anchoring, ray hit tests, the clients' draws.
    // a_reservedHand: the hand whose ray the settings panel owns (-1 none).
    void UpdateFrame(xr::Runtime& a_rt, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, bool a_began, const xr::Pose* a_head,
        int a_reservedHand);
    void MaskControllers(xr::ControllerState& a_c);
    bool OwnsPointer();
    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx);  // between BeginFrame and EndFrame
    void GestureToggle();                                        // any thread
    // pipe "panels [status|open <id>|close <id>|toggle <id>|aim <id> <u> <v> [down|up]|aim off]"
    std::string Command(const std::string& a_arg);
}
