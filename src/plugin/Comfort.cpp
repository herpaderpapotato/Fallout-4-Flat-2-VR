#include "plugin/Comfort.h"

#include "plugin/CameraDirector.h"

#include "plugin/GameBridge.h"
#include "plugin/Roomscale.h"
#include "plugin/ConfigStore.h"
#include "plugin/ControllerBindings.h"
#include "plugin/Settings.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>

namespace fo4flatvr::comfort
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kD2R = kPi / 180.0f;
        constexpr float kR2D = 180.0f / kPi;

        using Clock = std::chrono::steady_clock;

        enum TurnMode : int { kSmooth = 0, kSnap = 1, kGame = 2 };
        enum MoveDir : int { kHead = 0, kHand = 1, kBody = 2 };

        const char* TurnName(int m) { return m == kSmooth ? "smooth" : m == kSnap ? "snap" : "game"; }
        const char* MoveName(int m) { return m == kHead ? "head" : m == kHand ? "hand" : "body"; }

        float Wrap(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        double Sec(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

        struct Cfg
        {
            int   turnMode{ kSmooth };  // user 14 Nov: smooth by default
            float snapDeg{ 30.0f };
            float snapThreshold{ 0.7f };   // |stick x| to fire a snap
            float snapRearm{ 0.35f };      // |stick x| below this re-arms
            float snapCooldownMs{ 250.0f };
            float smoothDegPerSec{ 120.0f };
            float turnDeadzone{ 0.15f };   // smooth: rescaled so the edge of the deadzone reads 0
            int   moveDir{ kHead };
            bool  leftHanded{ false };
            bool  vignette{ true };
            float vignetteStrength{ 0.6f };
            float vignetteFadeMs{ 100.0f };  // 15 Nov: every fade in our code defaults to 100 ms (was 150)
            float moveDeadzone{ 0.2f };    // left stick magnitude that counts as stick locomotion (vignette)
            float snapVignetteMs{ 200.0f };  // vignette held this long after a snap
        };

        struct St
        {
            // turning
            bool              armed{ true };
            Clock::time_point lastSnap{};
            float             smoothStick{ 0.0f };  // -1..1 after the deadzone, while we own smooth turning
            Clock::time_point lastPad{};            // last FilterPad
            std::uint64_t     lastUpdates{ 0 };     // rs::PrimaryUpdates() at the last change
            Clock::time_point lastUpdateSeen{};     // when it last advanced
            int               camState{ -1 };
            std::uint32_t     inputFlags{ 0 };
            bool              owns{ false };
            unsigned          ownLogs{ 0 };
            Clock::time_point turningUntil{};
            // direction
            float             deltaDeg{ 0.0f }, heading{ 0.0f }, camYaw{ 0.0f }, handRelDeg{ 0.0f };
            bool              handUsed{ false }, rotated{ false };
            float             inLx{}, inLy{}, outLx{}, outLy{}, inRx{};
            // vignette
            bool              moving{ false }, turning{ false }, target{ false };
            float             alpha{ 0.0f };
            Clock::time_point lastFade{};
            // stats (comfort stats reset)
            std::uint64_t     snaps{ 0 }, ownedFrames{ 0 };
            double            turnDegTotal{ 0 }, snapDegTotal{ 0 }, smoothDegTotal{ 0 }, smoothSec{ 0 };
            float             lastTurnDeg{ 0 };
            std::uint64_t     frames{ 0 }, vigFrames{ 0 }, vigFramesActive{ 0 }, vigFramesTail{ 0 }, activeNoVig{ 0 };
            std::uint32_t     lastLayers{ 0 }, layersWithVig{ 0 }, layersWithoutVig{ 0 };
            std::uint64_t     uploads{ 0 };
        };

        std::mutex g_lock;
        Cfg        g_cfg;
        St         g_s;
        bool       g_installed = false;

        // the player's movement update (which consumes rs::UserTurn) ran recently: the game is not paused
        bool PathLive(const St& s, Clock::time_point now)
        {
            return s.lastUpdateSeen.time_since_epoch().count() && Sec(s.lastUpdateSeen, now) < 0.15;
        }

        // yaw (heading convention) of the camera root's forward, i.e. where the head looks in the world
        bool CameraYaw(float& a_yaw)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* root = cam ? cam->cameraRoot.get() : nullptr;
            if (!root) {
                return false;
            }
            const float* f = reinterpret_cast<const float*>(&root->world.rotate);  // stored row 1 = forward (R * +Y)
            const float  fx = f[4], fy = f[5];
            if (fx * fx + fy * fy < 0.09f) {  // looking almost straight up/down
                return false;
            }
            a_yaw = std::atan2(fx, fy);
            return true;
        }

        void InnerOuter(const Cfg& c, float& a_in, float& a_out)
        {
            const float s = std::clamp(c.vignetteStrength, 0.0f, 1.0f);
            a_in = 70.0f - 50.0f * s;  // 0.6 -> clear within 40 deg of the view axis
            a_out = a_in + 25.0f;      //        opaque beyond 65 deg
        }

        std::string CfgString()
        {
            const Cfg& c = g_cfg;
            float in, out;
            InnerOuter(c, in, out);
            return std::format("turn={} snapDeg={} snapThr={} snapRearm={} snapCooldownMs={} smoothDegPerSec={} turnDeadzone={} move={} "
                               "leftHanded={} vignette={} strength={} fadeMs={} inner={:.1f} outer={:.1f} moveDeadzone={} snapVignetteMs={}",
                TurnName(c.turnMode), c.snapDeg, c.snapThreshold, c.snapRearm, c.snapCooldownMs, c.smoothDegPerSec, c.turnDeadzone,
                MoveName(c.moveDir), c.leftHanded ? 1 : 0, c.vignette ? 1 : 0, c.vignetteStrength, c.vignetteFadeMs, in, out,
                c.moveDeadzone, c.snapVignetteMs);
        }
    }

    namespace
    {
        void ReadCfgLocked();
    }

    std::string Install()
    {
        static std::once_flag s_sub;
        std::call_once(s_sub, [] {
            // 20 Nov (live settings, ConfigStore.h): every [Comfort] turn / move / vignette key and the handedness, live
            cfgstore::Subscribe("comfort", "Comfort,Controllers/bLeftHanded", [](const cfgstore::Changes&) {
                std::scoped_lock l(g_lock);
                ReadCfgLocked();
                REX::INFO("comfort: settings re-read (live): {}", CfgString());
            });
        });
        std::scoped_lock l(g_lock);
        ReadCfgLocked();
        g_installed = true;
        const auto s = CfgString();
        REX::INFO("comfort: {}", s);
        return s;
    }

    namespace
    {
    void ReadCfgLocked()
    {
        const auto path = Settings::Path();
        auto rd = [&](const char* sec, const char* k, float d) {
            char buf[64]{};
            ::GetPrivateProfileStringA(sec, k, "", buf, sizeof(buf), path.c_str());
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : d;
        };
        Cfg& c = g_cfg;
        const Cfg dflt{};  // code defaults (a key removed from every layer goes back to these)
        c.turnMode = std::clamp(static_cast<int>(rd("Comfort", "iTurnMode", static_cast<float>(dflt.turnMode))), 0, 2);
        c.snapDeg = rd("Comfort", "fSnapTurnDeg", dflt.snapDeg);
        c.snapThreshold = rd("Comfort", "fSnapThreshold", dflt.snapThreshold);
        c.snapRearm = rd("Comfort", "fSnapRearm", dflt.snapRearm);
        c.snapCooldownMs = rd("Comfort", "fSnapCooldownMs", dflt.snapCooldownMs);
        c.smoothDegPerSec = rd("Comfort", "fSmoothTurnDegPerSec", dflt.smoothDegPerSec);
        c.turnDeadzone = std::clamp(rd("Comfort", "fTurnDeadzone", dflt.turnDeadzone), 0.0f, 0.9f);
        c.moveDir = std::clamp(static_cast<int>(rd("Comfort", "iMoveDirection", static_cast<float>(dflt.moveDir))), 0, 2);
        c.vignette = rd("Comfort", "bVignette", 1.0f) != 0.0f;
        c.vignetteStrength = std::clamp(rd("Comfort", "fVignetteStrength", dflt.vignetteStrength), 0.0f, 1.0f);
        c.vignetteFadeMs = rd("Comfort", "fVignetteFadeMs", dflt.vignetteFadeMs);
        c.leftHanded = rd("Controllers", "bLeftHanded", 0.0f) != 0.0f;
    }
    }

    void FilterPad(PadFrame& a_f)
    {
        const auto now = Clock::now();
        float      camYaw = 0.0f, heading = 0.0f;
        bool       haveCam = false, havePlayer = false;
        const int  camState = game::CurrentCameraState();
        const std::uint64_t updates = rs::PrimaryUpdates();
        std::uint32_t inputFlags = 0;
        if (auto* iem = RE::BSInputEnableManager::GetSingleton()) {
            inputFlags = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(iem) + 0x118);  // as Roomscale
        }
        // 20 Nov (camera agent, [Camera] bSameInputAllViews): every head-tracked view takes the same comfort handling.
        // Turning is ours where the view's yaw is the owned heading (first person, iron sights, the body camera); in a SCENE
        // view the yaw is the game camera's, so the game keeps turning it. The stick direction is rotated from the frame the
        // game moves the player in (first person: the body heading; any other state: the game camera's own yaw, which third
        // person moves along) to the head-tracked view. The free camera is the one exception: its stick flying already follows
        // the view (camdir HkFreeCamMove), so it gets no extra rotation.
        const bool bodyView = camdir::BodyView(camState);
        const bool fpFrame = camState == 0 || camState == 4;
        if (a_f.worldView && camState != 3) {
            haveCam = CameraYaw(camYaw);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                heading = player->data.angle.z;
                havePlayer = true;
            }
            if (!fpFrame && camdir::SameInputAllViews()) {
                float gy = 0.0f;
                if (camdir::GameCameraYaw(gy)) heading = gy;
            }
        }
        std::scoped_lock l(g_lock);
        const Cfg& c = g_cfg;
        St&        s = g_s;
        const double frameDt = s.lastPad.time_since_epoch().count() ? (std::min)(Sec(s.lastPad, now), 0.1) : 0.0;
        s.lastPad = now;
        s.inLx = a_f.lx;
        s.inLy = a_f.ly;
        s.inRx = a_f.rx;
        s.camState = camState;
        s.inputFlags = inputFlags;
        if (updates != s.lastUpdates) {
            s.lastUpdates = updates;
            s.lastUpdateSeen = now;
        }
        const bool live = PathLive(s, now);

        // ---- turning: rs::UserTurn (consumed by the next primary ModifyMovementData as angleDelta.z) ----
        // Owned only in the first-person / iron-sights world view with looking enabled (bit 1 of the input-enable
        // flags) while the movement update runs; elsewhere (furniture, VATS, menus, cinema) the stick goes to the game.
        const bool ownsBefore = s.owns;
        s.owns = c.turnMode != kGame && a_f.worldView && live && (camState == 0 || camState == 4 || bodyView) && (inputFlags & 2) != 0;
        if (s.owns != ownsBefore && s.ownLogs < 400) {
            // 20 Nov (user: "Turning controls can get stuck after using vats, requiring a wiggle"): every change of the
            // turning owner with its reason, so a stuck turn in a headset session shows which gate held it
            ++s.ownLogs;
            const char* why = c.turnMode == kGame                                ? "turn mode game"
                              : !a_f.worldView                                   ? "no head-tracked world view (menu / panel / VATS overlay / Pip-Boy)"
                              : !live                                            ? "player movement update not running"
                              : !(camState == 0 || camState == 4 || bodyView)    ? "camera state"
                              : (inputFlags & 2) == 0                            ? "looking disabled by the game"
                                                                                 : "head-tracked view, looking enabled";
            REX::INFO("comfort: turning {} ({}) camState={} inputFlags={:X} stick x={:.2f}", s.owns ? "ours" : "not ours", why, camState, inputFlags, a_f.rx);
        }
        const float x = a_f.rx;
        float       turnDeg = 0.0f;
        if (s.owns) {
            ++s.ownedFrames;
            if (c.turnMode == kSnap) {
                s.smoothStick = 0.0f;
                if (!s.armed && std::abs(x) < c.snapRearm) {
                    s.armed = true;
                }
                const bool cooled = !s.lastSnap.time_since_epoch().count() || Sec(s.lastSnap, now) * 1000.0 >= c.snapCooldownMs;
                if (s.armed && cooled && std::abs(x) > c.snapThreshold) {
                    turnDeg = x > 0.0f ? c.snapDeg : -c.snapDeg;
                    s.snapDegTotal += turnDeg;
                    s.armed = false;
                    s.lastSnap = now;
                    s.turningUntil = now + std::chrono::milliseconds(static_cast<int>(c.snapVignetteMs));
                    ++s.snaps;
                }
            } else {
                const float ax = std::abs(x);
                s.smoothStick = ax <= c.turnDeadzone ? 0.0f : std::copysign((std::min)((ax - c.turnDeadzone) / (1.0f - c.turnDeadzone), 1.0f), x);
                s.armed = true;
                if (s.smoothStick != 0.0f && frameDt > 0.0) {
                    turnDeg = static_cast<float>(c.smoothDegPerSec * s.smoothStick * frameDt);
                    s.smoothDegTotal += turnDeg;
                    s.smoothSec += frameDt;
                }
            }
            a_f.rx = 0.0f;  // the engine's own right-stick yaw must not apply on top
        } else {
            s.smoothStick = 0.0f;
            // 28 Nov: with [Controllers] bStickCarryOver=1 (default) a stick held while leaving VATS / a menu snaps at once
            // (the user primes the turn); 0 = it must come back below fSnapRearm first (before)
            s.armed = bindings::StickCarryOver() || std::abs(x) < c.snapRearm;
        }
        if (turnDeg != 0.0f) {
            rs::UserTurn(turnDeg * kD2R);
            s.turnDegTotal += turnDeg;
            s.lastTurnDeg = turnDeg;
        }

        // ---- locomotion direction ----
        s.rotated = false;
        s.handUsed = false;
        if (c.moveDir != kBody && a_f.worldView && haveCam && havePlayer && (a_f.lx != 0.0f || a_f.ly != 0.0f)) {
            float d = Wrap(camYaw - heading);  // head yaw relative to the body
            if (c.moveDir == kHand && a_f.handYawValid) {
                d = Wrap(d + a_f.handYawRel);
                s.handUsed = true;
            }
            // stick (x right, y forward) in the wanted frame -> body-local (heading convention rotation by d)
            const float cs = std::cos(d), sn = std::sin(d);
            const float ox = a_f.lx * cs + a_f.ly * sn;
            const float oy = -a_f.lx * sn + a_f.ly * cs;
            a_f.lx = std::clamp(ox, -1.0f, 1.0f);
            a_f.ly = std::clamp(oy, -1.0f, 1.0f);
            s.rotated = true;
            s.deltaDeg = d * kR2D;
        }
        s.heading = heading;
        s.camYaw = camYaw;
        s.handRelDeg = a_f.handYawValid ? a_f.handYawRel * kR2D : 0.0f;
        s.outLx = a_f.lx;
        s.outLy = a_f.ly;

        // ---- vignette target ----
        s.moving = a_f.worldView && live && std::hypot(s.inLx, s.inLy) > c.moveDeadzone;
        s.turning = s.owns && (s.smoothStick != 0.0f || now < s.turningUntil);
        s.target = c.vignette && (s.moving || s.turning);
    }

    bool Vignette(float& a_alpha, float& a_innerDeg, float& a_outerDeg)
    {
        const auto now = Clock::now();
        std::scoped_lock l(g_lock);
        const Cfg& c = g_cfg;
        St&        s = g_s;
        const double dt = s.lastFade.time_since_epoch().count() ? Sec(s.lastFade, now) : 0.0;
        s.lastFade = now;
        // the target lapses if FilterPad stops (no XR frames): fade out
        const bool target = s.target && c.vignette && s.lastPad.time_since_epoch().count() && Sec(s.lastPad, now) < 0.2;
        const float rate = c.vignetteFadeMs > 1.0f ? static_cast<float>(dt * 1000.0 / c.vignetteFadeMs) : 1.0f;
        s.alpha = std::clamp(s.alpha + (target ? rate : -rate), 0.0f, 1.0f);
        InnerOuter(c, a_innerDeg, a_outerDeg);
        a_alpha = s.alpha;
        return s.alpha > 0.0f;
    }

    void NoteFrame(bool a_vignetteSubmitted, std::uint32_t a_layerCount, std::uint64_t a_uploads)
    {
        std::scoped_lock l(g_lock);
        St& s = g_s;
        ++s.frames;
        s.lastLayers = a_layerCount;
        s.uploads = a_uploads;
        if (a_vignetteSubmitted) {
            ++s.vigFrames;
            ++(s.target ? s.vigFramesActive : s.vigFramesTail);
            s.layersWithVig = a_layerCount;
        } else {
            if (s.target) ++s.activeNoVig;
            s.layersWithoutVig = a_layerCount;
        }
    }

    std::string Command(const std::string& a_arg)
    {
        std::scoped_lock l(g_lock);
        Cfg& c = g_cfg;
        St&  s = g_s;
        if (a_arg == "turn snap" || a_arg == "turn smooth" || a_arg == "turn off" || a_arg == "turn game") {
            c.turnMode = a_arg == "turn snap" ? kSnap : a_arg == "turn smooth" ? kSmooth : kGame;
            REX::INFO("comfort: turn {} (pipe)", TurnName(c.turnMode));
            return std::format("turn {}", TurnName(c.turnMode));
        }
        if (a_arg == "move head" || a_arg == "move hand" || a_arg == "move body") {
            c.moveDir = a_arg == "move head" ? kHead : a_arg == "move hand" ? kHand : kBody;
            REX::INFO("comfort: move {} (pipe)", MoveName(c.moveDir));
            return std::format("move {}", MoveName(c.moveDir));
        }
        if (a_arg == "vignette on" || a_arg == "vignette off") {
            c.vignette = a_arg == "vignette on";
            REX::INFO("comfort: vignette {} (pipe)", c.vignette ? "on" : "off");
            return std::format("vignette {}", c.vignette ? "on" : "off");
        }
        if (a_arg == "stats reset") {
            s.snaps = s.ownedFrames = 0;
            s.turnDegTotal = s.snapDegTotal = s.smoothDegTotal = s.smoothSec = 0.0;
            s.lastTurnDeg = 0.0f;
            s.frames = s.vigFrames = s.vigFramesActive = s.vigFramesTail = s.activeNoVig = 0;
            return "stats reset";
        }
        if (a_arg.rfind("cfg", 0) == 0) {
            char  key[32]{};
            float v = 0;
            if (sscanf_s(a_arg.c_str() + 3, "%31s %f", key, static_cast<unsigned>(sizeof(key)), &v) == 2) {
                const std::string k = key;
                if (k == "snapdeg") c.snapDeg = v;
                else if (k == "snapthr") c.snapThreshold = v;
                else if (k == "snaprearm") c.snapRearm = v;
                else if (k == "cooldown") c.snapCooldownMs = v;
                else if (k == "rate") c.smoothDegPerSec = v;
                else if (k == "deadzone") c.turnDeadzone = std::clamp(v, 0.0f, 0.9f);
                else if (k == "strength") c.vignetteStrength = std::clamp(v, 0.0f, 1.0f);
                else if (k == "fade") c.vignetteFadeMs = v;
                else if (k == "movedz") c.moveDeadzone = v;
                else if (k == "lefthanded") c.leftHanded = v != 0.0f;
                else return "error: comfort cfg snapdeg|snapthr|snaprearm|cooldown|rate|deadzone|strength|fade|movedz|lefthanded <value>";
                REX::INFO("comfort: cfg {} = {} (pipe)", k, v);
            }
            return CfgString();
        }
        if (!a_arg.empty()) {
            return "error: comfort [cfg [<key> <value>]|turn snap|smooth|off|move head|hand|body|vignette on|off|stats reset]";
        }
        const auto now = Clock::now();
        return std::format(
            "installed={} turn={} move={} vignette={} owns={} live={} camState={} inputFlags={:X} armed={} smoothStick={:.3f} snaps={} "
            "turnDegTotal={:.3f} snapDegTotal={:.3f} smoothDegTotal={:.3f} smoothSec={:.4f} lastTurnDeg={:.3f} ownedFrames={} primaryUpdates={} "
            "heading={:.3f} camYaw={:.3f} deltaDeg={:.2f} handRelDeg={:.2f} handUsed={} rotated={} in=({:.3f},{:.3f}) out=({:.3f},{:.3f}) rxIn={:.3f} "
            "moving={} turning={} target={} alpha={:.3f} frames={} vigFrames={} vigActive={} vigTail={} activeNoVig={} lastLayers={} "
            "layersWithVig={} layersWithoutVig={} uploads={}",
            g_installed ? 1 : 0, TurnName(c.turnMode), MoveName(c.moveDir), c.vignette ? 1 : 0, s.owns ? 1 : 0, PathLive(s, now) ? 1 : 0,
            s.camState, s.inputFlags, s.armed ? 1 : 0, s.smoothStick, s.snaps, s.turnDegTotal, s.snapDegTotal, s.smoothDegTotal, s.smoothSec,
            s.lastTurnDeg, s.ownedFrames, s.lastUpdates, s.heading * kR2D, s.camYaw * kR2D, s.deltaDeg, s.handRelDeg, s.handUsed ? 1 : 0,
            s.rotated ? 1 : 0, s.inLx, s.inLy, s.outLx, s.outLy, s.inRx, s.moving ? 1 : 0, s.turning ? 1 : 0, s.target ? 1 : 0, s.alpha,
            s.frames, s.vigFrames, s.vigFramesActive, s.vigFramesTail, s.activeNoVig, s.lastLayers, s.layersWithVig, s.layersWithoutVig,
            s.uploads);
    }
}
