#pragma once
// Separated HUD (feature D1), D3D/XR side: owns the copies of the engine's HUD render targets, decides
// when the world image drops its HUD (H2), and submits the HUD as its own head-locked OpenXR quad.
// No RE types here (game side: GameHud.*).

#include <string>

struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
}

namespace fo4flatvr::hud
{
    struct Config
    {
        bool  enabled{ true };          // [HUD] bSeparateHud
        float distanceMeters{ 1.2f };   // [HUD] fDistanceMeters: quad centre ahead of the eyes
        float widthMeters{ 1.0f };      // [HUD] fWidthMeters (height follows the render aspect)
        float heightMeters{ -0.25f };   // [HUD] fHeightMeters: centre offset from eye level (negative = below)
        bool  hideCrosshair{ true };    // [HUD] bHideCrosshair: hide the screen-centre crosshair while the layer is up
        int   source{ 0x3E };           // [HUD] iSourceTarget: engine render target shown on the layer (0x3E glass result, 0x3F raw movie)
        bool  premultiplied{ true };    // [HUD] bPremultiplied: colour is already multiplied by alpha
    };

    void Configure(const Config& a_cfg);
    const Config& GetConfig();

    // ---- FO4VR HUD split (20 Nov; analysis/vr-hud.md section 9) ------------------------------------------------
    // FO4VR splits HUDMenu into world-space menus (Fallout4VR.exe 1.2.72 SendHUDMessage::ShowHUD 140AFBEB0, BuildVRNodes
    // 140EF4AE0): HP / rads / AP / ammo / explosives / effects / fatigue on the primary wand (WSPrimaryWandHUD,
    // world_primaryWand.nif on PrimaryUIAttachNode), the compass low under the chin (WSHMDHUDStatus, world_HMD_status.nif on
    // UprightHmdNode), notifications / subtitles / quest / XP on UprightHmdLagNode (WSHMDHUDInfo). Here: the clips' screen
    // rectangles (Scaleform bounds, game::HudSplitTick) are cut out of the HUD copy into a piece atlas and shown on their own
    // quads (xr::Runtime::SubmitHudPieces): the meters on the aim hand, the compass on the chin panel. The compass shows per
    // [HUD] iCompassShow (default: while a hand is raised, not high; VRIK's palm-up rule as option 3) and slides like VRIK's.
    // Every key is live: pipe "hud set <key> <value>" (config panel / MCM), "hud split" = status.
    struct SplitConfig
    {
        bool  handHud{ true };              // [HUD] bHandHud: meters on the aim hand (FO4VR WSPrimaryWandHUD)
        float handWidthM{ 0.13f };          // [HUD] fHandHudWidthM
        float handX{ -0.09f };              // [HUD] fHandHudX/Y/Z (m) in the aim pose (X right, mirrored for the left hand;
        float handY{ -0.02f };              //   Y up; Z back toward you). FO4VR: fPrimaryWandHUDX:VRUI -6 u (8.6 cm) inward
        float handZ{ 0.0f };
        float handPitchDeg{ 0.0f };         // [HUD] fHandHudPitchDeg: tilt about the panel's X (+ = top toward you)
        bool  compassSplit{ true };         // [HUD] bCompassChin: compass on FO4VR's chin panel (UprightHmdNode)
        float compassDistM{ 0.32f };        // [HUD] fCompassDistanceM: ahead of the eyes (FO4VR world_HMD_status.nif: 0.22-0.42)
        float compassHeightM{ -0.55f };     // [HUD] fCompassHeightM: below the eyes (FO4VR: 0.47-0.64 below)
        float compassWidthM{ 0.55f };       // [HUD] fCompassWidthM (FO4VR panel 0.6 m, curved)
        int   compassShow{ 2 };             // [HUD] iCompassShow: 0 always, 1 never, 2 hand raised (not high), 3 palm up (VRIK)
        int   compassHand{ 0 };             // [HUD] iCompassHand: 0 either, 1 left, 2 right
        float raiseLowM{ 0.45f };           // [HUD] fCompassRaiseLowM: hand at most this far below the eyes ...
        float raiseHighM{ 0.05f };          // [HUD] fCompassRaiseHighM: ... and at least this far below ("not high")
        float palmUpDot{ 0.65f };           // [HUD] fCompassPalmUpDot: VRIK's palm-up threshold (cos 49 deg)
        float slideM{ 0.714f };             // [HUD] fCompassSlideM: VRIK hides by sliding 50 u (0.71 m) down
        float slideRate{ 10.0f };           // [HUD] fCompassSlideRate: VRIK's rate (dz = dt * 10 * (z - 1), ~0.39 s)
        bool  leftHanded{ false };          // [Controllers] bLeftHanded: the aim hand (hand HUD) is the left one
    };
    void ConfigureSplit(const SplitConfig& a_cfg);
    SplitConfig GetSplitConfig();
    void LoadSplitConfig();  // reads the [HUD] split keys from the settings file (start-up)
    // Pipe "hud set <key> <value>" (ini key name, case-insensitive): live change. Returns the status line or an error.
    std::string SetSplitValue(const std::string& a_key, const std::string& a_value);
    std::string SplitStatus();
    // Game side (UI thread): a piece's rectangle in the HUD target (0..1 of the buffer). a_piece: 0 = hand left part
    // (HP), 1 = hand right part (AP / ammo), 2 = compass. a_valid false = the clips were not found (piece stays on the HUD).
    void SetPieceRect(int a_piece, float a_u0, float a_v0, float a_u1, float a_v1, bool a_valid);
    void SetPieceAniso(float a_sxOverSy);  // stage -> HUD target scale x / y (1 = square pixels; the quads undo it)
    bool SplitWanted();  // any piece enabled (game side polls the clip bounds only then)
    // Presenter, after BeginFrame: this frame's head (reference space; null = none) for the compass frame and the gesture.
    void UpdateFrame(xr::Runtime& a_rt, bool a_began, const xr::Pose* a_head);

