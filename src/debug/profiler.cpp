#include "profiler.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <string>
#endif

namespace vcs
{
#if defined(_WIN32)
    namespace
    {
        // up to n return addresses of a suspended thread's stack from its context. A fault while unwinding (a thread
        // caught where its unwind data does not describe the stack yet) ends the walk instead of the process.
        int unwindFrames(CONTEXT u, uintptr_t *out, int n)
        {
            int count = 0;
            __try
            {
                for (; count < n;)
                {
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
                    if (!fe)
                    {
                        u.Rip = *reinterpret_cast<const DWORD64 *>(u.Rsp); // a leaf: the return address on top
                        u.Rsp += 8;
                    }
                    else
                    {
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fe, &u, &handlerData, &establisher, nullptr);
                    }
                    if (!u.Rip)
                        break;
                    out[count++] = static_cast<uintptr_t>(u.Rip);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return count;
        }

        struct HostFn
        {
            uintptr_t host;
            uint32_t guest; // lowest guest address mapped to this host function = its start
        };

        std::vector<HostFn> buildHostMap()
        {
            std::unordered_map<uintptr_t, uint32_t> lowest;
            for (uint32_t slot = 0; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
            {
                if (auto fn = g_ps2RecompiledFunctionTable[slot])
                {
                    const uintptr_t host = reinterpret_cast<uintptr_t>(fn);
                    const uint32_t guest = g_ps2RecompiledFunctionTableBase + slot * 4u;
                    auto [it, inserted] = lowest.emplace(host, guest);
                    if (!inserted && guest < it->second)
                        it->second = guest;
                }
            }
            std::vector<HostFn> map;
            map.reserve(lowest.size());
            for (const auto &[host, guest] : lowest)
                map.push_back({host, guest});
            std::sort(map.begin(), map.end(), [](const HostFn &a, const HostFn &b) { return a.host < b.host; });
            return map;
        }

        std::vector<HANDLE> findGameThreads()
        {
            using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            auto getDesc = reinterpret_cast<GetThreadDescriptionFn>(
                GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription"));
            std::vector<HANDLE> out;
            if (!getDesc)
                return out;
            // VCS_PROFILE_THREAD=RenderWorker: sample the render submission worker instead of the game thread
            std::wstring wanted = L"GameThread";
            if (const char *thread = std::getenv("VCS_PROFILE_THREAD"))
                wanted = std::wstring(thread, thread + std::strlen(thread));
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h)
                    continue;
                PWSTR name = nullptr;
                bool keep = false;
                if (SUCCEEDED(getDesc(h, &name)) && name)
                {
                    keep = wanted == name;
                    LocalFree(name);
                }
                if (keep)
                    out.push_back(h);
                else
                    CloseHandle(h);
            }
            CloseHandle(snap);
            return out;
        }
    }

