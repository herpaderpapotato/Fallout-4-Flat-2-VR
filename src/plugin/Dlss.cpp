// Dlss.cpp - per-eye DLSS (DLAA) + DLSS neural rendering inside fo4flatvr (20 Nov, DLSS agent). See Dlss.h and
// analysis/dlss-in-fo4flatvr.md. Reference code (read as data): mods/dishonored2-vr/src/dlss_eye_upscaler.hpp (D3D11 NGX,
// subrect evaluates), mods/DLSS5-Feeder-2.0.0 (D3D11<->D3D12 bridge: textures shared from the D3D11 side, one shared fence,
// GPU waits), work/mod-research/renodx (RenoDX DLSS5 add-on source: the feature-18 contract and the SDR composition).
#include "plugin/Dlss.h"

#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"

#include <REX/LOG.h>

#include <Windows.h>
#undef ERROR
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#pragma warning(push, 0)
#include "nvsdk_ngx_helpers_d3d.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fo4flatvr::dlss
{
    namespace
    {
        // ---------------------------------------------------------------------------------------------------------------
        // settings ([DLSS], [NeuralRendering]; plugin/tools/config_spec.py, every key live)
        struct PassCfg
        {
            bool  follow{ true };
            float intensity{ 1.0f }, color{ 1.0f }, transfer{ 1.0f };
            int   style{ 0 };
            float localTone{ 1.0f }, structure{ 1.0f }, skin{ -1.0f };
            bool  autoMask{ false }, uiCorrection{ false };
        };
        struct Cfg
        {
            // DLSS
            bool  enabled{ true }, perEye{ true }, jitter{ true }, autoExposure{ true }, biasMask{ false }, gpuTimers{ true };
            bool  copyHistory{ true };
            int   preset{ 5 }, quality{ 0 }, jitterFlipX{ 0 }, jitterFlipY{ 0 }, depthInverted{ 0 }, createFlags{ -1 };
            int   resetEvery{ 0 }, createDelay{ 0 }, warmupRebuild{ 0 }, logFrames{ 3 };
            float sharpness{ 0.0f }, mvScaleX{ 1.0f }, mvScaleY{ 1.0f };
            // NR
            bool  nrEnabled{ true }, nrPerEye{ true }, nrSkinIndependent{ true }, nrFollowInputRes{ false }, nrChainedHistory{ true };
            int   nrDetailStability{ 0 };
            float nrStabilityAlpha{ 0.15f };
            int   nrHookPoint{ 0 }, nrPreset{ 0 }, nrDepthMode{ 0 }, nrBlackRestore{ 3 }, nrPasses{ 1 }, nrToggleKey{ 0x75 };
            float nrScale{ 0.5f }, nrMvScaleX{ 1.0f }, nrMvScaleY{ 1.0f };
            PassCfg p1;          // pass 1 = the main look controls
            PassCfg pass[3];     // passes 2-4
            // RenoDX look stage (look_stage.hpp, NRLook*): bLook = the whole stage (0 = the pre-look pipeline), bLookMode = NRLookMode
            bool  look{ true }, lookMode{ true }, lookStabDetail{ false };
            float lookStrength{ 1.0f }, lookBrighten{ 1.0f }, lookDarken{ 1.0f }, lookMaxBrighten{ 0.0f }, lookMaxDarken{ 0.0f };
            float lookColour{ 1.0f }, lookHue{ 1.0f }, lookMaxColour{ 0.0f }, lookShadows{ 1.0f }, lookMidtones{ 1.0f }, lookHighlights{ 1.0f };
            float lookTone{ 1.0f }, lookDetail{ 1.0f }, lookDetailRadius{ 1.0f }, lookHalo{ 0.0f }, lookHaloBright{ 0.0f }, lookStabMs{ 60.0f };
            int   lookStabilize{ 0 }, lookUpsample{ 0 };
        };

        // [DLSS] iPreset: 0 = the DLSS 5 Feed's default (L), 1 driver default, E F J K L M. Default 5 = K (20 Nov measured, 2 x 2688x2880,
        // RTX 5090: K 1.64 ms / L 7.4 ms GPU for both eyes, and K the steadier on a still scene: mean |dL| 0.08-0.10 vs 0.30-0.56)
        constexpr int kPresetValues[] = { 12, 0, 5, 6, 10, 11, 12, 13 };
        constexpr const char* kPresetNames[] = { "L (Feed default)", "driver default", "E", "F", "J", "K", "L", "M" };
        constexpr NVSDK_NGX_PerfQuality_Value kQualityValues[] = { NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_UltraQuality,
            NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
            NVSDK_NGX_PerfQuality_Value_UltraPerformance };
        constexpr const char* kQualityNames[] = { "DLAA", "UltraQuality", "Quality", "Balanced", "Performance", "UltraPerformance" };

        std::mutex       g_cfgLock;
        Cfg              g_cfg;
        std::atomic<int> g_cfgRev{ 0 };

        std::string Get(const std::map<std::string, std::string>& a_m, const char* a_sec, const char* a_key)
        {
            std::string k = std::string(a_sec) + "\n" + a_key;
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto it = a_m.find(k);
            if (it == a_m.end()) return {};
            std::string v = it->second;
            if (const auto sc = v.find_first_of(";#"); sc != std::string::npos) v.erase(sc);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            return v;
        }
        void ReadB(const std::map<std::string, std::string>& m, const char* s, const char* k, bool& v)
        {
            const auto t = Get(m, s, k);
            if (!t.empty()) v = std::strtol(t.c_str(), nullptr, 0) != 0;
        }
        void ReadI(const std::map<std::string, std::string>& m, const char* s, const char* k, int& v, int lo, int hi)
        {
            const auto t = Get(m, s, k);
            if (!t.empty()) v = std::clamp(static_cast<int>(std::strtol(t.c_str(), nullptr, 0)), lo, hi);
        }
        void ReadF(const std::map<std::string, std::string>& m, const char* s, const char* k, float& v, float lo, float hi)
        {
            const auto t = Get(m, s, k);
            if (!t.empty()) v = std::clamp(std::strtof(t.c_str(), nullptr), lo, hi);
        }

        Cfg LoadCfg()
        {
            Cfg c;
            const auto m = cfgstore::MergedValues();
            const char* D = "DLSS";
            ReadB(m, D, "bEnabled", c.enabled);
            ReadB(m, D, "bPerEye", c.perEye);
            ReadB(m, D, "bJitter", c.jitter);
            ReadB(m, D, "bAutoExposure", c.autoExposure);
            ReadB(m, D, "bBiasMask", c.biasMask);
            ReadB(m, D, "bGpuTimers", c.gpuTimers);
            ReadB(m, D, "bCopyHistory", c.copyHistory);
            ReadI(m, D, "iPreset", c.preset, 0, 7);
            ReadI(m, D, "iQuality", c.quality, 0, 5);
            ReadI(m, D, "iJitterFlipX", c.jitterFlipX, 0, 1);
            ReadI(m, D, "iJitterFlipY", c.jitterFlipY, 0, 1);
            ReadI(m, D, "iDepthInverted", c.depthInverted, 0, 2);
            ReadI(m, D, "iCreateFlags", c.createFlags, -1, 0xFFFF);
            ReadI(m, D, "iResetEvery", c.resetEvery, 0, 100000);
            ReadI(m, D, "iCreateDelay", c.createDelay, 0, 100000);
            ReadI(m, D, "iWarmupRebuild", c.warmupRebuild, 0, 100000);
            ReadI(m, D, "iLogFrames", c.logFrames, 0, 1000);
            ReadF(m, D, "fSharpness", c.sharpness, 0.0f, 1.0f);
            ReadF(m, D, "fMvScaleX", c.mvScaleX, -4.0f, 4.0f);
            ReadF(m, D, "fMvScaleY", c.mvScaleY, -4.0f, 4.0f);
            const char* N = "NeuralRendering";
            ReadB(m, N, "bEnabled", c.nrEnabled);
            ReadB(m, N, "bPerEye", c.nrPerEye);
            ReadB(m, N, "bSkinIndependent", c.nrSkinIndependent);
            ReadB(m, N, "bFollowInputRes", c.nrFollowInputRes);
            ReadB(m, N, "bChainedHistory", c.nrChainedHistory);
            ReadI(m, N, "iHookPoint", c.nrHookPoint, 0, 1);
            ReadI(m, N, "iPreset", c.nrPreset, 0, 3);
            ReadI(m, N, "iDepthMode", c.nrDepthMode, 0, 2);
            ReadI(m, N, "iSdrBlackRestore", c.nrBlackRestore, 0, 3);
            ReadI(m, N, "iPasses", c.nrPasses, 1, 4);
            ReadI(m, N, "iDetailStability", c.nrDetailStability, 0, 2);
            ReadF(m, N, "fDetailStabilityAlpha", c.nrStabilityAlpha, 0.02f, 1.0f);
            ReadI(m, N, "iToggleKey", c.nrToggleKey, 0, 255);
            ReadF(m, N, "fResolutionScale", c.nrScale, 0.33f, 1.0f);
            ReadF(m, N, "fMvScaleX", c.nrMvScaleX, -4.0f, 4.0f);
            ReadF(m, N, "fMvScaleY", c.nrMvScaleY, -4.0f, 4.0f);
            auto readPass = [&](const char* pre, PassCfg& p, bool withFollow) {
                auto k = [&](const char* name) { static thread_local std::string s; s = std::string(pre) + name; return s.c_str(); };
                if (withFollow) ReadB(m, N, k("bFollowPass1"), p.follow);
                ReadF(m, N, k("fIntensity"), p.intensity, 0.0f, 2.0f);
                ReadF(m, N, k("fColorStrength"), p.color, 0.0f, 1.0f);
                ReadF(m, N, k("fTransferStrength"), p.transfer, 0.0f, 1.0f);
                ReadI(m, N, k("iStyle"), p.style, 0, 2);
                ReadF(m, N, k("fLocalTone"), p.localTone, 0.0f, 2.0f);
                ReadF(m, N, k("fLocalStructure"), p.structure, 0.0f, 2.0f);
                ReadF(m, N, k("fSkinStructure"), p.skin, -1.0f, 1.0f);
                ReadB(m, N, k("bAutoMask"), p.autoMask);
                ReadB(m, N, k("bUiCorrection"), p.uiCorrection);
            };
            readPass("", c.p1, false);
            readPass("Pass2", c.pass[0], true);
            readPass("Pass3", c.pass[1], true);
            readPass("Pass4", c.pass[2], true);
            // look stage: ranges = RenoDX's SanitizeFloatKey / SanitizeEnumKey
            ReadB(m, N, "bLook", c.look);
            ReadB(m, N, "bLookMode", c.lookMode);
            ReadF(m, N, "fLookStrength", c.lookStrength, 0.0f, 2.0f);
            ReadF(m, N, "fLookBrighten", c.lookBrighten, 0.0f, 2.0f);
            ReadF(m, N, "fLookDarken", c.lookDarken, 0.0f, 2.0f);
            ReadF(m, N, "fLookMaxBrighten", c.lookMaxBrighten, 0.0f, 4.0f);
            ReadF(m, N, "fLookMaxDarken", c.lookMaxDarken, 0.0f, 4.0f);
            ReadF(m, N, "fLookColour", c.lookColour, 0.0f, 2.0f);
            ReadF(m, N, "fLookHue", c.lookHue, 0.0f, 2.0f);
            ReadF(m, N, "fLookMaxColour", c.lookMaxColour, 0.0f, 2.0f);
            ReadF(m, N, "fLookShadows", c.lookShadows, 0.0f, 2.0f);
            ReadF(m, N, "fLookMidtones", c.lookMidtones, 0.0f, 2.0f);
            ReadF(m, N, "fLookHighlights", c.lookHighlights, 0.0f, 2.0f);
            ReadF(m, N, "fLookTone", c.lookTone, 0.0f, 2.0f);
            ReadF(m, N, "fLookDetail", c.lookDetail, 0.0f, 2.0f);
            ReadF(m, N, "fLookDetailRadius", c.lookDetailRadius, 0.25f, 4.0f);
            ReadF(m, N, "fLookHalo", c.lookHalo, 0.0f, 1.0f);
            ReadF(m, N, "fLookHaloBrightSide", c.lookHaloBright, 0.0f, 1.0f);
            ReadI(m, N, "iLookStabilize", c.lookStabilize, 0, 2);
            ReadF(m, N, "fLookStabilizeMs", c.lookStabMs, 10.0f, 250.0f);
            ReadB(m, N, "bLookStabilizeDetail", c.lookStabDetail);
            ReadI(m, N, "iLookUpsample", c.lookUpsample, 0, 3);
            return c;
        }

        Cfg CurCfg()
        {
            std::scoped_lock l(g_cfgLock);
            return g_cfg;
        }

        // ---------------------------------------------------------------------------------------------------------------
        // small helpers
        template <class T>
        void SafeRelease(T*& p)
        {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        std::string NgxName(NVSDK_NGX_Result r)
        {
            switch (r) {
            case NVSDK_NGX_Result_Success: return "Success";
            case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
            case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
            case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
            case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
            case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
            case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
            case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
            case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
            case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
            case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
            case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
            case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
            case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
            case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
            case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
            case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
            case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
            case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
            default: return std::format("0x{:08X}", static_cast<std::uint32_t>(r));
            }
        }
        std::string Res(NVSDK_NGX_Result r) { return std::format("0x{:08X} ({})", static_cast<std::uint32_t>(r), NgxName(r)); }

        DXGI_FORMAT TypedOf(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
            default: return f;
            }
        }
        bool IsSrgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

        ID3D11Texture2D* TexOf(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* d = nullptr)
        {
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
                if (d) t->GetDesc(d);
                t->Release();  // weak: the caller holds the resource
            }
            return t;
        }

        std::wstring Widen(const std::string& s) { return std::filesystem::path(s).wstring(); }

        std::wstring ModuleDir()
        {
            HMODULE m = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleDir), &m);
            wchar_t p[MAX_PATH * 2]{};
            const DWORD n = ::GetModuleFileNameW(m, p, static_cast<DWORD>(std::size(p)));
            std::wstring s(p, n);
            if (const auto sl = s.find_last_of(L"\\/"); sl != std::wstring::npos) s.resize(sl);
            return s;
        }
        HMODULE SelfModule()
        {
            HMODULE m = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&SelfModule), &m);
            return m;
        }

        std::wstring g_ngxDir, g_dataDir;
        constexpr const char* kProjectId = "3b6e7a2c-5f1d-4c8e-9a0b-f04f1a7d1550";

        void NVSDK_CONV NgxLog(const char* a_msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature a_feat)
        {
            std::string_view t = a_msg ? a_msg : "";
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.remove_suffix(1);
            if (t.empty()) return;
            std::string low(t);
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            static std::atomic<int> s_lines{ 0 };
            const bool important = low.find("error") != std::string::npos || low.find("fail") != std::string::npos ||
                                   low.find("warn") != std::string::npos || low.find("unsupported") != std::string::npos ||
                                   low.find("loaded") != std::string::npos || low.find("version") != std::string::npos;
            if (important && s_lines.fetch_add(1) < 200) {
                REX::INFO("dlss: [ngx feature {}] {}", static_cast<int>(a_feat), t);
            }
        }

        // ---------------------------------------------------------------------------------------------------------------
        // D3D11 compute passes (compiled at first use; cs_5_0)
        constexpr const char* kCsSource = R"HLSL(
cbuffer C : register(b0)
{
    uint2 SrcBase;      // region origin in the double-wide source (full res)
    uint2 SrcSize;      // region size (full res)
    uint2 WorkBase;     // region origin in the working (NR) textures
    uint2 WorkSize;     // region size in the working textures
    uint2 BlockBase;    // region origin in the block-mean texture
    uint2 Pad0;
    float Transfer;
    float ColorStrength;
    float Gate;         // SDR black restore: dark gate in encoded luminance
    uint  RestoreMode;  // 0 off, 1 hard gate, 2 smooth gate
};
Texture2D<float4>   Src      : register(t0);
Texture2D<float4>   Proxy    : register(t1);
Texture2D<float4>   Neural   : register(t2);
Texture2D<float4>   Blocks   : register(t3);
Texture2D<float2>   MvIn     : register(t4);
Texture2D<float>    DepthIn  : register(t5);
RWTexture2D<float4> Out      : register(u0);
RWTexture2D<float2> MvOut    : register(u1);
RWTexture2D<float>  DepthOut : register(u2);
SamplerState        Lin      : register(s0);

float Luma(float3 c) { return dot(c, float3(0.212639, 0.715169, 0.072192)); }
float3 SrgbEncodeP(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 SrgbDecodeP(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

#ifdef DEPTH
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= SrcSize)) return;
    DepthOut[SrcBase + id.xy] = DepthIn.Load(int3(SrcBase + id.xy, 0)).r;
}
#endif

#ifdef PREP
// Proxy for the model: the area average of the source over the working pixel's footprint (SDR: display-referred already,
// passed through; alpha forced to 1, as RenoDX's legacy_encode). Guides: the motion vector and depth at the footprint centre.
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 w = id.xy;
    if (any(w >= WorkSize)) return;
    const float2 scale = float2(SrcSize) / float2(WorkSize);
    const float2 c = (float2(w) + 0.5) * scale;
    // Pad0.x: 0 = area mean in the encoded values (pre-look pipeline), 1 = point sample (RenoDX legacy_encode v7 input, the
    // look stage's Auto / Edge-aware / Classic), 2 = area mean in linear light (RenoDX NRLookUpsample 3, "area input")
    float3 proxy;
    if (Pad0.x == 1) {
        proxy = Src.Load(int3(SrcBase + min(uint2(c), SrcSize - 1), 0)).rgb;
    } else {
        const float2 fmin = max(c - 0.5 * scale, 0.0), fmax = min(c + 0.5 * scale, float2(SrcSize));
        const int2 first = int2(floor(fmin)), last = int2(ceil(fmax));
        float3 sum = 0; float wsum = 0;
        for (int y = first.y; y < last.y; ++y)
            for (int x = first.x; x < last.x; ++x) {
                const float2 o = min(float2(x + 1, y + 1), fmax) - max(float2(x, y), fmin);
                const float ww = max(o.x, 0) * max(o.y, 0);
                const float3 t = Src.Load(int3(SrcBase + int2(x, y), 0)).rgb;
                sum += (Pad0.x == 2 ? SrgbDecodeP(t) : t) * ww;
                wsum += ww;
            }
        proxy = wsum > 0 ? sum / wsum : 0;
        if (Pad0.x == 2) proxy = SrgbEncodeP(proxy);
    }
    Out[WorkBase + w] = float4(saturate(proxy), 1.0);
    const int2 cp = SrcBase + min(int2(c), int2(SrcSize) - 1);
    MvOut[WorkBase + w] = MvIn.Load(int3(cp, 0));
    DepthOut[WorkBase + w] = DepthIn.Load(int3(cp, 0));
}
#endif

#if defined(REDUCE) || defined(COMPOSE)
float3 Sample(Texture2D<float4> t, uint2 p)
{
    // region-local full-res pixel -> working texel (bilinear, clamped to the region)
    if (all(WorkSize == SrcSize)) return t.Load(int3(WorkBase + p, 0)).rgb;
    const float2 pos = (float2(p) + 0.5) * float2(WorkSize) / float2(SrcSize) - 0.5;
    const int2 mx = int2(WorkSize) - 1;
    const int2 b0 = clamp(int2(floor(pos)), 0, mx), b1 = min(b0 + 1, mx);
    const float2 f = saturate(pos - float2(b0));
    const float3 s00 = t.Load(int3(WorkBase + b0, 0)).rgb, s10 = t.Load(int3(WorkBase + int2(b1.x, b0.y), 0)).rgb;
    const float3 s01 = t.Load(int3(WorkBase + int2(b0.x, b1.y), 0)).rgb, s11 = t.Load(int3(WorkBase + b1, 0)).rgb;
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}
// RenoDX legacy (SDR) resolve: NR's result goes in (Transfer fades it towards the source; at full resolution the proxy IS the
// source, so this is lerp(original, neural, Transfer)); below full resolution the model's edit N - P is carried onto the
// full-res source. Then Colour strength mixes the luminance-only change with the full one (ratio clamped to 0..4).
float3 Resolve(uint2 p, float3 original)
{
    // Pad0.x = 1: t2 holds the model's edit after the detail-stability filter (STABILIZE), not the model output.
    // Pad0.x = 2: t2 is the look stage's edge-aware transport N'_up at full resolution (look_upsample; WorkSize == SrcSize,
    // WorkBase 0): RenoDX's SDR resolve takes it whole (upgraded = N'_up), so the edit is N'_up - original.
    const float3 edit = Pad0.x == 2 ? Sample(Neural, p) - original : Pad0.x != 0 ? Sample(Neural, p) : Sample(Neural, p) - Sample(Proxy, p);
    const float3 upgraded = saturate(original + Transfer * edit);
    const float oy = Luma(original), uy = Luma(upgraded);
    const float ratio = oy == 0 ? 1.0 : clamp(uy / oy, 0.0, 4.0);
    return lerp(original * ratio, upgraded, ColorStrength);
}
#endif

