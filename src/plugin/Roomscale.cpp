#include "plugin/Roomscale.h"

#include "plugin/GameBridge.h"
#include "plugin/HeadPose.h"
#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>

namespace fo4flatvr::rs
{
    namespace
    {
        // ---- 1.11.240 RVAs (analysis/vr-roomscale.md, db/labels/roomscale_research.csv) ----------
        constexpr std::uintptr_t kVtblSlotRva = 0x256D260;   // PlayerCharacter vtable[0] + 0x125*8
        constexpr std::uintptr_t kOrigMmdRva = 0xD5AB70;     // PlayerCharacter::ModifyMovementData
        constexpr std::uintptr_t kRetPrimary = 0xC66EF9;     // after call [rax+928] in UpdateMovement_Primary
        constexpr std::uintptr_t kRetGraph = 0xCAC5B7;       // after call [rax+928] in ProcessEvent(BSTransformDeltaEvent)
        constexpr std::uintptr_t kCallPrimary = 0xC66EF3;
        constexpr std::uintptr_t kCallGraph = 0xCAC5B1;
        constexpr std::uintptr_t kSpringMaxDist = 0x2F38D34;  // float 300.0, read only by FirstPersonState::Update
        constexpr std::uintptr_t kTclFlag = 0x30E8944;        // byte, toggled by console "tcl"
        constexpr std::uintptr_t kVatsSingleton = 0x30F9FD0;  // VATS*, mode at +0x40
        constexpr std::size_t    kSlot = 0x125;

        constexpr float kPi = 3.14159265358979f;
        constexpr float kD2R = kPi / 180.0f;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        float Wrap(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        // Engine heading convention rotation of a horizontal vector: frame with yaw t -> parent.
        // (x right, y forward) at heading t: forward -> (sin t, cos t), right -> (cos t, -sin t).
        void Rot(float t, double x, double y, double& ox, double& oy)
        {
            const double c = std::cos(t), s = std::sin(t);
            ox = x * c + y * s;
            oy = -x * s + y * c;
        }

        struct Cfg
        {
            bool  roomscale{ true };
            bool  bodyFollows{ true };
            bool  noSpring{ true };
            float deadzoneCm{ 12.0f };    // start moving the capsule when |head - capsule| exceeds this
            float stopCm{ 2.0f };         // ... and stop when below this
            float maxResidualCm{ 25.0f }; // camera never further than this from the capsule while blocked/gated
            float hardMaxCm{ 60.0f };     // ... and never further than this at all
            float gain{ 0.5f };           // fraction of the remaining error commanded per frame
            float maxSpeed{ 4.0f };       // m/s cap on the injected motion
            int   expireFrames{ 6 };      // a command not realized within this many updates is dropped
            float yawThresholdDeg{ 45.0f };
            float yawStopDeg{ 2.0f };
            float yawRateDeg{ 180.0f };   // body turn rate cap, deg/s
            float unitsPerMeter{ 70.0f };
            // Camera step 1 ([Camera] bOwnHeading): the room yaw is our own state, changed only by the user's
            // turn input (the game's angleDelta.z in the primary movement update: stick, mouse, magnetism) and
            // by resets. Game-driven SetHeading (setangle, VATS aim, furniture, dialogue facing) then turns the
            // body only, never the view. Off: roomYaw = heading - bodyRel as before (game turns drag the view).
            bool  ownHeading{ true };
            // Overshoot fix (14 Nov): displacement no outstanding command explains (the character controller
            // over-delivers the first command after rest by 30-170 %, measured) is split between us and the game
            // by their recent command activity (decaying sums, time constant fAttributionWindowS) instead of being
            // booked as external motion (room carried along), which left the anchor behind the capsule and put the
            // camera 1-6 units past the room-stable position. 0 = old behaviour (pipe "rs cfg attr 0").
            bool  attributeLeftover{ true };
            float attrWindowS{ 0.15f };
        };

        struct CmdEntry
        {
            double rx{}, ry{};    // our command still outstanding (world units)
            double gx{}, gy{};    // the game's own command still outstanding
            double r0{};          // |our command| when issued
            int    age{ 0 };
        };

        struct Hist
        {
            std::uint64_t frame;
            float rx, ry, dx, dy, ax, ay, k, e;
            int   outstanding;
            float dt, gx, gy, hx, hy;  // dt; game command (world); loc change since the previous update (world)
        };

        // All state below is guarded by g_lock (the movement hook may run on a job thread, the camera
        // hooks on the main thread, the readout on the pipe thread).
        std::mutex g_lock;
        Cfg        g_cfg;
        bool       g_installed = false;
        float      g_springOrig = 300.0f;
        bool       g_springPatched = false;
        bool       g_springOk = false;

        struct State
        {
            bool   valid{ false };          // anchor/yaw initialised for the current session
            double ax{}, ay{};              // anchor: room position (m) the capsule is under
            double lastLocX{}, lastLocY{};  // last observed data.location (xy)
            std::deque<CmdEntry> cmds;
            double recentOurs{ 0.0 }, recentGame{ 0.0 };  // decaying sums of |our| / |game| commands (world units)
            double leftoverOurs{ 0.0 }, leftoverExt{ 0.0 }; // unexplained displacement booked to us / external (totals)
            bool   moving{ false };
            float  eta{ 1.0f };             // EMA of the realized fraction of our commands
            float  bodyRel{ 0.0f };         // heading - roomYaw (realized)
            float  ownYaw{ 0.0f };          // owned room yaw (cfg.ownHeading): changes only by user turn input / resets
            std::uint64_t ownTurns{ 0 };    // updates that turned ownYaw (user input)
            std::uint64_t userTurns{ 0 };   // UserTurn() requests consumed
            float  userTurnTotal{ 0.0f };   // radians consumed from UserTurn()
            float  pendingYaw{ 0.0f };      // yaw injected at the last update, not yet confirmed
            bool   haveExpect{ false };     // expectHeading valid (camera step 2: game-driven heading changes)
            float  expectHeading{ 0.0f };   // heading after the last update's angleDelta.z is applied
            float  lastGameTurn{ 0.0f };    // last game-driven heading change (rad)
            std::uint64_t gameTurns{ 0 }, headingSnaps{ 0 };
            float  yawGamePrev{ 0.0f };     // game's own angleDelta.z at the last update
            float  prevHeading{ 0.0f };
            bool   following{ false };
            bool   jumpPending{ false };
            double prevHookLocX{}, prevHookLocY{};
            double prevHmdX{}, prevHmdY{};
            float  headSpeed{ 0.0f };   // m/s, smoothed (room frame, horizontal)
            unsigned recenterSeen{ 0 };
            RE::TESObjectCELL*  cell{ nullptr };
            std::chrono::steady_clock::time_point lastCall{};
            // diagnostics
            std::uint64_t frame{ 0 }, callsPrimary{ 0 }, callsGraph{ 0 }, callsOther{ 0 }, injectFrames{ 0 };
            std::uint64_t resets{ 0 };
            const char* lastReset{ "-" };
            const char* gate{ "init" };
            double dAccX{}, dAccY{}, aAccX{}, aAccY{};   // since last hook: observed / attributed (world)
            float  lastK{ 0.0f };
            float  gLocal[3]{}, gLocalAfter[3]{};        // moveDelta before / after our add
            float  injWorld[2]{};                        // injected this update (world units)
            float  dLoc[2]{}, aLoc[2]{};                 // observed / attributed since last update
            float  e[2]{}, eCm{ 0 }, eEffCm{ 0 };        // head - capsule (room, cm)
            float  hmd[3]{};                             // head offset (room, m)
            float  hmdYaw{ 0 }, yawInjected{ 0 }, gameYaw{ 0 };
            float  loc[3]{}, heading{ 0 }, looking{ 0 };
            int    ctrlState{ -1 }, numColl{ -1 }, sticky{ -1 };
            std::uint32_t ctrlFlags{ 0 };
            float  ctrlVel[3]{}, ctrlRadius{ 0 };
            float  spring[3]{}, springBlend{ 0 };
            int    tcl{ 0 }, sitSleep{ -1 }, controlsDriven{ -1 }, vats{ -1 }, camState{ -1 };
            std::uint32_t inputFlags{ 0 };
            float  camPos[3]{};
            float  camYaw{ 0 }, baseYawRot{ 0 };
            float  residualCm{ 0 };
            std::deque<Hist> hist;
        };
        State g_st;

        std::atomic<bool> g_positionPlayer{ false };
        std::atomic<int>  g_tfix{ 1 };
        float g_tRaw[3]{}, g_tPrevAdd[2]{};  // last raw GetTranslation and the offset subtracted from it (readout)

        // per camera update trace ("rs camhist"): everything needed to split a camera error into
        // feedback (camera - loc - base vs the residual) and bookkeeping (loc vs anchor) parts
        struct CamFrame
        {
            double t;                 // ms since the first record
            float  root[3];           // camera root world after FirstPersonState::Update
            float  loc[3];            // player data.location at that moment
            float  raw[3];            // original GetTranslation (before our add)
            float  add[2], d[2];      // full horizontal head offset (world units) / change applied (feedback mode)
            float  hmd[2];            // head offset (room, m)
            float  anchor[2];         // capsule anchor (room, m)
            float  yaw, heading;      // room yaw (owned or derived), actor heading
            int    in, out;           // GetTranslation calls during / outside the update
            std::uint64_t mvFrame;    // movement-hook frame counter at this camera update
        };
        std::deque<CamFrame> g_camHist;
        std::chrono::steady_clock::time_point g_camT0{};
        std::atomic<int>  g_requestReset{ 0 };
        std::atomic<float> g_userTurn{ 0.0f };  // queued by UserTurn(), consumed by the next primary movement update
        float              g_gameTurnJump = 0.0f; // largest unexplained heading change since TakeGameTurn (g_lock)
        std::atomic<bool>  g_freezeBody{ false };  // heading snap pending: no body-follow turn
        std::atomic<float> g_snapThreshold{ 0.0f }; // director's heading-snap threshold (rad), 0 = off
        bool               g_gameTurnSeen = false;
        std::atomic<std::uint64_t> g_primaryUpdates{ 0 };  // PrimaryUpdates() (comfort C8: turn path liveness)

        using Mmd_t = void (*)(RE::Actor*, float, RE::NiPoint3&, RE::NiPoint3&);
        Mmd_t g_origMmd = nullptr;

        class PositionPlayerSink : public RE::BSTEventSink<RE::PositionPlayerEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::PositionPlayerEvent& a_event,
                RE::BSTEventSource<RE::PositionPlayerEvent>*) override
            {
                if (a_event.type == RE::PositionPlayerEvent::kFinishPositionPlayer ||
                    a_event.type == RE::PositionPlayerEvent::kPostPositionPlayer) {
                    g_positionPlayer = true;
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        PositionPlayerSink g_ppSink;
        bool               g_ppRegistered = false;

        // ---- engine reads -------------------------------------------------------------------------
        std::uint8_t* CharController(RE::Actor* a)
        {
            auto* proc = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(a) + 0x300);
            if (!proc) return nullptr;
            auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x8);
            if (!mh) return nullptr;
            return *reinterpret_cast<std::uint8_t**>(mh + 0x3E0);  // AIProcess::GetCharController (D347C0)
        }

