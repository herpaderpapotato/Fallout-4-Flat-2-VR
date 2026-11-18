// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/CameraDirector.h"
#include "plugin/CameraTweaks.h"
#include "plugin/Comfort.h"
#include "plugin/ControllerBindings.h"
#include "plugin/Dialogue.h"
#include "plugin/GameBridge.h"
#include "plugin/HeadPose.h"
#include "plugin/HudLayer.h"
#include "plugin/MenuPointer.h"
#include "plugin/GameMenuPointer.h"
#include "plugin/GamePipboy.h"
#include "plugin/PipboyLayer.h"
#include "plugin/VirtualPad.h"
#include "plugin/Presenter.h"
#include "plugin/Roomscale.h"
#include "plugin/StereoPoC.h"
#include "plugin/VatsWindow.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/Gestures.h"
#include "plugin/Haptics.h"
#include "xr/XrRuntime.h"

#include <Windows.h>
#undef ERROR
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fo4flatvr::present
{
    namespace
    {
        using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        constexpr std::size_t kPresentSlot = 8;  // IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1)

        struct State
        {
            IDXGISwapChain*      swapChain{ nullptr };
            ID3D11Device*        device{ nullptr };
            ID3D11DeviceContext* context{ nullptr };
            Settings             settings;
            Present_t            original{ nullptr };

            xr::Runtime runtime;
            bool        xrTried{ false };
            bool        xrOk{ false };
            bool        submitting{ true };
            bool        recenterDown{ false };
            bool        toggleDown{ false };
            bool        cameraCycleDown{ false };  // round 8 hotkeys
            bool        povToggleDown{ false };

            std::atomic<std::uint64_t> frames{ 0 };
            std::atomic<std::uint64_t> xrFrames{ 0 };

            // Mono mode: the frame presented now was rendered from the pose/FOV handed to the game
            // at the previous Present, so that is what the projection layer must claim.
            bool     mono{ false };
            bool     afr{ false };              // alternate-eye stereo (implies the world view)
            int      afrNextEye{ 0 };
            int      renderEye{ 0 };            // eye the in-flight game frame is rendered for
            int      lastCamState{ -2 };
            std::string lastMenu;
            // Menus like FO4VR (UprightHmdLagNode, frozen while a menu is open): the quad (flat fallback or menu panel)
            // is anchored in front of the user when it starts showing, then world-locked.
            std::string lastPanel;                // menu shown as a panel over the live world view ("" = none)
            bool        vatsOverlay{ false };     // a FlatScreenModel menu (VATSMenu with the camera gate, ...) laid head-locked over the live world
            float       vatsPivotU{ -1.0f }, vatsPivotV{ -1.0f };  // last VATS label magnification pivot (status)
            std::string overlayMenu;              // its name ("" = none)
            std::uint64_t vatsOverlayFrames{ 0 };
            // world panorama behind the flat window (round 4): capture bookkeeping
            int           panoStable{ 0 };          // consecutive tracked world-view frames (capture from the 3rd on)
            float         panoLastYaw{ 0.0f };
            std::uint64_t panoTick{ 0 }, panoShown{ 0 }, panoFailed{ 0 };
            bool        quadEpisode{ false };     // the cinema quad / menu panel is shown
            std::string episodeKey;               // what it shows (menu name / camera state)
            float       anchorYaw{ 0.0f };        // head yaw the quad was anchored to (radians, OpenXR, + = left)
            std::uint64_t anchors{ 0 };
            bool          workbenchGeometry{ false };  // round 6: the last anchor used FO4VR's workbench panel geometry
            bool          workshopOnHand{ false };     // round 6: the WorkshopMenu panel follows the off-hand grip this frame
            std::uint64_t workshopHandFrames{ 0 };
            // round 7: a terminal on its own monitor in the live world (FO4VR), "" = none; panel geometry source of the last anchor
            std::string   worldScreen;
            std::uint64_t worldScreenFrames{ 0 };
            const char*   panelGeometry{ "cinema" };
            double   periodMs{ 0.0 };
            bool     frameOpen{ false };          // an XR frame was begun at the previous Present
            bool     hudLayer{ false };           // D1: the next frame's HUD goes to its own layer
            bool     frameShouldRender{ false };
            float    loggedFov{ 0.0f };
            float    loggedUsedFov{ 0.0f };
            bool     haveRenderPose{ false };
            long long frameXrTime{ 0 };           // htrace: predicted display time of the open XR frame
            unsigned long long frameSeq{ 0 };     // htrace: head publish sequence of the open XR frame
            bool     pubHeadValid{ false };       // head pose last handed to head::PublishXr (= what the
            xr::Pose pubHead;                     //   camera root now on screen was rendered with)
            xr::Pose renderPose;
            xr::Fov  renderFov;
            xr::SessionState lastState{ xr::SessionState::kNone };
        };

        State*         g_state = nullptr;
        std::once_flag g_installOnce;

        // Frame pacing statistics, logged every 600 frames: interval avg/max, intervals longer than
        // 1.5x the headset period, and time blocked in xrWaitFrame/BeginFrame.
        struct Pacing
        {
            LARGE_INTEGER last{};
            double        sum{ 0 }, max{ 0 }, waitSum{ 0 }, waitMax{ 0 };
            int           n{ 0 }, slow{ 0 };
        };
        Pacing g_pacing;

        double QpcMs(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
        {
            static LARGE_INTEGER f{};
            if (!f.QuadPart) {
                ::QueryPerformanceFrequency(&f);
            }
            return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        }


        // Cross-thread requests (command channel -> render thread) and status snapshot.
        std::mutex        g_reqLock;
        std::string       g_capturePath;
        std::string       g_stereoEyeCapStem;  // stereo PoC E7: back buffer + both submitted eye images of one frame
        // Burst capture ("captureburst <n> <stem>"): the back buffer of n consecutive presents into GPU staging copies,
        // written as <stem>_00.bmp ... after the last one (no file I/O between the frames).
        std::string                    g_burstStem;
        int                            g_burstWant{ 0 };
        std::vector<ID3D11Texture2D*>  g_burstTex;  // render thread only
        std::string                    g_burstActiveStem;
        int                            g_burstLeft{ 0 };
        std::uint64_t     g_stereoSubmits{ 0 };
        bool              g_recenterRequested{ false };
        int               g_modeRequest{ -1 };  // -1 none, 0 cinema, 1 mono
        std::string       g_status{ "presenter not installed" };

        void WriteBmp(const std::string& a_path, const std::uint8_t* a_rows, UINT a_pitch, UINT a_w, UINT a_h, bool a_bgra)
        {
            const std::uint32_t rowBytes = a_w * 4;
            const std::uint32_t imageSize = rowBytes * a_h;
            BITMAPFILEHEADER    fh{};
            BITMAPINFOHEADER    ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + imageSize;
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(a_w);
            ih.biHeight = -static_cast<LONG>(a_h);  // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = imageSize;

            HANDLE f = ::CreateFileA(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                REX::ERROR("capture: cannot open {} ({})", a_path, ::GetLastError());
                return;
            }
            DWORD written = 0;
            ::WriteFile(f, &fh, sizeof(fh), &written, nullptr);
            ::WriteFile(f, &ih, sizeof(ih), &written, nullptr);
            std::vector<std::uint8_t> row(rowBytes);
            for (UINT y = 0; y < a_h; ++y) {
                const std::uint8_t* src = a_rows + static_cast<std::size_t>(y) * a_pitch;
                for (UINT x = 0; x < a_w; ++x) {
                    const std::uint8_t* p = src + x * 4;
                    std::uint8_t*       d = row.data() + x * 4;
                    d[0] = a_bgra ? p[0] : p[2];  // BMP wants B,G,R,X
                    d[1] = p[1];
                    d[2] = a_bgra ? p[2] : p[0];
                    d[3] = 255;
                }
                ::WriteFile(f, row.data(), rowBytes, &written, nullptr);
            }
            ::CloseHandle(f);
            REX::INFO("capture: wrote {} ({}x{})", a_path, a_w, a_h);
        }

        void Capture(State& s, ID3D11Texture2D* a_src, const std::string& a_path)
        {
            D3D11_TEXTURE2D_DESC desc{};
            a_src->GetDesc(&desc);
            bool bgra = false;
            switch (desc.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                bgra = true;
                break;
            default:
                REX::ERROR("capture: unsupported backbuffer format {}", static_cast<int>(desc.Format));
                return;
            }
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.SampleDesc = { 1, 0 };
            if (desc.SampleDesc.Count > 1) {
                REX::ERROR("capture: multisampled backbuffer not supported");
                return;
            }
            ID3D11Texture2D* staging = nullptr;
            if (FAILED(s.device->CreateTexture2D(&sd, nullptr, &staging))) {
                REX::ERROR("capture: staging texture creation failed");
                return;
            }
            s.context->CopySubresourceRegion(staging, 0, 0, 0, 0, a_src, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(s.context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                WriteBmp(a_path, static_cast<const std::uint8_t*>(m.pData), m.RowPitch, desc.Width, desc.Height, bgra);
                s.context->Unmap(staging, 0);
            }
            staging->Release();
        }

        void BurstFrame(State& s, ID3D11Texture2D* a_src)
        {
            D3D11_TEXTURE2D_DESC desc{};
            a_src->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            ID3D11Texture2D* staging = nullptr;
            if (desc.SampleDesc.Count == 1 && SUCCEEDED(s.device->CreateTexture2D(&sd, nullptr, &staging))) {
                s.context->CopyResource(staging, a_src);
                g_burstTex.push_back(staging);
            }
            if (--g_burstLeft > 0) {
                return;
            }
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            for (std::size_t i = 0; i < g_burstTex.size(); ++i) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(s.context->Map(g_burstTex[i], 0, D3D11_MAP_READ, 0, &m))) {
                    WriteBmp(std::format("{}_{:02}.bmp", g_burstActiveStem, i), static_cast<const std::uint8_t*>(m.pData), m.RowPitch,
                        desc.Width, desc.Height, bgra);
                    s.context->Unmap(g_burstTex[i], 0);
                }
                g_burstTex[i]->Release();
            }
            REX::INFO("captureburst: {} frames -> {}_NN.bmp done", g_burstTex.size(), g_burstActiveStem);
            g_burstTex.clear();
        }

        // Crop burst ("cropburst <n> <file> <x0> <x1> <y> <w> <h> <step>"): two w x h rects of the back buffer (at x0 and x1,
        // row y) for n consecutive presents, decimated by step, appended to one raw file (header "FCRB", u32 w', h', n; then
        // per frame w' x h' BGR, both rects side by side). A ring of staging copies is read back 6 frames late (no stall).
        struct CropBurst
        {
            int                           left{ 0 }, total{ 0 }, done{ 0 };
            UINT                          x0{ 0 }, x1{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }, step{ 1 };
            std::string                   path;
            HANDLE                        f{ INVALID_HANDLE_VALUE };
            std::vector<ID3D11Texture2D*> ring;
            int                           issued{ 0 };
            bool                          bgra{ false };
        };
        CropBurst     g_crop;
        CropBurst     g_cropReq;
        constexpr int kCropRing = 6;

        void CropWrite(State& s, ID3D11Texture2D* a_t)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(s.context->Map(a_t, 0, D3D11_MAP_READ, 0, &m))) {
                return;
            }
            const UINT ow = (g_crop.w * 2) / g_crop.step, oh = g_crop.h / g_crop.step;
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(ow) * oh * 3);
            for (UINT yy = 0; yy < oh; ++yy) {
                const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(yy * g_crop.step) * m.RowPitch;
                for (UINT xx = 0; xx < ow; ++xx) {
                    const std::uint8_t* p = row + static_cast<std::size_t>(xx * g_crop.step) * 4;
                    std::uint8_t*       d = buf.data() + (static_cast<std::size_t>(yy) * ow + xx) * 3;
                    d[0] = g_crop.bgra ? p[0] : p[2];
                    d[1] = p[1];
                    d[2] = g_crop.bgra ? p[2] : p[0];
                }
            }
            s.context->Unmap(a_t, 0);
            DWORD wr = 0;
            ::WriteFile(g_crop.f, buf.data(), static_cast<DWORD>(buf.size()), &wr, nullptr);
            ++g_crop.done;
        }

        void CropFrame(State& s, ID3D11Texture2D* a_src)
        {
            if (g_crop.ring.empty()) {
                D3D11_TEXTURE2D_DESC desc{};
                a_src->GetDesc(&desc);
                g_crop.bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
                g_crop.w = (std::min)(g_crop.w, desc.Width);
                g_crop.h = (std::min)(g_crop.h, desc.Height);
                g_crop.x0 = (std::min)(g_crop.x0, desc.Width - g_crop.w);
                g_crop.x1 = (std::min)(g_crop.x1, desc.Width - g_crop.w);
                g_crop.y = (std::min)(g_crop.y, desc.Height - g_crop.h);
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Width = g_crop.w * 2;
                sd.Height = g_crop.h;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                for (int i = 0; i < kCropRing; ++i) {
                    ID3D11Texture2D* t = nullptr;
                    if (FAILED(s.device->CreateTexture2D(&sd, nullptr, &t))) {
                        REX::ERROR("cropburst: staging creation failed");
                        for (auto* r : g_crop.ring) {
                            r->Release();
                        }
                        g_crop = CropBurst{};
                        return;
                    }
                    g_crop.ring.push_back(t);
                }
                g_crop.f = ::CreateFileA(g_crop.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                const std::uint32_t hdr[4] = { 0x42524346u, (g_crop.w * 2) / g_crop.step, g_crop.h / g_crop.step,
                    static_cast<std::uint32_t>(g_crop.total) };
                DWORD wr = 0;
                ::WriteFile(g_crop.f, hdr, sizeof(hdr), &wr, nullptr);
            }
            if (g_crop.left > 0) {
                ID3D11Texture2D* t = g_crop.ring[g_crop.issued % kCropRing];
                if (g_crop.issued >= kCropRing) {
                    CropWrite(s, t);  // the copy issued kCropRing presents ago
                }
                D3D11_BOX b0{ g_crop.x0, g_crop.y, 0, g_crop.x0 + g_crop.w, g_crop.y + g_crop.h, 1 };
                D3D11_BOX b1{ g_crop.x1, g_crop.y, 0, g_crop.x1 + g_crop.w, g_crop.y + g_crop.h, 1 };
                s.context->CopySubresourceRegion(t, 0, 0, 0, 0, a_src, 0, &b0);
                s.context->CopySubresourceRegion(t, 0, g_crop.w, 0, 0, a_src, 0, &b1);
                ++g_crop.issued;
                --g_crop.left;
            }
            if (g_crop.left == 0) {
                for (int k = (std::max)(0, g_crop.issued - kCropRing); k < g_crop.issued; ++k) {
                    CropWrite(s, g_crop.ring[k % kCropRing]);
                }
                for (auto* t : g_crop.ring) {
                    t->Release();
                }
                if (g_crop.f != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(g_crop.f);
                }
                REX::INFO("cropburst: {} frames -> {} done", g_crop.done, g_crop.path);
                g_crop = CropBurst{};
            }
        }

        bool KeyPressed(int a_vk, bool& a_wasDown)
        {
            const bool down = a_vk != 0 && (::GetAsyncKeyState(a_vk) & 0x8000) != 0;
            const bool pressed = down && !a_wasDown;
            a_wasDown = down;
            return pressed;
        }

        void InitXr(State& s)
        {
            s.mono = s.settings.mono || s.settings.afr;
            s.afr = s.settings.afr;
            s.xrTried = true;
            if (!s.settings.xrRuntimeJson.empty()) {
                ::SetEnvironmentVariableA("XR_RUNTIME_JSON", s.settings.xrRuntimeJson.c_str());
                _putenv_s("XR_RUNTIME_JSON", s.settings.xrRuntimeJson.c_str());
                REX::INFO("XR_RUNTIME_JSON forced to {}", s.settings.xrRuntimeJson);
            }
            xr::Config cfg;
            cfg.preferStageSpace = s.settings.preferStageSpace;
            cfg.quad.widthMeters = s.settings.quadWidthMeters;
            cfg.quad.distanceMeters = s.settings.quadDistanceMeters;
            cfg.quad.heightMeters = s.settings.quadHeightMeters;
            s.xrOk = s.runtime.Init(s.device, cfg);
            if (s.xrOk) {
                REX::INFO("OpenXR up: {}", s.runtime.runtimeName());
            } else {
                REX::ERROR("OpenXR init failed: {} (VR disabled for this session)", s.runtime.lastError());
            }
        }

        xr::Vec3 Cross(const xr::Vec3& a, const xr::Vec3& b)
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }

        xr::Vec3 RotXr(const xr::Quat& q, const xr::Vec3& v)  // q * v * q^-1 (unit q)
        {
            const xr::Vec3 u{ q.x, q.y, q.z };
            const xr::Vec3 c = Cross(u, v);
            const xr::Vec3 t{ 2 * c.x, 2 * c.y, 2 * c.z };
            const xr::Vec3 c2 = Cross(u, t);
            return { v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z };
        }

        // Yaw (heading convention: + = right) of a pose's forward (-Z) in the reference space; false when it points
        // almost straight up/down. Comfort (C8): off-hand direction relative to the head.
        bool YawXr(const xr::Quat& q, float& a_yaw)
        {
            const xr::Vec3 f = RotXr(q, { 0.0f, 0.0f, -1.0f });
            if (f.x * f.x + f.z * f.z < 0.09f) {
                return false;
            }
            a_yaw = std::atan2(f.x, -f.z);
            return true;
        }

        // Flat 2D menus shown as a panel over the live, head-tracked world view (FO4VR: world-space menu models on
        // UprightHmdLagNode, world still rendered behind). Their FlatScreenModel output is copied (GameHud H1) and left
        // out of the world image (H2). Eligible = drawn entirely by FlatScreenModel, camera not taken, world behind, no
        // screen-space 3D preview (game::PanelEligible). Any other view-taking menu keeps the flat-screen fallback.
        // Round 8: [Menus] sWindowMenus - menus the user opts into the flat window (",name,name," lower-case; set in VrFrame)
        std::string g_windowList;
        bool WindowOptIn(const char* a_name)
        {
            if (!a_name || g_windowList.size() < 3) {
                return false;
            }
            std::string k = "," + std::string(a_name) + ",";
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return g_windowList.find(k) != std::string::npos;
        }

        bool IsPanelMenu(const char* a_name)
        {
            return !WindowOptIn(a_name) && game::PanelEligible(a_name);
        }

        // Crafting menus (WorkbenchMenuBase family; FO4VR renders them all through WSWorkbenchModel) + their exit prompt.
        bool IsCraftingMenu(const std::string& a_name)
        {
            return a_name == "CookingMenu" || a_name == "ExamineMenu" || a_name == "PowerArmorModMenu" || a_name == "RobotModMenu" ||
                   a_name == "ExamineConfirmMenu";
        }

        // Round 7 (analysis/vr-terminals-pipboy.md): view-taking menus that FO4VR shows in the live world and so do we, without
        // the flat window: a furniture terminal on its own monitor (game::WorldScreenMenu; FO4VR TerminalMenu::InitMenu BDFBF0),
        // and Pip-Boy screen content while the Pip-Boy is open directly (wrist / projected): holotape games draw into the
        // Pip-Boy renderer (PipboyManager::AddMenuToPipboy), a terminal without a terminal ref too (InitMenu: renderer
        // PipboyManager::Get3DRendererName), so they are on the Pip-Boy quad.
        bool InWorldMenu(const char* a_name)
        {
            if (!a_name || WindowOptIn(a_name)) {
                return false;
            }
            if (game::WorldScreenMenu(a_name)) {
                return true;
            }
            if (game::PipboyWristOpen()) {
                if (_stricmp(a_name, "PipboyHolotapeMenu") == 0) {
                    return true;
                }
                if (_stricmp(a_name, "TerminalMenu") == 0 && game::MenuUsesRenderer("TerminalMenu", "PipboyMenu")) {
                    return true;
                }
            }
            return false;
        }

        bool PanelOrWorldMenu(const char* a_name)
        {
            return IsPanelMenu(a_name) || InWorldMenu(a_name);
        }

        // FO4VR's world-space menu model per menu (WS*Model ctors read in Ghidra, meshes measured; analysis/vr-terminals-pipboy.md
        // section 3), as the flat stand-in quad: width = arc length, distance from the head, centre height (m). All these models
        // hang on UprightHmdLagNode with identity placement settings (f<Menu>X/Y/Z/Pitch = 0, scale 1; LevelUp scale 2).
        // false = the [Cinema] geometry.
        bool Fo4vrPanelGeometry(const State& s, const std::string& a_name, float& a_w, float& a_d, float& a_h, const char*& a_src)
        {
            const float ww = s.settings.menuWorkbenchWidthMeters, wd = s.settings.menuWorkbenchDistanceMeters, wh = s.settings.menuWorkbenchHeightMeters;
            {
                // round 8: [MenuGeometry] <MenuName>=width,distance,height wins over everything
                std::string key = a_name;
                for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (const auto it = s.settings.menuGeometry.find(key); it != s.settings.menuGeometry.end()) {
                    a_w = it->second[0], a_d = it->second[1], a_h = it->second[2], a_src = "[MenuGeometry]";
                    return true;
                }
            }
            if (a_name == "DialogueMenu" && s.settings.menuPanelFO4VR) {  // round 8: FO4VR dialogue band scale (world_Dialogue.nif)
                a_w = s.settings.menuDialogueWidthMeters, a_d = s.settings.menuDialogueDistanceMeters, a_h = s.settings.menuDialogueHeightMeters;
                a_src = "dialogue (world_Dialogue.nif scale)";
                return true;
            }
            if (s.settings.menuWorkbenchPanel && IsCraftingMenu(a_name)) {  // round 6: WSWorkbenchModel / WSExamineMenuModel
                a_w = ww, a_d = wd, a_h = wh, a_src = "workbench (world_projectedMenu.nif)";
                return true;
            }
            if (!s.settings.menuPanelFO4VR) {
                return false;
            }
            // WSContainerModel (Container + Barter), WSLockpickingModel, WSSleepWaitModel, WSSPECIALModel: world_projectedMenu.nif
            if (a_name == "ContainerMenu" || a_name == "BarterMenu" || a_name == "LockpickingMenu" || a_name == "SleepWaitMenu" ||
                a_name == "SPECIALMenu") {
                a_w = ww, a_d = wd, a_h = wh, a_src = "world_projectedMenu.nif";
                return true;
            }
            if (a_name == "LevelUpMenu") {  // WSLevelUpMenuModel: world_projectedMenu.nif at fLevelUpMenuScale:VRUI = 2
                a_w = 2.0f * ww, a_d = 2.0f * wd, a_h = 2.0f * wh, a_src = "world_projectedMenu.nif x2";
                return true;
            }
            if (a_name == "PauseMenu") {  // WSPauseMenuModel: world_projected_MainPause.nif (curved, 109 deg, r ~0.78 m, -0.045 m)
                a_w = s.settings.menuPauseWidthMeters, a_d = s.settings.menuPauseDistanceMeters, a_h = s.settings.menuPauseHeightMeters;
                a_src = "world_projected_MainPause.nif";
                return true;
            }
            if (a_name == "BookMenu") {  // WSBookMenuModel: world_projectedMenu_Flat.nif (flat 1.00 m at 0.584 m, -0.051 m)
                a_w = 1.0f, a_d = 0.584f, a_h = -0.051f, a_src = "world_projectedMenu_Flat.nif";
                return true;
            }
            return false;
        }

        // Reference space -> game-world orientation for the panorama: the owned heading as a yaw (camera = Rz(ownYaw) x head;
        // a right turn by h = rotation about +Y by -h in OpenXR axes). Without the owned heading: identity (a stick turn
        // then misplaces what was captured before it).
        xr::Quat PanoRefToWorld(float& a_yaw)
        {
            a_yaw = 0.0f;
            if (!rs::OwnedYaw(a_yaw)) {
                a_yaw = 0.0f;
            }
            return { 0.0f, std::sin(-a_yaw * 0.5f), 0.0f, std::cos(-a_yaw * 0.5f) };
        }

        float WrapRad(float a)
        {
            while (a > 3.14159265f) a -= 6.28318531f;
            while (a < -3.14159265f) a += 6.28318531f;
            return a;
        }

        // Controller aim: the aim hand relative to the head pose the camera root on screen was rendered
        // with (s.pubHead), in the game's camera-local basis. GameBridge turns it into the world ray.
        // Called after BeginFrame (fresh controller poses), before the new head pose is published.
        void PublishHandRay(State& s, bool a_began)
        {
            const auto&     c = s.runtime.Controllers();
            const bool      left = s.settings.leftHanded;
            const xr::Pose& hand = left ? c.leftAim : c.rightAim;
            const bool      ok = a_began && c.active && (left ? c.leftAimValid : c.rightAimValid) && s.pubHeadValid;
            float           pos[3]{}, dir[3]{};
            if (ok) {
                const auto&    hq = s.pubHead.orientation;
                const xr::Quat inv{ -hq.x, -hq.y, -hq.z, hq.w };
                const xr::Vec3 dp{ hand.position.x - s.pubHead.position.x, hand.position.y - s.pubHead.position.y,
                    hand.position.z - s.pubHead.position.z };
                const xr::Vec3 lp = RotXr(inv, dp);
                const xr::Vec3 ld = RotXr(inv, RotXr(hand.orientation, { 0.0f, 0.0f, -1.0f }));  // aim pose looks down -Z
                // OpenXR (X right, Y up, -Z forward) -> game (X right, Y forward, Z up): (x, -z, y), as HeadPose
                pos[0] = lp.x; pos[1] = -lp.z; pos[2] = lp.y;
                dir[0] = ld.x; dir[1] = -ld.z; dir[2] = ld.y;
            }
            game::UpdateHandRay(pos, dir, ok);
        }

        void VrFrame(State& s)
        {
            if (!s.xrTried) {
                InitXr(s);
            }
            // Camera state and view-taking menus are tracked (and logged) even without XR so tests can
            // see transitions. World view only in first person with no such menu open.
            // Wrist Pip-Boy (D3): a directly opened PipboyMenu keeps the world view (its screen is on the hand quad).
            if (g_windowList.empty()) {  // round 8: [Menus] sWindowMenus (once)
                g_windowList = ",";
                for (char ch : s.settings.menuWindowMenus) {
                    if (ch != ' ' && ch != '\t') g_windowList += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                g_windowList += ",";
                if (g_windowList.size() > 2) {
                    REX::INFO("menus opted into the flat window ([Menus] sWindowMenus): {}", s.settings.menuWindowMenus);
                }
            }
            const char* pipIgnore = game::PipboyWristOpen() ? "PipboyMenu" : nullptr;
            // round 7: a terminal on its monitor / holotape content on a directly opened Pip-Boy stay in the world (InWorldMenu)
            const char* menu = game::ViewTakingMenu(pipIgnore, &InWorldMenu);
            // Menu panel: a flat 2D menu over the live world view (only when every open view-taking menu is a panel
            // menu, the camera policy is a world view and the panel switch is on).
            const char* panel = nullptr;
            // panel only while the view is actually tracked (TRACK / LOCK): under FADE_SNAP the world is fading to black
            // (round 4: the workbench camera's Transition/Animated states held 5 s of black over the cooking panel) -> the
            // flat window with the panorama instead, and the director drops the fade while that window is up
            const camdir::Policy panelPol = camdir::ViewPolicy(game::CurrentCameraState());
            if (menu && IsPanelMenu(menu) && s.mono && hud::MenuPanelEnabled() &&
                (panelPol == camdir::Policy::kTrack || panelPol == camdir::Policy::kLock) &&
                !game::ViewTakingMenu(pipIgnore, &PanelOrWorldMenu)) {
                panel = menu;
                menu = nullptr;
            }
            camdir::SetMenuWindow(menu != nullptr);
            {
                // round 7: terminal on its own monitor in the live world (pointer target: TerminalMenu)
                std::string ws = (!menu && !panel && s.mono && game::WorldScreenMenu("TerminalMenu")) ? "TerminalMenu" : "";
                if (ws.empty() && !menu && !panel && s.mono && game::WorldScreenMenu("TerminalHolotapeMenu")) {
                    ws = "TerminalHolotapeMenu";
                }
                if (ws != s.worldScreen) {
                    REX::INFO("terminal in world (FO4VR: on its own monitor, live world, pointer on the screen) {} -> {}",
                        s.worldScreen.empty() ? "-" : s.worldScreen, ws.empty() ? "-" : ws);
                    s.worldScreen = ws;
                }
            }
            // Dialogue options (DialogueMenu, FlatScreenModel; not view-taking: first-person dialogue stays head-tracked) on
            // the same panel: otherwise the game draws them at the bottom centre of the double-wide stereo frame = the seam
            // between the eyes ("in my nose"). [Menus] bDialoguePanel; the [Dialogue] crop panel takes precedence if on.
            if (!menu && !panel && s.mono && s.settings.menuDialoguePanel && hud::MenuPanelEnabled() && !dlg::GetConfig().panel &&
                camdir::WorldView(game::CurrentCameraState()) && game::MenuOpen("DialogueMenu")) {
                panel = "DialogueMenu";
            }
            if (std::string(panel ? panel : "") != s.lastPanel) {
                REX::INFO("menu panel {} -> {}", s.lastPanel.empty() ? "-" : s.lastPanel, panel ? panel : "-");
                s.lastPanel = panel ? panel : "";
            }
            // VATS over the live world: with the VATS camera gate (camdir::VatsWorldView: the VATS camera is not pushed, the
            // player camera stays enabled; [Menus] bVatsLiveWorld turns the gate on) VATSMenu is not view-taking, the flat
            // VATS logic runs as usual, and its overlay (FlatScreenModel: target boxes, hit chances, queue) is laid
            // head-locked over the frame it was drawn for, so the labels stay on their targets.
            // The same for any other FlatScreenModel menu open in the world view that is not a panel (it would be drawn
            // screen-space into the double-wide stereo frame, split at the eye seam).
            {
                const std::string ov = (!panel && !menu && s.mono && hud::MenuPanelEnabled()) ? game::FlatOverlayMenu() : std::string{};
                if (ov != s.overlayMenu) {
                    REX::INFO("menu overlay (head-locked over the live world) {} -> {}", s.overlayMenu.empty() ? "-" : s.overlayMenu, ov.empty() ? "-" : ov);
                    s.overlayMenu = ov;
                }
                s.vatsOverlay = !s.overlayMenu.empty();
                // flat VATS shows VignetteMenu (dark screen edges); FO4VR has none, and over the live world it is a head-locked
                // dark frame ([Menus] bVatsNoVignette)
                // (also in the VATS window: VATSMenu view-taking, round 4)
                if (s.settings.menuVatsNoVignette && (s.overlayMenu == "VATSMenu" || (menu && std::strcmp(menu, "VATSMenu") == 0))) {
                    game::HideMenuIfOpen("VignetteMenu");
                }
            }
            if (const int camState = game::CurrentCameraState(); camState != s.lastCamState) {
                const bool wv = s.mono && camdir::WorldView(camState) && !menu;  // camera step 1: director policy
                REX::INFO("camera state {} -> {} ({}) policy {}", s.lastCamState, camState,
                    wv ? "world view" : s.mono ? "cinema fallback" : "cinema", camdir::PolicyName(camdir::ViewPolicy(camState)));
                s.lastCamState = camState;
            }
            if (std::string(menu ? menu : "") != s.lastMenu) {
                REX::INFO("view menu {} -> {}", s.lastMenu.empty() ? "-" : s.lastMenu, menu ? menu : "-");
                s.lastMenu = menu ? menu : "";
            }
            s.hudLayer = false;
            dlg::Tick();  // VR dialogue: UI / MenuTopicManager poll, weapon cull, haptic edge (main thread)
            camdir::PollWorkbenchBody();
            camdir::PollHeadHide();  // AAF POV: the head the view sits in is hidden (main thread)  // furniture gate: first-person body back after a workbench
            // [Camera] step 0: no ADS zoom while the head-tracked world view is shown; settings upkeep.
            game::camtweak::Tick(s.xrOk && s.mono && camdir::WorldView(s.lastCamState) && s.lastMenu.empty());
            if (!s.xrOk) {
                return;
            }
            if (KeyPressed(s.settings.toggleKey, s.toggleDown)) {
                s.submitting = !s.submitting;
                REX::INFO("cinema submission {}", s.submitting ? "on" : "off");
            }
            if (KeyPressed(s.settings.recenterKey, s.recenterDown)) {
                s.runtime.Recenter();
                head::Recenter();
                gestures::NoteRecentre();
                REX::INFO("recentred");
            }
            {
                // round 8 hotkeys (keyboard only, game window focused): cycle the camera state's view policy / first <-> third
                DWORD fgPid = 0;
                ::GetWindowThreadProcessId(::GetForegroundWindow(), &fgPid);
                const bool focused = fgPid == ::GetCurrentProcessId();
                const bool cyc = KeyPressed(s.settings.cameraCycleKey, s.cameraCycleDown) && focused;
                const bool pov = KeyPressed(s.settings.povToggleKey, s.povToggleDown) && focused;
                if (cyc || pov) {
                    const std::string r = camdir::Command(cyc ? "cycle" : "pov");
                    REX::INFO("hotkey {}: {}", cyc ? "camera cycle" : "pov toggle", r);
                    game::HudMessage("VR: " + r);
                }
            }

            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(s.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
                backBuffer = nullptr;
            }
            float aspect = 16.0f / 9.0f;
            if (backBuffer) {
                D3D11_TEXTURE2D_DESC d{};
                backBuffer->GetDesc(&d);
                if (d.Height) {
                    aspect = static_cast<float>(d.Width) / static_cast<float>(d.Height);
                }
            }

            // Only the first-person state is head-tracked. Any camera the game takes over (VATS,
            // dialogue, furniture, iron sights, third person, death cams) is shown on the cinema quad
            // instead of world-locked, so the user's head is never dragged around.
            // Camera step 1: the camera director's policy decides (TRACK / LOCK / FADE_SNAP = world view, CINEMA =
            // flat-screen fallback); without the director this is first person only, as before.
            const int  camState = s.lastCamState;
            const bool worldView = s.mono && camdir::WorldView(camState) && s.lastMenu.empty();
            const bool panelNow = worldView && !s.lastPanel.empty();
            // Free-camera scene view (camdir bFreeCamScene, AAF scenes): state 3 is now a world view, but every input path
            // (controller bindings context, comfort turn/direction, menu pointer, gun/FRIK hand feeds) keeps exactly what it
            // had there before, when state 3 was the flat screen: no controls change in the free camera.
            // 15 Nov: every SCENE state (third person, animated, ... the same method) keeps its flat-screen input mapping too.
            const bool inputWorld = worldView && !camdir::SceneView(camState);
            const float fadeAlpha = camdir::FadeAlpha();
            // round 5: VATS camera window ([Menus] bVatsCameraWindow, VatsWindow.h): VATS keeps the live world view (camera
            // gate) and the anchored window shows our own targeting camera with the VATS UI on top
            const bool vatsWin = worldView && s.overlayMenu == "VATSMenu" && vatswin::Enabled();
            vatswin::SetActive(vatsWin && s.runtime.running() && s.submitting);

            // Quad placement like FO4VR (menus hang on UprightHmdLagNode, which stops following the head while a menu
            // is open, PlayerCharacter::UpdateVRUI EF7180): when the flat fallback or a menu panel starts showing, the
            // quad is placed in front of where the user looks now (the head pose of the frame being ended) and stays
            // there. A different menu within the same episode moves it only if the head has turned more than
            // [Menus] fReanchorDeg away (the user is looking elsewhere).
            {
                const bool  quadShown = !worldView || panelNow || vatsWin;
                std::string key = !quadShown ? std::string{} : panelNow ? s.lastPanel : vatsWin ? std::string("VATSMenu") : !s.lastMenu.empty() ? s.lastMenu : std::format("camera {}", camState);
                float       headYaw = 0.0f;
                const bool  yawOk = s.pubHeadValid && YawXr(s.pubHead.orientation, headYaw);
                bool        anchor = false;
                const char* why = "";
                if (quadShown && !s.quadEpisode) {
                    anchor = true, why = "shown";
                } else if (quadShown && key != s.episodeKey && yawOk &&
                           std::abs(WrapRad(headYaw - s.anchorYaw)) > s.settings.menuReanchorDeg * 0.0174532925f) {
                    anchor = true, why = "new menu, head turned";
                } else if (quadShown && panelNow && s.settings.menuFollowWorkshop && s.lastPanel == "WorkshopMenu" && yawOk &&
                           std::abs(WrapRad(headYaw - s.anchorYaw)) > s.settings.menuReanchorDeg * 0.0174532925f) {
                    // workshop mode is long-lived and the user walks / looks around while building: the bar follows
                    anchor = true, why = "workshop, head turned";
                }
                const bool canAnchor = s.settings.menuAnchor && s.pubHeadValid && s.runtime.running();
                if (anchor && canAnchor) {
                    // round 6: crafting menus on FO4VR's workbench panel geometry (WSWorkbenchModel = world_projectedMenu.nif
                    // on UprightHmdLagNode, analysis/vr-workbenches.md); everything else on the [Cinema] geometry
                    // round 7: every panel menu on its own FO4VR model's geometry (Fo4vrPanelGeometry; [Menus] bPanelGeometryFO4VR)
                    float      gw = 0.0f, gd = 0.0f, gh = 0.0f;
                    const char* gsrc = "cinema";
                    const bool craft = panelNow && Fo4vrPanelGeometry(s, s.lastPanel, gw, gd, gh, gsrc);
                    if (craft) {
                        s.runtime.SetCinemaQuadGeometry(gw, gd, gh);
                    } else {
                        s.runtime.SetCinemaQuadGeometry(s.settings.quadWidthMeters, s.settings.quadDistanceMeters, 0.0f);
                        gsrc = "cinema";
                    }
                    s.workbenchGeometry = craft;
                    s.panelGeometry = gsrc;
                    s.runtime.AnchorCinemaQuad(s.pubHead);
                    s.anchorYaw = yawOk ? headYaw : 0.0f;
                    ++s.anchors;
                    xr::Pose qp;
                    float    qw = 0.0f, qh = 0.0f;
                    s.runtime.CinemaQuadPose(qp, qw, qh);
                    REX::INFO("menu quad anchored ({}: '{}'): head yaw {:.1f} deg pos {:.3f},{:.3f},{:.3f} -> quad centre {:.3f},{:.3f},{:.3f} {:.2f}x{:.2f} m geometry {}",
                        why, key, headYaw * 57.2957795f, s.pubHead.position.x, s.pubHead.position.y, s.pubHead.position.z, qp.position.x,
                        qp.position.y, qp.position.z, qw, qh, s.panelGeometry);
                }
                // no head pose yet (session starting): try again next frame
                s.quadEpisode = quadShown && (s.quadEpisode || canAnchor || !s.settings.menuAnchor);
                s.episodeKey = key;

                // round 6: FO4VR workshop mode shows the WorkshopMenu movie on the off-hand controller (WSWorkshopModel,
                // WS_InvWand.nif under the secondary wand node, ~0.40 m) - the cinema quad follows the off-hand grip each
                // frame (pointer hit tests follow, CinemaQuadPose). [Menus] bWorkshopOnHand.
                bool handQuad = false;
                if (quadShown && panelNow && s.settings.menuWorkshopOnHand && s.lastPanel == "WorkshopMenu") {
                    const auto& c = s.runtime.Controllers();
                    const bool  offRight = s.settings.leftHanded;  // off hand = the one that does not aim
                    const bool  gripOk = c.active && (offRight ? c.rightGripPoseValid : c.leftGripPoseValid);
                    if (gripOk) {
                        const xr::Pose& g = offRight ? c.rightGripPose : c.leftGripPose;
                        const float     half = s.settings.menuWorkshopHandPitchDeg * 0.5f * 0.0174532925f;
                        const xr::Quat  tilt{ std::sin(half), 0.0f, 0.0f, std::cos(half) };  // about the grip's +X
                        const xr::Quat& q = g.orientation;
                        const xr::Quat  o{ q.w * tilt.x + q.x * tilt.w + q.y * tilt.z - q.z * tilt.y,
                            q.w * tilt.y - q.x * tilt.z + q.y * tilt.w + q.z * tilt.x,
                            q.w * tilt.z + q.x * tilt.y - q.y * tilt.x + q.z * tilt.w,
                            q.w * tilt.w - q.x * tilt.x - q.y * tilt.y - q.z * tilt.z };
                        const xr::Vec3 off = RotXr(q, { 0.0f, s.settings.menuWorkshopHandUpMeters, s.settings.menuWorkshopHandBackMeters });
                        const xr::Pose hp{ o, { g.position.x + off.x, g.position.y + off.y, g.position.z + off.z } };
                        s.runtime.SetCinemaQuadOverride(&hp, s.settings.menuWorkshopHandWidthMeters);
                        handQuad = true;
                        ++s.workshopHandFrames;
                    }
                }
                if (!handQuad) {
                    s.runtime.SetCinemaQuadOverride(nullptr, 0.0f);
                }
                s.workshopOnHand = handQuad;
            }
            s.hudLayer = worldView && s.runtime.running() && s.submitting;  // next frame: HUD off the world image
            // C8 comfort vignette: opacity (fade advanced every Present) + shape; submitted in the world view only
            float vigAlpha = 0.0f, vigInner = 0.0f, vigOuter = 0.0f;
            const bool vigOn = comfort::Vignette(vigAlpha, vigInner, vigOuter);
            bool       vigSent = false;

            // 1. Finish the XR frame begun at the previous Present: the backbuffer now holds the image
            //    the game rendered from the pose/FOV published when that frame began.
            std::string eyeCapStem;  // stereo PoC E7 capture taken in this frame (see g_stereoEyeCapStem)
            if (s.frameOpen) {
                if (s.frameShouldRender && s.submitting && backBuffer) {
                    if (worldView && s.haveRenderPose) {
                        // Claim the FOV the game actually rendered with (the engine may clamp the
                        // requested worldFOV); a mismatch here shows as zoom and swimming.
                        xr::Fov used = s.renderFov;
                        float   l, r, t, b;
                        if (game::GetWorldFrustum(l, r, t, b)) {
                            used = { std::atan(l), std::atan(r), std::atan(t), std::atan(b) };
                            const float usedH = (used.angleRight - used.angleLeft) * 57.2957795f;
                            if (std::abs(usedH - s.loggedUsedFov) > 0.5f) {
                                // rate-limited: at most 8 lines per 5 s, then one summary line per 5 s with the change count
                                static std::uint64_t winStart = 0, changes = 0, logged = 0;
                                const std::uint64_t  now = ::GetTickCount64();
                                if (now - winStart > 5000) {
                                    if (changes > logged) {
                                        REX::WARN("game rendered FOV changed {} times in 5 s ({} logged) - last {:.1f} deg", changes, logged, usedH);
                                    }
                                    winStart = now, changes = 0, logged = 0;
                                }
                                ++changes;
                                if (logged < 8) {
                                    ++logged;
                                    REX::INFO("game rendered FOV {:.1f} x {:.1f} deg (requested {:.1f} x {:.1f})", usedH,
                                        (used.angleUp - used.angleDown) * 57.2957795f,
                                        (s.renderFov.angleRight - s.renderFov.angleLeft) * 57.2957795f,
                                        (s.renderFov.angleUp - s.renderFov.angleDown) * 57.2957795f);
                                }
                                s.loggedUsedFov = usedH;
                            }
                        }
                        stereo::EyeFrame u;
                        ID3D11Texture2D *eyeL = nullptr, *eyeR = nullptr;
                        const bool stereoNow = !s.afr && stereo::StereoSubmitActive();
                        if (stereoNow) {
                            std::scoped_lock reqLock(g_reqLock);
                            eyeCapStem.swap(g_stereoEyeCapStem);
                        }
                        if (!eyeCapStem.empty()) {
                            Capture(s, backBuffer, eyeCapStem + "_bb.bmp");
                            s.runtime.SetEyeCapture(true);
                        }
                        if (s.afr) {
                            s.runtime.SubmitEye(s.context, backBuffer, s.renderEye, s.renderPose, used);
                        } else if (stereo::StereoSubmitActive() && stereo::UsedEyes(u) &&
                                   stereo::SplitDoubleWide(s.context, backBuffer, eyeL, eyeR)) {
                            // Single-pass stereo (iMode 2/3): the back buffer is [left | right]; each half
                            // goes to its eye with the pose/FOV its constants were built from.
                            for (int e = 0; e < 2; ++e) {
                                const xr::Pose p{ { u.eyeQ[e][0], u.eyeQ[e][1], u.eyeQ[e][2], u.eyeQ[e][3] },
                                    { u.eyeP[e][0], u.eyeP[e][1], u.eyeP[e][2] } };
                                const xr::Fov f{ u.fov[e][0], u.fov[e][1], u.fov[e][2], u.fov[e][3] };
                                if (!s.runtime.SubmitEye(s.context, e ? eyeR : eyeL, e, p, f)) {
                                    REX::WARN("stereo: SubmitEye({}) failed: {}", e, s.runtime.lastError());
                                }
                            }
                            if (head::TraceArmed()) {  // htrace S: head the eyes were built from + submitted eye 0
                                head::TraceRec sr{ 'S' };
                                sr.seq = s.frameSeq;
                                sr.xrTime = s.frameXrTime;
                                for (int i = 0; i < 4; ++i) sr.v[i] = u.headQ[i], sr.v[7 + i] = u.eyeQ[0][i];
                                for (int i = 0; i < 3; ++i) sr.v[4 + i] = u.headP[i], sr.v[11 + i] = u.eyeP[0][i];
                                sr.v[14] = 1.0f;  // stereo
                                head::Trace(sr);
                            }
                            if (++g_stereoSubmits == 1 || g_stereoSubmits % 3000 == 0) {
                                REX::INFO("stereo: submitted double-wide halves as eyes ({} frames)", g_stereoSubmits);
                            }
                        } else {
                            s.runtime.SubmitMono(s.context, backBuffer, s.renderPose, used);
                            if (head::TraceArmed()) {  // htrace S (mono): the claimed render pose
                                const auto& o = s.renderPose.orientation;
                                const auto& p = s.renderPose.position;
                                head::TraceRec sr{ 'S' };
                                sr.seq = s.frameSeq;
                                sr.xrTime = s.frameXrTime;
                                sr.v[0] = o.x, sr.v[1] = o.y, sr.v[2] = o.z, sr.v[3] = o.w;
                                sr.v[4] = p.x, sr.v[5] = p.y, sr.v[6] = p.z;
                                head::Trace(sr);
                            }
                        }
                        // world panorama (round 4): this head-tracked frame into the cube, so a later flat window (VATS, ...)
                        // has the world all around it. Only steady tracked frames: TRACK policy, no fade, the heading not
                        // turning, from the 3rd frame on (the camera may still show the previous state's view), every 2nd frame.
                        if (s.settings.menuBackdropPanorama) {
                            float      yaw = 0.0f;
                            const auto r2w = PanoRefToWorld(yaw);
                            const bool steady = camdir::ViewPolicy(camState) == camdir::Policy::kTrack && fadeAlpha <= 0.0f &&
                                                std::abs(WrapRad(yaw - s.panoLastYaw)) < 0.002f;
                            s.panoLastYaw = yaw;
                            s.panoStable = steady ? s.panoStable + 1 : 0;
                            if (s.panoStable >= 3 && (++s.panoTick & 1) == 0) {
                                s.runtime.CapturePanorama(s.context, r2w);
                            }
                        }
                        if (vigOn) {  // C8 comfort vignette: head-locked, above the world, below the HUD
                            vigSent = s.runtime.SubmitVignette(s.context, vigAlpha, vigInner, vigOuter);
                        }
                        hud::Submit(s.runtime, s.context);  // HUD copied during this frame's render (D1)
                        pipboy::Submit(s.runtime, s.context);  // wrist Pip-Boy screen on the off-hand grip (D3)
                        dlg::Submit(s.runtime, s.context, s.pubHead, s.pubHeadValid);  // dialogue option panel (world quad 1)
                        if (panelNow) {
                            // Menu panel: the FlatScreenModel copy of this frame on the (anchored) cinema quad, alpha-blended
                            // over the live world; pointer + cursor dot as on the flat fallback.
                            bool premul = true;
                            if (auto* menuTex = static_cast<ID3D11Texture2D*>(hud::MenuPanelTexture(premul))) {
                                s.runtime.SubmitQuad(s.context, menuptr::PrepareCinema(s.context, menuTex), true, premul);
                            }
                        } else if (vatsWin) {
                            // VATS camera window: tone-mapped targeting view + VATSMenu copy on the anchored quad (opaque);
                            // until the first render only the menu (alpha) on the same quad
                            bool premul = true;
                            auto* menuTex = static_cast<ID3D11Texture2D*>(hud::MenuPanelTexture(premul));
                            if (auto* win = vatswin::Compose(s.context, menuTex, premul)) {
                                s.runtime.SubmitQuad(s.context, menuptr::PrepareCinema(s.context, win));
                                ++s.vatsOverlayFrames;
                            } else if (menuTex) {
                                s.runtime.SubmitQuad(s.context, menuptr::PrepareCinema(s.context, menuTex), true, premul);
                            }
                        } else if (s.vatsOverlay) {
                            // VATS overlay: the FlatScreenModel copy on a quad that spans the frustum this frame was rendered
                            // with, at the render pose (head-locked to that frame), 2 m ahead -> labels sit on their targets.
                            bool premul = true;
                            const auto& rp = s.renderPose;
                            if (auto* menuTex = static_cast<ID3D11Texture2D*>(hud::MenuPanelTexture(premul))) {
                                const float d = 2.0f;
                                const float tl = std::tan(used.angleLeft), tr = std::tan(used.angleRight);
                                const float tt = std::tan(used.angleUp), tb = std::tan(used.angleDown);
                                float cx = d * 0.5f * (tl + tr), cy = d * 0.5f * (tt + tb), w = d * (tr - tl);
                                // VATS labels are sized for a flat screen; spread over the ~140 deg render frustum they are a third
                                // of that angular size ("tiny, unreadable"). Magnify the overlay about the selected target's screen
                                // point: the labels around the target grow by k and stay on it ([Menus] fVatsLabelScale, 1 = off).
                                if (const float k = s.settings.menuVatsLabelScale; k > 1.001f && s.overlayMenu == "VATSMenu") {
                                    float pu = 0.5f, pv = 0.5f;
                                    if (!game::VatsTargetScreen(pu, pv) || pu < -0.2f || pu > 1.2f || pv < -0.2f || pv > 1.2f) {
                                        pu = pv = 0.5f;  // no target: about the view centre
                                    }
                                    const float px = d * (tl + pu * (tr - tl)), py = d * (tt - pv * (tt - tb));
                                    cx = px + k * (cx - px);
                                    cy = py + k * (cy - py);
                                    w *= k;
                                    s.vatsPivotU = pu, s.vatsPivotV = pv;
                                }
                                const xr::Vec3 off = RotXr(rp.orientation, { cx, cy, -d });
                                const xr::Pose qp{ rp.orientation, { rp.position.x + off.x, rp.position.y + off.y, rp.position.z + off.z } };
                                if (s.runtime.SubmitWorldQuad(s.context, menuTex, qp, w, true, premul, 2)) {
                                    ++s.vatsOverlayFrames;
                                }
                            }
                        }
                    } else if (!worldView) {
                        // Flat fallback. Behind it: the last world frame, re-shown world-locked and dimmed (the runtime
                        // reprojects it), so the surroundings are not black. Not for the main menu / loading screens.
                        const bool noWorld = s.lastMenu == "MainMenu" || s.lastMenu == "LoadingMenu";
                        s.panoStable = 0;
                        if (s.mono && s.settings.menuBackdrop && !noWorld) {
                            // round 4: the world panorama (everything looked at so far, world-locked all around, dim; never
                            // black); the frozen last frame only if the panorama is off or cannot be shown
                            float      yaw = 0.0f;
                            const auto r2w = PanoRefToWorld(yaw);
                            const float bright = 1.0f - std::clamp(s.settings.menuBackdropDim, 0.0f, 0.95f);
                            if (s.settings.menuBackdropPanorama &&
                                s.runtime.SubmitPanorama(s.context, r2w, bright, s.settings.menuPanoramaHoleDim)) {
                                if (++s.panoShown == 1) {
                                    REX::INFO("world panorama shown behind the flat window ({})", s.runtime.PanoramaReport());
                                }
                            } else {
                                if (s.settings.menuBackdropPanorama && ++s.panoFailed <= 5) {
                                    REX::WARN("world panorama not shown ({}; {}): frozen backdrop", s.runtime.PanoramaReport(), s.runtime.lastError());
                                }
                                s.runtime.SubmitBackdrop();
                                if (s.settings.menuBackdropDim > 0.0f) {
                                    s.runtime.SubmitVignette(s.context, (std::min)(s.settings.menuBackdropDim, 1.0f), 0.0f, 0.01f);
                                }
                            }
                        }
                        if (s.lastMenu == "LoadingMenu" || s.lastMenu == "MainMenu") {
                            s.runtime.ResetPanorama();  // a new place: what was captured belongs to the old one
                        }
                        s.runtime.SubmitQuad(s.context, menuptr::PrepareCinema(s.context, backBuffer));  // D2: + pointer cursor
                    }
                    // camera step 1: director fade (black quad over every layer); never over a view-taking menu
                    if (fadeAlpha > 0.0f && s.lastMenu.empty()) {
                        s.runtime.SubmitFade(s.context, fadeAlpha);
                    }
                }
                s.runtime.EndFrame();
                s.frameOpen = false;
                comfort::NoteFrame(vigSent, s.runtime.LastLayerCount(), s.runtime.VignetteUploads());
                if (!eyeCapStem.empty()) {
                    for (int e = 0; e < 2; ++e) {
                        if (ID3D11Texture2D* img = s.runtime.CapturedEyeImage(e)) {
                            Capture(s, img, std::format("{}_eye{}.bmp", eyeCapStem, e));
                            img->Release();
                        } else {
                            REX::WARN("stereoeyecap: no captured image for eye {}", e);
                        }
                    }
                    s.runtime.SetEyeCapture(false);
                }
                if (++s.xrFrames == 1 || s.xrFrames % 3000 == 0) {
                    REX::INFO("XR frames ended: {} (game presents: {})", s.xrFrames.load(), s.frames.load());
                }
                if (!s.runtime.lastError().empty() && s.xrFrames < 5) {
                    REX::WARN("XR: {}", s.runtime.lastError());
                }
            }

            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            if (g_pacing.last.QuadPart) {
                const double dt = QpcMs(g_pacing.last, now);
                g_pacing.sum += dt;
                g_pacing.max = (std::max)(g_pacing.max, dt);
                if (s.periodMs > 0.0 && dt > 1.5 * s.periodMs) {
                    ++g_pacing.slow;
                }
                if (++g_pacing.n == 600) {
                    REX::INFO("pacing: frame avg {:.2f} ms max {:.2f} ms; >1.5x period ({:.2f} ms): {}/600; wait avg {:.2f} max {:.2f} ms; {}",
                        g_pacing.sum / 600.0, g_pacing.max, s.periodMs, g_pacing.slow, g_pacing.waitSum / 600.0, g_pacing.waitMax,
                        head::PoseSyncStats(true));
                    g_pacing = Pacing{};
                }
            }
            g_pacing.last = now;

            s.runtime.PollEvents();
            if (const auto st = s.runtime.state(); st != s.lastState) {
                REX::INFO("XR session state {} -> {}", static_cast<int>(s.lastState), static_cast<int>(st));
                s.lastState = st;
            }

            // 2. Begin the next XR frame now, before the game updates/renders it: xrWaitFrame paces the
            //    game to the headset, and the predicted pose is for the display time of the frame the
            //    game is about to render.
            xr::FrameInfo fi;
            LARGE_INTEGER w0{}, w1{};
            ::QueryPerformanceCounter(&w0);
            const bool began = s.runtime.BeginFrame(fi);
            ::QueryPerformanceCounter(&w1);
            {
                const double w = QpcMs(w0, w1);
                g_pacing.waitSum += w;
                g_pacing.waitMax = (std::max)(g_pacing.waitMax, w);
            }
            if (began && head::TraceArmed()) {  // htrace L: xrWaitFrame/BeginFrame/xrLocateViews returned (eye poses)
                head::TraceRec r{ 'L' };
                r.seq = head::Seq();
                r.xrTime = fi.predictedDisplayTime;
                for (int e = 0; e < 2; ++e) {
                    const auto& v = fi.eyes[e].pose;
                    r.v[e * 7 + 0] = v.orientation.x, r.v[e * 7 + 1] = v.orientation.y, r.v[e * 7 + 2] = v.orientation.z;
                    r.v[e * 7 + 3] = v.orientation.w, r.v[e * 7 + 4] = v.position.x, r.v[e * 7 + 5] = v.position.y;
                    r.v[e * 7 + 6] = v.position.z;
                }
                r.v[14] = static_cast<float>(QpcMs(w0, w1));
                head::Trace(r);
            }
            // Wrist Pip-Boy: a direct open needs the XR world view; quad pose + pointer from this frame's poses.
            game::SetPipboyWorldViewAvailable(s.xrOk && s.mono && s.runtime.running() && s.submitting);
            const bool pipPointer = pipboy::UpdateFrame(s.runtime, began, began && fi.headValid ? &fi.head : nullptr);  // head: projected PA Pip-Boy
            // Menu laser pointer (D2): aim ray onto the cinema quad, which shows the next frame when not in world view.
            // Menu panels use the same (anchored) cinema quad.
            // round 7: terminal on its monitor in the world - the aim ray in the camera-local frame of the camera root on screen
            // (relative to s.pubHead, as PublishHandRay) for the engine pick on the terminal's Screen:0
            const bool worldScreenNow = worldView && !panelNow && !vatsWin && !s.worldScreen.empty() && s.lastMenu.empty();
            {
                const auto& c = s.runtime.Controllers();
                const bool  left = s.settings.leftHanded;
                const bool  ok = worldScreenNow && began && c.active && (left ? c.leftAimValid : c.rightAimValid) && s.pubHeadValid;
                float       lp[3]{}, ld[3]{};
                if (ok) {
                    const xr::Pose& hand = left ? c.leftAim : c.rightAim;
                    const auto&     hq = s.pubHead.orientation;
                    const xr::Quat  inv{ -hq.x, -hq.y, -hq.z, hq.w };
                    const xr::Vec3  p = RotXr(inv, { hand.position.x - s.pubHead.position.x, hand.position.y - s.pubHead.position.y,
                                                        hand.position.z - s.pubHead.position.z });
                    const xr::Vec3  d = RotXr(inv, RotXr(hand.orientation, { 0.0f, 0.0f, -1.0f }));
                    lp[0] = p.x, lp[1] = -p.z, lp[2] = p.y;  // OpenXR -> game camera-local (X right, Y forward, Z up)
                    ld[0] = d.x, ld[1] = -d.z, ld[2] = d.y;
                    ++s.worldScreenFrames;
                }
                // the pointer targets TerminalMenu (a holotape game on the terminal takes the pad only)
                menuptr::SetWorldScreen(ok && s.worldScreen == "TerminalMenu", "TerminalMenu", lp, ld, s.settings.unitsPerMeter);
            }
            const bool menuPointer = menuptr::UpdateFrame(s.runtime, began, (!inputWorld || panelNow || vatsWin) && s.submitting && s.runtime.running());
            dlg::UpdateXr(s.runtime, began);  // dialogue: haptic pulse when the options appear
            if (began) {
                haptics::Drain(s.runtime);  // gesture pulses + FRIK-flat's Vibrate (Haptics.h)
            }
            {
                // VR controllers -> the game's gamepad (Fallout 4 Xbox layout). Inactive (session not
                // focused, e.g. Quest system menu) maps to a neutral pad.
                const auto& c = s.runtime.Controllers();
                pad::State  p;
                p.active = began && c.active;
                if (p.active) {
                    // FO4VR default bindings per context (ControllerBindings.h; [Controllers] bFO4VRBindings=0 = old Xbox layout)
                    // a menu panel is a menu context (pad navigation, A accept, B back), not gameplay
                    // round 7: a terminal on its monitor is a menu context too (stick/A/B navigate the terminal)
                    // gestures (Gestures.h) run inside Map: they need this frame's head pose and the camera state
                    bindings::Map(c, { inputWorld && !panelNow && !worldScreenNow, game::PipboyWristOpen(), pipPointer, menuPointer, menuptr::LeftHand(),
                                         s.settings.leftHanded, began && fi.headValid ? &fi.head : nullptr, s.lastCamState },
                        p);
                    if (gestures::TakeRecentre() && s.xrOk) {
                        s.runtime.Recenter();
                        head::Recenter();
                        gestures::NoteRecentre();
                        REX::INFO("recentred (gesture)");
                    }
                    // C8 comfort: snap/smooth turn (rx -> rs::UserTurn, rx zeroed while owned), stick direction
                    // (head / off-hand aim / body), vignette target. Off hand = left unless bLeftHanded.
                    // VR dialogue: primary stick direction -> option face button (stick zeroed while DialogueMenu is open)
                    dlg::FilterPad(p, s.settings.leftHanded ? c.leftStickX : c.rightStickX, s.settings.leftHanded ? c.leftStickY : c.rightStickY);
                    comfort::PadFrame cf;
                    cf.lx = p.lx;
                    cf.ly = p.ly;
                    cf.rx = p.rx;
                    // (workshop mode is walked around in: turning / stick direction stay on with its panel)
                    cf.worldView = inputWorld && (!panelNow || s.lastPanel == "WorkshopMenu") && !s.vatsOverlay && !game::PipboyWristOpen() && !worldScreenNow;
                    const bool      offLeft = !s.settings.leftHanded;
                    const xr::Pose& off = offLeft ? c.leftAim : c.rightAim;
                    float           handYaw = 0.0f, headYaw = 0.0f;
                    cf.handYawValid = (offLeft ? c.leftAimValid : c.rightAimValid) && s.pubHeadValid &&
                                      YawXr(off.orientation, handYaw) && YawXr(s.pubHead.orientation, headYaw);
                    if (cf.handYawValid) {
                        cf.handYawRel = handYaw - headYaw;
                    }
                    comfort::FilterPad(cf);
                    p.lx = cf.lx;
                    p.ly = cf.ly;
                    p.rx = cf.rx;
                }
                pad::Publish(p);
            }
            if (began) {
                s.periodMs = static_cast<double>(fi.predictedDisplayPeriod) / 1.0e6;
                s.frameOpen = true;
                s.frameShouldRender = fi.shouldRender;
                // AFR: the next game frame renders one eye, alternating. Each frame records which
                // eye it is for, so a dropped frame can never swap the eyes.
                const bool  afrNow = worldView && s.afr && fi.viewsValid;
                const int   eye = s.afrNextEye;
                const auto& renderFrom = afrNow ? fi.eyes[eye].pose : fi.head;
                const auto& q = renderFrom.orientation;
                const auto& hp = renderFrom.position;
                // floating gun (FloatingGun.h): grip/aim relative to the new head (next game frame's camera) and to
                // s.pubHead (the hand ray's frame); before PublishHandRay, which takes P/D from the muzzle.
                gun::PublishXr(s.runtime.Controllers(), renderFrom, afrNow ? true : fi.headValid, s.pubHead, s.pubHeadValid, inputWorld,
                    s.settings.leftHanded);
                // FRIK-flat bridge (FrikBridge.h): its own controller/head feed, independent of the gun
                frikbridge::PublishXr(s.runtime.Controllers(), renderFrom, afrNow ? true : fi.headValid, inputWorld, s.settings.leftHanded);
                if (s.settings.aimWithController) {
                    PublishHandRay(s, true);
                }
                head::PublishXr(q.x, q.y, q.z, q.w, hp.x, hp.y, hp.z, afrNow ? true : fi.headValid);
                s.frameSeq = head::Seq();
                s.frameXrTime = fi.predictedDisplayTime;
                if (head::TraceArmed()) {  // htrace P: published head (OpenXR space) for the frame about to render
                    head::TraceRec r{ 'P' };
                    r.seq = s.frameSeq;
                    r.xrTime = fi.predictedDisplayTime;
                    r.v[0] = q.x, r.v[1] = q.y, r.v[2] = q.z, r.v[3] = q.w;
                    r.v[4] = hp.x, r.v[5] = hp.y, r.v[6] = hp.z;
                    r.v[7] = worldView ? 1.0f : 0.0f;
                    r.v[8] = static_cast<float>(QpcMs(w0, w1));  // ms blocked in xrWaitFrame + xrBeginFrame
                    r.v[9] = static_cast<float>(fi.predictedDisplayPeriod / 1.0e6);
                    r.v[10] = fi.headValid ? 1.0f : 0.0f;
                    r.v[11] = fi.viewsValid ? 1.0f : 0.0f;
                    r.v[12] = static_cast<float>(w0.QuadPart % 100000000);  // qpc before xrWaitFrame (mod 1e8)
                    head::Trace(r);
                }
                {
                    // Single-pass stereo: head + both eyes of this xrLocateViews for the frame about to render.
                    stereo::EyeFrame ef;
                    ef.valid = worldView && !afrNow && fi.viewsValid && fi.headValid;
                    if (ef.valid) {
                        const auto& h = fi.head;
                        ef.headQ[0] = h.orientation.x, ef.headQ[1] = h.orientation.y, ef.headQ[2] = h.orientation.z, ef.headQ[3] = h.orientation.w;
                        ef.headP[0] = h.position.x, ef.headP[1] = h.position.y, ef.headP[2] = h.position.z;
                        for (int e = 0; e < 2; ++e) {
                            const auto& v = fi.eyes[e];
                            ef.eyeQ[e][0] = v.pose.orientation.x, ef.eyeQ[e][1] = v.pose.orientation.y;
                            ef.eyeQ[e][2] = v.pose.orientation.z, ef.eyeQ[e][3] = v.pose.orientation.w;
                            ef.eyeP[e][0] = v.pose.position.x, ef.eyeP[e][1] = v.pose.position.y, ef.eyeP[e][2] = v.pose.position.z;
                            ef.fov[e][0] = v.fov.angleLeft, ef.fov[e][1] = v.fov.angleRight;
                            ef.fov[e][2] = v.fov.angleUp, ef.fov[e][3] = v.fov.angleDown;
                        }
                    }
                    stereo::PublishEyes(ef);
                }
                s.pubHead = renderFrom;
                s.pubHeadValid = afrNow ? true : fi.headValid;
                if (afrNow) {
                    s.renderEye = eye;
                    s.afrNextEye = 1 - eye;
                }
                if (worldView && (afrNow || fi.headValid)) {
                    s.renderPose = renderFrom;
                    s.renderFov = afrNow ? s.runtime.EyeRenderFov(eye, aspect) : s.runtime.MonoRenderFov(aspect);
                    // Pixel density at the view centre is (width/2)/tan(hfov/2): a wide symmetric view
                    // spends most pixels on the periphery. Optional cap trades periphery for sharpness.
                    if (const float capDeg = s.settings.maxRenderFov;
                        capDeg > 1.0f && 2.0f * s.renderFov.angleRight * 57.2957795f > capDeg) {
                        const float h = capDeg * 0.5f / 57.2957795f;
                        const float v = std::atan(std::tan(h) / aspect);
                        s.renderFov = { -h, h, v, -v };
                    }
                    s.haveRenderPose = true;
                    float hDeg = 2.0f * s.renderFov.angleRight * 57.2957795f;
                    if (const float st = stereo::TestEyesRenderHFovDeg(aspect); st > hDeg) {
                        hDeg = st;  // stereo test eyes: the culling frustum must contain both synthetic eyes
                    }
                    head::SetTargetFov(hDeg);
                    if (std::abs(hDeg - s.loggedFov) > 0.5f) {
                        REX::INFO("mono render FOV {:.1f} x {:.1f} deg (aspect {:.3f})", hDeg,
                            2.0f * s.renderFov.angleUp * 57.2957795f, aspect);
                        s.loggedFov = hDeg;
                    }
                }
            } else if (s.settings.aimWithController) {
                PublishHandRay(s, false);
            }
            if (!worldView) {
                // Not a tracked world view: drop the render pose and give the game its own FOV back.
                s.haveRenderPose = false;
                head::SetTargetFov(0.0f);
            }
            head::ClosePublishWindow();  // pose, eyes, hand ray, gun data and target FOV for the next game frame are out
            // next frame's H2: leave FlatScreenModel out of the world image while a menu panel is up
            const bool vatsNow = worldView && s.vatsOverlay;
            hud::SetMenuPanelNext((panelNow || vatsNow) && s.submitting && s.runtime.running());
            // a game-pausing panel menu (PauseMenu) stops the camera update: drive it for the next frame (head published above)
            // (first person only: the stall test counts FirstPersonState updates, which no other camera state makes)
            // (round 7: also a terminal on its monitor - TerminalMenu pauses the game, kPausesGame in flat and FO4VR)
            // round 8: any camera state (camera takeover menus as panels over the SCENE / third-person / furniture camera): the
            // stall test counts the director's ticks there (FirstPersonState updates only happen in first person)
            game::DriveCameraWhilePaused((panelNow || vatsNow || worldScreenNow) && s.runtime.running() && s.lastCamState >= 0,
                s.lastCamState == 0 ? &game::FirstPersonUpdates : &camdir::Ticks);
            // no screen-fixed blurred menu background behind a panel / VATS over the live world (it would move with the head)
            game::HoldMenuBlur(s.settings.menuNoBlur && (panelNow || vatsNow) && s.runtime.running());
            game::ApplyTargetFov();
            if (backBuffer) {
                backBuffer->Release();
            }
        }

        void UpdateStatus(State& s)
        {
            std::string line = std::format("presents={} xrInit={} xrOk={} state={} xrFrames={} submitting={} tracking={} override={} targetFov={:.1f} mode={} fov={:.1f}x{:.1f} runtime='{}' err='{}' fadeLayers={}",
                s.frames.load(), s.xrTried, s.xrOk, static_cast<int>(s.runtime.state()), s.xrFrames.load(), s.submitting,
                head::Tracking() ? 1 : 0, head::OverrideActive() ? 1 : 0, head::TargetFov(),
                s.afr ? "afr" : s.mono ? "mono" : "cinema", s.renderFov.angleRight * 2 * 57.2958f, s.renderFov.angleUp * 2 * 57.2958f,
                s.xrOk ? s.runtime.runtimeName() : std::string{}, s.runtime.lastError(), s.runtime.FadeLayersSubmitted()) +
                std::format(" viewMenu='{}' menuPanel='{}' quadAnchors={} backdropFrames={} layers={} overlay='{}' overlayFrames={} stereoSubmits={}",
                    s.lastMenu, s.lastPanel, s.anchors, s.runtime.BackdropFrames(), s.runtime.LastLayerCount(), s.overlayMenu,
                    s.vatsOverlayFrames, g_stereoSubmits) +
                std::format(" vatsPivot={:.3f},{:.3f}", s.vatsPivotU, s.vatsPivotV) +
                [&] {
                    xr::Pose qp;
                    float    qw = 0.0f, qh = 0.0f;
                    if (s.xrOk) {
                        s.runtime.CinemaQuadPose(qp, qw, qh);
                    }
                    return std::format(" wbGeom={} wsHand={} wsHandFrames={} quadPos={:.3f},{:.3f},{:.3f} quadSize={:.2f}x{:.2f} panelGeom='{}' worldScreen='{}' worldScreenFrames={}",
                        s.workbenchGeometry ? 1 : 0, s.workshopOnHand ? 1 : 0, s.workshopHandFrames, qp.position.x, qp.position.y,
                        qp.position.z, qw, qh, s.panelGeometry, s.worldScreen, s.worldScreenFrames);
                }() +
                std::format(" panoShown={} panoFailed={} {}", s.panoShown, s.panoFailed, s.xrOk ? s.runtime.PanoramaReport() : std::string{});
            std::scoped_lock l(g_reqLock);
            g_status = std::move(line);
        }

        HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* a_this, UINT a_sync, UINT a_flags)
        {
            auto* s = g_state;  // non-null: set before the vtable is patched
            if (a_this == s->swapChain && !(a_flags & DXGI_PRESENT_TEST)) {
                ++s->frames;
                // pose sync: head consumers on the game's job threads (the player/camera update runs concurrently with
                // this hook) wait until this frame's pose is published below (HeadPose.h WaitForPublish)
                if (s->xrOk && s->runtime.running()) {
                    head::OpenPublishWindow();
                }
                head::TraceSetFrame(s->frames.load());
                game::TraceWorldCamera();  // htrace C: the camera this frame was rendered with (if nothing updated it since)
                stereo::OnPresent();  // stereo PoC: stats, stereocap write-out, latch the stereo-frame flag
                std::string capturePath;
                bool        recenter = false;
                {
                    std::scoped_lock l(g_reqLock);
                    capturePath.swap(g_capturePath);
                    if (g_burstWant > 0 && g_burstLeft == 0) {
                        g_burstActiveStem.swap(g_burstStem);
                        g_burstLeft = std::exchange(g_burstWant, 0);
                        g_burstTex.clear();
                    }
                    recenter = std::exchange(g_recenterRequested, false);
                    if (g_cropReq.total > 0 && g_crop.left == 0 && g_crop.ring.empty()) {
                        g_crop = g_cropReq;
                        g_cropReq = CropBurst{};
                    }
                    if (const int m = std::exchange(g_modeRequest, -1); m >= 0) {
                        s->mono = m >= 1;
                        s->afr = m == 2;
                        REX::INFO("mode -> {}", s->afr ? "afr" : s->mono ? "mono" : "cinema");
                    }
                }
                if (g_burstLeft > 0) {
                    ID3D11Texture2D* bb = nullptr;
                    if (SUCCEEDED(a_this->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
                        BurstFrame(*s, bb);
                        bb->Release();
                    }
                }
                if (g_crop.left > 0) {
                    ID3D11Texture2D* bb = nullptr;
                    if (SUCCEEDED(a_this->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
                        CropFrame(*s, bb);
                        bb->Release();
                    }
                }
                if (!capturePath.empty()) {
                    ID3D11Texture2D* bb = nullptr;
                    if (SUCCEEDED(a_this->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
                        Capture(*s, bb, capturePath);
                        bb->Release();
                    }
                }
                if (recenter && s->xrOk) {
                    s->runtime.Recenter();
                    head::Recenter();
                    gestures::NoteRecentre();
                    REX::INFO("recentred (command)");
                }
                VrFrame(*s);
                head::ClosePublishWindow();  // every path (no XR frame begun, early returns)
                hud::OnPresent(s->hudLayer);
                dlg::OnPresent(s->hudLayer);  // same gate as the HUD layer: world view, session running, submitting
                pipboy::OnPresent();
                game::AimTick();
                gun::OnPresent();  // floating gun: P vs fire node at Present
                if (s->frames % 30 == 1) {
                    UpdateStatus(*s);
                }
            }
            // While the headset paces the game (xrWaitFrame), monitor vsync only adds a second clock
            // that beats against it; present without waiting for the monitor.
            if (a_this == s->swapChain && s->xrOk && s->runtime.running()) {
                a_sync = 0;
            }
            const HRESULT hr = s->original(a_this, a_sync, a_flags);
            if (a_this == s->swapChain && !(a_flags & DXGI_PRESENT_TEST)) {
                stereo::AfterPresent();
            }
            return hr;
        }
    }

    bool Install(void* a_swapChain, void* a_device, void* a_context, const Settings& a_settings)
    {
        bool ok = false;
        std::call_once(g_installOnce, [&] {
            auto* s = new State{};  // lives for the process
            s->swapChain = static_cast<IDXGISwapChain*>(a_swapChain);
            s->device = static_cast<ID3D11Device*>(a_device);
            s->context = static_cast<ID3D11DeviceContext*>(a_context);
            s->settings = a_settings;

            auto** vtbl = *reinterpret_cast<void***>(s->swapChain);
            s->original = reinterpret_cast<Present_t>(vtbl[kPresentSlot]);
            g_state = s;

            DWORD oldProtect = 0;
            if (!::VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                REX::ERROR("VirtualProtect on swapchain vtable failed ({})", ::GetLastError());
                return;
            }
            vtbl[kPresentSlot] = reinterpret_cast<void*>(&HkPresent);
            ::VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), oldProtect, &oldProtect);
            ::FlushInstructionCache(::GetCurrentProcess(), &vtbl[kPresentSlot], sizeof(void*));
            REX::INFO("Present hooked: swapchain {} vtable {} original {}", a_swapChain, static_cast<void*>(vtbl),
                reinterpret_cast<void*>(s->original));
            ok = true;
        });
        return ok;
    }

    std::string Counters()
    {
        auto* s = g_state;
        if (!s) {
            return "presents=0 xrFrames=0";
        }
        LARGE_INTEGER c{}, f{};
        ::QueryPerformanceCounter(&c);
        ::QueryPerformanceFrequency(&f);
        return std::format("presents={} xrFrames={} t={:.6f}", s->frames.load(), s->xrFrames.load(),
            static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart));
    }

    std::string Status()
    {
        std::scoped_lock l(g_reqLock);
        return g_status;
    }

    void RequestStereoEyeCapture(std::string a_stem)
    {
        std::scoped_lock l(g_reqLock);
        g_stereoEyeCapStem = std::move(a_stem);
    }

    void RequestCapture(std::string a_path)
    {
        std::scoped_lock l(g_reqLock);
        g_capturePath = std::move(a_path);
    }

    void RequestCaptureBurst(int a_frames, std::string a_stem)
    {
        std::scoped_lock l(g_reqLock);
        g_burstWant = (std::max)(1, (std::min)(a_frames, 32));
        g_burstStem = std::move(a_stem);
    }

    void RequestCropBurst(int a_frames, std::string a_path, unsigned a_x0, unsigned a_x1, unsigned a_y, unsigned a_w, unsigned a_h,
        unsigned a_step)
    {
        std::scoped_lock l(g_reqLock);
        CropBurst c;
        c.total = c.left = (std::max)(1, (std::min)(a_frames, 5000));
        c.path = std::move(a_path);
        c.x0 = a_x0, c.x1 = a_x1, c.y = a_y, c.w = (std::max)(8u, a_w), c.h = (std::max)(8u, a_h), c.step = (std::max)(1u, a_step);
        g_cropReq = std::move(c);
    }

    void RequestMode(int a_mode)
    {
        std::scoped_lock l(g_reqLock);
        g_modeRequest = a_mode;
    }

    void RequestRecenter()
    {
        std::scoped_lock l(g_reqLock);
        g_recenterRequested = true;
    }
}