    // Runtime switch (pipe "hud on|off"); only effective when the game hooks are installed.
    void SetEnabled(bool a_on);
    bool Enabled();

    // ---- render thread (game hooks) -------------------------------------------------------------
    // H1: the engine's HUDScreenModel targets were just drawn; copy them now (pooled targets are reused
    // by the next renderer). Arguments are ID3D11Texture2D* (either may be null).
    void OnHudTargets(void* a_tex3E, void* a_tex3F);
    // H2: skip HUDScreenModel's RenderMain this frame (separation on and the HUD layer is shown).
    bool SkipWorldHud();
    // C1: hide the crosshair widget this update.
    bool HideCrosshair();
    void NoteWorldHudSkipped();

    // ---- Presenter (main thread, inside Present) -------------------------------------------------
    // After the projection layer: queue the HUD copied during this frame's render on its own quad.
    // Returns true if a HUD layer was queued.
    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx);
    // End of every Present: a_layerNext = the next frame is shown as the world view with XR running,
    // i.e. the HUD goes to its own layer and leaves the world image. Also services "hudcap".
    void OnPresent(bool a_layerNext);

    // Pipe "hudcap <path.bmp>": on the next Present writes <path> (the layer source) plus <stem>_3e.bmp and
    // <stem>_3f.bmp, 32-bit BGRA with the alpha byte kept, and logs alpha statistics for each.
    void RequestCapture(std::string a_path);

    // ---- Power armour (B7; analysis/vr-power-armor.md) ------------------------------------------
    // FO4VR drops the flat screen-attached PA dash (renderer "PowerArmorRenderer": helmet frame, 3D dials, compass
    // movie through HUD glass) and the visor rain ("HUDRainRenderer") from the eye view and shows its own PA overlay
    // as a separate head-following model. Here: the PowerArmorRenderer composite (what RenderMain would draw onto
    // the frame) is captured into our own textures and composited under the HUD on the HUD layer; the world
    // image no longer carries it. Rain is dropped from the world view.
    // [PowerArmor] bHudOnLayer / bHideVisorRain; pipe "pa hud|rain on|off".
    void ConfigurePowerArmor(bool a_hudOnLayer, bool a_hideRain);
    void SetPaHudOnLayer(bool a_on);
    void SetHideVisorRain(bool a_on);
    bool PaHudOnLayer();    // render thread: capture the PA dash this frame (switch on and the HUD layer is active)
    bool HideVisorRain();   // render thread: skip HUDRainRenderer's RenderMain this frame
    // Render thread, from H2 instead of RenderMain(PowerArmorRenderer, dest): a_destTex/a_destRtv = the engine
    // target RenderMain draws into (ID3D11Texture2D* / ID3D11RenderTargetView*). Saves it, lets a_draw render
    // the dash over transparent black and over opaque white (alpha = 1 - (white - black), premultiplied colour
    // = the black result), keeps both, restores the target. Bindings are untouched (Copy/Clear only). False =
    // nothing done (caller draws normally).
    bool CapturePaOverlay(void* a_destTex, void* a_destRtv, void (*a_draw)(void*), void* a_ctx);
    std::string PaStatus();  // pipe "pa": PA HUD part

    // ---- Menu panel (flat 2D menus over the live world view; FO4VR WS*MenuModel on UprightHmdLagNode) ---------
    // While a panel menu (PauseMenu, FavoritesMenu, MessageBoxMenu: Presenter decides) is open in the world view, the
    // "FlatScreenModel" renderer (every GameMenuBase menu with kCustomRendering draws there, incl. ButtonBarMenu) is
    // copied in H1 and skipped in H2, so the menu leaves the world image; the Presenter shows the copy (source alpha)
    // on the cinema quad, anchored in front of the user when the menu opened. [Menus] bPanel / iPanelSource.
    void ConfigureMenuPanel(bool a_enabled, int a_source, bool a_premultiplied);
    bool MenuPanelEnabled();                       // switch on and H1/H2 installed
    void SetMenuPanelEnabled(bool a_on);           // pipe "menupanel on|off"
    bool MenuPanelWanted();                        // H1, renderer "FlatScreenModel": copy this draw
    void OnMenuTargets(void* a_tex3E, void* a_tex3F);  // H1: copy (ID3D11Texture2D*)
    bool SkipWorldMenu();                          // H2, renderer "FlatScreenModel": skip (panel active this frame)
    void NoteWorldMenuSkipped();
    // Presenter: the panel decision for the NEXT frame (H2 skip), set at every Present after the submissions.
    void SetMenuPanelNext(bool a_on);
    // Presenter: texture to show (the copy of this frame, or the last one while the panel stays up; null = none) and
    // whether it is premultiplied.
    void* MenuPanelTexture(bool& a_premultiplied);
    std::string MenuPanelStatus();                 // pipe "menupanel"
    // 3D item previews of panel menus (Container/Barter "Container3D", Cooking/Workbench "WorkbenchItem3D", Examine,
    // Workshop, Lockpicking, Book: own Interface3D renderers that RenderMain composites screen-space; FO4VR keeps them
    // with the world-space menu models). While the panel is active, H2 gives such a renderer's RenderMain to
    // CaptureMenuPreview instead of the world image: drawn over transparent black and over opaque white (same scheme as
    // CapturePaOverlay; a second renderer in the frame accumulates in draw order), destination restored. MenuPanelTexture
    // then returns the menu copy composited with it (over or under, by draw order). [Menus] bPanelPreview.
    void SetMenuPreviewEnabled(bool a_on);
    bool MenuPreviewEnabled();
    bool MenuPreviewWanted();                      // H2: panel active this frame and the switch on
    bool CaptureMenuPreview(const char* a_name, void* a_destTex, void* a_destRtv, void (*a_draw)(void*), void* a_ctx);
    // Pause-menu "decal" on the HUD layer (measured 14 Nov: after a PauseMenu the HUDScreenModel copy keeps ~960k px of the
    // menu image, identical grid to the menu capture): the HUD-glass renderers share the pooled targets 0x3E/0x3F and
    // HUD glass starts from an effect pass (RenderEffect 0x43 in RenderPrepassesAndMenus) instead of a clear, so what a
    // FlatScreenModel menu left in them is carried into the HUD. H2 calls this right after FlatScreenModel's RenderMain
    // (or its skip) with the RTVs H1 saw for that renderer: both are cleared to (0,0,0,0). [Menus] bClearMenuTargets.
    bool ClearMenuTargetsWanted();
    void SetClearMenuTargets(bool a_on);           // pipe "menupanel clear on|off"
    void ClearMenuTargets(void* a_rtv3E, void* a_rtv3F);  // ID3D11RenderTargetView*
    // Same for HUDScreenModel itself (H2, after its RenderMain or skip): HUD elements that are gone (pickup text,
    // HIDDEN / CAUTION) would otherwise linger through that feedback. [HUD] bClearTargets; pipe "hud clear on|off".
    bool ClearHudTargetsWanted();
    void SetClearHudTargets(bool a_on);

    // Pipe "hud": one status line (state, counters, last capture statistics).
    std::string Status();
    // Pipe "hud src 3e|3f", "hud premul 0|1", "hud crosshair 0|1".
    void SetSource(int a_rt);
    void SetPremultiplied(bool a_on);
    void SetHideCrosshair(bool a_on);
}
