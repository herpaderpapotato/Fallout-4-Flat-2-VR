#pragma once
// In-VR settings panel (20 Nov, analysis/vr-config-ui.md option d1, phase 1).
//
// A Dear ImGui panel drawn into an own D3D11 texture on the Presenter thread and shown as a world-locked OpenXR quad
// (xr::Runtime top world-quad slot, above menu panels and the flat window). Operated with either controller's aim ray:
// trigger = click, stick up/down = scroll, stick left/right = change the value under the ray, B / Y = close; the pointing
// hand's trigger, stick and face buttons are kept from the game while its ray is on the panel. The game is not paused.
// The pointer beam (PointerBeam.h, Source::kExternal) is offered while a ray is on the panel.
//
// Rows come from the settings table (ConfigSpec.gen.inl, generated with the MCM pages from plugin/tools/config_spec.py).
// Values: code default < fo4flatvr.ini < MCM's Data\MCM\Settings\fo4flatvr.ini < unsaved previews (ConfigStore.h). A change
// is a preview at once (live rows run their apply command, restart rows are marked); SAVE writes the MCM layer (MCM's API
// while MCM is loaded, else one ini line per key), DISCARD drops the previews and re-applies the saved values, RESET puts a
// row back to fo4flatvr.ini / the code default. fo4flatvr.ini is never written. Edits made elsewhere (MCM's menu, a text
// editor) re-apply their live rows within a second.
//
// Open / close: gesture action "VR settings panel" (default right stick Up-then-down), hold the left menu button
// [ConfigUI] fMenuHoldS (1 s; a tap still opens Pause, on release), [Keys] iConfigMenu + iConfigMenuMod (Ctrl+F2),
// pipe "cfgui open|close|toggle". Off switch: [ConfigUI] bEnabled=0 (nothing installed, no hold on the menu button).

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
    struct ControllerState;
}

namespace fo4flatvr::configui
{
    void Install();   // plugin load: reads [ConfigUI] bEnabled and the panel keys
    bool Enabled();
    void RequestToggle(const char* a_why);  // any thread (gesture, controller bindings, pipe)
    bool IsOpen();
    float MenuHoldSeconds();  // [ConfigUI] bMenuHold / fMenuHoldS for ControllerBindings: 0 = no hold (menu = Pause on press)

    // Presenter thread, every Present (also without XR): settings file poll + live re-apply of edits made elsewhere.
    void Tick();
    // Presenter thread, right after BeginFrame: hotkey, open/close, anchoring, ray hit test, the ImGui frame and its render
    // into the panel texture. a_head: this frame's head pose (null = invalid); a_focused: the game window has the focus.
    void UpdateFrame(xr::Runtime& a_rt, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, bool a_began, const xr::Pose* a_head,
        bool a_focused);
    // Presenter, before the controller bindings: the pointing hand's trigger / stick / face buttons go to the panel.
    void MaskControllers(xr::ControllerState& a_c);
    bool OwnsPointer();  // a ray is on the panel or a click is held: the menu pointer stays out
    int  PointerHand();  // 26 Nov: the hand whose ray is on the panel this frame (-1 none); PanelHost leaves it alone
    // Keyboard: true while the panel's hotkey modifier is held and a_vk is the panel key (the camera cycle on F2 skips it).
    bool HotkeyClaims(int a_vk);
    // Presenter, between BeginFrame and EndFrame of the frame being finished: the panel quad.
    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx);

    // Pipe "cfgui [status|open|close|toggle|page <name>|set <Section> <Key> <value>|save|discard|reset <Section> <Key>|
    //               rows|aim <u> <v> [down|up]|aim off|store]"
    std::string Command(const std::string& a_arg);
}