#ifdef STABILIZE
// RenoDX "Detail stability" (v8.5 NRDetailStability: the look stage's temporal filter on the whole edit, alpha 0.15): the model's
// edit E = N - P is blended with last frame's filtered edit, reprojected with the motion vectors and clipped to the 3x3
// neighbourhood range of this frame's edit. Constants: Transfer = alpha, ColorStrength / Gate = MV scale to working px,
// RestoreMode = history valid, SrcSize = working texture size. t3 = history in, u0 = history out (the filtered edit).
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 w = int2(id.xy);
    if (any(id.xy >= WorkSize)) return;
    const int2 p = int2(WorkBase) + w;
    const float3 e = Neural.Load(int3(p, 0)).rgb - Proxy.Load(int3(p, 0)).rgb;
    float3 mn = e, mx = e;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 q = int2(WorkBase) + clamp(w + int2(dx, dy), 0, int2(WorkSize) - 1);
            const float3 eq = Neural.Load(int3(q, 0)).rgb - Proxy.Load(int3(q, 0)).rgb;
            mn = min(mn, eq);
            mx = max(mx, eq);
        }
    float3 outE = e;
    if (RestoreMode != 0) {
        const float2 mv = MvIn.Load(int3(p, 0));
        const float2 prev = float2(w) + 0.5 + mv * float2(ColorStrength, Gate);
        if (all(prev >= 0.5) && all(prev <= float2(WorkSize) - 0.5)) {
            const float2 uv = (float2(WorkBase) + prev) / float2(SrcSize);
            const float3 h = clamp(Blocks.SampleLevel(Lin, uv, 0).rgb, mn, mx);
            outE = lerp(h, e, Transfer);
        }
    }
    Out[p] = float4(outE, 1);
}
#endif

#ifdef REDUCE
groupshared float gD[256], gO[256], gM[256];
groupshared uint gN[256];
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID)
{
    const uint ti = gt.y * 16 + gt.x;
    const uint2 base = gid.xy * 32 + gt.xy * 2;
    float sd = 0, so = 0, mo = 0; uint n = 0;
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x) {
            const uint2 p = base + uint2(x, y);
            if (all(p < SrcSize)) {
                const float3 o = Src.Load(int3(SrcBase + p, 0)).rgb;
                sd += Luma(Resolve(p, o));
                const float u = Luma(max(o, 0));
                so += u; mo = max(mo, u); ++n;
            }
        }
    gD[ti] = sd; gO[ti] = so; gM[ti] = mo; gN[ti] = n;
    GroupMemoryBarrierWithGroupSync();
    if (ti == 0) {
        float td = 0, to = 0, tm = 0; uint tn = 0;
        for (uint i = 0; i < 256; ++i) { td += gD[i]; to += gO[i]; tm = max(tm, gM[i]); tn += gN[i]; }
        const float cnt = max(float(tn), 1.0);
        Out[BlockBase + gid.xy] = float4(td / cnt, to / cnt, tm, 0);
    }
}
#endif

#ifdef COMPOSE
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (any(p >= SrcSize)) return;
    const float4 o = Src.Load(int3(SrcBase + p, 0));
    float3 r = Resolve(p, o.rgb);
    if (RestoreMode != 0) {
        // SDR black-level restore (RenoDX legacy_reduce/apply, rc11 dark gate): blocks whose brightest untouched pixel is
        // near black give back NR's average lift (at most 0.025)
        const int2 bmax = int2((SrcSize + 31) / 32) - 1;
        const float2 bp = (float2(p) + 0.5) / 32.0 - 0.5;
        const int2 b0 = clamp(int2(floor(bp)), 0, bmax), b1 = min(b0 + 1, bmax);
        const float2 f = saturate(bp - float2(b0));
        const float4 m = lerp(lerp(Blocks.Load(int3(BlockBase + b0, 0)), Blocks.Load(int3(BlockBase + int2(b1.x, b0.y), 0)), f.x),
                              lerp(Blocks.Load(int3(BlockBase + int2(b0.x, b1.y), 0)), Blocks.Load(int3(BlockBase + b1, 0)), f.x), f.y);
        const float w = RestoreMode == 2 && Gate > 0 ? 1.0 - smoothstep(0.5 * Gate, Gate, m.z) : (m.z <= Gate ? 1.0 : 0.0);
        const float ped = clamp(m.x - m.y, 0.0, 0.025) * w;
        if (ped > 0) r = max(r - ped, 0);
    }
    Out[SrcBase + p] = float4(r, o.a);
}
#endif
)HLSL";

        struct CsConst
        {
            std::uint32_t srcBase[2], srcSize[2], workBase[2], workSize[2], blockBase[2], pad0[2];
            float         transfer, color, gate;
            std::uint32_t restoreMode;
        };
        static_assert(sizeof(CsConst) % 16 == 0);

        // ---------------------------------------------------------------------------------------------------------------
        // RenoDX look stage (v8.5 look_stage.hpp + shaders/look_common/band/compose/upsample/trace), ported to D3D11 cs_5_0.
        // The math is RenoDX's line for line. D3D11 differences: the band surfaces a pass reads are SRVs (t3/t4) and the ones it
        // writes UAVs (u0/u1), the history ping-pong is a raw SRV in / raw UAV out, and every NR-grid read (P, N, MV) and the
        // look output are offset by WorkBase (the eye's origin in the NR textures; 0 per eye, e * width in the shared mode), the
        // reference by SrcBase (the eye's origin in the double-wide source). The stage reshapes NR's edit e = log2(n / p) and
        // writes N' = n * 2^(t * (e' - e)), which the resolve then consumes.
        constexpr const char* kLookSource = R"HLSL(
cbuffer L : register(b0)
{
    uint2 Size; uint2 NrSize;
    uint2 FullSize; uint Flags; uint Mode;
    float Strength; float Brighten; float Darken; float MaxBrighten;
    float MaxDarken; float ColourGain; float HueGain; float MaxColour;
    float Shadows; float Midtones; float Highlights; float Tone;
    float Detail; float Halo; uint Radius; float Alpha;
    float2 MotionScale; uint2 MotionGrid;
    float GfEps; float HaloBright; uint MinRadius; uint MidRadius;
    uint2 WorkBase; uint2 SrcBase;
};
Texture2D<float4> Reference : register(t0);
Texture2D<float4> Proxy     : register(t1);
Texture2D<float4> Neural    : register(t2);
Texture2D<float4> BandIn    : register(t3);
Texture2D<float2> BandMaxIn : register(t4);
Texture2D<float2> Motion    : register(t5);
ByteAddressBuffer HistoryIn : register(t6);
#if defined(LOOK_BAND)
RWTexture2D<float4> BandOut    : register(u0);
RWTexture2D<float2> BandMaxOut : register(u1);
#elif defined(LOOK_COMPOSE)
RWTexture2D<float4> LookOutput : register(u0);
RWByteAddressBuffer HistoryOut : register(u1);
#elif defined(LOOK_UPSAMPLE)
RWTexture2D<float4> UpProxy  : register(u0);
RWTexture2D<float4> UpNeural : register(u1);
#else
RWTexture2D<float4> TraceBlocks    : register(u0);
RWByteAddressBuffer TraceHistogram : register(u1);
#endif

static const uint kFlagBands = 1u << 0;
static const uint kFlagTemporal = 1u << 1;
static const uint kFlagMotion = 1u << 2;
static const uint kFlagTemporalDetail = 1u << 3;
static const uint kFlagTransport = 1u << 4;
static const uint kFlagRefLinear = 1u << 5;
static const uint kFlagHistoryValid = 1u << 6;
static const uint kFlagRangeConfidence = 1u << 7;
static const float kRatioFloor = 1.0 / 1024.0;
static const float3 kW = float3(0.212639, 0.715169, 0.072192);
static const float kLog2Grey = -2.4739312;
static const float kGuideFloor = 1e-8;
static const float kMidGain = 1.0;

float Luminance(float3 c) { return dot(c, kW); }
float3 SrgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 SrgbDecode(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float ProxyFloorTrust(float3 p) { return smoothstep(1.0 / 1024.0, 1.0 / 64.0, max(p.x, max(p.y, p.z))); }

struct Edit { float3 n; float3 lp; float3 e; float a; float3 c; float I; float t; };

Edit ReadEdit(int2 pixel)
{
    Edit x;
    const int3 at = int3(int2(WorkBase) + pixel, 0);
    const float3 p = SrgbDecode(Proxy.Load(at).rgb);
    x.n = SrgbDecode(Neural.Load(at).rgb);
    x.lp = log2(max(p, kRatioFloor));
    x.e = log2(max(x.n, kRatioFloor)) - x.lp;
    x.a = dot(kW, x.e);
    x.c = x.e - x.a;
    x.I = log2(max(dot(kW, p), kRatioFloor));
    x.t = ProxyFloorTrust(p);
    return x;
}

void ColourSplit(float3 c, float3 lp, out float3 c_r, out float3 c_t, out float s)
{
    const float3 o = lp - dot(kW, lp);
    const float oo = dot(kW, o * o);
    c_r = (dot(kW, c * o) / max(oo, 1e-6)) * o;
    c_t = c - c_r;
    s = smoothstep(0.05, 0.25, sqrt(oo));
}

#if defined(LOOK_COMPOSE)
float ZoneGain(float I)
{
    const float L = I - kLog2Grey;
    const float w_sh = 1.0 - smoothstep(-3.5, -1.5, L);
    const float w_hi = smoothstep(0.5, 2.5, L);
    return w_sh * Shadows + (1.0 - w_sh - w_hi) * Midtones + w_hi * Highlights;
}

float3 ShapeEdit(Edit x, float aB, float aD, float aM, float3 c, float m, float mn)
{
    const float wmax = smoothstep(1.0, 3.0, m - x.I);
    aD -= Halo * wmax * min(aD, 0.0);
    float mid_lift = 0.0;
    if (HaloBright > 0.0) {
        const float wmin = smoothstep(1.0, 3.0, x.I - mn);
        const float wD = max(wmax, wmin);
        aD -= min(HaloBright * wD, 1.0) * min(aD, 0.0);
        mid_lift = -min(HaloBright * kMidGain * wmin, 1.0) * min(aM, 0.0);
    }
    float a = Tone * aB + Detail * aD + Detail * mid_lift;
    a = Brighten * max(a, 0.0) + Darken * min(a, 0.0);
    float3 c_r, c_t;
    float s;
    ColourSplit(c, x.lp, c_r, c_t, s);
    c = s * (ColourGain * c_r + HueGain * c_t) + (1.0 - s) * ColourGain * c;
    const float g = Strength * ZoneGain(x.I);
    a *= g;
    c *= g;
    if (a > 0.0 && MaxBrighten > 0.0) a = MaxBrighten * tanh(min(a / MaxBrighten, 10.0));
    if (a < 0.0 && MaxDarken > 0.0) a = MaxDarken * tanh(max(a / MaxDarken, -10.0));
    if (MaxColour > 0.0) {
        const float norm = sqrt(dot(kW, c * c));
        if (norm > 1e-6) c *= MaxColour * tanh(min(norm / MaxColour, 10.0)) / norm;
    }
    return a + c;
}

struct BandSplit { float2 narrow; float2 wide; float peak; float trough; };

float4 Bilin4(int2 b0, int2 b1, float2 f)
{
    return BandIn.Load(int3(b0, 0)) * (1.0 - f.x) * (1.0 - f.y) + BandIn.Load(int3(b1.x, b0.y, 0)) * f.x * (1.0 - f.y) +
           BandIn.Load(int3(b0.x, b1.y, 0)) * (1.0 - f.x) * f.y + BandIn.Load(int3(b1, 0)) * f.x * f.y;
}

BandSplit BandCoefficients(int2 pixel)
{
    const uint2 half_size = (NrSize + 1u) / 2u;
    const float2 pos = clamp((float2(pixel) + 0.5) * 0.5 - 0.5, 0.0, float2(half_size - 1u));
    const int2 b0 = int2(floor(pos));
    const int2 b1 = min(b0 + 1, int2(half_size) - 1);
    const float2 f = pos - float2(b0);
    BandSplit s;
    const float4 q = Bilin4(b0, b1, f);
    s.narrow = q.zw;
    s.wide = q.xy;
    const float2 extrema = BandMaxIn.Load(int3(b0, 0)) * (1.0 - f.x) * (1.0 - f.y) + BandMaxIn.Load(int3(b1.x, b0.y, 0)) * f.x * (1.0 - f.y) +
                           BandMaxIn.Load(int3(b0.x, b1.y, 0)) * (1.0 - f.x) * f.y + BandMaxIn.Load(int3(b1, 0)) * f.x * f.y;
    s.peak = extrema.x;
    s.trough = -extrema.y;
    return s;
}

uint HistoryAddress(uint2 pixel) { return (pixel.y * NrSize.x + pixel.x) * 12u; }

float4 BandValues(int2 pixel, out Edit x, out float peak, out float trough, out float aM)
{
    x = ReadEdit(pixel);
    peak = x.I;
    trough = x.I;
    float aB = x.a;
    float aD = 0.0;
    aM = 0.0;
    if ((Flags & kFlagBands) != 0u) {
        const BandSplit bands = BandCoefficients(pixel);
        aB = bands.narrow.x * x.I + bands.narrow.y;
        aD = x.a - aB;
        aM = aB - (bands.wide.x * x.I + bands.wide.y);
        peak = bands.peak;
        trough = bands.trough;
    }
    return float4(aB, aD, x.c.r, x.c.b);
}

float4 LoadHistory(uint2 pixel, out float history_i, out float history_m)
{
    const uint3 words = HistoryIn.Load3(HistoryAddress(pixel));
    history_i = f16tof32(words.z);
    history_m = f16tof32(words.z >> 16);
    return float4(f16tof32(words.x), f16tof32(words.x >> 16), f16tof32(words.y), f16tof32(words.y >> 16));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatch_id.xy);
    if (any(dispatch_id.xy >= NrSize)) return;
    Edit x;
    float peak, trough, aM;
    float4 current = BandValues(pixel, x, peak, trough, aM);
    float3 c = x.c;
    if ((Flags & kFlagTemporal) != 0u) {
        float4 filtered = current;
        float filtered_m = aM;
        if ((Flags & kFlagHistoryValid) != 0u) {
            // previous = current + MV
            float2 previous = float2(pixel);
            if ((Flags & kFlagMotion) != 0u) {
                const uint2 texel = WorkBase + min(uint2((float2(pixel) + 0.5) * float2(MotionGrid) / float2(NrSize)), MotionGrid - 1u);
                previous += Motion.Load(int3(texel, 0)).xy * MotionScale;
            }
            const float2 limit = float2(NrSize) - 0.5;
            const bool onscreen = all(previous >= -0.5) && all(previous <= limit);
            const float2 pos = clamp(previous, 0.0, float2(NrSize - 1u));
            const uint2 h0 = uint2(floor(pos));
            const uint2 h1 = min(h0 + 1u, NrSize - 1u);
            const float2 f = pos - float2(h0);
            float i00, i10, i01, i11, m00, m10, m01, m11;
            const float4 q00 = LoadHistory(h0, i00, m00);
            const float4 q10 = LoadHistory(uint2(h1.x, h0.y), i10, m10);
            const float4 q01 = LoadHistory(uint2(h0.x, h1.y), i01, m01);
            const float4 q11 = LoadHistory(h1, i11, m11);
            const float4 history = q00 * (1.0 - f.x) * (1.0 - f.y) + q10 * f.x * (1.0 - f.y) + q01 * (1.0 - f.x) * f.y + q11 * f.x * f.y;
            const float history_i = i00 * (1.0 - f.x) * (1.0 - f.y) + i10 * f.x * (1.0 - f.y) + i01 * (1.0 - f.x) * f.y + i11 * f.x * f.y;
            const float history_m = m00 * (1.0 - f.x) * (1.0 - f.y) + m10 * f.x * (1.0 - f.y) + m01 * (1.0 - f.x) * f.y + m11 * f.x * f.y;
            float4 lo = current, hi = current;
            float lo_m = aM, hi_m = aM, i_lo = x.I, i_hi = x.I;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int2 neighbour = clamp(pixel + int2(dx, dy), 0, int2(NrSize) - 1);
                    Edit around;
                    float unused_peak, unused_trough, around_m;
                    const float4 value = BandValues(neighbour, around, unused_peak, unused_trough, around_m);
                    lo = min(lo, value);
                    hi = max(hi, value);
                    lo_m = min(lo_m, around_m);
                    hi_m = max(hi_m, around_m);
                    i_lo = min(i_lo, around.I);
                    i_hi = max(i_hi, around.I);
                }
            }
            const float input_change = (Flags & kFlagRangeConfidence) != 0u ? max(max(i_lo - history_i, history_i - i_hi), 0.0)
                                                                              : abs(x.I - history_i);
            const float confidence = onscreen ? 1.0 - smoothstep(0.1, 0.3, input_change) : 0.0;
            const float beta = max(Alpha, 1.0 - confidence);
            const float4 clipped = clamp(history, lo, hi);
            filtered = clipped + beta * (current - clipped);
            const float clipped_m = clamp(history_m, lo_m, hi_m);
            filtered_m = clipped_m + beta * (aM - clipped_m);
            if ((Flags & kFlagTemporalDetail) == 0u) {
                filtered.y = current.y;
                filtered_m = aM;
            }
        }
        const uint3 words = uint3(f32tof16(filtered.x) | (f32tof16(filtered.y) << 16), f32tof16(filtered.z) | (f32tof16(filtered.w) << 16),
                                  f32tof16(x.I) | (f32tof16(filtered_m) << 16));
        HistoryOut.Store3(HistoryAddress(uint2(pixel)), words);
        current = filtered;
        aM = filtered_m;
        c = float3(current.z, -(kW.x * current.z + kW.z * current.w) / kW.y, current.w);
    }
    const float3 shaped = ShapeEdit(x, current.x, current.y, aM, c, peak, trough);
    float guide = 1.0;
    if ((Flags & kFlagTransport) != 0u) {
        const float2 scale = float2(FullSize) / float2(NrSize);
        const float2 footprint_min = float2(pixel) * scale;
        const float2 footprint_max = min(footprint_min + scale, float2(FullSize));
        const int2 first = int2(floor(footprint_min));
        const int2 last = int2(ceil(footprint_max));
        float3 sum = 0.0;
        float weight = 0.0;
        for (int y = first.y; y < last.y; ++y) {
            for (int xx = first.x; xx < last.x; ++xx) {
                const float2 overlap = min(float2(xx + 1, y + 1), footprint_max) - max(float2(xx, y), footprint_min);
                const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
                const float3 texel = Reference.Load(int3(int2(SrcBase) + int2(xx, y), 0)).rgb;
                sum += max((Flags & kFlagRefLinear) != 0u ? texel : SrgbDecode(texel), 0.0) * w;
                weight += w;
            }
        }
        guide = log2(max(Luminance(sum / max(weight, 1e-12)), kGuideFloor));
    }
    LookOutput[int2(WorkBase) + pixel] = float4(SrgbEncode(x.n * exp2(x.t * (shaped - x.e))), guide);
}
#endif

#if defined(LOOK_BAND)
static const float kHaloPenalty = 2.0;

float4 BoxMoments(int2 pixel, bool horizontal, uint box_radius)
{
    const int2 last = int2(Size) - 1;
    const int radius = int(box_radius);
    const int centre = horizontal ? pixel.x : pixel.y;
    const int lo = max(centre - radius, 0);
    const int hi = min(centre + radius, horizontal ? last.x : last.y);
    float4 sum = 0.0;
    for (int i = lo; i <= hi; ++i) sum += BandIn.Load(int3(horizontal ? int2(i, pixel.y) : int2(pixel.x, i), 0));
    return sum / float(hi - lo + 1);
}

float2 Dilate(int2 pixel, bool horizontal, uint dilate_radius)
{
    const int2 last = int2(Size) - 1;
    const int radius = int(dilate_radius);
    const float k = kHaloPenalty / float(radius * radius);
    const int centre = horizontal ? pixel.x : pixel.y;
    const int lo = max(centre - 2 * radius, 0);
    const int hi = min(centre + 2 * radius, horizontal ? last.x : last.y);
    float2 peak = -1e30;
    for (int i = lo; i <= hi; ++i) {
        const float d = float(i - centre);
        peak = max(peak, BandMaxIn.Load(int3(horizontal ? int2(i, pixel.y) : int2(pixel.x, i), 0)) - k * d * d);
    }
    return peak;
}

