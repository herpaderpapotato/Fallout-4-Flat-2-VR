#include "plugin/CameraDirector.h"
#include "plugin/EngineCull.h"
#include "plugin/ThreadQueues.h"

#include "plugin/CameraTweaks.h"
#include "plugin/ConfigStore.h"
#include "plugin/GameMenuPointer.h"

#include "plugin/GameBridge.h"
#include "plugin/Gestures.h"
#include "plugin/HeadPose.h"
#include "plugin/Roomscale.h"
#include "plugin/Settings.h"
#include "plugin/VatsWindow.h"
#include "plugin/VirtualPad.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/HeadHideCore.h"

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <chrono>
#include <intrin.h>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace fo4flatvr::camdir
{
    namespace
    {
        // ---- 1.11.240 (analysis/vr-camera-states.md section 6; re-read in Ghidra for this) ---------------
        constexpr std::uintptr_t kCallSite = 0x1026035;     // PlayerCamera::UpdateCamera -> TESCamera::Update
        constexpr std::uintptr_t kCameraUpdate = 0x827C80;  // TESCamera::Update
        constexpr std::uint8_t   kCallBytes[5] = { 0xE8, 0x46, 0x1C, 0x80, 0xFF };
        constexpr std::uint8_t   kPrologue[9] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
        // Free camera (AAF scenes, tfc; analysis/mod-aaf-vr.md): PlayerCharacter::UpdateFreeCamera (D5A960, from
        // OnIdle_UpdatePlayer C34FA0) and the load-game path (D682F0, when the camera is state 3) update the camera through
        // this 5-byte thunk, not through UpdateCamera's call site above, so the director never ticked in state 3.
        // 1026290: E9 EB 19 80 FF = jmp 827C80 (TESCamera::Update), then CC padding (read in Ghidra 1.11.240).
        constexpr std::uintptr_t kFreeCamThunk = 0x1026290;
        constexpr std::uint8_t   kFreeCamThunkBytes[5] = { 0xE9, 0xEB, 0x19, 0x80, 0xFF };
        constexpr int            kStateFree = 3;
        constexpr int            kStateThird = 8;
        constexpr int            kStates = 13;

        constexpr float kPi = 3.14159265358979f;
        constexpr float kR2D = 180.0f / kPi;

        using Clock = std::chrono::steady_clock;

        const char* const kStateNames[kStates] = { "FirstPerson", "AutoVanity", "VATS", "Free", "IronSights", "Transition", "Tween",
            "Animated", "ThirdPerson", "Furniture", "Mount", "Bleedout", "Dialogue" };
        // Default table (15 Nov, user: "everything that we're not handling specific instances of ... should use the same
        // method that the AAF fix does. There should be no instances of the flat panel appearing and rendering a scene unless
        // we specifically opt in"): first person / iron sights / furniture (first-person sit) TRACK; VATS keeps its own
        // handling (the VATS window, user's choice) = CINEMA; every other game camera SCENE (the AAF free-camera method).
        // The flat panel is opt-in per state: sPolicy<State>=CINEMA.
        constexpr Policy kDefaultTable[kStates] = { Policy::kTrack, Policy::kScene, Policy::kCinema, Policy::kScene,
            Policy::kTrack, Policy::kScene, Policy::kScene, Policy::kScene, Policy::kScene, Policy::kTrack,
            Policy::kScene, Policy::kScene, Policy::kScene };

        float Wrap(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

        bool ParsePolicy(std::string s, Policy& a_out)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (s == "TRACK") a_out = Policy::kTrack;
            else if (s == "SCENE") a_out = Policy::kScene;
            else if (s == "FADE_SNAP" || s == "FADE" || s == "FADESNAP") a_out = Policy::kFadeSnap;
            else if (s == "LOCK") a_out = Policy::kLock;
            else if (s == "CINEMA") a_out = Policy::kCinema;
            else return false;
            return true;
        }

        int ParseState(const std::string& s)
        {
            for (int i = 0; i < kStates; ++i) {
                if (_stricmp(s.c_str(), kStateNames[i]) == 0) return i;
            }
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 0);
            return (end != s.c_str() && *end == 0 && v >= 0 && v < kStates) ? static_cast<int>(v) : -1;
        }

        const char* StateName(int s) { return s >= 0 && s < kStates ? kStateNames[s] : "none"; }

        struct V3 { float x{}, y{}, z{}; };

        // quaternion (w,x,y,z) from a column-vector rotation matrix (same as GameBridge's MatToQuat)
        void MatToQuat(const float m[3][3], float& w, float& x, float& y, float& z)
        {
            const float tr = m[0][0] + m[1][1] + m[2][2];
            if (tr > 0.0f) {
                const float s = std::sqrt(tr + 1.0f) * 2.0f;
                w = 0.25f * s; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s;
            } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
                const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
                w = (m[2][1] - m[1][2]) / s; x = 0.25f * s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s;
            } else if (m[1][1] > m[2][2]) {
                const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
                w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; y = 0.25f * s; z = (m[1][2] + m[2][1]) / s;
            } else {
                const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
                w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; z = 0.25f * s;
            }
        }

        // NiMatrix3 as FirstPersonState::Update (10204F0) writes it from the GetRotation quaternion: stored row i
        // = column i of R(q) (column-vector convention). Rows are 16 bytes apart (4th float = 0).
        void QuatToNi(float w, float x, float y, float z, float* f)
        {
            f[0] = 1.0f - 2.0f * (y * y + z * z); f[1] = 2.0f * (x * y + w * z);        f[2] = 2.0f * (x * z - w * y);        f[3] = 0.0f;
            f[4] = 2.0f * (x * y - w * z);        f[5] = 1.0f - 2.0f * (x * x + z * z); f[6] = 2.0f * (y * z + w * x);        f[7] = 0.0f;
            f[8] = 2.0f * (x * z + w * y);        f[9] = 2.0f * (y * z - w * x);        f[10] = 1.0f - 2.0f * (x * x + y * y); f[11] = 0.0f;
        }

        // yaw (heading convention: 0 = +Y, + = right) and pitch (+ = up) of a stored NiMatrix3's forward (R * +Y)
        void NiYawPitch(const float* f, float& a_yaw, float& a_pitch)
        {
            const float fx = f[4], fy = f[5], fz = f[6];
            a_yaw = std::atan2(fx, fy);
            a_pitch = std::asin(std::clamp(fz, -1.0f, 1.0f));
        }

        // SCENE base yaw of a game camera rotation: the forward's heading, or (camera looking almost straight down / up, as
        // furniture and orbit cameras do) the up vector's, so the base yaw does not spin through the pole
        float SceneYaw(const float* f)
        {
            const float fx = f[4], fy = f[5], fz = f[6];
            if (std::abs(fz) > 0.95f) {
                const float ux = f[8], uy = f[9];
                return fz < 0.0f ? std::atan2(ux, uy) : std::atan2(-ux, -uy);
            }
            return std::atan2(fx, fy);
        }

        // 20 Nov late ([Camera] bOrbitStatesScene): the game's orbit cameras (idle orbit 1, third person 8, mount 10, bleedout 11,
        // dialogue camera 12; BleedoutCameraState derives from ThirdPersonState). TRACK there writes Rz(owned heading) x head
        // into the orbit camera's root at the orbit position (user ini sPolicyBleedout=TRACK, 20 Nov 22:45 headset: "bleedout
        // status camera causing movement based on head orientation"); SCENE takes the orbit camera's own yaw as the base.
        bool OrbitState(int a_st) { return a_st == 1 || a_st == 8 || a_st == 10 || a_st == 11 || a_st == 12; }

        struct Cfg
        {
            bool   enabled{ true };
            Policy table[kStates]{};
            // 15 Nov (user: "these fade to blacks ... they're too long and they should default to 100ms"): every fade / black
            // hold in the director defaults to 100 ms
            float  fadeMs{ 100.0f };
            float  timeoutS{ 0.1f };
            float  jumpUnits{ 50.0f };  // TRACK: game translation jump per update that triggers a fade-snap blink (0 = off)
            float  unitsPerMeter{ 70.0f };
            float  heightOffsetCm{ 0.0f };
            // camera step 2 gates ([Camera]; patched when on at load, pipe "camdir gate <name> on|off" toggles a patched one).
            // PAUSED (14 Nov, coordinator): all default OFF until the step-2 verification is finished.
            bool   gateFurniture{ false };    // bGateFurniture: StartFurnitureMode -> HUD mode only (seated in first person)
            bool   gatePowerArmor{ false };   // bGatePowerArmor: same for power-armour furniture + fade hold while entering/exiting
            bool   workbenchDirect{ true };   // bWorkbenchDirectMenu: with bGateFurniture, workbenches open their crafting menu directly
            bool   gateWorkbenchBody{ false }; // with bGateFurniture: workbenches get the third-person body (tested 15 Nov: the sit still aborts after ~4 s, no menu; off)
            bool   gateAnimated{ false };     // bGateAnimated: StartAnimatedMode -> no-op (animated cams stay first person)
            bool   blockThirdPerson{ false }; // bBlockThirdPerson: Set3rdPerson / Force3rdPerson -> no-op
            bool   gateVats{ false };         // bGateVats: Start/ForceVATSMode -> no-op (+ StopVATSMode when nothing was pushed); VATSMenu world view
            bool   vatsKeepCamera{ false };   // bVatsKeepCamera (VG2): after VATS GameState::ChangeState(2): PlayerCamera::SetEnabled(true)
                                             // + QueueCullPlayerGeometry(false,false), so the camera and the 1st-person body stay live
            bool   pipboyStopTick{ false };   // bPipboyStopTick: StopPipboyMode's own TESCamera::Update call -> director tick too
            bool   headingSnap{ false };      // bHeadingSnap: game-driven heading jump >= fHeadingSnapDeg -> fade-snap the view to it
            float  headingSnapDeg{ 45.0f };
            float  paHoldS{ 0.1f };          // fPowerArmorHoldS: longest black hold for a power-armour enter/exit (was 12 s)
            // Free-camera scene view (AAF scenes; analysis/mod-aaf-vr.md option B). [Camera] bFreeCamScene (default 1):
            // the director also ticks in the free camera (thunk 1026290) and state 3 is a head-tracked world view:
            // translation = the free camera's (+ raw head offset), rotation = Rz(free-camera yaw) x head (game pitch
            // dropped). 0 = no thunk patch; state 3 follows sPolicyFree (default CINEMA) as before.
            bool   freeScene{ true };
            bool   freeBody{ true };          // bFreeCamBody: entering state 3 from first person shows the third-person body
            float  freeYawCutDeg{ 20.0f };    // fFreeCamYawCutDeg: free-camera yaw change in one update that blinks (0 = off)
            float  freeGlideSpeed{ 0.0f };    // fFreeCamGlideSpeed: game-driven camera speed (units/s) held black (0 = off; was 150)
            float  freeGlideHoldS{ 0.1f };    // fFreeCamGlideHoldS: longest such black hold (was 4 s)
            float  thirdGraceMs{ 100.0f };    // fThirdPersonGraceMs: a CINEMA state 8 fades this long first (0 = off; was 1000)
            // Return to first person (15 Nov, user: stuck in third person after AAF, "no hot keys would let me back"):
            // a third person a script put the player in (Force3rdPerson, or the free camera ending back in state 8) goes back
            // to first person once the player has been controls-driven (not AI-driven), not sitting and without a menu for
            // fThirdPersonReturnS. The player's own POV toggle (Set3rdPerson) is never undone. [Camera] bThirdPersonReturn.
            bool   thirdReturn{ true };
            float  thirdReturnS{ 1.0f };
            bool   freeAafThird{ true };      // bFreeCamAAFThirdPerson: AAF's own Force3rdPerson passes bBlockThirdPerson (player AAF-busy)
            bool   freeMoveView{ true };      // bFreeCamMoveWithView: free-camera stick flying follows the head-tracked view
            bool   freeHeadDelta{ true };     // bFreeCamHeadDelta: head offset counted from the scene entry / last snap (POV at the head)
            // 20 Nov (CameraDirector.h): the two camera-lock comparison modes, HUD notifications, same controls in every view
            bool   lockFp{ false };           // bLockFirstPerson
            bool   bodyCam{ false };          // bBodyCamera
            bool   hudNotify{ true };         // bCameraHudNotify
            bool   sameInput{ true };         // bSameInputAllViews
            // 20 Nov late (user: "AAF fails defeat scenes when third person is blocked"; "bleedout status camera causing movement
            // based on head orientation"; "AAF with PoV camera ... drifts back to a point left"; "locked out of pip boy"):
            bool   lockScenes{ true };        // bLockAllowScenes: under bLockFirstPerson, AAF / defeat scenes keep their cameras
            float  defeatWindowS{ 30.0f };    // fDefeatWindowS: a script's third person this long after a bleedout is the defeat flow
            bool   orbitScene{ true };        // bOrbitStatesScene: TRACK on an orbit camera (idle orbit, third person, mount,
                                             // bleedout, dialogue camera) is shown as SCENE
            bool   freeOwnStick{ true };      // bFreeCamOwnStickYaw: the stick's turn in the free camera is our own offset
            bool   aftercare{ true };         // bSceneAftercare: Pip-Boy refusals diagnosed; scene leftovers released
            // 21 Nov (user: "camera stays on the body" stays where the player's body was in a scene, not at the animated
            // location; "none of the POV AAF cameras actually follow an actor's head position")
            bool   bodyFollow{ true };        // bBodyCameraFollowsAnimation: the body camera rides the animated Head bone when
                                             // the body is animated away from the capsule (scenes, furniture, paired anims)
            float  bodyEyeFwd{ 8.0f };        // fBodyCameraEyeForward: eye in front of the Head bone along the face (units)
            bool   bodyYieldPov{ true };      // bBodyCameraYieldsToPov: free camera at ANOTHER actor's head (AAF POV) = SCENE
            bool   freeStatePos{ true };      // bFreeCamStatePosition: the free camera's own translation (after AAF's write)
        };

        struct Fade
        {
            float             alpha{ 0.0f };
            bool              target{ false };
            Clock::time_point lastAdvance{};
            // the FADE_SNAP episode
            bool              active{ false };
            bool              timedOut{ false };
            int               fromState{ -1 }, causeState{ -1 };
            Clock::time_point start{}, black{}, inStart{};
            bool              blackSeen{ false }, inRunning{ false };
            // last completed episode (readout)
            double            lastOutMs{ -1 }, lastHoldMs{ -1 }, lastInMs{ -1 };
            std::string       lastEpisode{ "-" };
            std::uint64_t     episodes{ 0 }, timeouts{ 0 };
            // pipe "camdir fadetest": fade out, hold, fade in (no pose change)
            bool              test{ false };
            double            testHoldMs{ 500 };
            // camera step 2: hold black until a condition clears (power armour), snap the owned heading at the snap
            int               hold{ 0 };              // 0 none, 1 power armour (player sit/sleep state != 0)
            Clock::time_point holdStart{}, holdClear{};
            bool              holdClearSeen{ false };
            bool              snapHeading{ false };
            std::uint64_t     headingSnaps{ 0 }, paHolds{ 0 }, paHoldTimeouts{ 0 };
            float             lastHeadingJumpDeg{ 0.0f };
            double            lastPaHoldMs{ -1 };
        };

        struct Dir
        {
            bool              installed{ false };
            const char*       installNote{ "not installed" };
            int               state{ -2 }, prevState{ -2 };
            Policy            policy{ Policy::kCinema }, eff{ Policy::kCinema };
            Clock::time_point stateSince{};
            std::uint64_t     ticks{ 0 }, writes[4]{}, transitions{ 0 };
            bool              haveGood{ false };
            V3                prevTg;
            bool              havePrevTg{ false };
            std::uint64_t     jumpFades{ 0 };
            float             lastJump{ 0 };
            V3                goodT, goodOff;   // last TRACK translation and the raw head offset (world units) then
            V3                holdBase;         // FADE_SNAP / LOCK anchor: output = holdBase + raw head offset now
            V3                tg, out;          // game's translation this tick / what we wrote (or left)
            V3                add, raw;         // TRACK head offset added (residual) / raw head offset (world units)
            V3                prevWorld;
            bool              havePrevWorld{ false };
            std::uint64_t     rootJumps{ 0 };
            float             ownYaw{ 0 }, heading{ 0 }, camYaw{ 0 }, camPitch{ 0 }, gameYaw{ 0 }, gamePitch{ 0 };
            bool              haveOwn{ false }, haveHead{ false }, wrote{ false };
            int               sitSleep{ -1 };
            // free-camera scene view (state 3 with bFreeCamScene)
            bool              freeHooked{ false };
            const char*       freeNote{ "off" };
            float             lastBaseYaw{ 0 };      // base yaw (heading convention) of the last written / tracked pose
            bool              lastBaseValid{ false };  // the last tick showed the world view (lastBaseYaw is the view's yaw)
            float             sceneYaw{ 0 };         // state 3: base yaw in use (frozen while a fade holds)
            float             freeYawOffset{ 0 };    // state 3: sceneYaw = free-camera yaw + this (continuity at entry; 0 after a snap)
            bool              freeEntry{ false };
            bool              havePrevGy{ false };
            float             prevGy{ 0 };
            int               glideFrames{ 0 };
            float             lastSpeed{ 0 };
            Clock::time_point lastTick{};
            bool              haveLastTick{ false };
            std::uint64_t     yawCuts{ 0 }, glideHolds{ 0 }, glideTimeouts{ 0 }, bodyShows{ 0 }, thirdGraces{ 0 };
            std::uint64_t     aafThirdPassed{ 0 }, moveViewCalls{ 0 };
            head::Vec3        freeOffBase{};         // state 3: head offset (m, body frame) at the entry / last snap
            bool              haveFreeOffBase{ false };
            bool              moveViewOk{ false };    // state 3 TRACK with the view written last tick: movement may use it
            float             moveViewYaw{ 0 }, moveViewPitch{ 0 };  // that view (heading convention; pitch + = up)
            // SCENE states (the AAF method for every game camera; Install fills it from the table)
            bool              sceneSt[kStates]{};
            bool              sceneKeepHead{ false };  // entry from another SCENE state: keep the head-motion base (continuity)
            bool              glideLatched{ false };   // one glide hold per glide (no strobe when the hold is short)
            Clock::time_point glideQuietSince{};
            std::uint64_t     sceneEntries{ 0 };
            // return to first person
            bool              thirdArmed{ false }, thirdQueued{ false };
            Clock::time_point thirdCtrlSince{}, fpQuietSince{};
            std::uint64_t     thirdReturns{ 0 };
            std::string       lastReturn{ "-" };
            // first-person lock backstop / body camera (20 Nov)
            Clock::time_point lockForceAt{};
            std::uint64_t     lockForces{ 0 };
            std::string       lockLast{ "-" };
            bool              body{ false };        // this tick: the body camera applied
            V3                bodyBase, prevBody;   // eye base on the body (before the head offset)
            bool              havePrevBody{ false };
            float             bodyEyeH{ 0.0f };
            std::uint64_t     bodyTicks{ 0 };
            // body camera follows the animation (bBodyCameraFollowsAnimation, 21 Nov)
            float             bodyFollowW{ 0.0f };     // 0 = capsule eye, 1 = animated eye (blended over kBodyBlendS)
            bool              bodyFollowWant{ false };
            bool              bodyAnimOk{ false };     // the Head bone was read this tick
            bool              bodySceneNow{ false };   // scene / furniture / bleedout signal (10 Hz)
            float             bodyDisp{ 0.0f };        // animated eye - capsule eye (units)
            V3                bodyAnimEye{};
            Clock::time_point bodyFollowAt{}, bodySceneAt{};
            std::uint64_t     bodyFollowSnaps{ 0 }, bodyFollowBlends{ 0 }, bodyAnimMiss{ 0 };
            // bBodyCameraYieldsToPov: the free camera sits at another actor's head (AAF POV on an NPC; PollHeadHide, 10 Hz)
            bool              bodyYield{ false };
            bool              lastScene{ false };      // last tick showed SCENE (body <-> SCENE switch in one state)
            std::string       bodyYieldWho{ "-" };
            std::uint64_t     bodyYields{ 0 };
            std::uint64_t     freeStateReads{ 0 };     // bFreeCamStatePosition: ticks that used the state's translation
            bool              preYawOk{ false };   // the game camera's own yaw (before our write), for GameCameraYaw
            float             preYaw{ 0.0f };
            // free camera: the stick's turn kept as our own yaw on top of the game's (AAF's POV auto mode re-aims the free
            // camera at the actor's gaze 1.2 s after the last look input, so a turn written into the camera drifted back)
            float             stickYaw{ 0.0f };
            std::uint64_t     stickTurnUpdates{ 0 };
            // AAF / defeat scenes under the camera locks
            Clock::time_point sceneFlowSeen{};      // last tick a scene / defeat signal held
            Clock::time_point flowCheckAt{};        // last SceneFlow evaluation (10 Hz)
            bool              sceneFlowNow{ false };
            std::uint64_t     scenePasses{ 0 }, lockForceSkips{ 0 };
            std::string       lastScenePass{ "-" };
        };
        // HUD notifications (PollNotify; main thread)
        struct Notify
        {
            int           state{ -100 };
            std::string   mode;
            std::uint64_t shown{ 0 }, refusedShown{ 0 };
            std::string   last{ "-" };
        };
        Notify             g_n;
        std::string        g_refused;            // a refused camera request to announce (g_lock)
        Clock::time_point  g_refusedAt{};        // last announced refusal (throttle)
        std::string        g_refusedLastText;
        std::atomic<std::uint64_t> g_freeTicks{ 0 };
        std::atomic<bool>          g_thirdArmReq{ false };  // HkCamFn<Force3rdPerson>: a script third person (arms the return)

        std::mutex g_lock;
        Cfg        g_cfg;
        Dir        g_d;
        // 20 Nov (live settings): the free-camera thunk is patched whatever bFreeCamScene says at load; this is the switch
        // (read without the lock by the thunk hook). FreeOn(): the scene view is on and the thunk patched (g_lock held).
        std::atomic<bool> g_freeSceneLive{ false };
        bool FreeOn() { return g_d.freeHooked && g_cfg.freeScene; }
        Fade       g_f;

        using Update_t = void (*)(RE::TESCamera*);
        Update_t g_orig = nullptr;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // ---- camera step 2 gates (analysis/vr-camera-states.md sections 4B, 6; FO4VR gates the same entry points) ----
        // 1.11.240 RVAs, read in Ghidra for this step:
        constexpr std::uintptr_t kStartFurniture = 0x1027C20;   // PlayerCamera::StartFurnitureMode(cam, ref)
        constexpr std::uintptr_t kIsInFurniture = 0x1027B70;    //   its call @1027C47
        constexpr std::uintptr_t kQFurnEnterExit = 0x9DD8A0;    //   HUDMode::QFurnitureEnterExit, its call @1027D9A
        constexpr std::uintptr_t kPushHudMode = 0xA373A0;       //   SendHUDMessage::PushHUDMode, its call @1027DA2
        constexpr std::uintptr_t kPaFurnKeyword = 0x374830;     // PowerArmor::GetFurnitureKeyword (call @44CBB3 in TESFurniture::Activate)
        constexpr std::uintptr_t kStartAnimated = 0x1028720;    // PlayerCamera::StartAnimatedMode(cam, const char* node, bool)
        constexpr std::uintptr_t kSet3rd = 0x1026D30;           // PlayerCamera::Set3rdPerson(cam)
        constexpr std::uintptr_t kForce3rd = 0x1026DD0;         // PlayerCamera::Force3rdPerson(cam)
        constexpr std::uintptr_t kStartVats = 0x10273D0;        // PlayerCamera::StartVATSMode(cam)
        constexpr std::uintptr_t kForceVats = 0x10274B0;        // PlayerCamera::ForceVATSMode(cam)
        constexpr std::uintptr_t kStopVats = 0x10274F0;         // PlayerCamera::StopVATSMode(cam)
        constexpr std::uintptr_t kPipboyStopCall = 0x10273A6;   // StopPipboyMode (1027250) -> TESCamera::Update
        constexpr std::uint8_t   kPipboyStopBytes[5] = { 0xE8, 0xD5, 0x08, 0x80, 0xFF };

        // VG2 (analysis/vr-vats-dialogue.md): VATSMenuPrivate::GameState::ChangeState; state 2 calls PlayerCamera vfunc +0x10
        // SetEnabled(false) (10263D0) and TaskQueueInterface::QueueCullPlayerGeometry(true,true) (call @B16C2A -> C4AF90)
        constexpr std::uintptr_t kVatsChangeState = 0xB16A40;
        constexpr std::uintptr_t kCamSetEnabled = 0x10263D0;
        constexpr std::uintptr_t kQueueCullPlayer = 0xC4AF90;
        constexpr std::uintptr_t kTaskQueueSingleton = 0x32FF808;  // TaskQueueInterface* (operand of the calls in ChangeState)
        constexpr std::uintptr_t kState2SetEnabled = 0xB16C0B;     // ChangeState(2): call [rax+10] = PlayerCamera::SetEnabled(false)
        constexpr std::uintptr_t kState2TaskQueueLoad = 0xB16C0E;  //   mov rcx,[TaskQueueInterface]
        constexpr std::uintptr_t kState2CullCall = 0xB16C2A;       //   call QueueCullPlayerGeometry(true,true)
        // VATS image-space modifier (VatsMenuObj::SetMenuState B18CD0: state 2 TriggerCrossFade -> +0x1240; leaving the
        // targeting states: ImageSpaceModifierInstance::Stop(+0x1240) @B18D84 -> 4AC3C0 = set flag 0x10 if in the active list)
        constexpr std::uintptr_t kImodInstStop = 0x4AC3C0;
        constexpr std::size_t    kVatsGameStateInObj = 0xF8;       // ChangeState's this = VatsMenuObj + 0xF8 (lea rcx,[rsi+0F8h])
        constexpr std::size_t    kVatsImodInstInObj = 0x1240;
        using ImodInstStop_t = void (*)(void*);
        ImodInstStop_t g_imodInstStop = nullptr;
        bool           g_vatsNoImod = true;  // [Menus] bVatsNoImod (with bVatsLiveWorld)
        constexpr std::uintptr_t kIsInFurnCallSite = 0x1027C47;    // StartFurnitureMode -> IsInFurnitureMode
        constexpr std::uintptr_t kQFurnCallSite = 0x1027D9A;       // StartFurnitureMode -> HUDMode::QFurnitureEnterExit
        constexpr std::uintptr_t kPushHudCallSite = 0x1027DA2;     // StartFurnitureMode -> SendHUDMessage::PushHUDMode
        constexpr std::uintptr_t kPaKwCallSite = 0x44CBB3;         // TESFurniture::Activate -> PowerArmor::GetFurnitureKeyword

        // 20 Nov, first-person lock (read in Ghidra 1.11.240): PlayerCamera::ToggleFreeCameraMode(cam, bool) 1027920 (enters
        // with PushState(3) unless the current state is the free camera +0xF8, else pops it), StartBleedoutMode(cam) 1028580
        // (SetState(+0x138) and the old state onto tempReturnStates; StopBleedoutMode 1028600 only removes +0x138 from that
        // stack when it is not current: harmless without a start), StartMountMode(cam, mount handle*) 10282E0 (PushState(10);
        // StopMountMode 1028370 searches the stack for +0x130 first: harmless without a start). The dialogue camera is not
        // detoured: StartDialogueMode also updates the followers' packages; bDialogueCameraEnable=0 stops TryDialogueCamera
        // before it (camtweak, forced on by the lock).
        constexpr std::uintptr_t kToggleFreeCam = 0x1027920;
        constexpr std::uintptr_t kStartBleedout = 0x1028580;
        constexpr std::uintptr_t kStartMount = 0x10282E0;

        enum GateId { kGFurniture, kGAnimated, kGSet3rd, kGForce3rd, kGStartVats, kGForceVats, kGStopVats, kGPipboyStop, kGVatsState,
            kGFreeCam, kGBleedout, kGMount, kGates };
        const char* const kGateNames[kGates] = { "StartFurnitureMode", "StartAnimatedMode", "Set3rdPerson", "Force3rdPerson",
            "StartVATSMode", "ForceVATSMode", "StopVATSMode", "StopPipboyMode->TESCamera::Update", "VATS ChangeState(2)",
            "ToggleFreeCameraMode", "StartBleedoutMode", "StartMountMode" };
        struct GateStat
        {
            bool          patched{ false };
            std::uint64_t calls{ 0 }, blocked{ 0 };
        };
        GateStat          g_gs[kGates];
        std::uint64_t     g_paFurniture{ 0 };
        std::string       g_lastGate{ "-" };
        bool              g_vatsBlocked = false;   // a StartVATSMode/ForceVATSMode was skipped (StopVATSMode then skips too)
        std::atomic<bool> g_paRequest{ false };     // power-armour furniture gated: director starts a held fade
        std::atomic<bool> g_menuWindow{ false };    // Presenter: a view-taking menu is in the flat window (SetMenuWindow)

        using StartFurn_t = void (*)(RE::PlayerCamera*, RE::TESObjectREFR*);
        using StartAnim_t = void (*)(RE::PlayerCamera*, const char*, bool);
        using CamFn_t = void (*)(RE::PlayerCamera*);
        using IsInFurn_t = bool (*)(RE::PlayerCamera*);
        using QHudMode_t = const void* (*)();
        using PushHud_t = void (*)(const void*);
        using GetKw_t = RE::BGSKeyword* (*)();
        StartFurn_t g_origStartFurn = nullptr;
        StartAnim_t g_origStartAnim = nullptr;
        CamFn_t     g_origSet3rd = nullptr, g_origForce3rd = nullptr, g_origStartVats = nullptr, g_origForceVats = nullptr,
                    g_origStopVats = nullptr;
        IsInFurn_t  g_isInFurniture = nullptr;
        QHudMode_t  g_qFurnEnterExit = nullptr;
        PushHud_t   g_pushHudMode = nullptr;
        GetKw_t     g_paKeyword = nullptr;
        // workbench body swap under the furniture gate (HkStartFurniture / PollWorkbenchBody)
        constexpr std::uintptr_t kShow1stPerson = 0xD5CF60;    // PlayerCharacter::Show1stPerson(player, bool) (StartFurnitureMode @1027D6F)
        constexpr std::uintptr_t kIsFurnitureRef = 0x55F960;   // TESObjectREFR::IsFurniture(ref) (@1027C32)
        constexpr std::uintptr_t kIsWorkbench = 0x44C630;      // TESFurniture::IsWorkbench(base, ref) (@1027C93)
        constexpr std::uintptr_t kQueueShow1st = 0xC4A2F0;     // TaskQueueInterface::QueueShow1stPerson(tq, bool) (SetMenuState @B19A03)
        using Show1st_t = void (*)(void*, bool);
        using IsFurnRef_t = bool (*)(void*);
        using IsWorkbench_t = bool (*)(void*, void*);
        using QueueShow1st_t = void (*)(void*, bool);
        Show1st_t      g_show1st = nullptr;
        IsFurnRef_t    g_isFurnitureRef = nullptr;
        IsWorkbench_t  g_isWorkbench = nullptr;
        QueueShow1st_t g_queueShow1st = nullptr;
        constexpr std::uintptr_t kShowCraftingMenu = 0x1086F30;  // UIUtils::ShowCraftingMenu(TESObjectREFR*) (SitOrSleep's call)
        using ShowCrafting_t = void (*)(void*);
        ShowCrafting_t g_showCrafting = nullptr;  // original (detour stub)
        std::uint64_t  g_workbenchDirectOpens = 0, g_workbenchClosesKept = 0;
        bool           g_directGuard = false;     // main thread: a directly opened crafting menu is up; the furniture exit's
                                                  // ShowCraftingMenu(nullptr) must not close it
        std::chrono::steady_clock::time_point g_directGuardStart{};
        bool g_directCloseSent = false;  // PollWorkbenchBody sent the hide for the directly opened menu
        // Round 6: with the game paused by the menu (bWorkbenchPause) the furniture activation that led to the direct open
        // is still pending when the menu closes; it resumes and calls StartFurnitureMode for the same bench again, which
        // reopened the menu (measured 14:06: close -> StartFurnitureMode 6625B 11 ms later -> second CookingMenu). A
        // re-entry for the bench just closed, within 3 s, is ignored (the sit then fails as before; player back to normal).
        RE::TESObjectREFR*                    g_directLastRef = nullptr;
        std::chrono::steady_clock::time_point g_directClosedAt{};
        std::uint64_t                         g_directReentriesIgnored = 0;
        // Actor::StopInteractingQuick(actor, quick, b, b) (CDBFA0; the failed sit's exit, which closes the crafting menu
        // from CDC286): run for the player when a directly opened menu closes, so the furniture activation left pending by
        // the pause is ended (sit state 0, furniture cleared) instead of resuming into the bench again.
        // TESObjectREFR::ReenableInputForPlayer(ref) (4FE860): PlayerCharacter::InitiateSitSleepPackage (D5D6B0) disables the
        // player's input with TESObjectREFR::DisableInputForPlayer(FURNITURE, "InitiateSitSleepPackage", 0x925): the layer is
        // kept in the FURNITURE's extra data. StopInteractingQuick re-enables it only for AIProcess::GetOccupiedFurniture,
        // which a sit that never started (our direct open) does not have; SetAIControlledPackage(false) re-enables the
        // player's own layer only. So the bench kept the layer: menu closed, player could not move (user, headset 15 Nov 18:46).
        constexpr std::uintptr_t kReenableInput = 0x4FE860;
        using Reenable_t = void (*)(void*);
        Reenable_t    g_reenableInput = nullptr;
        std::uint64_t g_benchInputReleases = 0;
        // PlayerCamera::Force1stPerson (1026AE0; Papyrus Game.ForceFirstPerson's native at 1129E90 jumps here)
        constexpr std::uintptr_t kForce1st = 0x1026AE0;
        using Force1st_t = void (*)(RE::PlayerCamera*);
        Force1st_t g_force1st = nullptr;
        constexpr std::uintptr_t kStopInteractingQuick = 0xCDBFA0;
        using StopInteract_t = void (*)(void*, bool, bool, bool);
        StopInteract_t g_stopInteract = nullptr;
        std::uint64_t  g_directStops = 0;
        // the pending activation is the player's AI-driven furniture package (sit state 3 = waiting for the sit animation,
        // frozen by the pause): StopInteractingQuick alone left it (measured: the next bench activation went to
        // StartFurnitureMode(6625B) again). Same calls StopInteractingQuick makes in its occupied-furniture branch:
        constexpr std::uintptr_t kClearRunOnce = 0xD346A0;   // AIProcess::ClearRunOncePackage(process, actor)
        constexpr std::uintptr_t kSetAiCtrlPkg = 0xD6F180;   // PlayerCharacter::SetAIControlledPackage(player, bool)
        constexpr std::uintptr_t kSetSitSleep = 0xD36460;    // AIProcess::SetSitSleepState(process, actor, state)
        using ClearRunOnce_t = void (*)(void*, void*);
        using SetAiCtrl_t = void (*)(void*, bool);
        using SetSit_t = void (*)(void*, void*, std::uint32_t);
        ClearRunOnce_t g_clearRunOnce = nullptr;
        SetAiCtrl_t    g_setAiCtrlPkg = nullptr;
        SetSit_t       g_setSitSleep = nullptr;

        void EndPendingFurniture(const char* a_why)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_stopInteract) {
                return;
            }
            g_stopInteract(player, true, false, false);
            // the bench's own input layer (see kReenableInput): released here, whatever state the sit reached
            if (g_reenableInput && g_directLastRef) {
                g_reenableInput(g_directLastRef);
                ++g_benchInputReleases;
            }
            if (g_clearRunOnce && g_setAiCtrlPkg && g_setSitSleep) {
                if (void* proc = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(player) + 0x300)) {
                    g_clearRunOnce(proc, player);
                    g_setSitSleep(proc, player, 0);
                }
                g_setAiCtrlPkg(player, false);
            }
            ++g_directStops;
            auto*      iem = RE::BSInputEnableManager::GetSingleton();
            const auto flags = iem ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(iem) + 0x118) : 0xFFFFFFFFu;
            REX::INFO("camdir gate: {} -> Actor::StopInteractingQuick(player) + bench {:08X} input layer {} (pending furniture "
                      "activation ended; input flags now {:X}, movement {})",
                a_why, g_directLastRef ? g_directLastRef->GetFormID() : 0, g_reenableInput ? "released" : "NOT released (helper missing)",
                flags, (flags & 1) ? "on" : "OFF");
        }
        // Round 6 (analysis/vr-workbenches.md): FO4VR's WorkbenchMenuBase ctor (VR 140BF9D90) sets the menu flags
        // |= 0x8040001 where flat (B26F00) sets |= 0x8040000: bit 0 = kPausesGame, so every crafting menu (Examine /
        // Cooking / PowerArmorMod / RobotMod derive from it) pauses the game in FO4VR. Same here: entry detour, original
        // first, then flags (+0x58) |= 1 before the UI pushes the menu (the pause counter is taken at the push).
        // [Menus] bWorkbenchPause.
        constexpr std::uintptr_t kWorkbenchCtor = 0xB26F00;  // WorkbenchMenuBase::WorkbenchMenuBase(this, HighlightMode)
        using WbCtor_t = void* (*)(void*, std::uint32_t);
        WbCtor_t      g_origWbCtor = nullptr;
        bool          g_workbenchPause = true;
        std::uint64_t g_workbenchPauseSet = 0;

        void* HkWorkbenchCtor(void* a_this, std::uint32_t a_mode)
        {
            void* r = g_origWbCtor(a_this, a_mode);
            if (g_workbenchPause && a_this) {
                *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(a_this) + 0x58) |= 1u;
                if (++g_workbenchPauseSet <= 20) {
                    REX::INFO("camdir: crafting menu (WorkbenchMenuBase mode {}) -> pauses the game like FO4VR (flags |= kPausesGame)", a_mode);
                }
            }
            return r;
        }

        bool DirectGuardActive(bool a_menuOpen)
        {
            if (!g_directGuard) {
                return false;
            }
            // the show message is queued: allow 2 s for the menu to appear, then guard while it is open
            if (a_menuOpen || std::chrono::steady_clock::now() - g_directGuardStart < std::chrono::seconds(2)) {
                return true;
            }
            g_directGuard = false;
            g_directClosedAt = std::chrono::steady_clock::now();  // menu gone (closed some other way): same re-entry window
            return false;
        }

        bool CraftingMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            for (const char* n : { "CookingMenu", "ExamineMenu", "PowerArmorModMenu", "RobotModMenu", "CraftingMenu" }) {
                if (ui->GetMenuOpen(n)) {
                    return true;
                }
            }
            return false;
        }

        void HkShowCrafting(void* a_ref)
        {
            if (!a_ref && g_directGuard) {
                if (DirectGuardActive(CraftingMenuOpen())) {
                    if (++g_workbenchClosesKept <= 20) {
                        REX::INFO("camdir gate: ShowCraftingMenu(close) from {:X} ignored (directly opened workbench menu stays)",
                            reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base());
                    }
                    return;
                }
                g_directGuard = false;
            }
            g_showCrafting(a_ref);
        }
        bool           g_workbenchBody = false;    // main thread: the third-person body was shown for a workbench
        bool           g_workbenchSeenSit = false;
        int            g_workbenchWaitPolls = 0;
        using ChangeState_t = void (*)(void*, int);
        using SetEnabled_t = void (*)(RE::PlayerCamera*, bool);
        using CullPlayer_t = void (*)(void*, bool, bool);
        ChangeState_t g_origVatsChangeState = nullptr;
        SetEnabled_t  g_camSetEnabled = nullptr;
        CullPlayer_t  g_queueCullPlayer = nullptr;
        std::uint8_t  g_camEnabledBefore = 0xFF, g_camEnabledAfter = 0xFF;  // TESCamera enabled byte around the hook (readout)

        std::string Hex(const std::uint8_t* p, std::size_t n)
        {
            std::string s;
            for (std::size_t i = 0; i < n; ++i) s += std::format("{:02X} ", p[i]);
            return s;
        }

        bool BytesAt(std::uintptr_t a_rva, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_rva);
            if (std::memcmp(p, a_expect, a_n) == 0) {
                return true;
            }
            REX::ERROR("camdir gate: {} at {:X} has bytes [{}], expected [{}]; skipped", a_what, a_rva, Hex(p, a_n), Hex(a_expect, a_n));
            return false;
        }

        // E8 rel32 at a_site (bytes checked) must land on a_target
        bool CallAt(std::uintptr_t a_site, std::uintptr_t a_target, const std::uint8_t (&a_bytes)[5], const char* a_what)
        {
            if (!BytesAt(a_site, a_bytes, 5, a_what)) {
                return false;
            }
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(Base() + a_site + 1), 4);
            if (static_cast<std::intptr_t>(a_site) + 5 + rel != static_cast<std::intptr_t>(a_target)) {
                REX::ERROR("camdir gate: {} call at {:X} targets {:X}, expected {:X}; skipped", a_what, a_site, a_site + 5 + rel, a_target);
                return false;
            }
            return true;
        }

        // Entry detour: prologue checked (a_n bytes), the first a_reloc bytes (whole, non-RIP-relative instructions) relocated
        // into a stub that jumps back; returns the stub (= callable original) or nullptr (nothing patched).
        void* Detour(std::uintptr_t a_rva, const std::uint8_t* a_pro, std::size_t a_n, std::size_t a_reloc, void* a_hook, const char* a_what)
        {
            if (!BytesAt(a_rva, a_pro, a_n, a_what)) {
                return nullptr;
            }
            auto&      tramp = REL::GetTrampoline();
            const auto addr = Base() + a_rva;
            auto*      stub = static_cast<std::uint8_t*>(tramp.allocate(a_reloc + 14));
            std::memcpy(stub, reinterpret_cast<const void*>(addr), a_reloc);
            stub[a_reloc] = 0xFF;
            stub[a_reloc + 1] = 0x25;
            std::memset(stub + a_reloc + 2, 0, 4);
            const std::uint64_t back = addr + a_reloc;
            std::memcpy(stub + a_reloc + 6, &back, 8);
            tramp.write_jmp<5>(addr, a_hook);
            return stub;
        }

        bool IsPowerArmorFurniture(RE::TESObjectREFR* a_ref)
        {
            if (!a_ref || !g_paKeyword) {
                return false;
            }
            auto* base = a_ref->data.objectReference;
            if (!base || base->GetFormType() != RE::ENUM_FORM_ID::kFURN) {
                return false;
            }
            auto* kw = g_paKeyword();
            // TESFurniture's IKeywordFormBase subobject sits at +0xD8 (BGSKeywordForm at +0xD0; TESFurniture::Activate 44CA40
            // calls [vtbl+8] = HasKeyword on +0xD8)
            const auto* kwf = reinterpret_cast<const RE::IKeywordFormBase*>(reinterpret_cast<const std::uint8_t*>(base) + 0xD8);
            return kw && kwf->HasKeyword(kw, nullptr);
        }

        void HkStartFurniture(RE::PlayerCamera* a_cam, RE::TESObjectREFR* a_ref)
        {
            const bool pa = IsPowerArmorFurniture(a_ref);
            bool       gate;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGFurniture].calls;
                if (pa) ++g_paFurniture;
                gate = (pa ? g_cfg.gatePowerArmor : g_cfg.gateFurniture) || g_cfg.lockFp;
                if (gate) {
                    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
                    const auto* cc = reinterpret_cast<const std::uintptr_t*>(base + 0x327A8A8);
                    if (cc[0] || cc[1]) {
                        gate = false;
                        REX::INFO("camdir gate: StartFurnitureMode during character creation - the game's camera (mirror) is kept");
                    }
                }
                if (gate) ++g_gs[kGFurniture].blocked;
                if (gate && g_cfg.lockFp) g_refused = pa ? "power armour camera refused" : "furniture camera refused";
                g_lastGate = std::format("StartFurnitureMode {:08X} pa={} {}", a_ref ? a_ref->GetFormID() : 0, pa ? 1 : 0,
                    gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: StartFurnitureMode ref {:08X} (power armour {}) -> {}", a_ref ? a_ref->GetFormID() : 0, pa,
                gate ? "HUD mode only (first person kept)" : "original");
            if (!gate) {
                g_origStartFurn(a_cam, a_ref);
                return;
            }
            // FO4VR's StartFurnitureMode: PushHUDMode(FurnitureEnterExit) only - no camera state, no Show1stPerson(false).
            // Same preconditions as the original's HUD push, so StopFurnitureMode's PopHUDMode stays balanced.
            auto*       c = reinterpret_cast<std::uint8_t*>(a_cam);
            void* const cur = *reinterpret_cast<void**>(c + 0x28);
            void* const animated = *reinterpret_cast<void**>(c + 0x118);
            // Workbenches: the crafting menu normally opens from AIProcess::SitOrSleep (D34950) once PlayFurnitureAnimation
            // succeeds, and that animation needs the third-person body the original's camera state 8 brings (FO4VR's player
            // always runs that graph). Open it directly instead (UIUtils::ShowCraftingMenu 1086F30, the same call SitOrSleep
            // makes), no HUD-mode push (no furniture exit handler will pop it); the sit then fails on the first-person graph
            // and the AI returns the player to normal (measured ~4 s). [Camera] bWorkbenchDirectMenu.
            if (a_ref && g_cfg.workbenchDirect && g_showCrafting && g_isFurnitureRef && g_isWorkbench && g_isFurnitureRef(a_ref)) {
                void* base = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(a_ref) + 0xE0);
                if (base && g_isWorkbench(base, a_ref)) {
                    if (a_ref == g_directLastRef && !g_directGuard &&
                        std::chrono::steady_clock::now() - g_directClosedAt < std::chrono::seconds(3)) {
                        ++g_directReentriesIgnored;
                        REX::INFO("camdir gate: workbench {:08X} re-entered right after its menu closed (pending activation resumed "
                                  "after the pause) -> ignored, menu not reopened", a_ref->GetFormID());
                        EndPendingFurniture("workbench re-entry");
                        return;
                    }
                    g_directLastRef = a_ref;
                    g_directGuard = true;
                    g_directCloseSent = false;
                    g_directGuardStart = std::chrono::steady_clock::now();
                    g_showCrafting(a_ref);
                    ++g_workbenchDirectOpens;
                    REX::INFO("camdir gate: workbench {:08X} -> crafting menu opened directly (first person, no furniture animation)",
                        a_ref->GetFormID());
                    return;
                }
            }
            if (a_ref && !g_isInFurniture(a_cam) && !(cur && cur == animated)) {
                g_pushHudMode(g_qFurnEnterExit());
            }
            if (pa) {
                g_paRequest = true;
            }
            // Workbenches (cooking, chemistry, weapon/armour benches, PA station): the crafting menu opens only from
            // AIProcess::SitOrSleep (D34950) once PlayFurnitureAnimation succeeds (-> UIUtils::ShowCraftingMenu 1086F30),
            // and that animation needs the third-person body + graph, which the original StartFurnitureMode gets by
            // switching the camera to state 8 (FO4VR's player always runs that graph, so it needs no camera change).
            // Here: the body swap only (PlayerCharacter::Show1stPerson(false), as the original's non-workbench branch),
            // the first-person head-tracked camera stays; PollWorkbenchBody swaps back when the player stands up.
            if (a_ref && g_cfg.gateWorkbenchBody && g_show1st && g_isFurnitureRef && g_isWorkbench && g_isFurnitureRef(a_ref)) {
                void* base = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(a_ref) + 0xE0);
                if (base && g_isWorkbench(base, a_ref)) {
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        g_show1st(player, false);
                        g_workbenchBody = true;
                        g_workbenchSeenSit = false;
                        REX::INFO("camdir gate: workbench {:08X} -> third-person body for the furniture animation, first-person camera kept",
                            a_ref->GetFormID());
                    }
                }
            }
        }

    }  // namespace (reopened below)

    void PollWorkbenchBody()
    {
        if (g_directGuard) {
            // Exit confirmed ("Exit Workbench?" -> ExamineMenu ExitCallback 140A71130 sets WorkbenchMenuBase+0x2F1): the
            // original then waits for the furniture exit to close the menu, which never comes without the sit (measured
            // round 4: the prompt closed, CookingMenu stayed). Close it ourselves (UIMessageQueue hide, as Call case 6
            // does in inspect mode) and stop guarding.
            if (auto* ui = RE::UI::GetSingleton(); ui && !g_directCloseSent) {
                for (const char* n : { "CookingMenu", "ExamineMenu", "PowerArmorModMenu", "RobotModMenu", "CraftingMenu" }) {
                    if (!ui->GetMenuOpen(n)) {
                        continue;
                    }
                    const auto m = ui->GetMenu(RE::BSFixedString(n));
                    if (m && *(reinterpret_cast<const std::uint8_t*>(m.get()) + 0x2F1) != 0) {
                        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                            q->AddMessage(RE::BSFixedString(n), RE::UI_MESSAGE_TYPE::kHide);
                            g_directCloseSent = true;
                            g_directGuard = false;
                            g_directClosedAt = std::chrono::steady_clock::now();
                            REX::INFO("camdir gate: directly opened {} - exit confirmed -> closed (no furniture exit to wait for)", n);
                            EndPendingFurniture("crafting menu closed");
                        }
                        break;
                    }
                }
            }
            if (g_directGuard) {
                DirectGuardActive(CraftingMenuOpen());  // clears it once the menu is gone
            }
        }
        if (!g_workbenchBody) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const auto st = static_cast<const RE::ActorState*>(player)->DoGetSitSleepState();
        if (st != RE::SIT_SLEEP_STATE::kNormal) {
            g_workbenchSeenSit = true;
            return;
        }
        if (!g_workbenchSeenSit) {
            if (++g_workbenchWaitPolls < 600) {  // ~5-10 s for the sit to start; else give the body back anyway
                return;
            }
        }
        g_workbenchBody = false;
        g_workbenchWaitPolls = 0;
        if (auto* tq = *reinterpret_cast<void**>(Base() + kTaskQueueSingleton); tq && g_queueShow1st) {
            g_queueShow1st(tq, true);
        }
        REX::INFO("camdir gate: workbench left (sit state normal{}) -> first-person body back", g_workbenchSeenSit ? "" : ", sit never started");
    }

    namespace
    {

        void HkStartAnimated(RE::PlayerCamera* a_cam, const char* a_node, bool a_b)
        {
            bool gate;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGAnimated].calls;
                gate = g_cfg.gateAnimated || g_cfg.lockFp;
                if (gate) ++g_gs[kGAnimated].blocked;
                if (gate && g_cfg.lockFp) g_refused = "animated camera refused";
                g_lastGate = std::format("StartAnimatedMode '{}' {}", a_node ? a_node : "", gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: StartAnimatedMode node '{}' {} -> {}", a_node ? a_node : "", a_b, gate ? "skipped" : "original");
            if (!gate) {
                g_origStartAnim(a_cam, a_node, a_b);
            }
        }

        // TESCamera::enabled (PlayerCamera+0x30, CommonLibF4 layout), written by TESCamera::SetEnabled; readout only.
        std::uint8_t CamEnabledByte()
        {
            auto* pc = RE::PlayerCamera::GetSingleton();
            return pc ? *(reinterpret_cast<const std::uint8_t*>(pc) + 0x30) : 0xFF;
        }

        void HkVatsChangeState(void* a_gs, int a_state)
        {
            g_origVatsChangeState(a_gs, a_state);
            if (a_state != 2) {
                return;
            }
            bool keep;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGVatsState].calls;
                keep = g_cfg.vatsKeepCamera && g_cfg.gateVats;
                if (keep) ++g_gs[kGVatsState].blocked;
                g_camEnabledBefore = CamEnabledByte();
            }
            if (keep) {
                if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                    g_camSetEnabled(pc, true);
                }
                if (auto* tq = *reinterpret_cast<void**>(Base() + kTaskQueueSingleton)) {
                    g_queueCullPlayer(tq, false, false);
                }
            }
            g_camEnabledAfter = CamEnabledByte();
            // VATS over the live world: no flat VATS image-space modifier. SetMenuState(2) (B18CD0) triggered
            // DFOB VATSMenuImageSpaceModifier = IMAD 00024590 'VATSScanImod' (tint + radial blur 0.1 centred on the
            // screen = on the eye seam of the double-wide stereo frame: the "half zoom / warp") into VatsMenuObj+0x1240
            // just before this ChangeState(2). FO4VR has no such imod (its VR slow-time imods instead).
            // Also in the VATS window (no gate): the radial blur is a comfort problem there too (user: keep it stopped).
            bool imodStopped = false;
            if (g_vatsNoImod && g_imodInstStop) {
                auto* obj = static_cast<std::uint8_t*>(a_gs) - kVatsGameStateInObj;
                if (void* inst = *reinterpret_cast<void**>(obj + kVatsImodInstInObj)) {
                    g_imodInstStop(inst);
                    imodStopped = true;
                }
            }
            REX::INFO("camdir gate: VATS ChangeState(2) -> {}{} (camera enabled byte {} -> {})",
                keep ? "SetEnabled(true) + QueueCullPlayerGeometry(false,false)" : "original only", imodStopped ? " + VATSScanImod stopped" : "",
                g_camEnabledBefore, g_camEnabledAfter);
        }

        // The player is being put into an AAF scene. AAF's lock runs after its walk (applyWalkQuest: Game.SetPlayerAIDriven() +
        // a travel package override), so the player is AI-driven there: MovementControllerNPC +0x1A1 = 0 (SetAIDriven DC8540
        // clears it, SetControlsDriven sets it; the same byte Roomscale's gate reads). The AAF_ActorBusy keyword test of the
        // 17:35 build never matched (HasKeywordHelper is not the reference's AddKeyword list: every lock in the 17:38 session
        // logged "Force3rdPerson -> skipped"); it is kept as a second signal.
        bool PlayerAafBusy()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            if (auto* mc = player->movementController.get()) {
                if (*reinterpret_cast<const std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(mc) + 0x1A1) == 0) {
                    return true;  // AI-driven player (scripted walk / scene)
                }
            }
            // 22 Nov (VATS head agent): only in the world. At the main menu the player has no cell / 3D and HasKeywordHelper
            // read garbage (crash 0xC0000005 read 0x137C6 at Fallout4.exe+CA8B41 from PollAftercare 8 s after launch, twice,
            // whenever AAF.esm's keyword was found before the first load).
            if (!player->parentCell || !player->Get3D()) {
                return false;
            }
            // the keyword is looked up once (LookupForm walks the plugin list; this runs every camera tick now)
            static std::atomic<RE::BGSKeyword*> s_kw{ nullptr };
            static std::atomic<int>             s_tries{ 0 };
            RE::BGSKeyword* kw = s_kw.load(std::memory_order_relaxed);
            if (!kw && s_tries.load(std::memory_order_relaxed) < 5) {
                ++s_tries;
                auto* dh = RE::TESDataHandler::GetSingleton();
                kw = dh ? dh->LookupForm<RE::BGSKeyword>(0x00915A, "AAF.esm") : nullptr;
                s_kw = kw;
            }
            return kw && player->HasKeywordHelper(kw, nullptr);
        }

        // ---- AAF / defeat scenes under the camera locks (20 Nov late, user: "AAF fails defeat scenes when third person is
        // blocked") ----
        // Defeat mods (AAF Violate FPV_OnHit Surrender: InputEnableLayer.DisablePlayerControls, wait while IsBleedingOut,
        // then "If Game.GetCameraState() == 0: Game.ForceThirdPerson()", then the AAF scene) ask for third person AFTER the
        // player's bleedout and BEFORE AAF's walk makes the player AI-driven: neither AAF signal holds yet. So a bleedout
        // (the player's life state essential-down / bleedout, or a StartBleedoutMode) opens a defeat window of
        // fDefeatWindowS in which a script's third person passes the blocks. User log 20 Nov 22:09 / 22:44: StartBleedoutMode
        // and Force3rdPerson refused by bLockFirstPerson -> the scene never started.
        std::atomic<std::int64_t> g_bleedoutSeenMs{ 0 };  // steady clock ms of the last bleedout signal (0 = never)
        std::int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
        bool PlayerBleedingOut()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;
            const auto ls = static_cast<const RE::ActorState*>(player)->lifeState;  // 7 essential down, 8 bleedout
            return ls == 7 || ls == 8;
        }
        void NoteBleedout() { g_bleedoutSeenMs = NowMs(); }
        // a scene / defeat flow holds (g_lock held or not: no director state read)
        bool SceneFlow(float a_defeatWindowS, const char** a_why = nullptr)
        {
            const char* why = nullptr;
            if (PlayerBleedingOut()) {
                NoteBleedout();
                why = "bleedout";
            } else if (PlayerAafBusy()) {
                why = "AI-driven / AAF-busy";
            } else {
                const auto seen = g_bleedoutSeenMs.load();
                if (seen && a_defeatWindowS > 0.0f && NowMs() - seen < static_cast<std::int64_t>(a_defeatWindowS * 1000.0f)) {
                    why = "defeat window after a bleedout";
                }
            }
            if (a_why) *a_why = why ? why : "-";
            return why != nullptr;
        }
        // a block (bBlockThirdPerson / bLockFirstPerson) lets this camera request through for an AAF / defeat scene (g_lock held)
        bool ScenePassLocked(const char** a_why)
        {
            const bool on = g_cfg.lockFp ? g_cfg.lockScenes : g_cfg.freeAafThird;
            return on && (g_cfg.lockFp || FreeOn()) && SceneFlow(g_cfg.defeatWindowS, a_why);
        }

        template <int G>
        void HkCamFn(RE::PlayerCamera* a_cam)
        {
            bool    gate = false;
            CamFn_t orig = nullptr;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[G].calls;
                if constexpr (G == kGSet3rd || G == kGForce3rd) {
                    gate = g_cfg.blockThirdPerson || g_cfg.lockFp;
                    orig = G == kGSet3rd ? g_origSet3rd : g_origForce3rd;
                    if constexpr (G == kGForce3rd) {
                        // AAF locks the player with Game.ForceThirdPerson() then the fly cam (AAF_MainQuestScript DoLockActor,
                        // after AddKeyword(AAF_ActorBusy)). Gated, the player stayed in the first-person body and behaviour graph
                        // until our late Show1stPerson(false) on the free-camera entry - after AAF's first PlayIdle - and the
                        // player's later idles did not take (user, headset 15 Nov 17:16-17:23; aaf_visibility.log shown3rd=0 at the
                        // first idle). Let AAF's own call through, as in the flat game: state 8 fades (fThirdPersonGraceMs), then 3.
                        // 20 Nov late: the defeat flow too (bleedout / defeat window, see SceneFlow), and under bLockFirstPerson
                        // with bLockAllowScenes (the scene's views are SCENE / the body camera; the lock's backstop waits).
                        const char* why = "-";
                        if (gate && ScenePassLocked(&why)) {
                            gate = false;
                            ++g_d.aafThirdPassed;
                            ++g_d.scenePasses;
                            g_d.lastScenePass = std::format("Force3rdPerson ({})", why);
                            REX::INFO("camdir gate: Force3rdPerson during an AAF / defeat scene ({}) -> passed ({})", why,
                                g_cfg.lockFp ? "bLockAllowScenes" : "bFreeCamAAFThirdPerson");
                        }
                    }
                    if (gate && g_cfg.lockFp) g_refused = "third person refused";
                } else if constexpr (G == kGStartVats || G == kGForceVats) {
                    gate = g_cfg.gateVats;
                    orig = G == kGStartVats ? g_origStartVats : g_origForceVats;
                    if (gate) g_vatsBlocked = true;
                } else {  // StopVATSMode: skip only when our gate kept the VATS camera out and it is not the current state
                    auto*       c = reinterpret_cast<std::uint8_t*>(a_cam);
                    void* const cur = *reinterpret_cast<void**>(c + 0x28);
                    void* const vats = *reinterpret_cast<void**>(c + 0xF0);
                    gate = g_cfg.gateVats && g_vatsBlocked && cur != vats;
                    g_vatsBlocked = false;
                    orig = g_origStopVats;
                }
                if (gate) ++g_gs[G].blocked;
                g_lastGate = std::format("{} {}", kGateNames[G], gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: {} -> {}", kGateNames[G], gate ? "skipped" : "original");
            if (!gate) {
                orig(a_cam);
                if constexpr (G == kGForce3rd) {
                    g_thirdArmReq = true;  // a script's third person (Game.ForceThirdPerson): bThirdPersonReturn may undo it
                } else if constexpr (G == kGSet3rd) {
                    g_thirdArmReq = false;  // the player's own POV toggle: kept
                    std::scoped_lock l(g_lock);
                    g_d.thirdArmed = false;
                }
            }
        }

        // 20 Nov, first-person lock: the free camera (entering only), bleedout and mount cameras
        using ToggleFree_t = void (*)(RE::PlayerCamera*, bool);
        using StartMount_t = void (*)(RE::PlayerCamera*, void*);
        ToggleFree_t g_origToggleFree = nullptr;
        CamFn_t      g_origStartBleed = nullptr;
        StartMount_t g_origStartMount = nullptr;

        void HkToggleFreeCam(RE::PlayerCamera* a_cam, bool a_b)
        {
            auto*       c = reinterpret_cast<std::uint8_t*>(a_cam);
            void* const cur = *reinterpret_cast<void**>(c + 0x28);
            void* const freeSt = *reinterpret_cast<void**>(c + 0xF8);  // cameraStates[3]
            const bool  entering = !cur || cur != freeSt;
            bool        gate;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGFreeCam].calls;
                gate = entering && g_cfg.lockFp;
                const char* why = "-";
                if (gate && ScenePassLocked(&why)) {  // AAF's fly cam during its scene (bLockAllowScenes)
                    gate = false;
                    ++g_d.scenePasses;
                    g_d.lastScenePass = std::format("ToggleFreeCameraMode ({})", why);
                    REX::INFO("camdir gate: free camera during an AAF / defeat scene ({}) -> passed (bLockAllowScenes)", why);
                }
                if (gate) {
                    ++g_gs[kGFreeCam].blocked;
                    g_refused = "free camera refused";
                }
                g_lastGate = std::format("ToggleFreeCameraMode {} {}", entering ? "enter" : "leave", gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: ToggleFreeCameraMode ({}) -> {}", entering ? "enter" : "leave", gate ? "skipped (first-person lock)" : "original");
            if (!gate) {
                g_origToggleFree(a_cam, a_b);
            }
        }

        void HkStartBleedout(RE::PlayerCamera* a_cam)
        {
            bool gate;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGBleedout].calls;
                NoteBleedout();  // opens the defeat window (SceneFlow)
                // the bleedout camera is the first step of a defeat flow: under bLockFirstPerson it passes with bLockAllowScenes
                // (shown SCENE / on the body; the lock's backstop waits while the flow lasts)
                gate = g_cfg.lockFp && !g_cfg.lockScenes;
                if (g_cfg.lockFp && !gate) {
                    ++g_d.scenePasses;
                    g_d.lastScenePass = "StartBleedoutMode (defeat flow)";
                }
                if (gate) {
                    ++g_gs[kGBleedout].blocked;
                    g_refused = "bleedout camera refused";
                }
                g_lastGate = std::format("StartBleedoutMode {}", gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: StartBleedoutMode -> {}", gate ? "skipped (first-person lock)" : "original");
            if (!gate) {
                g_origStartBleed(a_cam);
            }
        }

        void HkStartMount(RE::PlayerCamera* a_cam, void* a_mount)
        {
            bool gate;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGMount].calls;
                gate = g_cfg.lockFp;
                if (gate) {
                    ++g_gs[kGMount].blocked;
                    g_refused = "mount camera refused";
                }
                g_lastGate = std::format("StartMountMode {}", gate ? "gated" : "passed");
            }
            REX::INFO("camdir gate: StartMountMode -> {}", gate ? "skipped (first-person lock)" : "original");
            if (!gate) {
                g_origStartMount(a_cam, a_mount);
            }
        }

        // body camera applies to this state (g_lock held): every state but first person / iron sights (already on the body)
        // and VATS (the VATS window keeps the flat VATS experience)
        // 21 Nov: except the free camera while it sits at another actor's head (AAF POV on an NPC, bBodyCameraYieldsToPov):
        // that is AAF's chosen view, shown as SCENE (AAF's POV point + head motion)
        bool BodyAppliesLocked(int a_st)
        {
            return g_d.installed && g_cfg.enabled && g_cfg.bodyCam && a_st >= 0 && a_st < kStates && a_st != 0 && a_st != 4 && a_st != 2 &&
                   !(a_st == kStateFree && g_cfg.bodyYieldPov && g_d.bodyYield);
        }

        // ---- animated Head bone (21 Nov) ----
        // The actor's animated "Head" bone, world: the flattened bone tree's own entry (the body skeleton bone). A by-name
        // search can land on the facebones rig's static duplicate 'HEAD' after a 3D rebuild (AAF CameraCaller.cpp
        // FindHeadWorld: the camera froze at the root while the animation continued); it stays as the fallback.
        RE::BSFlattenedBoneTree* BoneTreeOf(RE::NiAVObject* a_root)
        {
            if (!a_root) return nullptr;
            if (auto* t = netimmerse_cast<RE::BSFlattenedBoneTree*>(a_root)) return t;
            if (auto* n = a_root->IsNode()) {
                for (auto& ch : n->children) {
                    if (auto* t = ch ? netimmerse_cast<RE::BSFlattenedBoneTree*>(ch.get()) : nullptr) return t;
                }
            }
            return nullptr;
        }

        const RE::NiTransform* HeadBoneWorld(RE::NiAVObject* a_root)
        {
            if (!a_root) return nullptr;
            static const RE::BSFixedString kHead("Head");
            if (auto* tree = BoneTreeOf(a_root); tree && tree->bone && tree->boneCount > 0) {
                const auto it = tree->boneMap.find(kHead);
                if (it != tree->boneMap.end() && it->second >= 0 && it->second < tree->boneCount) {
                    return &tree->bone[it->second].world;
                }
            }
            if (auto* node = a_root->GetObjectByName(kHead)) return &node->world;
            return nullptr;
        }

        // The player's third-person Head bone, cached per 3D root (camera tick, g_lock held). Read in the director tick: that
        // runs inside PlayerCamera::UpdateCamera (call 1026035) / the free-camera thunk, i.e. in Main::OnIdle_UpdatePlayer
        // AFTER PlayerCharacter::UpdateAnimation (C35018: graph -> bone locals) and PlayerCharacter::UpdateScenegraph
        // (C35076: NiAVObject::Update of the third-person 3D, root offset included) on the same job thread: this frame's
        // final pose, no lag, no race. (Update3rdPSceneGraph C3512F after the camera re-runs the same pose; FRIK-flat's
        // frame at C351F3 leaves the body to the engine outside first person / iron sights.)
        struct BodyHeadCache
        {
            RE::NiPointer<RE::NiAVObject> root, tree;
            int                           idx{ -1 };
            const char*                   name{ nullptr };  // the pooled name at idx (a rebuilt tree changes it)
        };
        BodyHeadCache g_bh;

        bool PlayerHeadWorld(RE::PlayerCharacter* a_player, RE::NiTransform& a_out)
        {
            auto* root = a_player ? static_cast<RE::TESObjectREFR*>(a_player)->Get3D(false) : nullptr;  // third-person 3D
            if (!root) {
                g_bh = {};
                return false;
            }
            auto* tree = static_cast<RE::BSFlattenedBoneTree*>(g_bh.tree.get());
            const bool valid = g_bh.root.get() == root && tree && g_bh.idx >= 0 && g_bh.idx < tree->boneCount && tree->bone &&
                               tree->bone[g_bh.idx].name.c_str() == g_bh.name;
            if (!valid) {
                g_bh = {};
                g_bh.root.reset(root);
                if (auto* t = BoneTreeOf(root); t && t->bone && t->boneCount > 0) {
                    static const RE::BSFixedString kHead("Head");
                    const auto it = t->boneMap.find(kHead);
                    if (it != t->boneMap.end() && it->second >= 0 && it->second < t->boneCount) {
                        g_bh.tree.reset(t);
                        g_bh.idx = it->second;
                        g_bh.name = t->bone[it->second].name.c_str();
                        tree = t;
                    }
                }
                if (g_bh.idx < 0) {  // no flattened tree: by name (uncached)
                    const auto* w = HeadBoneWorld(root);
                    if (!w) return false;
                    a_out = *w;
                    return true;
                }
            }
            a_out = tree->bone[g_bh.idx].world;
            return true;
        }

        // fade clock (g_lock held)
        void Advance(Clock::time_point now)
        {
            Fade& f = g_f;
            const double dt = f.lastAdvance.time_since_epoch().count() ? Ms(f.lastAdvance, now) : 0.0;
            f.lastAdvance = now;
            const float rate = g_cfg.fadeMs > 1.0f ? static_cast<float>(dt / g_cfg.fadeMs) : 1.0f;
            const float prev = f.alpha;
            f.alpha = std::clamp(f.alpha + (f.target ? rate : -rate), 0.0f, 1.0f);
            if (prev < 1.0f && f.alpha >= 1.0f && (f.active || f.test)) {
                f.black = now;
                f.blackSeen = true;
                REX::INFO("camdir: fade black after {:.0f} ms", Ms(f.start, now));
            }
            if (prev > 0.0f && f.alpha <= 0.0f && f.inRunning) {
                f.inRunning = false;
                f.lastInMs = Ms(f.inStart, now);
                REX::INFO("camdir: fade clear: out {:.0f} ms, black held {:.0f} ms, in {:.0f} ms ({})", f.lastOutMs, f.lastHoldMs, f.lastInMs,
                    f.lastEpisode);
            }
        }

        void BeginFadeIn(Clock::time_point now, const char* a_why)
        {
            Fade& f = g_f;
            f.lastOutMs = f.blackSeen ? Ms(f.start, f.black) : -1.0;
            f.lastHoldMs = f.blackSeen ? Ms(f.black, now) : 0.0;
            f.inStart = now;
            f.inRunning = f.alpha > 0.0f;
            f.lastEpisode = std::format("{} -> {} {}", StateName(f.fromState), StateName(f.causeState), a_why);
            REX::INFO("camdir: fade in start ({}), alpha {:.2f}, out {:.0f} ms, black held {:.0f} ms", f.lastEpisode, f.alpha, f.lastOutMs,
                f.lastHoldMs);
        }

        V3 RawOffsetWorld(float a_yaw)
        {
            head::Vec3 o;
            if (!head::GetOffset(o)) {
                return {};
            }
            const float u = g_cfg.unitsPerMeter, c = std::cos(a_yaw), s = std::sin(a_yaw);
            const float x = o.x * u, y = o.y * u;
            return { x * c + y * s, -x * s + y * c, o.z * u };  // heading convention: right -> (cos, -sin), fwd -> (sin, cos)
        }

        void Tick(RE::PlayerCamera* cam)
        {
            const auto now = Clock::now();
            std::scoped_lock l(g_lock);
            Dir&  d = g_d;
            Fade& f = g_f;
            ++d.ticks;
            const int st = cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1;
            Policy pol = !g_cfg.enabled ? (st == 0 ? Policy::kTrack : Policy::kCinema)
                                        : (st >= 0 && st < kStates ? g_cfg.table[st] : Policy::kCinema);
            const bool freeScene = g_cfg.enabled && FreeOn();  // bFreeCamScene and the thunk patched
            // bBodyCamera: this state is shown from the first-person eye on the body (TRACK), whatever its policy
            const bool body = BodyAppliesLocked(st);
            if (body) pol = Policy::kTrack;
            d.body = body;
            auto       sceneOf = [&](int a_st) {
                return g_cfg.enabled && a_st >= 0 && a_st < kStates && d.sceneSt[a_st] && !BodyAppliesLocked(a_st);
            };
            // SCENE (the AAF free-camera method for every game camera without its own handling): this state is head-tracked
            // from the game camera's position and yaw
            const bool scene = sceneOf(st);
            const bool changed = st != d.state;
            // AAF / defeat scene in progress (bLockAllowScenes: the lock's backstop waits; SceneFlow). The camera state itself
            // counts while the lock let it through: the free camera / bleedout with a scene pass seen.
            // 10 Hz (keyword test on the player), every tick on a state change; first person without the lock skips it
            if (st == 11) NoteBleedout();
            if ((g_cfg.lockFp || st != 0) && (changed || !d.flowCheckAt.time_since_epoch().count() || Ms(d.flowCheckAt, now) >= 100.0)) {
                d.flowCheckAt = now;
                d.sceneFlowNow = SceneFlow(g_cfg.defeatWindowS);
            } else if (!g_cfg.lockFp && st == 0) {
                d.sceneFlowNow = false;
            }
            if (d.sceneFlowNow) d.sceneFlowSeen = now;
            // free camera stick yaw: a scene-lifetime offset, dropped when the view leaves the SCENE states (first person ...)
            if (changed && !scene && d.stickYaw != 0.0f) {
                REX::INFO("camdir: free-camera stick yaw {:.1f} deg dropped (state {} {})", d.stickYaw * kR2D, st, StateName(st));
                d.stickYaw = 0.0f;
            }
            // 21 Nov: the body camera <-> SCENE within one state (bBodyCameraYieldsToPov, a live bBodyCamera toggle) is a scene
            // entry too (head-motion base reset: the view sits at the game camera's point, e.g. AAF's POV head)
            if (!changed && scene && !d.lastScene) {
                d.freeEntry = true;
                d.sceneKeepHead = false;
                d.havePrevGy = false;
                d.glideFrames = 0;
                ++d.sceneEntries;
            }
            d.lastScene = scene;
            if (changed) {
                ++d.transitions;
                d.prevState = d.state;
                d.state = st;
                d.stateSince = now;
                if (scene) {
                    // pose block: continuity yaw offset; the head-motion base is reset only when coming from a non-SCENE
                    // state (between SCENE states, e.g. third person -> free camera, the view stays continuous)
                    d.freeEntry = true;
                    d.sceneKeepHead = sceneOf(d.prevState) && d.haveFreeOffBase;
                    d.havePrevGy = false;
                    d.glideFrames = 0;
                    ++d.sceneEntries;
                }
                if (scene && st == kStateFree && (!g_cfg.lockFp || (g_cfg.lockScenes && d.sceneFlowNow))) {
                    // AAF with third person gated (bBlockThirdPerson) goes 0 -> 3 directly: the player keeps the first-person
                    // body, so the scene's animation would not be seen. Show the third-person body (Show1stPerson queues
                    // through the TaskQueueInterface; FirstPersonState::Begin 1020430 calls Show1stPerson(true) when the
                    // game returns to first person, so no restore is needed here).
                    if (g_cfg.freeBody && g_show1st && (d.prevState == 0 || d.prevState == 4)) {
                        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                            g_show1st(player, false);
                            ++d.bodyShows;
                            REX::INFO("camdir: free camera entered from first person: third-person body shown");
                        }
                    }
                }
                if (freeScene && st == kStateThird && g_cfg.thirdGraceMs > 0.0f) {
                    ++d.thirdGraces;
                }
            }
            // AAF locks the player with Force3rdPerson then the fly cam (0 -> 8 -> 3) and unlocks 3 -> 8 -> 0: the brief
            // third-person state fades instead of flashing the flat screen; a third person that stays goes to its own
            // policy (CINEMA) after fThirdPersonGraceMs (the fade then comes back in on the flat screen).
            if (freeScene && st == kStateThird && pol == Policy::kCinema && g_cfg.thirdGraceMs > 0.0f &&
                Ms(d.stateSince, now) < g_cfg.thirdGraceMs) {
                pol = Policy::kFadeSnap;
            }
            // a view-taking menu in the flat window: no fade-to-black hold (it would sit over / behind the window)
            const bool menuWin = g_menuWindow.load();
            if (menuWin && pol == Policy::kFadeSnap) {
                pol = Policy::kCinema;
            }
            if (changed) {
                REX::INFO("camdir: state {} ({}) -> {} ({}), policy {} -> {}", d.prevState, StateName(d.prevState), st, StateName(st),
                    PolicyName(d.policy), PolicyName(pol));
            }
            d.policy = pol;
            Advance(now);

            // ---- return to first person (bThirdPersonReturn) ----
            {
                if (g_thirdArmReq.exchange(false)) d.thirdArmed = true;            // Force3rdPerson (scripts: AAF lock / unlock ...)
                if (changed && st == kStateThird && d.prevState == kStateFree) d.thirdArmed = true;  // free camera ended in 3rd person
                if (changed) d.thirdQueued = false;
                auto*      player = RE::PlayerCharacter::GetSingleton();
                const auto* mc = player ? player->movementController.get() : nullptr;
                const bool ctrlDriven = mc && *(reinterpret_cast<const std::uint8_t*>(mc) + 0x1A1) == 1;
                const int  sit = player ? static_cast<int>(static_cast<RE::ActorState*>(player)->DoGetSitSleepState()) : -1;
                if (st != kStateThird || !ctrlDriven || sit != 0 || menuWin) d.thirdCtrlSince = now;
                // first person held (no menu) for 3 s: the script third person is over (a Pip-Boy opened from third person shows
                // first person and returns to 8 on close, so a short first person must not disarm)
                if (st != 0 || menuWin) d.fpQuietSince = now;
                if (d.thirdArmed && st == 0 && Ms(d.fpQuietSince, now) > 3000.0) d.thirdArmed = false;
                if (g_cfg.thirdReturn && g_force1st && d.thirdArmed && !d.thirdQueued && st == kStateThird &&
                    Ms(d.thirdCtrlSince, now) >= g_cfg.thirdReturnS * 1000.0f) {
                    d.thirdQueued = true;
                    ++d.thirdReturns;
                    d.lastReturn = std::format("after {:.0f} ms in third person (from {})", Ms(d.stateSince, now), StateName(d.prevState));
                    REX::INFO("camdir: script third person, player controls-driven again for {:.1f} s -> PlayerCamera::Force1stPerson "
                              "(bThirdPersonReturn; {})", g_cfg.thirdReturnS, d.lastReturn);
                    // main-idle queue (ThreadQueues.h): this runs inside TESCamera::Update (a job thread while the game runs)
                    tq::RunOnMain([]() {
                        auto* pc = RE::PlayerCamera::GetSingleton();
                        if (pc && pc->currentState && static_cast<int>(pc->currentState->id.get()) == kStateThird && g_force1st) {
                            g_force1st(pc);
                        }
                    });
                }
            }

            // ---- first-person lock backstop (bLockFirstPerson): a non-first-person state no gate caught (a save loaded in
            // third person, a mod's own PushState, a state the lock was switched on in) -> Force1stPerson; VATS excepted
            // 20 Nov late (bLockAllowScenes): not while an AAF / defeat scene runs, nor for 2 s after its last signal (AAF's unlock
            // goes 3 -> 8 -> 0 itself; Violate's surrender asks for third person seconds before AAF's walk). User log 22:44:06: the
            // backstop pulled the camera out of a running AAF scene.
            const bool sceneHold = g_cfg.lockScenes && d.sceneFlowSeen.time_since_epoch().count() && Ms(d.sceneFlowSeen, now) < 2000.0;
            if (g_cfg.lockFp && sceneHold && g_force1st && st >= 0 && st != 0 && st != 2 && st != 4 && changed) {
                ++d.lockForceSkips;
                REX::INFO("camdir: first-person lock: camera state {} {} kept for the AAF / defeat scene (bLockAllowScenes)", st, StateName(st));
            }
            if (g_cfg.lockFp && !sceneHold && g_force1st && st >= 0 && st != 0 && st != 2 && st != 4 &&
                (changed || Ms(d.lockForceAt, now) >= 500.0)) {
                d.lockForceAt = now;
                ++d.lockForces;
                d.lockLast = std::format("{} ({}) after {:.0f} ms", st, StateName(st), Ms(d.stateSince, now));
                if (d.lockForces <= 50) {
                    REX::INFO("camdir: first-person lock: camera state {} {} (no gate caught it) -> PlayerCamera::Force1stPerson", st, StateName(st));
                }
                g_refused = std::format("{} forced back to first person", StateName(st));
                tq::RunOnMain([st]() {  // main-idle queue (ThreadQueues.h), not from inside the camera update
                    auto* pc = RE::PlayerCamera::GetSingleton();
                    if (pc && pc->currentState && static_cast<int>(pc->currentState->id.get()) == st && g_force1st) {
                        g_force1st(pc);
                    }
                });
            }

            auto* root = cam->cameraRoot.get();
            float* R = root ? reinterpret_cast<float*>(&root->local.rotate) : nullptr;
            auto*  T = root ? &root->local.translate : nullptr;
            if (T) d.tg = { T->x, T->y, T->z };
            // 21 Nov, bFreeCamStatePosition: in the free camera, the state's own translation. FreeCameraState::Update (10223D0)
            // writes the camera root from it and returns; AAF's hook on that vfunc (CameraCaller.cpp UpdateThunk: POV head
            // anchor, glides) writes the state AFTER the original, so the root carries AAF's previous-frame position and the
            // POV trailed the animated head by a frame. The state holds this frame's (same value when nothing hooks it).
            if (T && st == kStateFree && g_cfg.freeStatePos && cam->currentState) {
                const auto& p = static_cast<const RE::FreeCameraState*>(cam->currentState.get())->translation;
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
                    d.tg = { p.x, p.y, p.z };
                    ++d.freeStateReads;
                }
            }

            // ---- effective policy (FADE_SNAP state machine) ----
            auto startEpisode = [&](const char* a_why) {
                f.active = true;
                f.timedOut = false;
                f.blackSeen = false;
                f.inRunning = false;
                f.start = now;
                f.fromState = d.prevState;
                f.causeState = st;
                ++f.episodes;
                // hold anchor: last good pose, else the game's current one
                const float yaw = scene ? d.sceneYaw : d.haveOwn ? d.ownYaw : d.gameYaw;
                const V3    off = RawOffsetWorld(yaw);
                d.holdBase = d.haveGood ? V3{ d.goodT.x - d.goodOff.x, d.goodT.y - d.goodOff.y, d.goodT.z - d.goodOff.z }
                                        : V3{ d.tg.x - off.x, d.tg.y - off.y, d.tg.z - off.z };
                REX::INFO("camdir: FADE_SNAP start ({}; state {} {}), fade out from alpha {:.2f}, hold ({:.1f},{:.1f},{:.1f})", a_why, st,
                    StateName(st), f.alpha, d.holdBase.x, d.holdBase.y, d.holdBase.z);
            };
            // a cut in the game's own camera translation while tracking (power-armour enter/exit orbit, warps):
            // blink = fade out on the held pose, snap, fade in
            float jump = 0.0f;
            if (d.havePrevTg && T) {
                const float dx = d.tg.x - d.prevTg.x, dy = d.tg.y - d.prevTg.y, dz = d.tg.z - d.prevTg.z;
                jump = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            // body camera: the eye base on the body (capsule + first-person eye height); jumps measured there, not on the
            // game camera (its orbit cuts are never shown)
            if (body) {
                float eyeH = -1.0f, hoff = 0.0f;
                game::FirstPersonEye(eyeH, hoff);
                auto* player = RE::PlayerCharacter::GetSingleton();
                const bool sneaking = player && (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(player) + 0x134) & 0x3800u) == 0x800u;
                if (!(eyeH > 10.0f)) eyeH = sneaking ? 80.5f : 120.5f;  // never sampled: flat 1.11.240's first-person camera node
                d.bodyEyeH = eyeH;
                if (player) {
                    const auto& loc = player->data.location;
                    V3 eye{ loc.x, loc.y, loc.z + eyeH };  // capsule eye (the plain path)
                    // 21 Nov, bBodyCameraFollowsAnimation: when the body is animated away from the capsule (AAF and other
                    // scenes, furniture, paired animations, bleedout), the eye rides the animated Head bone (+ fBodyCameraEyeForward
                    // along the face). Follow while a scene / furniture / bleedout signal holds, or while the animated eye is
                    // > kEnter from the capsule eye (left below kExit): walking / idle head bob stays on the capsule. Changes
                    // blend over kBlendS; a change bigger than kSnap is a cut (the jump blink, fJumpFadeUnits, as any cut).
                    constexpr float kEnter = 24.0f, kExit = 12.0f, kSnap = 40.0f, kBlendS = 0.3f;
                    d.bodyAnimOk = false;
                    if (g_cfg.bodyFollow) {
                        if (!d.bodySceneAt.time_since_epoch().count() || Ms(d.bodySceneAt, now) >= 100.0 || changed) {
                            d.bodySceneAt = now;
                            d.bodySceneNow = PlayerBleedingOut() || PlayerAafBusy() ||
                                             static_cast<int>(static_cast<RE::ActorState*>(player)->DoGetSitSleepState()) != 0;
                        }
                        RE::NiTransform hw;
                        if (PlayerHeadWorld(player, hw)) {
                            const auto& r = hw.rotate.entry[1];  // head-local +Y (the face) in world: stored row 1 (AAF POV_LFACE)
                            const float fw = g_cfg.bodyEyeFwd;
                            d.bodyAnimEye = { hw.translate.x + r.x * fw, hw.translate.y + r.y * fw, hw.translate.z + r.z * fw };
                            const float ex = d.bodyAnimEye.x - eye.x, ey = d.bodyAnimEye.y - eye.y, ez = d.bodyAnimEye.z - eye.z;
                            d.bodyDisp = std::sqrt(ex * ex + ey * ey + ez * ez);
                            d.bodyAnimOk = std::isfinite(d.bodyDisp);
                        } else {
                            ++d.bodyAnimMiss;
                        }
                    }
                    const bool want = d.bodyAnimOk && (d.bodySceneNow || d.bodyDisp > kEnter || (d.bodyFollowWant && d.bodyDisp > kExit));
                    const float target = want ? 1.0f : 0.0f;
                    const double fdt = d.bodyFollowAt.time_since_epoch().count() ? Ms(d.bodyFollowAt, now) / 1000.0 : 0.0;
                    d.bodyFollowAt = now;
                    if (want != d.bodyFollowWant) {
                        REX::INFO("camdir: body camera {} the animated head (state {} {}; animated eye {:.1f} u from the capsule eye{})",
                            want ? "follows" : "back from", st, StateName(st), d.bodyDisp, d.bodySceneNow ? ", scene / furniture signal" : "");
                    }
                    if (!d.havePrevBody || !d.bodyAnimOk || d.bodyDisp > kSnap) {
                        if (d.bodyFollowW != target && d.havePrevBody && d.bodyAnimOk) ++d.bodyFollowSnaps;
                        d.bodyFollowW = target;  // entry into the body view, a lost bone or a big change: no glide
                    } else if (d.bodyFollowW != target) {
                        if (want != d.bodyFollowWant) ++d.bodyFollowBlends;
                        const float step = static_cast<float>(std::min(fdt, 0.1) / kBlendS);
                        d.bodyFollowW = target > d.bodyFollowW ? std::min(1.0f, d.bodyFollowW + step) : std::max(0.0f, d.bodyFollowW - step);
                    }
                    d.bodyFollowWant = want;
                    if (d.bodyFollowW > 0.0f && d.bodyAnimOk) {
                        const float w = d.bodyFollowW * d.bodyFollowW * (3.0f - 2.0f * d.bodyFollowW);  // smoothstep
                        eye.x += (d.bodyAnimEye.x - eye.x) * w;
                        eye.y += (d.bodyAnimEye.y - eye.y) * w;
                        eye.z += (d.bodyAnimEye.z - eye.z) * w;
                    }
                    d.bodyBase = { eye.x, eye.y, eye.z + hoff };
                }
                ++d.bodyTicks;
                jump = 0.0f;
                if (d.havePrevBody) {
                    const float dx = d.bodyBase.x - d.prevBody.x, dy = d.bodyBase.y - d.prevBody.y, dz = d.bodyBase.z - d.prevBody.z;
                    jump = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                d.prevBody = d.bodyBase;
                d.havePrevBody = true;
            } else {
                d.havePrevBody = false;
                d.bodyFollowWant = false;
                d.bodyFollowW = 0.0f;
                d.bodyFollowAt = {};
                if (g_bh.root) g_bh = {};  // no reference kept on the player's 3D outside the body view
            }
            if (T) {
                d.prevTg = d.tg;
                d.havePrevTg = true;
            }
            // ---- free-camera scene view (state 3): yaw cuts and game-driven glides ----
            // AAF writes the free camera every frame (CameraCaller.cpp UpdateThunk): cuts (SetSceneCamera: position and yaw
            // jump in one update) and glides (StartSceneCameraTransition: a 1.5 s lerp or orbit). A translation cut is the
            // jump blink above; a yaw cut blinks here; a glide (fast movement while the controller sticks are idle, i.e.
            // not the player flying the camera) is held black until it settles.
            const double dtS = d.haveLastTick ? Ms(d.lastTick, now) / 1000.0 : 0.0;
            d.lastTick = now;
            d.haveLastTick = true;
            float yawStepDeg = 0.0f;
            bool  gliding = false;      // fast game-driven move (starts a glide hold after 3 updates)
            bool  gameMoving = false;   // still moving, slower (keeps a glide hold until the camera has settled)
            if (scene && R) {
                const float gy = SceneYaw(R);
                if (d.havePrevGy) {
                    yawStepDeg = std::abs(Wrap(gy - d.prevGy)) * kR2D;
                }
                d.prevGy = gy;
                d.havePrevGy = true;
                d.lastSpeed = dtS > 0.0005 ? static_cast<float>(jump / dtS) : 0.0f;
                const auto ps = pad::Last();
                const auto off = [](float v) { return std::abs(v) < 0.2f; };
                const bool sticksIdle = !ps.active || (off(ps.lx) && off(ps.ly) && off(ps.rx) && off(ps.ry) && ps.lt < 0.2f &&
                                                       ps.rt < 0.2f && !ps.lb && !ps.rb);
                gliding = g_cfg.freeGlideSpeed > 0.0f && d.lastSpeed > g_cfg.freeGlideSpeed &&
                          (g_cfg.jumpUnits <= 0.0f || jump <= g_cfg.jumpUnits) && sticksIdle;  // a cut is the jump blink
                gameMoving = g_cfg.freeGlideSpeed > 0.0f && d.lastSpeed > 0.15f * g_cfg.freeGlideSpeed && sticksIdle;
                d.glideFrames = gliding ? d.glideFrames + 1 : 0;
                // one hold per glide: re-armed once the game camera has been still for 250 ms
                if (gameMoving || gliding) d.glideQuietSince = now;
                else if (d.glideLatched && Ms(d.glideQuietSince, now) >= 250.0) d.glideLatched = false;
            } else {
                d.glideFrames = 0;
            }
            // ---- camera step 2: power-armour hold, large game-driven heading changes ----
            int sitSleep = -1;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                sitSleep = static_cast<int>(static_cast<RE::ActorState*>(player)->DoGetSitSleepState());
            }
            d.sitSleep = sitSleep;
            if (g_paRequest.exchange(false)) {
                if (!f.active) {
                    startEpisode("power armour furniture (gated)");
                }
                f.hold = 1;
                f.holdStart = now;
                f.holdClearSeen = false;
                f.snapHeading = g_cfg.headingSnap && rs::OwnHeadingEnabled();
                rs::FreezeBodyFollow(f.snapHeading);
                ++f.paHolds;
            }
            float jr = 0.0f;
            const bool turned = rs::TakeGameTurn(jr);
            const bool bigTurn = turned && std::abs(jr) * kR2D >= g_cfg.headingSnapDeg;
            if (bigTurn && !(g_cfg.headingSnap && rs::OwnHeadingEnabled() && pol != Policy::kCinema) && !f.snapHeading) {
                rs::FreezeBodyFollow(false);  // rs froze B3 on the jump; no snap follows here
            }
            if (bigTurn && g_cfg.headingSnap && rs::OwnHeadingEnabled() && pol != Policy::kCinema) {
                f.lastHeadingJumpDeg = jr * kR2D;
                REX::INFO("camdir: game-driven heading change {:.1f} deg (>= {:.0f}): fade-snap the view", jr * kR2D, g_cfg.headingSnapDeg);
                if (!f.active && !f.test) {
                    startEpisode(std::format("game heading change {:.1f} deg", jr * kR2D).c_str());
                }
                f.snapHeading = true;
                rs::FreezeBodyFollow(true);
            }
            bool holding = false;
            if (f.active && f.hold == 1) {
                const double heldMs = Ms(f.holdStart, now);
                if (sitSleep != 0) {
                    f.holdClearSeen = false;
                } else if (!f.holdClearSeen) {
                    f.holdClearSeen = true;
                    f.holdClear = now;
                }
                const bool clear = f.holdClearSeen && Ms(f.holdClear, now) >= 300.0 && heldMs >= 500.0;
                if (clear || heldMs >= g_cfg.paHoldS * 1000.0) {
                    if (!clear) {
                        ++f.paHoldTimeouts;
                        REX::WARN("camdir: power-armour hold timeout after {:.0f} ms (sit/sleep state {})", heldMs, sitSleep);
                    }
                    f.lastPaHoldMs = heldMs;
                    REX::INFO("camdir: power-armour hold released after {:.0f} ms (sit/sleep state {})", heldMs, sitSleep);
                    f.hold = 0;
                } else {
                    holding = true;
                }
            }
            if (f.active && f.hold == 2) {  // free-camera glide: black until the game camera has been still for 250 ms
                const double heldMs = Ms(f.holdStart, now);
                if (gameMoving) {
                    f.holdClearSeen = false;
                } else if (!f.holdClearSeen) {
                    f.holdClearSeen = true;
                    f.holdClear = now;
                }
                const bool clear = !scene || (f.holdClearSeen && Ms(f.holdClear, now) >= 250.0);
                if (clear || heldMs >= g_cfg.freeGlideHoldS * 1000.0) {
                    if (!clear) {
                        ++d.glideTimeouts;
                        REX::WARN("camdir: free-camera glide hold timeout after {:.0f} ms (speed {:.0f} units/s)", heldMs, d.lastSpeed);
                    }
                    REX::INFO("camdir: free-camera glide hold released after {:.0f} ms", heldMs);
                    f.hold = 0;
                } else {
                    holding = true;
                }
            }

            Policy eff = pol;
            if ((pol == Policy::kTrack || pol == Policy::kLock) && g_cfg.jumpUnits > 0.0f && jump > g_cfg.jumpUnits && !f.active &&
                d.haveGood) {
                ++d.jumpFades;
                d.lastJump = jump;
                startEpisode(std::format("translation jump {:.1f} units", jump).c_str());
            }
            if (scene && pol == Policy::kTrack && !f.active && d.haveGood) {
                if (g_cfg.freeYawCutDeg > 0.0f && yawStepDeg > g_cfg.freeYawCutDeg) {
                    ++d.yawCuts;
                    startEpisode(std::format("free-camera yaw cut {:.1f} deg", yawStepDeg).c_str());
                } else if (d.glideFrames >= 3 && !d.glideLatched) {
                    d.glideLatched = true;
                    ++d.glideHolds;
                    startEpisode(std::format("free-camera glide {:.0f} units/s", d.lastSpeed).c_str());
                    f.hold = 2;
                    f.holdStart = now;
                    f.holdClearSeen = false;
                    holding = true;
                }
            }
            if (pol == Policy::kFadeSnap) {
                if (!f.active) {
                    startEpisode("policy");
                }
                if (!f.timedOut && Ms(f.start, now) > g_cfg.timeoutS * 1000.0f) {
                    f.timedOut = true;
                    ++f.timeouts;
                    REX::WARN("camdir: FADE_SNAP timeout after {:.0f} ms in state {} ({}): cinema", Ms(f.start, now), st, StateName(st));
                    BeginFadeIn(now, "(timeout)");
                }
                eff = f.timedOut ? Policy::kCinema : Policy::kFadeSnap;
            } else if (f.active) {
                if (!f.timedOut && !menuWin && (f.alpha < 1.0f || holding)) {
                    eff = Policy::kFadeSnap;  // finish fading out (and any hold) before the snap
                } else {
                    f.active = false;
                    f.hold = 0;
                    if (!f.timedOut) {
                        REX::INFO("camdir: snap to state {} ({}) policy {}", st, StateName(st), PolicyName(pol));
                        f.causeState = st;
                        if (f.snapHeading) {
                            ++f.headingSnaps;
                            REX::INFO("camdir: heading snap: {}", rs::SnapViewToHeading());
                        }
                        if (scene) {
                            // the view is black: take the free camera's own yaw (AAF frames the scene straight ahead) and count
                            // head motion from here, so the view sits exactly at the free camera (AAF's POV head point)
                            d.freeYawOffset = 0.0f;
                            d.freeEntry = false;
                            head::Vec3 o;
                            d.haveFreeOffBase = head::GetOffset(o);
                            d.freeOffBase = o;
                        }
                        BeginFadeIn(now, "(snap)");
                    }
                    f.snapHeading = false;
                    rs::FreezeBodyFollow(false);
                }
            }
            if (f.test) {
                if (f.blackSeen && Ms(f.black, now) >= f.testHoldMs) {
                    f.test = false;
                    BeginFadeIn(now, "(fadetest)");
                }
            }
            f.target = eff == Policy::kFadeSnap || f.test;
            if (eff != d.eff) {
                REX::INFO("camdir: effective policy {} -> {} (state {} {})", PolicyName(d.eff), PolicyName(eff), st, StateName(st));
                if (eff == Policy::kLock) {
                    const float yaw = d.haveOwn ? d.ownYaw : d.gameYaw;
                    const V3    off = RawOffsetWorld(yaw);
                    d.holdBase = d.haveGood ? V3{ d.goodT.x - d.goodOff.x, d.goodT.y - d.goodOff.y, d.goodT.z - d.goodOff.z }
                                            : V3{ d.tg.x - off.x, d.tg.y - off.y, d.tg.z - off.z };
                }
                d.eff = eff;
            }

            // ---- pose ----
            d.wrote = false;
            if (!root) {
                d.moveViewOk = false;
                return;
            }
            NiYawPitch(R, d.gameYaw, d.gamePitch);
            d.preYaw = SceneYaw(R);  // the frame the game moves the player in (its own camera), before our write
            d.preYawOk = true;
            head::Mat3 h{};
            d.haveHead = head::Tracking() && head::Get(h);
            d.haveOwn = rs::OwnedYaw(d.ownYaw);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                d.heading = player->data.angle.z;
            }
            float yaw = d.haveOwn ? d.ownYaw : d.gameYaw;
            if (body && !d.haveOwn) {
                yaw = d.heading;  // the body's heading (an orbit camera's yaw / pitch never reach the body view)
            }
            if (scene) {
                // SCENE view (free camera, third person, animated, ...): the base yaw is the game camera's (AAF aims its shots
                // there; the player's stick turns it), the game camera's pitch is dropped (the head gives pitch). On entry without a fade it continues the
                // previous view's yaw (offset), after any snap the offset is 0. Frozen while a fade holds the old pose.
                const float sy = SceneYaw(R);
                // free camera: + the stick's own turn (bFreeCamOwnStickYaw; HkFreeCamMove keeps it out of the camera, where
                // AAF's POV auto mode would glide it back to the actor's gaze). Kept over cuts / snaps for the whole scene.
                const float stick = st == kStateFree && g_cfg.freeOwnStick ? d.stickYaw : 0.0f;
                if (d.freeEntry) {
                    d.freeEntry = false;
                    if (!d.sceneKeepHead) {
                        head::Vec3 o;
                        d.haveFreeOffBase = head::GetOffset(o);  // head motion counts from here (the game camera = the view)
                        d.freeOffBase = o;
                    }
                    d.freeYawOffset = d.lastBaseValid ? Wrap(d.lastBaseYaw - sy - stick) : 0.0f;
                    d.sceneYaw = d.lastBaseValid ? d.lastBaseYaw : Wrap(sy + stick);
                    // a blink that started this tick (entry jump) anchored on the full head offset (first person's); the hold now
                    // adds only the motion since this new base, so move the anchor by the base: no jump while fading out
                    if (!d.sceneKeepHead && f.active && g_cfg.freeHeadDelta && d.haveFreeOffBase) {
                        const float u = g_cfg.unitsPerMeter, c = std::cos(d.sceneYaw), sn = std::sin(d.sceneYaw);
                        const auto& o = d.freeOffBase;
                        d.holdBase.x += (o.x * c + o.y * sn) * u;
                        d.holdBase.y += (-o.x * sn + o.y * c) * u;
                        d.holdBase.z += o.z * u;
                    }
                    REX::INFO("camdir: scene view ({}): game camera yaw {:.1f}, view yaw {:.1f} (offset {:.1f}){}", StateName(st), sy * kR2D,
                        d.sceneYaw * kR2D, d.freeYawOffset * kR2D, d.sceneKeepHead ? ", head base kept" : "");
                }
                if (eff == Policy::kTrack) {
                    d.sceneYaw = Wrap(sy + d.freeYawOffset + stick);
                }
                yaw = d.sceneYaw;
            }
            V3 rawOff = RawOffsetWorld(yaw);
            if (scene && g_cfg.freeHeadDelta && d.haveFreeOffBase) {
                // Free camera: head motion since the entry / last snap only. The raw offset is from the recentre origin, so it
                // carries wherever the head was at recentre (a user ini compensates that with fHeightOffsetCm in first person);
                // added whole it lifted AAF's POV point above the NPC's head (user, headset 15 Nov).
                const float u = g_cfg.unitsPerMeter, c = std::cos(yaw), sn = std::sin(yaw);
                const auto& o = d.freeOffBase;
                rawOff.x -= (o.x * c + o.y * sn) * u;
                rawOff.y -= (-o.x * sn + o.y * c) * u;
                rawOff.z -= o.z * u;
            }
            d.raw = rawOff;

            if (eff != Policy::kCinema && d.haveHead && !(eff == Policy::kTrack && st == 0)) {
                // rotation: Rz(ownYaw) x head (game rotation x head if the heading is not owned); free-camera scene: Rz(sceneYaw)
                float bw, bx, by, bz;
                if (scene || body) {
                    bw = std::cos(-yaw * 0.5f); bx = 0.0f; by = 0.0f; bz = std::sin(-yaw * 0.5f);
                } else if (d.haveOwn) {
                    bw = std::cos(-d.ownYaw * 0.5f); bx = 0.0f; by = 0.0f; bz = std::sin(-d.ownYaw * 0.5f);
                } else {
                    float m[3][3];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) m[i][j] = R[j * 4 + i];  // stored row j = column j
                    MatToQuat(m, bw, bx, by, bz);
                }
                float hw, hx, hy, hz;
                MatToQuat(h.m, hw, hx, hy, hz);
                const float w = bw * hw - bx * hx - by * hy - bz * hz;
                const float x = bw * hx + bx * hw + by * hz - bz * hy;
                const float y = bw * hy - bx * hz + by * hw + bz * hx;
                const float z = bw * hz + bx * hy - by * hx + bz * hw;
                QuatToNi(w, x, y, z, R);
                // translation
                if (eff == Policy::kTrack && scene) {
                    // the free camera's position (AAF shot / POV head anchor / flown camera) + head motion from the recentre
                    // origin, in the scene yaw's frame (roomscale does not run in the free camera)
                    d.add = rawOff;
                    T->x = d.tg.x + rawOff.x;
                    T->y = d.tg.y + rawOff.y;
                    T->z = d.tg.z + rawOff.z;
                } else if (eff == Policy::kTrack && body) {
                    // body camera: the first-person eye on the body + head offset (roomscale residual), as head authority
                    // builds the first-person camera (GameBridge HkFpGetPos); the game camera's translation is ignored
                    head::Vec3 o;
                    V3         add{};
                    if (head::GetOffset(o)) {
                        rs::AdjustHeadOffset(o, yaw);
                        const float u = g_cfg.unitsPerMeter, c = std::cos(yaw), s = std::sin(yaw);
                        add = { (o.x * c + o.y * s) * u, (-o.x * s + o.y * c) * u, o.z * u };
                    }
                    d.add = add;
                    T->x = d.bodyBase.x + add.x;
                    T->y = d.bodyBase.y + add.y;
                    T->z = d.bodyBase.z + add.z;
                } else if (eff == Policy::kTrack) {
                    // game translation + head offset (roomscale residual, as in first person)
                    head::Vec3 o;
                    V3         add{};
                    if (head::GetOffset(o)) {
                        rs::AdjustHeadOffset(o, yaw);
                        const float u = g_cfg.unitsPerMeter, c = std::cos(yaw), s = std::sin(yaw);
                        add = { (o.x * c + o.y * s) * u, (-o.x * s + o.y * c) * u, o.z * u };
                    }
                    d.add = add;
                    T->x = d.tg.x + add.x;
                    T->y = d.tg.y + add.y;
                    T->z = d.tg.z + add.z + g_cfg.heightOffsetCm * 0.01f * g_cfg.unitsPerMeter;
                } else {  // FADE_SNAP hold / LOCK: anchor + head motion
                    T->x = d.holdBase.x + rawOff.x;
                    T->y = d.holdBase.y + rawOff.y;
                    T->z = d.holdBase.z + rawOff.z;
                }
                RE::NiUpdateData ud{};
                root->Update(ud);
                game::ApplyTargetFovNoSample();  // a non-first-person state's Update may have rewritten worldFOV
                d.wrote = true;
            }
            ++d.writes[static_cast<int>(eff)];
            d.out = { T->x, T->y, T->z };
            NiYawPitch(R, d.camYaw, d.camPitch);
            // free camera: the view just written steers the next update's stick flying (HkFreeCamMove)
            d.moveViewOk = scene && eff == Policy::kTrack && d.wrote;
            d.moveViewYaw = d.camYaw;
            d.moveViewPitch = d.camPitch;
            if (eff == Policy::kTrack && (d.wrote || st == 0)) {
                d.haveGood = true;
                d.goodT = d.out;
                d.goodOff = rawOff;
            }
            const auto& wt = root->world.translate;
            if (d.havePrevWorld) {
                const float dx = wt.x - d.prevWorld.x, dy = wt.y - d.prevWorld.y, dz = wt.z - d.prevWorld.z;
                const float j = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (j > 100.0f) {
                    ++d.rootJumps;
                    REX::INFO("camdir: camera root jump {:.1f} units in one update (state {} {}, effective {})", j, st, StateName(st),
                        PolicyName(eff));
                }
            }
            d.prevWorld = { wt.x, wt.y, wt.z };
            d.havePrevWorld = true;
            // view base yaw for the next state-3 entry (continuity); only meaningful while the world view was shown
            d.lastBaseValid = eff != Policy::kCinema;
            if (d.lastBaseValid) {
                d.lastBaseYaw = yaw;
            }
        }

        void HkUpdate(RE::TESCamera* a_cam)
        {
            head::WaitForPublish(5);  // pose sync (HeadPose.h): the game's job thread vs the Present hook
            g_orig(a_cam);
            auto* pc = RE::PlayerCamera::GetSingleton();
            if (pc && static_cast<RE::TESCamera*>(pc) == a_cam) {
                Tick(pc);
                vatswin::NoteCameraPitch(1);  // round-5 diagnostics (VATS pitch hunt)
            }
        }

        // Free camera: the thunk 1026290 (jmp TESCamera::Update) redirected here; its callers (UpdateFreeCamera D5A960, the
        // load-game path D682F0) pass the PlayerCamera. Same pose sync and post-tick as the UpdateCamera call site.
        void HkUpdateFreeCam(RE::TESCamera* a_cam)
        {
            if (!g_freeSceneLive.load(std::memory_order_relaxed)) {
                g_orig(a_cam);  // bFreeCamScene=0 (live switch): as if the thunk were not patched (no pose wait, no director tick)
                return;
            }
            head::WaitForPublish(5);
            g_orig(a_cam);  // TESCamera::Update 827C80 (the active state's Update: FreeCameraState, with AAF's own hook inside)
            auto* pc = RE::PlayerCamera::GetSingleton();
            if (pc && static_cast<RE::TESCamera*>(pc) == a_cam) {
                g_freeTicks.fetch_add(1, std::memory_order_relaxed);
                Tick(pc);
            }
        }

        // Free-camera stick flying along the head-tracked view (user, headset 15 Nov: "camera starts looking down on the animation,
        // look up, push forward and it moves down towards where it was originally looking"). FreeCameraState::UpdateMovement
        // (1022B60, called @1022435 from FreeCameraState::Update 10223D0) turns the camera by the right stick (+0x34 pitch, + =
        // down; +0x38 yaw; both kept in [0, 2pi)) and then moves it by the left stick / up-down in the frame of that rotation.
        // Here it runs with the rotation set to the view we rendered last tick; the stick's own turn is then carried over to
        // the camera's real rotation, so AAF's shot / POV aim and our base yaw are untouched. Input itself is not changed.
        using FreeMove_t = void (*)(RE::FreeCameraState*);
        FreeMove_t g_origFreeMove = nullptr;

        float WrapPos(float a)
        {
            a = std::fmod(a, 2.0f * kPi);
            return a < 0.0f ? a + 2.0f * kPi : a;
        }

        void HkFreeCamMove(RE::FreeCameraState* a_fs)
        {
            bool  use = false;
            float vy = 0.0f, vp = 0.0f;
            {
                std::scoped_lock l(g_lock);
                if (g_cfg.enabled && g_cfg.freeMoveView && FreeOn() && g_d.moveViewOk) {
                    use = true;
                    vy = g_d.moveViewYaw;
                    vp = g_d.moveViewPitch;
                    ++g_d.moveViewCalls;
                }
            }
            if (!use || !a_fs) {
                g_origFreeMove(a_fs);
                return;
            }
            const float sPitch = a_fs->rotation.x, sYaw = a_fs->rotation.y;
            const float tPitch = WrapPos(-vp), tYaw = WrapPos(vy);
            a_fs->rotation.x = tPitch;
            a_fs->rotation.y = tYaw;
            g_origFreeMove(a_fs);
            const float dPitch = Wrap(a_fs->rotation.x - tPitch), dYaw = Wrap(a_fs->rotation.y - tYaw);  // the stick's turn
            a_fs->rotation.x = WrapPos(sPitch + dPitch);
            // 20 Nov late (user: "AAF with PoV camera will rotate the direction of the camera at times. I use the stick to turn the
            // central facing direction right, and then it drifts back to a point left"): AAF's POV (CameraCaller ApplyPovFrame,
            // pov_control_mode=auto) treats any change of the free camera's rotation as a mouse takeover and, pov_takeover_cooldown
            // (1.2 s) later, glides the camera back to the actor's gaze (POV_RESUME_TAU 0.25 s); a takeover is also clamped to
            // pov_yaw_limit (75 deg) around the gaze. So the turn is kept as our own yaw on top of the camera's instead
            // (Tick: sceneYaw = free-camera yaw + continuity offset + stickYaw); AAF keeps aiming the camera at the gaze.
            bool own = false;
            {
                std::scoped_lock l(g_lock);
                own = g_cfg.freeOwnStick;
                if (own && dYaw != 0.0f) {
                    g_d.stickYaw = Wrap(g_d.stickYaw + dYaw);
                    ++g_d.stickTurnUpdates;
                }
            }
            a_fs->rotation.y = own ? sYaw : WrapPos(sYaw + dYaw);
        }

        // camera step 2: StopPipboyMode runs TESCamera::Update itself (1027250 -> call @10273A6), outside UpdateCamera;
        // without this the director missed that frame (the game's pose went out unreviewed for one update)
        void HkUpdatePipboyStop(RE::TESCamera* a_cam)
        {
            bool tick;
            {
                std::scoped_lock l(g_lock);
                ++g_gs[kGPipboyStop].calls;
                tick = g_cfg.pipboyStopTick;
                if (tick) ++g_gs[kGPipboyStop].blocked;
            }
            if (tick) {
                HkUpdate(a_cam);
            } else {
                g_orig(a_cam);
            }
        }

        // patches the step-2 gates (each byte-checked on its own; a mismatch skips that gate only). g_lock held.
        std::string InstallGates()
        {
            Cfg&        c = g_cfg;
            std::string out;
            auto note = [&](GateId a_g, bool a_want, bool a_ok) {
                g_gs[a_g].patched = a_ok;
                out += std::format(" {}={}", kGateNames[a_g], !a_want ? "off" : a_ok ? "ok" : "FAILED");
            };
            // helpers the furniture gate calls: call bytes + rel32 target inside StartFurnitureMode / TESFurniture::Activate,
            // and the callee prologues
            static constexpr std::uint8_t kIsInFurnCall[5] = { 0xE8, 0x24, 0xFF, 0xFF, 0xFF };
            static constexpr std::uint8_t kQFurnCall[5] = { 0xE8, 0x01, 0x5B, 0x9B, 0xFF };
            static constexpr std::uint8_t kPushHudCall[5] = { 0xE8, 0xF9, 0xF5, 0xA0, 0xFF };
            static constexpr std::uint8_t kPaKwCall[5] = { 0xE8, 0x78, 0x7C, 0xF2, 0xFF };
            static constexpr std::uint8_t kIsInFurnPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x8B, 0xB9, 0x28, 0x01, 0x00, 0x00 };
            static constexpr std::uint8_t kQFurnPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x65, 0x48, 0x8B, 0x04, 0x25 };
            static constexpr std::uint8_t kPushHudPro[] = { 0xB2, 0x01, 0xE9, 0x19, 0x00, 0x00, 0x00 };
            static constexpr std::uint8_t kPaKwPro[] = { 0x48, 0x8B, 0x05, 0x29, 0x1A, 0xD8, 0x02, 0x48, 0x85, 0xC0, 0x74, 0x06, 0x80, 0x78, 0x1A, 0x04 };
            // detour prologues (first instructions relocated: push/sub only, no RIP-relative operands)
            static constexpr std::uint8_t kStartFurnPro[] = { 0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 };  // reloc 5
            static constexpr std::uint8_t kStartAnimPro[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68 };  // reloc 6
            static constexpr std::uint8_t k3rdPro[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x33, 0xC0, 0x48, 0x8B, 0x49, 0x28 };  // reloc 6
            static constexpr std::uint8_t kStartVatsPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x05 };  // reloc 6
            static constexpr std::uint8_t kForceVatsPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x05 };  // reloc 6
            static constexpr std::uint8_t kStopVatsPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0xE8 };  // reloc 6

            // round 4: the furniture / animated / third-person detours are always installed (each passes straight to the
            // original while its gate flag is off), so "camdir gate <name> on" can test them without the ini switch
            const bool wantFurn = true;
            if (wantFurn) {
                const bool helpers = CallAt(kIsInFurnCallSite, kIsInFurniture, kIsInFurnCall, "IsInFurnitureMode call") &&
                                     CallAt(kQFurnCallSite, kQFurnEnterExit, kQFurnCall, "HUDMode::QFurnitureEnterExit call") &&
                                     CallAt(kPushHudCallSite, kPushHudMode, kPushHudCall, "SendHUDMessage::PushHUDMode call") &&
                                     CallAt(kPaKwCallSite, kPaFurnKeyword, kPaKwCall, "PowerArmor::GetFurnitureKeyword call") &&
                                     BytesAt(kIsInFurniture, kIsInFurnPro, sizeof(kIsInFurnPro), "IsInFurnitureMode prologue") &&
                                     BytesAt(kQFurnEnterExit, kQFurnPro, sizeof(kQFurnPro), "QFurnitureEnterExit prologue") &&
                                     BytesAt(kPushHudMode, kPushHudPro, sizeof(kPushHudPro), "PushHUDMode body") &&
                                     BytesAt(kPaFurnKeyword, kPaKwPro, sizeof(kPaKwPro), "GetFurnitureKeyword body");
                if (helpers) {
                    g_isInFurniture = reinterpret_cast<IsInFurn_t>(Base() + kIsInFurniture);
                    g_qFurnEnterExit = reinterpret_cast<QHudMode_t>(Base() + kQFurnEnterExit);
                    g_pushHudMode = reinterpret_cast<PushHud_t>(Base() + kPushHudMode);
                    g_paKeyword = reinterpret_cast<GetKw_t>(Base() + kPaFurnKeyword);
                    // workbench body swap: the call sites inside StartFurnitureMode must target these functions
                    static constexpr std::uint8_t kShowCall[5] = { 0xE8, 0xEC, 0x51, 0xD3, 0xFF };   // @1027D6F -> D5CF60
                    static constexpr std::uint8_t kIsFurnCall[5] = { 0xE8, 0x29, 0x7D, 0x53, 0xFF }; // @1027C32 -> 55F960
                    static constexpr std::uint8_t kIsWbCall[5] = { 0xE8, 0x98, 0x49, 0x42, 0xFF };   // @1027C93 -> 44C630
                    static constexpr std::uint8_t kQShowCall[5] = { 0xE8, 0xE8, 0x08, 0x13, 0x00 };  // @B19A03 -> C4A2F0
                    if (CallAt(0x1027D6F, kShow1stPerson, kShowCall, "Show1stPerson call") &&
                        CallAt(0x1027C32, kIsFurnitureRef, kIsFurnCall, "IsFurniture call") &&
                        CallAt(0x1027C93, kIsWorkbench, kIsWbCall, "IsWorkbench call") &&
                        CallAt(0xB19A03, kQueueShow1st, kQShowCall, "QueueShow1stPerson call")) {
                        g_show1st = reinterpret_cast<Show1st_t>(Base() + kShow1stPerson);
                        g_isFurnitureRef = reinterpret_cast<IsFurnRef_t>(Base() + kIsFurnitureRef);
                        g_isWorkbench = reinterpret_cast<IsWorkbench_t>(Base() + kIsWorkbench);
                        g_queueShow1st = reinterpret_cast<QueueShow1st_t>(Base() + kQueueShow1st);
                    }
                    static constexpr std::uint8_t kShowCraftPro[] = { 0x40, 0x53, 0x56, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0xF1 };
                    if (g_isFurnitureRef && g_isWorkbench) {  // inert unless a direct open set g_directGuard
                        // detour (push rbx; push rsi; sub rsp,78 = 7 relocatable bytes): the call with nullptr = "close the
                        // crafting menu" from the furniture exit path, which the failed sit runs right after a direct open
                        g_showCrafting = reinterpret_cast<ShowCrafting_t>(Detour(kShowCraftingMenu, kShowCraftPro, sizeof(kShowCraftPro), 7,
                            reinterpret_cast<void*>(&HkShowCrafting), "UIUtils::ShowCraftingMenu prologue"));
                    }
                    g_origStartFurn = reinterpret_cast<StartFurn_t>(Detour(kStartFurniture, kStartFurnPro, sizeof(kStartFurnPro), 5,
                        reinterpret_cast<void*>(&HkStartFurniture), "StartFurnitureMode prologue"));
                }
                note(kGFurniture, true, g_origStartFurn != nullptr);
            } else {
                note(kGFurniture, false, false);
            }
            if (true) {  // see wantFurn
                g_origStartAnim = reinterpret_cast<StartAnim_t>(Detour(kStartAnimated, kStartAnimPro, sizeof(kStartAnimPro), 6,
                    reinterpret_cast<void*>(&HkStartAnimated), "StartAnimatedMode prologue"));
            }
            note(kGAnimated, c.gateAnimated, g_origStartAnim != nullptr);
            if (true) {  // see wantFurn
                g_origSet3rd = reinterpret_cast<CamFn_t>(Detour(kSet3rd, k3rdPro, sizeof(k3rdPro), 6,
                    reinterpret_cast<void*>(&HkCamFn<kGSet3rd>), "Set3rdPerson prologue"));
                g_origForce3rd = reinterpret_cast<CamFn_t>(Detour(kForce3rd, k3rdPro, sizeof(k3rdPro), 6,
                    reinterpret_cast<void*>(&HkCamFn<kGForce3rd>), "Force3rdPerson prologue"));
            }
            note(kGSet3rd, c.blockThirdPerson, g_origSet3rd != nullptr);
            note(kGForce3rd, c.blockThirdPerson, g_origForce3rd != nullptr);
            {
                // 20 Nov, first-person lock: always installed (pass straight through while the lock is off), so
                // "camdir lock on" works at runtime. Prologues read in Ghidra 1.11.240; relocated bytes are whole push / sub /
                // mov [rsp+n] instructions (no RIP-relative operands).
                // mov [rsp+8],rbx; push rdi; sub rsp,40; mov rbx,rcx; movzx edi,dl (reloc 5)
                static constexpr std::uint8_t kFreePro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x0F, 0xB6, 0xFA };
                // push rbx; sub rsp,20; mov rax,[rcx+28]; lea rdx,[rcx+28] (reloc 6)
                static constexpr std::uint8_t kBleedPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x28, 0x48, 0x8D, 0x51, 0x28 };
                // mov [rsp+18],rsi; push rdi; sub rsp,20; mov rax,[rcx+28]; mov rsi,rdx (reloc 5)
                static constexpr std::uint8_t kMountPro[] = { 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x28, 0x48, 0x8B, 0xF2 };
                if (!g_origToggleFree) {
                    g_origToggleFree = reinterpret_cast<ToggleFree_t>(Detour(kToggleFreeCam, kFreePro, sizeof(kFreePro), 5,
                        reinterpret_cast<void*>(&HkToggleFreeCam), "ToggleFreeCameraMode prologue"));
                }
                if (!g_origStartBleed) {
                    g_origStartBleed = reinterpret_cast<CamFn_t>(Detour(kStartBleedout, kBleedPro, sizeof(kBleedPro), 6,
                        reinterpret_cast<void*>(&HkStartBleedout), "StartBleedoutMode prologue"));
                }
                if (!g_origStartMount) {
                    g_origStartMount = reinterpret_cast<StartMount_t>(Detour(kStartMount, kMountPro, sizeof(kMountPro), 5,
                        reinterpret_cast<void*>(&HkStartMount), "StartMountMode prologue"));
                }
                note(kGFreeCam, true, g_origToggleFree != nullptr);
                note(kGBleedout, true, g_origStartBleed != nullptr);
                note(kGMount, true, g_origStartMount != nullptr);
            }
            if (!g_stopInteract) {
                static constexpr std::uint8_t kStopIntPro[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x88, 0x50, 0x10, 0x55, 0x56, 0x41, 0x54 };
                if (BytesAt(kStopInteractingQuick, kStopIntPro, sizeof(kStopIntPro), "Actor::StopInteractingQuick prologue")) {
                    g_stopInteract = reinterpret_cast<StopInteract_t>(Base() + kStopInteractingQuick);
                }
                // mov [rsp+18],rbx; push rdi; sub rsp,20; mov rdi,rcx; mov qword [rsp+30],0; mov rcx,[rcx+100] (extra list)
                static constexpr std::uint8_t kReenablePro[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9,
                    0x48, 0xC7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x89, 0x00, 0x01, 0x00, 0x00 };
                if (BytesAt(kReenableInput, kReenablePro, sizeof(kReenablePro), "TESObjectREFR::ReenableInputForPlayer prologue")) {
                    g_reenableInput = reinterpret_cast<Reenable_t>(Base() + kReenableInput);
                }
                static constexpr std::uint8_t kClearRunOncePro[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 };
                static constexpr std::uint8_t kSetAiCtrlPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
                static constexpr std::uint8_t kSetSitPro[] = { 0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18, 0x55, 0x56, 0x57 };
                if (BytesAt(kClearRunOnce, kClearRunOncePro, sizeof(kClearRunOncePro), "AIProcess::ClearRunOncePackage prologue") &&
                    BytesAt(kSetAiCtrlPkg, kSetAiCtrlPro, sizeof(kSetAiCtrlPro), "PlayerCharacter::SetAIControlledPackage prologue") &&
                    BytesAt(kSetSitSleep, kSetSitPro, sizeof(kSetSitPro), "AIProcess::SetSitSleepState prologue")) {
                    g_clearRunOnce = reinterpret_cast<ClearRunOnce_t>(Base() + kClearRunOnce);
                    g_setAiCtrlPkg = reinterpret_cast<SetAiCtrl_t>(Base() + kSetAiCtrlPkg);
                    g_setSitSleep = reinterpret_cast<SetSit_t>(Base() + kSetSitSleep);
                }
            }
            if (!g_force1st) {
                // push rbx; sub rsp,20; mov rbx,rcx; mov rcx,[player singleton]
                static constexpr std::uint8_t kForce1stPro[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D };
                if (BytesAt(kForce1st, kForce1stPro, sizeof(kForce1stPro), "PlayerCamera::Force1stPerson prologue")) {
                    g_force1st = reinterpret_cast<Force1st_t>(Base() + kForce1st);
                }
            }
            if (!g_origWbCtor) {  // 20 Nov: installed always, g_workbenchPause gates it at runtime (live [Menus] bWorkbenchPause)
                // mov [rsp+8],rbx; mov [rsp+18],rbp; mov [rsp+20],rsi; push rdi; sub rsp,20 (20 bytes, no RIP-relative)
                static constexpr std::uint8_t kWbCtorPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                    0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20 };
                g_origWbCtor = reinterpret_cast<WbCtor_t>(Detour(kWorkbenchCtor, kWbCtorPro, sizeof(kWbCtorPro), 5,
                    reinterpret_cast<void*>(&HkWorkbenchCtor), "WorkbenchMenuBase ctor prologue"));
                REX::INFO("camdir: crafting menus pause the game like FO4VR: {}", g_origWbCtor ? "hook installed" : "NOT installed");
            }
            if (c.gateVats) {
                // all three or none: a skipped Start must pair with a skipped Stop
                const bool ok = BytesAt(kStartVats, kStartVatsPro, sizeof(kStartVatsPro), "StartVATSMode prologue") &&
                                BytesAt(kForceVats, kForceVatsPro, sizeof(kForceVatsPro), "ForceVATSMode prologue") &&
                                BytesAt(kStopVats, kStopVatsPro, sizeof(kStopVatsPro), "StopVATSMode prologue");
                if (ok) {
                    g_origStopVats = reinterpret_cast<CamFn_t>(Detour(kStopVats, kStopVatsPro, sizeof(kStopVatsPro), 6,
                        reinterpret_cast<void*>(&HkCamFn<kGStopVats>), "StopVATSMode prologue"));
                    g_origStartVats = reinterpret_cast<CamFn_t>(Detour(kStartVats, kStartVatsPro, sizeof(kStartVatsPro), 6,
                        reinterpret_cast<void*>(&HkCamFn<kGStartVats>), "StartVATSMode prologue"));
                    g_origForceVats = reinterpret_cast<CamFn_t>(Detour(kForceVats, kForceVatsPro, sizeof(kForceVatsPro), 6,
                        reinterpret_cast<void*>(&HkCamFn<kGForceVats>), "ForceVATSMode prologue"));
                }
            }
            // ChangeState hook: VG2 (live-world VATS) and/or the VATSScanImod stop (also in the VATS window)
            if ((c.gateVats && c.vatsKeepCamera && g_origStartVats) || g_vatsNoImod) {
                static constexpr std::uint8_t kChangeStatePro[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9 };
                static constexpr std::uint8_t kCullCall[5] = { 0xE8, 0x61, 0x43, 0x13, 0x00 };
                static constexpr std::uint8_t kCullPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
                // the call sequence in the state-2 branch: SetEnabled vcall [rax+10] with dl=0, then the task-queue load + cull call
                static constexpr std::uint8_t kState2Seq[] = { 0xFF, 0x50, 0x10, 0x48, 0x8B, 0x0D };
                REL::Relocation<std::uintptr_t> camVtbl{ RE::VTABLE::PlayerCamera[0] };
                const auto slot2 = reinterpret_cast<const std::uintptr_t*>(camVtbl.address())[2];
                std::int32_t tqRel = 0;
                std::memcpy(&tqRel, reinterpret_cast<const void*>(Base() + kState2TaskQueueLoad + 3), 4);
                const bool ok = BytesAt(kVatsChangeState, kChangeStatePro, sizeof(kChangeStatePro), "VATS GameState::ChangeState prologue") &&
                                BytesAt(kState2SetEnabled, kState2Seq, sizeof(kState2Seq), "ChangeState(2) SetEnabled vcall") &&
                                CallAt(kState2CullCall, kQueueCullPlayer, kCullCall, "QueueCullPlayerGeometry call") &&
                                BytesAt(kQueueCullPlayer, kCullPro, sizeof(kCullPro), "QueueCullPlayerGeometry prologue");
                const bool slotOk = slot2 == Base() + kCamSetEnabled;
                const bool tqOk = kState2TaskQueueLoad + 7 + tqRel == kTaskQueueSingleton;
                if (ok && slotOk && tqOk) {
                    g_camSetEnabled = reinterpret_cast<SetEnabled_t>(Base() + kCamSetEnabled);
                    g_queueCullPlayer = reinterpret_cast<CullPlayer_t>(Base() + kQueueCullPlayer);
                    g_origVatsChangeState = reinterpret_cast<ChangeState_t>(Detour(kVatsChangeState, kChangeStatePro, sizeof(kChangeStatePro), 6,
                        reinterpret_cast<void*>(&HkVatsChangeState), "VATS GameState::ChangeState prologue"));
                    // ImageSpaceModifierInstance::Stop: mov rdx,[rip+..] ; add rdx,0A0h
                    static constexpr std::uint8_t kStopA[] = { 0x48, 0x8B, 0x15 };
                    static constexpr std::uint8_t kStopB[] = { 0x48, 0x81, 0xC2, 0xA0, 0x00, 0x00, 0x00 };
                    if (BytesAt(kImodInstStop, kStopA, sizeof(kStopA), "ImageSpaceModifierInstance::Stop prologue") &&
                        BytesAt(kImodInstStop + 7, kStopB, sizeof(kStopB), "ImageSpaceModifierInstance::Stop body")) {
                        g_imodInstStop = reinterpret_cast<ImodInstStop_t>(Base() + kImodInstStop);
                    }
                } else if (ok) {
                    REX::ERROR("camdir gate: VG2 skipped (PlayerCamera vtable slot 2 = {:X}, expected {:X}; task queue global {:X}, expected {:X})",
                        slot2 - Base(), kCamSetEnabled, kState2TaskQueueLoad + 7 + tqRel, kTaskQueueSingleton);
                }
            }
            note(kGVatsState, (c.gateVats && c.vatsKeepCamera) || g_vatsNoImod, g_origVatsChangeState != nullptr);
            note(kGStartVats, c.gateVats, g_origStartVats != nullptr);
            note(kGForceVats, c.gateVats, g_origForceVats != nullptr);
            note(kGStopVats, c.gateVats, g_origStopVats != nullptr);
            if (!g_origStartVats) c.gateVats = false;  // VATSMenu keeps the cinema fallback without the camera gate
            if (c.pipboyStopTick) {
                if (CallAt(kPipboyStopCall, kCameraUpdate, kPipboyStopBytes, "StopPipboyMode TESCamera::Update call")) {
                    REL::GetTrampoline().write_call<5>(Base() + kPipboyStopCall, &HkUpdatePipboyStop);
                    g_gs[kGPipboyStop].patched = true;
                }
            }
            note(kGPipboyStop, c.pipboyStopTick, g_gs[kGPipboyStop].patched);
            rs::SetHeadingSnapThreshold(c.headingSnap ? c.headingSnapDeg / kR2D : 0.0f);
            out += std::format(" headingSnap={} (>= {} deg)", c.headingSnap ? "on" : "off", c.headingSnapDeg);
            return out;
        }

        std::string TableString()
        {
            std::string s;
            for (int i = 0; i < kStates; ++i) {
                s += std::format("{}{}={}", i ? "," : "", kStateNames[i], g_d.sceneSt[i] ? "SCENE" : PolicyName(g_cfg.table[i]));
            }
            return s;
        }
    }

    const char* PolicyName(Policy a_p)
    {
        switch (a_p) {
        case Policy::kTrack: return "TRACK";
        case Policy::kFadeSnap: return "FADE_SNAP";
        case Policy::kLock: return "LOCK";
        case Policy::kScene: return "SCENE";
        default: return "CINEMA";
        }
    }

    void ReloadLive(const cfgstore::Changes& a_ch);  // 20 Nov (live settings), below
    namespace
    {
        void InstallAftercare();  // below (scene aftercare: Pip-Boy refusals)
        const char* AftercareNote();
    }

    std::string Install()
    {
        static std::once_flag s_sub;
        std::call_once(s_sub, [] {
            cfgstore::Subscribe("camdir", "Camera,Menus/bCraftingLiveWorld,Menus/bWorkbenchPause,General/fUnitsPerMeter,General/fHeightOffsetCm",
                &ReloadLive);
        });
        std::scoped_lock l(g_lock);
        if (g_d.installed) {
            return "already installed";
        }
        const auto path = Settings::Path();
        Cfg&       c = g_cfg;
        auto rdf = [&](const char* sec, const char* k, float def) {
            char buf[64]{};
            ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), path.c_str());
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : def;
        };
        c.enabled = ::GetPrivateProfileIntA("Camera", "bCameraDirector", 1, path.c_str()) != 0;
        c.fadeMs = rdf("Camera", "fFadeMs", c.fadeMs);
        c.timeoutS = rdf("Camera", "fFadeTimeoutS", c.timeoutS);
        c.jumpUnits = rdf("Camera", "fJumpFadeUnits", c.jumpUnits);
        c.unitsPerMeter = rdf("General", "fUnitsPerMeter", 70.0f);
        if (!(c.unitsPerMeter > 1.0f)) c.unitsPerMeter = 70.0f;
        c.heightOffsetCm = rdf("General", "fHeightOffsetCm", 0.0f);
        auto rdb = [&](const char* k, bool def) { return ::GetPrivateProfileIntA("Camera", k, def ? 1 : 0, path.c_str()) != 0; };
        c.gateFurniture = rdb("bGateFurniture", false);
        c.gatePowerArmor = rdb("bGatePowerArmor", false);
        c.gateAnimated = rdb("bGateAnimated", false);
        c.workbenchDirect = rdb("bWorkbenchDirectMenu", true);
        c.blockThirdPerson = rdb("bBlockThirdPerson", false);
        c.gateVats = rdb("bGateVats", false);
        c.pipboyStopTick = rdb("bPipboyStopTick", false);
        c.vatsKeepCamera = rdb("bVatsKeepCamera", false);
        // [Menus] bVatsLiveWorld (default 0 since 15 Nov round 4: the live-world VATS moved/zoomed the view = nausea; VATS is
        // the flat VATS window with the world panorama around it): 1 = both VATS gates on (no VATS camera push, player
        // camera kept enabled); the Presenter lays the VATS overlay over the world view.
        if (::GetPrivateProfileIntA("Menus", "bVatsLiveWorld", 0, path.c_str()) != 0) {
            c.gateVats = true;
            c.vatsKeepCamera = true;
        }
        // [Menus] bVatsCameraWindow (round 5, VatsWindow.h): the VATS window shows our own targeting camera while the
        // head-tracked world stays live around it -> the same two VATS gates (no VATS camera push, player camera kept)
        if (vatswin::ConfiguredOn()) {
            c.gateVats = true;
            c.vatsKeepCamera = true;
        }
        // [Menus] bVatsNoImod (default 1): no flat VATS image-space modifier (VATSScanImod: tint + radial blur) - in the
        // live-world mode and in the VATS window
        g_vatsNoImod = ::GetPrivateProfileIntA("Menus", "bVatsNoImod", 1, path.c_str()) != 0;
        // [Menus] bWorkbenchPause (round 6, default 1): crafting menus pause the game like FO4VR (kPausesGame)
        // pause only with the FO4VR workbench path (bCraftingLiveWorld=1): with the game's own workbench camera the paused
        // sit never completes, so "Exit Workbench?" never leaves the menu (user 15 Nov 16:45)
        g_workbenchPause = ::GetPrivateProfileIntA("Menus", "bWorkbenchPause", 1, path.c_str()) != 0 &&
                           ::GetPrivateProfileIntA("Menus", "bCraftingLiveWorld", 1, path.c_str()) != 0;
        // [Menus] bCraftingLiveWorld (default 1 since round 4, user: "do what FO4VR does: no animation, just the menu"):
        // workbenches / crafting stations / chairs keep the first-person camera like FO4VR (StartFurnitureMode -> HUD mode
        // only; workbenches open their menu directly, [Camera] bWorkbenchDirectMenu), so their menus (Cooking, Examine,
        // Workshop, PowerArmorMod) are panels over the live head-tracked world.
        // 15 Nov (user, headset 18:46: "bBlockThirdPerson=0 with bCraftingLiveWorld=1 was the exact behaviour I wanted"):
        // bCraftingLiveWorld no longer forces the third-person block; [Camera] bBlockThirdPerson alone decides (default 0).
        if (::GetPrivateProfileIntA("Menus", "bCraftingLiveWorld", 1, path.c_str()) != 0) {
            c.gateFurniture = true;
            c.gateAnimated = true;
        }
        c.headingSnap = rdb("bHeadingSnap", false);
        c.headingSnapDeg = rdf("Camera", "fHeadingSnapDeg", c.headingSnapDeg);
        c.paHoldS = rdf("Camera", "fPowerArmorHoldS", c.paHoldS);
        c.freeScene = rdb("bFreeCamScene", true);
        c.freeBody = rdb("bFreeCamBody", true);
        c.freeYawCutDeg = rdf("Camera", "fFreeCamYawCutDeg", c.freeYawCutDeg);
        c.freeGlideSpeed = rdf("Camera", "fFreeCamGlideSpeed", c.freeGlideSpeed);
        c.freeGlideHoldS = rdf("Camera", "fFreeCamGlideHoldS", c.freeGlideHoldS);
        c.thirdGraceMs = rdf("Camera", "fThirdPersonGraceMs", c.thirdGraceMs);
        c.freeAafThird = rdb("bFreeCamAAFThirdPerson", true);
        c.freeMoveView = rdb("bFreeCamMoveWithView", true);
        c.freeHeadDelta = rdb("bFreeCamHeadDelta", true);
        c.thirdReturn = rdb("bThirdPersonReturn", true);
        c.thirdReturnS = rdf("Camera", "fThirdPersonReturnS", c.thirdReturnS);
        c.lockFp = rdb("bLockFirstPerson", false);
        c.bodyCam = rdb("bBodyCamera", false);
        c.hudNotify = rdb("bCameraHudNotify", true);
        c.sameInput = rdb("bSameInputAllViews", true);
        c.lockScenes = rdb("bLockAllowScenes", true);
        c.defeatWindowS = rdf("Camera", "fDefeatWindowS", c.defeatWindowS);
        c.orbitScene = rdb("bOrbitStatesScene", true);
        c.freeOwnStick = rdb("bFreeCamOwnStickYaw", true);
        c.aftercare = rdb("bSceneAftercare", true);
        c.bodyFollow = rdb("bBodyCameraFollowsAnimation", true);
        c.bodyEyeFwd = rdf("Camera", "fBodyCameraEyeForward", c.bodyEyeFwd);
        c.bodyYieldPov = rdb("bBodyCameraYieldsToPov", true);
        c.freeStatePos = rdb("bFreeCamStatePosition", true);
        for (int i = 0; i < kStates; ++i) {
            c.table[i] = kDefaultTable[i];
            char buf[32]{};
            ::GetPrivateProfileStringA("Camera", std::format("sPolicy{}", kStateNames[i]).c_str(), "", buf, sizeof(buf), path.c_str());
            Policy p;
            if (buf[0] && ParsePolicy(buf, p)) {
                c.table[i] = p;
            } else if (buf[0]) {
                REX::WARN("camdir: [Camera] sPolicy{}={} not understood (TRACK|FADE_SNAP|LOCK|CINEMA); default {}", kStateNames[i], buf,
                    PolicyName(kDefaultTable[i]));
            }
        }
        if (!c.enabled) {
            c.gateFurniture = c.gatePowerArmor = c.gateAnimated = c.blockThirdPerson = c.gateVats = c.pipboyStopTick = c.headingSnap = false;
            c.vatsKeepCamera = false;
            c.lockFp = c.bodyCam = false;
            g_d.installNote = "off (bCameraDirector=0)";
            REX::INFO("camdir: off (bCameraDirector=0): no patch; presenter keeps first person = world view, rest = cinema");
            return g_d.installNote;
        }
        const auto base = Base();
        const bool callOk = std::memcmp(reinterpret_cast<const void*>(base + kCallSite), kCallBytes, 5) == 0;
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(base + kCallSite + 1), 4);
        const bool targetOk = base + kCallSite + 5 + rel == base + kCameraUpdate;
        const bool prologueOk = std::memcmp(reinterpret_cast<const void*>(base + kCameraUpdate), kPrologue, sizeof(kPrologue)) == 0;
        if (!callOk || !targetOk || !prologueOk) {
            c.enabled = false;
            g_d.installNote = "byte check FAILED";
            REX::ERROR("camdir: byte check failed (call {:X} {}, target {}, prologue {:X} {}); NOT patching", kCallSite, callOk, targetOk,
                kCameraUpdate, prologueOk);
            return g_d.installNote;
        }
        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) {
            tramp.create(64);
        }
        g_orig = reinterpret_cast<Update_t>(tramp.write_call<5>(base + kCallSite, &HkUpdate));
        g_d.installed = true;
        g_d.installNote = "installed";
        const auto gates = InstallGates();
        InstallAftercare();  // always (gated at runtime by bSceneAftercare)
        // free-camera scene view (bFreeCamScene): redirect the free camera's TESCamera::Update thunk to the same post-tick and
        // make state 3 a TRACK world view (overrides sPolicyFree, as bCraftingLiveWorld overrides the gates). Byte check:
        // the thunk is exactly E9 rel32 landing on TESCamera::Update (prologue checked above); any mismatch = not patched,
        // state 3 keeps sPolicyFree.
        {  // 20 Nov (live settings): patched whatever bFreeCamScene says; g_freeSceneLive / FreeOn() switch it at runtime
            const bool thunkOk = std::memcmp(reinterpret_cast<const void*>(base + kFreeCamThunk), kFreeCamThunkBytes, 5) == 0;
            std::int32_t trel = 0;
            std::memcpy(&trel, reinterpret_cast<const void*>(base + kFreeCamThunk + 1), 4);
            const bool thunkTarget = base + kFreeCamThunk + 5 + trel == base + kCameraUpdate;
            if (!thunkOk || !thunkTarget) {
                g_d.freeNote = "byte check FAILED";
                REX::ERROR("camdir: free-camera thunk {:X} byte check failed (bytes {}, target {}); free camera keeps sPolicyFree={}",
                    kFreeCamThunk, thunkOk, thunkTarget, PolicyName(c.table[kStateFree]));
            } else if (tramp.free_size() < 32) {
                g_d.freeNote = "trampoline full";
                REX::ERROR("camdir: free-camera thunk not patched: trampoline has {} bytes free", tramp.free_size());
            } else {
                tramp.write_jmp<5>(base + kFreeCamThunk, &HkUpdateFreeCam);
                g_d.freeHooked = true;
                g_d.freeNote = "installed";
                // stick flying along the view: FreeCameraState::Update's call to UpdateMovement (bytes + target + callee prologue)
                if (true) {  // 20 Nov: installed always; g_cfg.freeMoveView gates HkFreeCamMove at runtime
                    constexpr std::uintptr_t kMoveCall = 0x1022435, kMove = 0x1022B60;
                    static constexpr std::uint8_t kMoveCallBytes[5] = { 0xE8, 0x26, 0x07, 0x00, 0x00 };
                    static constexpr std::uint8_t kMovePro[] = { 0x48, 0x8B, 0xC4 };
                    std::int32_t mrel = 0;
                    std::memcpy(&mrel, reinterpret_cast<const void*>(base + kMoveCall + 1), 4);
                    const bool ok = std::memcmp(reinterpret_cast<const void*>(base + kMoveCall), kMoveCallBytes, 5) == 0 &&
                                    base + kMoveCall + 5 + mrel == base + kMove &&
                                    std::memcmp(reinterpret_cast<const void*>(base + kMove), kMovePro, sizeof(kMovePro)) == 0;
                    if (ok && tramp.free_size() >= 16) {
                        g_origFreeMove = reinterpret_cast<FreeMove_t>(tramp.write_call<5>(base + kMoveCall, &HkFreeCamMove));
                    } else {
                        c.freeMoveView = false;
                        REX::ERROR("camdir: free-camera UpdateMovement call {:X} not patched (bytes/target/prologue {}, trampoline {} free)",
                            kMoveCall, ok, tramp.free_size());
                    }
                }
                if (c.freeScene) {
                    if (c.table[kStateFree] != Policy::kScene) {
                        REX::INFO("camdir: bFreeCamScene=1: state Free policy {} -> SCENE (free-camera scene view)", PolicyName(c.table[kStateFree]));
                    }
                    c.table[kStateFree] = Policy::kScene;
                } else {
                    g_d.freeNote = "installed, off (bFreeCamScene=0; live switch)";
                }
            }
        }
        g_freeSceneLive = c.freeScene && g_d.freeHooked;
        // SCENE states: TRACK + the scene flag. The free camera can only be SCENE with its thunk patched (no director tick in
        // state 3 otherwise: a world locked to the face), else it falls back to CINEMA.
        for (int i = 0; i < kStates; ++i) {
            g_d.sceneSt[i] = false;
            if (c.orbitScene && OrbitState(i) && c.table[i] == Policy::kTrack) {
                REX::INFO("camdir: sPolicy{}=TRACK on an orbit camera -> SCENE ([Camera] bOrbitStatesScene; TRACK head-rotates the "
                          "game's orbit camera, so the view and the movement followed the head)", kStateNames[i]);
                c.table[i] = Policy::kScene;
            }
            if (c.table[i] == Policy::kScene && (i == 0 || i == 4)) {
                // first person / iron sights are the FirstPersonState path (head authority, roomscale, FRIK); SCENE there would
                // drop the head-tracked first-person camera and switch the controls to the flat-screen mapping
                REX::WARN("camdir: sPolicy{}=SCENE is not allowed (first-person path): TRACK", kStateNames[i]);
                c.table[i] = Policy::kTrack;
                continue;
            }
            if (c.table[i] == Policy::kScene) {
                if (i == kStateFree && !FreeOn()) {
                    c.table[i] = Policy::kCinema;
                    REX::WARN("camdir: sPolicyFree=SCENE needs the free-camera thunk ({}): CINEMA", g_d.freeNote);
                    continue;
                }
                g_d.sceneSt[i] = true;
                c.table[i] = Policy::kTrack;
            }
        }
        game::camtweak::SetForced(c.lockFp);
        REX::INFO("camdir: first-person lock {} ([Camera] bLockFirstPerson), body camera {} (bBodyCamera), HUD notifications {} "
                  "(bCameraHudNotify), same controls in every view {} (bSameInputAllViews)",
            c.lockFp ? "ON" : "off", c.bodyCam ? "ON" : "off", c.hudNotify ? "on" : "off", c.sameInput ? "on" : "off");
        REX::INFO("camdir: body camera follows the animated Head bone {} (bBodyCameraFollowsAnimation; eye {} u along the face, "
                  "fBodyCameraEyeForward), yields to AAF's POV on another actor {} (bBodyCameraYieldsToPov); free camera position "
                  "from the state (after AAF's write) {} (bFreeCamStatePosition)",
            c.bodyFollow ? "on" : "off", c.bodyEyeFwd, c.bodyYieldPov ? "on" : "off", c.freeStatePos ? "on" : "off");
        REX::INFO("camdir: AAF / defeat scenes pass the camera locks {} (bLockAllowScenes; defeat window {} s after a bleedout), orbit "
                  "cameras TRACK -> SCENE {} (bOrbitStatesScene), free-camera stick yaw kept as our own {} (bFreeCamOwnStickYaw), "
                  "scene aftercare {} (bSceneAftercare: {})",
            c.lockScenes ? "on" : "off", c.defeatWindowS, c.orbitScene ? "on" : "off", c.freeOwnStick ? "on" : "off",
            c.aftercare ? "on" : "off", AftercareNote());
        const auto s = std::format("call {:X} -> TESCamera::Update {:X} hooked (bytes ok); fade {} ms, timeout {} s; table {}; step-2 gates:{}; "
                                   "free-camera scene view (thunk {:X}): {} (body {}, yaw cut {} deg, glide {} units/s hold {} s, "
                                   "third-person grace {} ms); third-person return {} after {} s (Force1stPerson {}); bench input release {}",
            kCallSite, kCameraUpdate, c.fadeMs, c.timeoutS, TableString(), gates, kFreeCamThunk, g_d.freeNote, c.freeBody ? 1 : 0,
            c.freeYawCutDeg, c.freeGlideSpeed, c.freeGlideHoldS, c.thirdGraceMs, c.thirdReturn ? "on" : "off", c.thirdReturnS,
            g_force1st ? "ok" : "MISSING", g_reenableInput ? "ok" : "MISSING");
        REX::INFO("camdir: {}", s);
        return s;
    }

    namespace
    {
        // ---- Scene aftercare: Pip-Boy refusals (20 Nov late, user: "After AAF animation I was locked out of pip boy and unable to
        // reactivate it"; [Camera] bSceneAftercare) ----
        // The user's 22:01-23:00 session: Pip-Boy opened 30+ times until 22:39:58, never again after the first AAF scene (22:41);
        // no "pipboy: open" line, so the game's PipboyHandler refused before PipboyManager::PlayPipboyOpenAnim (our O1 hook).
        // Its refusals (1.11.240 Ghidra, ButtonEvent handler 1077010 = PipboyHandler vtable slot 25B1F38):
        //   - PlayerCharacter::GetUnderAIControl (D6A3D0: player+0xDFE bit 0 / bit 1, +0xDFD bit 7) -> silent, the press never
        //     arms. Bit 1 is PlayerCharacter::SetAIControlledPackage (D6F180) = Papyrus Game.SetPlayerAIDriven (1129... jmp D6F180):
        //     AAF sets it for its walk and clears it last in its unlock (after its fly-cam loop); a scene that never reaches that
        //     line leaves the Pip-Boy refused while the player walks again (movement controller back to controls-driven).
        //   - user event kMainFour (0x100) disabled by an input layer (InputEnableLayer.DisablePlayerControls abMenu ->
        //     EnableUserEvent(id, 0x100); defeat mods: AAF Violate FPV_OnHit Surrender / AnimationEnd, Harassment) -> HUD
        //     "can't use the Pip-Boy"; a layer orphaned by a broken scene keeps it.
        //   - graph variable bBlockPipboy, dead / dying, power-armour state, commanding a companion.
        //   - PipboyManager +0x1C6 (opening) already set: PlayPipboyOpenAnim returns at once (the original and our O1 alike).
        // At a refused Pip-Boy press the blocker is logged (+ HUD); leftovers of a scene that is over (no AAF / defeat signal for
        // 5 s, camera first person) are released before the handler runs, so that same press opens the Pip-Boy:
        //   AI control bit 1 with the movement controller controls-driven -> SetAIControlledPackage(false) (what
        //   Game.SetPlayerAIDriven(false) runs); kMainFour layers that started disabling during a scene -> EnableUserEvent(id,
        //   0x100, true, 3) (Papyrus EnableMenu's call); the opening flag stuck for 1 s with no Pip-Boy menu -> cleared.
        constexpr std::uintptr_t kPipboyHandlerSlot = 0x25B1F38;  // PipboyHandler vtable: OnButtonEvent
        constexpr std::uintptr_t kPipboyHandlerFn = 0x1077010;
        constexpr std::uintptr_t kIsValidPipboyEvent = 0x1077560;  // PipboyHandler::IsValidPipboyOpenEvent(handler, BSFixedString*)
        constexpr std::uintptr_t kUserEventEnabled = 0x1668C20;    // BSInputEnableManager::IsUserEventEnabled(mgr, flags)
        constexpr std::uintptr_t kEnableUserEvent = 0x166A620;     // BSInputEnableManager::EnableUserEvent(mgr, id, flags, on, sender)
        constexpr std::uintptr_t kPipboyMgrPtr = 0x31F0888;        // PipboyManager*
        constexpr std::size_t    kPbOpening = 0x1C6, kPbClosing = 0x1C7;
        constexpr std::uint32_t  kMainFour = 0x100;
        static constexpr std::uint8_t kEnableUserEventPro[] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x45, 0x88, 0x4B, 0x20 };
        static constexpr std::uint8_t kPbHandlerPro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };

        using PbHandle_t = void (*)(void*, void*, void*);
        PbHandle_t g_origPbHandle = nullptr;

        struct LayerRec
        {
            bool disabled{ false }, sceneBorn{ false };
        };
        struct Aftercare
        {
            bool                  installed{ false };
            const char*           note{ "not installed" };
            std::vector<LayerRec> layers;
            Clock::time_point     lastPoll{}, flowSeen{}, lastHud{}, openingSeen{};
            bool                  haveFlow{ false }, openingStuck{ false };
            std::uint64_t         presses{ 0 }, refusals{ 0 }, aiReleases{ 0 }, layerReleases{ 0 }, openingClears{ 0 }, sceneLayers{ 0 };
            std::string           lastWhy{ "-" }, lastFix{ "-" };
        };
        Aftercare  g_ac;      // main thread (PipboyHandler events, main tick); readout under g_acLock
        std::mutex g_acLock;
        std::atomic<std::uint64_t> g_pbEvents{ 0 };     // ButtonEvents the PipboyHandler received (any button it is offered)
        std::atomic<void*>         g_pbHandler{ nullptr };
        // pad B (our bindings' Pip-Boy button in gameplay) pressed and released with no PipboyHandler event: the press never
        // reached the handler (its inputEventHandlingEnabled off, or an input context on the ControlMap stack that maps B to
        // something else, e.g. a free-camera TFC context left pushed). Main tick, every frame.
        struct PadWatch
        {
            bool              down{ false }, pending{ false };
            std::uint64_t     ev0{ 0 };
            Clock::time_point up{};
            std::uint64_t     unreached{ 0 };
            std::string       last{ "-" };
        };
        PadWatch g_pw;

        std::uint8_t* PipboyMgr() { return *reinterpret_cast<std::uint8_t**>(Base() + kPipboyMgrPtr); }
        bool UserEventEnabled(std::uint32_t a_flags)
        {
            auto* mgr = RE::BSInputEnableManager::GetSingleton();
            using En_t = bool (*)(void*, std::uint32_t);
            return !mgr || reinterpret_cast<En_t>(Base() + kUserEventEnabled)(mgr, a_flags);
        }
        int CameraStateNow()
        {
            auto* pc = RE::PlayerCamera::GetSingleton();
            return pc && pc->currentState ? static_cast<int>(pc->currentState->id.get()) : -1;
        }
        std::string LayerName(RE::BSInputEnableManager* a_mgr, std::uint32_t a_i)
        {
            if (a_i < a_mgr->debugNames.size()) {
                const char* s = a_mgr->debugNames[a_i].c_str();
                if (s && *s) return s;
            }
            return "?";
        }

        // the scene is over: no AAF / defeat signal for 5 s, first person, not sitting (main thread; g_acLock held)
        bool SceneOverLocked(Clock::time_point a_now, float a_defeatWindowS)
        {
            if (SceneFlow(a_defeatWindowS)) {
                g_ac.flowSeen = a_now;
                g_ac.haveFlow = true;
                return false;
            }
            const int cs = CameraStateNow();
            auto* player = RE::PlayerCharacter::GetSingleton();
            const int sit = player ? static_cast<int>(static_cast<RE::ActorState*>(player)->DoGetSitSleepState()) : -1;
            return (cs == 0 || cs == 4) && sit == 0 && (!g_ac.haveFlow || Ms(g_ac.flowSeen, a_now) >= 5000.0);
        }

        // the Pip-Boy blockers now, in the handler's order (a_press: the press, where AI control stops the handler; else the
        // armed release). Releases a scene's leftovers when a_fix. Returns "" when nothing blocks. Main thread, g_acLock held.
        std::string PipboyBlockers(bool a_press, bool a_fix, float a_defeatWindowS, std::string& a_fixed)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return {};
            const auto  now = Clock::now();
            auto*       pb = reinterpret_cast<std::uint8_t*>(player);
            const auto  dfe = pb[0xDFE], dfd = pb[0xDFD];
            const auto* mc = player->movementController.get();
            const bool  ctrlDriven = mc && *(reinterpret_cast<const std::uint8_t*>(mc) + 0x1A1) == 1;
            const bool  over = SceneOverLocked(now, a_defeatWindowS);
            std::string why;
            if ((dfe & 3) || (dfd & 0x80)) {
                why = std::format("AI control (player+DFE={:02X} DFD={:02X}: {}; movement {})", dfe, dfd,
                    (dfe & 2) ? "Game.SetPlayerAIDriven / AI package" : "AI move-to", ctrlDriven ? "controls-driven" : "AI-driven");
                if (a_fix && over && (dfe & 2) && !(dfe & 1) && !(dfd & 0x80) && ctrlDriven && g_setAiCtrlPkg) {
                    g_setAiCtrlPkg(player, false);  // PlayerCharacter::SetAIControlledPackage(false) = Game.SetPlayerAIDriven(false)
                    ++g_ac.aiReleases;
                    a_fixed = "the player's AI control (Game.SetPlayerAIDriven) left over from a scene";
                    return {};
                }
                return why;
            }
            if (a_press) return {};
            if (!UserEventEnabled(kMainFour)) {
                auto*       mgr = RE::BSInputEnableManager::GetSingleton();
                std::string list;
                std::vector<std::uint32_t> fix;
                if (mgr) {
                    const std::uint32_t n = mgr->layers.size();
                    for (std::uint32_t i = 0; i < n; ++i) {
                        const auto mask = *reinterpret_cast<const std::uint32_t*>(&mgr->layers[i]);
                        if (mask & kMainFour) continue;
                        const bool born = i < g_ac.layers.size() && g_ac.layers[i].sceneBorn;
                        list += std::format("{}{} '{}'{}", list.empty() ? "" : ", ", i, LayerName(mgr, i), born ? " (scene)" : "");
                        if (born) fix.push_back(i);
                    }
                }
                why = std::format("Pip-Boy input disabled by input layer(s) {}", list.empty() ? "-" : list);
                if (a_fix && over && mgr && !fix.empty()) {
                    using Enable_t = void (*)(void*, std::uint32_t, std::uint32_t, bool, std::uint32_t);
                    for (const auto i : fix) {
                        reinterpret_cast<Enable_t>(Base() + kEnableUserEvent)(mgr, i, kMainFour, true, 3);
                        if (i < g_ac.layers.size()) g_ac.layers[i].sceneBorn = false;
                        ++g_ac.layerReleases;
                    }
                    if (UserEventEnabled(kMainFour)) {
                        a_fixed = std::format("{} scene input layer(s) that kept the Pip-Boy disabled", fix.size());
                        why.clear();
                    }
                }
                if (!why.empty()) return why;
            }
            const auto ls = static_cast<const RE::ActorState*>(player)->lifeState;
            if (ls == 1 || ls == 2 || ls == 4 || ls == 5) return std::format("player life state {} (dead / dying)", ls);
            bool block = false;
            static const RE::BSFixedString kBlock("bBlockPipboy");
            if (static_cast<RE::IAnimationGraphManagerHolder*>(player)->GetGraphVariableImplBool(kBlock, block) && block) {
                return "the behaviour graph's bBlockPipboy is set (an idle / furniture animation state)";
            }
            if (auto* m = PipboyMgr(); m && m[kPbOpening] && !m[kPbClosing] && !game::MenuOpen("PipboyMenu")) {
                if (!a_fix) {
                    // readout
                } else if (!g_ac.openingStuck) {
                    g_ac.openingStuck = true;
                    g_ac.openingSeen = now;
                } else if (a_fix && Ms(g_ac.openingSeen, now) >= 1000.0) {
                    m[kPbOpening] = 0;  // PlayPipboyOpenAnim returns at once while it is set
                    g_ac.openingStuck = false;
                    ++g_ac.openingClears;
                    a_fixed = "the Pip-Boy manager's stuck 'opening' flag";
                    return {};
                }
                return "PipboyManager 'opening' is set with no Pip-Boy menu (an open that never finished)";
            }
            if (a_fix) g_ac.openingStuck = false;
            return {};
        }

        void HkPipboyHandle(void* a_h, void* a_ev, void* a_data)
        {
            bool  on = false;
            float window = 30.0f;
            {
                std::scoped_lock l(g_lock);
                on = g_cfg.aftercare;
                window = g_cfg.defeatWindowS;
            }
            auto* e = static_cast<std::uint8_t*>(a_ev);
            auto* h = static_cast<std::uint8_t*>(a_h);
            using Valid_t = bool (*)(void*, const void*);
            g_pbEvents.fetch_add(1, std::memory_order_relaxed);
            g_pbHandler = a_h;
            if (on && e && h && reinterpret_cast<Valid_t>(Base() + kIsValidPipboyEvent)(a_h, e + 0x28)) {
                const float value = *reinterpret_cast<const float*>(e + 0x38), held = *reinterpret_cast<const float*>(e + 0x3C);
                const bool  press = value != 0.0f && held == 0.0f;
                const bool  armedRelease = value == 0.0f && held > 0.0f && h[0x11] && !h[0x10];
                if (press || armedRelease) {
                    std::string fixed, why;
                    {
                        std::scoped_lock l(g_acLock);
                        if (press) ++g_ac.presses;
                        why = PipboyBlockers(press, true, window, fixed);
                        if (!fixed.empty()) g_ac.lastFix = fixed;
                        if (!why.empty()) {
                            ++g_ac.refusals;
                            g_ac.lastWhy = why;
                        }
                    }
                    const auto now = Clock::now();
                    if (!fixed.empty()) {
                        REX::INFO("camdir aftercare: Pip-Boy {}: released {} (bSceneAftercare)", press ? "press" : "release", fixed);
                        game::HudMessage("Pip-Boy: released " + fixed);
                    }
                    if (!why.empty()) {
                        REX::INFO("camdir aftercare: Pip-Boy {} refused by the game: {}", press ? "press" : "release", why);
                        std::scoped_lock l(g_acLock);
                        if (!g_ac.lastHud.time_since_epoch().count() || Ms(g_ac.lastHud, now) >= 3000.0) {
                            g_ac.lastHud = now;
                            game::HudMessage("Pip-Boy blocked: " + why);
                        }
                    }
                }
            }
            g_origPbHandle(a_h, a_ev, a_data);
        }

        // 4 Hz, main thread: which input layers start disabling the Pip-Boy (kMainFour) while an AAF / defeat scene runs
        void PollAftercare()
        {
            float window = 30.0f;
            bool  on = false;
            int   st = -1;
            {
                std::scoped_lock l(g_lock);
                on = g_cfg.aftercare && g_ac.installed;
                window = g_cfg.defeatWindowS;
                st = g_d.state;
            }
            if (!on) return;
            const auto now = Clock::now();
            std::scoped_lock l(g_acLock);
            {
                auto*      ui = RE::UI::GetSingleton();
                const auto ps = pad::Last();
                const bool gameplay = ui && ui->menuMode == 0 && (st == 0 || st == 4);
                if (ps.active && ps.b && !g_pw.down) {
                    g_pw.down = true;
                    g_pw.pending = false;
                    g_pw.ev0 = g_pbEvents.load();
                } else if (g_pw.down && !(ps.active && ps.b)) {
                    g_pw.down = false;
                    g_pw.pending = gameplay;
                    g_pw.up = now;
                }
                if (g_pw.pending && Ms(g_pw.up, now) >= 400.0) {
                    g_pw.pending = false;
                    if (g_pbEvents.load() == g_pw.ev0 && gameplay && !game::MenuOpen("PipboyMenu")) {
                        std::string ctx;
                        if (auto* cm = RE::ControlMap::GetSingleton()) {
                            for (std::uint32_t i = 0; i < cm->contextPriorityStack.size(); ++i) {
                                ctx += std::format("{}{}", i ? "," : "", static_cast<int>(*reinterpret_cast<const std::int32_t*>(&cm->contextPriorityStack[i])));
                            }
                        }
                        auto* hb = static_cast<const std::uint8_t*>(g_pbHandler.load());
                        g_pw.last = std::format("pad B (Pip-Boy) never reached the game's Pip-Boy handler: handler enabled={} input "
                                                "contexts (bottom..top, 0 gameplay, 10 TFC free camera)=[{}]",
                            hb ? (hb[0x8] ? "1" : "0") : "?", ctx);
                        ++g_pw.unreached;
                        REX::INFO("camdir aftercare: {}", g_pw.last);
                        if (!g_ac.lastHud.time_since_epoch().count() || Ms(g_ac.lastHud, now) >= 3000.0) {
                            g_ac.lastHud = now;
                            game::HudMessage("Pip-Boy blocked: the button did not reach the game's Pip-Boy handler (see log)");
                        }
                    }
                }
            }
            if (g_ac.lastPoll.time_since_epoch().count() && Ms(g_ac.lastPoll, now) < 250.0) return;
            g_ac.lastPoll = now;
            if (SceneFlow(window) || st == kStateFree) {
                g_ac.flowSeen = now;
                g_ac.haveFlow = true;
            }
            const bool recent = g_ac.haveFlow && Ms(g_ac.flowSeen, now) < 5000.0;
            auto* mgr = RE::BSInputEnableManager::GetSingleton();
            if (!mgr) return;
            const std::uint32_t n = mgr->layers.size();
            if (g_ac.layers.size() < n) g_ac.layers.resize(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const bool dis = !(*reinterpret_cast<const std::uint32_t*>(&mgr->layers[i]) & kMainFour);
                auto&      r = g_ac.layers[i];
                if (dis && !r.disabled) {
                    r.sceneBorn = recent;
                    if (recent) {
                        ++g_ac.sceneLayers;
                        REX::INFO("camdir aftercare: input layer {} '{}' disables the Pip-Boy during an AAF / defeat scene", i, LayerName(mgr, i));
                    }
                }
                if (!dis) r.sceneBorn = false;
                r.disabled = dis;
            }
        }

        void InstallAftercare()
        {
            if (g_ac.installed) return;
            const auto base = Base();
            auto** slot = reinterpret_cast<void**>(base + kPipboyHandlerSlot);
            if (*slot != reinterpret_cast<void*>(base + kPipboyHandlerFn) ||
                std::memcmp(reinterpret_cast<const void*>(base + kPipboyHandlerFn), kPbHandlerPro, sizeof(kPbHandlerPro)) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(base + kEnableUserEvent), kEnableUserEventPro, sizeof(kEnableUserEventPro)) != 0) {
                g_ac.note = "byte check FAILED";
                REX::ERROR("camdir aftercare: PipboyHandler slot {:X} / handler {:X} / EnableUserEvent {:X} not as expected: not installed",
                    kPipboyHandlerSlot, kPipboyHandlerFn, kEnableUserEvent);
                return;
            }
            DWORD old = 0;
            if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                g_ac.note = "VirtualProtect failed";
                REX::ERROR("camdir aftercare: VirtualProtect {:X} failed ({})", kPipboyHandlerSlot, ::GetLastError());
                return;
            }
            g_origPbHandle = reinterpret_cast<PbHandle_t>(*slot);
            *slot = reinterpret_cast<void*>(&HkPipboyHandle);
            ::VirtualProtect(slot, sizeof(void*), old, &old);
            g_ac.installed = true;
            g_ac.note = "installed";
            REX::INFO("camdir aftercare: PipboyHandler button handler hooked (vtable slot {:X}); Pip-Boy refusals are diagnosed and scene "
                      "leftovers released ([Camera] bSceneAftercare)", kPipboyHandlerSlot);
        }

        const char* AftercareNote() { return g_ac.note; }

        // pipe "camdir aftercare": main thread (graph variable / layer reads), no fix. Caller holds no lock.
        std::string AftercareStatus(float a_window)
        {
            std::scoped_lock l(g_acLock);
            std::string      fixed;
            const std::string why = PipboyBlockers(false, false, a_window, fixed);
            return std::format("aftercare={} presses={} refusals={} aiReleases={} layerReleases={} openingClears={} sceneLayers={} "
                               "lastWhy='{}' lastFix='{}' now='{}' pipboyEnabled={} handlerEvents={} unreached={} lastUnreached='{}'",
                g_ac.note, g_ac.presses, g_ac.refusals, g_ac.aiReleases, g_ac.layerReleases, g_ac.openingClears, g_ac.sceneLayers,
                g_ac.lastWhy, g_ac.lastFix, why.empty() ? "nothing blocks" : why, UserEventEnabled(kMainFour) ? 1 : 0, g_pbEvents.load(),
                g_pw.unreached, g_pw.last);
        }
    }

    namespace
    {
        // ---- POV head hide (PollHeadHide) ----
        struct HeadHide
        {
            bool                                   cfgRead{ false };
            bool                                   on{ true };
            float                                  radius{ 40.0f };
            RE::NiPointer<RE::Actor>               actor;          // whose head is hidden now
            RE::NiPointer<RE::Actor>               cand;           // in the radius on the last find, not hidden yet (dwell)
            int                                    candFinds{ 0 };
            headhide::Set                          culled;         // nodes this hid (HeadHideCore.h, shared with FRIK-flat C39)
            Clock::time_point                      lastFind{}, lastScan{};
            float                                  lastDist{ -1.0f };
            std::uint64_t                          hides{ 0 }, restores{ 0 }, noFace{ 0 };
            std::string                            last{ "-" };
            const char*                            why{ "-" };     // why nothing is hidden now (status)
        };
        HeadHide g_hh;

        // the actor's FaceGen head node, head-slot armour and head gear: HeadHideCore.h (one implementation with FRIK-flat C39)
        using headhide::kFaceNode;
        // AAF POV: the free camera (TFC) at the head bone. FirstPerson / IronSights (FRIK's own head, selfie), ThirdPerson
        // and every other orbit/scene state never hide a head (17 Nov, user: "ours is breaking the face")
        constexpr float kLeaveFactor = 1.5f;  // hysteresis: a hidden head is shown again beyond radius x this
        constexpr int   kDwellFinds = 2;      // 10 Hz finds in the radius before a head is hidden (~0.1-0.2 s)

        void HeadHideConfig()
        {
            if (g_hh.cfgRead) {
                return;
            }
            g_hh.cfgRead = true;
            const auto path = Settings::Path();
            g_hh.on = ::GetPrivateProfileIntA("Camera", "bPovHeadHide", 1, path.c_str()) != 0;
            char buf[64]{};
            ::GetPrivateProfileStringA("Camera", "fPovHeadRadius", "", buf, sizeof(buf), path.c_str());
            char* end = nullptr;
            const float r = std::strtof(buf, &end);
            if (end != buf && r > 1.0f) g_hh.radius = r;
            REX::INFO("camdir: POV head hide {} (radius {} units: the free camera's scene view within that of an actor's Head bone; never first person / selfie / third person)",
                g_hh.on ? "on" : "off (bPovHeadHide=0)", g_hh.radius);
        }

        RE::NiAVObject* ActorRoot(RE::Actor* a)
        {
            if (!a) return nullptr;
            return a == RE::PlayerCharacter::GetSingleton() ? static_cast<RE::TESObjectREFR*>(a)->Get3D(false) : a->Get3D();
        }

        // Whole head or nothing (17 Nov): see HeadHideCore.h. Returns false (nothing culled) without a FaceGen node.
        bool HeadHideScan(RE::Actor* a)
        {
            return g_hh.culled.Scan(a, ActorRoot(a));
        }

        void HeadHideRestore(const char* a_why)
        {
            if (!g_hh.actor && g_hh.culled.Empty()) {
                return;
            }
            const auto n = g_hh.culled.Restore();
            ++g_hh.restores;
            REX::INFO("camdir: POV head hide: {:08X} head shown again ({} nodes; {})", g_hh.actor ? g_hh.actor->GetFormID() : 0, n, a_why);
            g_hh.actor.reset();
        }

        // distance from the camera to the actor's Head bone, or -1 (no 3D / no Head bone / far from the actor)
        float HeadDist(RE::Actor* a, const V3& a_cam)
        {
            if (!a) return -1.0f;
            const auto& l = a->data.location;
            const float lx = l.x - a_cam.x, ly = l.y - a_cam.y, lz = l.z - a_cam.z;
            if (lx * lx + ly * ly + lz * lz > 400.0f * 400.0f) return -1.0f;  // the Head bone is within ~150 u of the actor's origin
            const auto* hw = HeadBoneWorld(ActorRoot(a));  // 21 Nov: the bone tree's Head first (not the facebones 'HEAD')
            if (!hw) return -1.0f;
            const auto& h = hw->translate;
            const float dx = h.x - a_cam.x, dy = h.y - a_cam.y, dz = h.z - a_cam.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // the actor whose Head bone the game camera sits at (nearest within a_radius), or null
        RE::NiPointer<RE::Actor> FindPovActor(const V3& a_cam, float a_radius, float& a_dist)
        {
            RE::NiPointer<RE::Actor> best;
            a_dist = -1.0f;
            float bestD = a_radius;
            auto consider = [&](RE::Actor* a) {
                const float d = HeadDist(a, a_cam);
                if (d >= 0.0f && d < bestD) {
                    bestD = d;
                    best.reset(a);
                    a_dist = d;
                }
            };
            consider(RE::PlayerCharacter::GetSingleton());
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                for (auto& hnd : pl->highActorHandles) {
                    if (auto a = hnd.get()) consider(a.get());
                }
            }
            return best;
        }

        // bBodyCameraYieldsToPov (21 Nov; user: "Tested with bBodyCamera=1 ... none of the POV AAF cameras actually follow an
        // actors head position"): bBodyCamera replaced every free-camera view, AAF's POV included. Rule now: in the free camera
        // the body camera is the player's own animated head (AAF's shots and the POV on the player alike); when AAF's camera
        // sits at ANOTHER actor's head (POV on an NPC: within fPovHeadRadius of that actor's Head bone for kDwellFinds finds,
        // released beyond radius x kLeaveFactor or when the player's head is the nearer one) the view is AAF's (SCENE: AAF's
        // POV point + head motion). Main tick, 10 Hz.
        struct BodyYieldScan
        {
            Clock::time_point        last{};
            RE::NiPointer<RE::Actor> cand;
            int                      finds{ 0 };
        };
        BodyYieldScan g_by;

        void PollBodyYield(int a_st, const V3& a_cam)
        {
            bool on, yielding;
            {
                std::scoped_lock l(g_lock);
                on = g_d.installed && g_cfg.enabled && g_cfg.bodyCam && g_cfg.bodyYieldPov && a_st == kStateFree;
                yielding = g_d.bodyYield;
            }
            auto set = [&](bool a_yield, const std::string& a_who) {
                std::scoped_lock l(g_lock);
                if (g_d.bodyYield == a_yield) return;
                g_d.bodyYield = a_yield;
                if (a_yield) {
                    ++g_d.bodyYields;
                    g_d.bodyYieldWho = a_who;
                }
                REX::INFO("camdir: body camera {} ({})", a_yield ? "yields to AAF's POV on another actor's head" : "back on the player's head",
                    a_who);
            };
            if (!on) {
                g_by.cand.reset();
                g_by.finds = 0;
                if (yielding) set(false, "not the body camera in the free camera");
                return;
            }
            const auto now = Clock::now();
            if (Ms(g_by.last, now) < 100.0) return;
            g_by.last = now;
            float d = -1.0f;
            auto  a = FindPovActor(a_cam, g_hh.radius * (yielding ? kLeaveFactor : 1.0f), d);
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool other = a && a.get() != player;
            if (!other) {
                g_by.cand.reset();
                g_by.finds = 0;
                if (yielding) set(false, a ? "camera at the player's head" : "camera left the actor's head");
                return;
            }
            g_by.finds = a.get() == g_by.cand.get() ? g_by.finds + 1 : 1;
            g_by.cand = a;
            if (!yielding && g_by.finds >= kDwellFinds) {
                set(true, std::format("{:08X} head at {:.1f} u", a->GetFormID(), d));
            }
        }
    }

    void PollHeadHide()
    {
        headhide::CullHook() = &enginecull::Set;  // 22 Nov: the POV head hide through the engine's SetAppCulled (EngineCull.h)
        PollAftercare();  // 20 Nov late: scene aftercare's input-layer watch (4 Hz; this main tick is camdir's)
        HeadHideConfig();
        bool active = false;
        V3   cam{};
        int  st = -1;
        {
            std::scoped_lock l(g_lock);
            st = g_d.state;
            // AAF POV = the free camera in its scene view only (17 Nov): never first person / iron sights (FRIK's head and
            // the selfie mirror body), third person or any other orbit / scene state
            active = g_hh.on && g_d.installed && g_cfg.enabled && st == kStateFree && g_d.sceneSt[st];
            cam = g_d.tg;  // the game camera's position (AAF's POV point), before the head offset
        }
        PollBodyYield(st, cam);
        bool bodyHead = false;  // bBodyCamera: the camera is at the player's own eye while the third-person body is shown
        {
            std::scoped_lock l(g_lock);
            bodyHead = g_hh.on && BodyAppliesLocked(st);
        }
        const char* why = !g_hh.on ? "off (bPovHeadHide=0)" : !active && !bodyHead ? "not the free-camera scene view" : nullptr;
        if (!why && gestures::SelfieState().active) {
            why = "selfie";
        }
        if (!why && bodyHead) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && g_hh.actor.get() != player) {
                if (g_hh.actor) HeadHideRestore("body camera: the player's own head");
                g_hh.actor.reset(player);
                g_hh.cand.reset();
                g_hh.candFinds = 0;
                g_hh.lastScan = {};
                ++g_hh.hides;
                g_hh.last = std::format("player (body camera, state {})", StateName(st));
            }
            g_hh.why = "-";
        }
        if (why) {
            g_hh.why = why;
            g_hh.cand.reset();
            g_hh.candFinds = 0;
            HeadHideRestore(why);
            return;
        }
        const auto now = Clock::now();
        if (!bodyHead && Ms(g_hh.lastFind, now) >= 100.0) {  // 10 Hz: the POV point moves with the head bone, actors do not swap often
            g_hh.lastFind = now;
            // a hidden head stays hidden out to radius x kLeaveFactor (no hide/show flicker at the edge); a head is hidden only
            // after kDwellFinds finds in a row inside the radius (a camera passing a head does not hide it)
            const float dCur = g_hh.actor ? HeadDist(g_hh.actor.get(), cam) : -1.0f;
            const bool  keep = g_hh.actor && dCur >= 0.0f && dCur <= g_hh.radius * kLeaveFactor;
            float       d = -1.0f;
            auto        a = FindPovActor(cam, g_hh.radius, d);
            if (keep && (!a || a.get() == g_hh.actor.get() || d >= dCur)) {
                g_hh.lastDist = dCur;
                g_hh.why = "-";
            } else {
                g_hh.lastDist = d;
                if (g_hh.actor) {
                    HeadHideRestore(a ? "view moved to another head" : "view left the head");
                }
                if (!a) {
                    g_hh.cand.reset();
                    g_hh.candFinds = 0;
                    g_hh.why = "camera not at a head";
                } else {
                    g_hh.candFinds = a.get() == g_hh.cand.get() ? g_hh.candFinds + 1 : 1;
                    g_hh.cand = a;
                    g_hh.why = "dwell";
                    if (g_hh.candFinds >= kDwellFinds) {
                        g_hh.actor = a;
                        g_hh.cand.reset();
                        g_hh.candFinds = 0;
                        g_hh.lastScan = {};
                        g_hh.why = "-";
                        ++g_hh.hides;
                        g_hh.last = std::format("{:08X} at {:.1f} u (state {})", a->GetFormID(), d, StateName(st));
                    }
                }
            }
        }
        if (!g_hh.actor) {
            return;
        }
        if (Ms(g_hh.lastScan, now) >= 500.0) {  // equipment / head parts can change during a scene (undress, re-equip)
            const bool first = g_hh.lastScan.time_since_epoch().count() == 0;
            g_hh.lastScan = now;
            const auto before = g_hh.culled.Size();
            const bool face = HeadHideScan(g_hh.actor.get());
            if (!face) {
                g_hh.why = "no FaceGen head node (nothing hidden)";
                if (first) ++g_hh.noFace;
            }
            if (first) {
                if (face) {
                    REX::INFO("camdir: POV head hide: view at the head of {} -> {} head nodes culled (FaceGen head + head gear)", g_hh.last,
                        g_hh.culled.Size() - before);
                } else {
                    REX::INFO("camdir: POV head hide: view at the head of {} but it has no {} node: nothing hidden (whole head or nothing)",
                        g_hh.last, kFaceNode);
                }
            }
        }
        g_hh.culled.Keep();  // keep them culled (a re-equip or another mod may clear the flag)
    }

    Policy ViewPolicy(int a_camState)
    {
        std::scoped_lock l(g_lock);
        if (!g_d.installed || !g_cfg.enabled) {
            return a_camState == 0 ? Policy::kTrack : Policy::kCinema;
        }
        if (a_camState == g_d.state) {
            return g_d.eff;
        }
        // the director has not seen this state yet (one frame): its table entry; FADE_SNAP already fades
        if (BodyAppliesLocked(a_camState)) {
            return Policy::kTrack;
        }
        Policy p = a_camState >= 0 && a_camState < kStates ? g_cfg.table[a_camState] : Policy::kCinema;
        if (a_camState == kStateThird && p == Policy::kCinema && FreeOn() && g_cfg.thirdGraceMs > 0.0f) {
            p = Policy::kFadeSnap;  // free-camera scene view: a new third-person state starts inside its fade grace (Tick)
        }
        return p == Policy::kFadeSnap && g_menuWindow.load() ? Policy::kCinema : p;
    }

    bool WorldView(int a_camState)
    {
        return ViewPolicy(a_camState) != Policy::kCinema;
    }

    bool SceneView(int a_camState)
    {
        std::scoped_lock l(g_lock);
        return g_d.installed && g_cfg.enabled && a_camState >= 0 && a_camState < kStates && g_d.sceneSt[a_camState] &&
               g_cfg.table[a_camState] == Policy::kTrack && !BodyAppliesLocked(a_camState);
    }

    bool BodyView(int a_camState)
    {
        std::scoped_lock l(g_lock);
        return BodyAppliesLocked(a_camState);
    }

    bool LockFirstPerson()
    {
        std::scoped_lock l(g_lock);
        return g_d.installed && g_cfg.enabled && g_cfg.lockFp;
    }

    bool SameInputAllViews()
    {
        std::scoped_lock l(g_lock);
        return g_cfg.sameInput;
    }

    bool GameCameraYaw(float& a_yaw)
    {
        std::scoped_lock l(g_lock);
        if (!g_d.installed || !g_cfg.enabled || !g_d.preYawOk || g_d.eff == Policy::kCinema) {
            return false;
        }
        a_yaw = g_d.preYaw;
        return true;
    }

    namespace
    {
        const char* const kStateLabels[kStates] = { "First person", "Idle orbit", "VATS", "Free camera", "Iron sights", "Transition",
            "Tween", "Animated camera", "Third person", "Furniture", "Mount", "Bleedout", "Dialogue camera" };

        // the view mode a state is shown in (g_lock held): its policy, the body camera, or an automatic rule that holds
        // (menu window, fade timeout); not the short blinks on cuts
        std::string ModeLabelLocked(int a_st)
        {
            if (a_st < 0 || a_st >= kStates) return "-";
            if (!g_d.installed || !g_cfg.enabled) return a_st == 0 ? "TRACK" : "CINEMA";
            if (BodyAppliesLocked(a_st)) return "TRACK on the body";
            if (g_d.sceneSt[a_st]) return "SCENE";
            const Policy p = g_cfg.table[a_st];
            if (p == Policy::kFadeSnap && g_menuWindow.load()) return "CINEMA (menu window)";
            if (p == Policy::kFadeSnap && a_st == g_d.state && g_f.timedOut) return "CINEMA (fade timeout)";
            return PolicyName(p);
        }
    }

    void PollNotify()
    {
        std::string msg;
        {
            std::scoped_lock l(g_lock);
            const int st = g_d.state;
            if (st < 0 || st >= kStates) {
                return;
            }
            const std::string mode = ModeLabelLocked(st);
            const auto        now = Clock::now();
            if (!g_refused.empty()) {
                const std::string r = std::move(g_refused);
                g_refused.clear();
                // the same refusal again within 3 s (a script retrying, AAF's 0.1 s timers): log only
                if (g_cfg.hudNotify && (r != g_refusedLastText || Ms(g_refusedAt, now) >= 3000.0)) {
                    msg = std::format("Camera: {} (first-person lock)", r);
                    g_refusedAt = now;
                    g_refusedLastText = r;
                    ++g_n.refusedShown;
                }
            }
            if (st != g_n.state || mode != g_n.mode) {
                const bool first = g_n.state == -100;
                const std::string change = st != g_n.state
                                               ? std::format("Camera: {} -> {} ({})", g_n.state >= 0 && g_n.state < kStates ? kStateLabels[g_n.state] : "-",
                                                     kStateLabels[st], mode)
                                               : std::format("Camera view: {} {} -> {}", kStateLabels[st], g_n.mode, mode);
                g_n.state = st;
                g_n.mode = mode;
                if (!first && g_cfg.hudNotify) {
                    msg = msg.empty() ? change : msg + "; " + change;
                    ++g_n.shown;
                    g_n.last = change;
                }
            }
        }
        if (msg.empty() || game::MenuOpen("LoadingMenu")) {
            return;
        }
        REX::INFO("camdir: HUD: {}", msg);
        game::HudMessage(msg);
    }

    bool CallerShouldShow(const std::string& a_result)
    {
        std::scoped_lock l(g_lock);
        // with the notifications on, a view change / state change is announced by PollNotify; errors and refusals are not
        return !g_cfg.hudNotify || a_result.rfind("error", 0) == 0 || a_result.rfind("refused", 0) == 0 || a_result.rfind("cycle:", 0) == 0;
    }

    bool VatsWorldView()
    {
        std::scoped_lock l(g_lock);
        return g_d.installed && g_cfg.enabled && g_cfg.gateVats && g_gs[kGStartVats].patched;
    }

    void SetMenuWindow(bool a_on)
    {
        if (g_menuWindow.exchange(a_on) != a_on) {
            REX::INFO("camdir: menu window {} (FADE_SNAP {})", a_on ? "on" : "off", a_on ? "-> CINEMA" : "as the table says");
        }
    }

    std::uint64_t Ticks()
    {
        std::scoped_lock l(g_lock);
        return g_d.ticks;
    }

    float FadeAlpha()
    {
        std::scoped_lock l(g_lock);
        Advance(Clock::now());
        return g_f.alpha;
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg == "aftercare") {  // 20 Nov late: Pip-Boy refusal diagnosis / scene leftovers ([Camera] bSceneAftercare)
            float window;
            {
                std::scoped_lock l(g_lock);
                window = g_cfg.defeatWindowS;
            }
            return tq::RunOnMainWait([window]() { return AftercareStatus(window); }, std::chrono::milliseconds(1000),
                "error: main thread did not answer (loading / paused?)");
        }
        std::scoped_lock l(g_lock);
        Cfg& c = g_cfg;
        if (a_arg == "on" || a_arg == "off") {
            if (!g_d.installed) {
                return std::format("error: director not installed ({})", g_d.installNote);
            }
            c.enabled = a_arg == "on";
            return std::format("director {}", c.enabled ? "on" : "off");
        }
        if (a_arg == "table") {
            return TableString();
        }
        if (a_arg.rfind("lock", 0) == 0 || a_arg.rfind("body", 0) == 0) {
            // 20 Nov: "camdir lock [on|off]" (bLockFirstPerson), "camdir body [on|off]" (bBodyCamera); runtime, not saved
            const bool lock = a_arg.rfind("lock", 0) == 0;
            const std::string v = a_arg.size() > 5 ? a_arg.substr(5) : std::string{};
            if (!v.empty() && v != "on" && v != "off" && v != "1" && v != "0") {
                return "error: camdir lock|body [on|off]";
            }
            if (!g_d.installed) {
                return std::format("error: director not installed ({})", g_d.installNote);
            }
            if (!v.empty()) {
                const bool on = v == "on" || v == "1";
                (lock ? c.lockFp : c.bodyCam) = on;
                if (lock) game::camtweak::SetForced(on);
                REX::INFO("camdir: {} {} (pipe / VR panel)", lock ? "first-person lock" : "body camera", on ? "ON" : "off");
            }
            return std::format("lock={} body={} lockForces={} lastLockForce='{}' bodyTicks={} bodyEyeH={:.1f} follow={} w={:.2f} want={} "
                               "anim={} disp={:.1f} sceneSignal={} snaps={} blends={} animMiss={} yield={} yields={} lastYield='{}' "
                               "statePosReads={} gates freeCam={}/{} bleedout={}/{} mount={}/{} 3rd={}/{} furniture={}/{} animated={}/{} "
                               "(calls/blocked)",
                c.lockFp ? 1 : 0, c.bodyCam ? 1 : 0, g_d.lockForces, g_d.lockLast, g_d.bodyTicks, g_d.bodyEyeH, c.bodyFollow ? 1 : 0,
                g_d.bodyFollowW, g_d.bodyFollowWant ? 1 : 0, g_d.bodyAnimOk ? 1 : 0, g_d.bodyDisp, g_d.bodySceneNow ? 1 : 0,
                g_d.bodyFollowSnaps, g_d.bodyFollowBlends, g_d.bodyAnimMiss, g_d.bodyYield ? 1 : 0, g_d.bodyYields, g_d.bodyYieldWho,
                g_d.freeStateReads, g_gs[kGFreeCam].calls,
                g_gs[kGFreeCam].blocked, g_gs[kGBleedout].calls, g_gs[kGBleedout].blocked, g_gs[kGMount].calls, g_gs[kGMount].blocked,
                g_gs[kGSet3rd].calls + g_gs[kGForce3rd].calls, g_gs[kGSet3rd].blocked + g_gs[kGForce3rd].blocked, g_gs[kGFurniture].calls,
                g_gs[kGFurniture].blocked, g_gs[kGAnimated].calls, g_gs[kGAnimated].blocked);
        }
        if (a_arg.rfind("policy", 0) == 0) {
            char st[32]{}, po[32]{};
            if (sscanf_s(a_arg.c_str() + 6, "%31s %31s", st, static_cast<unsigned>(sizeof(st)), po, static_cast<unsigned>(sizeof(po))) != 2) {
                return "error: camdir policy <state name|id> <TRACK|SCENE|FADE_SNAP|LOCK|CINEMA>";
            }
            const int i = ParseState(st);
            Policy    p;
            if (i < 0 || !ParsePolicy(po, p)) {
                return "error: camdir policy <state name|id> <TRACK|FADE_SNAP|LOCK|CINEMA>";
            }
            if (p == Policy::kScene && (i == 0 || i == 4)) {
                return "error: SCENE is not allowed for FirstPerson / IronSights (first-person path)";
            }
            if (p == Policy::kScene && i == kStateFree && !FreeOn()) {
                return "error: Free=SCENE needs the free-camera scene view (bFreeCamScene=1)";
            }
            g_d.sceneSt[i] = p == Policy::kScene;
            c.table[i] = p == Policy::kScene ? Policy::kTrack : p;
            REX::INFO("camdir: policy {} = {} (pipe)", kStateNames[i], PolicyName(p));
            return TableString();
        }
        if (a_arg.rfind("cfg ", 0) == 0) {
            // 20 Nov (VR settings panel, ConfigUI.h): the director's runtime values without a restart
            char  key[32]{};
            float v = 0;
            if (sscanf_s(a_arg.c_str() + 4, "%31s %f", key, static_cast<unsigned>(sizeof(key)), &v) != 2) {
                return "error: camdir cfg fadems|timeout|jump|return|returns|grace|headhide|lockfp|body|notify|sameinput|follow|eyefwd|yieldpov|statepos <value>";
            }
            const std::string k = key;
            if (k == "fadems") c.fadeMs = (std::max)(v, 0.0f);
            else if (k == "timeout") c.timeoutS = (std::max)(v, 0.0f);
            else if (k == "jump") c.jumpUnits = (std::max)(v, 0.0f);
            else if (k == "return") c.thirdReturn = v != 0.0f;
            else if (k == "returns") c.thirdReturnS = (std::max)(v, 0.0f);
            else if (k == "grace") c.thirdGraceMs = (std::max)(v, 0.0f);
            else if (k == "headhide") {
                g_hh.cfgRead = true;
                g_hh.on = v != 0.0f;
            } else if (k == "lockfp") {
                c.lockFp = v != 0.0f;
                game::camtweak::SetForced(c.lockFp);
            } else if (k == "body") c.bodyCam = v != 0.0f;
            else if (k == "notify") c.hudNotify = v != 0.0f;
            else if (k == "sameinput") c.sameInput = v != 0.0f;
            else if (k == "follow") c.bodyFollow = v != 0.0f;           // 21 Nov: bBodyCameraFollowsAnimation
            else if (k == "eyefwd") c.bodyEyeFwd = v;                   // fBodyCameraEyeForward
            else if (k == "yieldpov") c.bodyYieldPov = v != 0.0f;       // bBodyCameraYieldsToPov
            else if (k == "statepos") c.freeStatePos = v != 0.0f;       // bFreeCamStatePosition
            else {
                return "error: camdir cfg fadems|timeout|jump|return|returns|grace|headhide|lockfp|body|notify|sameinput|follow|eyefwd|yieldpov|statepos <value>";
            }
            REX::INFO("camdir: cfg {} = {} (pipe / VR panel)", k, v);
            return std::format("fadeMs={} timeoutS={} jumpUnits={} thirdReturn={} thirdReturnS={} thirdGraceMs={} povHeadHide={} lockFp={} "
                               "body={} notify={} sameInput={} follow={} eyeFwd={} yieldPov={} statePos={}",
                c.fadeMs, c.timeoutS, c.jumpUnits, c.thirdReturn ? 1 : 0, c.thirdReturnS, c.thirdGraceMs, g_hh.on ? 1 : 0, c.lockFp ? 1 : 0,
                c.bodyCam ? 1 : 0, c.hudNotify ? 1 : 0, c.sameInput ? 1 : 0, c.bodyFollow ? 1 : 0, c.bodyEyeFwd, c.bodyYieldPov ? 1 : 0,
                c.freeStatePos ? 1 : 0);
        }
        if (a_arg == "gates") {
            std::string out = "gates";
            for (int i = 0; i < kGates; ++i) {
                out += std::format(" {}=patched:{}/calls:{}/blocked:{}", kGateNames[i], g_gs[i].patched ? 1 : 0, g_gs[i].calls, g_gs[i].blocked);
            }
            return out + std::format(" | furniture={} powerArmor={} animated={} block3rd={} vats={} pipboyStop={} headingSnap={} snapDeg={} "
                                     "paFurniture={} paHolds={} paHoldTimeouts={} lastPaHoldMs={:.0f} headingSnaps={} lastHeadingJump={:.1f} "
                                     "sitSleep={} vatsCam={} camEnabledByte={}/{}/{} wbPause={}/{} wbDirect={} wbClosesKept={} wbReentries={} wbStops={} last='{}'",
                       c.gateFurniture ? 1 : 0, c.gatePowerArmor ? 1 : 0, c.gateAnimated ? 1 : 0, c.blockThirdPerson ? 1 : 0, c.gateVats ? 1 : 0,
                       c.pipboyStopTick ? 1 : 0, c.headingSnap ? 1 : 0, c.headingSnapDeg, g_paFurniture, g_f.paHolds, g_f.paHoldTimeouts,
                       g_f.lastPaHoldMs, g_f.headingSnaps, g_f.lastHeadingJumpDeg, g_d.sitSleep, c.vatsKeepCamera ? 1 : 0, g_camEnabledBefore,
                       g_camEnabledAfter, CamEnabledByte(), g_origWbCtor ? (g_workbenchPause ? 1 : 0) : -1, g_workbenchPauseSet,
                       g_workbenchDirectOpens, g_workbenchClosesKept, g_directReentriesIgnored, g_directStops, g_lastGate);
        }
        if (a_arg.rfind("gate ", 0) == 0) {
            char nm[32]{}, v[16]{};
            if (sscanf_s(a_arg.c_str() + 5, "%31s %15s", nm, static_cast<unsigned>(sizeof(nm)), v, static_cast<unsigned>(sizeof(v))) != 2) {
                return "error: camdir gate furniture|pa|animated|3rd|vats|vatscam|pipboy|heading <on|off|deg>";
            }
            const std::string n = nm, val = v;
            const bool on = val == "on" || val == "1";
            if (n == "furniture") c.gateFurniture = on && g_gs[kGFurniture].patched;
            else if (n == "pa") c.gatePowerArmor = on && g_gs[kGFurniture].patched;
            else if (n == "animated") c.gateAnimated = on && g_gs[kGAnimated].patched;
            else if (n == "3rd") c.blockThirdPerson = on && g_gs[kGSet3rd].patched;
            else if (n == "vats") c.gateVats = on && g_gs[kGStartVats].patched;
            else if (n == "pipboy") c.pipboyStopTick = on && g_gs[kGPipboyStop].patched;
            else if (n == "vatscam") c.vatsKeepCamera = on && g_gs[kGVatsState].patched;
            else if (n == "heading") {
                if (val == "on" || val == "off" || val == "1" || val == "0") c.headingSnap = on;
                else c.headingSnapDeg = std::strtof(v, nullptr);
            } else return "error: camdir gate furniture|pa|animated|3rd|vats|vatscam|pipboy|heading <on|off|deg>";
            rs::SetHeadingSnapThreshold(c.headingSnap ? c.headingSnapDeg / kR2D : 0.0f);
            return std::format("gate {} -> furniture={} pa={} animated={} 3rd={} vats={} pipboy={} heading={} ({} deg)", n, c.gateFurniture ? 1 : 0,
                c.gatePowerArmor ? 1 : 0, c.gateAnimated ? 1 : 0, c.blockThirdPerson ? 1 : 0, c.gateVats ? 1 : 0, c.pipboyStopTick ? 1 : 0,
                c.headingSnap ? 1 : 0, c.headingSnapDeg);
        }
        if (a_arg == "cycle") {
            // UI round 8 (user: a hotkey to cycle the view mode in a given situation, for testing; [Keys] iCameraCycle):
            // the current camera state's policy SCENE -> TRACK -> CINEMA -> SCENE (FirstPerson/IronSights: TRACK <-> CINEMA;
            // Free without the thunk: no SCENE). Runtime only, not saved to the ini.
            const int st = g_d.state;
            if (st < 0 || st >= kStates) {
                return "cycle: no camera state";
            }
            const bool sceneOk = st != 0 && st != 4 && (st != kStateFree || FreeOn());
            const Policy cur = g_d.sceneSt[st] ? Policy::kScene : c.table[st];
            Policy next;
            if (cur == Policy::kScene) next = Policy::kTrack;
            else if (cur == Policy::kTrack) next = Policy::kCinema;
            else next = sceneOk ? Policy::kScene : Policy::kTrack;
            g_d.sceneSt[st] = next == Policy::kScene;
            c.table[st] = next == Policy::kScene ? Policy::kTrack : next;
            REX::INFO("camdir: policy {} = {} (cycle key; was {})", kStateNames[st], PolicyName(next), PolicyName(cur));
            return std::format("camera {}: {}", kStateNames[st], PolicyName(next));
        }
        if (a_arg == "pov") {
            // UI round 8 (user: "a way to cycle in and out of 3rd person in case I get stuck"; [Keys] iPovToggle): first person
            // -> PlayerCamera::Force3rdPerson (the original, past our own bBlockThirdPerson gate and without arming the
            // script-third-person return), anything else -> Force1stPerson. Both are direct camera calls, so a mod that
            // disabled the POV control (AAF) does not stop them. Main thread (task).
            const CamFn_t third = g_origForce3rd;  // set only when its prologue was byte-checked (Detour)
            const bool    toThird = g_d.state == 0;
            if (toThird && c.lockFp) {
                return "refused: first-person lock is on ([Camera] bLockFirstPerson; camdir lock off)";
            }
            if (toThird ? !third : !g_force1st) {
                return toThird ? "error: Force3rdPerson not resolved" : "error: Force1stPerson not resolved";
            }
            if (tq::RunOnMain([toThird, third]() {  // main-idle queue (ThreadQueues.h)
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                        if (toThird) third(pc);
                        else g_force1st(pc);
                    }
                })) {
                REX::INFO("camdir: pov key -> {}", toThird ? "Force3rdPerson" : "Force1stPerson");
                return toThird ? "third person" : "first person";
            }
            return "error: no main-thread queue";
        }
        if (a_arg == "first") {  // PlayerCamera::Force1stPerson on the main thread (task), as Game.ForceFirstPerson
            if (!g_force1st) {
                return "error: Force1stPerson not resolved";
            }
            if (tq::RunOnMain([]() {  // main-idle queue (ThreadQueues.h)
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) g_force1st(pc);
                })) {
                return "first: Force1stPerson queued";
            }
            return "error: no main-thread queue";
        }
        if (a_arg.rfind("fadetest", 0) == 0) {
            const double hold = a_arg.size() > 8 ? std::strtod(a_arg.c_str() + 8, nullptr) : 500.0;
            g_f.test = true;
            g_f.testHoldMs = hold > 0 ? hold : 500.0;
            g_f.blackSeen = false;
            g_f.start = Clock::now();
            g_f.fromState = g_f.causeState = g_d.state;
            return std::format("fadetest: out {} ms, hold {} ms, in {} ms", c.fadeMs, g_f.testHoldMs, c.fadeMs);
        }
        if (!a_arg.empty()) {
            return "error: camdir [on|off|table|policy <state> <policy>|fadetest [holdMs]|gates|gate <name> <on|off>|first]";
        }
        const Dir&  d = g_d;
        const Fade& f = g_f;
        // free-camera scene view (bFreeCamScene)
        const std::string freeView = std::format(
            " | freeCam={} freeTicks={} sceneYaw={:.2f} freeYawOffset={:.2f} freeSpeed={:.0f} yawCuts={} glideHolds={} glideTimeouts={} "
            "bodyShows={} thirdGraces={} aafThirdPassed={} moveView={}/{} headDelta={} scene={} sceneEntries={} glideLatched={} "
            "thirdReturn={}/{} armed={} queued={} returns={} lastReturn='{}' benchInputReleases={} povHeadHide={} povActor={:08X} "
            "povDist={:.1f} povNodes={} povHides={} povRestores={} povLast='{}' povWhy='{}' povNoFace={} | lockFp={} lockForces={} "
            "lastLockForce='{}' body={} bodyNow={} bodyTicks={} bodyEyeH={:.1f} sameInput={} hudNotify={} hudShown={} hudRefused={} "
            "hudLast='{}' mode='{}'",
            d.freeNote, g_freeTicks.load(), Wrap(d.sceneYaw) * kR2D, d.freeYawOffset * kR2D, d.lastSpeed, d.yawCuts, d.glideHolds,
            d.glideTimeouts, d.bodyShows, d.thirdGraces, d.aafThirdPassed, c.freeMoveView ? 1 : 0, d.moveViewCalls,
            c.freeHeadDelta && d.haveFreeOffBase ? 1 : 0, d.state >= 0 && d.state < kStates && d.sceneSt[d.state] ? 1 : 0,
            d.sceneEntries, d.glideLatched ? 1 : 0, c.thirdReturn ? 1 : 0, c.thirdReturnS, d.thirdArmed ? 1 : 0, d.thirdQueued ? 1 : 0,
            d.thirdReturns, d.lastReturn, g_benchInputReleases, g_hh.on ? 1 : 0, g_hh.actor ? g_hh.actor->GetFormID() : 0, g_hh.lastDist,
            g_hh.culled.Size(), g_hh.hides, g_hh.restores, g_hh.last, g_hh.why, g_hh.noFace, c.lockFp ? 1 : 0, d.lockForces, d.lockLast,
            c.bodyCam ? 1 : 0, d.body ? 1 : 0, d.bodyTicks, d.bodyEyeH, c.sameInput ? 1 : 0, c.hudNotify ? 1 : 0, g_n.shown, g_n.refusedShown,
            g_n.last, ModeLabelLocked(d.state)) +
            std::format(" | lockScenes={} defeatWindowS={} sceneFlow={} scenePasses={} lastScenePass='{}' lockForceSkips={} orbitScene={} "
                        "ownStickYaw={} stickYaw={:.1f} stickTurnUpdates={} aftercare={}",
                c.lockScenes ? 1 : 0, c.defeatWindowS, d.sceneFlowNow ? 1 : 0, d.scenePasses, d.lastScenePass, d.lockForceSkips,
                c.orbitScene ? 1 : 0, c.freeOwnStick ? 1 : 0, d.stickYaw * kR2D, d.stickTurnUpdates, c.aftercare ? 1 : 0);
        return std::format(
            "installed={} enabled={} note='{}' state={} name={} policy={} eff={} stateMs={:.0f} ticks={} transitions={} "
            "writes=T{}/F{}/L{}/C{} wrote={} alpha={:.3f} target={} fadeActive={} timedOut={} episodes={} timeouts={} "
            "lastOutMs={:.0f} lastHoldMs={:.0f} lastInMs={:.0f} lastEpisode='{}' "
            "haveOwn={} ownYaw={:.2f} heading={:.2f} camYaw={:.2f} camPitch={:.2f} gameYaw={:.2f} gamePitch={:.2f} haveHead={} "
            "hold={} headingSnaps={} paHolds={} sitSleep={} tg=({:.2f},{:.2f},{:.2f}) out=({:.2f},{:.2f},{:.2f}) add=({:.2f},{:.2f},{:.2f}) raw=({:.2f},{:.2f},{:.2f}) hold=({:.2f},{:.2f},{:.2f}) rootJumps={} jumpFades={} lastJump={:.1f} jumpUnits={} fadeMs={} timeoutS={} table='{}'",
            d.installed ? 1 : 0, c.enabled ? 1 : 0, d.installNote, d.state, StateName(d.state), PolicyName(d.policy), PolicyName(d.eff),
            d.stateSince.time_since_epoch().count() ? Ms(d.stateSince, Clock::now()) : 0.0, d.ticks, d.transitions, d.writes[0], d.writes[1],
            d.writes[2], d.writes[3], d.wrote ? 1 : 0, f.alpha, f.target ? 1 : 0, f.active ? 1 : 0, f.timedOut ? 1 : 0, f.episodes,
            f.timeouts, f.lastOutMs, f.lastHoldMs, f.lastInMs, f.lastEpisode, d.haveOwn ? 1 : 0, Wrap(d.ownYaw) * kR2D,
            Wrap(d.heading) * kR2D, d.camYaw * kR2D, d.camPitch * kR2D, d.gameYaw * kR2D, d.gamePitch * kR2D, d.haveHead ? 1 : 0, f.hold,
            f.headingSnaps, f.paHolds, d.sitSleep, d.tg.x,
            d.tg.y, d.tg.z, d.out.x, d.out.y, d.out.z, d.add.x, d.add.y, d.add.z, d.raw.x, d.raw.y, d.raw.z, d.holdBase.x, d.holdBase.y, d.holdBase.z, d.rootJumps, d.jumpFades, d.lastJump, c.jumpUnits, c.fadeMs, c.timeoutS,
            TableString()) + freeView;
    }
    // 20 Nov (live settings, ConfigStore.h): the director's [Camera] keys (and the [Menus] / [General] ones it reads) that
    // changed are re-read and applied under the director lock. Only changed keys are applied, so pipe / F2 overrides of other
    // keys stay. Restart-only (installed at load): bCameraDirector, the VATS gates (bGateVats, bVatsKeepCamera, [Menus]
    // bVatsLiveWorld / bVatsCameraWindow / bVatsNoImod: their detours are installed in Start/Stop pairs only when on), and
    // bOwnHeading (Roomscale, the first-person yaw path).
    void ReloadLive(const cfgstore::Changes& a_ch)
    {
        const auto path = Settings::Path();
        auto rdf = [&](const char* sec, const char* k, float def) {
            char buf[64]{};
            ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), path.c_str());
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : def;
        };
        auto rdb = [&](const char* sec, const char* k, bool def) { return ::GetPrivateProfileIntA(sec, k, def ? 1 : 0, path.c_str()) != 0; };
        std::scoped_lock l(g_lock);
        if (!g_d.installed) {
            REX::INFO("camdir: [Camera] change not applied: the director is not installed ({}; restart)", g_d.installNote);
            return;
        }
        Cfg&        c = g_cfg;
        const Cfg   d{};
        std::string what;
        auto F = [&](const char* sec, const char* k, float& v, float def) {
            if (!a_ch.Has(sec, k)) return;
            v = rdf(sec, k, def);
            what += std::format(" {}={}", k, v);
        };
        auto B = [&](const char* k, bool& v, bool def) {
            if (!a_ch.Has("Camera", k)) return;
            v = rdb("Camera", k, def);
            what += std::format(" {}={}", k, v ? 1 : 0);
        };
        F("Camera", "fFadeMs", c.fadeMs, d.fadeMs);
        F("Camera", "fFadeTimeoutS", c.timeoutS, d.timeoutS);
        F("Camera", "fJumpFadeUnits", c.jumpUnits, d.jumpUnits);
        F("General", "fUnitsPerMeter", c.unitsPerMeter, 70.0f);
        if (!(c.unitsPerMeter > 1.0f)) c.unitsPerMeter = 70.0f;
        F("General", "fHeightOffsetCm", c.heightOffsetCm, 0.0f);
        const bool craft = rdb("Menus", "bCraftingLiveWorld", true);
        if (a_ch.Has("Camera", "bGateFurniture") || a_ch.Has("Menus", "bCraftingLiveWorld")) {
            c.gateFurniture = rdb("Camera", "bGateFurniture", false) || craft;
            what += std::format(" furnitureGate={}", c.gateFurniture ? 1 : 0);
        }
        if (a_ch.Has("Camera", "bGateAnimated") || a_ch.Has("Menus", "bCraftingLiveWorld")) {
            c.gateAnimated = rdb("Camera", "bGateAnimated", false) || craft;
            what += std::format(" animatedGate={}", c.gateAnimated ? 1 : 0);
        }
        if (a_ch.Has("Menus", "bWorkbenchPause") || a_ch.Has("Menus", "bCraftingLiveWorld")) {
            g_workbenchPause = rdb("Menus", "bWorkbenchPause", true) && craft;
            what += std::format(" workbenchPause={}", g_workbenchPause ? 1 : 0);
        }
        B("bGatePowerArmor", c.gatePowerArmor, d.gatePowerArmor);
        B("bWorkbenchDirectMenu", c.workbenchDirect, d.workbenchDirect);
        B("bBlockThirdPerson", c.blockThirdPerson, d.blockThirdPerson);
        B("bPipboyStopTick", c.pipboyStopTick, d.pipboyStopTick);
        B("bHeadingSnap", c.headingSnap, d.headingSnap);
        F("Camera", "fHeadingSnapDeg", c.headingSnapDeg, d.headingSnapDeg);
        F("Camera", "fPowerArmorHoldS", c.paHoldS, d.paHoldS);
        B("bFreeCamBody", c.freeBody, d.freeBody);
        F("Camera", "fFreeCamYawCutDeg", c.freeYawCutDeg, d.freeYawCutDeg);
        F("Camera", "fFreeCamGlideSpeed", c.freeGlideSpeed, d.freeGlideSpeed);
        F("Camera", "fFreeCamGlideHoldS", c.freeGlideHoldS, d.freeGlideHoldS);
        F("Camera", "fThirdPersonGraceMs", c.thirdGraceMs, d.thirdGraceMs);
        B("bFreeCamAAFThirdPerson", c.freeAafThird, d.freeAafThird);
        B("bFreeCamMoveWithView", c.freeMoveView, d.freeMoveView);
        B("bFreeCamHeadDelta", c.freeHeadDelta, d.freeHeadDelta);
        B("bThirdPersonReturn", c.thirdReturn, d.thirdReturn);
        F("Camera", "fThirdPersonReturnS", c.thirdReturnS, d.thirdReturnS);
        if (a_ch.Has("Camera", "bLockFirstPerson")) {
            c.lockFp = rdb("Camera", "bLockFirstPerson", false);
            game::camtweak::SetForced(c.lockFp);
            what += std::format(" bLockFirstPerson={}", c.lockFp ? 1 : 0);
        }
        B("bBodyCamera", c.bodyCam, d.bodyCam);
        B("bCameraHudNotify", c.hudNotify, d.hudNotify);
        B("bSameInputAllViews", c.sameInput, d.sameInput);
        B("bLockAllowScenes", c.lockScenes, d.lockScenes);
        F("Camera", "fDefeatWindowS", c.defeatWindowS, d.defeatWindowS);
        B("bFreeCamOwnStickYaw", c.freeOwnStick, d.freeOwnStick);
        B("bSceneAftercare", c.aftercare, d.aftercare);
        B("bBodyCameraFollowsAnimation", c.bodyFollow, d.bodyFollow);
        F("Camera", "fBodyCameraEyeForward", c.bodyEyeFwd, d.bodyEyeFwd);
        B("bBodyCameraYieldsToPov", c.bodyYieldPov, d.bodyYieldPov);
        B("bFreeCamStatePosition", c.freeStatePos, d.freeStatePos);
        const bool orbitChanged = a_ch.Has("Camera", "bOrbitStatesScene");
        B("bOrbitStatesScene", c.orbitScene, d.orbitScene);
        if (a_ch.Has("Camera", "bPovHeadHide") || a_ch.Has("Camera", "fPovHeadRadius")) {
            g_hh.cfgRead = false;  // HeadHideConfig re-reads at the next poll (main tick); off restores a hidden head there
            what += " povHeadHide (re-read)";
        }
        // free-camera scene view + the policy table
        const bool freeChanged = a_ch.Has("Camera", "bFreeCamScene");
        if (freeChanged) {
            c.freeScene = rdb("Camera", "bFreeCamScene", true);
            g_freeSceneLive = c.freeScene && g_d.freeHooked;
            what += std::format(" bFreeCamScene={}{}", c.freeScene ? 1 : 0, g_d.freeHooked ? "" : " (thunk NOT patched: no scene view)");
        }
        for (int i = 0; i < kStates; ++i) {
            const auto key = std::format("sPolicy{}", kStateNames[i]);
            if (!a_ch.Has("Camera", key.c_str()) && !(i == kStateFree && freeChanged) && !(orbitChanged && OrbitState(i))) continue;
            Policy p = kDefaultTable[i];
            char   buf[32]{};
            ::GetPrivateProfileStringA("Camera", key.c_str(), "", buf, sizeof(buf), path.c_str());
            if (buf[0] && !ParsePolicy(buf, p)) {
                REX::WARN("camdir: [Camera] {}={} not understood; default {}", key, buf, PolicyName(kDefaultTable[i]));
                p = kDefaultTable[i];
            }
            if (c.orbitScene && OrbitState(i) && p == Policy::kTrack) p = Policy::kScene;  // bOrbitStatesScene, as at load
            if (i == kStateFree && FreeOn()) p = Policy::kScene;  // bFreeCamScene=1 overrides sPolicyFree, as at load
            if (p == Policy::kScene && (i == 0 || i == 4)) p = Policy::kTrack;
            if (p == Policy::kScene && i == kStateFree && !FreeOn()) p = Policy::kCinema;
            g_d.sceneSt[i] = p == Policy::kScene;
            c.table[i] = p == Policy::kScene ? Policy::kTrack : p;
            what += std::format(" {}={}", key, PolicyName(p));
        }
        REX::INFO("camdir: settings re-read (live):{}", what.empty() ? " nothing of the director's" : what);
    }
}
