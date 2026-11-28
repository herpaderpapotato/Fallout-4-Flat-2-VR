#pragma once
// Scopes like FO4VR (27 Nov, scopes agent; work/handoff/scopes.md, analysis/fo4vr-functions/evidence/scope.md).
//
// FO4VR 1.2.72, every frame with a drawn gun whose instance has kHasScope (EF7B00): eye point = HMD node + fHMDScopeOffset
// (0, -16, 0), scope point = weapon node + fWeaponScopeOffset (0, 0, 8); the scope is entered when the eye->scope direction
// is within 25 deg of the HMD forward, within 7 deg x (60 / dist)^2 of the weapon forward and the distance is < 38 u (exit
// 35 deg / 15 deg / 40 u: hysteresis). Not in Favorites / Dialogue / VATS. A blackout fade (min of the three ramps 25..30 deg,
// 7..10 deg, 38..42 u, x fScopeFadeMagnitude 1.25) darkens the world as the gun comes up; scoped (EFAA60): the game's scoped
// state, the scope model (ScopeParent: the scope camera's render target on the gun) and the world-space ScopeMenu overlay
// (reticle), the weapon's zoom from its ZoomData; lowered = back to normal.
//
// Flat (this module): the same entry test and fade on the headset / primary controller poses; entering holds the game's
// aim-down-sights (pad LT) so the flat game runs its own scoped state (ScopeMenu, zoom data, sway, hold breath, FRIK-flat's
// "looking through scope"). While ScopeMenu is open the main single-pass stereo render itself is the scope camera: both eyes
// are published at the eye, looking along the primary controller's aim ray (= the shot direction), with the weapon's zoom
// (vertical flat ADS FOV = ZoomData fovMult x fDefaultWorldFOV, the circle spans it). The eye image goes, masked round and
// with the ScopeMenu overlay (its reticle) on top, onto a world quad along the aim ray; no projection layer = the world is
// black around it (FO4VR's blackout). The first-person WEAPON node is culled while scoped (FO4VR culls the player's nodes
// in its scope pass). The world camera, its culling frustum and the hand ray are untouched: the normal first-person view is
// not changed (no per-frame cost when no scoped gun is drawn).
//
// fo4flatvr.ini [Scope] (live): bEnabled=1, fViewDeg, fViewDistance, fFadeMagnitude, fZoomScale, fOverlayScale,
// bHoldBreathGrip. Pipe: `scope [status|on|off|cap <bmp>]`.

#include <string>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace xr
{
    struct Pose;
    class Runtime;
}

namespace fo4flatvr::stereo
{
    struct EyeFrame;
}

namespace fo4flatvr::scope
{
    // Plugin load: reads [Scope], subscribes to its changes, registers the main tick (weapon state, WEAPON cull).
    void Install();

    // Presenter (Present thread), each frame after xrBeginFrame: this frame's head and primary aim pose (reference space),
    // whether gameplay input reaches the world view, and whether a menu that ends the scope is open (FO4VR: Favorites,
    // Dialogue, VATS; ours also Pip-Boy, panels). Runs the FO4VR entry / exit test and the fade.
    void Update(const xr::Pose& a_head, bool a_headValid, const xr::Pose& a_aim, bool a_aimValid, bool a_gameplay, bool a_blocked,
        float a_unitsPerMeter, bool a_scopeMenuOpen, bool a_stereo);

    bool Enabled();
    // Raised to the eye (our state): the bindings hold the game's aim (pad LT).
    bool HoldAim();
    // Primary grip = hold breath (FO4VR's ScopeMenu hint on the grip) while the game's scope is up. [Scope] bHoldBreathGrip.
    bool GripHoldsBreath();
    // Presenter's view selection: ScopeMenu stays in the world view (not the flat window) while raised and shortly after.
    bool ScopeMenuInWorld();
    // Raised and the game's ScopeMenu is open: the next frame is rendered as the scope view.
    bool ViewActive();
    // The ScopeMenu FlatScreenModel copy is wanted next frame (kept out of the world image).
    bool WantMenuCopy();
    // Presenter, eye publish: replaces the eyes of a_ef with the scope camera (needs ViewActive()). Records the pose for
    // the frame's submission.
    void FillEyes(stereo::EyeFrame& a_ef);
    // Presenter, at submission: true when the frame being presented was rendered as the scope view (FillEyes at its begin).
    bool PresentedIsScope();
    // Presenter, at submission of a scope frame: the eye region [a_x, a_x + a_w) x [0, a_h) of the back buffer, masked round,
    // the ScopeMenu copy a_menu on top, on the scope quad. False = nothing submitted (caller falls back to the eyes).
    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_backBuffer, unsigned a_x, unsigned a_w, unsigned a_h,
        ID3D11Texture2D* a_menu, bool a_menuPremultiplied);
    // The blackout fade over the world (0..1) while the gun comes up (not scoped).
    float Fade();

    // GameHud H1 (render / main thread, after a renderer's post FX): for the renderer "ScopeMenu" the game's scope overlay
    // (ScopeMenu.swf: reticle + ring) is in the pooled targets 0x3E (glass) / 0x3F (movie); copied for the scope image.
    bool WantOverlayTargets();
    void OnOverlayTargets(void* a_glassTex, void* a_movieTex);  // ID3D11Texture2D*
    // GameHud H2: the ScopeMenu renderer's own screen draw is left out while the scope image carries its overlay
    bool SkipScopeMenuDraw();
    // GameHud H2, renderer "ScopeMenu": its offscreen target id and mode fields (diagnostics, logged on change)
    void NoteOverlayRenderer(int a_target, int a_mode60, int a_mode68, int a_mode6C, int a_rt1C0, int a_rt1C4);

    // Main thread (ScopeGame.cpp): the drawn weapon (scoped gun?, zoom) and the WEAPON node cull.
    namespace detail
    {
        void MainTick();
        struct WeaponInfo
        {
            bool        scopedGun{ false };  // drawn, type gun, instance flags kHasScope (0x200000)
            float       fovMult{ 0.0f };     // ZoomData fovMult = magnification (zoomed FOV = the game's FOV / it; 0 = none)
            float       zoomHDeg{ 0.0f };    // the flat game's zoomed horizontal FOV (0 = unknown)
            float       rawFovMult{ 0.0f };  // as read (diagnostics)
            float       defaultFov{ 0.0f };  // fDefaultWorldFOV:Display (0 = not found)
            unsigned    form{ 0 };
            int         iconId{ -1 };        // Pip-Boy icon id (flat GetItemIconID B41520)
            float       scopeOffX{ 0.0f };   // FO4VR FUN_140f0b220 scope point offset (weapon frame, game units)
            float       scopeOffZ{ 8.0f };
        };
        WeaponInfo Weapon();
        void       SetWantCull(bool a_cull);  // any thread; applied at the main tick
        std::string GameStatus();
    }

    std::string Status();
    std::string Command(const std::string& a_arg);
}
