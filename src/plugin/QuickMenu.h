#pragma once
// FO4VR quick menu (28 Nov, UI agent; work/handoff/ui-menus.md "28 Nov controls"). User: "the vr quick menu was the only way to
// holster before ... It needs to be implemented in the same way the vr version does today."
//
// FO4VR 1.2.72 (Ghidra): FavoritesHandler FUN_1412f1390 opens FavoritesMenu on the primary wand's stick click. Its movie is
// world_Favorites.swf (1024x1024: a ring of 12 favourites, the item name strip above it, "SHEATHE WEAPON" / "DRAW WEAPON" /
// "PUT AWAY WEAPON" on the grip), drawn on world_favorites.nif under the wand (disc r 8.31 u, name strip above, the shape
// 20.57 u above the wand origin, fFavoritesX/Y/Z/Pitch 0, scale 1). FUN_1412f4060 (thumbstick): angle of the stick from up,
// clockwise, + 15 deg, / 30 = entry 0..11 (centred = 12), UpdateCursorState(0, 0, x, y) + ProcessWandEvent(entry, "pressed")
// on every stick event, a 0.3 / 0.01 s pulse on the hand when the entry changes. FUN_1412f4900: WandTrigger release ->
// ProcessWandEvent(entry, "just_released") (the SWF uses the quickkey: BGSCodeObj.useQuickkey); WandGrip -> the player's
// DrawWeaponMagicHands (vtable 0xC9) with draw = weapon not drawn; the menu stays open. Its code object adds getUnsheathed.
//
// Flat 1.11.240: FavoritesMenu ctor 1049880 loads "FavoritesMenu" (call 104994D -> BSScaleformManager::LoadMovie 1B1B1D0)
// with "root1.Menu_mc" as menu object; PopulateFavorites (104A0FC) sets Menu_mc.favInfoArray (the VR SWF reads the same
// member); its code object maps useQuickkey / closeMenu / PlaySound (no getUnsheathed). ReadyWeapon's hold (PlayerControls
// handler 140DE01D0) calls the same vtable 0xC9 (draw = (ActorState+0xC & 0xE) < 6).
// 28 Nov (controls agent): measured in game, the player's vtable 0xC9 (PlayerCharacter::DrawWeaponMagicHands 140DA13D0: draw =
// ActionDraw 0x38, else ActionSheathe 0x39 through TESActionData) holsters and draws with FavoritesMenu open and the menu stays
// open (ws 3 -> 0 -> 3), as FO4VR's WandGrip; the earlier "did not holster" read came from CommonLib's drawn check (reads 1
// while holstered). The ReadyWeapon pad-X workaround is gone. The movie's weapon hint (testBox_mc: "$PUT AWAY WEAPON" /
// "$DRAW WEAPON") is translated through Interface/Translations/fo4flatvr_en.txt (QuickMenu.cpp LoadTranslations); its grip
// icon is FO4VR's Touch GRAB glyph: the movie runs at FO4VR's Touch platform 5 with FO4VR's "Controller VR" font loaded
// through Interface/fo4flatvr_vrfonts.swf + fontconfig.txt (install_vr_favorites.py; QuickMenu.cpp TouchPlatform).
// Here: the load call is hooked - with [Controllers] bQuickMenu=1 and Data/Interface/world_Favorites.swf present (FO4VR's file,
// installed by plugin/tools/install_vr_favorites.py) the VR movie is loaded instead; getUnsheathed is added to its code object;
// the bindings feed the stick / trigger / grip as FO4VR does; the Presenter shows the movie on a quad at the primary wand with
// the nif's geometry.
#include <string>

namespace fo4flatvr::quickmenu
{
    void Install();

    // bindings: [Controllers] bQuickMenu (live: read when FavoritesMenu is constructed)
    void SetEnabled(bool a_on);

    // the player's weapon draw / put away (Actor::DrawWeaponMagicHands with draw = not drawn), queued on the main thread;
    // FavoritesMenu (VR or flat movie) stays open. Also the stick click + grip holster combo is unaffected (ReadyWeapon held).
    void ToggleDrawn();
    // raw weapon state (Actor+0x134 bits 1..3; >= 3 drawn); CommonLib's drawn check reads drawn while holstered on flat
    int WeaponStateRaw();

    // FavoritesMenu is open with the VR movie (any thread)
    bool Active();

    // bindings (XR thread) while Active(): primary physical hand 0/1, primary stick, trigger held, grip pressed this frame
    void Input(int a_hand, float a_x, float a_y, bool a_trigger, bool a_gripPressed);

    // the quad's geometry in the wand frame (metres): width, centre up along the wand's up axis, crop of the 1024x1024 stage
    struct Geometry
    {
        float width{ 0.4496f };   // 31.47 u (name strip width)
        float up{ 0.3539f };      // 20.57 + 4.20 u (shape offset + centre of the cropped area)
        float u0{ 0.003f }, v0{ 0.004f }, u1{ 0.999f }, v1{ 0.811f };
    };
    Geometry GetGeometry();

    // close the menu without a selection (main-thread UI message; B / off grip / a second stick click)
    void Close(const char* a_why);

    std::string Status();
    std::string Command(const std::string& a_arg);
}
