// VATS camera window, game side (VatsWindow.h). Engine calls only; the D3D work is in VatsWindowGpu.cpp.
#include "plugin/VatsWindow.h"
#include "plugin/VatsWindowInternal.h"
#include "plugin/Settings.h"
#include "plugin/StereoPoC.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <Windows.h>
#include <excpt.h>
#undef ERROR
#undef near
#undef far
#include <intrin.h>
#include <TlHelp32.h>

namespace fo4flatvr::vatswin
{
    namespace detail
    {
        Shared& S()
        {
            static Shared s;
            return s;
        }
    }

    namespace
    {
        using detail::S;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // ---- 1.11.240 RVAs (Ghidra, this round; analysis/second-camera-render.md section 7) ----
        // hook sites
        constexpr std::uintptr_t kEndCall = 0xC383F6;         // Main::DrawWorld_And_UI: call DrawWorld::End
        constexpr std::uintptr_t kDrawWorldEnd = 0x21FA3B0;
        constexpr std::uintptr_t kLabelCall1 = 0xB1E319;      // VatsMenuObj::UpdateState: off-screen test camera
        constexpr std::uintptr_t kLabelCall2 = 0xB1E9C1;      // VatsMenuObj::UpdateState: label placement camera
        constexpr std::uintptr_t kWorldRootCamera = 0xC37DD0; // Main::WorldRootCamera (returns *(WorldRootNode+0x140))
        constexpr std::uintptr_t kBaseCamCall = 0xB180D5;     // VatsMenuObj::Update: call VATSCamera::BaseCamera::Update
        constexpr std::uintptr_t kBaseCamCall2 = 0xB15CB1;    // VATSMenu::AdvanceMovie (inlined VatsMenuObj::Update): the same call
        constexpr std::uintptr_t kNiAVObjectUpdate = 0x16C8570;  // NiAVObject::Update(NiUpdateData&)
        constexpr std::uintptr_t kBaseCamUpdate = 0x832B00;   //   (writes the PlayerCamera root's local rotation at 832DB6)
        // functions
        constexpr std::uintptr_t kNiCameraCtor = 0x16D0A30;
        constexpr std::uintptr_t kAccCtor = 0x21D45C0;        // BSShaderAccumulator(BATCHRENDERER_CREATION_MODE)
        constexpr std::uintptr_t kCullCtor = 0x17E1B20, kCullDtor = 0x17E1BA0, kCullSetAcc = 0x17E1C10;
        constexpr std::uintptr_t kProcessQueuedLights = 0x21BA2E0;
        constexpr std::uintptr_t kAccumulateScene = 0x21C3830;
        constexpr std::uintptr_t kRenderSceneDeferred = 0x21C3ED0;
        constexpr std::uintptr_t kClearRenderPasses = 0x21C8CA0, kClearActivePasses = 0x21D6DE0, kClearGroupPasses = 0x21D67B0;
        constexpr std::uintptr_t kResetZPrePass = 0x181F780, kFlush = 0x1818640, kSetClearColor = 0x1818650;
        constexpr std::uintptr_t kSetRT = 0x1839B70, kSetDS = 0x1839C80, kVpDefault = 0x1839E00, kCreateRT = 0x1839130;
        constexpr std::uintptr_t kCacheCameraData = 0x182BF20, kSetCameraData = 0x182BC80;
        constexpr std::uintptr_t kDistantQInstance = 0x3738C0;
        constexpr std::uintptr_t kSettingUpdated = 0x179FE40;
        constexpr std::uintptr_t kRenderMask = 0x37A0E0;       // VatsEffectControl::RenderMask(NiCamera*)
        constexpr std::uintptr_t kGetLight = 0x21BBC00, kGetShadowLight = 0x21BBC20;  // ShadowSceneNode::GetLight / GetShadowLight
        // data
        constexpr std::uintptr_t kRtm = 0x2F4D750;             // BSGraphics::RenderTargetManager::Singleton
        constexpr std::uintptr_t kRenderer = 0x3A25D00;        // BSGraphics::Renderer (aid 2704525)
        constexpr std::uintptr_t kGfxState = 0x3D871A0;        // BSGraphics::State::Singleton
        constexpr std::uintptr_t kRendererData = 0x38E1398;    // aid 2704428: fallback TLS render state
        constexpr std::uintptr_t kTlsIndex = 0x3E73018;        // the exe's _tls_index
        constexpr std::uintptr_t kWorldSsn = 0x3E5DF60;        // BSShaderManager::State::shadowSceneNode[0]
        constexpr std::uintptr_t kDeferredRgbEmit = 0x3E5DFB6; // State+0x56: GBuffer RT 0x1F in use
        constexpr std::uintptr_t kCurCamera = 0x3E5DE48;       // aid 2712461: camera the shader state was set from
        constexpr std::uintptr_t kSaoDispatchFlag = 0x3E625F0; // aid 2712999 (DrawModel clears it around its render)
        constexpr std::uintptr_t kHbaoSetting = 0x2F4D268;     // bEnable:NVHBAO (value byte +8)
        constexpr std::uintptr_t kIsmSingleton = 0x3E5E4B0;    // ImageSpaceManager*
        constexpr std::uintptr_t kRtHandles = 0xDF4;           // RenderTargetManager+0xDF4: u32 handle per RT id
        constexpr int            kRtCount = 101;

        // expected bytes at the hook sites (E8 rel32 to the known targets; checked with the target computed)
        struct CallSite
        {
            std::uintptr_t site, target;
            const char*    name;
        };
        constexpr CallSite kSites[] = { { kEndCall, kDrawWorldEnd, "R1 DrawWorld::End call C383F6" },
            { kLabelCall1, kWorldRootCamera, "L1 UpdateState WorldRootCamera B1E319" },
            { kLabelCall2, kWorldRootCamera, "L2 UpdateState WorldRootCamera B1E9C1" },
            { kBaseCamCall, kBaseCamUpdate, "P1 VatsMenuObj::Update BaseCamera::Update B180D5" },
            { kBaseCamCall2, kBaseCamUpdate, "P2 VATSMenu::AdvanceMovie BaseCamera::Update B15CB1" } };

        using VoidFn = void (*)();
        using CamFn = RE::NiCamera* (*)();
        using Ctor1 = void* (*)(void*);
        using AccCtor = void* (*)(void*, int);
        using CullCtor = void* (*)(void*, void*);
        using CullDtor = void (*)(void*);
        using CullSetAcc = void (*)(void*, void*);
        using ProcessLights = void (*)(void*, void*);
        using Accumulate = void (*)(void*, void*, void*, bool);
        using RenderDeferred = void (*)(void*, void*, void*, void*, int, int, bool, bool);
        using ObjFn = void (*)(void*);
        using AccIntFn = void (*)(void*, int);
        using RendererFn = void (*)(void*);
        using ClearColorFn = void (*)(void*, float, float, float, float);
        using SetRTFn = void (*)(void*, unsigned, int, int);
        using SetDSFn = void (*)(void*, int, int, int, bool);
        using RtmFn = void (*)(void*);
        using CreateRTFn = void (*)(void*, int, const void*, std::uint8_t);
        using CacheCamFn = void (*)(void*, void*, bool);
        using SetCamFn = void (*)(void*, void*, bool, float, float);
        using QInstanceFn = std::uint8_t* (*)();
        using SettingFn = void (*)(void*);
        using MaskFn = int (*)(void*);
        using GetLightFn = std::uint8_t* (*)(void*, unsigned);

        template <class T>
        T Fn(std::uintptr_t a_rva) { return reinterpret_cast<T>(Base() + a_rva); }

