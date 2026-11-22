// Floating gun (FO4VR baseline). See FloatingGun.h and analysis/vr-hands-weapon.md ("FO4VR baseline").
// Derived from the retired stage-1 weapon-in-hand code (plugin/src/plugin/_retired/Hands.cpp): same controller chain,
// FRIK glue and per-weapon JSONs, but only WEAPON's own local is written (the arm rig is not moved) and the
// first-person body/arm geometry is culled instead of posed.
#include "plugin/FloatingGun.h"
#include "plugin/FloatingGunInternal.h"
#include "plugin/ConfigStore.h"
#include "plugin/EngineCull.h"  // 22 Nov: hides / shows through the engine's SetAppCulled
#include "plugin/GameBridge.h"
#include "plugin/HeadPose.h"
#include "plugin/Settings.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR

#include "plugin/CallChain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

namespace fo4flatvr::gun
{
    namespace
    {
        // ---- 1.11.240 RVAs (analysis/vr-hands-weapon.md) --------------------------------------------------------
        constexpr std::uintptr_t kNotifyCallSite = 0xC351F3;   // Main::OnIdle_UpdatePlayer -> PlayerUpdateEvent Notify
        constexpr std::uintptr_t kNotify = 0xC403B0;
        constexpr std::uintptr_t kUpdateNode = 0xD9F7D0;       // (float time, NiAVObject*, NiCamera*, bool) -> NiAVObject::Update
        constexpr std::uintptr_t kQ1stPersonCamera = 0x21FA470;
        constexpr std::uintptr_t kFrameTime = 0x30E8940;       // float passed by UpdateScenegraph to D9F7D0
        constexpr std::uintptr_t kFpOffset = 0x34484E0;        // NiPoint3: 1st-person world offset (Main::Swap)
        constexpr std::uintptr_t kVatsSingleton = 0x30F9FD0;   // VATS*, mode at +0x40
        constexpr std::uintptr_t kActorInPowerArmor = 0x9893F0;
        // A1: DrawWorld::Add1stPersonGeomToCuller 21F1FD0 adds every child of the first-person root to the culling group
        // (call 21F2051 E8 DA DF 5E FF -> BSCullingGroup::Add 17E0030) without testing AppCulled; FO4VR's version (VR
        // 142842BC0) skips children with AppCulled set. A1 puts that test back, so culled biped parts are not drawn.
        constexpr std::uintptr_t kCullAddCallSite = 0x21F2051;
        constexpr std::uintptr_t kCullAdd = 0x17E0030;

        constexpr long long kFreshMs = 200;
        constexpr double    kR2D = 57.29577951308232;

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        // ---- math: column-vector rigid transforms, v' = s R v + t -------------------------------------------------
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

        // ---- constants of the chain (analysis/vr-hands-weapon.md section 2) ---------------------------------------
        // FRIK glue G (Skeleton::placeFirstPersonArm): storage rows = the rotation in FRIK's 10mm.json.
        const double kGRows[9] = { -0.122, 0.990, 0.069, 0.987, 0.114, 0.109, 0.100, 0.081, -0.992 };
        const V      kGT{ 4.389, -1.899, -3.133 };
        // FO4VR PrimaryWeaponToWandNode: translate (fWeaponOffset_X/Y/Z:VR) = (0, -4, 1), rotation 0.
        const V      kWandToOffset{ 0.0, -4.0, 1.0 };
        // Skeleton bind of WEAPON in RArm_Hand (skeleton.nif): J for weapons without a JSON when auto-align is off.
        const double kBindRows[9] = { -0.994, 0.028, -0.110, 0.109, -0.011, -0.994, -0.029, -1.0, 0.008 };
        const V      kBindT{ 6.744, -3.485, 0.176 };

        // ---- config ----------------------------------------------------------------------------------------------
        struct Cfg
        {
            bool  enabled{ true };        // [Weapon] bFloatingGun
            bool  hideArms{ true };       // [Weapon] bHideArms
            bool  aimFromMuzzle{ true };  // bAimFromMuzzle
            bool  noGunLowering{ true };  // bNoGunLowering
            int   align{ 0 };             // iAlign: 0 = FRIK JSON (auto-align without one), 1 = always auto-align, 2 = JSON/bind only
            float offCm[3]{ 0.0f, 0.0f, 0.0f };     // fGripOffsetX/Y/Z (grip frame: right, forward, up; cm)
            float rotDeg[3]{ -60.0f, 0.0f, 0.0f };  // fGripRotX/Y/ZDeg; -60 = barrel along the SteamVR Touch aim pose
            int   gripAnchor{ 1 };            // iGripAnchor: 1 = the weapon's handle (grip mod bound centre) at the controller grip
                                              // origin; 0 = WEAPON origin from the FO4VR wand offset + FRIK glue chain
            float handleOffCm[3]{ 0.0f, 0.0f, 0.0f };  // fHandleOffsetX/Y/Z: handle centre in the grip frame (right, forward, up; cm)
            float unitsPerMeter{ 70.0f };
            std::string jsonDir;
            bool  kickback{ true };                    // bUseKickback (FO4VR bUseKickback:VR)
            float kickOff[2]{ 2.0f, 20.0f };           // fKickbackMinOffset / MaxOffset (units)
            float kickDur[2]{ 0.25f, 2.8f };           // fKickbackMinDuration / MaxDuration (s)
            bool  recoilRot{ true };                   // bUseRecoil (FO4VR bUseRecoil:VR): aim-model recoil rotates the gun
            bool  meleeNode{ true };                   // bMeleeNode: melee weapons on FO4VR's PrimaryMeleeWeaponOffsetNode
            bool  inPowerArmor{ true };                // bInPowerArmor: the gun floats in power armour too
            bool  ctrlModels{ true };                  // bControllerModels
            std::string ctrlPath[2];                   // sControllerModelRight / Left (0 = right, 1 = left)
            float ctrlOffCm[3]{ 0.0f, 0.0f, 0.0f };    // fControllerModelOffsetX/Y/Z (wand frame, cm)
            float ctrlRotDeg[3]{ 0.0f, 0.0f, 0.0f };   // fControllerModelRotX/Y/ZDeg
            float ctrlScale{ 1.0f };                   // fControllerModelScale
        };

        struct Stats
        {
            std::uint64_t calls{ 0 }, active{ 0 }, restores{ 0 };
            const char*   gate{ "init" };
            double        costUs{ 0 }, costAvgUs{ 0 }, costMaxUs{ 0 };
            std::uint64_t costN{ 0 };
            std::string   weapon{ "-" };
            const char*   jmode{ "-" };
            V             tgt, act, actWorld, before;
            double        posErr{ 0 }, angErr{ 0 }, shift{ 0 }, rot{ 0 };
            V             fireRel, fireWorld, fireFwd;
            bool          fireValid{ false };
            V             camHook, loc;
            int           weaponState{ -1 };
            V             fwdInGrip, aimInGrip, upInGrip;   // calibration aids in the grip frame
            V             handleInGripCm, originInGripCm;   // after placement: handle centre / WEAPON origin in the grip frame (cm)
            std::string   handleNode{ "-" };
            bool          melee{ false }, inPA{ false };
            bool          handleValid{ false };
            std::uint64_t written{ 0 }, persisted{ 0 }, rewritten{ 0 };  // WEAPON local: kept / rewritten by the graph next frame
            V             handVsWeapon;                     // RArm_Hand world - WEAPON world (units): the arm is left where the animation put it
            double        snapAgeMs{ 0 };
            std::uint64_t snapSeq{ 0 };
            // arms
            int           armsCandidates{ 0 }, armsCulled{ 0 };
            bool          armsHidden{ false };
            std::string   armsNames;
            // P/D (Present)
            std::uint64_t aimCalls{ 0 }, aimUsed{ 0 };
            V             P, D;
            double        pFireDist{ -1 }, dFireDeg{ -1 };
            V             fireAtPresent;
        };

        std::mutex        g_lock;
        Cfg               g_cfg;
        detail::Snap      g_snap;
        Stats             g_st;
        std::atomic<bool> g_on{ false };
        std::atomic<bool> g_aside{ false };      // FRIK-flat owns the weapon
        std::atomic<bool> g_armsOn{ true };      // pipe "gun arms on|off" (test aid; ini bHideArms gates it too)
        bool              g_hooked = false, g_fnOk = false, g_paOk = false;
        std::string       g_hookWhy;
        std::unordered_map<std::string, Xf> g_json;  // lower-case weapon name (+ "-powerarmor" ...) -> J
        std::string       g_dumpReq;
        std::atomic<bool> g_dumpPending{ false };

        // published by the hook for AimFromWeapon (guarded by g_lock)
        bool   g_lastActive = false;
        std::chrono::steady_clock::time_point g_lastActiveT{};
        Xf     g_F;  // WEAPON -> fire node (WEAPON-local)
        bool   g_FValid = false;
        Xf     g_J;
        int    g_jKind = 0;  // 1 json, 2 bind, 3 auto
        V      g_handleLocal;  // handle centre in WEAPON-local (FindHandle), for AimFromWeapon
        bool   g_handleOk = false;
        bool   g_melee = false;
        double g_wScale = 1.0;

        // WEAPON local override bookkeeping (hook thread only)
        bool                     g_wrote = false;
        RE::NiTransform          g_written{};
        RE::NiTransform          g_saved{};  // animated local before our first write (graphs that do not rewrite it)
        bool                     g_haveSaved = false;
        RE::NiNode*              g_writtenNode = nullptr;
        int                      g_writtenIdx = -1;
        RE::BSFlattenedBoneTree* g_writtenTree = nullptr;

        // arm hiding (hook thread only)
        std::vector<RE::NiAVObject*> g_culled;  // objects whose AppCulled bit we set (bit was clear)
        // race diagnostic (pipe "gun race"): player-relative WEAPON world before our write (animated) and our target
        std::atomic<float> g_raceAnim[3]{}, g_raceTgt[3]{};
        std::atomic<std::uint64_t> g_raceHook{ 0 };
        std::atomic<RE::NiNode*>   g_raceWeapon{ nullptr };
        std::atomic<RE::NiAVObject*> g_raceTree{ nullptr };
        std::atomic<std::uint64_t> g_rtTgt{ 0 }, g_rtAnim{ 0 }, g_rtOther{ 0 };  // WEAPON state when the 1st-person pass is collected
        std::atomic<std::uint64_t> g_rtFrame{ 0 };
        RE::NiAVObject*              g_culledRoot = nullptr;

        // FO4VR kickback / recoil (VR EF96B0 on fire, EFD1C0 / EFD0B0 per frame on PrimaryWeaponKickbackRecoilNode):
        // translate (0, -amp (1 - t/dur)^2, 0), rotate FromEulerAnglesZXY(aim+0x70, -aim+0x74, 0). Hook thread writes,
        // AimFromWeapon reads (g_lock).
        struct Kick
        {
            double        amp{ 0 }, dur{ 0 }, t{ 0 }, d{ 0 };
            bool          on{ false };
            M3            rot;
            double        rotDeg{ 0 };
            std::uint64_t launches{ 0 }, kicks{ 0 };
            double        lastK{ 0 }, maxD{ 0 };
            int           kFrames{ 0 };     // W1 frames since the shot over which k is still taken (max)
            double        r68Prev{ 0 };
        };
        Kick   g_kick;
        double g_kickTUs = 0;

