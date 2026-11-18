#include "plugin/CameraDirector.h"

#include "plugin/GameBridge.h"
#include "plugin/HeadPose.h"
#include "plugin/Roomscale.h"
#include "plugin/Settings.h"
#include "plugin/VatsWindow.h"
#include "plugin/VirtualPad.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

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
        };
        std::atomic<std::uint64_t> g_freeTicks{ 0 };
        std::atomic<bool>          g_thirdArmReq{ false };  // HkCamFn<Force3rdPerson>: a script third person (arms the return)

        std::mutex g_lock;
        Cfg        g_cfg;
        Dir        g_d;
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

        enum GateId { kGFurniture, kGAnimated, kGSet3rd, kGForce3rd, kGStartVats, kGForceVats, kGStopVats, kGPipboyStop, kGVatsState, kGates };
        const char* const kGateNames[kGates] = { "StartFurnitureMode", "StartAnimatedMode", "Set3rdPerson", "Force3rdPerson",
            "StartVATSMode", "ForceVATSMode", "StopVATSMode", "StopPipboyMode->TESCamera::Update", "VATS ChangeState(2)" };
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
                gate = pa ? g_cfg.gatePowerArmor : g_cfg.gateFurniture;
                if (gate) ++g_gs[kGFurniture].blocked;
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
                gate = g_cfg.gateAnimated;
                if (gate) ++g_gs[kGAnimated].blocked;
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
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* kw = dh ? dh->LookupForm<RE::BGSKeyword>(0x00915A, "AAF.esm") : nullptr;
            return kw && player->HasKeywordHelper(kw, nullptr);
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
                    gate = g_cfg.blockThirdPerson;
                    orig = G == kGSet3rd ? g_origSet3rd : g_origForce3rd;
                    if constexpr (G == kGForce3rd) {
                        // AAF locks the player with Game.ForceThirdPerson() then the fly cam (AAF_MainQuestScript DoLockActor,
                        // after AddKeyword(AAF_ActorBusy)). Gated, the player stayed in the first-person body and behaviour graph
                        // until our late Show1stPerson(false) on the free-camera entry - after AAF's first PlayIdle - and the
                        // player's later idles did not take (user, headset 15 Nov 17:16-17:23; aaf_visibility.log shown3rd=0 at the
                        // first idle). Let AAF's own call through, as in the flat game: state 8 fades (fThirdPersonGraceMs), then 3.
                        if (gate && g_cfg.freeAafThird && g_d.freeHooked && PlayerAafBusy()) {
                            gate = false;
                            ++g_d.aafThirdPassed;
                            REX::INFO("camdir gate: Force3rdPerson with the player AI-driven / AAF-busy (scene lock) -> passed (bFreeCamAAFThirdPerson)");
                        }
                    }
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
            const bool freeScene = g_cfg.enabled && d.freeHooked;  // bFreeCamScene and the thunk patched
            auto       sceneOf = [&](int a_st) { return g_cfg.enabled && a_st >= 0 && a_st < kStates && d.sceneSt[a_st]; };
            // SCENE (the AAF free-camera method for every game camera without its own handling): this state is head-tracked
            // from the game camera's position and yaw
            const bool scene = sceneOf(st);
            const bool changed = st != d.state;
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
                if (scene && st == kStateFree) {
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
                    if (const auto tasks = F4SE::GetTaskInterface()) {
                        tasks->AddTask([]() {
                            auto* pc = RE::PlayerCamera::GetSingleton();
                            if (pc && pc->currentState && static_cast<int>(pc->currentState->id.get()) == kStateThird && g_force1st) {
                                g_force1st(pc);
                            }
                        });
                    }
                }
            }

            auto* root = cam->cameraRoot.get();
            float* R = root ? reinterpret_cast<float*>(&root->local.rotate) : nullptr;
            auto*  T = root ? &root->local.translate : nullptr;
            if (T) d.tg = { T->x, T->y, T->z };

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
            head::Mat3 h{};
            d.haveHead = head::Tracking() && head::Get(h);
            d.haveOwn = rs::OwnedYaw(d.ownYaw);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                d.heading = player->data.angle.z;
            }
            float yaw = d.haveOwn ? d.ownYaw : d.gameYaw;
            if (scene) {
                // SCENE view (free camera, third person, animated, ...): the base yaw is the game camera's (AAF aims its shots
                // there; the player's stick turns it), the game camera's pitch is dropped (the head gives pitch). On entry without a fade it continues the
                // previous view's yaw (offset), after any snap the offset is 0. Frozen while a fade holds the old pose.
                const float sy = SceneYaw(R);
                if (d.freeEntry) {
                    d.freeEntry = false;
                    if (!d.sceneKeepHead) {
                        head::Vec3 o;
                        d.haveFreeOffBase = head::GetOffset(o);  // head motion counts from here (the game camera = the view)
                        d.freeOffBase = o;
                    }
                    d.freeYawOffset = d.lastBaseValid ? Wrap(d.lastBaseYaw - sy) : 0.0f;
                    d.sceneYaw = d.lastBaseValid ? d.lastBaseYaw : sy;
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
                    d.sceneYaw = Wrap(sy + d.freeYawOffset);
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
                if (scene) {
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
                if (g_cfg.enabled && g_cfg.freeMoveView && g_d.freeHooked && g_d.moveViewOk) {
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
            a_fs->rotation.y = WrapPos(sYaw + dYaw);
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
            if (g_workbenchPause && !g_origWbCtor) {
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

    std::string Install()
    {
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
        // free-camera scene view (bFreeCamScene): redirect the free camera's TESCamera::Update thunk to the same post-tick and
        // make state 3 a TRACK world view (overrides sPolicyFree, as bCraftingLiveWorld overrides the gates). Byte check:
        // the thunk is exactly E9 rel32 landing on TESCamera::Update (prologue checked above); any mismatch = not patched,
        // state 3 keeps sPolicyFree.
        if (c.freeScene) {
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
                if (c.freeMoveView) {
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
                if (c.table[kStateFree] != Policy::kScene) {
                    REX::INFO("camdir: bFreeCamScene=1: state Free policy {} -> SCENE (free-camera scene view)", PolicyName(c.table[kStateFree]));
                }
                c.table[kStateFree] = Policy::kScene;
            }
        } else {
            g_d.freeNote = "off (bFreeCamScene=0)";
        }
        // SCENE states: TRACK + the scene flag. The free camera can only be SCENE with its thunk patched (no director tick in
        // state 3 otherwise: a world locked to the face), else it falls back to CINEMA.
        for (int i = 0; i < kStates; ++i) {
            g_d.sceneSt[i] = false;
            if (c.table[i] == Policy::kScene && (i == 0 || i == 4)) {
                // first person / iron sights are the FirstPersonState path (head authority, roomscale, FRIK); SCENE there would
                // drop the head-tracked first-person camera and switch the controls to the flat-screen mapping
                REX::WARN("camdir: sPolicy{}=SCENE is not allowed (first-person path): TRACK", kStateNames[i]);
                c.table[i] = Policy::kTrack;
                continue;
            }
            if (c.table[i] == Policy::kScene) {
                if (i == kStateFree && !g_d.freeHooked) {
                    c.table[i] = Policy::kCinema;
                    REX::WARN("camdir: sPolicyFree=SCENE needs the free-camera thunk ({}): CINEMA", g_d.freeNote);
                    continue;
                }
                g_d.sceneSt[i] = true;
                c.table[i] = Policy::kTrack;
            }
        }
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
        // ---- POV head hide (PollHeadHide) ----
        struct HeadHide
        {
            bool                                   cfgRead{ false };
            bool                                   on{ true };
            float                                  radius{ 40.0f };
            std::vector<std::string>               tokens;
            RE::NiPointer<RE::Actor>               actor;          // whose head is hidden now
            std::vector<RE::NiPointer<RE::NiAVObject>> culled;     // nodes this hid (flag bit 0 was clear before)
            Clock::time_point                      lastFind{}, lastScan{};
            float                                  lastDist{ -1.0f };
            std::uint64_t                          hides{ 0 }, restores{ 0 };
            std::string                            last{ "-" };
        };
        HeadHide g_hh;

        // FRIK's Mesh_Hide\face.ini list (FRIK-flat\FRIK_Config), the geometry names FRIK hides for the player's head
        constexpr const char* kHeadTokens[] = { "bandage", "beard", "cap", "cowl", "crown", "eyepiece", "eyes", "fedora", "glasses",
            "goggles", "hair", "head", "helm", "helmet", "hood", "mask", "mouth", "neckgore", "optics prototype", "shades", "visor",
            "wighairtop" };
        // head biped slots FRIK hides (CullGeometryHandler::restoreEquipment): hair top/long, FaceGen head, headband, eyes, beard,
        // mouth, neck, scalp
        constexpr int kHeadSlots[] = { 0, 1, 2, 16, 17, 18, 19, 20, 22 };

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
            for (const char* t : kHeadTokens) g_hh.tokens.emplace_back(t);
            REX::INFO("camdir: POV head hide {} (radius {} units: the game camera within that of an actor's Head bone in a SCENE state)",
                g_hh.on ? "on" : "off (bPovHeadHide=0)", g_hh.radius);
        }

        bool HeadName(const char* a_name)
        {
            if (!a_name || !*a_name) {
                return false;
            }
            std::string n = a_name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& t : g_hh.tokens) {
                if (n.find(t) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }

        RE::NiAVObject* ActorRoot(RE::Actor* a)
        {
            if (!a) return nullptr;
            return a == RE::PlayerCharacter::GetSingleton() ? static_cast<RE::TESObjectREFR*>(a)->Get3D(false) : a->Get3D();
        }

        void CullNode(RE::NiAVObject* o)
        {
            if (!o || (o->flags.flags & 1)) {
                return;  // already culled (by the game, FRIK or another mod): never ours to show again
            }
            o->flags.flags |= 1;
            g_hh.culled.emplace_back(o);
        }

        void CollectHead(RE::NiAVObject* o, int a_depth)
        {
            if (!o || a_depth > 40) {
                return;
            }
            if (o->IsGeometry()) {
                if (HeadName(o->name.c_str())) CullNode(o);
                return;
            }
            if (auto* n = o->IsNode()) {
                for (auto& c : n->children) {
                    if (c) CollectHead(c.get(), a_depth + 1);
                }
            }
        }

        void HeadHideScan(RE::Actor* a)
        {
            if (auto* root = ActorRoot(a)) {
                CollectHead(root, 0);
            }
            if (auto* biped = a->biped.get()) {
                for (const int slot : kHeadSlots) {
                    const auto& b = biped->object[slot];
                    if (b.parent.object && b.partClone && b.parent.object->GetFormType() == RE::ENUM_FORM_ID::kARMO) {
                        CullNode(b.partClone.get());
                    }
                }
            }
        }

        void HeadHideRestore(const char* a_why)
        {
            if (!g_hh.actor && g_hh.culled.empty()) {
                return;
            }
            for (auto& o : g_hh.culled) {
                if (o) o->flags.flags &= ~static_cast<std::uint64_t>(1);
            }
            const auto n = g_hh.culled.size();
            g_hh.culled.clear();
            ++g_hh.restores;
            REX::INFO("camdir: POV head hide: {:08X} head shown again ({} nodes; {})", g_hh.actor ? g_hh.actor->GetFormID() : 0, n, a_why);
            g_hh.actor.reset();
        }

        // the actor whose Head bone the game camera sits at (nearest within the radius), or null
        RE::NiPointer<RE::Actor> FindPovActor(const V3& a_cam, float& a_dist)
        {
            RE::NiPointer<RE::Actor> best;
            a_dist = -1.0f;
            float bestD = g_hh.radius;
            const RE::BSFixedString headName("Head");
            auto consider = [&](RE::Actor* a) {
                if (!a) return;
                const auto& l = a->data.location;
                const float lx = l.x - a_cam.x, ly = l.y - a_cam.y, lz = l.z - a_cam.z;
                if (lx * lx + ly * ly + lz * lz > 400.0f * 400.0f) return;  // the Head bone is within ~150 u of the actor's origin
                auto* root = ActorRoot(a);
                auto* head = root ? root->GetObjectByName(headName) : nullptr;
                if (!head) return;
                const auto& h = head->world.translate;
                const float dx = h.x - a_cam.x, dy = h.y - a_cam.y, dz = h.z - a_cam.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < bestD) {
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
    }

    void PollHeadHide()
    {
        HeadHideConfig();
        bool active = false;
        V3   cam{};
        int  st = -1;
        {
            std::scoped_lock l(g_lock);
            st = g_d.state;
            active = g_hh.on && g_d.installed && g_cfg.enabled && st >= 0 && st < kStates && g_d.sceneSt[st];
            cam = g_d.tg;  // the game camera's position (AAF's POV point), before the head offset
        }
        if (!active) {
            HeadHideRestore("view not in a SCENE state");
            return;
        }
        const auto now = Clock::now();
        if (Ms(g_hh.lastFind, now) >= 100.0) {  // 10 Hz: the POV point moves with the head bone, actors do not swap often
            g_hh.lastFind = now;
            float d = -1.0f;
            auto  a = FindPovActor(cam, d);
            g_hh.lastDist = d;
            if (a.get() != g_hh.actor.get()) {
                HeadHideRestore(a ? "view moved to another head" : "view left the head");
                if (a) {
                    g_hh.actor = a;
                    g_hh.lastScan = {};
                    ++g_hh.hides;
                    g_hh.last = std::format("{:08X} at {:.1f} u (state {})", a->GetFormID(), d, StateName(st));
                }
            }
        }
        if (!g_hh.actor) {
            return;
        }
        if (Ms(g_hh.lastScan, now) >= 500.0) {  // equipment / head parts can change during a scene (undress, re-equip)
            const bool first = g_hh.lastScan.time_since_epoch().count() == 0;
            g_hh.lastScan = now;
            const auto before = g_hh.culled.size();
            HeadHideScan(g_hh.actor.get());
            if (first) {
                REX::INFO("camdir: POV head hide: view at the head of {} -> {} head nodes culled", g_hh.last, g_hh.culled.size() - before);
            }
        }
        for (auto& o : g_hh.culled) {  // keep them culled (a re-equip or another mod may clear the flag)
            if (o) o->flags.flags |= 1;
        }
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
        Policy p = a_camState >= 0 && a_camState < kStates ? g_cfg.table[a_camState] : Policy::kCinema;
        if (a_camState == kStateThird && p == Policy::kCinema && g_d.freeHooked && g_cfg.thirdGraceMs > 0.0f) {
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
               g_cfg.table[a_camState] == Policy::kTrack;
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
            if (p == Policy::kScene && i == kStateFree && !g_d.freeHooked) {
                return "error: Free=SCENE needs the free-camera thunk (bFreeCamScene=1)";
            }
            g_d.sceneSt[i] = p == Policy::kScene;
            c.table[i] = p == Policy::kScene ? Policy::kTrack : p;
            REX::INFO("camdir: policy {} = {} (pipe)", kStateNames[i], PolicyName(p));
            return TableString();
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
            const bool sceneOk = st != 0 && st != 4 && (st != kStateFree || g_d.freeHooked);
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
            if (toThird ? !third : !g_force1st) {
                return toThird ? "error: Force3rdPerson not resolved" : "error: Force1stPerson not resolved";
            }
            if (const auto tasks = F4SE::GetTaskInterface()) {
                tasks->AddTask([toThird, third]() {
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                        if (toThird) third(pc);
                        else g_force1st(pc);
                    }
                });
                REX::INFO("camdir: pov key -> {}", toThird ? "Force3rdPerson" : "Force1stPerson");
                return toThird ? "third person" : "first person";
            }
            return "error: no task interface";
        }
        if (a_arg == "first") {  // PlayerCamera::Force1stPerson on the main thread (task), as Game.ForceFirstPerson
            if (!g_force1st) {
                return "error: Force1stPerson not resolved";
            }
            if (const auto tasks = F4SE::GetTaskInterface()) {
                tasks->AddTask([]() {
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) g_force1st(pc);
                });
                return "first: Force1stPerson queued";
            }
            return "error: no task interface";
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
            "povDist={:.1f} povNodes={} povHides={} povRestores={} povLast='{}'",
            d.freeNote, g_freeTicks.load(), Wrap(d.sceneYaw) * kR2D, d.freeYawOffset * kR2D, d.lastSpeed, d.yawCuts, d.glideHolds,
            d.glideTimeouts, d.bodyShows, d.thirdGraces, d.aafThirdPassed, c.freeMoveView ? 1 : 0, d.moveViewCalls,
            c.freeHeadDelta && d.haveFreeOffBase ? 1 : 0, d.state >= 0 && d.state < kStates && d.sceneSt[d.state] ? 1 : 0,
            d.sceneEntries, d.glideLatched ? 1 : 0, c.thirdReturn ? 1 : 0, c.thirdReturnS, d.thirdArmed ? 1 : 0, d.thirdQueued ? 1 : 0,
            d.thirdReturns, d.lastReturn, g_benchInputReleases, g_hh.on ? 1 : 0, g_hh.actor ? g_hh.actor->GetFormID() : 0, g_hh.lastDist,
            g_hh.culled.size(), g_hh.hides, g_hh.restores, g_hh.last);
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
}
