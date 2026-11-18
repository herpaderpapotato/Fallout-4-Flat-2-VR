#pragma once
// VR gestures, VRIK style (16 Nov; analysis/vr-gestures.md). Presenter thread only (called from bindings::Map).
//
// Gesture button = a stick click. Per hand (physical Right / Left, as VRIK), 13 slots in VRIK's order:
//   0 Tap (press without moving), 1 Up, 2 Down, 3 Left, 4 Right, 5 Back, 6 Forward,
//   7 Up+Down, 8 Down+Up, 9 Left+Right, 10 Right+Left, 11 Back+Forward, 12 Forward+Back (out more than the threshold, then
//   back within it: VRIK GestureMonitor::update).
// Directions are in the head's yaw frame captured when the click goes down (VRIK), hand position relative to the head (head
// motion during the gesture does not count). Classification on release (VRIK): net move >= fMoveCm -> the dominant axis of
// the net move (ours: it must be fDominance x the second, else nothing); net move < fMoveCm and the largest excursion >
// fMoveCm -> out-and-back (the axis/direction of the largest excursion from the end point, same dominance test); else a tap.
// Each slot's action is chosen in MCM (Data\MCM\Config\fo4flatvr): 0 none (default, as VRIK ships), 1 key press (a key
// + modifier from the MCM list, sent with SendInput while the game window has the focus: game controls, MCM hotkeys of
// other mods, and our own F2/F3/F8), 2 recentre, 3 first/third person, 4 camera view cycle.
// Safeguards: cancelled by the same hand's grip (holster chord), the other stick's click (FRIK's config chord), a hold over
// fMaxHoldS, an invalid pose or leaving gameplay; fCooldownS after a gesture fires; while a gesture stick is clicked, that
// stick's own jobs (turn / jump / sneak / move) are held at zero until it is back in the dead zone. VRIK button delay for a
// stick click that the game also uses (the off hand's = sprint): the click reaches the game only after fButtonDelayS held
// still (haptic tick), and then that press is no gesture.
//
// Both hands up = VRIK's selfie mode with FRIK-flat's mirror body ([Selfie]): raising the chosen hand(s) fHandsUpCm above
// the HMD for fHoldS toggles it (re-arms only below the HMD). While the hands stay up after turning it on, the body is placed
// fDistanceM in front of the head, facing you, and rolling the steering hand's wrist turns it (fRollGain, fRollDeadzoneDeg).
// Lowering the hands freezes it. Ends on the gesture again, any menu / Pip-Boy, or a camera state other than first person.
//
// Settings: fo4flatvr.ini [Gestures] / [Selfie] (optional), overridden by MCM's Data\MCM\Settings\fo4flatvr.ini (the file
// the game sees through MO2's VFS: overwrite\MCM\Settings), re-read about once a second when its time stamp changes - MCM
// changes apply without a restart. Off switch: [Gestures] bEnabled=0 (and [Selfie] bEnabled=0).
// Pipe: gesture [status|reload|on|off|test <hand R|L> <slot 0-12>]

#include <string>

namespace xr
{
    struct ControllerState;
    struct Pose;
}

namespace fo4flatvr::gestures
{
    struct Frame
    {
        const xr::ControllerState* c{ nullptr };
        const xr::Pose*            head{ nullptr };  // this frame's head pose (reference space), null = invalid
        bool                       gameplay{ false };  // bindings gameplay / quick-loot context (world view, no menu)
        int                        camState{ -1 };     // game camera state (0 = first person)
        bool                       leftHanded{ false };
    };

    // Once per XR frame, before the gameplay mapping uses the sticks.
    void Update(const Frame& a_f);

    // physical hand 0 = left, 1 = right: that stick's deflection is held at zero (click held / latched after a gesture)
    bool StickHeld(int a_hand);
    // physical hand: the click the game should see (off hand = sprint: only after the button delay when its gestures are on)
    bool ClickToGame(int a_hand, bool a_raw);

    // Presenter: a recentre was asked for by a gesture (taken once)
    bool TakeRecentre();
    // Presenter: any recentre happened (keyboard, pipe, gesture): hands-up is ignored for a moment after it
    void NoteRecentre();

    // Selfie request for FRIK-flat (FrikBridge GetSelfie, any thread)
    struct Selfie
    {
        bool  active{ false };
        bool  follow{ false };     // hands up: place the body in front of the head now and steer it
        float distanceM{ 1.4f };
        float yawRad{ 0.0f };      // extra turn about up from "facing the viewer" (counter-clockwise seen from above)
        unsigned seq{ 0 };         // increments on every on/off toggle
    };
    Selfie SelfieState();

    std::string Command(const std::string& a_arg);
}
