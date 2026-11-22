#pragma once
// Pipe `facediag [npc]`: the player's FaceGen head (geometry, shader flags, material textures with a GPU read-back of a small
// mip) as `facediag:` log lines; `npc` adds the nearest NPC's head for comparison. Diagnostic only (19 Nov black-face report).
#include <string>

namespace fo4flatvr::facediag
{
    std::string Command(const std::string& a_arg);  // facediag [npc] | status | mode 0|1|2
    void        Install();
    void        AutoDump(const char* a_why);         // one dump 2 s later (at Present), at most 4 per session (selfie on)
    void        OnPresent();                         // Presenter thread, every Present: runs queued dumps + GPU read-backs                           // FaceGen customization render guard (call sites 6DF69B / 6EC07E)
}
