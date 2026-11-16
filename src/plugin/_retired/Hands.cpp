// Weapon in hand, stage 0 (probes) + stage 1 (rigid weapon follow). See Hands.h and analysis/vr-hands-weapon.md.
#include "plugin/Hands.h"
#include "plugin/HandsInternal.h"
#include "plugin/Settings.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo4flatvr::hands
{
    namespace
    {
        // ---- 1.11.240 RVAs (analysis/vr-hands-weapon.md, db/labels/hands_research.csv) ------------------
        constexpr std::uintptr_t kNotifyCallSite = 0xC351F3;   // Main::OnIdle_UpdatePlayer -> PlayerUpdateEvent Notify
        constexpr std::uintptr_t kNotify = 0xC403B0;
        constexpr std::uintptr_t kSwapCallSite = 0xC346AC;     // Main::OnIdle -> Main::Swap
        constexpr std::uintptr_t kSwap = 0xC33250;
        constexpr std::uintptr_t kUpdateNode = 0xD9F7D0;       // (float time, NiAVObject*, NiCamera*, bool) -> NiAVObject::Update
        constexpr std::uintptr_t kQ1stPersonCamera = 0x21FA470;
        constexpr std::uintptr_t kFrameTime = 0x30E8940;       // float passed by UpdateScenegraph to D9F7D0
        constexpr std::uintptr_t kFpOffset = 0x34484E0;        // NiPoint3: 1st-person world offset (Main::Swap)
        constexpr std::uintptr_t kFpCamera = 0x3E622D0;        // NiCamera* 1st-person camera
        constexpr std::uintptr_t kFpCameraLoc = 0x3E622B8;     // NiPoint3 1st-person camera location
        constexpr std::uintptr_t kVatsSingleton = 0x30F9FD0;   // VATS*, mode at +0x40
        constexpr std::uintptr_t kActorInPowerArmor = 0x9893F0;

        constexpr long long kFreshMs = 200;
        constexpr double    kR2D = 57.29577951308232;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // ---- math: column-vector rigid transforms, v' = s R v + t -----------------------------------------
        struct V { double x{}, y{}, z{}; };
        V      operator+(const V& a, const V& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        V      operator-(const V& a, const V& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        V      operator*(const V& a, double k) { return { a.x * k, a.y * k, a.z * k }; }
        double Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V      Cross(const V& a, const V& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        double Len(const V& a) { return std::sqrt(Dot(a, a)); }
        V      Norm(const V& a)
        {
            const double l = Len(a);
            return l > 1e-12 ? a * (1.0 / l) : V{};
        }
        std::string F(const V& v, int p = 2) { return std::format("({:.{}f},{:.{}f},{:.{}f})", v.x, p, v.y, p, v.z, p); }

        struct M3
        {
            double m[3][3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            V      Col(int c) const { return { m[0][c], m[1][c], m[2][c] }; }
            void   SetCol(int c, const V& v) { m[0][c] = v.x, m[1][c] = v.y, m[2][c] = v.z; }
        };
        M3 Mul(const M3& a, const M3& b)
        {
            M3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            return r;
        }
        M3 T(const M3& a)
        {
            M3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r.m[i][j] = a.m[j][i];
            return r;
        }
        V Mul(const M3& a, const V& v)
        {
            return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
                a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
        }
        // Gram-Schmidt on the columns (keeps column 1 = forward exact, then column 2).
        M3 Ortho(const M3& a)
        {
            const V y = Norm(a.Col(1));
            V       z = a.Col(2);
            z = Norm(z - y * Dot(z, y));
            const V x = Cross(y, z);
            M3      r;
            r.SetCol(0, x), r.SetCol(1, y), r.SetCol(2, z);
            return r;
        }
        double AngleDeg(const M3& a, const M3& b)  // rotation angle of a * b^T
        {
            const M3     d = Mul(a, T(b));
            const double c = std::clamp((d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0) * 0.5, -1.0, 1.0);
            return std::acos(c) * kR2D;
        }
        M3 Rx(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[1][1] = c, r.m[1][2] = -s, r.m[2][1] = s, r.m[2][2] = c; return r; }
        M3 Ry(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[0][0] = c, r.m[0][2] = s, r.m[2][0] = -s, r.m[2][2] = c; return r; }
        M3 Rz(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[0][0] = c, r.m[0][1] = -s, r.m[1][0] = s, r.m[1][1] = c; return r; }
        // Right-handed rotation by a about the unit axis u (column-vector convention).
        M3 AxisAngle(const V& a_u, double a)
        {
            const V      u = Norm(a_u);
            const double c = std::cos(a), s = std::sin(a), t = 1.0 - c;
            M3           r;
            r.m[0][0] = c + u.x * u.x * t, r.m[0][1] = u.x * u.y * t - u.z * s, r.m[0][2] = u.x * u.z * t + u.y * s;
            r.m[1][0] = u.y * u.x * t + u.z * s, r.m[1][1] = c + u.y * u.y * t, r.m[1][2] = u.y * u.z * t - u.x * s;
            r.m[2][0] = u.z * u.x * t - u.y * s, r.m[2][1] = u.z * u.y * t + u.x * s, r.m[2][2] = c + u.z * u.z * t;
            return r;
        }
        // Minimal rotation taking direction a_from to a_to (FRIK getMatrixFromRotateVectorVec(to, from), column form).
        M3 RotFromTo(const V& a_from, const V& a_to)
        {
            const V      f = Norm(a_from), t = Norm(a_to);
            const double d = Dot(f, t);
            if (d >= 0.99999) return M3{};
            if (d <= -0.99999) {
                const V h = std::abs(f.x) < 0.9 ? V{ 1, 0, 0 } : V{ 0, 1, 0 };
                return AxisAngle(Cross(f, h), 3.14159265358979);
            }
            return AxisAngle(Cross(f, t), std::acos(std::clamp(d, -1.0, 1.0)));
        }
        // Rotation <-> unit quaternion (w, x, y, z), for slerp.
        struct Q4 { double w{ 1 }, x{}, y{}, z{}; };
        Q4 ToQ(const M3& m)
        {
            Q4           q;
            const double tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
            if (tr > 0) {
                const double s = std::sqrt(tr + 1.0) * 2;
                q = { 0.25 * s, (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s };
            } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
                const double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
                q = { (m.m[2][1] - m.m[1][2]) / s, 0.25 * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s };
            } else if (m.m[1][1] > m.m[2][2]) {
                const double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
                q = { (m.m[0][2] - m.m[2][0]) / s, (m.m[0][1] + m.m[1][0]) / s, 0.25 * s, (m.m[1][2] + m.m[2][1]) / s };
            } else {
                const double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
                q = { (m.m[1][0] - m.m[0][1]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25 * s };
            }
            const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            return { q.w / n, q.x / n, q.y / n, q.z / n };
        }
        M3 FromQ(const Q4& q)
        {
            M3 m;
            m.m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z), m.m[0][1] = 2 * (q.x * q.y - q.z * q.w), m.m[0][2] = 2 * (q.x * q.z + q.y * q.w);
            m.m[1][0] = 2 * (q.x * q.y + q.z * q.w), m.m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z), m.m[1][2] = 2 * (q.y * q.z - q.x * q.w);
            m.m[2][0] = 2 * (q.x * q.z - q.y * q.w), m.m[2][1] = 2 * (q.y * q.z + q.x * q.w), m.m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
            return m;
        }
        M3 Slerp(const M3& a, const M3& b, double t)
        {
            const Q4 qa = ToQ(a);
            Q4       qb = ToQ(b);
            double   d = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
            if (d < 0) qb = { -qb.w, -qb.x, -qb.y, -qb.z }, d = -d;
            double ka, kb;
            if (d > 0.9995) {
                ka = 1 - t, kb = t;
            } else {
                const double th = std::acos(std::clamp(d, -1.0, 1.0)), s = std::sin(th);
                ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
            }
            Q4           q{ ka * qa.w + kb * qb.w, ka * qa.x + kb * qb.x, ka * qa.y + kb * qb.y, ka * qa.z + kb * qb.z };
            const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            return FromQ({ q.w / n, q.x / n, q.y / n, q.z / n });
        }
        // a_k-th power of a rotation (angle scaled about the same axis).
        M3 Scale(const M3& a, double a_k) { return Slerp(M3{}, a, a_k); }
        M3 Diag(double a, double b, double c) { M3 r; r.m[0][0] = a, r.m[1][1] = b, r.m[2][2] = c; return r; }

        struct Xf
        {
            M3     R;
            V      t;
            double s{ 1.0 };
        };
        Xf Mul(const Xf& a, const Xf& b) { return { Mul(a.R, b.R), a.t + Mul(a.R, b.t) * a.s, a.s * b.s }; }
        Xf Inv(const Xf& a)
        {
            Xf r;
            r.R = T(a.R);
            r.s = 1.0 / a.s;
            r.t = Mul(r.R, a.t) * (-r.s);
            return r;
        }
        // NiTransform storage: entry[i] = row i = local axis i in the parent (= column i of R).
        Xf FromNi(const RE::NiTransform& n)
        {
            Xf x;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    x.R.m[r][c] = n.rotate.entry[c][r];
            x.t = { n.translate.x, n.translate.y, n.translate.z };
            x.s = n.scale;
            return x;
        }
        void ToNi(const Xf& x, RE::NiTransform& n)
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    n.rotate.entry[c][r] = static_cast<float>(x.R.m[r][c]);
            n.translate = { static_cast<float>(x.t.x), static_cast<float>(x.t.y), static_cast<float>(x.t.z) };
            n.scale = static_cast<float>(x.s);
        }
        // From storage rows (FRIK JSON "rotation", NIF data) + translation.
        Xf FromRows(const double rows[9], const V& t)
        {
            Xf x;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    x.R.m[r][c] = rows[c * 3 + r];
            x.R = Ortho(x.R);
            x.t = t;
            return x;
        }
        V NiV(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }

        // ---- constants of the chain (analysis/vr-hands-weapon.md section 2) -------------------------------
        // FRIK glue G (Skeleton::placeFirstPersonArm): MatrixUtils::getMatrix fills columns, so the storage rows
        // are the transpose of its argument list (= exactly the rotation in FRIK's 10mm.json).
        const double kGRows[9] = { -0.122, 0.990, 0.069, 0.987, 0.114, 0.109, 0.100, 0.081, -0.992 };
        const V      kGT{ 4.389, -1.899, -3.133 };
        // FO4VR PrimaryWeaponToWandNode: translate (fWeaponOffset_X/Y/Z:VR) = (0, -4, 1), rotation 0.
        const V      kWandToOffset{ 0.0, -4.0, 1.0 };
        // Skeleton bind of WEAPON in RArm_Hand (skeleton.nif, design doc 1.1); replaced by the measured in-game
        // local at runtime when available (bind "J" for weapons without a JSON).
        const double kBindRows[9] = { -0.994, 0.028, -0.110, 0.109, -0.011, -0.994, -0.029, -1.0, 0.008 };
        const V      kBindT{ 6.744, -3.485, 0.176 };

        // ---- config --------------------------------------------------------------------------------------
        struct Cfg
        {
            bool  enabled{ true };       // [Hands] bWeaponInHand
            bool  aimFromMuzzle{ true }; // [Hands] bAimFromMuzzle
            bool  noGunLowering{ true }; // [Hands] bNoGunLowering
            int   align{ 0 };            // [Hands] iAlign: 0 = FRIK JSON (auto-align without one), 1 = always auto-align, 2 = JSON/bind only
            float offCm[3]{ 0.0f, 0.0f, 0.0f };   // [Hands] fGripOffsetX/Y/Z (grip frame: right, forward, up; cm)
            float rotDeg[3]{ -60.0f, 0.0f, 0.0f };  // [Hands] fGripRotX/Y/ZDeg (about grip X, then Y, then Z); -60 = barrel along the SteamVR Touch aim pose
            float unitsPerMeter{ 70.0f };
            std::string jsonDir;
            // ---- stage 2 (analysis/vr-hands-weapon.md section 3, stage 2) ----
            bool  armIK{ true };          // [Hands] bArmIK: body under the HMD at COM + two-bone IK of both arms
            bool  twoHanded{ false };     // [Hands] bTwoHanded: off hand snaps to the animation's support grip when near it
            bool  keepRecoil{ true };     // [Hands] bKeepRecoil: re-apply the animation's kick (delta from its pre-shot pose)
            bool  fingers{ true };        // [Hands] bFingers: free off-hand fingers from trigger/grip/thumb
            bool  pipboyOnBone{ true };   // [Hands] bPipboyOnBone: wrist Pip-Boy quad at PipboyBone while the IK runs
            float neckFwdCm{ 8.0f };      // [Hands] fEyeToNeckFwdCm / fEyeToNeckUpCm: eye -> neck pivot (head frame)
            float neckUpCm{ 12.0f };
            float bodyFwdCm{ 0.0f };      // [Hands] fBodyOffsetFwdCm / fBodyOffsetUpCm: extra body offset (yaw frame)
            float bodyUpCm{ 0.0f };
            float armLength{ 36.74f };    // [Hands] fArmLength: FRIK armLength (game units; 36.74 = bind, no scaling)
            float maxStretch{ 1.5f };     // [Hands] fArmMaxStretch: off-hand bone stretch limit (the gun hand always reaches)
            float shoulderReach{ 0.15f }; // [Hands] fArmShoulderReachFraction (FRIK)
            float shoulderDownDamp{ 0.4f };
            float minFlexDeg{ 10.0f };    // [Hands] fArmElbowMinFlexionDeg / fArmElbowMaxFlexionDeg (FRIK)
            float maxFlexDeg{ 145.0f };
            float twistWindowDeg{ 15.0f };
            float forearmTwist[3]{ 0.0f, 0.5f, 0.5f };  // FRIK armForearmTwistWeight1..3
            float snapCm{ 10.0f };        // [Hands] fTwoHandedSnapCm / fTwoHandedReleaseCm
            float releaseCm{ 15.0f };
            float gripMaxDist{ 40.0f };   // [Hands] fSupportGripMaxDist (units): animated support hand farther than this from WEAPON = no grip point
            float recoilScale{ 1.0f };    // [Hands] fRecoilScale, fRecoilWindowMs, fRecoilMaxCm, fRecoilMaxDeg
            float recoilWindowMs{ 400.0f };
            float recoilMaxCm{ 8.0f };
            float recoilMaxDeg{ 20.0f };
            float pipboyLiftCm{ 1.5f };   // [Hands] fPipboyLiftCm: quad lifted off PipboyBone along the back of the hand
        };

        struct Probe
        {
            int           framesLeft{ 0 };
            int           frames{ 0 };
            // WEAPON world at the hook end (A), pre-Swap (B), Present (C)
            bool          aFresh{ false }, bSeen{ false };   // A not yet compared at Present / at Swap
            double        tA{ 0 }, tB{ 0 }, tC{ 0 };          // QPC us of the last A, B, C
            int           orderABC{ 0 }, orderACB{ 0 }, orderOther{ 0 };
            double        tA0{ 0 }, tBend{ 0 };               // hook start, Swap return
            int           hookDuringSwap{ 0 }, hookAfterSwap{ 0 }, hookBeforeSwap{ 0 };
            Xf            a;
            V             camA;
            double        maxAB{ 0 }, maxAC{ 0 }, maxRotAB{ 0 }, maxRotAC{ 0 };
            int           nAB{ 0 }, nAC{ 0 };
            double        maxCamAC{ 0 };
            unsigned long tidA{ 0 }, tidB{ 0 }, tidC{ 0 };
            // arms camera vs world camera - location (Present)
            double        maxArmLoc{ 0 }, maxArmCamNode{ 0 }, maxOffLoc{ 0 };
            V             lastArmLoc, lastCamRel, lastArmNode, lastOff, lastLoc;
            int           nArm{ 0 };
        };

        struct Stats
        {
            std::uint64_t calls{ 0 }, active{ 0 }, overrides{ 0 }, restores{ 0 };
            const char*   gate{ "init" };
            double        costUs{ 0 }, costAvgUs{ 0 }, costMaxUs{ 0 };
            std::uint64_t costN{ 0 };
            std::string   weapon{ "-" };
            const char*   jmode{ "-" };
            std::string   anchor{ "-" };
            bool          anchorNode{ false };
            V             tgt, act, actWorld, before;   // player-relative target / actual after update; actual world; before
            double        posErr{ 0 }, angErr{ 0 }, shift{ 0 }, rot{ 0 };
            V             fireRel, fireWorld, fireFwd;
            bool          fireValid{ false };
            V             camHook;                     // camera root world at the hook
            V             loc;
            int           weaponState{ -1 };
            V             fwdInGrip, aimInGrip, upInGrip;   // calibration aids: fire forward / aim / fire up in the grip frame
            std::uint64_t comRewritten{ 0 }, comPersisted{ 0 };
            int           boneCount{ 0 };
            std::uint64_t snapSeq{ 0 };
            double        snapAgeMs{ 0 };
            // P/D from AimFromWeapon (Present)
            std::uint64_t aimCalls{ 0 }, aimUsed{ 0 };
            V             P, D;
            double        pFireDist{ -1 }, dFireDeg{ -1 };
            V             fireAtPresent;
            // ---- stage 2 ----
            const char*   ik{ "-" };                  // "on", "off", reason
            std::uint64_t ikFrames{ 0 };
            double        fkErr{ 0 };                 // own forward kinematics vs the engine's worlds before any write (max over bones)
            V             body;                       // COM world after placement (player-relative)
            double        bodyYawDeg{ 0 }, rootYawDeg{ 0 };
            V             neck, shoulderMid;
            struct Arm
            {
                bool   solved{ false };
                V      tgt, hand, elbow, shoulder, clav;
                double err{ 0 }, angErr{ 0 }, stretch{ 1 }, reach{ 0 }, dist{ 0 }, upperLen{ 0 }, foreLen{ 0 }, elbowBelow{ 0 };
                double twistDeg{ 0 };
            } arm[2];                                 // 0 right (gun), 1 left
            const char*   offMode{ "-" };            // "free", "2h", "anim"
            double        gripDist{ -1 };            // off controller -> support grip point (units)
            double        barrelLR{ -1 };            // two-handed: angle between the barrel and (L - R) (deg)
            V             offRelT;                   // animated support hand in the WEAPON frame
            bool          offRelValid{ false };
            std::uint64_t twoHandedEnters{ 0 };
            // recoil
            std::uint64_t shots{ 0 };
            int           ammo{ -1 };
            double        recT{ 0 }, recR{ 0 }, recMaxT{ 0 }, recMaxR{ 0 };
            double        kickT{ 0 }, kickR{ 0 }, kickMaxT{ 0 }, kickMaxR{ 0 };  // rendered WEAPON vs the recoil-free target
            bool          recWindow{ false };
            // fingers (off hand)
            float         flex[5]{ 1, 1, 1, 1, 1 };
            double        fingerDeg{ 0 };            // LArm_Finger22 (index) local vs its open rotation (deg), from the tree after the update
            double        fingerDeg2{ 0 };           // LArm_Finger32 (middle)
            V             gripRef, offCtlRef, offCtlRig, gunHandRef;
            int           fingersWritten{ 0 };
            // pip-boy bone
            bool          pipValid{ false };
            V             pipGrip;                   // PipboyBone in the off grip frame (OpenXR axes, m)
            V             pipRef;                    // PipboyBone in the XR reference space at the hook (m)
            V             pipRig;
            double        ikCostUs{ 0 }, ikCostAvgUs{ 0 }, ikCostMaxUs{ 0 };
            std::uint64_t ikPersisted{ 0 }, ikRewritten{ 0 };  // our arm/finger locals found unchanged / rewritten by the graph next frame
        };

        std::mutex       g_lock;
        Cfg              g_cfg;
        detail::Snap     g_snap;
        Stats            g_st;
        Probe            g_probe;
        std::atomic<bool> g_on{ false };
        bool             g_notifyHooked = false, g_swapHooked = false, g_fnOk = false, g_paOk = false;
        std::unordered_map<std::string, Xf> g_json;   // lower-case weapon name (+ "-powerarmor" ...) -> J
        std::string      g_dumpReq;  // "" none, "pending", else result
        std::atomic<bool> g_dumpPending{ false };

        // per-frame state shared hook -> Present (guarded by g_lock)
        bool   g_lastActive = false;
        std::chrono::steady_clock::time_point g_lastActiveT{};
        Xf     g_F;            // WEAPON -> fire node (WEAPON-local)
        bool   g_FValid = false;
        Xf     g_J;            // J in use
        int    g_jKind = 0;    // 0 none, 1 json, 2 bind, 3 auto
        double g_wScale = 1.0;
        Xf     g_bindJ;        // measured skeleton bind of WEAPON (first frames) or constant
        // stage 2, published by the hook for AimFromWeapon / PipboyBoneOffset (guarded by g_lock)
        bool   g_ikLast = false;       // the last hook call ran the arm IK
        bool   g_twoHandedOn = false;  // off hand snapped to the support grip
        Xf     g_recoilD;              // current kick delta (WEAPON frame), identity outside a shot window
        Xf     g_offRel;               // animated support hand in the WEAPON frame
        bool   g_offRelValid = false;
        Xf     g_Janim;                // animated WEAPON in RArm_Hand
        bool   g_JanimValid = false;
        float  g_pipGrip[3]{};         // PipboyBone in the off grip frame (OpenXR axes, m), incl. lift
        bool   g_pipValid = false;

        // COM override bookkeeping (hook thread only)
        bool            g_wrote = false;
        RE::NiTransform g_written{};
        RE::NiTransform g_saved{};    // animated local before our first write (used when the graph does not rewrite it)
        bool            g_haveSaved = false;
        RE::NiNode*     g_writtenNode = nullptr;
        int             g_writtenIdx = -1;
        RE::BSFlattenedBoneTree* g_writtenTree = nullptr;

        using Notify_t = int (*)(void*, void*);
        using Swap_t = void (*)(void*);
        using UpdateNode_t = void (*)(float, RE::NiAVObject*, void*, bool);
        using QCam_t = void* (*)();
        using ActorBool_t = bool (*)(RE::Actor*);
        Notify_t g_origNotify = nullptr;
        Swap_t   g_origSwap = nullptr;

        double QpcUs()
        {
            static const double f = [] { LARGE_INTEGER q; ::QueryPerformanceFrequency(&q); return static_cast<double>(q.QuadPart); }();
            LARGE_INTEGER c;
            ::QueryPerformanceCounter(&c);
            return static_cast<double>(c.QuadPart) * 1.0e6 / f;
        }

        std::string Lower(std::string s)
        {
            for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        // ---- FRIK weapon offset JSONs (read-only copies in Data/F4SE/Plugins/fo4flatvr_weapons) ---------------
        bool ParseNumberAfter(const std::string& s, std::size_t from, const char* key, double& out)
        {
            const auto k = s.find(std::string("\"") + key + "\"", from);
            if (k == std::string::npos) return false;
            const auto c = s.find(':', k);
            if (c == std::string::npos) return false;
            char* end = nullptr;
            out = std::strtod(s.c_str() + c + 1, &end);
            return end != s.c_str() + c + 1;
        }

        int LoadJsons(const std::string& a_dir)
        {
            int n = 0;
            std::error_code ec;
            if (!std::filesystem::is_directory(a_dir, ec)) {
                REX::WARN("hands: weapon offsets dir '{}' not found (auto-align / bind only)", a_dir);
                return 0;
            }
            for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
                if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".json") continue;
                std::ifstream f(e.path(), std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string s = ss.str();
                const auto q0 = s.find('"');
                const auto q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
                const auto rot = s.find("\"rotation\"");
                const auto br = rot == std::string::npos ? rot : s.find('[', rot);
                if (q1 == std::string::npos || br == std::string::npos) continue;
                const std::string name = s.substr(q0 + 1, q1 - q0 - 1);
                double v12[12]{};
                const char* p = s.c_str() + br + 1;
                bool ok = true;
                for (int i = 0; i < 12 && ok; ++i) {
                    char* end = nullptr;
                    v12[i] = std::strtod(p, &end);
                    ok = end != p;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
                }
                double x, y, z, sc = 1.0;
                if (!ok || !ParseNumberAfter(s, br, "x", x) || !ParseNumberAfter(s, br, "y", y) || !ParseNumberAfter(s, br, "z", z)) {
                    REX::WARN("hands: could not parse {}", e.path().string());
                    continue;
                }
                ParseNumberAfter(s, br, "scale", sc);
                const double rows[9] = { v12[0], v12[1], v12[2], v12[4], v12[5], v12[6], v12[8], v12[9], v12[10] };
                Xf j = FromRows(rows, { x, y, z });
                j.s = 1.0;  // rigid: FRIK's VR scale is not applied (the WEAPON local is not written)
                g_json[Lower(name)] = j;
                ++n;
            }
            return n;
        }

        // ---- game access -------------------------------------------------------------------------------------
        RE::BSFlattenedBoneTree* FindTree(RE::NiAVObject* a_obj, int a_depth)
        {
            if (!a_obj) return nullptr;
            if (const auto* rtti = a_obj->GetRTTI(); rtti && rtti->GetName() && std::strcmp(rtti->GetName(), "BSFlattenedBoneTree") == 0) {
                return static_cast<RE::BSFlattenedBoneTree*>(a_obj);
            }
            if (a_depth <= 0) return nullptr;
            if (auto* n = a_obj->IsNode()) {
                for (auto& c : n->children) {
                    if (auto* t = FindTree(c.get(), a_depth - 1)) return t;
                }
            }
            return nullptr;
        }

        RE::NiAVObject* FindByName(RE::NiAVObject* a_obj, const char* a_name, int a_depth)
        {
            if (!a_obj) return nullptr;
            if (const char* nm = a_obj->name.c_str(); nm && _stricmp(nm, a_name) == 0) return a_obj;
            if (a_depth <= 0) return nullptr;
            if (auto* n = a_obj->IsNode()) {
                for (auto& c : n->children) {
                    if (auto* r = FindByName(c.get(), a_name, a_depth - 1)) return r;
                }
            }
            return nullptr;
        }

        int BoneIndex(RE::BSFlattenedBoneTree* a_t, const char* a_name)
        {
            for (int i = 0; i < a_t->boneCount; ++i) {
                const char* nm = a_t->bone[i].name.c_str();
                if (nm && _stricmp(nm, a_name) == 0) return i;
            }
            return -1;
        }

        // ---- stage 2 bone sets -------------------------------------------------------------------------------------
        struct ArmIdx
        {
            int  clav{ -1 }, upper{ -1 }, f1{ -1 }, f2{ -1 }, f3{ -1 }, hand{ -1 };
            bool Ok() const { return clav >= 0 && upper >= 0 && f1 >= 0 && f2 >= 0 && f3 >= 0 && hand >= 0; }
        };

        // Finger poses: FRIK HandPose open/closed tables (flex 0 = closed, 1 = open). All finger bones are bone-only.
#include "plugin/HandsFingerData.inl"
        struct FingerSet
        {
            int  idx[15]{};
            M3   open[15], closed[15];
            int  finger[15]{};  // 0 thumb .. 4 pinky
            bool ok{ false };
        };
        void BuildFingers(RE::BSFlattenedBoneTree* a_t, const char a_side, FingerSet& a_out)
        {
            int k = 0;
            for (const auto& d : kFingerData) {
                if (d.name[0] != a_side || k >= 15) continue;
                double o[9], cl[9];
                for (int i = 0; i < 9; ++i) o[i] = d.open[i], cl[i] = d.closed[i];
                a_out.idx[k] = BoneIndex(a_t, d.name);
                a_out.open[k] = FromRows(o, {}).R;
                a_out.closed[k] = FromRows(cl, {}).R;
                const std::size_t len = std::strlen(d.name);
                a_out.finger[k] = d.name[len - 2] - '1';
                if (a_out.idx[k] < 0) return;
                ++k;
            }
            a_out.ok = k == 15;
        }

        struct Rig
        {
            RE::PlayerCharacter*     player{ nullptr };
            RE::NiNode*              root{ nullptr };
            RE::BSFlattenedBoneTree* tree{ nullptr };
            int                      iCom{ -1 }, iWeapon{ -1 }, iHand{ -1 }, iCamera{ -1 };
            RE::NiNode*              weapon{ nullptr };
            RE::NiAVObject*          fire{ nullptr };
            // stage 2
            int                      iChest{ -1 }, iLHand{ -1 }, iPip{ -1 };
            ArmIdx                   arm[2];   // 0 right, 1 left
            const FingerSet*         fingersL{ nullptr };
        };

        bool GetRig(Rig& r, const char*& a_why)
        {
            r.player = RE::PlayerCharacter::GetSingleton();
            if (!r.player) { a_why = "no player"; return false; }
            r.root = r.player->firstPerson3D.get();
            if (!r.root) { a_why = "no firstPerson3D"; return false; }
            r.tree = FindTree(r.root, 3);
            if (!r.tree || !r.tree->bone || r.tree->boneCount <= 0) { a_why = "no flattened bone tree"; return false; }
            static RE::BSFlattenedBoneTree* cachedTree = nullptr;
            static int cachedCount = -1, cCom = -1, cWeapon = -1, cHand = -1, cCamera = -1, cChest = -1, cLHand = -1, cPip = -1;
            static ArmIdx    cArm[2];
            static FingerSet cFingersL;
            if (cachedTree != r.tree || cachedCount != r.tree->boneCount) {
                cachedTree = r.tree;
                cachedCount = r.tree->boneCount;
                cCom = BoneIndex(r.tree, "COM");
                cWeapon = BoneIndex(r.tree, "WEAPON");
                cHand = BoneIndex(r.tree, "RArm_Hand");
                cCamera = BoneIndex(r.tree, "Camera");
                cChest = BoneIndex(r.tree, "Chest");
                cLHand = BoneIndex(r.tree, "LArm_Hand");
                cPip = BoneIndex(r.tree, "PipboyBone");
                for (int s = 0; s < 2; ++s) {
                    const std::string p = s == 0 ? "RArm_" : "LArm_";
                    cArm[s].clav = BoneIndex(r.tree, (p + "Collarbone").c_str());
                    cArm[s].upper = BoneIndex(r.tree, (p + "UpperArm").c_str());
                    cArm[s].f1 = BoneIndex(r.tree, (p + "ForeArm1").c_str());
                    cArm[s].f2 = BoneIndex(r.tree, (p + "ForeArm2").c_str());
                    cArm[s].f3 = BoneIndex(r.tree, (p + "ForeArm3").c_str());
                    cArm[s].hand = BoneIndex(r.tree, (p + "Hand").c_str());
                }
                cFingersL = FingerSet{};
                BuildFingers(r.tree, 'L', cFingersL);
            }
            r.iCom = cCom, r.iWeapon = cWeapon, r.iHand = cHand, r.iCamera = cCamera;
            r.iChest = cChest, r.iLHand = cLHand, r.iPip = cPip;
            r.arm[0] = cArm[0], r.arm[1] = cArm[1];
            r.fingersL = cFingersL.ok ? &cFingersL : nullptr;
            if (r.iWeapon < 0 || r.iCom < 0) { a_why = "WEAPON/COM bone missing"; return false; }
            r.weapon = r.tree->bone[r.iWeapon].node.get();
            if (!r.weapon) { a_why = "WEAPON has no node"; return false; }
            r.fire = FindByName(r.weapon, "ProjectileNode", 10);
            return true;
        }

        // ActorState (Actor+0x128) +0xC bits 1..3 = WEAPON_STATE (3 = drawn), read raw (the CommonLib bitfield
        // accessor returned false with the weapon drawn).
        int WeaponState(const RE::PlayerCharacter* a_p)
        {
            return static_cast<int>((*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(a_p) + 0x134) >> 1) & 7);
        }

        bool VatsActive()
        {
            const auto vats = *reinterpret_cast<std::uint8_t**>(Base() + kVatsSingleton);
            return vats && *reinterpret_cast<const std::uint32_t*>(vats + 0x40) != 0;
        }

        int CamState()
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            return cam && cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1;
        }

        bool CamRootWorld(Xf& a_out)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* root = cam ? cam->cameraRoot.get() : nullptr;
            if (!root) return false;
            a_out = FromNi(root->world);
            a_out.s = 1.0;
            return true;
        }

        const RE::TESForm* EquippedWeapon(RE::PlayerCharacter* a_p)
        {
            auto* mh = a_p->currentProcess ? a_p->currentProcess->middleHigh : nullptr;
            if (!mh) return nullptr;
            for (auto& it : mh->equippedItems) {
                if (it.item.object && it.item.object->IsWeapon()) return it.item.object;
            }
            return nullptr;
        }

        // Rounds in the equipped weapon (EquippedWeaponData +0x18 ammoCount), -1 if unknown. A decrease = a shot.
        int EquippedAmmo(RE::PlayerCharacter* a_p)
        {
            auto* mh = a_p->currentProcess ? a_p->currentProcess->middleHigh : nullptr;
            if (!mh) return -1;
            for (auto& it : mh->equippedItems) {
                if (it.item.object && it.item.object->IsWeapon() && it.data) {
                    return static_cast<int>(static_cast<const RE::EquippedWeaponData*>(it.data.get())->ammoCount);
                }
            }
            return -1;
        }

        Xf CalibC(const Cfg& c)
        {
            constexpr double d2r = 3.14159265358979 / 180.0;
            Xf x;
            x.R = Mul(Rz(c.rotDeg[2] * d2r), Mul(Ry(c.rotDeg[1] * d2r), Rx(c.rotDeg[0] * d2r)));
            x.t = V{ c.offCm[0], c.offCm[1], c.offCm[2] } * (0.01 * c.unitsPerMeter);
            return x;
        }

        Xf HandXf(const detail::HandRel& h, double upm)
        {
            Xf x;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    x.R.m[r][c] = h.rot[r][c];
            x.R = Ortho(x.R);
            x.t = V{ h.pos[0], h.pos[1], h.pos[2] } * upm;
            return x;
        }

        // Target WEAPON world from the camera root world (any frame: world or player-relative) and the hand
        // relative to the head the camera was built from. a_kind: 1 json, 2 bind, 3 auto-align.
        Xf TargetWeapon(const Xf& a_cam, const detail::HandRel& a_h, const Cfg& a_c, const Xf& a_J, int a_kind, const Xf& a_F, bool a_FValid,
            double a_scale, Xf* a_grip = nullptr)
        {
            const Xf H = Mul(a_cam, HandXf(a_h, a_c.unitsPerMeter));
            if (a_grip) *a_grip = H;
            const Xf wand = Mul(H, CalibC(a_c));
            Xf       off;
            off.t = kWandToOffset;
            static const Xf G = FromRows(kGRows, kGT);
            static const Xf Ginv = Inv(G);
            const Xf hand = Mul(Mul(wand, off), Ginv);
            Xf       w = Mul(hand, a_J);
            if (a_kind == 3 && a_FValid) {
                // barrel (fire node local +Y) along the controller aim, fire-node up closest to the controller up
                const V d = Norm(Mul(a_cam.R, V{ a_h.aimDir[0], a_h.aimDir[1], a_h.aimDir[2] }));
                const V u = wand.R.Col(2);
                const V fF = Norm(a_F.R.Col(1)), fU = a_F.R.Col(2);
                auto basis = [](const V& f, const V& up) {
                    const V z = Norm(up - f * Dot(up, f));
                    M3      b;
                    b.SetCol(0, Cross(f, z)), b.SetCol(1, f), b.SetCol(2, z);
                    return b;
                };
                w.R = Mul(basis(d, u), T(basis(fF, fU)));
            }
            w.s = a_scale;
            return w;
        }

        // ---- stage 2: hand targets ------------------------------------------------------------------------
        // Grip frame -> RArm_Hand of an empty right hand at the controller: C . T_wto . G^-1 (the stage-1 chain without
        // J). The off (left) hand is its mirror image: world mirrored about X, the two OpenXR grip frames mirrored about
        // grip X (both +X point right in a mirrored pose), left arm bones mirrored about their local Z (WeaponLeft t =
        // WEAPON t with z negated, skeleton.nif).
        Xf KRight(const Cfg& a_c)
        {
            Xf off;
            off.t = kWandToOffset;
            static const Xf Ginv = Inv(FromRows(kGRows, kGT));
            return Mul(Mul(CalibC(a_c), off), Ginv);
        }
        Xf KLeft(const Cfg& a_c)
        {
            const Xf k = KRight(a_c);
            const M3 sg = Diag(-1, 1, 1), sb = Diag(1, 1, -1);
            Xf       r;
            r.R = Mul(Mul(sg, k.R), sb);
            r.t = Mul(sg, k.t);
            return r;
        }

        struct GunState
        {
            Xf     J;
            int    kind{ 2 };
            Xf     F;
            bool   FValid{ false };
            double scale{ 1.0 };
            Xf     recoil;              // kick delta in the WEAPON frame (identity = none)
            bool   twoHanded{ false };  // off hand snapped: barrel along (off controller - gun hand)
            Xf     offRel;              // animated support hand in the WEAPON frame
            bool   offRelValid{ false };
            Xf     Janim;               // animated WEAPON in RArm_Hand
            bool   JanimValid{ false };
        };
        GunState g_gun;  // the last hook's gun inputs, for AimFromWeapon (guarded by g_lock)

        // WEAPON* and the gun-hand target that carries it; optional: off-controller hand target, support-hand target,
        // barrel vs (L - R) angle. Used by the hook (new head) and by AimFromWeapon (old head) with the same inputs.
        Xf GunPose(const Xf& a_cam, const detail::HandRel& a_r, const detail::HandRel& a_l, const Cfg& a_c, const GunState& a_g, Xf* a_hand,
            Xf* a_offCtl, Xf* a_offGrip, double* a_barrelLR)
        {
            Xf w = TargetWeapon(a_cam, a_r, a_c, a_g.J, a_g.kind, a_g.F, a_g.FValid, a_g.scale);
            if (a_c.keepRecoil) {
                const double s = w.s;
                w = Mul(w, a_g.recoil);
                w.s = s;
            }
            Xf hand = Mul(w, Inv(a_g.JanimValid ? a_g.Janim : a_g.J));
            Xf offCtl;
            if (a_l.valid) offCtl = Mul(Mul(a_cam, HandXf(a_l, a_c.unitsPerMeter)), KLeft(a_c));
            if (a_offCtl) *a_offCtl = offCtl;
            if (a_g.twoHanded && a_l.valid && a_g.offRelValid) {
                const V pivot = hand.t;
                const V d = Norm(offCtl.t - pivot);
                auto    barrel = [&](const Xf& ww) { return a_g.FValid ? Norm(Mul(ww, a_g.F).R.Col(1)) : Norm(Mul(ww, a_g.offRel).t - pivot); };
                const M3 q = RotFromTo(barrel(w), d);
                w.R = Mul(q, w.R);
                w.t = pivot + Mul(q, w.t - pivot);
                hand.R = Mul(q, hand.R);
                if (a_barrelLR) *a_barrelLR = std::acos(std::clamp(Dot(barrel(w), d), -1.0, 1.0)) * kR2D;
            }
            if (a_offGrip && a_g.offRelValid) *a_offGrip = Mul(w, a_g.offRel);
            if (a_hand) *a_hand = hand;
            return w;
        }

        // ---- stage 2: own copy of the flattened tree's pose ----------------------------------------------------
        // local/world of every bone (node local for bones with a NiNode, FlattenedBone.local otherwise); setting a local
        // recomputes the worlds below it (the array is parent-first). WriteBack puts the changed locals into the tree
        // entry and the NiNode; the engine's own update then recomputes everything (skin, weapon, fire node, bounds).
        struct PoseCache
        {
            RE::BSFlattenedBoneTree*  tree{ nullptr };
            int                       n{ 0 };
            std::vector<Xf>           local, world;
            std::vector<int>          parent;
            std::vector<std::uint8_t> written, mark;
            double                    fkErr{ 0 };
            Xf                        rootWorld;  // the BSFlattenedBoneTree node's world: parent of top-level bones (COM, Camera)
            const Xf& ParentWorld(int a_i) const { return parent[a_i] >= 0 ? world[parent[a_i]] : rootWorld; }

            void Load(RE::BSFlattenedBoneTree* a_t)
            {
                tree = a_t;
                n = a_t->boneCount;
                local.resize(n), world.resize(n), parent.resize(n), written.assign(n, 0), mark.assign(n, 0);
                for (int i = 0; i < n; ++i) {
                    const auto& b = a_t->bone[i];
                    local[i] = FromNi(b.node ? b.node->local : b.local);
                    world[i] = FromNi(b.world);
                    parent[i] = b.parent;
                }
                rootWorld = FromNi(a_t->world);
                fkErr = 0;
                for (int i = 0; i < n; ++i) {
                    if (parent[i] < i) fkErr = (std::max)(fkErr, Len(Mul(ParentWorld(i), local[i]).t - world[i].t));
                }
            }
            void Recompute(int a_i)
            {
                world[a_i] = Mul(ParentWorld(a_i), local[a_i]);
                std::fill(mark.begin(), mark.end(), std::uint8_t{ 0 });
                mark[a_i] = 1;
                for (int j = a_i + 1; j < n; ++j) {
                    const int p = parent[j];
                    if (p >= 0 && mark[p]) {
                        world[j] = Mul(world[p], local[j]);
                        mark[j] = 1;
                    }
                }
            }
            void SetLocal(int a_i, const Xf& a_l)
            {
                local[a_i] = a_l;
                written[a_i] = 1;
                Recompute(a_i);
            }
            void SetWorld(int a_i, const Xf& a_w)
            {
                Xf l = Mul(Inv(ParentWorld(a_i)), a_w);
                l.s = local[a_i].s;
                SetLocal(a_i, l);
            }
            int WriteBack()
            {
                int k = 0;
                for (int i = 0; i < n; ++i) {
                    if (!written[i]) continue;
                    auto&           b = tree->bone[i];
                    RE::NiTransform t = b.node ? b.node->local : b.local;
                    const float     s = t.scale;
                    ToNi(local[i], t);
                    t.scale = s;
                    b.local = t;
                    if (b.node) b.node->local = t;
                    ++k;
                }
                return k;
            }
        };

        // ---- stage 2: arm IK, FRIK Skeleton::solveArmToHandWorldTarget (VRIK-derived) ported to column-vector math --
        // NiMatrix3 storage S = R^T: S*v = R^T v, S^T*v = R v, storage product A*B = (B A)^T, getMatrixFromRotateVectorVec
        // (to, from) = RotFromTo(from, to), getMatrixFromEulerAngles(h,0,0) left-multiplied = local . Rx(-h).
        struct ArmSmooth
        {
            double twistPrev{ 0 };
            bool   init{ false };
        };
        struct BodyFrame
        {
            V      fwd, right;
            double chestZ{ 0 };
        };
        double SmoothMax(double a, double b, double k)
        {
            const double h = (std::max)(k - std::abs(a - b), 0.0) / k;
            return (std::max)(a, b) + h * h * k * 0.25;
        }
        double SmoothMin(double a, double b, double k) { return -SmoothMax(-a, -b, k); }
        V      RotXY(const V& v, double a) { return { v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a), v.z }; }
        bool   Finite(const V& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        bool SolveArm(PoseCache& P, const ArmIdx& a, bool isLeft, const Xf& a_target, double a_maxStretch, const Cfg& c, const BodyFrame& bf, double dt,
            ArmSmooth& sm, Stats::Arm& out)
        {
            constexpr double d2r = 3.14159265358979 / 180.0, pi = 3.14159265358979;
            V                handPos = a_target.t;
            const M3         handRot = a_target.R;
            const Xf         U0 = P.world[a.upper];
            if (!Finite(handPos) || Len(U0.t - handPos) > 200.0) return false;
            const double adj = c.armLength / 36.74, armLength = c.armLength;
            const double sU = U0.s;
            const double upperLen0 = Len(P.local[a.f1].t) * sU;
            const double foreLen0 = (Len(P.local[a.hand].t) + Len(P.local[a.f2].t) + Len(P.local[a.f3].t)) * sU;
            double       upperLen = upperLen0 * adj, forearmLen = foreLen0 * adj;
            const V      shoulderToHand = handPos - U0.t;
            if (Len(shoulderToHand) > (upperLen + forearmLen) * 2.25) return false;

            // shoulder (clavicle swings toward the hand)
            const double adjustAmount = std::clamp(Len(shoulderToHand) - armLength * 0.5, 0.0, armLength * 0.85) / (armLength * 0.85);
            V            shoulderOffset = Norm(shoulderToHand) * (adjustAmount * armLength * c.shoulderReach);
            if (shoulderOffset.z < 0.0) shoulderOffset.z *= c.shoulderDownDamp;
            {
                const Xf S = P.world[a.clav];
                const V  sLocalDir = Mul(T(S.R), U0.t + shoulderOffset - S.t) * (1.0 / S.s);
                Xf       l = P.local[a.clav];
                l.R = Mul(l.R, RotFromTo({ 1, 0, 0 }, sLocalDir));
                P.SetLocal(a.clav, l);
            }
            const Xf     Sw = P.world[a.clav];
            const M3     preAimUpperWorldRot = P.world[a.upper].R;
            const double origEHLen = Len(P.world[a.hand].t - P.world[a.f1].t);
            const double negLeft = isLeft ? -1.0 : 1.0;
            const V      Uwp = P.world[a.upper].t;
            V            handToShoulder = Uwp - handPos;
            double       hsLen = (std::max)(Len(handToShoulder), 0.1);

            // reach: with a flexion floor the bones stretch so the hand is reached with the elbow still bent that much
            double       stretch = 1.0;
            const double minFlexion = c.minFlexDeg * d2r;
            if (minFlexion > 0.0) {
                const double reachAtMin = std::sqrt(upperLen * upperLen + forearmLen * forearmLen + 2.0 * upperLen * forearmLen * std::cos(minFlexion));
                out.reach = reachAtMin;
                if (hsLen > reachAtMin) {
                    stretch = hsLen / reachAtMin;
                    if (a_maxStretch > 0.0 && stretch > a_maxStretch) {  // the hand stops short (off hand only)
                        stretch = a_maxStretch;
                        hsLen = reachAtMin * a_maxStretch;
                        handPos = Uwp - Norm(handToShoulder) * hsLen;
                        handToShoulder = Uwp - handPos;
                    }
                    upperLen *= stretch;
                    forearmLen *= stretch;
                }
            } else if (hsLen > upperLen + forearmLen) {
                const double diff = hsLen - upperLen - forearmLen, ratio = forearmLen / (forearmLen + upperLen);
                forearmLen += ratio * diff + 0.1;
                upperLen += (1.0 - ratio) * diff + 0.1;
            }
            out.stretch = stretch;
            double       triangleLen = hsLen;
            const double maxFlexion = c.maxFlexDeg * d2r;
            if (maxFlexion > 0.0) {
                const double reachAtMax =
                    std::sqrt((std::max)(upperLen * upperLen + forearmLen * forearmLen + 2.0 * upperLen * forearmLen * std::cos(maxFlexion), 0.0));
                triangleLen = (std::max)(hsLen, reachAtMax);
            }

            // elbow direction (FRIK/VRIK heuristics, all in the body yaw frame, Z up)
            const V forwardDir = Norm(bf.fwd);
            const V sidewaysDir = Norm(bf.right * negLeft);
            const V handBack = handRot.Col(0) * -1.0;
            double  twistAngle = std::asin(std::clamp(handBack.z, -0.999, 0.999));
            const V handSide = handRot.Col(1) * -1.0;
            const V handInSide = handSide * negLeft;
            const double twistAngle2 = -std::asin(std::clamp(handSide.z, -0.599, 0.999));
            const double interpTwist = std::clamp((handBack.z + 0.866) * 1.155, 0.45, 0.8);
            twistAngle = twistAngle + interpTwist * (twistAngle2 - twistAngle);
            const double moveFrac = 1.0 - std::pow(0.75, std::clamp(dt, 0.0, 3.0 / 90.0) * 90.0);
            if (!sm.init) sm.twistPrev = twistAngle, sm.init = true;
            twistAngle = sm.twistPrev + (twistAngle - sm.twistPrev) * moveFrac;
            sm.twistPrev = twistAngle;
            out.twistDeg = twistAngle / d2r;

            const V      Sp = Sw.t;
            const double behindD = -(forwardDir.x * Sp.x + forwardDir.y * Sp.y) - 10.0;
            const double handBehindDist = -(handPos.x * forwardDir.x + handPos.y * forwardDir.y + behindD);
            const double behindAmount = std::clamp(handBehindDist / 40.0, 0.0, 1.0);
            const V      planeDir = RotXY(forwardDir, negLeft * 135.0 * d2r);
            const double planeD = -(planeDir.x * Sp.x + planeDir.y * Sp.y) + 16.0;
            const double armCrossAmount = std::clamp((handPos.x * planeDir.x + handPos.y * planeDir.y + planeD) / 20.0, 0.0, 1.0);
            const double armLiftLimit = std::clamp((bf.chestZ + 60.0 - handPos.z) / 60.0, 0.0, 1.0);
            const double upLimit = std::clamp((1.0 - armLiftLimit) * 1.4, 0.0, 1.0);
            constexpr double kAmountBand = 0.1;
            const double     kAngleBand = 9.0 * d2r;
            const double adjustMinAmount = SmoothMax(behindAmount, SmoothMin(armCrossAmount, armLiftLimit, kAmountBand), kAmountBand);
            const double twistMinAngle = -85.0 * d2r + 50.0 * d2r * adjustMinAmount;
            double twistMaxAngle = 55.0 * d2r - SmoothMax(90.0 * d2r * armCrossAmount, 70.0 * d2r * upLimit, kAngleBand);
            twistMaxAngle = (std::max)(twistMaxAngle, twistMinAngle + c.twistWindowDeg * d2r);
            const double twistLimitAngle = twistMinAngle + (twistAngle + pi / 2.0) / pi * (twistMaxAngle - twistMinAngle);
            const V      bendDownDir = Mul(AxisAngle(sidewaysDir * negLeft, twistLimitAngle), forwardDir);
            const V      xDir = Norm(handToShoulder);
            const double sideD = -(sidewaysDir.x * Sp.x + sidewaysDir.y * Sp.y) - 8.0;
            double       acrossAmount = -(handPos.x * sidewaysDir.x + handPos.y * sidewaysDir.y + sideD) / 16.0;
            const double handSideTwistOutward = Dot(handSide, Norm(sidewaysDir + forwardDir * 0.5));
            const double armTwist = std::clamp(handSideTwistOutward - (std::max)(0.0, acrossAmount + 0.25), 0.0, 1.0);
            if (acrossAmount < 0) acrossAmount *= 0.2;
            const double handBehindHead = std::clamp(handBehindDist / 15.0, 0.0, 1.0) * std::clamp(upLimit * 1.2, 0.0, 1.0);
            const double elbowsTwistForward = SmoothMax(acrossAmount * 90.0 * d2r, handBehindHead * 120.0 * d2r, kAngleBand);
            const V      elbowDir = RotXY(bendDownDir, -negLeft * (150.0 * d2r - armTwist * 25.0 * d2r - elbowsTwistForward));
            const V      yDir = Norm(elbowDir - xDir * Dot(elbowDir, xDir));

            double wristAngle = std::acos((forearmLen * forearmLen + triangleLen * triangleLen - upperLen * upperLen) / (2 * forearmLen * triangleLen));
            if (!std::isfinite(wristAngle)) {
                forearmLen = upperLen = (upperLen0 + foreLen0) / 2.0 * adj;
                wristAngle = std::acos((forearmLen * forearmLen + triangleLen * triangleLen - upperLen * upperLen) / (2 * forearmLen * triangleLen));
                if (!std::isfinite(wristAngle)) return false;
            }
            const V elbowWorld = handPos + xDir * (std::cos(wristAngle) * forearmLen) + yDir * (std::sin(wristAngle) * forearmLen);
            if (!Finite(elbowWorld)) return false;

            // upper arm: aim at the elbow, then roll so the forearm lies in the upper arm's bend plane
            Xf upper = P.local[a.upper];
            {
                const Xf U = P.world[a.upper];
                const V  uLocalDir = Mul(T(U.R), Norm(elbowWorld - Uwp)) * (1.0 / U.s);
                upper.R = Mul(upper.R, RotFromTo(P.local[a.f1].t, uLocalDir));
            }
            M3 Uwr = Mul(Sw.R, upper.R);
            V  uLocalTwist = Mul(T(Uwr), Norm(handPos - elbowWorld));
            uLocalTwist.x = 0;
            V uloc = Mul(T(Sw.R), preAimUpperWorldRot.Col(1));
            uloc.x = 0;
            const double upperCos = std::clamp(Dot(Norm(uLocalTwist), Norm(uloc)), -1.0, 1.0);
            const double upperAngle = std::acos(upperCos) * (uLocalTwist.z > 0 ? 1.0 : -1.0);
            upper.R = Mul(upper.R, Rx(upperAngle));
            Uwr = Mul(Sw.R, upper.R);
            Xf f1 = P.local[a.f1];
            f1.R = Mul(f1.R, Rx(upperAngle));

            // forearm: aim at the hand
            M3 Fwr = Mul(Uwr, f1.R);
            {
                const V fLocalDir = Mul(T(Fwr), Norm(handPos - elbowWorld));
                f1.R = Mul(f1.R, RotFromTo({ 1, 0, 0 }, fLocalDir));
                Fwr = Mul(Uwr, f1.R);
            }
            // forearm roll spread over ForeArm1..3 from the wrist's side vector
            Xf f2 = P.local[a.f2], f3 = P.local[a.f3];
            M3 Fwr2 = Mul(Fwr, f2.R), Fwr3 = Mul(Fwr2, f3.R);
            {
                V wLocalDir = Mul(T(Fwr3), Norm(handInSide));
                wLocalDir.x = 0;
                const V forearm3Side = Fwr3.Col(2) * -1.0;
                V       floc = Mul(T(Fwr2), Norm(forearm3Side));
                floc.x = 0;
                const V      wn = Norm(wLocalDir), fn = Norm(floc);
                const double fcos = Dot(wn, fn), fsin = Dot(wn, Cross(fn, V{ -1, 0, 0 }));
                const double forearmAngle = -1.0 * negLeft * std::atan2(fsin, fcos);
                if (c.forearmTwist[0] != 0.0f) {
                    f1.R = Mul(f1.R, Rx(-negLeft * forearmAngle * c.forearmTwist[0]));
                    Fwr = Mul(Uwr, f1.R);
                }
                f2.R = Mul(f2.R, Rx(-negLeft * forearmAngle * c.forearmTwist[1]));
                f3.R = Mul(f3.R, Rx(-negLeft * forearmAngle * c.forearmTwist[2]));
                Fwr2 = Mul(Fwr, f2.R);
                Fwr3 = Mul(Fwr2, f3.R);
            }
            Xf hand = P.local[a.hand];
            hand.R = Mul(T(Fwr3), handRot);
            // elbow position (upper arm length) and forearm length
            f1.t = Mul(T(Uwr), elbowWorld - Uwp) * (1.0 / P.world[a.upper].s);
            const double forearmRatio = origEHLen > 1e-6 ? forearmLen / origEHLen : 1.0;
            f2.t = f2.t * forearmRatio;
            f3.t = f3.t * forearmRatio;
            hand.t = hand.t * forearmRatio;
            P.SetLocal(a.upper, upper);
            P.SetLocal(a.f1, f1);
            P.SetLocal(a.f2, f2);
            P.SetLocal(a.f3, f3);
            P.SetLocal(a.hand, hand);

            out.solved = true;
            out.tgt = a_target.t;
            out.hand = P.world[a.hand].t;
            out.elbow = P.world[a.f1].t;
            out.shoulder = Uwp;
            out.clav = Sw.t;
            out.err = Len(out.hand - a_target.t);
            out.angErr = AngleDeg(P.world[a.hand].R, handRot);
            out.dist = Len(Uwp - a_target.t);
            out.upperLen = Len(out.elbow - Uwp);
            out.foreLen = Len(out.hand - out.elbow);
            const V xd = Norm(out.hand - Uwp);
            const V onLine = Uwp + xd * Dot(out.elbow - Uwp, xd);
            out.elbowBelow = onLine.z - out.elbow.z;  // > 0: elbow below the shoulder-hand line
            return true;
        }


        // ---- the frame (hook thread) ------------------------------------------------------------------------
        // a_anchor / a_tree: the current rig's anchor node and tree (null without a rig). The node we wrote is
        // touched only if it is still that one (a cell change or 3D reload may have freed it).
        void RestoreIfNeeded(bool a_update, RE::NiNode* a_anchor, RE::BSFlattenedBoneTree* a_tree)
        {
            if (!g_wrote || !g_writtenNode || g_writtenNode != a_anchor || g_writtenTree != a_tree) {
                g_wrote = false;
                g_haveSaved = false;
                return;
            }
            // only when the graph did not rewrite the local itself
            if (std::memcmp(&g_writtenNode->local, &g_written, sizeof(RE::NiTransform)) == 0 && g_haveSaved) {
                g_writtenNode->local = g_saved;
                if (g_writtenTree && g_writtenIdx >= 0 && g_writtenIdx < g_writtenTree->boneCount) {
                    g_writtenTree->bone[g_writtenIdx].local = g_saved;
                }
                if (a_update && g_fnOk) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (auto* root = player ? player->firstPerson3D.get() : nullptr) {
                        const float t = *reinterpret_cast<const float*>(Base() + kFrameTime);
                        reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(t, root, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
                    }
                }
                std::scoped_lock l(g_lock);
                ++g_st.restores;
            }
            g_wrote = false;
            g_haveSaved = false;
        }

        std::string DumpRig(const Rig& r)
        {
            std::string out;
            auto* t = r.tree;
            int   withNode = 0, parentFirst = 1;
            for (int i = 0; i < t->boneCount; ++i) {
                const auto& b = t->bone[i];
                if (b.node) ++withNode;
                if (b.parent >= i) parentFirst = 0;
                const char* nm = b.name.c_str();
                REX::INFO("hands dump: bone {:3} parent {:3} node {} '{}' local t {} world t {}", i, b.parent, b.node ? 1 : 0, nm ? nm : "",
                    F(NiV(b.local.translate)), F(NiV(b.world.translate)));
            }
            std::string rootKids;
            for (auto& c : r.root->children) {
                if (c) {
                    const auto* rt = c->GetRTTI();
                    rootKids += std::format("{}'{}':{} ", rootKids.empty() ? "" : "", c->name.c_str() ? c->name.c_str() : "", rt ? rt->GetName() : "?");
                }
            }
            auto nodeOf = [&](const char* nm) {
                const int i = BoneIndex(t, nm);
                return i < 0 ? std::string("missing") : t->bone[i].node ? std::string("node") : std::string("bone-only");
            };
            std::string noNode;
            for (int i = 0; i < t->boneCount; ++i) {
                if (!t->bone[i].node && t->bone[i].name.c_str()) noNode += std::format("{},", t->bone[i].name.c_str());
            }
            auto* get3d = r.player->Get3D(true);
            const Xf wl = FromNi(r.weapon->local);
            out = std::format("bones={} withNode={} parentFirst={} get3D(1)==firstPerson3D={} rootKids=[{}] tree='{}' COM={} WEAPON={} RArm_Hand={} "
                              "Camera={} RArm_UpperArm={} RArm_ForeArm1={} LArm_Hand={} Chest={} fire={} weaponLocal rows=({:.3f},{:.3f},{:.3f}|{:.3f},{:.3f},{:.3f}|{:.3f},{:.3f},{:.3f}) t={} s={:.3f} noNode=[{}]",
                t->boneCount, withNode, parentFirst, get3d == r.root ? 1 : 0, rootKids, t->name.c_str() ? t->name.c_str() : "", nodeOf("COM"),
                nodeOf("WEAPON"), nodeOf("RArm_Hand"), nodeOf("Camera"), nodeOf("RArm_UpperArm"), nodeOf("RArm_ForeArm1"), nodeOf("LArm_Hand"),
                nodeOf("Chest"), r.fire ? r.fire->name.c_str() : "none", wl.R.m[0][0], wl.R.m[1][0], wl.R.m[2][0], wl.R.m[0][1], wl.R.m[1][1],
                wl.R.m[2][1], wl.R.m[0][2], wl.R.m[1][2], wl.R.m[2][2], F(wl.t, 3), wl.s, noNode);
            if (r.weapon) {
                // weapon model subtree names (first levels)
                std::string kids;
                for (auto& c : r.weapon->children) {
                    if (c) kids += std::format("'{}' ", c->name.c_str() ? c->name.c_str() : "");
                }
                out += " weaponKids=[" + kids + "]";
            }
            // stage 2: left vs right arm locals (mirror check: L = Sb R Sb with Sb = diag(1,1,-1)?)
            std::string mir;
            for (const char* b : { "Collarbone", "UpperArm", "ForeArm1", "ForeArm2", "ForeArm3", "Hand", "Finger21", "Finger22" }) {
                const int il = BoneIndex(t, (std::string("LArm_") + b).c_str()), ir = BoneIndex(t, (std::string("RArm_") + b).c_str());
                if (il < 0 || ir < 0) continue;
                const Xf L = FromNi(t->bone[il].node ? t->bone[il].node->local : t->bone[il].local);
                const Xf R = FromNi(t->bone[ir].node ? t->bone[ir].node->local : t->bone[ir].local);
                const M3 sb = Diag(1, 1, -1);
                const V  rt = Mul(sb, R.t);
                mir += std::format("{}: Lt {} Rt {} |Lt-SbRt| {:.3f} rotLvsSbRSb {:.2f}deg; ", b, F(L.t, 3), F(R.t, 3), Len(L.t - rt),
                    AngleDeg(L.R, Mul(Mul(sb, R.R), sb)));
            }
            REX::INFO("hands dump: mirror {}", mir);
            out += " mirror=[" + mir + "]";
            REX::INFO("hands dump: {}", out);
            return out;
        }

        // ---- stage 2: kept recoil ------------------------------------------------------------------------------
        // A = animated WEAPON relative to the Camera bone. Outside a shot window the rest pose follows A (delta =
        // identity: sway, bob, ADS raise stay cancelled). A shot (ammoCount of the equipped weapon drops) freezes the
        // rest at the pre-shot A for fRecoilWindowMs; the delta rest^-1 . A (clamped, faded out over the last quarter
        // of the window, scaled) is applied to WEAPON* in its own frame.
        struct RecoilState
        {
            bool                                  prevValid{ false };
            Xf                                    rest, prev;
            std::chrono::steady_clock::time_point shotT{};
            int                                   lastAmmo{ -1 };
            const void*                           lastForm{ nullptr };
            std::uint64_t                         shots{ 0 };
        };
        Xf UpdateRecoil(RecoilState& rs, const Xf& a_A, int a_ammo, const void* a_form, const Cfg& c, std::chrono::steady_clock::time_point a_now,
            double& a_t, double& a_r, bool& a_in)
        {
            if (a_form != rs.lastForm) {
                const auto shots = rs.shots;
                rs = RecoilState{};
                rs.lastForm = a_form;
                rs.shots = shots;
            }
            const bool shot = a_ammo >= 0 && rs.lastAmmo >= 0 && a_ammo < rs.lastAmmo;
            rs.lastAmmo = a_ammo;
            auto ageMs = [&] { return std::chrono::duration<double, std::milli>(a_now - rs.shotT).count(); };
            const bool wasIn = rs.shotT.time_since_epoch().count() != 0 && ageMs() < c.recoilWindowMs;
            if (shot) {
                ++rs.shots;
                if (!wasIn) rs.rest = rs.prevValid ? rs.prev : a_A;
                rs.shotT = a_now;
            }
            a_in = rs.shotT.time_since_epoch().count() != 0 && ageMs() < c.recoilWindowMs;
            Xf D;
            if (a_in) {
                D = Mul(Inv(rs.rest), a_A);
                D.s = 1.0;
                const double lt = Len(D.t), maxT = c.recoilMaxCm * 0.01 * c.unitsPerMeter;
                if (lt > maxT && lt > 0) D.t = D.t * (maxT / lt);
                const double ang = AngleDeg(D.R, M3{});
                if (ang > c.recoilMaxDeg && ang > 0) D.R = Scale(D.R, c.recoilMaxDeg / ang);
                const double env = std::clamp((c.recoilWindowMs - ageMs()) / (0.25 * c.recoilWindowMs), 0.0, 1.0) * c.recoilScale;
                D.t = D.t * env;
                D.R = Scale(D.R, env);
            } else {
                rs.rest = a_A;
            }
            rs.prev = a_A;
            rs.prevValid = true;
            a_t = Len(D.t);
            a_r = AngleDeg(D.R, M3{});
            return D;
        }

        // ---- stage 2: one frame (hook thread) ---------------------------------------------------------------------
        struct S2Out
        {
            Xf                 w;            // WEAPON* (player-relative)
            bool               twoHanded{ false };
            Stats::Arm         arm[2];
            const char*        offMode{ "-" };
            double             gripDist{ -1 }, barrelLR{ -1 }, fkErr{ 0 };
            V                  body, neck;
            double             bodyYawDeg{ 0 }, rootYawDeg{ 0 };
            float              flex[5]{ 1, 1, 1, 1, 1 };
            double             fingerDeg{ 0 };
            int                fingersWritten{ 0 };
            bool               pipValid{ false };
            float              pipGrip[3]{};
            V                  pipRef, pipRig;
            std::uint64_t      persisted{ 0 }, rewritten{ 0 };
            V                  gripRef, offCtlRef, offCtlRig, gunHandRef;  // support grip (one-handed gun), off-controller hand target, gun hand: ref space (m) / rig
            double             fingerDeg2{ 0 };                       // LArm_Finger32 (middle) local vs open, read back after the update
        };
        ArmSmooth g_sm[2];          // hook thread only
        bool      g_2hState = false;  // hook thread only

        const char* Stage2(const Rig& rig, const Cfg& cfg, const detail::Snap& snap, const Xf& camRel, GunState& g, double dt, S2Out& o)
        {
            if (!rig.arm[0].Ok() || !rig.arm[1].Ok() || rig.iChest < 0 || rig.iCamera < 0) return "arm bones missing";
            static PoseCache P;  // the hook runs on one job at a time
            // persistence probe: does the graph rewrite the arm/finger locals we wrote last frame?
            static RE::NiTransform lastUpper{}, lastFinger{};
            static bool            haveLast = false;
            const int              fIdx = rig.fingersL ? rig.fingersL->idx[4] : -1;  // LArm_Finger22
            if (haveLast && rig.tree->bone[rig.arm[0].upper].node) {
                const bool same = std::memcmp(&rig.tree->bone[rig.arm[0].upper].node->local, &lastUpper, sizeof(lastUpper)) == 0 &&
                                  (fIdx < 0 || std::memcmp(&rig.tree->bone[fIdx].local, &lastFinger, sizeof(lastFinger)) == 0);
                (same ? o.persisted : o.rewritten)++;
            }
            P.Load(rig.tree);
            o.fkErr = P.fkErr;
            const double upm = cfg.unitsPerMeter, cm = 0.01 * upm;

            // 1) body under the HMD: COM placed with the yaw-only player heading, its Camera bone (bind, unpitched) at
            //    the neck pivot + the eye offset; the flat rig's pitch (Actor::GetLooking) is removed.
            const Xf     rootW = P.ParentWorld(rig.iCom);
            const Xf     camInRoot = Mul(Inv(rootW), P.world[rig.iCamera]);
            const Xf     comInRoot = Mul(Inv(rootW), P.world[rig.iCom]);
            const double h = rig.player->data.angle.z;
            M3           yaw;
            yaw.SetCol(0, { std::cos(h), -std::sin(h), 0 });
            yaw.SetCol(1, { std::sin(h), std::cos(h), 0 });
            yaw.SetCol(2, { 0, 0, 1 });
            const V e{ 0, cfg.neckFwdCm * cm, cfg.neckUpCm * cm };
            const V b{ 0, cfg.bodyFwdCm * cm, cfg.bodyUpCm * cm };
            const V neck = camRel.t - Mul(camRel.R, e);
            const V eyeLevel = neck + Mul(yaw, e) + Mul(yaw, b);
            Xf      root2;
            root2.R = yaw;
            root2.s = rootW.s;
            root2.t = eyeLevel - Mul(yaw, camInRoot.t) * rootW.s;
            P.SetWorld(rig.iCom, Mul(root2, comInRoot));
            o.body = P.world[rig.iCom].t;
            o.neck = neck;
            o.bodyYawDeg = std::atan2(yaw.m[0][1], yaw.m[1][1]) * kR2D;
            o.rootYawDeg = std::atan2(rootW.R.m[0][1], rootW.R.m[1][1]) * kR2D;
            BodyFrame bf;
            bf.fwd = yaw.Col(1);
            bf.right = yaw.Col(0);
            bf.chestZ = P.world[rig.iChest].t.z;

            // 2) gun + hand targets; two-handed snap with hysteresis (enter near the one-handed support grip, leave
            //    when the off controller is farther than fTwoHandedReleaseCm from the two-handed one)
            const bool lOk = snap.lNew.valid;
            Xf         hand1, offCtl, offGrip1;
            {
                GunState g1 = g;
                g1.twoHanded = false;
                GunPose(camRel, snap.rNew, snap.lNew, cfg, g1, &hand1, &offCtl, &offGrip1, nullptr);
            }
            if (cfg.twoHanded && lOk && g.offRelValid) {
                if (!g_2hState) {
                    o.gripDist = Len(offCtl.t - offGrip1.t);
                    g_2hState = o.gripDist < cfg.snapCm * cm;
                } else {
                    GunState g2 = g;
                    g2.twoHanded = true;
                    Xf og;
                    GunPose(camRel, snap.rNew, snap.lNew, cfg, g2, nullptr, nullptr, &og, nullptr);
                    o.gripDist = Len(offCtl.t - og.t);
                    g_2hState = o.gripDist < cfg.releaseCm * cm;
                }
            } else {
                g_2hState = false;
            }
            g.twoHanded = g_2hState;
            o.twoHanded = g_2hState;
            Xf handT, offGrip;
            o.w = GunPose(camRel, snap.rNew, snap.lNew, cfg, g, &handT, &offCtl, &offGrip, &o.barrelLR);
            if (!g_2hState) o.barrelLR = -1;

            // 3) arms. The gun hand always reaches (stretch unlimited); the free off hand stops at fArmMaxStretch.
            if (!SolveArm(P, rig.arm[0], false, handT, 0.0, cfg, bf, dt, g_sm[0], o.arm[0])) return "gun arm unsolvable";
            if (lOk) {
                const Xf& lt = g_2hState ? offGrip : offCtl;
                o.offMode = g_2hState ? "2h" : "free";
                if (!SolveArm(P, rig.arm[1], true, lt, g_2hState ? 0.0 : cfg.maxStretch, cfg, bf, dt, g_sm[1], o.arm[1])) o.offMode = "anim(unsolved)";
            } else {
                o.offMode = "anim";
            }

            // 4) free off-hand fingers: index <- trigger, middle/ring/pinky <- grip, thumb <- face button / stick
            if (cfg.fingers && lOk && !g_2hState && rig.fingersL) {
                const auto& in = snap.inOff;
                o.flex[0] = 1.0f - in.thumb;
                o.flex[1] = 1.0f - in.trigger;
                o.flex[2] = o.flex[3] = o.flex[4] = 1.0f - in.grip;
                const auto& fs = *rig.fingersL;
                for (int k = 0; k < 15; ++k) {
                    Xf l = P.local[fs.idx[k]];
                    l.R = Slerp(fs.closed[k], fs.open[k], std::clamp(static_cast<double>(o.flex[fs.finger[k]]), 0.0, 1.0));
                    P.SetLocal(fs.idx[k], l);
                    ++o.fingersWritten;
                }
            }

            // 5) write and re-run the engine's first-person update
            P.WriteBack();
            const float ft = *reinterpret_cast<const float*>(Base() + kFrameTime);
            reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(ft, rig.root, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
            if (rig.tree->bone[rig.arm[0].upper].node) lastUpper = rig.tree->bone[rig.arm[0].upper].node->local;
            if (fIdx >= 0) lastFinger = rig.tree->bone[fIdx].local;
            haveLast = true;

            // 6) measured from the engine's worlds after its update
            for (int s = 0; s < 2; ++s) {
                auto& a = o.arm[s];
                if (!a.solved) continue;
                const Xf hw = FromNi(rig.tree->bone[rig.arm[s].hand].world);
                const Xf tg = s == 0 ? handT : (g_2hState ? offGrip : offCtl);
                a.hand = hw.t;
                a.err = Len(hw.t - tg.t);
                a.angErr = AngleDeg(hw.R, tg.R);
                a.elbow = NiV(rig.tree->bone[rig.arm[s].f1].world.translate);
                a.shoulder = NiV(rig.tree->bone[rig.arm[s].upper].world.translate);
                a.upperLen = Len(a.elbow - a.shoulder);
                a.foreLen = Len(a.hand - a.elbow);
            }
            auto toRef = [&](const V& a_rig) {
                if (!snap.headNewValid) return V{};
                const V  m = Mul(Inv(camRel), Xf{ M3{}, a_rig, 1.0 }).t * (1.0 / upm);
                const Q4 hq{ snap.headNewQ[3], snap.headNewQ[0], snap.headNewQ[1], snap.headNewQ[2] };
                return V{ snap.headNewP[0], snap.headNewP[1], snap.headNewP[2] } + Mul(FromQ(hq), V{ m.x, m.z, -m.y });
            };
            if (g.offRelValid) o.gripRef = toRef(offGrip1.t);
            if (lOk) o.offCtlRef = toRef(offCtl.t), o.offCtlRig = offCtl.t;
            o.gunHandRef = toRef(handT.t);
            if (rig.fingersL) {
                const auto& fs = *rig.fingersL;
                o.fingerDeg = AngleDeg(FromNi(rig.tree->bone[fs.idx[4]].local).R, fs.open[4]);
                o.fingerDeg2 = AngleDeg(FromNi(rig.tree->bone[fs.idx[7]].local).R, fs.open[7]);
            }
            if (rig.iPip >= 0 && lOk && o.arm[1].solved) {
                const Xf pip = FromNi(rig.tree->bone[rig.iPip].world);
                o.pipRig = pip.t;
                const Xf HL = Mul(camRel, HandXf(snap.lNew, upm));
                const V  inGrip = Mul(Inv(HL), pip).t;  // game-basis grip frame, units
                const V  lifted = inGrip - V{ cfg.pipboyLiftCm * cm, 0, 0 };  // back of the left hand = grip -X
                o.pipGrip[0] = static_cast<float>(lifted.x / upm);
                o.pipGrip[1] = static_cast<float>(lifted.z / upm);
                o.pipGrip[2] = static_cast<float>(-lifted.y / upm);
                o.pipValid = true;
                if (snap.headNewValid) {
                    const V  m = Mul(Inv(camRel), pip).t * (1.0 / upm);  // camera-local metres, game basis
                    const Q4 hq{ snap.headNewQ[3], snap.headNewQ[0], snap.headNewQ[1], snap.headNewQ[2] };
                    const V  r = Mul(FromQ(hq), V{ m.x, m.z, -m.y });
                    o.pipRef = V{ snap.headNewP[0], snap.headNewP[1], snap.headNewP[2] } + r;
                }
            }
            return nullptr;
        }

        void Frame()
        {
            const double t0 = QpcUs();
            Cfg          cfg;
            detail::Snap snap;
            {
                std::scoped_lock l(g_lock);
                cfg = g_cfg;
                snap = g_snap;
                ++g_st.calls;
            }
            const char* why = nullptr;
            Rig         rig;
            const bool  haveRig = GetRig(rig, why);
            if (haveRig && g_dumpPending.exchange(false)) {
                const auto d = DumpRig(rig);
                std::scoped_lock l(g_lock);
                g_dumpReq = d;
            }
            // probe 4: does the graph rewrite the anchor local every frame?
            if (g_wrote && g_writtenNode && haveRig && g_writtenTree == rig.tree && g_writtenNode == rig.tree->bone[rig.iCom].node.get()) {
                const bool persisted = std::memcmp(&g_writtenNode->local, &g_written, sizeof(RE::NiTransform)) == 0;
                std::scoped_lock l(g_lock);
                (persisted ? g_st.comPersisted : g_st.comRewritten)++;
            }
            const auto now = std::chrono::steady_clock::now();
            const double ageMs = std::chrono::duration<double, std::milli>(now - snap.t).count();
            Xf cam;
            const int cs = CamState();
            if (!haveRig) {
            } else if (!g_on.load()) {
                why = "off";
            } else if (cs != 0 && cs != 4) {
                why = "camera state";
            } else if (!snap.worldView) {
                why = "no world view (menu/state)";
            } else if (!snap.rNew.valid || ageMs > kFreshMs) {
                why = "no fresh controller snapshot";
            } else if (VatsActive()) {
                why = "vats";
            } else if (g_paOk && reinterpret_cast<ActorBool_t>(Base() + kActorInPowerArmor)(rig.player)) {
                why = "power armour";
            } else if (WeaponState(rig.player) != 3) {
                why = "weapon not drawn";
            } else if (!CamRootWorld(cam)) {
                why = "no camera root";
            } else if (!g_fnOk) {
                why = "update function check failed";
            }
            // node to move: COM if it has a NiNode, else the top-most ancestor of WEAPON (below Root) that has one
            RE::NiNode* anchor = nullptr;
            int         anchorIdx = -1;
            if (haveRig) {
                if (rig.tree->bone[rig.iCom].node) {
                    anchor = rig.tree->bone[rig.iCom].node.get();
                    anchorIdx = rig.iCom;
                } else {
                    for (int i = rig.iWeapon; i >= 0; i = rig.tree->bone[i].parent) {
                        if (rig.tree->bone[i].node && rig.tree->bone[i].parent >= 0 && rig.tree->bone[rig.tree->bone[i].parent].parent >= 0) {
                            anchor = rig.tree->bone[i].node.get();
                            anchorIdx = i;
                        }
                        if (rig.tree->bone[i].parent < 0) break;
                    }
                }
                if (!why && (!anchor || !anchor->parent)) why = "no anchor node";
            }
            if (why) {
                RestoreIfNeeded(true, haveRig ? anchor : nullptr, haveRig ? rig.tree : nullptr);
            }
            const bool active = !why;

            // weapon form + J
            std::string wname = "-";
            Xf          J = FromRows(kBindRows, kBindT);
            int         jkind = 2;
            if (haveRig) {
                static const RE::TESForm* lastForm = nullptr;
                static std::string        lastName;
                const RE::TESForm*        form = EquippedWeapon(rig.player);
                if (form != lastForm) {
                    lastForm = form;
                    lastName = form ? std::string(RE::TESFullName::GetFullName(*form)) : std::string();
                }
                wname = lastName.empty() ? "-" : lastName;
                if (!lastName.empty() && cfg.align != 1) {
                    std::scoped_lock l(g_lock);
                    if (auto it = g_json.find(Lower(lastName)); it != g_json.end()) {
                        J = it->second;
                        jkind = 1;
                    }
                }
            }
            // F = WEAPON^-1 . fireNode (animation's model transform; unchanged by a rigid move above WEAPON)
            Xf   Fx;
            bool fOk = false;
            if (haveRig && rig.fire) {
                const Xf w = FromNi(rig.weapon->world), f = FromNi(rig.fire->world);
                Fx = Mul(Inv(w), f);
                fOk = true;
            }
            if (jkind != 1 && fOk && cfg.align != 2) {
                jkind = 3;
            }

            Xf     before, tgt, after;
            double posErr = 0, angErr = 0, shift = 0, rotDeg = 0;
            V      loc;
            Xf     gripH;
            if (haveRig) {
                loc = NiV(rig.player->data.location);
                before = FromNi(rig.weapon->world);
            }
            // stage 2 inputs from the animated pose (before any write this frame)
            static RecoilState recoil;
            static double      lastFrameUs = 0;
            GunState           gun;
            gun.J = J, gun.kind = jkind, gun.F = Fx, gun.FValid = fOk, gun.scale = haveRig ? before.s : 1.0;
            S2Out       s2;
            const char* ikWhy = "off";
            bool        ikDone = false;
            double      recT = 0, recR = 0;
            bool        recIn = false;
            int         ammo = -1;
            const double dt = lastFrameUs > 0 ? (t0 - lastFrameUs) * 1e-6 : 1.0 / 90.0;
            lastFrameUs = t0;
            if (active) {
                const auto& bn = rig.tree->bone;
                const Xf    wAnim = before;
                if (rig.iHand >= 0) {
                    gun.Janim = Mul(Inv(FromNi(bn[rig.iHand].world)), wAnim);
                    gun.JanimValid = true;
                }
                if (rig.iLHand >= 0) {
                    gun.offRel = Mul(Inv(wAnim), FromNi(bn[rig.iLHand].world));
                    gun.offRelValid = Len(gun.offRel.t) < cfg.gripMaxDist;
                }
                ammo = EquippedAmmo(rig.player);
                if (rig.iCamera >= 0) {
                    const Xf A = Mul(Inv(FromNi(bn[rig.iCamera].world)), wAnim);
                    gun.recoil = UpdateRecoil(recoil, A, ammo, EquippedWeapon(rig.player), cfg, now, recT, recR, recIn);
                }
                if (!cfg.keepRecoil) gun.recoil = Xf{};
            } else {
                g_sm[0].init = g_sm[1].init = false;
                g_2hState = false;
            }
            if (active) {
                Xf camRel = cam;
                camRel.t = cam.t - loc;  // the rig lives in player-relative space
                gripH = Mul(camRel, HandXf(snap.rNew, cfg.unitsPerMeter));
                if (!cfg.armIK) {
                    ikWhy = "off";
                } else if (snap.leftHanded) {
                    ikWhy = "left-handed (weapon node is in the right hand)";
                } else {
                    ikWhy = Stage2(rig, cfg, snap, camRel, gun, dt, s2);
                    ikDone = ikWhy == nullptr;
                    if (ikDone) ikWhy = "on";
                }
            }
            double kickT = 0, kickR = 0;
            if (active && ikDone) {
                tgt = s2.w;
                {
                    Xf camRel = cam;
                    camRel.t = cam.t - loc;
                    GunState g0 = gun;
                    g0.recoil = Xf{};
                    g0.twoHanded = s2.twoHanded;
                    const Xf w0 = GunPose(camRel, snap.rNew, snap.lNew, cfg, g0, nullptr, nullptr, nullptr, nullptr);
                    const Xf wr = FromNi(rig.weapon->world);
                    kickT = Len(wr.t - w0.t);
                    kickR = AngleDeg(wr.R, w0.R);
                }
                after = FromNi(rig.weapon->world);
                posErr = Len(after.t - tgt.t);
                angErr = AngleDeg(after.R, tgt.R);
                shift = Len(after.t - before.t);
                rotDeg = AngleDeg(after.R, before.R);
                g_wrote = false;  // COM etc. are rewritten by the graph every frame (stage-0 item 4)
            } else if (active) {
                const double s = before.s;
                Xf           camRel = cam;
                camRel.t = cam.t - loc;  // the rig lives in player-relative space
                gun.twoHanded = false;
                tgt = GunPose(camRel, snap.rNew, snap.lNew, cfg, gun, nullptr, nullptr, nullptr, nullptr);
                tgt.s = s;
                // rigid correction at the anchor
                M3       dR = Mul(tgt.R, T(before.R));
                const V  dT = tgt.t - Mul(dR, before.t);
                const Xf aw = FromNi(anchor->world);
                Xf       aw2;
                aw2.R = Ortho(Mul(dR, aw.R));
                aw2.t = Mul(dR, aw.t) + dT;
                aw2.s = aw.s;
                const Xf pw = FromNi(anchor->parent->world);
                Xf       al = Mul(Inv(pw), aw2);
                al.s = anchor->local.scale;
                // save the animated local before the first write (graphs that do not rewrite it every frame)
                const bool rewritten = !(g_wrote && g_writtenNode == anchor && std::memcmp(&anchor->local, &g_written, sizeof(RE::NiTransform)) == 0);
                if (rewritten || !g_haveSaved) {
                    g_saved = anchor->local;
                    g_haveSaved = true;
                }
                ToNi(al, anchor->local);
                rig.tree->bone[anchorIdx].local = anchor->local;
                g_written = anchor->local;
                g_writtenNode = anchor;
                g_writtenIdx = anchorIdx;
                g_writtenTree = rig.tree;
                g_wrote = true;
                const float ft = *reinterpret_cast<const float*>(Base() + kFrameTime);
                reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(ft, rig.root, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
                after = FromNi(rig.weapon->world);
                posErr = Len(after.t - tgt.t);
                angErr = AngleDeg(after.R, tgt.R);
                shift = Len(dT + Mul(dR, before.t) - before.t);
                rotDeg = AngleDeg(dR, M3{});
            } else if (haveRig) {
                after = before;
            }
            // fire node after the override (player-relative and world)
            V    fireRel, fireFwd;
            bool fireValid = false;
            if (haveRig && rig.fire) {
                const Xf f = FromNi(rig.fire->world);
                fireRel = f.t;
                fireFwd = Norm(f.R.Col(1));
                fireValid = true;
            }
            const double t1 = QpcUs();
            Xf camW;
            const bool haveCam = CamRootWorld(camW);
            std::scoped_lock l(g_lock);
            auto& st = g_st;
            st.gate = why ? why : "active";
            st.weapon = wname;
            st.jmode = !active ? "-" : jkind == 1 ? "json" : jkind == 3 ? "auto" : "bind";
            st.anchor = anchorIdx >= 0 && haveRig ? std::string(rig.tree->bone[anchorIdx].name.c_str()) : "-";
            st.anchorNode = anchor != nullptr;
            st.boneCount = haveRig ? rig.tree->boneCount : 0;
            st.snapSeq = snap.seq;
            st.snapAgeMs = ageMs;
            st.loc = loc;
            st.before = before.t;
            st.weaponState = haveRig ? WeaponState(rig.player) : -1;
            if (haveCam) st.camHook = camW.t;
            if (active) {
                ++st.active;
                ++st.overrides;
                st.tgt = tgt.t;
                st.act = after.t;
                st.posErr = posErr;
                st.angErr = angErr;
                st.shift = shift;
                st.rot = rotDeg;
                st.costUs = t1 - t0;
                st.costAvgUs = st.costN == 0 ? st.costUs : st.costAvgUs * 0.98 + st.costUs * 0.02;
                st.costMaxUs = (std::max)(st.costMaxUs, st.costUs);
                ++st.costN;
                // calibration aids in the grip frame (identity C)
                if (fireValid) {
                    const V fw = Mul(T(gripH.R), fireFwd);
                    st.fwdInGrip = fw;
                    st.upInGrip = Mul(T(gripH.R), Norm(FromNi(rig.fire->world).R.Col(2)));
                }
                const Xf hx = HandXf(snap.rNew, 1.0);
                st.aimInGrip = Mul(T(hx.R), V{ snap.rNew.aimDir[0], snap.rNew.aimDir[1], snap.rNew.aimDir[2] });
            }
            st.actWorld = after.t + loc;
            st.fireValid = fireValid;
            st.fireRel = fireRel;
            st.fireWorld = fireRel + loc;
            st.fireFwd = fireFwd;
            // stage 2
            st.ik = active ? ikWhy : "-";
            if (active) {
                st.shots = recoil.shots;
                st.ammo = ammo;
                st.recT = recT, st.recR = recR, st.recWindow = recIn;
                st.recMaxT = (std::max)(st.recMaxT, recT), st.recMaxR = (std::max)(st.recMaxR, recR);
                st.kickT = kickT, st.kickR = kickR;
                st.kickMaxT = (std::max)(st.kickMaxT, kickT), st.kickMaxR = (std::max)(st.kickMaxR, kickR);
                st.offRelT = gun.offRel.t;
                st.offRelValid = gun.offRelValid;
            }
            if (ikDone) {
                ++st.ikFrames;
                st.fkErr = s2.fkErr;
                st.body = s2.body;
                st.neck = s2.neck;
                st.bodyYawDeg = s2.bodyYawDeg, st.rootYawDeg = s2.rootYawDeg;
                st.arm[0] = s2.arm[0], st.arm[1] = s2.arm[1];
                st.offMode = s2.offMode;
                st.gripDist = s2.gripDist;
                st.barrelLR = s2.barrelLR;
                if (s2.twoHanded && !g_twoHandedOn) ++st.twoHandedEnters;
                std::memcpy(st.flex, s2.flex, sizeof(st.flex));
                st.fingerDeg = s2.fingerDeg;
                st.fingerDeg2 = s2.fingerDeg2;
                st.gripRef = s2.gripRef, st.offCtlRef = s2.offCtlRef, st.offCtlRig = s2.offCtlRig, st.gunHandRef = s2.gunHandRef;
                st.fingersWritten = s2.fingersWritten;
                st.pipValid = s2.pipValid;
                st.pipGrip = { s2.pipGrip[0], s2.pipGrip[1], s2.pipGrip[2] };
                st.pipRef = s2.pipRef;
                st.pipRig = s2.pipRig + loc;
                st.ikPersisted += s2.persisted, st.ikRewritten += s2.rewritten;
                st.ikCostUs = t1 - t0;
                st.ikCostAvgUs = st.ikCostAvgUs == 0 ? st.ikCostUs : st.ikCostAvgUs * 0.98 + st.ikCostUs * 0.02;
                st.ikCostMaxUs = (std::max)(st.ikCostMaxUs, st.ikCostUs);
            }
            g_ikLast = ikDone;
            g_twoHandedOn = ikDone && s2.twoHanded;
            g_pipValid = ikDone && s2.pipValid && cfg.pipboyOnBone;
            if (g_pipValid) std::memcpy(g_pipGrip, s2.pipGrip, sizeof(g_pipGrip));
            gun.twoHanded = g_twoHandedOn;
            g_gun = gun;
            g_lastActive = active;
            if (active) g_lastActiveT = now;
            g_F = Fx;
            g_FValid = fOk;
            g_J = J;
            g_jKind = jkind;
            g_wScale = before.s;
            // probe A (end of the hook)
            if (g_probe.framesLeft > 0 && haveRig) {
                g_probe.a = FromNi(rig.weapon->world);
                g_probe.camA = haveCam ? camW.t : V{};
                g_probe.aFresh = true;
                g_probe.bSeen = false;
                g_probe.tidA = ::GetCurrentThreadId();
                g_probe.tA = QpcUs();
                g_probe.tA0 = t0;
            }
        }

        int HkNotify(void* a_src, void* a_evt)
        {
            Frame();
            return g_origNotify(a_src, a_evt);
        }

        double MaxAbsDiff(const M3& a, const M3& b)
        {
            double m = 0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) m = (std::max)(m, std::abs(a.m[i][j] - b.m[i][j]));
            return m;
        }

        void HkSwap(void* a_main)
        {
            {
                std::scoped_lock l(g_lock);
                if (g_probe.framesLeft > 0 && !g_probe.bSeen && g_probe.tA > 0) {
                    g_probe.tB = QpcUs();
                    if (auto* p = RE::PlayerCharacter::GetSingleton(); p && p->firstPerson3D) {
                        Rig rig;
                        const char* why = nullptr;
                        if (GetRig(rig, why)) {
                            const Xf b = FromNi(rig.weapon->world);
                            g_probe.maxAB = (std::max)(g_probe.maxAB, Len(b.t - g_probe.a.t));
                            g_probe.maxRotAB = (std::max)(g_probe.maxRotAB, MaxAbsDiff(b.R, g_probe.a.R));
                            ++g_probe.nAB;
                            g_probe.tidB = ::GetCurrentThreadId();
                        }
                    }
                    g_probe.bSeen = true;
                }
            }
            g_origSwap(a_main);
            {
                std::scoped_lock l(g_lock);
                if (g_probe.framesLeft > 0) g_probe.tBend = QpcUs();
            }
        }

        bool BytesMatch(std::uintptr_t a_rva, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_rva);
            if (std::memcmp(p, a_expect, a_n) == 0) return true;
            std::string got;
            for (std::size_t i = 0; i < a_n; ++i) got += std::format("{:02X} ", p[i]);
            REX::ERROR("hands: {} at {:X} has bytes [{}]; not patching", a_what, a_rva, got);
            return false;
        }

        bool CallMatches(std::uintptr_t a_site, std::uintptr_t a_target, const std::uint8_t (&a_bytes)[5], const char* a_what)
        {
            if (!BytesMatch(a_site, a_bytes, 5, a_what)) return false;
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(Base() + a_site + 1), 4);
            if (a_site + 5 + rel != a_target) {
                REX::ERROR("hands: {} call at {:X} targets {:X}, expected {:X}", a_what, a_site, a_site + 5 + rel, a_target);
                return false;
            }
            return true;
        }
    }

    namespace detail
    {
        void SetSnapshot(const Snap& a_s)
        {
            std::scoped_lock l(g_lock);
            g_snap = a_s;
        }
    }

    std::string Install()
    {
        static std::string summary;
        if (!summary.empty()) return summary;
        const auto path = Settings::Path();
        auto rd = [&](const char* k, float d) {
            char buf[64]{};
            ::GetPrivateProfileStringA("Hands", k, "", buf, sizeof(buf), path.c_str());
            char*       end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : d;
        };
        Cfg& c = g_cfg;
        c.enabled = rd("bWeaponInHand", 1.0f) != 0.0f;
        c.aimFromMuzzle = rd("bAimFromMuzzle", 1.0f) != 0.0f;
        c.noGunLowering = rd("bNoGunLowering", 1.0f) != 0.0f;
        c.align = static_cast<int>(rd("iAlign", 0.0f));
        const char* ok[3] = { "fGripOffsetX", "fGripOffsetY", "fGripOffsetZ" };
        const char* rk[3] = { "fGripRotXDeg", "fGripRotYDeg", "fGripRotZDeg" };
        for (int i = 0; i < 3; ++i) {
            c.offCm[i] = rd(ok[i], c.offCm[i]);
            c.rotDeg[i] = rd(rk[i], c.rotDeg[i]);
        }
        // stage 2
        c.armIK = rd("bArmIK", 1.0f) != 0.0f;
        c.twoHanded = rd("bTwoHanded", 0.0f) != 0.0f;
        c.keepRecoil = rd("bKeepRecoil", 1.0f) != 0.0f;
        c.fingers = rd("bFingers", 1.0f) != 0.0f;
        c.pipboyOnBone = rd("bPipboyOnBone", 1.0f) != 0.0f;
        c.neckFwdCm = rd("fEyeToNeckFwdCm", c.neckFwdCm);
        c.neckUpCm = rd("fEyeToNeckUpCm", c.neckUpCm);
        c.bodyFwdCm = rd("fBodyOffsetFwdCm", c.bodyFwdCm);
        c.bodyUpCm = rd("fBodyOffsetUpCm", c.bodyUpCm);
        c.armLength = rd("fArmLength", c.armLength);
        c.maxStretch = rd("fArmMaxStretch", c.maxStretch);
        c.shoulderReach = rd("fArmShoulderReachFraction", c.shoulderReach);
        c.minFlexDeg = rd("fArmElbowMinFlexionDeg", c.minFlexDeg);
        c.maxFlexDeg = rd("fArmElbowMaxFlexionDeg", c.maxFlexDeg);
        c.snapCm = rd("fTwoHandedSnapCm", c.snapCm);
        c.releaseCm = rd("fTwoHandedReleaseCm", c.releaseCm);
        c.gripMaxDist = rd("fSupportGripMaxDist", c.gripMaxDist);
        c.recoilScale = rd("fRecoilScale", c.recoilScale);
        c.recoilWindowMs = rd("fRecoilWindowMs", c.recoilWindowMs);
        c.recoilMaxCm = rd("fRecoilMaxCm", c.recoilMaxCm);
        c.recoilMaxDeg = rd("fRecoilMaxDeg", c.recoilMaxDeg);
        c.pipboyLiftCm = rd("fPipboyLiftCm", c.pipboyLiftCm);
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("General", "fUnitsPerMeter", "70", buf, sizeof(buf), path.c_str());
            const float u = std::strtof(buf, nullptr);
            c.unitsPerMeter = u > 1.0f ? u : 70.0f;
            char dir[512]{};
            ::GetPrivateProfileStringA("Hands", "sWeaponOffsetsDir", "", dir, sizeof(dir), path.c_str());
            c.jsonDir = dir[0] ? dir : std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr_weapons").string();
        }
        if (!c.enabled) {
            summary = "off ([Hands] bWeaponInHand=0): not patched";
            REX::INFO("hands: {}", summary);
            return summary;
        }
        const int nJson = LoadJsons(c.jsonDir);

        static constexpr std::uint8_t kUpdPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 };
        static constexpr std::uint8_t kQCam[] = { 0x48, 0x8B, 0x05, 0x59, 0x7E, 0xC6, 0x01, 0xC3 };  // mov rax,[3E622D0]; ret
        g_fnOk = BytesMatch(kUpdateNode, kUpdPrologue, sizeof(kUpdPrologue), "UpdateNode D9F7D0 prologue") &&
                 BytesMatch(kQ1stPersonCamera, kQCam, sizeof(kQCam), "Q1stPersonCamera");
        static constexpr std::uint8_t kPaPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 };
        g_paOk = BytesMatch(kActorInPowerArmor, kPaPrologue, sizeof(kPaPrologue), "ActorInPowerArmor prologue");

        auto& tramp = REL::GetTrampoline();
        if (tramp.empty()) {
            tramp.create(256);
        }
        static constexpr std::uint8_t kNotifyCall[5] = { 0xE8, 0xB8, 0xB1, 0x00, 0x00 };
        if (g_fnOk && CallMatches(kNotifyCallSite, kNotify, kNotifyCall, "PlayerUpdateEvent Notify")) {
            g_origNotify = reinterpret_cast<Notify_t>(tramp.write_call<5>(Base() + kNotifyCallSite, &HkNotify));
            g_notifyHooked = true;
        }
        static constexpr std::uint8_t kSwapCall[5] = { 0xE8, 0x9F, 0xEB, 0xFF, 0xFF };
        if (CallMatches(kSwapCallSite, kSwap, kSwapCall, "Main::Swap")) {
            g_origSwap = reinterpret_cast<Swap_t>(tramp.write_call<5>(Base() + kSwapCallSite, &HkSwap));
            g_swapHooked = true;
        }
        g_on = g_notifyHooked;
        summary = std::format("W1(C351F3)={} W2(C346AC)={} fn={} pa={} jsons={} dir='{}' grip off=({},{},{})cm rot=({},{},{})deg align={} aimFromMuzzle={} noGunLowering={} "
                              "armIK={} twoHanded={} keepRecoil={} fingers={} pipboyOnBone={}",
            g_notifyHooked ? "ok" : "FAILED", g_swapHooked ? "ok" : "FAILED", g_fnOk ? "ok" : "FAILED", g_paOk ? "ok" : "FAILED", nJson, c.jsonDir,
            c.offCm[0], c.offCm[1], c.offCm[2], c.rotDeg[0], c.rotDeg[1], c.rotDeg[2], c.align, c.aimFromMuzzle, c.noGunLowering, c.armIK, c.twoHanded,
            c.keepRecoil, c.fingers, c.pipboyOnBone);
        REX::INFO("hands: {}", summary);
        return summary;
    }

    bool AimFromWeapon(const float a_camRows[9], const float a_camPos[3], float a_P[3], float a_D[3])
    {
        std::scoped_lock l(g_lock);
        ++g_st.aimCalls;
        const auto now = std::chrono::steady_clock::now();
        if (!g_on.load() || !g_cfg.aimFromMuzzle || !g_lastActive || !g_FValid ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastActiveT).count() > kFreshMs) {
            return false;
        }
        const auto& h = g_snap.rOld;
        if (!h.valid || std::chrono::duration_cast<std::chrono::milliseconds>(now - g_snap.t).count() > kFreshMs) {
            return false;
        }
        Xf cam;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cam.R.m[r][c] = a_camRows[c * 3 + r];
        cam.t = { a_camPos[0], a_camPos[1], a_camPos[2] };
        // same chain as the hook's gun (kept recoil, two-handed barrel), from the hand poses of the rendered head
        GunState g = g_gun;
        g.twoHanded = g_twoHandedOn && g_snap.lOld.valid;
        const Xf w = GunPose(cam, h, g_snap.lOld, g_cfg, g, nullptr, nullptr, nullptr, nullptr);
        const Xf m = Mul(w, g_F);
        const V  d = Norm(m.R.Col(1));
        a_P[0] = static_cast<float>(m.t.x), a_P[1] = static_cast<float>(m.t.y), a_P[2] = static_cast<float>(m.t.z);
        a_D[0] = static_cast<float>(d.x), a_D[1] = static_cast<float>(d.y), a_D[2] = static_cast<float>(d.z);
        ++g_st.aimUsed;
        g_st.P = m.t;
        g_st.D = d;
        return true;
    }

    bool PipboyBoneOffset(float a_offGrip[3])
    {
        std::scoped_lock l(g_lock);
        if (!g_on.load() || !g_pipValid) return false;
        a_offGrip[0] = g_pipGrip[0], a_offGrip[1] = g_pipGrip[1], a_offGrip[2] = g_pipGrip[2];
        return true;
    }

    bool SuppressGunLowering()
    {
        if (!g_on.load()) return false;
        std::scoped_lock l(g_lock);
        return g_cfg.noGunLowering && g_lastActive &&
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_lastActiveT).count() <= kFreshMs;
    }

    void OnPresent()
    {
        if (!g_notifyHooked) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        Rig         rig;
        const char* why = nullptr;
        const bool  haveRig = player && player->GetParentCell() && GetRig(rig, why);
        Xf          cam;
        const bool  haveCam = CamRootWorld(cam);
        const auto  base = Base();
        const V     off = NiV(*reinterpret_cast<const RE::NiPoint3*>(base + kFpOffset));
        std::scoped_lock l(g_lock);
        if (haveRig && rig.fire) {
            const Xf f = FromNi(rig.fire->world);
            g_st.fireAtPresent = f.t + off;
            if (g_st.aimUsed > 0) {
                g_st.pFireDist = Len(g_st.P - g_st.fireAtPresent);
                g_st.dFireDeg = std::acos(std::clamp(Dot(g_st.D, Norm(f.R.Col(1))), -1.0, 1.0)) * kR2D;
            }
        }
        auto& p = g_probe;
        if (p.framesLeft <= 0) return;
        --p.framesLeft;
        ++p.frames;
        p.tC = QpcUs();
        if (p.tA > 0 && p.tB > 0) {
            // order of the last hook (A), pre-Swap (B) and this Present (C)
            if (p.tA < p.tB && p.tB < p.tC) ++p.orderABC;
            else if (p.tA < p.tC && p.tC < p.tB) ++p.orderACB;
            else ++p.orderOther;
            // does the hook (job thread) run while Main::Swap renders?
            if (p.tBend > p.tB) {
                if (p.tA0 < p.tBend && p.tA > p.tB) ++p.hookDuringSwap;
                else if (p.tA0 >= p.tBend) ++p.hookAfterSwap;
                else ++p.hookBeforeSwap;
            }
        }
        if (haveRig && p.aFresh) {
            const Xf c = FromNi(rig.weapon->world);
            p.maxAC = (std::max)(p.maxAC, Len(c.t - p.a.t));
            p.maxRotAC = (std::max)(p.maxRotAC, MaxAbsDiff(c.R, p.a.R));
            ++p.nAC;
            if (haveCam) p.maxCamAC = (std::max)(p.maxCamAC, Len(cam.t - p.camA));
            p.tidC = ::GetCurrentThreadId();
        }
        p.aFresh = false;
        if (haveRig && haveCam) {
            const V loc = NiV(player->data.location);
            const V armLoc = NiV(*reinterpret_cast<const RE::NiPoint3*>(base + kFpCameraLoc));
            const auto* armCam = *reinterpret_cast<RE::NiAVObject* const*>(base + kFpCamera);
            const V camRel = cam.t - loc;
            p.lastArmLoc = armLoc;
            p.lastCamRel = camRel;
            p.lastOff = off;
            p.lastLoc = loc;
            p.maxArmLoc = (std::max)(p.maxArmLoc, Len(camRel - armLoc));
            p.maxOffLoc = (std::max)(p.maxOffLoc, Len(off - loc));
            if (armCam) {
                p.lastArmNode = NiV(armCam->world.translate);
                p.maxArmCamNode = (std::max)(p.maxArmCamNode, Len(camRel - p.lastArmNode));
            }
            ++p.nArm;
        }
        if (p.framesLeft == 0) {
            REX::INFO("hands probe: frames={} WEAPON hook->Swap max {:.4f} u / rot {:.5f} (n {}), hook->Present max {:.4f} u / rot {:.5f} (n {}); "
                      "camRoot hook->Present {:.4f}; tids hook {} swap {} present {}; arms cam: |(cam-loc)-3E622B8| max {:.4f}, |(cam-loc)-armCam.world| max {:.4f}, "
                      "|34484E0-loc| max {:.4f} (n {}) last cam-loc {} 3E622B8 {} armCam {} off {} loc {}; order A<B<C {} A<C<B {} other {}; hook vs Swap: during {} after {} before {}",
                p.frames, p.maxAB, p.maxRotAB, p.nAB, p.maxAC, p.maxRotAC, p.nAC, p.maxCamAC, p.tidA, p.tidB, p.tidC, p.maxArmLoc, p.maxArmCamNode,
                p.maxOffLoc, p.nArm, F(p.lastCamRel, 3), F(p.lastArmLoc, 3), F(p.lastArmNode, 3), F(p.lastOff, 3), F(p.lastLoc, 3), p.orderABC,
                p.orderACB, p.orderOther, p.hookDuringSwap, p.hookAfterSwap, p.hookBeforeSwap);
        }
    }

    namespace
    {
        std::string Readout()
        {
            std::scoped_lock l(g_lock);
            const auto& s = g_st;
            const auto& p = g_probe;
            const auto& c = g_cfg;
            return std::format(
                "hands on={} W1={} W2={} gate='{}' ws={} weapon='{}' j={} anchor={}{} bones={} calls={} active={} restores={} snapAgeMs={:.0f} "
                "tgt={} act={} err={:.3f} angErr={:.3f} shift={:.2f} rot={:.2f} wWorld={} loc={} cam={} before={} "
                "fire={} fireFwd={} fireAtPresent={} P={} D={} pFire={:.3f} dFireDeg={:.3f} aim={}/{} "
                "costUs={:.1f} avg={:.1f} max={:.1f} fwdInGrip={} upInGrip={} aimInGrip={} comRewritten={} comPersisted={} "
                "cfg off=({},{},{}) rot=({},{},{}) align={} muzzle={} noLower={} "
                "probe left={} n={} AB={:.4f}/{:.5f} AC={:.4f}/{:.5f} camAC={:.4f} armLoc={:.4f} armNode={:.4f} offLoc={:.4f} tids={}/{}/{} nAB={} nAC={} order={}/{}/{}",
                g_on.load() ? 1 : 0, g_notifyHooked ? "ok" : "no", g_swapHooked ? "ok" : "no", s.gate, s.weaponState, s.weapon, s.jmode, s.anchor,
                s.anchorNode ? "" : "(no node)", s.boneCount, s.calls, s.active, s.restores, s.snapAgeMs, F(s.tgt, 3), F(s.act, 3), s.posErr, s.angErr,
                s.shift, s.rot, F(s.actWorld, 3), F(s.loc, 2), F(s.camHook, 3), F(s.before, 3), F(s.fireWorld, 3), F(s.fireFwd, 4),
                F(s.fireAtPresent, 3), F(s.P, 3), F(s.D, 4), s.pFireDist, s.dFireDeg, s.aimUsed, s.aimCalls, s.costUs, s.costAvgUs, s.costMaxUs,
                F(s.fwdInGrip, 4), F(s.upInGrip, 4), F(s.aimInGrip, 4), s.comRewritten, s.comPersisted, c.offCm[0], c.offCm[1], c.offCm[2],
                c.rotDeg[0], c.rotDeg[1], c.rotDeg[2], c.align, c.aimFromMuzzle ? 1 : 0, c.noGunLowering ? 1 : 0, p.framesLeft, p.frames, p.maxAB,
                p.maxRotAB, p.maxAC, p.maxRotAC, p.maxCamAC, p.maxArmLoc, p.maxArmCamNode, p.maxOffLoc, p.tidA, p.tidB, p.tidC, p.nAB, p.nAC,
                p.orderABC, p.orderACB, p.orderOther);
        }

        // "hands ik": stage-2 line (player-relative rig space unless noted; lengths in game units)
        std::string ReadoutIk()
        {
            std::scoped_lock l(g_lock);
            const auto& s = g_st;
            const auto& c = g_cfg;
            auto arm = [](const char* p, const Stats::Arm& a) {
                return std::format("{0}Solved={1} {0}Tgt={2} {0}Hand={3} {0}Err={4:.3f} {0}AngErr={5:.3f} {0}Elbow={6} {0}Shoulder={7} {0}Clav={8} "
                                   "{0}Stretch={9:.4f} {0}Reach={10:.2f} {0}Dist={11:.2f} {0}Upper={12:.2f} {0}Fore={13:.2f} {0}ElbowBelow={14:.2f} {0}Twist={15:.1f}",
                    p, a.solved ? 1 : 0, F(a.tgt, 3), F(a.hand, 3), a.err, a.angErr, F(a.elbow, 3), F(a.shoulder, 3), F(a.clav, 3), a.stretch, a.reach,
                    a.dist, a.upperLen, a.foreLen, a.elbowBelow, a.twistDeg);
            };
            return std::format(
                "hands ik={} ikFrames={} gate='{}' fkErr={:.5f} body={} neck={} bodyYaw={:.2f} rootYaw={:.2f} {} {} offMode={} gripDist={:.2f} barrelLR={:.3f} "
                "twoHandedEnters={} offRel={} offRelValid={} shots={} ammo={} recT={:.3f} recR={:.3f} recWin={} recMaxT={:.3f} recMaxR={:.3f} "
                "kickT={:.3f} kickR={:.3f} kickMaxT={:.3f} kickMaxR={:.3f} "
                "flex=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) fingerDeg={:.2f} fingerDeg2={:.2f} fingersWritten={} pipValid={} pipGrip={} pipRef={} pipRig={} "
                "gripRef={} offCtlRef={} offCtlRig={} gunHandRef={} "
                "ikPersisted={} ikRewritten={} ikCostUs={:.1f} ikAvg={:.1f} ikMax={:.1f} err={:.3f} angErr={:.3f} "
                "cIk={} cTwoHanded={} cRecoil={} cFingers={} cPipBone={} cNeck=({},{}) cBody=({},{}) cArmLen={} cMaxStretch={} cSnap={} cRelease={} cRec=({},{},{},{}) cLift={}",
                s.ik, s.ikFrames, s.gate, s.fkErr, F(s.body, 3), F(s.neck, 3), s.bodyYawDeg, s.rootYawDeg, arm("r", s.arm[0]), arm("l", s.arm[1]), s.offMode,
                s.gripDist, s.barrelLR, s.twoHandedEnters, F(s.offRelT, 2), s.offRelValid ? 1 : 0, s.shots, s.ammo, s.recT, s.recR, s.recWindow ? 1 : 0,
                s.recMaxT, s.recMaxR, s.kickT, s.kickR, s.kickMaxT, s.kickMaxR, s.flex[0], s.flex[1], s.flex[2], s.flex[3], s.flex[4], s.fingerDeg, s.fingerDeg2, s.fingersWritten, s.pipValid ? 1 : 0,
                F(s.pipGrip, 4), F(s.pipRef, 4), F(s.pipRig, 3), F(s.gripRef, 4), F(s.offCtlRef, 4), F(s.offCtlRig, 3), F(s.gunHandRef, 4), s.ikPersisted, s.ikRewritten, s.ikCostUs, s.ikCostAvgUs, s.ikCostMaxUs, s.posErr,
                s.angErr, c.armIK, c.twoHanded, c.keepRecoil, c.fingers, c.pipboyOnBone, c.neckFwdCm, c.neckUpCm, c.bodyFwdCm, c.bodyUpCm, c.armLength,
                c.maxStretch, c.snapCm, c.releaseCm, c.recoilScale, c.recoilWindowMs, c.recoilMaxCm, c.recoilMaxDeg, c.pipboyLiftCm);
        }
    }

    std::string Command(const std::string& a_arg)
    {
        std::istringstream is(a_arg);
        std::string        verb;
        is >> verb;
        if (verb.empty()) return Readout();
        if (verb == "ik") return ReadoutIk();
        if (verb == "on" || verb == "off") {
            if (!g_notifyHooked) return "hands: not installed (bWeaponInHand=0 at load or byte check failed)";
            g_on = verb == "on";
            return std::string("hands ") + verb;
        }
        if (verb == "probe") {
            int n = 120;
            is >> n;
            std::scoped_lock l(g_lock);
            g_probe = Probe{};
            g_probe.framesLeft = (std::max)(1, n);
            return std::format("hands probe: {} frames (result in log and 'hands' readout)", g_probe.framesLeft);
        }
        if (verb == "dump") {
            {
                std::scoped_lock l(g_lock);
                g_dumpReq.clear();
            }
            g_dumpPending = true;
            for (int i = 0; i < 50; ++i) {
                ::Sleep(20);
                std::scoped_lock l(g_lock);
                if (!g_dumpReq.empty()) return "hands dump: " + g_dumpReq;
            }
            g_dumpPending = false;
            return "hands dump: no frame ran the hook (not in game?)";
        }
        if (verb == "cfg") {
            std::string k;
            double      v = 0;
            is >> k >> v;
            std::scoped_lock l(g_lock);
            auto& c = g_cfg;
            if (k == "offx") c.offCm[0] = static_cast<float>(v);
            else if (k == "offy") c.offCm[1] = static_cast<float>(v);
            else if (k == "offz") c.offCm[2] = static_cast<float>(v);
            else if (k == "rotx") c.rotDeg[0] = static_cast<float>(v);
            else if (k == "roty") c.rotDeg[1] = static_cast<float>(v);
            else if (k == "rotz") c.rotDeg[2] = static_cast<float>(v);
            else if (k == "align") c.align = static_cast<int>(v);
            else if (k == "muzzle") c.aimFromMuzzle = v != 0;
            else if (k == "nolower") c.noGunLowering = v != 0;
            else if (k == "resetmax") { g_st.costMaxUs = 0; g_st.costN = 0; g_st.ikCostMaxUs = 0; g_st.recMaxT = 0; g_st.recMaxR = 0; g_st.kickMaxT = 0; g_st.kickMaxR = 0; }
            else if (k == "ik") c.armIK = v != 0;
            else if (k == "twohanded") c.twoHanded = v != 0;
            else if (k == "recoil") c.keepRecoil = v != 0;
            else if (k == "fingers") c.fingers = v != 0;
            else if (k == "pipbone") c.pipboyOnBone = v != 0;
            else if (k == "neckfwd") c.neckFwdCm = static_cast<float>(v);
            else if (k == "neckup") c.neckUpCm = static_cast<float>(v);
            else if (k == "bodyfwd") c.bodyFwdCm = static_cast<float>(v);
            else if (k == "bodyup") c.bodyUpCm = static_cast<float>(v);
            else if (k == "armlen") c.armLength = static_cast<float>(v);
            else if (k == "maxstretch") c.maxStretch = static_cast<float>(v);
            else if (k == "snap") c.snapCm = static_cast<float>(v);
            else if (k == "release") c.releaseCm = static_cast<float>(v);
            else if (k == "gripmax") c.gripMaxDist = static_cast<float>(v);
            else if (k == "recscale") c.recoilScale = static_cast<float>(v);
            else if (k == "recwin") c.recoilWindowMs = static_cast<float>(v);
            else if (k == "recmaxcm") c.recoilMaxCm = static_cast<float>(v);
            else if (k == "recmaxdeg") c.recoilMaxDeg = static_cast<float>(v);
            else if (k == "lift") c.pipboyLiftCm = static_cast<float>(v);
            else return "error: hands cfg offx|offy|offz|rotx|roty|rotz|align|muzzle|nolower|resetmax|ik|twohanded|recoil|fingers|pipbone|neckfwd|neckup|"
                        "bodyfwd|bodyup|armlen|maxstretch|snap|release|gripmax|recscale|recwin|recmaxcm|recmaxdeg|lift <v>";
            return std::format("hands cfg {} = {}", k, v);
        }
        return "error: hands [on|off|ik|probe [frames]|dump|cfg <key> <value>]";
    }
}
