// Crash reporting: uncaught C++ exceptions, abort() and SEH exceptions (Windows) / fatal signals (Linux, PS5) print the
// reason and the host stack to stderr before the process dies. Windows symbolizes with dbghelp; Linux prints each
// frame as module + offset (and the symbol dladdr knows: the exe is linked -rdynamic) and an llvm-symbolizer command
// for file and line (docs/LINUX.md). The PS5 walks the frame pointers from the faulting context and prints offsets
// into build/ps5/vcs.elf with the llvm-symbolizer command for them (docs/PS5.md).
#include "crash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <exception>
#include <initializer_list>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#elif defined(__PROSPERO__)
#include <cstddef>
#include <pthread.h>
#include <pthread_np.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" const char __ehdr_start[] __attribute__((weak)); // the image's first byte (lld): offsets into vcs.elf
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
#elif defined(__linux__)
        // Frames as "module+0xoffset (symbol+0xoffset)", then one llvm-symbolizer line per module seen. Called from
        // signal handlers: no allocation; dladdr is not async-signal-safe in theory, but the process is ending anyway.
        void printStack(void *faultPc)
        {
            void *frames[64];
            int n = backtrace(frames, 64);
            int first = 0;
            if (faultPc) // skip the handler and the signal trampoline: start at the faulting instruction
                for (int i = 0; i < n; ++i)
                    if (frames[i] == faultPc)
                    {
                        first = i;
                        break;
                    }
            const char *modules[8] = {};
            for (int i = first; i < n; ++i)
            {
                Dl_info info{};
                const bool known = dladdr(frames[i], &info) != 0 && info.dli_fname;
                const unsigned long long off = known ? reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
                std::fprintf(stderr, "[crash]   #%02d %016llx %s+0x%llx", i - first,
                             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])), known ? info.dli_fname : "?", off);
                if (known && info.dli_sname)
                    std::fprintf(stderr, " (%s+0x%llx)", info.dli_sname,
                                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_saddr)));
                std::fprintf(stderr, "\n");
                for (const char *&m : modules)
                    if (known && (!m || m == info.dli_fname))
                    {
                        m = info.dli_fname;
                        break;
                    }
            }
            for (const char *m : modules)
            {
                if (!m)
                    break;
                std::fprintf(stderr, "[crash] file and line: llvm-symbolizer -e %s", m);
                for (int i = first; i < n; ++i)
                {
                    Dl_info info{};
                    if (dladdr(frames[i], &info) && info.dli_fname == m)
                        // the return address minus one: the call, not the instruction after it (not for the fault)
                        std::fprintf(stderr, " 0x%llx",
                                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase) -
                                                                     (i == first && faultPc ? 0 : 1)));
                }
                std::fprintf(stderr, "\n");
            }
            std::fflush(stderr);
        }

        void onFatalSignal(int sig, siginfo_t *info, void *context)
        {
            static volatile sig_atomic_t inside = 0;
            if (inside) // a fault while reporting one: just go
                _exit(128 + sig);
            inside = 1;
            const ucontext_t *uc = static_cast<const ucontext_t *>(context);
            void *pc = uc ? reinterpret_cast<void *>(uc->uc_mcontext.gregs[REG_RIP]) : nullptr;
            const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "signal";
            std::fprintf(stderr, "[crash] %s at %p", name, pc);
            if ((sig == SIGSEGV || sig == SIGBUS) && info)
                std::fprintf(stderr, " %s address %p", info->si_code == SEGV_ACCERR ? "no access to" : "bad", info->si_addr);
            std::fprintf(stderr, " (thread %ld)\n", static_cast<long>(syscall(SYS_gettid)));
            printStack(pc);
            // the default action: the exit status (and a core dump where enabled) say what happened
            signal(sig, SIG_DFL);
            raise(sig);
        }

