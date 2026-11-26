#include "plugin/GameBridge.h"
#include "plugin/CameraDirector.h"
#include "plugin/CameraTweaks.h"
#include "plugin/ConfigStore.h"
#include "plugin/HeadPose.h"
#include "plugin/FloatingGun.h"
#include "plugin/FrikBridge.h"
#include "plugin/ThreadQueues.h"
#include "plugin/Roomscale.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>
#include <mutex>
#include <utility>

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

namespace fo4flatvr::game
{
    bool GetRenderHandles(RenderHandles& a_out)
    {
        const auto data = RE::BSGraphics::GetRendererData();
        if (!data || !data->initialized || !data->device || !data->context) {
            return false;
        }
        const auto& window = data->renderWindow[0];
        if (!window.swapChain) {
            return false;
        }
        a_out.device = data->device;
        a_out.context = data->context;
        a_out.swapChain = window.swapChain;
        a_out.hwnd = window.hwnd;
        a_out.width = window.windowWidth;
        a_out.height = window.windowHeight;
        return true;
    }

    namespace
    {
        // Head rotation is composed in FirstPersonState::GetRotation (TESCameraState vfunc 0x0C):
        // PlayerCamera::Update asks the active state for its rotation and writes it to the camera
        // root, so modifying the returned quaternion needs no transform bookkeeping. Vtable-entry
        // patches only (data), no code bytes. PlayerCamera::Update itself is not called through
        // its vtable (measured: 0 calls), so it cannot be the hook point.
        using StateUpdate_t = void (*)(RE::TESCameraState*, RE::BSTSmartPointer<RE::TESCameraState>&);
        using StateGetRot_t = void (*)(const RE::TESCameraState*, RE::NiQuaternion&);
        using StateGetPos_t = void (*)(const RE::TESCameraState*, RE::NiPoint3&);
        StateUpdate_t g_fpUpdate = nullptr;
        StateGetRot_t g_fpGetRot = nullptr;
        StateGetPos_t g_fpGetPos = nullptr;

        std::atomic<std::uint64_t> g_fpUpdateCalls{ 0 }, g_fpGetRotCalls{ 0 }, g_fpGetPosCalls{ 0 };
        std::atomic<std::uint64_t> g_fpOwnedCalls{ 0 };  // camera step 1: GetRotation calls composed as Rz(ownYaw) x head
        bool                       g_headApplied = false;

        // Quaternion from a rotation matrix (column-vector convention), as w,x,y,z.
        void MatToQuat(const head::Mat3& a, float& w, float& x, float& y, float& z)
        {
            const auto& m = a.m;
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

        // worldFOV is the horizontal FOV of the world NiCamera (measured: frustum r = tan(worldFOV/2),
        // t = r * h/w). firstPersonFOV is set equal so arms/weapon do not float.
        float g_savedWorldFov = 0.0f, g_savedFirstPersonFov = 0.0f;
        bool  g_fovOverridden = false;
        float g_lastSetFov = 0.0f;  // worldFOV we wrote last frame
        float g_fovGain = 1.0f;     // measured frustum r / tan(worldFOV/2)
        // worldFOV written at the previous Present sample: the gain is re-measured only when the value has been the same
        // for a whole frame. Measuring every call (job-thread GetRot, director, Present) against the last written value
        // locked the loop into a 2-cycle (vremu runs 14 Nov 21:52/22:03: frustum 161.6 / 80.0 deg on alternate frames for
        // up to 2 minutes, "game rendered FOV" logged every frame) whenever the frustum seen lagged the value by a frame.
        float         g_sampleSetFov = 0.0f;
        std::atomic<bool> g_fovStableGain{ true };  // pipe "fovloop old|new" (A/B: old = re-measure on every call)
        std::uint64_t g_fovGainUpdates = 0, g_fovGainSkips = 0;
        // Camera step 0: the frustum is built from GetAdjustedWorldFOV = worldFOV (+ fovAdjustCurrent
        // + fovAnimatorAdjust while adjustFOV is set: ADS zoom). The gain must be measured against that
        // adjusted value, or the loop would fold the zoom into the gain and fight/half-cancel it. Sampled
        // at Present (a_sample), when the fields are still those the presented frame was rendered with.
        float g_lastFovAdjust = 0.0f;

        void ApplyTargetFovImpl(bool a_sample = false)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) {
                return;
            }
            if (a_sample) {
                g_lastFovAdjust = camtweak::AdjustedWorldFov() - cam->worldFOV;
            }
            const float target = head::TargetFov();
            if (target > 1.0f) {
                if (!g_fovOverridden) {
                    g_savedWorldFov = cam->worldFOV;
                    g_savedFirstPersonFov = cam->firstPersonFOV;
                    g_fovOverridden = true;
                }
                // The engine derives the frustum from worldFOV *and the window's aspect* (Hor+
                // widening on non-16:9 windows), while it renders into our forced 16:9 buffer. Close
                // the loop: k = frustum r / tan(worldFOV/2) from what the engine actually produced
                // last frame, then choose worldFOV so that r = tan(target/2).
                constexpr float d2r = 3.14159265f / 180.0f;
                float           l, r, t, b;
                const bool stable = g_fovStableGain.load(std::memory_order_relaxed);
                if ((a_sample || !stable) && g_lastSetFov > 1.0f && GetWorldFrustum(l, r, t, b)) {
                    if (!stable || std::abs(g_lastSetFov - g_sampleSetFov) < 0.01f) {
                        const float k = r / std::tan((g_lastSetFov + g_lastFovAdjust) * 0.5f * d2r);
                        if (k > 0.2f && k < 5.0f) {
                            g_fovGain = k;
                            ++g_fovGainUpdates;
                        }
                    } else {
                        ++g_fovGainSkips;  // the value changed during the last frame: the frustum may be from either
                    }
                }
                const float worldFov = 2.0f * std::atan(std::tan(target * 0.5f * d2r) / g_fovGain) / d2r;
                cam->worldFOV = worldFov;
                cam->firstPersonFOV = worldFov;
                g_lastSetFov = worldFov;
                if (a_sample) {
                    g_sampleSetFov = worldFov;
                }
            } else if (g_fovOverridden) {
                g_lastSetFov = 0.0f;
                cam->worldFOV = g_savedWorldFov;
                cam->firstPersonFOV = g_savedFirstPersonFov;
                g_fovOverridden = false;
            }
        }

        // Measured (rs probe, 14 Nov): the original GetTranslation (camera node world translate) comes back
        // next frame with the horizontal change this hook made to it (raw(k+1) = c + (returned(k) - raw(k));
        // z is not fed back). Adding the full head offset therefore showed it twice horizontally (posepos
        // 30 cm right -> camera 42 units instead of 21). Modes (rs::TranslationFeedbackMode, "rs cfg tfix"):
        //  0 = add the offset as before (2x in steady state);
        //  1 = add half of it: steady state exact, a change shows as the mean of this and the last frame;
        //  2 = subtract last frame's change, then add the full offset: exact every frame, but the change
        //      alternates between the offset and 0, so whatever the engine positions from it alternates too.
        float g_fpPrevDeltaX = 0.0f, g_fpPrevDeltaY = 0.0f, g_fpLastDeltaX = 0.0f, g_fpLastDeltaY = 0.0f;
        float g_fpLastAddX = 0.0f, g_fpLastAddY = 0.0f;  // full horizontal head offset of the last GetTranslation (readout)
        bool  g_inFpUpdate = false;
        int   g_fpGetPosInUpdate = 0, g_fpGetPosOutside = 0;  // GetTranslation calls during / outside FirstPersonState::Update

