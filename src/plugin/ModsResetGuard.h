#pragma once
// New-game crash in character creation (26 Nov, plugin README "New-game crash in character creation").
//
// The game's mod manager asks for a reset when the installed / loaded mod set changed or a loaded plugin is on Bethesda.net's
// blacklist: MessageMenuManager::CreateOKMessage("$ChangedMods_ConfirmReset", ModsChanged_ConfirmResetCallback) from
// ModManager::CloseModManager (call A91B48) and StartMenuBase::InitialGetBlacklistCallback (call A87EE5, async network reply).
// The callback frees both CharacterCreation objects (A7EC70) and sets Main::resetGame / fullReset. When that prompt is
// answered while character creation (LooksMenu) is open, LooksMenu reads the freed object on its next frame and the game
// crashes at A8107C (reproduced 6/6).
// The guard: both call sites (byte-checked, chained) log the request with its caller stack; while LooksMenu is open the
// prompt is held back and shown when LooksMenu has closed (the game's own reset then runs as designed, outside character
// creation). Off switch: [General] bModsResetGuard=0 (the prompt is shown at once, as the game does).
#include <string>

namespace fo4flatvr::modsreset
{
    void        Install();
    std::string Status();
}
