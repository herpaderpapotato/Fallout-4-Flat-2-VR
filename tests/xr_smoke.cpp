// Standalone check of xrcore: own D3D11 device, synthetic "backbuffer", quad layer for N frames.
//   xr_smoke [frames=300] [--typeless] [--resize] [--mono [--sweep]] [--afr] [--input] [--hud]
//   --typeless: source is R8G8B8A8_TYPELESS; --resize: second half submits a 1280x720 source.
//   --mono: render a procedural world (floor checker, sky grid, compass pillars) from the head pose with
//           MonoRenderFov(16/9) and submit it via SubmitMono instead of the quad.
//   --sweep: (with --mono) render from the head pose yawed by +-25 deg over time. With correct
//           reprojection the world stays put and only the image border swings.
//   --afr:  alternate-eye stereo: render the world from the left eye's pose on even frames and the right
//           eye's on odd frames (EyeRenderFov(eye, 16/9)) and submit via SubmitEye. Every 50 frames (two in a row) the
//           per-eye swapchain images are captured and the far pillar / near post columns on the centre
//           row are compared with the projection expected from that eye's render pose and FOV.
//   --input: print the controller state whenever it changes (max 10 lines/s), pulse haptics once, and
//           check that input syncs without errors (active while focused, all-zero otherwise).
//   --hud:  also submit a semi-transparent, premultiplied 1920x1080 "HUD" (compass bar bottom centre, health
//           block bottom left, ring in the centre, the rest alpha 0) via SubmitHud, head-locked 1.2 m ahead,
//           0.25 m below eye level, 1.0 m wide. Every 50 frames the HUD swapchain image is captured and
//           compared with the source byte by byte, alpha included. Best combined with --mono.
// Run against the Meta XR Simulator with XR_RUNTIME_JSON pointing at its manifest.

#include "xr/XrRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kWidth = 1920;
    constexpr UINT kHeight = 1080;
    constexpr UINT kBarsTop = 900;  // frame-counter bars occupy rows [kBarsTop, kHeight)
    constexpr int  kBits = 16;

    // "Game" renderer for --mono/--afr: per-pixel ray from the render pose/FOV into a simple world.
    // LOCAL space: origin at eye level, floor 1.6 m below. Colours are written as display (sRGB) values.
    // Far compass pillars (direction only) test orientation/FOV; a near magenta post at kPost tests
    // position (eye parallax).
    constexpr float kPostX = 0.3f, kPostZ = -1.0f, kPostR = 0.04f, kPostTop = 0.3f, kFloorY = -1.6f;

    constexpr char kWorldHlsl[] = R"(
