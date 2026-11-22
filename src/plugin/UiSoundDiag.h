#pragma once
// 20 Nov (controls; user: "Activating vats or another menu starts audio playing that has a tick sound. That sound repeats and
// stacks"): UI sound diagnostic. Detour of UIUtils::PlayMenuSound(const char* name) (flat 1.11.240 1086570: the path of every
// AS3 "PlaySound" call and of the engine's own UI sounds - UIMenuFocus, UIVATSSelectPart, UIPipBoyLightOn ...): counts plays
// per sound, logs the first two of each with the caller (module+rva, containing function), and a sound that repeats
// (>= 12 plays within 3 s) at most once per 10 s with its callers and the open menu stack. Pass-through only (the original
// always runs). Byte-checked; installed by bindings::InstallGlyphs. Pipe "bindings sounds".
//
// 21 Nov (user: "'UIItemFocus' REPEATING ... something constantly going off"): the tick is PlayerCharacter::UpdatePickRef's
// (D70BE0) "UIItemFocus", played whenever the activate pick changes to a non-empty one. With the hand ray / head-tracked
// camera the pick flickers between neighbouring refs every few frames, so it ticked constantly. Focus tick filter (default):
// a ref ticks only when it was not under the pick in the last fPickFocusSoundRepeatS (1.5 s).
// [Controllers] iPickFocusSound: 0 = no focus tick (FO4VR has none), 1 = filtered (default), 2 = the engine's (every change).
//
// 21 Nov (user: "Ticking again after vats", "its like a keypress"; the 02:33 log had no PlayMenuSound repeat): every
// engine sound. BSAudioManager GetSoundHandle / GetSoundHandleByFile (handle -> descriptor name or file, creator) and
// Play / PlayAfter / FadeInPlay / Stop (starter, handles playing per sound, polled with BSSoundHandle::IsPlaying).
// Log "engine sound: '<name> [formid]' ..." first two starts, REPEATING (>= 5 in 10 s), STACKING (more handles of one sound
// playing at once than before), LONG (one handle > 10 s). PlayMenuSound's REPEATING now also >= 5 in 10 s (a slow steady
// tick never reached 12 in 3 s). [Controllers] bSoundDiag=0 = engine sound hooks not installed (restart).

#include <string>

namespace fo4flatvr::uisound
{
    std::string Install();
    std::string Report();
    // the bindings context name for the repeat log (ControllerBindings, any thread)
    void SetContextName(const char* a_ctx);
}