        // ---- configuration ----
        struct Config
        {
            // [Menus] bVatsCameraWindow: default 1 since 15 Nov ~03:40 (milestone C verified; the +2.2 deg VATS pitch of the world
            // view - VATSCamera::BaseCamera::Update writing the camera root, rva 832DB6 - is undone by P1/P2)
            bool  on{ true };
            int   every{ 3 };        // render every n-th frame (VATS runs the world at x0.04)
            int   scene{ 0 };        // 0 = target only (milestone C), 1 = full scene (milestone B)
            float fovDeg{ 0.0f };    // vertical FOV; 0 = fit the target
            float fitScale{ 1.6f };  // auto FOV: the target's bound diameter x this fills the window height
            float aimTauS{ 0.08f };  // aim smoothing time constant (inside the window only)
            float aimPart{ 0.35f };  // aim point: bound centre -> selected part by this fraction (0 = whole target framed)
        };
        Config            g_cfg;
        std::atomic<bool> g_enabled{ false };   // switch on (runtime) and hooks installed
        bool              g_installed = false;
        std::string       g_installNote{ "not installed" };
        std::atomic<bool> g_active{ false };    // the window is the shown view (Presenter)
        std::atomic<int>  g_every{ 3 }, g_scene{ 0 };
        std::atomic<float> g_fovDeg{ 0.0f };
        std::atomic<bool> g_keepLists{ true };  // experiment: ssn+0x225 held at 1 around the render (LocalMap)
        std::atomic<bool> g_fxOff{ true };      // experiment: effects [69]/[70]/[71] off around the render (DrawModel)
        // [Menus] iVatsWindowSun: 0 = no sun in the window; 1 = sun lit with the main view's shadow map (its cascades are not
        // re-fitted or re-rendered for our camera: no shadow-map work, main-view shadows untouched); 2 = engine behaviour
        // (RenderSceneDeferred re-fits and re-renders the sun's shadow map for our camera)
        std::atomic<int>  g_sun{ 0 };

        // per-object vtable for the sun during our render (mode 1): slot 0x70 (fit to camera) -> true, 0x48 (accumulate
        // casters) and 0x50 (render the shadow map) -> nothing; RTTI locator copied (vtbl[-1])
        constexpr int kVtSlots = 96;
        void*  g_sunVtOrig = nullptr;
        void*  g_sunVt[kVtSlots + 1]{};
        bool   SunFitStub(void*, void*) { return true; }
        void   SunAccStub(void*, void*) {}
        void   SunRenderStub(void*, int) {}
        void** SunVtable(void* a_orig)
        {
            if (a_orig != g_sunVtOrig) {
                auto** o = static_cast<void**>(a_orig);
                std::memcpy(g_sunVt, o - 1, sizeof(void*) * (kVtSlots + 1));
                g_sunVt[1 + 0x70 / 8] = reinterpret_cast<void*>(&SunFitStub);
                g_sunVt[1 + 0x48 / 8] = reinterpret_cast<void*>(&SunAccStub);
                g_sunVt[1 + 0x50 / 8] = reinterpret_cast<void*>(&SunRenderStub);
                g_sunVtOrig = a_orig;
            }
            return g_sunVt + 1;
        }

        VoidFn g_origEnd = nullptr;
        using BaseCamUpdFn = void (*)(void*, float);
        BaseCamUpdFn g_origBaseCam = nullptr;
        std::atomic<std::uint64_t> g_baseCamCalls{ 0 }, g_baseCamUndone{ 0 };
        CamFn  g_origCam1 = nullptr, g_origCam2 = nullptr;
        REL::Trampoline g_tramp{ "fo4flatvr vatswin" };

        // ---- camera pose, published by the label hook (UI update) and read by the render hook ----
        struct Pose
        {
            float         rot[3][3]{};    // world rotate rows (NiMatrix3 entry[r][c]): row 0 dir, 1 up, 2 right
            float         pos[3]{};
            float         l{}, r{}, t{}, b{}, n{ 5.0f }, f{ 10000.0f };
            std::uint64_t seq{ 0 };
            bool          valid{ false };
        };
        std::mutex                     g_poseLock;
        Pose                           g_pose;
        RE::NiPointer<RE::NiAVObject>  g_target;  // target 3D root (under g_poseLock)
        std::uint32_t                  g_targetId{ 0 };

        // label-side state (UI thread)
        RE::NiCamera* g_labelCam = nullptr;   // own NiCamera for the label projection (kept alive, refcount 1)
        float         g_aim[3]{};
        bool          g_aimValid = false;
        std::uint32_t g_aimTarget = 0;
        LARGE_INTEGER g_aimLast{};
        std::atomic<std::uint64_t> g_labelCalls{ 0 }, g_labelOurs{ 0 };
        std::string   g_labelNote{ "-" };      // last aim line (UI thread; read under g_poseLock)

        // render-side state (render thread)
        RE::NiCamera* g_renderCam = nullptr;
        void*         g_acc = nullptr;        // BSShaderAccumulator (kept alive)
        int           g_rtId = -1;
        std::uint64_t g_frame = 0, g_lastSeq = 0;
        std::atomic<std::uint64_t> g_hookCalls{ 0 }, g_renderFails{ 0 }, g_skips{ 0 };
        std::atomic<double> g_cpuMsAvg{ 0.0 }, g_cpuMsMax{ 0.0 };
        std::string   g_rtNote{ "none" };
        std::string   g_lastErr;

        float Clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

        // ---- engine object creation ----
        RE::NiCamera* NewCamera()
        {
            void* mem = RE::malloc(0x1A0);
            if (!mem) {
                return nullptr;
            }
            std::memset(mem, 0, 0x1A0);
            auto* cam = static_cast<RE::NiCamera*>(Fn<Ctor1>(kNiCameraCtor)(mem));
            if (cam) {
                cam->IncRefCount();  // ours forever
            }
            return cam;
        }

        void* NewAccumulator()
        {
            void* mem = RE::malloc(0x590);
            if (!mem) {
                return nullptr;
            }
            std::memset(mem, 0, 0x590);
            auto* acc = static_cast<std::uint8_t*>(Fn<AccCtor>(kAccCtor)(mem, 99));  // 99 as Interface3D / LocalMap
            if (acc) {
                reinterpret_cast<RE::NiRefObject*>(acc)->IncRefCount();
                acc[0xB1] = 1;  // z-prepass
            }
            return acc;
        }

        // RT id of our render target: created once in a free id (renderTargetData[id] all zero) with the properties of
        // the main scene target (id 1: scene size, R11G11B10F, as the engine's own composite target); re-created when
        // the scene size changes.
        std::uint8_t* Rtm() { return reinterpret_cast<std::uint8_t*>(Base() + kRtm); }
        std::uint32_t RtHandle(int a_id) { return *reinterpret_cast<const std::uint32_t*>(Rtm() + kRtHandles + 4 * a_id); }

        void* RtTexture(int a_id)
        {
            auto* data = RE::BSGraphics::GetRendererData();
            const std::uint32_t h = RtHandle(a_id);
            if (!data || h >= 101) {
                return nullptr;
            }
            return data->renderTargets[h].texture;
        }

        bool EnsureTarget()
        {
            auto* rtm = Rtm();
            const auto* main = rtm + 0x20 * 1;
            if (g_rtId >= 0) {
                const auto* mine = rtm + 0x20 * g_rtId;
                const bool  sizeOk = std::memcmp(mine, main, 8) == 0;
                bool        shared = false;
                const auto  h = RtHandle(g_rtId);
                for (int i = 0; i < kRtCount && !shared; ++i) {
                    shared = i != g_rtId && RtHandle(i) == h;
                }
                if (sizeOk && !shared && h < 101 && RtTexture(g_rtId)) {
                    S().sceneTex.store(RtTexture(g_rtId));
                    return true;
                }
                REX::WARN("vatswin: render target id {:X} no longer valid (size ok {}, handle {} shared {}); re-creating", g_rtId, sizeOk, h, shared);
            }
            int id = g_rtId;
            if (id < 0) {
                for (int c = 0x5A; c <= 0x62 && id < 0; ++c) {
                    bool zero = true;
                    for (int k = 0; k < 0x20 && zero; ++k) {
                        zero = rtm[0x20 * c + k] == 0;
                    }
                    if (zero) {
                        id = c;
                    }
                }
            }
            if (id < 0) {
                g_rtNote = "no free render target id in 5A..62";
                return false;
            }
            std::uint8_t props[0x20];
            std::memcpy(props, main, 0x20);
            Fn<CreateRTFn>(kCreateRT)(rtm, id, props, 1);
            const auto h = RtHandle(id);
            bool shared = false;
            for (int i = 0; i < kRtCount && !shared; ++i) {
                shared = i != id && RtHandle(i) == h;
            }
            void* tex = RtTexture(id);
            g_rtNote = std::format("id {:X} handle {} {}x{} fmt {} shared {} tex {}", id, h, *reinterpret_cast<const std::uint32_t*>(props),
                *reinterpret_cast<const std::uint32_t*>(props + 4), *reinterpret_cast<const std::uint32_t*>(props + 8), shared, tex);
            REX::INFO("vatswin: own render target: {}", g_rtNote);
            if (!tex || shared || h >= 101) {
                g_rtId = -1;
                return false;
            }
            g_rtId = id;
            S().sceneTex.store(tex);
            return true;
        }

