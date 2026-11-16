#pragma once
// Comfort effects: camera shake and view-warping image-space effects off in VR (work/handoff/comfort-effects.md).
// [Comfort] keys in the plugin ini, ON by default except bNoDepthOfField (1 = suppressed):
//  bNoCameraShake   ShakeCamera (827370) returns at once (first byte -> C3), as FO4VR's ShakeCamera is an empty
//                   function. Every camera shake goes through it: explosions (Explosion::CheckInit3D), the
//                   animation event "CameraShake" (CameraShakeHandler, e.g. power-armour footsteps / landings),
//                   Papyrus Game.ShakeCamera and the console CameraShake. The shake state (strength 326D360,
//                   time 326D364; read by GetCameraShake <- PlayerCamera::ApplyCameraShake <- FirstPersonState::
//                   Update) is zeroed at install and after every game load (LoadGame restores it from the save).
//  bNoRadialBlur    ImageSpaceEffectRadialBlur::IsActive (vtbl slot 29177F8 -> 22379E0) -> false. Imod radial blur
//                   (explosions, VATS scan, criticals, ImageSpaceModifierInstanceRB). FO4VR: IsActive = return false.
//  bNoDoubleVision  ImageSpaceEffectGetHit::IsActive (2917748 -> 2237470) -> false: the double-vision "get hit"
//                   effect (imod doubleVisionStrength; hit, concussion/crippled head, explosions, chems).
//  bNoImodBlur      ImageSpaceEffectFullScreenBlur::IsActive (2917688 -> 2235BB0) -> false: imod blur radius
//                   (concussion, damage, menu backdrops).
//  bNoDepthOfField  (DEFAULT 0) ImageSpaceEffectDepthOfField / ...SplitScreen / BokehDepthOfField ::IsActive (291A6E8
//                   -> 22566F0, 291A750 -> 22579E0, 2916000 -> 2215B90) -> false: ALL depth of field. The imod-blended
//                   DOF strength (manager +0x10C) is > 0 at rest outdoors, so imod DOF pulses cannot be separated from
//                   the world's own DOF look; 1 = no DOF at all.
// Tints (fade colour, tint, saturation/brightness/contrast) are untouched. Every site is byte-checked (vtable slot
// value + function bytes); a mismatch logs and leaves that item uninstalled. Vtable entries are data writes; the
// wrappers call the engine's IsActive when the item is switched off at runtime.

#include <string>

namespace fo4flatvr::comfx
{
    // Once at plugin load: reads [Comfort] bNoCameraShake / bNoRadialBlur / bNoDoubleVision / bNoImodBlur / bNoDepthOfField,
    // checks and patches the sites. Returns a summary for the log.
    std::string Install();

    // Main thread, after a save is loaded / new game: clears a camera shake restored from the save.
    void OnGameLoaded();

    // Pipe "comfx [status | shake|radial|double|blur|dof 0|1]": switches, install state, shake state, counters
    // (imod effect frames the engine wanted active and we suppressed).
    std::string Command(const std::string& a_arg);
}
