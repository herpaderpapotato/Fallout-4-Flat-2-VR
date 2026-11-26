#pragma once
// OpenXR + D3D11 core for fo4flatvr. No game dependency: the plugin and tests/xr_smoke.cpp both use it.
//
// Threading: every call except Init/Shutdown must come from the one thread that owns the D3D11
// immediate context (in the game: the thread that calls IDXGISwapChain::Present).
//
// Coordinate conventions are OpenXR's (right-handed, +Y up, -Z forward, metres). Conversion to the
// game's Z-up units happens in the plugin, never here.

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>

namespace xr
{
    struct Vec3 { float x{}, y{}, z{}; };
    struct Quat { float x{}, y{}, z{}, w{ 1.0f }; };
    struct Pose { Quat orientation; Vec3 position; };
    struct Fov  { float angleLeft{}, angleRight{}, angleUp{}, angleDown{}; };  // radians, as XrFovf

    struct EyeView { Pose pose; Fov fov; };

    enum class SessionState : std::uint8_t
    {
        kNone,          // no session (Init not called or failed)
        kIdle,
        kReady,
        kSynchronized,
        kVisible,
        kFocused,
        kStopping,
        kLossPending,
        kExiting,
    };

    struct FrameInfo
    {
        bool          shouldRender{ false };  // XrFrameState::shouldRender
        std::int64_t  predictedDisplayTime{ 0 };
        std::int64_t  predictedDisplayPeriod{ 0 };
        bool          viewsValid{ false };     // orientation+position valid for both eyes
        EyeView       eyes[2];                 // [0] left, [1] right; in the reference space the layers use
        bool          headValid{ false };      // orientation+position valid for head
        Pose          head;                    // VIEW space located in the reference space (the space SubmitMono's
                                               // renderPose and the projection layer use)
    };

    struct ControllerState
    {
        bool  active{ false };          // actions synced this frame and the session is focused
        float leftStickX{}, leftStickY{}, rightStickX{}, rightStickY{};  // -1..1, +Y up
        float leftTrigger{}, rightTrigger{};    // 0..1
        float leftGrip{}, rightGrip{};          // 0..1
        bool  a{}, b{}, x{}, y{};               // Quest: A/B right hand, X/Y left hand
        bool  leftStickClick{}, rightStickClick{};
        bool  menu{};                           // left-hand menu button
        Pose  leftAim{}, rightAim{};            // aim poses in the reference space
        bool  leftAimValid{}, rightAimValid{};
        Pose  leftGripPose{}, rightGripPose{};  // grip poses (OpenXR .../input/grip/pose) in the reference space
        bool  leftGripPoseValid{}, rightGripPoseValid{};
    };

    struct QuadConfig
    {
        float widthMeters{ 3.0f };      // height follows the source aspect ratio
        float distanceMeters{ 2.5f };   // straight ahead of the recentred origin
        float heightMeters{ 0.0f };     // centre offset from the origin's eye level
    };

    struct Config
    {
        std::string appName{ "fo4flatvr" };
        bool        preferStageSpace{ false };  // default LOCAL (seated)
        QuadConfig  quad;
    };

    class Runtime
    {
    public:
        Runtime();
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // Creates instance, system, D3D11 session on a_device (fails cleanly if the runtime's
        // required adapter LUID differs from a_device's adapter) and the reference space.
        // Returns false and leaves the object inert on any failure; lastError() says why.
        bool Init(ID3D11Device* a_device, const Config& a_config);
        void Shutdown();

        // Pump xrPollEvent and drive the session state machine (xrBeginSession on READY,
        // xrEndSession on STOPPING). Cheap; call every frame.
        void PollEvents();

        // xrWaitFrame + xrBeginFrame + xrLocateViews. Returns false when no frame should be
        // begun (session not running). Blocks in xrWaitFrame to pace to the headset.
        bool BeginFrame(FrameInfo& a_out);

        // Copy a_src (any 2D texture: the game's backbuffer is the main case) into the quad
        // swapchain for this frame. Handles size/format differences (incl. typeless and sRGB
        // variants) with a GPU blit. The quad swapchain is (re)created to match a_src's size.
        // Must be called between BeginFrame and EndFrame. a_ctx is the immediate context.
        // a_alphaBlend: keep the source alpha and blend the quad over the layers below (a menu panel over the world
        // view; a_premultiplied as for SubmitHud); default opaque (the whole flat frame).
        void SubmitQuad(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, bool a_alphaBlend = false, bool a_premultiplied = true);

