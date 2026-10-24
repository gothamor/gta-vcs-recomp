// [gta-recomp] Guest RAM write watch - see runtime/ps2_write_watch.h.
#include "runtime/ps2_write_watch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ps2writewatch
{
    namespace
    {
        uint8_t *g_base = nullptr; // the watched allocation (nullptr: not watched)
        size_t g_bytes = 0;
        std::vector<uint32_t> g_generation;
        std::vector<void *> g_written; // GetWriteWatch's output, kept

        struct Stats
        {
            uint64_t folds = 0, pages = 0, maxPages = 0;
            double ms = 0.0;
        } g_stats;
        auto g_reportAt = std::chrono::steady_clock::now();

        bool wanted()
        {
            const char *e = std::getenv("PS2X_WRITE_WATCH");
            return !(e && e[0] == '0');
        }
    }

    uint8_t *allocate(size_t bytes)
    {
#if defined(_WIN32)
        if (wanted() && !g_base)
        {
            void *p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
            if (p)
            {
                g_base = static_cast<uint8_t *>(p);
                g_bytes = bytes;
                g_generation.assign((bytes + kPageSize - 1u) >> kPageShift, 1u);
                g_written.resize(g_generation.size());
                ResetWriteWatch(p, bytes); // committed memory is zero: nothing written yet
                return g_base;
            }
            std::fprintf(stderr, "[write-watch] VirtualAlloc with MEM_WRITE_WATCH failed (%lu): not watched\n", GetLastError());
        }
#endif
        uint8_t *ram = new uint8_t[bytes];
        std::memset(ram, 0, bytes);
        return ram;
    }

    void release(uint8_t *ram)
    {
        if (!ram)
            return;
#if defined(_WIN32)
        if (ram == g_base)
        {
            VirtualFree(ram, 0, MEM_RELEASE);
            g_base = nullptr;
            g_bytes = 0;
            g_generation.clear();
            return;
        }
#endif
        delete[] ram;
    }

    bool enabled() { return g_base != nullptr; }

    uint32_t fold()
    {
#if defined(_WIN32)
        if (!g_base)
            return 0u;
        const auto start = std::chrono::steady_clock::now();
        ULONG_PTR count = static_cast<ULONG_PTR>(g_written.size());
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, g_bytes, g_written.data(), &count, &granularity) != 0)
            return 0u;
        for (ULONG_PTR i = 0; i < count; ++i)
        {
            const size_t page = static_cast<size_t>(static_cast<uint8_t *>(g_written[i]) - g_base) >> kPageShift;
            if (page < g_generation.size())
                ++g_generation[page];
        }
        static const bool log = std::getenv("PS2X_WRITE_WATCH_LOG") != nullptr;
        if (log)
        {
            const auto now = std::chrono::steady_clock::now();
            ++g_stats.folds;
            g_stats.pages += count;
            g_stats.maxPages = std::max<uint64_t>(g_stats.maxPages, count);
            g_stats.ms += std::chrono::duration<double, std::milli>(now - start).count();
            if (now - g_reportAt > std::chrono::seconds(5))
            {
                g_reportAt = now;
                std::fprintf(stderr, "[write-watch] %llu folds: %.0f pages written per fold (max %llu of %zu), %.3f ms per fold\n",
                             (unsigned long long)g_stats.folds, g_stats.pages / double(g_stats.folds), (unsigned long long)g_stats.maxPages,
                             g_generation.size(), g_stats.ms / double(g_stats.folds));
                g_stats = Stats{};
            }
        }
        return static_cast<uint32_t>(count);
#else
        return 0u;
#endif
    }

    uint32_t generation(uint32_t page) { return page < g_generation.size() ? g_generation[page] : 0u; }

    bool unchangedSince(uint32_t page, uint32_t gen) { return g_base && page < g_generation.size() && g_generation[page] == gen; }
}
