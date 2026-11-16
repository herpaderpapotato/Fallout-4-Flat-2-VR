#pragma once
// Separated HUD (feature D1), game side. Second TU with CommonLibF4 RE types (GameBridge is the first);
// this header has none. Research: analysis/vr-hud.md.
//
// Code patches, installed once at plugin load (before the game runs), each byte-verified first
// (mismatch = logged and skipped):
//  H1  call A3D3C5 (Interface3D::Renderer::RenderPrepassesAndMenus -> DrawPostFX A3D920): after the
//      original, for renderer "HUDScreenModel", hands the engine textures of render targets 0x3E (HUD
//      glass result) and 0x3F (raw Scaleform movie) to hud::OnHudTargets, which copies them at once.
//      Hooked after DrawPostFX rather than after RenderPrepassesAndMenus because that function releases
//      the pooled target 0x3F before it returns (ReleaseRenderTarget sets its handle to -1).
//  H2  call A3CD30 (Interface3D::Renderer::RenderAll -> RenderMain A3D450): skipped for "HUDScreenModel"
//      while hud::SkipWorldHud() (HUD layer active), so the world image carries no HUD. RenderAll rebinds
//      the destination target, depth and viewport after the loop, so skipping leaves no state behind.
//      Power armour (same call site, analysis/vr-power-armor.md), while the HUD layer is active:
//       - "PowerArmorRenderer" (screen-attached PA dash + compass): the original runs twice into its own
//         destination, cleared to transparent black and to opaque white, both results kept by
//         hud::CapturePaOverlay, destination restored -> the dash is on the HUD layer, not in the world image
//         ([PowerArmor] bHudOnLayer);
//       - "HUDRainRenderer" (visor rain): skipped ([PowerArmor] bHideVisorRain; FO4VR has no such renderer);
//       - "PipboyMenu" while the projected PA Pip-Boy is open: skipped (its screen is on our quad).
//  C1  HUDCrosshair vtable 252F2C8 slot 4 (UpdateDisplayObject 9BD280): while hud::HideCrosshair(),
//      the setting byte bCrosshairEnabled (2F2E060, an INI *pref*) reads 0 for the duration of the call
//      only, so the widget hides without the user's preference ever holding 0 outside it.

#include <string>

namespace fo4flatvr::game
{
    // Returns a short summary ("h1=ok h2=ok crosshair=ok").
    std::string InstallHudHooks();
    bool        HudHooksInstalled();  // H1 and H2 both in

    // Pipe "menus": the UI menu stack (render order) as one line: count, then per menu
    // name, flags (hex), custom renderer name, depth priority, on-stack/advanced bits.
    // Runs on the main thread through the F4SE task queue; waits up to 2 s.
    std::string MenusReport();

    // Shared with the wrist Pip-Boy (D3, GamePipboy.cpp): H1 also hands renderer "PipboyMenu"'s offscreen target
    // 0x3D to pipboy::OnOffscreenTarget. RenderTargetTexture: ID3D11Texture2D* of engine render target a_id while
    // it is acquired (pooled targets: only inside RenderPrepassesAndMenus), else nullptr. Render thread.
    void* RenderTargetTexture(int a_id);
    bool  H1Installed();

    // Diagnostics: H1/H2/C1 call counts.
    std::string HudHookStats();
}
