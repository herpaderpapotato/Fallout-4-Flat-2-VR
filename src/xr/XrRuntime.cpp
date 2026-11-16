#include "xr/XrRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

using Microsoft::WRL::ComPtr;

namespace xr
{
    namespace
    {
        constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

        // Fullscreen triangle; the source is the same size as the target, so Load() by pixel.
        constexpr char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float4 t = src.Load(int3(pos.xy, 0));
    float3 c = t.rgb;
#if DECODE_SRGB
    c = saturate(c);
    c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#endif
#if KEEP_ALPHA
    return float4(c, t.a);
#else
    return float4(c, 1);
#endif
}
)";

        // World panorama: capture (a projection image -> cube faces) and show (cube -> an eye image). Fullscreen triangle VS
        // (kBlitHlsl's vs); SV_Position in pixels. Directions: game-world frame expressed in OpenXR axes (+Y up).
        constexpr char kPanoHlsl[] = R"(
cbuffer C : register(b0)
{
    float4 qa;          // capture: world -> source view; show: eye view -> world
    float4 tn;          // tan(angleLeft), tan(angleRight), tan(angleUp), tan(angleDown)
    float2 sz;          // target size in pixels
    float  bright;      // show: colour scale
    float  holeBright;  // show: extra scale where nothing was captured
    uint   face;        // capture: cube face 0..5
    float  maxLod;      // show: last mip
    float2 pad;
};
SamplerState smp : register(s0);
float3 rot(float4 q, float3 v)
{
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
float3 faceDir(uint f, float2 uv)
{
    float s = uv.x * 2 - 1, t = uv.y * 2 - 1;
    if (f == 0) return float3(1, -t, -s);
    if (f == 1) return float3(-1, -t, s);
    if (f == 2) return float3(s, 1, t);
    if (f == 3) return float3(s, -1, -t);
    if (f == 4) return float3(s, -t, 1);
    return float3(-s, -t, -1);
}
#if PANO_CAPTURE
Texture2D<float4> src : register(t0);
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float3 w = normalize(faceDir(face, pos.xy / sz));
    float3 v = rot(qa, w);
    if (v.z > -1e-3) discard;
    float2 p = v.xy / -v.z;
    float2 uv = float2((p.x - tn.x) / (tn.y - tn.x), (tn.z - p.y) / (tn.z - tn.w));
    if (any(uv < 0.005) || any(uv > 0.995)) discard;
    return float4(src.SampleLevel(smp, uv, 0).rgb, 1);
}
#else
TextureCube<float4> cube : register(t0);
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy / sz;
    float3 v = normalize(float3(lerp(tn.x, tn.y, uv.x), lerp(tn.z, tn.w, uv.y), -1));
    float3 w = rot(qa, v);
    // fallback: the average colour of everything seen (top mip of each face), else a dark grey
    float4 avg = cube.SampleLevel(smp, float3(1, 0, 0), maxLod) + cube.SampleLevel(smp, float3(-1, 0, 0), maxLod) +
                 cube.SampleLevel(smp, float3(0, 1, 0), maxLod) + cube.SampleLevel(smp, float3(0, -1, 0), maxLod) +
                 cube.SampleLevel(smp, float3(0, 0, 1), maxLod) + cube.SampleLevel(smp, float3(0, 0, -1), maxLod);
    float3 col = avg.a > 1e-4 ? avg.rgb / avg.a : float3(0.12, 0.12, 0.13);
    // pull-push hole fill: coarse to fine, each level's coverage-normalised colour over the coarser result
    [unroll] for (int i = 5; i >= 0; --i) {
        float4 s = cube.SampleLevel(smp, w, maxLod * i / 5.0);
        if (s.a > 1e-4) col = lerp(col, s.rgb / s.a, saturate(s.a * (i == 0 ? 1.0 : 2.0)));
    }
    float seen = saturate(cube.SampleLevel(smp, w, 0).a);
    col *= bright * lerp(holeBright, 1.0, seen);
#if DECODE_SRGB
    col = saturate(col);
    col = col <= 0.04045 ? col / 12.92 : pow((col + 0.055) / 1.055, 2.4);