        int VatsMode()
        {
            auto* v = *reinterpret_cast<std::uint8_t**>(Base() + kVatsSingleton);
            return v ? *reinterpret_cast<const std::int32_t*>(v + 0x40) : -1;
        }

        void ReadSpring(State& s)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* st = cam && cam->currentState ? reinterpret_cast<std::uint8_t*>(cam->currentState.get()) : nullptr;
            if (st && cam->currentState->id.get() == RE::CameraState::kFirstPerson) {
                std::memcpy(s.spring, st + 0x40, sizeof(s.spring));
                s.springBlend = *reinterpret_cast<const float*>(st + 0x7C);
            } else {
                s.spring[0] = s.spring[1] = s.spring[2] = 0.0f;
                s.springBlend = -1.0f;
            }
            if (cam && cam->cameraRoot) {
                const auto& t = cam->cameraRoot->world.translate;
                s.camPos[0] = t.x;
                s.camPos[1] = t.y;
                s.camPos[2] = t.z;
            }
        }

        void SetSpring(bool a_disable)
        {
            if (!g_springOk || a_disable == g_springPatched) {
                return;
            }
            auto* p = reinterpret_cast<float*>(Base() + kSpringMaxDist);
            DWORD old = 0;
            if (::VirtualProtect(p, sizeof(float), PAGE_READWRITE, &old)) {
                *p = a_disable ? -1.0f : g_springOrig;
                ::VirtualProtect(p, sizeof(float), old, &old);
                g_springPatched = a_disable;
                REX::INFO("roomscale: first-person camera spring {} (threshold {:X} = {})", a_disable ? "disabled" : "restored",
                    kSpringMaxDist, *p);
            }
        }

        // ---- core ---------------------------------------------------------------------------------
        float BodyRelEff(const State& s) { return s.bodyRel + s.pendingYaw; }
        // Room yaw at a moment the heading is a_heading: owned (camera step 1) or derived from the body (B3).
        float RoomYaw(const State& s, float a_heading) { return g_cfg.ownHeading ? s.ownYaw : a_heading - BodyRelEff(s); }