cbuffer World : register(b0) { float4 q; float4 tanLRUD; float4 camPos; float4 post; }  // post: x, z, radius, top
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 rot(float4 r, float3 v) { return v + 2 * cross(r.xyz, cross(r.xyz, v) + r.w * v); }
float onGrid(float deg, float step, float width) { return abs(frac(deg / step + 0.5) - 0.5) * step < width; }
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 v = float3(lerp(tanLRUD.x, tanLRUD.y, uv.x), lerp(tanLRUD.z, tanLRUD.w, uv.y), -1);
    float3 d = normalize(rot(q, v));
    float3 o = camPos.xyz;
    float yaw = degrees(atan2(d.x, -d.z));  // 0 = -Z, +90 = +X
    float pitch = degrees(asin(d.y));
    float3 c;
    float tFloor = 1e9;
    if (d.y < -0.01) {
        tFloor = (camPos.w - o.y) / d.y;
        float2 p = o.xz + d.xz * tFloor;
        int2 t = (int2)floor(p);
        c = ((t.x + t.y) & 1) ? float3(0.75, 0.75, 0.70) : float3(0.25, 0.30, 0.25);
        c = lerp(float3(0.45, 0.5, 0.55), c, saturate(6 / length(p - o.xz)));
    } else {
        c = lerp(float3(0.55, 0.70, 0.90), float3(0.10, 0.20, 0.55), saturate(pitch / 70));
        if (onGrid(yaw, 15, 0.2) || onGrid(pitch, 15, 0.2)) c = 0.95;
    }
    if (abs(pitch) < 8) {  // compass pillars on the horizon: -Z red, +X green, +Z blue, -X yellow
        if (abs(yaw) < 2.5) c = float3(1, 0, 0);
        if (abs(yaw - 90) < 2.5) c = float3(0, 1, 0);
        if (abs(abs(yaw) - 180) < 2.5) c = float3(0, 0.3, 1);
        if (abs(yaw + 90) < 2.5) c = float3(1, 1, 0);
    }
    // Near post: vertical cylinder from the floor to post.w.
    float2 oc = o.xz - post.xy;
    float a = dot(d.xz, d.xz), b = dot(oc, d.xz), cc = dot(oc, oc) - post.z * post.z;
    float disc = b * b - a * cc;
    if (disc > 0) {
        float t = (-b - sqrt(disc)) / a;
        float y = o.y + d.y * t;
        if (t > 0 && t < tFloor && y < post.w && y > camPos.w) c = float3(1, 0, 1);
    }
    return float4(c, 1);
}
)";

    struct WorldConstants { float q[4]; float tanLRUD[4]; float camPos[4]; float post[4]; };

    struct WorldRenderer
    {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader>  ps;
        ComPtr<ID3D11Buffer>       cb;

        bool Create(ID3D11Device* a_dev)
        {
            ComPtr<ID3DBlob> vsb, psb, errs;
            if (FAILED(D3DCompile(kWorldHlsl, sizeof(kWorldHlsl) - 1, "world", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &errs)) ||
                FAILED(D3DCompile(kWorldHlsl, sizeof(kWorldHlsl) - 1, "world", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &errs))) {
                std::printf("world shader: %s\n", errs ? static_cast<const char*>(errs->GetBufferPointer()) : "?");
                return false;
            }
            D3D11_BUFFER_DESC bd{ sizeof(WorldConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
            return SUCCEEDED(a_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) &&
                   SUCCEEDED(a_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps)) &&
                   SUCCEEDED(a_dev->CreateBuffer(&bd, nullptr, &cb));
        }

        void Draw(ID3D11DeviceContext* a_ctx, ID3D11RenderTargetView* a_rtv, const xr::Pose& a_pose, const xr::Fov& a_fov)
        {
            const auto& q = a_pose.orientation;
            const auto& p = a_pose.position;
            const WorldConstants c{ { q.x, q.y, q.z, q.w },
                                    { std::tan(a_fov.angleLeft), std::tan(a_fov.angleRight), std::tan(a_fov.angleUp), std::tan(a_fov.angleDown) },
                                    { p.x, p.y, p.z, kFloorY },
                                    { kPostX, kPostZ, kPostR, kPostTop } };
            a_ctx->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
            const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1 };
            a_ctx->IASetInputLayout(nullptr);
            a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            a_ctx->VSSetShader(vs.Get(), nullptr, 0);
            a_ctx->PSSetShader(ps.Get(), nullptr, 0);
            a_ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
            a_ctx->RSSetState(nullptr);
            a_ctx->RSSetViewports(1, &vp);
            a_ctx->OMSetRenderTargets(1, &a_rtv, nullptr);
            a_ctx->Draw(3, 0);
        }
    };

    // Expected pixel column of world point (or direction, a_isDir) for an image rendered with pose/fov.
    double ExpectedColumn(const xr::Pose& a_pose, const xr::Fov& a_fov, double a_x, double a_y, double a_z, bool a_isDir, UINT a_width)
    {
        double v[3] = { a_x, a_y, a_z };
        if (!a_isDir) {
            v[0] -= a_pose.position.x;
            v[1] -= a_pose.position.y;
            v[2] -= a_pose.position.z;
        }
        // rotate by the inverse orientation: v' = v + 2*cross(-q, cross(-q, v) + w*v)
        const double qx = -a_pose.orientation.x, qy = -a_pose.orientation.y, qz = -a_pose.orientation.z, qw = a_pose.orientation.w;
        const double tx = qy * v[2] - qz * v[1] + qw * v[0];
        const double ty = qz * v[0] - qx * v[2] + qw * v[1];
        const double tz = qx * v[1] - qy * v[0] + qw * v[2];
        const double cx = v[0] + 2 * (qy * tz - qz * ty);
        const double cz = v[2] + 2 * (qx * ty - qy * tx);
        const double xt = cx / -cz;
        const double tl = std::tan(a_fov.angleLeft), tr = std::tan(a_fov.angleRight);
        return (xt - tl) / (tr - tl) * a_width;
    }

    // Centroid (in pixel units, pixel i covers [i, i+1)) of the pixels in a_row matching a colour class.
    double MeasureColumn(const std::uint8_t* a_row, UINT a_width, bool a_bgra, bool a_magenta)
    {
        double sum = 0;
        int    n = 0;
        for (UINT x = 0; x < a_width; ++x) {
            const std::uint8_t* px = a_row + x * 4;
            const int r = a_bgra ? px[2] : px[0], g = px[1], b = a_bgra ? px[0] : px[2];
            const bool match = r > 180 && g < 80 && (a_magenta ? b > 180 : b < 80);
            if (match) {
                sum += x + 0.5;
                ++n;
            }
        }
        return n ? sum / n : -1.0;
    }

    xr::Quat Mul(const xr::Quat& a, const xr::Quat& b)
    {
        return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }

    std::string FormatControllers(const xr::ControllerState& c)
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "active=%d LS=(%+.2f %+.2f) RS=(%+.2f %+.2f) LT=%.2f RT=%.2f LG=%.2f RG=%.2f A%d B%d X%d Y%d LSc%d RSc%d menu%d "
                      "Laim%s(%+.2f %+.2f %+.2f) Raim%s(%+.2f %+.2f %+.2f)",
                      c.active, c.leftStickX, c.leftStickY, c.rightStickX, c.rightStickY, c.leftTrigger, c.rightTrigger, c.leftGrip,
                      c.rightGrip, c.a, c.b, c.x, c.y, c.leftStickClick, c.rightStickClick, c.menu, c.leftAimValid ? "" : "(invalid)",
                      c.leftAim.position.x, c.leftAim.position.y, c.leftAim.position.z, c.rightAimValid ? "" : "(invalid)",
                      c.rightAim.position.x, c.rightAim.position.y, c.rightAim.position.z);
        return buf;
    }

    // Buttons/axes rounded to 0.01 and aim positions to 1 cm, so tracking noise doesn't count as a change.
    std::string ControllerKey(const xr::ControllerState& c)
    {
        auto r = [](float v, float step) { return static_cast<int>(std::lround(v / step)); };
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%d|%d %d %d %d|%d %d %d %d|%d%d%d%d%d%d%d|%d %d %d %d|%d %d %d %d", c.active, r(c.leftStickX, 0.01f),
                      r(c.leftStickY, 0.01f), r(c.rightStickX, 0.01f), r(c.rightStickY, 0.01f), r(c.leftTrigger, 0.01f), r(c.rightTrigger, 0.01f),
                      r(c.leftGrip, 0.01f), r(c.rightGrip, 0.01f), c.a, c.b, c.x, c.y, c.leftStickClick, c.rightStickClick, c.menu,
                      c.leftAimValid, r(c.leftAim.position.x, 0.01f), r(c.leftAim.position.y, 0.01f), r(c.leftAim.position.z, 0.01f),
                      c.rightAimValid, r(c.rightAim.position.x, 0.01f), r(c.rightAim.position.y, 0.01f), r(c.rightAim.position.z, 0.01f));
        return buf;
    }

    std::chrono::steady_clock::time_point LongAgo() { return std::chrono::steady_clock::now() - std::chrono::hours(1); }

    bool IsZero(const xr::ControllerState& c)
    {
        return !c.active && c.leftStickX == 0 && c.leftStickY == 0 && c.rightStickX == 0 && c.rightStickY == 0 && c.leftTrigger == 0 &&
               c.rightTrigger == 0 && c.leftGrip == 0 && c.rightGrip == 0 && !c.a && !c.b && !c.x && !c.y && !c.leftStickClick &&
               !c.rightStickClick && !c.menu && !c.leftAimValid && !c.rightAimValid;
    }

    const char* StateName(xr::SessionState s)
    {
        switch (s) {
        case xr::SessionState::kNone:         return "NONE";
        case xr::SessionState::kIdle:         return "IDLE";
        case xr::SessionState::kReady:        return "READY";
        case xr::SessionState::kSynchronized: return "SYNCHRONIZED";
        case xr::SessionState::kVisible:      return "VISIBLE";
        case xr::SessionState::kFocused:      return "FOCUSED";
        case xr::SessionState::kStopping:     return "STOPPING";
        case xr::SessionState::kLossPending:  return "LOSS_PENDING";
        case xr::SessionState::kExiting:      return "EXITING";
        }
        return "?";
    }

    const char* FormatName(std::int64_t f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "DXGI_FORMAT_R10G10B10A2_UNORM";
        default:                                return "other";
        }
    }

    // The runtime dictates the adapter; ask it with a throwaway instance (the game never needs this:
    // its device already lives on the adapter driving the headset, or Init reports the mismatch).
    bool QueryRequiredAdapter(LUID& a_luid, D3D_FEATURE_LEVEL& a_minLevel, std::string& a_err)
    {
        const char* ext[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
        std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "xr_smoke_probe");
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = 1;
        ci.enabledExtensionNames = ext;
        XrInstance inst = XR_NULL_HANDLE;
        XrResult r = xrCreateInstance(&ci, &inst);
        if (XR_FAILED(r)) {
            a_err = "probe xrCreateInstance failed: " + std::to_string(r);
            return false;
        }
        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        if (XR_FAILED(r = xrGetSystem(inst, &sgi, &sys)))
            a_err = "probe xrGetSystem failed: ";
        else if (XR_FAILED(r = xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReq))))
            a_err = "probe xrGetInstanceProcAddr failed: ";
        else if (XR_FAILED(r = getReq(inst, sys, &req)))
            a_err = "probe xrGetD3D11GraphicsRequirementsKHR failed: ";
        if (XR_FAILED(r)) {
            char buf[XR_MAX_RESULT_STRING_SIZE]{};
            xrResultToString(inst, r, buf);
            a_err += buf;
        }
        xrDestroyInstance(inst);
        a_luid = req.adapterLuid;
        a_minLevel = req.minFeatureLevel;
        return XR_SUCCEEDED(r);
    }

    bool CreateDevice(const LUID& a_luid, D3D_FEATURE_LEVEL a_minLevel, ComPtr<ID3D11Device>& a_dev,
                      ComPtr<ID3D11DeviceContext>& a_ctx, bool& a_debug)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 ad{};
            adapter->GetDesc1(&ad);
            if (ad.AdapterLuid.LowPart == a_luid.LowPart && ad.AdapterLuid.HighPart == a_luid.HighPart) {
                std::printf("adapter: %ls\n", ad.Description);
                break;
            }
            adapter.Reset();
        }
        if (!adapter) {
            std::printf("no DXGI adapter matches the runtime's LUID\n");
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        for (const bool debug : { true, false }) {  // debug layer if installed, to catch state misuse
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
            D3D_FEATURE_LEVEL got{};
            if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                            D3D11_SDK_VERSION, &a_dev, &got, &a_ctx))) {
                a_debug = debug;
                return got >= a_minLevel;
            }
        }
        return false;
    }

    void FillBase(std::vector<std::uint32_t>& a_px)
    {
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                std::uint32_t c;
                if (y < 120) {
                    // 8 grey steps: mid-grey 128 should look mid-grey in the headset if gamma is right.
                    const std::uint32_t v = (x * 8 / kWidth) * 255 / 7;
                    c = v | (v << 8) | (v << 16);
                } else if (y < 240) {
                    // 1-pixel black/white stripes next to a solid 50% grey (should match from a distance).
                    c = x < kWidth / 2 ? ((x & 1) ? 0xFFFFFF : 0) : 0xBCBCBC;  // 188 = sRGB-encoded 50% linear
                } else {
                    const bool check = ((x / 120) + (y / 120)) & 1;
                    const std::uint32_t r = x * 255 / kWidth, g = y * 255 / kHeight;
                    c = check ? (r | (g << 8) | (0x40 << 16)) : 0x202020;
                }
                // Red/green/blue corner markers to show orientation (R8G8B8A8: byte0 = R).
                if (x < 80 && y >= 240 && y < 320) c = 0x0000FF;          // top-left red
                if (x >= kWidth - 80 && y >= 240 && y < 320) c = 0x00FF00; // top-right green
                a_px[y * kWidth + x] = c | 0xFF000000u;
            }
        }
    }

    // Binary frame counter (16 bars, MSB left) plus a sweeping marker.
    // --hud test image, premultiplied RGBA8 (R in the low byte): transparent except three HUD-like elements.
    std::uint32_t Premul(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
    {
        return (r * a / 255) | ((g * a / 255) << 8) | ((b * a / 255) << 16) | (a << 24);
    }

    void FillHud(std::vector<std::uint32_t>& a_px)
    {
        std::fill(a_px.begin(), a_px.end(), 0u);
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x) {
                std::uint32_t& p = a_px[y * kWidth + x];
                if (y >= 960 && y < 1010 && x >= 660 && x < 1260)  // compass: green, alpha ramps 64..255 left to right
                    p = Premul(40, 230, 60, 64 + (x - 660) * 191 / 599);
                else if (y >= 940 && y < 1030 && x >= 60 && x < 420)  // health: opaque white
                    p = Premul(255, 255, 255, 255);
                else {
                    const int dx = static_cast<int>(x) - 960, dy = static_cast<int>(y) - 540;
                    const int r2 = dx * dx + dy * dy;
                    if (r2 >= 30 * 30 && r2 < 36 * 36)  // crosshair ring: half-transparent orange
                        p = Premul(255, 160, 0, 128);
                }
            }
    }

    void FillBars(std::vector<std::uint32_t>& a_strip, std::uint32_t a_frame)
    {
        const UINT rows = kHeight - kBarsTop;
        const UINT sweep = (a_frame * 8) % kWidth;
        for (UINT y = 0; y < rows; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                const int bit = kBits - 1 - static_cast<int>(x * kBits / kWidth);
                const bool on = (a_frame >> bit) & 1;
                const bool gap = (x % (kWidth / kBits)) < 4;
                std::uint32_t c = gap ? 0x000000 : (on ? 0xFFFFFF : 0x303030);
                if (x >= sweep && x < sweep + 16 && y < rows / 3)
                    c = 0xFF0000;  // blue sweep
                a_strip[y * kWidth + x] = c | 0xFF000000u;
            }
        }
    }

    void DumpDebugMessages(ID3D11Device* a_dev)
    {
        ComPtr<ID3D11InfoQueue> q;
        if (FAILED(a_dev->QueryInterface(IID_PPV_ARGS(&q))))
            return;
        const UINT64 n = q->GetNumStoredMessages();
        std::uint32_t shown = 0;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            q->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
            if (FAILED(q->GetMessage(i, msg, &len)) || msg->Severity > D3D11_MESSAGE_SEVERITY_WARNING)
                continue;
            if (shown++ < 20)
                std::printf("d3d11 debug: %.*s\n", static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
        }
        std::printf("d3d11 debug layer: %u warning/error message(s)\n", shown);
        q->ClearStoredMessages();
    }
}