        // Menus (FO4VR UprightHmdLagNode, frozen while a menu is open): place the cinema quad in front of a_head (yaw only,
        // upright; centre = head position + quad (0, heightMeters, -distanceMeters) turned by the head yaw). It stays
        // world-locked there until the next call or a recentre. a_head in the reference space (FrameInfo::head).
        void AnchorCinemaQuad(const Pose& a_head);

        // Cinema quad geometry for the following anchors (width, distance ahead of the anchor, centre height offset;
        // metres). The menu presenter sets FO4VR's workbench geometry (world_projectedMenu.nif) for crafting menus and the
        // configured cinema geometry otherwise. Takes effect at once (pose and size are derived every frame).
        void SetCinemaQuadGeometry(float a_widthMeters, float a_distanceMeters, float a_heightMeters);
        // Per-frame pose override (reference space) + width for the cinema quad, e.g. a menu on the off-hand controller
        // (FO4VR workshop menu on the secondary wand); nullptr = back to the anchored placement. Pointer hit tests
        // (CinemaQuadPose) follow it.
        void SetCinemaQuadOverride(const Pose* a_poseInRef, float a_widthMeters);
        // 21 Nov (workshop agent): show only a sub-rectangle of the cinema quad's image (uv 0..1, origin top-left: u0, v0, u1,
        // v1); the quad keeps its width and takes the sub-rectangle's aspect. nullptr = the whole image. CinemaQuadCrop reports
        // the active crop (pointer hits map quad uv -> image uv with it).
        void SetCinemaQuadCrop(const float* a_uv4);
        bool CinemaQuadCrop(float a_uv4[4]) const;

        // Backdrop: in this frame, if no new projection image is submitted (SubmitMono / SubmitEye), show the last one
        // again with the pose/FOV it was rendered with (world-locked, the runtime reprojects it): mono = the mono
        // swapchain's last released image (no copy), eyes = each eye's last stage. Without this call such a frame has
        // no projection layer. Between BeginFrame and EndFrame.
        void SubmitBackdrop();
        [[nodiscard]] std::uint64_t BackdropFrames() const noexcept;  // diagnostics: frames that re-showed an old projection

        // World panorama (round 4: the surround behind the VATS window / any flat window, never black).
        // A cube map (1024^2 per face, RGBA16F) in the GAME-WORLD orientation (reference space turned by a_refToWorld, the
        // player's heading as a yaw), filled from the head-tracked projection images as the user looks around: each
        // CapturePanorama reprojects this frame's submitted image (SubmitMono, or one of the SubmitEye images, alternating)
        // into the cube with the pose/FOV it was rendered with. Directions only (the world is treated as far away).
        // Call after SubmitMono / SubmitEye in the same frame. Returns false if nothing was captured.
        bool CapturePanorama(ID3D11DeviceContext* a_ctx, const Quat& a_refToWorld);
        // Show the cube as this frame's projection layer, world-locked (both eyes rendered from this frame's located views,
        // turned by a_refToWorld), colour scaled by a_brightness. Directions never captured are filled from the blurred
        // neighbourhood (cube mips) and, failing that, the average colour of everything seen, scaled by a_holeBrightness
        // in addition: dim, never black. Takes precedence over SubmitBackdrop; ignored when SubmitMono/SubmitEye ran in
        // this frame. Between BeginFrame and EndFrame.
        bool SubmitPanorama(ID3D11DeviceContext* a_ctx, const Quat& a_refToWorld, float a_brightness, float a_holeBrightness);
        void ResetPanorama();                        // forget everything captured (load / new cell)
        [[nodiscard]] std::string PanoramaReport() const;  // diagnostics: captures, shows, coverage state

        // Mono world view: copy a_src (same handling as SubmitQuad, own swapchain sized to a_src) and, at
        // EndFrame, submit a projection layer whose two views both show that image with pose a_renderPose
        // and fov a_renderFov, i.e. exactly what the game rendered with (reference space, as FrameInfo::head).
        // The runtime's reprojection then corrects the latency between render and display.
        // Can be combined with SubmitQuad in the same frame: the projection layer goes first, the quad on top.
        // Calling either twice in one frame replaces that layer's image. Returns false if nothing was queued
        // (no frame begun, !shouldRender, or an error: see lastError()).
        bool SubmitMono(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_renderPose, const Fov& a_renderFov);