        // New capsule displacement since the last observation -> attributed to the outstanding commands
        // (ours + the game's), proportionally; only our share moves the anchor. Called from the movement
        // hook and from the camera hook, so the camera always pairs data.location with a matching anchor.
        void Observe(State& s, double locX, double locY, float heading)
        {
            const double dx = locX - s.lastLocX, dy = locY - s.lastLocY;
            if (std::abs(dx) < 1e-5 && std::abs(dy) < 1e-5) {
                return;
            }
            s.lastLocX = locX;
            s.lastLocY = locY;
            if (dx * dx + dy * dy > 250.0 * 250.0) {  // teleport (coc, doors, moveto, fast travel): reset at next update
                s.jumpPending = true;
                s.cmds.clear();
                return;
            }
            double tx = 0, ty = 0, ux = 0, uy = 0;
            for (const auto& c : s.cmds) {
                tx += c.rx + c.gx;
                ty += c.ry + c.gy;
                ux += c.rx;
                uy += c.ry;
            }
            const double tt = tx * tx + ty * ty;
            double       k = 0.0, ax = 0.0, ay = 0.0;
            double ox = 0.0, oy = 0.0;  // oldest outstanding command (ours + game's)
            for (const auto& c : s.cmds) {
                if (std::abs(c.rx + c.gx) + std::abs(c.ry + c.gy) > 1e-6) {
                    ox = c.rx + c.gx;
                    oy = c.ry + c.gy;
                    break;
                }
            }
            if (tt > 1e-8 && dx * tx + dy * ty < 0.0 && dx * ox + dy * oy < 0.0) {
                // the capsule moved back against the outstanding commands (pushed out of a wall it was
                // driven into): undo that much of our attribution instead of counting it as external
                // motion, which would ratchet the anchor forward while pressing into a wall
                k = (std::max)((dx * tx + dy * ty) / tt, -1.0);
                ax = k * ux;
                ay = k * uy;
                if (g_cfg.attributeLeftover && s.recentGame < 0.01 && s.recentOurs > 0.01) {
                    ax = dx;  // only our commands were active: every bit of the capsule's settling is ours
                    ay = dy;
                }
            } else {
                // commands are realized in order (measured: one update later), so consume them oldest
                // first; what no command explains is external motion (stick-free pushes, platforms)
                double rx = dx, ry = dy;
                for (auto& c : s.cmds) {
                    const double cx = c.rx + c.gx, cy = c.ry + c.gy, c2 = cx * cx + cy * cy;
                    if (c2 < 1e-8) {
                        continue;
                    }
                    const double p = std::clamp((rx * cx + ry * cy) / c2, 0.0, 1.0);
                    if (p <= 0.0) {
                        continue;
                    }
                    ax += p * c.rx;
                    ay += p * c.ry;
                    rx -= p * cx;
                    ry -= p * cy;
                    c.rx *= 1.0 - p;
                    c.ry *= 1.0 - p;
                    c.gx *= 1.0 - p;
                    c.gy *= 1.0 - p;
                }
                const double d2 = dx * dx + dy * dy;
                k = d2 > 1e-12 ? 1.0 - std::sqrt((rx * rx + ry * ry) / d2) : 0.0;  // explained fraction
                // what no outstanding command explains: the controller's over-delivery / coasting of recent commands
                // (ours and the game's, split by recent activity), or external motion when neither was active
                const double lo = std::sqrt(rx * rx + ry * ry);
                const double wo = s.recentOurs, wg = s.recentGame;
                if (g_cfg.attributeLeftover && lo > 0.0 && wo + wg > 0.01) {
                    const double f = wo / (wo + wg);
                    ax += f * rx;
                    ay += f * ry;
                    s.leftoverOurs += f * lo;
                    s.leftoverExt += (1.0 - f) * lo;
                } else {
                    s.leftoverExt += lo;
                }
            }
            s.dAccX += dx;
            s.dAccY += dy;
            s.aAccX += ax;
            s.aAccY += ay;
            s.lastK = static_cast<float>(k);
            if (ax != 0.0 || ay != 0.0) {
                double rx, ry;
                const float roomYaw = RoomYaw(s, heading);
                Rot(-roomYaw, ax, ay, rx, ry);  // world -> room
                s.ax += rx / g_cfg.unitsPerMeter;
                s.ay += ry / g_cfg.unitsPerMeter;
            }
        }

        float HmdYaw(bool& a_ok)
        {
            head::Mat3 m;
            a_ok = head::Get(m);
            if (!a_ok) {
                return 0.0f;
            }
            float fx = m.m[0][1], fy = m.m[1][1];
            if (fx * fx + fy * fy < 0.09f) {  // looking almost straight up/down: use the up vector
                const float sgn = m.m[2][1] < 0.0f ? 1.0f : -1.0f;
                fx = sgn * m.m[0][2];
                fy = sgn * m.m[1][2];
            }
            return std::atan2(fx, fy);
        }

        // kStart: tracking (re)appeared or roomscale switched on: the capsule is under the recentre origin and
        //         the room is aligned with the body, so the camera stays where the plain head offset put it
        //         (the capsule then walks to the head).
        // kTeleport: the game moved the player (load, door, coc, PositionPlayer) or the user recentred: the
        //         capsule is under the head and the view faces the body's heading.
        // kGap: no movement update for a while (menus): only the bookkeeping restarts.
        enum class ResetMode { kStart, kTeleport, kGap };

        void Reset(State& s, const char* a_why, ResetMode a_mode, double locX, double locY, float heading, const head::Vec3& hmd,
            float hmdYaw, bool hmdYawOk)
        {
            const bool full = a_mode != ResetMode::kGap;
            if (a_mode == ResetMode::kStart || !g_cfg.roomscale) {
                s.ax = s.ay = 0.0;
            } else if (a_mode == ResetMode::kTeleport) {
                s.ax = hmd.x;
                s.ay = hmd.y;
            }
            s.jumpPending = false;
            s.lastLocX = locX;
            s.lastLocY = locY;
            s.cmds.clear();
            s.recentOurs = s.recentGame = 0.0;
            s.moving = false;
            s.eta = 1.0f;
            s.pendingYaw = 0.0f;
            s.yawGamePrev = 0.0f;
            s.haveExpect = false;
            s.prevHeading = heading;
            if (a_mode == ResetMode::kStart || !s.valid) {
                s.bodyRel = 0.0f;
                s.following = false;
            } else if (a_mode == ResetMode::kTeleport) {
                // the view faces the body's heading again: roomYaw = heading - hmdYaw
                s.bodyRel = (g_cfg.bodyFollows && hmdYawOk) ? hmdYaw : 0.0f;
                s.following = false;
            }
            if (full || !s.valid) {
                s.ownYaw = Wrap(heading - s.bodyRel);  // the view faces the body's heading (minus the head yaw)
            }
            s.valid = true;
            s.recenterSeen = head::RecenterCount();
            ++s.resets;
            s.lastReset = a_why;
            REX::INFO("roomscale: reset ({}{}) anchor=({:.3f},{:.3f}) bodyRel={:.1f} deg", a_why, full ? ", full" : "", s.ax, s.ay,
                s.bodyRel / kD2R);
        }

