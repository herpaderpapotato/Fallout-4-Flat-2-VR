#pragma once
// VATS camera window (UI round 5): the VATS targeting view rendered by our own second camera into an offscreen
// target and shown, with the VATS UI on top, on the anchored VATS window quad, while the head-tracked (stereo) world
// keeps rendering live around it. Recipe: analysis/second-camera-render.md (approach B; milestone C = target only).
//
//  R1  pre-hook of the DrawWorld::End call C383F6 in Main::DrawWorld_And_UI (E8 -> 21FA3B0, byte-checked): our
//      NiCamera + BSShaderAccumulator + BSCullingProcess, BSShaderUtil::RenderSceneDeferred 21C3ED0 into our own
//      scene-size render target (created once with RenderTargetManager::CreateRenderTarget in a free id).
//  L1/L2 the two Main::WorldRootCamera calls in VatsMenuObj::UpdateState (B1E319 off-screen test, B1E9C1 label
//      placement; E8 -> C37DD0, byte-checked) return our camera while the window is up, so the VATS labels are
//      projected for the window view and line up with the target in it.
//  Present: tone map of the HDR result + the VATSMenu FlatScreenModel copy on top -> the window texture.
//
// [Menus] bVatsCameraWindow (needs the VATS camera gate: it turns it on like bVatsLiveWorld), iVatsWindowEvery,
// iVatsWindowScene (0 target only, 1 full scene), fVatsWindowFovDeg (0 auto), fVatsWindowExposure,
// iVatsWindowWidth, bVatsWindowMask. Pipe: `vatswin [status|on|off|every n|scene 0|1|fov d|exposure x|mask 0|1]`.

#include <string>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace fo4flatvr::vatswin
{
    // Reads the ini keys; with bVatsCameraWindow=1 byte-checks and installs R1/L1/L2. Returns a summary line.
    std::string Install();
    // Read by CameraDirector before its own install: the window needs the VATS camera gate.
    bool ConfiguredOn();
    // Switch on and hooks in place.
    bool Enabled();
    // Presenter (Present thread): the VATS window is the shown view for the next game frame.
    void SetActive(bool a_on);
    // Presenter (Present thread): the window image for this frame (tone-mapped camera view + a_menu, premultiplied
    // or straight alpha), or nullptr (nothing rendered yet / failure: the caller falls back).
    ID3D11Texture2D* Compose(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_menu, bool a_premultiplied);
    std::string Status();
    // Diagnostics (round 5 pitch hunt): camera pitch seen at a site (1 = after TESCamera::Update / director tick,
    // 2 = at the render hook R1, world camera). Cheap; counts frames with |pitch| > 0.5 deg.
    void NoteCameraPitch(int a_site);
    std::string Command(const std::string& a_arg);
}