        // Symmetric FOV covering both eyes' FOVs from the last located views (max |left|,|right| and max
        // |up|,|down| across eyes). If a_aspect > 0 (width / height), the narrower axis is widened so
        // tan(h) / tan(v) == a_aspect. Before any views are located it starts from 90 x 90 degrees.
        [[nodiscard]] Fov MonoRenderFov(float a_aspect) const;

        // AFR stereo. The game renders one eye per frame (any order/cadence) and hands it in here:
        // a_src is copied into that eye's own intermediate texture (same copy/gamma/state rules as
        // SubmitQuad), replacing only that eye; a_renderPose/a_renderFov are what it was rendered with
        // (reference space, like FrameInfo::eyes). Call between BeginFrame and EndFrame.
        // At EndFrame, once BOTH eyes have an image (possibly from different frames), each eye's latest
        // image is blitted into a freshly acquired image of that eye's swapchain and one projection layer
        // is submitted with view[i] = eye i's image/pose/fov. Until both eyes have one, no projection is
        // submitted. A SubmitMono in the same frame takes precedence (the eye images are kept).
        // Returns false if nothing was stored (no frame begun, bad args, or an error: see lastError()).
        bool SubmitEye(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, int a_eye, const Pose& a_renderPose, const Fov& a_renderFov);
        // Perf pass 26 Nov: the same for the region [a_x, a_x + a_width) x [0, a_height) of a_src (the double-wide back buffer),
        // without the caller's split copy. a_direct: the region is copied straight into the acquired swapchain image (one
        // copy per eye; needs an 8-bit source of the swapchain's format family, else the stage path is used). Frames with no
        // new eye image then show the swapchain's last released image again.
        bool SubmitEyeRegion(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, unsigned a_x, unsigned a_width, unsigned a_height, int a_eye,
            const Pose& a_renderPose, const Fov& a_renderFov, bool a_direct);
        std::string EyeCopyStatus() const;

        // HUD overlay: a_src (alpha kept, same copy/gamma/state rules as SubmitQuad, own swapchain sized
        // to a_src) on a head-locked quad layer. a_poseInView is the quad centre/orientation in VIEW space
        // (OpenXR: -Z forward, +Y up); width a_widthMeters, height from a_src's aspect. a_premultiplied:
        // colour already multiplied by alpha (Scaleform drawn into a (0,0,0,0)-cleared target) -> only
        // SOURCE_ALPHA; false adds UNPREMULTIPLIED_ALPHA. Layer order at EndFrame: projection, HUD,
        // cinema quad. Between BeginFrame and EndFrame; returns false if nothing was queued.
        bool SubmitHud(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_poseInView, float a_widthMeters,
            bool a_premultiplied = true);

        // HUD pieces (20 Nov, HUD agent; FO4VR's split HUD: wand HUD, chin compass): one "piece atlas" image (own swapchain,
        // alpha kept, same copy rules as SubmitHud) shown as up to kMaxHudPieces quad layers, each a sub-rectangle of the
        // atlas (pixels) with its own pose - in VIEW space (inView, head-locked) or in the REFERENCE space (a hand, an upright
        // head frame). Width a_pieces[i].widthMeters, height from the rectangle's aspect. Layered right after the HUD quad.
        // Once per frame with all pieces, between BeginFrame and EndFrame; false = nothing queued.
        static constexpr int kMaxHudPieces = 4;
        struct HudPiece
        {
            std::int32_t x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
            Pose         pose{};
            bool         inView{ false };
            float        widthMeters{ 0.1f };
            float        heightMeters{ 0.0f };  // 0 = from the rectangle's aspect
        };
        bool SubmitHudPieces(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_atlas, const HudPiece* a_pieces, int a_count,
            bool a_premultiplied = true);
        [[nodiscard]] std::uint64_t HudPieceLayersSubmitted() const noexcept;  // diagnostics: piece layers sent to xrEndFrame

