// [gta-recomp] Native IOP services - see runtime/ps2_iop_native.h.
#include "runtime/ps2_iop_native.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2_iop_native
{
    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::unordered_map<uint32_t, Server> servers;
        };

        State &state()
        {
            static State *s = new State(); // never destroyed: servers may be called while the runtime shuts down
            return *s;
        }

        std::atomic<bool> g_emulatedIop{[]
                                        {
                                            const char *v = std::getenv("PS2X_IOP");
                                            return !(v && v[0] == '0');
                                        }()};
        std::atomic<bool> g_emulatedAudioMuted{false};
        std::atomic<Clock> g_clock{nullptr};
        std::atomic<uint64_t> g_eeCycles{0};
    }

    void registerServer(uint32_t sid, Server server)
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().servers[sid] = std::move(server);
    }

    bool hasServer(uint32_t sid)
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        return state().servers.count(sid) != 0;
    }

    bool dispatch(uint8_t *rdram, const ps2x::iop::RpcRequest &request, ps2x::iop::RpcResult &result)
    {
        Server server;
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            const auto it = state().servers.find(request.sid);
            if (it == state().servers.end())
                return false;
            server = it->second;
        }
        server(rdram, request);
        // as the emulated IOP answers a server that returned (ps2xIOP iop_rpc.cpp IopRpcBridge::handleRpc)
        result = {};
        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ps2x::iop::ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return true;
    }

    void setClock(Clock clock) { g_clock.store(clock); }

    void tick(uint64_t eeCycles)
    {
        g_eeCycles.fetch_add(eeCycles, std::memory_order_relaxed);
        if (const Clock clock = g_clock.load(std::memory_order_acquire))
            clock(eeCycles);
    }

    uint64_t eeCycles() { return g_eeCycles.load(std::memory_order_relaxed); }

    bool recording()
    {
        static const bool on = std::getenv("PS2X_RPC_RECORD") != nullptr;
        return on;
    }

    void record(const uint8_t *rdram, const ps2x::iop::RpcRequest &request, uint64_t iopTimeEeCycles)
    {
        static const char *path = std::getenv("PS2X_RPC_RECORD");
        if (!path)
            return;
        static const std::vector<uint32_t> sids = []
        {
            std::vector<uint32_t> list;
            const char *v = std::getenv("PS2X_RPC_RECORD_SIDS");
            for (const char *p = v; p && *p;)
            {
                char *end = nullptr;
                const unsigned long sid = std::strtoul(p, &end, 16);
                if (end == p)
                    break;
                list.push_back(static_cast<uint32_t>(sid));
                p = *end == ',' ? end + 1 : end;
            }
            return list;
        }();
        if (!sids.empty() && std::find(sids.begin(), sids.end(), request.sid) == sids.end())
            return;
        static std::mutex mutex;
        static std::FILE *file = std::fopen(path, "wb");
        if (!file)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        const uint64_t time = iopTimeEeCycles;
        const uint32_t send = request.send.address & 0x1FFFFFFFu;
        const uint32_t size = (rdram && send && send + request.send.size <= 0x2000000u) ? request.send.size : 0u;
        const uint32_t header[4] = {request.sid, request.function, size, request.receive.size};
        std::fwrite(&time, 8, 1, file);
        std::fwrite(header, 4, 4, file);
        if (size)
            std::fwrite(rdram + send, 1, size, file);
        std::fflush(file); // the game leaves through _Exit
    }

    void setEmulatedIop(bool enabled) { g_emulatedIop.store(enabled); }
    bool emulatedIop() { return g_emulatedIop.load(); }
    void setEmulatedAudioMuted(bool muted) { g_emulatedAudioMuted.store(muted); }
    bool emulatedAudioMuted() { return g_emulatedAudioMuted.load(); }
}
