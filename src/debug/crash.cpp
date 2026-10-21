// Crash reporting: uncaught C++ exceptions, abort() and SEH exceptions print the
// reason and a symbolized host stack to stderr before the process dies.
#include "crash.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

namespace vcs
{
    namespace
    {
#if defined(_WIN32)
        void printStack(CONTEXT *context)
        {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            SymInitialize(process, nullptr, TRUE);
            CONTEXT local{};
            if (!context)
            {
                RtlCaptureContext(&local);
                context = &local;
            }
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context->Rip;
            frame.AddrFrame.Offset = context->Rbp;
            frame.AddrStack.Offset = context->Rsp;
            frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 40; ++i)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                    frame.AddrPC.Offset == 0)
                    break;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                IMAGEHLP_LINE64 line{sizeof(line)};
                DWORD lineDisp = 0;
                const bool haveSym = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym);
                const bool haveLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line);
                std::fprintf(stderr, "[crash]   #%02d %016llx %s+0x%llx %s:%lu\n", i,
                             static_cast<unsigned long long>(frame.AddrPC.Offset), haveSym ? sym->Name : "?",
                             static_cast<unsigned long long>(disp), haveLine ? line.FileName : "",
                             haveLine ? line.LineNumber : 0ul);
            }
            std::fflush(stderr);
        }

        // First-chance faults (seen before any handler or a later __fastfail): log the first few.
        LONG WINAPI onFirstChance(EXCEPTION_POINTERS *info)
        {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
                code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000409u)
                return EXCEPTION_CONTINUE_SEARCH;
            static volatile LONG reported = 0;
            if (InterlockedIncrement(&reported) > 3)
                return EXCEPTION_CONTINUE_SEARCH;
            std::fprintf(stderr, "[crash] first-chance exception 0x%08lx at %p (thread %lu)", code,
                         info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
            if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
                std::fprintf(stderr, " %s 0x%llx", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                             static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
            std::fprintf(stderr, "\n");
            printStack(info->ContextRecord);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The CRT's invalid-parameter path ends in __fastfail (0xC0000409) without any exception.
        void onInvalidParameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned int line,
                                uintptr_t)
        {
            std::fprintf(stderr, "[crash] CRT invalid parameter (%ls in %ls at %ls:%u)\n", expression ? expression : L"?",
                         function ? function : L"?", file ? file : L"?", line);
            printStack(nullptr);
            std::_Exit(3);
        }

        LONG WINAPI onUnhandled(EXCEPTION_POINTERS *info)
        {
            std::fprintf(stderr, "[crash] unhandled exception 0x%08lx at %p\n", info->ExceptionRecord->ExceptionCode,
                         info->ExceptionRecord->ExceptionAddress);
            printStack(info->ContextRecord);
            return EXCEPTION_CONTINUE_SEARCH;
        }
#endif

        void onTerminate()
        {
            std::fprintf(stderr, "[crash] std::terminate");
            if (auto ep = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, ": %s", e.what());
                }
                catch (...)
                {
                    std::fprintf(stderr, ": non-std exception");
                }
            }
            std::fprintf(stderr, "\n");
#if defined(_WIN32)
            printStack(nullptr);
#endif
            std::_Exit(3);
        }

        void onAbort(int)
        {
            std::fprintf(stderr, "[crash] abort()\n");
#if defined(_WIN32)
            printStack(nullptr);
#endif
            std::_Exit(3);
        }
    }

    void installCrashHandlers()
    {
        std::set_terminate(&onTerminate);
        std::signal(SIGABRT, &onAbort);
#if defined(_WIN32)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetUnhandledExceptionFilter(&onUnhandled);
        _set_invalid_parameter_handler(&onInvalidParameter);
        if (std::getenv("VCS_CRASH_FIRST_CHANCE"))
            AddVectoredExceptionHandler(1, &onFirstChance);
#endif
    }
}
