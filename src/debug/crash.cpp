// Crash reporting: uncaught C++ exceptions, abort() and fatal signals print the reason and the host stack to stderr
// (logs/vcs.log) before the process dies. The stack is walked along the frame pointers from the faulting context and
// printed as offsets into build/ps5/vcs.elf, with the llvm-symbolizer command for file and line (docs/PS5.md).
#include "crash.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <exception>
#include <initializer_list>

#include <pthread.h>
#include <pthread_np.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" const char __ehdr_start[] __attribute__((weak)); // the image's first byte (lld): offsets into vcs.elf

namespace vcs
{
    namespace
    {
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
            printStack(0, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
            std::_Exit(3);
        }

        void onAbort(int)
        {
            std::fprintf(stderr, "[crash] abort()\n");
            printStack(0, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
            std::_Exit(3);
        }
    }

    void installCrashHandlers()
    {
        std::set_terminate(&onTerminate);
        std::signal(SIGABRT, &onAbort);
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
    }
}