#endif
    return float4(col, 1);
}
#endif
)";

        struct PanoCb
        {
            float        qa[4];
            float        tn[4];
            float        sz[2];
            float        bright;
            float        holeBright;
            std::uint32_t face;
            float        maxLod;
            float        pad[2];
        };
        static_assert(sizeof(PanoCb) == 64);

        struct SourceFormat
        {
            DXGI_FORMAT texture;  // typeless family format for the intermediate copy target
            DXGI_FORMAT view;     // non-sRGB view used to sample it
            bool        linear;   // values are linear light (float formats) rather than sRGB-encoded
        };

        SourceFormat ClassifySource(DXGI_FORMAT a_fmt)
        {
            switch (a_fmt) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, false };
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, false };
            case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                return { DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, false };
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, false };
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, true };
            case DXGI_FORMAT_R11G11B10_FLOAT:
                return { DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, true };
            default:
                return { a_fmt, a_fmt, false };
            }
        }

        DXGI_FORMAT NonSrgb(DXGI_FORMAT a_fmt)
        {
            switch (a_fmt) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
            default:                              return a_fmt;
            }
        }

        bool IsTypeless(DXGI_FORMAT a_fmt)
        {
            switch (a_fmt) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                return true;
            default:
                return false;
            }
        }

        XrQuaternionf Mul(const XrQuaternionf& a, const XrQuaternionf& b)
        {
            return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        }

        XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
        {
            const XrQuaternionf p{ v.x, v.y, v.z, 0.0f };
            const XrQuaternionf r = Mul(Mul(q, p), XrQuaternionf{ -q.x, -q.y, -q.z, q.w });
            return { r.x, r.y, r.z };
        }

        Pose ToPose(const XrPosef& p)
        {
            return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w },
                     { p.position.x, p.position.y, p.position.z } };
        }

        SessionState ToState(XrSessionState s)
        {
            switch (s) {
            case XR_SESSION_STATE_IDLE:         return SessionState::kIdle;
            case XR_SESSION_STATE_READY:        return SessionState::kReady;
            case XR_SESSION_STATE_SYNCHRONIZED: return SessionState::kSynchronized;
            case XR_SESSION_STATE_VISIBLE:      return SessionState::kVisible;
            case XR_SESSION_STATE_FOCUSED:      return SessionState::kFocused;
            case XR_SESSION_STATE_STOPPING:     return SessionState::kStopping;
            case XR_SESSION_STATE_LOSS_PENDING: return SessionState::kLossPending;
            case XR_SESSION_STATE_EXITING:      return SessionState::kExiting;
            default:                            return SessionState::kNone;
            }
        }

        // Captures exactly the immediate-context bindings the blit changes and puts them back.
        struct SavedContextState
        {
            static constexpr UINT kMaxClassInstances = 256;

            ID3D11DeviceContext*      ctx{};
            ID3D11InputLayout*        inputLayout{};
            D3D11_PRIMITIVE_TOPOLOGY  topology{};
            ID3D11VertexShader*       vs{};
            ID3D11ClassInstance*      vsInst[kMaxClassInstances]{};
            UINT                      vsInstCount{ kMaxClassInstances };
            ID3D11HullShader*         hs{};
            ID3D11ClassInstance*      hsInst[kMaxClassInstances]{};
            UINT                      hsInstCount{ kMaxClassInstances };
            ID3D11DomainShader*       ds{};
            ID3D11ClassInstance*      dsInst[kMaxClassInstances]{};
            UINT                      dsInstCount{ kMaxClassInstances };
            ID3D11GeometryShader*     gs{};
            ID3D11ClassInstance*      gsInst[kMaxClassInstances]{};
            UINT                      gsInstCount{ kMaxClassInstances };
            ID3D11PixelShader*        ps{};
            ID3D11ClassInstance*      psInst[kMaxClassInstances]{};
            UINT                      psInstCount{ kMaxClassInstances };
            ID3D11ShaderResourceView* psSrv0{};
            ID3D11Buffer*             soTargets[D3D11_SO_BUFFER_SLOT_COUNT]{};
            ID3D11RasterizerState*    rs{};
            D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                      viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
            ID3D11BlendState*         blend{};
            FLOAT                     blendFactor[4]{};
            UINT                      sampleMask{};
            ID3D11DepthStencilState*  dss{};
            UINT                      stencilRef{};
            ID3D11RenderTargetView*   rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView*   dsv{};

            explicit SavedContextState(ID3D11DeviceContext* a_ctx) :
                ctx(a_ctx)
            {
                ctx->IAGetInputLayout(&inputLayout);
                ctx->IAGetPrimitiveTopology(&topology);
                ctx->VSGetShader(&vs, vsInst, &vsInstCount);
                ctx->HSGetShader(&hs, hsInst, &hsInstCount);
                ctx->DSGetShader(&ds, dsInst, &dsInstCount);
                ctx->GSGetShader(&gs, gsInst, &gsInstCount);
                ctx->PSGetShader(&ps, psInst, &psInstCount);
                ctx->PSGetShaderResources(0, 1, &psSrv0);
                ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, soTargets);
                ctx->RSGetState(&rs);
                ctx->RSGetViewports(&viewportCount, viewports);
                ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
                ctx->OMGetDepthStencilState(&dss, &stencilRef);
                ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
            }

            ~SavedContextState()
            {
                // Unbind our SRV/RTV first so restoring can't hit an SRV/RTV hazard with our resources.
                ID3D11ShaderResourceView* nullSrv = nullptr;
                ctx->PSSetShaderResources(0, 1, &nullSrv);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
                ctx->OMSetDepthStencilState(dss, stencilRef);
                ctx->OMSetBlendState(blend, blendFactor, sampleMask);
                ctx->RSSetViewports(viewportCount, viewports);
                ctx->RSSetState(rs);
                UINT soOffsets[D3D11_SO_BUFFER_SLOT_COUNT];
                std::fill(std::begin(soOffsets), std::end(soOffsets), static_cast<UINT>(-1));  // append
                ctx->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, soTargets, soOffsets);
                ctx->PSSetShaderResources(0, 1, &psSrv0);
                ctx->PSSetShader(ps, psInst, psInstCount);
                ctx->GSSetShader(gs, gsInst, gsInstCount);
                ctx->DSSetShader(ds, dsInst, dsInstCount);
                ctx->HSSetShader(hs, hsInst, hsInstCount);
                ctx->VSSetShader(vs, vsInst, vsInstCount);
                ctx->IASetPrimitiveTopology(topology);
                ctx->IASetInputLayout(inputLayout);

                auto release = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
                release(inputLayout);
                release(vs); release(hs); release(ds); release(gs); release(ps);
                for (auto& p : vsInst) release(p);
                for (auto& p : hsInst) release(p);
                for (auto& p : dsInst) release(p);
                for (auto& p : gsInst) release(p);
                for (auto& p : psInst) release(p);
                release(psSrv0);
                for (auto& p : soTargets) release(p);
                release(rs);
                release(blend);
                release(dss);
                for (auto& p : rtvs) release(p);
                release(dsv);
            }

            SavedContextState(const SavedContextState&) = delete;
            SavedContextState& operator=(const SavedContextState&) = delete;
        };
    }

    struct Runtime::Impl
    {
        Config      config;
        std::string error;
        std::string runtimeName;

        ComPtr<ID3D11Device> device;

        // OpenXR
        XrInstance        instance{ XR_NULL_HANDLE };
        XrSystemId        systemId{ XR_NULL_SYSTEM_ID };
        XrSession         session{ XR_NULL_HANDLE };
        XrSpace           refSpace{ XR_NULL_HANDLE };
        XrSpace           viewSpace{ XR_NULL_HANDLE };
        XrEnvironmentBlendMode blendMode{ XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        XrSessionState    xrState{ XR_SESSION_STATE_UNKNOWN };
        bool              sessionRunning{ false };
        bool              sessionLost{ false };   // XR_ERROR_SESSION_LOST / LOSS_PENDING: recreate later
        bool              instanceLost{ false };
        std::chrono::steady_clock::time_point nextRecreateAttempt{};
        PFN_xrGetD3D11GraphicsRequirementsKHR getGraphicsRequirements{ nullptr };

        // Frame
        bool   frameBegun{ false };
        XrTime displayTime{ 0 };
        bool   shouldRender{ false };
        bool   quadSubmitted{ false };
        bool   quadBlend{ false };        // cinema quad: source alpha (menu panel over the world view)
        bool   quadPremultiplied{ true };
        bool   monoSubmitted{ false };
        XrPosef monoPose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        XrFovf  monoFov{};
        // Projection bookkeeping for the backdrop (SubmitBackdrop): which kind the last projection layer was.
        bool          eyeSubmitted{ false };       // SubmitEye called in this frame
        bool          backdropWanted{ false };     // SubmitBackdrop called in this frame
        bool          lastProjMono{ false };       // the last projection layer was mono (else eyes)
        bool          monoReleased{ false };       // the mono swapchain has a released image
        std::uint64_t backdropFrames{ 0 };

        // One per layer: the source copy (intermediate texture with SRV) and the swapchain it's blitted into.
        struct ColorSwapchain
        {
            XrSwapchain handle{ XR_NULL_HANDLE };
            DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
            UINT        width{ 0 };
            UINT        height{ 0 };
            std::vector<ComPtr<ID3D11RenderTargetView>> rtvNative;  // swapchain format
            std::vector<ComPtr<ID3D11RenderTargetView>> rtvUnorm;   // non-sRGB alias, only when the image is typeless
            std::vector<ID3D11Texture2D*>               images;     // owned by the runtime
            ComPtr<ID3D11Texture2D>          stage;
            ComPtr<ID3D11ShaderResourceView> stageSrv;
            D3D11_TEXTURE2D_DESC             stageDesc{};
            bool                             stageLinear{ false };  // stage holds linear (float) values
            bool                             hasContent{ false };   // stage holds a submitted image
            bool                             keepAlpha{ false };    // blit keeps source alpha (HUD); else alpha = 1
        };
        ColorSwapchain quadSc;
        ColorSwapchain monoSc;

        // HUD overlay quad (head-locked, VIEW space), source alpha kept.
        ColorSwapchain          hudSc;
        bool                    hudSubmitted{ false };
        bool                    hudPremultiplied{ true };
        XrPosef                 hudPose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        XrExtent2Df             hudSize{ 1.0f, 0.5625f };
        ComPtr<ID3D11Texture2D> hudCapture;

        // World-locked quads, reference space: one swapchain per slot (0 = wrist Pip-Boy, 1 = dialogue panel, ...).
        struct WorldQuad
        {
            ColorSwapchain sc;
            bool           submitted{ false };
            bool           blend{ false };
            bool           premultiplied{ true };
            XrPosef        pose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
            XrExtent2Df    size{ 0.2f, 0.16f };
        };
        WorldQuad     world[Runtime::kMaxWorldQuads];
        std::uint64_t worldFrames[Runtime::kMaxWorldQuads]{};

        // Fade (camera step 1): black head-locked quad on top, cleared to (0,0,0,alpha) premultiplied.
        ColorSwapchain fadeSc;
        bool           fadeSubmitted{ false };
        float          fadeClearedAlpha{ -1.0f };  // alpha the last released image holds (-1 = none)
        std::uint64_t  fadeFrames{ 0 };

        // Comfort vignette (C8): head-locked radial alpha quad between the projection and the HUD. The image is
        // generated on the CPU into vigSrc (R8G8B8A8_UNORM) and copied through the usual stage/blit path.
        ColorSwapchain           vigSc;
        ComPtr<ID3D11Texture2D>  vigSrc;
        bool                     vigSubmitted{ false };
        float                    vigKey[3]{ -1.0f, -1.0f, -1.0f };  // alpha (quantised), inner, outer of the last image
        std::uint64_t            vigFrames{ 0 }, vigUploads{ 0 };
        std::uint32_t            lastLayerCount{ 0 };

        // AFR stereo: per-eye stage + swapchain; the stage keeps the eye's latest image across frames.
        ColorSwapchain eyeSc[2];
        XrPosef        eyeRenderPose[2]{ { { 0, 0, 0, 1 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        XrFovf         eyeRenderFov[2]{};
        bool           eyeCaptureEnabled{ false };
        ComPtr<ID3D11Texture2D> eyeCapture[2];

        // World panorama (CapturePanorama / SubmitPanorama): cube in the game-world orientation + per-eye show swapchains.
        struct Panorama
        {
            static constexpr UINT kFace = 1024;
            ComPtr<ID3D11Texture2D>          cube;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView>   rtv[6];
            UINT                             mips{ 0 };
            ComPtr<ID3D11PixelShader>        psCapture, psShow, psShowDecode;
            ComPtr<ID3D11Buffer>             cb;
            ComPtr<ID3D11SamplerState>       smp;
            bool                             triedInit{ false }, ok{ false };
            bool                             linear{ false };      // stored values are linear light (float sources)
            bool                             mipsDirty{ true };
            bool                             everCaptured{ false };
            ColorSwapchain                   sc[2];
            bool                             submitted{ false };
            XrPosef                          pose[2]{ { { 0, 0, 0, 1 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
            XrFovf                           fov[2]{};
            int                              nextEye{ 0 };
            std::uint64_t                    captures{ 0 }, shows{ 0 }, resets{ 0 };
        } pano;
        // this frame's located views (BeginFrame), for SubmitPanorama
        bool    viewsValid{ false };
        XrPosef viewPose[2]{ { { 0, 0, 0, 1 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };

        // Controller input: one action set, instance-level actions, per-session attach + aim spaces.
        struct Input
        {
            XrActionSet set{ XR_NULL_HANDLE };
            XrAction    leftStick{}, rightStick{}, leftTrigger{}, rightTrigger{}, leftGrip{}, rightGrip{};
            XrAction    a{}, b{}, x{}, y{}, leftStickClick{}, rightStickClick{}, menu{};
            XrAction    aim{}, haptic{};    // per hand (subaction paths)
            XrAction    grip{};             // grip pose, per hand
            XrPath      hand[2]{};          // /user/hand/left, /user/hand/right
            XrSpace     aimSpace[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
            XrSpace     gripSpace[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
            bool        attached{ false };
        } input;
        ControllerState controllers;

        // Blit resources
        ComPtr<ID3D11VertexShader>       blitVs;
        ComPtr<ID3D11PixelShader>        blitPs;        // passthrough
        ComPtr<ID3D11PixelShader>        blitPsDecode;  // sRGB -> linear
        ComPtr<ID3D11PixelShader>        blitPsAlpha;        // passthrough, source alpha kept
        ComPtr<ID3D11PixelShader>        blitPsAlphaDecode;  // sRGB -> linear, source alpha kept
        ComPtr<ID3D11RasterizerState>    blitRs;

        // Last located eye FOVs, for MonoRenderFov().
        bool   eyeFovValid{ false };
        XrFovf eyeFov[2]{};

        // Pose
        bool          recenterPending{ true };
        bool          headValid{ false };
        XrPosef       head{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        XrPosef       origin{ { 0, 0, 0, 1 }, { 0, 0, 0 } };  // yaw-only orientation
        float         quadAspect{ 9.0f / 16.0f };            // height / width
        bool          quadOverride{ false };                 // SetCinemaQuadOverride: pose/width given per frame
        XrPosef       quadOverridePose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        float         quadOverrideWidth{ 0.4f };

        // ---------------------------------------------------------------- errors

        std::string ResultString(XrResult a_result) const
        {
            char buf[XR_MAX_RESULT_STRING_SIZE]{};
            if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, a_result, buf)))
                std::snprintf(buf, sizeof(buf), "XrResult(%d)", static_cast<int>(a_result));
            return buf;
        }

        bool Check(XrResult a_result, const char* a_call)
        {
            if (XR_SUCCEEDED(a_result))
                return true;
            error = std::string(a_call) + " failed: " + ResultString(a_result);
            if (a_result == XR_ERROR_SESSION_LOST)
                sessionLost = true;
            else if (a_result == XR_ERROR_INSTANCE_LOST)
                instanceLost = true;
            return false;
        }

        bool CheckHr(HRESULT a_hr, const char* a_call)
        {
            if (SUCCEEDED(a_hr))
                return true;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s failed: HRESULT 0x%08X", a_call, static_cast<unsigned>(a_hr));
            error = buf;
            return false;
        }

        // ---------------------------------------------------------------- setup

        bool CreateInstance()
        {
            std::uint32_t count = 0;
            if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
                return false;
            std::vector<XrExtensionProperties> exts(count, { XR_TYPE_EXTENSION_PROPERTIES });
            if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data()), "xrEnumerateInstanceExtensionProperties"))
                return false;
            const bool hasD3D11 = std::any_of(exts.begin(), exts.end(), [](const XrExtensionProperties& e) {
                return std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
            });
            if (!hasD3D11) {
                error = "OpenXR runtime does not support " XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
                return false;
            }

            const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
            XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
            std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", config.appName.c_str());
            ci.applicationInfo.applicationVersion = 1;
            std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "xrcore");
            ci.applicationInfo.engineVersion = 1;
            ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;  // nothing here needs 1.1; widest runtime support
            ci.enabledExtensionCount = 1;
            ci.enabledExtensionNames = enabled;
            if (!Check(xrCreateInstance(&ci, &instance), "xrCreateInstance"))
                return false;

            XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
            if (!Check(xrGetInstanceProperties(instance, &props), "xrGetInstanceProperties"))
                return false;
            char name[XR_MAX_RUNTIME_NAME_SIZE + 64];
            std::snprintf(name, sizeof(name), "%s %u.%u.%u", props.runtimeName,
                          XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                          XR_VERSION_PATCH(props.runtimeVersion));
            runtimeName = name;

            return Check(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                               reinterpret_cast<PFN_xrVoidFunction*>(&getGraphicsRequirements)),
                         "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        }

        bool CreateSession()
        {
            XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
            sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            if (!Check(xrGetSystem(instance, &sgi, &systemId), "xrGetSystem"))
                return false;

            // Required before xrCreateSession, and tells us which adapter the runtime composites on.
            XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
            if (!Check(getGraphicsRequirements(instance, systemId, &req), "xrGetD3D11GraphicsRequirementsKHR"))
                return false;

            ComPtr<IDXGIDevice>  dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC    ad{};
            if (!CheckHr(device.As(&dxgiDevice), "QueryInterface(IDXGIDevice)") ||
                !CheckHr(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter") ||
                !CheckHr(adapter->GetDesc(&ad), "IDXGIAdapter::GetDesc"))
                return false;
            if (ad.AdapterLuid.LowPart != req.adapterLuid.LowPart || ad.AdapterLuid.HighPart != req.adapterLuid.HighPart) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "D3D11 device adapter LUID %08lX:%08lX does not match the adapter the OpenXR runtime requires (%08lX:%08lX)",
                              static_cast<unsigned long>(ad.AdapterLuid.HighPart), ad.AdapterLuid.LowPart,
                              static_cast<unsigned long>(req.adapterLuid.HighPart), req.adapterLuid.LowPart);
                error = buf;
                return false;
            }
            if (device->GetFeatureLevel() < req.minFeatureLevel) {
                error = "D3D11 device feature level is below the OpenXR runtime's minimum";
                return false;
            }

            std::uint32_t count = 0;
            if (!Check(xrEnumerateViewConfigurations(instance, systemId, 0, &count, nullptr), "xrEnumerateViewConfigurations"))
                return false;
            std::vector<XrViewConfigurationType> configs(count);
            if (!Check(xrEnumerateViewConfigurations(instance, systemId, count, &count, configs.data()), "xrEnumerateViewConfigurations"))
                return false;
            if (std::find(configs.begin(), configs.end(), kViewConfig) == configs.end()) {
                error = "OpenXR system does not support PRIMARY_STEREO";
                return false;
            }

            if (!Check(xrEnumerateEnvironmentBlendModes(instance, systemId, kViewConfig, 0, &count, nullptr), "xrEnumerateEnvironmentBlendModes"))
                return false;
            // Meta XR Simulator v207 answers the second call with XR_ERROR_SIZE_INSUFFICIENT for the count it just
            // reported: retry with a larger buffer, and fall back to OPAQUE (every VR runtime supports it).
            std::vector<XrEnvironmentBlendMode> modes;
            XrResult br = XR_ERROR_SIZE_INSUFFICIENT;
            for (std::uint32_t cap = (std::max)(count, 1u); cap <= 64 && br == XR_ERROR_SIZE_INSUFFICIENT; cap *= 4) {
                modes.assign(cap, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
                std::uint32_t got = 0;
                br = xrEnumerateEnvironmentBlendModes(instance, systemId, kViewConfig, cap, &got, modes.data());
                if (XR_SUCCEEDED(br)) {
                    modes.resize(got);
                }
            }
            if (XR_FAILED(br) || modes.empty()) {
                modes.assign(1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
            }
            blendMode = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != modes.end() ?
                            XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes.front();

            XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
            binding.device = device.Get();
            XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
            sci.next = &binding;
            sci.systemId = systemId;
            if (!Check(xrCreateSession(instance, &sci, &session), "xrCreateSession"))
                return false;
            xrState = XR_SESSION_STATE_UNKNOWN;

            if (!Check(xrEnumerateReferenceSpaces(session, 0, &count, nullptr), "xrEnumerateReferenceSpaces"))
                return false;
            std::vector<XrReferenceSpaceType> spaces(count);
            if (!Check(xrEnumerateReferenceSpaces(session, count, &count, spaces.data()), "xrEnumerateReferenceSpaces"))
                return false;
            const bool hasStage = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();

            XrReferenceSpaceCreateInfo rci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            rci.poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
            rci.referenceSpaceType = config.preferStageSpace && hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (!Check(xrCreateReferenceSpace(session, &rci, &refSpace), "xrCreateReferenceSpace(LOCAL/STAGE)"))
                return false;
            rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            if (!Check(xrCreateReferenceSpace(session, &rci, &viewSpace), "xrCreateReferenceSpace(VIEW)"))
                return false;

            recenterPending = true;
            headValid = false;
            if (input.set != XR_NULL_HANDLE && !AttachInput())
                DestroyInputSession();  // input disabled for this session; lastError() says why
            return true;
        }

        bool CreateBlitResources()
        {
            auto compile = [&](const char* a_entry, const char* a_target, const D3D_SHADER_MACRO* a_defines, ComPtr<ID3DBlob>& a_out) {
                ComPtr<ID3DBlob> errors;
                const HRESULT hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "xrcore_blit", a_defines, nullptr, a_entry, a_target,
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &a_out, &errors);
                if (FAILED(hr)) {
                    error = std::string("D3DCompile(") + a_entry + ") failed";
                    if (errors)
                        error += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
                    return false;
                }
                return true;
            };

            const D3D_SHADER_MACRO passthrough[] = { { "DECODE_SRGB", "0" }, { "KEEP_ALPHA", "0" }, { nullptr, nullptr } };
            const D3D_SHADER_MACRO decode[] = { { "DECODE_SRGB", "1" }, { "KEEP_ALPHA", "0" }, { nullptr, nullptr } };
            const D3D_SHADER_MACRO alpha[] = { { "DECODE_SRGB", "0" }, { "KEEP_ALPHA", "1" }, { nullptr, nullptr } };
            const D3D_SHADER_MACRO alphaDecode[] = { { "DECODE_SRGB", "1" }, { "KEEP_ALPHA", "1" }, { nullptr, nullptr } };
            ComPtr<ID3DBlob> vsBlob, psBlob, psDecodeBlob, psAlphaBlob, psAlphaDecodeBlob;
            if (!compile("vs", "vs_5_0", passthrough, vsBlob) || !compile("ps", "ps_5_0", passthrough, psBlob) ||
                !compile("ps", "ps_5_0", decode, psDecodeBlob) || !compile("ps", "ps_5_0", alpha, psAlphaBlob) ||
                !compile("ps", "ps_5_0", alphaDecode, psAlphaDecodeBlob))
                return false;

            if (!CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &blitVs), "CreateVertexShader") ||
                !CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &blitPs), "CreatePixelShader") ||
                !CheckHr(device->CreatePixelShader(psDecodeBlob->GetBufferPointer(), psDecodeBlob->GetBufferSize(), nullptr, &blitPsDecode), "CreatePixelShader") ||
                !CheckHr(device->CreatePixelShader(psAlphaBlob->GetBufferPointer(), psAlphaBlob->GetBufferSize(), nullptr, &blitPsAlpha), "CreatePixelShader") ||
                !CheckHr(device->CreatePixelShader(psAlphaDecodeBlob->GetBufferPointer(), psAlphaDecodeBlob->GetBufferSize(), nullptr, &blitPsAlphaDecode), "CreatePixelShader"))
                return false;

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            return CheckHr(device->CreateRasterizerState(&rd, &blitRs), "CreateRasterizerState");
        }

        // ---------------------------------------------------------------- input

        bool CreateAction(XrAction& a_out, const char* a_name, const char* a_localized, XrActionType a_type, bool a_perHand)
        {
            XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
            ci.actionType = a_type;
            std::snprintf(ci.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", a_name);
            std::snprintf(ci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", a_localized);
            if (a_perHand) {
                ci.countSubactionPaths = 2;
                ci.subactionPaths = input.hand;
            }
            return Check(xrCreateAction(input.set, &ci, &a_out), "xrCreateAction");
        }

        // Suggests a_binds for a_profile. One unsupported path makes the whole call fail with
        // XR_ERROR_PATH_UNSUPPORTED, so in that case each binding is probed alone (every call replaces the
        // profile's previous suggestion) and only the accepted ones are suggested again.
        void SuggestProfile(const char* a_profile, std::initializer_list<std::pair<XrAction, const char*>> a_binds)
        {
            XrPath profile = XR_NULL_PATH;
            if (!Check(xrStringToPath(instance, a_profile, &profile), "xrStringToPath(profile)"))
                return;
            std::vector<XrActionSuggestedBinding> all;
            for (const auto& [action, path] : a_binds) {
                XrPath binding = XR_NULL_PATH;
                if (Check(xrStringToPath(instance, path, &binding), "xrStringToPath(binding)"))
                    all.push_back({ action, binding });
            }
            auto suggest = [&](const XrActionSuggestedBinding* a_b, std::uint32_t a_n) {
                XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
                sb.interactionProfile = profile;
                sb.countSuggestedBindings = a_n;
                sb.suggestedBindings = a_b;
                return xrSuggestInteractionProfileBindings(instance, &sb);
            };
            XrResult r = suggest(all.data(), static_cast<std::uint32_t>(all.size()));
            if (r == XR_ERROR_PATH_UNSUPPORTED) {
                std::vector<XrActionSuggestedBinding> good;
                for (const auto& b : all)
                    if (XR_SUCCEEDED(suggest(&b, 1)))
                        good.push_back(b);
                if (good.empty())
                    return;  // profile unknown to this runtime
                r = suggest(good.data(), static_cast<std::uint32_t>(good.size()));
            }
            Check(r, "xrSuggestInteractionProfileBindings");
        }

        // Instance level; must run before the first session is created. Failure only disables input.
        bool CreateInputActions()
        {
            XrActionSetCreateInfo si{ XR_TYPE_ACTION_SET_CREATE_INFO };
            std::snprintf(si.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
            std::snprintf(si.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
            if (!Check(xrStringToPath(instance, "/user/hand/left", &input.hand[0]), "xrStringToPath") ||
                !Check(xrStringToPath(instance, "/user/hand/right", &input.hand[1]), "xrStringToPath") ||
                !Check(xrCreateActionSet(instance, &si, &input.set), "xrCreateActionSet"))
                return false;

            auto& in = input;
            const bool ok =
                CreateAction(in.leftStick, "left_stick", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
                CreateAction(in.rightStick, "right_stick", "Look", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
                CreateAction(in.leftTrigger, "left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT, false) &&
                CreateAction(in.rightTrigger, "right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT, false) &&
                CreateAction(in.leftGrip, "left_grip", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT, false) &&
                CreateAction(in.rightGrip, "right_grip", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT, false) &&
                CreateAction(in.a, "button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.b, "button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.x, "button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.y, "button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.leftStickClick, "left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.rightStickClick, "right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                CreateAction(in.aim, "aim", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
                CreateAction(in.grip, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
                CreateAction(in.haptic, "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
            if (!ok)
                return false;

            SuggestProfile("/interaction_profiles/oculus/touch_controller", {
                { in.leftStick, "/user/hand/left/input/thumbstick" },
                { in.rightStick, "/user/hand/right/input/thumbstick" },
                { in.leftTrigger, "/user/hand/left/input/trigger/value" },
                { in.rightTrigger, "/user/hand/right/input/trigger/value" },
                { in.leftGrip, "/user/hand/left/input/squeeze/value" },
                { in.rightGrip, "/user/hand/right/input/squeeze/value" },
                { in.a, "/user/hand/right/input/a/click" },
                { in.b, "/user/hand/right/input/b/click" },
                { in.x, "/user/hand/left/input/x/click" },
                { in.y, "/user/hand/left/input/y/click" },
                { in.leftStickClick, "/user/hand/left/input/thumbstick/click" },
                { in.rightStickClick, "/user/hand/right/input/thumbstick/click" },
                { in.menu, "/user/hand/left/input/menu/click" },
                { in.aim, "/user/hand/left/input/aim/pose" },
                { in.aim, "/user/hand/right/input/aim/pose" },
                { in.grip, "/user/hand/left/input/grip/pose" },
                { in.grip, "/user/hand/right/input/grip/pose" },
                { in.haptic, "/user/hand/left/output/haptic" },
                { in.haptic, "/user/hand/right/output/haptic" },
            });
            // Index: no menu button (system is reserved); left A/B stand in for X/Y.
            SuggestProfile("/interaction_profiles/valve/index_controller", {
                { in.leftStick, "/user/hand/left/input/thumbstick" },
                { in.rightStick, "/user/hand/right/input/thumbstick" },
                { in.leftTrigger, "/user/hand/left/input/trigger/value" },
                { in.rightTrigger, "/user/hand/right/input/trigger/value" },
                { in.leftGrip, "/user/hand/left/input/squeeze/value" },
                { in.rightGrip, "/user/hand/right/input/squeeze/value" },
                { in.a, "/user/hand/right/input/a/click" },
                { in.b, "/user/hand/right/input/b/click" },
                { in.x, "/user/hand/left/input/a/click" },
                { in.y, "/user/hand/left/input/b/click" },
                { in.leftStickClick, "/user/hand/left/input/thumbstick/click" },
                { in.rightStickClick, "/user/hand/right/input/thumbstick/click" },
                { in.aim, "/user/hand/left/input/aim/pose" },
                { in.aim, "/user/hand/right/input/aim/pose" },
                { in.grip, "/user/hand/left/input/grip/pose" },
                { in.grip, "/user/hand/right/input/grip/pose" },
                { in.haptic, "/user/hand/left/output/haptic" },
                { in.haptic, "/user/hand/right/output/haptic" },
            });
            // Vive wands: trackpads as sticks, squeeze is a click; right menu -> B. No A/X/Y.
            SuggestProfile("/interaction_profiles/htc/vive_controller", {
                { in.leftStick, "/user/hand/left/input/trackpad" },
                { in.rightStick, "/user/hand/right/input/trackpad" },
                { in.leftTrigger, "/user/hand/left/input/trigger/value" },
                { in.rightTrigger, "/user/hand/right/input/trigger/value" },
                { in.leftGrip, "/user/hand/left/input/squeeze/click" },
                { in.rightGrip, "/user/hand/right/input/squeeze/click" },
                { in.leftStickClick, "/user/hand/left/input/trackpad/click" },
                { in.rightStickClick, "/user/hand/right/input/trackpad/click" },
                { in.menu, "/user/hand/left/input/menu/click" },
                { in.b, "/user/hand/right/input/menu/click" },
                { in.aim, "/user/hand/left/input/aim/pose" },
                { in.aim, "/user/hand/right/input/aim/pose" },
                { in.grip, "/user/hand/left/input/grip/pose" },
                { in.grip, "/user/hand/right/input/grip/pose" },
                { in.haptic, "/user/hand/left/output/haptic" },
                { in.haptic, "/user/hand/right/output/haptic" },
            });
            SuggestProfile("/interaction_profiles/khr/simple_controller", {
                { in.leftTrigger, "/user/hand/left/input/select/click" },
                { in.rightTrigger, "/user/hand/right/input/select/click" },
                { in.menu, "/user/hand/left/input/menu/click" },
                { in.menu, "/user/hand/right/input/menu/click" },
                { in.aim, "/user/hand/left/input/aim/pose" },
                { in.aim, "/user/hand/right/input/aim/pose" },
                { in.grip, "/user/hand/left/input/grip/pose" },
                { in.grip, "/user/hand/right/input/grip/pose" },
                { in.haptic, "/user/hand/left/output/haptic" },
                { in.haptic, "/user/hand/right/output/haptic" },
            });
            return true;
        }

        // Per session. Failure only disables input.
        bool AttachInput()
        {
            if (input.set == XR_NULL_HANDLE)
                return false;
            XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
            ai.countActionSets = 1;
            ai.actionSets = &input.set;
            if (!Check(xrAttachSessionActionSets(session, &ai), "xrAttachSessionActionSets"))
                return false;
            for (int h = 0; h < 2; ++h) {
                XrActionSpaceCreateInfo ci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
                ci.action = input.aim;
                ci.subactionPath = input.hand[h];
                ci.poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
                if (!Check(xrCreateActionSpace(session, &ci, &input.aimSpace[h]), "xrCreateActionSpace(aim)"))
                    return false;
                ci.action = input.grip;
                if (!Check(xrCreateActionSpace(session, &ci, &input.gripSpace[h]), "xrCreateActionSpace(grip)"))
                    return false;
            }
            input.attached = true;
            return true;
        }

        void DestroyInputSession()
        {
            for (XrSpace& s : input.aimSpace) {
                if (s != XR_NULL_HANDLE)
                    xrDestroySpace(s);
                s = XR_NULL_HANDLE;
            }
            for (XrSpace& s : input.gripSpace) {
                if (s != XR_NULL_HANDLE)
                    xrDestroySpace(s);
                s = XR_NULL_HANDLE;
            }
            input.attached = false;
            controllers = {};
        }

        void DestroyInputInstance()
        {
            auto& in = input;
            for (XrAction a : { in.leftStick, in.rightStick, in.leftTrigger, in.rightTrigger, in.leftGrip, in.rightGrip, in.a, in.b, in.x,
                                in.y, in.leftStickClick, in.rightStickClick, in.menu, in.aim, in.haptic, in.grip })
                if (a != XR_NULL_HANDLE)
                    xrDestroyAction(a);
            if (in.set != XR_NULL_HANDLE)
                xrDestroyActionSet(in.set);
            input = {};
        }

        float GetFloat(XrAction a_action)
        {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = a_action;
            XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
            return Check(xrGetActionStateFloat(session, &gi, &st), "xrGetActionStateFloat") && st.isActive ? st.currentState : 0.0f;
        }

        bool GetBool(XrAction a_action)
        {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = a_action;
            XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
            return Check(xrGetActionStateBoolean(session, &gi, &st), "xrGetActionStateBoolean") && st.isActive && st.currentState;
        }

        void GetVec2(XrAction a_action, float& a_x, float& a_y)
        {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = a_action;
            XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
            const bool ok = Check(xrGetActionStateVector2f(session, &gi, &st), "xrGetActionStateVector2f") && st.isActive;
            a_x = ok ? st.currentState.x : 0.0f;
            a_y = ok ? st.currentState.y : 0.0f;
        }

        // Once per frame (from BeginFrame). Only syncs while FOCUSED; otherwise the state is zeroed.
        void SyncInput(XrTime a_time)
        {
            ControllerState cs{};
            if (input.attached && xrState == XR_SESSION_STATE_FOCUSED) {
                XrActiveActionSet active{ input.set, XR_NULL_PATH };
                XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
                si.countActiveActionSets = 1;
                si.activeActionSets = &active;
                const XrResult r = xrSyncActions(session, &si);
                if (Check(r, "xrSyncActions") && r == XR_SUCCESS) {  // XR_SESSION_NOT_FOCUSED: inputs inactive
                    auto& in = input;
                    cs.active = true;
                    GetVec2(in.leftStick, cs.leftStickX, cs.leftStickY);
                    GetVec2(in.rightStick, cs.rightStickX, cs.rightStickY);
                    cs.leftTrigger = GetFloat(in.leftTrigger);
                    cs.rightTrigger = GetFloat(in.rightTrigger);
                    cs.leftGrip = GetFloat(in.leftGrip);
                    cs.rightGrip = GetFloat(in.rightGrip);
                    cs.a = GetBool(in.a);
                    cs.b = GetBool(in.b);
                    cs.x = GetBool(in.x);
                    cs.y = GetBool(in.y);
                    cs.leftStickClick = GetBool(in.leftStickClick);
                    cs.rightStickClick = GetBool(in.rightStickClick);
                    cs.menu = GetBool(in.menu);
                    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    for (int h = 0; h < 2; ++h) {
                        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
                        if (!Check(xrLocateSpace(input.aimSpace[h], refSpace, a_time, &loc), "xrLocateSpace(aim)"))
                            continue;
                        const bool valid = (loc.locationFlags & kValid) == kValid;
                        (h == 0 ? cs.leftAimValid : cs.rightAimValid) = valid;
                        if (valid)
                            (h == 0 ? cs.leftAim : cs.rightAim) = ToPose(loc.pose);
                    }
                    for (int h = 0; h < 2; ++h) {
                        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
                        if (input.gripSpace[h] == XR_NULL_HANDLE ||
                            !Check(xrLocateSpace(input.gripSpace[h], refSpace, a_time, &loc), "xrLocateSpace(grip)"))
                            continue;
                        const bool valid = (loc.locationFlags & kValid) == kValid;
                        (h == 0 ? cs.leftGripPoseValid : cs.rightGripPoseValid) = valid;
                        if (valid)
                            (h == 0 ? cs.leftGripPose : cs.rightGripPose) = ToPose(loc.pose);
                    }
                }
            }
            controllers = cs;
        }

        // ---------------------------------------------------------------- teardown

        static void DestroySwapchain(ColorSwapchain& a_sc)
        {
            a_sc.rtvNative.clear();
            a_sc.rtvUnorm.clear();
            a_sc.images.clear();
            if (a_sc.handle != XR_NULL_HANDLE)
                xrDestroySwapchain(a_sc.handle);
            a_sc.handle = XR_NULL_HANDLE;
            a_sc.format = DXGI_FORMAT_UNKNOWN;
            a_sc.width = a_sc.height = 0;
        }

        void DestroySession()
        {
            DestroySwapchain(quadSc);
            DestroySwapchain(monoSc);
            DestroySwapchain(hudSc);
            for (auto& w : world) {
                DestroySwapchain(w.sc);
                w.submitted = false;
            }
            DestroySwapchain(fadeSc);
            fadeSubmitted = false;
            fadeClearedAlpha = -1.0f;
            DestroySwapchain(vigSc);
            vigSubmitted = false;
            vigKey[0] = vigKey[1] = vigKey[2] = -1.0f;
            DestroySwapchain(eyeSc[0]);
            DestroySwapchain(eyeSc[1]);
            DestroySwapchain(pano.sc[0]);
            DestroySwapchain(pano.sc[1]);
            pano.submitted = false;
            DestroyInputSession();
            if (viewSpace != XR_NULL_HANDLE)
                xrDestroySpace(viewSpace);
            if (refSpace != XR_NULL_HANDLE)
                xrDestroySpace(refSpace);
            if (session != XR_NULL_HANDLE)
                xrDestroySession(session);
            viewSpace = refSpace = XR_NULL_HANDLE;
            session = XR_NULL_HANDLE;
            sessionRunning = false;
            frameBegun = false;
            quadSubmitted = false;
            monoSubmitted = false;
            monoReleased = false;  // swapchains go with the session
            eyeSubmitted = false;
            backdropWanted = false;
            hudSubmitted = false;
            headValid = false;
            eyeFovValid = false;
        }

        void DestroyAll()
        {
            DestroySession();
            DestroyInputInstance();
            if (instance != XR_NULL_HANDLE)
                xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
            systemId = XR_NULL_SYSTEM_ID;
            xrState = XR_SESSION_STATE_UNKNOWN;
            sessionLost = instanceLost = false;
            auto resetStage = [](ColorSwapchain* sc) {
                sc->stage.Reset();
                sc->stageSrv.Reset();
                sc->stageDesc = {};
                sc->hasContent = false;
            };
            for (ColorSwapchain* sc : { &quadSc, &monoSc, &hudSc, &eyeSc[0], &eyeSc[1] }) {
                resetStage(sc);
            }
            for (auto& wq : world) {
                resetStage(&wq.sc);
            }
            eyeCapture[0].Reset();
            eyeCapture[1].Reset();
            hudCapture.Reset();
            pano = Panorama{};  // cube, shaders, views (the swapchains went with the session)
            blitVs.Reset();
            blitPs.Reset();
            blitPsDecode.Reset();
            blitPsAlpha.Reset();
            blitPsAlphaDecode.Reset();
            blitRs.Reset();
            device.Reset();
        }

        // Handles losses flagged by Check(). Returns true if a session is still usable.
        bool HandleLosses()
        {
            if (instanceLost) {
                const std::string why = error;
                DestroyAll();
                error = "OpenXR instance lost (" + why + "); call Init again";
                return false;
            }
            if (sessionLost && session != XR_NULL_HANDLE) {
                DestroySession();
                xrState = XR_SESSION_STATE_LOSS_PENDING;
                nextRecreateAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            return session != XR_NULL_HANDLE;
        }

        void TryRecreateSession()
        {
            if (!sessionLost || instance == XR_NULL_HANDLE || !device)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now < nextRecreateAttempt)
                return;
            nextRecreateAttempt = now + std::chrono::seconds(1);
            sessionLost = false;
            if (!CreateSession()) {
                DestroySession();
                if (!instanceLost)
                    sessionLost = true;  // keep retrying (e.g. headset unplugged: XR_ERROR_FORM_FACTOR_UNAVAILABLE)
                xrState = XR_SESSION_STATE_LOSS_PENDING;
            }
        }

        // ---------------------------------------------------------------- layers

        bool EnsureSwapchain(ColorSwapchain& a_sc, UINT a_width, UINT a_height)
        {
            if (a_sc.handle != XR_NULL_HANDLE && a_sc.width == a_width && a_sc.height == a_height)
                return true;
            DestroySwapchain(a_sc);

            std::uint32_t count = 0;
            if (!Check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "xrEnumerateSwapchainFormats"))
                return false;
            std::vector<std::int64_t> formats(count);
            if (!Check(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "xrEnumerateSwapchainFormats"))
                return false;
            if (formats.empty()) {
                error = "OpenXR runtime offers no swapchain formats";
                return false;
            }
            DXGI_FORMAT chosen = static_cast<DXGI_FORMAT>(formats.front());
            for (const DXGI_FORMAT preferred : { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB }) {
                if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(preferred)) != formats.end()) {
                    chosen = preferred;
                    break;
                }
            }

            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
            ci.format = chosen;
            ci.sampleCount = 1;
            ci.width = a_width;
            ci.height = a_height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (!Check(xrCreateSwapchain(session, &ci, &a_sc.handle), "xrCreateSwapchain"))
                return false;
            a_sc.format = chosen;
            a_sc.width = a_width;
            a_sc.height = a_height;

            if (!Check(xrEnumerateSwapchainImages(a_sc.handle, 0, &count, nullptr), "xrEnumerateSwapchainImages"))
                return DestroySwapchain(a_sc), false;
            std::vector<XrSwapchainImageD3D11KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            if (!Check(xrEnumerateSwapchainImages(a_sc.handle, count, &count,
                                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                       "xrEnumerateSwapchainImages"))
                return DestroySwapchain(a_sc), false;

            const DXGI_FORMAT unormFormat = NonSrgb(chosen);
            for (const auto& image : images) {
                D3D11_TEXTURE2D_DESC td{};
                image.texture->GetDesc(&td);
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                if (td.ArraySize > 1) {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray.ArraySize = 1;
                } else {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                }

                ComPtr<ID3D11RenderTargetView> native, unorm;
                rd.Format = chosen;
                if (!CheckHr(device->CreateRenderTargetView(image.texture, &rd, &native), "CreateRenderTargetView(swapchain)"))
                    return DestroySwapchain(a_sc), false;
                // A non-sRGB alias of an sRGB image needs a typeless texture; without one we decode in the shader instead.
                if (unormFormat != chosen && IsTypeless(td.Format)) {
                    rd.Format = unormFormat;
                    device->CreateRenderTargetView(image.texture, &rd, &unorm);
                }
                a_sc.rtvNative.push_back(native);
                a_sc.rtvUnorm.push_back(unorm);
                a_sc.images.push_back(image.texture);
            }
            return true;
        }

        bool EnsureStage(ColorSwapchain& a_sc, const D3D11_TEXTURE2D_DESC& a_src, const SourceFormat& a_fmt)
        {
            auto& stage = a_sc.stage;
            auto& stageSrv = a_sc.stageSrv;
            auto& stageDesc = a_sc.stageDesc;
            if (stage && stageDesc.Width == a_src.Width && stageDesc.Height == a_src.Height && stageDesc.Format == a_fmt.texture)
                return true;
            stage.Reset();
            stageSrv.Reset();
            stageDesc = {};

            D3D11_TEXTURE2D_DESC td{};
            td.Width = a_src.Width;
            td.Height = a_src.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = a_fmt.texture;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (!CheckHr(device->CreateTexture2D(&td, nullptr, &stage), "CreateTexture2D(intermediate)"))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = a_fmt.view;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (!CheckHr(device->CreateShaderResourceView(stage.Get(), &sd, &stageSrv), "CreateShaderResourceView(intermediate)")) {
                stage.Reset();
                return false;
            }
            stageDesc = td;
            return true;
        }

        void Blit(ID3D11DeviceContext* a_ctx, const ColorSwapchain& a_sc, ID3D11RenderTargetView* a_rtv, bool a_decode)
        {
            SavedContextState saved(a_ctx);

            a_ctx->IASetInputLayout(nullptr);
            a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            a_ctx->VSSetShader(blitVs.Get(), nullptr, 0);
            a_ctx->HSSetShader(nullptr, nullptr, 0);
            a_ctx->DSSetShader(nullptr, nullptr, 0);
            a_ctx->GSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer* noSo[D3D11_SO_BUFFER_SLOT_COUNT]{};
            const UINT    noSoOffsets[D3D11_SO_BUFFER_SLOT_COUNT]{};
            a_ctx->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, noSo, noSoOffsets);
            ID3D11PixelShader* ps = a_sc.keepAlpha ? (a_decode ? blitPsAlphaDecode.Get() : blitPsAlpha.Get())
                                                   : (a_decode ? blitPsDecode.Get() : blitPs.Get());
            a_ctx->PSSetShader(ps, nullptr, 0);
            a_ctx->RSSetState(blitRs.Get());
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_sc.width), static_cast<float>(a_sc.height), 0.0f, 1.0f };
            a_ctx->RSSetViewports(1, &vp);
            a_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            a_ctx->OMSetDepthStencilState(nullptr, 0);
            a_ctx->OMSetRenderTargets(1, &a_rtv, nullptr);
            ID3D11ShaderResourceView* srv = a_sc.stageSrv.Get();
            a_ctx->PSSetShaderResources(0, 1, &srv);
            a_ctx->Draw(3, 0);
        }

        // Copy a_src into a_sc's intermediate texture (recreated to a_src's size/format family).
        bool CopyToStage(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, ColorSwapchain& a_sc)
        {
            D3D11_TEXTURE2D_DESC sd{};
            a_src->GetDesc(&sd);
            const SourceFormat fmt = ClassifySource(sd.Format);
            if (!EnsureStage(a_sc, sd, fmt))
                return false;
            if (sd.SampleDesc.Count > 1)
                a_ctx->ResolveSubresource(a_sc.stage.Get(), 0, a_src, 0, fmt.view);
            else
                a_ctx->CopySubresourceRegion(a_sc.stage.Get(), 0, 0, 0, 0, a_src, 0, nullptr);
            a_sc.stageLinear = fmt.linear;
            a_sc.hasContent = true;
            return true;
        }

        // Acquire the next image of a_sc (recreated to the stage size), blit the stage into it, release.
        // a_capture (test hook): also copy the finished image to a CPU-readable texture before release.
        bool BlitStageToSwapchain(ID3D11DeviceContext* a_ctx, ColorSwapchain& a_sc, ComPtr<ID3D11Texture2D>* a_capture = nullptr)
        {
            if (!a_sc.hasContent || !EnsureSwapchain(a_sc, a_sc.stageDesc.Width, a_sc.stageDesc.Height))
                return false;

            std::uint32_t index = 0;
            if (!Check(xrAcquireSwapchainImage(a_sc.handle, nullptr, &index), "xrAcquireSwapchainImage"))
                return false;
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            const XrResult wr = xrWaitSwapchainImage(a_sc.handle, &wi);
            if (wr != XR_SUCCESS) {  // nothing waited, so nothing to release
                if (XR_FAILED(wr))
                    Check(wr, "xrWaitSwapchainImage");
                else
                    error = "xrWaitSwapchainImage: timed out";
                return false;
            }

            // Source values are sRGB-encoded: write them unchanged through a UNORM alias when we have one,
            // otherwise decode so the sRGB RTV (or a linear swapchain) ends up with the same picture.
            ID3D11RenderTargetView* rtv = a_sc.rtvNative[index].Get();
            bool decode = false;
            if (!a_sc.stageLinear) {
                if (a_sc.rtvUnorm[index])
                    rtv = a_sc.rtvUnorm[index].Get();
                else
                    decode = true;
            }
            Blit(a_ctx, a_sc, rtv, decode);
            if (a_capture)
                Capture(a_ctx, a_sc.images[index], *a_capture);

            return Check(xrReleaseSwapchainImage(a_sc.handle, nullptr), "xrReleaseSwapchainImage");
        }

        bool CopyToSwapchain(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, ColorSwapchain& a_sc)
        {
            return CopyToStage(a_ctx, a_src, a_sc) && BlitStageToSwapchain(a_ctx, a_sc);
        }

        void Capture(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_image, ComPtr<ID3D11Texture2D>& a_out)
        {
            D3D11_TEXTURE2D_DESC id{};
            a_image->GetDesc(&id);
            D3D11_TEXTURE2D_DESC cd{};
            if (a_out)
                a_out->GetDesc(&cd);
            if (!a_out || cd.Width != id.Width || cd.Height != id.Height || cd.Format != id.Format) {
                a_out.Reset();
                cd = id;
                cd.MipLevels = 1;
                cd.ArraySize = 1;
                cd.Usage = D3D11_USAGE_STAGING;
                cd.BindFlags = 0;
                cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                cd.MiscFlags = 0;
                if (!CheckHr(device->CreateTexture2D(&cd, nullptr, &a_out), "CreateTexture2D(capture)"))
                    return;
            }
            a_ctx->CopySubresourceRegion(a_out.Get(), 0, 0, 0, 0, a_image, 0, nullptr);
        }

        // ---------------------------------------------------------------- world panorama

        bool EnsurePano()
        {
            auto& p = pano;
            if (p.triedInit)
                return p.ok;
            p.triedInit = true;
            auto compile = [&](const D3D_SHADER_MACRO* a_defines, ComPtr<ID3D11PixelShader>& a_out) {
                ComPtr<ID3DBlob> blob, errors;
                if (FAILED(D3DCompile(kPanoHlsl, sizeof(kPanoHlsl) - 1, "xrcore_pano", a_defines, nullptr, "ps", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
                    error = "D3DCompile(pano) failed";
                    if (errors)
                        error += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
                    return false;
                }
                return CheckHr(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &a_out), "CreatePixelShader(pano)");
            };
            const D3D_SHADER_MACRO capture[] = { { "PANO_CAPTURE", "1" }, { "DECODE_SRGB", "0" }, { nullptr, nullptr } };
            const D3D_SHADER_MACRO show[] = { { "PANO_CAPTURE", "0" }, { "DECODE_SRGB", "0" }, { nullptr, nullptr } };
            const D3D_SHADER_MACRO showDecode[] = { { "PANO_CAPTURE", "0" }, { "DECODE_SRGB", "1" }, { nullptr, nullptr } };
            if (!compile(capture, p.psCapture) || !compile(show, p.psShow) || !compile(showDecode, p.psShowDecode))
                return false;

            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = Panorama::kFace;
            td.MipLevels = 0;  // full chain
            td.ArraySize = 6;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (!CheckHr(device->CreateTexture2D(&td, nullptr, &p.cube), "CreateTexture2D(panorama cube)"))
                return false;
            p.cube->GetDesc(&td);
            p.mips = td.MipLevels;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = static_cast<UINT>(-1);
            if (!CheckHr(device->CreateShaderResourceView(p.cube.Get(), &sd, &p.srv), "CreateShaderResourceView(panorama)"))
                return false;
            for (UINT f = 0; f < 6; ++f) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = td.Format;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = 0;
                rd.Texture2DArray.FirstArraySlice = f;
                rd.Texture2DArray.ArraySize = 1;
                if (!CheckHr(device->CreateRenderTargetView(p.cube.Get(), &rd, &p.rtv[f]), "CreateRenderTargetView(panorama face)"))
                    return false;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(PanoCb);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (!CheckHr(device->CreateBuffer(&bd, nullptr, &p.cb), "CreateBuffer(panorama)"))
                return false;
            D3D11_SAMPLER_DESC smd{};
            smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            smd.MaxLOD = D3D11_FLOAT32_MAX;
            if (!CheckHr(device->CreateSamplerState(&smd, &p.smp), "CreateSamplerState(panorama)"))
                return false;
            ComPtr<ID3D11DeviceContext> ctx;
            device->GetImmediateContext(&ctx);
            ClearPano(ctx.Get());
            p.ok = true;
            return true;
        }

        void ClearPano(ID3D11DeviceContext* a_ctx)
        {
            const float zero[4]{ 0, 0, 0, 0 };
            for (auto& r : pano.rtv)
                if (r)
                    a_ctx->ClearRenderTargetView(r.Get(), zero);
            pano.mipsDirty = true;
            pano.everCaptured = false;
        }

        // Draw the fullscreen triangle with a pano pixel shader into a_rtv (a_w x a_h); saves/restores what it touches.
        void PanoDraw(ID3D11DeviceContext* a_ctx, ID3D11PixelShader* a_ps, ID3D11ShaderResourceView* a_srv, ID3D11RenderTargetView* a_rtv,
            UINT a_w, UINT a_h, const PanoCb& a_cb)
        {
            SavedContextState saved(a_ctx);
            ID3D11SamplerState* oldSmp = nullptr;
            ID3D11Buffer*       oldCb = nullptr;
            a_ctx->PSGetSamplers(0, 1, &oldSmp);
            a_ctx->PSGetConstantBuffers(0, 1, &oldCb);

            a_ctx->UpdateSubresource(pano.cb.Get(), 0, nullptr, &a_cb, 0, 0);
            a_ctx->IASetInputLayout(nullptr);
            a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            a_ctx->VSSetShader(blitVs.Get(), nullptr, 0);
            a_ctx->HSSetShader(nullptr, nullptr, 0);
            a_ctx->DSSetShader(nullptr, nullptr, 0);
            a_ctx->GSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer* noSo[D3D11_SO_BUFFER_SLOT_COUNT]{};
            const UINT    noSoOffsets[D3D11_SO_BUFFER_SLOT_COUNT]{};
            a_ctx->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, noSo, noSoOffsets);
            a_ctx->PSSetShader(a_ps, nullptr, 0);
            ID3D11Buffer* cb = pano.cb.Get();
            a_ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11SamplerState* smp = pano.smp.Get();
            a_ctx->PSSetSamplers(0, 1, &smp);
            a_ctx->RSSetState(blitRs.Get());
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_w), static_cast<float>(a_h), 0.0f, 1.0f };
            a_ctx->RSSetViewports(1, &vp);
            a_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            a_ctx->OMSetDepthStencilState(nullptr, 0);
            a_ctx->OMSetRenderTargets(1, &a_rtv, nullptr);
            a_ctx->PSSetShaderResources(0, 1, &a_srv);
            a_ctx->Draw(3, 0);

            a_ctx->PSSetSamplers(0, 1, &oldSmp);
            a_ctx->PSSetConstantBuffers(0, 1, &oldCb);
            if (oldSmp)
                oldSmp->Release();
            if (oldCb)
                oldCb->Release();
        }

        static XrQuaternionf Conj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

        // One projection image (stage SRV, rendered with a_pose/a_fov in the reference space) into the cube.
        void PanoCapture(ID3D11DeviceContext* a_ctx, ColorSwapchain& a_src, const XrPosef& a_pose, const XrFovf& a_fov,
            const XrQuaternionf& a_refToWorld)
        {
            if (pano.everCaptured && pano.linear != a_src.stageLinear)
                ClearPano(a_ctx);  // the source kind changed: don't mix encodings
            pano.linear = a_src.stageLinear;
            const XrQuaternionf viewToWorld = Mul(a_refToWorld, a_pose.orientation);
            const XrQuaternionf worldToView = Conj(viewToWorld);
            PanoCb cb{};
            cb.qa[0] = worldToView.x, cb.qa[1] = worldToView.y, cb.qa[2] = worldToView.z, cb.qa[3] = worldToView.w;
            cb.tn[0] = std::tan(a_fov.angleLeft), cb.tn[1] = std::tan(a_fov.angleRight);
            cb.tn[2] = std::tan(a_fov.angleUp), cb.tn[3] = std::tan(a_fov.angleDown);
            cb.sz[0] = cb.sz[1] = static_cast<float>(Panorama::kFace);
            // faces facing away from the view cannot hold any of it (face half-diagonal 54.7 deg + view half-angle <= ~80 deg)
            const XrVector3f fwd = Rotate(viewToWorld, { 0.0f, 0.0f, -1.0f });
            const float      nrm[6][3]{ { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            for (std::uint32_t f = 0; f < 6; ++f) {
                if (fwd.x * nrm[f][0] + fwd.y * nrm[f][1] + fwd.z * nrm[f][2] < -0.75f)
                    continue;
                cb.face = f;
                PanoDraw(a_ctx, pano.psCapture.Get(), a_src.stageSrv.Get(), pano.rtv[f].Get(), Panorama::kFace, Panorama::kFace, cb);
            }
            pano.mipsDirty = true;
            pano.everCaptured = true;
            ++pano.captures;
        }

        // Symmetric FOV containing a_fovs, widened on the narrower axis to match a_aspect (w/h) when > 0.
        static Fov SymmetricFov(const XrFovf* a_fovs, int a_count, float a_aspect)
        {
            float tanH = 1.0f;  // 90 x 90 degrees when nothing is known yet
            float tanV = 1.0f;
            if (a_count > 0) {
                float h = 0.0f, v = 0.0f;
                for (int i = 0; i < a_count; ++i) {
                    h = std::max({ h, std::fabs(a_fovs[i].angleLeft), std::fabs(a_fovs[i].angleRight) });
                    v = std::max({ v, std::fabs(a_fovs[i].angleUp), std::fabs(a_fovs[i].angleDown) });
                }
                constexpr float kMaxHalfAngle = 1.5f;  // ~86 deg; keeps tan() finite on odd runtimes
                tanH = std::tan(std::min(h, kMaxHalfAngle));
                tanV = std::tan(std::min(v, kMaxHalfAngle));
            }
            if (a_aspect > 0.0f) {
                if (tanH / tanV < a_aspect)
                    tanH = tanV * a_aspect;  // too narrow: widen horizontally
                else
                    tanV = tanH / a_aspect;  // too wide: widen vertically
            }
            const float h = std::atan(tanH);
            const float v = std::atan(tanV);
            return { -h, h, v, -v };
        }

        XrPosef QuadPose() const
        {
            if (quadOverride)
                return quadOverridePose;
            const XrVector3f offset = Rotate(origin.orientation, { 0.0f, config.quad.heightMeters, -config.quad.distanceMeters });
            return { origin.orientation,
                     { origin.position.x + offset.x, origin.position.y + offset.y, origin.position.z + offset.z } };
        }

        void ApplyRecenter() { AnchorQuadAt(head); }

        void AnchorQuadAt(const XrPosef& a_head)
        {
            const XrVector3f fwd = Rotate(a_head.orientation, { 0.0f, 0.0f, -1.0f });
            float yaw = 0.0f;
            if (fwd.x * fwd.x + fwd.z * fwd.z > 1e-6f)  // looking straight up/down: keep yaw 0
                yaw = std::atan2(-fwd.x, -fwd.z);
            origin.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            origin.position = a_head.position;
            recenterPending = false;
        }
    };

    Runtime::Runtime() :
        _impl(new Impl)
    {}

    Runtime::~Runtime()
    {
        Shutdown();
        delete _impl;
    }

    bool Runtime::Init(ID3D11Device* a_device, const Config& a_config)
    {
        Shutdown();
        auto& d = *_impl;
        d.error.clear();
        d.config = a_config;
        if (!a_device) {
            d.error = "Init: null D3D11 device";
            return false;
        }
        d.device = a_device;

        bool ok = false;
        try {
            ok = d.CreateInstance();
            if (ok && !d.CreateInputActions())
                d.DestroyInputInstance();  // input disabled; lastError() says why, Init continues
            ok = ok && d.CreateSession() && d.CreateBlitResources();
        } catch (const std::exception& e) {  // bad_alloc from the enumeration vectors; keep it inside
            d.error = std::string("Init: exception: ") + e.what();
        }
        if (!ok) {
            const std::string why = d.error;
            d.DestroyAll();
            d.error = why;
        }
        return ok;
    }

    void Runtime::Shutdown()
    {
        auto& d = *_impl;
        if (d.frameBegun)
            EndFrame();
        d.DestroyAll();
        d.runtimeName.clear();
    }

    void Runtime::PollEvents()
    {
        auto& d = *_impl;
        if (d.instance == XR_NULL_HANDLE)
            return;
        d.TryRecreateSession();

        for (;;) {
            XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
            const XrResult r = xrPollEvent(d.instance, &ev);
            if (r == XR_EVENT_UNAVAILABLE)
                break;
            if (!d.Check(r, "xrPollEvent"))
                break;

            switch (ev.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                d.instanceLost = true;
                d.error = "XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING";
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                d.recenterPending = true;  // user recentred in the runtime: re-anchor the quad to match
                break;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                {
                    const auto& sc = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                    if (sc.session != d.session)
                        break;
                    d.xrState = sc.state;
                    switch (sc.state) {
                    case XR_SESSION_STATE_READY:
                        {
                            XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                            bi.primaryViewConfigurationType = kViewConfig;
                            if (d.Check(xrBeginSession(d.session, &bi), "xrBeginSession"))
                                d.sessionRunning = true;
                        }
                        break;
                    case XR_SESSION_STATE_STOPPING:
                        if (d.frameBegun)
                            EndFrame();
                        d.sessionRunning = false;
                        d.Check(xrEndSession(d.session), "xrEndSession");
                        break;
                    case XR_SESSION_STATE_LOSS_PENDING:
                        d.sessionLost = true;
                        break;
                    case XR_SESSION_STATE_EXITING:
                        d.DestroySession();
                        d.xrState = XR_SESSION_STATE_EXITING;
                        break;
                    default:
                        break;
                    }
                }
                break;
            default:
                break;
            }
            if (d.instanceLost || d.session == XR_NULL_HANDLE)
                break;
        }
        d.HandleLosses();
    }

    bool Runtime::BeginFrame(FrameInfo& a_out)
    {
        auto& d = *_impl;
        a_out = {};
        if (d.frameBegun)
            EndFrame();  // never leave an xrBeginFrame unmatched
        if (!d.HandleLosses() || !d.sessionRunning) {
            d.controllers = {};
            return false;
        }

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (!d.Check(xrWaitFrame(d.session, nullptr, &fs), "xrWaitFrame")) {
            d.HandleLosses();
            return false;
        }
        if (!d.Check(xrBeginFrame(d.session, nullptr), "xrBeginFrame")) {
            d.HandleLosses();
            return false;
        }
        d.frameBegun = true;
        d.quadSubmitted = false;
        d.monoSubmitted = false;
        d.eyeSubmitted = false;
        d.backdropWanted = false;
        d.hudSubmitted = false;
        for (auto& w : d.world)
            w.submitted = false;
        d.displayTime = fs.predictedDisplayTime;
        d.shouldRender = fs.shouldRender == XR_TRUE;

        a_out.shouldRender = d.shouldRender;
        a_out.predictedDisplayTime = fs.predictedDisplayTime;
        a_out.predictedDisplayPeriod = fs.predictedDisplayPeriod;

        XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = kViewConfig;
        li.displayTime = fs.predictedDisplayTime;
        li.space = d.refSpace;
        XrViewState vs{ XR_TYPE_VIEW_STATE };
        XrView views[2]{ { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        std::uint32_t viewCount = 0;
        if (d.Check(xrLocateViews(d.session, &li, &vs, 2, &viewCount, views), "xrLocateViews") && viewCount == 2) {
            constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
            a_out.viewsValid = (vs.viewStateFlags & kValid) == kValid;
            for (int i = 0; i < 2; ++i) {
                a_out.eyes[i].pose = ToPose(views[i].pose);
                a_out.eyes[i].fov = { views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown };
            }
            d.eyeFov[0] = views[0].fov;
            d.eyeFov[1] = views[1].fov;
            d.eyeFovValid = true;
            d.viewPose[0] = views[0].pose;
            d.viewPose[1] = views[1].pose;
            d.viewsValid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        } else {
            d.viewsValid = false;
        }
        d.pano.submitted = false;

        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (d.Check(xrLocateSpace(d.viewSpace, d.refSpace, fs.predictedDisplayTime, &loc), "xrLocateSpace(VIEW)")) {
            constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            d.headValid = (loc.locationFlags & kValid) == kValid;
            a_out.headValid = d.headValid;
            if (d.headValid) {
                d.head = loc.pose;
                a_out.head = ToPose(loc.pose);
                if (d.recenterPending)
                    d.ApplyRecenter();
            }
        }
        d.SyncInput(fs.predictedDisplayTime);
        return true;
    }

    void Runtime::SubmitQuad(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, bool a_alphaBlend, bool a_premultiplied)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !a_src)
            return;
        d.quadSc.keepAlpha = a_alphaBlend;
        if (d.CopyToSwapchain(a_ctx, a_src, d.quadSc)) {
            d.quadSubmitted = true;
            d.quadBlend = a_alphaBlend;
            d.quadPremultiplied = a_premultiplied;
            d.quadAspect = static_cast<float>(d.quadSc.height) / static_cast<float>(d.quadSc.width);
        }
    }

    void Runtime::SetCinemaQuadGeometry(float a_widthMeters, float a_distanceMeters, float a_heightMeters)
    {
        auto& d = *_impl;
        d.config.quad.widthMeters = a_widthMeters;
        d.config.quad.distanceMeters = a_distanceMeters;
        d.config.quad.heightMeters = a_heightMeters;
    }

    void Runtime::SetCinemaQuadOverride(const Pose* a_poseInRef, float a_widthMeters)
    {
        auto& d = *_impl;
        d.quadOverride = a_poseInRef != nullptr;
        if (a_poseInRef) {
            const auto& o = a_poseInRef->orientation;
            const auto& p = a_poseInRef->position;
            d.quadOverridePose = { { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } };
            d.quadOverrideWidth = a_widthMeters;
        }
    }

    void Runtime::AnchorCinemaQuad(const Pose& a_head)
    {
        auto& d = *_impl;
        const auto& o = a_head.orientation;
        const auto& p = a_head.position;
        d.AnchorQuadAt({ { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } });
    }

    void Runtime::SubmitBackdrop()
    {
        auto& d = *_impl;
        if (d.frameBegun)
            d.backdropWanted = true;
    }

    std::uint64_t Runtime::BackdropFrames() const noexcept { return _impl->backdropFrames; }

    bool Runtime::CapturePanorama(ID3D11DeviceContext* a_ctx, const Quat& a_refToWorld)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !a_ctx || !d.EnsurePano())
            return false;
        const XrQuaternionf r{ a_refToWorld.x, a_refToWorld.y, a_refToWorld.z, a_refToWorld.w };
        if (d.monoSubmitted && d.monoSc.hasContent && d.monoSc.stageSrv) {
            d.PanoCapture(a_ctx, d.monoSc, d.monoPose, d.monoFov, r);
            return true;
        }
        if (d.eyeSubmitted) {
            // one eye per call, alternating (half the cost; the eyes overlap almost entirely)
            for (int k = 0; k < 2; ++k) {
                const int e = (d.pano.nextEye + k) & 1;
                if (d.eyeSc[e].hasContent && d.eyeSc[e].stageSrv) {
                    d.PanoCapture(a_ctx, d.eyeSc[e], d.eyeRenderPose[e], d.eyeRenderFov[e], r);
                    d.pano.nextEye = e ^ 1;
                    return true;
                }
            }
        }
        return false;
    }

    bool Runtime::SubmitPanorama(ID3D11DeviceContext* a_ctx, const Quat& a_refToWorld, float a_brightness, float a_holeBrightness)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !d.viewsValid || d.monoSubmitted || d.eyeSubmitted || !d.EnsurePano())
            return false;
        auto& p = d.pano;
        if (p.mipsDirty) {
            a_ctx->GenerateMips(p.srv.Get());
            p.mipsDirty = false;
        }
        const XrQuaternionf r{ a_refToWorld.x, a_refToWorld.y, a_refToWorld.z, a_refToWorld.w };
        for (int e = 0; e < 2; ++e) {
            const XrFovf& f = d.eyeFov[e];
            const float   tl = std::tan(f.angleLeft), tr = std::tan(f.angleRight), tu = std::tan(f.angleUp), tb = std::tan(f.angleDown);
            const UINT    w = 1280;
            const UINT    h = static_cast<UINT>(std::clamp(1280.0f * (tu - tb) / (std::max)(tr - tl, 0.1f), 256.0f, 2048.0f));
            auto&         sc = p.sc[e];
            if (!d.EnsureSwapchain(sc, w, h))
                return false;
            std::uint32_t index = 0;
            if (!d.Check(xrAcquireSwapchainImage(sc.handle, nullptr, &index), "xrAcquireSwapchainImage(panorama)"))
                return false;
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            const XrResult wr = xrWaitSwapchainImage(sc.handle, &wi);
            if (wr != XR_SUCCESS) {
                if (XR_FAILED(wr))
                    d.Check(wr, "xrWaitSwapchainImage(panorama)");
                return false;
            }
            // stored values: sRGB-encoded (8-bit sources) -> write through the UNORM alias, or decode for an sRGB view;
            // linear (float sources) -> the native view
            ID3D11RenderTargetView* rtv = sc.rtvNative[index].Get();
            bool                    decode = false;
            if (!p.linear) {
                if (sc.rtvUnorm[index])
                    rtv = sc.rtvUnorm[index].Get();
                else
                    decode = true;
            }
            const XrQuaternionf viewToWorld = Mul(r, d.viewPose[e].orientation);
            PanoCb cb{};
            cb.qa[0] = viewToWorld.x, cb.qa[1] = viewToWorld.y, cb.qa[2] = viewToWorld.z, cb.qa[3] = viewToWorld.w;
            cb.tn[0] = tl, cb.tn[1] = tr, cb.tn[2] = tu, cb.tn[3] = tb;
            cb.sz[0] = static_cast<float>(w), cb.sz[1] = static_cast<float>(h);
            cb.bright = std::clamp(a_brightness, 0.0f, 1.0f);
            cb.holeBright = std::clamp(a_holeBrightness, 0.0f, 1.0f);
            cb.maxLod = static_cast<float>(p.mips ? p.mips - 1 : 0);
            d.PanoDraw(a_ctx, decode ? p.psShowDecode.Get() : p.psShow.Get(), p.srv.Get(), rtv, w, h, cb);
            if (!d.Check(xrReleaseSwapchainImage(sc.handle, nullptr), "xrReleaseSwapchainImage(panorama)"))
                return false;
            p.pose[e] = d.viewPose[e];
            p.fov[e] = f;
        }
        p.submitted = true;
        ++p.shows;
        return true;
    }

    void Runtime::ResetPanorama()
    {
        auto& d = *_impl;
        if (!d.pano.ok || !d.pano.everCaptured)
            return;
        ComPtr<ID3D11DeviceContext> ctx;
        d.device->GetImmediateContext(&ctx);
        d.ClearPano(ctx.Get());
        ++d.pano.resets;
    }

    std::string Runtime::PanoramaReport() const
    {
        const auto& p = _impl->pano;
        char        buf[256];
        std::snprintf(buf, sizeof(buf), "panoOk=%d panoCaptured=%d panoCaptures=%llu panoShows=%llu panoResets=%llu panoLinear=%d panoMips=%u", p.ok ? 1 : 0,
            p.everCaptured ? 1 : 0, static_cast<unsigned long long>(p.captures), static_cast<unsigned long long>(p.shows),
            static_cast<unsigned long long>(p.resets), p.linear ? 1 : 0, p.mips);
        return buf;
    }

    bool Runtime::SubmitMono(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_renderPose, const Fov& a_renderFov)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !a_src)
            return false;
        if (!d.CopyToSwapchain(a_ctx, a_src, d.monoSc))
            return false;
        const auto& o = a_renderPose.orientation;
        const auto& p = a_renderPose.position;
        d.monoPose = { { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } };
        d.monoFov = { a_renderFov.angleLeft, a_renderFov.angleRight, a_renderFov.angleUp, a_renderFov.angleDown };
        d.monoSubmitted = true;
        return true;
    }

    bool Runtime::SubmitHud(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_poseInView, float a_widthMeters,
        bool a_premultiplied)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !a_src || !(a_widthMeters > 0.0f) || d.viewSpace == XR_NULL_HANDLE)
            return false;
        d.hudSc.keepAlpha = true;
        if (!d.CopyToStage(a_ctx, a_src, d.hudSc) ||
            !d.BlitStageToSwapchain(a_ctx, d.hudSc, d.eyeCaptureEnabled ? std::addressof(d.hudCapture) : nullptr))
            return false;
        const auto& o = a_poseInView.orientation;
        const auto& p = a_poseInView.position;
        d.hudPose = { { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } };
        d.hudSize = { a_widthMeters, a_widthMeters * static_cast<float>(d.hudSc.height) / static_cast<float>(d.hudSc.width) };
        d.hudPremultiplied = a_premultiplied;
        d.hudSubmitted = true;
        return true;
    }

    bool Runtime::SubmitWorldQuad(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, const Pose& a_poseInRef, float a_widthMeters,
        bool a_alphaBlend, bool a_premultiplied, int a_slot)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !a_src || !(a_widthMeters > 0.0f) || d.refSpace == XR_NULL_HANDLE ||
            a_slot < 0 || a_slot >= kMaxWorldQuads)
            return false;
        auto& w = d.world[a_slot];
        w.sc.keepAlpha = a_alphaBlend;
        if (!d.CopyToStage(a_ctx, a_src, w.sc) || !d.BlitStageToSwapchain(a_ctx, w.sc))
            return false;
        const auto& o = a_poseInRef.orientation;
        const auto& p = a_poseInRef.position;
        w.pose = { { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } };
        w.size = { a_widthMeters, a_widthMeters * static_cast<float>(w.sc.height) / static_cast<float>(w.sc.width) };
        w.blend = a_alphaBlend;
        w.premultiplied = a_premultiplied;
        w.submitted = true;
        return true;
    }

    std::uint64_t Runtime::WorldQuadsSubmitted(int a_slot) const noexcept
    {
        return a_slot >= 0 && a_slot < kMaxWorldQuads ? _impl->worldFrames[a_slot] : 0;
    }

    bool Runtime::SubmitFade(ID3D11DeviceContext* a_ctx, float a_alpha)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !(a_alpha > 0.0f) || d.viewSpace == XR_NULL_HANDLE)
            return false;
        const float a = std::round((a_alpha < 1.0f ? a_alpha : 1.0f) * 255.0f) / 255.0f;
        if (!d.EnsureSwapchain(d.fadeSc, 8, 8))
            return false;
        if (a != d.fadeClearedAlpha) {
            std::uint32_t index = 0;
            if (!d.Check(xrAcquireSwapchainImage(d.fadeSc.handle, nullptr, &index), "xrAcquireSwapchainImage(fade)"))
                return false;
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            const XrResult wr = xrWaitSwapchainImage(d.fadeSc.handle, &wi);
            if (wr != XR_SUCCESS) {
                if (XR_FAILED(wr))
                    d.Check(wr, "xrWaitSwapchainImage(fade)");
                return false;
            }
            // premultiplied black: colour 0 (identical in sRGB and linear), alpha = opacity
            const float c[4]{ 0.0f, 0.0f, 0.0f, a };
            a_ctx->ClearRenderTargetView(d.fadeSc.rtvNative[index].Get(), c);
            if (!d.Check(xrReleaseSwapchainImage(d.fadeSc.handle, nullptr), "xrReleaseSwapchainImage(fade)"))
                return false;
            d.fadeClearedAlpha = a;
        }
        d.fadeSubmitted = true;
        return true;
    }

    std::uint64_t Runtime::FadeLayersSubmitted() const noexcept { return _impl->fadeFrames; }

    bool Runtime::SubmitVignette(ID3D11DeviceContext* a_ctx, float a_alpha, float a_innerDeg, float a_outerDeg)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !d.shouldRender || !a_ctx || !(a_alpha > 0.0f) || d.viewSpace == XR_NULL_HANDLE)
            return false;
        constexpr UINT  kSize = 256;
        constexpr float kDist = 0.25f, kHalf = 2.0f;  // quad: 0.25 m ahead, 4 x 4 m (as the fade)
        const float a = std::round((a_alpha < 1.0f ? a_alpha : 1.0f) * 64.0f) / 64.0f;
        const float inner = (std::max)(a_innerDeg, 0.0f);
        const float outer = (std::max)(a_outerDeg, inner + 0.5f);
        if (a <= 0.0f)
            return false;
        if (a != d.vigKey[0] || inner != d.vigKey[1] || outer != d.vigKey[2] || !d.vigSc.hasContent || d.vigSc.handle == XR_NULL_HANDLE) {
            if (!d.vigSrc) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = td.Height = kSize;
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (!d.CheckHr(d.device->CreateTexture2D(&td, nullptr, &d.vigSrc), "CreateTexture2D(vignette)"))
                    return false;
            }
            // premultiplied black: rgb 0, alpha = a * smoothstep(inner, outer, angle from the view axis)
            std::vector<std::uint32_t> px(static_cast<std::size_t>(kSize) * kSize);
            const float r2d = 57.2957795f;
            for (UINT y = 0; y < kSize; ++y) {
                const float my = ((static_cast<float>(y) + 0.5f) / kSize * 2.0f - 1.0f) * kHalf;
                for (UINT x = 0; x < kSize; ++x) {
                    const float mx = ((static_cast<float>(x) + 0.5f) / kSize * 2.0f - 1.0f) * kHalf;
                    const float th = std::atan(std::sqrt(mx * mx + my * my) / kDist) * r2d;
                    float t = (th - inner) / (outer - inner);
                    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                    const float v = a * t * t * (3.0f - 2.0f * t);
                    const auto  b = static_cast<std::uint32_t>(std::lround(v * 255.0f));
                    px[static_cast<std::size_t>(y) * kSize + x] = b << 24;  // R8G8B8A8: A in the top byte
                }
            }
            a_ctx->UpdateSubresource(d.vigSrc.Get(), 0, nullptr, px.data(), kSize * 4, 0);
            d.vigSc.keepAlpha = true;
            if (!d.CopyToSwapchain(a_ctx, d.vigSrc.Get(), d.vigSc))
                return false;
            d.vigKey[0] = a;
            d.vigKey[1] = inner;
            d.vigKey[2] = outer;
            ++d.vigUploads;
        }
        d.vigSubmitted = true;
        return true;
    }

    std::uint64_t Runtime::VignetteLayersSubmitted() const noexcept { return _impl->vigFrames; }
    std::uint64_t Runtime::VignetteUploads() const noexcept { return _impl->vigUploads; }
    std::uint32_t Runtime::LastLayerCount() const noexcept { return _impl->lastLayerCount; }

    ID3D11Texture2D* Runtime::CapturedHudImage() const
    {
        if (!_impl->hudCapture)
            return nullptr;
        ID3D11Texture2D* t = _impl->hudCapture.Get();
        t->AddRef();
        return t;
    }

    Fov Runtime::MonoRenderFov(float a_aspect) const
    {
        const auto& d = *_impl;
        return Impl::SymmetricFov(d.eyeFov, d.eyeFovValid ? 2 : 0, a_aspect);
    }

    Fov Runtime::EyeRenderFov(int a_eye, float a_aspect) const
    {
        const auto& d = *_impl;
        if (a_eye < 0 || a_eye > 1)
            return {};
        return Impl::SymmetricFov(&d.eyeFov[a_eye], d.eyeFovValid ? 1 : 0, a_aspect);
    }

    bool Runtime::SubmitEye(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src, int a_eye, const Pose& a_renderPose, const Fov& a_renderFov)
    {
        auto& d = *_impl;
        if (!d.frameBegun || !a_ctx || !a_src || a_eye < 0 || a_eye > 1)
            return false;
        if (!d.CopyToStage(a_ctx, a_src, d.eyeSc[a_eye]))
            return false;
        d.eyeSubmitted = true;
        const auto& o = a_renderPose.orientation;
        const auto& p = a_renderPose.position;
        d.eyeRenderPose[a_eye] = { { o.x, o.y, o.z, o.w }, { p.x, p.y, p.z } };
        d.eyeRenderFov[a_eye] = { a_renderFov.angleLeft, a_renderFov.angleRight, a_renderFov.angleUp, a_renderFov.angleDown };
        return true;
    }

    void Runtime::SetEyeCapture(bool a_enable) noexcept { _impl->eyeCaptureEnabled = a_enable; }

    ID3D11Texture2D* Runtime::CapturedEyeImage(int a_eye) const
    {
        if (a_eye < 0 || a_eye > 1 || !_impl->eyeCapture[a_eye])
            return nullptr;
        ID3D11Texture2D* t = _impl->eyeCapture[a_eye].Get();
        t->AddRef();
        return t;
    }

    void Runtime::EndFrame()
    {
        auto& d = *_impl;
        if (!d.frameBegun)
            return;
        d.frameBegun = false;

        // Projection (world view) first, then the HUD quad, then the cinema quad on top.
        const XrCompositionLayerBaseHeader* layers[5 + kMaxWorldQuads]{};
        std::uint32_t layerCount = 0;

        XrCompositionLayerProjectionView views[2]{ { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        bool haveProj = false;
        // Backdrop (SubmitBackdrop): no new projection image this frame -> the last one again, of the kind it was.
        // World panorama (SubmitPanorama) first: it replaces the frozen backdrop.
        const bool panoNow = d.pano.submitted && !d.monoSubmitted && !d.eyeSubmitted && d.shouldRender;
        const bool backdrop = d.backdropWanted && !d.monoSubmitted && !d.eyeSubmitted && !panoNow;
        const bool monoBackdrop = backdrop && d.lastProjMono && d.monoReleased && d.monoSc.handle != XR_NULL_HANDLE;
        if (panoNow) {
            for (int i = 0; i < 2; ++i) {
                auto& v = views[i];
                v.pose = d.pano.pose[i];
                v.fov = d.pano.fov[i];
                v.subImage.swapchain = d.pano.sc[i].handle;
                v.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.pano.sc[i].width), static_cast<std::int32_t>(d.pano.sc[i].height) } };
            }
            haveProj = true;
        } else if ((d.monoSubmitted || monoBackdrop) && d.shouldRender) {
            // mono: both eyes show the same image, rendered from one pose. Backdrop: no acquire/release this frame, so
            // the swapchain's last released image (the last world frame) is what the compositor shows.
            for (auto& v : views) {
                v.pose = d.monoPose;
                v.fov = d.monoFov;
                v.subImage.swapchain = d.monoSc.handle;
                v.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.monoSc.width), static_cast<std::int32_t>(d.monoSc.height) } };
            }
            haveProj = true;
            if (d.monoSubmitted) {
                d.lastProjMono = true;
                d.monoReleased = true;
            } else {
                ++d.backdropFrames;
            }
        } else if (d.shouldRender && (d.eyeSubmitted || (backdrop && !d.lastProjMono)) && d.eyeSc[0].hasContent && d.eyeSc[1].hasContent) {
            if (d.eyeSubmitted)
                d.lastProjMono = false;
            else
                ++d.backdropFrames;
            // AFR: every frame, each eye's latest stage (new or stale) is blitted into a freshly acquired
            // image of its swapchain, so every referenced swapchain has an image released in this frame.
            ComPtr<ID3D11DeviceContext> ctx;
            d.device->GetImmediateContext(&ctx);
            haveProj = true;
            for (int i = 0; i < 2; ++i) {
                if (!d.BlitStageToSwapchain(ctx.Get(), d.eyeSc[i], d.eyeCaptureEnabled ? std::addressof(d.eyeCapture[i]) : nullptr)) {
                    haveProj = false;
                    break;
                }
                auto& v = views[i];
                v.pose = d.eyeRenderPose[i];
                v.fov = d.eyeRenderFov[i];
                v.subImage.swapchain = d.eyeSc[i].handle;
                v.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.eyeSc[i].width), static_cast<std::int32_t>(d.eyeSc[i].height) } };
            }
        }
        if (haveProj) {
            proj.layerFlags = 0;
            proj.space = d.refSpace;
            proj.viewCount = 2;
            proj.views = views;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        }

        // Comfort vignette: above the world, below the HUD (premultiplied black, head-locked).
        XrCompositionLayerQuad vig{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        if (d.vigSubmitted && d.shouldRender && d.vigSc.handle != XR_NULL_HANDLE) {
            vig.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            vig.space = d.viewSpace;
            vig.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            vig.subImage.swapchain = d.vigSc.handle;
            vig.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.vigSc.width), static_cast<std::int32_t>(d.vigSc.height) } };
            vig.subImage.imageArrayIndex = 0;
            vig.pose = { { 0, 0, 0, 1 }, { 0, 0, -0.25f } };
            vig.size = { 4.0f, 4.0f };
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vig);
            ++d.vigFrames;
        }

        XrCompositionLayerQuad hud{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        if (d.hudSubmitted && d.shouldRender && d.hudSc.handle != XR_NULL_HANDLE) {
            // Source alpha blends the HUD over whatever is below. Scaleform drawn into a target cleared to
            // (0,0,0,0) yields premultiplied colour; the caller says which it has.
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                             (d.hudPremultiplied ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
            hud.space = d.viewSpace;  // head-locked
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain = d.hudSc.handle;
            hud.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.hudSc.width), static_cast<std::int32_t>(d.hudSc.height) } };
            hud.subImage.imageArrayIndex = 0;
            hud.pose = d.hudPose;
            hud.size = d.hudSize;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
        }

        // World quads in slot order (0 = Pip-Boy, 1 = dialogue panel): above the HUD, below the cinema quad.
        XrCompositionLayerQuad world[kMaxWorldQuads]{};
        for (int i = 0; i < kMaxWorldQuads; ++i) {
            const auto& w = d.world[i];
            if (!w.submitted || !d.shouldRender || w.sc.handle == XR_NULL_HANDLE)
                continue;
            auto& l = world[i];
            l.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            l.layerFlags = w.blend ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                         (w.premultiplied ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT))
                                   : 0;
            l.space = d.refSpace;
            l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            l.subImage.swapchain = w.sc.handle;
            l.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(w.sc.width), static_cast<std::int32_t>(w.sc.height) } };
            l.subImage.imageArrayIndex = 0;
            l.pose = w.pose;
            l.size = w.size;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l);
            ++d.worldFrames[i];
        }

        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        if (d.quadSubmitted && d.shouldRender) {
            // opaque for the flat frame (back buffer alpha is meaningless); source alpha for a menu panel
            quad.layerFlags = d.quadBlend ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                                (d.quadPremultiplied ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT))
                                          : 0;
            quad.space = d.refSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = d.quadSc.handle;
            quad.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.quadSc.width), static_cast<std::int32_t>(d.quadSc.height) } };
            quad.subImage.imageArrayIndex = 0;
            quad.pose = d.QuadPose();
            const float qw = d.quadOverride ? d.quadOverrideWidth : d.config.quad.widthMeters;
            quad.size = { qw, qw * d.quadAspect };
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        }

        XrCompositionLayerQuad fade{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        if (d.fadeSubmitted && d.shouldRender && d.fadeSc.handle != XR_NULL_HANDLE) {
            // 0.25 m ahead, 4 x 4 m: covers +-83 deg in both axes, beyond any headset's FOV
            fade.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            fade.space = d.viewSpace;
            fade.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            fade.subImage.swapchain = d.fadeSc.handle;
            fade.subImage.imageRect = { { 0, 0 }, { static_cast<std::int32_t>(d.fadeSc.width), static_cast<std::int32_t>(d.fadeSc.height) } };
            fade.subImage.imageArrayIndex = 0;
            fade.pose = { { 0, 0, 0, 1 }, { 0, 0, -0.25f } };
            fade.size = { 4.0f, 4.0f };
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fade);
            ++d.fadeFrames;
        }
        d.fadeSubmitted = false;
        d.vigSubmitted = false;
        d.lastLayerCount = layerCount;
        d.quadSubmitted = false;
        d.monoSubmitted = false;
        d.eyeSubmitted = false;
        d.backdropWanted = false;
        d.pano.submitted = false;
        d.hudSubmitted = false;
        for (auto& w : d.world)
            w.submitted = false;

        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
        ei.displayTime = d.displayTime;
        ei.environmentBlendMode = d.blendMode;
        ei.layerCount = layerCount;
        ei.layers = layerCount ? layers : nullptr;
        d.Check(xrEndFrame(d.session, &ei), "xrEndFrame");
    }

    void Runtime::CinemaQuadPose(Pose& a_pose, float& a_width, float& a_height) const
    {
        const auto& d = *_impl;
        a_pose = ToPose(d.QuadPose());
        a_width = d.quadOverride ? d.quadOverrideWidth : d.config.quad.widthMeters;
        a_height = a_width * d.quadAspect;
    }

    void Runtime::Recenter()
    {
        auto& d = *_impl;
        if (d.headValid)
            d.ApplyRecenter();
        else
            d.recenterPending = true;
    }

    SessionState Runtime::state() const noexcept
    {
        const auto& d = *_impl;
        if (d.instance == XR_NULL_HANDLE)
            return SessionState::kNone;
        if (d.session != XR_NULL_HANDLE && d.xrState == XR_SESSION_STATE_UNKNOWN)
            return SessionState::kIdle;  // created, first state event not polled yet
        return ToState(d.xrState);
    }

    bool Runtime::running() const noexcept
    {
        const auto& d = *_impl;
        return d.sessionRunning && d.xrState != XR_SESSION_STATE_STOPPING;
    }

    const std::string& Runtime::lastError() const noexcept { return _impl->error; }

    const ControllerState& Runtime::Controllers() const noexcept { return _impl->controllers; }

    void Runtime::Vibrate(int a_hand, float a_amplitude, float a_seconds)
    {
        auto& d = *_impl;
        if (!d.input.attached || d.session == XR_NULL_HANDLE || a_hand < 0 || a_hand > 1)
            return;
        XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO };
        hi.action = d.input.haptic;
        hi.subactionPath = d.input.hand[a_hand];
        if (a_amplitude <= 0.0f) {
            d.Check(xrStopHapticFeedback(d.session, &hi), "xrStopHapticFeedback");
            return;
        }
        XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
        v.amplitude = std::min(a_amplitude, 1.0f);
        v.duration = a_seconds > 0.0f ? static_cast<XrDuration>(a_seconds * 1e9) : XR_MIN_HAPTIC_DURATION;
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        d.Check(xrApplyHapticFeedback(d.session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v)), "xrApplyHapticFeedback");
    }

    std::string Runtime::runtimeName() const { return _impl->runtimeName; }

    std::int64_t Runtime::swapchainFormat() const noexcept
    {
        for (const auto* sc : { &_impl->quadSc, &_impl->monoSc, &_impl->eyeSc[0] })
            if (sc->handle != XR_NULL_HANDLE)
                return sc->format;
        return 0;
    }
}