    void startProfiler(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_PROFILE");
        if (!env || !*env)
            return;
        const double reportSeconds = std::atof(env) > 0.0 ? std::atof(env) : 5.0;

        std::thread([&runtime, reportSeconds]()
                    {
            const std::vector<HostFn> map = buildHostMap();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            std::vector<HANDLE> threads;
            // is the host function at `host` generated code (FUN_*), not a hook installed with replaceFunction - and how
            // long is it? [gta-recomp] A sample counts for a generated function only within the function's own symbol
            // size: in a ThinLTO / PGO build the linker places hot runtime functions between generated ones, and
            // "the nearest generated function below, within 1 MB" counted them as guest code.
            struct GeneratedInfo
            {
                bool generated = false;
                uint64_t size = 0x100000u; // unknown size: the old 1 MB window
            };
            std::unordered_map<uintptr_t, GeneratedInfo> generatedCache;
            auto generatedInfo = [&generatedCache](uintptr_t host) -> const GeneratedInfo & {
                auto found = generatedCache.find(host);
                if (found != generatedCache.end())
                    return found->second;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 displacement = 0;
                GeneratedInfo info;
                const bool known = SymFromAddr(GetCurrentProcess(), host, &displacement, sym) != 0;
                info.generated = !known || std::strncmp(sym->Name, "FUN_", 4) == 0 || std::strstr(sym->Name, "::FUN_") != nullptr;
                if (known && sym->Size && displacement == 0)
                    info.size = sym->Size;
                return generatedCache.emplace(host, info).first->second;
            };
            // pc inside the generated function that starts at `host`
            auto withinGenerated = [&generatedInfo](uintptr_t host, uintptr_t pc) {
                const GeneratedInfo &info = generatedInfo(host);
                return info.generated && pc - host < info.size;
            };
            // VCS_PROFILE_CALLERS=name[,name...]: for samples in host functions whose name contains one of these, the call
            // stack (unwound up to the first generated function) - who calls memchr / memmove / the allocator
            std::vector<std::string> callersOf;
            if (const char *e = std::getenv("VCS_PROFILE_CALLERS"))
                for (const char *p = e; *p;)
                {
                    const char *comma = std::strchr(p, ',');
                    callersOf.emplace_back(p, comma ? comma : p + std::strlen(p));
                    p = comma ? comma + 1 : p + std::strlen(p);
                }
            std::unordered_map<uintptr_t, bool> callerMatch; // host RIP -> its function is one of callersOf
            auto symbolName = [](uintptr_t addr) -> std::string {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 displacement = 0;
                return SymFromAddr(GetCurrentProcess(), addr, &displacement, sym) ? std::string(sym->Name) : std::string("?");
            };
            std::map<std::vector<uintptr_t>, uint64_t> stacks; // frames (innermost first) -> samples
            std::unordered_map<uint32_t, uint64_t> hits;    // guest function -> samples
            std::unordered_map<uintptr_t, uint64_t> other;  // non-generated host RIP (page) -> samples
            uint64_t total = 0;
            auto lastReport = std::chrono::steady_clock::now();
            while (!runtime.isStopRequested())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if (threads.empty())
                {
                    threads = findGameThreads();
                    continue;
                }
                for (HANDLE h : threads)
                {
                    if (SuspendThread(h) == static_cast<DWORD>(-1))
                        continue;
                    CONTEXT c{};
                    c.ContextFlags = callersOf.empty() ? CONTEXT_CONTROL : CONTEXT_FULL;
                    const bool got = GetThreadContext(h, &c) != 0;
                    const uintptr_t rip = c.Rip;
                    auto it = std::upper_bound(map.begin(), map.end(), rip,
                                               [](uintptr_t v, const HostFn &f) { return v < f.host; });
                    auto inGenerated = [&](uintptr_t pc) {
                        auto f = std::upper_bound(map.begin(), map.end(), pc, [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                        return f != map.begin() && f != map.end() && withinGenerated(std::prev(f)->host, pc);
                    };
                    if (got && !callersOf.empty() && !inGenerated(rip))
                    {
                        auto m = callerMatch.find(rip);
                        if (m == callerMatch.end())
                        {
                            const std::string name = symbolName(rip);
                            bool match = false;
                            for (const std::string &want : callersOf)
                                match |= name.find(want) != std::string::npos;
                            m = callerMatch.emplace(rip, match).first;
                        }
                        if (m->second) // unwind while the thread is still suspended
                        {
                            uintptr_t raw[12];
                            const int n = unwindFrames(c, raw, 12);
                            std::vector<uintptr_t> frames{rip}; // the sampled function first, then its callers
                            for (int i = 0; i < n; ++i)
                            {
                                frames.push_back(raw[i]);
                                if (inGenerated(raw[i]))
                                    break;
                            }
                            ++stacks[frames];
                        }
                    }
                    ResumeThread(h);
                    if (!got)
                        continue;
                    ++total;
                    // Generated functions are at most a few hundred KB of host code each.
                    // (not past the last one: whatever the linker placed after it - e.g. compiled VU1 code - was
                    // counted as that function before)
                    // (a replaced function's slot points at a host hook outside the generated code: a sample after it
                    // is host code, not that guest function - checked by the symbol name below)
                    if (it != map.begin() && it != map.end() && withinGenerated(std::prev(it)->host, rip))
                        ++hits[std::prev(it)->guest];
                    else
                        ++other[rip];
                }
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - lastReport).count() < reportSeconds || total == 0)
                    continue;
                lastReport = now;
                std::vector<std::pair<uint32_t, uint64_t>> top(hits.begin(), hits.end());
                std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
                uint64_t guestTotal = 0;
                for (const auto &[pc, n] : hits)
                    guestTotal += n;
                std::fprintf(stderr, "[profile] %llu samples, %.1f%% in generated (guest) code\n", static_cast<unsigned long long>(total),
                             100.0 * guestTotal / total);
                for (size_t i = 0; i < top.size() && i < 15; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  guest 0x%08x\n", 100.0 * top[i].second / total, top[i].first);
                // VCS_PROFILE_GUEST_FILE=<path>: every guest function's samples, appended per report window ("window <s> total
                // N" then "0xADDR N" lines; s = seconds since the profiler started) - for summing by subsystem offline
                static const char *guestFile = std::getenv("VCS_PROFILE_GUEST_FILE");
                static const auto profileStart = std::chrono::steady_clock::now();
                if (guestFile && *guestFile)
                    if (FILE *f = std::fopen(guestFile, "a"))
                    {
                        std::fprintf(f, "window %.0f total %llu\n", std::chrono::duration<double>(now - profileStart).count(),
                                     static_cast<unsigned long long>(total));
                        for (const auto &[pc, n] : top)
                            std::fprintf(f, "0x%08x %llu\n", pc, static_cast<unsigned long long>(n));
                        std::fclose(f);
                    }
                // Resolve non-generated samples to host symbols (needs the PDB; see VCS_RUNTIME_DEBUG_INFO).
                std::unordered_map<std::string, uint64_t> byName;
                for (const auto &[rip, count] : other)
                {
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    std::string name;
                    if (SymFromAddr(GetCurrentProcess(), rip, &displacement, sym))
                        name = sym->Name;
                    else
                    {
                        HMODULE mod = nullptr;
                        char modName[MAX_PATH] = "?";
                        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCSTR>(rip), &mod))
                            GetModuleFileNameA(mod, modName, MAX_PATH);
                        const char *leaf = std::strrchr(modName, '\\');
                        name = std::string(leaf ? leaf + 1 : modName) + "!?";
                    }
                    byName[name] += count;
                }
                std::vector<std::pair<std::string, uint64_t>> rest(byName.begin(), byName.end());
                std::sort(rest.begin(), rest.end(), [](auto &a, auto &b) { return a.second > b.second; });
                for (size_t i = 0; i < rest.size() && i < 30; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  host %s\n", 100.0 * rest[i].second / total, rest[i].first.c_str());
                // VCS_PROFILE_LINES=<name>: the source lines of the host functions whose name contains it
                static const char *linesOf = std::getenv("VCS_PROFILE_LINES");
                if (linesOf && *linesOf)
                {
                    std::unordered_map<std::string, uint64_t> byLine;
                    for (const auto &[rip, count] : other)
                    {
                        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        sym->MaxNameLen = 255;
                        DWORD64 displacement = 0;
                        if (!SymFromAddr(GetCurrentProcess(), rip, &displacement, sym) || !std::strstr(sym->Name, linesOf))
                            continue;
                        IMAGEHLP_LINE64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD lineDisplacement = 0;
                        std::string key = "?";
                        if (SymGetLineFromAddr64(GetCurrentProcess(), rip, &lineDisplacement, &line))
                        {
                            const char *leaf = std::strrchr(line.FileName, '\\');
                            key = std::string(leaf ? leaf + 1 : line.FileName) + ":" + std::to_string(line.LineNumber);
                        }
                        byLine[key] += count;
                    }
                    std::vector<std::pair<std::string, uint64_t>> lines(byLine.begin(), byLine.end());
                    std::sort(lines.begin(), lines.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < lines.size() && i < 25; ++i)
                        std::fprintf(stderr, "[profile]     %5.1f%%  line %s\n", 100.0 * lines[i].second / total, lines[i].first.c_str());
                }
                if (!stacks.empty())
                {
                    // by caller chain, names resolved now (a guest function by its address)
                    std::unordered_map<std::string, uint64_t> byChain;
                    for (const auto &[frames, count] : stacks)
                    {
                        std::string chain;
                        for (uintptr_t pc : frames)
                        {
                            auto f = std::upper_bound(map.begin(), map.end(), pc, [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                            const bool gen = f != map.begin() && f != map.end() && withinGenerated(std::prev(f)->host, pc);
                            char guest[24];
                            if (gen)
                                std::snprintf(guest, sizeof(guest), "guest 0x%08x", std::prev(f)->guest);
                            chain += (chain.empty() ? "" : " < ") + (gen ? std::string(guest) : symbolName(pc));
                        }
                        byChain[chain] += count;
                    }
                    std::vector<std::pair<std::string, uint64_t>> chains(byChain.begin(), byChain.end());
                    std::sort(chains.begin(), chains.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < chains.size() && i < 80; ++i)
                        std::fprintf(stderr, "[profile]     %5.1f%%  called from %s\n", 100.0 * chains[i].second / total, chains[i].first.c_str());
                    stacks.clear();
                }
                std::fflush(stderr);
                hits.clear();
                other.clear();
                total = 0;
            } })
            .detach();
    }
#else
    void startProfiler(PS2Runtime &) {}
#endif
}
