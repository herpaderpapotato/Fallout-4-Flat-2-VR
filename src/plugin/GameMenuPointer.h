#pragma once
// Menu laser pointer (feature D2), game side. TU with CommonLibF4 RE types; this header has none.
// D3D/XR side: MenuPointer.* (ray onto the cinema quad, cursor dot, trigger/wheel state).
//
// Target menu = what the engine itself would give a mouse event: UI::ProcessMessages resolves "TopMenu"
// with UI::GetTopMenu (141A80500: the top-most stack entry with depthPriority (+0x64) < 0x15, read in Ghidra)
// and passes the Scaleform event down the stack until a menu handles it or a modal (0x10) one is reached.
// Here: walk UI::menuStack (+0x190, lock 3DBD368, same as GetTopMenu) from the top, skip depth >= 0x15, take
// the first menu with kUsesCursor (0x4) and a movie; stop at a modal menu without the cursor flag. Menus whose
// movie is drawn onto a 3D screen (PipboyMenu in the flat fallback) are not targets (uv would not map).
// No code patches.

#include <cstdint>
#include <string>

namespace fo4flatvr::game
{
    struct MenuPointerTarget
    {
        bool          valid{ false };
        std::string   name;
        std::uint32_t flags{ 0 };
        int           depth{ 0 };
        int           terminalMode{ -1 };  // TerminalMenu only: mode field (+0x240, written by TerminalMenu::SetMode B0CE70)
    };
    // Main thread (Presenter). Cheap: one stack walk under the stack's read lock.
    bool FindPointerMenu(MenuPointerTarget& a_out);

    struct MenuPointerInput
    {
        bool  onQuad{ false };
        float u{ 0.0f }, v{ 0.0f };  // 0..1 over the cinema image (= the back buffer), origin top-left
        bool  down{ false };         // trigger held
        int   wheel{ 0 };            // wheel steps this frame (+ = up)
        bool  movieUv{ false };      // u/v are already movie uv (world-screen pick, PickWorldScreen): no 3D-screen remap
    };
    // Main thread, every frame while the cinema quad is shown: queues Scaleform MouseEvents (move on change,
    // down/up on the trigger edge, wheel) and drains them in an F4SE UI task into the current target menu's
    // movie at uv x the movie's viewport buffer size. A mouse-up goes to the menu that got the down (dropped
    // if it closed). a_active = false (no cinema quad / pointer off) releases a held button.
    // a_forceMenu: target this menu by name instead of the stack walk (world panel: DialogueMenu, Dialogue.h).
    void SetMenuPointer(const MenuPointerInput& a_in, bool a_active, const char* a_forceMenu = nullptr);

    // Menu panels over the live world view: a game-pausing menu (PauseMenu; UI +0x1E0 > 0) stops the player camera update
    // (measured: FirstPersonState::Update count frozen while PauseMenu is open; it keeps running under the Pip-Boy), so the
    // world image would freeze while the projection layer claims the moving head. FO4VR keeps its HMD camera live behind
    // menus. Main thread (Present, after the head pose for the next frame is published): if the camera was not updated
    // since the last call (a_fpUpdates = game::FirstPersonUpdates()), run PlayerCharacter::UpdateCamera (1.11 D5A670,
    // prologue byte-checked once) for the next frame. Returns true if it ran. a_active = false resets the tracking.
    // Round 8: a_counter = the update counter of the active camera state (game::FirstPersonUpdates in first person,
    // camdir::Ticks otherwise), read before and again after a driven update.
    bool DriveCameraWhilePaused(bool a_active, std::uint64_t (*a_counter)());
    std::string PausedCameraReport();

    // Menu panels: a view-taking menu that can be shown over the live world view - drawn entirely by the
    // "FlatScreenModel" renderer (kCustomRendering 0x8000; captured by H1, skipped by H2), not a menu that takes the
    // camera or has no world behind it, and not one whose 3D preview is composited screen-space by its own Interface3D
    // renderer (that part would end up head-locked in the world image). Main thread. a_name: a ViewTakingMenu name.
    bool PanelEligible(const char* a_name);
    bool MenuOpen(const char* a_name);  // UI::GetMenuOpen