#elif defined(__PROSPERO__)
        uintptr_t imageBase() { return reinterpret_cast<uintptr_t>(__ehdr_start); }

        // `pc` and the return addresses up the frame-pointer chain from `fp` (the runtime and our code keep frame
        // pointers; generated code may not, and the walk stops where the chain does), as offsets into vcs.elf
        void printStack(uintptr_t pc, uintptr_t fp)
        {
            const uintptr_t base = imageBase();
            uintptr_t frames[32];
            int n = 0;
            if (pc)
                frames[n++] = pc;
            for (int depth = 0; depth < 31 && fp && (fp & 7) == 0; ++depth)
            {
                const uintptr_t *frame = reinterpret_cast<const uintptr_t *>(fp);
                const uintptr_t next = frame[0], ret = frame[1];
                if (!ret)
                    break;
                frames[n++] = ret;
                if (next <= fp || next - fp > (16u << 20)) // the chain goes up the stack, a frame at a time
                    break;
                fp = next;
            }
            for (int i = 0; i < n; ++i)
                std::fprintf(stderr, "[crash]   #%02d %016llx vcs.elf+0x%llx\n", i, static_cast<unsigned long long>(frames[i]),
                             static_cast<unsigned long long>(frames[i] - base));
            std::fprintf(stderr, "[crash] file and line: llvm-symbolizer -e build/ps5/vcs.elf");
            for (int i = 0; i < n; ++i) // the return addresses minus one: the call, not the instruction after it
                std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(frames[i] - base - (i == 0 && pc ? 0 : 1)));
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }

        // The console's ucontext holds 48 bytes more than FreeBSD's before the machine context (measured by the
        // payload SDK fork's probe: uc_mcontext at 64, where FreeBSD's headers, and the SDK release's, say 16). The
        // registers are read at the measured place whichever headers built this; within it, FreeBSD's amd64 layout.
        constexpr size_t kPs5McontextOffset = 64;
        uintptr_t contextRegister(const void *context, size_t offset)
        {
            uintptr_t v = 0;
            std::memcpy(&v, static_cast<const char *>(context) + kPs5McontextOffset + offset, sizeof(v));
            return v;
        }

        void onFatalSignal(int sig, siginfo_t *info, void *context)
        {
            static volatile sig_atomic_t inside = 0;
            if (inside)
                _exit(128 + sig);
            inside = 1;
            const uintptr_t pc = context ? contextRegister(context, offsetof(mcontext_t, mc_rip)) : 0;
            const uintptr_t fp = context ? contextRegister(context, offsetof(mcontext_t, mc_rbp)) : 0;
            const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "signal";
            std::fprintf(stderr, "[crash] %s at %p (vcs.elf+0x%llx)", name, reinterpret_cast<void *>(pc),
                         static_cast<unsigned long long>(pc - imageBase()));
            if ((sig == SIGSEGV || sig == SIGBUS) && info)
                std::fprintf(stderr, " address %p", info->si_addr);
            std::fprintf(stderr, " (thread %d)\n", pthread_getthreadid_np());
            printStack(pc, fp);
            signal(sig, SIG_DFL);
            raise(sig);
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
#if defined(_WIN32) || defined(__linux__)
            printStack(nullptr);
#elif defined(__PROSPERO__)
            printStack(0, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
#endif
            std::_Exit(3);
        }

        void onAbort(int)
        {
            std::fprintf(stderr, "[crash] abort()\n");
#if defined(_WIN32) || defined(__linux__)
            printStack(nullptr);
#elif defined(__PROSPERO__)
            printStack(0, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
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
#elif defined(__linux__)
        // the handler runs on its own stack, so a stack overflow is reported too (the main thread's; other threads
        // report on their own stack)
        static char altStack[64 * 1024];
        stack_t ss{};
        ss.ss_sp = altStack;
        ss.ss_size = sizeof(altStack);
        sigaltstack(&ss, nullptr);
        void *warm[1];
        backtrace(warm, 1); // loads the unwinder now, not inside a signal handler
        struct sigaction sa{};
        sa.sa_sigaction = &onFatalSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE})
            sigaction(sig, &sa, nullptr);
#elif defined(__PROSPERO__)
        static char altStack[64 * 1024];
        stack_t ss{};
        ss.ss_sp = altStack;
        ss.ss_size = sizeof(altStack);
        sigaltstack(&ss, nullptr);
        struct sigaction sa{};
        sa.sa_sigaction = &onFatalSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE})
            sigaction(sig, &sa, nullptr);
        std::fprintf(stderr, "[crash] handlers on (vcs.elf at %p)\n", static_cast<const void *>(__ehdr_start));
#endif
    }
}