        const char* Gate(RE::PlayerCharacter* p, State& s, std::uint8_t* ctrl)
        {
            s.camState = game::CurrentCameraState();
            s.sitSleep = static_cast<int>(static_cast<RE::ActorState*>(p)->DoGetSitSleepState());
            auto* mc = p->movementController.get();
            s.controlsDriven = mc ? *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uint8_t*>(mc) + 0x1A1) : -1;
            s.vats = VatsMode();
            auto* iem = RE::BSInputEnableManager::GetSingleton();
            s.inputFlags = iem ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(iem) + 0x118) : 0xFFFFFFFF;
            if (s.camState != 0) return "camera not first person";
            if (s.sitSleep != 0) return "sit/sleep state";
            if (s.controlsDriven != 1) return "not controls-driven (AI)";
            if (s.vats != 0) return "vats";
            if (!ctrl) return "no char controller";
            if (s.ctrlState == 4) return "flying";
            if (!(s.inputFlags & 1)) return "movement input disabled";
            return nullptr;
        }

        void HkModifyMovementData(RE::Actor* a_this, float a_dt, RE::NiPoint3& a_move, RE::NiPoint3& a_angle)
        {
            g_origMmd(a_this, a_dt, a_move, a_angle);
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base();
            auto*      player = RE::PlayerCharacter::GetSingleton();
            if (!player || a_this != player) {
                return;
            }
            if (ret == kRetPrimary) {
                head::WaitForPublish(1);  // pose sync (HeadPose.h): before taking g_lock, never while holding it
            }
            std::scoped_lock l(g_lock);
            State& s = g_st;
            if (ret != kRetPrimary) {
                // root-motion path (graph-driven) or unknown caller: never inject
                ++(ret == kRetGraph ? s.callsGraph : s.callsOther);
                return;
            }
            ++s.callsPrimary;
            ++s.frame;
            g_primaryUpdates.fetch_add(1, std::memory_order_relaxed);
            if (!g_ppRegistered) {
                static_cast<RE::BSTEventSource<RE::PositionPlayerEvent>*>(player)->RegisterSink(&g_ppSink);
                g_ppRegistered = true;
                REX::INFO("roomscale: PositionPlayerEvent sink registered");
            }
            const Cfg& c = g_cfg;
            const auto now = std::chrono::steady_clock::now();
            const double gapS = s.lastCall.time_since_epoch().count() ? std::chrono::duration<double>(now - s.lastCall).count() : 0.0;
            s.lastCall = now;

            const auto& loc = player->data.location;
            const float heading = player->data.angle.z;
            s.loc[0] = loc.x; s.loc[1] = loc.y; s.loc[2] = loc.z;
            s.heading = heading;
            s.looking = player->data.angle.x;
            s.gLocal[0] = a_move.x; s.gLocal[1] = a_move.y; s.gLocal[2] = a_move.z;
            // UserTurn (snap/smooth turn from other features): becomes part of the user's turn input, so it turns
            // the body (ApplyMovementDelta) and, with the owned heading, the room/view by exactly the same amount
            if (const float t = g_userTurn.exchange(0.0f); t != 0.0f) {
                a_angle.z += t;
                s.userTurnTotal += t;
                ++s.userTurns;
            }
            s.gameYaw = a_angle.z;
            auto* ctrl = CharController(player);
            if (ctrl) {
                s.ctrlState = *reinterpret_cast<const std::int32_t*>(ctrl + 0x2C0);
                s.numColl = *reinterpret_cast<const std::int32_t*>(ctrl + 0x434);
                s.sticky = *reinterpret_cast<const std::uint8_t*>(ctrl + 0x43D);
                s.ctrlFlags = *reinterpret_cast<const std::uint32_t*>(ctrl + 0x300);
                std::memcpy(s.ctrlVel, ctrl + 0x250, sizeof(s.ctrlVel));
                s.ctrlRadius = *reinterpret_cast<const float*>(ctrl + 0x370);
            } else {
                s.ctrlState = s.numColl = s.sticky = -1;
            }
            s.tcl = *reinterpret_cast<const std::uint8_t*>(Base() + kTclFlag);
            ReadSpring(s);

            head::Vec3 hmd;
            unsigned long long hseq = 0;
            const bool haveOff = head::GetOffset(hmd, hseq);
            if (head::TraceArmed()) {  // htrace M: primary movement update (runs only while the game updates)
                head::TraceRec r{ 'M' };
                r.seq = hseq;
                r.v[0] = a_dt, r.v[1] = heading, r.v[2] = s.ownYaw, r.v[3] = s.gameYaw;
                r.v[4] = static_cast<float>(loc.x), r.v[5] = static_cast<float>(loc.y), r.v[6] = static_cast<float>(loc.z);
                r.v[7] = a_move.x, r.v[8] = a_move.y;
                head::Trace(r);
            }
            bool       yawOk = false;
            const float hmdYaw = HmdYaw(yawOk);
            s.hmd[0] = hmd.x; s.hmd[1] = hmd.y; s.hmd[2] = hmd.z;
            s.hmdYaw = hmdYaw;
            s.injWorld[0] = s.injWorld[1] = 0.0f;
            s.yawInjected = 0.0f;
            for (int i = 0; i < 3; ++i) s.gLocalAfter[i] = s.gLocal[i];

            const bool on = (c.roomscale || c.bodyFollows || c.ownHeading) && haveOff;
            SetSpring(on && c.roomscale && c.noSpring);
            if (!on) {
                s.valid = false;
                s.haveExpect = false;
                s.gate = haveOff ? "off" : "no head pose";
                return;
            }

            // ---- resets ----
            auto* cell = player->GetParentCell();
            const char* why = nullptr;
            ResetMode   mode = ResetMode::kTeleport;
            if (!s.valid) {
                why = "start";
                mode = ResetMode::kStart;
            }
            else if (g_requestReset.exchange(0)) why = "command";
            else if (s.jumpPending) why = "position jump";
            else if (head::RecenterCount() != s.recenterSeen) why = "recenter";
            else if (g_positionPlayer.exchange(false)) why = "PositionPlayer";
            else if (cell != s.cell && (!cell || !s.cell || cell->IsInterior() || s.cell->IsInterior() ||
                                           cell->worldSpace != s.cell->worldSpace)) why = "cell change";
            else {
                const double jx = loc.x - s.lastLocX, jy = loc.y - s.lastLocY;
                if (std::sqrt(jx * jx + jy * jy) > 200.0 + 1000.0 * std::clamp(static_cast<double>(a_dt), 0.0, 0.5)) {
                    why = "position jump";
                } else if (gapS > 2.0) {
                    why = "update gap";
                    mode = ResetMode::kGap;
                }
            }
            s.cell = cell;
            g_positionPlayer = false;
            if (why) {
                Reset(s, why, mode, loc.x, loc.y, heading, hmd, hmdYaw, yawOk);
                s.gate = "reset";
                return;
            }

            // ---- camera step 2: heading change the last update's angleDelta.z does not explain (setangle, scripts, VATS
            //      aim, furniture/PA alignment) ----
            if (s.haveExpect && VatsMode() == 0) {  // VATS aim turns are left alone (owned yaw keeps the view)
                const float dg = Wrap(heading - s.expectHeading);
                if (std::abs(dg) > 0.5f * kD2R) {
                    s.lastGameTurn = dg;
                    ++s.gameTurns;
                    if (const float thr = g_snapThreshold.load(std::memory_order_relaxed); thr > 0.0f && std::abs(dg) >= thr) {
                        g_freezeBody = true;  // the director fades and snaps; B3 must not turn the body back meanwhile
                    }
                    if (!g_gameTurnSeen || std::abs(dg) > std::abs(g_gameTurnJump)) {
                        g_gameTurnJump = dg;
                        g_gameTurnSeen = true;
                    }
                }
            }

            // ---- B3 bookkeeping: confirm last update's yaw injection from the measured heading change ----
            if (c.ownHeading) {
                // owned room yaw: whatever turned the body since the last update (our injection, setangle, VATS
                // aim, furniture, ...) is body-relative; only the user's turn input (added below) turns the room
                s.bodyRel = Wrap(heading - s.ownYaw);
                s.pendingYaw = 0.0f;
            } else if (s.pendingYaw != 0.0f) {
                const float dh = Wrap(heading - s.prevHeading);
                const float expect = s.yawGamePrev + s.pendingYaw;
                const float ratio = std::abs(expect) > 1e-6f ? std::clamp(dh / expect, 0.0f, 1.0f) : 0.0f;
                s.bodyRel = Wrap(s.bodyRel + s.pendingYaw * ratio);
                s.pendingYaw = 0.0f;
            }
            s.prevHeading = heading;
            s.yawGamePrev = a_angle.z;

            // head speed: the capsule keeps following while the head is still moving
            if (a_dt > 0.0f) {
                const double vx = (hmd.x - s.prevHmdX) / a_dt, vy = (hmd.y - s.prevHmdY) / a_dt;
                s.headSpeed = 0.7f * s.headSpeed + 0.3f * static_cast<float>((std::min)(std::sqrt(vx * vx + vy * vy), 20.0));
            }
            s.prevHmdX = hmd.x;
            s.prevHmdY = hmd.y;

            // ---- B4 measurement ----
            s.dAccX = s.dAccY = s.aAccX = s.aAccY = 0.0;
            Observe(s, loc.x, loc.y, heading);
            s.dLoc[0] = static_cast<float>(s.dAccX); s.dLoc[1] = static_cast<float>(s.dAccY);
            s.aLoc[0] = static_cast<float>(s.aAccX); s.aLoc[1] = static_cast<float>(s.aAccY);
            // age / expire outstanding commands; the realized fraction of expired ones feeds eta
            for (auto& e : s.cmds) ++e.age;
            while (!s.cmds.empty() && s.cmds.front().age > c.expireFrames) {
                const auto& e = s.cmds.front();
                if (e.r0 > 1.0) {  // small end-of-move commands are within the controller's noise
                    const double rem = std::sqrt(e.rx * e.rx + e.ry * e.ry);
                    s.eta = 0.7f * s.eta + 0.3f * static_cast<float>(std::clamp(1.0 - rem / e.r0, 0.0, 1.0));
                }
                s.cmds.pop_front();
            }

            const float roomYaw = heading - s.bodyRel;
            const char* gate = Gate(player, s, ctrl);
            const bool  blocked = s.eta < 0.4f;
            s.gate = gate ? gate : (!c.roomscale ? "translation off" : blocked ? "blocked" : "-");

            double gwx = 0, gwy = 0;
            Rot(heading, a_move.x, a_move.y, gwx, gwy);  // game's command, local -> world
            double rwx = 0, rwy = 0;

            if (c.roomscale) {
                double ex = hmd.x - s.ax, ey = hmd.y - s.ay;  // head - capsule, room metres
                double en = std::sqrt(ex * ex + ey * ey);
                const double lim = (gate || blocked) ? c.maxResidualCm * 0.01 : (std::max)(c.hardMaxCm, c.maxResidualCm) * 0.01;
                if (en > lim) {  // slide the room: the head never gets further than lim from the capsule
                    const double f = (en - lim) / en;
                    s.ax += ex * f;
                    s.ay += ey * f;
                    ex -= ex * f;
                    ey -= ey * f;
                    en = lim;
                }
                // subtract what is already commanded but not yet realized (Smith predictor)
                double ux = 0, uy = 0;
                for (const auto& e : s.cmds) {
                    ux += e.rx;
                    uy += e.ry;
                }
                double urx, ury;
                Rot(-roomYaw, ux, uy, urx, ury);
                const double fx = ex - urx / c.unitsPerMeter, fy = ey - ury / c.unitsPerMeter;
                const double fn = std::sqrt(fx * fx + fy * fy);
                s.e[0] = static_cast<float>(ex * 100.0);
                s.e[1] = static_cast<float>(ey * 100.0);
                s.eCm = static_cast<float>(en * 100.0);
                s.eEffCm = static_cast<float>(fn * 100.0);
                if (gate) {
                    s.moving = false;
                } else if (!s.moving && en * 100.0 > c.deadzoneCm) {
                    s.moving = true;
                } else if (s.moving && fn * 100.0 < c.stopCm && en * 100.0 < c.stopCm * 2.0 && s.headSpeed < 0.15f) {
                    s.moving = false;
                }
                const bool stickyStop = s.sticky == 1 && s.numColl > 0;  // the engine zeroed movement on purpose
                if (s.moving && !stickyStop && a_dt > 0.0f) {
                    double wx = fx * c.gain, wy = fy * c.gain;  // metres this update
                    const double wn = std::sqrt(wx * wx + wy * wy), wmax = c.maxSpeed * a_dt;
                    if (wn > wmax) {
                        wx *= wmax / wn;
                        wy *= wmax / wn;
                    }
                    Rot(roomYaw, wx * c.unitsPerMeter, wy * c.unitsPerMeter, rwx, rwy);  // room -> world units
                    double lx, ly;
                    Rot(-heading, rwx, rwy, lx, ly);  // world -> actor-local
                    a_move.x += static_cast<float>(lx);
                    a_move.y += static_cast<float>(ly);
                    ++s.injectFrames;
                }
            } else {
                s.ax = s.ay = 0.0;  // camera keeps the whole head offset
                s.e[0] = s.e[1] = s.eCm = s.eEffCm = 0.0f;
            }
            {
                const double decay = std::exp(-static_cast<double>((std::max)(a_dt, 0.0f)) / (std::max)(c.attrWindowS, 0.01f));
                s.recentOurs = s.recentOurs * decay + std::sqrt(rwx * rwx + rwy * rwy);
                s.recentGame = s.recentGame * decay + std::sqrt(gwx * gwx + gwy * gwy);
            }
            CmdEntry ce;
            ce.rx = rwx; ce.ry = rwy; ce.gx = gwx; ce.gy = gwy;
            ce.r0 = std::sqrt(rwx * rwx + rwy * rwy);
            if (ce.r0 > 1e-6 || gwx != 0.0 || gwy != 0.0) {
                s.cmds.push_back(ce);
            }
            s.injWorld[0] = static_cast<float>(rwx);
            s.injWorld[1] = static_cast<float>(rwy);

            // ---- B3: body yaw follows the head ----
            if (c.bodyFollows && !gate && yawOk && a_dt > 0.0f && !g_freezeBody.load(std::memory_order_relaxed)) {
                const float d = Wrap(hmdYaw - s.bodyRel);
                const float stickSpeed = std::sqrt(s.gLocal[0] * s.gLocal[0] + s.gLocal[1] * s.gLocal[1]) / a_dt;
                const bool stickMoving = stickSpeed > 20.0f;  // game units/s from the stick
                if (std::abs(d) > c.yawThresholdDeg * kD2R || stickMoving) {
                    s.following = true;
                } else if (std::abs(d) < c.yawStopDeg * kD2R) {
                    s.following = false;
                }
                if (s.following) {
                    const float maxStep = c.yawRateDeg * kD2R * a_dt;
                    const float step = std::clamp(d, -maxStep, maxStep);
                    a_angle.z += step;
                    s.pendingYaw = step;
                    s.yawInjected = step;
                }
            } else {
                s.following = false;
            }
            if (c.ownHeading && s.gameYaw != 0.0f) {
                // the game's own angleDelta.z of the primary movement update = the user's turn (stick / mouse) plus
                // magnetism: it turns the room (and the view); applied by ApplyMovementDelta right after this hook
                s.ownYaw = Wrap(s.ownYaw + s.gameYaw);
                ++s.ownTurns;
            }
            s.gLocalAfter[0] = a_move.x; s.gLocalAfter[1] = a_move.y; s.gLocalAfter[2] = a_move.z;
            s.expectHeading = Wrap(heading + a_angle.z);  // ApplyMovementDelta: SetHeading(GetHeading() + angleDelta.z)
            s.haveExpect = true;

            s.hist.push_back({ s.frame, s.injWorld[0], s.injWorld[1], s.dLoc[0], s.dLoc[1], s.aLoc[0], s.aLoc[1], s.lastK, s.eCm,
                static_cast<int>(s.cmds.size()), a_dt, static_cast<float>(gwx), static_cast<float>(gwy),
                static_cast<float>(loc.x - s.prevHookLocX), static_cast<float>(loc.y - s.prevHookLocY) });
            while (s.hist.size() > 240) s.hist.pop_front();
            s.prevHookLocX = loc.x;
            s.prevHookLocY = loc.y;
        }

        // body-yaw correction the camera applies on top of the game's camera yaw (heading convention)
        float CameraYawDelta(const State& s, float a_baseYaw, float a_heading)
        {
            if (g_cfg.ownHeading) {
                return Wrap(s.ownYaw - a_baseYaw);  // owned: the view yaw never depends on the game's heading
            }
            const float roomYaw = a_heading - BodyRelEff(s);
            const float d = Wrap(roomYaw - a_baseYaw);
            // the camera's base yaw normally equals the heading; if it does not (odd camera situations),
            // only apply the body-relative part
            return std::abs(Wrap(a_heading - a_baseYaw)) < 20.0f * kD2R ? d : -BodyRelEff(s);
        }

        bool ParseFloat(const std::string& a, float& v)
        {
            char*       end = nullptr;
            const float f = std::strtof(a.c_str(), &end);
            if (end == a.c_str()) return false;
            v = f;
            return true;
        }
    }

    namespace
    {
        // 20 Nov (live settings, ConfigStore.h): the [Roomscale] tuning keys that changed (and fUnitsPerMeter), under the lock.
        // Restart-only: [Camera] bOwnHeading (the first-person yaw owner: switching it re-anchors the view; head-smoothness
        // rule, A/B through "rs cfg own"), [Roomscale] bHeadAuthority / fEyeHeightSmoothS (GameBridge, first-person camera
        // source / eye-height filter).
        void ReloadLive(const cfgstore::Changes& a_ch)
        {
            const auto path = Settings::Path();
            auto rd = [&](const char* sec, const char* k, float d) {
                char buf[64]{};
                ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), path.c_str());
                char*       end = nullptr;
                const float v = std::strtof(buf, &end);
                return end != buf ? v : d;
            };
            std::scoped_lock l(g_lock);
            Cfg&        c = g_cfg;
            const Cfg   d{};
            std::string what;
            auto F = [&](const char* k, float& v, float def) {
                if (!a_ch.Has("Roomscale", k)) return;
                v = rd("Roomscale", k, def);
                what += std::format(" {}={}", k, v);
            };
            auto B = [&](const char* k, bool& v, bool def) {
                if (!a_ch.Has("Roomscale", k)) return;
                v = rd("Roomscale", k, def ? 1.0f : 0.0f) != 0.0f;
                what += std::format(" {}={}", k, v ? 1 : 0);
            };
            B("bRoomscale", c.roomscale, d.roomscale);
            B("bBodyFollowsHead", c.bodyFollows, d.bodyFollows);
            B("bNoCameraSpring", c.noSpring, d.noSpring);  // the spring patch follows it every update (SetSpring)
            F("fDeadzoneCm", c.deadzoneCm, d.deadzoneCm);
            F("fStopCm", c.stopCm, d.stopCm);
            F("fMaxResidualCm", c.maxResidualCm, d.maxResidualCm);
            F("fHardMaxResidualCm", c.hardMaxCm, d.hardMaxCm);
            F("fMaxSpeed", c.maxSpeed, d.maxSpeed);
            F("fGain", c.gain, d.gain);
            F("fBodyYawThresholdDeg", c.yawThresholdDeg, d.yawThresholdDeg);
            F("fBodyYawRateDeg", c.yawRateDeg, d.yawRateDeg);
            B("bAttributeOverDelivery", c.attributeLeftover, d.attributeLeftover);
            F("fAttributionWindowS", c.attrWindowS, d.attrWindowS);
            if (a_ch.Has("Roomscale", "iCameraFeedbackFix")) {
                g_tfix = std::clamp(static_cast<int>(rd("Roomscale", "iCameraFeedbackFix", 1.0f)), 0, 2);
                what += std::format(" iCameraFeedbackFix={}", g_tfix.load());
            }
            if (a_ch.Has("General", "fUnitsPerMeter")) {
                const float u = rd("General", "fUnitsPerMeter", 70.0f);
                c.unitsPerMeter = u > 1.0f ? u : 70.0f;
                what += std::format(" unitsPerMeter={}", c.unitsPerMeter);
            }
            if (!what.empty()) REX::INFO("roomscale: settings re-read (live):{}", what);
        }
    }

    std::string Install()
    {
        static std::once_flag s_sub;
        std::call_once(s_sub, [] { cfgstore::Subscribe("roomscale", "Roomscale,General/fUnitsPerMeter", &ReloadLive); });
        std::scoped_lock l(g_lock);
        if (g_installed) {
            return "already installed";
        }
        const auto path = Settings::Path();
        auto rd = [&](const char* k, float d) {
            char buf[64]{};
            ::GetPrivateProfileStringA("Roomscale", k, "", buf, sizeof(buf), path.c_str());
            char* end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : d;
        };
        Cfg& c = g_cfg;
        c.roomscale = rd("bRoomscale", 1.0f) != 0.0f;
        c.bodyFollows = rd("bBodyFollowsHead", 1.0f) != 0.0f;
        c.noSpring = rd("bNoCameraSpring", 1.0f) != 0.0f;
        c.deadzoneCm = rd("fDeadzoneCm", c.deadzoneCm);
        c.stopCm = rd("fStopCm", c.stopCm);
        c.maxResidualCm = rd("fMaxResidualCm", c.maxResidualCm);
        c.hardMaxCm = rd("fHardMaxResidualCm", c.hardMaxCm);
        c.maxSpeed = rd("fMaxSpeed", c.maxSpeed);
        c.gain = rd("fGain", c.gain);
        c.yawThresholdDeg = rd("fBodyYawThresholdDeg", c.yawThresholdDeg);
        c.yawRateDeg = rd("fBodyYawRateDeg", c.yawRateDeg);
        g_tfix = std::clamp(static_cast<int>(rd("iCameraFeedbackFix", 1.0f)), 0, 2);
        c.attributeLeftover = rd("bAttributeOverDelivery", 1.0f) != 0.0f;
        c.attrWindowS = rd("fAttributionWindowS", c.attrWindowS);
        {
            // camera step 1 switch lives under [Camera] (CameraDirector.h)
            c.ownHeading = ::GetPrivateProfileIntA("Camera", "bOwnHeading", 1, path.c_str()) != 0;
        }
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("General", "fUnitsPerMeter", "70", buf, sizeof(buf), path.c_str());
            const float u = std::strtof(buf, nullptr);
            c.unitsPerMeter = u > 1.0f ? u : 70.0f;
        }

        const auto base = Base();
        // spring threshold: must hold 300.0f
        {
            const float v = *reinterpret_cast<const float*>(base + kSpringMaxDist);
            g_springOk = v == 300.0f;
            g_springOrig = v;
            if (!g_springOk) {
                REX::ERROR("roomscale: spring threshold at {:X} is {} (expected 300); spring left alone", kSpringMaxDist, v);
            }
        }
        // the two vcall sites (ff 90 28 09 00 00 = call [rax+0x928])
        static constexpr std::uint8_t kVcall[6] = { 0xFF, 0x90, 0x28, 0x09, 0x00, 0x00 };
        const bool siteA = std::memcmp(reinterpret_cast<const void*>(base + kCallPrimary), kVcall, 6) == 0;
        const bool siteB = std::memcmp(reinterpret_cast<const void*>(base + kCallGraph), kVcall, 6) == 0;

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerCharacter[0] };
        const std::uintptr_t slotAddr = vtbl.address() + kSlot * sizeof(void*);
        const std::uintptr_t cur = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
        if (slotAddr != base + kVtblSlotRva || cur != base + kOrigMmdRva || !siteA || !siteB) {
            REX::ERROR("roomscale: byte check failed (slot {:X} expected {:X}, value {:X} expected {:X}, vcall C66EF3 {}, CAC5B1 {}); "
                       "NOT patching",
                slotAddr - base, kVtblSlotRva, cur - base, kOrigMmdRva, siteA, siteB);
            return "byte check FAILED";
        }
        g_origMmd = reinterpret_cast<Mmd_t>(vtbl.write_vfunc(kSlot, &HkModifyMovementData));
        g_installed = true;
        const auto s = std::format("vtable slot 0x125 @ {:X} ({:X} -> hook); roomscale={} bodyFollows={} noSpring={} deadzone={}cm "
                                   "maxResidual={}cm cameraFeedbackFix={} ownHeading={}",
            kVtblSlotRva, kOrigMmdRva, c.roomscale, c.bodyFollows, c.noSpring, c.deadzoneCm, c.maxResidualCm, g_tfix.load(), c.ownHeading);
        REX::INFO("roomscale: {}", s);
        return s;
    }

    void AdjustHeadRotation(head::Mat3& a_head, float a_baseYaw)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        std::scoped_lock l(g_lock);
        State& s = g_st;
        if (!g_installed || !s.valid || !g_cfg.bodyFollows || !player) {
            return;
        }
        const float dlt = CameraYawDelta(s, a_baseYaw, player->data.angle.z);
        // heading +d (turn right) = rotation about +Z by -d (column-vector convention): M' = Rz(-d) * M
        const float c = std::cos(-dlt), sn = std::sin(-dlt);
        head::Mat3 r = a_head;
        for (int j = 0; j < 3; ++j) {
            r.m[0][j] = c * a_head.m[0][j] - sn * a_head.m[1][j];
            r.m[1][j] = sn * a_head.m[0][j] + c * a_head.m[1][j];
        }
        a_head = r;
        s.baseYawRot = a_baseYaw;
        const float fx = a_head.m[0][1], fy = a_head.m[1][1];
        s.camYaw = Wrap(a_baseYaw + std::atan2(fx, fy));
    }

    void AdjustHeadOffset(head::Vec3& a_offsetM, float a_baseYaw)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        std::scoped_lock l(g_lock);
        State& s = g_st;
        if (!g_installed || !s.valid || !player) {
            return;
        }
        const float heading = player->data.angle.z;
        if (g_cfg.roomscale) {
            Observe(s, player->data.location.x, player->data.location.y, heading);
        }
        double ox = a_offsetM.x - s.ax, oy = a_offsetM.y - s.ay;
        if (g_cfg.roomscale) {
            const double n = std::sqrt(ox * ox + oy * oy), lim = (std::max)(g_cfg.hardMaxCm, g_cfg.maxResidualCm) * 0.01;
            if (n > lim) {
                ox *= lim / n;
                oy *= lim / n;
            }
            s.residualCm = static_cast<float>(std::sqrt(ox * ox + oy * oy) * 100.0);
        }
        // room -> the frame GameBridge rotates by a_baseYaw: R(roomYaw - baseYaw)
        const float dlt = CameraYawDelta(s, a_baseYaw, heading);
        double bx, by;
        Rot(dlt, ox, oy, bx, by);
        a_offsetM.x = static_cast<float>(bx);
        a_offsetM.y = static_cast<float>(by);
    }

    bool OwnedYaw(float& a_yaw)
    {
        std::scoped_lock l(g_lock);
        if (!g_installed || !g_cfg.ownHeading || !g_st.valid) {
            return false;
        }
        a_yaw = g_st.ownYaw;
        return true;
    }

    bool TakeGameTurn(float& a_radians)
    {
        std::scoped_lock l(g_lock);
        if (!g_gameTurnSeen) {
            return false;
        }
        a_radians = g_gameTurnJump;
        g_gameTurnSeen = false;
        g_gameTurnJump = 0.0f;
        return true;
    }

    std::string SnapViewToHeading()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        bool  yawOk = false;
        const float hmdYaw = HmdYaw(yawOk);
        std::scoped_lock l(g_lock);
        State& s = g_st;
        if (!player || !g_installed || !g_cfg.ownHeading || !s.valid) {
            return "not applied (no owned heading)";
        }
        const float heading = player->data.angle.z, before = s.ownYaw;
        s.ownYaw = Wrap(heading - (yawOk ? hmdYaw : 0.0f));  // view yaw = ownYaw + head yaw = the body's heading
        s.bodyRel = Wrap(heading - s.ownYaw);
        s.following = false;
        ++s.headingSnaps;
        g_freezeBody = false;
        return std::format("ownYaw {:.1f} -> {:.1f} deg (heading {:.1f}, head yaw {:.1f})", before / kD2R, s.ownYaw / kD2R, heading / kD2R,
            (yawOk ? hmdYaw : 0.0f) / kD2R);
    }

    void FreezeBodyFollow(bool a_freeze)
    {
        g_freezeBody = a_freeze;
    }

    void SetHeadingSnapThreshold(float a_radians)
    {
        g_snapThreshold = std::isfinite(a_radians) && a_radians > 0.0f ? a_radians : 0.0f;
    }

    std::uint64_t PrimaryUpdates()
    {
        return g_primaryUpdates.load(std::memory_order_relaxed);
    }

    void UserTurn(float a_radians)
    {
        if (std::isfinite(a_radians)) {
            float cur = g_userTurn.load();
            while (!g_userTurn.compare_exchange_weak(cur, cur + a_radians)) {
            }
        }
    }

    bool OwnHeadingEnabled()
    {
        std::scoped_lock l(g_lock);
        return g_installed && g_cfg.ownHeading;
    }

    int TranslationFeedbackMode()
    {
        return g_tfix.load(std::memory_order_relaxed);
    }

    void NoteCameraBase(float a_x, float a_y, float a_z, float a_prevAddX, float a_prevAddY)
    {
        std::scoped_lock l(g_lock);
        g_tRaw[0] = a_x;
        g_tRaw[1] = a_y;
        g_tRaw[2] = a_z;
        g_tPrevAdd[0] = a_prevAddX;
        g_tPrevAdd[1] = a_prevAddY;
    }

    void NoteCameraFrame(float a_x, float a_y, float a_z, float a_addX, float a_addY, float a_dX, float a_dY, int a_getPosIn,
        int a_getPosOut)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        head::Vec3 hmd{};
        const bool haveHmd = head::GetOffset(hmd);
        std::scoped_lock l(g_lock);
        const auto now = std::chrono::steady_clock::now();
        if (!g_camT0.time_since_epoch().count()) {
            g_camT0 = now;
        }
        CamFrame f{};
        f.t = std::chrono::duration<double, std::milli>(now - g_camT0).count();
        f.root[0] = a_x; f.root[1] = a_y; f.root[2] = a_z;
        if (player) {
            f.loc[0] = player->data.location.x; f.loc[1] = player->data.location.y; f.loc[2] = player->data.location.z;
            f.heading = player->data.angle.z;
        }
        std::memcpy(f.raw, g_tRaw, sizeof(f.raw));
        f.add[0] = a_addX; f.add[1] = a_addY; f.d[0] = a_dX; f.d[1] = a_dY;
        if (haveHmd) {
            f.hmd[0] = hmd.x; f.hmd[1] = hmd.y;
        }
        f.anchor[0] = static_cast<float>(g_st.ax); f.anchor[1] = static_cast<float>(g_st.ay);
        f.yaw = RoomYaw(g_st, f.heading);
        f.in = a_getPosIn;
        f.out = a_getPosOut;
        f.mvFrame = g_st.frame;
        g_camHist.push_back(f);
        while (g_camHist.size() > 900) g_camHist.pop_front();
    }

    std::string Command(const std::string& a_arg)
    {
        std::scoped_lock l(g_lock);
        Cfg& c = g_cfg;
        if (a_arg == "on" || a_arg == "off") {
            c.roomscale = a_arg == "on";
            g_st.valid = false;  // re-anchor
            return std::format("roomscale {} (installed={})", c.roomscale ? "on" : "off", g_installed ? 1 : 0);
        }
        if (a_arg.rfind("turn ", 0) == 0) {  // rs turn <deg>: UserTurn test (+ = right)
            const float deg = std::strtof(a_arg.c_str() + 5, nullptr);
            UserTurn(deg * kD2R);
            return std::format("user turn {:.1f} deg queued", deg);
        }
        if (a_arg == "reset") {
            g_requestReset = 1;
            return "reset requested";
        }
        if (a_arg == "hist") {
            // per update: r = our command, g = game's (world units), dh = loc change since the previous update,
            // d/a = displacement observed / attributed to us at this update (the camera hook observes too)
            std::string out = "hist";
            for (const auto& h : g_st.hist) {
                out += std::format(" | f{} dt={:.4f} r=({:.2f},{:.2f}) g=({:.2f},{:.2f}) dh=({:.2f},{:.2f}) d=({:.2f},{:.2f}) a=({:.2f},{:.2f}) k={:.2f} e={:.1f} n={}",
                    h.frame, h.dt, h.rx, h.ry, h.gx, h.gy, h.hx, h.hy, h.dx, h.dy, h.ax, h.ay, h.k, h.e, h.outstanding);
            }
            return out;
        }
        if (a_arg.rfind("camhist", 0) == 0) {
            // per camera update: t ms, mv frame, root xyz, loc xyz, raw xyz, add xy, d xy, hmd xy (m), anchor xy (m), yaw, heading, in/out
            std::size_t n = a_arg.size() > 7 ? std::strtoul(a_arg.c_str() + 7, nullptr, 10) : 300;
            n = (std::min)(n, g_camHist.size());
            std::string out = std::format("camhist {}", n);
            for (std::size_t i = g_camHist.size() - n; i < g_camHist.size(); ++i) {
                const auto& f = g_camHist[i];
                out += std::format(" | {:.1f} {} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} "
                                   "{:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {} {}",
                    f.t, f.mvFrame, f.root[0], f.root[1], f.root[2], f.loc[0], f.loc[1], f.loc[2], f.raw[0], f.raw[1], f.raw[2], f.add[0],
                    f.add[1], f.d[0], f.d[1], f.hmd[0], f.hmd[1], f.anchor[0], f.anchor[1], f.yaw, f.heading, f.in, f.out);
            }
            return out;
        }
        if (a_arg.rfind("cfg", 0) == 0) {
            char  key[32]{};
            float v = 0;
            if (sscanf_s(a_arg.c_str() + 3, "%31s %f", key, static_cast<unsigned>(sizeof(key)), &v) == 2) {
                const std::string k = key;
                if (k == "deadzone") c.deadzoneCm = v;
                else if (k == "stop") c.stopCm = v;
                else if (k == "max") c.maxResidualCm = v;
                else if (k == "hardmax") c.hardMaxCm = v;
                else if (k == "gain") c.gain = v;
                else if (k == "speed") c.maxSpeed = v;
                else if (k == "expire") c.expireFrames = static_cast<int>(v);
                else if (k == "yawthr") c.yawThresholdDeg = v;
                else if (k == "yawrate") c.yawRateDeg = v;
                else if (k == "body") c.bodyFollows = v != 0.0f;
                else if (k == "spring") c.noSpring = v == 0.0f;  // "spring 1" = keep the game's spring
                else if (k == "tfix") g_tfix = std::clamp(static_cast<int>(v), 0, 2);
                else if (k == "attr") c.attributeLeftover = v != 0.0f;
                else if (k == "attrwin") c.attrWindowS = v;
                else if (k == "own") {
                    c.ownHeading = v != 0.0f;
                    g_st.valid = false;  // re-anchor (owned yaw from the current heading)
                }
                else if (k == "auth") game::SetHeadAuthority(v != 0.0f, game::HeadAuthorityTau());  // [Roomscale] bHeadAuthority (A/B)
                else return "error: rs cfg deadzone|stop|max|hardmax|gain|speed|expire|yawthr|yawrate|body|spring|tfix|attr|attrwin|own|auth <value>";
            }
            return std::format("cfg tfix={} attr={} attrwin={} roomscale={} body={} noSpring={} deadzone={} stop={} max={} hardmax={} gain={} speed={} expire={} "
                               "yawthr={} yawrate={} own={}",
                g_tfix.load(), c.attributeLeftover ? 1 : 0, c.attrWindowS, c.roomscale ? 1 : 0, c.bodyFollows ? 1 : 0, c.noSpring ? 1 : 0, c.deadzoneCm, c.stopCm, c.maxResidualCm, c.hardMaxCm,
                c.gain, c.maxSpeed, c.expireFrames, c.yawThresholdDeg, c.yawRateDeg, c.ownHeading ? 1 : 0);
        }
        if (!a_arg.empty()) {
            return "error: rs [on|off|reset|hist|cfg <key> <value>]";
        }
        const State& s = g_st;
        const float roomYaw = c.ownHeading ? s.ownYaw : Wrap(s.heading - s.bodyRel);
        return std::format(
            "installed={} on={} body={} valid={} gate='{}' frame={} calls={}/{}/{} injFrames={} resets={} lastReset='{}' "
            "loc=({:.2f},{:.2f},{:.2f}) heading={:.4f} looking={:.4f} "
            "ctrlState={} numColl={} sticky={} ctrlFlags={:X} ctrlVel=({:.3f},{:.3f},{:.3f}) radius={:.3f} "
            "gMove=({:.3f},{:.3f},{:.3f}) gMoveAfter=({:.3f},{:.3f},{:.3f}) inj=({:.3f},{:.3f}) dLoc=({:.3f},{:.3f}) a=({:.3f},{:.3f}) k={:.2f} "
            "anchor=({:.4f},{:.4f}) hmd=({:.4f},{:.4f},{:.4f}) e=({:.2f},{:.2f}) eCm={:.2f} eEffCm={:.2f} residualCm={:.2f} moving={} eta={:.2f} headSpeed={:.2f} "
            "outstanding={} spring=({:.3f},{:.3f},{:.3f}) springBlend={:.3f} springPatched={} tcl={} "
            "hmdYaw={:.4f} bodyRel={:.4f} roomYaw={:.4f} following={} yawInj={:.5f} gameYaw={:.5f} camYaw={:.4f} baseYaw={:.4f} "
            "camPos=({:.2f},{:.2f},{:.2f}) camState={} sitSleep={} controlsDriven={} vats={} inputFlags={:X} "
            "tRaw=({:.2f},{:.2f},{:.2f}) tPrevDelta=({:.2f},{:.2f}) tfix={} attr={} recentOurs={:.3f} recentGame={:.3f} leftoverOurs={:.2f} leftoverExt={:.2f} own={} ownYaw={:.4f} ownTurns={} userTurns={} userTurnTotal={:.4f} gameTurns={} lastGameTurn={:.4f} headingSnaps={}",
            g_installed ? 1 : 0, c.roomscale ? 1 : 0, c.bodyFollows ? 1 : 0, s.valid ? 1 : 0, s.gate, s.frame, s.callsPrimary, s.callsGraph,
            s.callsOther, s.injectFrames, s.resets, s.lastReset, s.loc[0], s.loc[1], s.loc[2], s.heading, s.looking, s.ctrlState, s.numColl,
            s.sticky, s.ctrlFlags, s.ctrlVel[0], s.ctrlVel[1], s.ctrlVel[2], s.ctrlRadius, s.gLocal[0], s.gLocal[1], s.gLocal[2],
            s.gLocalAfter[0], s.gLocalAfter[1], s.gLocalAfter[2], s.injWorld[0], s.injWorld[1], s.dLoc[0], s.dLoc[1], s.aLoc[0], s.aLoc[1],
            s.lastK, s.ax, s.ay, s.hmd[0], s.hmd[1], s.hmd[2], s.e[0], s.e[1], s.eCm, s.eEffCm, s.residualCm, s.moving ? 1 : 0, s.eta,
            s.headSpeed, s.cmds.size(), s.spring[0], s.spring[1], s.spring[2], s.springBlend, g_springPatched ? 1 : 0, s.tcl, s.hmdYaw, s.bodyRel,
            roomYaw, s.following ? 1 : 0, s.yawInjected, s.gameYaw, s.camYaw, s.baseYawRot, s.camPos[0], s.camPos[1], s.camPos[2],
            s.camState, s.sitSleep, s.controlsDriven, s.vats, s.inputFlags, g_tRaw[0], g_tRaw[1], g_tRaw[2], g_tPrevAdd[0],
            g_tPrevAdd[1], g_tfix.load(), c.attributeLeftover ? 1 : 0, s.recentOurs, s.recentGame, s.leftoverOurs, s.leftoverExt, c.ownHeading ? 1 : 0, s.ownYaw, s.ownTurns, s.userTurns, s.userTurnTotal, s.gameTurns, s.lastGameTurn, s.headingSnaps);
    }
}
