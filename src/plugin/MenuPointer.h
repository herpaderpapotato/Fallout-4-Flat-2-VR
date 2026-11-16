#pragma once
// Menu laser pointer (feature D2), D3D/XR side: casts the aim-hand ray onto the cinema quad (menus and every
// other flat-screen fallback), turns trigger / thumbstick into pointer state for the game side
// (GameMenuPointer.*: Scaleform MouseEvents into the top-most cursor menu's movie) and draws a cursor dot into
// the image the cinema quad shows. No RE types here.

#include <string>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace xr
{
    class Runtime;
}

namespace fo4flatvr::menuptr
{
    struct Config
    {
        bool enabled{ true };      // [Menus] bPointer
        bool leftHanded{ false };  // [Controllers] bLeftHanded: pointer from the left controller
        bool cursorDot{ true };    // [Menus] bCursorDot
    };

    void          Configure(const Config& a_cfg);
    const Config& GetConfig();
    void          SetEnabled(bool a_on);  // pipe "pointer on|off"
    bool          Enabled();

    // Presenter, right after BeginFrame (fresh controller poses). a_cinema: the cinema quad carries the frame
    // the game is about to render (flat-screen fallback / cinema mode, submitting). Hit-tests the aim ray against
    // the cinema quad and hands pointer state to the game side. Returns true while the pointing hand's trigger
    // belongs to the pointer (ray on the quad with a target menu, or a click in progress): the pad then gets no
    // trigger from that hand.
    bool UpdateFrame(xr::Runtime& a_rt, bool a_began, bool a_cinema);
    // True if the pointing hand is the left one ([Controllers] bLeftHanded).
    bool LeftHand();

    // A world-locked menu panel (the dialogue option panel, Dialogue.h). While valid and the cinema quad is not shown,
    // UpdateFrame hit-tests this quad (reference space: centre pose, size in metres) instead of the cinema quad and
    // sends the events to the menu named `menu`; quad uv (0..1, origin top-left) maps to movie uv
    // u0 + u * (u1 - u0), v0 + v * (v1 - v0). No wheel from the stick on a panel. Main thread: set every frame
    // before UpdateFrame (valid = false clears it).
    struct Panel
    {
        bool        valid{ false };
        float       pos[3]{};
        float       quat[4]{ 0.0f, 0.0f, 0.0f, 1.0f };  // x, y, z, w
        float       width{ 0.0f }, height{ 0.0f };
        float       u0{ 0.0f }, v0{ 0.0f }, u1{ 1.0f }, v1{ 1.0f };
        std::string menu;
    };
    void SetPanel(const Panel& a_panel);
    // Pointer on the panel at the last UpdateFrame: quad uv and trigger state. False when not on the panel.
    bool PanelCursor(float& a_u, float& a_v, bool& a_down);

    // Presenter, when submitting the cinema quad: the texture to submit. With the pointer on the quad and
    // bCursorDot, a copy of a_backBuffer with the cursor drawn; else a_backBuffer itself.
    ID3D11Texture2D* PrepareCinema(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_backBuffer);

    std::string Status();  // pipe "pointer"
}