// five passes at half NR resolution (Mode): 0 down, 1 H, 2 V + coefficients, 3 H (narrow / wide), 4 V
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatch_id.xy);
    if (any(dispatch_id.xy >= Size)) return;
    if (Mode == 0u) {
        float4 moments = 0.0;
        float peak = -1e30, trough = 1e30, count = 0.0;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                const int2 source = pixel * 2 + int2(dx, dy);
                if (any(source >= int2(NrSize))) continue;
                const Edit x = ReadEdit(source);
                moments += float4(x.I, x.a, x.I * x.I, x.I * x.a);
                peak = max(peak, x.I);
                trough = min(trough, x.I);
                count += 1.0;
            }
        }
        BandOut[pixel] = moments / count;
        BandMaxOut[pixel] = float2(peak, -trough);
    } else if (Mode == 1u) {
        BandOut[pixel] = BoxMoments(pixel, true, Radius);
        const float2 dilated = Dilate(pixel, true, Radius);
        const float2 dilated_min = Dilate(pixel, true, MinRadius);
        BandMaxOut[pixel] = float2(dilated.x, dilated_min.y);
    } else if (Mode == 2u) {
        const float4 m = BoxMoments(pixel, false, Radius);
        const float variance = max(m.z - m.x * m.x, 0.0);
        const float covariance = m.w - m.x * m.y;
        const float slope = covariance / (variance + GfEps);
        BandOut[pixel] = float4(slope, m.y - slope * m.x, 0.0, 0.0);
        const float2 dilated = Dilate(pixel, false, Radius);
        const float2 dilated_min = Dilate(pixel, false, MinRadius);
        BandMaxOut[pixel] = float2(dilated.x, dilated_min.y);
    } else if (Mode == 3u) {
        BandOut[pixel] = float4(BoxMoments(pixel, true, Radius).xy, BoxMoments(pixel, true, MidRadius).xy);
    } else {
        BandOut[pixel] = float4(BoxMoments(pixel, false, MidRadius).zw, BoxMoments(pixel, false, Radius).xy);
    }
}
#endif

#if defined(LOOK_UPSAMPLE)
static const float kSpatialSigma = 0.5;
static const float kRangeSigma = 0.5;
static const int kTile = 20;
groupshared float4 tile_edit[kTile * kTile];
groupshared float3 tile_proxy[kTile * kTile];

uint TileIndex(int2 local)
{
    const int2 at = clamp(local, 0, kTile - 1);
    return uint(at.y * kTile + at.x);
}

