#pragma once
// fo4flatvr engine cull (22 Nov, VATS head agent): every fo4flatvr node hide / show goes through here (FloatingGun, GameDialogue,
// HeadHideCore's AAF POV hide). Same mechanism as FRIK-flat's vrshim/EngineCull.h:
// NiAVObject::SetAppCulled (vtable 0x2D, flat 16C86C0) sets the app-cull bit AND marks the node and its parents changed
// (0x20000000); a direct bit flip skipped that mark, so a node shown again later could stay undrawn (the player's head in VATS
// playback). Set(): bit 0 at once on the caller's thread, then the engine's SetAppCulled on the main thread (tq main queue,
// OnIdle C34794, no render running) re-applies the same state so the engine marks the node and its parents.
// Switch: fo4flatvr.ini [FRIK] bEngineCullFlag (one switch for both DLLs; live). 0 = the direct bit only, as before.
#include <RE/Fallout.h>

#include <string>

namespace fo4flatvr::enginecull
{
    void Set(RE::NiAVObject* a_node, bool a_culled);
    bool On();
    std::string Status();  // counters for the pipe / logs
}
