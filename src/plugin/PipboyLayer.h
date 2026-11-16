#pragma once
// Wrist Pip-Boy (feature D3, stage 1), D3D/XR side: owns the copies of the Pip-Boy screen render targets,
// places the screen as a world-locked OpenXR quad on the off-hand controller grip, casts the aim-hand ray
// onto it and turns it into pointer state for the game side. No RE types here (game side: GamePipboy.*).
// Research: analysis/vr-pipboy.md.

#include <string>

struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
}

namespace fo4flatvr::pipboy
{
    struct Config
    {
        bool  enabled{ true };            // [Pipboy] bWristPipboy
        float widthMeters{ 0.20f };       // [Pipboy] fWidthMeters (height follows the 0x3C aspect)
        float offset[3]{ -0.05f, 0.0f, 0.13f };  // [Pipboy] fOffsetX/Y/Z: quad centre in the Pip-Boy hand's GRIP space (m),
                                                 //   given for the left hand (-X = back of the left hand, +Z = toward the
                                                 //   elbow); X is mirrored for the right hand
        float rotDeg[3]{ 0.0f, 0.0f, 0.0f };      // [Pipboy] fRotXDeg/fRotYDeg/fRotZDeg: extra rotation about the quad's
                                                 //   own axes (applied Y, then X, then Z) on top of the wrist base pose
        bool  leftHanded{ false };        // [Controllers] bLeftHanded: Pip-Boy on the right hand, pointer from the left
        int   source{ 0x3D };             // [Pipboy] iSourceTarget: 61 = 0x3D offscreen (menu + post FX; measured: the only one
                                          //   written for the wrist Pip-Boy), 60 = 0x3C (falls back to 0x3D while never copied)
        bool  stopImod{ true };           // [Pipboy] bStopImod: stop PipboyMenuImod after a wrist open (world stays unfiltered)
        bool  cursorDot{ true };          // [Pipboy] bCursorDot: draw the pointer position into the displayed screen
        bool  tint{ true };               // [Pipboy] bTint: tint the shown screen like the game's PipboyScreen effect
                                          //   (luminance x fPipboyEffectColorR/G/B:Pipboy x brightness)
        // Power armour: projected Pip-Boy (FO4VR bUIPowerArmorGeometry_UseProjectedPipboy:VRUI = 1; analysis/vr-power-armor.md)
        bool  projected{ false };          // [PowerArmor] bProjectedPipboy: in PA open directly, screen on a quad in front of the head
        float projDistanceMeters{ 0.60f };  // [PowerArmor] fPipboyDistanceMeters: quad centre ahead of the eyes
        float projHeightMeters{ -0.12f };   // [PowerArmor] fPipboyHeightMeters: centre offset from eye level (negative = below)
        float projWidthMeters{ 0.50f };     // [PowerArmor] fPipboyWidthMeters
        float projFollowSeconds{ 0.30f };   // [PowerArmor] fPipboyFollowSeconds: lag of the upright anchor behind the head (0 = head-locked)
        // FO4VR bAlwaysUseProjectedPipboy:VR (VR_UseProjectedPipboy C325E0): the projected screen outside power armour too.
        bool  floating{ false };            // [Pipboy] bFloating: open directly, screen floating in front of the head (projected
                                            //   placement above); the game pauses as with the flat PipboyMenu (kPausesGame)
    };

    void          Configure(const Config& a_cfg);
    const Config& GetConfig();
    void          SetEnabled(bool a_on);  // pipe "pipboy on|off" (applies to the next open)
    bool          Enabled();              // switch on and the game hooks are installed
    // Power armour projected Pip-Boy: switch ([PowerArmor] bProjectedPipboy, pipe "pa pipboy on|off") and hooks in.
    bool          ProjectedEnabled();
    void          SetProjectedEnabled(bool a_on);
    // Floating Pip-Boy outside power armour ([Pipboy] bFloating, pipe "pipboy floating on|off"; next open).
    bool          FloatingEnabled();
    void          SetFloatingEnabled(bool a_on);
    // H2 (render thread): a projected Pip-Boy is open -> skip renderer "PipboyMenu"'s RenderMain (the PA dash screen).
    bool          SkipWorldScreen();

    // ---- render thread (game hooks) -------------------------------------------------------------
    bool WantsTargets();  // copies are wanted (wrist Pip-Boy open, or a pipcap pending)
    // H1 (after DrawPostFX) for every renderer while WantsTargets(): a_isPipboy = renderer "PipboyMenu";
    // a_tex3D = its offscreen target 0x3D (ID3D11Texture2D*). Arms the 0x3C copy below.
    void OnRendererPostFX(bool a_isPipboy, void* a_tex3D);
    // H3 (after RenderEffect 0xC offscreen -> final): a_tex3C = render target 0x3C. Copied only if the last H1
    // was the "PipboyMenu" renderer.
    void OnFinalTarget(void* a_tex3C);

    // ---- Presenter (main thread, inside Present) -------------------------------------------------
    // Right after BeginFrame (fresh controller poses, same predicted display time as the layer that will
    // carry this frame): computes the quad pose from the Pip-Boy hand's grip, the aim-hand ray hit, and
    // hands the pointer to the game side (injects only while the wrist Pip-Boy is open).
    // a_rightStickY: thumbstick for scrolling. Returns true if the pointer is on the quad (the trigger then
    // clicks instead of acting as the pad's right trigger).
    // a_head: this frame's predicted head pose in the reference space (null if not located); used by the
    // projected (power armour) placement.
    bool UpdateFrame(xr::Runtime& a_rt, bool a_began, const xr::Pose* a_head = nullptr);
    // World view, after the projection/HUD: queue the Pip-Boy quad (pose from the UpdateFrame of the frame
    // being ended). Returns true if a layer was queued.
    bool Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx);
    // End of every Present: clears per-frame flags, services "pipcap".
    void OnPresent();
    // True while the pointer is on the quad and the wrist Pip-Boy is open (pad trigger suppressed).
    bool PointerOnQuad();

    // Pipe "pipcap <path.bmp>": next Present writes <path> (the layer source) and <stem>_3c.bmp / <stem>_3d.bmp
    // (BGRA, alpha kept) and logs statistics (non-black pixels, mean, bbox, hash).
    void        RequestCapture(std::string a_path);
    std::string Status();  // pipe "pipboy"
    void        SetSource(int a_rt);
    void        SetTint(bool a_on);  // pipe "pipboy tint on|off"
}