        // ---- the render (render thread, inside Main::DrawWorld_And_UI before DrawWorld::End) ----
        struct RenderArgs
        {
            void* cam;
            void* acc;
            void* root;  // target 3D (scene 0)
            int   rtId;
            int   scene;
            bool  mask;
        };

        std::uint8_t* TlsRenderState()
        {
            const auto      tlsIndex = *reinterpret_cast<const std::uint32_t*>(Base() + kTlsIndex);
            const auto      tlsArray = __readgsqword(0x58);
            const auto      block = *reinterpret_cast<const std::uintptr_t*>(tlsArray + 8ull * tlsIndex);
            auto*           st = *reinterpret_cast<std::uint8_t**>(block + 0xB20);
            return st ? st : *reinterpret_cast<std::uint8_t**>(Base() + kRendererData);
        }

        int IdForHandle(std::int32_t a_handle, int a_prefer)
        {
            if (a_handle < 0) {
                return -1;
            }
            if (a_prefer >= 0 && a_prefer < kRtCount && static_cast<std::int32_t>(RtHandle(a_prefer)) == a_handle) {
                return a_prefer;
            }
            for (int i = 0; i < kRtCount; ++i) {
                if (static_cast<std::int32_t>(RtHandle(i)) == a_handle) {
                    return i;
                }
            }
            return -2;  // unknown handle: leave the slot alone
        }

        // values RenderBody changed, put back by the SEH handler if the render faults half-way
        struct Undo
        {
            std::uint8_t* p[8]{};
            std::uint8_t  v[8]{};
            int           n{ 0 };
            void**        curCam{ nullptr };
            void*         cam{ nullptr };
            std::uint64_t* qp[4]{};
            std::uint64_t  qv[4]{};
            int            qn{ 0 };
            void Byte(std::uint8_t* a_p)
            {
                if (a_p && n < 8) p[n] = a_p, v[n] = *a_p, ++n;
            }
            void Qword(void* a_p)
            {
                if (a_p && qn < 4) qp[qn] = static_cast<std::uint64_t*>(a_p), qv[qn] = *qp[qn], ++qn;
            }
            void Apply()
            {
                for (int i = qn; i-- > 0;) *qp[i] = qv[i];
                qn = 0;
                for (int i = n; i-- > 0;) *p[i] = v[i];
                if (curCam) *curCam = cam;
                n = 0, curCam = nullptr;
            }
        };
        Undo g_undo;
        // light diagnostics of the last render (render thread; copied into the status line)
        struct LightDiag
        {
            int           lights{ 0 }, shadowLights{ 0 };
            int           lightsLit{ 0 }, shadowLit{ 0 };
            std::uint32_t shadowState[4]{};
            std::uint32_t shadowType[4]{};
            std::uint32_t sunState{ 0xFFFFFFFF };
            std::int32_t  sunMap{ -2 };     // *(*(sun+0x198)+0x50): RenderSceneDeferred does the light pass only if != -1
            std::uint32_t sunType{ 0 };
        };
        LightDiag g_ld;

