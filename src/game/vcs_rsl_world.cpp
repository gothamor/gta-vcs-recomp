// World and model draws from the RSL draw calls - see game/vcs_rsl_world.h.
#include "game/vcs_rsl_world.h"

#include "game/vcs_hle_world.h"
#include "ps2_runtime.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_hle_survey.h"
#include "runtime/ps2_write_watch.h"

#include <algorithm>
#include <immintrin.h>
#include <atomic>
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
            uint32_t units = 0; // step 4: a block copied without its bytes - its length (8-byte units) as last copied
            bool wouldSkip = false; // VCS_RSL_SKIP_VERIFY: copied all the same, checked against the recording
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
            uint64_t seq = 0;               // step 4: which kick (g_chainSeq)
        };
        uint64_t g_chainSeq = 0; // game thread

        // step 4: the worker's word that a block's recording matches a copy (recording key, the chain it was in) - handed
        // over at the end of each chain, read by the game thread at the next kick
        std::mutex g_confirmMutex;
        std::vector<std::pair<uint64_t, uint64_t>> g_confirmPending;
        thread_local std::vector<std::pair<uint64_t, uint64_t>> t_confirm;
        std::atomic<uint64_t> g_skipped{0}, g_skipReplayed{0}, g_skipFallback{0};
        std::atomic<uint64_t> g_skipVerifySame{0}, g_skipVerifyDiffer{0};
        std::atomic<uint64_t> g_skipLost{0}, g_skipSeen{0}; // step 4: skip marks the worker saw / could not place
        bool g_skipVerify = false;
        ChainTable g_building;
        std::mutex g_tablesMutex;
        std::map<const void *, std::shared_ptr<ChainTable>> g_tables;
        thread_local std::shared_ptr<ChainTable> t_table;
        thread_local const uint8_t *t_chain = nullptr; // the chain being processed (its copy's data)
        // VCS_RSL_MEASURE=1 (step 4 sizing): per chain, the bytes of marked blocks, and of distinct ones
        struct Measure
        {
            uint64_t chains = 0, chainBytes = 0, blocks = 0, blockBytes = 0, distinct = 0, distinctBytes = 0;
        } g_measure;
        thread_local std::unordered_map<uint64_t, uint32_t> t_seen;
        // step 4: marked blocks copied, and those unchanged since their last copy (game thread; logged in report)
        struct CleanStats
        {
            uint64_t blocks = 0, clean = 0, unseen = 0;
        } g_cleanStats;
        std::atomic<uint32_t> g_lastChainBytes{0};

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
                    // one copy when the run does not wrap around VU memory; masked lanes as one blend per qword
                    if (op.lanes == 0xFu && (at & 0x3FFu) + op.qwords <= 0x400u)
                        std::memcpy(vu + (at & 0x3FFu) * 16u, op.data.data(), op.qwords * 16u);
                    else if (op.lanes == 0xFu)
                        for (uint32_t i = 0; i < op.qwords; ++i)
                            std::memcpy(vu + ((at + i) & 0x3FFu) * 16u, op.data.data() + i * 16u, 16);
                    else
                    {
                        const __m128i keep = _mm_setr_epi32((op.lanes & 1u) ? 0 : -1, (op.lanes & 2u) ? 0 : -1,
                                                            (op.lanes & 4u) ? 0 : -1, (op.lanes & 8u) ? 0 : -1);
                        for (uint32_t i = 0; i < op.qwords; ++i)
                        {
                            auto *dst = reinterpret_cast<__m128i *>(vu + ((at + i) & 0x3FFu) * 16u);
                            const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i *>(op.data.data() + i * 16u));
                            _mm_storeu_si128(dst, _mm_or_si128(_mm_and_si128(keep, _mm_loadu_si128(dst)), _mm_andnot_si128(keep, src)));
                        }
                    }
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
            if (g_cleanStats.blocks) // (game thread counts; read here only for the log)
            {
                std::fprintf(stderr, "[rsl-world] write watch: %llu blocks copied, %llu (%.1f%%) unchanged since their last copy, %llu seen for the first time\n",
                             (unsigned long long)g_cleanStats.blocks, (unsigned long long)g_cleanStats.clean,
                             100.0 * g_cleanStats.clean / double(g_cleanStats.blocks), (unsigned long long)g_cleanStats.unseen);
                g_cleanStats = CleanStats{};
            }
            if (g_skipVerify)
                std::fprintf(stderr, "[rsl-world] skip verify: %llu blocks would have replayed their recording rightly, %llu wrongly\n",
                             (unsigned long long)g_skipVerifySame.exchange(0), (unsigned long long)g_skipVerifyDiffer.exchange(0));
            if (g_skipped.load())
            {
                std::fprintf(stderr, "[rsl-world] skip marks seen %llu, without a table entry %llu\n", (unsigned long long)g_skipSeen.exchange(0),
                             (unsigned long long)g_skipLost.exchange(0));
                std::fprintf(stderr, "[rsl-world] skipped (not copied): %llu blocks, %llu replayed, %llu from guest RAM\n",
                             (unsigned long long)g_skipped.exchange(0), (unsigned long long)g_skipReplayed.exchange(0),
                             (unsigned long long)g_skipFallback.exchange(0));
            }
            if (g_measure.chains)
            {
                const double c = double(g_measure.chains);
                std::fprintf(stderr, "[rsl-world] per chain: %.0f KB copied, %.0f marked blocks (%.0f KB), %.0f distinct (%.0f KB)\n",
                             g_measure.chainBytes / c / 1024.0, g_measure.blocks / c, g_measure.blockBytes / c / 1024.0, g_measure.distinct / c,
                             g_measure.distinctBytes / c / 1024.0);
                g_measure = Measure{};
            }
            g_refusals.clear();
        }

        uint64_t recordingKey(Kind kind, uint32_t addr, uint32_t units)
        {
            return (uint64_t(kind) << 56) | (uint64_t(addr & kRamMask) << 24) | (units & 0xFFFFFFu);
        }

        // a CALLed packet flattened from guest RAM as the chain walker copies it (each tag's upper half, then its payload) up
        // to the RET that returns from it - step 4's fallback for a skipped block whose recording cannot be used
        bool flattenCall(const uint8_t *rdram, uint32_t target, std::vector<uint8_t> &out)
        {
            out.clear();
            uint32_t tag = target & kRamMask, ret[2] = {}, depth = 0;
            for (int n = 0; n < 65536; ++n)
            {
                if ((tag & 0xF0000000u) == 0x70000000u || tag + 16u > kRamMask + 1u)
                    return false;
                uint64_t lo;
                std::memcpy(&lo, rdram + tag, 8);
                out.insert(out.end(), rdram + tag + 8, rdram + tag + 16);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu), id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>(lo >> 32) & kRamMask;
                const uint32_t after = tag + 16u;
                const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : after;
                if (qwc)
                {
                    if ((data & 0xF0000000u) == 0x70000000u || (data & kRamMask) + qwc * 16u > kRamMask + 1u)
                        return false;
                    out.insert(out.end(), rdram + (data & kRamMask), rdram + (data & kRamMask) + qwc * 16u);
                }
                switch (id)
                {
                case 1: tag = after + qwc * 16u; break;                  // cnt
                case 2: tag = addr; break;                               // next
                case 3: case 4: tag = after; break;                      // ref, refs
                case 5:                                                  // call
                    if (depth >= 2u)
                        return false;
                    ret[depth++] = after + qwc * 16u;
                    tag = addr;
                    break;
                case 6:                                                  // ret: from a nested call, or out of the packet
                    if (depth == 0u)
                        return true;
                    tag = ret[--depth];
                    break;
                default: return false;                                   // refe / end inside a CALLed packet
                }
            }
            return false;
        }

        uint32_t handleBlock(PS2Memory &memory, const Draw &d, uint64_t seq, const uint8_t *bytes, uint32_t size, uint32_t units);

        // step 4: a block the walker copied no bytes of - its recording, or (cannot be used) its bytes from guest RAM, which
        // by the game thread's write watch still hold what the recording was made from
        void replaySkipped(PS2Memory &memory, const Draw &d, uint64_t seq)
        {
            const uint64_t key = recordingKey(d.kind, d.addr, d.units);
            const auto it = g_recordings.find(key);
            if (it != g_recordings.end())
            {
                Recording &rec = it->second;
                if (!rec.ops.empty() && rec.replayable && rec.mode == memory.vif1_regs.mode && rec.cycle == memory.vif1_regs.cycle)
                {
                    replay(memory, rec);
                    ++g_stats[d.kind].blocks;
                    ++g_stats[d.kind].replayed;
                    ++g_skipReplayed;
                    t_confirm.push_back({key, seq});
                    return;
                }
            }
            ++g_skipFallback;
            if (d.kind == World)
            {
                handleBlock(memory, d, seq, memory.getRDRAM() + (d.addr & kRamMask), d.units * 8u, d.units);
                return;
            }
            thread_local std::vector<uint8_t> flat;
            if (flattenCall(memory.getRDRAM(), d.addr, flat) && flat.size() == size_t(d.units) * 8u)
                handleBlock(memory, d, seq, flat.data(), static_cast<uint32_t>(flat.size()), d.units);
        }

        // the MARK before a marked REF's payload / CALL's packet (the processing thread, in stream order); the marked
        // bytes follow the MARK qword: their length (8-byte units) in the word after the MARK, then two NOPs
        uint32_t onMark(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail)
        {
            const ChainTable *table = t_table.get();
            if (avail >= 4u) // step 4: skipped marks the worker cannot place (their block would be lost)
            {
                uint32_t w;
                std::memcpy(&w, next, 4);
                if ((w & 0x00FFFFFFu) == ps2chain::kSkippedLength && (!table || imm >= table->draws.size()))
                    ++g_skipLost;
            }
            if (!table || imm >= table->draws.size() || avail < 12u)
                return 0u; // a MARK the game wrote itself
            const Draw &d = table->draws[imm];
            uint32_t units;
            std::memcpy(&units, next, 4);
            units &= 0x00FFFFFFu;
            if (units == ps2chain::kSkippedLength) // step 4: no bytes copied
            {
                ++g_skipSeen;
                replaySkipped(memory, d, table->seq);
                report();
                return 12u;
            }
            const uint32_t size = units * 8u;
            if (!size || size > avail - 12u)
            {
                ++g_unmarked; // a CALL whose RET the chain did not reach: the packet path runs on
                return 0u;
            }
            return 12u + handleBlock(memory, d, table->seq, next + 12u, size, units);
        }

        // a marked block's bytes: replayed from its recording, recorded, or processed by the packet path; returns size
        uint32_t handleBlock(PS2Memory &memory, const Draw &d, uint64_t seq, const uint8_t *bytes, uint32_t size, uint32_t units)
        {
            static const bool measure = std::getenv("VCS_RSL_MEASURE") != nullptr;
            if (measure)
            {
                ++g_measure.blocks;
                g_measure.blockBytes += size;
                if (t_seen.emplace((uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units, size).second)
                {
                    ++g_measure.distinct;
                    g_measure.distinctBytes += size;
                }
            }
            Stats &stats = g_stats[d.kind];
            ++stats.blocks;
            Recording &rec = g_recordings[(uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units];
            if (rec.skip && !g_verify)
            {
                --rec.skip;
                memory.processVIF1Data(bytes, size);
                ++(rec.replayable ? stats.changing : stats.notReplayable);
                return size; // (onMark adds the MARK qword's 12 bytes: returning them here too swallowed the next tag)
            }
            const uint64_t hash = hashBlock(bytes, size);
            if (d.wouldSkip) // VCS_RSL_SKIP_VERIFY: the skip would have replayed the recording of these bytes?
            {
                const auto it = g_recordings.find(recordingKey(d.kind, d.addr, d.units));
                const bool same = units == d.units && it != g_recordings.end() && it->second.hash == hash && !it->second.ops.empty();
                ++(same ? g_skipVerifySame : g_skipVerifyDiffer);
                static int logged = 0;
                if (!same && logged++ < 10)
                    std::fprintf(stderr, "[rsl-world] skip verify: %s block at 0x%08x would have replayed %s\n", kKindNames[d.kind], d.addr,
                                 units != d.units ? "another length" : it == g_recordings.end() ? "no recording" : "a recording of other bytes");
            }
            const bool valid = rec.hash == hash && !rec.ops.empty() && rec.mode == memory.vif1_regs.mode &&
                               rec.cycle == memory.vif1_regs.cycle;
            const uint64_t key = (uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units;
            if (valid && rec.replayable && !g_verify)
            {
                replay(memory, rec);
                rec.changes = 0;
                ++stats.replayed;
                t_confirm.push_back({key, seq}); // step 4: the recording matches this copy
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
                if (rec.replayable)
                    t_confirm.push_back({key, seq});
            }
            report();
            return size;
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
        // Step 4 (docs/FULLY_NATIVE.md): the guest pages a marked block is made of - a world REF's payload, a CALLed
        // packet's tags and payloads - with their write-watch generations when it was last copied. Unchanged pages: the
        // block holds the bytes it held then (runtime/ps2_write_watch.h).
        struct PageSnapshot
        {
            std::vector<std::pair<uint32_t, uint32_t>> pages; // page, generation
            bool usable = false;                                // only RAM pages, all seen
            uint32_t units = 0;                                 // its length as copied (8-byte units)
            uint64_t stableSince = 0;                           // the copy (g_chainSeq) since which its pages are unchanged
        };
        std::unordered_map<uint64_t, uint64_t> g_confirmed; // recording key -> latest chain the worker confirmed it in
        bool g_skip = false;
        std::unordered_map<uint64_t, PageSnapshot> g_snapshots;
        struct OpenBlock
        {
            bool open = false;
            uint64_t key = 0;
            int depth = 0;
            bool usable = true;
            uint32_t units = 0; // bytes copied so far / 8
            std::vector<uint32_t> pages;
        } g_openBlock;

        bool unchanged(const PageSnapshot &snap);

        // step 4: may this block go uncopied? Its pages unchanged since a copy the worker confirmed its recording for.
        // The fill-in for snapshotKey's Draw: its length as last copied.
        bool skippable(uint64_t snapshotKey, Kind kind, uint32_t addr, uint32_t &units)
        {
            // VCS_RSL_SKIP_KINDS: a bit per Kind (default all)
            static const unsigned kinds = std::getenv("VCS_RSL_SKIP_KINDS") ? unsigned(std::strtoul(std::getenv("VCS_RSL_SKIP_KINDS"), nullptr, 0)) : 0xFu;
            if (!g_skip || !((kinds >> kind) & 1u))
                return false;
            const auto it = g_snapshots.find(snapshotKey);
            if (it == g_snapshots.end() || !unchanged(it->second))
                return false;
            const auto c = g_confirmed.find(recordingKey(kind, addr, it->second.units));
            if (c == g_confirmed.end() || c->second < it->second.stableSince)
                return false;
            units = it->second.units;
            return true;
        }

        uint64_t blockKey(Kind kind, uint32_t addr, uint32_t qwc)
        {
            return (uint64_t(kind) << 56) | (uint64_t(addr & kRamMask) << 24) | (qwc & 0xFFFFFFu);
        }

        void addPages(std::vector<uint32_t> &pages, bool &usable, uint32_t addr, uint32_t bytes)
        {
            if ((addr & 0xF0000000u) == 0x70000000u) // scratchpad: not watched
            {
                usable = false;
                return;
            }
            addr &= kRamMask;
            for (uint32_t p = addr >> ps2writewatch::kPageShift; bytes && p <= (addr + bytes - 1u) >> ps2writewatch::kPageShift; ++p)
                pages.push_back(p);
        }

        // a marked block's page set complete: was it unchanged since its last copy? then remember this copy's
        bool unchanged(const PageSnapshot &snap)
        {
            if (!snap.usable)
                return false;
            for (const auto &[page, gen] : snap.pages)
                if (!ps2writewatch::unchangedSince(page, gen))
                    return false;
            return true;
        }

        void closeBlock(uint64_t key, std::vector<uint32_t> &pages, bool usable, uint32_t units)
        {
            ++g_cleanStats.blocks;
            PageSnapshot &snap = g_snapshots[key];
            const bool clean = unchanged(snap) && snap.units == units;
            if (snap.usable)
                g_cleanStats.clean += clean ? 1u : 0u;
            else
                ++g_cleanStats.unseen;
            if (!clean)
                snap.stableSince = g_chainSeq; // this copy is the first of the bytes it holds now
            snap.units = units;
            std::sort(pages.begin(), pages.end());
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
            snap.pages.clear();
            for (uint32_t page : pages)
                snap.pages.push_back({page, ps2writewatch::generation(page)});
            snap.usable = usable && ps2writewatch::enabled();
        }

        // every tag of the chain, while a CALLed packet is open: its pages (the walker copies the tag's upper half and the
        // payload); the matching RET closes it
        void trackOpenBlock(uint32_t tagAddr, uint32_t id, uint32_t dataAddr, uint32_t qwc)
        {
            if (!g_openBlock.open)
                return;
            addPages(g_openBlock.pages, g_openBlock.usable, tagAddr, 16u);
            if (dataAddr && qwc)
                addPages(g_openBlock.pages, g_openBlock.usable, dataAddr, qwc * 16u);
            g_openBlock.units += 1u + qwc * 2u; // the tag's upper half and its payload, as the walker copies them
            if (id == 5u)
                ++g_openBlock.depth;
            else if (id == 6u && --g_openBlock.depth == 0)
            {
                g_openBlock.open = false;
                closeBlock(g_openBlock.key, g_openBlock.pages, g_openBlock.usable, g_openBlock.units);
            }
            else if (id == 0u || id == 7u) // the chain ended inside it
                g_openBlock.open = false;
        }
        // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
        PS2_GUEST_CC void worldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_current.push_back({3u, getRegU32(ctx, 4) & kRamMask, getRegU32(ctx, 5) & 0xFFFFu, World});
            g_worldOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_current.push_back({5u, getRegU32(ctx, 4) & kRamMask, 0u, Model});
            g_modelOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelBHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
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
            {
                g_building = ChainTable{};
                g_building.seq = ++g_chainSeq;
                g_openBlock.open = false;
                std::lock_guard<std::mutex> lock(g_confirmMutex); // the worker's confirmations since the last kick
                for (const auto &[key, seq] : g_confirmPending)
                {
                    uint64_t &c = g_confirmed[key];
                    c = std::max(c, seq);
                }
                g_confirmPending.clear();
            }
            trackOpenBlock(tagAddr, id, dataAddr, qwc);
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
                    const uint64_t snapshotKey = blockKey(World, want->addr, qwc);
                    uint32_t units = 0;
                    if (!g_openBlock.open && skippable(snapshotKey, World, want->addr, units))
                    {
                        Draw d = *want;
                        d.units = units;
                        if (g_skipVerify)
                            d.wouldSkip = true; // copied all the same (below), checked by the worker
                        else if (const uint32_t m = mark(d))
                        {
                            ++g_skipped;
                            return m | ps2chain::kSkipWithMark;
                        }
                        if (d.wouldSkip)
                        {
                            const uint32_t m = mark(d);
                            if (m)
                            {
                                std::vector<uint32_t> pages;
                                bool usable = true;
                                addPages(pages, usable, dataAddr, qwc * 16u);
                                closeBlock(snapshotKey, pages, usable, qwc * 2u);
                            }
                            return m;
                        }
                    }
                    const uint32_t m = mark(*want);
                    if (m && !g_openBlock.open)
                    {
                        std::vector<uint32_t> pages;
                        bool usable = true;
                        addPages(pages, usable, dataAddr, qwc * 16u);
                        closeBlock(snapshotKey, pages, usable, qwc * 2u);
                    }
                    return m;
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
            uint32_t units = 0;
            if (!g_openBlock.open && skippable(blockKey(d.kind, d.addr, 0u), d.kind, d.addr, units))
            {
                d.units = units;
                if (g_skipVerify)
                    d.wouldSkip = true; // copied all the same, checked by the worker
                else if (const uint32_t m = mark(d))
                {
                    ++g_skipped;
                    return m | ps2chain::kSkipWithMark; // the walker goes on after the CALL
                }
            }
            const uint32_t m = mark(d);
            if (m && !g_openBlock.open) // the walker marks a CALL outside a marked packet only
            {
                g_openBlock = OpenBlock{};
                g_openBlock.open = true;
                g_openBlock.key = blockKey(d.kind, d.addr, 0u);
                g_openBlock.depth = 1;
            }
            return m;
        }

        void queued(const void *key, uint32_t sizeBytes)
        {
            g_lastChainBytes.store(sizeBytes, std::memory_order_relaxed);
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
                if (!t_confirm.empty())
                {
                    std::lock_guard<std::mutex> lock(g_confirmMutex);
                    g_confirmPending.insert(g_confirmPending.end(), t_confirm.begin(), t_confirm.end());
                    t_confirm.clear();
                }
                return;
            }
            t_chain = static_cast<const uint8_t *>(key);
            t_seen.clear();
            ++g_measure.chains;
            g_measure.chainBytes += g_lastChainBytes.load(std::memory_order_relaxed);
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
        // step 4: blocks known unchanged replayed without being copied (needs the write watch; never while verifying)
        g_skip = !off("VCS_RSL_SKIP") && !g_verify && ps2writewatch::enabled();
        g_skipVerify = g_skip && std::getenv("VCS_RSL_SKIP_VERIFY") != nullptr; // decide, copy anyway, compare
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