        void HkFpUpdate(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
        {
            head::WaitForPublish(2);  // pose sync: job thread vs the Present hook (HeadPose.h)
            ++g_fpUpdateCalls;
            g_fpPrevDeltaX = std::exchange(g_fpLastDeltaX, 0.0f);  // horizontal change made during the previous update
            g_fpPrevDeltaY = std::exchange(g_fpLastDeltaY, 0.0f);
            g_fpLastAddX = g_fpLastAddY = 0.0f;
            const int outside = std::exchange(g_fpGetPosOutside, 0);
            g_fpGetPosInUpdate = 0;
            g_inFpUpdate = true;
            frikbridge::StepCameraOffset();  // FRIK smooth movement (FrikBridge.h): once per update, before the camera is built
            g_fpUpdate(a_this, a_next);
            g_inFpUpdate = false;
            // roomscale camera trace ("rs camhist"): the camera root the update just wrote
            if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
                const auto& t = cam->cameraRoot->world.translate;
                rs::NoteCameraFrame(t.x, t.y, t.z, g_fpLastAddX, g_fpLastAddY, g_fpLastDeltaX, g_fpLastDeltaY, g_fpGetPosInUpdate, outside);
                if (head::TraceArmed()) {  // htrace U: camera root world after the FP update (rotate 3x3 rows + translate)
                    head::TraceRec r{ 'U' };
                    r.seq = head::Seq();
                    const float* m = reinterpret_cast<const float*>(&cam->cameraRoot->world.rotate);
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) r.v[i * 3 + j] = m[i * 4 + j];
                    r.v[9] = t.x, r.v[10] = t.y, r.v[11] = t.z;
                    head::Trace(r);
                }
            }
        }

        // Yaw of the game's own camera rotation (before the head is applied), as cos/sin, so the
        // head position offset follows the body's facing but not the gamepad pitch.
        float g_baseYawCos = 1.0f, g_baseYawSin = 0.0f;
        float g_unitsPerMeter = 70.0f;
        float g_heightOffsetCm = 0.0f;
        bool  g_headAuthority = true;
        float g_eyeTauS = 0.6f;
        float g_eyeH = -1.0f;  // low-passed (game camera node z - capsule z), units; < 0 = not sampled yet
        std::chrono::steady_clock::time_point g_eyeT{};
        std::atomic<bool> g_rigFitRaw{ true };   // SetRigFitRaw
        std::atomic<std::uint64_t> g_rigFitCalls{ 0 };
        constexpr std::uintptr_t kRigFitRet = 0xD7C9CD;  // return address of UpdateActor3DPosition's Get1stPersonCameraPosition call

        // Sneak height step (20 Nov, user: "Can the sneak toggle adjust to match fo4vr. i.e. the same abrupt, not gradual height
        // change"). The eye height below follows the game's animated camera node, which blends down/up over the sneak
        // transition, and the 0.6 s low-pass slowed it further. FO4VR changes the view height in one step when the sneak flag
        // flips (Ghidra, Fallout4VR 1.2.72 FUN_140ef7180 = the HMD-node update: HMD local z = tracked height x
        // fComfortSneakHeight (0.7) while the sneak stance bits are set, no smoothing). Here: on the frame the sneak bits
        // flips, the eye height jumps to the other stance's level (the learned sneak/stand ratio of the last settled levels,
        // or kSneakRatioDefault before one is learned) and is held there while the game's camera node is still blending
        // (until the raw node arrives near it or stops moving, at most kSneakHoldS); then the usual low-pass takes the
        // residual. Only the eye height: ground height (stairs, slopes, FRIK smooth movement) is not part of it.
        // [Roomscale] bSneakHeightStep=0 = the gradual change as before.
        bool  g_sneakStep = true;
        int   g_stance = -1;                     // 0 standing, 1 sneaking, -1 unknown
        float g_stanceLevel[2]{ -1.0f, -1.0f };  // settled low-passed eye height per stance (units), < 0 = not learned
        float g_sneakRatio = -1.0f;              // learned sneak / stand eye height ratio, < 0 = none
        std::chrono::steady_clock::time_point g_stanceSince{}, g_holdUntil{}, g_holdStart{}, g_rawMovedT{};
        float    g_rawPrev = -1.0f;
        unsigned g_sneakSteps = 0, g_sneakHoldsEarly = 0;
        float    g_lastStepFrom = 0.0f, g_lastStepTo = 0.0f;
        // flat 1.11.240 first-person camera node: 120.5 u standing, 80.5 u sneaking (20 Nov, no-XR, TestProfileVR latest save) =
        // 0.668; FO4VR comfort sneak uses 0.7 x the HMD height. Used until a ratio is learned from settled levels this session.
        float    g_sneakRatioDefault = 0.668f;
        constexpr float kSneakHoldS = 1.2f;
        // Plausible sneak / stand eye-height ratios (flat 0.668, FO4VR 0.7). 20 Nov (VATS agent): the user log 23:25 learned 0.949,
        // then 1.000, from a frozen first-person camera node (after VATS the player's animation manager kept the third-person
        // graph active in first person - FRIK-flat PovSwitchSync - so graph 1 no longer posed the "Camera" bone and it stayed at
        // the sneak height 80.5 u); every later step was then 80.5 -> 80.5 u. Levels / ratios outside this band are not learned.
        constexpr float kSneakRatioMin = 0.5f, kSneakRatioMax = 0.85f;
        // Frozen-node guard (same report: "stuck in crouch height ... right stick down to switch back to standing ... camera
        // height stays stuck low"): stance standing for kSneakHoldS, both levels learned and apart, and the game's node still
        // sitting on the learned sneak level -> the eye height stays at the learned standing level until the node leaves it.
        bool     g_frozenGuard = false;
        unsigned g_frozenGuards = 0;

        // The sneak stance bits FO4VR's HMD-node update tests (VR FUN_140ef7180: (actor+0x134 >> 12 & 7) == 1); on flat
        // 1.11.240 the same field and Actor::IsSneaking's first test (666050: (actor+0x134 & 0x3800) == 0x800). Read
        // directly: lock-free (IsSneaking also takes the extra-list read lock) on the camera job thread.
        bool PlayerSneaking()
        {
            const auto* pc = RE::PlayerCharacter::GetSingleton();
            return pc && (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(pc) + 0x134) & 0x3800u) == 0x800u;
        }

        // a_eh: the game's eye height this frame; returns true while the step holds g_eyeH (no low-pass this frame)
        bool SneakStep(float a_eh, std::chrono::steady_clock::time_point a_now)
        {
            using namespace std::chrono;
            if (!g_sneakStep || g_eyeH < 0.0f) {
                g_stance = -1;
                g_holdUntil = {};
                return false;
            }
            if (g_rawPrev < 0.0f || std::abs(a_eh - g_rawPrev) > 0.15f) g_rawMovedT = a_now;
            g_rawPrev = a_eh;
            const int st = PlayerSneaking() ? 1 : 0;
            if (g_stance < 0) {
                g_stance = st, g_stanceSince = a_now;
            } else if (st != g_stance) {
                const float from = g_eyeH;
                const float r = g_sneakRatio >= kSneakRatioMin && g_sneakRatio <= kSneakRatioMax ? g_sneakRatio : g_sneakRatioDefault;
                g_eyeH = st == 1 ? from * r : from / r;
                g_lastStepFrom = from, g_lastStepTo = g_eyeH;
                g_stance = st, g_stanceSince = a_now;
                g_holdStart = a_now;
                g_holdUntil = a_now + duration_cast<steady_clock::duration>(duration<float>(kSneakHoldS));
                if (++g_sneakSteps <= 20) {
                    REX::INFO("camera: sneak {} -> eye height step {:.1f} -> {:.1f} u (ratio {:.3f}{}); game camera node now {:.1f}", st ? "on" : "off", from,
                        g_eyeH, r, g_sneakRatio > 0.0f ? " learned" : " default", a_eh);
                }
                return true;
            }
            if (a_now < g_holdUntil) {
                // the game's node is still blending: hold, unless it has arrived near the step or stopped moving
                const bool arrived = std::abs(a_eh - g_eyeH) < 1.5f;
                // "stopped moving" only once it has moved since the step: a node that never left the old level is not settled
                const bool settled = a_now - g_holdStart > milliseconds(150) && g_rawMovedT > g_holdStart && a_now - g_rawMovedT > milliseconds(150);
                if (!arrived && !settled) return true;
                ++g_sneakHoldsEarly;
                g_holdUntil = {};
            }
            // frozen-node guard (see g_frozenGuard): standing, but the game's camera node still on the sneak level
            const float stand = g_stanceLevel[0], sneak = g_stanceLevel[1];
            const bool  frozen = st == 0 && stand > 0.0f && sneak > 0.0f && stand - sneak > 15.0f && std::abs(a_eh - sneak) < 2.0f &&
                                a_now - g_stanceSince > duration_cast<steady_clock::duration>(duration<float>(kSneakHoldS));
            if (frozen != g_frozenGuard) {
                g_frozenGuard = frozen;
                if (frozen) ++g_frozenGuards;
                if (g_frozenGuards <= 20) {
                    if (frozen) {
                        REX::INFO("camera: standing, but the game's camera node stays at the sneak level {:.1f} u -> eye height held at the "
                                  "learned standing level {:.1f} u (#{})", a_eh, stand, g_frozenGuards);
                    } else {
                        REX::INFO("camera: the game's camera node left the sneak level ({:.1f} u) - eye height follows it again ({:.1f} u, #{})",
                            a_eh, g_eyeH, g_frozenGuards);
                    }
                }
            }
            if (frozen) {
                g_eyeH = stand;
                return true;  // no low-pass toward the frozen node, nothing learned from it
            }
            // learn the settled level of this stance (low-passed, so walking bob is averaged), and the ratio from both; a level
            // that would give an implausible ratio against the other stance's is not taken (frozen node, see kSneakRatioMin)
            if (a_now - g_stanceSince > milliseconds(1500)) {
                const float other = g_stanceLevel[1 - st];
                const float r = other > 0.0f ? (st == 1 ? g_eyeH / other : other / g_eyeH) : -1.0f;
                if (other <= 0.0f || (r >= kSneakRatioMin && r <= kSneakRatioMax)) {
                    g_stanceLevel[st] = g_eyeH;
                    if (r > 0.0f) {
                        if (g_sneakRatio < 0.0f || std::abs(r - g_sneakRatio) > 0.01f) {
                            REX::INFO("camera: sneak eye height ratio learned {:.3f} (stand {:.1f} u, sneak {:.1f} u)", r, g_stanceLevel[0], g_stanceLevel[1]);
                        }
                        g_sneakRatio = r;
                    }
                }
            }
            return false;
        }

        // Head authority: camera = capsule + low-passed eye height + head offset (roomscale residual), ignoring the
        // game's animated camera node (whose horizontal offset from the capsule swings with body yaw, and whose
        // height carries bob / idle sway / turn animations). The raw node is only used for the eye height.
        // GetTranslation is RELATIVE TO THE PLAYER (camera root = data.location + returned value, rs camhist), so
        // the result is the offset only: horizontal = head offset, vertical = low-passed raw z + head height.
        bool HeadAuthorityPos(const RE::NiPoint3& a_raw, float a_addX, float a_addY, float a_addZ, RE::NiPoint3& a_out)
        {
            if (!g_headAuthority) {
                return false;
            }
            const float eh = a_raw.z;
            const auto  now = std::chrono::steady_clock::now();
            float       dt = std::chrono::duration<float>(now - g_eyeT).count();
            g_eyeT = now;
            if (g_eyeH < 0.0f || dt > 1.0f || std::abs(eh - g_eyeH) > 80.0f) {
                g_eyeH = eh;  // first sample, after a gap, or a teleport-sized jump
                g_stance = -1;
                g_holdUntil = {};
            } else if (SneakStep(eh, now)) {
                // sneak toggled: stepped / held (no low-pass while the game's camera node is still blending)
            } else if (g_eyeTauS > 0.0f) {
                g_eyeH += (eh - g_eyeH) * (1.0f - std::exp(-dt / g_eyeTauS));
            } else {
                g_eyeH = eh;
            }
            a_out.x = a_addX;
            a_out.y = a_addY;
            a_out.z = g_eyeH + a_addZ;
            return true;
        }

        void HkFpGetPos(const RE::TESCameraState* a_this, RE::NiPoint3& a_out)
        {
            head::WaitForPublish(4);
            ++g_fpGetPosCalls;
            ++(g_inFpUpdate ? g_fpGetPosInUpdate : g_fpGetPosOutside);
            g_fpGetPos(a_this, a_out);
            struct TraceOut  // htrace T: head publish used, raw node offset, result
            {
                head::TraceRec r{ 'T' };
                RE::NiPoint3&  p;
                TraceOut(RE::NiPoint3& a_p, void* a_ret) : p(a_p)
                {
                    r.seq = head::Seq();  // approximation: GetOffset below reads the same g_seq unless a publish lands between
                    const auto rva = reinterpret_cast<std::uintptr_t>(a_ret) - REL::Offset(0).address();  // caller
                    r.v[11] = static_cast<float>(rva & 0xFFFFFF), r.v[12] = static_cast<float>(rva >> 24);
                    r.v[3] = p.x, r.v[4] = p.y, r.v[5] = p.z;
                    r.v[6] = g_inFpUpdate ? 1.0f : 0.0f;
                }
                ~TraceOut()
                {
                    if (head::TraceArmed()) {
                        r.v[0] = p.x, r.v[1] = p.y, r.v[2] = p.z;
                        head::Trace(r);
                    }
                }
            } tr(a_out, _ReturnAddress());
            if (head::Vec3 ho; g_headAuthority && head::GetOffset(ho)) {
                const RE::NiPoint3 raw = a_out;
                rs::NoteCameraBase(raw.x, raw.y, raw.z, 0.0f, 0.0f);
                rs::AdjustHeadOffset(ho, std::atan2(-g_baseYawSin, g_baseYawCos));  // roomscale: residual only
                const float x = ho.x * g_unitsPerMeter, y = ho.y * g_unitsPerMeter;
                const float addX = g_baseYawCos * x - g_baseYawSin * y;
                const float addY = g_baseYawSin * x + g_baseYawCos * y;
                const float addZ = ho.z * g_unitsPerMeter + g_heightOffsetCm * 0.01f * g_unitsPerMeter;
                tr.r.v[7] = addX, tr.r.v[8] = addY, tr.r.v[9] = addZ, tr.r.v[10] = 1.0f;  // htrace: head-authority offset
                if (HeadAuthorityPos(raw, addX, addY, addZ, a_out)) {
                    if (float sx, sy, sz; frikbridge::CameraOffset(sx, sy, sz)) {  // FRIK smooth movement = FO4VR PlayerWorldNode offset
                        a_out.x += sx, a_out.y += sy, a_out.z += sz;
                    }
                    g_fpLastDeltaX = g_fpLastDeltaY = 0.0f;  // the raw node is not fed back into the result
                    g_fpLastAddX = addX;
                    g_fpLastAddY = addY;
                    // rig fit: the vanilla value keeps the first-person root at rest (no two-position alternation)
                    if (g_rigFitRaw.load(std::memory_order_relaxed) &&
                        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == REL::Offset(kRigFitRet).address()) {
                        ++g_rigFitCalls;
                        a_out = raw;
                    }
                    return;
                }
                a_out = raw;
            }
            const int mode = rs::TranslationFeedbackMode();
            rs::NoteCameraBase(a_out.x, a_out.y, a_out.z, g_fpPrevDeltaX, g_fpPrevDeltaY);
            float addX = 0.0f, addY = 0.0f;
            head::Vec3 o;
            const bool have = head::GetOffset(o);
            if (have) {
                rs::AdjustHeadOffset(o, std::atan2(-g_baseYawSin, g_baseYawCos));  // roomscale: residual only
                a_out.z += g_heightOffsetCm * 0.01f * g_unitsPerMeter;
                const float x = o.x * g_unitsPerMeter, y = o.y * g_unitsPerMeter;
                addX = g_baseYawCos * x - g_baseYawSin * y;
                addY = g_baseYawSin * x + g_baseYawCos * y;
                a_out.z += o.z * g_unitsPerMeter;
            }
            float dX = addX, dY = addY;
            if (mode == 1) {
                dX = 0.5f * addX;
                dY = 0.5f * addY;
            } else if (mode == 2) {
                dX = addX - g_fpPrevDeltaX;
                dY = addY - g_fpPrevDeltaY;
            }
            a_out.x += dX;
            a_out.y += dY;
            if (float sx, sy, sz; frikbridge::CameraOffset(sx, sy, sz)) {  // FRIK smooth movement
                a_out.x += sx, a_out.y += sy, a_out.z += sz;
            }
            tr.r.v[7] = addX, tr.r.v[8] = addY, tr.r.v[9] = have ? o.z * g_unitsPerMeter : 0.0f, tr.r.v[10] = 2.0f;  // htrace
            g_fpLastDeltaX = dX;
            g_fpLastDeltaY = dY;
            g_fpLastAddX = addX;
            g_fpLastAddY = addY;
        }

        void HkFpGetRot(const RE::TESCameraState* a_this, RE::NiQuaternion& a_out)
        {
            head::WaitForPublish(3);
            ++g_fpGetRotCalls;
            g_fpGetRot(a_this, a_out);
            ApplyTargetFovImpl();
            // htrace R: which head publish this rotation was built from, the game's own rotation and the result
            struct TraceOut
            {
                head::TraceRec r{ 'R' };
                RE::NiQuaternion& q;
                explicit TraceOut(RE::NiQuaternion& a_q) : q(a_q)
                {
                    r.v[4] = q.w, r.v[5] = q.x, r.v[6] = q.y, r.v[7] = q.z;
                }
                ~TraceOut()
                {
                    if (head::TraceArmed()) {
                        r.v[0] = q.w, r.v[1] = q.x, r.v[2] = q.y, r.v[3] = q.z;
                        head::Trace(r);
                    }
                }
            } tr(a_out);
            // Camera step 1 (owned heading): view = Rz(ownYaw) x head. The game's rotation (actor yaw from any
            // source, pitch/looking) is dropped, so setangle / VATS aim / furniture turns never move the view.
            if (float own = 0.0f; head::Tracking() && rs::OwnedYaw(own)) {
                head::Mat3 h;
                tr.r.v[8] = own;
                tr.r.v[9] = 1.0f;
                if (head::Get(h, tr.r.seq)) {
                    g_baseYawCos = std::cos(own);
                    g_baseYawSin = -std::sin(own);  // same convention as below: body +Y -> (sin t, cos t)
                    const float yw = std::cos(-own * 0.5f), yz = std::sin(-own * 0.5f);  // heading t = rotation about +Z by -t
                    float hw, hx, hy, hz;
                    MatToQuat(h, hw, hx, hy, hz);
                    a_out.w = yw * hw - yz * hz;
                    a_out.x = yw * hx - yz * hy;
                    a_out.y = yw * hy + yz * hx;
                    a_out.z = yw * hz + yz * hw;
                    g_headApplied = true;
                    ++g_fpOwnedCalls;
                    return;
                }
            }
            {
                // base forward = R(q) * (0,1,0); keep only its horizontal direction
                const RE::NiQuaternion& q = a_out;
                const float fx = 2.0f * (q.x * q.y - q.w * q.z);
                const float fy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
                const float len = std::sqrt(fx * fx + fy * fy);
                if (len > 1e-4f) {
                    // rotation taking body +Y to (fx,fy): cos = fy/len, sin = -fx/len
                    g_baseYawCos = fy / len;
                    g_baseYawSin = -fx / len;
                }
            }
            head::Mat3 h;
            if (!head::Get(h, tr.r.seq)) {
                g_headApplied = false;
                return;
            }
            rs::AdjustHeadRotation(h, std::atan2(-g_baseYawSin, g_baseYawCos));  // body yaw: head relative to body
            float hw, hx, hy, hz;
            MatToQuat(h, hw, hx, hy, hz);
            const RE::NiQuaternion q = a_out;
            const bool             pre = head::Convention() == 1;  // debug: h*q instead of q*h
            const float aw = pre ? hw : q.w, ax = pre ? hx : q.x, ay = pre ? hy : q.y, az = pre ? hz : q.z;
            const float bw = pre ? q.w : hw, bx = pre ? q.x : hx, by = pre ? q.y : hy, bz = pre ? q.z : hz;
            a_out.w = aw * bw - ax * bx - ay * by - az * bz;
            a_out.x = aw * bx + ax * bw + ay * bz - az * by;
            a_out.y = aw * by - ax * bz + ay * bw + az * bx;
            a_out.z = aw * bz + ax * by - ay * bx + az * bw;
            g_headApplied = true;
        }
    }

    void SetHeightOffsetCm(float a_cm)
    {
        g_heightOffsetCm = a_cm;
    }

    void FirstPersonEye(float& a_eyeHeightUnits, float& a_heightOffsetUnits)
    {
        a_eyeHeightUnits = g_headAuthority ? g_eyeH : -1.0f;
        a_heightOffsetUnits = g_heightOffsetCm * 0.01f * g_unitsPerMeter;
    }

    void SetHeadAuthority(bool a_on, float a_tauS)
    {
        g_headAuthority = a_on;
        g_eyeTauS = a_tauS;
        g_eyeH = -1.0f;
        REX::INFO("camera: head authority {} (eye height smoothing {:.2f} s)", a_on ? "on" : "off", a_tauS);
    }

    bool HeadAuthority()
    {
        return g_headAuthority;
    }

    void SetSneakHeightStep(bool a_on)
    {
        g_sneakStep = a_on;
        REX::INFO("camera: sneak eye height {} ([Roomscale] bSneakHeightStep)", a_on ? "steps at once on a sneak toggle (FO4VR)" : "follows the game's gradual change");
    }

    std::string SneakStepCommand(const std::string& a_arg)
    {
        if (a_arg == "on" || a_arg == "off") SetSneakHeightStep(a_arg == "on");
        return std::format("sneakstep on={} headAuthority={} sneaking={} stance={} eyeH={:.1f} raw={:.1f} levels stand={:.1f} sneak={:.1f} ratio={:.3f} (default {:.3f}) "
                           "steps={} holdsEndedEarly={} last {:.1f}->{:.1f} tau={:.2f}s frozenGuard={} frozenGuards={}",
            g_sneakStep ? 1 : 0, g_headAuthority ? 1 : 0, PlayerSneaking() ? 1 : 0, g_stance, g_eyeH, g_rawPrev, g_stanceLevel[0], g_stanceLevel[1], g_sneakRatio,
            g_sneakRatioDefault, g_sneakSteps, g_sneakHoldsEarly, g_lastStepFrom, g_lastStepTo, g_eyeTauS, g_frozenGuard ? 1 : 0, g_frozenGuards);
    }

    void SetRigFitRaw(bool a_on)
    {
        g_rigFitRaw = a_on;
    }

    bool RigFitRaw()
    {
        return g_rigFitRaw.load();
    }

    float HeadAuthorityTau()
    {
        return g_eyeTauS;
    }

    void SetUnitsPerMeter(float a_units)
    {
        g_unitsPerMeter = a_units;
    }

    bool InstallCameraHook()
    {
        static bool installed = false;
        if (installed) {
            return true;
        }
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::FirstPersonState[0] };
        {
            // Address-manifest check: vtable 25AAB08, slots 0B/0C/0D must still hold the original
            // FirstPersonState Update / GetRotation / GetTranslation (10204F0 / 1020FA0 / 1021050).
            const std::uintptr_t base = REL::Offset(0).address();
            const auto*          slots = reinterpret_cast<const std::uintptr_t*>(vtbl.address());
            constexpr std::uintptr_t kExpect[3] = { 0x10204F0, 0x1020FA0, 0x1021050 };
            bool ok = vtbl.address() - base == 0x25AAB08;
            for (int i = 0; i < 3; ++i) {
                ok = ok && slots[0x0B + i] == base + kExpect[i];
            }
            if (!ok) {
                REX::ERROR("camera hook: FirstPersonState vtable {:X} slots 0B/0C/0D = {:X}/{:X}/{:X} (expected vtable 25AAB08, "
                           "10204F0/1020FA0/1021050); NOT patching (no head tracking in first person)",
                    vtbl.address() - base, slots[0x0B] - base, slots[0x0C] - base, slots[0x0D] - base);
                return false;
            }
        }
        g_fpUpdate = reinterpret_cast<StateUpdate_t>(vtbl.write_vfunc(0x0B, &HkFpUpdate));
        g_fpGetRot = reinterpret_cast<StateGetRot_t>(vtbl.write_vfunc(0x0C, &HkFpGetRot));
        g_fpGetPos = reinterpret_cast<StateGetPos_t>(vtbl.write_vfunc(0x0D, &HkFpGetPos));
        installed = true;
        REX::INFO("camera hook: FirstPersonState vtable {:X} slots 0B/0C/0D (original targets verified)", vtbl.address());
        return true;
    }

    void ApplyTargetFov()
    {
        ApplyTargetFovImpl(true);
    }

    void SetFovLoopStable(bool a_on)
    {
        g_fovStableGain.store(a_on);
        REX::INFO("fov loop: {}", a_on ? "stable-frame gain (new)" : "gain re-measured every call (old)");
    }

    void ApplyTargetFovNoSample()
    {
        ApplyTargetFovImpl(false);
    }

    bool GetWorldFrustum(float& a_left, float& a_right, float& a_top, float& a_bottom)
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* root = cam ? cam->cameraRoot.get() : nullptr;
        if (!root) {
            return false;
        }
        for (auto& c : root->children) {
            if (auto* nc = ::netimmerse_cast<RE::NiCamera*>(c.get())) {
                const auto& f = nc->viewFrustum;
                if (f.ortho || !(f.right > 0.0f) || !(f.top > 0.0f)) {
                    return false;
                }
                a_left = f.left;
                a_right = f.right;
                a_top = f.top;
                a_bottom = f.bottom;
                return true;
            }
        }
        return false;
    }

    void TraceWorldCamera()
    {
        if (!head::TraceArmed()) {
            return;
        }
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* root = cam ? cam->cameraRoot.get() : nullptr;
        if (!root) {
            return;
        }
        for (auto& c : root->children) {
            if (auto* nc = ::netimmerse_cast<RE::NiCamera*>(c.get())) {
                // C: camera root world (same layout as U, matched exactly offline) + NiCamera world translate
                head::TraceRec r{ 'C' };
                r.seq = head::Seq();
                const float* m = reinterpret_cast<const float*>(&root->world.rotate);
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) r.v[i * 3 + j] = m[i * 4 + j];
                r.v[9] = root->world.translate.x, r.v[10] = root->world.translate.y, r.v[11] = root->world.translate.z;
                r.v[12] = nc->world.translate.x, r.v[13] = nc->world.translate.y, r.v[14] = nc->world.translate.z;
                r.v[15] = static_cast<float>(rs::PrimaryUpdates() & 0xFFFFFF);
                head::Trace(r);
                return;
            }
        }
    }

    const char* ViewTakingMenu(const char* a_ignore, bool (*a_skip)(const char*))
    {
        static constexpr const char* kMenus[] = { "PipboyMenu", "PipboyHolotapeMenu", "PauseMenu", "MainMenu",
            "LoadingMenu", "LooksMenu", "ContainerMenu", "BarterMenu", "ExamineMenu", "WorkshopMenu", "TerminalMenu",
            "TerminalHolotapeMenu", "LockpickingMenu", "BookMenu", "Console", "LevelUpMenu", "SPECIALMenu",
            "FavoritesMenu", "CookingMenu", "PowerArmorModMenu", "CreditsMenu", "VATSMenu",
            "SleepWaitMenu", "MessageBoxMenu",
            // Camera step 1: "SitWaitMenu" removed - it is only the "Wait" prompt open for as long as the player sits
            // (measured: opens when the furniture-enter camera returns to FirstPerson); the wait dialog itself is
            // SleepWaitMenu. Sitting now stays in the head-tracked world view.
            // Camera step 0: scoped ADS (ScopeMenu, sent by SetInIronSightsImpl D7F2E0) keeps the flat-screen
            // fallback with the game's zoom. DialogueMenu is deliberately absent: with bFirstPersonDialogue the
            // dialogue stays in FirstPersonState and head-tracked.
            "ScopeMenu" };
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return nullptr;
        }
        // perf pass 26 Nov: the names are interned once (was a BSFixedString built per menu per call, twice a frame)
        static const std::vector<RE::BSFixedString> kNames = [] {
            std::vector<RE::BSFixedString> v;
            for (const char* m : kMenus) {
                v.emplace_back(m);
            }
            return v;
        }();
        std::size_t i = 0;
        for (const char* m : kMenus) {
            const auto& name = kNames[i++];
            if ((a_ignore && std::strcmp(m, a_ignore) == 0) || (a_skip && a_skip(m))) {
                continue;
            }
            // Camera step 2: with the VATS camera gated, VATS targeting stays in the head-tracked world view
            if (std::strcmp(m, "VATSMenu") == 0 && camdir::VatsWorldView()) {
                continue;
            }
            if (ui->GetMenuOpen(name)) {
                return m;
            }
        }
        return nullptr;
    }

    std::uint64_t FirstPersonUpdates()
    {
        return g_fpUpdateCalls.load();
    }

    int CurrentCameraState()
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->currentState) {
            return -1;
        }
        return static_cast<int>(cam->currentState->id.get());
    }

    namespace
    {
        int  g_origW = -1, g_origH = -1;
        bool g_sizeOverridden = false;
    }

    void OverrideDisplaySize(int a_w, int a_h)
    {
        if (a_w <= 0 || a_h <= 0) {
            return;
        }
        auto* w = RE::GetINISetting("iSize W:Display");
        auto* h = RE::GetINISetting("iSize H:Display");
        if (!w || !h) {
            REX::WARN("display size override: iSize settings not found (INI not loaded yet?)");
            return;
        }
        g_origW = w->GetInt();
        g_origH = h->GetInt();
        w->SetInt(a_w);
        h->SetInt(a_h);
        g_sizeOverridden = true;
        REX::INFO("display size override: iSize {}x{} -> {}x{} (in memory, restored after renderer init)", g_origW,
            g_origH, a_w, a_h);
    }

    void RestoreDisplaySize()
    {
        if (!g_sizeOverridden) {
            return;
        }
        if (auto* w = RE::GetINISetting("iSize W:Display")) {
            w->SetInt(g_origW);
        }
        if (auto* h = RE::GetINISetting("iSize H:Display")) {
            h->SetInt(g_origH);
        }
        g_sizeOverridden = false;
        REX::INFO("display size override: restored iSize {}x{}", g_origW, g_origH);
    }

    std::string RenderTargetReport()
    {
        // renderTargetData[id] = properties; the D3D texture lives at RendererData::renderTargets[handle],
        // handle = renderTargetID[id] (u32 at RenderTargetManager+0xDF4 in 1.11.240, measured from
        // DestroyRenderTargets; -1 = pooled).
        std::string s;
        auto*       rtm = RE::BSGraphics::RenderTargetManager::GetSingleton();
        auto*       data = RE::BSGraphics::GetRendererData();
        auto texSize = [&](std::uint32_t a_handle) -> std::string {
            if (!data || a_handle >= 101) {
                return "-";
            }
            auto* tex = data->renderTargets[a_handle].texture;
            if (!tex) {
                return "null";
            }
            REX::W32::D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            return std::format("{}x{}", d.width, d.height);
        };
        if (rtm) {
            const auto* ids = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(rtm) + 0xDF4);
            for (int id : { 0, 1, 2, 4, 0x14, 0x16, 0x3A }) {
                s += std::format(" id{:X}={}x{}/tex {}", id, rtm->renderTargetData[id].width, rtm->renderTargetData[id].height,
                    texSize(ids[id]));
            }
        }
        if (data && data->renderWindow[0].swapChainRenderTarget.texture) {
            REX::W32::D3D11_TEXTURE2D_DESC d{};
            data->renderWindow[0].swapChainRenderTarget.texture->GetDesc(&d);
            s += std::format(" | backbuffer {}x{}", d.width, d.height);
        }
        return s.empty() ? "renderer not ready" : s;
    }

    std::string ReadyReport()
    {
        auto*       ui = RE::UI::GetSingleton();
        const bool  loading = ui && ui->GetMenuOpen("LoadingMenu");
        const bool  mainMenu = ui && ui->GetMenuOpen("MainMenu");
        auto*       player = RE::PlayerCharacter::GetSingleton();
        auto*       cell = player ? player->GetParentCell() : nullptr;
        const char* cellName = cell ? cell->GetFormEditorID() : nullptr;
        const bool  inGame = cell && !loading && !mainMenu;
        return std::format("ingame={} loading={} mainmenu={} cell={} camstate={}", inGame ? 1 : 0, loading ? 1 : 0,
            mainMenu ? 1 : 0, cellName && *cellName ? cellName : (cell ? "?" : "-"), CurrentCameraState());
    }

    std::string CameraReport()
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) {
            return "no PlayerCamera";
        }
        auto fmtRot = [](const RE::NiMatrix3& m) {
            const auto* f = reinterpret_cast<const float*>(&m);
            return std::format("[{:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f}]",
                f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10]);
        };
        std::string s = std::format("fp update={} getRot={} getPos={} owned={} applied={} state={} ", g_fpUpdateCalls.load(), g_fpGetRotCalls.load(), g_fpGetPosCalls.load(), g_fpOwnedCalls.load(), g_headApplied,
            cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1);
        if (auto* root = cam->cameraRoot.get()) {
            s += std::format("root '{}' localRot={} worldRot={} worldPos=({:.1f},{:.1f},{:.1f}) children={}",
                root->name.c_str(), fmtRot(root->local.rotate), fmtRot(root->world.rotate),
                root->world.translate.x, root->world.translate.y, root->world.translate.z, root->children.size());
            for (auto& c : root->children) {
                if (c) {
                    s += std::format(" child '{}' rtti={} worldRot={}", c->name.c_str(),
                        c->GetRTTI() ? c->GetRTTI()->GetName() : "?", fmtRot(c->world.rotate));
                    if (auto* nc = ::netimmerse_cast<RE::NiCamera*>(c.get())) {
                        const auto& f = nc->viewFrustum;
                        s += std::format(" frustum l={:.4f} r={:.4f} t={:.4f} b={:.4f} n={:.2f} f={:.0f} ortho={}"
                                         " port=({:.2f},{:.2f},{:.2f},{:.2f})",
                            f.left, f.right, f.top, f.bottom, f.near, f.far, f.ortho,
                            nc->port.left, nc->port.right, nc->port.top, nc->port.bottom);
                    }
                }
            }
            s += std::format(" worldFOV={:.2f} firstPersonFOV={:.2f}", cam->worldFOV, cam->firstPersonFOV);
        } else {
            s += "no cameraRoot";
        }
        return s;
    }

    bool QueueConsoleCommand(std::string a_command)
    {
        // main-idle queue (ThreadQueues.h): right before the engine runs its own queued console commands, same thread / point
        return tq::RunOnMain([cmd = std::move(a_command)]() {
            REX::INFO("console: {}", cmd);
            RE::Console::ExecuteCommand(cmd.c_str());
        });
    }

    // ==== Controller aim ==========================================================================
    namespace
    {
        // 1.11.240 RVAs (analysis/vr-aim-and-activate.md, db/labels/aim_research.csv).
        constexpr std::uintptr_t kSetOriginAndDirection = 0xE42EA0;
        constexpr std::uintptr_t kPickCallSite = 0xD70D0D;      // call ViewCaster::UpdatePickRef
        constexpr std::uintptr_t kViewCasterUpdatePick = 0x994B80;
        constexpr std::uintptr_t kLaunchCallSite = 0x47A096;    // call Projectile::Launch in TESObjectWEAP::Fire
        constexpr std::uintptr_t kProjectileLaunch = 0xE421E0;
        constexpr std::uintptr_t kGetViewCasterData = 0x994460;
        constexpr std::uintptr_t kPlayerViewCaster = 0x30F9FF8; // ViewCaster* global (id 4797738)
        constexpr std::uintptr_t kVatsSingleton = 0x30F9FD0;    // VATS* (id 4797733), mode at +0x40

        constexpr float kMaxHandDistM = 1.0f;     // P further than this from the camera: ignore the ray
        constexpr long long kFreshMs = 200;       // ray older than this (no XR frames): ignore

        struct V3 { float x{}, y{}, z{}; };
        V3    Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        float Len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
        V3    Norm(const V3& a)
        {
            const float l = Len(a);
            return l > 1e-6f ? V3{ a.x / l, a.y / l, a.z / l } : V3{};
        }
        float HeadingDeg(const V3& d) { return std::atan2(d.x, d.y) * 57.2957795f; }  // engine zAngle convention, 0 = +Y
        float PitchUpDeg(const V3& d)
        {
            const float l = Len(d);
            return l > 1e-6f ? std::asin(std::clamp(d.z / l, -1.0f, 1.0f)) * 57.2957795f : 0.0f;
        }
        std::string F3(const V3& v, int prec = 1)
        {
            return prec == 1 ? std::format("({:.1f},{:.1f},{:.1f})", v.x, v.y, v.z)
                             : std::format("({:.4f},{:.4f},{:.4f})", v.x, v.y, v.z);
        }

        struct HandRay
        {
            bool   valid{ false };
            V3     P, D;              // world
            V3     camP, camF;        // camera root world position / forward (same convention)
            V3     localM, localDir;  // input, camera-local (metres)
            float  distM{ 0.0f };
            int    camState{ -1 };
            const char* why{ "none yet" };
            V3     rawP;                // P before the camera -> P wall clamp
            float  clampFrac{ 1.0f };   // hit fraction of camera -> rawP (1 = clear)
            bool   clamped{ false };
            std::chrono::steady_clock::time_point t;
        };

        struct PickInfo
        {
            V3            engineO, engineD;  // what the engine passed (camera ray)
            V3            usedO, usedD;      // what the ViewCaster got
            bool          redirected{ false };
            std::uint32_t handle{ 0 };
            std::uint32_t formID{ 0 };
            std::string   name;
            std::uint64_t calls{ 0 }, redirects{ 0 };
        };

        struct FireInfo
        {
            std::uint64_t calls{ 0 }, player{ 0 }, overridden{ 0 };
            const char*   lastSkip{ "-" };
            V3            origO, newO;
            float         origZ{}, origX{}, newZ{}, newX{};
            // projectile from the last player Launch
            std::uint64_t launches{ 0 };
            std::uint32_t projHandle{ 0 };
            V3            launchO;
            float         launchZ{}, launchX{};
            bool          launchUseOrigin{ false };
            bool          projSeen{ false };
            int           projFrames{ 0 };
            V3            projFirst, projLast;
            V3            projAngle, projVel, projMoveDir, projImpact;  // last seen while alive
            int           projImpacts{ 0 };
        };

        std::mutex g_aimLock;
        HandRay    g_ray;
        // per-hand activation (FO4VR: A picks with the right controller's ray, X with the left's): the ray the pick uses
        HandRay                    g_rayOff;               // off hand, computed only while it is requested
        std::atomic<int>           g_pickHandReq{ 0 };     // 0 primary, 1 off hand (SetPickHand, ControllerBindings)
        std::atomic<std::uint64_t> g_pickRuns[2]{};        // UpdatePickRef runs that used each hand's ray
        PickInfo   g_pick;
        FireInfo   g_fire;
        std::atomic<bool> g_aimEnabled{ false };
        // [Controllers] bAimThrowsWithController (default 1): grenades (WEAP type 10) and mines (11) leave along the primary
        // controller's pointing direction, pitch included (F1 angles only; the game keeps its own throw origin and the
        // projectile's speed / gravity arc), and the perk trajectory preview (PerkUtilities::CalculateGrenadeTrajectory
        // D825F0) uses the same angles. 0 = the game's actor heading + look pitch (vanilla, FO4VR's code path).
        std::atomic<bool>          g_optThrow{ true };
        std::atomic<std::uint64_t> g_throwOverrides{ 0 }, g_throwSkips{ 0 }, g_trajOverrides{ 0 }, g_trajCalls{ 0 };
        bool                       g_trajHooked = false;
        float                      g_throwLastZ = 0.0f, g_throwLastX = 0.0f, g_throwGameZ = 0.0f, g_throwGameX = 0.0f;

        // ---- [Weapon] bNoSpread: no cone of fire for the player's shots outside VATS ----
        // TESObjectWEAP::Fire (4799B0) inits ProjectileLaunchData::coneOfFireRadiusMult (+0x8C) = 1.0 at 479BC0, calls
        // SetOriginAndDirection (479C35, our F1) and reads +0x8C once, at 479FDF, as the coneMult argument of
        // AimModel::CalculateFireVecs (836540): cone radius = ((max - min) * aimModel+0x90 + min) * coneMult, then
        // BSRandom::Float(0, radius) per projectile. coneMult 0 = the shot leaves exactly along the aim angles. Same effect as
        // the AMDL cone-of-fire fields = 0 of "More Accurate Weapons" (DNAM min/max/increase), for every weapon and OMOD.
        // Not touched: VATS (SetOriginAndDirection returns early through VATS::SetProjectileLaunchData; we skip while VATS
        // plays back), grenades/mines, recoil (AimModel::ApplyShotRecoil, [Controllers] bSuppressCameraRecoil), and - unless
        // bNoSpreadPellets=1 - multi-projectile weapons (shotgun pellet pattern; "More Accurate Weapons" keeps it too).
        std::atomic<bool>          g_noSpread{ false }, g_noSpreadPellets{ false };
        bool                       g_fireF1Installed = false;
        std::atomic<std::uint64_t> g_nsZeroed{ 0 }, g_nsSkipVats{ 0 }, g_nsSkipPellets{ 0 }, g_nsSkipOther{ 0 }, g_nsOff{ 0 };
        const char*                g_nsLast = "-";
        float                      g_nsBaseZ = 0.0f, g_nsBaseX = 0.0f;  // aim angles of the last player shot (after F1)
        bool                       g_nsBaseValid = false, g_nsBaseZeroed = false;
        int                        g_nsBasePellets = 1;
        struct NsShot
        {
            float dev;      // degrees between the launched projectile and the shot's aim angles
            int   pellets;  // projectiles per shot
            bool  zeroed;   // coneMult was 0
        };
        std::vector<NsShot> g_nsShots;  // per launched projectile (g_aimLock)
        std::atomic<int>  g_aimConv{ 0 };
        // A2: set while ViewCaster::UpdatePickRef runs with the hand ray (job thread), so the hooks
        // inside it (auto-aim actor, teammate cone) know the pick is the hand's, not the camera's.
        thread_local bool t_handPick = false;
        bool       g_fireHooked = false, g_pickHooked = false, g_launchHooked = false;

        using SetOriginAndDirection_t = bool (*)(RE::ProjectileLaunchData*);
        using UpdatePick_t = void (*)(void*, void*, const RE::NiPoint3*, const RE::NiPoint3*, std::uint32_t);
        using Launch_t = std::uint32_t* (*)(std::uint32_t*, RE::ProjectileLaunchData*);
        using GetViewCasterData_t = void* (*)(void*, void*);
        SetOriginAndDirection_t g_origSOD = nullptr;
        UpdatePick_t            g_origPick = nullptr;
        Launch_t                g_origLaunch = nullptr;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // camera-local (X right, Y fwd, Z up) -> world rotation only
        V3 LocalToWorld(const RE::NiMatrix3& m, const V3& l, int conv)
        {
            if (conv == 0) {  // rows are the world images of the local axes
                return { l.x * m.entry[0].x + l.y * m.entry[1].x + l.z * m.entry[2].x,
                         l.x * m.entry[0].y + l.y * m.entry[1].y + l.z * m.entry[2].y,
                         l.x * m.entry[0].z + l.y * m.entry[1].z + l.z * m.entry[2].z };
            }
            return { m.entry[0].x * l.x + m.entry[0].y * l.y + m.entry[0].z * l.z,
                     m.entry[1].x * l.x + m.entry[1].y * l.y + m.entry[1].z * l.z,
                     m.entry[2].x * l.x + m.entry[2].y * l.y + m.entry[2].z * l.z };
        }

        // Fresh, valid hand ray (any thread).
        bool GetFreshRay(HandRay& a_out)
        {
            std::scoped_lock l(g_aimLock);
            if (!g_ray.valid) {
                return false;
            }
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_ray.t).count();
            if (age > kFreshMs) {
                return false;
            }
            a_out = g_ray;
            return true;
        }

        bool GetFreshOffRay(HandRay& a_out)
        {
            std::scoped_lock l(g_aimLock);
            if (!g_rayOff.valid ||
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_rayOff.t).count() > kFreshMs) {
                return false;
            }
            a_out = g_rayOff;
            return true;
        }

        bool VatsPlayback()
        {
            const auto vats = *reinterpret_cast<std::uint8_t**>(Base() + kVatsSingleton);
            return vats && *reinterpret_cast<const std::uint32_t*>(vats + 0x40) == 1;
        }

        RE::NiPointer<RE::TESObjectREFR> LookupRef(std::uint32_t a_handle)
        {
            RE::BSPointerHandle<RE::TESObjectREFR> h;
            static_assert(sizeof(h) == sizeof(std::uint32_t));
            std::memcpy(static_cast<void*>(&h), &a_handle, sizeof(a_handle));
            return h.get();
        }

        // ---- VATS shot origin (20 Nov, VATS agent; user: "high percentage shots will fail ... point blank will hit") --------
        // Flat VATS aims each playback shot itself: SetOriginAndDirection (E42EA0) takes origin = Actor::GetFireNode world
        // (+ the first-person offset global 34484E0 only while player+0xDFE bit 4, PlayerCharacter vfunc 0x470, says first
        // person; Main::Swap writes that global = player location only in camera state 0, else 0), then, with VATS mode 1,
        // VATS::SetProjectileLaunchData (E5C050) sets the angles from that origin to the queued part (hit: its centre of mass
        // + lead; miss: just outside its pose bound). The first-person rig lives in player-relative space, so a playback
        // shot from the first-person ProjectileNode (DoShow1stPerson returned early with FRIK's body shown - see FRIK-flat
        // PovSwitchSync - or the offset global 0 in the VATS camera state) starts near the WORLD origin (user log 23:17:47:
        // origin (-4.6,46.0,74.5), player at (-31588,12284,2000); angles = world origin -> target) and never reaches the
        // target. Fix: such an origin (far from the player, itself small) + player.data.location, then
        // SetProjectileLaunchData again so the angles start from the real muzzle. [Controllers] bVatsShotFix (default 1).
        constexpr std::uintptr_t   kVatsSetLaunchData = 0xE5C050;  // VATS::SetProjectileLaunchData(VATS*, data, fireNode)
        constexpr std::uintptr_t   kVatsPredictRet1 = 0xE5E9A6;    // VATS::PredictRangedAttackCollision -> SetOriginAndDirection
        constexpr std::uintptr_t   kVatsPredictRet2 = 0xE609CE;    // VATSUtils::CreateAttackPredictionProjectile -> same
        using VatsSetLaunch_t = void (*)(void*, RE::ProjectileLaunchData*, void*);
        std::atomic<bool>          g_vatsShotFix{ true };
        std::atomic<std::uint64_t> g_vatsShots{ 0 }, g_vatsFixed{ 0 }, g_vatsPredicts{ 0 };

        void VatsShotFix(RE::ProjectileLaunchData* a_data, RE::PlayerCharacter* a_pc)
        {
            const auto  n = ++g_vatsShots;
            const auto& L = a_pc->data.location;
            const V3    o{ a_data->origin.x, a_data->origin.y, a_data->origin.z };
            const float fromPlayer = Len(Sub(o, V3{ L.x, L.y, L.z }));
            const bool  third = ((*reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(a_pc) + 0xDFE)) >> 4) & 1;
            auto* const pcam = RE::PlayerCamera::GetSingleton();
            const int   cs = pcam && pcam->currentState ? static_cast<int>(pcam->currentState->id.get()) : -1;
            const float z0 = a_data->zAngle, x0 = a_data->xAngle;
            // player-relative: the shot origin should be within reach of the body; a first-person node is a small vector
            const bool relative = fromPlayer > 400.0f && Len(o) < 300.0f;
            bool       fixed = false;
            if (relative && g_vatsShotFix.load(std::memory_order_relaxed)) {
                if (auto* vats = *reinterpret_cast<void**>(Base() + kVatsSingleton)) {
                    a_data->origin = { o.x + L.x, o.y + L.y, o.z + L.z };
                    reinterpret_cast<VatsSetLaunch_t>(Base() + kVatsSetLaunchData)(vats, a_data, nullptr);
                    fixed = true;
                    ++g_vatsFixed;
                }
            }
            if (n <= 40 || n % 50 == 0 || fixed) {
                REX::INFO("vats shot #{}: origin {} ({:.0f} u from the player) thirdPersonFlag {} camState {} angles z {:.4f} x {:.4f}{}", n, F3(o),
                    fromPlayer, third ? 1 : 0, cs, z0, x0,
                    fixed ? std::format(" -> player-relative origin fixed: {} angles z {:.4f} x {:.4f}",
                                F3(V3{ a_data->origin.x, a_data->origin.y, a_data->origin.z }), a_data->zAngle, a_data->xAngle)
                          : relative ? std::string(" (player-relative origin, bVatsShotFix=0: not fixed)") : std::string());
            }
        }

        // ---- F1 -------------------------------------------------------------------------------------
        bool SodAim(RE::ProjectileLaunchData* a_data, bool a_vatsPredict)
        {
            const bool ok = g_origSOD(a_data);
            if (ok && a_data && VatsPlayback()) {
                if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && a_data->shooter == pc) {
                    VatsShotFix(a_data, pc);
                }
            }
            if (!ok || !a_data || !g_aimEnabled.load(std::memory_order_relaxed)) {
                return ok;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || a_data->shooter != player) {
                std::scoped_lock l(g_aimLock);
                ++g_fire.calls;
                return ok;
            }
            if (a_vatsPredict) {  // VATS hit-chance prediction projectile: the game's own aim, as flat (no hand ray, no log)
                ++g_vatsPredicts;
                return ok;
            }
            const char* skip = nullptr;
            HandRay     r;
            // fromWeapon.object (+0x30) is the TESObjectWEAP; weapon type byte at +0x2CF (10 grenade, 11 mine)
            const auto* weap = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(a_data) + 0x30);
            const std::uint8_t wtype = weap ? weap[0x2CF] : 0xFF;
            const bool         thrown = wtype == 10 || wtype == 11;
            if (VatsPlayback()) {
                skip = "vats";
            } else if (thrown && !g_optThrow.load(std::memory_order_relaxed)) {
                skip = "grenade/mine (bAimThrowsWithController=0)";
            } else if (!GetFreshRay(r)) {
                skip = "no fresh hand ray";
            }
            const V3 o{ a_data->origin.x, a_data->origin.y, a_data->origin.z };
            const float z0 = a_data->zAngle, x0 = a_data->xAngle;
            if (!skip) {
                a_data->zAngle = std::atan2(r.D.x, r.D.y);
                a_data->xAngle = -std::asin(std::clamp(r.D.z / (std::max)(Len(r.D), 1e-6f), -1.0f, 1.0f));
                if (!thrown) {
                    a_data->origin = { r.P.x, r.P.y, r.P.z };
                    a_data->useOrigin = true;
                }
                // thrown: only the direction. The origin stays the game's throw point (grenade: Actor::GetCurrentFireLocation,
                // as in FO4VR 14104F5EF; mine: its fire node), the arc stays the projectile's speed + gravity.
            }
            if (thrown) {
                if (skip) {
                    ++g_throwSkips;
                } else {
                    ++g_throwOverrides;
                }
                {
                    std::scoped_lock l(g_aimLock);
                    g_throwGameZ = z0;
                    g_throwGameX = x0;
                    g_throwLastZ = a_data->zAngle;
                    g_throwLastX = a_data->xAngle;
                }
                REX::INFO("aim throw ({}): {} ; zAngle {:.4f} -> {:.4f} ; xAngle {:.4f} -> {:.4f} (hand hdg {:.1f} pitchUp {:.1f}); origin {}",
                    wtype == 10 ? "grenade" : "mine", skip ? skip : "hand direction", z0, a_data->zAngle, x0, a_data->xAngle,
                    skip ? 0.0f : HeadingDeg(r.D), skip ? 0.0f : PitchUpDeg(r.D), F3(o));
            }
            std::uint64_t n;
            {
                std::scoped_lock l(g_aimLock);
                ++g_fire.calls;
                n = ++g_fire.player;
                g_fire.lastSkip = skip ? skip : "-";
                g_fire.origO = o;
                g_fire.origZ = z0;
                g_fire.origX = x0;
                if (!skip) {
                    ++g_fire.overridden;
                    g_fire.newO = thrown ? o : r.P;
                    g_fire.newZ = a_data->zAngle;
                    g_fire.newX = a_data->xAngle;
                }
            }
            // beams/flamers re-aim through here every frame: log the first 20, then every 200th
            if (n <= 20 || n % 200 == 0) {
                if (skip) {
                    REX::INFO("aim F1 #{}: player shot not redirected ({}); origin {} z={:.4f} x={:.4f}", n, skip, F3(o), z0, x0);
                } else {
                    REX::INFO("aim F1 #{}: origin {} -> {} ; zAngle {:.4f} -> {:.4f} ; xAngle {:.4f} -> {:.4f} (hdg {:.1f} pitchUp {:.1f})",
                        n, F3(o), F3(thrown ? o : r.P), z0, a_data->zAngle, x0, a_data->xAngle, HeadingDeg(r.D), PitchUpDeg(r.D));
                }
            }
            return ok;
        }

        // [Weapon] bNoSpread on the launch data the shot is about to use (F1, after the aim override)
        void NoSpread(RE::ProjectileLaunchData* a_data)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || a_data->shooter != player) {
                return;
            }
            const auto* d = reinterpret_cast<const std::uint8_t*>(a_data);
            const auto* weap = *reinterpret_cast<const std::uint8_t* const*>(d + 0x30);  // fromWeapon.object
            const auto* inst = *reinterpret_cast<const std::uint8_t* const*>(d + 0x38);  // fromWeapon.instanceData
            const std::uint8_t wtype = weap ? weap[0x2CF] : 0xFF;
            // projectiles per shot as Fire reads them: instanceData->rangedData (+0x98), else the form's (+0x230); numProjectiles +0x2C
            const std::uint8_t* rd = inst ? *reinterpret_cast<const std::uint8_t* const*>(inst + 0x98) : nullptr;
            if (!rd && weap) rd = *reinterpret_cast<const std::uint8_t* const*>(weap + 0x230);
            const int pellets = rd ? static_cast<int>(*reinterpret_cast<const std::int8_t*>(rd + 0x2C)) : 1;
            const char* skip = nullptr;
            if (!g_noSpread.load(std::memory_order_relaxed)) {
                skip = "off", ++g_nsOff;
            } else if (VatsPlayback()) {
                skip = "vats", ++g_nsSkipVats;
            } else if (wtype == 10 || wtype == 11) {
                skip = "grenade/mine", ++g_nsSkipOther;
            } else if (pellets > 1 && !g_noSpreadPellets.load(std::memory_order_relaxed)) {
                skip = "pellets", ++g_nsSkipPellets;
            }
            if (!skip) {
                a_data->coneOfFireRadiusMult = 0.0f;
                ++g_nsZeroed;
            }
            std::scoped_lock l(g_aimLock);
            g_nsLast = skip ? skip : "zeroed";
            g_nsBaseZ = a_data->zAngle;
            g_nsBaseX = a_data->xAngle;
            g_nsBaseValid = true;
            g_nsBaseZeroed = !skip;
            g_nsBasePellets = pellets;
        }

        bool HkSetOriginAndDirection(RE::ProjectileLaunchData* a_data)
        {
            // reached by a jmp from the function's entry: the return address is the engine caller's
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base();
            const bool predict = ret == kVatsPredictRet1 || ret == kVatsPredictRet2;
            const bool ok = SodAim(a_data, predict);
            if (ok && a_data && !predict) {
                NoSpread(a_data);
            }
            return ok;
        }

        // ---- A1 -------------------------------------------------------------------------------------
        void HkUpdatePick(void* a_vc, void* a_world, const RE::NiPoint3* a_origin, const RE::NiPoint3* a_dir, std::uint32_t a_layer)
        {
            HandRay           r;
            const int         hand = g_pickHandReq.load(std::memory_order_relaxed);
            const bool        use = g_aimEnabled.load(std::memory_order_relaxed) && (hand == 1 ? GetFreshOffRay(r) : GetFreshRay(r));
            if (use) ++g_pickRuns[hand == 1 ? 1 : 0];
            const RE::NiPoint3 o{ r.P.x, r.P.y, r.P.z };
            const RE::NiPoint3 d{ r.D.x, r.D.y, r.D.z };
            t_handPick = use;
            g_origPick(a_vc, a_world, use ? &o : a_origin, use ? &d : a_dir, a_layer);
            t_handPick = false;

            alignas(16) std::uint8_t data[0x40]{};
            reinterpret_cast<GetViewCasterData_t>(Base() + kGetViewCasterData)(a_vc, data);
            std::uint32_t handle;
            std::memcpy(&handle, data, sizeof(handle));
            std::scoped_lock l(g_aimLock);
            auto& p = g_pick;
            ++p.calls;
            p.redirected = use;
            if (use) {
                ++p.redirects;
            }
            p.engineO = { a_origin->x, a_origin->y, a_origin->z };
            p.engineD = { a_dir->x, a_dir->y, a_dir->z };
            p.usedO = use ? r.P : p.engineO;
            p.usedD = use ? r.D : p.engineD;
            if (handle != p.handle) {
                p.handle = handle;
                p.formID = 0;
                p.name.clear();
                if (handle) {
                    if (auto ref = LookupRef(handle)) {
                        p.formID = ref->GetFormID();
                        const char* nm = ref->GetDisplayFullName();
                        p.name = nm ? nm : "";
                    }
                }
                REX::INFO("aim A1: pick ref -> {:08X} '{}' ({})", p.formID, p.name, !use ? "camera ray" : hand == 1 ? "off-hand ray" : "hand ray");
            }
        }

        // ---- diag: Projectile::Launch from TESObjectWEAP::Fire ----------------------------------------
        std::uint32_t* HkLaunch(std::uint32_t* a_out, RE::ProjectileLaunchData* a_data)
        {
            auto* ret = g_origLaunch(a_out, a_data);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (ret && a_data && player && a_data->shooter == player) {
                std::scoped_lock l(g_aimLock);
                if (g_nsBaseValid) {  // bNoSpread readout: this projectile vs the shot's aim angles (spread added by Fire)
                    auto dir = [](float z, float x) { return V3{ std::sin(z) * std::cos(x), std::cos(z) * std::cos(x), -std::sin(x) }; };
                    const V3    a = dir(g_nsBaseZ, g_nsBaseX), b = dir(a_data->zAngle, a_data->xAngle);
                    const float c = std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0f, 1.0f);
                    if (g_nsShots.size() >= 4096) g_nsShots.erase(g_nsShots.begin(), g_nsShots.begin() + 2048);
                    g_nsShots.push_back({ std::acos(c) * 57.2957795f, g_nsBasePellets, g_nsBaseZeroed });
                }
                auto& f = g_fire;
                ++f.launches;
                f.projHandle = *ret;
                f.launchO = { a_data->origin.x, a_data->origin.y, a_data->origin.z };
                f.launchZ = a_data->zAngle;
                f.launchX = a_data->xAngle;
                f.launchUseOrigin = a_data->useOrigin;
                f.projSeen = false;
                f.projFrames = 0;
                if (f.launches <= 20) {
                    REX::INFO("aim launch #{}: handle {:08X} origin {} zAngle {:.4f} xAngle {:.4f} useOrigin {}", f.launches,
                        f.projHandle, F3(f.launchO), f.launchZ, f.launchX, f.launchUseOrigin);
                }
            }
            return ret;
        }


        // ==== Aim follow-ups (F3 line of fire, F4 recoil, A2 auto-aim, P wall clamp) ====================
        // RVAs (1.11.240), decompiled 14 Nov 2026:
        //  F3  anon GetLineOfFireData(float len, NiPoint3* outOrigin, NiPoint3* outVec) DA6640: not VATS ->
        //      origin = world NiCamera ViewPointToRay(centre) with z = player.z + eye height, outVec = dir*len.
        //      Callers: PlayerCharacter::UpdateGunBlocked DA1CB0 (via UpdateCover, gun lowered when the
        //      segment hits), CanEnterIronSights DA1FA0, ExitIronSights DA26D0 (cover lean).
        //  F4  AimModel::ApplyShotRecoil 836980 only accumulates: recoil target +0x68/+0x6C (random arc),
        //      cone growth +0x90 (spread), shot count +0x98. It moves nothing. The kick reaches the view
        //      through AimModel::ApplyAimAdjust 837040 (out = spring offset delta), whose only caller is
        //      PlayerCharacter::ModifyMovementData D5AB70: rotation delta x += out.y, z += out.x - i.e.
        //      the player's (and so the camera's) angles. Spread (+0x90 -> CalculateFireVecs) is separate.
        //  A2  ViewCaster::UpdatePickRef 994B80: GetAutoAimActor (DA3FD0, call 9967DA) replaces an empty
        //      pick with the camera-screen-space auto-aim actor (if activatable at a distance, e.g. a
        //      companion); IsCameraLookingAtTeammate (997330, call 9956C0) makes a teammate hit by the ray
        //      see-through unless it is within the camera's command cone.
        //  clamp: anon CalculatePickHitFraction(NiPoint3& from, NiPoint3& vec, bool projectileLOS) DA6400 -
        //      the gun-blocked segment test (TES::Pick -> bhkWorld::PickObject, which takes the havok
        //      world read lock itself; movement-LOS layer mask, player's own collision group ignored).
        constexpr std::uintptr_t kLineOfFire = 0xDA6640;
        constexpr std::uintptr_t kAimAdjustCallSite = 0xD5AC41;   // in PlayerCharacter::ModifyMovementData
        constexpr std::uintptr_t kApplyAimAdjust = 0x837040;
        constexpr std::uintptr_t kAutoAimCallSite = 0x9967DA;     // in ViewCaster::UpdatePickRef
        constexpr std::uintptr_t kGetAutoAimActor = 0xDA3FD0;
        constexpr std::uintptr_t kTeammateCallSite = 0x9956C0;    // in ViewCaster::UpdatePickRef
        constexpr std::uintptr_t kIsCameraLookingAtTeammate = 0x997330;
        constexpr std::uintptr_t kCalcPickHitFraction = 0xDA6400;
        constexpr float          kClampMarginUnits = 4.0f;          // P stops this far before the hit (~6 cm)

        struct AimExtra
        {
            std::uint64_t lofCalls{ 0 }, lofOverrides{ 0 };
            float         lofLen{ 0.0f };
            V3            lofEngineO, lofEngineV, lofO, lofV;
            std::uint64_t recoilCalls{ 0 }, recoilSuppressed{ 0 };
            float         recoilSumYaw{ 0.0f }, recoilSumPitch{ 0.0f };  // suppressed view deltas (rad)
            float         am68{}, am6c{}, am70{}, am74{}, am90{};       // player AimModel: recoil target / spring offset / cone
            std::uint64_t autoAimSuppressed{ 0 }, teammateForced{ 0 };
            std::uint32_t autoAimSuppressedID{ 0 };
            std::uint64_t clampTests{ 0 }, clamped{ 0 };
            int           gunState{ -1 }, gunBlocked{ -1 };
            float         coverTimer{ 0.0f };
            std::uint32_t autoAimID{ 0 };
            std::string   autoAimName;
            std::string   probe{ "-" };
        };
        AimExtra          g_x;
        std::atomic<bool> g_optLof{ true }, g_optRecoil{ true }, g_optAutoAim{ true }, g_optClamp{ true };
        std::chrono::steady_clock::time_point g_amT{};  // last ApplyAimAdjust (player aim model values in g_x.am*)
        std::atomic<bool> g_probeRequest{ false };
        bool              g_lofHooked = false, g_recoilHooked = false, g_autoAimHooked = false, g_teammateHooked = false,
                          g_clampOk = false;

        using LineOfFire_t = void (*)(float, RE::NiPoint3*, RE::NiPoint3*);
        using ApplyAimAdjust_t = void (*)(void*, float*);
        using GetAutoAimActor_t = void (*)(void*, RE::NiPointer<RE::Actor>*);
        using Teammate_t = bool (*)(void*, RE::Actor*);
        using CalcHit_t = float (*)(RE::NiPoint3&, RE::NiPoint3&, bool);
        LineOfFire_t      g_origLof = nullptr;
        ApplyAimAdjust_t  g_origAimAdjust = nullptr;
        GetAutoAimActor_t g_origAutoAim = nullptr;
        Teammate_t        g_origTeammate = nullptr;
        CalcHit_t         g_calcHit = nullptr;

        bool VatsActive()
        {
            const auto vats = *reinterpret_cast<std::uint8_t**>(Base() + kVatsSingleton);
            return vats && *reinterpret_cast<const std::uint32_t*>(vats + 0x40) != 0;
        }

        // ---- F3 -------------------------------------------------------------------------------------
        void HkLineOfFire(float a_len, RE::NiPoint3* a_origin, RE::NiPoint3* a_vec)
        {
            g_origLof(a_len, a_origin, a_vec);
            HandRay    r;
            const bool use = g_aimEnabled.load(std::memory_order_relaxed) && g_optLof.load(std::memory_order_relaxed) &&
                             !VatsActive() && GetFreshRay(r);
            const V3 eo{ a_origin->x, a_origin->y, a_origin->z }, ev{ a_vec->x, a_vec->y, a_vec->z };
            if (use) {
                // Keep the engine's eye origin and aim the segment at the end of the hand's "barrel":
                // end = P + D * (len - |P - eye|) (at least len/2). A segment that starts at P itself misses
                // a wall P is a few units from (measured: wall 3 units ahead of P not detected - the havok
                // ray starts inside the shape's convex radius); from the eye it crosses the wall. P is
                // already clamped to the eye's side of any wall (bClampHandToCamera).
                const float barrel = (std::max)(a_len - Len(Sub(r.P, eo)), 0.5f * a_len);
                const V3    end{ r.P.x + r.D.x * barrel, r.P.y + r.D.y * barrel, r.P.z + r.D.z * barrel };
                *a_vec = { end.x - eo.x, end.y - eo.y, end.z - eo.z };
                if (gun::SuppressGunLowering()) {
                    // floating gun: the gun is physical, so a blocked test must not lower it (shots stay honest
                    // through the P clamp); a 1-unit segment from the eye only hits when the eye itself is inside
                    const float l = (std::max)(Len(ev), 1e-3f);
                    *a_vec = { ev.x / l, ev.y / l, ev.z / l };
                }
            }
            std::scoped_lock l(g_aimLock);
            ++g_x.lofCalls;
            g_x.lofLen = a_len;
            g_x.lofEngineO = eo;
            g_x.lofEngineV = ev;
            g_x.lofO = { a_origin->x, a_origin->y, a_origin->z };
            g_x.lofV = { a_vec->x, a_vec->y, a_vec->z };
            if (use) {
                ++g_x.lofOverrides;
            }
        }

        // ---- F4 -------------------------------------------------------------------------------------
        void HkApplyAimAdjust(void* a_model, float* a_out)
        {
            g_origAimAdjust(a_model, a_out);
            HandRay    r;
            const bool suppress = g_aimEnabled.load(std::memory_order_relaxed) && g_optRecoil.load(std::memory_order_relaxed) &&
                                  GetFreshRay(r);
            const auto* m = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(a_model) + 0x68);
            std::scoped_lock l(g_aimLock);
            g_amT = std::chrono::steady_clock::now();
            ++g_x.recoilCalls;
            g_x.am68 = m[0];
            g_x.am6c = m[1];
            g_x.am70 = m[2];
            g_x.am74 = m[3];
            g_x.am90 = *reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(a_model) + 0x90);
            if (suppress && (a_out[0] != 0.0f || a_out[1] != 0.0f)) {
                ++g_x.recoilSuppressed;
                g_x.recoilSumYaw += a_out[0];
                g_x.recoilSumPitch += a_out[1];
                a_out[0] = 0.0f;
                a_out[1] = 0.0f;
            }
        }

        // ---- A2 -------------------------------------------------------------------------------------
        void HkGetAutoAimActor(void* a_player, RE::NiPointer<RE::Actor>* a_out)
        {
            if (!(t_handPick && g_optAutoAim.load(std::memory_order_relaxed))) {
                g_origAutoAim(a_player, a_out);
                return;
            }
            // hand pick: the camera-screen-space actor must not stand in for an empty hand target.
            // *a_out stays null (the caller zeroes it before the call); the actor ref is released here.
            RE::NiPointer<RE::Actor> tmp;
            g_origAutoAim(a_player, &tmp);
            if (tmp) {
                const auto id = tmp->GetFormID();
                std::scoped_lock l(g_aimLock);
                ++g_x.autoAimSuppressed;
                g_x.autoAimSuppressedID = id;
            }
        }

        bool HkTeammate(void* a_vc, RE::Actor* a_actor)
        {
            if (t_handPick && g_optAutoAim.load(std::memory_order_relaxed)) {
                std::scoped_lock l(g_aimLock);
                ++g_x.teammateForced;
                return true;  // the hand ray hit this teammate directly: it is the target, not see-through
            }
            return g_origTeammate(a_vc, a_actor);
        }

        // camera -> P segment test (Present thread, in game world only). Returns the hit fraction.
        float SegmentHitFraction(const V3& a_from, const V3& a_to)
        {
            RE::NiPoint3 from{ a_from.x, a_from.y, a_from.z };
            RE::NiPoint3 vec{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
            return g_calcHit(from, vec, false);
        }

        bool InGameWorld()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->GetParentCell()) {
                return false;
            }
            auto* ui = RE::UI::GetSingleton();
            return !(ui && ui->GetMenuOpen("LoadingMenu"));
        }

        bool BytesMatch(std::uintptr_t a_addr, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
            if (std::memcmp(p, a_expect, a_n) == 0) {
                return true;
            }
            std::string got, want;
            for (std::size_t i = 0; i < a_n; ++i) {
                got += std::format("{:02X} ", p[i]);
                want += std::format("{:02X} ", a_expect[i]);
            }
            REX::ERROR("aim: {} at {:X} has bytes [{}], expected [{}]; not patching", a_what, a_addr - Base(), got, want);
            return false;
        }

        // E8 rel32 call at a_site must target a_target (both RVAs).
        bool CallMatches(std::uintptr_t a_site, std::uintptr_t a_target, const std::uint8_t (&a_bytes)[5], const char* a_what)
        {
            if (!BytesMatch(Base() + a_site, a_bytes, 5, a_what)) {
                return false;
            }
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(Base() + a_site + 1), 4);
            if (Base() + a_site + 5 + rel != Base() + a_target) {
                REX::ERROR("aim: {} call at {:X} targets {:X}, expected {:X}", a_what, a_site, a_site + 5 + rel, a_target);
                return false;
            }
            return true;
        }

        // ---- throw preview: PerkUtilities::CalculateGrenadeTrajectory (D825F0, perk "Show Grenade Trajectory") ----
        // It builds the arc from the player's look pitch (Actor::GetLooking C834E0, called at D8265A and D828B3) and
        // heading (vfunc 0x638 = PlayerCharacter::GetHeading D62DA0, call [rax+638] at D82668), like the grenade branch of
        // SetOriginAndDirection. These three calls return the hand's angles while bAimThrowsWithController applies, so the
        // preview arc is the one the grenade will fly (FO4VR 140F39D90 is the same code).
        constexpr std::uintptr_t kGetLooking = 0xC834E0;
        constexpr std::uintptr_t kTrajLookSite1 = 0xD8265A;
        constexpr std::uintptr_t kTrajHeadingSite = 0xD82668;
        constexpr std::uintptr_t kTrajLookSite2 = 0xD828B3;
        using GetLooking_t = float (*)(RE::Actor*);
        GetLooking_t g_origTrajLook1 = nullptr, g_origTrajLook2 = nullptr;

        // hand angles in the engine convention (zAngle heading 0 = +Y wrapped to [0, 2pi) as GetHeading returns it;
        // xAngle pitch, positive = down), or false = use the game's.
        bool ThrowAngles(float& a_z, float& a_x)
        {
            HandRay r;
            if (!g_aimEnabled.load(std::memory_order_relaxed) || !g_optThrow.load(std::memory_order_relaxed) || VatsActive() ||
                !GetFreshRay(r)) {
                return false;
            }
            a_z = std::atan2(r.D.x, r.D.y);
            if (a_z < 0.0f) a_z += 6.2831855f;
            a_x = -std::asin(std::clamp(r.D.z / (std::max)(Len(r.D), 1e-6f), -1.0f, 1.0f));
            return true;
        }

        float HkTrajLook1(RE::Actor* a_actor)
        {
            ++g_trajCalls;
            float z, x;
            if (ThrowAngles(z, x)) {
                ++g_trajOverrides;
                return x;
            }
            return g_origTrajLook1(a_actor);
        }

        float HkTrajLook2(RE::Actor* a_actor)
        {
            float z, x;
            return ThrowAngles(z, x) ? x : g_origTrajLook2(a_actor);
        }

        float HkTrajHeading(RE::Actor* a_actor)
        {
            float z, x;
            if (ThrowAngles(z, x)) {
                return z;
            }
            const auto* vtbl = *reinterpret_cast<const std::uintptr_t* const*>(a_actor);
            return reinterpret_cast<float (*)(RE::Actor*)>(vtbl[0x638 / 8])(a_actor);
        }
    }

    // F1 (also for [Weapon] bNoSpread alone): idempotent
    static void InstallFireDetour()
    {
        if (g_fireF1Installed) return;
        g_fireF1Installed = true;
        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) tramp.create(256);
        const auto base = Base();
        static constexpr std::uint8_t kSodPrologue[] = { 0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x49, 0x8D, 0xAB, 0x48, 0xFF, 0xFF, 0xFF };
        const std::uintptr_t sod = base + kSetOriginAndDirection;
        if (BytesMatch(sod, kSodPrologue, sizeof(kSodPrologue), "Projectile::SetOriginAndDirection prologue")) {
            // original = the 5 relocated prologue bytes (3 whole, non-RIP-relative instructions) + jmp back
            auto* stub = static_cast<std::uint8_t*>(tramp.allocate(5 + 14));
            std::memcpy(stub, reinterpret_cast<const void*>(sod), 5);
            stub[5] = 0xFF;
            stub[6] = 0x25;
            std::memset(stub + 7, 0, 4);
            const std::uint64_t back = sod + 5;
            std::memcpy(stub + 11, &back, 8);
            g_origSOD = reinterpret_cast<SetOriginAndDirection_t>(stub);
            tramp.write_jmp<5>(sod, &HkSetOriginAndDirection);
            g_fireHooked = true;
            REX::INFO("aim F1: detoured Projectile::SetOriginAndDirection at rva {:X} (original via stub {})", kSetOriginAndDirection,
                static_cast<void*>(stub));
        }
    }

    std::string InstallAimHooks()
    {
        static std::string summary;
        if (!summary.empty()) {
            return summary;
        }
        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) {
            REX::WARN("aim: F4SE trampoline not set up; creating a local one");
            tramp.create(256);
        }
        const auto base = Base();
        {
            const std::uintptr_t vcById = REL::ID(4797738).address(), vatsById = REL::ID(4797733).address();
            REX::INFO("aim: ViewCaster global id 4797738 -> rva {:X} (expected {:X}); VATS id 4797733 -> rva {:X} (expected {:X})",
                vcById - base, kPlayerViewCaster, vatsById - base, kVatsSingleton);
        }

        // F1: Projectile::SetOriginAndDirection prologue: mov r11,rsp; push rbp; push rdi; lea rbp,[r11-0xB8]
        InstallFireDetour();
        {  // [Controllers] bVatsShotFix (live): VATS playback shots from a player-relative first-person muzzle (VatsShotFix)
            static auto read = []() {
                std::string v;
                const bool on = !cfgstore::EffectiveValue("Controllers", "bVatsShotFix", v) || std::atoi(v.c_str()) != 0;
                if (on != g_vatsShotFix.exchange(on)) REX::INFO("aim: VATS shot origin fix {} ([Controllers] bVatsShotFix)", on ? "on" : "off");
                return on;
            };
            const bool on = read();
            cfgstore::Subscribe("vatsshot", "Controllers/bVatsShotFix", [](const cfgstore::Changes&) { read(); });
            REX::INFO("aim: VATS shot origin fix {} ([Controllers] bVatsShotFix; F1 {}); VATS prediction calls {:X}/{:X} keep the game's aim",
                on ? "on" : "off", g_fireHooked ? "hooked" : "NOT hooked", kVatsPredictRet1 - 5, kVatsPredictRet2 - 5);
        }

        // A1: call ViewCaster::UpdatePickRef in PlayerCharacter::UpdatePickRef
        static constexpr std::uint8_t kPickCall[5] = { 0xE8, 0x6E, 0x3E, 0xC2, 0xFF };
        if (CallMatches(kPickCallSite, kViewCasterUpdatePick, kPickCall, "ViewCaster::UpdatePickRef")) {
            g_origPick = reinterpret_cast<UpdatePick_t>(tramp.write_call<5>(base + kPickCallSite, &HkUpdatePick));
            g_pickHooked = true;
            REX::INFO("aim A1: call at rva {:X} -> hook (original {:X})", kPickCallSite, reinterpret_cast<std::uintptr_t>(g_origPick) - base);
        }

        // diagnostics: call Projectile::Launch in TESObjectWEAP::Fire
        static constexpr std::uint8_t kLaunchCall[5] = { 0xE8, 0x45, 0x81, 0x9C, 0x00 };
        if (CallMatches(kLaunchCallSite, kProjectileLaunch, kLaunchCall, "Projectile::Launch")) {
            g_origLaunch = reinterpret_cast<Launch_t>(tramp.write_call<5>(base + kLaunchCallSite, &HkLaunch));
            g_launchHooked = true;
            REX::INFO("aim diag: call at rva {:X} -> hook (original {:X})", kLaunchCallSite, reinterpret_cast<std::uintptr_t>(g_origLaunch) - base);
        }

        // Throw preview ([Controllers] bAimThrowsWithController): 3 calls in PerkUtilities::CalculateGrenadeTrajectory.
        // Own trampoline (36 B) so the shared one is not stretched. All three byte-checked, or none patched.
        {
            static constexpr std::uint8_t kLook1[5] = { 0xE8, 0x81, 0x0E, 0xF0, 0xFF };
            static constexpr std::uint8_t kLook2[5] = { 0xE8, 0x28, 0x0C, 0xF0, 0xFF };
            static constexpr std::uint8_t kHeadingCall[6] = { 0xFF, 0x90, 0x38, 0x06, 0x00, 0x00 };  // call [rax+638]
            if (CallMatches(kTrajLookSite1, kGetLooking, kLook1, "GetLooking call 1 in CalculateGrenadeTrajectory") &&
                CallMatches(kTrajLookSite2, kGetLooking, kLook2, "GetLooking call 2 in CalculateGrenadeTrajectory") &&
                BytesMatch(base + kTrajHeadingSite, kHeadingCall, sizeof(kHeadingCall), "GetHeading vcall in CalculateGrenadeTrajectory")) {
                static REL::Trampoline throwTramp{ "fo4flatvr throw preview" };
                throwTramp.create(64);
                g_origTrajLook1 = reinterpret_cast<GetLooking_t>(throwTramp.write_call<5>(base + kTrajLookSite1, &HkTrajLook1));
                g_origTrajLook2 = reinterpret_cast<GetLooking_t>(throwTramp.write_call<5>(base + kTrajLookSite2, &HkTrajLook2));
                throwTramp.write_call<6>(base + kTrajHeadingSite, &HkTrajHeading);  // HkTrajHeading calls the vfunc itself
                g_trajHooked = true;
                REX::INFO("aim throw: trajectory preview calls {:X}/{:X}/{:X} -> hand angles", kTrajLookSite1, kTrajHeadingSite, kTrajLookSite2);
            }
        }

        // ---- follow-ups ----
        // F3: GetLineOfFireData prologue: mov [rsp+10],rsi (5 B, relocated) | push rdi | sub rsp,40 | mov rax,[rip+..]
        // 20 Nov (live settings): F3 / F4 / A2 / clamp are installed whatever their switch says at load and pass through while
        // it is off (each hook checks its g_opt* atomic), so [Controllers] bHandLineOfFire ... apply live
        if (true) {  // g_optLof gates at runtime
            static constexpr std::uint8_t kLofPrologue[] = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x05 };
            const std::uintptr_t lof = base + kLineOfFire;
            if (BytesMatch(lof, kLofPrologue, sizeof(kLofPrologue), "GetLineOfFireData prologue")) {
                auto* stub = static_cast<std::uint8_t*>(tramp.allocate(5 + 14));
                std::memcpy(stub, reinterpret_cast<const void*>(lof), 5);
                stub[5] = 0xFF;
                stub[6] = 0x25;
                std::memset(stub + 7, 0, 4);
                const std::uint64_t back = lof + 5;
                std::memcpy(stub + 11, &back, 8);
                g_origLof = reinterpret_cast<LineOfFire_t>(stub);
                tramp.write_jmp<5>(lof, &HkLineOfFire);
                g_lofHooked = true;
                REX::INFO("aim F3: detoured GetLineOfFireData at rva {:X}", kLineOfFire);
            }
        }
        // F4: call AimModel::ApplyAimAdjust in PlayerCharacter::ModifyMovementData
        if (true) {  // g_optRecoil gates at runtime
            static constexpr std::uint8_t kAdjCall[5] = { 0xE8, 0xFA, 0xC3, 0xAD, 0xFF };
            if (CallMatches(kAimAdjustCallSite, kApplyAimAdjust, kAdjCall, "AimModel::ApplyAimAdjust")) {
                g_origAimAdjust = reinterpret_cast<ApplyAimAdjust_t>(tramp.write_call<5>(base + kAimAdjustCallSite, &HkApplyAimAdjust));
                g_recoilHooked = true;
                REX::INFO("aim F4: call at rva {:X} -> hook (ApplyAimAdjust {:X})", kAimAdjustCallSite, kApplyAimAdjust);
            }
        }
        // A2: GetAutoAimActor + IsCameraLookingAtTeammate calls inside ViewCaster::UpdatePickRef (needs A1)
        if (g_pickHooked) {  // g_optAutoAim gates at runtime
            static constexpr std::uint8_t kAutoAimCall[5] = { 0xE8, 0xF1, 0xD7, 0x40, 0x00 };
            if (CallMatches(kAutoAimCallSite, kGetAutoAimActor, kAutoAimCall, "PlayerCharacter::GetAutoAimActor")) {
                g_origAutoAim = reinterpret_cast<GetAutoAimActor_t>(tramp.write_call<5>(base + kAutoAimCallSite, &HkGetAutoAimActor));
                g_autoAimHooked = true;
            }
            static constexpr std::uint8_t kTeamCall[5] = { 0xE8, 0x6B, 0x1C, 0x00, 0x00 };
            if (CallMatches(kTeammateCallSite, kIsCameraLookingAtTeammate, kTeamCall, "ViewCaster::IsCameraLookingAtTeammate")) {
                g_origTeammate = reinterpret_cast<Teammate_t>(tramp.write_call<5>(base + kTeammateCallSite, &HkTeammate));
                g_teammateHooked = true;
            }
            REX::INFO("aim A2: auto-aim call {:X} {}, teammate call {:X} {}", kAutoAimCallSite, g_autoAimHooked ? "hooked" : "FAILED",
                kTeammateCallSite, g_teammateHooked ? "hooked" : "FAILED");
        }
        // clamp: no patch, a call into CalculatePickHitFraction (prologue checked: mov rax,rsp; mov [rax+8],rbx; mov [rax+10],rsi; push rbp)
        if (true) {  // g_optClamp gates at runtime
            static constexpr std::uint8_t kCalcPrologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x55 };
            if (BytesMatch(base + kCalcPickHitFraction, kCalcPrologue, sizeof(kCalcPrologue), "CalculatePickHitFraction prologue")) {
                g_calcHit = reinterpret_cast<CalcHit_t>(base + kCalcPickHitFraction);
                g_clampOk = true;
            }
        }
        auto st = [](bool a_want, bool a_ok) { return !a_want ? "off" : a_ok ? "ok" : "FAILED"; };
        summary = std::format("fire={} pick={} launch={} lof={} recoil={} autoaim={} clamp={} throw={} throwPreview={}", g_fireHooked ? "ok" : "FAILED",
            g_pickHooked ? "ok" : "FAILED", g_launchHooked ? "ok" : "FAILED", st(g_optLof, g_lofHooked), st(g_optRecoil, g_recoilHooked),
            st(g_optAutoAim.load(), g_autoAimHooked && g_teammateHooked), st(g_optClamp, g_clampOk), st(g_optThrow.load(), g_fireHooked),
            g_trajHooked ? "ok" : "FAILED");
        g_aimEnabled = g_fireHooked || g_pickHooked;
        REX::INFO("aim hooks: {} (trampoline {}B used of {}B)", summary, tramp.allocated_size(), tramp.capacity());
        return summary;
    }

    std::string InstallNoSpread(bool a_on, bool a_pellets)
    {
        g_noSpread = a_on;
        g_noSpreadPellets = a_pellets;
        if (g_noSpread.load()) {
            InstallFireDetour();  // shared with controller aim (F1); no patch at all with bNoSpread=0 and aim off
        }
        const auto r = std::format("bNoSpread={} bNoSpreadPellets={} F1={}", g_noSpread.load() ? 1 : 0, g_noSpreadPellets.load() ? 1 : 0,
            g_fireHooked ? "ok" : "not installed");
        REX::INFO("[Weapon] no spread: {}", r);
        return r;
    }

    void SetNoSpread(bool a_on, bool a_pellets)
    {
        g_noSpreadPellets = a_pellets;
        if (g_noSpread.exchange(a_on) == a_on) {
            return;
        }
        REX::INFO("[Weapon] no spread {} (live; pellets {})", a_on ? "on" : "off", a_pellets ? 1 : 0);
        if (a_on && !g_fireHooked) {
            // F1 was not installed at load (controller aim and bNoSpread both off): patch it on the main thread at the
            // main-idle point (no job thread inside TESObjectWEAP::Fire then)
            tq::RunOnMain([]() { InstallFireDetour(); });
        }
    }

    std::string NoSpreadCommand(const std::string& a_arg)
    {
        if (a_arg == "on" || a_arg == "off") {
            g_noSpread = a_arg == "on";
            if (g_noSpread.load()) InstallFireDetour();
        } else if (a_arg == "pellets on" || a_arg == "pellets off") {
            g_noSpreadPellets = a_arg == "pellets on";
        } else if (a_arg == "reset") {
            std::scoped_lock l(g_aimLock);
            g_nsShots.clear();
        } else if (a_arg.rfind("hist", 0) == 0) {
            std::scoped_lock l(g_aimLock);
            std::string out = std::format("nospread hist {}", g_nsShots.size());
            for (const auto& x : g_nsShots) out += std::format(" | {:.4f} {} {}", x.dev, x.pellets, x.zeroed ? 1 : 0);
            return out;
        } else if (!a_arg.empty() && a_arg != "status") {
            return "error: nospread [status|on|off|pellets on|off|reset|hist]";
        }
        std::scoped_lock l(g_aimLock);
        double sum = 0.0, mx = 0.0;
        for (const auto& x : g_nsShots) sum += x.dev, mx = (std::max)(mx, static_cast<double>(x.dev));
        const auto n = g_nsShots.size();
        return std::format("nospread on={} pellets={} f1={} zeroed={} off={} vats={} pelletsKept={} other={} last={} proj={} devMean={:.4f} devMax={:.4f}",
            g_noSpread.load() ? 1 : 0, g_noSpreadPellets.load() ? 1 : 0, g_fireHooked ? 1 : 0, g_nsZeroed.load(), g_nsOff.load(),
            g_nsSkipVats.load(), g_nsSkipPellets.load(), g_nsSkipOther.load(), g_nsLast, n, n ? sum / n : 0.0, mx);
    }

    void SetAimEnabled(bool a_on)
    {
        g_aimEnabled = a_on && (g_fireHooked || g_pickHooked);
    }

    bool AimEnabled()
    {
        return g_aimEnabled.load();
    }

    void SetAimConvention(int a_conv)
    {
        g_aimConv = a_conv == 1 ? 1 : 0;
    }

    void ConfigureAimExtras(bool a_lineOfFire, bool a_suppressRecoil, bool a_autoAimOverride, bool a_clamp)
    {
        g_optLof = a_lineOfFire;
        g_optRecoil = a_suppressRecoil;
        g_optAutoAim = a_autoAimOverride;
        g_optClamp = a_clamp;
    }

    void ConfigureAimThrows(bool a_on)
    {
        g_optThrow = a_on;
    }

    std::string SetAimFeature(const std::string& a_name, bool a_on)
    {
        std::atomic<bool>* f = a_name == "lof" ? &g_optLof : a_name == "recoil" ? &g_optRecoil :
                               a_name == "autoaim" ? &g_optAutoAim : a_name == "clamp" ? &g_optClamp :
                               a_name == "throw" ? &g_optThrow : a_name == "vatsshot" ? &g_vatsShotFix : nullptr;
        const bool installed = a_name == "lof" ? g_lofHooked : a_name == "recoil" ? g_recoilHooked :
                               a_name == "autoaim" ? (g_autoAimHooked || g_teammateHooked) : a_name == "clamp" ? g_clampOk :
                               a_name == "throw" || a_name == "vatsshot" ? g_fireHooked : false;
        if (!f) {
            return "error: aim lof|recoil|autoaim|clamp|throw|vatsshot on|off";
        }
        *f = a_on;
        return std::format("aim {} {}{}", a_name, a_on ? "on" : "off", installed ? "" : " (not installed at load: no effect)");
    }

    void RequestAimProbe()
    {
        g_probeRequest = true;
    }

    void UpdateHandRay(const float a_posM[3], const float a_dir[3], bool a_valid)
    {
        if (!g_fireHooked && !g_pickHooked) {
            return;
        }
        HandRay r;
        r.t = std::chrono::steady_clock::now();
        r.localM = { a_posM[0], a_posM[1], a_posM[2] };
        r.localDir = { a_dir[0], a_dir[1], a_dir[2] };
        r.camState = CurrentCameraState();
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* root = cam ? cam->cameraRoot.get() : nullptr;
        if (!a_valid) {
            r.why = "no hand/head pose";
        } else if (!root) {
            r.why = "no camera root";
        } else if (r.camState != 0 && r.camState != 4) {  // first person / iron sights only
            r.why = "camera state not first person";
        } else {
            const int   conv = g_aimConv.load();
            const auto& m = root->world.rotate;
            r.camP = { root->world.translate.x, root->world.translate.y, root->world.translate.z };
            r.camF = Norm(LocalToWorld(m, { 0.0f, 1.0f, 0.0f }, conv));
            const V3 off = LocalToWorld(m, r.localM, conv);
            r.P = { r.camP.x + off.x * g_unitsPerMeter, r.camP.y + off.y * g_unitsPerMeter, r.camP.z + off.z * g_unitsPerMeter };
            r.D = Norm(LocalToWorld(m, r.localDir, conv));
            {
                // floating gun (FloatingGun.h): P/D from the weapon's fire node, same controller snapshot
                const float rows[9] = { m.entry[0].x, m.entry[0].y, m.entry[0].z, m.entry[1].x, m.entry[1].y, m.entry[1].z,
                    m.entry[2].x, m.entry[2].y, m.entry[2].z };
                const float cp[3] = { r.camP.x, r.camP.y, r.camP.z };
                float       mp[3], md[3];
                if (conv == 0 && gun::AimFromWeapon(rows, cp, mp, md)) {
                    r.P = { mp[0], mp[1], mp[2] };
                    r.D = { md[0], md[1], md[2] };
                }
            }
            r.distM = Len(r.localM);
            if (r.distM > kMaxHandDistM) {
                r.why = "hand > 1 m from camera";
            } else if (Len(r.D) < 0.5f) {
                r.why = "zero direction";
            } else {
                r.valid = true;
                r.why = "ok";
            }
        }
        if (r.valid && g_clampOk && g_optClamp.load() && g_aimEnabled.load() && InGameWorld()) {
            // P must not be inside / behind geometry seen from the camera (shots and picks would start there)
            r.rawP = r.P;
            r.clampFrac = SegmentHitFraction(r.camP, r.P);
            const V3    v = Sub(r.P, r.camP);
            const float len = Len(v);
            if (r.clampFrac >= 0.0f && r.clampFrac < 1.0f && len > 1e-3f) {
                const float t = (std::max)(0.0f, r.clampFrac - kClampMarginUnits / len);
                r.P = { r.camP.x + v.x * t, r.camP.y + v.y * t, r.camP.z + v.z * t };
                r.clamped = true;
            }
            std::scoped_lock l(g_aimLock);
            ++g_x.clampTests;
            if (r.clamped) {
                ++g_x.clamped;
            }
        } else {
            r.rawP = r.P;
        }
        // off-hand ray for the per-hand pick, only while requested (one more wall test per frame)
        HandRay o;
        o.t = r.t;
        o.camState = r.camState;
        float op[3], od[3];
        if (g_pickHandReq.load() == 1 && root && (r.camState == 0 || r.camState == 4) && gun::OffHandAim(op, od)) {
            const int   conv = g_aimConv.load();
            const auto& m = root->world.rotate;
            o.camP = { root->world.translate.x, root->world.translate.y, root->world.translate.z };
            o.camF = Norm(LocalToWorld(m, { 0.0f, 1.0f, 0.0f }, conv));
            o.localM = { op[0], op[1], op[2] };
            o.localDir = { od[0], od[1], od[2] };
            const V3 off = LocalToWorld(m, o.localM, conv);
            o.P = { o.camP.x + off.x * g_unitsPerMeter, o.camP.y + off.y * g_unitsPerMeter, o.camP.z + off.z * g_unitsPerMeter };
            o.D = Norm(LocalToWorld(m, o.localDir, conv));
            o.distM = Len(o.localM);
            o.rawP = o.P;
            o.valid = o.distM <= kMaxHandDistM && Len(o.D) > 0.5f;
            o.why = o.valid ? "ok" : "off hand > 1 m or zero direction";
            if (o.valid && g_clampOk && g_optClamp.load() && g_aimEnabled.load() && InGameWorld()) {
                o.clampFrac = SegmentHitFraction(o.camP, o.P);
                const V3    v = Sub(o.P, o.camP);
                const float len = Len(v);
                if (o.clampFrac >= 0.0f && o.clampFrac < 1.0f && len > 1e-3f) {
                    const float t = (std::max)(0.0f, o.clampFrac - kClampMarginUnits / len);
                    o.P = { o.camP.x + v.x * t, o.camP.y + v.y * t, o.camP.z + v.z * t };
                    o.clamped = true;
                }
            }
        } else {
            o.why = "not requested / no off-hand pose";
        }
        std::scoped_lock l(g_aimLock);
        g_ray = r;
        g_rayOff = o;
    }

    bool PlayerAimModel(float a_out[4])
    {
        std::scoped_lock l(g_aimLock);
        if (!g_recoilHooked ||
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_amT).count() > kFreshMs) {
            return false;
        }
        a_out[0] = g_x.am68, a_out[1] = g_x.am6c, a_out[2] = g_x.am70, a_out[3] = g_x.am74;
        return true;
    }

    std::uint64_t PlayerLaunches()
    {
        std::scoped_lock l(g_aimLock);
        return g_fire.launches;
    }

    void SetPickHand(int a_hand)
    {
        g_pickHandReq = a_hand == 1 ? 1 : 0;
    }

    bool HandRayWorld(float a_origin[3], float a_dir[3])
    {
        HandRay r;
        if (!GetFreshRay(r)) {
            return false;
        }
        a_origin[0] = r.P.x, a_origin[1] = r.P.y, a_origin[2] = r.P.z;
        a_dir[0] = r.D.x, a_dir[1] = r.D.y, a_dir[2] = r.D.z;
        return true;
    }

    // Pointer beam (PointerBeam.h): first collision along the primary hand ray (the same P/D the pick and the shots use).
    bool HandRayWorldHit(float a_maxM, float& a_distM, bool& a_hit)
    {
        HandRay r;
        if (!g_calcHit || !(a_maxM > 0.0f) || !GetFreshRay(r) || !InGameWorld()) {
            return false;
        }
        const float  maxU = a_maxM * g_unitsPerMeter;
        RE::NiPoint3 from{ r.P.x, r.P.y, r.P.z };
        RE::NiPoint3 vec{ r.D.x * maxU, r.D.y * maxU, r.D.z * maxU };
        const float  f = g_calcHit(from, vec, false);
        a_hit = f >= 0.0f && f < 1.0f;
        a_distM = (a_hit ? f : 1.0f) * a_maxM;
        return true;
    }

    // FO4VR activation preview (bShowWandActivationPreview; VR 140EF0F30 per hand, 140F18DC0 reach): reach =
    // fPrimaryWandMaxGrabDistance (96 u), longer when pointing down: max(reach, (hand z - feet z + reach) * 1.5 * down^2)
    // (fDownwardGrabFloorDistanceMultiplier:VRWand 1.5); target = that hand's ViewCaster activate pick ref -> raycast to the
    // hit (here up to a_maxM, the flat pick reaches further than 96 u), else raycast clamped to the reach.
    bool ActivatePreview(float a_reachM, float a_floorMult, float a_maxM, float& a_lenM, bool& a_target)
    {
        HandRay r;
        auto*   player = RE::PlayerCharacter::GetSingleton();
        if (!g_calcHit || !player || !(a_reachM > 0.0f) || !GetFreshRay(r) || !InGameWorld()) {
            return false;
        }
        a_target = false;
        {
            std::scoped_lock l(g_aimLock);
            a_target = g_pick.redirected && g_pick.handle != 0 && g_pickHandReq.load() == 0;
        }
        if (a_target) {
            std::uint32_t h;
            {
                std::scoped_lock l(g_aimLock);
                h = g_pick.handle;
            }
            const auto ref = LookupRef(h);
            a_target = ref && !ref->IsDisabled() && !ref->IsDeleted();
        }
        float       reach = a_reachM;
        const float down = -r.D.z / (std::max)(Len(r.D), 1e-6f);
        if (down >= 0.0f) {
            const float handH = (r.P.z - player->data.location.z) / g_unitsPerMeter;
            reach = (std::max)(reach, (handH + a_reachM) * a_floorMult * down * down);
        }
        const float  lim = a_target ? (std::max)(a_maxM, reach) : reach;
        const float  limU = lim * g_unitsPerMeter;
        RE::NiPoint3 from{ r.P.x, r.P.y, r.P.z };
        RE::NiPoint3 vec{ r.D.x * limU, r.D.y * limU, r.D.z * limU };
        const float  f = g_calcHit(from, vec, false);
        a_lenM = (f >= 0.0f && f < 1.0f ? f : 1.0f) * lim;
        if (a_target && !(f >= 0.0f && f < 1.0f)) {
            a_lenM = reach;  // picked but not hit by this test (e.g. a non-collision activator): end at the reach
        }
        return true;
    }

    // FO4VR shows its wand laser (PrimaryWandLaserPointer) while Workshop::QEnabled or PlayerCharacter::GetActorDoingPlayerCommand.
    // 1.11.240: Workshop::QEnabled 37E430 = movzx eax, byte [30F6EDD]; ret. GetActorDoingPlayerCommand D73010 reads the handle at
    // PlayerCharacter+0xB1C.
    int BeamWorldMode()
    {
        static const std::uint8_t* s_workshop = [] {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + 0x37E430);
            if (p[0] != 0x0F || p[1] != 0xB6 || p[2] != 0x05 || p[7] != 0xC3) {
                REX::WARN("beam: Workshop::QEnabled (37E430) bytes differ; workshop beam off");
                return static_cast<const std::uint8_t*>(nullptr);
            }
            std::int32_t rel;
            std::memcpy(&rel, p + 3, sizeof(rel));
            return p + 7 + rel;
        }();
        if (s_workshop && *s_workshop) {
            return 1;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0;
        }
        std::uint32_t handle;
        std::memcpy(&handle, reinterpret_cast<const std::uint8_t*>(player) + 0xB1C, sizeof(handle));
        return handle && LookupRef(handle) ? 2 : 0;
    }

    std::uint64_t PickRuns(int a_hand)
    {
        return g_pickRuns[a_hand == 1 ? 1 : 0].load();
    }

    namespace
    {
        std::string                g_equipped;  // diagnostics: player's equipped items (main thread, every 30 ticks)
        std::atomic<std::uint32_t> g_equipRequest{ 0 };
        std::atomic<int>           g_equipMode{ 0 };
        std::string                g_equipResult{ "-" };

        // Test helper for the fire check: equip + draw a weapon the player carries (main thread).
        void DoEquip(std::uint32_t a_formID, int a_mode)
        {
            auto*       player = RE::PlayerCharacter::GetSingleton();
            auto*       form = RE::TESForm::GetFormByID(a_formID);
            auto*       bound = form ? form->As<RE::TESBoundObject>() : nullptr;
            auto*       mgr = RE::ActorEquipManager::GetSingleton();
            std::string res;
            if (!player || !bound || !mgr) {
                res = std::format("{:08X}: no player/form/manager", a_formID);
            } else {
                const auto count = player->GetInventoryObjectCount(bound);
                RE::BGSObjectInstance inst(form, nullptr);
                const bool ok = a_mode == 2 ? mgr->UnequipObject(player, &inst, 1, nullptr, 0, false, true, false, true, nullptr)
                                            : count > 0 && mgr->EquipObject(player, inst, 0, 1, nullptr, false, true, false, true, false);
                if (ok && a_mode == 0) {
                    player->DrawWeaponMagicHands(true);
                }
                res = std::format("{:08X} '{}': inventory count {}, {} {}", a_formID, RE::TESFullName::GetFullName(*form),
                    count, a_mode == 2 ? "UnequipObject" : a_mode == 1 ? "EquipObject (no draw)" : "EquipObject", ok ? "ok" : "failed");
            }
            REX::INFO("aim equip: {}", res);
            std::scoped_lock l(g_aimLock);
            g_equipResult = res;
        }
    }

    void RequestEquip(std::uint32_t a_formID, int a_mode)
    {
        g_equipMode = a_mode;
        g_equipRequest = a_formID;  // AimTick (a main tick at the main-idle point since 20 Nov: ActorEquipManager off the job threads)
    }

    void AimTick()
    {
        if (!g_launchHooked) {
            return;
        }
        if (const auto eq = g_equipRequest.exchange(0)) {
            DoEquip(eq, g_equipMode.load());
        }
        if (static std::uint32_t n = 0; ++n % 30 == 0) {
            std::string e;
            auto*       player = RE::PlayerCharacter::GetSingleton();
            auto*       mh = player && player->currentProcess ? player->currentProcess->middleHigh : nullptr;
            if (mh) {
                for (auto& it : mh->equippedItems) {
                    if (it.item.object) {
                        const auto nm = RE::TESFullName::GetFullName(*it.item.object);
                        e += std::format("{}{:08X}:{}", e.empty() ? "" : ",", it.item.object->GetFormID(), nm);
                    }
                }
            }
            std::scoped_lock l(g_aimLock);
            g_equipped = e.empty() ? "-" : e;
        }
        {
            // follow-up diagnostics: gun state (Actor+0x134 bits 14..17; 2 = blocked/lowered), gun-blocked
            // flag (+0x6C0, set by UpdateGunBlocked), cover timer (+0x6B0), auto-aim actor handle (+0xC58)
            auto*          player = RE::PlayerCharacter::GetSingleton();
            const auto*    pb = reinterpret_cast<const std::uint8_t*>(player);
            int            gs = -1, gb = -1;
            float          cover = 0.0f;
            std::uint32_t  aaID = 0;
            std::string    aaName;
            if (player) {
                gs = static_cast<int>((*reinterpret_cast<const std::uint32_t*>(pb + 0x134) >> 14) & 0xF);
                gb = pb[0x6C0];
                cover = *reinterpret_cast<const float*>(pb + 0x6B0);
                if (const auto aa = *reinterpret_cast<const std::uint32_t*>(pb + 0xC58)) {  // what GetAutoAimActor returns
                    if (auto ref = LookupRef(aa)) {
                        aaID = ref->GetFormID();
                        const char* nm = ref->GetDisplayFullName();
                        aaName = nm ? nm : "";
                    }
                }
            }
            std::string probe;
            if (g_probeRequest.exchange(false) && g_calcHit && player && InGameWorld()) {
                // horizontal wall distances around the camera (test helper for the F3/clamp checks)
                auto* cam = RE::PlayerCamera::GetSingleton();
                if (auto* root = cam ? cam->cameraRoot.get() : nullptr) {
                    const V3 c{ root->world.translate.x, root->world.translate.y, root->world.translate.z };
                    probe = std::format("cam={}", F3(c));
                    constexpr float kLen = 3000.0f;
                    // per heading: movement-LOS distance at eye height, projectile-LOS distance at eye height
                    // and 30 units lower (hand height) - "h:mov:projEye:projLow"
                    auto proj = [&](const V3& a_from, const V3& a_to) {
                        RE::NiPoint3 f{ a_from.x, a_from.y, a_from.z }, v{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
                        return g_calcHit(f, v, true);
                    };
                    for (int h = -180; h < 180; h += 15) {
                        const float a = static_cast<float>(h) / 57.2957795f;
                        const V3    to{ c.x + std::sin(a) * kLen, c.y + std::cos(a) * kLen, c.z };
                        const V3    lowFrom{ c.x, c.y, c.z - 30.0f }, lowTo{ to.x, to.y, to.z - 30.0f };
                        probe += std::format(",{}:{:.0f}:{:.0f}:{:.0f}", h, SegmentHitFraction(c, to) * kLen, proj(c, to) * kLen,
                            proj(lowFrom, lowTo) * kLen);
                    }
                    // segment tests with both layer sets (false = movement LOS, true = projectile LOS):
                    // eye along camera forward, eye -> last line-of-fire end, P along D (96 units)
                    HandRay hr;
                    V3      lo, lv;
                    {
                        std::scoped_lock l(g_aimLock);
                        hr = g_ray;
                        lo = g_x.lofO;
                        lv = g_x.lofV;
                    }
                    auto seg = [&](const char* a_name, const V3& a_from, const V3& a_vec) {
                        RE::NiPoint3 f{ a_from.x, a_from.y, a_from.z }, v{ a_vec.x, a_vec.y, a_vec.z };
                        const float  m0 = g_calcHit(f, v, false);
                        RE::NiPoint3 f2{ a_from.x, a_from.y, a_from.z }, v2{ a_vec.x, a_vec.y, a_vec.z };
                        const float  m1 = g_calcHit(f2, v2, true);
                        probe += std::format(" {}=mov:{:.3f}/proj:{:.3f}", a_name, m0, m1);
                    };
                    seg("eyeF", c, { hr.camF.x * 96.0f, hr.camF.y * 96.0f, hr.camF.z * 96.0f });
                    seg("eyeLof", lo, lv);
                    seg("PD", hr.P, { hr.D.x * 96.0f, hr.D.y * 96.0f, hr.D.z * 96.0f });
                    seg("eyeP", c, Sub(hr.P, c));
                }
            }
            std::scoped_lock l(g_aimLock);
            g_x.gunState = gs;
            g_x.gunBlocked = gb;
            g_x.coverTimer = cover;
            g_x.autoAimID = aaID;
            g_x.autoAimName = aaName;
            if (!probe.empty()) {
                g_x.probe = probe;
            }
        }
        std::uint32_t h;
        {
            std::scoped_lock l(g_aimLock);
            if (!g_fire.projHandle || g_fire.projFrames >= 600) {
                return;
            }
            h = g_fire.projHandle;
        }
        auto ref = LookupRef(h);
        std::scoped_lock l(g_aimLock);
        if (g_fire.projHandle != h) {
            return;
        }
        if (!ref) {
            g_fire.projFrames = 600;  // gone
            return;
        }
        const V3 pos{ ref->data.location.x, ref->data.location.y, ref->data.location.z };
        if (!g_fire.projSeen) {
            g_fire.projFirst = pos;
            g_fire.projSeen = true;
        }
        g_fire.projLast = pos;
        g_fire.projAngle = { ref->data.angle.x, ref->data.angle.y, ref->data.angle.z };
        if (auto* proj = ref->As<RE::Projectile>()) {
            g_fire.projVel = { proj->velocity.x, proj->velocity.y, proj->velocity.z };
            g_fire.projMoveDir = { proj->movementDirection.x, proj->movementDirection.y, proj->movementDirection.z };
            g_fire.projImpacts = static_cast<int>(proj->impacts.size());
            if (!proj->impacts.empty()) {
                const auto& loc = proj->impacts[0].location;
                g_fire.projImpact = { loc.x, loc.y, loc.z };
            }
        }
        ++g_fire.projFrames;
    }

    std::string AimReport()
    {
        HandRay     r;
        PickInfo    p;
        FireInfo    f;
        std::string equipped;
        {
            std::scoped_lock l(g_aimLock);
            r = g_ray;
            p = g_pick;
            f = g_fire;
            equipped = g_equipped + " | equip: " + g_equipResult;
        }
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r.t).count();
        std::string s = std::format("enabled={} hooks=fire:{},pick:{},launch:{} conv={} ray={} why='{}' age={}ms camstate={}",
            g_aimEnabled.load() ? 1 : 0, g_fireHooked ? 1 : 0, g_pickHooked ? 1 : 0, g_launchHooked ? 1 : 0, g_aimConv.load(),
            r.valid ? 1 : 0, r.why, r.t.time_since_epoch().count() ? age : -1, r.camState);
        s += std::format(" local={} localDir={} P={} D={} cam={} camF={} hdgD={:.1f} hdgCam={:.1f} pitchD={:.1f} pitchCam={:.1f} handDist={:.2f}m",
            F3(r.localM, 4), F3(r.localDir, 4), F3(r.P), F3(r.D, 4), F3(r.camP), F3(r.camF, 4), HeadingDeg(r.D), HeadingDeg(r.camF),
            PitchUpDeg(r.D), PitchUpDeg(r.camF), r.distM);
        // engine's own camera ray (NiCamera row 0) vs camera-root row 1 / column 1: settles the matrix convention
        if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
            const auto& m = cam->cameraRoot->world.rotate;
            s += std::format(" rootRow1={} rootCol1={}", F3(V3{ m.entry[1].x, m.entry[1].y, m.entry[1].z }, 4),
                F3(V3{ m.entry[0].y, m.entry[1].y, m.entry[2].y }, 4));
        }
        s += std::format(" engineO={} engineD={} pickRedirected={} pickCalls={} pickRedirects={} pick={:08X} pickName='{}'",
            F3(p.engineO), F3(p.engineD, 4), p.redirected ? 1 : 0, p.calls, p.redirects, p.formID, p.name);
        {
            HandRay o;
            {
                std::scoped_lock l(g_aimLock);
                o = g_rayOff;
            }
            s += std::format(" pickHand={} pickRunsPrimary={} pickRunsOff={} offRay={} offWhy='{}' offP={} offD={} offHdg={:.1f} offPitch={:.1f}",
                g_pickHandReq.load() == 1 ? "off" : "primary", g_pickRuns[0].load(), g_pickRuns[1].load(), o.valid ? 1 : 0, o.why, F3(o.P), F3(o.D, 4),
                HeadingDeg(o.D), PitchUpDeg(o.D));
        }
        s += std::format(" fireCalls={} firePlayer={} fireOverridden={} fireSkip='{}' fireOrigO={} fireNewO={} fireZ={:.4f}->{:.4f} fireX={:.4f}->{:.4f}",
            f.calls, f.player, f.overridden, f.lastSkip, F3(f.origO), F3(f.newO), f.origZ, f.newZ, f.origX, f.newX);
        const V3 travel = Sub(f.projLast, f.launchO);
        s += std::format(" launches={} proj={:08X} launchO={} launchZ={:.4f} launchX={:.4f} useOrigin={} projFrames={} projFirst={} projLast={} projTravel={:.1f} projHdg={:.1f} projPitch={:.1f} launchHdg={:.1f} launchPitch={:.1f}",
            f.launches, f.projHandle, F3(f.launchO), f.launchZ, f.launchX, f.launchUseOrigin ? 1 : 0, f.projFrames, F3(f.projFirst),
            F3(f.projLast), Len(travel), HeadingDeg(travel), PitchUpDeg(travel), f.launchZ * 57.2957795f, -f.launchX * 57.2957795f);
        const V3 toImpact = Sub(f.projImpact, f.launchO);
        s += std::format(" projAngle=({:.4f},{:.4f}) projVel={} projVelHdg={:.1f} projVelPitch={:.1f} projMoveDir={} projImpacts={} projImpact={} impactDist={:.1f} impactHdg={:.1f} impactPitch={:.1f}",
            f.projAngle.x, f.projAngle.z, F3(f.projVel), HeadingDeg(f.projVel), PitchUpDeg(f.projVel), F3(f.projMoveDir, 4), f.projImpacts,
            F3(f.projImpact), Len(toImpact), HeadingDeg(toImpact), PitchUpDeg(toImpact));
        {
            AimExtra x;
            {
                std::scoped_lock l(g_aimLock);
                x = g_x;
            }
            constexpr float r2d = 57.2957795f;
            s += std::format(" opts=lof:{}{},recoil:{}{},autoaim:{}{},clamp:{}{}", g_optLof.load() ? 1 : 0, g_lofHooked ? "" : "!",
                g_optRecoil.load() ? 1 : 0, g_recoilHooked ? "" : "!", g_optAutoAim.load() ? 1 : 0, g_autoAimHooked ? "" : "!",
                g_optClamp.load() ? 1 : 0, g_clampOk ? "" : "!");
            {
                std::scoped_lock l(g_aimLock);
                s += std::format(" throw={} throwOverrides={} throwSkips={} throwGameHdg={:.1f} throwGamePitchDown={:.1f} throwHdg={:.1f} throwPitchDown={:.1f} trajHooked={} trajCalls={} trajOverrides={}",
                    g_optThrow.load() ? 1 : 0, g_throwOverrides.load(), g_throwSkips.load(), g_throwGameZ * r2d, g_throwGameX * r2d,
                    g_throwLastZ * r2d, g_throwLastX * r2d, g_trajHooked ? 1 : 0, g_trajCalls.load(), g_trajOverrides.load());
                s += std::format(" vatsShotFix={} vatsShots={} vatsFixed={} vatsPredictions={}", g_vatsShotFix.load() ? 1 : 0, g_vatsShots.load(),
                    g_vatsFixed.load(), g_vatsPredicts.load());
            }
            const V3 lofEnd{ x.lofO.x + x.lofV.x, x.lofO.y + x.lofV.y, x.lofO.z + x.lofV.z };
            s += std::format(" lofCalls={} lofOverrides={} lofLen={:.1f} lofEngineO={} lofEngineHdg={:.1f} lofEnginePitch={:.1f} lofO={} lofEnd={} lofVecLen={:.1f} lofHdg={:.1f} lofPitch={:.1f}",
                x.lofCalls, x.lofOverrides, x.lofLen, F3(x.lofEngineO), HeadingDeg(x.lofEngineV), PitchUpDeg(x.lofEngineV), F3(x.lofO),
                F3(lofEnd), Len(x.lofV), HeadingDeg(x.lofV), PitchUpDeg(x.lofV));
            s += std::format(" gunState={} gunBlocked={} coverTimer={:.1f}", x.gunState, x.gunBlocked, x.coverTimer);
            s += std::format(" recoilCalls={} recoilSuppressed={} recoilSumYaw={:.3f} recoilSumPitch={:.3f} am68={:.4f} am6c={:.4f} am70={:.4f} am74={:.4f} amCone={:.3f}",
                x.recoilCalls, x.recoilSuppressed, x.recoilSumYaw * r2d, x.recoilSumPitch * r2d, x.am68, x.am6c, x.am70, x.am74, x.am90);
            s += std::format(" autoAim={:08X} autoAimName='{}' autoAimSuppressed={} autoAimSuppressedID={:08X} teammateForced={}",
                x.autoAimID, x.autoAimName, x.autoAimSuppressed, x.autoAimSuppressedID, x.teammateForced);
            s += std::format(" rawP={} clampFrac={:.4f} clamped={} clampTests={} clampedFrames={} rawDist={:.1f} PDist={:.1f}", F3(r.rawP),
                r.clampFrac, r.clamped ? 1 : 0, x.clampTests, x.clamped, Len(Sub(r.rawP, r.camP)), Len(Sub(r.P, r.camP)));
            s += std::format(" probe='{}'", x.probe);
        }
        s += std::format(" equipped='{}'", equipped);
        return s;
    }
}
