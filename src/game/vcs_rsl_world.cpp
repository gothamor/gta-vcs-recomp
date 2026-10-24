// World and model draws from the RSL draw calls - see game/vcs_rsl_world.h.
#include "game/vcs_rsl_world.h"

#include "game/vcs_hle_world.h"
#include "ps2_runtime.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/gs/gs_hw.h"
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
        constexpr uint32_t kWorldDraw = 0x002F7EB0u;               // FUN_002F7EB0(geometry, qwc): REF geometry
        constexpr uint32_t kModelDraw = 0x002F7D58u;               // FUN_002F7D58(packet): CALL packet
        constexpr uint32_t kModelDrawB = 0x002F80A0u;              // FUN_002F80A0(packet): CALL packet

        bool g_world = false, g_models = false, g_rasters = false, g_verify = false, g_log = false;
        PS2Runtime::RecompiledFunction g_worldOriginal = nullptr, g_modelOriginal = nullptr, g_modelBOriginal = nullptr;

        enum Kind : uint8_t
        {
            World,     // a world draw's REF
            Model,     // FUN_002F7D58's CALL
            ModelB,    // FUN_002F80A0's CALL
            OtherCall, // a CALL in the packet buffer no draw call above wrote (FUN_002F80A0's state packet, ...)
            RasterUpload, // a REF sending a raster's pixels (VIF DIRECT): its address names the texture (step 3d)
            KindCount
        };
        const char *const kKindNames[KindCount] = {"world", "model", "model-b", "other call", "raster upload"};

        struct Draw
        {
            uint32_t id, addr, qwc; // the tag the draw call writes: REF addr qwc / CALL addr (qwc unused)
            Kind kind;
        };
        std::vector<Draw> g_current, g_kicked; // this frame's draw calls / the frame whose chain is being copied
        size_t g_next = 0;                     // the next of g_kicked whose tag the chain should hold

        // the copied chain's marked tags, by MARK index, for the thread that processes it
        struct RasterRef // a texture upload's pixels: where they are in the copied chain, whose they are
        {
            uint32_t offset, address, bytes;
        };
        struct ChainTable
        {
            std::vector<Draw> draws;
            std::vector<RasterRef> rasters; // by offset
        };
        ChainTable g_building;
        std::mutex g_tablesMutex;
        std::map<const void *, std::shared_ptr<ChainTable>> g_tables;
        thread_local std::shared_ptr<ChainTable> t_table;
        thread_local const uint8_t *t_chain = nullptr; // the chain being processed (its copy's data)

        // the raster upload whose pixels are at `data` in the chain being processed (step 3d), if one is
        const RasterRef *rasterAt(const uint8_t *data, uint32_t bytes)
        {
            const ChainTable *table = t_table.get();
            if (!table || table->rasters.empty() || !t_chain || data < t_chain)
                return nullptr;
            const uint32_t offset = static_cast<uint32_t>(data - t_chain);
            const auto it = std::lower_bound(table->rasters.begin(), table->rasters.end(), offset,
                                             [](const RasterRef &r, uint32_t o) { return r.offset < o; });
            return it != table->rasters.end() && it->offset == offset && it->bytes == bytes ? &*it : nullptr;
        }

        // ---- recordings (the processing thread) ----------------------------------------------------------------------
        struct Op
        {
            enum Type : uint8_t
            {
                Unpack,
                Mscal,
                Direct
            } type = Unpack;
            uint32_t startPc = 0;            // MSCAL
            vcs::hleworld::WorldMesh mesh;   // MSCAL: its world packet's mesh
            bool topRelative = false;        // UNPACK
            uint8_t lanes = 0xF;             // UNPACK: the fields it writes (the others keep VU memory)
            bool directHl = false;           // DIRECT
            uint32_t rasterAddress = 0, rasterBytes = 0; // DIRECT: the raster upload it sends (step 3d), if known
            uint32_t vuAddr = 0, qwords = 0; // UNPACK: VU address (relative to TOPS when topRelative); DIRECT: qwords
            std::vector<uint8_t> data;       // UNPACK: the qwords as written; DIRECT: the GS data
        };

        struct Recording
        {
            uint64_t hash = 0;
            uint32_t mode = 0, cycle = 0; // VIF state the UNPACK results depend on
            // a block the game rebuilds every frame (changed bytes three times running) or one not replayable: processed
            // by the packet path for the next `skip` hits, without hashing or recording
            uint32_t changes = 0, skip = 0;
            bool probe = false; // the skip ran out: record once more to see whether it settled
            bool replayable = false;
            const char *refused = nullptr; // why not
            std::vector<Op> ops;
            bool setsMask = false, setsRow = false, setsCycle = false; // the block's STMASK / STROW / STCYCL
            uint32_t mask = 0, row[4] = {}, finalCycle = 0;             // the VIF mask / row / cycle it leaves
        };

        class Recorder final : public ps2chain::VifRecorder
        {
        public:
            Recording *rec = nullptr;
            const char *failed = nullptr;
            bool maskSet = false, rowSet = false, cycleSet = false;

            void fail(const char *why)
            {
                if (!failed)
                    failed = why;
            }

            void unpack(uint32_t vuAddr, uint32_t qwords, bool topRelative, uint32_t tops, uint32_t cycle, bool masked, uint32_t mode,
                        uint32_t lanes, bool lanesFromCol, const uint8_t *vuData) override
            {
                if ((cycle & 0xFFFFu) != 0x0101u && (cycle & 0xFFFFu) != 0u)
                    fail("write cycle"); // CL / WL other than 1 / 1: the writes skip, not reproduced
                if (masked && (!maskSet || !rowSet))
                    fail("mask from before"); // the mask / row would come from before the block
                if (lanesFromCol)
                    fail("COL"); // the block never sets COL
                if ((mode & 3u) != 0u)
                    fail("mode"); // MODE adds / accumulates ROW
                Op &op = rec->ops.emplace_back();
                op.topRelative = topRelative;
                op.lanes = static_cast<uint8_t>(lanes & 0xFu);
                op.vuAddr = topRelative ? (vuAddr - tops) & 0x3FFu : vuAddr & 0x3FFu;
                op.qwords = qwords;
                op.data.resize(size_t(qwords) * 16u);
                for (uint32_t i = 0; i < qwords; ++i)
                    std::memcpy(op.data.data() + i * 16u, vuData + ((vuAddr + i) & 0x3FFu) * 16u, 16);
            }

            void mscal(uint32_t startPc) override
            {
                Op &op = rec->ops.emplace_back();
                op.type = Op::Mscal;
                op.startPc = startPc;
                // the native world handler stores a drawn world packet's mesh (the program at 0x30)
                vcs::hleworld::setMeshCapture(startPc == 0x30u ? &op.mesh : nullptr);
            }

            void direct(const uint8_t *data, uint32_t qwords, bool directHl, bool complete) override
            {
                if (!complete)
                    fail("DIRECT cut off");
                Op &op = rec->ops.emplace_back();
                op.type = Op::Direct;
                op.directHl = directHl;
                op.qwords = qwords;
                op.data.assign(data, data + size_t(qwords) * 16u);
                if (const RasterRef *r = rasterAt(data, qwords * 16u))
                {
                    op.rasterAddress = r->address;
                    op.rasterBytes = r->bytes;
                }
            }

            void other(uint32_t vifcode) override
            {
                const uint32_t opcode = (vifcode >> 24) & 0x7Fu;
                if (opcode == 0x20u) // STMASK / STROW / STCYCL: the block sets its own (restored after a replay)
                    maskSet = true;
                else if (opcode == 0x30u)
                    rowSet = true;
                else if (opcode == 0x01u)
                    cycleSet = true; // (the cycle each UNPACK saw is checked there)
                else if (opcode != 0x00u && opcode != 0x10u && opcode != 0x11u && opcode != 0x13u && opcode != 0x50u && opcode != 0x51u)
                    fail(vifcodeName(opcode)); // not NOP / FLUSH* / DIRECT*: OFFSET / BASE / ITOP / STMOD / MSKPATH3 / MARK / ...
            }

            static const char *vifcodeName(uint32_t opcode)
            {
                static char names[128][16];
                std::snprintf(names[opcode & 0x7Fu], sizeof(names[0]), "VIFcode %02x", opcode & 0x7Fu);
                return names[opcode & 0x7Fu];
            }
        };

        std::unordered_map<uint64_t, Recording> g_recordings; // by (kind, address, length)
        struct Stats
        {
            uint64_t blocks = 0, replayed = 0, recorded = 0, notReplayable = 0, changing = 0;
        } g_stats[KindCount];
        uint64_t g_packets = 0, g_verifySame = 0, g_verifyDiffer = 0, g_unmarked = 0, g_rasterMarks = 0;
        std::map<const char *, uint64_t> g_refusals;
        auto g_reportAt = std::chrono::steady_clock::now();

        uint64_t hashBlock(const uint8_t *p, uint32_t bytes)
        {
            // four lanes: the model packets are most of the chain's bytes
            uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ bytes, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
            uint32_t i = 0;
            for (; i + 32u <= bytes; i += 32u)
                for (int l = 0; l < 4; ++l)
                {
                    uint64_t w;
                    std::memcpy(&w, p + i + l * 8u, 8);
                    h[l] = (h[l] ^ w) * 0xFF51AFD7ED558CCDull;
                    h[l] ^= h[l] >> 29;
                }
            for (; i + 8u <= bytes; i += 8u)
            {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h[0] = (h[0] ^ w) * 0xFF51AFD7ED558CCDull;
                h[0] ^= h[0] >> 29;
            }
            return ((h[0] * 31u + h[1]) * 31u + h[2]) * 31u + h[3];
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
            rec.refused = recorder.failed;
            rec.setsMask = recorder.maskSet;
            rec.setsRow = recorder.rowSet;
            rec.setsCycle = recorder.cycleSet;
            rec.mask = memory.vif1_regs.mask;
            std::memcpy(rec.row, memory.vif1_regs.row, sizeof(rec.row));
            rec.finalCycle = memory.vif1_regs.cycle;
        }

        void replay(PS2Memory &memory, const Recording &rec)
        {
            uint8_t *vu = memory.getVU1Data();
            for (const Op &op : rec.ops)
            {
                if (op.type == Op::Mscal)
                {
                    vcs::hleworld::setMeshHint(op.mesh.valid ? &op.mesh : nullptr);
                    memory.vu1Mscal(op.startPc);
                    vcs::hleworld::setMeshHint(nullptr);
                    ++g_packets;
                }
                else if (op.type == Op::Direct)
                {
                    if (op.rasterBytes) // the texture keeps its name when its upload comes from a recording
                        gsHwSetUploadSource(op.rasterAddress, op.rasterBytes);
                    memory.submitVif1DirectData(op.data.data(), op.qwords, op.directHl);
                }
                else
                {
                    const uint32_t at = op.topRelative ? (op.vuAddr + memory.vif1_regs.tops) & 0x3FFu : op.vuAddr;
                    if (op.lanes == 0xFu)
                        for (uint32_t i = 0; i < op.qwords; ++i)
                            std::memcpy(vu + ((at + i) & 0x3FFu) * 16u, op.data.data() + i * 16u, 16);
                    else
                        for (uint32_t i = 0; i < op.qwords; ++i)
                            for (uint32_t f = 0; f < 4u; ++f)
                                if ((op.lanes >> f) & 1u)
                                    std::memcpy(vu + ((at + i) & 0x3FFu) * 16u + f * 4u, op.data.data() + i * 16u + f * 4u, 4);
                }
            }
            if (rec.setsMask)
                memory.vif1_regs.mask = rec.mask;
            if (rec.setsRow)
                std::memcpy(memory.vif1_regs.row, rec.row, sizeof(rec.row));
            if (rec.setsCycle)
                memory.vif1_regs.cycle = rec.finalCycle;
        }

        bool sameLanes(const Op &x, const Op &y)
        {
            if (x.lanes == 0xFu)
                return x.data == y.data;
            for (uint32_t i = 0; i < x.qwords; ++i)
                for (uint32_t f = 0; f < 4u; ++f)
                    if (((x.lanes >> f) & 1u) && std::memcmp(x.data.data() + i * 16u + f * 4u, y.data.data() + i * 16u + f * 4u, 4) != 0)
                        return false;
            return true;
        }

        // the cached recording against a fresh one of the same block; a mesh is compared when both drew the packet (whether
        // a packet is culled / clipped depends on the camera, not on the block)
        bool same(const Recording &a, const Recording &b, Kind kind)
        {
            const char *what = nullptr;
            size_t at = 0;
            if (a.ops.size() != b.ops.size())
                what = "op count";
            else if (a.setsMask != b.setsMask || a.setsRow != b.setsRow || a.setsCycle != b.setsCycle ||
                     (a.setsMask && a.mask != b.mask) || (a.setsRow && std::memcmp(a.row, b.row, sizeof(a.row)) != 0) ||
                     (a.setsCycle && a.finalCycle != b.finalCycle))
                what = "final mask / row / cycle";
            for (size_t i = 0; !what && i < a.ops.size(); ++i)
            {
                const Op &x = a.ops[i], &y = b.ops[i];
                at = i;
                if (x.type != y.type || x.startPc != y.startPc || x.directHl != y.directHl)
                    what = "op kind";
                else if (x.topRelative != y.topRelative || x.vuAddr != y.vuAddr || x.qwords != y.qwords || x.lanes != y.lanes)
                    what = "unpack address / size / fields";
                else if (x.type == Op::Direct ? x.data != y.data : !sameLanes(x, y))
                    what = x.type == Op::Direct ? "direct data" : "unpack data";
                else if (x.mesh.valid && y.mesh.valid && (x.mesh.key != y.mesh.key || x.mesh.vertices != y.mesh.vertices))
                    what = "mesh";
            }
            if (what)
            {
                static int logged = 0;
                if (logged++ < 12)
                {
                    std::fprintf(stderr, "[rsl-world] verify: %s block differs in %s (op %zu of %zu / %zu)", kKindNames[kind], what, at,
                                 a.ops.size(), b.ops.size());
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
            for (int k = 0; k < KindCount; ++k)
            {
                const Stats &s = g_stats[k];
                if (s.blocks)
                    std::fprintf(stderr, "[rsl-world] %s: %llu blocks: %llu replayed, %llu recorded, %llu not replayable, %llu changing\n",
                                 kKindNames[k], (unsigned long long)s.blocks, (unsigned long long)s.replayed, (unsigned long long)s.recorded,
                                 (unsigned long long)s.notReplayable, (unsigned long long)s.changing);
                g_stats[k] = Stats{};
            }
            std::fprintf(stderr, "[rsl-world] %llu packets replayed, %llu marks without a length, %llu raster uploads named; %zu recordings\n",
                         (unsigned long long)g_packets, (unsigned long long)g_unmarked, (unsigned long long)g_rasterMarks, g_recordings.size());
            if (!g_refusals.empty())
            {
                std::fprintf(stderr, "[rsl-world] not replayable:");
                for (const auto &[why, n] : g_refusals)
                    std::fprintf(stderr, " %s %llu", why, (unsigned long long)n);
                std::fprintf(stderr, "\n");
            }
            if (g_verify)
                std::fprintf(stderr, "[rsl-world] verify: %llu recordings the same as the packet path, %llu differ\n",
                             (unsigned long long)g_verifySame, (unsigned long long)g_verifyDiffer);
            g_packets = g_verifySame = g_verifyDiffer = g_unmarked = g_rasterMarks = 0;
            g_refusals.clear();
        }

        // the MARK before a marked REF's payload / CALL's packet (the processing thread, in stream order); the marked
        // bytes follow the MARK qword: their length (8-byte units) in the word after the MARK, then two NOPs
        uint32_t onMark(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail)
        {
            const ChainTable *table = t_table.get();
            if (!table || imm >= table->draws.size() || avail < 12u)
                return 0u; // a MARK the game wrote itself
            const Draw &d = table->draws[imm];
            uint32_t units;
            std::memcpy(&units, next, 4);
            units &= 0x00FFFFFFu;
            const uint32_t size = units * 8u;
            if (!size || size > avail - 12u)
            {
                ++g_unmarked; // a CALL whose RET the chain did not reach: the packet path runs on
                return 0u;
            }
            const uint8_t *bytes = next + 12u;
            Stats &stats = g_stats[d.kind];
            ++stats.blocks;
            Recording &rec = g_recordings[(uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units];
            if (rec.skip && !g_verify)
            {
                --rec.skip;
                memory.processVIF1Data(bytes, size);
                ++(rec.replayable ? stats.changing : stats.notReplayable);
                return 12u + size;
            }
            const uint64_t hash = hashBlock(bytes, size);
            const bool valid = rec.hash == hash && !rec.ops.empty() && rec.mode == memory.vif1_regs.mode &&
                               rec.cycle == memory.vif1_regs.cycle;
            if (valid && rec.replayable && !g_verify)
            {
                replay(memory, rec);
                rec.changes = 0;
                ++stats.replayed;
            }
            else if (valid && rec.replayable && g_verify)
            {
                Recording fresh;
                fresh.hash = hash;
                record(memory, bytes, size, fresh);
                (same(rec, fresh, d.kind) ? g_verifySame : g_verifyDiffer) += 1;
            }
            else if (valid)
            {
                memory.processVIF1Data(bytes, size); // recorded before: not replayable
                rec.skip = 255;
                ++stats.notReplayable;
                ++g_refusals[rec.refused];
            }
            else if (rec.changes >= 3 && !rec.probe && !g_verify)
            {
                memory.processVIF1Data(bytes, size); // rebuilt every frame: recording it costs more than it saves
                rec.skip = 63;
                rec.probe = true;
                ++stats.changing;
            }
            else
            {
                if (!rec.ops.empty())
                    ++rec.changes;
                rec.probe = false;
                rec.hash = hash;
                record(memory, bytes, size, rec);
                ++stats.recorded;
            }
            report();
            return 12u + size;
        }

        // a DIRECT's data on the processing thread: a raster upload's pixels tell the renderer whose they are (step 3d)
        void onDirect(const uint8_t *data, uint32_t bytes)
        {
            if (const RasterRef *r = rasterAt(data, bytes))
            {
                gsHwSetUploadSource(r->address, r->bytes);
                ++g_rasterMarks;
            }
        }

        // ---- the game thread ----------------------------------------------------------------------------------------
        // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
        void worldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_current.push_back({3u, getRegU32(ctx, 4) & kRamMask, getRegU32(ctx, 5) & 0xFFFFu, World});
            g_worldOriginal(rdram, ctx, runtime);
        }

        void modelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_current.push_back({5u, getRegU32(ctx, 4) & kRamMask, 0u, Model});
            g_modelOriginal(rdram, ctx, runtime);
        }

        void modelBHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_current.push_back({5u, getRegU32(ctx, 4) & kRamMask, 0u, ModelB});
            g_modelBOriginal(rdram, ctx, runtime);
        }

        uint32_t mark(const Draw &d)
        {
            if (g_building.draws.size() >= 0xFFFFu)
                return 0u;
            g_building.draws.push_back(d);
            return ps2chain::kReplaceWithMark | static_cast<uint32_t>(g_building.draws.size() - 1u);
        }

        uint32_t observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                         uint32_t flatOffset, bool chainStart)
        {
            if (chainStart)
                g_building = ChainTable{};
            tagAddr &= kRamMask;
            // a raster's pixels: a REF whose VIFcodes end in DIRECT / DIRECTHL of its own size (texture uploads,
            // FUN_003298D8: the header REF before it holds the GS registers and the IMAGE tag) - in the packet buffer or
            // in a CALLed packet (the world's cached sector setup). Not marked: the MARK would land inside the DIRECT's
            // data; found by its offset in the copy instead (onDirect, rasterAt).
            if (id == 3u && g_rasters && qwc)
            {
                const uint32_t code = static_cast<uint32_t>(vifCodes >> 32);
                if (((code >> 24) & 0x7Eu) == 0x50u && (code & 0xFFFFu) == qwc)
                {
                    g_building.rasters.push_back({flatOffset + 8u, dataAddr & kRamMask, qwc * 16u});
                    return 0u;
                }
            }
            if ((id != 3u && id != 5u) || tagAddr < kPktLo || tagAddr >= kPktHi)
                return 0u;
            const Draw *want = g_next < g_kicked.size() ? &g_kicked[g_next] : nullptr;
            if (id == 3u)
            {
                // a world draw's REF: the one the next draw call wrote
                if (g_world && qwc && want && want->id == 3u && (dataAddr & kRamMask) == want->addr && qwc == want->qwc)
                {
                    ++g_next;
                    return mark(*want);
                }
                return 0u;
            }
            if (!g_models)
                return 0u;
            // a CALL: a model draw's packet when it is the next draw call's, else another packet in the buffer
            Draw d{5u, tagTarget & kRamMask, 0u, OtherCall};
            if (want && want->id == 5u && want->addr == d.addr)
            {
                d.kind = want->kind;
                ++g_next;
            }
            return mark(d);
        }

        void queued(const void *key, uint32_t sizeBytes)
        {
            (void)sizeBytes;
            if (g_building.draws.empty() && g_building.rasters.empty())
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
                t_chain = nullptr;
                return;
            }
            t_chain = static_cast<const uint8_t *>(key);
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            const auto it = g_tables.find(key);
            if (it == g_tables.end())
                return;
            t_table = std::move(it->second);
            g_tables.erase(it);
        }

        bool off(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e == '0';
        }

        bool hook(PS2Runtime &runtime, uint32_t addr, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction replacement)
        {
            original = runtime.lookupFunction(addr);
            if (!original)
                return false;
            runtime.replaceFunction(addr, replacement);
            return true;
        }
    }

    void install(PS2Runtime &runtime)
    {
        // needs the native world handler (not the VU1 path, not its checker, not the survey)
        if (off("PS2X_HLE") || std::getenv("PS2X_HLE_VERIFY") || ps2hle::surveyEnabled())
            return;
        g_world = !off("VCS_RSL_WORLD") && hook(runtime, kWorldDraw, g_worldOriginal, &worldHook);
        g_models = !off("VCS_RSL_MODELS") && hook(runtime, kModelDraw, g_modelOriginal, &modelHook) &&
                   hook(runtime, kModelDrawB, g_modelBOriginal, &modelBHook);
        g_rasters = !off("VCS_RSL_RASTERS"); // textures named by raster (docs/FULLY_NATIVE.md step 3d)
        if (!g_world && !g_models && !g_rasters)
            return;
        g_verify = std::getenv("VCS_RSL_WORLD_VERIFY") != nullptr;
        g_log = g_verify || std::getenv("VCS_RSL_WORLD_LOG") != nullptr;
        ps2chain::addTagObserver(&observe);
        ps2chain::addChainObservers(&queued, &processing);
        ps2chain::setMarkHandler(&onMark);
        if (g_rasters)
            ps2chain::setDirectHandler(&onDirect);
    }

    void onFrameKick()
    {
        if (!g_world && !g_models && !g_rasters)
            return;
        g_kicked.swap(g_current);
        g_current.clear();
        g_next = 0;
    }
}
