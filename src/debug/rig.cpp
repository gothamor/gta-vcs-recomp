// See rig.h.
#include "debug/rig.h"

#include "ps2_runtime.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vcs::rig
{
    namespace
    {
        constexpr uint32_t kRamSize = 0x02000000u; // 32 MB EE RAM
        constexpr uint32_t kRamMask = kRamSize - 1u;

        uint8_t *g_rdram = nullptr;
        std::atomic<uint64_t> g_frames{0};
        std::atomic<bool> g_paused{false};
        std::mutex g_jobsMutex;
        std::deque<std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>> g_jobs;

        bool parseU32(const std::string &text, uint32_t &out)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            const unsigned long v = std::strtoul(text.c_str(), &end, 0);
            if (!end || *end)
                return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        std::vector<std::string> words(const std::string &args)
        {
            std::istringstream in(args);
            std::vector<std::string> out;
            for (std::string w; in >> w;)
                out.push_back(w);
            return out;
        }

        float loadF32(uint32_t addr)
        {
            float v;
            std::memcpy(&v, g_rdram + (addr & kRamMask), 4);
            return v;
        }

        std::string hex(const uint8_t *data, size_t n)
        {
            static const char *digits = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                s.push_back(digits[data[i] >> 4]);
                s.push_back(digits[data[i] & 15]);
            }
            return s;
        }

        std::string fmt(const char *format, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        // Range arguments "lo hi" at words[i], defaulting to all of RAM.
        void range(const std::vector<std::string> &w, size_t i, uint32_t &lo, uint32_t &hi)
        {
            lo = 0x00100000u;
            hi = kRamSize;
            if (w.size() > i + 1)
            {
                parseU32(w[i], lo);
                parseU32(w[i + 1], hi);
            }
            lo &= ~3u;
            hi = std::min(hi, kRamSize);
        }

        void registerMemoryCommands()
        {
            // peek <addr> [bytes=64] -> ok <hex>
            ps2control::addCommand("peek", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0, n = 64;
                if (w.empty() || !parseU32(w[0], addr) || (w.size() > 1 && !parseU32(w[1], n)) || n == 0 || n > 4096)
                    return reply("error usage: peek <addr> [bytes<=4096]");
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                addr &= kRamMask;
                n = std::min(n, kRamSize - addr);
                reply("ok " + hex(g_rdram + addr, n));
            });
            // peekf <addr> [count=4] -> ok f0 f1 ...
            ps2control::addCommand("peekf", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0, n = 4;
                if (w.empty() || !parseU32(w[0], addr) || (w.size() > 1 && !parseU32(w[1], n)) || n == 0 || n > 256)
                    return reply("error usage: peekf <addr> [count<=256]");
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                std::string out = "ok";
                for (uint32_t i = 0; i < n; ++i)
                    out += fmt(" %.6g", loadF32(addr + i * 4));
                reply(out);
            });
            // poke <addr> <hex bytes> -> written at the next frame kick
            ps2control::addCommand("poke", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0;
                if (w.size() != 2 || !parseU32(w[0], addr) || w[1].size() % 2 || w[1].empty())
                    return reply("error usage: poke <addr> <hexbytes>");
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i < w[1].size(); i += 2)
                    bytes.push_back(static_cast<uint8_t>(std::strtoul(w[1].substr(i, 2).c_str(), nullptr, 16)));
                runOnGameThread([addr, bytes, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    for (size_t i = 0; i < bytes.size(); ++i)
                        rdram[(addr + i) & kRamMask] = bytes[i];
                    reply("ok");
                });
            });
            // pokef <addr> <f> [f...]
            ps2control::addCommand("pokef", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0;
                if (w.size() < 2 || !parseU32(w[0], addr))
                    return reply("error usage: pokef <addr> <float> [float...]");
                std::vector<float> values;
                for (size_t i = 1; i < w.size(); ++i)
                    values.push_back(std::strtof(w[i].c_str(), nullptr));
                runOnGameThread([addr, values, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    for (size_t i = 0; i < values.size(); ++i)
                        std::memcpy(rdram + ((addr + i * 4) & kRamMask), &values[i], 4);
                    reply("ok");
                });
            });
            // findvec <x> <y> <z> <tolerance> [lo hi] -> ok addr addr ... (three consecutive floats near the point)
            ps2control::addCommand("findvec", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                if (w.size() < 4 || !g_rdram)
                    return reply("error usage: findvec <x> <y> <z> <tolerance> [lo hi]");
                const float x = std::strtof(w[0].c_str(), nullptr), y = std::strtof(w[1].c_str(), nullptr),
                            z = std::strtof(w[2].c_str(), nullptr), tol = std::strtof(w[3].c_str(), nullptr);
                uint32_t lo, hi;
                range(w, 4, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 12 <= hi && found < 400; a += 4)
                {
                    const float fx = loadF32(a);
                    if (!(std::fabs(fx - x) <= tol))
                        continue;
                    if (std::fabs(loadF32(a + 4) - y) <= tol && std::fabs(loadF32(a + 8) - z) <= tol)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // findu32 <value> [lo hi] -> ok addr ...
            ps2control::addCommand("findu32", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t value = 0;
                if (w.empty() || !parseU32(w[0], value) || !g_rdram)
                    return reply("error usage: findu32 <value> [lo hi]");
                uint32_t lo, hi;
                range(w, 1, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 4 <= hi && found < 400; a += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, g_rdram + a, 4);
                    if (v == value)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // findrange <vlo> <vhi> [lo hi] -> ok addr ... (u32 words with vlo <= value < vhi: pointers into a block)
            ps2control::addCommand("findrange", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t vlo = 0, vhi = 0;
                if (w.size() < 2 || !parseU32(w[0], vlo) || !parseU32(w[1], vhi) || !g_rdram)
                    return reply("error usage: findrange <vlo> <vhi> [lo hi]");
                uint32_t lo, hi;
                range(w, 2, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 4 <= hi && found < 2000; a += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, g_rdram + a, 4);
                    if (v >= vlo && v < vhi)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // dump <path> [addr len] -> RAM (all 32 MB by default) to a file, taken on the game thread between frames.
            // Game memory: keep dumps under build/analysis/ (never committed).
            ps2control::addCommand("dump", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0, len = kRamSize;
                if (w.empty() || (w.size() >= 3 && (!parseU32(w[1], addr) || !parseU32(w[2], len))))
                    return reply("error usage: dump <path> [addr len]");
                addr &= kRamMask;
                len = std::min(len, kRamSize - addr);
                const std::string path = w[0];
                runOnGameThread([path, addr, len, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    FILE *f = std::fopen(path.c_str(), "wb");
                    if (!f)
                        return reply("error cannot write " + path);
                    const size_t n = std::fwrite(rdram + addr, 1, len, f);
                    std::fclose(f);
                    reply(n == len ? fmt("ok %u", len) : std::string("error short write"));
                });
            });
            // pause on|off: hold the game thread at the frame kick (the window keeps showing the last frame)
            ps2control::addCommand("pause", [](const std::string &args, ps2control::Reply reply) {
                if (args != "on" && args != "off")
                    return reply("error usage: pause on|off");
                g_paused.store(args == "on");
                reply("ok");
            });
            // frames -> ok <game frames since start>
            ps2control::addCommand("frames", [](const std::string &, ps2control::Reply reply) {
                reply(fmt("ok %llu", static_cast<unsigned long long>(g_frames.load())));
            });
        }
    }

    namespace
    {
        // ---- The game's script VM (docs/game-internals.md section 11) -------------------------------------------
        constexpr uint32_t kScriptSpacePtr = 0x0048741Cu; // -> ScriptSpace (MAIN.SCM from file offset 8)
        constexpr uint32_t kActiveScripts = 0x004CD3C4u;  // CRunningScript list heads (+0 next, +4 prev)
        constexpr uint32_t kIdleScripts = 0x004CD3C8u;
        constexpr uint32_t kNextScriptId = 0x0048742Cu;
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8, set by LOAD_AND_LAUNCH_MISSION_INTERNAL
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kScriptSize = 0x218u;
        // A rig script's code lives in its own locals 0x30.. (+0x114..+0x1c4): locals 0..0x2f stay free for outputs,
        // the timers (locals 0x68 / 0x69 at +0x1f4) are not touched.
        constexpr uint32_t kCodeOffset = 0x114u;
        constexpr uint32_t kCodeMax = 0x1c4u - kCodeOffset;

        uint32_t rd32(uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        // CTheScripts::StartNewScript (FUN_00255950) done from the host: take an idle script, Init it (FUN_002551d0),
        // set its IP and put it at the head of the active list. Game thread, between frames only.
        uint32_t startScript(uint8_t *ram, uint32_t ip, const char *name)
        {
            const uint32_t s = rd32(ram, kIdleScripts);
            if (!s)
                return 0;
            const uint32_t next = rd32(ram, s), prev = rd32(ram, s + 4); // unlink from the idle list
            if (prev)
                wr32(ram, prev, next);
            else
                wr32(ram, kIdleScripts, next);
            if (next)
                wr32(ram, next + 4, prev);
            const uint32_t id = rd32(ram, kNextScriptId);
            wr32(ram, kNextScriptId, id + 1);
            std::memset(ram + (s & kRamMask), 0, kScriptSize); // Init: every field it sets is 0 except +0x20d
            wr32(ram, s + 8, id);
            ram[(s + 0x20d) & kRamMask] = 1;
            std::strncpy(reinterpret_cast<char *>(ram + ((s + 0x20f) & kRamMask)), name, 7);
            wr32(ram, s + 0x10, ip);
            const uint32_t head = rd32(ram, kActiveScripts); // add at the head of the active list
            wr32(ram, s, head);
            wr32(ram, s + 4, 0);
            if (head)
                wr32(ram, head + 4, s);
            wr32(ram, kActiveScripts, s);
            ram[(s + 0x208) & kRamMask] = 1;
            return s;
        }

        struct PendingScript
        {
            uint32_t script = 0;
            uint64_t startFrame = 0;
            ps2control::Reply reply;
        };
        std::vector<PendingScript> g_pending; // game thread

        void checkPendingScripts(uint8_t *ram)
        {
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                // TERMINATE_THIS_SCRIPT clears +0x208 and moves the script to the idle list
                if (ram[(it->script + 0x208) & kRamMask] == 0)
                {
                    it->reply("ok");
                    it = g_pending.erase(it);
                }
                else if (g_frames.load() - it->startFrame > 1800) // 60 s at 30 fps: still running
                {
                    it->reply(fmt("error script %08x still running after 1800 frames", it->script));
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            }
        }

        void registerScriptCommands()
        {
            // scm <hex bytecode> -> runs it as a new script; answers when the script terminated (end it with 23 00)
            ps2control::addCommand("scm", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                std::string code;
                for (const auto &part : w)
                    code += part;
                if (code.empty() || code.size() % 2 || code.size() / 2 > kCodeMax)
                    return reply(fmt("error usage: scm <hex bytecode, at most %u bytes>", kCodeMax));
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i < code.size(); i += 2)
                    bytes.push_back(static_cast<uint8_t>(std::strtoul(code.substr(i, 2).c_str(), nullptr, 16)));
                runOnGameThread([bytes, reply](uint8_t *ram, R5900Context *, PS2Runtime *) {
                    const uint32_t space = rd32(ram, kScriptSpacePtr);
                    if (!space)
                        return reply("error the script engine is not running yet");
                    // the code goes into the new script's locals, so take the slot first and write after Init
                    const uint32_t idle = rd32(ram, kIdleScripts);
                    if (!idle)
                        return reply("error no free script slot");
                    const uint32_t s = startScript(ram, idle + kCodeOffset - space, "rig");
                    std::memcpy(ram + ((s + kCodeOffset) & kRamMask), bytes.data(), bytes.size());
                    g_pending.push_back({s, g_frames.load(), reply});
                });
            });
            // scripts -> ok <id>:<name>:<ip>:<mission> ...
            ps2control::addCommand("scripts", [](const std::string &, ps2control::Reply reply) {
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                std::string out = "ok";
                uint32_t s = rd32(g_rdram, kActiveScripts);
                for (int n = 0; s && n < 256; ++n, s = rd32(g_rdram, s))
                {
                    char name[9] = {};
                    std::memcpy(name, g_rdram + ((s + 0x20f) & kRamMask), 8);
                    out += fmt(" %u:%s:%x:%u", rd32(g_rdram, s + 8), name, rd32(g_rdram, s + 0x10),
                               g_rdram[(s + 0x20a) & kRamMask]);
                }
                reply(out);
            });
            // player -> ok <ped> <x> <y> <z> <mission running 0|1>
            ps2control::addCommand("player", [](const std::string &, ps2control::Reply reply) {
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                const uint32_t ped = rd32(g_rdram, kPlayerPed);
                if (!ped)
                    return reply("error no player");
                reply(fmt("ok %08x %.3f %.3f %.3f %u", ped, loadF32(ped + 0x30), loadF32(ped + 0x34), loadF32(ped + 0x38),
                          g_rdram[kMissionRunning]));
            });
        }
    }

    void runOnGameThread(std::function<void(uint8_t *, R5900Context *, PS2Runtime *)> job)
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_jobs.push_back(std::move(job));
    }

    uint64_t gameFrames() { return g_frames.load(std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        registerMemoryCommands();
        registerScriptCommands();
    }

    void onGameFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_rdram = rdram;
        g_frames.fetch_add(1, std::memory_order_relaxed);
        checkPendingScripts(rdram);
        for (;;)
        {
            std::deque<std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>> jobs;
            {
                std::lock_guard<std::mutex> lock(g_jobsMutex);
                jobs.swap(g_jobs);
            }
            for (auto &job : jobs)
                job(rdram, ctx, runtime);
            if (!g_paused.load(std::memory_order_relaxed) || runtime->isStopRequested())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // paused: keep serving jobs
        }
    }
}