        // World-locked quads: a_src (same copy/gamma/state rules as SubmitQuad, own swapchain per slot sized to
        // a_src) on a quad layer at a_poseInRef in the REFERENCE space (FrameInfo::head's space; the caller re-poses
        // it every frame, e.g. from a controller grip pose). Width a_widthMeters, height from a_src's aspect.
        // a_alphaBlend = false: opaque (alpha ignored); true: source alpha kept, a_premultiplied as for SubmitHud.
        // a_slot 0..kMaxWorldQuads-1, one layer each: 0 = wrist Pip-Boy screen, 1 = dialogue option panel.
        // Layer order at EndFrame: projection, vignette, HUD, world quads (slot order), cinema quad, fade.
        // Between BeginFrame and EndFrame; returns false if nothing was queued.
        // 20 Nov: slot kTopWorldQuad (4, the VR settings panel) is layered after the cinema quad instead (below the beams).
        // 26 Nov: slots above kTopWorldQuad (5..7) are other plugins' panels (PanelHost.h), layered with it after the cinema quad.
        static constexpr int kMaxWorldQuads = 8;
        static constexpr int kTopWorldQuad = 4;
        bool SubmitWorldQuad(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_poseInRef, float a_widthMeters,
            bool a_alphaBlend = false, bool a_premultiplied = true, int a_slot = 0);
        [[nodiscard]] std::uint64_t WorldQuadsSubmitted(int a_slot) const noexcept;  // diagnostics: xrEndFrame calls with that slot's layer

        // Fade (camera step 1): a black head-locked quad over every other layer, opacity a_alpha (0..1; <= 0 =
        // nothing submitted). Own tiny swapchain, re-cleared only when the alpha changes. Layer order at
        // EndFrame: projection, HUD, world quad, cinema quad, fade. Between BeginFrame and EndFrame.
        bool SubmitFade(ID3D11DeviceContext* a_ctx, float a_alpha);
        [[nodiscard]] std::uint64_t FadeLayersSubmitted() const noexcept;  // diagnostics: frames with a fade layer

        // Comfort vignette (C8): a head-locked black quad (0.25 m ahead, 4 x 4 m, like the fade) whose opacity grows
        // with the angle theta from the view axis: alpha = a_alpha * smoothstep(a_innerDeg, a_outerDeg, theta)
        // (premultiplied black). The 256x256 image is generated on the CPU and re-uploaded only when a_alpha
        // (quantised to 1/64) or the angles change; otherwise the last image is shown again. <= 0 = nothing
        // submitted. Layer order at EndFrame: projection, vignette, HUD, world quad, cinema quad, fade.
        // Between BeginFrame and EndFrame.
        bool SubmitVignette(ID3D11DeviceContext* a_ctx, float a_alpha, float a_innerDeg, float a_outerDeg);
        [[nodiscard]] std::uint64_t VignetteLayersSubmitted() const noexcept;  // diagnostics: frames with a vignette layer
        [[nodiscard]] std::uint64_t VignetteUploads() const noexcept;          // diagnostics: image regenerations
        [[nodiscard]] std::uint32_t LastLayerCount() const noexcept;           // layers in the last xrEndFrame
        [[nodiscard]] std::uint32_t MaxLayerCount() const noexcept;          // 26 Nov: the runtime's composition layer limit (0 unknown)
        [[nodiscard]] std::uint64_t FramesOverLayerLimit() const noexcept;   // frames that left layers out to stay within it

        // Pointer beams (laser pointer; plugin/PointerBeam.h decides when and where). Slot 0..kMaxBeams-1 (one per hand):
        // a ribbon quad from a_from to a_to (REFERENCE space), a_widthMeters wide, turned about its own axis to face
        // a_viewer (the head), coloured a_rgba (sRGB, straight alpha; soft edges). a_dotSizeMeters > 0 adds an end dot
        // at a_to facing a_viewer (white with a dark rim, green while a_pressed). Compositor layers, so both eyes see it
        // at its true 3D place (stereo-correct) and it draws over the panels/menus it ends on. All beams of a frame share
        // one colour image (the last a_rgba wins). Layer order at EndFrame: ..., cinema quad, beams, end dots, fade.
        // Between BeginFrame and EndFrame; returns false if nothing was queued.
        // a_style kBeamPreview: FO4VR's activation preview (bShowWandActivationPreview) instead - the ribbon is a cone
        // (full width at a_from, a point at a_to; alpha rising from 0 at a_from to a_rgba[3] at a_to: world_targetIndicator_ray.nif)
        // and the end marker an additive glow disc a_dotSizeMeters across in a_rgba's colour: a_pressed = "target" (bright
        // core + halo, activateArc_Target.nif) else a faint even disc (activateArc_noTarget.nif). Own images (the laser's stay).
        static constexpr int kMaxBeams = 2;
        // a_dotAt: the end dot / marker there instead of at a_to. A ribbon with a_rgba[3] <= 0 is not drawn (its dot still is).
        static constexpr int kBeamLaser = 0, kBeamPreview = 1;
        bool SubmitBeam(ID3D11DeviceContext* a_ctx, int a_slot, const Vec3& a_from, const Vec3& a_to, const Vec3& a_viewer, float a_widthMeters,
            const float a_rgba[4], float a_dotSizeMeters, bool a_pressed, int a_style = kBeamLaser, const Vec3* a_dotAt = nullptr);
        [[nodiscard]] std::uint64_t BeamLayersSubmitted() const noexcept;  // diagnostics: beam layers (all slots) sent to xrEndFrame
        [[nodiscard]] std::uint64_t BeamUploads() const noexcept;          // diagnostics: beam/dot image (re)generations

