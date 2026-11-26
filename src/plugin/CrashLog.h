#pragma once
// Crash report (26 Nov; the user's new-game crash left no log, no crash logger was installed): an unhandled-exception
// filter that writes the exception, the faulting thread's stack (module+offset, symbols where pdbs sit next to their
// dlls) into fo4flatvr.log and a minidump fo4flatvr-crash-<time>.dmp into My Games\Fallout4\F4SE, then chains to the
// filter that was there before. Installed at plugin load and again at game data ready (the game / other plugins may set
// their own in between; ours then runs first and chains to theirs). Off switch: [General] bCrashLog=0.
namespace fo4flatvr::crashlog
{
    void Install(const char* a_when);   // "load" reads [General] bCrashLog
}
