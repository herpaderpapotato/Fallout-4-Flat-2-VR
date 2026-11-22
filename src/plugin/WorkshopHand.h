#pragma once
// Workshop placement and selection with the primary hand's pointer, as Fallout 4 VR (21 Nov, workshop agent;
// work/handoff/workshop.md).
//
// FO4VR 1.2.72 (Ghidra): JobListManager::AutoAim_PickRef (140D04AF0) and the main-render workshop tick (140D08150) build a
// Workshop::ContextData from the player (eye position + look direction, PlayerCharacter::GetEyeVector) and then overwrite
// its origin with the primary wand node's world translate and its direction with the node's forward axis (world rotate
// row 1) before Workshop::UpdateForegroundReferenceJob (highlight of existing objects) and Workshop::Update (the new /
// moved item: origin + direction * hold distance, CastSelection, snapping). Every other ContextData user (menu events,
// UpdateItemHoldDistance from the stick, OnAccept ...) keeps the eye.
// Flat 1.11.240 has the same two sites: AutoAim_PickRef BD5980 (call ContextData BD5999) and MainRenderAEnd BD5660
// (call BD56AC). Both calls are hooked (byte-checked): the original ctor, then origin / direction = the primary hand ray
// (game::HandRayWorld, the wall-clamped hand point the activation pick and the shots use).
// [Controllers] bWorkshopHandPointer=1 (live); 0 = the camera ray (flat). Pipe "wshand [on|off]".
#include <string>

namespace fo4flatvr::wshand
{
    void        Install();
    std::string Status();
    std::string Command(const std::string& a_arg);
}
