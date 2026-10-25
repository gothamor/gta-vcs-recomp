// World and model draws from the RSL draw calls - see game/vcs_rsl_world.h.
#include "game/vcs_rsl_world.h"

#include "game/vcs_hle_world.h"
#include "game/vcs_rsl_native.h"
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
#include <string>
#include <vector>

namespace vcs::rslworld
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPktLo = 0x520F90u, kPktHi = 0x6EDC70u; // the packet buffer (game-internals.md 6i)
        constexpr uint32_t kWorldDraw = 0x002F7EB0u;               // FUN_002F7EB0(geometry, qwc): REF geometry
        constexpr uint32_t kSectorDraw = 0x002409C8u;              // FUN_002409C8(level, sector, pass): a sector's draws
        constexpr uint32_t kSectorCaching = 0x00241060u;           // its return address when FUN_00240ED8 caches its packets
        // step 5a: FUN_002409C8 caches a material's setup - FUN_00408F30 (texture bind) and FUN_002F7B30 (GS registers)
        // between these calls are copied into a block CALLed on later frames (game-internals.md 6i)
        constexpr uint32_t kTextureBind = 0x00408F30u, kMaterialCacheBind = 0x00240D1Cu, kMaterialCacheGs = 0x00240D24u;
        constexpr uint32_t kGsSendA = 0x002F7908u, kGsSendB = 0x002F7B30u; // send the queued GS registers
        constexpr uint32_t kTexMatrix = 0x002F7090u;                       // FUN_002F7090(m): 3 qwords -> VU 0x2C
        constexpr uint32_t kVuConstants = 0x001CEF18u;                     // FUN_001CEF18(vu, m): 3 qwords -> VU vu
        constexpr uint32_t kFlushA = 0x002F9CE8u;                          // FUN_002F9CE8: CNT with FLUSHA
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
            uint64_t flatHash = 0;  // VCS_RSL_ITEMS_VERIFY: the hash of the block as an item would have copied it (0: none)
        };

        // ---- step 4c: draw items (docs/FULLY_NATIVE.md step 4) -------------------------------------------------------
        // A world / model draw call whose state flush is native writes no packets for itself: its flush (when it is all
        // CNTs), its GS registers and its block go into an item, and the packet buffer gets one 16-byte CNT tag whose two
        // VIFcodes are MARK (kItemMark + index) and a NOP holding kItemNop - its place in the chain. At the kick the item's
        // block is decided as the walker decides a marked block (skipped when unchanged and confirmed, else copied from
        // guest RAM as the walker copies it); at the MARK the worker runs the item's VIF bytes and the block.
        constexpr uint32_t kItemMark = 0x8000u;    // MARK imm of an item: kItemMark + index (the walker's own marks stay below)
        // the NOP after an item's MARK (opcode 0: a NOP; its low bits ignored): kItemNop | the frame's number (low 16 bits) -
        // a tag the game copied and sends again in a later frame is told apart from this frame's (FUN_00240ED8 caches whole
        // sector draws, game-internals.md 6i)
        constexpr uint32_t kItemNop = 0x00D40000u;
        constexpr uint32_t kMaxItems = 0x7FFFu;
        struct RasterInBlock // a raster upload inside an item's copied block (step 3d names textures by them)
        {
            uint32_t offset, address, bytes; // offset: in the block's bytes
        };
        struct Item
        {
            Draw d;
            uint32_t prefixAt = 0, prefixBytes = 0; // in the table's arena: the flush's VIF bytes, then the GS registers' DIRECTHL
            uint32_t blockAt = 0, blockBytes = 0;   // in the arena: the block, copied at the kick (none when skipped)
            uint32_t rastersAt = 0, rasterCount = 0; // in the table's itemRasters
            bool skip = false, lost = false;         // skip: replayed by key; lost: its block could not be copied
            bool hasBlock = false;                   // step 5a: an item of state ops alone has none
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
            std::vector<Item> items;        // step 4c: the frame's draw items, by MARK index - kItemMark
            std::vector<uint8_t> arena;     // their VIF bytes and blocks
            std::vector<RasterInBlock> itemRasters;
            uint32_t chainBytes = 0;        // the copied chain's size
            uint32_t itemFrame = 0;         // the frame whose items these are (g_itemFrame)
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
        // step 4c: an item's block being processed (its bytes are in the table's arena, not in the chain) and its rasters
        thread_local const uint8_t *t_block = nullptr;
        thread_local uint32_t t_blockBytes = 0;
        thread_local const RasterInBlock *t_blockRasters = nullptr;
        thread_local uint32_t t_blockRasterCount = 0;
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
            if (t_block && data >= t_block && data < t_block + t_blockBytes) // inside an item's block
            {
                thread_local RasterRef found;
                const uint32_t offset = static_cast<uint32_t>(data - t_block);
                for (uint32_t i = 0; i < t_blockRasterCount; ++i)
                    if (t_blockRasters[i].offset == offset && t_blockRasters[i].bytes == bytes)
                    {
                        found = {offset, t_blockRasters[i].address, bytes};
                        return &found;
                    }
                return nullptr;
            }
            const ChainTable *table = t_table.get();
            if (!table || table->rasters.empty() || !t_chain || data < t_chain || data >= t_chain + table->chainBytes)
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

        void reportItems(); // step 4c

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
            reportItems();
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

        // step 4c: an item's MARK - its VIF bytes (the flush, the GS registers), then its block
        void onItem(PS2Memory &memory, uint32_t index, uint32_t frame);

        // the MARK before a marked REF's payload / CALL's packet (the processing thread, in stream order); the marked
        // bytes follow the MARK qword: their length (8-byte units) in the word after the MARK, then two NOPs
        uint32_t onMark(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail)
        {
            if (imm >= kItemMark && avail >= 4u)
            {
                uint32_t w;
                std::memcpy(&w, next, 4);
                if ((w & 0xFFFF0000u) == kItemNop)
                {
                    onItem(memory, imm - kItemMark, w & 0xFFFFu);
                    return 4u; // its NOP
                }
            }
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

        std::atomic<uint64_t> g_itemsSeenW{0}, g_itemsUnplacedW{0}, g_itemBlockSameW{0}, g_itemBlockDifferW{0};

        std::atomic<uint64_t> g_itemsStaleW{0};
        void onItem(PS2Memory &memory, uint32_t index, uint32_t frame)
        {
            const ChainTable *table = t_table.get();
            thread_local int depth = 0;
            if (table && (frame != (table->itemFrame & 0xFFFFu) || depth > 0)) // (an item inside an item's block: copied too)
            {
                ++g_itemsStaleW; // another frame's tag (the game copied it) - never drawn from this frame's items
                static int logged = 0;
                if (logged++ < 10)
                    std::fprintf(stderr, "[rsl-items] a MARK of frame %u in frame %u's chain (index %u, depth %d): not drawn\n", frame,
                                 table->itemFrame & 0xFFFFu, index, depth);
                return;
            }
            if (!table || index >= table->items.size())
            {
                ++g_itemsUnplacedW;
                return;
            }
            const Item &it = table->items[index];
            ++g_itemsSeenW;
            ++depth;
            struct Leave
            {
                int &d;
                ~Leave() { --d; }
            } leave{depth};
            if (it.prefixBytes)
                memory.processVIF1Data(table->arena.data() + it.prefixAt, it.prefixBytes);
            if (it.skip)
                replaySkipped(memory, it.d, table->seq);
            else if (it.blockBytes)
            {
                t_block = table->arena.data() + it.blockAt;
                t_blockBytes = it.blockBytes;
                t_blockRasters = it.rasterCount ? &table->itemRasters[it.rastersAt] : nullptr;
                t_blockRasterCount = it.rasterCount;
                handleBlock(memory, it.d, table->seq, t_block, it.blockBytes, it.blockBytes / 8u);
                t_block = nullptr;
                t_blockBytes = 0;
                t_blockRasters = nullptr;
                t_blockRasterCount = 0;
            }
            report();
        }

        // a marked block's bytes: replayed from its recording, recorded, or processed by the packet path; returns size
        uint32_t handleBlock(PS2Memory &memory, const Draw &d, uint64_t seq, const uint8_t *bytes, uint32_t size, uint32_t units)
        {
            if (d.flatHash) // VCS_RSL_ITEMS_VERIFY: an item would have copied these bytes?
            {
                const bool same = hashBlock(bytes, size) == d.flatHash;
                ++(same ? g_itemBlockSameW : g_itemBlockDifferW);
                static int logged = 0;
                if (!same && logged++ < 10)
                    std::fprintf(stderr, "[rsl-items] verify: %s block at 0x%08x (%u bytes) copied otherwise than flattenBlock copies it\n",
                                 kKindNames[d.kind], d.addr, size);
            }
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

        void noteFlattenable(uint64_t key, uint32_t units); // step 4c

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
                if (g_openBlock.usable)
                    noteFlattenable(g_openBlock.key, g_openBlock.units); // step 4c
            }
            else if (id == 0u || id == 7u) // the chain ended inside it
                g_openBlock.open = false;
        }
        // ---- step 4c: draw items, the game thread ----------------------------------------------------------------
        bool g_items = false, g_itemVerify = false, g_ops = false;
        const uint8_t *g_rdram = nullptr;
        std::vector<Item> g_itemsCurrent, g_itemsKicked;       // this frame's items / the frame whose chain is being copied
        std::vector<uint8_t> g_arenaCurrent, g_arenaKicked;    // their VIF bytes
        // the tables' arenas / item lists go back to the game thread after processing (a fresh one each frame cost page
        // faults on ~1 MB a frame)
        std::mutex g_poolMutex;
        std::vector<std::vector<uint8_t>> g_arenaPool;
        std::vector<std::vector<Item>> g_itemPool;
        template <typename T>
        void takePooled(std::vector<std::vector<T>> &pool, std::vector<T> &into)
        {
            if (into.capacity())
                return;
            std::lock_guard<std::mutex> lock(g_poolMutex);
            if (!pool.empty())
            {
                into = std::move(pool.back());
                pool.pop_back();
                into.clear();
            }
        }
        std::unordered_map<uint64_t, bool> g_flatOk;           // model blocks (blockKey): flattenBlock copies them as the walker does
        uint32_t g_itemFrame = 1, g_itemFrameKicked = 0;       // frames counted at the kick: whose items an item tag names
        bool g_caching = false; // FUN_00240ED8 is caching a sector's packets (they are copied and sent again): no items
        PS2Runtime::RecompiledFunction g_sectorOriginal = nullptr;
        struct ItemStats
        {
            uint64_t made[KindCount] = {}, flush = 0, stateCall = 0, unknown = 0, noRoom = 0, shape = 0, full = 0, caching = 0;
            uint64_t skipped = 0, copied = 0, lost = 0, verifySame = 0, verifyDiffer = 0;
        } g_itemStats;                                          // game thread

        constexpr uint32_t kPacketCursor = 0x520F90u + 0x18u; // the packet buffer's DMA cursor (game-internals.md 6i)
        constexpr uint32_t kDirty = 0x4882A0u, kGsQueue = 0x729510u, kGsQueueCount = 0x488308u;

        uint32_t rd32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // DMA tags with inline data (CNT only) as the VIF stream the walker makes of them: each tag's upper half, then its
        // payload
        void tagsToVif(const uint8_t *tags, uint32_t size, std::vector<uint8_t> &out)
        {
            size_t at = out.size();
            out.resize(at + size); // at most the tags' size (each loses its lower half)
            for (uint32_t t = 0; t + 16u <= size;)
            {
                uint64_t lo;
                std::memcpy(&lo, tags + t, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu);
                std::memcpy(out.data() + at, tags + t + 8, 8 + qwc * 16u); // the upper half and the payload after it
                at += 8u + qwc * 16u;
                t += 16u + qwc * 16u;
            }
            out.resize(at);
        }

        // the draw's GS register packet as FUN_002F7D58 / FUN_002F80A0 / FUN_002F7EB0 write it (game-internals.md 6i): CNT
        // (n + 1), FLUSHE, DIRECTHL (n + 1), a GIFtag, the n queued {value, register} pairs - the queue in guest memory,
        // then the flush's own
        void gsPacket(const uint8_t *rdram, const vcs::rslnative::Flush &f, std::vector<uint8_t> &out)
        {
            const uint32_t inRam = rd32(rdram, kGsQueueCount), n = inRam + f.queued;
            if (n == 0u)
                return;
            const uint64_t head[4] = {uint64_t(n + 1u) | 0x10000000u, 0x10000000ull | (uint64_t((n + 1u) | 0x51000000u) << 32),
                                      uint64_t(n) | 0x1000000000008000ull, 0xEull};
            const auto *h = reinterpret_cast<const uint8_t *>(head);
            out.insert(out.end(), h, h + sizeof(head));
            out.insert(out.end(), rdram + kGsQueue, rdram + kGsQueue + inRam * 16u);
            const auto *q = reinterpret_cast<const uint8_t *>(f.queue);
            out.insert(out.end(), q, q + f.queued * 16u);
        }

        // the GS register packet as the VIF stream the walker makes of it (FLUSHE, DIRECTHL, then the payload), in one append
        void gsVif(const uint8_t *rdram, const vcs::rslnative::Flush &f, std::vector<uint8_t> &out)
        {
            const uint32_t inRam = rd32(rdram, kGsQueueCount), n = inRam + f.queued;
            if (n == 0u)
                return;
            const size_t at = out.size();
            out.resize(at + 8u + 16u + n * 16u);
            uint8_t *p = out.data() + at;
            const uint64_t head[3] = {0x10000000ull | (uint64_t((n + 1u) | 0x51000000u) << 32), uint64_t(n) | 0x1000000000008000ull, 0xEull};
            std::memcpy(p, head, sizeof(head));
            std::memcpy(p + 24, rdram + kGsQueue, inRam * 16u);
            std::memcpy(p + 24 + inRam * 16u, f.queue, f.queued * 16u);
        }

        // a CALLed packet flattened from guest RAM as the chain walker copies it (each tag's upper half, then its payload)
        // up to the RET that returns from it, with the pages it is made of (as trackOpenBlock notes them) and the raster
        // uploads in it (as observe finds them); false: not as the walker would copy it (scratchpad, nesting, no RET)
        bool flattenBlock(const uint8_t *rdram, uint32_t target, std::vector<uint8_t> &out, std::vector<uint32_t> &pages, bool &usable,
                          std::vector<RasterInBlock> *rasters)
        {
            const size_t start = out.size();
            uint32_t tag = target, ret[2] = {}, depth = 0;
            for (int n = 0; n < 65536; ++n)
            {
                if ((tag & 0xF0000000u) == 0x70000000u) // a tag in scratchpad (the walker reads it there)
                    return false;
                tag &= kRamMask;
                if (tag + 16u > kRamMask + 1u)
                    return false;
                uint64_t lo, hi;
                std::memcpy(&lo, rdram + tag, 8);
                std::memcpy(&hi, rdram + tag + 8, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu), id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>(lo >> 32) & 0x7FFFFFFFu;
                const uint32_t after = tag + 16u;
                const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : after;
                addPages(pages, usable, tag, 16u);
                out.insert(out.end(), rdram + tag + 8, rdram + tag + 16);
                if (qwc)
                {
                    if ((data & 0xF0000000u) == 0x70000000u || (data & kRamMask) + qwc * 16u > kRamMask + 1u)
                        return false;
                    addPages(pages, usable, data, qwc * 16u);
                    const uint32_t code = static_cast<uint32_t>(hi >> 32);
                    if (rasters && g_rasters && id == 3u && ((code >> 24) & 0x7Eu) == 0x50u && (code & 0xFFFFu) == qwc)
                        rasters->push_back({static_cast<uint32_t>(out.size() - start), data & kRamMask, qwc * 16u});
                    out.insert(out.end(), rdram + (data & kRamMask), rdram + (data & kRamMask) + qwc * 16u);
                }
                switch (id)
                {
                case 1: tag = after + qwc * 16u; break; // cnt
                case 2: tag = addr; break;              // next
                case 3: case 4: tag = after; break;     // ref, refs
                case 5:                                 // call
                    if (depth >= 1u)                    // the walker's ASR holds two: this packet's return and one more
                        return false;
                    ret[depth++] = after + qwc * 16u;
                    tag = addr;
                    break;
                case 6: // ret: from a nested call, or out of the packet
                    if (depth == 0u)
                        return true;
                    tag = ret[--depth];
                    break;
                default: return false; // refe / end inside a CALLed packet
                }
            }
            return false;
        }

        // ---- step 5a: state ops ----------------------------------------------------------------------------------------
        // An item of VIF bytes alone (a setter's UNPACK, a GS register packet, a standalone flush), in chain order like a
        // draw's. Consecutive ones - and the draw after them - share one item and one tag while nothing else was written
        // to the packet buffer in between (the cursor is still right after the open item's tag).
        int g_openItem = -1;      // the last item, still taking ops (no block yet)
        uint32_t g_openEnd = 0;   // the cursor right after its tag
        bool g_materialCaching = false; // FUN_002409C8 is caching a material's setup (its packets are copied)
        struct OpStats
        {
            uint64_t ops = 0, merged = 0, packets = 0, caching = 0, mergedDraws = 0;
        } g_opStats;

        bool opsAllowed() { return g_items && !g_itemVerify && !g_caching && !g_materialCaching; }

        // the half in use and the room for `bytes` at the cursor: false = the packet path
        bool cursorForItem(const uint8_t *rdram, uint32_t bytes, uint32_t &cursor)
        {
            if (!vcs::rslnative::packetRoom(rdram, bytes, cursor))
                return false;
            const uint32_t half = 0x520F90u + rd32(rdram, 0x520F90u + 0x14u) * 0xE6670u + 0x20u;
            return cursor >= half && cursor + bytes <= half + 0xE6670u;
        }

        void writeItemTag(uint8_t *rdram, uint32_t cursor, uint32_t index)
        {
            const uint32_t tag[4] = {0x10000000u, 0u, 0x07000000u | (kItemMark + index), kItemNop | (g_itemFrame & 0xFFFFu)}; // CNT: MARK, NOP
            std::memcpy(rdram + (cursor & kRamMask), tag, 16);
            wr32(rdram, kPacketCursor, cursor + 16u);
        }

        // VIF bytes as a state op: into the open item, or a new item and its tag; false = the caller writes its packets
        bool appendOp(uint8_t *rdram, const uint8_t *vif, uint32_t bytes)
        {
            uint32_t cursor = 0;
            if (!cursorForItem(rdram, 16u, cursor))
                return false;
            if (g_openItem >= 0 && cursor == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size())
            {
                g_arenaCurrent.insert(g_arenaCurrent.end(), vif, vif + bytes);
                g_itemsCurrent[g_openItem].prefixBytes += bytes;
                ++g_opStats.merged;
                return true;
            }
            if (g_itemsCurrent.size() >= kMaxItems)
                return false;
            const uint32_t index = static_cast<uint32_t>(g_itemsCurrent.size());
            Item &it = g_itemsCurrent.emplace_back();
            it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
            g_arenaCurrent.insert(g_arenaCurrent.end(), vif, vif + bytes);
            it.prefixBytes = bytes;
            writeItemTag(rdram, cursor, index);
            g_openItem = static_cast<int>(index);
            g_openEnd = cursor + 16u;
            ++g_opStats.ops;
            return true;
        }

        // the draw call as an item (game thread, the draw function's whole work); false: the original runs
        bool itemDraw(uint8_t *rdram, R5900Context *ctx, Kind kind, uint32_t raw0, uint32_t raw1)
        {
            ItemStats &st = g_itemStats;
            // FUN_002F80A0 CALLs the shared state packet (*0x48830C) after the GS packet when bit 31 is set: then the flush,
            // the GS packet and that CALL stay in the chain as the original writes them, and the item is the block alone
            const bool stateCall = kind == ModelB && static_cast<int32_t>(rd32(rdram, kDirty)) < 0;
            const uint32_t addr = raw0 & kRamMask;
            if (kind == World ? ((raw0 & 0xF0000000u) != 0u || (raw1 & 0xFFFFu) == 0u || (raw1 & 0x0FFF0000u) != 0u)
                              : (raw0 & 0xF0000000u) != 0u)
            {
                ++st.shape;
                return false;
            }
            if (kind != World && !g_flatOk.count(blockKey(kind, addr, 0u)))
            {
                ++st.unknown; // not yet seen copied by the walker as flattenBlock copies it
                return false;
            }
            if (g_itemsCurrent.size() >= kMaxItems)
            {
                ++st.full;
                return false;
            }
            if (g_caching)
            {
                ++st.caching; // the packets are copied into the sector's cache: the packet path
                return false;
            }
            thread_local vcs::rslnative::Flush f;
            if (!vcs::rslnative::buildFlush(rdram, f))
            {
                ++st.flush; // callbacks / FUN_001CFAE0: the original
                return false;
            }
            // what the original would write: the flush's tags (kept in the packet buffer when they CALL / REF), the GS packet,
            // the draw's tag - here an item's CNT instead of the draw's tag
            thread_local std::vector<uint8_t> tags; // what stays in the packet buffer
            tags.clear();
            if (!f.cntOnly || stateCall)
                tags.assign(f.bytes, f.bytes + f.size);
            if (stateCall)
            {
                gsPacket(rdram, f, tags);
                const uint64_t call[2] = {(uint64_t(rd32(rdram, 0x48830Cu)) << 32) | 0x50000000u, 0u};
                const auto *c = reinterpret_cast<const uint8_t *>(call);
                tags.insert(tags.end(), c, c + 16);
            }
            const uint32_t written = static_cast<uint32_t>(tags.size()) + 16u;
            uint32_t cursor = 0;
            if (!cursorForItem(rdram, written, cursor))
            {
                ++st.noRoom; // no room, or the cursor is somewhere else (a packet built for later): the packet path
                return false;
            }
            // step 5a: the open item's ops come right before this draw - it takes the draw (no tag of its own)
            const bool merge = tags.empty() && g_openItem >= 0 && cursor == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size();
            const uint32_t index = merge ? static_cast<uint32_t>(g_openItem) : static_cast<uint32_t>(g_itemsCurrent.size());
            Item &it = merge ? g_itemsCurrent[index] : g_itemsCurrent.emplace_back();
            it.d = {kind == World ? 3u : 5u, addr, kind == World ? (raw1 & 0xFFFFu) : 0u, kind};
            it.hasBlock = true;
            if (!merge)
                it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
            const uint32_t prefixBefore = merge ? it.prefixBytes : 0u;
            g_openItem = -1; // a draw ends the item
            if (f.cntOnly && !stateCall)
                tagsToVif(f.bytes, f.size, g_arenaCurrent);
            if (!stateCall)
                gsVif(rdram, f, g_arenaCurrent);
            it.prefixBytes = static_cast<uint32_t>(g_arenaCurrent.size()) - it.prefixAt;
            (void)prefixBefore;
            if (!tags.empty())
            {
                std::memcpy(rdram + (cursor & kRamMask), tags.data(), tags.size());
                cursor += static_cast<uint32_t>(tags.size());
            }
            if (stateCall)
                ++st.stateCall; // (made as an item all the same)
            if (merge)
                ++g_opStats.mergedDraws; // its tag is the open item's, already in the buffer
            else
                writeItemTag(rdram, cursor, index);
            wr32(rdram, kGsQueueCount, 0u); // the draw sent the queue
            wr32(rdram, kDirty, stateCall ? f.dirtyAfter & 0x7FFFFFFFu : f.dirtyAfter);
            ++st.made[kind];
            ctx->pc = getRegU32(ctx, 31);
            return true;
        }

        // VCS_RSL_ITEMS_VERIFY=1: the item is built but the original runs; what it wrote is compared with what the item
        // stands for (at the next draw call / the kick: code after the original returns is not relied on)
        struct ItemCheck
        {
            bool armed = false;
            uint32_t cursor = 0;
            std::vector<uint8_t> expect; // the flush's tags, the GS packet, the draw's tag
        } g_itemCheck;

        void checkItem(const uint8_t *rdram)
        {
            if (!g_itemCheck.armed)
                return;
            g_itemCheck.armed = false;
            const bool same = std::memcmp(rdram + (g_itemCheck.cursor & kRamMask), g_itemCheck.expect.data(), g_itemCheck.expect.size()) == 0;
            ++(same ? g_itemStats.verifySame : g_itemStats.verifyDiffer);
            static int logged = 0;
            if (!same && logged++ < 10)
                for (size_t i = 0; i < g_itemCheck.expect.size(); i += 8u)
                {
                    uint64_t want, got;
                    std::memcpy(&want, g_itemCheck.expect.data() + i, 8);
                    std::memcpy(&got, rdram + ((g_itemCheck.cursor + i) & kRamMask), 8);
                    if (want != got)
                    {
                        std::fprintf(stderr, "[rsl-items] verify: draw at 0x%08x differs at +0x%zx of %zu: item %016llx original %016llx\n",
                                     g_itemCheck.cursor, i, g_itemCheck.expect.size(), (unsigned long long)want, (unsigned long long)got);
                        break;
                    }
                }
        }

        void verifyDraw(const uint8_t *rdram, Kind kind, uint32_t raw0, uint32_t raw1)
        {
            checkItem(rdram);
            const bool stateCall = kind == ModelB && static_cast<int32_t>(rd32(rdram, kDirty)) < 0;
            thread_local vcs::rslnative::Flush f;
            if (!vcs::rslnative::buildFlush(rdram, f))
                return;
            auto &e = g_itemCheck.expect;
            e.assign(f.bytes, f.bytes + f.size);
            const size_t gsAt = e.size();
            gsPacket(rdram, f, e);
            { // the item's VIF bytes for the GS packet (gsVif) are the walker's for these tags
                thread_local std::vector<uint8_t> walker, item;
                walker.clear();
                item.clear();
                tagsToVif(e.data() + gsAt, static_cast<uint32_t>(e.size() - gsAt), walker);
                gsVif(rdram, f, item);
                if (walker != item)
                    ++g_itemStats.verifyDiffer;
            }
            if (stateCall)
            {
                const uint64_t call[2] = {(uint64_t(rd32(rdram, 0x48830Cu)) << 32) | 0x50000000u, 0u};
                const auto *c = reinterpret_cast<const uint8_t *>(call);
                e.insert(e.end(), c, c + 16);
            }
            const uint64_t drawTag[2] = {kind == World ? (uint64_t(raw0) << 32) | 0x30000000u | (raw1 & 0x0FFFFFFFu)
                                                       : (uint64_t(raw0) << 32) | 0x50000000u,
                                         0u};
            const auto *t = reinterpret_cast<const uint8_t *>(drawTag);
            e.insert(e.end(), t, t + 16);
            g_itemCheck.cursor = rd32(rdram, kPacketCursor);
            g_itemCheck.armed = true;
        }

        // at the kick, after the write watch's fold: each item's block skipped (unchanged since a confirmed copy) or copied
        // from guest RAM, as the walker would for the draw's tag
        void resolveItems(ChainTable &table)
        {
            thread_local std::vector<uint32_t> pages;
            for (Item &it : table.items)
            {
                if (!it.hasBlock)
                    continue; // step 5a: state ops alone
                Draw &d = it.d;
                const uint64_t snapshotKey = d.kind == World ? blockKey(World, d.addr, d.qwc) : blockKey(d.kind, d.addr, 0u);
                uint32_t units = 0;
                if (skippable(snapshotKey, d.kind, d.addr, units))
                {
                    d.units = units;
                    if (!g_skipVerify)
                    {
                        it.skip = true;
                        ++g_itemStats.skipped;
                        ++g_skipped;
                        continue;
                    }
                    d.wouldSkip = true; // copied all the same, checked by the worker
                }
                pages.clear();
                bool usable = true;
                it.blockAt = static_cast<uint32_t>(table.arena.size());
                it.rastersAt = static_cast<uint32_t>(table.itemRasters.size());
                if (d.kind == World)
                {
                    addPages(pages, usable, d.addr, d.qwc * 16u);
                    table.arena.insert(table.arena.end(), g_rdram + d.addr, g_rdram + d.addr + d.qwc * 16u);
                }
                else if (!flattenBlock(g_rdram, d.addr, table.arena, pages, usable, &table.itemRasters))
                {
                    table.arena.resize(it.blockAt);
                    table.itemRasters.resize(it.rastersAt);
                    it.lost = true; // not as the walker would copy it: drawn by the packet path from the next frame on
                    g_flatOk.erase(snapshotKey);
                    ++g_itemStats.lost;
                    continue;
                }
                it.blockBytes = static_cast<uint32_t>(table.arena.size()) - it.blockAt;
                it.rasterCount = static_cast<uint32_t>(table.itemRasters.size()) - it.rastersAt;
                closeBlock(snapshotKey, pages, usable, it.blockBytes / 8u);
                ++g_itemStats.copied;
            }
        }

        // a model block the walker copied (the packet path): may later draws of it be items?
        void noteFlattenable(uint64_t key, uint32_t units)
        {
            const Kind kind = static_cast<Kind>(key >> 56);
            if (!g_items || (kind != Model && kind != ModelB) || g_flatOk.count(key))
                return;
            thread_local std::vector<uint8_t> flat;
            thread_local std::vector<uint32_t> pages;
            flat.clear();
            pages.clear();
            bool usable = true;
            const uint32_t addr = static_cast<uint32_t>(key >> 24) & kRamMask;
            if (flattenBlock(g_rdram, addr, flat, pages, usable, nullptr) && flat.size() == size_t(units) * 8u && usable)
                g_flatOk[key] = true;
        }

        void reportItems()
        {
            if (!g_items)
                return;
            ItemStats &s = g_itemStats;
            std::fprintf(stderr, "[rsl-items] made: world %llu, model %llu, model-b %llu (with the state packet %llu); packet path: flush %llu, "
                                 "not known %llu, no room %llu, shape %llu, full %llu, caching %llu; at the kick: %llu skipped, %llu copied, %llu lost; "
                                 "worker: %llu seen, %llu not placed, %llu another frame's\n",
                         (unsigned long long)s.made[World], (unsigned long long)s.made[Model], (unsigned long long)s.made[ModelB],
                         (unsigned long long)s.stateCall, (unsigned long long)s.flush, (unsigned long long)s.unknown,
                         (unsigned long long)s.noRoom, (unsigned long long)s.shape, (unsigned long long)s.full, (unsigned long long)s.caching,
                         (unsigned long long)s.skipped,
                         (unsigned long long)s.copied, (unsigned long long)s.lost, (unsigned long long)g_itemsSeenW.exchange(0),
                         (unsigned long long)g_itemsUnplacedW.exchange(0), (unsigned long long)g_itemsStaleW.exchange(0));
            std::fprintf(stderr, "[rsl-items] state ops: %llu items of their own, %llu merged into one, %llu draws into an op item; packet path: %llu "
                                 "(no room / not RAM), %llu (a cache being filled)\n",
                         (unsigned long long)g_opStats.ops, (unsigned long long)g_opStats.merged, (unsigned long long)g_opStats.mergedDraws,
                         (unsigned long long)g_opStats.packets, (unsigned long long)g_opStats.caching);
            g_opStats = OpStats{};
            if (g_itemVerify)
                std::fprintf(stderr, "[rsl-items] verify: %llu draws / ops wrote what their item stands for, %llu differ; blocks: %llu copied as the walker "
                                     "copies them, %llu differ\n",
                             (unsigned long long)s.verifySame, (unsigned long long)s.verifyDiffer,
                             (unsigned long long)g_itemBlockSameW.exchange(0), (unsigned long long)g_itemBlockDifferW.exchange(0));
            s = ItemStats{};
        }

        PS2Runtime::RecompiledFunction g_gsSendOriginal[2] = {}, g_texMatrixOriginal = nullptr, g_vuConstantsOriginal = nullptr,
                                       g_flushAOriginal = nullptr, g_textureBindOriginal = nullptr;

        // VCS_RSL_ITEMS_VERIFY: what a setter would have made an op of, compared with what its original writes (checkItem)
        void verifyOp(const uint8_t *rdram, const uint8_t *tags, uint32_t bytes)
        {
            checkItem(rdram);
            g_itemCheck.expect.assign(tags, tags + bytes);
            g_itemCheck.cursor = rd32(rdram, kPacketCursor);
            g_itemCheck.armed = bytes != 0u;
        }

        // FUN_002F7908 / FUN_002F7B30 (the same code, game-internals.md 6i): queue FOG / FOGCOL / registers 0x14 / 0x15 by
        // the dirty bits 0x80 / 0x100 / 0x20 / 0x40, clear bits 0x1E0, send the queue as CNT + DIRECTHL
        template <int Which>
        PS2_GUEST_CC void gsSendHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (Which == 1 && ra == kMaterialCacheGs)
            {
                g_materialCaching = false; // the material cache's own GS packet (copied with it): the packet path, and its end
                ++g_opStats.caching;
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            const uint32_t dirty = rd32(rdram, kDirty);
            thread_local vcs::rslnative::Flush adds;
            adds.queued = 0;
            const struct
            {
                uint32_t bit, value, reg;
            } regs[4] = {{0x80u, 0x4882E0u, 6u}, {0x100u, 0x4882E8u, 7u}, {0x20u, 0x4882F0u, 0x14u}, {0x40u, 0x4882F8u, 0x15u}};
            for (const auto &g : regs)
                if (dirty & g.bit)
                {
                    uint64_t v;
                    std::memcpy(&v, rdram + g.value, 8);
                    adds.queue[adds.queued][0] = v;
                    adds.queue[adds.queued][1] = g.reg;
                    ++adds.queued;
                }
            if (g_itemVerify)
            {
                thread_local std::vector<uint8_t> e;
                e.clear();
                gsPacket(rdram, adds, e);
                verifyOp(rdram, e.data(), static_cast<uint32_t>(e.size()));
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            thread_local std::vector<uint8_t> vif;
            vif.clear();
            gsVif(rdram, adds, vif);
            if (!vif.empty() && (!opsAllowed() || !appendOp(rdram, vif.data(), static_cast<uint32_t>(vif.size()))))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            const uint32_t count = rd32(rdram, kGsQueueCount); // the queue as the original leaves it
            for (uint32_t i = 0; i < adds.queued; ++i)
                std::memcpy(rdram + kGsQueue + (count + i) * 16u, adds.queue[i], 16);
            wr32(rdram, kGsQueueCount, vif.empty() ? count : 0u);
            wr32(rdram, kDirty, dirty & 0xFFFFFE1Fu);
            ctx->pc = ra;
        }

        // a setter's CNT (FLUSHE, UNPACK V4-32 x3 -> vu) with the three qwords at src, as an op
        bool unpack3Op(uint8_t *rdram, uint32_t code1, uint32_t src, PS2Runtime::RecompiledFunction original, R5900Context *ctx,
                       PS2Runtime *runtime)
        {
            uint8_t tags[16 + 48];
            const uint64_t tag[2] = {0x10000003u, 0x10000000ull | (uint64_t(code1) << 32)};
            std::memcpy(tags, tag, 16);
            if ((src & 0xF0000000u) == 0u)
                std::memcpy(tags + 16, rdram + (src & kRamMask), 48);
            if (g_itemVerify)
            {
                if ((src & 0xF0000000u) == 0u)
                    verifyOp(rdram, tags, sizeof(tags));
                original(rdram, ctx, runtime);
                return false;
            }
            if ((src & 0xF0000000u) != 0u || !opsAllowed() || !appendOp(rdram, tags + 8, sizeof(tags) - 8u))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                original(rdram, ctx, runtime);
                return false;
            }
            ctx->pc = getRegU32(ctx, 31);
            return true;
        }

        PS2_GUEST_CC void texMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            unpack3Op(rdram, 0x6C03002Cu, getRegU32(ctx, 4), g_texMatrixOriginal, ctx, runtime);
        }

        PS2_GUEST_CC void vuConstantsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (unpack3Op(rdram, getRegU32(ctx, 4) | 0x6C030000u, getRegU32(ctx, 5), g_vuConstantsOriginal, ctx, runtime))
                setReturnS32(ctx, 3); // its return value
        }

        PS2_GUEST_CC void flushAHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t tag[2] = {0x10000000u, 0x1300000000000000ull}; // CNT 0: NOP, FLUSHA
            const auto *t = reinterpret_cast<const uint8_t *>(tag);
            if (g_itemVerify)
            {
                verifyOp(rdram, t, 16u);
                g_flushAOriginal(rdram, ctx, runtime);
                return;
            }
            if (!opsAllowed() || !appendOp(rdram, t + 8, 8u))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                g_flushAOriginal(rdram, ctx, runtime);
                return;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // before the original only: the material cache's texture bind opens the window its packets are copied from
        PS2_GUEST_CC void textureBindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_materialCaching = getRegU32(ctx, 31) == kMaterialCacheBind;
            g_textureBindOriginal(rdram, ctx, runtime);
        }

        // FUN_002409C8 called by FUN_00240ED8 to cache the sector: its draws stay packets (until the next RSL draw call
        // after it - the cached block's own draw, or the sector drawn again when caching failed: game-internals.md 6i)
        PS2_GUEST_CC void sectorHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_caching = getRegU32(ctx, 31) == kSectorCaching;
            g_sectorOriginal(rdram, ctx, runtime);
        }

        // before the original only (DECISIONS.md "Rule for hooks on recompiled functions"), or the whole work (an item)
        PS2_GUEST_CC void worldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
            g_materialCaching = false;
            if (g_items && g_world && !g_itemVerify && itemDraw(rdram, ctx, World, a0, a1))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, World, a0, a1);
            g_current.push_back({3u, a0 & kRamMask, a1 & 0xFFFFu, World});
            g_worldOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t a0 = getRegU32(ctx, 4);
            g_caching = false; // FUN_002409C8 draws no models: this is after the cached sector (FUN_00240ED8 draws it)
            if (g_items && !g_itemVerify && itemDraw(rdram, ctx, Model, a0, 0u))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, Model, a0, 0u);
            g_current.push_back({5u, a0 & kRamMask, 0u, Model});
            g_modelOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelBHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t a0 = getRegU32(ctx, 4);
            g_caching = false;
            if (g_items && !g_itemVerify && itemDraw(rdram, ctx, ModelB, a0, 0u))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, ModelB, a0, 0u);
            g_current.push_back({5u, a0 & kRamMask, 0u, ModelB});
            g_modelBOriginal(rdram, ctx, runtime);
        }

        uint32_t mark(const Draw &d)
        {
            if (g_building.draws.size() >= 0xFFFFu)
                return 0u;
            g_building.draws.push_back(d);
            return ps2chain::kReplaceWithMark | static_cast<uint32_t>(g_building.draws.size() - 1u);
        }

        // VCS_RSL_CENSUS=1 (docs/FULLY_NATIVE.md step 5): what the chain still carries outside marked / item blocks - each
        // top-level tag by DMA id, the VIF command it starts with and where its data is, tags and quadwords per chain
        const char *censusWhere(uint32_t a)
        {
            a &= kRamMask;
            if (a >= kPktLo && a < kPktHi)
                return "packet buffer";
            if (a >= 0x6EDC70u && a < 0xCB7A80u)
                return "cells / heap";
            if (a >= 0x1200000u && a < 0x1800000u)
                return "model heap";
            if (a >= 0x100000u && a < 0x4D0000u)
                return "code / data";
            return "other";
        }
        const char *vifName(uint32_t code)
        {
            const uint32_t cmd = (code >> 24) & 0x7Fu;
            if (cmd >= 0x60u)
                return "UNPACK";
            switch (cmd)
            {
            case 0x00: return "NOP";
            case 0x01: return "STCYCL";
            case 0x07: return "MARK";
            case 0x10: return "FLUSHE";
            case 0x11: return "FLUSH";
            case 0x13: return "FLUSHA";
            case 0x14: return "MSCAL";
            case 0x20: return "STMASK";
            case 0x30: return "STROW";
            case 0x4A: return "MPG";
            case 0x50: return "DIRECT";
            case 0x51: return "DIRECTHL";
            default: return "other";
            }
        }
        struct CensusRow
        {
            uint64_t tags = 0, qwords = 0;
        };
        std::map<std::string, CensusRow> g_census;
        uint64_t g_censusChains = 0;
        auto g_censusAt = std::chrono::steady_clock::now();

        void census(uint32_t tagAddr, uint32_t id, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes, bool chainStart)
        {
            if (chainStart)
            {
                ++g_censusChains;
                const auto now = std::chrono::steady_clock::now();
                if (now - g_censusAt > std::chrono::seconds(5) && g_censusChains > 1)
                {
                    g_censusAt = now;
                    const double c = double(g_censusChains - 1);
                    std::vector<std::pair<std::string, CensusRow>> rows(g_census.begin(), g_census.end());
                    std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) { return x.second.tags > y.second.tags; });
                    uint64_t tags = 0, qw = 0;
                    for (const auto &[k, r] : rows)
                        tags += r.tags, qw += r.qwords;
                    std::fprintf(stderr, "[rsl-census] per chain: %.0f top-level tags, %.0f payload qwords\n", tags / c, qw / c);
                    for (size_t i = 0; i < rows.size() && i < 24u; ++i)
                        std::fprintf(stderr, "[rsl-census] %8.1f tags %9.1f qw  %s\n", rows[i].second.tags / c, rows[i].second.qwords / c,
                                     rows[i].first.c_str());
                    g_census.clear();
                    g_censusChains = 1;
                }
            }
            if (g_openBlock.open)
                return; // inside a marked packet (counted as that block)
            static const char *const ids[8] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
            const uint32_t code0 = static_cast<uint32_t>(vifCodes), code1 = static_cast<uint32_t>(vifCodes >> 32);
            const uint32_t first = ((code0 >> 24) & 0x7Fu) != 0u ? code0 : code1;
            std::string key = std::string(ids[id & 7u]) + " " + vifName(first);
            if (((code0 >> 24) & 0x7Fu) == 0x07u && (code1 & 0xFFFF0000u) == kItemNop)
                key = "item";
            key += std::string(" | tag in ") + censusWhere(tagAddr);
            if (qwc)
                key += std::string(", data in ") + censusWhere(dataAddr);
            CensusRow &r = g_census[key];
            ++r.tags;
            r.qwords += qwc;
        }

        uint32_t observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                         uint32_t flatOffset, bool chainStart)
        {
            static const bool doCensus = std::getenv("VCS_RSL_CENSUS") != nullptr;
            if (doCensus)
                census(tagAddr, id, dataAddr, qwc, vifCodes, chainStart);
            if (chainStart)
            {
                g_building = ChainTable{};
                g_building.seq = ++g_chainSeq;
                g_openBlock.open = false;
                {
                    std::lock_guard<std::mutex> lock(g_confirmMutex); // the worker's confirmations since the last kick
                    for (const auto &[key, seq] : g_confirmPending)
                    {
                        uint64_t &c = g_confirmed[key];
                        c = std::max(c, seq);
                    }
                    g_confirmPending.clear();
                }
                if (!g_itemsKicked.empty()) // step 4c: the kicked frame's items (the fold has run: the walk starts after it)
                {
                    g_building.items = std::move(g_itemsKicked);
                    g_building.arena = std::move(g_arenaKicked);
                    g_building.itemFrame = g_itemFrameKicked;
                    g_itemsKicked.clear();
                    g_arenaKicked.clear();
                    resolveItems(g_building);
                }
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
                if (g_itemVerify && !g_openBlock.open) // what an item of this draw would have copied
                {
                    thread_local std::vector<uint8_t> flat;
                    thread_local std::vector<uint32_t> pages;
                    flat.clear();
                    pages.clear();
                    bool usable = true;
                    if (flattenBlock(g_rdram, d.addr, flat, pages, usable, nullptr))
                        d.flatHash = hashBlock(flat.data(), static_cast<uint32_t>(flat.size()));
                }
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
            if (g_building.draws.empty() && g_building.rasters.empty() && g_building.items.empty())
                return;
            auto table = std::make_shared<ChainTable>(std::move(g_building));
            table->chainBytes = sizeBytes;
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
                if (t_table && t_table.use_count() == 1 && (t_table->arena.capacity() || t_table->items.capacity()))
                {
                    std::lock_guard<std::mutex> lock(g_poolMutex); // step 4c: the arena and the item list for a later frame
                    if (g_arenaPool.size() < 4u)
                        g_arenaPool.push_back(std::move(t_table->arena));
                    if (g_itemPool.size() < 4u)
                        g_itemPool.push_back(std::move(t_table->items));
                }
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
        // step 4c: world / model draws as items (needs the native flush; not with the RSL debug tools, which read the draws'
        // packets)
        g_rdram = runtime.memory().getRDRAM();
        g_items = (g_world || g_models) && vcs::rslnative::enabled() && !off("VCS_RSL_ITEMS") && !std::getenv("VCS_RSL_SHADOW") &&
                  !std::getenv("VCS_TRACE_RSL");
        if (g_items && g_world && !hook(runtime, kSectorDraw, g_sectorOriginal, &sectorHook))
            g_items = false; // world items need to know when a sector is cached
        // step 5a: the state setters as ops (VCS_RSL_OPS=0: their packets)
        if (g_items && !off("VCS_RSL_OPS") && hook(runtime, kTextureBind, g_textureBindOriginal, &textureBindHook))
        {
            g_ops = true;
            hook(runtime, kGsSendA, g_gsSendOriginal[0], &gsSendHook<0>);
            hook(runtime, kGsSendB, g_gsSendOriginal[1], &gsSendHook<1>);
            hook(runtime, kTexMatrix, g_texMatrixOriginal, &texMatrixHook);
            hook(runtime, kVuConstants, g_vuConstantsOriginal, &vuConstantsHook);
            hook(runtime, kFlushA, g_flushAOriginal, &flushAHook);
        }
        g_itemVerify = g_items && std::getenv("VCS_RSL_ITEMS_VERIFY") != nullptr;
        g_log = g_verify || g_itemVerify || std::getenv("VCS_RSL_WORLD_LOG") != nullptr;
        ps2chain::addTagObserver(&observe);
        ps2chain::addChainObservers(&queued, &processing);
        ps2chain::setMarkHandler(&onMark);
        if (g_rasters)
            ps2chain::setDirectHandler(&onDirect);
    }

    bool flushAsOp(uint8_t *rdram, const uint8_t *tags, uint32_t bytes)
    {
        if (!g_ops || !opsAllowed())
        {
            if (g_ops)
                ++g_opStats.caching;
            return false;
        }
        thread_local std::vector<uint8_t> vif;
        vif.clear();
        tagsToVif(tags, bytes, vif);
        return vif.empty() || appendOp(rdram, vif.data(), static_cast<uint32_t>(vif.size()));
    }

    void onFrameKick()
    {
        if (!g_world && !g_models && !g_rasters)
            return;
        g_kicked.swap(g_current);
        g_current.clear();
        g_next = 0;
        if (g_itemVerify)
            checkItem(g_rdram);
        g_itemsKicked.swap(g_itemsCurrent); // step 4c (a frame whose chain was not observed loses its items with it)
        g_itemFrameKicked = g_itemFrame++;
        g_caching = false;
        g_materialCaching = false;
        g_openItem = -1;
        g_itemsCurrent.clear();
        g_arenaKicked.swap(g_arenaCurrent);
        g_arenaCurrent.clear();
        takePooled(g_arenaPool, g_arenaCurrent);
        takePooled(g_itemPool, g_itemsCurrent);
    }
}
