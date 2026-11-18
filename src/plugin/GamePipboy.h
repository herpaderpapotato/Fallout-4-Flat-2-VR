#pragma once
// Wrist Pip-Boy (feature D3, stage 1), game side. TU with CommonLibF4 RE types; this header has none.
// Research: analysis/vr-pipboy.md (flat 1.11.240 flow re-read for this: see the README "Wrist Pip-Boy").
//
// Code patches, installed once at plugin load, each byte-verified first (mismatch = logged and skipped):
//  O1  detour PipboyManager::PlayPipboyOpenAnim (B4F980; PipboyHandler's open path). While the wrist Pip-Boy
//      can be used (switch on, XR world view running, first-person camera, menu "PipboyMenu"; in power armour
//      only with [PowerArmor] bProjectedPipboy = "projected") it opens directly like FO4VR's VR_OpenDirect
//      (which also shows the menu in power armour without the gaze test): menuToOpen = name, waitAnimEvent = "" ,
//      StartAnimationGraphListening, OnPipboyOpenAnim (InitPipboy + OnPipboyOpened + UIMessageQueue show).
//      Skipped: PlayerCamera::StartPipboyMode and the ActionPipboy arm animation (TaskQueue ShowPipboy).
//      Otherwise the original runs.
//  O2  detour PipboyManager::PlayPipboyCloseAnim (B50580): for a wrist-opened Pip-Boy, closes without the
//      ActionPipboyClose animation: closeAnimEvent = strPowerArmorClose (strForceClose when immediate) and
//      OnPipboyCloseAnim directly, which tears down (Interface3D off, imod, cursor, light) and calls
//      OnPipboyClosed (StopPipboyMode no-op, autosave, fast travel, item idle) and makes PipboyManager::Update
//      hide the menu next frame. Otherwise the original runs.
//  H3  call A3D3EC (RenderPrepassesAndMenus -> RenderEffect 0xC, offscreen 0x3D -> final 0x3C): after the
//      original, hands render target 0x3C to pipboy::OnFinalTarget (copied only for renderer "PipboyMenu").
//      Measured: not called while the Pip-Boy is open (renderer +0x64 != 1), so the shown source is the
//      offscreen 0x3D that H1 (GameHud.cpp) copies for renderer "PipboyMenu".
//  O3  detour PipboyManager::PlayPipboyLoadHolotapeAnim (B4FB60): while a wrist Pip-Boy is open, a holotape
//      load uses the game's own immediate branch (bool = 1: OnPipboyOpenAnim now) instead of waiting for the
//      ActionPipboyLoadHolotape arm animation, which never plays with the arm down. Round 7: the holotape menus
//      themselves (PipboyHolotapeMenu; TerminalMenu without a terminal ref) draw into the Pip-Boy renderer, so they
//      show on the wrist / projected quad (Presenter InWorldMenu); the Pip-Boy pointer then targets TerminalMenu.
// PlayPipboyOpenTerminalAnim (B4FA70) and PlayPipboyGenericOpenAnim (B4FE10, GenericMenu) are not patched:
// they keep the original (animated) flow.

#include <string>

namespace fo4flatvr::game
{
    std::string InstallPipboyHooks();   // "open=ok close=ok h3=ok"
    bool        PipboyHooksInstalled(); // O1, O2 and H3

    // Presenter, every frame: XR world view is up (mono, session running, submitting), so a wrist open may
    // be used. Main thread.
    void SetPipboyWorldViewAvailable(bool a_on);
    // True while a wrist (direct) open is in effect: from the open until the PipboyMenu has closed and the
    // manager is idle. Main thread (polls and clears the state).
    bool PipboyWristOpen();
    // True while that direct open is projected: in power armour ([PowerArmor] bProjectedPipboy) or anywhere with
    // [Pipboy] bFloating (FO4VR bAlwaysUseProjectedPipboy): screen on a quad in front of the head instead of the
    // wrist; same capture, pointer and close.
    bool PipboyProjected();
    // ... and the player is in power armour (PA screen colour fPAEffectColor*).
    bool PipboyProjectedInPowerArmor();

    struct PipboyPointer
    {
        bool  onQuad{ false };
        float u{ 0.0f }, v{ 0.0f };  // 0..1, origin top-left of the screen texture
        bool  down{ false };         // trigger held (click = down then up)
        int   wheel{ 0 };            // wheel steps this frame (+ = up)
    };
    // Presenter, every frame while the wrist Pip-Boy is open: queues Scaleform mouse events into the
    // PipboyMenu movie (F4SE UI task) when the pointer moved, the button changed or the wheel turned.
    void SetPipboyPointer(const PipboyPointer& a_p);

    // Pipe "pipboy": game-side line (hooks, wrist state, page/tab, movie viewport, injected events).
    std::string PipboyGameReport();
}