        // No C++ objects with destructors in here (SEH wrapper below).
        void RenderBody(const RenderArgs& a, int& a_stage)
        {
            const auto base = Base();
            void* const rtm = Rtm();
            void* const renderer = reinterpret_cast<void*>(base + kRenderer);
            void* const gstate = reinterpret_cast<void*>(base + kGfxState);
            void* const ssn = *reinterpret_cast<void**>(base + kWorldSsn);
            if (!ssn) {
                a_stage = -1;
                return;
            }
            a_stage = 1;
            // current render target / depth state (TLS render state), restored at the end with mode 3 (no clear)
            std::uint8_t* const ts = TlsRenderState();
            const std::int32_t  savedRt0 = *reinterpret_cast<std::int32_t*>(ts + 0x1BF4);
            std::int32_t        savedH[8];
            for (int s = 0; s < 8; ++s) {
                savedH[s] = *reinterpret_cast<std::int32_t*>(ts + 0x1B9C + 4 * s);
            }
            const std::int32_t savedDs = *reinterpret_cast<std::int32_t*>(ts + 0x1BF8);
            const std::int32_t savedDsSlice = *reinterpret_cast<std::int32_t*>(ts + 0x1BC0);

            // image-space effects [69]/[70]/[71] off around the render (as Interface3D::Renderer::DrawModel): otherwise the
            // composite reads the main view's SAO/SSR results
            std::uint8_t* const ism = *reinterpret_cast<std::uint8_t**>(base + kIsmSingleton);
            std::uint8_t* const effs = ism ? *reinterpret_cast<std::uint8_t**>(ism + 0x18) : nullptr;
            std::uint8_t* const e69 = effs ? *reinterpret_cast<std::uint8_t**>(effs + 0x228) : nullptr;
            std::uint8_t* const e70 = effs ? *reinterpret_cast<std::uint8_t**>(effs + 0x230) : nullptr;
            std::uint8_t* const e71 = effs ? *reinterpret_cast<std::uint8_t**>(effs + 0x238) : nullptr;
            const std::uint8_t s69 = e69 ? e69[8] : 0, s70 = e70 ? e70[8] : 0, s71 = e71 ? e71[8] : 0, s71b = e71 ? e71[0x121] : 0;
            auto* const        hbao = reinterpret_cast<std::uint8_t*>(base + kHbaoSetting);
            const std::uint8_t sHbao = hbao[8];
            auto* const        saoFlag = reinterpret_cast<std::uint8_t*>(base + kSaoDispatchFlag);
            const std::uint8_t sSao = *saoFlag;
            void** const       curCam = reinterpret_cast<void**>(base + kCurCamera);
            void* const        savedCam = *curCam;
            g_undo = Undo{};
            g_undo.curCam = curCam, g_undo.cam = savedCam;
            if (e69) g_undo.Byte(e69 + 8);
            if (e70) g_undo.Byte(e70 + 8);
            if (e71) g_undo.Byte(e71 + 8), g_undo.Byte(e71 + 0x121);
            g_undo.Byte(hbao + 8);
            g_undo.Byte(saoFlag);
            const bool fxOff = g_fxOff.load();
            if (e70 && fxOff) e70[8] = 0;
            if (sHbao) {
                hbao[8] = 0;
                Fn<SettingFn>(kSettingUpdated)(hbao);
            }
            if (e69 && fxOff) e69[8] = 0;
            if (e71 && fxOff) {
                e71[0x121] = 0;
                e71[8] = e71[0x120] != 0;
            }
            *saoFlag = 0;

            a_stage = 2;
            alignas(16) std::uint8_t cull[0x1A0]{};
            Fn<CullCtor>(kCullCtor)(cull, nullptr);
            *reinterpret_cast<std::uint32_t*>(cull + 0x158) = 0;  // kNormal
            *reinterpret_cast<void**>(cull + 0x18) = a.cam;        // NiCullingProcess camera (DrawModel / LocalMap set it)
            a_stage = 21;
            Fn<CullSetAcc>(kCullSetAcc)(cull, a.acc);
            a_stage = 22;
            auto* acc = static_cast<std::uint8_t*>(a.acc);
            auto* cam = static_cast<std::uint8_t*>(a.cam);
            *reinterpret_cast<void**>(acc + 0x558) = ssn;
            std::memcpy(acc + 0x570, cam + 0xA0, 12);
            *reinterpret_cast<std::uint32_t*>(acc + 0x560) = 0x18;  // deferred render mode (DrawModel)
            acc[0xB1] = 1;

            *curCam = a.cam;
            a_stage = 23;
            Fn<CacheCamFn>(kCacheCameraData)(gstate, a.cam, false);
            a_stage = 24;
            Fn<SetCamFn>(kSetCameraData)(gstate, a.cam, false, 0.0f, 1.0f);
            a_stage = 25;
            // as LocalMapRenderer::Render: ssn+0x225 = 1 while we render -> ProcessQueuedLights does not rebuild the world
            // SSN's light lists for our camera (the main view's lists stay as they are)
            auto* const        ssnB = static_cast<std::uint8_t*>(ssn);
            const std::uint8_t sKeepLists = ssnB[0x225];
            g_undo.Byte(ssnB + 0x225);
            if (g_keepLists.load()) ssnB[0x225] = 1;
            Fn<ProcessLights>(kProcessQueuedLights)(ssn, cull);
            a_stage = 26;
            Fn<RendererFn>(kResetZPrePass)(renderer);
            a_stage = 3;
            if (a.scene == 0) {
                if (a.root) {
                    Fn<Accumulate>(kAccumulateScene)(a.cam, a.root, cull, false);  // 4th arg 0 as LocalMapRenderer in this slot
                }
            } else {
                Fn<Accumulate>(kAccumulateScene)(a.cam, ssn, cull, false);  // the world scene graph as a whole (approach B, a)
            }
            a_stage = 4;
            Fn<ClearColorFn>(kSetClearColor)(renderer, 0.0f, 0.0f, 0.0f, 0.0f);
            auto setRt = Fn<SetRTFn>(kSetRT);
            setRt(rtm, 0, a.rtId, 0);  // our target cleared first (the composite leaves far pixels alone)
            Fn<RendererFn>(kFlush)(renderer);
            Fn<SetDSFn>(kSetDS)(rtm, 1, 0, 0, false);  // main depth, cleared (mode 0)
            setRt(rtm, 0, 0x1A, 0);  // GBuffer, cleared (mode 0) as DrawModel
            setRt(rtm, 1, 0x1B, 0);
            setRt(rtm, 2, 0x1D, 0);
            setRt(rtm, 3, 0x1E, 0);
            if (*reinterpret_cast<const std::uint8_t*>(base + kDeferredRgbEmit)) {
                setRt(rtm, 4, 0x1F, 0);
            }
            setRt(rtm, 5, 0x20, 3);
            Fn<RtmFn>(kVpDefault)(rtm);
            std::uint8_t* const dist = Fn<QInstanceFn>(kDistantQInstance)();
            const std::uint8_t  sDist = dist ? dist[0x70] : 0;
            if (dist) dist[0x70] = 0;
            Fn<RendererFn>(kFlush)(renderer);
            if (*reinterpret_cast<std::uint64_t*>(ts + 0x1F80) != 0) {  // as DrawModel: drop a cached binding
                *reinterpret_cast<std::uint32_t*>(ts + 0x1B84) |= 0x40;
                *reinterpret_cast<std::uint64_t*>(ts + 0x1F80) = 0;
            }
            a_stage = 5;
            // The sun: the main view lights it separately (DrawWorld::DeferredLightsImpl 21F7xxx: *(ssn+0x208), a
            // BSShadowDirectionalLight, fitted + shadow-mapped + its own light pass); RenderSceneDeferred only walks the SSN's
            // shadow-light array (+0x170 data, +0x180 count) and its light list -> without this the target is lit by local
            // lights only. For our render the array is temporarily just [sun] (RenderSceneDeferred fits the cascades to our
            // camera, renders the map and the sun light pass); put back right after.
            void* sunArr[1]{};
            auto* const sb0 = static_cast<std::uint8_t*>(ssn);
            void* const sun = *reinterpret_cast<void**>(sb0 + 0x208);
            const int   sunMode = g_sun.load();
            // (experimental, crashed once: the sun's light node +0xB8 was null in a frame -> only with a node)
            const bool  sunIn = sunMode > 0 && sun && *reinterpret_cast<void**>(static_cast<std::uint8_t*>(sun) + 0xB8);
            if (sunIn) {
                if (sunMode == 1) {
                    g_undo.Qword(sun);  // vtable pointer
                    *reinterpret_cast<void***>(sun) = SunVtable(*reinterpret_cast<void**>(sun));
                }
                g_undo.Qword(sb0 + 0x170);
                g_undo.Qword(sb0 + 0x178);
                g_undo.Qword(sb0 + 0x180);
                sunArr[0] = sun;
                *reinterpret_cast<void**>(sb0 + 0x170) = sunArr;
                *reinterpret_cast<std::uint32_t*>(sb0 + 0x178) = 1;
                *reinterpret_cast<std::uint32_t*>(sb0 + 0x180) = 1;
            }
            a_stage = 51;
            Fn<RenderDeferred>(kRenderSceneDeferred)(a.cam, a.acc, cull, ssn, a.rtId, 1, false, false);
            if (sunIn) {
                g_ld.sunState = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(sun) + 0x18);
                g_ld.sunType = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(sun) + 0x180);
                if (auto* sm = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sun) + 0x198)) {
                    g_ld.sunMap = *reinterpret_cast<std::int32_t*>(sm + 0x50);
                }
                for (int i = g_undo.qn; i-- > 0;) *g_undo.qp[i] = g_undo.qv[i];
                g_undo.qn = 0;
            }
            a_stage = 6;
            {
                auto* const sb = static_cast<std::uint8_t*>(ssn);
                const auto sunSt = g_ld.sunState;
                const auto sunMap = g_ld.sunMap;
                const auto sunType = g_ld.sunType;
                g_ld = LightDiag{};
                g_ld.sunState = sunSt, g_ld.sunMap = sunMap, g_ld.sunType = sunType;
                g_ld.lights = *reinterpret_cast<std::uint16_t*>(sb + 0x168);
                g_ld.shadowLights = *reinterpret_cast<std::uint16_t*>(sb + 0x180);
                for (int i = 0; i < g_ld.lights && i < 256; ++i) {
                    if (auto* l = Fn<GetLightFn>(kGetLight)(ssn, i); l && *reinterpret_cast<std::uint32_t*>(l + 0x18) != 0xFF) ++g_ld.lightsLit;
                }
                for (int i = 0; i < g_ld.shadowLights && i < 64; ++i) {
                    if (auto* l = Fn<GetLightFn>(kGetShadowLight)(ssn, i)) {
                        const auto st = *reinterpret_cast<std::uint32_t*>(l + 0x18);
                        g_ld.shadowLit += st != 0xFF;
                        if (i < 4) g_ld.shadowState[i] = st, g_ld.shadowType[i] = *reinterpret_cast<std::uint32_t*>(l + 0x180);
                    }
                }
            }
            if (dist) dist[0x70] = sDist;
            setRt(rtm, 0, a.rtId, 3);
            for (unsigned s = 1; s < 5; ++s) {
                setRt(rtm, s, -1, 3);
            }
            setRt(rtm, 5, -1, 0);
            if (a.mask) {
                a_stage = 7;
                Fn<MaskFn>(kRenderMask)(a.cam);  // VATS target mask for our camera into RT 0xF
            }
            a_stage = 8;
            Fn<AccIntFn>(kClearActivePasses)(a.acc, 0);
            Fn<AccIntFn>(kClearGroupPasses)(a.acc, 5);
            if (a.scene == 0 && a.root) {
                Fn<ObjFn>(kClearRenderPasses)(a.root);
            }
            Fn<CullSetAcc>(kCullSetAcc)(cull, nullptr);
            Fn<CullDtor>(kCullDtor)(cull);

            // restore
            ssnB[0x225] = sKeepLists;
            if (e70) e70[8] = s70;
            if (sHbao) {
                hbao[8] = sHbao;
                Fn<SettingFn>(kSettingUpdated)(hbao);
            }
            if (e69) e69[8] = s69;
            if (e71) {
                e71[0x121] = s71b;
                e71[8] = s71;
            }
            *saoFlag = sSao;
            *curCam = savedCam;
            if (savedCam) {
                Fn<SetCamFn>(kSetCameraData)(gstate, savedCam, false, 0.0f, 1.0f);  // as LocalMapRenderer::Render
            }
            for (unsigned s = 0; s < 8; ++s) {
                const int id = s == 0 ? (savedH[0] < 0 ? -1 : IdForHandle(savedH[0], savedRt0)) : IdForHandle(savedH[s], -1);
                if (id >= -1) {
                    setRt(rtm, s, id, 3);
                }
            }
            Fn<SetDSFn>(kSetDS)(rtm, savedDs, 3, savedDsSlice, false);
            g_undo = Undo{};
            a_stage = 9;
        }

        std::uintptr_t g_faultAddr = 0;
        int Filter(EXCEPTION_POINTERS* a_ep)
        {
            g_faultAddr = a_ep && a_ep->ExceptionRecord ? reinterpret_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionAddress) : 0;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        int RenderGuarded(const RenderArgs& a_args, int& a_stage)
        {
            __try {
                RenderBody(a_args, a_stage);
                return 0;
            } __except (Filter(GetExceptionInformation())) {
                g_undo.Apply();  // engine flags / camera back as they were
                return static_cast<int>(GetExceptionCode());
            }
        }

        void SetCameraFrom(RE::NiCamera* a_cam, const Pose& a_p)
        {
            auto* c = reinterpret_cast<std::uint8_t*>(a_cam);
            for (int t : { 0x30, 0x70 }) {  // local and world (no parent)
                auto* m = reinterpret_cast<float*>(c + t);
                for (int r = 0; r < 3; ++r) {
                    for (int k = 0; k < 3; ++k) {
                        m[r * 4 + k] = a_p.rot[r][k];
                    }
                    m[r * 4 + 3] = 0.0f;
                }
                std::memcpy(c + t + 0x30, a_p.pos, 12);
                *reinterpret_cast<float*>(c + t + 0x3C) = 1.0f;
            }
            auto& fr = a_cam->viewFrustum;
            fr.left = a_p.l, fr.right = a_p.r, fr.top = a_p.t, fr.bottom = a_p.b, fr.near = a_p.n, fr.far = a_p.f, fr.ortho = false;
            a_cam->port.left = 0.0f, a_cam->port.right = 1.0f, a_cam->port.top = 1.0f, a_cam->port.bottom = 0.0f;
            alignas(16) std::uint8_t ud[0x40]{};  // NiUpdateData (zeroed: time 0, no flags)
            a_cam->UpdateWorldData(reinterpret_cast<RE::NiUpdateData*>(ud));
            std::memcpy(c + 0x70, c + 0x30, 0x40);  // world = local (UpdateWorldData without parent does the same)
        }

        void Render()
        {
            ++g_frame;
            Pose p;
            RE::NiPointer<RE::NiAVObject> root;
            {
                std::scoped_lock l(g_poseLock);
                p = g_pose;
                root = g_target;
            }
            if (!p.valid) {
                ++g_skips;
                return;
            }
            const int every = (std::max)(1, g_every.load());
            const bool first = S().episodeRenders.load() == 0;
            if (!first && (g_frame % static_cast<std::uint64_t>(every)) != 0) {
                return;
            }
            if (g_scene.load() == 0 && !root) {
                ++g_skips;
                return;
            }
            if (!g_renderCam) g_renderCam = NewCamera();
            if (!g_acc) g_acc = NewAccumulator();
            if (!g_renderCam || !g_acc || !EnsureTarget()) {
                if (++g_renderFails <= 3) REX::ERROR("vatswin: render resources missing (cam {} acc {} rt {})", static_cast<void*>(g_renderCam), g_acc, g_rtNote);
                return;
            }
            SetCameraFrom(g_renderCam, p);
            auto* data = RE::BSGraphics::GetRendererData();
            void* ctx = data ? data->context : nullptr;
            LARGE_INTEGER t0{}, t1{}, fq{};
            ::QueryPerformanceCounter(&t0);
            if (ctx) detail::GpuTimerBegin(ctx);
            RenderArgs a{ g_renderCam, g_acc, root.get(), g_rtId, g_scene.load(), S().mask.load() };
            int        stage = 0;
            stereo::SetSecondCameraPass(true);
            const int  code = RenderGuarded(a, stage);
            stereo::SetSecondCameraPass(false);
            if (ctx) detail::GpuTimerEnd(ctx);
            ::QueryPerformanceCounter(&t1);
            ::QueryPerformanceFrequency(&fq);
            if (code != 0) {
                g_lastErr = std::format("exception {:08X} at stage {} addr {:X} (rva {:X})", static_cast<std::uint32_t>(code), stage, g_faultAddr,
                    g_faultAddr - Base());
                REX::ERROR("vatswin: render failed: {}; window rendering switched off", g_lastErr);
                g_enabled.store(false);
                return;
            }
            if (stage != 9) {
                g_lastErr = std::format("stopped at stage {}", stage);
                ++g_skips;
                return;
            }
            if (a.mask && ctx) {
                S().maskValid.store(detail::MaskCopy(ctx, RtTexture(0xF)));
            }
            if (ctx && S().ambient.load() > 0.0f) {
                S().albedoValid.store(detail::AlbedoCopy(ctx, RtTexture(0x1A)));  // our render's albedo (GBuffer RT 0)
            }
            const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
            g_cpuMsAvg.store(g_cpuMsAvg.load() * 0.95 + ms * 0.05);
            g_cpuMsMax.store((std::max)(g_cpuMsMax.load() * 0.999, ms));
            g_lastSeq = p.seq;
            if (S().renders.fetch_add(1) == 0) {
                REX::INFO("vatswin: first window render ({:.2f} ms CPU, rt {})", ms, g_rtNote);
            }
            S().episodeRenders.fetch_add(1);
        }

        void HkDrawWorldEnd()
        {
            ++g_hookCalls;
            for (int site = 2; site <= 5; ++site) NoteCameraPitch(site);
            if (g_enabled.load(std::memory_order_relaxed) && g_active.load(std::memory_order_relaxed)) {
                Render();
            }
            g_origEnd();
        }

        // ---- label camera (UI update thread) ----
        RE::TESObjectREFR* CurrentTarget(std::uint8_t*& a_obj)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return nullptr;
            }
            Scaleform::Ptr<RE::IMenu> m = ui->GetMenu(RE::BSFixedString("VATSMenu"));
            if (!m) {
                return nullptr;
            }
            auto* obj = *reinterpret_cast<std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(m.get()) + 0xE8);
            if (!obj) {
                return nullptr;
            }
            a_obj = obj;
            const std::uint32_t idx = *reinterpret_cast<const std::uint32_t*>(obj + 0x1230);
            const std::uint32_t cnt = *reinterpret_cast<const std::uint32_t*>(obj + 0x248);
            const auto*         order = *reinterpret_cast<const std::uint8_t* const*>(obj + 0x238);
            const auto*         refs = *reinterpret_cast<RE::TESObjectREFR* const* const*>(obj + 0x220);
            if (idx >= cnt || cnt > 256 || !order || !refs) {
                return nullptr;
            }
            const std::uint32_t k = *reinterpret_cast<const std::uint32_t*>(order + static_cast<std::size_t>(idx) * 8);
            return refs[k];
        }

        bool SelectedPartPos(const std::uint8_t* a_obj, float a_out[3])
        {
            const std::uint32_t sel = *reinterpret_cast<const std::uint32_t*>(a_obj + 0x1234);
            const std::uint32_t n = *reinterpret_cast<const std::uint32_t*>(a_obj + 0x698);
            if (sel >= n || n > 64) {
                return false;
            }
            const std::uint8_t* arr = a_obj + 0x318;
            if (*reinterpret_cast<const std::int32_t*>(a_obj + 0x310) >= 0) {
                arr = *reinterpret_cast<const std::uint8_t* const*>(a_obj + 0x318);  // heap storage
            }
            if (!arr) {
                return false;
            }
            const auto* node = *reinterpret_cast<const std::uint8_t* const*>(arr + static_cast<std::size_t>(sel) * 0x38 + 8);
            if (!node) {
                return false;
            }
            std::memcpy(a_out, node + 0xA0, 12);
            return std::isfinite(a_out[0]) && std::isfinite(a_out[1]) && std::isfinite(a_out[2]);
        }

        bool UpdateAim(RE::NiCamera* a_world)
        {
            if (!a_world) {
                return false;
            }
            std::uint8_t* obj = nullptr;
            RE::TESObjectREFR* ref = CurrentTarget(obj);
            RE::NiAVObject*    root = ref ? ref->Get3D() : nullptr;
            if (!ref || !root || !obj) {
                return false;
            }
            const auto* wc = reinterpret_cast<const std::uint8_t*>(a_world);
            float eye[3];
            std::memcpy(eye, wc + 0xA0, 12);
            const auto* rb = reinterpret_cast<const std::uint8_t*>(root);
            float       center[3];
            std::memcpy(center, rb + 0xB0, 12);
            const float radius = Clampf(*reinterpret_cast<const float*>(rb + 0xBC), 10.0f, 400.0f);
            float aim[3];
            if (!SelectedPartPos(obj, aim)) {
                std::memcpy(aim, center, 12);
            }
            for (int i = 0; i < 3; ++i) {
                aim[i] = center[i] + (aim[i] - center[i]) * g_cfg.aimPart;
            }
            // smooth the aim point (only the window view moves with it); snap on a new target
            LARGE_INTEGER now{}, fq{};
            ::QueryPerformanceCounter(&now);
            ::QueryPerformanceFrequency(&fq);
            const float dt = g_aimLast.QuadPart ? static_cast<float>(now.QuadPart - g_aimLast.QuadPart) / static_cast<float>(fq.QuadPart) : 1.0f;
            g_aimLast = now;
            const std::uint32_t tid = ref->GetFormID();
            if (!g_aimValid || tid != g_aimTarget || dt > 0.5f) {
                std::memcpy(g_aim, aim, 12);
                g_aimValid = true;
                g_aimTarget = tid;
            } else {
                const float k = 1.0f - std::exp(-dt / (std::max)(g_cfg.aimTauS, 0.001f));
                for (int i = 0; i < 3; ++i) g_aim[i] += (aim[i] - g_aim[i]) * k;
            }
            float d[3] = { g_aim[0] - eye[0], g_aim[1] - eye[1], g_aim[2] - eye[2] };
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist < 1.0f) {
                return false;
            }
            for (float& v : d) v /= dist;
            // right = dir x Z, up = right x dir (Z-up world)
            float rt[3] = { d[1], -d[0], 0.0f };
            float rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1]);
            if (rl < 1e-4f) {
                rt[0] = 1.0f, rt[1] = 0.0f, rl = 1.0f;
            }
            rt[0] /= rl, rt[1] /= rl;
            const float up[3] = { rt[1] * d[2] - rt[2] * d[1], rt[2] * d[0] - rt[0] * d[2], rt[0] * d[1] - rt[1] * d[0] };
            Pose p;
            // FO4 NiCamera rotate in memory: row 0 = view direction, row 1 = up, row 2 = right (NiCamera::WorldToCameraMatrix
            // 16D25D0: w row = rotate row 0, y row = row 1, x row = row 2; the world camera dump of heading 0 reads
            // [0 1 0 | 0 0 1 | 1 0 0])
            for (int k = 0; k < 3; ++k) {
                p.rot[0][k] = d[k];
                p.rot[1][k] = up[k];
                p.rot[2][k] = rt[k];
            }
            std::memcpy(p.pos, eye, 12);
            // vertical FOV: fixed, or the target's bound diameter x fitScale fills the window height
            float vfov = g_fovDeg.load();
            if (!(vfov > 0.5f)) {
                const float cdist = std::sqrt((center[0] - eye[0]) * (center[0] - eye[0]) + (center[1] - eye[1]) * (center[1] - eye[1]) +
                                              (center[2] - eye[2]) * (center[2] - eye[2]));
                vfov = 2.0f * std::atan(radius * g_cfg.fitScale * 0.5f / (std::max)(cdist, 1.0f)) * 57.2957795f;
            }
            vfov = Clampf(vfov, 4.0f, 60.0f);
            const auto*  rtm = Rtm();
            const float  w = static_cast<float>(*reinterpret_cast<const std::uint32_t*>(rtm + 0x20));
            const float  h = static_cast<float>(*reinterpret_cast<const std::uint32_t*>(rtm + 0x24));
            const float  aspect = (w > 0.0f && h > 0.0f) ? w / h : 16.0f / 9.0f;
            const float  tv = std::tan(vfov * 0.5f / 57.2957795f);
            p.t = tv, p.b = -tv, p.r = tv * aspect, p.l = -tv * aspect;
            const auto& wf = a_world->viewFrustum;
            p.n = wf.near > 0.0f ? wf.near : 5.0f;
            p.f = wf.far > p.n ? wf.far : 100000.0f;
            p.valid = true;
            SetCameraFrom(g_labelCam, p);
            {
                std::scoped_lock l(g_poseLock);
                p.seq = g_pose.seq + 1;
                g_pose = p;
                if (g_target.get() != root) {
                    g_target.reset(root);
                }
                g_targetId = tid;
                g_labelNote = std::format("target {:08X} dist {:.0f} r {:.0f} vfov {:.1f} aspect {:.3f} aim {:.0f},{:.0f},{:.0f}", tid, dist, radius,
                    vfov, aspect, g_aim[0], g_aim[1], g_aim[2]);
            }
            return true;
        }

        RE::NiCamera* LabelCamera(CamFn a_orig)
        {
            RE::NiCamera* world = a_orig();
            ++g_labelCalls;
            if (!g_enabled.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_relaxed)) {
                return world;
            }
            if (!g_labelCam) {
                g_labelCam = NewCamera();
                if (!g_labelCam) {
                    return world;
                }
            }
            if (!UpdateAim(world)) {
                return world;
            }
            ++g_labelOurs;
            return g_labelCam;
        }

        // P1: with the VATS camera gate the VATS base camera still writes the player camera root's local rotation (rva
        // 832DB6; hardware write watch, 15 Nov 03:35: the +2.2 deg pitch of the world view while VATS is open, written after
        // TESCamera::Update and before the render). Its own data update is kept, the root's local transform is put back.
        void HkBaseCamUpdate(void* a_cam, float a_dt)
        {
            ++g_baseCamCalls;
            auto* pc = RE::PlayerCamera::GetSingleton();
            auto* root = pc ? reinterpret_cast<std::uint8_t*>(pc->cameraRoot.get()) : nullptr;
            if (!root || !g_enabled.load(std::memory_order_relaxed)) {
                g_origBaseCam(a_cam, a_dt);
                return;
            }
            alignas(16) std::uint8_t saved[0x40];
            std::memcpy(saved, root + 0x30, sizeof(saved));
            g_origBaseCam(a_cam, a_dt);
            if (std::memcmp(saved, root + 0x30, sizeof(saved)) != 0) {
                std::memcpy(root + 0x30, saved, sizeof(saved));
                // the base camera also updated the root's world + its NiCamera child from the tilted local: re-run the update
                // down the root (NiAVObject::Update 16C8570, zeroed NiUpdateData as Interface3D::Renderer::DrawModel)
                alignas(16) std::uint8_t ud[0x40]{};
                using NiUpdateFn = void (*)(void*, void*);
                Fn<NiUpdateFn>(kNiAVObjectUpdate)(root, ud);
                ++g_baseCamUndone;
            }
        }

        RE::NiCamera* HkLabel1() { return LabelCamera(g_origCam1); }
        RE::NiCamera* HkLabel2() { return LabelCamera(g_origCam2); }

        bool CallSiteOk(const CallSite& a_s)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_s.site);
            if (p[0] != 0xE8) {
                return false;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, p + 1, 4);
            return Base() + a_s.site + 5 + rel == Base() + a_s.target;
        }

        void ReadIni()
        {
            const auto path = Settings::Path();
            auto rdi = [&](const char* k, int d) { return static_cast<int>(::GetPrivateProfileIntA("Menus", k, d, path.c_str())); };
            auto rdf = [&](const char* k, float d) {
                char buf[64]{};
                ::GetPrivateProfileStringA("Menus", k, "", buf, sizeof(buf), path.c_str());
                char*       end = nullptr;
                const float v = std::strtof(buf, &end);
                return end != buf ? v : d;
            };
            g_cfg.on = rdi("bVatsCameraWindow", g_cfg.on ? 1 : 0) != 0;
            g_cfg.aimPart = Clampf(rdf("fVatsWindowAimPart", g_cfg.aimPart), 0.0f, 1.0f);
            g_cfg.every = std::clamp(rdi("iVatsWindowEvery", g_cfg.every), 1, 8);
            g_cfg.scene = std::clamp(rdi("iVatsWindowScene", g_cfg.scene), 0, 1);
            g_cfg.fovDeg = rdf("fVatsWindowFovDeg", g_cfg.fovDeg);
            g_cfg.fitScale = Clampf(rdf("fVatsWindowFit", g_cfg.fitScale), 0.5f, 6.0f);
            S().exposure.store(Clampf(rdf("fVatsWindowExposure", S().exposure.load()), 0.01f, 64.0f));
            S().width.store(std::clamp(rdi("iVatsWindowWidth", S().width.load()), 256, 4096));
            S().mask.store(rdi("bVatsWindowMask", 1) != 0);
            S().maskStrength.store(Clampf(rdf("fVatsWindowMaskStrength", S().maskStrength.load()), 0.0f, 1.0f));
            g_sun.store(std::clamp(rdi("iVatsWindowSun", 0), 0, 2));
            S().ambient.store(Clampf(rdf("fVatsWindowAmbient", S().ambient.load()), 0.0f, 8.0f));
            g_every.store(g_cfg.every);
            g_scene.store(g_cfg.scene);
            g_fovDeg.store(g_cfg.fovDeg);
        }
    }

    bool ConfiguredOn()
    {
        static std::once_flag once;
        std::call_once(once, ReadIni);
        return g_cfg.on;
    }

    std::string Install()
    {
        if (g_installed) {
            return g_installNote;
        }
        ConfiguredOn();
        if (!g_cfg.on) {
            g_installNote = "off ([Menus] bVatsCameraWindow=0): no hooks";
            REX::INFO("vatswin: {}", g_installNote);
            return g_installNote;
        }
        for (const auto& s : kSites) {
            if (!CallSiteOk(s)) {
                g_installNote = std::format("byte check FAILED at {} - VATS camera window not installed (round-4 window stays)", s.name);
                REX::ERROR("vatswin: {}", g_installNote);
                return g_installNote;
            }
        }
        g_tramp.create(64);
        g_origEnd = reinterpret_cast<VoidFn>(g_tramp.write_call<5>(Base() + kEndCall, &HkDrawWorldEnd));
        g_origCam1 = reinterpret_cast<CamFn>(g_tramp.write_call<5>(Base() + kLabelCall1, &HkLabel1));
        g_origCam2 = reinterpret_cast<CamFn>(g_tramp.write_call<5>(Base() + kLabelCall2, &HkLabel2));
        g_origBaseCam = reinterpret_cast<BaseCamUpdFn>(g_tramp.write_call<5>(Base() + kBaseCamCall, &HkBaseCamUpdate));
        g_tramp.write_call<5>(Base() + kBaseCamCall2, &HkBaseCamUpdate);  // same target (byte-checked above)
        g_installed = true;
        g_enabled.store(true);
        g_installNote = std::format("installed (R1 C383F6, L1 B1E319, L2 B1E9C1, P1 B180D5, P2 B15CB1 byte-checked); every {} scene {} fov {} exposure {:.2f} width {} mask {}",
            g_cfg.every, g_cfg.scene, g_cfg.fovDeg, S().exposure.load(), S().width.load(), S().mask.load() ? 1 : 0);
        REX::INFO("vatswin: {}", g_installNote);
        return g_installNote;
    }

    bool Enabled()
    {
        return g_installed && g_enabled.load(std::memory_order_relaxed);
    }

    void SetActive(bool a_on)
    {
        const bool was = g_active.exchange(a_on && Enabled());
        if (was != (a_on && Enabled())) {
            REX::INFO("vatswin: window {}", a_on ? "active" : "inactive");
            if (a_on) {
                S().episodeRenders.store(0);
                g_aimValid = false;
            }
        }
    }

    namespace
    {
        struct PitchSite
        {
            std::atomic<std::uint64_t> n{ 0 }, tilted{ 0 };
            std::atomic<float>         last{ 0.0f }, maxAbs{ 0.0f };
        };
        PitchSite g_pitch[6];
    }

    // ---- write watch (diagnostics): hardware data breakpoint (DR0, 4-byte write) on the camera root's local rotate
    // row-1 z (the VATS pitch term) in every thread; a vectored handler records the writing instruction (rva) ----
    namespace
    {
        std::atomic<int>       g_watchLeft{ 0 };
        std::uintptr_t         g_watchAddr = 0;
        PVOID                  g_veh = nullptr;
        std::mutex             g_watchLock;
        std::vector<std::pair<std::uintptr_t, int>> g_watchHits;  // rva, count

        LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* a_ep)
        {
            if (a_ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(a_ep->ContextRecord->Dr6 & 1)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            a_ep->ContextRecord->Dr6 = 0;
            const auto rip = static_cast<std::uintptr_t>(a_ep->ContextRecord->Rip);
            {
                std::scoped_lock l(g_watchLock);
                bool found = false;
                for (auto& h : g_watchHits) {
                    if (h.first == rip) {
                        ++h.second, found = true;
                        break;
                    }
                }
                if (!found && g_watchHits.size() < 32) g_watchHits.emplace_back(rip, 1);
            }
            if (--g_watchLeft <= 0) {
                a_ep->ContextRecord->Dr7 &= ~std::uint64_t{ 3 };  // this thread stops; others are cleared by Disarm
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        int SetDrAllThreads(std::uintptr_t a_addr, bool a_on)
        {
            int         n = 0;
            const DWORD pid = ::GetCurrentProcessId(), self = ::GetCurrentThreadId();
            HANDLE      snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return 0;
            THREADENTRY32 te{ sizeof(te) };
            for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE t = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!t) continue;
                if (::SuspendThread(t) != static_cast<DWORD>(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (::GetThreadContext(t, &c)) {
                        if (a_on) {
                            c.Dr0 = a_addr;
                            c.Dr7 = (c.Dr7 & ~std::uint64_t{ 0xF0003 }) | 1 | (std::uint64_t{ 1 } << 16) | (std::uint64_t{ 3 } << 18);  // L0, write, 4 bytes
                        } else {
                            c.Dr0 = 0;
                            c.Dr7 &= ~std::uint64_t{ 0xF0003 };
                        }
                        n += ::SetThreadContext(t, &c) ? 1 : 0;
                    }
                    ::ResumeThread(t);
                }
                ::CloseHandle(t);
            }
            ::CloseHandle(snap);
            return n;
        }
    }

    std::string WatchRoot(int a_hits)
    {
        if (a_hits <= 0) {
            const int n = SetDrAllThreads(0, false);
            g_watchLeft = 0;
            std::string s = std::format("watch off ({} threads); hits:", n);
            std::scoped_lock l(g_watchLock);
            for (auto& h : g_watchHits) s += std::format(" rva {:X} x{}", h.first - Base(), h.second);
            return s;
        }
        auto* pc = RE::PlayerCamera::GetSingleton();
        auto* root = pc ? reinterpret_cast<std::uint8_t*>(pc->cameraRoot.get()) : nullptr;
        if (!root) return "no camera root";
        if (!g_veh) g_veh = ::AddVectoredExceptionHandler(1, &WatchVeh);
        {
            std::scoped_lock l(g_watchLock);
            g_watchHits.clear();
        }
        g_watchAddr = reinterpret_cast<std::uintptr_t>(root + 0x30 + 16 + 8);
        g_watchLeft = a_hits;
        const int n = SetDrAllThreads(g_watchAddr, true);
        return std::format("watching {:X} (root local row1.z) for {} hits in {} threads", g_watchAddr, a_hits, n);
    }

    void NoteCameraPitch(int a_site)
    {
        if (a_site < 1 || a_site > 5) {
            return;
        }
        float z = 0.0f;
        if (a_site == 1) {
            auto* pc = RE::PlayerCamera::GetSingleton();
            auto* root = pc ? pc->cameraRoot.get() : nullptr;
            if (!root) return;
            z = reinterpret_cast<const float*>(&root->local.rotate)[4 + 2];  // row 1 = view dir (NiCamera row 0)
        } else if (a_site == 2) {
            auto* cam = *reinterpret_cast<std::uint8_t**>(Base() + 0x3E62210);  // DrawWorld::spCamera
            if (!cam) return;
            z = *reinterpret_cast<const float*>(cam + 0x70 + 8);  // world rotate row 0 = view dir
        } else {
            // 3 = camera root local at render, 4 = camera root world at render, 5 = spCamera local row 0 (relative to root)
            auto* pc = RE::PlayerCamera::GetSingleton();
            auto* root = pc ? reinterpret_cast<std::uint8_t*>(pc->cameraRoot.get()) : nullptr;
            auto* cam = *reinterpret_cast<std::uint8_t**>(Base() + 0x3E62210);
            if (a_site == 3 && root) z = *reinterpret_cast<const float*>(root + 0x30 + 16 + 8);
            else if (a_site == 4 && root) z = *reinterpret_cast<const float*>(root + 0x70 + 16 + 8);
            else if (a_site == 5 && cam) z = *reinterpret_cast<const float*>(cam + 0x30 + 8);
            else return;
        }
        const float deg = std::asin(std::clamp(z, -1.0f, 1.0f)) * 57.2957795f;
        auto& p = g_pitch[a_site];
        ++p.n;
        if (std::abs(deg) > 0.5f) ++p.tilted;
        p.last.store(deg);
        if (std::abs(deg) > p.maxAbs.load()) p.maxAbs.store(std::abs(deg));
    }

    std::string Status()
    {
        std::string label;
        std::uint64_t seq = 0;
        bool          valid = false;
        {
            std::scoped_lock l(g_poseLock);
            label = g_labelNote;
            seq = g_pose.seq;
            valid = g_pose.valid;
        }
        return std::format("vatswin: {} enabled={} active={} every={} scene={} fov={:.1f} renders={} episode={} composes={} hookCalls={} skips={} fails={} "
                           "labelCalls={} labelOurs={} pose={}#{} cpuAvg={:.3f}ms cpuMax={:.3f}ms rt=[{}] maskValid={} err='{}' {} | {}",
            g_installNote, Enabled() ? 1 : 0, g_active.load() ? 1 : 0, g_every.load(), g_scene.load(), g_fovDeg.load(), S().renders.load(),
            S().episodeRenders.load(), S().composes.load(), g_hookCalls.load(), g_skips.load(), g_renderFails.load(), g_labelCalls.load(),
            g_labelOurs.load(), valid ? 1 : 0, seq, g_cpuMsAvg.load(), g_cpuMsMax.load(), g_rtNote, S().maskValid.load() ? 1 : 0, g_lastErr,
            detail::GpuStatus(), label) +
               std::format(" | pitch update n {} tilted {} last {:.2f} max {:.2f} | render n {} tilted {} last {:.2f} max {:.2f}", g_pitch[1].n.load(),
                   g_pitch[1].tilted.load(), g_pitch[1].last.load(), g_pitch[1].maxAbs.load(), g_pitch[2].n.load(), g_pitch[2].tilted.load(),
                   g_pitch[2].last.load(), g_pitch[2].maxAbs.load()) +
               std::format(" | rootLocal {:.2f} rootWorld {:.2f} camLocal {:.2f} | baseCam {} undone {}", g_pitch[3].last.load(), g_pitch[4].last.load(),
                   g_pitch[5].last.load(), g_baseCamCalls.load(), g_baseCamUndone.load()) +
               std::format(" | sun {} {:X} map {} type {} lights {}/{} shadow {}/{} s0 {:X}/{} s1 {:X}/{} s2 {:X}/{}", g_sun.load(), g_ld.sunState, g_ld.sunMap, g_ld.sunType, g_ld.lightsLit, g_ld.lights, g_ld.shadowLit, g_ld.shadowLights,
                   g_ld.shadowState[0], g_ld.shadowType[0], g_ld.shadowState[1], g_ld.shadowType[1], g_ld.shadowState[2], g_ld.shadowType[2]);
    }

    std::string Command(const std::string& a_arg)
    {
        auto sp = a_arg.find(' ');
        const std::string k = a_arg.substr(0, sp);
        const std::string v = sp == std::string::npos ? std::string{} : a_arg.substr(sp + 1);
        if (k == "on" || k == "off") {
            if (!g_installed) {
                return "error: not installed (" + g_installNote + ")";
            }
            g_enabled.store(k == "on");
        } else if (k == "every" && !v.empty()) {
            g_every.store(std::clamp(std::atoi(v.c_str()), 1, 8));
        } else if (k == "scene" && !v.empty()) {
            g_scene.store(std::clamp(std::atoi(v.c_str()), 0, 1));
        } else if (k == "fov" && !v.empty()) {
            g_fovDeg.store(static_cast<float>(std::atof(v.c_str())));
        } else if (k == "exposure" && !v.empty()) {
            S().exposure.store(Clampf(static_cast<float>(std::atof(v.c_str())), 0.01f, 64.0f));
        } else if (k == "mask" && !v.empty()) {
            S().mask.store(std::atoi(v.c_str()) != 0);
        } else if (k == "maskstrength" && !v.empty()) {
            S().maskStrength.store(Clampf(static_cast<float>(std::atof(v.c_str())), 0.0f, 1.0f));
        } else if (k == "keeplists" && !v.empty()) {
            g_keepLists.store(std::atoi(v.c_str()) != 0);
        } else if (k == "sun" && !v.empty()) {
            g_sun.store(std::clamp(std::atoi(v.c_str()), 0, 2));
        } else if (k == "ambient" && !v.empty()) {
            S().ambient.store(Clampf(static_cast<float>(std::atof(v.c_str())), 0.0f, 8.0f));
        } else if (k == "fxoff" && !v.empty()) {
            g_fxOff.store(std::atoi(v.c_str()) != 0);
        } else if (k == "watch") {
            return WatchRoot(v.empty() ? 0 : std::atoi(v.c_str()));
        } else if (k == "pitchreset") {
            for (auto& p : g_pitch) p.n = 0, p.tilted = 0, p.last = 0.0f, p.maxAbs = 0.0f;
        } else if (k == "cap" && !v.empty()) {
            detail::RequestCapture(v);
            return "capture requested: " + v;
        } else if (!k.empty() && k != "status") {
            return "usage: vatswin [status|on|off|every n|scene 0|1|fov deg|exposure x|mask 0|1|maskstrength x|cap <path.bmp>]";
        }
        return Status();
    }
}
