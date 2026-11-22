#pragma once
#include <string>
// Latest head pose, shared between the XR side (publisher) and the camera hooks (consumers).
// A scripted override (pipe "pose"/"posepos") replaces the HMD for headless testing.
//
// Everything handed out is relative to the recentre origin (yaw + position), in the game's basis
// (X right, Y forward, Z up). Rotation matrices use the column-vector convention: v' = M * v.

namespace fo4flatvr::head
{
    struct Mat3 { float m[3][3]; };
    struct Vec3 { float x{}, y{}, z{}; };

    // OpenXR VIEW pose in the reference space (orientation quaternion + position in metres).
    void PublishXr(float a_qx, float a_qy, float a_qz, float a_qw, float a_px, float a_py, float a_pz, bool a_valid);

    // Re-anchor: the current HMD yaw becomes "forward" and the current position the origin.
    // Happens automatically on the first valid pose.
    void Recenter();
    // Number of re-anchors so far (incl. the automatic first one). Any thread.
    unsigned RecenterCount();
    // Until the first explicit Recenter(), a valid pose more than a_metres from the previous valid one re-anchors
    // (OpenXR runtimes that hand out a placeholder pose first; HeadPose.cpp). <= 0 = off. [OpenXR] bRecentreOnPoseJump.
    void     SetRecentreOnPoseJump(float a_metres);
    unsigned PoseJumpRecentres(float* a_lastJumpM = nullptr);

    // Degrees. yaw + = look right, pitch + = look up, roll + = tilt head right.
    void SetOverride(float a_yawDeg, float a_pitchDeg, float a_rollDeg);
    void ClearOverride();
    // Centimetres in the body frame: x + = right, y + = forward, z + = up. Cleared by ClearOverride.
    void SetPositionOverride(float a_xCm, float a_yCm, float a_zCm);

    void SetTracking(bool a_on);  // master switch for applying any head pose
    bool Tracking();
    bool OverrideActive();
    void SetPositionTracking(bool a_on);

    // False when nothing should be applied (tracking off, or no valid pose and no override).
    bool Get(Mat3& a_out);
    // Head offset from the recentre origin in metres, body frame. False when nothing to apply.
    bool GetOffset(Vec3& a_out);

    // Horizontal FOV (degrees) the game camera should render with; 0 = leave the game's own.
    void  SetTargetFov(float a_hDeg);
    float TargetFov();

    // Debug knob: composition order in the camera hook (0 = base*head, 1 = head*base).
    void SetConvention(int a_mode);
    int  Convention();

    // ---- pose sync ([General] bPoseSync) ----
    // Measured (htrace, 14 Nov): while the game runs, the player/camera update runs on a job thread concurrently with
    // the Present hook, ~0.3-2 ms after hook entry, i.e. while the hook is still blocked in xrWaitFrame, so the camera
    // root was built from the PREVIOUS publish while the stereo eyes / projection layers claim the new one (residual =
    // one frame of head motion). Paused, the update runs on the main thread after the publish (residual 0).
    // The Presenter opens a window at Present entry and closes it right after the new pose is published; head
    // consumers on other threads call WaitForPublish() first and block (bounded) while the window is open.
    void SetPoseSync(bool a_on, float a_timeoutMs);
    bool PoseSync();
    void OpenPublishWindow();   // presenter thread, Present entry (XR running)
    void ClosePublishWindow();  // presenter thread, after PublishXr / Present exit (idempotent)
    // a_site: 1 roomscale MMD, 2 FP Update, 3 FP GetRotation, 4 FP GetTranslation, 5 camera director, 6 other
    void WaitForPublish(int a_site);
    std::string PoseSyncStats(bool a_reset);

    // ---- head-timing diagnostics ("htrace") ----
    // Every PublishXr bumps a sequence number; consumers can ask which publish their pose came from.
    unsigned long long Seq();
    bool Get(Mat3& a_out, unsigned long long& a_seq);
    bool GetOffset(Vec3& a_out, unsigned long long& a_seq);

    // Per-frame trace ring (off unless armed). kind: P publish, S submit, R FP GetRotation, T FP GetTranslation,
    // U FP Update end (camera root world), C world NiCamera at Present, D director tick, M roomscale primary update.
    struct TraceRec
    {
        char               kind{};
        unsigned           tid{};
        long long          qpc{};
        unsigned long long frame{};  // presents counted so far (TraceFrame)
        unsigned long long seq{};    // head publish sequence the record refers to
        long long          xrTime{};
        float              v[16]{};
    };
    bool        TraceArmed();
    void        Trace(TraceRec& a_rec);  // fills tid, qpc, frame; no-op unless armed
    void        TraceSetFrame(unsigned long long a_frame);
    std::string TraceCommand(const std::string& a_arg);  // "htrace arm <frames>" / "htrace dump <path>" / "htrace"
    // [Debug] bPoseTrace: record continuously, one CSV per a_framesPerFile frames into a_dir
    // (fo4flatvr_posetrace_NNN.csv), at most a_maxFiles files; written on a worker thread.
    void TraceConfigure(bool a_continuous, int a_framesPerFile, int a_maxFiles, const std::string& a_dir);
}