        // Symmetric FOV containing eye a_eye's (asymmetric) FOV from the last located views, with the
        // narrower axis widened so tan(h) / tan(v) == a_aspect (width / height) when a_aspect > 0.
        // Before any views are located it starts from 90 x 90 degrees.
        [[nodiscard]] Fov EyeRenderFov(int a_eye, float a_aspect) const;

        // Test hook: when enabled, EndFrame also copies each eye's finished swapchain image (before
        // releasing it) into a CPU-readable STAGING texture. CapturedEyeImage returns it AddRef'd
        // (caller releases), or nullptr. Costs a full-image copy per eye per frame; off by default.
        void SetEyeCapture(bool a_enable) noexcept;
        [[nodiscard]] ID3D11Texture2D* CapturedEyeImage(int a_eye) const;
        // Same test hook for the HUD swapchain image (captured in SubmitHud while eye capture is on).
        [[nodiscard]] ID3D11Texture2D* CapturedHudImage() const;

        // xrEndFrame with whatever layers were submitted since BeginFrame (none is valid).
        void EndFrame();

        // Latest state, updated by BeginFrame (xrSyncActions once per frame when the session is focused).
        // Input setup failing doesn't fail Init (lastError() says why); active then stays false.
        [[nodiscard]] const ControllerState& Controllers() const noexcept;
        // hand 0 = left, 1 = right; amplitude 0..1 (<= 0 stops); seconds <= 0 = shortest pulse the runtime supports.
        void Vibrate(int hand, float amplitude, float seconds);

        // Re-anchor the quad (and later, the play origin) to the current head yaw/position.
        void Recenter();

        // Cinema quad placement (the SubmitQuad layer) in the reference space: centre + orientation, width and
        // height in metres (height from the last submitted quad image's aspect; 9:16 before the first). For
        // pointer hit tests (menu pointer). Any thread that owns the context; cheap.
        void CinemaQuadPose(Pose& a_pose, float& a_width, float& a_height) const;

        [[nodiscard]] SessionState state() const noexcept;
        [[nodiscard]] bool         running() const noexcept;  // session begun and not stopping
        [[nodiscard]] const std::string& lastError() const noexcept;
        [[nodiscard]] std::string  runtimeName() const;      // XrInstanceProperties::runtimeName + version
        [[nodiscard]] std::string  apiLayers() const;        // 20 Nov: API layers the loader offers (implicit ones load) + ReShade modules seen
        [[nodiscard]] std::int64_t swapchainFormat() const noexcept;  // quad, else mono, else eye swapchain DXGI_FORMAT; 0 before the first submit

        // 22 Nov (hidden-area agent): XR_KHR_visibility_mask. Enabled at instance creation when the runtime offers it; the
        // hidden-triangle mesh of both eyes is fetched once the session runs and again on XrEventDataVisibilityMaskChangedKHR
        // (PollEvents). Vertices are view-space tangents (x right, y up, on the z = -1 plane), three indices per triangle.
        // VisibilityMaskSerial() changes whenever a new mesh is stored (0 = none yet); poll it and re-read.
        [[nodiscard]] bool          VisibilityMaskSupported() const noexcept;
        [[nodiscard]] std::uint64_t VisibilityMaskSerial() const noexcept;
        bool                        VisibilityMask(int a_eye, std::vector<float>& a_xy, std::vector<std::uint32_t>& a_indices) const;

    private:
        struct Impl;
        Impl* _impl;
    };
}