int main(int argc, char** argv)
{
    int  maxFrames = 300;
    bool typeless = false;
    bool resize = false;
    bool mono = false;
    bool sweep = false;
    bool afr = false;
    bool input = false;
    bool hud = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--typeless") == 0)
            typeless = true;
        else if (std::strcmp(argv[i], "--resize") == 0)
            resize = true;
        else if (std::strcmp(argv[i], "--mono") == 0)
            mono = true;
        else if (std::strcmp(argv[i], "--sweep") == 0)
            sweep = true;
        else if (std::strcmp(argv[i], "--afr") == 0)
            afr = true;
        else if (std::strcmp(argv[i], "--input") == 0)
            input = true;
        else if (std::strcmp(argv[i], "--hud") == 0)
            hud = true;
        else
            maxFrames = std::max(1, std::atoi(argv[i]));
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    LUID              luid{};
    D3D_FEATURE_LEVEL minLevel{};
    std::string       err;
    if (!QueryRequiredAdapter(luid, minLevel, err)) {
        std::printf("error: %s\n", err.c_str());
        return 2;
    }
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    bool                        debugLayer = false;
    if (!CreateDevice(luid, minLevel, dev, ctx, debugLayer)) {
        std::printf("error: could not create a D3D11 device on the required adapter\n");
        return 2;
    }
    std::printf("d3d11 debug layer: %s\n", debugLayer ? "on" : "unavailable");

    // Like a game backbuffer: no SRV bind flag; optionally typeless.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    std::vector<std::uint32_t> pixels(kWidth * kHeight);
    FillBase(pixels);
    const D3D11_SUBRESOURCE_DATA init{ pixels.data(), kWidth * 4, 0 };
    ComPtr<ID3D11Texture2D> src;
    if (FAILED(dev->CreateTexture2D(&td, &init, &src))) {
        std::printf("error: CreateTexture2D(source) failed\n");
        return 2;
    }
    // A "game" render target to bind around SubmitQuad, to check the context state is restored.
    D3D11_RENDER_TARGET_VIEW_DESC rvd{ DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D };
    ComPtr<ID3D11RenderTargetView> gameRtv;
    dev->CreateRenderTargetView(src.Get(), &rvd, &gameRtv);
    std::printf("source: %ux%u %s\n", kWidth, kHeight, typeless ? "R8G8B8A8_TYPELESS" : "R8G8B8A8_UNORM");

    // --resize: a smaller source, refreshed each frame from the top-left of the big one.
    ComPtr<ID3D11Texture2D> smallSrc;
    const D3D11_BOX         smallBox{ 0, 0, 0, 1280, 720, 1 };
    if (resize) {
        td.Width = 1280;
        td.Height = 720;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &smallSrc))) {
            std::printf("error: CreateTexture2D(small source) failed\n");
            return 2;
        }
    }

    // --hud: premultiplied test HUD, a render target like the game's Interface3D target (no SRV flag needed).
    std::vector<std::uint32_t> hudPixels;
    ComPtr<ID3D11Texture2D>    hudSrc, hudStaging;
    if (hud) {
        hudPixels.resize(kWidth * kHeight);
        FillHud(hudPixels);
        D3D11_TEXTURE2D_DESC hd = td;
        hd.Width = kWidth;
        hd.Height = kHeight;
        hd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hd.BindFlags = D3D11_BIND_RENDER_TARGET;
        const D3D11_SUBRESOURCE_DATA hinit{ hudPixels.data(), kWidth * 4, 0 };
        if (FAILED(dev->CreateTexture2D(&hd, &hinit, &hudSrc))) {
            std::printf("error: CreateTexture2D(hud) failed\n");
            return 2;
        }
        std::size_t visible = 0;
        for (const auto p : hudPixels)
            visible += (p >> 24) != 0;
        std::printf("hud source: %ux%u premultiplied, %zu of %u pixels with alpha > 0\n", kWidth, kHeight, visible, kWidth * kHeight);
    }
    int  hudChecks = 0, hudFailures = 0, hudSubmits = 0;
    int  hudMaxDiff = 0, hudMaxAlphaDiff = 0;

    WorldRenderer world;
    if ((mono || afr) && !world.Create(dev.Get())) {
        std::printf("error: world renderer creation failed\n");
        return 2;
    }

    xr::Runtime rt;
    xr::Config  cfg;
    cfg.appName = "xr_smoke";
    if (!rt.Init(dev.Get(), cfg)) {
        std::printf("error: Init failed: %s\n", rt.lastError().c_str());
        return 1;
    }
    std::printf("runtime: %s\n", rt.runtimeName().c_str());
    const std::string initErr = rt.lastError();  // non-fatal setup problems (e.g. input) are reported but don't fail frames
    if (!initErr.empty())
        std::printf("init note: %s\n", initErr.c_str());

    std::vector<std::uint32_t> strip(kWidth * (kHeight - kBarsTop));
    const D3D11_BOX            barsBox{ 0, kBarsTop, 0, kWidth, kHeight, 1 };
    xr::SessionState           lastState = rt.state();
    std::printf("state: %s\n", StateName(lastState));
    std::int64_t lastFormat = 0;
    std::string  lastErr;
    int  begun = 0, ended = 0, submitted = 0, stateMismatches = 0;
    bool everRunning = false;
    // --input bookkeeping
    std::string lastKey;
    auto        lastPrint = LongAgo();
    int         activeFrames = 0, inactiveNonZero = 0, inputLines = 0;
    bool        pulsed = false;
    // --afr bookkeeping and self-check results
    xr::Pose eyePose[2];
    xr::Fov  eyeFov[2];
    bool     eyeRendered[2]{};
    int      checks = 0, checkFailures = 0;
    double   maxColErr = 0.0;
    int      maxByteDiff = 0;
    ComPtr<ID3D11Texture2D> srcStaging;
    if (afr) {
        D3D11_TEXTURE2D_DESC sdesc{};
        src->GetDesc(&sdesc);
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.BindFlags = 0;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&sdesc, nullptr, &srcStaging);
    }
    const auto start = std::chrono::steady_clock::now();

    while (ended < maxFrames) {
        rt.PollEvents();
        const xr::SessionState s = rt.state();
        if (s != lastState) {
            std::printf("state: %s -> %s\n", StateName(lastState), StateName(s));
            lastState = s;
        }
        if (rt.lastError() != lastErr) {
            lastErr = rt.lastError();
            if (!lastErr.empty())
                std::printf("error: %s\n", lastErr.c_str());
        }
        if (s == xr::SessionState::kExiting || s == xr::SessionState::kNone)
            break;
        if (!everRunning && std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
            std::printf("timeout: session never started running\n");
            break;
        }

        xr::FrameInfo fi;
        if (!rt.BeginFrame(fi)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        ++begun;
        everRunning = true;
        if (input) {
            const xr::ControllerState& c = rt.Controllers();
            if (c.active)
                ++activeFrames;
            else if (!IsZero(c))
                ++inactiveNonZero;
            const std::string key = ControllerKey(c);
            const auto now = std::chrono::steady_clock::now();
            if (key != lastKey && now - lastPrint > std::chrono::milliseconds(100)) {
                std::printf("input frame %d: %s\n", begun, FormatControllers(c).c_str());
                lastKey = key;
                lastPrint = now;
                ++inputLines;
            }
            if (c.active && !pulsed) {
                rt.Vibrate(1, 0.5f, 0.1f);
                pulsed = true;
            }
        }
        if (begun == 1)
            std::printf("first frame: shouldRender=%d viewsValid=%d head=(%.2f %.2f %.2f) period=%lld ns\n", fi.shouldRender,
                        fi.viewsValid, fi.head.position.x, fi.head.position.y, fi.head.position.z,
                        static_cast<long long>(fi.predictedDisplayPeriod));

        xr::Pose renderPose = fi.head;
        xr::Fov  renderFov{};
        const int eye = (begun - 1) % 2;  // AFR: left on the first frame, then alternate
        if (afr) {
            renderPose = fi.eyes[eye].pose;
            renderFov = rt.EyeRenderFov(eye, 16.0f / 9.0f);
            if (begun <= 2)
                std::printf("eye %d: pos=(%.4f %.4f %.4f) located fov L/R/U/D=%.1f/%.1f/%.1f/%.1f render fov L/R/U/D=%.1f/%.1f/%.1f/%.1f deg\n", eye,
                            renderPose.position.x, renderPose.position.y, renderPose.position.z, fi.eyes[eye].fov.angleLeft * 57.2958f,
                            fi.eyes[eye].fov.angleRight * 57.2958f, fi.eyes[eye].fov.angleUp * 57.2958f, fi.eyes[eye].fov.angleDown * 57.2958f,
                            renderFov.angleLeft * 57.2958f, renderFov.angleRight * 57.2958f, renderFov.angleUp * 57.2958f,
                            renderFov.angleDown * 57.2958f);
            world.Draw(ctx.Get(), gameRtv.Get(), renderPose, renderFov);
        } else if (mono) {
            renderFov = rt.MonoRenderFov(16.0f / 9.0f);
            if (sweep) {  // yaw the render pose about the head's own up axis
                const float secsNow = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
                const float yaw = 0.4363f * std::sin(secsNow * 1.5708f);  // +-25 deg, 4 s period
                renderPose.orientation = Mul(fi.head.orientation, { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) });
            }
            if (begun == 1 || (sweep && begun % 100 == 0))
                std::printf("frame %d: renderFov L/R/U/D = %.1f/%.1f/%.1f/%.1f deg  eye0 L/R/U/D = %.1f/%.1f/%.1f/%.1f deg\n", begun,
                            renderFov.angleLeft * 57.2958f, renderFov.angleRight * 57.2958f, renderFov.angleUp * 57.2958f,
                            renderFov.angleDown * 57.2958f, fi.eyes[0].fov.angleLeft * 57.2958f, fi.eyes[0].fov.angleRight * 57.2958f,
                            fi.eyes[0].fov.angleUp * 57.2958f, fi.eyes[0].fov.angleDown * 57.2958f);
            world.Draw(ctx.Get(), gameRtv.Get(), renderPose, renderFov);
        } else {
            FillBars(strip, static_cast<std::uint32_t>(begun));
            ctx->UpdateSubresource(src.Get(), 0, &barsBox, strip.data(), kWidth * 4, 0);
        }

        // Simulate mid-frame game state, then check SubmitQuad put it back.
        const D3D11_VIEWPORT gameVp{ 10, 20, 640, 360, 0, 1 };
        ID3D11RenderTargetView* rtvIn = gameRtv.Get();  // the source is bound as RT, like a real backbuffer
        ctx->OMSetRenderTargets(1, &rtvIn, nullptr);
        ctx->RSSetViewports(1, &gameVp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

        ID3D11Texture2D* quadSrc = src.Get();
        if (smallSrc && ended >= maxFrames / 2) {
            if (ended == maxFrames / 2)
                std::printf("source switched to 1280x720 at frame %d\n", ended);
            ctx->CopySubresourceRegion(smallSrc.Get(), 0, 0, 0, 0, src.Get(), 0, &smallBox);
            quadSrc = smallSrc.Get();
        }
        bool queued = true;
        if (afr) {
            queued = rt.SubmitEye(ctx.Get(), src.Get(), eye, renderPose, renderFov);
            if (queued) {
                eyePose[eye] = renderPose;
                eyeFov[eye] = renderFov;
                eyeRendered[eye] = true;
            }
        } else if (mono)
            queued = rt.SubmitMono(ctx.Get(), src.Get(), renderPose, renderFov);
        else
            rt.SubmitQuad(ctx.Get(), quadSrc);
        // --hud: head-locked 1.2 m ahead, 0.25 m below eye level, tilted to face the eye.
        const bool hudCheck = hud && fi.shouldRender && begun % 50 == 10;
        if (hud) {
            const float    tilt = std::atan2(-0.25f, 1.2f);
            const xr::Pose hudPose{ { std::sin(tilt * 0.5f), 0.0f, 0.0f, std::cos(tilt * 0.5f) }, { 0.0f, -0.25f, -1.2f } };
            if (hudCheck)
                rt.SetEyeCapture(true);
            if (rt.SubmitHud(ctx.Get(), hudSrc.Get(), hudPose, 1.0f, true))
                ++hudSubmits;
            else if (fi.shouldRender)
                queued = false;
            if (hudCheck && !(afr && fi.shouldRender && eyeRendered[0] && eyeRendered[1] && begun % 50 < 2))
                rt.SetEyeCapture(false);
        }
        if (queued && fi.shouldRender && rt.running() && rt.lastError() == initErr)  // errors are sticky: count only clean frames
            ++submitted;

        const bool check = afr && fi.shouldRender && eyeRendered[0] && eyeRendered[1] && begun % 50 < 2;  // two consecutive frames: both eyes get a byte check
        if (check) {
            rt.SetEyeCapture(true);
            ctx->CopyResource(srcStaging.Get(), src.Get());  // this frame's eye, as rendered
        }

        rt.EndFrame();
        ++ended;

        // The game's state must survive SubmitX and EndFrame (AFR blits inside EndFrame).
        UINT           nvp = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nvp, &vp);
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ctx->IAGetPrimitiveTopology(&topo);
        ComPtr<ID3D11RenderTargetView> rtvOut;
        ctx->OMGetRenderTargets(1, &rtvOut, nullptr);
        if (nvp != 1 || vp.TopLeftX != 10 || vp.Width != 640 || topo != D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP || rtvOut.Get() != rtvIn)
            ++stateMismatches;

        if (check) {
            rt.SetEyeCapture(false);
            D3D11_MAPPED_SUBRESOURCE sm{};
            ctx->Map(srcStaging.Get(), 0, D3D11_MAP_READ, 0, &sm);
            for (int e = 0; e < 2; ++e) {
                ComPtr<ID3D11Texture2D> cap;
                cap.Attach(rt.CapturedEyeImage(e));
                D3D11_TEXTURE2D_DESC cd{};
                D3D11_MAPPED_SUBRESOURCE cm{};
                if (!cap || (cap->GetDesc(&cd), FAILED(ctx->Map(cap.Get(), 0, D3D11_MAP_READ, 0, &cm)))) {
                    std::printf("check frame %d eye %d: no captured image\n", begun, e);
                    ++checkFailures;
                    continue;
                }
                const bool bgra = cd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                const UINT row = cd.Height / 2;
                const auto* line = static_cast<const std::uint8_t*>(cm.pData) + static_cast<size_t>(row) * cm.RowPitch;
                const double postExp = ExpectedColumn(eyePose[e], eyeFov[e], kPostX, 0.0, kPostZ, false, cd.Width);
                const double pillarExp = ExpectedColumn(eyePose[e], eyeFov[e], 0.0, 0.0, -1.0, true, cd.Width);
                const double postGot = MeasureColumn(line, cd.Width, bgra, true);
                const double pillarGot = MeasureColumn(line, cd.Width, bgra, false);
                const double colErr = std::max(std::fabs(postGot - postExp), std::fabs(pillarGot - pillarExp));
                // Byte-exactness vs the source, only for the eye rendered this frame (the other is from the last frame).
                int byteDiff = -1;
                if (e == eye && cd.Width == kWidth) {
                    byteDiff = 0;
                    const auto* sline = static_cast<const std::uint8_t*>(sm.pData) + static_cast<size_t>(row) * sm.RowPitch;
                    for (UINT x = 0; x < kWidth; ++x)
                        for (int ch = 0; ch < 3; ++ch) {
                            const int c = bgra ? (ch == 0 ? 2 : ch == 2 ? 0 : 1) : ch;
                            byteDiff = std::max(byteDiff, std::abs(int(line[x * 4 + c]) - int(sline[x * 4 + ch])));
                        }
                    maxByteDiff = std::max(maxByteDiff, byteDiff);
                }
                ctx->Unmap(cap.Get(), 0);
                const bool ok = postGot >= 0 && pillarGot >= 0 && colErr < 1.5 && byteDiff <= 1;
                if (!ok)
                    ++checkFailures;
                maxColErr = std::max(maxColErr, colErr);
                ++checks;
                if (checks <= 4 || !ok)
                    std::printf("check frame %d eye %d (%s): post exp %.2f got %.2f | pillar exp %.2f got %.2f | max col err %.2f px | bytes vs src %s%d -> %s\n",
                                begun, e, e == eye ? "rendered this frame" : "from previous frame", postExp, postGot, pillarExp, pillarGot, colErr,
                                byteDiff < 0 ? "n/a " : "maxdiff ", byteDiff < 0 ? 0 : byteDiff, ok ? "ok" : "FAIL");
            }
            ctx->Unmap(srcStaging.Get(), 0);
        }

        if (hudCheck) {
            ComPtr<ID3D11Texture2D> cap;
            cap.Attach(rt.CapturedHudImage());
            D3D11_TEXTURE2D_DESC     cd{};
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (!cap || (cap->GetDesc(&cd), cd.Width != kWidth || cd.Height != kHeight) ||
                FAILED(ctx->Map(cap.Get(), 0, D3D11_MAP_READ, 0, &cm))) {
                std::printf("hud check frame %d: no captured image\n", begun);
                ++hudFailures;
            } else {
                const bool bgra = cd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                int         maxDiff = 0, maxAlphaDiff = 0;
                std::size_t visible = 0, visibleSrc = 0;
                for (UINT y = 0; y < kHeight; ++y) {
                    const auto* line = static_cast<const std::uint8_t*>(cm.pData) + static_cast<size_t>(y) * cm.RowPitch;
                    for (UINT x = 0; x < kWidth; ++x) {
                        const std::uint32_t sp = hudPixels[y * kWidth + x];
                        const int           sa = static_cast<int>(sp >> 24);
                        const int           ga = line[x * 4 + 3];
                        visible += ga != 0;
                        visibleSrc += sa != 0;
                        maxAlphaDiff = std::max(maxAlphaDiff, std::abs(ga - sa));
                        for (int ch = 0; ch < 3; ++ch) {
                            const int c = bgra ? (ch == 0 ? 2 : ch == 2 ? 0 : 1) : ch;
                            maxDiff = std::max(maxDiff, std::abs(int(line[x * 4 + c]) - int((sp >> (8 * ch)) & 0xFF)));
                        }
                    }
                }
                ctx->Unmap(cap.Get(), 0);
                const bool ok = maxAlphaDiff <= 1 && maxDiff <= 1 && visible == visibleSrc;
                hudFailures += ok ? 0 : 1;
                hudMaxDiff = std::max(hudMaxDiff, maxDiff);
                hudMaxAlphaDiff = std::max(hudMaxAlphaDiff, maxAlphaDiff);
                ++hudChecks;
                if (hudChecks <= 2 || !ok)
                    std::printf("hud check frame %d: swapchain %ux%u fmt %d, alpha>0 pixels %zu (source %zu), max alpha diff %d, max rgb diff %d -> %s\n",
                                begun, cd.Width, cd.Height, static_cast<int>(cd.Format), visible, visibleSrc, maxAlphaDiff, maxDiff, ok ? "ok" : "FAIL");
            }
        }

        if (rt.swapchainFormat() != lastFormat) {
            lastFormat = rt.swapchainFormat();
            std::printf("%s swapchain format: %lld (%s)\n", afr ? "eye" : mono ? "mono" : "quad", static_cast<long long>(lastFormat), FormatName(lastFormat));
        }
        if (ended == 60)
            rt.Recenter();
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("frames begun=%d ended=%d submitted(shouldRender)=%d in %.1fs, final state %s\n", begun, ended, submitted,
                secs, StateName(rt.state()));
    std::printf("context state restore: %s\n", stateMismatches ? "MISMATCH" : "ok");
    if (input)
        std::printf("input self-check: active on %d of %d frames, non-zero while inactive: %d, state lines printed: %d, haptic pulse %s\n",
                    activeFrames, begun, inactiveNonZero, inputLines, pulsed ? "sent" : "not sent");
    if (afr)
        std::printf("afr self-check: %d eye images checked, %d failed, max column error %.2f px, max byte diff vs source %d\n", checks,
                    checkFailures, maxColErr, maxByteDiff);
    if (hud)
        std::printf("hud self-check: submitted %d frames, %d images checked, %d failed, max alpha diff %d, max rgb diff %d\n", hudSubmits,
                    hudChecks, hudFailures, hudMaxAlphaDiff, hudMaxDiff);
    if (!rt.lastError().empty())
        std::printf("last error: %s\n", rt.lastError().c_str());
    if (debugLayer)
        DumpDebugMessages(dev.Get());

    rt.Shutdown();
    const bool afrOk = !afr || (checks > 0 && checkFailures == 0);
    const bool inputOk = !input || (activeFrames > 0 && inactiveNonZero == 0 && initErr.empty() && rt.lastError().empty());
    const bool hudOk = !hud || (hudSubmits > 0 && hudChecks > 0 && hudFailures == 0);
    return submitted > 0 && stateMismatches == 0 && afrOk && inputOk && hudOk ? 0 : 1;
}