        // controller stand-ins (hook thread only; index 0 = right controller, 1 = left)
        struct CtrlModel
        {
            RE::NiPointer<RE::NiNode> node;
            bool                      tried{ false };
            bool                      shown{ false };
            std::string               status{ "not loaded" };
        };
        CtrlModel                  g_ctrl[2];
        std::atomic<std::uint64_t> g_ctrlShownFrames[2]{};

        using Notify_t = int (*)(void*, void*);
        using UpdateNode_t = void (*)(float, RE::NiAVObject*, void*, bool);
        using QCam_t = void* (*)();
        using ActorBool_t = bool (*)(RE::Actor*);
        Notify_t g_origNotify = nullptr;
        using CullAdd_t = void (*)(void*, RE::NiAVObject*, void*, std::uint32_t);
        CullAdd_t                  g_origCullAdd = nullptr;
        bool                       g_cullHooked = false;
        std::atomic<std::uint64_t> g_cullSkips{ 0 }, g_cullAdds{ 0 };
        // arm hiding state shared with A1 (render thread) and the diagnostics ([Weapon] bArmsDiagLog)
        std::atomic<bool>          g_hideActive{ false };   // the last W1 decided to hide the arms
        std::atomic<std::uint64_t> g_cullTypeSkips{ 0 };    // skinned root child skipped although its AppCulled bit was clear
        std::atomic<std::uint64_t> g_flagClears{ 0 }, g_newParts{ 0 }, g_staleFrames{ 0 };
        std::atomic<std::uint64_t> g_collects{ 0 }, g_collectInWindow{ 0 }, g_collectDuringJob{ 0 };
        std::atomic<double>        g_tAnimPre{ 0 }, g_tFpPost{ 0 }, g_tGunExit{ 0 };
        bool                       g_diagLog = false;
        std::atomic<std::uint64_t> g_hookCount{ 0 };
        std::atomic<int>           g_trIdxHand{ -1 };

        // A1 (render thread, Main::Swap -> DrawWorld): FO4VR's test - a first-person root child with AppCulled is not added
        void SampleRenderState()
        {
            auto* w = g_raceWeapon.load();
            if (!w) return;
            const auto& p = w->world.translate;
            const float dt = std::hypot(p.x - g_raceTgt[0].load(), p.y - g_raceTgt[1].load(), p.z - g_raceTgt[2].load());
            const float da = std::hypot(p.x - g_raceAnim[0].load(), p.y - g_raceAnim[1].load(), p.z - g_raceAnim[2].load());
            (dt < 1.0f ? g_rtTgt : da < 3.0f ? g_rtAnim : g_rtOther)++;
        }

        // A biped body / armour part: skinned geometry, or a node whose subtree holds skinned geometry (outfits such as the
        // Drifter outfit are attached as a NiNode wrapping several skinned shapes; the Vault suit as one shape).
        bool HasSkinned(RE::NiAVObject* a_o, int a_depth)
        {
            if (!a_o) return false;
            if (auto* geom = a_o->IsGeometry()) return geom->skinInstance != nullptr;
            if (a_depth <= 0) return false;
            if (auto* n = a_o->IsNode()) {
                for (auto& c : n->children) {
                    if (HasSkinned(c.get(), a_depth - 1)) return true;
                }
            }
            return false;
        }

        // rig alternation diagnostics (render collect): RArm_Hand / first-person root / camera root steps that reverse
        std::atomic<std::uint64_t> g_altHand{ 0 }, g_altRoot{ 0 }, g_altCam{ 0 }, g_altFpCam{ 0 }, g_altFpOff{ 0 };
        std::atomic<double>        g_altHandAmp{ 0 };

        void TraceSample(char a_where);  // per-frame transform trace (pipe "gun trace"), defined below
        void TraceEvent(std::uint16_t a_site);
        void TraceSetRig(RE::NiAVObject* a_root, RE::BSFlattenedBoneTree* a_tree);

