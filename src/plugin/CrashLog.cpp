// REX/LOG.h must precede <Windows.h>: wingdi.h #defines ERROR, which collides with REX::ERROR.
#include <REX/LOG.h>

#include "plugin/CrashLog.h"

#include "plugin/Settings.h"

#include <Windows.h>
#undef ERROR
#include <DbgHelp.h>
#include <ShlObj.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

namespace fo4flatvr::crashlog
{
    namespace
    {
        LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
        std::atomic<bool>            g_in{ false };
        bool                         g_enabled = true;

        std::filesystem::path Dir()
        {
            PWSTR docs = nullptr;
            std::filesystem::path p;
            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
                p = std::filesystem::path(docs) / L"My Games" / L"Fallout4" / L"F4SE";
            }
            if (docs) ::CoTaskMemFree(docs);
            return p;
        }

        std::string Where(DWORD64 a_addr)
        {
            HMODULE m = nullptr;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_addr), &m) && m) {
                char name[MAX_PATH]{};
                ::GetModuleFileNameA(m, name, MAX_PATH);
                const char* base = std::strrchr(name, '\\');
                return std::format("{}+{:X}", base ? base + 1 : name, a_addr - reinterpret_cast<DWORD64>(m));
            }
            return std::format("{:X}", a_addr);
        }

        LONG WINAPI Filter(EXCEPTION_POINTERS* a_ep)
        {
            if (!g_in.exchange(true) && a_ep && a_ep->ExceptionRecord && a_ep->ContextRecord) {
                const auto* er = a_ep->ExceptionRecord;
                const auto  now = std::chrono::zoned_time{ std::chrono::current_zone(), std::chrono::system_clock::now() }.get_local_time();
                const auto  stamp = std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(now));
                REX::CRITICAL("CRASH: exception {:08X} at {} on thread {}{}", er->ExceptionCode, Where(reinterpret_cast<DWORD64>(er->ExceptionAddress)),
                    ::GetCurrentThreadId(),
                    er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 ?
                        std::format(" ({} {:X})", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                            er->ExceptionInformation[1]) :
                        std::string());
                // the faulting thread's stack, module+offset per frame (symbols where a pdb sits next to its dll)
                HANDLE proc = ::GetCurrentProcess();
                ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
                const bool sym = ::SymInitialize(proc, nullptr, TRUE) != FALSE;
                CONTEXT ctx = *a_ep->ContextRecord;
                STACKFRAME64 sf{};
                sf.AddrPC.Offset = ctx.Rip, sf.AddrPC.Mode = AddrModeFlat;
                sf.AddrFrame.Offset = ctx.Rbp, sf.AddrFrame.Mode = AddrModeFlat;
                sf.AddrStack.Offset = ctx.Rsp, sf.AddrStack.Mode = AddrModeFlat;
                for (int i = 0; i < 48; ++i) {
                    if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, ::GetCurrentThread(), &sf, &ctx, nullptr, ::SymFunctionTableAccess64,
                            ::SymGetModuleBase64, nullptr) ||
                        !sf.AddrPC.Offset)
                        break;
                    std::string name;
                    if (sym) {
                        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
                        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
                        si->SizeOfStruct = sizeof(SYMBOL_INFO);
                        si->MaxNameLen = 255;
                        DWORD64 disp = 0;
                        if (::SymFromAddr(proc, sf.AddrPC.Offset, &disp, si)) name = std::format(" {}+{:X}", si->Name, disp);
                    }
                    REX::CRITICAL("CRASH:   #{:02} {}{}", i, Where(sf.AddrPC.Offset), name);
                }
                if (sym) ::SymCleanup(proc);
                // a minidump next to the log (what a debugger / WinDbg opens)
                const auto dir = Dir();
                if (!dir.empty()) {
                    const auto file = dir / ("fo4flatvr-crash-" + stamp + ".dmp");
                    HANDLE     f = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        MINIDUMP_EXCEPTION_INFORMATION mei{ ::GetCurrentThreadId(), a_ep, FALSE };
                        const BOOL ok = ::MiniDumpWriteDump(proc, ::GetCurrentProcessId(), f,
                            static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                                       MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules),
                            &mei, nullptr, nullptr);
                        ::CloseHandle(f);
                        REX::CRITICAL("CRASH: minidump {} {}", file.filename().string(), ok ? "written" : "FAILED");
                    }
                }
                REX::CRITICAL("CRASH: end of report (the game may still be caught by another handler)");
                if (auto* lg = spdlog::default_logger_raw()) lg->flush();
            }
            return g_prev ? g_prev(a_ep) : EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void Install(const char* a_when)
    {
        if (std::string(a_when) == "load") g_enabled = ::GetPrivateProfileIntA("General", "bCrashLog", 1, Settings::Path().c_str()) != 0;
        if (!g_enabled) {
            REX::INFO("crash log: off ([General] bCrashLog=0)");
            return;
        }
        const auto prev = ::SetUnhandledExceptionFilter(&Filter);
        if (prev != &Filter) g_prev = prev;  // the game or another plugin set one since: chained, ours runs first
        REX::INFO("crash log: unhandled-exception report + minidump into My Games\\Fallout4\\F4SE ({}; chained to {})", a_when,
            g_prev ? Where(reinterpret_cast<DWORD64>(g_prev)) : std::string("nothing"));
    }
}