// edge-aware transport (joint bilateral over the 4x4 NR pixels around each output pixel): P_up and N'_up at full resolution
[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex)
{
    const int2 last = int2(NrSize) - 1;
    const int2 origin = int2(floor((float2(group_id.xy * 16u) + 0.5) * float2(NrSize) / float2(Size) - 0.5)) - 1;
    for (uint i = group_index; i < uint(kTile * kTile); i += 256u) {
        const int2 tap = clamp(origin + int2(i % uint(kTile), i / uint(kTile)), 0, last);
        const float4 neural = Neural.Load(int3(int2(WorkBase) + tap, 0));
        const float3 proxy = SrgbDecode(Proxy.Load(int3(int2(WorkBase) + tap, 0)).rgb);
        tile_proxy[i] = proxy;
        tile_edit[i] = float4(log2(max(SrgbDecode(neural.rgb), kRatioFloor)) - log2(max(proxy, kRatioFloor)), neural.a);
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 pixel = int2(dispatch_id.xy);
    if (any(dispatch_id.xy >= Size)) return;
    const float2 position = (float2(pixel) + 0.5) * float2(NrSize) / float2(Size) - 0.5;
    const int2 base = int2(floor(position));
    const bool linear_reference = (Flags & kFlagRefLinear) != 0u;
    const float3 reference = Reference.Load(int3(int2(SrcBase) + pixel, 0)).rgb;
    const float3 original = linear_reference ? reference : SrgbDecode(reference);
    const float guide_full = log2(max(Luminance(max(original, 0.0)), kGuideFloor));
    float3 joint = 0.0, spatial = 0.0;
    float joint_weight = 0.0, spatial_weight = 0.0;
    for (int dy = -1; dy <= 2; ++dy) {
        for (int dx = -1; dx <= 2; ++dx) {
            const int2 tap = base + int2(dx, dy);
            const float2 distance = float2(tap) - position;
            const float ws = exp(-dot(distance, distance) / (2.0 * kSpatialSigma * kSpatialSigma));
            const float4 edit = tile_edit[TileIndex(tap - origin)];
            const float difference = guide_full - edit.a;
            const float wr = exp(-(difference * difference) / (2.0 * kRangeSigma * kRangeSigma));
            joint += ws * wr * edit.rgb;
            joint_weight += ws * wr;
            spatial += ws * edit.rgb;
            spatial_weight += ws;
        }
    }
    const float3 edit_up = joint_weight > 1e-6 ? joint / joint_weight : spatial / spatial_weight;
    const float2 clamped = clamp(position, 0.0, float2(last));
    const int2 b0 = int2(floor(clamped));
    const int2 b1 = min(b0 + 1, last);
    const float2 f = clamped - float2(b0);
    const float3 proxy_up = tile_proxy[TileIndex(b0 - origin)] * (1.0 - f.x) * (1.0 - f.y) +
                            tile_proxy[TileIndex(int2(b1.x, b0.y) - origin)] * f.x * (1.0 - f.y) +
                            tile_proxy[TileIndex(int2(b0.x, b1.y) - origin)] * (1.0 - f.x) * f.y +
                            tile_proxy[TileIndex(b1 - origin)] * f.x * f.y;
    const float3 gain = exp2(edit_up);
    const float3 neural_up = linear_reference ? max(proxy_up, kRatioFloor) * gain
                                              : (original >= kRatioFloor ? original * gain : original + kRatioFloor * (gain - 1.0));
    UpProxy[pixel] = float4(SrgbEncode(proxy_up), 1.0);
    UpNeural[pixel] = float4(SrgbEncode(neural_up), 1.0);
}
#endif

#if defined(LOOK_TRACE)
// NREditTrace (observe-only statistics of the RAW edit): Mode 0 clears, Mode 1 = one group per 32x32 NR block
static const uint kTraceBins = 128;
static const uint kTraceWords = kTraceBins + 4;
static const float kTraceLow = -4.0;
static const float kTraceStep = 1.0 / 16.0;
groupshared uint g_histogram[kTraceWords];
groupshared uint g_peak;
groupshared uint g_trough;
groupshared float4 g_sum[256];

uint OrderedFloat(float value)
{
    const uint bits = asuint(value);
    return (bits & 0x80000000u) != 0u ? ~bits : bits | 0x80000000u;
}
float UnorderedFloat(uint bits) { return asfloat((bits & 0x80000000u) != 0u ? bits & 0x7FFFFFFFu : ~bits); }

[numthreads(16, 16, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if (Mode == 0u) {
        if (index < kTraceWords) TraceHistogram.Store(index * 4u, 0u);
        return;
    }
    if (index < kTraceWords) g_histogram[index] = 0u;
    if (index == 0u) {
        g_peak = 0u;
        g_trough = 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 origin = int2(group.xy) * 32 + int2(thread.xy) * 2;
    float I[4], a[4];
    bool valid[4];
    float4 sum = 0.0;
    [unroll] for (uint k = 0; k < 4u; ++k) {
        const int2 pixel = origin + int2(k & 1u, k >> 1);
        valid[k] = all(pixel < int2(NrSize));
        I[k] = 0.0;
        a[k] = 0.0;
        if (valid[k]) {
            const Edit x = ReadEdit(pixel);
            float3 c_r, c_t;
            float s;
            ColourSplit(x.c, x.lp, c_r, c_t, s);
            I[k] = x.I;
            a[k] = x.a;
            const uint bin = uint(clamp(floor((x.a - kTraceLow) / kTraceStep), 0.0, float(kTraceBins - 1u)));
            InterlockedAdd(g_histogram[bin], 1u);
            InterlockedAdd(g_histogram[kTraceBins], 1u);
            if (abs(x.a) > 1.0) InterlockedAdd(g_histogram[kTraceBins + 1u], 1u);
            InterlockedMax(g_peak, OrderedFloat(x.I));
            InterlockedMin(g_trough, OrderedFloat(x.I));
            sum += float4(x.a, x.I, sqrt(dot(kW, c_r * c_r)), sqrt(dot(kW, c_t * c_t)));
        }
    }
    g_sum[index] = sum;
    GroupMemoryBarrierWithGroupSync();
    const float peak = UnorderedFloat(g_peak);
    const float trough = UnorderedFloat(g_trough);
    [unroll] for (uint j = 0; j < 4u; ++j) {
        if (valid[j] && peak - I[j] > 2.0 && a[j] < -0.1) InterlockedAdd(g_histogram[kTraceBins + 2u], 1u);
        if (valid[j] && I[j] - trough > 2.0 && a[j] < -0.1) InterlockedAdd(g_histogram[kTraceBins + 3u], 1u);
    }
    for (uint stride = 128u; stride > 0u; stride >>= 1) {
        if (index < stride) g_sum[index] += g_sum[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u) {
        const uint count = g_histogram[kTraceBins];
        TraceBlocks[group.xy] = count != 0u ? g_sum[0] / float(count) : 0.0;
    }
    if (index < kTraceWords && g_histogram[index] != 0u) {
        uint previous;
        TraceHistogram.InterlockedAdd(index * 4u, g_histogram[index], previous);
    }
}
#endif
)HLSL";

        constexpr std::uint32_t kLookBands = 1u << 0, kLookTemporal = 1u << 1, kLookMotion = 1u << 2, kLookTemporalDetail = 1u << 3,
                                kLookTransport = 1u << 4, kLookHistoryValid = 1u << 6, kLookRangeConfidence = 1u << 7;
        constexpr std::uint32_t kTraceWords = 132;  // 128 bins of a over [-4, 4) at 1/16 stop, count, |a| > 1, halo, halo2

        struct LookConst
        {
            std::uint32_t size[2], nrSize[2], fullSize[2], flags, mode;
            float strength, brighten, darken, maxBrighten, maxDarken, colour, hue, maxColour, shadows, midtones, highlights, tone, detail, halo;
            std::uint32_t radius;
            float alpha, motionScale[2];
            std::uint32_t motionGrid[2];
            float gfEps, haloBright;
            std::uint32_t minRadius, midRadius, workBase[2], srcBase[2];
        };
        static_assert(sizeof(LookConst) == 144);

        // look::Settings (look_stage.hpp), every default the identity
        struct LookSettings
        {
            float strength{ 1 }, brighten{ 1 }, darken{ 1 }, maxBrighten{ 0 }, maxDarken{ 0 }, colour{ 1 }, hue{ 1 }, maxColour{ 0 };
            float shadows{ 1 }, midtones{ 1 }, highlights{ 1 }, tone{ 1 }, detail{ 1 }, halo{ 0 }, haloBright{ 0 }, detailRadius{ 1 };
            std::uint32_t stabilize{ 0 };
            float stabMs{ 60 };
            bool stabDetail{ false };
            float stabAlpha{ 0 };  // detail stability: a per-frame weight on the whole edit (0 = the player's Stabilize)
            std::uint32_t upsample{ 0 };  // 0 Auto, 1 Edge-aware, 2 Classic, 3 Edge-aware (area input)
        };
        bool LookGainsNeutral(const LookSettings& s)
        {
            return s.strength == 1 && s.brighten == 1 && s.darken == 1 && s.maxBrighten <= 0 && s.maxDarken <= 0 && s.colour == 1 && s.hue == 1 &&
                   s.maxColour <= 0 && s.shadows == 1 && s.midtones == 1 && s.highlights == 1 && s.tone == 1 && s.detail == 1 && s.halo <= 0 &&
                   s.haloBright <= 0;
        }
        bool LookBandsWanted(const LookSettings& s)
        {
            return s.tone != 1 || s.detail != 1 || s.halo > 0 || s.haloBright > 0 || (s.stabilize != 0 && s.stabAlpha <= 0);
        }
        bool LookTransportWanted(const LookSettings& s, unsigned nw, unsigned nh, unsigned fw, unsigned fh)
        {
            return s.upsample != 2 && nw <= fw && nh <= fh && (nw != fw || nh != fh);
        }

        // ---------------------------------------------------------------------------------------------------------------
        // render-thread state
        ID3D11Device*            g_dev = nullptr;
        ID3D11Device1*           g_dev1 = nullptr;
        ID3D11Device5*           g_dev5 = nullptr;
        ID3D11DeviceContext*     g_ctx = nullptr;
        ID3D11DeviceContext1*    g_ctx1 = nullptr;
        ID3D11DeviceContext4*    g_ctx4 = nullptr;
        ID3DDeviceContextState*  g_ourState = nullptr;
        ID3D11ComputeShader*     g_csDepth = nullptr;
        ID3D11ComputeShader*     g_csPrep = nullptr;
        ID3D11ComputeShader*     g_csReduce = nullptr;
        ID3D11ComputeShader*     g_csCompose = nullptr;
        ID3D11ComputeShader*     g_csStabilize = nullptr;
        ID3D11ComputeShader*     g_csLookBand = nullptr;
        ID3D11ComputeShader*     g_csLookCompose = nullptr;
        ID3D11ComputeShader*     g_csLookUpsample = nullptr;
        ID3D11ComputeShader*     g_csLookTrace = nullptr;
        ID3D11Buffer*            g_lookCb = nullptr;
        ID3D11Buffer*            g_cb = nullptr;
        ID3D11SamplerState*      g_lin = nullptr;
        bool                     g_csFailed = false;
        bool                     g_lookProgramsFailed = true;  // until compiled

        std::uint64_t g_frame = 0, g_lastEvalFrame = 0;
        bool          g_ngxInit = false, g_ngxFailed = false, g_dlssFailed = false;
        std::string   g_ngxError;
        NVSDK_NGX_Parameter* g_caps = nullptr;
        int           g_nrAvailCap = -1;
        std::uint64_t g_firstSeenFrame = 0;

        struct Feat
        {
            NVSDK_NGX_Handle*    h{ nullptr };
            NVSDK_NGX_Parameter* p{ nullptr };
            bool                 reset{ true };
        };
        Feat g_feat[2];
        int  g_featCount = 0;
        struct FeatKey
        {
            unsigned w{ 0 }, h{ 0 };
            int      flags{ 0 }, preset{ 0 }, quality{ 0 };
            bool     perEye{ true };
            int      rebuild{ 0 };
            bool operator==(const FeatKey&) const = default;
        };
        FeatKey g_featKey{};
        int     g_rebuildSerial = 0;
        bool    g_warmupDone = false;
        bool    g_qualityFellBack = false;

        // DW-sized textures
        struct Dw
        {
            unsigned          w{ 0 }, h{ 0 };
            DXGI_FORMAT       fmt{ DXGI_FORMAT_UNKNOWN };
            ID3D11Texture2D*  out{ nullptr };  // DLSS output (UAV)
            ID3D11ShaderResourceView* outSrv{ nullptr };
            ID3D11Texture2D*  pp[2]{};          // NR results (ping-pong)
            ID3D11ShaderResourceView*  ppSrv[2]{};
            ID3D11UnorderedAccessView* ppUav[2]{};
            ID3D11Texture2D*  depth{ nullptr };  // R32F copy of the engine depth
            ID3D11ShaderResourceView*  depthSrv{ nullptr };
            ID3D11UnorderedAccessView* depthUav{ nullptr };
            ID3D11Texture2D*  colTyped{ nullptr };  // typed copy of a typeless colour input
            ID3D11ShaderResourceView* colTypedSrv{ nullptr };
        };
        Dw g_dw;

        // census of the TAA draw's resources (logged once per change)
        std::string g_census;

        // timers (D3D11)
        constexpr int kRing = 6;
        struct Timer
        {
            ID3D11Query* disjoint{ nullptr };
            ID3D11Query* ts[4]{};
            bool         pending{ false };
            bool         nr{ false };
        };
        Timer  g_timers[kRing];
        int    g_timerSlot = 0;
        bool   g_timerOpen = false;
        double g_sumTotal = 0, g_sumDlss = 0, g_sumNr = 0, g_sumNr12 = 0, g_sumNr12Eye[2]{};
        int    g_nTimed = 0, g_nNr12 = 0;
        double g_maxTotal = 0;
        std::uint64_t g_evalOk = 0, g_evalFail = 0, g_resets = 0, g_nrOk = 0, g_nrFail = 0, g_lastLogFrame = 0;

        // jitter sequence study (first 64 frames with jitter)
        std::vector<std::pair<float, float>> g_jitSeen;
        double g_lastJx = 0, g_lastJy = 0;
        bool g_jitLogged = false;

        // ---------------------------------------------------------------------------------------------------------------
        bool CompileCs(const char* a_define, ID3D11ComputeShader** a_out, const char* a_src = kCsSource)
        {
            const D3D_SHADER_MACRO defs[] = { { a_define, "1" }, { nullptr, nullptr } };
            ID3DBlob* code = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = ::D3DCompile(a_src, std::strlen(a_src), "fo4flatvr_dlss", defs, nullptr, "main", "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
            if (FAILED(hr) || !code) {
                REX::ERROR("dlss: compute shader {} failed to compile 0x{:08X}: {}", a_define, static_cast<std::uint32_t>(hr),
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "");
                SafeRelease(err);
                return false;
            }
            SafeRelease(err);
            const HRESULT h2 = g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, a_out);
            code->Release();
            return SUCCEEDED(h2);
        }

        bool EnsureD3D11(ID3D11DeviceContext* a_ctx)
        {
            if (g_ctx == a_ctx && g_ourState) return !g_csFailed;
            if (g_ctx) return false;  // a different context than the first one: not supported
            g_ctx = a_ctx;
            a_ctx->GetDevice(&g_dev);
            g_dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_dev1));
            g_dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_dev5));
            a_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));
            a_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_ctx4));
            if (!g_dev1 || !g_ctx1) {
                REX::ERROR("dlss: ID3D11Device1/ID3D11DeviceContext1 unavailable");
                g_csFailed = true;
                return false;
            }
            const D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel();
            D3D_FEATURE_LEVEL chosen{};
            if (FAILED(g_dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &g_ourState))) {
                REX::ERROR("dlss: CreateDeviceContextState failed");
                g_csFailed = true;
                return false;
            }
            bool ok = CompileCs("DEPTH", &g_csDepth) && CompileCs("PREP", &g_csPrep) && CompileCs("REDUCE", &g_csReduce) &&
                      CompileCs("COMPOSE", &g_csCompose) && CompileCs("STABILIZE", &g_csStabilize);
            D3D11_BUFFER_DESC bd{ sizeof(CsConst), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            ok = ok && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_cb));
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            ok = ok && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_lin));
            g_csFailed = !ok;
            // the look stage's programs (a failure turns the stage off, NR keeps the pre-look path)
            if (ok) {
                const auto t0 = std::chrono::steady_clock::now();
                const bool lok = CompileCs("LOOK_BAND", &g_csLookBand, kLookSource) && CompileCs("LOOK_COMPOSE", &g_csLookCompose, kLookSource) &&
                                 CompileCs("LOOK_UPSAMPLE", &g_csLookUpsample, kLookSource) && CompileCs("LOOK_TRACE", &g_csLookTrace, kLookSource);
                D3D11_BUFFER_DESC lbd{ sizeof(LookConst), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
                const bool cbok = lok && SUCCEEDED(g_dev->CreateBuffer(&lbd, nullptr, &g_lookCb));
                g_lookProgramsFailed = !cbok;
                REX::INFO("dlss: look stage programs {} ({} ms)", cbok ? "ready" : "FAILED (look stage off)",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            }
            REX::INFO("dlss: D3D11 helpers {} (feature level {:X}, context4 {})", ok ? "ready" : "FAILED", static_cast<int>(fl),
                g_ctx4 ? "yes" : "no");
            return ok;
        }

        // Runs a_fn with the immediate context in a clean state of its own (SwapDeviceContextState) and the stereo hooks
        // bypassed; the engine's whole state comes back afterwards.
        template <class F>
        void Isolated(F&& a_fn)
        {
            ID3DDeviceContextState* prev = nullptr;
            g_bypass.fetch_add(1, std::memory_order_relaxed);
            g_ctx1->SwapDeviceContextState(g_ourState, &prev);
            a_fn();
            g_ctx1->SwapDeviceContextState(prev, nullptr);
            SafeRelease(prev);
            g_bypass.fetch_sub(1, std::memory_order_relaxed);
        }

        void SetConst(const CsConst& k) { g_ctx->UpdateSubresource(g_cb, 0, nullptr, &k, 0, 0); }

        void BindCs(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srv,
            std::initializer_list<ID3D11UnorderedAccessView*> uav)
        {
            g_ctx->CSSetShader(cs, nullptr, 0);
            ID3D11ShaderResourceView* s[6]{};
            int i = 0;
            for (auto* v : srv) s[i++] = v;
            g_ctx->CSSetShaderResources(0, 6, s);
            ID3D11UnorderedAccessView* u[3]{};
            i = 0;
            for (auto* v : uav) u[i++] = v;
            g_ctx->CSSetUnorderedAccessViews(0, 3, u, nullptr);
            g_ctx->CSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->CSSetSamplers(0, 1, &g_lin);
        }
        void UnbindCs()
        {
            ID3D11ShaderResourceView* s[6]{};
            ID3D11UnorderedAccessView* u[3]{};
            g_ctx->CSSetShaderResources(0, 6, s);
            g_ctx->CSSetUnorderedAccessViews(0, 3, u, nullptr);
        }

        // ---------------------------------------------------------------------------------------------------------------
        // NGX D3D11 (phase 1)
        bool EnsureNgx()
        {
            if (g_ngxInit) return true;
            if (g_ngxFailed) return false;
            std::error_code ec;
            std::filesystem::create_directories(g_dataDir, ec);
            const bool haveDll = std::filesystem::exists(g_ngxDir + L"\\nvngx_dlss.dll", ec);
            REX::INFO("dlss: NGX init: feature path '{}' (nvngx_dlss.dll {}), data path '{}'", std::filesystem::path(g_ngxDir).string(),
                haveDll ? "present" : "MISSING", std::filesystem::path(g_dataDir).string());
            static const wchar_t* paths[1];
            paths[0] = g_ngxDir.c_str();
            static NVSDK_NGX_FeatureCommonInfo common{};
            common.PathListInfo.Path = paths;
            common.PathListInfo.Length = 1;
            common.LoggingInfo.LoggingCallback = &NgxLog;
            common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            common.LoggingInfo.DisableOtherLoggingSinks = false;
            NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
            Isolated([&] {
                r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "fo4flatvr-1", g_dataDir.c_str(), g_dev,
                    &common, NVSDK_NGX_Version_API);
            });
            REX::INFO("dlss: NVSDK_NGX_D3D11_Init_with_ProjectID -> {}", Res(r));
            if (NVSDK_NGX_FAILED(r)) {
                g_ngxFailed = true;
                g_ngxError = "init " + Res(r);
                return false;
            }
            r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
            int avail = 0, needDrv = 0, initRes = 0;
            unsigned major = 0, minor = 0;
            if (g_caps) {
                NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
                NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needDrv);
                NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initRes);
                NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
                NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
                int nr = -1;
                if (!NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(g_caps, "DLSSNR.Available", &nr))) g_nrAvailCap = nr;
            }
            REX::INFO("dlss: capabilities {}: SuperSampling.Available={} NeedsUpdatedDriver={} MinDriver={}.{} FeatureInitResult=0x{:X} "
                      "DLSSNR.Available={} (D3D11 query; NR runs on D3D12)",
                Res(r), avail, needDrv, major, minor, static_cast<std::uint32_t>(initRes), g_nrAvailCap);
            if (!avail || needDrv) {
                g_ngxFailed = true;
                g_ngxError = std::format("DLSS not available (Available={} NeedsUpdatedDriver={})", avail, needDrv);
                return false;
            }
            g_ngxInit = true;
            return true;
        }

        void ReleaseFeatures()
        {
            for (auto& f : g_feat) {
                if (f.h) NVSDK_NGX_D3D11_ReleaseFeature(f.h);
                if (f.p) NVSDK_NGX_D3D11_DestroyParameters(f.p);
                f = Feat{};
            }
            g_featCount = 0;
            g_featKey = FeatKey{};
        }

        int CreateFlags(const Cfg& c)
        {
            if (c.createFlags >= 0) return c.createFlags;
            int f = 0;
            if (c.autoExposure) f |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            if (c.jitter) f |= 0;  // motion vectors exclude the jitter (built from the unjittered rows): MVJittered stays off
            if (c.depthInverted == 2) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
            return f;
        }

        bool EnsureFeatures(const Cfg& c, unsigned a_w, unsigned a_h)
        {
            FeatKey k{ a_w, a_h, CreateFlags(c), kPresetValues[c.preset], g_qualityFellBack ? 0 : c.quality, c.perEye, g_rebuildSerial };
            if (g_featCount > 0 && k == g_featKey) return true;
            Isolated([&] { ReleaseFeatures(); });
            const int n = c.perEye ? 2 : 1;
            bool ok = true;
            Isolated([&] {
                for (int e = 0; e < n && ok; ++e) {
                    auto& f = g_feat[e];
                    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_AllocateParameters(&f.p);
                    if (NVSDK_NGX_FAILED(r) || !f.p) {
                        REX::ERROR("dlss: AllocateParameters {}", Res(r));
                        ok = false;
                        break;
                    }
                    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality }) {
                        NVSDK_NGX_Parameter_SetUI(f.p, key, static_cast<unsigned>(k.preset));
                    }
                    NVSDK_NGX_DLSS_Create_Params cp{};
                    cp.Feature.InWidth = a_w;
                    cp.Feature.InHeight = a_h;
                    cp.Feature.InTargetWidth = a_w;
                    cp.Feature.InTargetHeight = a_h;
                    cp.Feature.InPerfQualityValue = kQualityValues[k.quality];
                    cp.InFeatureCreateFlags = k.flags;
                    cp.InEnableOutputSubrects = true;
                    r = NGX_D3D11_CREATE_DLSS_EXT(g_ctx, &f.h, f.p, &cp);
                    if ((NVSDK_NGX_FAILED(r) || !f.h) && k.quality != 0) {
                        REX::WARN("dlss: create with quality {} at render = output size {}x{} -> {}; quality modes below DLAA need a lower render "
                                  "size (not part of this build): DLAA used", kQualityNames[k.quality], a_w, a_h, Res(r));
                        g_qualityFellBack = true;
                        k.quality = 0;
                        cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
                        r = NGX_D3D11_CREATE_DLSS_EXT(g_ctx, &f.h, f.p, &cp);
                    }
                    REX::INFO("dlss: feature {} of {} create {}x{} quality {} preset {} ({}) flags 0x{:X} -> {}", e, n, a_w, a_h,
                        kQualityNames[k.quality], k.preset, kPresetNames[c.preset], k.flags, Res(r));
                    if (NVSDK_NGX_FAILED(r) || !f.h) {
                        ok = false;
                        break;
                    }
                    f.reset = true;
                }
            });
            if (!ok) {
                ReleaseFeatures();
                return false;
            }
            g_featCount = n;
            g_featKey = k;
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------------
        void ReleaseDw()
        {
            SafeRelease(g_dw.outSrv);
            SafeRelease(g_dw.out);
            for (int i = 0; i < 2; ++i) {
                SafeRelease(g_dw.ppSrv[i]);
                SafeRelease(g_dw.ppUav[i]);
                SafeRelease(g_dw.pp[i]);
            }
            SafeRelease(g_dw.depthSrv);
            SafeRelease(g_dw.depthUav);
            SafeRelease(g_dw.depth);
            SafeRelease(g_dw.colTypedSrv);
            SafeRelease(g_dw.colTyped);
            g_dw = Dw{};
        }

        bool MakeTex(unsigned w, unsigned h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t, ID3D11ShaderResourceView** srv,
            ID3D11UnorderedAccessView** uav, UINT misc = 0)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = f;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = bind;
            d.MiscFlags = misc;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t))) return false;
            if (srv && FAILED(g_dev->CreateShaderResourceView(*t, nullptr, srv))) return false;
            if (uav && FAILED(g_dev->CreateUnorderedAccessView(*t, nullptr, uav))) return false;
            return true;
        }

        bool EnsureDw(unsigned w, unsigned h, DXGI_FORMAT outFmt, bool needPP)
        {
            if (g_dw.out && g_dw.w == w && g_dw.h == h && g_dw.fmt == outFmt && (!needPP || g_dw.pp[0])) return true;
            ReleaseDw();
            bool ok = MakeTex(w, h, outFmt, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &g_dw.out, &g_dw.outSrv, nullptr) &&
                      MakeTex(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &g_dw.depth,
                          &g_dw.depthSrv, &g_dw.depthUav);
            if (ok && needPP) {
                for (int i = 0; i < 2 && ok; ++i) {
                    ok = MakeTex(w, h, outFmt, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &g_dw.pp[i], &g_dw.ppSrv[i],
                        &g_dw.ppUav[i]);
                }
            }
            if (!ok) {
                REX::ERROR("dlss: double-wide textures {}x{} format {} failed", w, h, static_cast<int>(outFmt));
                ReleaseDw();
                return false;
            }
            g_dw.w = w;
            g_dw.h = h;
            g_dw.fmt = outFmt;
            REX::INFO("dlss: double-wide textures {}x{} format {}{}", w, h, static_cast<int>(outFmt), needPP ? " (+ NR results)" : "");
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------------
        // timers
        void TimerMark(int a_i)
        {
            if (!g_timerOpen) return;
            g_ctx->End(g_timers[g_timerSlot].ts[a_i]);
        }
        void TimerBegin(bool a_on)
        {
            g_timerOpen = false;
            if (!a_on) return;
            auto& t = g_timers[g_timerSlot];
            if (!t.disjoint) {
                D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                if (FAILED(g_dev->CreateQuery(&qd, &t.disjoint))) return;
                qd.Query = D3D11_QUERY_TIMESTAMP;
                for (auto*& q : t.ts) {
                    if (FAILED(g_dev->CreateQuery(&qd, &q))) return;
                }
            }
            if (t.pending) return;  // not read back yet: skip this frame's timing
            g_ctx->Begin(t.disjoint);
            g_timerOpen = true;
            TimerMark(0);
        }
        void TimerEnd(bool a_nr)
        {
            if (!g_timerOpen) return;
            auto& t = g_timers[g_timerSlot];
            TimerMark(3);
            g_ctx->End(t.disjoint);
            t.pending = true;
            t.nr = a_nr;
            g_timerOpen = false;
            g_timerSlot = (g_timerSlot + 1) % kRing;
        }
        void TimerCollect()
        {
            if (!g_ctx) return;
            for (auto& t : g_timers) {
                if (!t.pending) continue;
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                if (g_ctx->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
                std::uint64_t v[4]{};
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) ok = g_ctx->GetData(t.ts[i], &v[i], sizeof(v[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (!ok) continue;
                t.pending = false;
                if (dj.Disjoint || !dj.Frequency) continue;
                const double f = 1000.0 / static_cast<double>(dj.Frequency);
                const double total = (v[3] - v[0]) * f, dl = (v[2] - v[1]) * f, nr = (v[3] - v[2]) * f;
                if (total < 0 || total > 1000) continue;
                g_sumTotal += total;
                g_sumDlss += dl;
                g_sumNr += nr;
                g_maxTotal = (std::max)(g_maxTotal, total);
                ++g_nTimed;
            }
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Phase 2: the neural pass (feature 18) on a private D3D12 device
        using DirectInitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
            const NVSDK_NGX_Parameter*);
        using DirectCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
            NVSDK_NGX_Handle**);
        using DirectEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
            const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
        using DirectReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
        using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
        constexpr unsigned long long kNrApplicationId = 0x876232cULL;  // RenoDX's direct-runtime application id
        constexpr NVSDK_NGX_Feature  kFeatureNr = static_cast<NVSDK_NGX_Feature>(18);

        struct Shared
        {
            ID3D11Texture2D*           t11{ nullptr };
            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            ID3D12Resource*            t12{ nullptr };
        };
        struct NrFeat
        {
            NVSDK_NGX_Handle*    h{ nullptr };
            NVSDK_NGX_Parameter* p{ nullptr };
            bool                 viaCore{ false };
            bool                 reset{ true };
            std::array<float, 8> model{};  // last steering sent (a change restarts the history)
        };
        struct Nr
        {
            bool failed{ false }, ready{ false };
            std::string error;
            HMODULE runtime{ nullptr };
            DirectInitFn init{ nullptr };
            DirectCreateFn create{ nullptr };
            DirectEvaluateFn evaluate{ nullptr };
            DirectReleaseFn release{ nullptr };
            void** gmfnIat{ nullptr };
            ID3D12Device* dev{ nullptr };
            ID3D12CommandQueue* queue{ nullptr };
            static constexpr int kAlloc = 16;
            ID3D12CommandAllocator* alloc[kAlloc]{};
            std::uint64_t allocFence[kAlloc]{};
            int allocNext{ 0 };
            ID3D12GraphicsCommandList* list{ nullptr };
            ID3D12Fence* fence12{ nullptr };
            ID3D11Fence* fence11{ nullptr };
            std::uint64_t fenceValue{ 0 };
            HANDLE event{ nullptr };
            bool ngx12{ false };
            // timers
            ID3D12QueryHeap* qheap{ nullptr };
            ID3D12Resource* qread{ nullptr };
            std::uint64_t qfreq{ 0 };
            static constexpr int kQSlots = 16;
            std::uint64_t qFence[kQSlots]{};
            bool qPending[kQSlots]{};
            int  qEyes[kQSlots]{};
            int qNext{ 0 };
            // resources: [eye] (shared mode: index 0 holds both eyes side by side)
            Shared proxy[2], neural[2], mv[2], depth[2];
            Shared hist[2][2];  // [texture][ping-pong]: detail-stability filtered edit (D3D11 only)
            int histCur{ 0 };
            bool histValid{ false };
            ID3D11Texture2D* blocks{ nullptr };
            ID3D11ShaderResourceView* blocksSrv{ nullptr };
            ID3D11UnorderedAccessView* blocksUav{ nullptr };
            unsigned workW{ 0 }, workH{ 0 };  // per eye
            bool perEye{ true };
            NrFeat feat[4][2];
            int featPreset{ -1 }, featFlags{ -1 };
            unsigned featW{ 0 }, featH{ 0 };
            bool featPerEye{ true };
        };
        Nr g_nr;
        bool g_nrToggledOff = false;  // the NR toggle key (not saved)
        HMODULE g_self = nullptr;
        GetModuleFileNameWFn g_realGmfn = nullptr;

        DWORD WINAPI CallerGetModuleFileNameW(HMODULE a_module, LPWSTR a_name, DWORD a_size)
        {
            // The signed NR runtime checks that its caller is NGX: our module answers as nvngx.dll (RenoDX's caller identity).
            if (a_module == g_self && a_name && a_size) {
                constexpr wchar_t kId[] = L"nvngx.dll";
                if (std::size(kId) > a_size) {
                    a_name[0] = 0;
                    ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    return a_size;
                }
                std::memcpy(a_name, kId, sizeof(kId));
                return static_cast<DWORD>(std::size(kId) - 1);
            }
            return g_realGmfn ? g_realGmfn(a_module, a_name, a_size) : 0;
        }

        void** FindImport(HMODULE a_mod, const char* a_name)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(a_mod);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return nullptr;
            for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
                auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++addrs) {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                    auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), a_name) == 0) return reinterpret_cast<void**>(&addrs->u1.Function);
                }
            }
            return nullptr;
        }

        bool NrFail(const std::string& a_why)
        {
            g_nr.failed = true;
            g_nr.error = a_why;
            REX::ERROR("dlss: neural rendering off for this session: {}", a_why);
            return false;
        }

        // the 166 MB runtime: loaded on a worker at plugin load (LoadLibrary only), adopted on the render thread
        std::atomic<HMODULE> g_preloaded{ nullptr };
        std::atomic<bool>    g_preloadDone{ false };

        bool EnsureNrDevice()
        {
            if (g_nr.ready) return true;
            if (g_nr.failed) return false;
            if (!g_preloadDone.load()) return false;  // still loading: try again next frame
            if (!g_ctx4 || !g_dev5) return NrFail("ID3D11DeviceContext4 / ID3D11Device5 unavailable (shared fence)");
            g_nr.runtime = g_preloaded.load();
            if (!g_nr.runtime) return NrFail(std::format("nvngx_dlssnr.dll not loaded from {}", std::filesystem::path(g_ngxDir).string()));
            g_nr.init = reinterpret_cast<DirectInitFn>(::GetProcAddress(g_nr.runtime, "NVSDK_NGX_D3D12_Init_Ext"));
            g_nr.create = reinterpret_cast<DirectCreateFn>(::GetProcAddress(g_nr.runtime, "NVSDK_NGX_D3D12_CreateFeature"));
            g_nr.evaluate = reinterpret_cast<DirectEvaluateFn>(::GetProcAddress(g_nr.runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
            g_nr.release = reinterpret_cast<DirectReleaseFn>(::GetProcAddress(g_nr.runtime, "NVSDK_NGX_D3D12_ReleaseFeature"));
            if (!g_nr.init || !g_nr.create || !g_nr.evaluate || !g_nr.release) return NrFail("nvngx_dlssnr.dll lacks a D3D12 export");
            // caller identity (RenoDX InstallCallerIdentity): the runtime's GetModuleFileNameW import
            g_self = SelfModule();
            g_nr.gmfnIat = FindImport(g_nr.runtime, "GetModuleFileNameW");
            if (g_nr.gmfnIat) {
                DWORD old = 0;
                if (::VirtualProtect(g_nr.gmfnIat, sizeof(void*), PAGE_READWRITE, &old)) {
                    g_realGmfn = reinterpret_cast<GetModuleFileNameWFn>(*g_nr.gmfnIat);
                    *g_nr.gmfnIat = reinterpret_cast<void*>(&CallerGetModuleFileNameW);
                    ::VirtualProtect(g_nr.gmfnIat, sizeof(void*), old, &old);
                }
            }
            // private D3D12 device on the game's adapter
            IDXGIDevice* dxgiDev = nullptr;
            IDXGIAdapter* ad = nullptr;
            g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
            if (dxgiDev) dxgiDev->GetAdapter(&ad);
            SafeRelease(dxgiDev);
            HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
            auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(::GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
            HRESULT hr = create ? create(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g_nr.dev)) : E_FAIL;
            DXGI_ADAPTER_DESC add{};
            if (ad) ad->GetDesc(&add);
            SafeRelease(ad);
            if (FAILED(hr) || !g_nr.dev) return NrFail(std::format("D3D12CreateDevice 0x{:08X}", static_cast<std::uint32_t>(hr)));
            g_nr.dev->SetName(L"fo4flatvr NR device");
            REX::INFO("dlss: NR D3D12 device on '{}' (LUID {:08X}:{:08X})", std::filesystem::path(add.Description).string(),
                static_cast<std::uint32_t>(add.AdapterLuid.HighPart), add.AdapterLuid.LowPart);
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(g_nr.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_nr.queue))))
                return NrFail("CreateCommandQueue");
            for (auto*& a : g_nr.alloc) {
                if (FAILED(g_nr.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                        reinterpret_cast<void**>(&a))))
                    return NrFail("CreateCommandAllocator");
            }
            if (FAILED(g_nr.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_nr.alloc[0], nullptr,
                    __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g_nr.list))))
                return NrFail("CreateCommandList");
            g_nr.list->Close();
            HANDLE fh = nullptr;
            hr = g_nr.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_nr.fence12));
            if (SUCCEEDED(hr)) hr = g_nr.dev->CreateSharedHandle(g_nr.fence12, nullptr, GENERIC_ALL, nullptr, &fh);
            if (SUCCEEDED(hr)) hr = g_dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_nr.fence11));
            if (fh) ::CloseHandle(fh);
            if (FAILED(hr) || !g_nr.fence11) return NrFail(std::format("shared fence 0x{:08X}", static_cast<std::uint32_t>(hr)));
            g_nr.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            // timestamps
            D3D12_QUERY_HEAP_DESC qhd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Nr::kQSlots * 4, 0 };
            if (SUCCEEDED(g_nr.dev->CreateQueryHeap(&qhd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&g_nr.qheap)))) {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = Nr::kQSlots * 4 * sizeof(std::uint64_t);
                rd.Height = 1;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                g_nr.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&g_nr.qread));
                g_nr.queue->GetTimestampFrequency(&g_nr.qfreq);
            }
            // NGX on the private device (the core: parameter blocks; feature 18 is created through it when it can)
            static const wchar_t* paths[1];
            paths[0] = g_ngxDir.c_str();
            static NVSDK_NGX_FeatureCommonInfo common{};
            common.PathListInfo.Path = paths;
            common.PathListInfo.Length = 1;
            common.LoggingInfo.LoggingCallback = &NgxLog;
            common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "fo4flatvr-1",
                g_dataDir.c_str(), g_nr.dev, &common, NVSDK_NGX_Version_API);
            REX::INFO("dlss: NR: NVSDK_NGX_D3D12_Init_with_ProjectID -> {}", Res(r));
            if (NVSDK_NGX_FAILED(r)) return NrFail("NGX D3D12 init " + Res(r));
            g_nr.ngx12 = true;
            NVSDK_NGX_Parameter* caps = nullptr;
            if (!NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
                int a = -1, nd = -1, ir = 0;
                NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.Available", &a);
                NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.NeedsUpdatedDriver", &nd);
                NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.FeatureInitResult", &ir);
                REX::INFO("dlss: NR capability (D3D12 core): DLSSNR.Available={} NeedsUpdatedDriver={} FeatureInitResult=0x{:X} "
                          "(the query is not a gate: RenoDX notes it reports NotImplemented while creation works)",
                    a, nd, static_cast<std::uint32_t>(ir));
            }
            // the signed runtime itself
            r = g_nr.init(kNrApplicationId, g_dataDir.c_str(), g_nr.dev, NVSDK_NGX_Version_API, nullptr);
            REX::INFO("dlss: NR: nvngx_dlssnr.dll NVSDK_NGX_D3D12_Init_Ext -> {} (caller identity patch {})", Res(r),
                g_nr.gmfnIat ? "on" : "not found");
            if (NVSDK_NGX_FAILED(r)) return NrFail("nvngx_dlssnr.dll Init_Ext " + Res(r));
            g_nr.ready = true;
            return true;
        }

        void NrWaitIdle()
        {
            if (!g_nr.queue || !g_nr.fence12) return;
            const std::uint64_t v = ++g_nr.fenceValue;
            g_nr.queue->Signal(g_nr.fence12, v);
            if (g_nr.fence12->GetCompletedValue() < v) {
                g_nr.fence12->SetEventOnCompletion(v, g_nr.event);
                ::WaitForSingleObject(g_nr.event, 2000);
            }
        }

        void ReleaseShared(Shared& s)
        {
            SafeRelease(s.srv);
            SafeRelease(s.uav);
            SafeRelease(s.t11);
            SafeRelease(s.t12);
        }

        void ReleaseNrFeatures()
        {
            NrWaitIdle();
            for (auto& pass : g_nr.feat) {
                for (auto& f : pass) {
                    if (f.h) {
                        if (f.viaCore) NVSDK_NGX_D3D12_ReleaseFeature(f.h);
                        else g_nr.release(f.h);
                    }
                    if (f.p) NVSDK_NGX_D3D12_DestroyParameters(f.p);
                    f = NrFeat{};
                }
            }
            g_nr.featPreset = -1;
        }

        bool MakeShared(Shared& s, unsigned w, unsigned h, DXGI_FORMAT f, bool uav)
        {
            if (!MakeTex(w, h, f, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &s.t11, &s.srv, &s.uav,
                    D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED))
                return false;
            (void)uav;
            IDXGIResource1* dr = nullptr;
            HANDLE hnd = nullptr;
            HRESULT hr = s.t11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dr));
            if (SUCCEEDED(hr)) hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd);
            SafeRelease(dr);
            if (SUCCEEDED(hr)) hr = g_nr.dev->OpenSharedHandle(hnd, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&s.t12));
            if (hnd) ::CloseHandle(hnd);
            if (FAILED(hr)) {
                REX::ERROR("dlss: NR shared texture {}x{} fmt {} failed 0x{:08X}", w, h, static_cast<int>(f), static_cast<std::uint32_t>(hr));
                return false;
            }
            return true;
        }

        bool EnsureNrResources(unsigned a_eyeW, unsigned a_h, const Cfg& c)
        {
            const float s = c.nrFollowInputRes ? 1.0f : c.nrScale;
            const unsigned ww = (std::max)(64u, static_cast<unsigned>(std::lround(a_eyeW * s)));
            const unsigned wh = (std::max)(64u, static_cast<unsigned>(std::lround(a_h * s)));
            if (g_nr.proxy[0].t11 && g_nr.workW == ww && g_nr.workH == wh && g_nr.perEye == c.nrPerEye) return true;
            ReleaseNrFeatures();
            for (int e = 0; e < 2; ++e) {
                ReleaseShared(g_nr.proxy[e]);
                ReleaseShared(g_nr.neural[e]);
                ReleaseShared(g_nr.mv[e]);
                ReleaseShared(g_nr.depth[e]);
            }
            SafeRelease(g_nr.blocksSrv);
            SafeRelease(g_nr.blocksUav);
            SafeRelease(g_nr.blocks);
            for (auto& hh : g_nr.hist)
                for (auto& h : hh) ReleaseShared(h);
            g_nr.histValid = false;
            const int n = c.nrPerEye ? 2 : 1;
            const unsigned tw = c.nrPerEye ? ww : ww * 2;
            bool ok = true;
            for (int e = 0; e < n && ok; ++e) {
                ok = MakeShared(g_nr.proxy[e], tw, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, false) &&
                     MakeShared(g_nr.neural[e], tw, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
                     MakeShared(g_nr.mv[e], tw, wh, DXGI_FORMAT_R16G16_FLOAT, false) &&
                     MakeShared(g_nr.depth[e], tw, wh, DXGI_FORMAT_R32_FLOAT, false);
            }
            for (int e = 0; e < n && ok; ++e)
                for (int k = 0; k < 2 && ok; ++k)
                    ok = MakeTex(tw, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        &g_nr.hist[e][k].t11, &g_nr.hist[e][k].srv, &g_nr.hist[e][k].uav);
            const unsigned bw = (a_eyeW + 31) / 32, bh = (a_h + 31) / 32;
            ok = ok && MakeTex(bw * 2, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_nr.blocks,
                           &g_nr.blocksSrv, &g_nr.blocksUav);
            if (!ok) return NrFail("shared NR textures");
            g_nr.workW = ww;
            g_nr.workH = wh;
            g_nr.perEye = c.nrPerEye;
            REX::INFO("dlss: NR working size {}x{} per eye (scale {:.2f}), {} feature(s) per pass", ww, wh, s, n);
            return true;
        }

        void SetNrDims(NVSDK_NGX_Parameter* p, unsigned w, unsigned h)
        {
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, w);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, h);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_OutWidth, w);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_OutHeight, h);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Width", w);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Height", h);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.InputWidth", w);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.InputHeight", h);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputWidth", w);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputHeight", h);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Output.Width", w);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Output.Height", h);
            NVSDK_NGX_Parameter_SetF(p, "DLSSNR.ScalingRatio", 1.0f);
            NVSDK_NGX_Parameter_SetF(p, "DLSSNR.Scale", 1.0f);
        }

        NVSDK_NGX_Result NVSDK_CONV ScalingRatioCallback(NVSDK_NGX_Parameter* a_p)
        {
            if (!a_p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
            NVSDK_NGX_Parameter_SetF(a_p, "DLSSNR.ScalingRatio", 1.0f);
            return NVSDK_NGX_Result_Success;
        }

        int NrFlags(const Cfg& c)
        {
            int f = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            if (c.nrDepthMode == 2) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
            return f;
        }

        ID3D12CommandAllocator* NextAllocator(std::uint64_t a_signalValue)
        {
            const int i = g_nr.allocNext;
            g_nr.allocNext = (i + 1) % Nr::kAlloc;
            if (g_nr.fence12->GetCompletedValue() < g_nr.allocFence[i]) {
                g_nr.fence12->SetEventOnCompletion(g_nr.allocFence[i], g_nr.event);
                ::WaitForSingleObject(g_nr.event, 2000);
            }
            g_nr.allocFence[i] = a_signalValue;
            g_nr.alloc[i]->Reset();
            return g_nr.alloc[i];
        }

        bool EnsureNrFeatures(const Cfg& c, int a_passes)
        {
            const int n = c.nrPerEye ? 2 : 1;
            const unsigned fw = c.nrPerEye ? g_nr.workW : g_nr.workW * 2, fh = g_nr.workH;
            if (g_nr.featPreset != c.nrPreset || g_nr.featFlags != NrFlags(c) || g_nr.featW != fw || g_nr.featH != fh ||
                g_nr.featPerEye != c.nrPerEye) {
                ReleaseNrFeatures();
                g_nr.featPreset = c.nrPreset;
                g_nr.featFlags = NrFlags(c);
                g_nr.featW = fw;
                g_nr.featH = fh;
                g_nr.featPerEye = c.nrPerEye;
            }
            bool need = false;
            for (int p = 0; p < a_passes; ++p)
                for (int e = 0; e < n; ++e) need |= g_nr.feat[p][e].h == nullptr;
            if (!need) return true;
            const std::uint64_t sig = ++g_nr.fenceValue;
            ID3D12CommandAllocator* al = NextAllocator(sig);
            g_nr.list->Reset(al, nullptr);
            bool ok = true;
            for (int p = 0; p < a_passes && ok; ++p) {
                for (int e = 0; e < n && ok; ++e) {
                    auto& f = g_nr.feat[p][e];
                    if (f.h) continue;
                    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&f.p);
                    if (NVSDK_NGX_FAILED(r) || !f.p) {
                        ok = NrFail("NR AllocateParameters " + Res(r));
                        break;
                    }
                    SetNrDims(f.p, fw, fh);
                    NVSDK_NGX_Parameter_SetVoidPointer(f.p, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatioCallback));
                    NVSDK_NGX_Parameter_SetUI(f.p, "DLSSNR.Hint.Render.Preset", static_cast<unsigned>(c.nrPreset));
                    NVSDK_NGX_Parameter_SetI(f.p, NVSDK_NGX_Parameter_PerfQualityValue, 0);
                    NVSDK_NGX_Parameter_SetI(f.p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NrFlags(c));
                    NVSDK_NGX_Parameter_SetUI(f.p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
                    NVSDK_NGX_Parameter_SetUI(f.p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
                    r = NVSDK_NGX_D3D12_CreateFeature(g_nr.list, kFeatureNr, f.p, &f.h);
                    f.viaCore = !NVSDK_NGX_FAILED(r) && f.h;
                    NVSDK_NGX_Result r2 = r;
                    if (!f.viaCore) {
                        f.h = nullptr;
                        r2 = g_nr.create(g_nr.list, kFeatureNr, f.p, &f.h);
                    }
                    REX::INFO("dlss: NR feature 18 pass {} eye {} {}x{} preset {}: via NGX core {}{}", p + 1, n == 1 ? -1 : e, fw, fh,
                        c.nrPreset, Res(r), f.viaCore ? "" : std::format(", via the signed runtime {}", Res(r2)));
                    if (!f.h || (!f.viaCore && NVSDK_NGX_FAILED(r2))) {
                        ok = NrFail("feature 18 create " + Res(r2));
                        break;
                    }
                    f.reset = true;
                }
            }
            g_nr.list->Close();
            ID3D12CommandList* lists[] = { g_nr.list };
            g_nr.queue->ExecuteCommandLists(1, lists);
            g_nr.queue->Signal(g_nr.fence12, sig);
            return ok;
        }

        void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b,
            std::vector<D3D12_RESOURCE_BARRIER>& v)
        {
            (void)l;
            D3D12_RESOURCE_BARRIER br{};
            br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            br.Transition.pResource = r;
            br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            br.Transition.StateBefore = a;
            br.Transition.StateAfter = b;
            v.push_back(br);
        }

        PassCfg PassOf(const Cfg& c, int p)
        {
            if (p == 0) return c.p1;
            PassCfg q = c.pass[p - 1];
            if (q.follow) {
                // "Same as pass 1": the model steering follows pass 1; intensity / strengths stay the pass's own (RenoDX PassModel)
                const PassCfg& a = c.p1;
                q.style = a.style;
                q.localTone = a.localTone;
                q.structure = a.structure;
                q.skin = a.skin;
                q.autoMask = a.autoMask;
                q.uiCorrection = a.uiCorrection;
            }
            return q;
        }

        // D3D12 part of one NR pass: evaluate every eye (or the shared feature) with the shared fence around it.
        bool NrEvaluate(const Cfg& c, int a_pass, double a_mvScaleX, double a_mvScaleY, bool a_haveJitter, float a_jx, float a_jy,
            bool a_forceReset)
        {
            const int n = c.nrPerEye ? 2 : 1;
            const unsigned fw = g_nr.featW, fh = g_nr.featH;
            const PassCfg pc = PassOf(c, a_pass);
            // D3D11 -> D3D12
            const std::uint64_t ready = ++g_nr.fenceValue;
            g_ctx4->Signal(g_nr.fence11, ready);
            g_ctx->Flush();
            g_nr.queue->Wait(g_nr.fence12, ready);
            const std::uint64_t done = ++g_nr.fenceValue;
            ID3D12CommandAllocator* al = NextAllocator(done);
            g_nr.list->Reset(al, nullptr);
            std::vector<D3D12_RESOURCE_BARRIER> bars;
            for (int e = 0; e < n; ++e) {
                Barrier(g_nr.list, g_nr.proxy[e].t12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, bars);
                Barrier(g_nr.list, g_nr.mv[e].t12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, bars);
                Barrier(g_nr.list, g_nr.depth[e].t12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, bars);
                Barrier(g_nr.list, g_nr.neural[e].t12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bars);
            }
            g_nr.list->ResourceBarrier(static_cast<UINT>(bars.size()), bars.data());
            const int qs = g_nr.qNext;
            const bool timed = g_nr.qheap && g_nr.qread && c.gpuTimers && !g_nr.qPending[qs];
            if (timed) g_nr.list->EndQuery(g_nr.qheap, D3D12_QUERY_TYPE_TIMESTAMP, qs * 4 + 0);
            bool ok = true;
            for (int e = 0; e < n && ok; ++e) {
                auto& f = g_nr.feat[a_pass][e];
                NVSDK_NGX_Parameter* p = f.p;
                NVSDK_NGX_Parameter_SetD3d12Resource(p, "DLSSNR.Color", g_nr.proxy[e].t12);
                NVSDK_NGX_Parameter_SetD3d12Resource(p, "DLSSNR.Output", g_nr.neural[e].t12);
                NVSDK_NGX_Parameter_SetD3d12Resource(p, "DLSSNR.MVec", g_nr.mv[e].t12);
                NVSDK_NGX_Parameter_SetD3d12Resource(p, "DLSSNR.Depth", g_nr.depth[e].t12);
                SetNrDims(p, fw, fh);
                for (const char* pre : { "DLSSNR.Color", "DLSSNR.Depth", "DLSSNR.MVec", "DLSSNR.Output" }) {
                    const std::string b(pre);
                    NVSDK_NGX_Parameter_SetUI(p, (b + "SubrectBaseX").c_str(), 0);
                    NVSDK_NGX_Parameter_SetUI(p, (b + "SubrectBaseY").c_str(), 0);
                    NVSDK_NGX_Parameter_SetUI(p, (b + "SubrectWidth").c_str(), fw);
                    NVSDK_NGX_Parameter_SetUI(p, (b + "SubrectHeight").c_str(), fh);
                }
                NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatioCallback));
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.MVecScaleX", static_cast<float>(a_mvScaleX));
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.MVecScaleY", static_cast<float>(a_mvScaleY));
                if (a_haveJitter) {
                    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.JitterOffsetX", a_jx);
                    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.JitterOffsetY", a_jy);
                }
                NVSDK_NGX_Parameter_SetI(p, "DLSSNR.DepthInverted", c.nrDepthMode == 2 ? 1 : 0);
                NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Enabled", 1);
                // steering; RenoDX SentSkinSteering: with the character mask and "skin independent", structure <= 1, stock skin = 1
                float structure = pc.structure, skin = pc.skin;
                if (pc.autoMask && c.nrSkinIndependent) {
                    structure = (std::min)(structure, 1.0f);
                    skin = skin < 0.0f ? 1.0f : skin;
                }
                const std::array<float, 8> model{ static_cast<float>(pc.style), pc.localTone, structure, skin, pc.autoMask ? 1.0f : 0.0f,
                    pc.uiCorrection ? 1.0f : 0.0f, static_cast<float>(c.nrDepthMode), 0.0f };
                const bool steerChanged = model != f.model;
                f.model = model;
                const bool reset = f.reset || a_forceReset || steerChanged || (a_pass > 0 && !c.nrChainedHistory);
                NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Reset", reset ? 1 : 0);
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.Intensity", pc.intensity);
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.LocalToneStrength", pc.localTone);
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.LocalStructureStrength", structure);
                NVSDK_NGX_Parameter_SetF(p, "DLSSNR.SkinStructureStrength", skin);
                NVSDK_NGX_Parameter_SetI(p, "DLSSNR.UseAutoMask", pc.autoMask ? 1 : 0);
                NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Style", static_cast<unsigned>(pc.style));
                NVSDK_NGX_Parameter_SetI(p, "DLSSNR.UICorrection", pc.uiCorrection ? 1 : 0);
                NVSDK_NGX_Parameter_SetI(p, "DLSS.Indicator.Invert.X.Axis", 0);
                NVSDK_NGX_Parameter_SetI(p, "DLSS.Indicator.Invert.Y.Axis", 0);
                const NVSDK_NGX_Result r = f.viaCore ? NVSDK_NGX_D3D12_EvaluateFeature(g_nr.list, f.h, p, nullptr)
                                                     : g_nr.evaluate(g_nr.list, f.h, p, nullptr);
                if (NVSDK_NGX_FAILED(r)) {
                    ++g_nrFail;
                    if (g_nrFail <= 10) REX::ERROR("dlss: NR evaluate pass {} eye {} -> {}", a_pass + 1, e, Res(r));
                    if (g_nrFail >= 30 && g_nrOk == 0) NrFail("feature 18 evaluates keep failing (" + Res(r) + ")");
                    ok = false;
                } else {
                    f.reset = false;
                    ++g_nrOk;
                }
                if (timed) g_nr.list->EndQuery(g_nr.qheap, D3D12_QUERY_TYPE_TIMESTAMP, qs * 4 + 1 + e);
            }
            bars.clear();
            for (int e = 0; e < n; ++e) {
                Barrier(g_nr.list, g_nr.proxy[e].t12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, bars);
                Barrier(g_nr.list, g_nr.mv[e].t12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, bars);
                Barrier(g_nr.list, g_nr.depth[e].t12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, bars);
                Barrier(g_nr.list, g_nr.neural[e].t12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, bars);
            }
            g_nr.list->ResourceBarrier(static_cast<UINT>(bars.size()), bars.data());
            if (timed) {
                g_nr.list->ResolveQueryData(g_nr.qheap, D3D12_QUERY_TYPE_TIMESTAMP, qs * 4, 1 + n, g_nr.qread,
                    static_cast<UINT64>(qs) * 4 * sizeof(std::uint64_t));
                g_nr.qPending[qs] = true;
                g_nr.qFence[qs] = done;
                g_nr.qEyes[qs] = n;
                g_nr.qNext = (qs + 1) % Nr::kQSlots;
            }
            g_nr.list->Close();
            ID3D12CommandList* lists[] = { g_nr.list };
            g_nr.queue->ExecuteCommandLists(1, lists);
            g_nr.queue->Signal(g_nr.fence12, done);
            // D3D12 -> D3D11 (GPU wait)
            g_ctx4->Wait(g_nr.fence11, done);
            return ok;
        }

        void NrTimerCollect()
        {
            if (!g_nr.qread || !g_nr.fence12 || !g_nr.qfreq) return;
            const std::uint64_t comp = g_nr.fence12->GetCompletedValue();
            std::uint64_t* data = nullptr;
            D3D12_RANGE rr{ 0, Nr::kQSlots * 4 * sizeof(std::uint64_t) };
            bool mapped = false;
            for (int i = 0; i < Nr::kQSlots; ++i) {
                if (!g_nr.qPending[i] || comp < g_nr.qFence[i]) continue;
                if (!mapped) {
                    if (FAILED(g_nr.qread->Map(0, &rr, reinterpret_cast<void**>(&data)))) return;
                    mapped = true;
                }
                const std::uint64_t* q = data + i * 4;
                const double f = 1000.0 / static_cast<double>(g_nr.qfreq);
                const int n = g_nr.qEyes[i];
                const double tot = (q[n] - q[0]) * f;
                if (tot >= 0 && tot < 1000) {
                    g_sumNr12 += tot;
                    g_sumNr12Eye[0] += (q[1] - q[0]) * f;
                    if (n > 1) g_sumNr12Eye[1] += (q[2] - q[1]) * f;
                    ++g_nNr12;
                }
                g_nr.qPending[i] = false;
            }
            if (mapped) {
                D3D12_RANGE none{ 0, 0 };
                g_nr.qread->Unmap(0, &none);
            }
        }

        // ---------------------------------------------------------------------------------------------------------------
        // NR stereo-consistency probe (pipe "dlss nrstereo"): read back both eyes' proxy + neural planes, then on a worker
        // match blocks between the eyes along x (disparity search on the proxy) and compare the model's edits (N - P).
        struct StereoProbe
        {
            int stage{ 0 };  // 0 idle, 1 requested, 2 copied (waiting to map)
            ID3D11Texture2D* st[4]{};  // proxy L, neural L, proxy R, neural R
            unsigned w{ 0 }, h{ 0 };
            std::uint64_t copiedFrame{ 0 };
        };
        StereoProbe g_probe;
        std::mutex g_probeLock;
        std::string g_probeResult = "not run";

        float Half(std::uint16_t h)
        {
            const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
            return s ? -v : v;
        }

        void ProbeAnalyse(std::vector<float> pL, std::vector<float> nL, std::vector<float> pR, std::vector<float> nR, unsigned w, unsigned h)
        {
            // luminance planes; 16x16 blocks of the left eye, disparity search 0..w/6 to the left in the right eye
            const int B = 16, maxD = static_cast<int>(w / 6);
            double sEL = 0, sER = 0, sELR = 0, sEL2 = 0, sER2 = 0, sDiff2 = 0, sPdiff2 = 0, sD = 0;
            long long cnt = 0;
            int blocks = 0, skipped = 0;
            for (unsigned by = 0; by + B <= h; by += B) {
                for (unsigned bx = 0; bx + B <= w; bx += B) {
                    // texture: skip flat blocks (no reliable match)
                    double mean = 0, var = 0;
                    for (int y = 0; y < B; ++y)
                        for (int x = 0; x < B; ++x) mean += pL[(by + y) * w + bx + x];
                    mean /= B * B;
                    for (int y = 0; y < B; ++y)
                        for (int x = 0; x < B; ++x) {
                            const double d = pL[(by + y) * w + bx + x] - mean;
                            var += d * d;
                        }
                    if (var / (B * B) < 1e-4) {
                        ++skipped;
                        continue;
                    }
                    int best = -1;
                    double bestSad = 1e30;
                    for (int d = 0; d <= maxD; ++d) {
                        if (static_cast<int>(bx) - d < 0) break;
                        double sad = 0;
                        for (int y = 0; y < B && sad < bestSad; ++y)
                            for (int x = 0; x < B; ++x) sad += std::abs(pL[(by + y) * w + bx + x] - pR[(by + y) * w + bx + x - d]);
                        if (sad < bestSad) bestSad = sad, best = d;
                    }
                    if (best < 0 || bestSad / (B * B) > 0.02) {
                        ++skipped;
                        continue;
                    }
                    ++blocks;
                    sD += best;
                    for (int y = 0; y < B; ++y)
                        for (int x = 0; x < B; ++x) {
                            const std::size_t il = (by + y) * w + bx + x, ir = (by + y) * w + bx + x - best;
                            const double eL = nL[il] - pL[il], eR = nR[ir] - pR[ir];
                            sEL += eL, sER += eR, sELR += eL * eR, sEL2 += eL * eL, sER2 += eR * eR;
                            sDiff2 += (eL - eR) * (eL - eR);
                            sPdiff2 += (pL[il] - pR[ir]) * (pL[il] - pR[ir]);
                            ++cnt;
                        }
                }
            }
            std::string r;
            if (cnt == 0) {
                r = std::format("no matched blocks ({} skipped)", skipped);
            } else {
                const double n = static_cast<double>(cnt);
                const double mL = sEL / n, mR = sER / n;
                const double cov = sELR / n - mL * mR, vL = sEL2 / n - mL * mL, vR = sER2 / n - mR * mR;
                const double corr = (vL > 0 && vR > 0) ? cov / std::sqrt(vL * vR) : 0.0;
                r = std::format("blocks {} matched ({} skipped), mean disparity {:.1f} px | NR edit rms L {:.4f} R {:.4f}, mean L {:+.4f} R {:+.4f} | "
                                "edit difference rms {:.4f} (input mismatch rms {:.4f}) | edit correlation {:.3f}",
                    blocks, skipped, sD / blocks, std::sqrt(sEL2 / n), std::sqrt(sER2 / n), mL, mR, std::sqrt(sDiff2 / n),
                    std::sqrt(sPdiff2 / n), corr);
            }
            REX::INFO("dlss: nrstereo (working {}x{} per eye, luminance): {}", w, h, r);
            std::scoped_lock l(g_probeLock);
            g_probeResult = r;
        }

        // Temporal stability probe (pipe "dlss stability"): eye 0's final image of two consecutive frames, mean absolute luminance
        // difference (0-255). On a still scene a correct jitter sign / motion-vector scale converges to a steadier image.
        struct StabProbe
        {
            int stage{ 0 };  // 1-2 copy frames, 3 waiting to map
            ID3D11Texture2D* st[3]{};  // final frame A, final frame B, input colour frame A
            unsigned w{ 0 }, h{ 0 };
            std::uint64_t frame{ 0 };
            std::string tag;
        };
        StabProbe g_stab;
        std::string g_stabResult = "not run";

        DXGI_FORMAT g_stabInFmt = DXGI_FORMAT_UNKNOWN;
        void StabCopy(ID3D11Texture2D* a_fin, ID3D11Resource* a_in, DXGI_FORMAT a_inFmt, const D3D11_BOX& a_box, DXGI_FORMAT a_fmt)
        {
            if (g_stab.stage < 1 || g_stab.stage > 2) return;
            const int i = g_stab.stage - 1;
            for (int k : { i, 2 }) {
            if (k == 2 && i != 0) break;
            if (!g_stab.st[k]) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = a_box.right - a_box.left;
                d.Height = a_box.bottom - a_box.top;
                d.MipLevels = d.ArraySize = 1;
                d.Format = k == 2 ? a_inFmt : a_fmt;
                if (k == 2) g_stabInFmt = a_inFmt;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_stab.st[k]))) {
                    g_stab.stage = 0;
                    return;
                }
                g_stab.w = d.Width;
                g_stab.h = d.Height;
            }
            }
            g_ctx->CopySubresourceRegion(g_stab.st[i], 0, 0, 0, 0, a_fin, 0, &a_box);
            if (i == 0) g_ctx->CopySubresourceRegion(g_stab.st[2], 0, 0, 0, 0, a_in, 0, &a_box);
            g_stab.frame = g_frame;
            ++g_stab.stage;
        }

        void StabTick()
        {
            if (g_stab.stage != 3 || g_frame < g_stab.frame + 3) return;
            D3D11_MAPPED_SUBRESOURCE m[3]{};
            for (int k = 0; k < 3; ++k) {
                if (g_ctx->Map(g_stab.st[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[k]) != S_OK) {
                    for (int j = 0; j < k; ++j) g_ctx->Unmap(g_stab.st[j], 0);
                    return;
                }
            }
            double sum = 0, sumC = 0, meanOut = 0, meanIn = 0, inOut = 0;
            std::uint64_t n = 0, changed = 0;
            for (unsigned y = 0; y < g_stab.h; y += 2) {
                const auto* a = static_cast<const std::uint8_t*>(m[0].pData) + y * m[0].RowPitch;
                const auto* b = static_cast<const std::uint8_t*>(m[1].pData) + y * m[1].RowPitch;
                const auto* c = static_cast<const std::uint8_t*>(m[2].pData) + y * m[2].RowPitch;
                for (unsigned x = 0; x < g_stab.w; x += 2) {
                    const double la = 0.2126 * a[x * 4] + 0.7152 * a[x * 4 + 1] + 0.0722 * a[x * 4 + 2];
                    const double lb = 0.2126 * b[x * 4] + 0.7152 * b[x * 4 + 1] + 0.0722 * b[x * 4 + 2];
                    double lc;
                    if (g_stabInFmt == DXGI_FORMAT_R11G11B10_FLOAT) {
                        // packed floats: R 11 bit (6m 5e) | G 11 bit | B 10 bit (5m 5e), no sign
                        std::uint32_t v;
                        std::memcpy(&v, c + x * 4, 4);
                        auto f = [](std::uint32_t bits, int mbits) {
                            const std::uint32_t m = bits & ((1u << mbits) - 1), e = bits >> mbits;
                            return e == 0 ? std::ldexp(static_cast<double>(m), -14 - mbits) : std::ldexp(1.0 + m / static_cast<double>(1u << mbits), static_cast<int>(e) - 15);
                        };
                        lc = 255.0 * (0.2126 * f(v & 0x7FF, 6) + 0.7152 * f((v >> 11) & 0x7FF, 6) + 0.0722 * f(v >> 22, 5));
                    } else {
                        lc = 0.2126 * c[x * 4] + 0.7152 * c[x * 4 + 1] + 0.0722 * c[x * 4 + 2];
                    }
                    meanOut += la, meanIn += lc, inOut += std::abs(la - lc);
                    const double d = std::abs(la - lb);
                    sum += d;
                    if (d > 2.0) ++changed, sumC += d;
                    ++n;
                }
            }
            for (int k = 0; k < 3; ++k) g_ctx->Unmap(g_stab.st[k], 0);
            for (auto*& t : g_stab.st) SafeRelease(t);
            g_stab.stage = 0;
            g_stabResult = std::format("{}: frame-to-frame mean |dL| {:.3f} (0-255), pixels changing > 2: {:.2f}% | mean L out {:.1f} vs "
                                       "TAA input {:.1f}, mean |out - in| {:.2f}", g_stab.tag, n ? sum / n : 0.0, n ? 100.0 * changed / n : 0.0,
                n ? meanOut / n : 0.0, n ? meanIn / n : 0.0, n ? inOut / n : 0.0);
            REX::INFO("dlss: stability {}", g_stabResult);
        }

        void ProbeTick()
        {
            if (g_probe.stage != 2 || g_frame < g_probe.copiedFrame + 3) return;
            std::vector<float> planes[4];
            for (int i = 0; i < 4; ++i) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (g_ctx->Map(g_probe.st[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) {
                    for (int k = 0; k < i; ++k) g_ctx->Unmap(g_probe.st[k], 0);
                    return;  // not yet
                }
                planes[i].resize(static_cast<std::size_t>(g_probe.w) * g_probe.h);
                for (unsigned y = 0; y < g_probe.h; ++y) {
                    const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch);
                    for (unsigned x = 0; x < g_probe.w; ++x) {
                        const float r = Half(row[x * 4]), g = Half(row[x * 4 + 1]), b = Half(row[x * 4 + 2]);
                        planes[i][y * g_probe.w + x] = 0.212639f * r + 0.715169f * g + 0.072192f * b;
                    }
                }
            }
            for (int i = 0; i < 4; ++i) {
                g_ctx->Unmap(g_probe.st[i], 0);
                SafeRelease(g_probe.st[i]);
            }
            g_probe.stage = 0;
            std::thread(ProbeAnalyse, std::move(planes[0]), std::move(planes[1]), std::move(planes[2]), std::move(planes[3]), g_probe.w,
                g_probe.h)
                .detach();
        }

        // copies the planes right after the D3D11 side waited for pass 1 (render thread, inside Isolated)
        void ProbeCopy(const Cfg& c)
        {
            if (g_probe.stage != 1) return;
            const unsigned ww = g_nr.workW, wh = g_nr.workH;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = ww;
            d.Height = wh;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (int i = 0; i < 4; ++i) {
                if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_probe.st[i]))) {
                    for (auto*& t : g_probe.st) SafeRelease(t);
                    g_probe.stage = 0;
                    return;
                }
            }
            for (int e = 0; e < 2; ++e) {
                const int ti = c.nrPerEye ? e : 0;
                const unsigned x0 = c.nrPerEye ? 0 : e * ww;
                D3D11_BOX box{ x0, 0, 0, x0 + ww, wh, 1 };
                g_ctx->CopySubresourceRegion(g_probe.st[e * 2], 0, 0, 0, 0, g_nr.proxy[ti].t11, 0, &box);
                g_ctx->CopySubresourceRegion(g_probe.st[e * 2 + 1], 0, 0, 0, 0, g_nr.neural[ti].t11, 0, &box);
            }
            g_probe.w = ww;
            g_probe.h = wh;
            g_probe.copiedFrame = g_frame;
            g_probe.stage = 2;
        }

        // ---------------------------------------------------------------------------------------------------------------
        // the NR chain on the D3D11 side: src (double-wide SRV) -> pp[x]; returns the index of the result in g_dw.pp or -1
        struct Frame
        {
            unsigned W{ 0 }, H{ 0 }, eyeW{ 0 }, x0[2]{};
            double   jxPx{ 0 }, jyPx{ 0 };  // jitter in render pixels (DLSS convention, signs applied)
        };

        // ---------------------------------------------------------------------------------------------------------------
        // look stage resources (D3D11): band scratch at half NR res and the transport targets at the eye size are shared by
        // the eyes (each eye's look runs and is resolved before the next one's); N' has the NR textures' layout; the
        // temporal history is a raw ping-pong per pass and eye (RenoDX: per NR handle), 12 bytes per NR pixel.
        struct Tex
        {
            ID3D11Texture2D*           t{ nullptr };
            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
        };
        struct RawBuf
        {
            ID3D11Buffer*              b{ nullptr };
            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
        };
        struct Look
        {
            unsigned ww{ 0 }, wh{ 0 }, tw{ 0 }, fw{ 0 }, fh{ 0 };
            Tex bandA, bandB, bandMaxA, bandMaxB, out[2], upP, upN, traceBlocks;
            RawBuf hist[4][2][2];
            int  cur[4][2]{};
            bool valid[4][2]{};
            RawBuf traceHist;
            ID3D11Buffer* traceStage{ nullptr };
            bool failed{ false };
            std::string lastPlan;
            std::chrono::steady_clock::time_point lastRun{};
        };
        Look g_look;
        struct Trace
        {
            int stage{ 0 };  // 0 idle, 1 requested, 2 copied (waiting to map)
            std::uint64_t frame{ 0 };
            std::string result{ "none" };
        };
        Trace g_trace;

        void ReleaseTex(Tex& t)
        {
            SafeRelease(t.srv);
            SafeRelease(t.uav);
            SafeRelease(t.t);
        }
        void ReleaseRaw(RawBuf& b)
        {
            SafeRelease(b.srv);
            SafeRelease(b.uav);
            SafeRelease(b.b);
        }
        bool MakeT(Tex& t, unsigned w, unsigned h, DXGI_FORMAT f)
        {
            return MakeTex(w, h, f, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &t.t, &t.srv, &t.uav);
        }
        bool MakeRaw(RawBuf& o, UINT a_bytes)
        {
            D3D11_BUFFER_DESC d{ a_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0,
                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
            if (FAILED(g_dev->CreateBuffer(&d, nullptr, &o.b))) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx.NumElements = a_bytes / 4;
            sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_R32_TYPELESS;
            uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uv.Buffer.NumElements = a_bytes / 4;
            uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            return SUCCEEDED(g_dev->CreateShaderResourceView(o.b, &sv, &o.srv)) && SUCCEEDED(g_dev->CreateUnorderedAccessView(o.b, &uv, &o.uav));
        }
        void ReleaseLook()
        {
            for (Tex* t : { &g_look.bandA, &g_look.bandB, &g_look.bandMaxA, &g_look.bandMaxB, &g_look.out[0], &g_look.out[1], &g_look.upP,
                     &g_look.upN, &g_look.traceBlocks })
                ReleaseTex(*t);
            for (auto& p : g_look.hist)
                for (auto& e : p)
                    for (auto& b : e) ReleaseRaw(b);
            ReleaseRaw(g_look.traceHist);
            SafeRelease(g_look.traceStage);
            std::memset(g_look.valid, 0, sizeof(g_look.valid));
            g_look.ww = 0;
        }
        bool EnsureLook(unsigned ww, unsigned wh, unsigned tw, unsigned fw, unsigned fh, int a_tex)
        {
            if (g_look.failed || g_lookProgramsFailed) return false;
            if (g_look.ww == ww && g_look.wh == wh && g_look.tw == tw && g_look.fw == fw && g_look.fh == fh && g_look.out[a_tex - 1].t) return true;
            ReleaseLook();
            const unsigned hw = (ww + 1) / 2, hh = (wh + 1) / 2;
            bool ok = MakeT(g_look.bandA, hw, hh, DXGI_FORMAT_R32G32B32A32_FLOAT) && MakeT(g_look.bandB, hw, hh, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                      MakeT(g_look.bandMaxA, hw, hh, DXGI_FORMAT_R32G32_FLOAT) && MakeT(g_look.bandMaxB, hw, hh, DXGI_FORMAT_R32G32_FLOAT) &&
                      MakeT(g_look.upP, fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT) && MakeT(g_look.upN, fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      MakeT(g_look.traceBlocks, (ww + 31) / 32, (wh + 31) / 32, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                      MakeRaw(g_look.traceHist, kTraceWords * 4);
            for (int t = 0; t < a_tex && ok; ++t) ok = MakeT(g_look.out[t], tw, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (ok) {
                D3D11_BUFFER_DESC sd{ kTraceWords * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
                ok = SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &g_look.traceStage));
            }
            if (!ok) {
                ReleaseLook();
                g_look.failed = true;
                REX::ERROR("dlss: look stage textures {}x{} (eye {}x{}) failed: look stage off for this session", ww, wh, fw, fh);
                return false;
            }
            g_look.ww = ww;
            g_look.wh = wh;
            g_look.tw = tw;
            g_look.fw = fw;
            g_look.fh = fh;
            REX::INFO("dlss: look stage surfaces: bands {}x{}, N' {}x{} x{}, transport {}x{}", hw, hh, tw, wh, a_tex, fw, fh);
            return true;
        }

        LookSettings LookOf(const Cfg& c, bool a_detailStability)
        {
            // RenoDX LookSettingsSnapshot: the gains only with NRLookMode on; Upsampling always; detail stability takes the
            // temporal filter (whole edit, per-frame weight, range confidence) when the player's Stabilize is off
            LookSettings s;
            s.upsample = static_cast<std::uint32_t>(c.lookUpsample);
            if (c.lookMode) {
                s.strength = c.lookStrength;
                s.brighten = c.lookBrighten;
                s.darken = c.lookDarken;
                s.maxBrighten = c.lookMaxBrighten;
                s.maxDarken = c.lookMaxDarken;
                s.colour = c.lookColour;
                s.hue = c.lookHue;
                s.maxColour = c.lookMaxColour;
                s.shadows = c.lookShadows;
                s.midtones = c.lookMidtones;
                s.highlights = c.lookHighlights;
                s.tone = c.lookTone;
                s.detail = c.lookDetail;
                s.halo = c.lookHalo;
                s.haloBright = c.lookHaloBright;
                s.detailRadius = c.lookDetailRadius;
                s.stabilize = static_cast<std::uint32_t>(c.lookStabilize);
                s.stabMs = c.lookStabMs;
                s.stabDetail = c.lookStabDetail;
            }
            if (s.stabilize == 0 && a_detailStability) {
                s.stabilize = 2;
                s.stabDetail = true;
                s.stabAlpha = c.nrStabilityAlpha;
            }
            return s;
        }

        void BindLook(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> a_srv, ID3D11UnorderedAccessView* u0,
            ID3D11UnorderedAccessView* u1)
        {
            ID3D11UnorderedAccessView* none[3]{};
            g_ctx->CSSetUnorderedAccessViews(0, 3, none, nullptr);
            ID3D11ShaderResourceView* s[7]{};
            int i = 0;
            for (auto* v : a_srv) s[i++] = v;
            g_ctx->CSSetShaderResources(0, 7, s);
            ID3D11UnorderedAccessView* u[2]{ u0, u1 };
            g_ctx->CSSetUnorderedAccessViews(0, 2, u, nullptr);
            g_ctx->CSSetShader(cs, nullptr, 0);
            g_ctx->CSSetConstantBuffers(0, 1, &g_lookCb);
        }
        void UnbindLook()
        {
            ID3D11ShaderResourceView* s[7]{};
            ID3D11UnorderedAccessView* u[3]{};
            g_ctx->CSSetUnorderedAccessViews(0, 3, u, nullptr);
            g_ctx->CSSetShaderResources(0, 7, s);
        }

        // One eye of one pass (RenoDX RecordCompose + RecordUpsample): bands (5 passes at half NR res) when a band gain or the
        // player's Stabilize needs them, the compose (temporal filter + gains -> N'), then the edge-aware transport when NR runs
        // below the eye size. a_ti = the NR texture set (eye, or 0 in the shared mode), a_workBase = the eye's x in it.
        void LookDispatch(const LookSettings& s, int a_pass, int a_eye, int a_ti, unsigned a_workBase, unsigned a_srcBase,
            ID3D11ShaderResourceView* a_src, bool a_transport, double a_mvsx, double a_mvsy, float a_dt, bool a_reset)
        {
            const unsigned ww = g_look.ww, wh = g_look.wh;
            bool temporal = s.stabilize != 0;
            RawBuf* hin = nullptr;
            RawBuf* hout = nullptr;
            if (temporal) {
                auto& hp = g_look.hist[a_pass][a_eye];
                const UINT bytes = ww * wh * 12;
                for (auto& b : hp)
                    if (!b.b && !MakeRaw(b, bytes)) temporal = false;
                if (temporal) {
                    hin = &hp[g_look.cur[a_pass][a_eye]];
                    hout = &hp[g_look.cur[a_pass][a_eye] ^ 1];
                }
            }
            const bool histValid = temporal && g_look.valid[a_pass][a_eye] && !a_reset && a_dt <= 0.25f;
            LookConst k{};
            k.nrSize[0] = ww;
            k.nrSize[1] = wh;
            k.fullSize[0] = g_look.fw;
            k.fullSize[1] = g_look.fh;
            k.flags = (LookBandsWanted(s) ? kLookBands : 0u) | (temporal ? kLookTemporal : 0u) | (temporal && s.stabilize == 2 ? kLookMotion : 0u) |
                      (temporal && s.stabDetail ? kLookTemporalDetail : 0u) | (a_transport ? kLookTransport : 0u) |
                      (histValid ? kLookHistoryValid : 0u) | (temporal && s.stabAlpha > 0 ? kLookRangeConfidence : 0u);
            k.strength = s.strength;
            k.brighten = s.brighten;
            k.darken = s.darken;
            k.maxBrighten = s.maxBrighten;
            k.maxDarken = s.maxDarken;
            k.colour = s.colour;
            k.hue = s.hue;
            k.maxColour = s.maxColour;
            k.shadows = s.shadows;
            k.midtones = s.midtones;
            k.highlights = s.highlights;
            k.tone = s.tone;
            k.detail = s.detail;
            k.halo = s.halo;
            k.haloBright = s.haloBright;
            k.radius = std::clamp(static_cast<std::uint32_t>(std::lround(s.detailRadius * 0.01f * static_cast<float>(wh) * 0.5f)), 1u, 64u);
            k.minRadius = (std::min)(k.radius * 4u, 48u);
            k.midRadius = (std::min)(k.radius * 6u, 64u);
            k.alpha = s.stabAlpha > 0 ? s.stabAlpha : 1.0f - std::exp(-a_dt / (std::max)(s.stabMs * 1e-3f, 1e-3f));
            k.motionScale[0] = static_cast<float>(a_mvsx);
            k.motionScale[1] = static_cast<float>(a_mvsy);
            k.motionGrid[0] = ww;
            k.motionGrid[1] = wh;
            k.gfEps = 0.01f;
            k.workBase[0] = a_workBase;
            k.srcBase[0] = a_srcBase;
            ID3D11ShaderResourceView* const P = g_nr.proxy[a_ti].srv;
            ID3D11ShaderResourceView* const N = g_nr.neural[a_ti].srv;
            if (k.flags & kLookBands) {
                LookConst b = k;
                b.size[0] = (ww + 1) / 2;
                b.size[1] = (wh + 1) / 2;
                for (std::uint32_t mode = 0; mode < 5; ++mode) {
                    b.mode = mode;
                    g_ctx->UpdateSubresource(g_lookCb, 0, nullptr, &b, 0, 0);
                    switch (mode) {
                    case 0: BindLook(g_csLookBand, { nullptr, P, N }, g_look.bandA.uav, g_look.bandMaxA.uav); break;
                    case 1: BindLook(g_csLookBand, { nullptr, P, N, g_look.bandA.srv, g_look.bandMaxA.srv }, g_look.bandB.uav, g_look.bandMaxB.uav); break;
                    case 2: BindLook(g_csLookBand, { nullptr, P, N, g_look.bandB.srv, g_look.bandMaxB.srv }, g_look.bandA.uav, g_look.bandMaxA.uav); break;
                    case 3: BindLook(g_csLookBand, { nullptr, P, N, g_look.bandA.srv }, g_look.bandB.uav, nullptr); break;
                    default: BindLook(g_csLookBand, { nullptr, P, N, g_look.bandB.srv }, g_look.bandA.uav, nullptr); break;
                    }
                    g_ctx->Dispatch((b.size[0] + 7) / 8, (b.size[1] + 7) / 8, 1);
                }
            }
            k.size[0] = ww;
            k.size[1] = wh;
            g_ctx->UpdateSubresource(g_lookCb, 0, nullptr, &k, 0, 0);
            BindLook(g_csLookCompose, { a_src, P, N, g_look.bandA.srv, g_look.bandMaxA.srv, g_nr.mv[a_ti].srv, hin ? hin->srv : nullptr },
                g_look.out[a_ti].uav, hout ? hout->uav : nullptr);
            g_ctx->Dispatch((ww + 15) / 16, (wh + 15) / 16, 1);
            if (temporal) {
                g_look.cur[a_pass][a_eye] ^= 1;
                g_look.valid[a_pass][a_eye] = true;
            } else {
                g_look.valid[a_pass][a_eye] = false;
            }
            if (a_transport) {
                k.size[0] = g_look.fw;
                k.size[1] = g_look.fh;
                g_ctx->UpdateSubresource(g_lookCb, 0, nullptr, &k, 0, 0);
                BindLook(g_csLookUpsample, { a_src, P, g_look.out[a_ti].srv }, g_look.upP.uav, g_look.upN.uav);
                g_ctx->Dispatch((g_look.fw + 15) / 16, (g_look.fh + 15) / 16, 1);
            }
            UnbindLook();
        }

        // NREditTrace (RenoDX RecordTrace), one shot from the pipe: the raw edit of pass 1, eye 0, read back a frame later
        void LookTrace(int a_ti, unsigned a_workBase)
        {
            LookConst k{};
            k.size[0] = k.nrSize[0] = g_look.ww;
            k.size[1] = k.nrSize[1] = g_look.wh;
            k.workBase[0] = a_workBase;
            k.mode = 0;
            g_ctx->UpdateSubresource(g_lookCb, 0, nullptr, &k, 0, 0);
            BindLook(g_csLookTrace, { nullptr, g_nr.proxy[a_ti].srv, g_nr.neural[a_ti].srv }, g_look.traceBlocks.uav, g_look.traceHist.uav);
            g_ctx->Dispatch(1, 1, 1);
            k.mode = 1;
            g_ctx->UpdateSubresource(g_lookCb, 0, nullptr, &k, 0, 0);
            g_ctx->Dispatch((g_look.ww + 31) / 32, (g_look.wh + 31) / 32, 1);
            UnbindLook();
            g_ctx->CopyResource(g_look.traceStage, g_look.traceHist.b);
            g_trace.frame = g_frame;
            g_trace.stage = 2;
        }

        void TraceTick()
        {
            if (g_trace.stage != 2 || !g_look.traceStage || !g_ctx || g_frame < g_trace.frame + 2) return;
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (FAILED(g_ctx->Map(g_look.traceStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms))) return;
            std::uint32_t h[kTraceWords];
            std::memcpy(h, ms.pData, sizeof(h));
            g_ctx->Unmap(g_look.traceStage, 0);
            g_trace.stage = 0;
            const double n = (std::max)(1u, h[128]);
            double mean = 0, acc = 0, p05 = 0, p50 = 0, p95 = 0;
            for (int i = 0; i < 128; ++i) {
                const double centre = -4.0 + (i + 0.5) / 16.0;
                mean += centre * h[i];
                const double before = acc;
                acc += h[i];
                if (before < 0.05 * n && acc >= 0.05 * n) p05 = centre;
                if (before < 0.50 * n && acc >= 0.50 * n) p50 = centre;
                if (before < 0.95 * n && acc >= 0.95 * n) p95 = centre;
            }
            g_trace.result = std::format("pixels {} | achromatic edit a (stops): mean {:.3f} p05 {:.3f} p50 {:.3f} p95 {:.3f} | |a|>1 {:.2f}% | "
                                         "halo {:.2f}% | halo (bright side) {:.2f}%",
                h[128], mean / n, p05, p50, p95, 100.0 * h[129] / n, 100.0 * h[130] / n, 100.0 * h[131] / n);
            REX::INFO("dlss: edit trace: {}", g_trace.result);
        }

        int RunNr(const Cfg& c, const Frame& fr, ID3D11ShaderResourceView* a_src, ID3D11ShaderResourceView* a_mv, bool a_srcJittered,
            bool a_forceReset)
        {
            if (!EnsureNrDevice() || !EnsureNrResources(fr.eyeW, fr.H, c) || !EnsureNrFeatures(c, c.nrPasses)) return -1;
            const unsigned ww = g_nr.workW, wh = g_nr.workH;
            const double s = static_cast<double>(ww) / fr.eyeW;
            const double mvsx = fr.W * s * c.nrMvScaleX * c.mvScaleX, mvsy = fr.H * (static_cast<double>(wh) / fr.H) * c.nrMvScaleY * c.mvScaleY;
            const int n = c.nrPerEye ? 2 : 1;
            ID3D11ShaderResourceView* src = a_src;
            int out = -1;
            // look stage: the frame time (its Stabilize time constant; a gap over 0.25 s breaks the history) and the model's input
            // (RenoDX legacy_encode: point sample for Auto / Edge-aware / Classic, linear-light area for "area input"; the
            // pre-look area mean when the stage is off). A change of input restarts NR's history.
            const auto now = std::chrono::steady_clock::now();
            const float lookDt = g_look.lastRun.time_since_epoch().count() == 0 ? 1.0f
                                                                                : std::chrono::duration<float>(now - g_look.lastRun).count();
            g_look.lastRun = now;
            const bool lookOn = c.look && !g_look.failed && !g_lookProgramsFailed;
            const std::uint32_t prepMode = !lookOn ? 0u : c.lookUpsample == 3 ? 2u : 1u;
            static std::uint32_t s_prepMode = 0xFFFFFFFFu;
            if (s_prepMode != prepMode) {
                if (s_prepMode != 0xFFFFFFFFu) {
                    for (auto& fp : g_nr.feat)
                        for (auto& f : fp) f.reset = true;
                    std::memset(g_look.valid, 0, sizeof(g_look.valid));
                    REX::INFO("dlss: NR input {} (look stage {}, upsampling {}): NR history restarted",
                        prepMode == 0 ? "area mean (encoded)" : prepMode == 1 ? "point sample" : "area mean (linear light)", lookOn ? "on" : "off",
                        c.lookUpsample);
                }
                s_prepMode = prepMode;
            }
            for (int pass = 0; pass < c.nrPasses; ++pass) {
                const PassCfg pc = PassOf(c, pass);
                // encode: proxy (+ guides on pass 1)
                for (int e = 0; e < 2; ++e) {
                    const int ti = c.nrPerEye ? e : 0;
                    CsConst k{};
                    k.srcBase[0] = fr.x0[e];
                    k.srcSize[0] = fr.eyeW;
                    k.srcSize[1] = fr.H;
                    k.workBase[0] = c.nrPerEye ? 0 : e * ww;
                    k.workSize[0] = ww;
                    k.workSize[1] = wh;
                    k.pad0[0] = prepMode;
                    SetConst(k);
                    BindCs(g_csPrep, { src, nullptr, nullptr, nullptr, a_mv, g_dw.depthSrv },
                        { g_nr.proxy[ti].uav, g_nr.mv[ti].uav, g_nr.depth[ti].uav });
                    g_ctx->Dispatch((ww + 15) / 16, (wh + 15) / 16, 1);
                }
                UnbindCs();
                if (!NrEvaluate(c, pass, mvsx, mvsy, a_srcJittered, static_cast<float>(fr.jxPx * s), static_cast<float>(fr.jyPx * s),
                        a_forceReset)) {
                    return -1;
                }
                if (pass == 0) ProbeCopy(c);
                // detail stability (pass 1): 0 Auto = on after DLSS (RenoDX: Auto engages on the after path with the game's motion
                // vectors), 1 off, 2 always. With the look stage on it runs inside the look's compose exactly as RenoDX v8.5
                // (log-domain edit, range confidence, per-frame weight fDetailStabilityAlpha); the STABILIZE pass below is the
                // pre-look version for bLook=0.
                const bool detailStab = pass == 0 && (c.nrDetailStability == 2 || (c.nrDetailStability == 0 && !a_srcJittered));
                LookSettings ls{};
                bool lookRun = false, transport = false;
                if (lookOn) {
                    ls = LookOf(c, detailStab);
                    transport = LookTransportWanted(ls, ww, wh, fr.eyeW, fr.H);
                    lookRun = (!LookGainsNeutral(ls) || ls.stabilize != 0 || transport) &&
                              EnsureLook(ww, wh, c.nrPerEye ? ww : ww * 2, fr.eyeW, fr.H, c.nrPerEye ? 2 : 1);
                    const std::string plan = std::format("pass {}: {} (gains {}, bands {}, temporal {}{}, transport {}, input {})", pass + 1,
                        lookRun ? "dispatched" : "not dispatched (N read as is)", LookGainsNeutral(ls) ? "identity" : "shaping",
                        LookBandsWanted(ls) ? "on" : "off", ls.stabilize == 0 ? "off" : ls.stabilize == 1 ? "static" : "motion",
                        ls.stabAlpha > 0 ? std::format(" detail-stability {:.2f}", ls.stabAlpha) : std::format(" {:.0f} ms", ls.stabMs),
                        transport ? "edge-aware" : "classic", prepMode == 2 ? "area" : "point");
                    if (pass == 0 && plan != g_look.lastPlan) {
                        g_look.lastPlan = plan;
                        REX::INFO("dlss: look stage {}", plan);
                    }
                    if (!lookRun) {
                        for (auto& v : g_look.valid[pass]) v = false;
                    }
                }
                if (g_trace.stage == 1 && pass == 0 && EnsureLook(ww, wh, c.nrPerEye ? ww : ww * 2, fr.eyeW, fr.H, c.nrPerEye ? 2 : 1))
                    LookTrace(0, 0);
                const bool stab = !lookOn && detailStab;
                if (stab) {
                    const int nxt = g_nr.histCur ^ 1;
                    const bool valid = g_nr.histValid && !a_forceReset;
                    for (int e = 0; e < 2; ++e) {
                        const int ti = c.nrPerEye ? e : 0;
                        CsConst k{};
                        k.srcSize[0] = c.nrPerEye ? ww : ww * 2;
                        k.srcSize[1] = wh;
                        k.workBase[0] = c.nrPerEye ? 0 : e * ww;
                        k.workSize[0] = ww;
                        k.workSize[1] = wh;
                        k.transfer = c.nrStabilityAlpha;
                        k.color = static_cast<float>(mvsx);
                        k.gate = static_cast<float>(mvsy);
                        k.restoreMode = valid ? 1u : 0u;
                        SetConst(k);
                        BindCs(g_csStabilize, { nullptr, g_nr.proxy[ti].srv, g_nr.neural[ti].srv, g_nr.hist[ti][g_nr.histCur].srv, g_nr.mv[ti].srv },
                            { g_nr.hist[ti][nxt].uav });
                        g_ctx->Dispatch((ww + 15) / 16, (wh + 15) / 16, 1);
                        UnbindCs();
                    }
                    g_nr.histCur = nxt;
                    g_nr.histValid = true;
                } else if (pass == 0) {
                    g_nr.histValid = false;
                }
                // resolve into the ping-pong target
                const int dst = (out + 1) & 1;
                const int restore = c.nrBlackRestore;  // 0 off, 1 auto (hard gate), 2 every block, 3 smooth
                for (int e = 0; e < 2; ++e) {
                    const int ti = c.nrPerEye ? e : 0;
                    CsConst k{};
                    k.srcBase[0] = fr.x0[e];
                    k.srcSize[0] = fr.eyeW;
                    k.srcSize[1] = fr.H;
                    k.workBase[0] = c.nrPerEye ? 0 : e * ww;
                    k.workSize[0] = ww;
                    k.workSize[1] = wh;
                    k.blockBase[0] = e * ((fr.eyeW + 31) / 32);
                    k.transfer = pc.transfer;
                    k.color = pc.color;
                    k.gate = restore == 2 ? 2.0f : 0.061f;
                    k.restoreMode = restore == 0 ? 0u : restore == 3 ? 2u : 1u;
                    k.pad0[0] = stab ? 1u : 0u;
                    if (lookRun) {
                        LookDispatch(ls, pass, e, ti, c.nrPerEye ? 0u : e * ww, fr.x0[e], src, transport, mvsx, mvsy, lookDt, a_forceReset);
                        k.pad0[0] = 0;  // black-level restore: NR's own (unshaped) lift, as RenoDX's MEASURE_UNSHAPED reduce
                    }
                    SetConst(k);
                    ID3D11ShaderResourceView* editSrv = stab ? g_nr.hist[ti][g_nr.histCur].srv : g_nr.neural[ti].srv;
                    if (restore != 0) {
                        BindCs(g_csReduce, { src, g_nr.proxy[ti].srv, editSrv }, { g_nr.blocksUav });
                        g_ctx->Dispatch((fr.eyeW + 31) / 32, (fr.H + 31) / 32, 1);
                        UnbindCs();
                    }
                    if (lookRun && transport) {
                        // N'_up at the eye size: read 1:1 (WorkBase 0, WorkSize = SrcSize), taken whole (Pad0.x 2)
                        k.workBase[0] = 0;
                        k.workSize[0] = fr.eyeW;
                        k.workSize[1] = fr.H;
                        k.pad0[0] = 2;
                        SetConst(k);
                        editSrv = g_look.upN.srv;
                    } else if (lookRun) {
                        editSrv = g_look.out[ti].srv;  // N' with N's layout: the pre-look resolve (bilinear edit N' - P)
                    }
                    BindCs(g_csCompose, { src, g_nr.proxy[ti].srv, editSrv, g_nr.blocksSrv }, { g_dw.ppUav[dst] });
                    g_ctx->Dispatch((fr.eyeW + 15) / 16, (fr.H + 15) / 16, 1);
                    UnbindCs();
                }
                out = dst;
                src = g_dw.ppSrv[dst];
                (void)n;
            }
            return out;
        }

        // ---------------------------------------------------------------------------------------------------------------
        bool   g_postTaaPending = false;
        Frame  g_postFrame{};
        ID3D11ShaderResourceView* g_postMv = nullptr;
        ID3D11Resource* g_postRt = nullptr;
        ID3D11ShaderResourceView* g_postDepth = nullptr;

        std::uint64_t g_lastTaaFrame = 0;
        bool          g_forceReset = true;
        std::chrono::steady_clock::time_point g_lastEvalTime{};
        std::uint64_t g_census0 = 0;
        int           g_logFramesLeft = 0;

        std::string DescOf(ID3D11Resource* r)
        {
            D3D11_TEXTURE2D_DESC d{};
            if (!r || !TexOf(r, &d)) return "-";
            return std::format("{}x{} fmt {} bind 0x{:X} misc 0x{:X}", d.Width, d.Height, static_cast<int>(d.Format), d.BindFlags, d.MiscFlags);
        }
        std::string SrvDesc(ID3D11ShaderResourceView* v)
        {
            if (!v) return "-";
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            v->GetDesc(&vd);
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            std::string s = std::format("{} (view fmt {})", DescOf(r), static_cast<int>(vd.Format));
            SafeRelease(r);
            return s;
        }

        void RequestReset(const char* a_why)
        {
            if (!g_forceReset && g_logFramesLeft <= 0) REX::INFO("dlss: history reset ({})", a_why);
            g_forceReset = true;
            ++g_resets;
        }

        bool NrActive(const Cfg& c) { return c.nrEnabled && !g_nrToggledOff && !g_nr.failed; }
    }

    // ===================================================================================================================
    void Install()
    {
        g_ngxDir = ModuleDir() + L"\\fo4flatvr\\ngx";
        wchar_t la[MAX_PATH]{};
        const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        g_dataDir = (n ? std::wstring(la, n) : ModuleDir()) + L"\\fo4flatvr\\ngx";
        {
            std::scoped_lock l(g_cfgLock);
            g_cfg = LoadCfg();
        }
        const Cfg c = CurCfg();
        g_logFramesLeft = c.logFrames;
        cfgstore::Subscribe("dlss", "DLSS,NeuralRendering", [](const cfgstore::Changes&) {
            const Cfg nc = LoadCfg();
            {
                std::scoped_lock l(g_cfgLock);
                g_cfg = nc;
            }
            g_cfgRev.fetch_add(1);
            REX::INFO("dlss: settings reloaded: DLSS {} perEye {} preset {} jitter {} | NR {} hook {} scale {:.2f} passes {} intensity {:.2f} | "
                      "look {} adjust {} upsample {} stabilize {}",
                nc.enabled, nc.perEye, kPresetNames[nc.preset], nc.jitter, nc.nrEnabled, nc.nrHookPoint ? "before DLSS" : "after DLSS",
                nc.nrScale, nc.nrPasses, nc.p1.intensity, nc.look, nc.lookMode, nc.lookUpsample, nc.lookStabilize);
        });
        REX::INFO("dlss: installed: DLSS {} (per eye {}, preset {}, jitter {}), NR {} (scale {:.2f}, per eye {}); ngx dir '{}'", c.enabled,
            c.perEye, kPresetNames[c.preset], c.jitter, c.nrEnabled, c.nrScale, c.nrPerEye, std::filesystem::path(g_ngxDir).string());
        // preload the NR runtime off the render thread (166 MB, mostly the model weights)
        std::thread([] {
            const std::wstring p = g_ngxDir + L"\\nvngx_dlssnr.dll";
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                const auto t0 = std::chrono::steady_clock::now();
                HMODULE m = ::LoadLibraryW(p.c_str());
                const DWORD err = m ? 0 : ::GetLastError();
                g_preloaded.store(m);
                REX::INFO("dlss: nvngx_dlssnr.dll preload {} ({} ms{})", m ? "ok" : "FAILED",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                    m ? "" : std::format(", error {}", err));
            } else {
                REX::WARN("dlss: {} not found: neural rendering unavailable", std::filesystem::path(p).string());
            }
            g_preloadDone.store(true);
        }).detach();
    }

    bool WantsTaa()
    {
        const Cfg c = CurCfg();
        const bool dlssUsable = c.enabled && !g_ngxFailed && !g_dlssFailed && !g_csFailed;
        const bool nrUsable = NrActive(c) && !g_csFailed;
        return dlssUsable || nrUsable;
    }

    bool WantsJitter()
    {
        // DLSS on: its switch. NR alone (the engine's TAA draws, NR runs on its result): the engine's TAA keeps its jitter.
        const Cfg c = CurCfg();
        const bool dlssUsable = c.enabled && !g_ngxFailed && !g_dlssFailed;
        return dlssUsable ? c.jitter : true;
    }

    bool OnTaaDraw(const TaaDraw& a)
    {
        const Cfg c = CurCfg();
        const bool dlssOn = c.enabled && !g_ngxFailed && !g_dlssFailed;
        const bool nrOn = NrActive(c);
        if ((!dlssOn && !nrOn) || !a.ctx || !EnsureD3D11(a.ctx)) return false;
        if (g_lastTaaFrame == g_frame && g_frame != 0) return false;  // one main-view TAA per frame
        if (g_lastTaaFrame + 2 < g_frame) RequestReset("TAA resumed");
        g_lastTaaFrame = g_frame;
        if (!g_firstSeenFrame) g_firstSeenFrame = g_frame;
        if (c.createDelay > 0 && g_frame < g_firstSeenFrame + static_cast<std::uint64_t>(c.createDelay)) return false;

        // inputs of the engine's TAA draw
        ID3D11ShaderResourceView* srv[5]{};
        a.ctx->PSGetShaderResources(0, 5, srv);
        ID3D11RenderTargetView* rtv[2]{};
        a.ctx->OMGetRenderTargets(2, rtv, nullptr);
        struct Rel
        {
            ID3D11ShaderResourceView** s;
            ID3D11RenderTargetView**   r;
            ~Rel()
            {
                for (int i = 0; i < 5; ++i) SafeRelease(s[i]);
                for (int i = 0; i < 2; ++i) SafeRelease(r[i]);
            }
        } rel{ srv, rtv };
        if (!srv[0] || !srv[2] || !srv[3] || !rtv[0]) return false;
        ID3D11Resource* colRes = nullptr;
        ID3D11Resource* mvRes = nullptr;
        ID3D11Resource* rt0 = nullptr;
        ID3D11Resource* rt1 = nullptr;
        srv[0]->GetResource(&colRes);
        srv[2]->GetResource(&mvRes);
        rtv[0]->GetResource(&rt0);
        if (rtv[1]) rtv[1]->GetResource(&rt1);
        struct RelR
        {
            ID3D11Resource** r[4];
            ~RelR()
            {
                for (auto* p : r) SafeRelease(*p);
            }
        } relr{ { &colRes, &mvRes, &rt0, &rt1 } };
        D3D11_TEXTURE2D_DESC dc{}, dr{}, dm{}, dr1{};
        if (!TexOf(colRes, &dc) || !TexOf(rt0, &dr) || !TexOf(mvRes, &dm)) return false;
        if (rt1) TexOf(rt1, &dr1);
        {
            std::string census = std::format("t0 colour {} | t1 {} | t2 mv {} | t3 depth {} | t4 mask {} | rt0 {} | rt1 {}", SrvDesc(srv[0]),
                SrvDesc(srv[1]), SrvDesc(srv[2]), SrvDesc(srv[3]), SrvDesc(srv[4]), DescOf(rt0), rt1 ? DescOf(rt1) : "-");
            if (census != g_census) {
                g_census = census;
                REX::INFO("dlss: TAA draw inputs: {} | seam pad {} full width {} | mv fix {} sign {}", census, a.seamPad, a.fullWidth,
                    a.mvFixed, a.mvSign);
            }
        }
        Frame fr;
        fr.W = dr.Width;
        fr.H = dr.Height;
        const unsigned pad = static_cast<unsigned>(std::lround(a.seamPad));
        if (dc.Width != fr.W || dc.Height != fr.H || std::abs(a.fullWidth - fr.W) > 0.5 || fr.W / 2 <= pad + 64) return false;
        fr.eyeW = fr.W / 2 - pad;
        fr.x0[0] = pad / 2;
        fr.x0[1] = fr.W / 2 + pad / 2;
        // This frame's TAA jitter, from BSGraphics::State (3D871A0): +4/+8 = (2*Halton(2,3) - 1) / width|height, set once per
        // frame by DrawWorld::Begin (aid_2277095 182D580, 8 phases; 0 while +0xAC == 0 or the menu counter +0xB0 > 0); the
        // world camera's cached projection adds +4 / [2F4E708], +8 / [2F4E70C] to clip x / y (182D750) when the camera is
        // built with jitter. (The stereo path's last-applied-camera value, a.jitterX, is the frame's last camera application,
        // often an unjittered one: measured 0 every frame while |VP - VPunj| reached 2.7e-4.)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
            const float dx = *reinterpret_cast<const float*>(base + 0x2F4E708), dy = *reinterpret_cast<const float*>(base + 0x2F4E70C);
            const float s4 = *reinterpret_cast<const float*>(base + 0x3D871A0 + 4), s8 = *reinterpret_cast<const float*>(base + 0x3D871A0 + 8);
            const bool jit = WantsJitter();
            g_lastJx = (jit && dx != 0.0f) ? s4 / dx : 0.0;
            g_lastJy = (jit && dy != 0.0f) ? s8 / dy : 0.0;
        }
        const double sx = c.jitterFlipX ? -1.0 : 1.0, sy = c.jitterFlipY ? -1.0 : 1.0;
        fr.jxPx = sx * g_lastJx * fr.W * 0.5;
        fr.jyPx = -sy * g_lastJy * fr.H * 0.5;
        // the engine's jitter sequence (once): phase count and range, for the log
        if (!g_jitLogged && (g_lastJx != 0.0 || g_lastJy != 0.0)) {
            g_jitSeen.emplace_back(static_cast<float>(fr.jxPx), static_cast<float>(fr.jyPx));
            if (g_jitSeen.size() >= 64) {
                std::set<std::pair<int, int>> distinct;
                float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
                for (auto [x, y] : g_jitSeen) {
                    distinct.insert({ static_cast<int>(std::lround(x * 1000)), static_cast<int>(std::lround(y * 1000)) });
                    mnx = (std::min)(mnx, x), mxx = (std::max)(mxx, x), mny = (std::min)(mny, y), mxy = (std::max)(mxy, y);
                }
                REX::INFO("dlss: engine jitter over 64 frames: {} distinct offsets, x [{:.3f}, {:.3f}] px, y [{:.3f}, {:.3f}] px (DLSS input, "
                          "signs applied)", distinct.size(), mnx, mxx, mny, mxy);
                g_jitLogged = true;
            }
        }

        // output format: RT0's family with a typed UAV
        DXGI_FORMAT outFmt = TypedOf(dr.Format);
        UINT sup = 0;
        if (FAILED(g_dev->CheckFormatSupport(outFmt, &sup)) || !(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
            static bool logged = false;
            if (!logged) REX::ERROR("dlss: TAA target format {} has no typed UAV form: DLSS off", static_cast<int>(dr.Format));
            logged = true;
            g_dlssFailed = true;
            return false;
        }
        if (IsSrgb(dr.Format) != IsSrgb(dc.Format)) {
            static bool logged = false;
            if (!logged) REX::WARN("dlss: colour input fmt {} and TAA target fmt {} differ in sRGB-ness (colour read as stored)",
                static_cast<int>(dc.Format), static_cast<int>(dr.Format));
            logged = true;
        }
        if (dlssOn && !EnsureNgx()) return false;
        if (!EnsureDw(fr.W, fr.H, outFmt, nrOn)) return false;
        if (dlssOn) {
            const unsigned fw = c.perEye ? fr.eyeW : fr.W - pad;
            if (c.warmupRebuild > 0 && !g_warmupDone && g_evalOk > static_cast<std::uint64_t>(c.warmupRebuild)) {
                g_warmupDone = true;
                ++g_rebuildSerial;
                REX::INFO("dlss: warm-up rebuild after {} evaluates", c.warmupRebuild);
            }
            if (!EnsureFeatures(c, fw, fr.H)) {
                g_dlssFailed = true;
                REX::ERROR("dlss: feature creation failed: DLSS off for this session (engine TAA / bCutTaa again)");
                return false;
            }
        }
        if (c.resetEvery > 0 && g_frame % static_cast<std::uint64_t>(c.resetEvery) == 0) RequestReset("iResetEvery");
        static int s_lastRev = -1;
        if (s_lastRev != g_cfgRev.load()) {
            s_lastRev = g_cfgRev.load();
            RequestReset("settings");
        }

        if (!dlssOn) {
            // NR only: the engine's TAA draws, the neural pass runs on its result (AfterDraw)
            g_postTaaPending = true;
            g_postFrame = fr;
            SafeRelease(g_postMv);
            SafeRelease(g_postDepth);
            SafeRelease(g_postRt);
            g_postMv = srv[2];
            g_postMv->AddRef();
            g_postDepth = srv[3];
            g_postDepth->AddRef();
            g_postRt = rt0;
            g_postRt->AddRef();
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        float dtMs = g_lastEvalTime.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(now - g_lastEvalTime).count() : 11.0f;
        g_lastEvalTime = now;
        dtMs = std::clamp(dtMs, 1.0f, 200.0f);
        const bool forceReset = std::exchange(g_forceReset, false);
        bool ok = true;
        int nrOut = -1;
        Isolated([&] {
            TimerBegin(c.gpuTimers);
            // depth -> R32F (NGX gets a typed float depth; the engine's is a typeless depth-stencil)
            {
                CsConst k{};
                k.srcSize[0] = fr.W;
                k.srcSize[1] = fr.H;
                SetConst(k);
                BindCs(g_csDepth, { nullptr, nullptr, nullptr, nullptr, nullptr, srv[3] }, { nullptr, nullptr, g_dw.depthUav });
                g_ctx->Dispatch((fr.W + 15) / 16, (fr.H + 15) / 16, 1);
                UnbindCs();
            }
            // colour input: a typed copy when the engine's texture is typeless
            ID3D11Resource* colour = colRes;
            ID3D11ShaderResourceView* colourSrv = srv[0];
            if (TypedOf(dc.Format) != dc.Format && !IsSrgb(dc.Format)) {
                if (!g_dw.colTyped) MakeTex(fr.W, fr.H, TypedOf(dc.Format), D3D11_BIND_SHADER_RESOURCE, &g_dw.colTyped, &g_dw.colTypedSrv, nullptr);
                if (g_dw.colTyped) {
                    g_ctx->CopyResource(g_dw.colTyped, colRes);
                    colour = g_dw.colTyped;
                    colourSrv = g_dw.colTypedSrv;
                }
            }
            bool nrBefore = nrOn && c.nrHookPoint == 1;
            if (nrBefore) {
                const int r = RunNr(c, fr, colourSrv, srv[2], true, forceReset);
                if (r >= 0) {
                    colour = g_dw.pp[r];
                } else {
                    nrBefore = false;
                }
            }
            TimerMark(1);
            // DLSS per eye (or one shared feature over both eyes and the guard between them)
            const int n = g_featCount;
            for (int e = 0; e < n && ok; ++e) {
                auto& f = g_feat[e];
                NVSDK_NGX_D3D11_DLSS_Eval_Params ev{};
                ev.Feature.pInColor = colour;
                ev.Feature.pInOutput = g_dw.out;
                ev.Feature.InSharpness = c.sharpness;
                ev.pInDepth = g_dw.depth;
                ev.pInMotionVectors = mvRes;
                ev.InJitterOffsetX = static_cast<float>(fr.jxPx);
                ev.InJitterOffsetY = static_cast<float>(fr.jyPx);
                const unsigned x = n == 2 ? fr.x0[e] : fr.x0[0];
                const unsigned w = n == 2 ? fr.eyeW : fr.W - pad;
                ev.InRenderSubrectDimensions = { w, fr.H };
                ev.InColorSubrectBase = { x, 0 };
                ev.InDepthSubrectBase = { x, 0 };
                ev.InMVSubrectBase = { x, 0 };
                ev.InOutputSubrectBase = { x, 0 };
                ev.InBiasCurrentColorSubrectBase = { x, 0 };
                ID3D11Resource* mask = nullptr;
                if (c.biasMask && srv[4]) {
                    srv[4]->GetResource(&mask);
                    ev.pInBiasCurrentColorMask = mask;
                }
                ev.InReset = (f.reset || forceReset) ? 1 : 0;
                ev.InMVScaleX = static_cast<float>(fr.W * c.mvScaleX);
                ev.InMVScaleY = static_cast<float>(fr.H * c.mvScaleY);
                ev.InPreExposure = 1.0f;
                ev.InExposureScale = 1.0f;
                ev.InFrameTimeDeltaInMsec = dtMs;
                const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(g_ctx, f.h, f.p, &ev);
                SafeRelease(mask);
                if (NVSDK_NGX_FAILED(r)) {
                    ++g_evalFail;
                    if (g_evalFail <= 10) REX::ERROR("dlss: evaluate eye {} -> {}", n == 2 ? e : -1, Res(r));
                    ok = false;
                } else {
                    ++g_evalOk;
                    f.reset = false;
                    if (g_logFramesLeft > 0) {
                        REX::INFO("dlss: evaluate eye {} ok: subrect x {} w {} h {} jitter ({:.3f}, {:.3f}) px mv scale ({:.0f}, {:.0f}) reset {} "
                                  "dt {:.2f} ms", n == 2 ? e : -1, x, w, fr.H, fr.jxPx, fr.jyPx, ev.InMVScaleX, ev.InMVScaleY, ev.InReset, dtMs);
                    }
                }
            }
            TimerMark(2);
            if (ok && nrOn && !nrBefore && c.nrHookPoint == 0) nrOut = RunNr(c, fr, g_dw.outSrv, srv[2], false, forceReset);
            if (ok) {
                ID3D11Texture2D* fin = nrOut >= 0 ? g_dw.pp[nrOut] : g_dw.out;
                for (int e = 0; e < 2; ++e) {
                    D3D11_BOX box{ fr.x0[e], 0, 0, fr.x0[e] + fr.eyeW, fr.H, 1 };
                    g_ctx->CopySubresourceRegion(rt0, 0, fr.x0[e], 0, 0, fin, 0, &box);
                    // the engine's TAA history target too (when it is one we can copy into): DLSS off again resumes cleanly
                    if (c.copyHistory && rt1 && dr1.Width == fr.W && dr1.Height == fr.H && TypedOf(dr1.Format) == outFmt)
                        g_ctx->CopySubresourceRegion(rt1, 0, fr.x0[e], 0, 0, fin, 0, &box);
                }
            }
            if (ok && g_stab.stage >= 1 && g_stab.stage <= 2) {
                D3D11_BOX box{ fr.x0[0], 0, 0, fr.x0[0] + fr.eyeW, fr.H, 1 };
                StabCopy(nrOut >= 0 ? g_dw.pp[nrOut] : g_dw.out, colRes, dc.Format, box, outFmt);
            }
            TimerEnd(nrOut >= 0);
        });
        if (g_logFramesLeft > 0) --g_logFramesLeft;
        if (!ok && g_evalFail >= 30 && g_evalOk == 0) {
            g_dlssFailed = true;
            REX::ERROR("dlss: evaluates keep failing: DLSS off for this session (engine TAA / bCutTaa again)");
        }
        if (ok) g_lastEvalFrame = g_frame;
        return ok;
    }

    void OnAfterTaaDraw(ID3D11DeviceContext* a_ctx)
    {
        if (!g_postTaaPending) return;
        g_postTaaPending = false;
        const Cfg c = CurCfg();
        if (!NrActive(c) || a_ctx != g_ctx || !g_postRt) return;
        const bool forceReset = std::exchange(g_forceReset, false);
        const Frame fr = g_postFrame;
        Isolated([&] {
            TimerBegin(c.gpuTimers);
            CsConst k{};
            k.srcSize[0] = fr.W;
            k.srcSize[1] = fr.H;
            SetConst(k);
            BindCs(g_csDepth, { nullptr, nullptr, nullptr, nullptr, nullptr, g_postDepth }, { nullptr, nullptr, g_dw.depthUav });
            g_ctx->Dispatch((fr.W + 15) / 16, (fr.H + 15) / 16, 1);
            UnbindCs();
            // the engine's TAA output -> our copy (its target is bound as a render target; read a copy)
            g_ctx->CopyResource(g_dw.out, g_postRt);
            TimerMark(1);
            TimerMark(2);
            const int r = RunNr(c, fr, g_dw.outSrv, g_postMv, false, forceReset);
            if (r >= 0) {
                for (int e = 0; e < 2; ++e) {
                    D3D11_BOX box{ fr.x0[e], 0, 0, fr.x0[e] + fr.eyeW, fr.H, 1 };
                    g_ctx->CopySubresourceRegion(g_postRt, 0, fr.x0[e], 0, 0, g_dw.pp[r], 0, &box);
                }
            }
            TimerEnd(r >= 0);
        });
        SafeRelease(g_postMv);
        SafeRelease(g_postDepth);
        SafeRelease(g_postRt);
    }

    void OnPresent()
    {
        ++g_frame;
        if (!g_ctx) return;
        const Cfg c = CurCfg();
        TimerCollect();
        NrTimerCollect();
        ProbeTick();
        StabTick();
        TraceTick();
        // the NR toggle key (RenoDX's NRToggleKey, F6 by default), only while the game window has focus
        if (c.nrToggleKey > 0) {
            static bool s_down = false;
            DWORD pid = 0;
            ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
            const bool down = pid == ::GetCurrentProcessId() && (::GetAsyncKeyState(c.nrToggleKey) & 0x8000) != 0;
            if (down && !s_down) {
                g_nrToggledOff = !g_nrToggledOff;
                RequestReset("NR toggle key");
                REX::INFO("dlss: neural rendering toggled {} (key 0x{:X})", g_nrToggledOff ? "off" : "on", c.nrToggleKey);
            }
            s_down = down;
        }
        if (g_frame >= g_lastLogFrame + 600 && (g_nTimed > 0 || g_nNr12 > 0)) {
            g_lastLogFrame = g_frame;
            REX::INFO("dlss: gpu ms (avg over {} frames): ours total {:.3f} (max {:.3f}) | dlaa {:.3f} | nr incl. cross-API wait {:.3f} | nr d3d12 "
                      "{:.3f} (eye0 {:.3f} eye1 {:.3f}, {} frames) | evaluates ok {} fail {} | nr ok {} fail {} | resets {}",
                g_nTimed, g_nTimed ? g_sumTotal / g_nTimed : 0.0, g_maxTotal, g_nTimed ? g_sumDlss / g_nTimed : 0.0,
                g_nTimed ? g_sumNr / g_nTimed : 0.0, g_nNr12 ? g_sumNr12 / g_nNr12 : 0.0, g_nNr12 ? g_sumNr12Eye[0] / g_nNr12 : 0.0,
                g_nNr12 ? g_sumNr12Eye[1] / g_nNr12 : 0.0, g_nNr12, g_evalOk, g_evalFail, g_nrOk, g_nrFail, g_resets);
            g_sumTotal = g_sumDlss = g_sumNr = g_sumNr12 = g_sumNr12Eye[0] = g_sumNr12Eye[1] = 0;
            g_nTimed = g_nNr12 = 0;
            g_maxTotal = 0;
        }
    }

    std::string Status()
    {
        const Cfg c = CurCfg();
        std::string probe;
        {
            std::scoped_lock l(g_probeLock);
            probe = g_probeResult;
        }
        // BSGraphics::State (3D871A0): +0 jitter phase, +4/+8 jitter (aid_2277095 182D580: Halton(2,3) x8, only while +0xAC != 0
        // and +0xB0 == 0), +0xAC TAA on, +0xB0 menu counter (PauseMenu & co. increment it: no jitter while > 0)
        const auto* st = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr)) + 0x3D871A0;
        const std::string state = std::format("State taa={} menuHold={} phase={} jitter=({:.6f}, {:.6f})", *reinterpret_cast<const int*>(st + 0xAC),
            *reinterpret_cast<const int*>(st + 0xB0), *reinterpret_cast<const int*>(st), *reinterpret_cast<const float*>(st + 4),
            *reinterpret_cast<const float*>(st + 8)) +
            std::format(" jitterDiv=({}, {}) lastJitterClip=({:.3g}, {:.3g})",
                *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr)) + 0x2F4E708),
                *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr)) + 0x2F4E70C),
                g_lastJx, g_lastJy);
        return state + " | " + std::format("dlss: enabled={} ngx={} failed={}{} features={} {}x{} preset={} quality={}{} perEye={} jitter={} evals ok={} fail={} "
                           "resets={} lastEvalFrame={} frame={} | nr: enabled={} toggledOff={} ready={} failed={}{} work={}x{} passes={} "
                           "hook={} perEye={} ok={} fail={} | look: {} {} | edit trace: {} | nrAvailCap={} | census: {} | nrstereo: {}",
            c.enabled ? 1 : 0, g_ngxInit ? 1 : 0, g_ngxFailed || g_dlssFailed ? 1 : 0, g_ngxError.empty() ? "" : " (" + g_ngxError + ")",
            g_featCount, g_featKey.w, g_featKey.h, kPresetNames[c.preset], kQualityNames[c.quality], g_qualityFellBack ? " (fell back to DLAA)" : "",
            c.perEye ? 1 : 0, c.jitter ? 1 : 0, g_evalOk, g_evalFail, g_resets, g_lastEvalFrame, g_frame, c.nrEnabled ? 1 : 0,
            g_nrToggledOff ? 1 : 0, g_nr.ready ? 1 : 0, g_nr.failed ? 1 : 0, g_nr.error.empty() ? "" : " (" + g_nr.error + ")", g_nr.workW,
            g_nr.workH, c.nrPasses, c.nrHookPoint ? "before" : "after", c.nrPerEye ? 1 : 0, g_nrOk, g_nrFail,
            !c.look ? "off (bLook=0)" : g_lookProgramsFailed ? "programs unavailable" : g_look.failed ? "FAILED (surfaces)" : "on",
            g_look.lastPlan, g_trace.result, g_nrAvailCap, g_census, probe);
    }

    std::string Command(const std::string& a_arg)
    {
        std::string a = a_arg;
        while (!a.empty() && a.back() == ' ') a.pop_back();
        if (a.empty() || a == "status") return Status();
        if (a == "reset") {
            RequestReset("pipe");
            return "reset requested";
        }
        if (a == "rebuild") {
            ++g_rebuildSerial;
            g_nr.featPreset = -1;  // NR features too (released at the next NR pass)
            return "rebuild requested";
        }
        if (a == "nr retry") {
            g_nr.failed = false;
            g_nr.error.clear();
            return "NR retry armed";
        }
        if (a == "nrstereo") {
            if (!g_nr.ready) return "error: NR not running";
            g_probe.stage = 1;
            return "requested (result in the log line 'dlss: nrstereo' and in 'dlss status')";
        }
        if (a.rfind("stability", 0) == 0) {
            if (g_stab.stage) return "busy";
            g_stab.tag = a.size() > 10 ? a.substr(10) : std::string("-");
            g_stab.stage = 1;
            return "requested (log line 'dlss: stability')";
        }
        if (a == "trace") {
            if (!g_nr.ready) return "error: NR not running";
            if (g_lookProgramsFailed) return "error: look stage programs unavailable";
            if (g_trace.stage) return "busy";
            g_trace.stage = 1;
            return "requested (log line 'dlss: edit trace', last result in 'dlss status')";
        }
        if (a == "log") {
            g_logFramesLeft = (std::max)(1, CurCfg().logFrames);
            return std::format("logging the next {} frames", g_logFramesLeft);
        }
        return "error: dlss [status|reset|rebuild|nr retry|nrstereo|stability <tag>|trace|log]";
    }
}
