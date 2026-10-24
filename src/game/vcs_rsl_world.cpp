// World draws from the RSL world draw calls - see game/vcs_rsl_world.h.
#include "game/vcs_rsl_world.h"

#include "game/vcs_hle_world.h"
#include "ps2_runtime.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/ps2_hle_survey.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vcs::rslworld
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPktLo = 0x520F90u, kPktHi = 0x6EDC70u; // the packet buffer (game-internals.md 6i)
        constexpr uint32_t kWorldDraw = 0x002F7EB0u;               // FUN_002F7EB0(geometry, qwc)

        bool g_enabled = false, g_verify = false, g_log = false;
        PS2Runtime::RecompiledFunction g_original = nullptr;

        struct Draw
        {
            uint32_t addr, qwc;
        };
        std::vector<Draw> g_current, g_kicked; // this frame's world draw calls / the frame whose chain is being copied
        size_t g_next = 0;                     // the next of g_kicked whose REF the chain should hold

        // the copied chain's replaced REFs, by MARK index, for the thread that processes it
        struct ChainTable
        {
            std::vector<Draw> draws;
        };
        ChainTable g_building;
        std::mutex g_tablesMutex;
        std::map<const void *, std::shared_ptr<ChainTable>> g_tables;
        thread_local std::shared_ptr<ChainTable> t_table;

        // ---- recordings (the processing thread) ----------------------------------------------------------------------
        struct Op
        {
            bool mscal = false;
            uint32_t startPc = 0;            // MSCAL
            vcs::hleworld::WorldMesh mesh;   // MSCAL: its world packet's mesh
            bool topRelative = false;        // UNPACK
            uint32_t vuAddr = 0, qwords = 0; // UNPACK: VU address (relative to TOPS when topRelative)
            std::vector<uint8_t> data;       // UNPACK: the qwords as written
        };

        struct Recording
        {
            uint64_t hash = 0;
            uint32_t mode = 0, cycle = 0; // VIF state the UNPACK results depend on
            bool replayable = false;
            std::vector<Op> ops;
            uint32_t mask = 0, row[4] = {}; // the VIF mask / row the block leaves
        };

        class Recorder final : public ps2chain::VifRecorder
        {
        public:
            Recording *rec = nullptr;
            bool failed = false, maskSet = false;

            void unpack(uint32_t vuAddr, uint32_t qwords, bool topRelative, uint32_t tops, uint32_t cycle, bool masked, uint32_t mode,
                        const uint8_t *vuData) override
            {
                if ((cycle & 0xFFFFu) != 0x0101u && (cycle & 0xFFFFu) != 0u)
                    failed = true; // write cycles other than CL = WL = 1: not reproduced
                if (masked && !maskSet)
                    failed = true; // the mask / row would come from before the block
                if ((mode & 3u) != 0u)
                    failed = true; // MODE adds / accumulates ROW
                Op &op = rec->ops.emplace_back();
                op.topRelative = topRelative;
                op.vuAddr = topRelative ? (vuAddr - tops) & 0x3FFu : vuAddr & 0x3FFu;
                op.qwords = qwords;
                op.data.resize(size_t(qwords) * 16u);
                for (uint32_t i = 0; i < qwords; ++i)
                    std::memcpy(op.data.data() + i * 16u, vuData + ((vuAddr + i) & 0x3FFu) * 16u, 16);
            }

            void mscal(uint32_t startPc) override
            {
                if (startPc != 0x30u)
                    failed = true; // only the world program is expected here
                Op &op = rec->ops.emplace_back();
                op.mscal = true;
                op.startPc = startPc;
                vcs::hleworld::setMeshCapture(&op.mesh); // the handler stores the packet's mesh (a drawn packet)
            }

            void other(uint32_t vifcode) override
            {
                const uint32_t opcode = (vifcode >> 24) & 0x7Fu;
                if (opcode == 0x20u || opcode == 0x30u) // STMASK / STROW: the block sets its own
                    maskSet = true;
                else if (opcode != 0x00u && opcode != 0x10u && opcode != 0x11u && opcode != 0x13u) // NOP / FLUSH*
                    failed = true;
            }
        };

        std::unordered_map<uint64_t, Recording> g_recordings; // by (address, qwc)
        struct Stats
        {
            uint64_t blocks = 0, replayed = 0, recorded = 0, notReplayable = 0, packets = 0, verifySame = 0, verifyDiffer = 0;
        } g_stats;
        auto g_reportAt = std::chrono::steady_clock::now();

        uint64_t hashBlock(const uint8_t *p, uint32_t bytes)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
            for (uint32_t i = 0; i + 8u <= bytes; i += 8u)
            {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 29;
            }
            return h;
        }

        // process the block as the packet path would, recording what it does
        void record(PS2Memory &memory, const uint8_t *bytes, uint32_t size, Recording &rec)
        {
            rec.ops.clear();
            rec.mode = memory.vif1_regs.mode;
            rec.cycle = memory.vif1_regs.cycle;
            Recorder recorder;
            recorder.rec = &rec;
            ps2chain::setVifRecorder(&recorder);
            memory.processVIF1Data(bytes, size);
            ps2chain::setVifRecorder(nullptr);
            vcs::hleworld::setMeshCapture(nullptr);
            rec.replayable = !recorder.failed;
            rec.mask = memory.vif1_regs.mask;
            std::memcpy(rec.row, memory.vif1_regs.row, sizeof(rec.row));
        }

        void replay(PS2Memory &memory, const Recording &rec)
        {
            uint8_t *vu = memory.getVU1Data();
            for (const Op &op : rec.ops)
            {
                if (op.mscal)
                {
                    vcs::hleworld::setMeshHint(&op.mesh);
                    memory.vu1Mscal(op.startPc);
                    vcs::hleworld::setMeshHint(nullptr);
                    ++g_stats.packets;
                    continue;
                }
                const uint32_t at = op.topRelative ? (op.vuAddr + memory.vif1_regs.tops) & 0x3FFu : op.vuAddr;
                for (uint32_t i = 0; i < op.qwords; ++i)
                    std::memcpy(vu + ((at + i) & 0x3FFu) * 16u, op.data.data() + i * 16u, 16);
            }
            memory.vif1_regs.mask = rec.mask;
            std::memcpy(memory.vif1_regs.row, rec.row, sizeof(rec.row));
        }

        // the cached recording against a fresh one of the same block; a mesh is compared when both drew the packet (whether
        // a packet is culled / clipped depends on the camera, not on the block)
        bool same(const Recording &a, const Recording &b)
        {
            const char *what = nullptr;
            size_t at = 0;
            if (a.ops.size() != b.ops.size())
                what = "op count";
            else if (a.mask != b.mask || std::memcmp(a.row, b.row, sizeof(a.row)) != 0)
                what = "final mask / row";
            for (size_t i = 0; !what && i < a.ops.size(); ++i)
            {
                const Op &x = a.ops[i], &y = b.ops[i];
                at = i;
                if (x.mscal != y.mscal || x.startPc != y.startPc)
                    what = "op kind";
                else if (x.topRelative != y.topRelative || x.vuAddr != y.vuAddr || x.qwords != y.qwords)
                    what = "unpack address / size";
                else if (x.data != y.data)
                    what = "unpack data";
                else if (x.mesh.valid && y.mesh.valid && (x.mesh.key != y.mesh.key || x.mesh.vertices != y.mesh.vertices))
                    what = "mesh";
            }
            if (what)
            {
                static int logged = 0;
                if (logged++ < 12)
                {
                    std::fprintf(stderr, "[rsl-world] verify: differs in %s (op %zu of %zu / %zu)", what, at, a.ops.size(), b.ops.size());
                    if (std::strcmp(what, "unpack data") == 0)
                    {
                        const Op &x = a.ops[at], &y = b.ops[at];
                        size_t q = 0;
                        while (q * 16u < x.data.size() && std::memcmp(x.data.data() + q * 16u, y.data.data() + q * 16u, 16) == 0)
                            ++q;
                        uint32_t u[4], v[4];
                        std::memcpy(u, x.data.data() + q * 16u, 16);
                        std::memcpy(v, y.data.data() + q * 16u, 16);
                        std::fprintf(stderr, ": unpack at %03x%s, %u qwords, first differing qword %zu: cached %08x %08x %08x %08x now %08x %08x %08x %08x",
                                     x.vuAddr, x.topRelative ? "+TOP" : "", x.qwords, q, u[0], u[1], u[2], u[3], v[0], v[1], v[2], v[3]);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            return !what;
        }

        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            if (!g_log || now - g_reportAt < std::chrono::seconds(10))
                return;
            g_reportAt = now;
            const Stats &s = g_stats;
            std::fprintf(stderr,
                         "[rsl-world] %llu blocks: %llu replayed (%llu packets), %llu recorded, %llu not replayable; %zu recordings%s\n",
                         (unsigned long long)s.blocks, (unsigned long long)s.replayed, (unsigned long long)s.packets,
                         (unsigned long long)s.recorded, (unsigned long long)s.notReplayable, g_recordings.size(),
                         g_verify ? "" : "");
            if (g_verify)
                std::fprintf(stderr, "[rsl-world] verify: %llu recordings the same as the packet path, %llu differ\n",
                             (unsigned long long)s.verifySame, (unsigned long long)s.verifyDiffer);
            g_stats = Stats{};
        }

        // the MARK in place of a world draw's REF (the processing thread, in stream order)
        void onMark(PS2Memory &memory, uint32_t imm)
        {
            const ChainTable *table = t_table.get();
            if (!table || imm >= table->draws.size())
                return; // a MARK the game wrote itself
            const Draw &d = table->draws[imm];
            const uint32_t addr = d.addr & kRamMask, size = d.qwc * 16u;
            if (addr + size > kRamMask + 1u)
                return;
            const uint8_t *bytes = memory.getRDRAM() + addr;
            ++g_stats.blocks;
            const uint64_t hash = hashBlock(bytes, size);
            Recording &rec = g_recordings[(uint64_t(addr) << 16) | d.qwc];
            const bool valid = rec.hash == hash && !rec.ops.empty() && rec.mode == memory.vif1_regs.mode &&
                               rec.cycle == memory.vif1_regs.cycle;
            if (valid && rec.replayable && !g_verify)
            {
                replay(memory, rec);
                ++g_stats.replayed;
            }
            else if (valid && rec.replayable && g_verify)
            {
                Recording fresh;
                fresh.hash = hash;
                record(memory, bytes, size, fresh);
                (same(rec, fresh) ? g_stats.verifySame : g_stats.verifyDiffer) += 1;
            }
            else if (valid)
            {
                memory.processVIF1Data(bytes, size); // recorded before: not replayable
                ++g_stats.notReplayable;
            }
            else
            {
                rec.hash = hash;
                record(memory, bytes, size, rec);
                ++g_stats.recorded;
            }
            report();
        }

        // ---- the game thread ----------------------------------------------------------------------------------------
        void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
            g_current.push_back({getRegU32(ctx, 4) & kRamMask, getRegU32(ctx, 5) & 0xFFFFu});
            g_original(rdram, ctx, runtime);
        }

        uint32_t observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                         uint32_t flatOffset, bool chainStart)
        {
            (void)tagTarget;
            (void)vifCodes;
            (void)flatOffset;
            if (chainStart)
                g_building = ChainTable{};
            tagAddr &= kRamMask;
            if (id != 3u || !qwc || tagAddr < kPktLo || tagAddr >= kPktHi || g_next >= g_kicked.size())
                return 0u;
            const Draw &want = g_kicked[g_next];
            if ((dataAddr & kRamMask) != want.addr || qwc != want.qwc || g_building.draws.size() >= 0xFFFFu)
                return 0u;
            ++g_next;
            g_building.draws.push_back(want);
            return ps2chain::kReplaceWithMark | static_cast<uint32_t>(g_building.draws.size() - 1u);
        }

        void queued(const void *key, uint32_t sizeBytes)
        {
            (void)sizeBytes;
            if (g_building.draws.empty())
                return;
            auto table = std::make_shared<ChainTable>(std::move(g_building));
            g_building = ChainTable{};
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            g_tables[key] = std::move(table);
            if (g_tables.size() > 8u)
                g_tables.erase(g_tables.begin());
        }

        void processing(const void *key, bool begin)
        {
            if (!begin)
            {
                t_table.reset();
                return;
            }
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            const auto it = g_tables.find(key);
            if (it == g_tables.end())
                return;
            t_table = std::move(it->second);
            g_tables.erase(it);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_RSL_WORLD");
        const char *hle = std::getenv("PS2X_HLE");
        // needs the native world handler (not the VU1 path, not its checker, not the survey)
        g_enabled = !(e && *e == '0') && !(hle && *hle == '0') && !std::getenv("PS2X_HLE_VERIFY") && !ps2hle::surveyEnabled();
        if (!g_enabled)
            return;
        g_verify = std::getenv("VCS_RSL_WORLD_VERIFY") != nullptr;
        g_log = g_verify || std::getenv("VCS_RSL_WORLD_LOG") != nullptr;
        g_original = runtime.lookupFunction(kWorldDraw);
        if (!g_original)
        {
            g_enabled = false;
            return;
        }
        runtime.replaceFunction(kWorldDraw, &hook);
        ps2chain::addTagObserver(&observe);
        ps2chain::addChainObservers(&queued, &processing);
        ps2chain::setMarkHandler(&onMark);
    }

    void onFrameKick()
    {
        if (!g_enabled)
            return;
        g_kicked.swap(g_current);
        g_current.clear();
        g_next = 0;
    }
}
