#include <F4SE/F4SE.h>

#include "plugin/Plugin.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <system_error>

#include <ShlObj.h>
#include <Windows.h>
#undef ERROR

namespace
{
    // Keep every session's log: the log is opened fresh at each launch, so an agent's launch right after a
    // headset session used to overwrite the user's timing data. Rename the previous fo4flatvr.log to
    // fo4flatvr-<its last-write time>.log before the logger opens it (nothing is ever deleted).
    void RotatePreviousLog()
    {
        PWSTR docs = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) {
            return;
        }
        const std::filesystem::path dir = std::filesystem::path(docs) / L"My Games" / L"Fallout4" / L"F4SE";
        ::CoTaskMemFree(docs);
        const auto      log = dir / L"fo4flatvr.log";
        std::error_code ec;
        if (!std::filesystem::exists(log, ec)) {
            return;
        }
        const auto ft = std::filesystem::last_write_time(log, ec);
        if (ec) {
            return;
        }
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        const auto local = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(sys));
        auto       target = dir / std::format(L"fo4flatvr-{:%Y%m%d-%H%M%S}.log", local);
        for (int i = 1; std::filesystem::exists(target, ec) && i < 100; ++i) {
            target = dir / std::format(L"fo4flatvr-{:%Y%m%d-%H%M%S}-{}.log", local, i);
        }
        std::filesystem::rename(log, target, ec);
    }
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    RotatePreviousLog();
    // Trampoline for the code hooks (14 B per call/jmp branch; the aim hooks use 61 B incl. a
    // relocated-prologue stub, the HUD call sites 28 B; camera step 2 gates ~250 B). Generous headroom; unused bytes are just reserved.
    F4SE::Init(a_f4se, { .trampoline = true, .trampolineSize = 1024 });
    return fo4flatvr::OnLoad();
}