        void HkCullAdd(void* a_group, RE::NiAVObject* a_obj, void* a_bound, std::uint32_t a_flags)
        {
            if (a_obj && a_obj == g_raceTree.load()) {  // once per frame: the skeleton child
                SampleRenderState();
                TraceSample('R');
                ++g_collects;
                if (g_diagLog) {
                    struct P3 { float x{}, y{}, z{}; };
                    static P3   lastH, lastR, lastC, dH, dR, dC, lastF, lastO, dF, dO;
                    static bool have = false;
                    auto* tree = static_cast<RE::BSFlattenedBoneTree*>(g_raceTree.load());
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    auto* cam = RE::PlayerCamera::GetSingleton();
                    const int hi = g_trIdxHand.load();
                    if (tree && pl && pl->firstPerson3D && cam && cam->cameraRoot && hi >= 0 && hi < tree->boneCount) {
                        const auto& b = tree->bone[hi];
                        const auto& th = b.node ? b.node->world.translate : b.world.translate;
                        const auto& tr = pl->firstPerson3D->world.translate;
                        const auto& tc = cam->cameraRoot->world.translate;
                        const P3 h{ th.x, th.y, th.z }, r{ tr.x, tr.y, tr.z }, c{ tc.x, tc.y, tc.z };
                        const auto* fl = reinterpret_cast<const RE::NiPoint3*>(Base() + 0x3E622B8);  // 1st-person camera location
                        const auto* fo = reinterpret_cast<const RE::NiPoint3*>(Base() + 0x34484E0);  // 1st-person world offset
                        const P3 f{ fl->x, fl->y, fl->z }, o{ fo->x, fo->y, fo->z };
                        auto step = [](const P3& a, const P3& b) { return P3{ b.x - a.x, b.y - a.y, b.z - a.z }; };
                        auto len = [](const P3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
                        auto rev = [&](const P3& d0, const P3& d1) { return len(d0) > 2.0f && len(d1) > 2.0f && d0.x * d1.x + d0.y * d1.y + d0.z * d1.z < 0.0f; };
                        if (have) {
                            const P3 nh = step(lastH, h), nr = step(lastR, r), nc = step(lastC, c);
                            if (rev(dH, nh)) ++g_altHand, g_altHandAmp = len(nh);
                            if (rev(dR, nr)) ++g_altRoot;
                            if (rev(dC, nc)) ++g_altCam;
                            const P3 nf = step(lastF, f), no = step(lastO, o);
                            if (rev(dF, nf)) ++g_altFpCam;
                            if (rev(dO, no)) ++g_altFpOff;
                            dH = nh, dR = nr, dC = nc, dF = nf, dO = no;
                        }
                        lastH = h, lastR = r, lastC = c, lastF = f, lastO = o;
                        have = true;
                    }
                }
                const double tAnim = g_tAnimPre.load(), tFp = g_tFpPost.load(), tGun = g_tGunExit.load();
                if (tAnim > tGun) ++g_collectDuringJob;      // the next frame's player update had started, W1 not yet run
                if (tFp > tGun) ++g_collectInWindow;          // ... and the engine had already rewritten the rig (animated pose)
            }
            if (a_obj && (a_obj->flags.flags & 1)) {
                ++g_cullSkips;
                return;
            }
            // while the arms are hidden, a skinned body/armour part is never added, whatever its AppCulled bit says (the
            // game may clear the bit or rebuild the part between our W1 and this collect)
            if (a_obj && g_hideActive.load(std::memory_order_relaxed) && a_obj != static_cast<RE::NiAVObject*>(g_raceTree.load()) && HasSkinned(a_obj, 6)) {
                ++g_cullTypeSkips;
                ++g_cullSkips;
                return;
            }
            ++g_cullAdds;
            g_origCullAdd(a_group, a_obj, a_bound, a_flags);
        }

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

        // ---- FRIK weapon offset JSONs (read-only copies in Data/F4SE/Plugins/fo4flatvr_weapons) ---------------------
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
                REX::WARN("gun: weapon offsets dir '{}' not found (auto-align / bind only)", a_dir);
                return 0;
            }
            for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
                if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".json") continue;
                std::ifstream     f(e.path(), std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string s = ss.str();
                const auto q0 = s.find('"');
                const auto q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
                const auto rot = s.find("\"rotation\"");
                const auto br = rot == std::string::npos ? rot : s.find('[', rot);
                if (q1 == std::string::npos || br == std::string::npos) continue;
                const std::string name = s.substr(q0 + 1, q1 - q0 - 1);
                double      v12[12]{};
                const char* p = s.c_str() + br + 1;
                bool        ok = true;
                for (int i = 0; i < 12 && ok; ++i) {
                    char* end = nullptr;
                    v12[i] = std::strtod(p, &end);
                    ok = end != p;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
                }
                double x, y, z;
                if (!ok || !ParseNumberAfter(s, br, "x", x) || !ParseNumberAfter(s, br, "y", y) || !ParseNumberAfter(s, br, "z", z)) {
                    REX::WARN("gun: could not parse {}", e.path().string());
                    continue;
                }
                const double rows[9] = { v12[0], v12[1], v12[2], v12[4], v12[5], v12[6], v12[8], v12[9], v12[10] };
                Xf j = FromRows(rows, { x, y, z });
                j.s = 1.0;  // rigid: FRIK's VR scale is not applied
                g_json[Lower(name)] = j;
                ++n;
            }
            return n;
        }

        // ---- game access -------------------------------------------------------------------------------------------
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

        struct Rig
        {
            RE::PlayerCharacter*            player{ nullptr };
            RE::NiNode*                     root{ nullptr };
            RE::BSFlattenedBoneTree*        tree{ nullptr };
            int                             iWeapon{ -1 }, iHand{ -1 };
            RE::NiNode*                     weapon{ nullptr };
            RE::NiAVObject*                 fire{ nullptr };
            RE::NiNode*                     pipBone{ nullptr };
            const std::unordered_set<const void*>* boneNodes{ nullptr };
        };

        bool GetRig(Rig& r, const char*& a_why)
        {
            r.player = RE::PlayerCharacter::GetSingleton();
            if (!r.player) { a_why = "no player"; return false; }
            r.root = r.player->firstPerson3D.get();
            if (!r.root) { a_why = "no firstPerson3D"; return false; }
            r.tree = FindTree(r.root, 3);
            if (!r.tree || !r.tree->bone || r.tree->boneCount <= 0) { a_why = "no flattened bone tree"; return false; }
            static RE::BSFlattenedBoneTree*        cachedTree = nullptr;
            static int                             cachedCount = -1, cWeapon = -1, cHand = -1, cPip = -1;
            static std::unordered_set<const void*> cBoneNodes;
            if (cachedTree != r.tree || cachedCount != r.tree->boneCount) {
                cachedTree = r.tree;
                cachedCount = r.tree->boneCount;
                cWeapon = BoneIndex(r.tree, "WEAPON");
                cHand = BoneIndex(r.tree, "RArm_Hand");
                cPip = BoneIndex(r.tree, "PipboyBone");
                cBoneNodes.clear();
                for (int i = 0; i < r.tree->boneCount; ++i) {
                    if (r.tree->bone[i].node) cBoneNodes.insert(r.tree->bone[i].node.get());
                }
            }
            r.iWeapon = cWeapon, r.iHand = cHand;
            r.boneNodes = &cBoneNodes;
            r.pipBone = cPip >= 0 && r.tree->bone[cPip].node ? r.tree->bone[cPip].node.get() : nullptr;
            if (r.iWeapon < 0) { a_why = "WEAPON bone missing"; return false; }
            r.weapon = r.tree->bone[r.iWeapon].node.get();
            if (!r.weapon) { a_why = "WEAPON has no node"; return false; }
            r.fire = FindByName(r.weapon, "ProjectileNode", 10);
            return true;
        }

        // ActorState (Actor+0x128) +0xC bits 1..3 = WEAPON_STATE (3 = drawn), read raw.
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

        // Target WEAPON world from the camera root world (any frame: world or player-relative) and the hand relative
        // to the head the camera was built from. a_kind: 1 json, 2 bind, 3 auto-align.
        Xf TargetWeapon(const Xf& a_cam, const detail::HandRel& a_h, const Cfg& a_c, const Xf& a_J, int a_kind, const Xf& a_F, bool a_FValid,
            double a_scale, Xf* a_grip = nullptr, const V* a_handleLocal = nullptr, const Kick* a_kick = nullptr, bool a_melee = false)
        {
            const Xf H = Mul(a_cam, HandXf(a_h, a_c.unitsPerMeter));
            if (a_grip) *a_grip = H;
            const Xf wand = Mul(H, CalibC(a_c));
            if (a_melee) {
                // FO4VR melee: WEAPON.world = PrimaryMeleeWeaponOffsetNode = wand . T(fMeleeWeaponOffset 0,-4,0), no rotation,
                // no kickback node (VR BuildVRNodes EF21A0, Update1stPersonArms EF5BE0 for weapon type <= 6)
                Xf m = Mul(wand, Xf{ M3{}, V{ 0.0, -4.0, 0.0 }, 1.0 });
                m.s = a_scale;
                return m;
            }
            Xf       off;
            off.t = kWandToOffset;
            static const Xf Ginv = Inv(FromRows(kGRows, kGT));
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
            if (a_c.gripAnchor == 1 && a_handleLocal) {
                // keep the rotation, translate so the handle centre sits at the grip origin (+ fHandleOffset)
                const V target = Mul(H, Xf{ M3{}, V{ a_c.handleOffCm[0], a_c.handleOffCm[1], a_c.handleOffCm[2] } * (0.01 * a_c.unitsPerMeter), 1.0 }).t;
                w.t = target - Mul(w.R, *a_handleLocal) * w.s;
            }
            if (a_kick && (a_kick->d != 0.0 || a_kick->rotDeg != 0.0)) {
                // the kickback node (child of PrimaryWeaponToWandNode = wand . T(0,-4,1)) moves everything below it
                const Xf K = Mul(wand, Xf{ M3{}, kWandToOffset, 1.0 });
                Xf       k{ a_kick->rot, V{ 0.0, -a_kick->d, 0.0 }, 1.0 };
                const Xf ws = w;
                w = Mul(Mul(Mul(K, k), Inv(K)), ws);
                w.s = ws.s;
            }
            return w;
        }

        // The weapon's handle: the grip mod node (10mm '10mmGrip', .44 '44MagGrips', ...; attached at the receiver's P-Grip
        // connect point) - its world bound centre. Nodes named like the grip mods only, depth-first under WEAPON.
        RE::NiAVObject* FindHandle(RE::NiAVObject* a_o, int a_depth)
        {
            if (!a_o || a_depth < 0) return nullptr;
            if (const char* nm = a_o->name.c_str(); nm && a_o->worldBound.fRadius > 0.0f) {
                std::string l = Lower(nm);
                if (l.find("grip") != std::string::npos && l.find("p-grip") == std::string::npos) return a_o;
            }
            if (auto* n = a_o->IsNode()) {
                for (auto& c : n->children) {
                    if (auto* r = FindHandle(c.get(), a_depth - 1)) return r;
                }
            }
            return nullptr;
        }

        void RunFirstPersonUpdate(RE::NiAVObject* a_root)
        {
            const float ft = *reinterpret_cast<const float*>(Base() + kFrameTime);
            reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(ft, a_root, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
        }

        // ---- arms: cull the first-person biped geometry (body, arms, hands, Pip-Boy, gloves) -----------------------
        // The biped models are attached as skinned BSGeometry directly under the firstPerson3D root (BipedAnim::
        // AttachSkinnedObject); only those are culled. Unskinned children (muzzle flash 'MuzPistolSmall01' BSFadeNode,
        // magic/torch nodes) and the skeleton itself stay; the weapon model hangs under the WEAPON bone.
        // a_out: candidates / culled now, names of the candidates.
        void ApplyArms(const Rig& r, bool a_hide, int& a_candidates, int& a_culled, std::string* a_names)
        {
            if (g_culledRoot != r.root) {
                g_culled.clear();  // a new rig: the old objects are not ours to touch any more
                g_culledRoot = r.root;
            }
            a_candidates = a_culled = 0;
            auto visit = [&](RE::NiNode* a_parent) {
                if (!a_parent) return;
                for (auto& c : a_parent->children) {
                    RE::NiAVObject* o = c.get();
                    if (!o || o == r.tree || r.boneNodes->contains(o)) continue;
                    if (!HasSkinned(o, 6)) continue;  // only body / armour parts (skinned, possibly wrapped in a node)
                    ++a_candidates;
                    if (a_names && a_names->size() < 400) {
                        const auto* rt = o->GetRTTI();
                        *a_names += std::format("{}'{}':{}{} ", a_names->empty() ? "" : "", o->name.c_str() ? o->name.c_str() : "", rt ? rt->GetName() : "?",
                            o->GetAppCulled() ? "(culled)" : "");
                    }
                    const bool ours = std::find(g_culled.begin(), g_culled.end(), o) != g_culled.end();
                    if (a_hide) {
                        if (!o->GetAppCulled()) {
                            if (ours) {
                                ++g_flagClears;  // someone cleared the bit we set
                                if (g_diagLog) REX::INFO("arms diag: AppCulled cleared by the game on '{}' (frame {})", o->name.c_str() ? o->name.c_str() : "", g_hookCount.load());
                            } else {
                                ++g_newParts;    // a part we had not seen (new equipment / rebuilt biped)
                                if (g_diagLog) REX::INFO("arms diag: new skinned part '{}' (frame {})", o->name.c_str() ? o->name.c_str() : "", g_hookCount.load());
                            }
                            enginecull::Set(o, true);
                            if (!ours) g_culled.push_back(o);
                        }
                        ++a_culled;
                    } else if (ours) {
                        enginecull::Set(o, false);
                    }
                }
            };
            visit(r.root);
            if (static_cast<RE::NiAVObject*>(r.tree) != r.root) visit(r.tree);
            // the first-person Pip-Boy model hangs (unskinned) under the PipboyBone bone; FO4VR shows its own on the wand,
            // here the wrist quad ([Pipboy]) replaces it
            if (r.pipBone) {
                for (auto& c : r.pipBone->children) {
                    RE::NiAVObject* o = c.get();
                    if (!o || r.boneNodes->contains(o)) continue;
                    ++a_candidates;
                    if (a_names && a_names->size() < 400) *a_names += std::format("'{}':PipboyBone{} ", o->name.c_str() ? o->name.c_str() : "", o->GetAppCulled() ? "(culled)" : "");
                    const bool ours = std::find(g_culled.begin(), g_culled.end(), o) != g_culled.end();
                    if (a_hide) {
                        if (!o->GetAppCulled()) {
                            enginecull::Set(o, true);
                            if (!ours) g_culled.push_back(o);
                        }
                        ++a_culled;
                    } else if (ours) {
                        enginecull::Set(o, false);
                    }
                }
            }
            if (!a_hide) g_culled.clear();
        }

        // ---- WEAPON local override -------------------------------------------------------------------------------
        // The node we wrote is touched only if it is still the current rig's WEAPON (a cell change or 3D reload may
        // have freed it), and only when the graph did not rewrite the local itself.
        void RestoreIfNeeded(bool a_update, RE::NiNode* a_node, RE::BSFlattenedBoneTree* a_tree, RE::NiAVObject* a_root)
        {
            if (!g_wrote || !g_writtenNode || g_writtenNode != a_node || g_writtenTree != a_tree) {
                g_wrote = false;
                g_haveSaved = false;
                return;
            }
            if (std::memcmp(&g_writtenNode->local, &g_written, sizeof(RE::NiTransform)) == 0 && g_haveSaved) {
                g_writtenNode->local = g_saved;
                if (g_writtenTree && g_writtenIdx >= 0 && g_writtenIdx < g_writtenTree->boneCount) {
                    g_writtenTree->bone[g_writtenIdx].local = g_saved;
                }
                if (a_update && g_fnOk && a_root) RunFirstPersonUpdate(a_root);
                std::scoped_lock l(g_lock);
                ++g_st.restores;
            }
            g_wrote = false;
            g_haveSaved = false;
        }

        std::string DumpRig(const Rig& r)
        {
            std::string rootKids, treeKids;
            auto list = [&](RE::NiNode* p, std::string& out) {
                if (!p) return;
                for (auto& c : p->children) {
                    if (!c) continue;
                    const auto* rt = c->GetRTTI();
                    const auto& wt = c->world.translate;
                    out += std::format("'{}':{}{}{}@({:.1f},{:.1f},{:.1f}) ", c->name.c_str() ? c->name.c_str() : "", rt ? rt->GetName() : "?",
                        r.boneNodes->contains(c.get()) ? "[bone]" : "", c->GetAppCulled() ? "(culled)" : "", wt.x, wt.y, wt.z);
                }
            };
            list(r.root, rootKids);
            if (static_cast<RE::NiAVObject*>(r.tree) != r.root) list(r.tree, treeKids);
            std::string attached;  // non-bone children of bone nodes = attached models
            for (int i = 0; i < r.tree->boneCount; ++i) {
                auto* bn = r.tree->bone[i].node.get();
                if (!bn) continue;
                for (auto& c : bn->children) {
                    if (c && !r.boneNodes->contains(c.get())) {
                        attached += std::format("{}/'{}'{} ", r.tree->bone[i].name.c_str() ? r.tree->bone[i].name.c_str() : "?", c->name.c_str() ? c->name.c_str() : "",
                            c->GetAppCulled() ? "(culled)" : "");
                    }
                }
            }
            // every geometry under the first-person root that would be drawn (no culled ancestor), outside the WEAPON subtree
            std::string visible;
            int         nVisible = 0, nWeaponGeom = 0;
            std::function<void(RE::NiAVObject*, bool)> walk = [&](RE::NiAVObject* o, bool inWeapon) {
                if (!o || (o->flags.flags & 1)) return;
                inWeapon = inWeapon || o == static_cast<RE::NiAVObject*>(r.weapon);
                if (o->IsGeometry()) {
                    if (inWeapon) {
                        ++nWeaponGeom;
                    } else {
                        ++nVisible;
                        if (visible.size() < 600) visible += std::format("'{}'@'{}' ", o->name.c_str() ? o->name.c_str() : "", o->parent && o->parent->name.c_str() ? o->parent->name.c_str() : "");
                    }
                }
                if (auto* n = o->IsNode()) {
                    for (auto& c : n->children) walk(c.get(), inWeapon);
                }
            };
            walk(r.root, false);
            std::string weaponKids = std::format("visibleGeomOutsideWeapon={} [{}] weaponGeom={} | ", nVisible, visible, nWeaponGeom);
            for (auto& c : r.weapon->children) {
                if (c) weaponKids += std::format("'{}' ", c->name.c_str() ? c->name.c_str() : "");
            }
            const auto out = std::format("bones={} tree='{}' treeIsRoot={} rootKids=[{}] treeKids=[{}] attached=[{}] weaponParent='{}' weaponKids=[{}] fire={}@({:.1f},{:.1f},{:.1f}) rootAt=({:.1f},{:.1f},{:.1f})",
                r.tree->boneCount, r.tree->name.c_str() ? r.tree->name.c_str() : "", static_cast<RE::NiAVObject*>(r.tree) == r.root ? 1 : 0, rootKids,
                treeKids, attached, r.weapon->parent && r.weapon->parent->name.c_str() ? r.weapon->parent->name.c_str() : "-", weaponKids,
                r.fire ? r.fire->name.c_str() : "none", r.fire ? r.fire->world.translate.x : 0.0f, r.fire ? r.fire->world.translate.y : 0.0f,
                r.fire ? r.fire->world.translate.z : 0.0f, r.root->world.translate.x, r.root->world.translate.y, r.root->world.translate.z);
            REX::INFO("gun dump: {}", out);
            return out;
        }

        // ---- controller stand-ins (FO4VR: the controller render model, fallback Data/Meshes/Vive_Wand.nif, on each wand
        // node; VR 140C61D80). Here: one NIF per controller under the first-person root, placed on the FO4VR wand frame
        // (grip . fGripRot) every W1, hidden with AppCulled (A1 skips culled root children).
        void LoadCtrl(int a_i, const Cfg& a_c)
        {
            auto& m = g_ctrl[a_i];
            if (m.tried) return;
            m.tried = true;
            const std::string& path = a_c.ctrlPath[a_i];
            std::error_code ec;
            if (path.empty() || !std::filesystem::exists(path, ec)) {
                m.status = "missing file '" + path + "' (run tools/install_controller_models.py)";
                REX::WARN("gun: controller model {}: {}", a_i == 0 ? "right" : "left", m.status);
                return;
            }
            RE::BSModelDB::DBTraits::ArgsType args{};
            std::memset(&args, 0, sizeof(args));
            reinterpret_cast<std::uint8_t*>(&args)[8] = 0xED;  // prepareAfterLoad, useErrorMarker, performProcess, loadTextures (as FRIK's loader)
            RE::NiPointer<RE::NiNode> n;
            const auto err = RE::BSModelDB::Demand(path.c_str(), &n, args);
            if (!n) {
                m.status = std::format("load failed (error {})", static_cast<int>(err));
                REX::ERROR("gun: controller model {} '{}': {}", a_i == 0 ? "right" : "left", path, m.status);
                return;
            }
            enginecull::Set(n.get(), true);  // hidden until placed
            m.node = n;
            m.status = "loaded";
            REX::INFO("gun: controller model {} '{}' loaded (bound radius {:.1f})", a_i == 0 ? "right" : "left", path, n->worldBound.fRadius);
        }

        Xf CtrlXf(const Cfg& a_c)
        {
            constexpr double d2r = 3.14159265358979 / 180.0;
            Xf x;
            x.R = Mul(Rz(a_c.ctrlRotDeg[2] * d2r), Mul(Ry(a_c.ctrlRotDeg[1] * d2r), Rx(a_c.ctrlRotDeg[0] * d2r)));
            x.t = V{ a_c.ctrlOffCm[0], a_c.ctrlOffCm[1], a_c.ctrlOffCm[2] } * (0.01 * a_c.unitsPerMeter);
            // FO4VR's Vive_Wand.nif is an OpenVR render model in metres (bound radius 0.11): scale by fUnitsPerMeter
            x.s = (a_c.ctrlScale > 0.0f ? a_c.ctrlScale : 1.0) * a_c.unitsPerMeter;
            return x;
        }

        // a_show: draw it this frame; a_camRel: camera root in the rig's (player-relative) space
        void PlaceCtrl(int a_i, const Rig& r, bool a_show, const Xf& a_camRel, const detail::HandRel& a_h, const Cfg& a_c)
        {
            auto& m = g_ctrl[a_i];
            RE::NiNode* n = m.node.get();
            if (!n) return;
            if (n->parent != r.root) {
                if (n->parent) n->parent->DetachChild(n);
                r.root->AttachChild(n, true);
            }
            if (!a_show || !a_h.valid) {
                enginecull::Set(n, true);
                m.shown = false;
                return;
            }
            const Xf wand = Mul(Mul(a_camRel, HandXf(a_h, a_c.unitsPerMeter)), CalibC(a_c));
            const Xf tgt = Mul(wand, CtrlXf(a_c));
            Xf       local = Mul(Inv(FromNi(r.root->world)), tgt);
            local.R = Ortho(local.R);
            ToNi(local, n->local);
            enginecull::Set(n, false);
            const float ft = *reinterpret_cast<const float*>(Base() + kFrameTime);
            reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(ft, n, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
            m.shown = true;
            ++g_ctrlShownFrames[a_i];
        }

        // ---- the frame (hook thread) ---------------------------------------------------------------------------------
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
            const auto   now = std::chrono::steady_clock::now();
            const double ageMs = std::chrono::duration<double, std::milli>(now - snap.t).count();
            const bool   xrFresh = snap.seq > 0 && ageMs <= kFreshMs;

            // arms: hidden whenever a headset feeds the plugin (FO4VR shows no first-person arms at all)
            int         armsCand = 0, armsCulled = 0;
            std::string armsNames;
            // hide while a headset feeds the plugin; a snapshot is "alive" for 2 s (a hitch must not show the arms)
            const bool xrAlive = snap.seq > 0 && ageMs <= 2000.0;
            if (!xrFresh && xrAlive) ++g_staleFrames;
            const bool hideArms = g_on.load() && cfg.hideArms && g_armsOn.load() && xrAlive;
            g_hideActive = hideArms;
            if (haveRig) {
                ApplyArms(rig, hideArms, armsCand, armsCulled, &armsNames);
                g_raceWeapon = rig.weapon;
                g_raceTree = rig.tree;
                TraceSetRig(rig.root, rig.tree);
            }

            // persistence probe: does the graph rewrite WEAPON's local every frame?
            if (g_wrote && g_writtenNode && haveRig && g_writtenTree == rig.tree && g_writtenNode == rig.weapon) {
                const bool persisted = std::memcmp(&g_writtenNode->local, &g_written, sizeof(RE::NiTransform)) == 0;
                std::scoped_lock l(g_lock);
                (persisted ? g_st.persisted : g_st.rewritten)++;
            }

            Xf        cam;
            const int cs = CamState();
            if (!haveRig) {
            } else if (!g_on.load()) {
                why = "off";
            } else if (g_aside.load()) {
                why = "FRIK owns the weapon";
            } else if (cs != 0 && cs != 4) {
                why = "camera state";
            } else if (!snap.worldView) {
                why = "no world view (menu/state)";
            } else if (!snap.rNew.valid || !xrFresh) {
                why = "no fresh controller snapshot";
            } else if (VatsActive()) {
                why = "vats";
            } else if (g_paOk && !cfg.inPowerArmor && reinterpret_cast<ActorBool_t>(Base() + kActorInPowerArmor)(rig.player)) {
                why = "power armour";
            } else if (WeaponState(rig.player) != 3) {
                why = "weapon not drawn";
            } else if (!CamRootWorld(cam)) {
                why = "no camera root";
            } else if (!g_fnOk) {
                why = "update function check failed";
            } else if (!rig.weapon->parent) {
                why = "WEAPON has no parent";
            }
            if (why) {
                RestoreIfNeeded(true, haveRig ? rig.weapon : nullptr, haveRig ? rig.tree : nullptr, haveRig ? rig.root : nullptr);
            }
            const bool active = !why;

            // weapon form + J
            std::string wname = "-";
            bool        melee = false, inPA = false;
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
                // weapon type byte (TESObjectWEAP +0x2CF, as GameBridge F1): 0..6 = melee / hand to hand
                const std::uint8_t wtype = form ? reinterpret_cast<const std::uint8_t*>(form)[0x2CF] : 0xFF;
                melee = cfg.meleeNode && form && wtype <= 6;
                inPA = g_paOk && reinterpret_cast<ActorBool_t>(Base() + kActorInPowerArmor)(rig.player);
                if (!lastName.empty() && cfg.align != 1) {
                    std::scoped_lock l(g_lock);
                    auto it = inPA ? g_json.find(Lower(lastName) + "-powerarmor") : g_json.end();
                    if (it == g_json.end()) it = g_json.find(Lower(lastName));
                    if (it != g_json.end()) {
                        J = it->second;
                        jkind = 1;
                    }
                }
            }
            // F = WEAPON^-1 . fireNode (the model's own transform; unchanged by a move of WEAPON)
            Xf   Fx;
            bool fOk = false;
            if (haveRig && rig.fire) {
                Fx = Mul(Inv(FromNi(rig.weapon->world)), FromNi(rig.fire->world));
                fOk = true;
            }
            if (jkind != 1 && fOk && cfg.align != 2) jkind = 3;

            Xf     before, tgt, after, gripH;
            double posErr = 0, angErr = 0, shift = 0, rotDeg = 0;
            V      loc, handVsWeapon;
            V           handleLocal;
            bool        handleOk = false;
            std::string handleName = "-";
            RE::NiAVObject* handleObj = nullptr;
            if (haveRig) {
                loc = NiV(rig.player->data.location);
                before = FromNi(rig.weapon->world);
                if ((handleObj = FindHandle(rig.weapon, 8))) {
                    const auto& c = handleObj->worldBound.center;
                    handleLocal = Mul(T(before.R), V{ c.x, c.y, c.z } - before.t) * (1.0 / before.s);  // WEAPON-local
                    handleOk = Len(handleLocal) < 60.0;
                    handleName = handleObj->name.c_str() ? handleObj->name.c_str() : "?";
                }
            }
            // FO4VR kickback / recoil rotation (on fire: amplitude and duration from the aim model's recoil)
            Kick kick;
            {
                const double tNow = QpcUs();
                const double dt = g_kickTUs > 0 ? std::clamp((tNow - g_kickTUs) * 1e-6, 0.0, 0.1) : 0.0;
                g_kickTUs = tNow;
                float am[4]{};
                const bool amOk = game::PlayerAimModel(am);
                const std::uint64_t launches = game::PlayerLaunches();
                std::scoped_lock l(g_lock);
                Kick& k = g_kick;
                // k (FO4VR EF96B0, at the shot): |len(recoil target +0x68) - len(spring +0x70)| . 4/pi. Here the shot's
                // ApplyShotRecoil may land after Launch, so k is the max over the shot's first 3 W1 frames of that
                // difference and of the target's jump [I: approximation of the FO4VR timing]
                const double r68 = std::hypot(am[0], am[1]), r70 = std::hypot(am[2], am[3]);
                if (launches != k.launches) {
                    if (active && !melee && cfg.kickback && launches > k.launches) {
                        k.t = 0, k.on = true, k.lastK = 0, k.kFrames = 3;
                        ++k.kicks;
                    }
                    k.launches = launches;
                }
                if (k.on && k.kFrames > 0 && amOk) {
                    --k.kFrames;
                    const double kk = std::clamp((std::max)(std::abs(r68 - r70), std::abs(r68 - k.r68Prev)) * 4.0 / 3.14159265358979, 0.0, 1.0);
                    k.lastK = (std::max)(k.lastK, kk);
                }
                k.amp = cfg.kickOff[0] + (cfg.kickOff[1] - cfg.kickOff[0]) * k.lastK;
                k.dur = cfg.kickDur[0] + (cfg.kickDur[1] - cfg.kickDur[0]) * k.lastK;
                if (amOk) k.r68Prev = r68;
                k.d = 0;
                if (k.on) {
                    k.t += dt;
                    if (k.t >= k.dur || !active || melee) {
                        k.on = false;
                    } else {
                        const double f = 1.0 - k.t / k.dur;
                        k.d = std::clamp(f * f * k.amp, 0.0, k.amp);
                        k.maxD = (std::max)(k.maxD, k.d);
                    }
                }
                k.rot = M3{};
                k.rotDeg = 0;
                if (cfg.recoilRot && amOk && active && !melee) {
                    // FromEulerAnglesZXY(aim+0x70, -aim+0x74, 0) in FO4VR; the storage/column convention flips the sign here:
                    // measured 14 Nov, Rx(-am74) pitched the 10mm's muzzle DOWN by 1 deg per shot, FO4VR's kicks up
                    k.rot = Mul(Rz(am[2]), Rx(am[3]));
                    k.rotDeg = AngleDeg(k.rot, M3{});
                }
                kick = k;
            }
            if (active) {
                const double s = before.s;
                Xf           camRel = cam;
                camRel.t = cam.t - loc;  // the rig lives in player-relative space
                tgt = TargetWeapon(camRel, snap.rNew, cfg, J, jkind, Fx, fOk, s, &gripH, handleOk ? &handleLocal : nullptr, &kick, melee);
                tgt.s = s;
                RE::NiNode* w = rig.weapon;
                const Xf    pw = FromNi(w->parent->world);
                Xf          wl = Mul(Inv(pw), tgt);
                wl.R = Ortho(wl.R);
                wl.s = w->local.scale;
                // save the animated local before the first write (graphs that do not rewrite it every frame)
                const bool rewritten = !(g_wrote && g_writtenNode == w && std::memcmp(&w->local, &g_written, sizeof(RE::NiTransform)) == 0);
                if (rewritten || !g_haveSaved) {
                    g_saved = w->local;
                    g_haveSaved = true;
                }
                ToNi(wl, w->local);
                rig.tree->bone[rig.iWeapon].local = w->local;
                g_written = w->local;
                g_writtenNode = w;
                g_writtenIdx = rig.iWeapon;
                g_writtenTree = rig.tree;
                g_wrote = true;
                RunFirstPersonUpdate(rig.root);
                TraceEvent(6);
                after = FromNi(w->world);
                posErr = Len(after.t - tgt.t);
                angErr = AngleDeg(after.R, tgt.R);
                shift = Len(after.t - before.t);
                rotDeg = AngleDeg(after.R, before.R);
                if (rig.iHand >= 0) handVsWeapon = NiV(rig.tree->bone[rig.iHand].world.translate) - after.t;
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
            // controller stand-ins: off hand always, primary hand while no weapon model is placed
            if (haveRig && cfg.ctrlModels && g_cullHooked && g_fnOk) {
                const int pi = snap.leftHanded ? 1 : 0, oi = 1 - pi;
                LoadCtrl(0, cfg), LoadCtrl(1, cfg);
                bool gunModel = false;
                if (active) {
                    for (auto& c : rig.weapon->children) {
                        if (c && !rig.boneNodes->contains(c.get())) { gunModel = true; break; }
                    }
                }
                Xf         camC;
                const bool camOk = CamRootWorld(camC);
                const bool base = g_on.load() && !g_aside.load() && camOk && (cs == 0 || cs == 4) && snap.worldView && xrFresh && !VatsActive();
                camC.t = camC.t - NiV(rig.player->data.location);
                PlaceCtrl(pi, rig, base && !gunModel, camC, snap.rNew, cfg);
                PlaceCtrl(oi, rig, base, camC, snap.oNew, cfg);
            } else {
                for (auto& m : g_ctrl) {
                    if (m.node) enginecull::Set(m.node.get(), true), m.shown = false;
                }
            }
            const double t1 = QpcUs();
            Xf           camW;
            const bool   haveCam = CamRootWorld(camW);
            std::scoped_lock l(g_lock);
            auto& st = g_st;
            st.gate = why ? why : "active";
            st.weapon = wname;
            st.handleNode = handleName;
            st.handleValid = handleOk;
            st.jmode = !active ? "-" : jkind == 1 ? "json" : jkind == 3 ? "auto" : "bind";
            st.snapSeq = snap.seq;
            st.snapAgeMs = ageMs;
            st.loc = loc;
            st.before = before.t;
            st.weaponState = haveRig ? WeaponState(rig.player) : -1;
            st.armsCandidates = armsCand;
            st.armsCulled = armsCulled;
            st.armsHidden = hideArms;
            st.armsNames = armsNames;
            if (haveCam) st.camHook = camW.t;
            if (active) {
                ++st.active;
                ++st.written;
                st.tgt = tgt.t;
                st.act = after.t;
                st.posErr = posErr;
                st.angErr = angErr;
                st.shift = shift;
                st.rot = rotDeg;
                st.handVsWeapon = handVsWeapon;
                st.costUs = t1 - t0;
                st.costAvgUs = st.costN == 0 ? st.costUs : st.costAvgUs * 0.98 + st.costUs * 0.02;
                st.costMaxUs = (std::max)(st.costMaxUs, st.costUs);
                ++st.costN;
                if (fireValid) {
                    st.fwdInGrip = Mul(T(gripH.R), fireFwd);
                    st.upInGrip = Mul(T(gripH.R), Norm(FromNi(rig.fire->world).R.Col(2)));
                }
                const Xf hx = HandXf(snap.rNew, 1.0);
                st.aimInGrip = Mul(T(hx.R), V{ snap.rNew.aimDir[0], snap.rNew.aimDir[1], snap.rNew.aimDir[2] });
                const double toCm = 100.0 / cfg.unitsPerMeter;
                st.originInGripCm = Mul(T(gripH.R), after.t - gripH.t) * toCm;
                if (handleOk) st.handleInGripCm = Mul(T(gripH.R), Mul(after, Xf{ M3{}, handleLocal, 1.0 }).t - gripH.t) * toCm;
            }
            st.actWorld = after.t + loc;
            st.fireValid = fireValid;
            st.fireRel = fireRel;
            st.fireWorld = fireRel + loc;
            st.fireFwd = fireFwd;
            if (active) {
                for (int i = 0; i < 3; ++i) {
                    g_raceAnim[i] = static_cast<float>(i == 0 ? before.t.x : i == 1 ? before.t.y : before.t.z);
                    g_raceTgt[i] = static_cast<float>(i == 0 ? tgt.t.x : i == 1 ? tgt.t.y : tgt.t.z);
                }
                ++g_raceHook;
            }
            g_lastActive = active;
            if (active) g_lastActiveT = now;
            g_F = Fx;
            g_FValid = fOk;
            g_J = J;
            g_jKind = jkind;
            g_wScale = before.s;
            g_handleLocal = handleLocal;
            g_handleOk = handleOk;
            g_melee = melee;
            st.melee = melee;
            st.inPA = inPA;
        }

        // ---- per-event trace (pipe "gun trace <frames> <csv>") --------------------------------------------------------
        // Every write/read site of the first-person rig we can see logs an event into a lock-free ring: QPC time, thread,
        // frame index (W1 hook count), Present count, site, and the values (rig space = player-relative, cam = world).
        // Sites: 1/2 animation update (PlayerCharacter vtbl 0x4F8 = D61DD0) pre/post, 3/4 the engine's first-person
        // NiAVObject::Update (call D6236C -> D9F7D0) pre/post, 5 W1 entry, 6 after our WEAPON write + update, 7 W1 exit,
        // 8 render thread first-person collect (A1, skeleton child), 9 Present, 10 watcher thread saw a change.
        struct TraceEv
        {
            double        tUs;
            std::uint32_t tid;
            std::uint16_t site;
            std::uint16_t culledMask;
            std::uint64_t frame, present;
            float         wW[3], wL[3], rh[3], lh[3], rUp[3], lUp[3], cam[3];
        };
        constexpr std::size_t                 kRing = 1u << 17;
        TraceEv*                              g_ring = nullptr;
        std::atomic<std::uint64_t>            g_ringHead{ 0 };
        std::atomic<bool>                     g_traceOn{ false };
        std::atomic<std::uint64_t>            g_presentCount{ 0 };
        std::atomic<int>                      g_trIdx[5]{ -1, -1, -1, -1, -1 };  // WEAPON, RArm_Hand, LArm_Hand, RArm_UpperArm, LArm_UpperArm
        std::atomic<RE::BSFlattenedBoneTree*> g_trTree{ nullptr };
        std::atomic<RE::NiAVObject*>          g_trRoot{ nullptr };
        const char* kSiteNames[11] = { "?", "anim_pre", "anim_post", "fpupdate_pre", "fpupdate_post", "gun_entry", "gun_write", "gun_exit",
            "render_collect", "present", "watch_change" };

        void TraceSetRig(RE::NiAVObject* a_root, RE::BSFlattenedBoneTree* a_tree)
        {
            if (g_trTree.load() != a_tree) {
                const char* names[5] = { "WEAPON", "RArm_Hand", "LArm_Hand", "RArm_UpperArm", "LArm_UpperArm" };
                for (int i = 0; i < 5; ++i) g_trIdx[i] = a_tree ? BoneIndex(a_tree, names[i]) : -1;
                g_trIdxHand = g_trIdx[1].load();
                g_trTree = a_tree;
            }
            g_trRoot = a_root;
        }

        void Fill(TraceEv& e)
        {
            auto* tree = g_trTree.load();
            auto  get = [&](int k, float o[3], bool a_local) {
                const int i = g_trIdx[k].load();
                if (!tree || i < 0 || i >= tree->boneCount) return;
                const auto& b = tree->bone[i];
                const auto& t = a_local ? (b.node ? b.node->local.translate : b.local.translate) : (b.node ? b.node->world.translate : b.world.translate);
                o[0] = t.x, o[1] = t.y, o[2] = t.z;
            };
            get(0, e.wW, false), get(0, e.wL, true), get(1, e.rh, false), get(2, e.lh, false), get(3, e.rUp, false), get(4, e.lUp, false);
            if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
                const auto& t = cam->cameraRoot->world.translate;
                e.cam[0] = t.x, e.cam[1] = t.y, e.cam[2] = t.z;
            }
            if (auto* root = g_trRoot.load(); root && root->IsNode()) {
                std::uint16_t bit = 0;
                for (auto& c : root->IsNode()->children) {
                    if (!c || c.get() == static_cast<RE::NiAVObject*>(tree)) continue;
                    if (c->flags.flags & 1) e.culledMask |= static_cast<std::uint16_t>(1u << bit);
                    if (++bit >= 16) break;
                }
            }
        }

        void TraceEvent(std::uint16_t a_site)
        {
            if (!g_traceOn.load(std::memory_order_relaxed) || !g_ring) return;
            const std::uint64_t i = g_ringHead.fetch_add(1, std::memory_order_relaxed);
            TraceEv             e{};
            e.tUs = QpcUs();
            e.tid = ::GetCurrentThreadId();
            e.site = a_site;
            e.frame = g_hookCount.load(std::memory_order_relaxed);
            e.present = g_presentCount.load(std::memory_order_relaxed);
            Fill(e);
            g_ring[i & (kRing - 1)] = e;
        }
        void TraceSample(char a_where) { TraceEvent(static_cast<std::uint16_t>(a_where == 'R' ? 8 : a_where == 'P' ? 9 : 0)); }

        // diagnostic hooks (installed by the first "gun trace")
        using AnimUpd_t = void (*)(void*, float);
        using UpdNode_t = void (*)(float, RE::NiAVObject*, void*, bool);
        AnimUpd_t g_origAnim = nullptr;
        UpdNode_t g_origFpUpd = nullptr;
        bool      g_traceHooks = false;
        void      HkAnim(void* a_this, float a_dt)
        {
            const bool me = a_this == RE::PlayerCharacter::GetSingleton();
            if (me) TraceEvent(1), g_tAnimPre = QpcUs();
            g_origAnim(a_this, a_dt);
            if (me) TraceEvent(2);
        }
        void HkFpUpd(float a_t, RE::NiAVObject* a_obj, void* a_cam, bool a_b)
        {
            TraceEvent(3);
            g_origFpUpd(a_t, a_obj, a_cam, a_b);
            TraceEvent(4);
            g_tFpPost = QpcUs();
        }
        std::string InstallTraceHooks()
        {
            if (g_traceHooks) return "ok";
            const auto base = Base();
            // call D6236C in PlayerCharacter::UpdateScenegraph D61FD0: E8 5F D4 03 00 -> D9F7D0 (first-person 3D update)
            static constexpr std::uint8_t kCall[5] = { 0xE8, 0x5F, 0xD4, 0x03, 0x00 };
            if (std::memcmp(reinterpret_cast<const void*>(base + 0xD6236C), kCall, 5) != 0) return "D6236C bytes differ";
            // the live PlayerCharacter's vtable, slot 0x4F8 (Main::OnIdle_UpdatePlayer C35018: call [vtbl+0x4F8] = UpdateAnimation)
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return "no player";
            auto* slot = reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(player) + 0x4F8);
            REX::INFO("gun trace: player vtable {:X} slot 0x4F8 -> {:X}", *reinterpret_cast<std::uintptr_t*>(player) - base, *slot - base);
            auto& tramp = REL::GetTrampoline();
            g_origFpUpd = reinterpret_cast<UpdNode_t>(tramp.write_call<5>(base + 0xD6236C, &HkFpUpd));
            g_origAnim = reinterpret_cast<AnimUpd_t>(*slot);
            DWORD old = 0;
            ::VirtualProtect(slot, 8, PAGE_EXECUTE_READWRITE, &old);
            reinterpret_cast<std::atomic<std::uintptr_t>*>(slot)->store(reinterpret_cast<std::uintptr_t>(&HkAnim));
            ::VirtualProtect(slot, 8, old, &old);
            g_traceHooks = true;
            REX::INFO("gun trace: diagnostic hooks installed (call D6236C, PlayerCharacter vtbl 0x4F8)");
            return "ok";
        }

        int HkNotify(void* a_src, void* a_evt)
        {
            ++g_hookCount;
            if (g_diagLog && !g_traceHooks) {  // [Weapon] bArmsDiagLog: the timing hooks go in once a player exists
                static bool tried = false;
                if (!tried) {
                    tried = true;
                    REX::INFO("arms diag: timing hooks {}", InstallTraceHooks());
                }
            }
            // [General] bPoseSync (HeadPose.h): this job-thread hook must use the head pose / controller snapshot of the
            // camera being built, not the previous Present's (both are published together in the Presenter)
            head::WaitForPublish(6);
            TraceEvent(5);
            Frame();
            TraceEvent(7);
            g_tGunExit = QpcUs();
            return g_origNotify(a_src, a_evt);
        }

        bool BytesMatch(std::uintptr_t a_rva, const std::uint8_t* a_expect, std::size_t a_n, const char* a_what)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_rva);
            if (std::memcmp(p, a_expect, a_n) == 0) return true;
            std::string got;
            for (std::size_t i = 0; i < a_n; ++i) got += std::format("{:02X} ", p[i]);
            REX::ERROR("gun: {} at {:X} has bytes [{}]; not patching", a_what, a_rva, got);
            return false;
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

    namespace
    {
        void ReadCfg(Cfg& c, bool a_load)
        {
        const auto path = Settings::Path();
        // [Weapon] key, else the pre-retirement [Hands] key (the user's test config), else the default
        auto rd = [&](const char* k, float d, bool a_legacy = true) {
            for (const char* sect : { "Weapon", "Hands" }) {
                if (!a_legacy && std::strcmp(sect, "Hands") == 0) break;
                char buf[64]{};
                ::GetPrivateProfileStringA(sect, k, "", buf, sizeof(buf), path.c_str());
                char*       end = nullptr;
                const float v = std::strtof(buf, &end);
                if (end != buf) return v;
            }
            return d;
        };
        c.enabled = rd("bFloatingGun", 1.0f, false) != 0.0f;
        c.hideArms = rd("bHideArms", 1.0f, false) != 0.0f;
        g_diagLog = rd("bArmsDiagLog", 0.0f, false) != 0.0f;
        c.aimFromMuzzle = rd("bAimFromMuzzle", 1.0f) != 0.0f;
        c.noGunLowering = rd("bNoGunLowering", 1.0f) != 0.0f;
        c.align = static_cast<int>(rd("iAlign", 0.0f));
        c.gripAnchor = static_cast<int>(rd("iGripAnchor", 1.0f, false));
        c.handleOffCm[0] = rd("fHandleOffsetX", 0.0f, false);
        c.handleOffCm[1] = rd("fHandleOffsetY", 0.0f, false);
        c.handleOffCm[2] = rd("fHandleOffsetZ", 0.0f, false);
        const char* ok[3] = { "fGripOffsetX", "fGripOffsetY", "fGripOffsetZ" };
        const char* rk[3] = { "fGripRotXDeg", "fGripRotYDeg", "fGripRotZDeg" };
        for (int i = 0; i < 3; ++i) {
            c.offCm[i] = rd(ok[i], c.offCm[i]);
            c.rotDeg[i] = rd(rk[i], c.rotDeg[i]);
        }
        {
            char buf[64]{};
            ::GetPrivateProfileStringA("General", "fUnitsPerMeter", "70", buf, sizeof(buf), path.c_str());
            const float u = std::strtof(buf, nullptr);
            c.unitsPerMeter = u > 1.0f ? u : 70.0f;
            char dir[512]{};
            ::GetPrivateProfileStringA("Weapon", "sWeaponOffsetsDir", "", dir, sizeof(dir), path.c_str());
            if (!dir[0]) ::GetPrivateProfileStringA("Hands", "sWeaponOffsetsDir", "", dir, sizeof(dir), path.c_str());
            c.jsonDir = dir[0] ? dir : std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr_weapons").string();
        }
        c.ctrlModels = rd("bControllerModels", 1.0f, false) != 0.0f;
        c.kickback = rd("bUseKickback", 1.0f, false) != 0.0f;
        c.kickOff[0] = rd("fKickbackMinOffset", 2.0f, false);
        c.kickOff[1] = rd("fKickbackMaxOffset", 20.0f, false);
        c.kickDur[0] = rd("fKickbackMinDuration", 0.25f, false);
        c.kickDur[1] = rd("fKickbackMaxDuration", 2.8f, false);
        c.recoilRot = rd("bUseRecoil", 1.0f, false) != 0.0f;
        c.meleeNode = rd("bMeleeNode", 1.0f, false) != 0.0f;
        c.inPowerArmor = rd("bInPowerArmor", 1.0f, false) != 0.0f;
        // head authority: the first-person rig fit gets the vanilla translation (GameBridge.h SetRigFitRaw)
        if (a_load) game::SetRigFitRaw(rd("bStableRig", 1.0f, false) != 0.0f);  // first-person rig fit (head path): load only
        {
            const char* ck[3] = { "fControllerModelOffsetX", "fControllerModelOffsetY", "fControllerModelOffsetZ" };
            const char* cr[3] = { "fControllerModelRotXDeg", "fControllerModelRotYDeg", "fControllerModelRotZDeg" };
            for (int i = 0; i < 3; ++i) {
                c.ctrlOffCm[i] = rd(ck[i], c.ctrlOffCm[i], false);
                c.ctrlRotDeg[i] = rd(cr[i], c.ctrlRotDeg[i], false);
            }
            c.ctrlScale = rd("fControllerModelScale", 1.0f, false);
            const char* pk[2] = { "sControllerModelRight", "sControllerModelLeft" };
            const char* pd[2] = { "Data/Meshes/fo4flatvr/ControllerR.nif", "Data/Meshes/fo4flatvr/ControllerL.nif" };
            for (int i = 0; i < 2; ++i) {
                char buf[512]{};
                ::GetPrivateProfileStringA("Weapon", pk[i], pd[i], buf, sizeof(buf), path.c_str());
                c.ctrlPath[i] = buf;
            }
        }
        }

        // 20 Nov (live settings, ConfigStore.h): the gun's placement / kickback / recoil / controller-model offsets and
        // fUnitsPerMeter apply live. Restart-only: bFloatingGun / bHideArms (W1 / A1 patches at load), sWeaponOffsetsDir and
        // the controller model paths (loaded once), bStableRig (first-person rig fit, head path).
        void ReloadLive(const cfgstore::Changes&)
        {
            Cfg n;
            ReadCfg(n, false);
            std::scoped_lock l(g_lock);
            Cfg& c = g_cfg;
            n.enabled = c.enabled;
            n.hideArms = c.hideArms;
            n.jsonDir = c.jsonDir;
            n.ctrlPath[0] = c.ctrlPath[0];
            n.ctrlPath[1] = c.ctrlPath[1];
            c = n;
            REX::INFO("gun: settings re-read (live): grip off=({},{},{})cm rot=({},{},{})deg align={} anchor={} aimFromMuzzle={} noGunLowering={} "
                      "kickback={} recoil={} melee={} inPA={} controllerModels={} unitsPerMeter={}",
                c.offCm[0], c.offCm[1], c.offCm[2], c.rotDeg[0], c.rotDeg[1], c.rotDeg[2], c.align, c.gripAnchor, c.aimFromMuzzle ? 1 : 0,
                c.noGunLowering ? 1 : 0, c.kickback ? 1 : 0, c.recoilRot ? 1 : 0, c.meleeNode ? 1 : 0, c.inPowerArmor ? 1 : 0,
                c.ctrlModels ? 1 : 0, c.unitsPerMeter);
        }
    }

    std::string Install()
    {
        static std::string summary;
        if (!summary.empty()) return summary;
        Cfg& c = g_cfg;
        ReadCfg(c, true);
        cfgstore::Subscribe("gun", "Weapon,Hands,General/fUnitsPerMeter", &ReloadLive);
        if (!c.enabled) {
            summary = "off ([Weapon] bFloatingGun=0): not patched, arms not hidden";
            REX::INFO("gun: {}", summary);
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
        // W1: the original call is E8 B8 B1 00 00 -> Notify C403B0; another fo4flatvr hook there is chained onto
        const std::uintptr_t site = Base() + kNotifyCallSite;
        if (g_fnOk && callchain::Hookable(site, Base() + kNotify, g_hookWhy)) {
            g_origNotify = reinterpret_cast<Notify_t>(tramp.write_call<5>(site, &HkNotify));
            g_hooked = true;
        } else if (g_fnOk) {
            REX::ERROR("gun: W1 call site C351F3 not hookable: {}", g_hookWhy);
        }
        // A1 (arm hiding needs it: flat ignores AppCulled on first-person root children)
        if (g_hooked && c.hideArms) {
            static constexpr std::uint8_t kCullCall[5] = { 0xE8, 0xDA, 0xDF, 0x5E, 0xFF };
            static constexpr std::uint8_t kBefore[13] = { 0x4C, 0x8D, 0x82, 0xB0, 0x00, 0x00, 0x00, 0x45, 0x33, 0xC9, 0x48, 0x8D, 0x0D };
            if (BytesMatch(kCullAddCallSite, kCullCall, 5, "Add1stPersonGeomToCuller call 21F2051") &&
                BytesMatch(kCullAddCallSite - 0x11, kBefore, sizeof(kBefore), "Add1stPersonGeomToCuller loop 21F2040")) {
                std::int32_t rel;
                std::memcpy(&rel, reinterpret_cast<const void*>(Base() + kCullAddCallSite + 1), 4);
                if (kCullAddCallSite + 5 + rel == kCullAdd) {
                    g_origCullAdd = reinterpret_cast<CullAdd_t>(tramp.write_call<5>(Base() + kCullAddCallSite, &HkCullAdd));
                    g_cullHooked = true;
                }
            }
            if (!g_cullHooked) REX::ERROR("gun: A1 (21F2051) not patched: first-person arms stay visible");
        }
        g_on = g_hooked;
        summary = std::format("A1(21F2051)={} W1(C351F3)={} ({}) fn={} pa={} jsons={} dir='{}' grip off=({},{},{})cm rot=({},{},{})deg align={} aimFromMuzzle={} "
                              "noGunLowering={} hideArms={} controllerModels={}",
            g_cullHooked ? "ok" : (c.hideArms ? "FAILED" : "off"), g_hooked ? "ok" : "FAILED", g_hookWhy, g_fnOk ? "ok" : "FAILED", g_paOk ? "ok" : "FAILED", nJson, c.jsonDir, c.offCm[0], c.offCm[1],
            c.offCm[2], c.rotDeg[0], c.rotDeg[1], c.rotDeg[2], c.align, c.aimFromMuzzle, c.noGunLowering, c.hideArms, c.ctrlModels);
        REX::INFO("gun: {}", summary);
        return summary;
    }

    bool AimFromWeapon(const float a_camRows[9], const float a_camPos[3], float a_P[3], float a_D[3])
    {
        std::scoped_lock l(g_lock);
        ++g_st.aimCalls;
        const auto now = std::chrono::steady_clock::now();
        if (!g_on.load() || g_aside.load() || !g_cfg.aimFromMuzzle || !g_lastActive || !g_FValid ||
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
        // same chain as the hook, from the hand pose relative to the head the camera on screen was rendered with
        const Xf w = TargetWeapon(cam, h, g_cfg, g_J, g_jKind, g_F, g_FValid, g_wScale, nullptr, g_handleOk ? &g_handleLocal : nullptr, &g_kick, g_melee);
        const Xf m = Mul(w, g_F);
        const V  d = Norm(m.R.Col(1));
        a_P[0] = static_cast<float>(m.t.x), a_P[1] = static_cast<float>(m.t.y), a_P[2] = static_cast<float>(m.t.z);
        a_D[0] = static_cast<float>(d.x), a_D[1] = static_cast<float>(d.y), a_D[2] = static_cast<float>(d.z);
        ++g_st.aimUsed;
        g_st.P = m.t;
        g_st.D = d;
        return true;
    }

    bool OffHandAim(float a_posM[3], float a_dir[3])
    {
        std::scoped_lock l(g_lock);
        const auto& h = g_snap.oOld;
        if (!h.valid || std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_snap.t).count() > kFreshMs) {
            return false;
        }
        for (int i = 0; i < 3; ++i) a_posM[i] = h.aimPos[i], a_dir[i] = h.aimDir[i];
        return true;
    }

    bool SuppressGunLowering()
    {
        if (!g_on.load() || g_aside.load()) return false;
        std::scoped_lock l(g_lock);
        return g_cfg.noGunLowering && g_lastActive &&
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_lastActiveT).count() <= kFreshMs;
    }

    void StepAside(bool a_aside)
    {
        g_aside = a_aside;
        REX::INFO("gun: placement {} (FRIK-flat {})", a_aside ? "steps aside" : "resumes", a_aside ? "active" : "inactive");
    }

    void OnPresent()
    {
        if (!g_hooked) return;
        ++g_presentCount;
        TraceSample('P');
        if (g_diagLog) {
            static double tLast = 0;
            static std::uint64_t last[8]{};
            const double t = QpcUs();
            if (t - tLast > 2.0e6) {
                tLast = t;
                const std::uint64_t now8[8] = { g_hookCount.load(), g_collects.load(), g_collectDuringJob.load(), g_collectInWindow.load(), g_cullTypeSkips.load(),
                    g_flagClears.load(), g_newParts.load(), g_staleFrames.load() };
                static std::uint64_t lastAlt[5]{};
                const std::uint64_t alt[5] = { g_altHand.load(), g_altRoot.load(), g_altCam.load(), g_altFpCam.load(), g_altFpOff.load() };
                REX::INFO("arms diag 2s: step reversals at the render collect (>2 u): RArm_Hand {} (last amplitude {:.1f} u) firstPerson3D root {} camera root {} "
                          "1st-person camera loc {} 1st-person offset {}",
                    alt[0] - lastAlt[0], g_altHandAmp.load(), alt[1] - lastAlt[1], alt[2] - lastAlt[2], alt[3] - lastAlt[3], alt[4] - lastAlt[4]);
                std::memcpy(lastAlt, alt, sizeof(lastAlt));
                REX::INFO("arms diag 2s: W1 {} collects {} collectDuringNextUpdate {} collectWithAnimatedRig {} typeSkips(rescued draws) {} flagClears {} newParts {} "
                          "staleSnapshotFrames {} hide={} presents {}",
                    now8[0] - last[0], now8[1] - last[1], now8[2] - last[2], now8[3] - last[3], now8[4] - last[4], now8[5] - last[5], now8[6] - last[6],
                    now8[7] - last[7], g_hideActive.load() ? 1 : 0, g_presentCount.load());
                std::memcpy(last, now8, sizeof(last));
            }
        }
        auto*       player = RE::PlayerCharacter::GetSingleton();
        Rig         rig;
        const char* why = nullptr;
        const bool  haveRig = player && player->GetParentCell() && GetRig(rig, why);
        if (!haveRig || !rig.fire) return;
        const V  off = NiV(*reinterpret_cast<const RE::NiPoint3*>(Base() + kFpOffset));
        const Xf f = FromNi(rig.fire->world);
        std::scoped_lock l(g_lock);
        g_st.fireAtPresent = f.t + off;
        if (g_st.aimUsed > 0) {
            g_st.pFireDist = Len(g_st.P - g_st.fireAtPresent);
            g_st.dFireDeg = std::acos(std::clamp(Dot(g_st.D, Norm(f.R.Col(1))), -1.0, 1.0)) * kR2D;
        }
    }

    namespace
    {
        std::string Readout()
        {
            std::scoped_lock l(g_lock);
            const auto& s = g_st;
            const auto& c = g_cfg;
            return std::format(
                "gun on={} W1={} aside={} gate='{}' ws={} weapon='{}' j={} calls={} active={} restores={} snapAgeMs={:.0f} "
                "tgt={} act={} err={:.3f} angErr={:.3f} shift={:.2f} rot={:.2f} wWorld={} loc={} cam={} before={} handVsWeapon={} "
                "fire={} fireFwd={} fireAtPresent={} P={} D={} pFire={:.3f} dFireDeg={:.3f} aim={}/{} "
                "costUs={:.1f} avg={:.1f} max={:.1f} fwdInGrip={} upInGrip={} aimInGrip={} weaponLocalPersisted={} weaponLocalRewritten={} "
                "handleNode='{}' handleValid={} handleInGripCm={} originInGripCm={} gripAnchor={} "
                "armsHidden={} armsCulled={} armsCandidates={} A1={} cullAdds={} cullSkips={} typeSkips={} flagClears={} newParts={} staleFrames={} collects={} "
                "collectDuringNextUpdate={} collectWithAnimatedRig={} diag={} rigFit={} altHand={} altRoot={} altCam={} altFpCam={} altFpOff={} altHandAmp={:.2f} ctrlR={}/{} ctrlL={}/{} ctrlShownFrames={}/{} melee={} inPA={} kicks={} kickOn={} kickD={:.2f} kickMaxD={:.2f} kickAmp={:.2f} kickDur={:.2f} kickK={:.3f} recoilRotDeg={:.3f} cfg off=({},{},{}) rot=({},{},{}) align={} muzzle={} noLower={} hideArms={}",
                g_on.load() ? 1 : 0, g_hooked ? "ok" : "no", g_aside.load() ? 1 : 0, s.gate, s.weaponState, s.weapon, s.jmode, s.calls, s.active,
                s.restores, s.snapAgeMs, F(s.tgt, 3), F(s.act, 3), s.posErr, s.angErr, s.shift, s.rot, F(s.actWorld, 3), F(s.loc, 2), F(s.camHook, 3),
                F(s.before, 3), F(s.handVsWeapon, 2), F(s.fireWorld, 3), F(s.fireFwd, 4), F(s.fireAtPresent, 3), F(s.P, 3), F(s.D, 4), s.pFireDist,
                s.dFireDeg, s.aimUsed, s.aimCalls, s.costUs, s.costAvgUs, s.costMaxUs, F(s.fwdInGrip, 4), F(s.upInGrip, 4), F(s.aimInGrip, 4),
                s.persisted, s.rewritten, s.handleNode, s.handleValid ? 1 : 0, F(s.handleInGripCm, 2), F(s.originInGripCm, 2), c.gripAnchor,
                s.armsHidden ? 1 : 0, s.armsCulled, s.armsCandidates, g_cullHooked ? "ok" : "no", g_cullAdds.load(),
                g_cullSkips.load(), g_cullTypeSkips.load(), g_flagClears.load(), g_newParts.load(), g_staleFrames.load(), g_collects.load(),
                g_collectDuringJob.load(), g_collectInWindow.load(), g_diagLog ? 1 : 0, game::RigFitRaw() ? 1 : 0, g_altHand.load(), g_altRoot.load(), g_altCam.load(), g_altFpCam.load(),
                g_altFpOff.load(), g_altHandAmp.load(), g_ctrl[0].shown ? "shown" : "hidden", g_ctrl[0].status, g_ctrl[1].shown ? "shown" : "hidden",
                g_ctrl[1].status, g_ctrlShownFrames[0].load(), g_ctrlShownFrames[1].load(), s.melee ? 1 : 0, s.inPA ? 1 : 0, g_kick.kicks, g_kick.on ? 1 : 0,
                g_kick.d, g_kick.maxD, g_kick.amp, g_kick.dur, g_kick.lastK, g_kick.rotDeg, c.offCm[0], c.offCm[1], c.offCm[2], c.rotDeg[0],
                c.rotDeg[1], c.rotDeg[2], c.align, c.aimFromMuzzle ? 1 : 0, c.noGunLowering ? 1 : 0, c.hideArms ? 1 : 0);
        }
    }

    std::string Command(const std::string& a_arg)
    {
        std::istringstream is(a_arg);
        std::string        verb;
        is >> verb;
        if (verb.empty()) return Readout();
        if (verb == "on" || verb == "off") {
            if (!g_hooked) return "gun: not installed (bFloatingGun=0 at load or byte check failed)";
            g_on = verb == "on";
            return std::string("gun ") + verb;
        }
        if (verb == "arms") {
            std::string v;
            is >> v;
            if (v == "on" || v == "off") g_armsOn = v == "on";  // "arms on" = hiding on
            std::scoped_lock l(g_lock);
            return std::format("gun arms hiding={} hidden={} culled={}/{} [{}]", g_armsOn.load() ? "on" : "off", g_st.armsHidden ? 1 : 0, g_st.armsCulled,
                g_st.armsCandidates, g_st.armsNames);
        }
        if (verb == "diag") {
            std::string v;
            is >> v;
            if (v == "on" || v == "off") g_diagLog = v == "on";
            const auto h = g_diagLog ? InstallTraceHooks() : std::string("-");
            return std::format("gun diag log={} timing hooks={}", g_diagLog ? 1 : 0, h);
        }
        if (verb == "trace") {
            int         frames = 300;
            std::string path;
            is >> frames >> path;
            frames = std::clamp(frames, 10, 3000);
            if (path.empty()) return "error: gun trace <frames> <csv path>";
            if (const auto h = InstallTraceHooks(); h != "ok") return "gun trace: diagnostic hooks not installed: " + h;
            if (!g_ring) g_ring = new TraceEv[kRing];
            g_ringHead = 0;
            const std::uint64_t f0 = g_hookCount.load();
            std::atomic<bool>   stop{ false };
            // watcher: logs any change of the watched values (writer = whoever ran between the surrounding site events)
            std::thread watcher([&] {
                TraceEv last{};
                bool    have = false;
                while (!stop.load()) {
                    TraceEv e{};
                    Fill(e);
                    if (!have || std::memcmp(e.wW, last.wW, sizeof(float) * 3 * 6) != 0 || e.culledMask != last.culledMask) {
                        TraceEvent(10);
                        last = e;
                        have = true;
                    }
                    for (volatile int i = 0; i < 300; ++i) {}
                }
            });
            g_traceOn = true;
            for (int i = 0; i < 1200 && g_hookCount.load() - f0 < static_cast<std::uint64_t>(frames); ++i) ::Sleep(25);
            g_traceOn = false;
            stop = true;
            watcher.join();
            const std::uint64_t head = g_ringHead.load();
            const std::uint64_t n = (std::min)(head, static_cast<std::uint64_t>(kRing));
            std::vector<TraceEv> ev(n);
            for (std::uint64_t i = 0; i < n; ++i) ev[i] = g_ring[(head - n + i) & (kRing - 1)];
            std::sort(ev.begin(), ev.end(), [](const TraceEv& x, const TraceEv& y) { return x.tUs < y.tUs; });
            std::ofstream f(path, std::ios::binary);
            if (!f) return "error: cannot write " + path;
            f << "tUs,tid,frame,present,site,culledMask,wW_x,wW_y,wW_z,wL_x,wL_y,wL_z,rh_x,rh_y,rh_z,lh_x,lh_y,lh_z,rUp_x,rUp_y,rUp_z,lUp_x,lUp_y,lUp_z,cam_x,cam_y,cam_z" << '\n';
            for (const auto& e : ev) {
                f << std::format("{:.1f},{},{},{},{},{}", e.tUs, e.tid, e.frame, e.present, e.site < 11 ? kSiteNames[e.site] : "?", e.culledMask);
                for (const float* v : { e.wW, e.wL, e.rh, e.lh, e.rUp, e.lUp, e.cam }) f << std::format(",{:.3f},{:.3f},{:.3f}", v[0], v[1], v[2]);
                f << '\n';
            }
            return std::format("gun trace: {} events over {} frames (ring head {}) -> {}", n, g_hookCount.load() - f0, head, path);
        }
        if (verb == "race") {
            // Sample WEAPON's world (player-relative) and the culled bits of the first-person root children for <ms>:
            // how long per game frame the rig holds the animated pose / anything else, and whether a culled arm
            // model ever loses its bit. Read-only, benign data race (diagnostic).
            int ms = 1000;
            is >> ms;
            ms = std::clamp(ms, 50, 5000);
            Rig         rig;
            const char* why = nullptr;
            if (!GetRig(rig, why)) return std::string("gun race: no rig: ") + why;
            std::vector<RE::NiAVObject*> kids;
            for (auto& c : rig.root->children) {
                if (c && c.get() != rig.tree) kids.push_back(c.get());
            }
            const std::uint64_t rt0[3] = { g_rtTgt.load(), g_rtAnim.load(), g_rtOther.load() };
            const double t0 = QpcUs(), tEnd = t0 + ms * 1000.0;
            std::uint64_t n = 0, nTgt = 0, nAnim = 0, nOther = 0, nUnculled = 0, flips = 0, h0 = g_raceHook.load();
            int    last = -1;
            double runStart = t0, maxAnimRun = 0, maxOtherRun = 0, maxTgtRun = 0, maxOtherDist = 0;
            while (QpcUs() < tEnd) {
                const auto& w = rig.weapon->world.translate;
                const V     p{ w.x, w.y, w.z };
                const V     a{ g_raceAnim[0].load(), g_raceAnim[1].load(), g_raceAnim[2].load() };
                const V     tg{ g_raceTgt[0].load(), g_raceTgt[1].load(), g_raceTgt[2].load() };
                const double dT = Len(p - tg), dA = Len(p - a);
                const int   st = dT < 1.0 ? 0 : dA < 3.0 ? 1 : 2;
                (st == 0 ? nTgt : st == 1 ? nAnim : nOther)++;
                if (st == 2) maxOtherDist = (std::max)(maxOtherDist, dT);
                for (auto* k : kids) {
                    if (k->IsGeometry() && !(k->flags.flags & 1)) { ++nUnculled; break; }
                }
                const double now = QpcUs();
                if (st != last) {
                    if (last >= 0) {
                        const double run = now - runStart;
                        (last == 0 ? maxTgtRun : last == 1 ? maxAnimRun : maxOtherRun) =
                            (std::max)(last == 0 ? maxTgtRun : last == 1 ? maxAnimRun : maxOtherRun, run);
                        ++flips;
                    }
                    last = st;
                    runStart = now;
                }
                ++n;
                for (volatile int i = 0; i < 200; ++i) {}
            }
            const std::uint64_t hooks = g_raceHook.load() - h0;
            return std::format("gun race ms={} samples={} hooks={} atTarget={:.2f}% atAnimated={:.2f}% other={:.2f}% (max {:.1f} u from target) "
                               "flips={} maxRunUs target={:.0f} animated={:.0f} other={:.0f} armGeomUnculledSamples={} kids={} | at 1st-person collect (render thread, "
                               "this window): target={} animated={} other={}",
                ms, n, hooks, 100.0 * nTgt / (std::max)(n, std::uint64_t{ 1 }), 100.0 * nAnim / (std::max)(n, std::uint64_t{ 1 }),
                100.0 * nOther / (std::max)(n, std::uint64_t{ 1 }), maxOtherDist, flips, maxTgtRun, maxAnimRun, maxOtherRun, nUnculled, kids.size(), g_rtTgt.load() - rt0[0], g_rtAnim.load() - rt0[1], g_rtOther.load() - rt0[2]);
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
                if (!g_dumpReq.empty()) return "gun dump: " + g_dumpReq;
            }
            g_dumpPending = false;
            return "gun dump: no frame ran the hook (not in game?)";
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
            else if (k == "hidearms") c.hideArms = v != 0;
            else if (k == "anchor") c.gripAnchor = static_cast<int>(v);
            else if (k == "hoffx") c.handleOffCm[0] = static_cast<float>(v);
            else if (k == "hoffy") c.handleOffCm[1] = static_cast<float>(v);
            else if (k == "hoffz") c.handleOffCm[2] = static_cast<float>(v);
            else if (k == "rigfit") game::SetRigFitRaw(v != 0);
            else if (k == "ctrl") c.ctrlModels = v != 0;
            else if (k == "kick") c.kickback = v != 0;
            else if (k == "recoil") c.recoilRot = v != 0;
            else if (k == "melee") c.meleeNode = v != 0;
            else if (k == "pa") c.inPowerArmor = v != 0;
            else if (k == "kickreset") g_kick.maxD = 0;
            else if (k == "coffx") c.ctrlOffCm[0] = static_cast<float>(v);
            else if (k == "coffy") c.ctrlOffCm[1] = static_cast<float>(v);
            else if (k == "coffz") c.ctrlOffCm[2] = static_cast<float>(v);
            else if (k == "crotx") c.ctrlRotDeg[0] = static_cast<float>(v);
            else if (k == "croty") c.ctrlRotDeg[1] = static_cast<float>(v);
            else if (k == "crotz") c.ctrlRotDeg[2] = static_cast<float>(v);
            else if (k == "cscale") c.ctrlScale = static_cast<float>(v);
            else if (k == "resetmax") { g_st.costMaxUs = 0; g_st.costN = 0; }
            else return "error: gun cfg offx|offy|offz|rotx|roty|rotz|align|muzzle|nolower|hidearms|anchor|hoffx|hoffy|hoffz|rigfit|ctrl|kick|recoil|melee|pa|kickreset|coffx|coffy|coffz|crotx|croty|crotz|cscale|resetmax <v>";
            return std::format("gun cfg {} = {}", k, v);
        }
        return "error: gun [on|off|arms [on|off]|dump|cfg <key> <value>]";
    }
}
