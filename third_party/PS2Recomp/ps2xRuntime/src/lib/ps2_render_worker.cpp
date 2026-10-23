// [gta-recomp] The render submission worker: see runtime/ps2_render_worker.h.
#include "runtime/ps2_render_worker.h"
#include "runtime/ps2_perf.h"
#include "ThreadNaming.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <xmmintrin.h>

namespace ps2_render_worker
{
    namespace
    {
        constexpr size_t kReasons = static_cast<size_t>(FenceReason::Count);
        constexpr const char *kReasonNames[kReasons] = {"gs-priv", "gif-reg", "vif1-reg", "fifo", "vu1-mem", "gs-stub", "other"};
        constexpr size_t kMaxItems = 65536u; // a bound on queued work besides the frame bound

        struct Item
        {
            std::function<void()> work;
            bool frameEnd = false;
            unsigned csr = 0u; // the game thread's MXCSR (rounding, flush-to-zero): VU1 and GS work runs in it
        };

        struct State
        {
            std::mutex mutex;
            std::condition_variable workCv; // the worker waits for work
            std::condition_variable doneCv; // the game thread waits for the worker to get on
            std::deque<Item> queue;
            bool busy = false, stopping = false, started = false;
            uint32_t framesQueued = 0u; // frameEnd items not finished yet
            uint32_t waiters = 0u;      // game-thread waits in progress (doneCv is only signalled for them)
            std::thread thread;

            std::mutex poolMutex;
            std::vector<std::vector<uint8_t>> pool;

            // statistics (mutex)
            uint64_t items = 0u, frames = 0u, postWaits = 0u, postWaitNs = 0u;
            uint64_t fences[kReasons]{}, fenceWaits[kReasons]{}, fenceWaitNs = 0u;
            std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
        };

        // Never destroyed: the worker may still be parked on it while static destructors run at exit.
        State &state()
        {
            static State *s = new State();
            return *s;
        }

        std::atomic<int> g_enabled{-1}; // -1 until configure()
        thread_local bool t_onWorker = false;

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        uint32_t framesAhead()
        {
            static const uint32_t ahead = [] {
                const char *v = std::getenv("PS2X_RENDER_THREAD_AHEAD");
                const int n = v ? std::atoi(v) : 1;
                return static_cast<uint32_t>(n < 1 ? 1 : n);
            }();
            return ahead;
        }

        void run()
        {
            ThreadNaming::SetCurrentThreadName("RenderWorker");
            t_onWorker = true;
            State &s = state();
            std::unique_lock<std::mutex> lock(s.mutex);
            for (;;)
            {
                s.workCv.wait(lock, [&] { return !s.queue.empty() || s.stopping; });
                if (s.queue.empty())
                    break;
                Item item = std::move(s.queue.front());
                s.queue.pop_front();
                s.busy = true;
                lock.unlock();
                if (_mm_getcsr() != item.csr)
                    _mm_setcsr(item.csr);
                const uint64_t start = nowNs();
                try
                {
                    item.work();
                }
                catch (const std::exception &e)
                {
                    static int reported = 0;
                    if (reported++ < 20)
                        std::fprintf(stderr, "[render-worker] work failed: %s\n", e.what());
                }
                catch (...)
                {
                    static int reported = 0;
                    if (reported++ < 20)
                        std::fprintf(stderr, "[render-worker] work failed (unknown exception)\n");
                }
                item.work = nullptr; // its captures (chain buffers) go outside the lock
                ps2perf::addWorkerTime(nowNs() - start);
                lock.lock();
                s.busy = false;
                if (item.frameEnd)
                    --s.framesQueued;
                if (s.waiters)
                    s.doneCv.notify_all();
            }
        }

        // Caller holds the lock.
        void maybeLog(State &s)
        {
            static const bool log = [] {
                const char *v = std::getenv("PS2X_RENDER_THREAD_LOG");
                return v && *v && *v != '0';
            }();
            if (!log)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now - s.lastLog < std::chrono::seconds(5))
                return;
            const double secs = std::chrono::duration<double>(now - s.lastLog).count();
            s.lastLog = now;
            std::fprintf(stderr, "[render-worker] %.0f items/s, %.1f frames/s, post waits %llu (%.1f ms/s); fences (waited):",
                         s.items / secs, s.frames / secs, static_cast<unsigned long long>(s.postWaits), s.postWaitNs / 1e6 / secs);
            for (size_t r = 0; r < kReasons; ++r)
                if (s.fences[r])
                    std::fprintf(stderr, " %s %llu (%llu)", kReasonNames[r], static_cast<unsigned long long>(s.fences[r]),
                                 static_cast<unsigned long long>(s.fenceWaits[r]));
            std::fprintf(stderr, ", fence wait %.1f ms/s\n", s.fenceWaitNs / 1e6 / secs);
            s.items = s.frames = s.postWaits = s.postWaitNs = s.fenceWaitNs = 0u;
            for (size_t r = 0; r < kReasons; ++r)
                s.fences[r] = s.fenceWaits[r] = 0u;
        }
    }

    void configure(bool defaultOn)
    {
        const char *v = std::getenv("PS2X_RENDER_THREAD");
        const bool on = v && *v ? *v != '0' : defaultOn;
        g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
        std::fprintf(stderr, "[render-worker] %s\n", on ? "on: render submission runs on its own thread" : "off");
    }

    bool enabled()
    {
        return g_enabled.load(std::memory_order_relaxed) == 1;
    }

    bool onWorker()
    {
        return t_onWorker;
    }

    void post(std::function<void()> work, bool frameEnd)
    {
        if (!enabled() || t_onWorker)
        {
            work();
            return;
        }
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.started)
        {
            s.started = true;
            s.thread = std::thread(run);
        }
        auto full = [&] { return s.queue.size() >= kMaxItems || (frameEnd && s.framesQueued >= framesAhead()); };
        if (full())
        {
            const uint64_t start = nowNs();
            ++s.waiters;
            s.doneCv.wait(lock, [&] { return !full(); });
            --s.waiters;
            ++s.postWaits;
            s.postWaitNs += nowNs() - start;
        }
        s.queue.push_back({std::move(work), frameEnd, _mm_getcsr()});
        if (frameEnd)
        {
            ++s.framesQueued;
            ++s.frames;
        }
        ++s.items;
        maybeLog(s);
        lock.unlock();
        s.workCv.notify_one();
    }

    void fence(FenceReason reason)
    {
        if (!enabled() || t_onWorker)
            return;
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        const size_t r = static_cast<size_t>(reason);
        ++s.fences[r];
        if (s.queue.empty() && !s.busy)
            return;
        const uint64_t start = nowNs();
        ++s.waiters;
        s.doneCv.wait(lock, [&] { return s.queue.empty() && !s.busy; });
        --s.waiters;
        ++s.fenceWaits[r];
        s.fenceWaitNs += nowNs() - start;
    }

    std::vector<uint8_t> takeBuffer()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.poolMutex);
        if (s.pool.empty())
            return {};
        std::vector<uint8_t> buffer = std::move(s.pool.back());
        s.pool.pop_back();
        buffer.clear();
        return buffer;
    }

    void recycleBuffer(std::vector<uint8_t> &&buffer)
    {
        if (buffer.capacity() == 0u)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.poolMutex);
        if (s.pool.size() < 8u)
            s.pool.push_back(std::move(buffer));
    }

    void shutdown()
    {
        State &s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.started || s.stopping)
                return;
            s.stopping = true;
        }
        s.workCv.notify_one();
        if (s.thread.joinable())
            s.thread.join();
    }
}