    // Round 7 (FO4VR terminals, analysis/vr-terminals-pipboy.md): FO4VR shows a furniture terminal on the terminal's own
    // monitor in the world (TerminalMenu::InitMenu VR BDFBF0: renderer "TerminalMenu" world-attached to the terminal ref's
    // "Screen:0", exactly as flat 1.11 does; the player stays in first person, no camera zoom), pointed at with the laser.
    // True for TerminalMenu / TerminalHolotapeMenu while that holds here: the menu is open with custom renderer
    // "TerminalMenu", the terminal reference (328FC68) has a 3D with "Screen:0", the camera is first person and the switch
    // ([Menus] bTerminalInWorld) is on. Such a menu is not view-taking. Main thread.
    bool WorldScreenMenu(const char* a_name);
    void SetWorldScreenEnabled(bool a_on);
    bool WorldScreenEnabled();
    // The aim ray (camera-local metres / unit direction relative to the camera root on screen, as PublishHandRay builds it)
    // against the terminal's "Screen:0" with the engine's own NiPick (NiPick ctor 16FD560 / PickObjects 16FD660 / dtor 16FD610,
    // set up as UIUtils::PickScreen 1087930 does, prologues byte-checked once): movie uv of the hit (record +0x24/+0x28) and
    // the hit distance in game units. Main thread.
    bool PickWorldScreen(const float a_localPos[3], const float a_localDir[3], float a_unitsPerMeter, float& a_u, float& a_v, float& a_dist);
    std::string WorldScreenReport();
    // a_menu is open and its custom renderer is a_renderer (e.g. TerminalMenu on "PipboyMenu" = a holotape terminal on the
    // Pip-Boy screen). Main thread.
    bool MenuUsesRenderer(const char* a_menu, const char* a_renderer);
    // Round 8: a HUD notification (top-left message, SendHUDMessage::ShowHUDMessage A37270, prologue byte-checked), queued as
    // an F4SE task. Any thread.
    void HudMessage(const std::string& a_text);
    // Flat VATS: the selected target's screen position (u right, v down, 0..1 over the world NiCamera's frustum; may be
    // outside) from VATSMenu+0xE8 VatsMenuObj (current target index +0x1230 / +0x248 / +0x238 / refs +0x220, as
    // VatsMenuObj::GetCurrentTarget B1D220), projected at chest height. Main thread; false = no VATS target.
    bool VatsTargetScreen(float& a_u, float& a_v);
    // Queue a UIMessage hide for a_name if it is open (at most every 0.5 s). Main thread.
    void HideMenuIfOpen(const char* a_name);
    // First open menu (top of the stack first) drawn by "FlatScreenModel" (flags 0x8000) other than ButtonBarMenu, or
    // "" - in the world view such a menu would otherwise be drawn screen-space into the double-wide stereo frame.
    std::string FlatOverlayMenu();

    // Menus with kUsesBlurredBackground (0x400000: PauseMenu, VATSMenu) raise UI+0x1F8 [measured: 1 with PauseMenu or
    // VATSMenu open, 0 with the Pip-Boy (flag absent)], and the frame then shows a blurred copy of the scene taken when
    // the menu opened - screen-fixed, so in VR it moves with the head. While a_active (menu panel / VATS overlay over
    // the live world) the counter is held at 0 (what it held is kept aside and given back when a_active ends, so the
    // game's own increments/decrements stay balanced). Main thread, every Present. [Menus] bNoMenuBlur.
    void HoldMenuBlur(bool a_active);
    std::string MenuBlurReport();

    // Pipe "pointer": game-side part (target menu, flags, depth, movie viewport, events sent, last event).
    std::string MenuPointerGameReport();
    // Test helpers (pipe "pointer findref <hexBase>" / "pointer count <hexForm> [hexRef]"), main thread via the F4SE task
    // queue: newest reference of a base form in the player's cell; item counts in the player's / a ref's inventory.
    std::string MenuTestCommand(const std::string& a_arg);

    // Wrist Pip-Boy tint (feature D3 follow-up): the engine's own Pip-Boy screen colour, i.e. the constants
    // ImageSpaceEffectPipboyScreen::UpdateParams (2233B40) hands to the PipboyScreen shader as cb2[3].xyz and the
    // brightness cb2[1].w: fPipboyEffectColorR/G/B:Pipboy (INI pref, via RE::GetINISetting; values 2F98BD8/BF0/C08)
    // and the float at 2F98BC4. The shader ends in out = dot(c.rgb, (0.3, 0.59, 0.11)) * colour * brightness.
    // Main thread. Returns false (and leaves a_rgb) if the settings are not available yet.
    // a_powerArmor: the power-armour colour instead (fPAEffectColorR/G/B:Pipboy, values 2F98C20/38/50), which
    // UpdateParams uses while the PowerArmorPipboy flag is set.
    bool PipboyScreenColor(float a_rgb[3], float& a_brightness, bool a_powerArmor = false);
    // The brightness alone (2F98BC4): Interface3D::Renderer::DrawPostFX (store at A3DA70) writes it from the renderer
    // being drawn (+0xCC; it animates, ~0.88..1.04 measured), so it is the Pip-Boy's only when read right after the
    // "PipboyMenu" renderer's DrawPostFX (H1). Sanitised: 1.0 if implausible. Render/main thread.
    float PipboyScreenBrightness();
}
