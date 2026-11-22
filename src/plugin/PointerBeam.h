#pragma once
// Pointer beam: the visible laser from the pointing controller to what it points at, with an end dot at the hit.
// One component for every pointer consumer (menu pointer on the cinema quad / menu panels / flat window, terminals on
// their monitor, the wrist Pip-Boy pointer, the world pick in workshop and companion-command mode, the activation pick,
// and any later UI such as the in-VR config panel): each consumer OFFERS a ray for this frame; per hand the best offer
// is drawn (a hit beats a miss, then the higher priority), as OpenXR compositor quads (XrRuntime::SubmitBeam), so it is
// stereo-correct and draws over the menu it ends on.
//
// FO4VR reference (Fallout4VR.exe 1.2.72, work/handoff/ui-menus.md "Pointer beam"): PlayerCharacter "PrimaryWandLaserPointer", a
// procedural 0.5 x 1 x 0.5 unit vertex-coloured box (cyan 0,1,1), stretched along the primary wand's forward axis to the
// first collision (max fPrimaryWorkshopGrabDistance:VR = 1200 units), updated only in workshop mode and while commanding
// a companion. FO4VR has no end dot and no laser on menus or the Pip-Boy (touchpad navigation there).
//
// Frame protocol (Presenter thread): BeginFrame() right after xr::Runtime::BeginFrame, consumers Offer() during that
// frame's update, Submit() just before xr::Runtime::EndFrame of the same XR frame.

#include <string>

struct ID3D11DeviceContext;

namespace xr
{
    class Runtime;
    struct Pose;
    struct Vec3;
}

namespace fo4flatvr::beam
{
    struct Config
    {
        bool  enabled{ true };            // [Beam] bBeam: master switch
        bool  dot{ true };                // [Beam] bDot: end dot at the hit point (replaces the in-image cursor dots)
        float rgba[4]{ 0.0f, 1.0f, 1.0f, 1.0f };  // [Beam] fColorR/G/B/fAlpha (FO4VR: vertex colour 0,1,1,1)
        float widthCm{ 0.7f };            // [Beam] fWidthCm (FO4VR box: 0.5 units = 0.71 cm)
        float maxLengthM{ 17.1f };        // [Beam] fMaxLengthM: length when nothing is hit (FO4VR: 1200 units)
        float dotSizeDeg{ 0.8f };         // [Beam] fDotSizeDeg: end dot size as seen from the hand (constant angle)
        float dotMinCm{ 0.5f };           // [Beam] fDotMinCm: smallest end dot (very close hits, the wrist Pip-Boy)
        float startCm{ 1.0f };            // [Beam] fStartCm: beam starts this far ahead of the aim pose (FO4VR: 3 units)
        bool  menus{ true };              // [Beam] bMenus: menu pointer (flat window, menu panels, dialogue panel, terminals)
        bool  pipboy{ true };             // [Beam] bPipboy: wrist / projected Pip-Boy pointer (only while on the screen)
        bool  workshop{ true };           // [Beam] bWorkshop: FO4VR's laser - workshop mode and commanding a companion
        bool  activate{ false };          // [Beam] bActivate: FO4VR's activation preview (bShowWandActivationPreview:VR,
                                          //   default 0 there too) while the world view is up: a cone from the hand
                                          //   (white; green when the activation pick has a target) + end marker.
        float activateReachM{ 1.371f };   // [Beam] fActivateReachM: preview length (FO4VR fPrimaryWandMaxGrabDistance 96 u)
        float activateFloorMult{ 1.5f };  // [Beam] fActivateFloorMult: longer when pointing down (fDownwardGrabFloorDistanceMultiplier)
        float activateFadeStartS{ 10.0f };  // [Beam] fActivateFadeStartS: cone fully visible this long (fActivationPreviewFadeTime1)
        float activateFadeS{ 10.0f };     // [Beam] fActivateFadeS: then fades out over this long (fActivationPreviewFadeTime2);
                                          //   restarts after any menu / mode change. 0 and 0 = never fade.
        float activateMarkerCm{ 18.8f };  // [Beam] fActivateMarkerCm: end marker size (activateArc_*.nif at scale 0.5)
        bool  activatePipboyColor{ false };  // [Beam] bActivatePipboyColor: Pip-Boy colour instead of white/green
                                             //   (bWandTrajetoryUsePipboyColor:VR)
        bool  leftHanded{ false };        // [Controllers] bLeftHanded: primary (pointing) hand
    };

    void          Configure(const Config& a_cfg);
    // Reads [Beam] from a_iniPath into a Config (+ a_leftHanded) and applies it.
    void          LoadAndConfigure(const std::string& a_iniPath, bool a_leftHanded);
    const Config& GetConfig();
    void          SetEnabled(bool a_on);  // pipe "beam on|off"
    bool          Enabled();
    enum class Source : int
    {
        kWorld = 0,   // workshop / companion command (FO4VR's laser)
        kActivate,    // activation preview (opt-in; FO4VR look: cone + marker, Ray::pressed = has a target)
        kPipboy,      // wrist / projected Pip-Boy pointer
        kMenu,        // menu pointer (cinema quad, panels, terminals)
        kExternal,    // other UI (in-VR config panel, ...)
        kCount
    };

    // True while beams with end dots are on for a_src: consumers that otherwise draw their own cursor dot into their
    // image (menuptr::PrepareCinema, the Pip-Boy screen) skip theirs then ([Beam] bDot=0 or bBeam=0 brings them back).
    bool DotReplacesCursor(Source a_src);

    struct Ray
    {
        Source src{ Source::kExternal };
        int    priority{ 0 };      // among offers of the same hit state: higher wins (default: by source, kMenu highest)
        bool   hit{ false };       // the ray ends on something: draw to `length` with the end dot
        float  length{ 0.0f };     // metres from the aim pose to the hit (ignored on a miss: maxLengthM)
        bool   pressed{ false };   // trigger held (dot turns green)
    };

    // Presenter, right after xr::Runtime::BeginFrame: clears last frame's offers and keeps this frame's head pose (the
    // ribbon faces it). a_head nullptr = no head pose (nothing drawn). a_worldAllowed: the world view takes hand input
    // (first person, no flat window / VATS / wrist Pip-Boy / terminal screen): world beams may be shown.
    void BeginFrame(const xr::Pose* a_head, bool a_worldAllowed);
    // Any consumer during the frame's update: a ray from the aim pose of hand a_hand (0 left, 1 right; Primary() = the
    // pointing hand). The beam follows that hand's aim pose (OpenXR .../aim/pose, -Z forward) at Submit time.
    void Offer(int a_hand, const Ray& a_ray);
    int  Primary();  // 1 = right unless [Controllers] bLeftHanded

    // Presenter, just before xr::Runtime::EndFrame (same XR frame as BeginFrame): adds the world offers (workshop /
    // command / activation; game queries, Present thread) and submits one beam (+ end dot) per hand.
    void Submit(xr::Runtime& a_rt, ID3D11DeviceContext* a_ctx);

    std::string Status();  // pipe "beam"
}
