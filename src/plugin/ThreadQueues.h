#pragma once
// ThreadQueues - run work on the engine thread that owns the state it touches (20 Nov, thread-safety audit;
// analysis/threading.md). Replaces F4SE's AddTask / AddUITask in fo4flatvr: on flat 1.11.240 F4SE drains both from
// whatever thread empties its queue (the user's logs: six thread ids for one per-second UI task, mostly the game's job
// threads while they run the player update and while the main thread renders), so "queue it as a task" never meant
// "main thread". FaceDiag's selfie hang (D3D immediate context from such a thread) was the first symptom.
//
// Flat frame (Main::OnIdle C339D0, all on the main thread): ... JobListManager::StartThreads -> Swap (render + our
// Present hook, job threads running the player / camera update concurrently) -> WaitForThreads -> UI::ProcessMessages ->
// UI::AdvanceMenus (menus advanced serially here; Ghidra 1A81810) -> Console::ExecuteQueuedCommand (C34794) -> ...
//
// Queues:
// - Main (= UI): drained at C34794, the call of Console::ExecuteQueuedCommand in OnIdle (where the engine itself runs console
//   commands): main thread, after WaitForThreads (no job thread in the player / actor / camera update), after the menus
//   advanced and before the next Swap draws anything. Scene-graph nodes, actors, PlayerCamera, the UI menu stack and
//   Scaleform movies (GFx values, Invoke, mouse events) are all owned by this thread at that point. Runs paused or not.
// - Render: drained at the Present hook (Presenter HkPresent): the thread that owns the D3D11 immediate context.
// - Main ticks: per-frame polls that change scene-graph / game state (they ran at Present, concurrently with the job
//   threads), now called once per OnIdle right after the main queue.
// Off switch [General] bMainThreadQueue=0 (live since 20 Nov: the hook is installed anyway and passes through): tasks go
// to F4SE AddTask / AddUITask and the ticks run at Present, as before. If the call site is not hookable (another mod), the same fallback is used and logged.
#include <chrono>
#include <functional>
#include <string>

namespace fo4flatvr::tq
{
    void Install();  // OnLoad, before the game loop runs (reads [General] bMainThreadQueue)
    bool MainHooked();

    // a_fn runs once on the main thread at the main-idle point (from the main thread itself: on the next frame).
    // Any thread. false = not queued (no hook and no F4SE task interface).
    bool RunOnMain(std::function<void()> a_fn);
    // Scaleform / menu work: the UI is the main thread on flat 1.11 (AdvanceMenus is serial, menus render in Swap).
    inline bool RunOnUI(std::function<void()> a_fn) { return RunOnMain(std::move(a_fn)); }

    // Pipe / test helpers that need an answer: run a_fn on the main thread and wait for its result. From the main thread
    // itself it runs inline. On timeout returns a_timeoutText (the task still runs later; it must own its captures).
    std::string RunOnMainWait(std::function<std::string()> a_fn, std::chrono::milliseconds a_timeout, const char* a_timeoutText);

    // a_fn runs once at the next Present on the Present thread (D3D11 immediate context owner). Any thread.
    bool RunOnRender(std::function<void()> a_fn);

    // Per-frame poll on the main thread after the main queue (registration at load / Install time only, before the game
    // loop). With the fallback (no hook) the ticks run from PresentFallbackTicks instead.
    void AddMainTick(void (*a_fn)(), const char* a_name);

    void OnPresent();             // Presenter HkPresent: drains the render queue
    void PresentFallbackTicks();  // Presenter VrFrame (where dlg::Tick ran): the main ticks, only when the hook is not installed

    bool        IsMainThread();   // the thread that drains the main queue (known after the first drain)
    std::string Status();         // pipe "threads"
}
