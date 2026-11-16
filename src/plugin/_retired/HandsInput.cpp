// Weapon in hand: controller snapshot from the XR frame (Presenter thread). No RE types here.
#include "plugin/Hands.h"
#include "plugin/FrikBridge.h"  // FRIK-flat bridge feed (one call in PublishXr)
#include "plugin/HandsInternal.h"
#include "xr/XrRuntime.h"

#include <algorithm>
#include <cmath>

namespace fo4flatvr::hands
{
    namespace
    {
        xr::Vec3 Cross(const xr::Vec3& a, const xr::Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

        xr::Vec3 Rot(const xr::Quat& q, const xr::Vec3& v)  // q * v * q^-1 (unit q)
        {
            const xr::Vec3 u{ q.x, q.y, q.z };
            const xr::Vec3 c = Cross(u, v);
            const xr::Vec3 t{ 2 * c.x, 2 * c.y, 2 * c.z };
            const xr::Vec3 c2 = Cross(u, t);
            return { v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z };
        }

        // OpenXR vector (X right, Y up, -Z forward) -> game basis (X right, Y forward, Z up)
        void ToGame(const xr::Vec3& v, float o[3])
        {
            o[0] = v.x;
            o[1] = -v.z;
            o[2] = v.y;
        }

        void Rel(const xr::Pose& a_grip, bool a_gripValid, const xr::Pose& a_aim, bool a_aimValid, const xr::Pose& a_head, bool a_headValid,
            detail::HandRel& a_out)
        {
            a_out.valid = a_gripValid && a_aimValid && a_headValid;
            if (!a_out.valid) {
                return;
            }
            const auto&    hq = a_head.orientation;
            const xr::Quat inv{ -hq.x, -hq.y, -hq.z, hq.w };
            auto local = [&](const xr::Vec3& p) {
                return Rot(inv, { p.x - a_head.position.x, p.y - a_head.position.y, p.z - a_head.position.z });
            };
            ToGame(local(a_grip.position), a_out.pos);
            ToGame(local(a_aim.position), a_out.aimPos);
            ToGame(Rot(inv, Rot(a_aim.orientation, { 0.0f, 0.0f, -1.0f })), a_out.aimDir);
            // grip frame axes in game basis: X = +X, Y = -Z, Z = +Y; their images in the head frame = columns
            const xr::Vec3 axes[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } };
            for (int c = 0; c < 3; ++c) {
                float g[3];
                ToGame(Rot(inv, Rot(a_grip.orientation, axes[c])), g);
                for (int r = 0; r < 3; ++r) {
                    a_out.rot[r][c] = g[r];
                }
            }
        }

        detail::HandInput Input(const xr::ControllerState& a_c, bool a_left)
        {
            detail::HandInput in;
            if (!a_c.active) return in;
            in.trigger = std::clamp(a_left ? a_c.leftTrigger : a_c.rightTrigger, 0.0f, 1.0f);
            in.grip = std::clamp(a_left ? a_c.leftGrip : a_c.rightGrip, 0.0f, 1.0f);
            const bool  button = a_left ? (a_c.x || a_c.y || a_c.leftStickClick) : (a_c.a || a_c.b || a_c.rightStickClick);
            const float sx = a_left ? a_c.leftStickX : a_c.rightStickX, sy = a_left ? a_c.leftStickY : a_c.rightStickY;
            in.thumb = button ? 1.0f : (std::sqrt(sx * sx + sy * sy) > 0.3f ? 0.6f : 0.0f);
            return in;
        }
    }

    void PublishXr(const xr::ControllerState& a_c, const xr::Pose& a_newHead, bool a_newValid, const xr::Pose& a_oldHead, bool a_oldValid,
        bool a_worldView, bool a_leftHanded)
    {
        frikbridge::PublishXr(a_c, a_newHead, a_newValid, a_worldView, a_leftHanded);  // FRIK-flat (FrikBridge.h)
        static unsigned long long seq = 0;
        detail::Snap s;
        s.worldView = a_worldView;
        s.leftHanded = a_leftHanded;
        const bool active = a_c.active;
        for (int side = 0; side < 2; ++side) {
            const bool       left = (side == 0) == a_leftHanded;  // side 0 = primary
            const xr::Pose&  grip = left ? a_c.leftGripPose : a_c.rightGripPose;
            const bool       gripValid = active && (left ? a_c.leftGripPoseValid : a_c.rightGripPoseValid);
            const xr::Pose&  aim = left ? a_c.leftAim : a_c.rightAim;
            const bool       aimValid = active && (left ? a_c.leftAimValid : a_c.rightAimValid);
            Rel(grip, gripValid, aim, aimValid, a_newHead, a_newValid, side == 0 ? s.rNew : s.lNew);
            Rel(grip, gripValid, aim, aimValid, a_oldHead, a_oldValid, side == 0 ? s.rOld : s.lOld);
            (side == 0 ? s.inPrimary : s.inOff) = Input(a_c, left);
        }
        s.headNewValid = a_newValid;
        s.headNewQ[0] = a_newHead.orientation.x, s.headNewQ[1] = a_newHead.orientation.y, s.headNewQ[2] = a_newHead.orientation.z,
        s.headNewQ[3] = a_newHead.orientation.w;
        s.headNewP[0] = a_newHead.position.x, s.headNewP[1] = a_newHead.position.y, s.headNewP[2] = a_newHead.position.z;
        s.t = std::chrono::steady_clock::now();
        s.seq = ++seq;
        detail::SetSnapshot(s);
    }
}
