#include "profiler.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
                    keep = wcscmp(name, L"GameThread") == 0;
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
            // is the host function at `host` generated code (FUN_*), not a hook installed with replaceFunction?
            std::unordered_map<uintptr_t, bool> generatedCache;
            auto isGenerated = [&generatedCache](uintptr_t host) {
                auto found = generatedCache.find(host);
                if (found != generatedCache.end())
                    return found->second;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 displacement = 0;
                const bool generated = !SymFromAddr(GetCurrentProcess(), host, &displacement, sym) || std::strncmp(sym->Name, "FUN_", 4) == 0 ||
                                       std::strstr(sym->Name, "::FUN_") != nullptr;
                generatedCache.emplace(host, generated);
                return generated;
            };
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
                    c.ContextFlags = CONTEXT_CONTROL;
                    const bool got = GetThreadContext(h, &c) != 0;
                    ResumeThread(h);
                    if (!got)
                        continue;
                    ++total;
                    const uintptr_t rip = c.Rip;
                    auto it = std::upper_bound(map.begin(), map.end(), rip,
                                               [](uintptr_t v, const HostFn &f) { return v < f.host; });
                    // Generated functions are at most a few hundred KB of host code each.
                    // (not past the last one: whatever the linker placed after it - e.g. compiled VU1 code - was
                    // counted as that function before)
                    // (a replaced function's slot points at a host hook outside the generated code: a sample after it
                    // is host code, not that guest function - checked by the symbol name below)
                    if (it != map.begin() && it != map.end() && rip - std::prev(it)->host < 0x100000 &&
                        isGenerated(std::prev(it)->host))
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
