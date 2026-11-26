#pragma once
// fo4flatvr panel host: other F4SE plugins put their own Dear ImGui (or any D3D11-drawn) panels into the VR view, handled
// exactly like fo4flatvr's own VR settings panel (ConfigUI.h): world-locked quads anchored where you look when they open,
// or quads carried on a controller (a wrist dock), pointed at with either controller's aim ray (trigger = click, stick
// up/down = scroll, stick left/right = step, B / Y = close), that hand's trigger / stick / face buttons kept from the game
// while its ray is on a panel, the shared pointer beam and haptic ticks. 26 Nov 2026 (Vioxsis Anatomy Bridge live menu).
//
// Plain C ABI; this header is included by fo4flatvr and by client plugins (by relative path). A client calls, after every
// plugin is loaded (F4SE kPostLoad):
//     auto get = (FO4FlatVR_GetPanelHost_t)GetProcAddress(GetModuleHandleW(L"fo4flatvr.dll"), "FO4FLATVR_GetPanelHost");
//     const FO4FlatVR_PanelHost* host = get ? get(FO4FLATVR_PANEL_HOST_VERSION) : nullptr;
// A null result: no fo4flatvr, or an older one.
//
// Drawing: the host calls a panel's Draw on its Presenter thread (the render thread, inside Present) once per frame while
// the panel is open and an OpenXR session renders. Draw renders into a texture the client created on in->device (any
// RGBA8 render-target texture of the panel's texW x texH, straight alpha premultiplied = 0, or premultiplied as declared)
// and returns it in out->texture. Draw must not block: game state is read from a snapshot the client keeps, game work is
// queued to the game's main thread. The host never calls Draw re-entrantly; the API functions may be called from Draw
// (they only queue requests, applied on the next frame) and from any other thread.
#include <stdint.h>

#define FO4FLATVR_PANEL_HOST_VERSION 1u

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

extern "C" {

enum FO4FlatVR_PanelPlacement : uint32_t
{
    FO4FLATVR_PANEL_WORLD = 0,       // world-locked; anchored ahead of the head when it opens (pos = {0, height, -distance})
    FO4FLATVR_PANEL_LEFT_HAND = 1,   // carried on the left controller's grip pose (pos / ypr = offset in grip space)
    FO4FLATVR_PANEL_RIGHT_HAND = 2,  // carried on the right controller's grip pose
};

enum FO4FlatVR_PanelFlags : uint32_t
{
    FO4FLATVR_PANEL_SHOW_WHEN_FACING = 1u,  // hand panels: shown only while the panel faces the head (watch-style)
    FO4FLATVR_PANEL_GESTURE_TOGGLE = 2u,    // the gesture / controller action "Mod panel" toggles this panel
    FO4FLATVR_PANEL_B_CLOSES = 4u,          // B / Y while pointing at it closes it (the settings panel's rule)
    FO4FLATVR_PANEL_PREMULTIPLIED = 8u,     // the texture is premultiplied alpha (else straight alpha)
};

struct FO4FlatVR_PanelInput
{
    uint8_t hover;        // a ray is on the panel this frame (x / y valid)
    uint8_t down;         // the pointing hand's trigger is held (click), after the host's arming / hysteresis
    uint8_t closeButton;  // B / Y pressed this frame on the pointing hand (edge)
    uint8_t pad0;
    float   x, y;         // texture pixels
    float   wheel;        // stick up/down this frame, in ImGui wheel units
    int32_t step;         // stick left/right: -1 / +1 once per repeat step (450 ms first, then 150 ms), else 0
    int32_t hand;         // pointing hand 0 left, 1 right, -1 none
};

struct FO4FlatVR_PanelFrame
{
    uint32_t              size;      // sizeof(FO4FlatVR_PanelFrame) as the host filled it
    ID3D11Device*         device;
    ID3D11DeviceContext*  context;
    float                 dt;        // seconds since this panel's last Draw
    uint8_t               visible;   // always 1: a hand panel turned away from the head is neither drawn nor submitted
    uint8_t               pad[3];
    FO4FlatVR_PanelInput  input;
};

struct FO4FlatVR_PanelOutput
{
    ID3D11Texture2D* texture;       // what to show (null = nothing this frame)
    uint8_t          requestClose;  // the panel closes after this frame
    uint8_t          pad[7];
};

typedef void (*FO4FlatVR_PanelDraw_t)(void* user, const FO4FlatVR_PanelFrame* in, FO4FlatVR_PanelOutput* out);
typedef void (*FO4FlatVR_PanelEvent_t)(void* user, int32_t panel, uint32_t event);  // 1 opened, 2 closed

struct FO4FlatVR_PanelDesc
{
    uint32_t               size;       // sizeof(FO4FlatVR_PanelDesc)
    const char*            id;         // unique, e.g. "vioxsis.main" (copied)
    const char*            title;      // for logs / the host's status (copied)
    uint32_t               texW, texH;
    float                  widthM;     // quad width in metres (height from texW / texH)
    uint32_t               placement;  // FO4FlatVR_PanelPlacement
    float                  pos[3];     // see FO4FlatVR_PanelPlacement (metres; OpenXR axes: +X right, +Y up, -Z forward)
    float                  ypr[3];     // hand panels: yaw, pitch, roll in degrees, applied in grip space
    uint32_t               flags;      // FO4FlatVR_PanelFlags
    FO4FlatVR_PanelDraw_t  draw;
    FO4FlatVR_PanelEvent_t onEvent;    // may be null
    void*                  user;
};

struct FO4FlatVR_PanelHost
{
    uint32_t version;   // FO4FLATVR_PANEL_HOST_VERSION the host serves
    uint32_t size;      // sizeof the table
    int32_t (*Register)(const FO4FlatVR_PanelDesc* desc);   // panel handle >= 0, -1 = refused (log says why)
    void (*SetOpen)(int32_t panel, uint8_t open);
    void (*Toggle)(int32_t panel);
    uint8_t (*IsOpen)(int32_t panel);
    void (*Reanchor)(int32_t panel);                          // world panels: ahead of the head again on the next frame
    // placement / pos / ypr / widthM / flags from a desc (only those fields are read; id, draw, user are ignored)
    void (*SetPlacement)(int32_t panel, const FO4FlatVR_PanelDesc* desc);
    // world panels: open just under another (open) world panel, facing the same way, a_gapM below its lower edge
    void (*PlaceBelow)(int32_t panel, int32_t other, float gapM);
    uint8_t (*XrRunning)();                                   // an OpenXR session renders (panels can be seen)
    void (*Haptic)(int32_t hand, float amplitude, float seconds);
};

// exported by fo4flatvr.dll as "FO4FLATVR_GetPanelHost"; null for version 0 or a newer version than the host serves
typedef const FO4FlatVR_PanelHost* (*FO4FlatVR_GetPanelHost_t)(uint32_t version);
}
