// [gta-recomp] Hardware GS renderer (D3D11 through the RHI). See runtime/gs/gs_hw.h.
#include "runtime/ps2_perf.h" // [gta-recomp]
#include "runtime/gs/gs_hw.h"
#include "runtime/gs/ps2_gs_memory.h" // [gta-recomp] RectBlocks (lazy uploads)
#include "runtime/gs/gs_cpu_backend.h"

#include "runtime/ps2_control.h" // [gta-recomp] keys: physical or injected

#include "render/gs_shaders.h" // [gta-recomp] D3D11 (RHI) execution
#include "rhi/rhi.h"
#include "runtime/ps2_hle_survey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::atomic<float> g_hudSqueeze{1.0f}; // [gta-recomp] gsHwSetHudSqueeze
}

void gsHwSetHudSqueeze(float factor) { g_hudSqueeze.store(factor, std::memory_order_relaxed); }

GSHwOptions GSHwOptions::fromEnvironment()
{
    GSHwOptions o;
    if (const char *s = std::getenv("PS2X_HW_SCALE"))
        o.scale = std::clamp<uint32_t>(static_cast<uint32_t>(std::atoi(s)), 1u, 8u);
    return o;
}

namespace
{
    // ------------------------------------------------------------------------------------------------------
    // Recorded data (game thread -> main thread)
    // ------------------------------------------------------------------------------------------------------
    struct HwVertex
    {
        float x, y, z;    // PS2 pixels in the target; depth 0..1
        float s, t, q;    // STQ, or U/V in texels with q = 1 (FST)
        uint8_t r, g, b, a;
        float fog;        // 0..1 (1 = no fog)
    };

    // Everything that selects GPU state for a draw. Compared bytewise to merge consecutive primitives.
    struct HwState
    {
        uint32_t fbp, fbw, fpsm, fbmsk;
        uint32_t zbp, zpsm;
        uint16_t scx0, scy0, scx1, scy1;
        uint8_t zte, ztst, zmsk, ate, atst, aref, afail, abe;
        uint8_t ba, bb, bc, bd, fix, tme, tfx, tcc;
        uint8_t fst, linear, wms, wmt, fge, fogR, fogG, fogB;
        uint8_t iip, mode, texIsRt, pad0;
        uint16_t tw, th, minu, maxu, minv, maxv;
        uint32_t rtFbp;
        uint16_t rtOx, rtOy;
        uint64_t texKey;
    };
    static_assert(std::is_trivially_copyable_v<HwState>);

    enum class CmdType : uint8_t
    {
        Draw,
        TexCreate,
        TexDelete,
        RtUpload,
        RtDrop,
        Clear,
        DepthFill,
        MeshCreate, // [gta-recomp] packet-level HLE (GSNativeDraw)
        MeshDelete,
        NativeDraw,
    };

    struct Cmd
    {
        CmdType type;
        uint32_t index; // into the frame's typed array
    };

    struct DrawCmd
    {
        HwState s;
        uint32_t first, count;
    };

    struct TexCreate
    {
        uint64_t key;
        uint32_t w, h;
        std::vector<uint32_t> pixels;
    };

    struct RtUpload
    {
        uint32_t fbp, fbw, x, y, w, h;
        std::vector<uint32_t> pixels;
    };

    struct ClearCmd
    {
        HwState s; // target + scissor
        uint32_t rgba;
    };

    // [gta-recomp] A flat colour draw into the memory of a depth buffer, as a depth clear of the rows it covers.
    struct DepthFillCmd
    {
        uint32_t zbp, width, y0, y1; // depth-buffer pixels
        float value;                 // 0..1, same scale as HwVertex::z
    };

    // [gta-recomp] static vertex data of a native draw, kept on the GPU by content
    struct MeshCreate
    {
        uint64_t key = 0;
        uint32_t stride = 0, count = 0;
        std::vector<uint8_t> bytes;
    };

    struct NativeDrawCmd
    {
        HwState s;
        uint64_t meshKey = 0;
        uint32_t kind = 0, drawCount = 0;
        bool transient = false;
        uint32_t transientOffset = 0; // bytes into the frame's nativeBytes
        float constants[128] = {};    // the kind's 124 + (XYOFFSET x, y in pixels, Z scale, 0)
    };

    struct Frame
    {
        std::vector<Cmd> cmds;
        std::vector<HwVertex> vertices;
        std::vector<DrawCmd> draws;
        std::vector<TexCreate> textures;
        std::vector<uint64_t> texDeletes;
        std::vector<RtUpload> rtUploads;
        std::vector<uint32_t> rtDrops;
        std::vector<ClearCmd> clears;
        std::vector<DepthFillCmd> depthFills;
        std::vector<MeshCreate> meshes;
        std::vector<uint64_t> meshDeletes;
        std::vector<NativeDrawCmd> nativeDraws;
        std::vector<uint8_t> nativeBytes; // transient native vertices
        uint64_t nativeVertices = 0;
        bool had3d = false; // [gta-recomp] any depth-tested or native draw (widescreen presentation)
        uint64_t privRegs[15]{};
        // Display from CPU VRAM when no render target holds the displayed buffer.
        std::vector<uint32_t> cpuDisplay;
        uint32_t cpuDisplayW = 0, cpuDisplayH = 0;
        bool displayFromRt = false;
        uint32_t displayFbp = 0, displayX = 0, displayY = 0, displayW = 0, displayH = 0;
    };

    // GS page geometry per PSM: 8 KiB pages.
    // [gta-recomp] PS2X_HW_LOG_DROPS, read once (it is tested per upload - thousands of times a second)
    bool logDrops()
    {
        static const bool on = std::getenv("PS2X_HW_LOG_DROPS") != nullptr;
        return on;
    }

    void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: w = 64; h = 64; break;
        case GS_PSM_T8: case GS_PSM_T8H: w = 128; h = 64; break;
        case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: w = 128; h = 128; break;
        default: w = 64; h = 32; break;
        }
        // T8H/T4HL/T4HH live in a CT32 layout
        if (psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
        {
            w = 64;
            h = 32;
        }
    }

    // Pages [first, first+count) touched by a w x h rect at (x,y) of a buffer at `blockBase` (256-byte blocks).
    void pageRange(uint32_t blockBase, uint32_t bufWidth64, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t &first, uint32_t &last)
    {
        uint32_t pw, ph;
        pageSize(psm, pw, ph);
        const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bufWidth64, 1u) * 64u + pw - 1u) / pw);
        const uint32_t basePage = blockBase / 32u;
        const uint32_t row0 = y / ph, row1 = (y + std::max<uint32_t>(h, 1u) - 1u) / ph;
        (void)x;
        (void)w;
        first = basePage + row0 * perRow;
        last = basePage + (row1 + 1u) * perRow + ((blockBase % 32u) ? 1u : 0u); // exclusive
    }

    uint64_t mix64(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xFF51AFD7ED558CCDull;
    }

    // ------------------------------------------------------------------------------------------------------
    // [gta-recomp] Execution on the renderer's RHI device (main thread). Render targets keep PS2 rows top-down
    // (texture row = PS2 row); the vertex shader flips Y for D3D's NDC.
    // ------------------------------------------------------------------------------------------------------
    struct DrawConstants // render/shaders/gs_draw.hlsl, cbuffer Draw
    {
        float targetRtFix[4];
        int32_t tmeTfxTccFst[4];
        int32_t linearIipFgeAte[4];
        int32_t atstArefAfailRt[4];
        int32_t texSizeWrap[4];
        int32_t region[4];
        float rtOrigin[4];
        int32_t blendCPremul[4];
        float fogColor[4];
    };

    struct QuadConstants // render/shaders/gs_util.hlsl, cbuffer Quad
    {
        float rect[4];
        float uvRect[4];
        float color[4];
        float params[4];
    };

    // GS blend (A - B) * C / 128 + D as a fixed-function blend (C from the shader's second output); false when not
    // expressible (drawn unblended).
    struct GsBlend
    {
        bool enable = false;
        rhi::BlendFactor src = rhi::BlendFactor::One;
        rhi::BlendFactor dst = rhi::BlendFactor::Zero;
        rhi::BlendOp op = rhi::BlendOp::Add;
        int premul = 0; // 0: none, 1: rgb *= C, 2: rgb *= 1 + C
    };

    bool gsBlend(const HwState &s, GsBlend &b)
    {
        using F = rhi::BlendFactor;
        using O = rhi::BlendOp;
        b = GsBlend{};
        if (!s.abe)
            return true;
        const int A = s.ba, B = s.bb, D = s.bd;
        O op = O::Add;
        F src = F::One, dst = F::Zero;
        if (A == B)
        {
            // (X - X) * C + D = D
            if (D == 0) { src = F::One; dst = F::Zero; }
            else if (D == 1) { src = F::Zero; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 0 && B == 1)
        {
            if (D == 1) { src = F::Src1Alpha; dst = F::InvSrc1Alpha; }                          // Cs*C + Cd*(1-C)
            else if (D == 0) { b.premul = 2; op = O::Subtract; src = F::One; dst = F::Src1Alpha; } // Cs*(1+C) - Cd*C
            else { src = F::Src1Alpha; dst = F::Src1Alpha; op = O::Subtract; }                 // (Cs - Cd)*C
        }
        else if (A == 0 && B == 2)
        {
            b.premul = 1; // Cs*C + D
            src = F::One;
            dst = (D == 1) ? F::One : F::Zero;
            if (D == 0) { b.premul = 2; src = F::One; dst = F::Zero; }
        }
        else if (A == 1 && B == 0)
        {
            if (D == 0) { src = F::InvSrc1Alpha; dst = F::Src1Alpha; } // Cs*(1-C) + Cd*C
            else return false;
        }
        else if (A == 2 && B == 0)
        {
            b.premul = 1; // D - Cs*C
            if (D == 1) { op = O::ReverseSubtract; src = F::One; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 2 && B == 1)
        {
            if (D == 1) { src = F::Zero; dst = F::InvSrc1Alpha; }                               // Cd*(1-C)
            else if (D == 0) { op = O::ReverseSubtract; src = F::One; dst = F::Src1Alpha; }       // Cs - Cd*C
            else return false;
        }
        else if (A == 1 && B == 2)
        {
            if (D == 0) { src = F::One; dst = F::Src1Alpha; }  // Cs + Cd*C
            else if (D == 2) { src = F::Zero; dst = F::Src1Alpha; } // Cd*C
            else return false;
        }
        else
        {
            return false;
        }
        b.enable = true;
        b.src = src;
        b.dst = dst;
        b.op = op;
        return true;
    }

    // F12 / PS2X_HW_DUMP: one line per draw of the dumped frame (state + bounds).
    void logDraw(const Frame &f, const DrawCmd &d)
    {
        const HwState &s = d.s;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        uint32_t rgbaOr = 0, fogMin = 255;
        for (uint32_t i = 0; i < d.count; ++i)
        {
            const HwVertex &v = f.vertices[d.first + i];
            x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
            z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
            rgbaOr |= v.r | (v.g << 8) | (v.b << 16) | (uint32_t(v.a) << 24);
            fogMin = std::min<uint32_t>(fogMin, static_cast<uint32_t>(v.fog * 255.0f));
        }
        std::fprintf(stderr,
                     "[draw] n=%u fbp=%u zbp=%u zte=%u ztst=%u zmsk=%u tme=%u tex(psm? key=%llx %ux%u rt=%u) tfx=%u tcc=%u abe=%u blend=%u%u%u%u fix=%u ate=%u atst=%u aref=%u afail=%u fge=%u fogMin=%u fogCol=%u,%u,%u fbmsk=%x prim=%u iip=%u fst=%u bounds=(%.0f,%.0f)-(%.0f,%.0f) z=%.5f..%.5f rgbaOr=%08x\n",
                     d.count, s.fbp, s.zbp, s.zte, s.ztst, s.zmsk, s.tme, (unsigned long long)s.texKey, s.tw, s.th, s.texIsRt,
                     s.tfx, s.tcc, s.abe, s.ba, s.bb, s.bc, s.bd, s.fix, s.ate, s.atst, s.aref, s.afail, s.fge, fogMin,
                     s.fogR, s.fogG, s.fogB, s.fbmsk, s.mode, s.iip, s.fst, x0, y0, x1, y1, z0, z1, rgbaOr);
        if (s.texIsRt) // [gta-recomp] which render target a frame copy / screen effect reads, and where in it
            std::fprintf(stderr, "[draw]   src rt fbp=%u at %u,%u tex %ux%u tbw? st=(%.1f,%.1f)\n", s.rtFbp, s.rtOx, s.rtOy, s.tw, s.th,
                         d.count ? f.vertices[d.first].s : 0.0f, d.count ? f.vertices[d.first].t : 0.0f);
        if (d.count <= 24 || s.texIsRt) // [gta-recomp] small draws (overlays, fades) and copies: their vertices
            for (uint32_t i = 0; i < std::min<uint32_t>(d.count, 24u); ++i)
            {
                const HwVertex &v = f.vertices[d.first + i];
                std::fprintf(stderr, "[draw]   v%u (%.1f, %.1f, %.6f) rgba %02x %02x %02x %02x stq %.2f %.2f %.3f\n", i, v.x, v.y, v.z,
                             v.r, v.g, v.b, v.a, v.s, v.t, v.q);
            }
    }

    void writePpm(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h)
    {
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6 %u %u 255 ", w, h);
            for (size_t i = 0; i < size_t(w) * h; ++i)
                std::fwrite(&rgba[i * 4u], 1, 3, f);
            std::fclose(f);
            std::fprintf(stderr, "[gs-hw] dumped %s (%ux%u)\n", path.c_str(), w, h);
        }
    }

    class RhiExecutor
    {
    public:
        RhiExecutor(rhi::Device &device, uint32_t scale) : m_dev(device), m_scale(scale)
        {
            const render::GsShaders &sh = render::gsShaders();
            m_drawCb = m_dev.createBuffer({sizeof(DrawConstants), rhi::BufferUsage::Constant, true, "gs draw constants"});
            m_quadCb = m_dev.createBuffer({sizeof(QuadConstants), rhi::BufferUsage::Constant, true, "gs quad constants"});
            m_worldCb = m_dev.createBuffer({128u * sizeof(float), rhi::BufferUsage::Constant, true, "gs native draw constants"});
            m_point = m_dev.createSampler({rhi::Filter::Nearest, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
            m_linear = m_dev.createSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
            rhi::PipelineDesc q;
            q.vertex = sh.quadVS;
            q.topology = rhi::Topology::TriangleStrip;
            q.pixel = sh.blitPS;
            q.name = "gs blit";
            m_blit = m_dev.createPipeline(q);
            q.pixel = sh.fillColorPS;
            q.name = "gs fill colour";
            m_fillColor = m_dev.createPipeline(q);
            q.pixel = sh.fillDepthPS;
            q.blend.writeMask = 0;
            q.depth = {true, true, rhi::Compare::Always};
            q.name = "gs fill depth";
            m_fillDepth = m_dev.createPipeline(q);
            m_ready = m_drawCb && m_quadCb && m_worldCb && m_point && m_linear && m_blit && m_fillColor && m_fillDepth;
        }

        bool ready() const { return m_ready; }
        rhi::Device &device() { return m_dev; }
        rhi::Texture *display() const { return m_display.get(); }
        uint32_t displayWidth() const { return m_displayW * m_scale; }
        uint32_t displayHeight() const { return m_displayH * m_scale; }

        void execute(const Frame &f, bool logDraws)
        {
            m_dev.pushMarker("gs frame");
            const size_t bytes = f.vertices.size() * sizeof(HwVertex);
            if (bytes)
            {
                if (!m_vb || m_vbCapacity < bytes)
                {
                    m_vbCapacity = std::max<size_t>(bytes + bytes / 2, 1u << 20);
                    m_vb = m_dev.createBuffer({m_vbCapacity, rhi::BufferUsage::Vertex, true, "gs vertices"});
                }
                m_dev.updateBuffer(*m_vb, f.vertices.data(), bytes);
            }
            if (!f.nativeBytes.empty())
            {
                if (!m_nativeVb || m_nativeVbCapacity < f.nativeBytes.size())
                {
                    m_nativeVbCapacity = std::max<size_t>(f.nativeBytes.size() + f.nativeBytes.size() / 2, 1u << 20);
                    m_nativeVb = m_dev.createBuffer({m_nativeVbCapacity, rhi::BufferUsage::Vertex, true, "gs native transient vertices"});
                }
                m_dev.updateBuffer(*m_nativeVb, f.nativeBytes.data(), f.nativeBytes.size());
            }
            for (const Cmd &c : f.cmds)
            {
                switch (c.type)
                {
                case CmdType::TexCreate:
                {
                    const TexCreate &t = f.textures[c.index];
                    rhi::TextureDesc d;
                    d.width = t.w;
                    d.height = t.h;
                    d.usage = rhi::UsageSampled;
                    d.name = "gs texture";
                    m_textures[t.key] = m_dev.createTexture(d, t.pixels.data(), t.w * 4u);
                    break;
                }
                case CmdType::TexDelete:
                    m_textures.erase(f.texDeletes[c.index]);
                    break;
                case CmdType::RtDrop:
                    endPass();
                    m_rts.erase(f.rtDrops[c.index]);
                    break;
                case CmdType::RtUpload:
                    rtUpload(f.rtUploads[c.index]);
                    break;
                case CmdType::Clear:
                    clear(f.clears[c.index]);
                    break;
                case CmdType::DepthFill:
                    depthFill(f.depthFills[c.index], logDraws);
                    break;
                case CmdType::Draw:
                {
                    if (logDraws)
                        logDraw(f, f.draws[c.index]);
                    // [gta-recomp] PS2X_HW_SKIP=<mask> (diagnostics): 1 skips draws that sample a render target (frame
                    // copies, screen effects), 2 skips untextured full-screen draws, 4 skips everything but native draws
                    static const unsigned skip = std::getenv("PS2X_HW_SKIP") ? std::strtoul(std::getenv("PS2X_HW_SKIP"), nullptr, 0) : 0u;
                    const HwState &ds = f.draws[c.index].s;
                    if (skip && (((skip & 1u) && ds.texIsRt) || (skip & 4u)))
                        break;
                    draw(ds, f.draws[c.index].first, f.draws[c.index].count, nullptr);
                    // [gta-recomp] PS2X_HW_DUMP_STEPS=1: while draws are logged, write the target after every draw that
                    // covers the whole screen or reads a render target (where a frame goes black, step by step)
                    static const bool steps = std::getenv("PS2X_HW_DUMP_STEPS") != nullptr;
                    if (steps && logDraws)
                    {
                        const DrawCmd &dc = f.draws[c.index];
                        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                        for (uint32_t i = 0; i < dc.count; ++i)
                        {
                            const HwVertex &v = f.vertices[dc.first + i];
                            x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                        }
                        const bool full = x0 <= 1.0f && x1 >= 639.0f && y0 <= 1.0f && y1 >= 447.0f;
                        auto it = m_rts.find(ds.fbp);
                        if ((full || ds.texIsRt) && it != m_rts.end())
                        {
                            endPass();
                            static int step = 0;
                            dumpTexture(*it->second.color, "step" + std::to_string(step++) + "_fbp" + std::to_string(ds.fbp));
                        }
                    }
                    break;
                }
                case CmdType::MeshCreate:
                {
                    const MeshCreate &m = f.meshes[c.index];
                    m_meshes[m.key] = m_dev.createBuffer({m.bytes.size(), rhi::BufferUsage::Vertex, false, "gs native mesh"}, m.bytes.data());
                    break;
                }
                case CmdType::MeshDelete:
                    m_meshes.erase(f.meshDeletes[c.index]);
                    break;
                case CmdType::NativeDraw:
                {
                    const NativeDrawCmd &n = f.nativeDraws[c.index];
                    if (logDraws)
                        std::fprintf(stderr, "[draw] native kind=%u n=%u mesh=%016llx fbp=%u zbp=%u tme=%u abe=%u fge=%u\n", n.kind, n.drawCount,
                                     (unsigned long long)n.meshKey, n.s.fbp, n.s.zbp, n.s.tme, n.s.abe, n.s.fge);
                    draw(n.s, 0, n.drawCount, &n);
                    break;
                }
                }
            }
            endPass();
            m_dev.popMarker();
        }

        void composeDisplay(const Frame &f)
        {
            m_displayW = f.displayW;
            m_displayH = f.displayH;
            const uint32_t w = m_displayW * m_scale, h = m_displayH * m_scale;
            if (!w || !h)
                return;
            if (!m_display || m_display->desc().width != w || m_display->desc().height != h)
            {
                endPass();
                m_display = m_dev.createTexture({w, h, rhi::Format::RGBA8, rhi::UsageRenderTarget | rhi::UsageSampled, "gs display"});
            }
            auto it = m_rts.find(f.displayFbp);
            if (f.displayFromRt && it != m_rts.end())
            {
                // the PS2 frame buffer's alpha is not shown: written as 1
                const Rt &rt = it->second;
                const float tw = static_cast<float>(rt.color->desc().width), th = static_cast<float>(rt.color->desc().height);
                const float x0 = static_cast<float>(f.displayX * m_scale), y0 = static_cast<float>(f.displayY * m_scale);
                blit(*rt.color, {x0 / tw, y0 / th, (x0 + w) / tw, (y0 + h) / th}, *m_display, 0, 0, w, h, false, true);
            }
            else if (!f.cpuDisplay.empty())
            {
                static int logged = 0; // [gta-recomp] normal for uploaded images (loading screens), suspicious in play
                if (logged < 50 && f.had3d)
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] 3D frame displayed from CPU VRAM: fbp=%u fromRt=%d; targets:%s\n", f.displayFbp,
                                 f.displayFromRt ? 1 : 0, describeTargets().c_str());
                }
                auto tmp = m_dev.createTexture({f.cpuDisplayW, f.cpuDisplayH, rhi::Format::RGBA8, rhi::UsageSampled, "gs cpu display"},
                                               f.cpuDisplay.data(), f.cpuDisplayW * 4u);
                if (tmp)
                    blit(*tmp, {0.0f, 0.0f, 1.0f, 1.0f}, *m_display, 0, 0, w, h, true, true);
            }
            else
            {
                // [gta-recomp] nothing to show: a black frame. Logged - a candidate for the black-flash bug
                static int logged = 0;
                if (logged < 50)
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] display filled black: fbp=%u fromRt=%d %ux%u at %u,%u; targets:%s\n", f.displayFbp,
                                 f.displayFromRt ? 1 : 0, f.displayW, f.displayH, f.displayX, f.displayY, describeTargets().c_str());
                }
                fill(m_display.get(), nullptr, 0, 0, w, h, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
            }
            endPass();
        }

        // F12 / PS2X_HW_DUMP: render targets and the display as PPM files.
        void dump()
        {
            endPass();
            std::vector<uint8_t> px;
            for (const auto &[fbp, rt] : m_rts)
                dumpTexture(*rt.color, "rt_" + std::to_string(fbp));
            if (m_display)
                dumpTexture(*m_display, "display");
        }

        std::string describeTargets() const
        {
            std::string s;
            for (const auto &[fbp, rt] : m_rts)
                s += " " + std::to_string(fbp) + "(" + std::to_string(rt.w) + "x" + std::to_string(rt.h) + ")";
            return s;
        }

    private:
        struct Rt
        {
            std::unique_ptr<rhi::Texture> color;
            uint32_t w = 0, h = 0; // PS2 pixels
        };
        struct Depth
        {
            std::unique_ptr<rhi::Texture> tex;
            uint32_t w = 0, h = 0; // PS2 pixels
        };

        void dumpTexture(rhi::Texture &t, const std::string &name)
        {
            const uint32_t w = t.desc().width, h = t.desc().height;
            std::vector<uint8_t> px(size_t(w) * h * 4u);
            if (m_dev.readTexture(t, 0, 0, w, h, px.data()))
                writePpm("hw_" + name + ".ppm", px.data(), w, h);
        }

        void endPass()
        {
            if (m_inPass)
                m_dev.endPass();
            m_inPass = false;
            m_passColor = m_passDepth = nullptr;
        }

        // color null + depth: depth only
        void bindPass(rhi::Texture *color, rhi::Texture *depth)
        {
            if (m_inPass && color == m_passColor && depth == m_passDepth)
                return;
            endPass();
            rhi::PassDesc p;
            p.color = color;
            p.depth = depth;
            p.depthOnly = color == nullptr;
            m_dev.beginPass(p);
            m_inPass = true;
            m_passColor = color;
            m_passDepth = depth;
        }

        static void ndcRect(float x, float y, float w, float h, float targetW, float targetH, float out[4])
        {
            out[0] = x / targetW * 2.0f - 1.0f;
            out[1] = 1.0f - y / targetH * 2.0f;
            out[2] = (x + w) / targetW * 2.0f - 1.0f;
            out[3] = 1.0f - (y + h) / targetH * 2.0f;
        }

        // src (uv rect, 0..1) -> dst pixel rect, point or bilinear; opaque: alpha written as 1
        void blit(rhi::Texture &src, std::array<float, 4> uv, rhi::Texture &dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  bool linear, bool opaque)
        {
            bindPass(&dst, nullptr);
            QuadConstants q{};
            ndcRect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h),
                    static_cast<float>(dst.desc().width), static_cast<float>(dst.desc().height), q.rect);
            std::memcpy(q.uvRect, uv.data(), sizeof(q.uvRect));
            q.params[1] = opaque ? 1.0f : 0.0f;
            m_dev.setScissor(0, 0, static_cast<int>(dst.desc().width), static_cast<int>(dst.desc().height));
            m_dev.setPipeline(*m_blit);
            m_dev.updateBuffer(*m_quadCb, &q, sizeof(q));
            m_dev.setConstants(0, *m_quadCb);
            m_dev.setTexture(0, &src, linear ? m_linear.get() : m_point.get());
            m_dev.draw(4);
            m_dev.setTexture(0, nullptr, nullptr);
        }

        // solid colour into `color`, or a depth value into `depth` (depth-only), inside a pixel rect
        void fill(rhi::Texture *color, rhi::Texture *depth, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::array<float, 4> rgba,
                  float depthValue)
        {
            rhi::Texture *target = color ? color : depth;
            bindPass(color, color ? nullptr : depth);
            const int tw = static_cast<int>(target->desc().width), th = static_cast<int>(target->desc().height);
            const int x0 = std::min<int>(static_cast<int>(x), tw), y0 = std::min<int>(static_cast<int>(y), th);
            m_dev.setScissor(x0, y0, std::min<int>(static_cast<int>(w), tw - x0), std::min<int>(static_cast<int>(h), th - y0));
            QuadConstants q{};
            q.rect[0] = -1.0f; q.rect[1] = 1.0f; q.rect[2] = 1.0f; q.rect[3] = -1.0f;
            std::memcpy(q.color, rgba.data(), sizeof(q.color));
            q.params[0] = depthValue;
            m_dev.setPipeline(color ? *m_fillColor : *m_fillDepth);
            m_dev.updateBuffer(*m_quadCb, &q, sizeof(q));
            m_dev.setConstants(0, *m_quadCb);
            m_dev.draw(4);
        }

        std::unique_ptr<rhi::Texture> makeTarget(uint32_t w, uint32_t h, bool depth)
        {
            rhi::TextureDesc d;
            d.width = w * m_scale;
            d.height = h * m_scale;
            d.format = depth ? rhi::Format::D32F : rhi::Format::RGBA8;
            d.usage = depth ? rhi::UsageDepth : (rhi::UsageRenderTarget | rhi::UsageSampled);
            d.name = depth ? "gs depth" : "gs target";
            auto t = m_dev.createTexture(d);
            endPass();
            rhi::PassDesc p;
            p.name = "gs target clear";
            if (depth)
            {
                p.depth = t.get();
                p.depthOnly = true;
                p.clearDepth = true;
                p.clearDepthValue = 0.0f;
            }
            else
            {
                p.color = t.get();
                p.clearColor = true;
                p.clearRgba[3] = 0.0f;
            }
            m_dev.beginPass(p);
            m_dev.endPass();
            return t;
        }

        // grows the target to at least w x h (PS2 pixels), keeping its contents
        void growRt(Rt &rt, uint32_t w, uint32_t h)
        {
            if (rt.color && rt.w >= w && rt.h >= h)
                return;
            Rt grown;
            grown.w = std::max(rt.w, w);
            grown.h = std::max(rt.h, h);
            grown.color = makeTarget(grown.w, grown.h, false);
            if (rt.color)
                m_dev.copyTexture(*grown.color, 0, 0, *rt.color, 0, 0, rt.w * m_scale, rt.h * m_scale);
            rt = std::move(grown);
        }

        Rt &rtFor(uint32_t fbp, uint32_t fbw, uint32_t minH)
        {
            Rt &rt = m_rts[fbp];
            growRt(rt, std::max<uint32_t>(fbw, 1u) * 64u, std::max<uint32_t>(minH, 512u));
            return rt;
        }

        // grows (recreated, cleared to 0) to at least w x h
        Depth &depthFor(uint32_t zbp, uint32_t w, uint32_t h)
        {
            Depth &d = m_depth[zbp];
            if (d.tex && d.w >= w && d.h >= h)
                return d;
            d.w = std::max(d.w, w);
            d.h = std::max(d.h, h);
            d.tex = makeTarget(d.w, d.h, true);
            return d;
        }

        void rtUpload(const RtUpload &u)
        {
            Rt &rt = rtFor(u.fbp, u.fbw, u.y + u.h);
            // uploaded at 1x, then drawn into the target at its scale
            auto tmp = m_dev.createTexture({u.w, u.h, rhi::Format::RGBA8, rhi::UsageSampled, "gs rt upload"}, u.pixels.data(), u.w * 4u);
            if (tmp)
                blit(*tmp, {0.0f, 0.0f, 1.0f, 1.0f}, *rt.color, u.x * m_scale, u.y * m_scale, u.w * m_scale, u.h * m_scale, false, false);
        }

        void clear(const ClearCmd &cl)
        {
            Rt &rt = rtFor(cl.s.fbp, cl.s.fbw, cl.s.scy1 + 1u);
            const std::array<float, 4> rgba = {(cl.rgba & 0xFF) / 255.0f, ((cl.rgba >> 8) & 0xFF) / 255.0f, ((cl.rgba >> 16) & 0xFF) / 255.0f,
                                               ((cl.rgba >> 24) & 0xFF) / 255.0f};
            fill(rt.color.get(), nullptr, cl.s.scx0 * m_scale, cl.s.scy0 * m_scale, (cl.s.scx1 - cl.s.scx0 + 1u) * m_scale,
                 (cl.s.scy1 - cl.s.scy0 + 1u) * m_scale, rgba, 0.0f);
        }

        void depthFill(const DepthFillCmd &f, bool log)
        {
            const auto it = m_depth.find(f.zbp);
            if (it == m_depth.end() || !it->second.tex)
                return; // created (cleared) on first use
            fill(nullptr, it->second.tex.get(), 0, f.y0 * m_scale, f.width * m_scale, (f.y1 - f.y0) * m_scale, {0, 0, 0, 0}, f.value);
            if (log)
                std::fprintf(stderr, "[draw] depth fill zbp=%u rows %u..%u width %u value %.6f\n", f.zbp, f.y0, f.y1, f.width, f.value);
        }

        rhi::Pipeline &drawPipeline(const GsBlend &b, uint8_t mask, bool depthTest, uint32_t ztst, bool zwrite, uint8_t mode)
        {
            const bool dual = b.enable && (b.src == rhi::BlendFactor::Src1Alpha || b.src == rhi::BlendFactor::InvSrc1Alpha ||
                                           b.dst == rhi::BlendFactor::Src1Alpha || b.dst == rhi::BlendFactor::InvSrc1Alpha);
            const uint32_t key = (b.enable ? 1u : 0u) | (static_cast<uint32_t>(b.src) << 1) | (static_cast<uint32_t>(b.dst) << 5) |
                                 (static_cast<uint32_t>(b.op) << 9) | (uint32_t(mask & 0xF) << 12) | ((depthTest ? 1u : 0u) << 16) |
                                 ((ztst & 3u) << 17) | ((zwrite ? 1u : 0u) << 19) | (uint32_t(mode & 7u) << 20) | ((dual ? 1u : 0u) << 23);
            std::unique_ptr<rhi::Pipeline> &p = m_drawPipelines[key];
            if (!p)
            {
                const render::GsShaders &sh = render::gsShaders();
                rhi::PipelineDesc d;
                d.pixel = dual ? sh.drawPSDual : sh.drawPS;
                if (mode == 4)
                {
                    // [gta-recomp] native VCS lit model (GSNativeDraw::VcsLit): skinned on the CPU, lit / transformed here
                    d.vertex = sh.litVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, 0},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, 12},
                        {"NORMAL", 0, rhi::VertexFormat::Float3, 24},
                    };
                    d.stride = 48;
                    d.topology = rhi::Topology::TriangleStrip;
                    d.depthClip = false;
                }
                else if (mode == 3)
                {
                    // [gta-recomp] native VCS world draw (GSNativeDraw::VcsWorld): 32-byte vertices, transformed on the GPU
                    d.vertex = sh.worldVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, 0},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, 12},
                        {"COLOR", 0, rhi::VertexFormat::UInt1, 24},
                    };
                    d.stride = 32;
                    d.topology = rhi::Topology::TriangleStrip;
                    d.depthClip = false; // the PS2 clip planes come as clip distances
                }
                else
                {
                    d.vertex = sh.drawVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(HwVertex, x))},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(HwVertex, s))},
                        {"COLOR", 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(HwVertex, r))},
                        {"TEXCOORD", 1, rhi::VertexFormat::Float1, static_cast<uint32_t>(offsetof(HwVertex, fog))},
                    };
                    d.stride = sizeof(HwVertex);
                    d.topology = mode == 1 ? rhi::Topology::Lines : mode == 2 ? rhi::Topology::Points : rhi::Topology::Triangles;
                }
                d.blend.enable = b.enable;
                d.blend.srcColor = b.src;
                d.blend.dstColor = b.dst;
                d.blend.colorOp = b.op;
                d.blend.srcAlpha = rhi::BlendFactor::One;
                d.blend.dstAlpha = rhi::BlendFactor::Zero;
                d.blend.writeMask = mask;
                d.depth.test = depthTest;
                d.depth.write = depthTest && zwrite;
                d.depth.func = ztst == 1 ? rhi::Compare::Always : ztst == 2 ? rhi::Compare::GreaterEqual : rhi::Compare::Greater;
                d.name = "gs draw";
                p = m_dev.createPipeline(d);
            }
            return *p;
        }

        void draw(const HwState &s, uint32_t first, uint32_t count, const NativeDrawCmd *native)
        {
            rhi::Buffer *vertices = m_vb.get();
            uint32_t vertexOffset = 0;
            if (native && native->transient)
            {
                vertices = m_nativeVb.get();
                vertexOffset = native->transientOffset;
                if (vertices)
                    m_dev.updateBuffer(*m_worldCb, native->constants, sizeof(native->constants));
            }
            else if (native)
            {
                auto mesh = m_meshes.find(native->meshKey);
                vertices = mesh != m_meshes.end() ? mesh->second.get() : nullptr;
                if (vertices)
                    m_dev.updateBuffer(*m_worldCb, native->constants, sizeof(native->constants));
            }
            if (!count || !vertices)
                return;
            Rt &rt = rtFor(s.fbp, s.fbw, s.scy1 + 1u);
            const bool depthOn = s.zte != 0u;
            Depth *depth = nullptr;
            if (depthOn)
            {
                // D3D binds a depth buffer only with a colour target of the same size: grow whichever is smaller
                depth = &depthFor(s.zbp, rt.w, rt.h);
                growRt(rt, depth->w, depth->h);
            }

            // texture: decoded VRAM, another render target, or this target itself (sampled from a copy)
            rhi::Texture *tex = nullptr;
            float rtScale = 1.0f;
            if (s.tme)
            {
                if (s.texIsRt)
                {
                    auto it = m_rts.find(s.rtFbp);
                    if (it != m_rts.end())
                    {
                        tex = it->second.color.get();
                        if (s.rtFbp == s.fbp)
                            tex = feedbackCopy(*tex);
                    }
                    rtScale = static_cast<float>(m_scale);
                }
                else
                {
                    auto it = m_textures.find(s.texKey);
                    tex = it != m_textures.end() ? it->second.get() : nullptr;
                }
            }

            bindPass(rt.color.get(), depth ? depth->tex.get() : nullptr);
            m_dev.setScissor(s.scx0 * m_scale, s.scy0 * m_scale, (s.scx1 - s.scx0 + 1) * m_scale, (s.scy1 - s.scy0 + 1) * m_scale);

            DrawConstants c{};
            c.targetRtFix[0] = static_cast<float>(rt.w);
            c.targetRtFix[1] = static_cast<float>(rt.h);
            c.targetRtFix[2] = rtScale;
            c.targetRtFix[3] = s.fix;
            c.tmeTfxTccFst[0] = s.tme;
            c.tmeTfxTccFst[1] = s.tfx;
            c.tmeTfxTccFst[2] = s.tcc;
            c.tmeTfxTccFst[3] = s.fst;
            c.linearIipFgeAte[0] = s.linear;
            c.linearIipFgeAte[1] = s.iip;
            c.linearIipFgeAte[2] = s.fge;
            c.linearIipFgeAte[3] = s.ate;
            c.atstArefAfailRt[0] = s.atst;
            c.atstArefAfailRt[1] = s.aref;
            c.atstArefAfailRt[2] = 0;
            c.atstArefAfailRt[3] = s.texIsRt;
            c.texSizeWrap[0] = s.tw;
            c.texSizeWrap[1] = s.th;
            c.texSizeWrap[2] = s.wms;
            c.texSizeWrap[3] = s.wmt;
            c.region[0] = s.minu;
            c.region[1] = s.maxu;
            c.region[2] = s.minv;
            c.region[3] = s.maxv;
            c.rtOrigin[0] = s.rtOx;
            c.rtOrigin[1] = s.rtOy;
            c.fogColor[0] = s.fogR;
            c.fogColor[1] = s.fogG;
            c.fogColor[2] = s.fogB;

            GsBlend b;
            if (!gsBlend(s, b))
            {
                b.enable = false;
                if (m_reportedBlend.insert(s.ba | (s.bb << 2) | (s.bd << 6)).second)
                    std::fprintf(stderr, "[gs-hw] unsupported blend A=%u B=%u C=%u D=%u (drawn opaque)\n", s.ba, s.bb, s.bc, s.bd);
            }
            c.blendCPremul[0] = s.bc;
            c.blendCPremul[1] = b.premul;

            // frame write mask (whole channels only)
            const uint8_t mr = (s.fbmsk & 0xFFu) != 0xFFu, mg = (s.fbmsk & 0xFF00u) != 0xFF00u, mb = (s.fbmsk & 0xFF0000u) != 0xFF0000u,
                          ma = (s.fbmsk & 0xFF000000u) != 0xFF000000u;
            uint8_t mask = static_cast<uint8_t>(mr | (mg << 1) | (mb << 2) | (ma << 3));
            bool zwrite = depthOn && !s.zmsk;

            // pass 1: pixels passing the alpha test (or all of them)
            auto submit = [&](rhi::Pipeline &pipeline)
            {
                m_dev.setPipeline(pipeline);
                m_dev.setVertexBuffer(*vertices, vertexOffset);
                m_dev.updateBuffer(*m_drawCb, &c, sizeof(c));
                m_dev.setConstants(0, *m_drawCb);
                if (native)
                    m_dev.setConstants(1, *m_worldCb);
                m_dev.setTexture(0, tex, nullptr);
                m_dev.draw(count, first);
            };
            if (s.ate && s.atst != 1u && s.afail != 0u)
            {
                // AFAIL other than KEEP: draw the passing pixels normally, the failing ones with the AFAIL mask
                submit(drawPipeline(b, mask, depthOn, s.ztst, zwrite, s.mode));
                c.atstArefAfailRt[2] = 1;
                if (s.afail == 1u) zwrite = false;                      // FB_ONLY
                else if (s.afail == 2u) mask = 0;                       // ZB_ONLY
                else { mask = static_cast<uint8_t>(mask & 7u); zwrite = false; } // RGB_ONLY
            }
            else if (!(s.ate && s.atst != 1u))
            {
                c.linearIipFgeAte[3] = 0; // no alpha test (ATST ALWAYS)
            }
            submit(drawPipeline(b, mask, depthOn, s.ztst, zwrite, s.mode));
        }

        // a draw that samples its own target reads a copy (D3D cannot sample a bound render target)
        rhi::Texture *feedbackCopy(rhi::Texture &target)
        {
            const rhi::TextureDesc &td = target.desc();
            if (!m_feedback || m_feedback->desc().width != td.width || m_feedback->desc().height != td.height)
                m_feedback = m_dev.createTexture({td.width, td.height, rhi::Format::RGBA8, rhi::UsageSampled, "gs feedback copy"});
            m_dev.copyTexture(*m_feedback, 0, 0, target, 0, 0, td.width, td.height);
            if (m_feedbackReported++ == 0)
                std::fprintf(stderr, "[gs-hw] a draw samples its own render target: reading a copy\n");
            return m_feedback.get();
        }

        rhi::Device &m_dev;
        uint32_t m_scale;
        bool m_ready = false;
        std::unique_ptr<rhi::Buffer> m_vb, m_drawCb, m_quadCb, m_worldCb;
        std::unordered_map<uint64_t, std::unique_ptr<rhi::Buffer>> m_meshes; // native draws' vertex data
        std::unique_ptr<rhi::Buffer> m_nativeVb; // transient native vertices of the frame
        size_t m_nativeVbCapacity = 0;
        size_t m_vbCapacity = 0;
        std::unique_ptr<rhi::Sampler> m_point, m_linear;
        std::unique_ptr<rhi::Pipeline> m_blit, m_fillColor, m_fillDepth;
        std::unordered_map<uint32_t, std::unique_ptr<rhi::Pipeline>> m_drawPipelines;
        std::unordered_map<uint32_t, Rt> m_rts;
        std::unordered_map<uint32_t, Depth> m_depth;
        std::unordered_map<uint64_t, std::unique_ptr<rhi::Texture>> m_textures;
        std::unique_ptr<rhi::Texture> m_display, m_feedback;
        uint32_t m_displayW = 640, m_displayH = 448;
        std::unordered_set<uint32_t> m_reportedBlend;
        uint64_t m_feedbackReported = 0;
        bool m_inPass = false;
        rhi::Texture *m_passColor = nullptr, *m_passDepth = nullptr;
    };

    // ------------------------------------------------------------------------------------------------------
    class HwBackend final : public GSHwBackendHandle, public GSHwPresenter
    {
    public:
        explicit HwBackend(const GSHwOptions &o) : m_scale(o.scale)
        {
            m_frame = std::make_unique<Frame>();
        }

        // ---- GSRasterBackend (game thread) ----------------------------------------------------------------
        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_cpu.Initialize(vram, vramSize); }
        void Reset() override { m_cpu.Reset(); }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            if (batch.vertexCount == 0u)
                return;
            HwState s{};
            if (!buildState(batch, s))
                return;
            const uint8_t type = batch.state.prim.type;
            const auto &ctx = batch.state.context;
            const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f;
            const float zs = (ctx.zbuf.psm == GS_PSM_Z32) ? (1.0f / 4294967296.0f)
                           : (ctx.zbuf.psm == GS_PSM_Z16 || ctx.zbuf.psm == GS_PSM_Z16S) ? (1.0f / 65536.0f)
                                                                                         : (1.0f / 16777216.0f);
            auto mk = [&](const GSVertex &v) {
                HwVertex h{};
                h.x = v.x - ofx;
                h.y = v.y - ofy;
                h.z = std::min(static_cast<float>(v.z * zs), 1.0f);
                if (batch.state.prim.fst)
                {
                    h.s = v.u / 16.0f;
                    h.t = v.v / 16.0f;
                    h.q = 1.0f;
                }
                else
                {
                    h.s = v.s;
                    h.t = v.t;
                    h.q = v.q;
                }
                h.r = v.r; h.g = v.g; h.b = v.b; h.a = v.a;
                h.fog = v.fog / 255.0f;
                return h;
            };
            HwVertex out[6];
            uint32_t n = 0;
            if (type == GS_PRIM_SPRITE && batch.vertexCount >= 2)
            {
                // Sprite: corners from v0/v1, everything else from v1.
                HwVertex a = mk(batch.vertices[0]), b = mk(batch.vertices[1]);
                HwVertex c = b, d = b;
                c.x = a.x; c.s = a.s; d.y = a.y; d.t = a.t;
                a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a; a.z = b.z; a.fog = b.fog; a.q = b.q;
                c.q = b.q; d.q = b.q;
                out[0] = a; out[1] = d; out[2] = c; out[3] = d; out[4] = b; out[5] = c;
                n = 6;
                s.mode = 0;
            }
            else if ((type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) && batch.vertexCount >= 2)
            {
                out[0] = mk(batch.vertices[0]);
                out[1] = mk(batch.vertices[1]);
                n = 2;
                s.mode = 1;
            }
            else if (type == GS_PRIM_POINT)
            {
                out[0] = mk(batch.vertices[0]);
                n = 1;
                s.mode = 2;
            }
            else if (batch.vertexCount >= 3)
            {
                out[0] = mk(batch.vertices[0]);
                out[1] = mk(batch.vertices[1]);
                out[2] = mk(batch.vertices[2]);
                n = 3;
                s.mode = 0;
            }
            if (!n)
                return;
            if (!s.iip) // [gta-recomp] flat: the GS colours with the last vertex; give every vertex that colour
                for (uint32_t i = 0; i + 1 < n; ++i)
                {
                    out[i].r = out[n - 1].r;
                    out[i].g = out[n - 1].g;
                    out[i].b = out[n - 1].b;
                    out[i].a = out[n - 1].a;
                }
            // [gta-recomp] VCS draws its HUD, text, radar and menus (RwIm2D) at the largest Z, whatever the depth test
            // (RenderWare keeps ZTE on: ZTE=0 is prohibited on the GS); projected sprites (clouds, coronas) take the
            // same path at their real depth.
            bool far = true;
            float x0 = out[0].x, x1 = out[0].x;
            for (uint32_t i = 0; i < n; ++i)
            {
                x0 = std::min(x0, out[i].x);
                x1 = std::max(x1, out[i].x);
                far = far && out[i].z >= 0.99999f; // the largest Z24 / Z32 value
            }
            // 3D (widescreen presentation): depth-tested, not at the far plane
            if (s.zte && s.ztst >= 2u && !far)
                m_frame->had3d = true;
            // widescreen: the HUD keeps its 4:3 shape in the middle (gsHwSetHudSqueeze) while the picture is shown
            // wide - draws at the far plane, except full-width ones (sky, fades) and the screen effects that sample the
            // frame (drawn in strips). The scissor goes with them (the radar and menu labels are clipped by it).
            if (far && s.fbw == 10u && !s.texIsRt && !(x0 <= 1.0f && x1 >= 639.0f) && m_showing3d.load(std::memory_order_relaxed))
            {
                const float squeeze = g_hudSqueeze.load(std::memory_order_relaxed);
                if (squeeze < 0.9999f)
                {
                    for (uint32_t i = 0; i < n; ++i)
                        out[i].x = 320.0f + (out[i].x - 320.0f) * squeeze;
                    if (s.scx0 > 0u || s.scx1 < 639u)
                    {
                        s.scx0 = static_cast<uint16_t>(std::max(0.0f, std::floor(320.0f + (s.scx0 - 320.0f) * squeeze)));
                        s.scx1 = static_cast<uint16_t>(std::min(639.0f, std::ceil(320.0f + (s.scx1 + 1.0f - 320.0f) * squeeze) - 1.0f));
                    }
                }
            }
            if (s.zte && s.ztst != 1u && s.zbp != s.fbp) // a depth buffer in real use (tested against)
            {
                DepthInfo &di = m_depthBufs[s.zbp];
                di.zpsm = s.zpsm;
                di.fbw = s.fbw;
                di.height = std::max<uint32_t>(di.height, std::min<uint32_t>(s.scy1 + 1u, 1024u));
            }
            if (depthFill(batch, s, out, n))
                return;
            noteTarget(s.fbp, s.fbw, s.fpsm, out, n, s.scy1);
            Frame &f = *m_frame;
            if (f.draws.empty() || f.cmds.empty() || f.cmds.back().type != CmdType::Draw ||
                std::memcmp(&f.draws.back().s, &s, sizeof(HwState)) != 0)
            {
                f.cmds.push_back({CmdType::Draw, static_cast<uint32_t>(f.draws.size())});
                f.draws.push_back({s, static_cast<uint32_t>(f.vertices.size()), 0u});
            }
            f.vertices.insert(f.vertices.end(), out, out + n);
            f.draws.back().count += n;
            ps2hle::g_gsVerticesRecorded += n; // [gta-recomp] PS2X_HLE_SURVEY
        }

        struct DepthInfo // [gta-recomp] depth buffers seen in use (see depthFill)
        {
            uint32_t zpsm = 0, fbw = 1, height = 1; // height: lowest scissor row + 1 of the draws using it
        };

        // [gta-recomp] Games clear Z by drawing a flat colour sprite into the depth buffer's memory. VCS (Z24 640x448
        // at zbp 280) draws one 640x224 CT32 sprite with FBP = 280 and ZBP = 350, Z test ALWAYS, Z write on: the colour
        // write clears the first half of the Z buffer's pages, its own depth write (Z 0 at page 350 onwards) the second
        // half. Our depth buffers are GPU textures, not VRAM, so both writes become depth clears of the rows of the real
        // depth buffer whose pages they cover. Without this the bottom half of the Z buffer kept stale depth: the world
        // vanished wherever the camera had moved (ground and sky gone when driving, after camera cuts).
        bool depthFill(const GSPrimitiveBatch &batch, const HwState &s, const HwVertex *v, uint32_t n)
        {
            if (!depthBufferAt(s.fbp) || s.tme || s.abe || s.fbmsk != 0u)
                return false;
            if (s.fpsm != GS_PSM_CT32 && s.fpsm != GS_PSM_CT24 && s.fpsm != GS_PSM_CT16 && s.fpsm != GS_PSM_CT16S)
                return false;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t i = 0; i < n; ++i)
            {
                x0 = std::min(x0, v[i].x);
                x1 = std::max(x1, v[i].x);
                y0 = std::min(y0, v[i].y);
                y1 = std::max(y1, v[i].y);
            }
            // VCS draws the clear as 40 narrow column strips; each clears whole rows here (the strips together
            // cover them anyway), so the x extent is not checked
            (void)x0;
            (void)x1;
            y0 = std::max(y0, static_cast<float>(s.scy0));
            y1 = std::min(y1, static_cast<float>(s.scy1) + 1.0f);
            if (y1 <= y0)
                return true;
            const GSVertex &last = batch.vertices[batch.vertexCount - 1u];
            const uint32_t rgba = last.r | (uint32_t(last.g) << 8) | (uint32_t(last.b) << 16) | (uint32_t(last.a) << 24);
            // the colour write
            fillDepthPages(s.fbp, s.fbw, s.fpsm, y0, y1, [&](uint32_t zpsm) {
                return (zpsm == GS_PSM_Z32) ? static_cast<float>(rgba / 4294967296.0)
                     : (zpsm == GS_PSM_Z16 || zpsm == GS_PSM_Z16S) ? (rgba & 0xFFFFu) / 65536.0f
                                                                   : (rgba & 0xFFFFFFu) / 16777216.0f;
            });
            // the draw's own depth write, when it lands inside a depth buffer too
            if (s.zte && s.ztst == 1u && !s.zmsk)
            {
                const float z = v[n - 1u].z;
                fillDepthPages(s.zbp, s.fbw, s.zpsm, y0, y1, [&](uint32_t) { return z; });
            }
            return true;
        }

        // The depth buffer whose pages contain `page`, if any.
        const std::pair<const uint32_t, DepthInfo> *depthBufferAt(uint32_t page) const
        {
            for (const auto &entry : m_depthBufs)
            {
                uint32_t zpw, zph;
                pageSize(entry.second.zpsm, zpw, zph);
                const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(entry.second.fbw, 1u) * 64u + zpw - 1u) / zpw);
                if (page >= entry.first && page < entry.first + perRow * ((entry.second.height + zph - 1u) / zph))
                    return &entry;
            }
            return nullptr;
        }

        // Rows y0..y1 of a buffer at `base` (width fbw, format psm) as a depth clear of the depth buffer holding them.
        template <typename ValueFn>
        void fillDepthPages(uint32_t base, uint32_t fbw, uint32_t psm, float y0, float y1, ValueFn value)
        {
            const auto *depth = depthBufferAt(base);
            if (!depth)
                return;
            const DepthInfo &di = depth->second;
            uint32_t pw, ph, zpw, zph;
            pageSize(psm, pw, ph);
            pageSize(di.zpsm, zpw, zph);
            const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(fbw, 1u) * 64u + pw - 1u) / pw);
            const uint32_t depthPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(di.fbw, 1u) * 64u + zpw - 1u) / zpw);
            const uint32_t pageFirst = base - depth->first + static_cast<uint32_t>(y0) / ph * perRow;
            const uint32_t pageLast = base - depth->first + (static_cast<uint32_t>(y1) + ph - 1u) / ph * perRow; // exclusive
            DepthFillCmd fill;
            fill.zbp = depth->first;
            fill.width = di.fbw * 64u;
            fill.y0 = pageFirst / depthPerRow * zph;
            fill.y1 = (pageLast + depthPerRow - 1u) / depthPerRow * zph;
            fill.value = value(di.zpsm);
            static int logged = 0;
            static const bool hwDebug = std::getenv("PS2X_HW_DEBUG") != nullptr; // [gta-recomp] read once
            if (hwDebug && logged++ < 8)
                std::fprintf(stderr, "[gs-hw] depth fill: base=%u fbw=%u psm=%u y=%.0f..%.0f -> zbp=%u zpsm=%u rows %u..%u value %f\n", base,
                             fbw, psm, y0, y1, fill.zbp, di.zpsm, fill.y0, fill.y1, fill.value);
            Frame &f = *m_frame;
            f.cmds.push_back({CmdType::DepthFill, static_cast<uint32_t>(f.depthFills.size())});
            f.depthFills.push_back(fill);
            static int extra = 0; // a clear is two fills per frame; more would be a misread draw
            if (f.depthFills.size() > 2u && hwDebug && extra++ < 20)
                std::fprintf(stderr, "[gs-hw] unexpected depth fill #%zu this frame: base=%u fbw=%u psm=%u rows %u..%u\n", f.depthFills.size(),
                             base, fbw, psm, fill.y0, fill.y1);
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && tex0.cld != 0u && m_loggedTransfers < 400u)
            {
                ++m_loggedTransfers;
                std::fprintf(stderr, "[gs-hw] clut cld=%u cbp=%u cpsm=%u csm=%u csa=%u psm=%u\n", tex0.cld, tex0.cbp, tex0.cpsm, tex0.csm,
                             tex0.csa, tex0.psm);
            }
            if (tex0.cld != 0u)
            {
                const bool eightBit = tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
                if (materializeRectKeepTransfer(tex0.cpsm, tex0.cbp, 1u, 0u, 0u, eightBit ? 16u : 8u, eightBit ? 16u : 2u))
                    ++m_lazyWhy[0];
            }
            m_cpu.LoadClut(tex0, texclut);
            ++m_texStateVersion;
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            if (command.direction != 0u) // local -> host / local -> local read VRAM right away
                materializeAll();
            m_transfer = command;
            m_cpu.BeginTransfer(command);
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_loggedTransfers < 400u)
            {
                ++m_loggedTransfers;
                std::fprintf(stderr, "[gs-hw] transfer dir=%u sbp=%u sbw=%u spsm=%u ss=%u,%u -> dbp=%u dbw=%u dpsm=%u ds=%u,%u size=%ux%u\n",
                             command.direction, command.bitbltbuf.sbp, command.bitbltbuf.sbw, command.bitbltbuf.spsm,
                             command.trxpos.ssax, command.trxpos.ssay, command.bitbltbuf.dbp, command.bitbltbuf.dbw,
                             command.bitbltbuf.dpsm, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh);
            }
            if (command.direction == 2u) // local -> local: destination pages change
                touchTransferDest();
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_transfer.bitbltbuf.dbp >= 13584u && m_transfer.bitbltbuf.dbp < 13600u && m_loggedTransfers < 400u)
            {
                std::fprintf(stderr, "[gs-hw]   data %u bytes: %02x%02x%02x%02x %02x%02x%02x%02x\n", sizeBytes, data[0], data[1], data[2],
                             data[3], data[4], data[5], data[6], data[7]);
                static int dumpedCluts = 0;
                if (sizeBytes == 1024u && dumpedCluts < 8)
                {
                    char name[32];
                    std::snprintf(name, sizeof(name), "hw_clut%d.bin", dumpedCluts++);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(data, 1, sizeBytes, f);
                        std::fclose(f);
                    }
                }
            }
            if (debug && sizeBytes >= 65536u)
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < sizeBytes; ++i)
                    h = (h ^ data[i]) * 1099511628211ull;
                std::fprintf(stderr, "[gs-hw] upload %u bytes dpsm=%u fnv=%016llx first=%02x%02x%02x%02x%02x%02x%02x%02x\n", sizeBytes,
                             m_transfer.bitbltbuf.dpsm, static_cast<unsigned long long>(h), data[0], data[1], data[2], data[3], data[4],
                             data[5], data[6], data[7]);
                static int dumped = 0;
                if (dumped < 8 && sizeBytes == 262144u)
                {
                    char name[32];
                    std::snprintf(name, sizeof(name), "hw_upload%d.bin", dumped++);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(data, 1, sizeBytes, f);
                        std::fclose(f);
                    }
                }
            }
            // [gta-recomp] Games re-send the same textures every frame. A whole-transfer upload identical to the last
            // one to the same place, with nothing else written to those pages since, changes nothing: skip the
            // swizzled write and keep the page generations (so cached GPU textures stay valid).
            const GSTransferCommand &t = m_transfer;
            uint32_t first = 0, last = 0;
            pageRange(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                      first, last);
            const uint64_t bitsPerPixel = transferBitsPerPixel(t.bitbltbuf.dpsm);
            const bool wholeTransfer = bitsPerPixel != 0u && last <= kPages &&
                                       static_cast<uint64_t>(t.trxreg.rrw) * t.trxreg.rrh * bitsPerPixel / 8u == sizeBytes;
            uint64_t uploadKey = 0, dataHash = 0;
            { // PS2X_HW_UPSTATS=1: upload kinds every 5 s
                static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
                static uint64_t whole = 0, partial = 0, bytesWhole = 0, bytesPartial = 0;
                static auto last = std::chrono::steady_clock::now();
                if (stats)
                {
                    (wholeTransfer ? whole : partial) += 1;
                    (wholeTransfer ? bytesWhole : bytesPartial) += sizeBytes;
                    if (!wholeTransfer && partial < 20)
                        std::fprintf(stderr, "[upload] partial: %u bytes, rect %ux%u psm %u (expects %llu)\n", sizeBytes, t.trxreg.rrw,
                                     t.trxreg.rrh, t.bitbltbuf.dpsm,
                                     static_cast<unsigned long long>(uint64_t(t.trxreg.rrw) * t.trxreg.rrh * bitsPerPixel / 8u));
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::seconds(5))
                    {
                        last = now;
                        std::fprintf(stderr, "[upload] whole %llu (%llu KB) partial %llu (%llu KB) skipped %llu\n",
                                     (unsigned long long)whole, (unsigned long long)(bytesWhole >> 10), (unsigned long long)partial,
                                     (unsigned long long)(bytesPartial >> 10), (unsigned long long)m_skippedUploads);
                        whole = partial = bytesWhole = bytesPartial = 0;
                        m_skippedUploads = 0;
                    }
                }
            }
            if (wholeTransfer)
            {
                uploadKey = mix64(mix64(0x51ull, t.bitbltbuf.dbp | (uint64_t(t.bitbltbuf.dbw) << 16) | (uint64_t(t.bitbltbuf.dpsm) << 24) |
                                                     (uint64_t(t.trxpos.dsax) << 32) | (uint64_t(t.trxpos.dsay) << 48)),
                                  t.trxreg.rrw | (uint64_t(t.trxreg.rrh) << 16) | (uint64_t(sizeBytes) << 32));
                dataHash = hashBytes(data, sizeBytes);
                auto it = m_uploads.find(uploadKey);
                if (it != m_uploads.end() && it->second.hash == dataHash && it->second.first == first && it->second.gens.size() == last - first)
                {
                    bool untouched = true;
                    for (uint32_t p = first; p < last && untouched; ++p)
                        untouched = m_pageGen[p] == it->second.gens[p - first];
                    if (untouched)
                    {
                        ++m_skippedUploads;
                        return;
                    }
                }
            }
            const uint32_t dpsm = t.bitbltbuf.dpsm;
            if (wholeTransfer && sizeBytes != 0u && lazyUploadsEnabled() &&
                (dpsm == GS_PSM_T4 || dpsm == GS_PSM_T8 || dpsm == GS_PSM_CT32 || dpsm == GS_PSM_CT16) && !overlapsRenderTarget(first, last))
            {
                deferUpload(data, sizeBytes);
            }
            else
            {
                if (prepareDirectUpload() && ++m_lazyWhy[2])
                    m_cpu.BeginTransfer(m_transfer); // replaying queued uploads replaced the CPU backend's transfer state
                m_cpu.UploadImage(data, sizeBytes);
            }
            touchTransferDest();
            if (wholeTransfer)
            {
                UploadInfo &info = m_uploadAt[t.bitbltbuf.dbp];
                info.hash = dataHash;
                info.dbw = t.bitbltbuf.dbw;
                info.dpsm = t.bitbltbuf.dpsm;
                info.x = t.trxpos.dsax;
                info.y = t.trxpos.dsay;
                info.w = t.trxreg.rrw;
                info.h = t.trxreg.rrh;
                info.first = first;
                info.gens.assign(m_pageGen + first, m_pageGen + last);
                if (m_uploads.size() > 4096u)
                    m_uploads.clear();
                UploadRecord &rec = m_uploads[uploadKey];
                rec.hash = dataHash;
                rec.first = first;
                rec.gens.assign(m_pageGen + first, m_pageGen + last);
            }
        }

        static uint64_t transferBitsPerPixel(uint32_t psm)
        {
            switch (psm)
            {
            case GS_PSM_CT32: case GS_PSM_Z32: return 32u;
            case GS_PSM_CT24: case GS_PSM_Z24: return 24u;
            case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: return 16u;
            case GS_PSM_T8: case GS_PSM_T8H: return 8u;
            case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: return 4u;
            default: return 0u;
            }
        }

        static uint64_t hashBytes(const uint8_t *data, uint32_t size)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
            uint32_t i = 0;
            for (; i + 8u <= size; i += 8u)
            {
                uint64_t v;
                std::memcpy(&v, data + i, 8);
                h = (h ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
                h ^= h >> 29;
            }
            for (; i < size; ++i)
                h = (h ^ data[i]) * 0x100000001B3ull;
            return h;
        }

        void Flush() override { m_cpu.Flush(); }
        void TextureFlush() override { m_cpu.TextureFlush(); }
        void Sync(GSSyncReason reason) override { m_cpu.Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &request) override { return m_cpu.Present(request); }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            materializeAllKeepTransfer();
            HwState s{};
            s.fbp = context.frame.fbp;
            s.fbw = context.frame.fbw;
            s.fpsm = context.frame.psm;
            s.scx0 = context.scissor.x0;
            s.scy0 = context.scissor.y0;
            s.scx1 = context.scissor.x1;
            s.scy1 = context.scissor.y1;
            Frame &f = *m_frame;
            f.cmds.push_back({CmdType::Clear, static_cast<uint32_t>(f.clears.size())});
            f.clears.push_back({s, rgba});
            m_cpu.ClearFramebuffer(context, rgba);
            return true;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            materializeAllKeepTransfer();
            return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            return m_cpu.ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            materializeAllKeepTransfer();
            m_cpu.WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            m_cpu.SnapshotVram(out);
        }
        GSTransferSnapshot GetTransferSnapshot() const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            return m_cpu.GetTransferSnapshot();
        }

        // ---- [gta-recomp] lazy texture uploads ---------------------------------------------------------------------
        // VCS re-sends its T4/T8 model textures into shared VRAM every frame (~20 MB/s). The GPU side finds them in
        // its content-keyed texture cache, so the swizzled copy in CPU VRAM is only needed when something reads those
        // bytes: a texture decode on a cache miss, a CLUT load, a readback, a local transfer, a clear. Whole T4/T8
        // uploads (not over a render target) are therefore queued and written into CPU VRAM - in order, together with
        // every earlier queued upload overlapping them - only when such a read touches their 256-byte blocks. Blocks,
        // not pages: VCS keeps each texture's CLUT in the same page. A queued upload whose blocks have all been
        // completely overwritten by later uploads is dropped (the steady state: the same slots refilled each frame).
        // PS2X_HW_LAZY_UPLOADS=0 writes every upload immediately (as before).
        static constexpr uint32_t kBlocks = 16384u; // 4 MiB / 256 B

        struct PendingUpload
        {
            GSTransferCommand cmd;
            std::vector<uint8_t> data;
            std::vector<uint16_t> blocks; // touched
            uint32_t live = 0;            // blocks not yet completely overwritten by a later upload
            bool done = false;            // written or dropped
        };

        static bool lazyUploadsEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("PS2X_HW_LAZY_UPLOADS");
                return !(v && v[0] == '0');
            }();
            return on;
        }

        bool overlapsRenderTarget(uint32_t first, uint32_t last) const
        {
            for (const auto &[fbp, rt] : m_rts)
                if (first < fbp + rt.pageRows * rt.fbw && last > fbp)
                    return true;
            return false;
        }

        // Blocks of a rectangle; formats RectBlocks does not know count as their whole pages.
        void rectBlocks(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint16_t> &touched,
                        std::vector<uint16_t> *full)
        {
            if (GSMem::RectBlocks(psm, bp, bw, x, y, w, h, touched, full))
                return;
            uint32_t first, last;
            pageRange(bp, bw, psm, x, y, w, h, first, last);
            for (uint32_t pg = first; pg < last; ++pg)
                for (uint32_t b = 0; b < 32u; ++b)
                    touched.push_back(static_cast<uint16_t>(((pg * 32u) + b) % kBlocks));
        }

        PendingUpload *pendingById(uint64_t id)
        {
            if (id < m_pendingFrontId || id - m_pendingFrontId >= m_pendingUploads.size())
                return nullptr;
            PendingUpload &u = m_pendingUploads[static_cast<size_t>(id - m_pendingFrontId)];
            return u.done ? nullptr : &u;
        }

        void retire(PendingUpload &u)
        {
            u.done = true;
            for (uint16_t b : u.blocks)
                --m_blockPending[b];
            m_pendingBytes -= u.data.size();
            std::vector<uint8_t>().swap(u.data);
            while (!m_pendingUploads.empty() && m_pendingUploads.front().done)
            {
                m_pendingUploads.pop_front();
                ++m_pendingFrontId;
            }
        }

        void deferUpload(const uint8_t *data, uint32_t sizeBytes)
        {
            const GSTransferCommand &t = m_transfer;
            m_scratchTouched.clear();
            m_scratchFull.clear();
            rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                       m_scratchTouched, &m_scratchFull);
            // blocks this upload overwrites completely: earlier queued writers of them lose those blocks
            for (uint16_t b : m_scratchFull)
            {
                for (uint64_t id : m_blockWriters[b])
                    if (PendingUpload *old = pendingById(id))
                        if (old->live && --old->live == 0u)
                        {
                            retire(*old);
                            ++m_lazyDropped;
                        }
                m_blockWriters[b].clear();
            }
            PendingUpload pu;
            pu.cmd = t;
            pu.data.assign(data, data + sizeBytes);
            pu.blocks = m_scratchTouched;
            std::sort(pu.blocks.begin(), pu.blocks.end());
            pu.blocks.erase(std::unique(pu.blocks.begin(), pu.blocks.end()), pu.blocks.end());
            pu.live = static_cast<uint32_t>(pu.blocks.size());
            const uint64_t id = m_pendingFrontId + m_pendingUploads.size();
            for (uint16_t b : pu.blocks)
            {
                ++m_blockPending[b];
                m_blockWriters[b].push_back(id);
            }
            m_pendingBytes += sizeBytes;
            m_pendingUploads.push_back(std::move(pu));
            ++m_lazyDeferred;
            if (m_pendingUploads.size() > 16384u || m_pendingBytes > (64u << 20))
                materializeAll(); // bounded memory; deferring carries on afterwards
            reportLazyStats();
        }

        // Writes the queued uploads touching `blocks` - and, to keep the order, every earlier queued upload overlapping
        // those - into CPU VRAM. True if anything was written.
        bool materializeBlocks(const std::vector<uint16_t> &blocks, bool all = false)
        {
            if (m_pendingBytes == 0u && m_pendingUploads.empty())
                return false;
            bool any = all;
            for (size_t i = 0; i < blocks.size() && !any; ++i)
                any = m_blockPending[blocks[i]] != 0u;
            if (!any)
                return false;
            // wanted blocks: m_wantStamp[b] == stamp (no 16 KB clear per call)
            if (++m_wantGeneration == 0u)
            {
                std::fill(m_wantStamp.begin(), m_wantStamp.end(), 0u);
                m_wantGeneration = 1u;
            }
            const uint32_t stamp = m_wantGeneration;
            auto wanted = [&](uint16_t b) { return all || m_wantStamp[b] == stamp; };
            for (uint16_t b : blocks)
                m_wantStamp[b] = stamp;
            std::vector<uint8_t> &take = m_scratchTake;
            take.assign(m_pendingUploads.size(), 0u);
            for (bool changed = true; changed;)
            {
                changed = false;
                for (size_t i = 0; i < m_pendingUploads.size(); ++i)
                {
                    const PendingUpload &u = m_pendingUploads[i];
                    if (take[i] || u.done)
                        continue;
                    bool hit = false;
                    for (size_t k = 0; k < u.blocks.size() && !hit; ++k)
                        hit = wanted(u.blocks[k]);
                    if (!hit)
                        continue;
                    take[i] = 1u;
                    changed = true;
                    for (uint16_t b : u.blocks)
                        m_wantStamp[b] = stamp;
                }
            }
            bool wrote = false;
            for (size_t i = 0; i < take.size(); ++i)
            {
                if (!take[i])
                    continue;
                PendingUpload &u = m_pendingUploads[i];
                m_cpu.BeginTransfer(u.cmd);
                m_cpu.UploadImage(u.data.data(), static_cast<uint32_t>(u.data.size()));
                u.done = true;
                for (uint16_t b : u.blocks)
                    --m_blockPending[b];
                m_pendingBytes -= u.data.size();
                std::vector<uint8_t>().swap(u.data);
                ++m_lazyWritten;
                wrote = true;
            }
            while (!m_pendingUploads.empty() && m_pendingUploads.front().done)
            {
                m_pendingUploads.pop_front();
                ++m_pendingFrontId;
            }
            return wrote;
        }

        // Before an upload that is written straight away: queued uploads it overwrites completely are dropped; queued
        // uploads it only partly overwrites, or that keep other live blocks, are written first (in order), so a later
        // write of theirs cannot land on top of this one. True if anything was written.
        bool prepareDirectUpload()
        {
            if (m_pendingUploads.empty())
                return false;
            const GSTransferCommand &t = m_transfer;
            m_scratchTouched.clear();
            m_scratchFull.clear();
            rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                       m_scratchTouched, &m_scratchFull);
            for (uint16_t b : m_scratchFull)
            {
                if (!m_blockPending[b])
                    continue;
                for (uint64_t id : m_blockWriters[b])
                    if (PendingUpload *old = pendingById(id))
                        if (old->live && --old->live == 0u)
                        {
                            retire(*old);
                            ++m_lazyDropped;
                        }
                m_blockWriters[b].clear();
            }
            // whatever is still queued on these blocks (partly covered, or alive elsewhere) goes out first
            return materializeBlocks(m_scratchTouched);
        }

        bool materializeRect(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
        {
            if (m_pendingUploads.empty())
                return false;
            m_scratchRead.clear();
            rectBlocks(psm, bp, bw, x, y, w, h, m_scratchRead, nullptr);
            return materializeBlocks(m_scratchRead);
        }

        bool materializeAll()
        {
            if (m_pendingUploads.empty())
                return false;
            static const std::vector<uint16_t> none;
            return materializeBlocks(none, true);
        }

        // Outside an upload: afterwards the CPU backend's transfer state is put back to the current transfer (an
        // upload that has not received its data yet - the usual case between GIF packets).
        bool materializeRectKeepTransfer(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
        {
            if (!materializeRect(psm, bp, bw, x, y, w, h))
                return false;
            if (m_transfer.direction == 0u)
                m_cpu.BeginTransfer(m_transfer);
            return true;
        }
        void materializeAllKeepTransfer()
        {
            if (materializeAll())
            {
                ++m_lazyWhy[3];
                if (m_transfer.direction == 0u)
                    m_cpu.BeginTransfer(m_transfer);
            }
        }

        void reportLazyStats()
        {
            static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
            static auto lastReport = std::chrono::steady_clock::now();
            if (!stats || std::chrono::steady_clock::now() - lastReport < std::chrono::seconds(5))
                return;
            lastReport = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[upload] lazy: deferred %llu, written later %llu, made obsolete %llu, queued now %zu (%zu KB); forced by clut %llu, decode %llu, direct upload %llu, other %llu\n",
                         (unsigned long long)m_lazyDeferred, (unsigned long long)m_lazyWritten, (unsigned long long)m_lazyDropped,
                         m_pendingUploads.size(), m_pendingBytes >> 10, (unsigned long long)m_lazyWhy[0], (unsigned long long)m_lazyWhy[1],
                         (unsigned long long)m_lazyWhy[2], (unsigned long long)m_lazyWhy[3]);
            m_lazyDeferred = m_lazyWritten = m_lazyDropped = 0;
            for (uint64_t &w : m_lazyWhy)
                w = 0;
        }

        // ---- frame hand-over (game thread) -------------------------------------------------------------------
        void endFrame(const uint64_t (&privRegs)[15]) override
        {
            Frame &f = *m_frame;
            std::memcpy(f.privRegs, privRegs, sizeof(f.privRegs));
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_frameIndex >= 100u && m_frameIndex < 130u)
                std::fprintf(stderr, "[gs-hw] vblank %llu: draws=%zu dispfb1=%llx dispfb2=%llx last draw fbp=%u\n",
                             static_cast<unsigned long long>(m_frameIndex), f.draws.size(), static_cast<unsigned long long>(privRegs[7]),
                             static_cast<unsigned long long>(privRegs[9]), f.draws.empty() ? 9999u : f.draws.back().s.fbp);
            if (debug && m_frameIndex >= 100u && m_frameIndex < 104u)
                for (const DrawCmd &dc : f.draws)
                    std::fprintf(stderr, "[gs-hw]   draw fbp=%u fbw=%u psm=%u zbp=%u zte=%u n=%u mode=%u tme=%u rt=%u(%u @%u,%u) tex=%llx %ux%u tfx=%u abe=%u fbmsk=%x sc=%u,%u-%u,%u\n",
                                 dc.s.fbp, dc.s.fbw, dc.s.fpsm, dc.s.zbp, dc.s.zte, dc.count, dc.s.mode, dc.s.tme, dc.s.texIsRt,
                                 dc.s.rtFbp, dc.s.rtOx, dc.s.rtOy, static_cast<unsigned long long>(dc.s.texKey), dc.s.tw, dc.s.th,
                                 dc.s.tfx, dc.s.abe, dc.s.fbmsk, dc.s.scx0, dc.s.scy0, dc.s.scx1, dc.s.scy1);
            // Which buffer is displayed: circuit 1 if enabled, else 2.
            const uint64_t pmode = privRegs[0];
            const bool use1 = (pmode & 1u) != 0u || (pmode & 2u) == 0u;
            const uint64_t dispfb = use1 ? privRegs[7] : privRegs[9];
            const uint64_t display = use1 ? privRegs[8] : privRegs[10];
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
            const uint32_t dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
            const uint32_t dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
            const uint32_t magv = static_cast<uint32_t>((display >> 27) & 0x3u) + 1u;
            uint32_t w = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / magh;
            uint32_t h = (static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u) / magv;
            w = std::clamp<uint32_t>(w, 1u, 1024u);
            h = std::clamp<uint32_t>(h, 1u, 1024u);
            f.displayFbp = fbp;
            f.displayX = dbx;
            f.displayY = dby;
            f.displayW = w;
            f.displayH = h;
            f.displayFromRt = m_rts.count(fbp) != 0;
            if (!f.displayFromRt && display != 0u && fbw != 0u)
            {
                // Not drawn by the GPU this session: show what CPU VRAM holds (uploaded images).
                f.cpuDisplayW = w;
                f.cpuDisplayH = h;
                f.cpuDisplay.resize(static_cast<size_t>(w) * h);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint32_t v = m_cpu.ReadVram(psm, fbp * 32u, fbw, dbx + x, dby + y);
                        if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
                            v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19);
                        f.cpuDisplay[static_cast<size_t>(y) * w + x] = v | 0xFF000000u;
                    }
            }
            ps2perf::add(ps2perf::Counter::Draws, f.draws.size() + f.nativeDraws.size()); // [gta-recomp] once per frame
            ps2perf::add(ps2perf::Counter::Vertices, f.vertices.size() + f.nativeVertices);
            ps2perf::add(ps2perf::Counter::TexUploads, f.textures.size());
            ++m_frameIndex;
            shrinkTargets(); // [gta-recomp]
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                // Don't let the game run more than a few frames ahead of the main thread.
                m_queueCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return m_queue.size() < 3u; });
                m_queue.push_back(std::move(m_frame));
            }
            m_frame = takeFreeFrame();
        }

        // [gta-recomp] Frames are recycled with their vectors' capacity: building each from empty vectors cost
        // reallocation copies on the game thread and page frees on the main thread (~10% of a busy frame).
        std::unique_ptr<Frame> takeFreeFrame()
        {
            std::unique_ptr<Frame> frame;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_freeFrames.empty())
                {
                    frame = std::move(m_freeFrames.back());
                    m_freeFrames.pop_back();
                }
            }
            if (!frame)
                return std::make_unique<Frame>();
            Frame &f = *frame;
            f.cmds.clear();
            f.vertices.clear();
            f.draws.clear();
            f.textures.clear(); // owns pixel vectors; their memory goes with them
            f.texDeletes.clear();
            f.rtUploads.clear();
            f.rtDrops.clear();
            f.clears.clear();
            f.depthFills.clear();
            f.meshes.clear();
            f.meshDeletes.clear();
            f.nativeDraws.clear();
            f.nativeBytes.clear();
            f.nativeVertices = 0;
            f.had3d = false;
            std::memset(f.privRegs, 0, sizeof(f.privRegs));
            f.cpuDisplay.clear();
            f.cpuDisplayW = f.cpuDisplayH = 0;
            f.displayFromRt = false;
            f.displayFbp = f.displayX = f.displayY = f.displayW = f.displayH = 0;
            return frame;
        }

        // [gta-recomp] packet-level HLE: a draw the GPU transforms (see GSNativeDraw)
        bool SubmitNative(const GSDrawState &state, const GSNativeDraw &draw) override
        {
            if ((draw.kind != GSNativeDraw::VcsWorld && draw.kind != GSNativeDraw::VcsLit) || !draw.drawCount || !draw.vertexCount ||
                draw.constantCount > 124u)
                return false;
            GSPrimitiveBatch batch{};
            batch.state = state;
            HwState s{};
            if (!buildState(batch, s))
                return true; // ZTST NEVER: nothing drawn
            s.mode = draw.kind == GSNativeDraw::VcsLit ? 4 : 3; // native strip kinds (pipeline / merge key only)
            if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
            {
                DepthInfo &di = m_depthBufs[s.zbp];
                di.zpsm = s.zpsm;
                di.fbw = s.fbw;
                di.height = std::max<uint32_t>(di.height, std::min<uint32_t>(s.scy1 + 1u, 1024u));
            }
            HwVertex extent{};
            extent.y = static_cast<float>(s.scy1); // the scissor bounds what the draw can touch
            noteTarget(s.fbp, s.fbw, s.fpsm, &extent, 1, s.scy1);
            Frame &f = *m_frame;
            uint32_t transientOffset = 0;
            if (draw.transient)
            {
                transientOffset = static_cast<uint32_t>(f.nativeBytes.size());
                const auto *bytes = static_cast<const uint8_t *>(draw.vertices);
                f.nativeBytes.insert(f.nativeBytes.end(), bytes, bytes + size_t(draw.vertexStride) * draw.vertexCount);
            }
            auto lru = draw.transient ? m_meshLru.end() : m_meshLru.find(draw.meshKey);
            if (draw.transient)
            {
            }
            else if (lru == m_meshLru.end())
            {
                MeshCreate mc;
                mc.key = draw.meshKey;
                mc.stride = draw.vertexStride;
                mc.count = draw.vertexCount;
                const auto *bytes = static_cast<const uint8_t *>(draw.vertices);
                mc.bytes.assign(bytes, bytes + size_t(draw.vertexStride) * draw.vertexCount);
                f.cmds.push_back({CmdType::MeshCreate, static_cast<uint32_t>(f.meshes.size())});
                f.meshes.push_back(std::move(mc));
                m_meshLru[draw.meshKey] = m_frameIndex;
                if (m_meshLru.size() > kMaxMeshes)
                    evictMeshes();
            }
            else
            {
                lru->second = m_frameIndex;
            }
            NativeDrawCmd nd;
            nd.s = s;
            nd.meshKey = draw.meshKey;
            nd.kind = draw.kind;
            nd.drawCount = draw.drawCount;
            nd.transient = draw.transient;
            nd.transientOffset = transientOffset;
            std::memcpy(nd.constants, draw.constants, sizeof(float) * draw.constantCount);
            const float zs = (s.zpsm == GS_PSM_Z32) ? (1.0f / 4294967296.0f)
                           : (s.zpsm == GS_PSM_Z16 || s.zpsm == GS_PSM_Z16S) ? (1.0f / 65536.0f)
                                                                             : (1.0f / 16777216.0f);
            nd.constants[124] = state.context.xyoffset.ofx / 16.0f;
            nd.constants[125] = state.context.xyoffset.ofy / 16.0f;
            nd.constants[126] = zs;
            f.cmds.push_back({CmdType::NativeDraw, static_cast<uint32_t>(f.nativeDraws.size())});
            f.nativeDraws.push_back(nd);
            f.nativeVertices += draw.drawCount;
            f.had3d = true;
            ps2hle::g_gsVerticesRecorded += draw.drawCount;
            return true;
        }

        void evictMeshes()
        {
            std::vector<std::pair<uint64_t, uint64_t>> byAge(m_meshLru.begin(), m_meshLru.end());
            std::sort(byAge.begin(), byAge.end(), [](auto &a, auto &b) { return a.second < b.second; });
            Frame &f = *m_frame;
            for (size_t i = 0; i < byAge.size() / 4; ++i)
            {
                if (byAge[i].second == m_frameIndex)
                    break;
                f.cmds.push_back({CmdType::MeshDelete, static_cast<uint32_t>(f.meshDeletes.size())});
                f.meshDeletes.push_back(byAge[i].first);
                m_meshLru.erase(byAge[i].first);
            }
        }

        GSHwPresenter &presenter() override { return *this; }

        // ---- GSHwPresenter (main thread) ---------------------------------------------------------------------
        bool executeAndGetDisplay(rhi::Device &device, rhi::Texture *&texture, uint32_t &width, uint32_t &height) override
        {
            if (!m_rhi)
            {
                m_rhi = std::make_unique<RhiExecutor>(device, m_scale);
                std::fprintf(stderr, m_rhi->ready() ? "[gs-hw] renderer ready (%s), scale %ux\n" : "[gs-hw] renderer failed (%s)\n",
                             device.adapterName().c_str(), m_scale);
            }
            if (!m_rhi->ready())
                return false;
            std::deque<std::unique_ptr<Frame>> frames;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                frames.swap(m_queue);
            }
            m_queueCv.notify_all();
            if (!frames.empty())
            {
                // [gta-recomp] F12 (or PS2X_HW_DUMP=<frame>): list the next frame's draws and dump its targets
                static const long dumpAt = std::getenv("PS2X_HW_DUMP") ? std::atol(std::getenv("PS2X_HW_DUMP")) : -1;
                const bool f12 = ps2control::keyDown(301 /* KEY_F12 */);
                if (f12 && !m_f12Down)
                    m_dumpPending = true;
                m_f12Down = f12;
                if (dumpAt >= 0 && !m_dumped && m_executedFrames + frames.size() >= static_cast<uint64_t>(dumpAt))
                    m_dumpPending = true;
                // the next batch that draws anything (the game draws in every other VBlank at 30 fps)
                const bool dumpNow = m_dumpPending &&
                                     std::any_of(frames.begin(), frames.end(), [](const auto &fr) { return !fr->cmds.empty(); });
                // PS2X_HW_DUMP_BATCHES=<n>: list the draws of n consecutive batches (a game frame's 3D and its
                // post-effect / HUD frame can arrive in different batches); the targets are written after the last
                static const int dumpBatches = std::max(1, std::getenv("PS2X_HW_DUMP_BATCHES") ? std::atoi(std::getenv("PS2X_HW_DUMP_BATCHES")) : 1);
                if (dumpNow)
                {
                    m_dumpPending = false;
                    m_dumpBatchesLeft = dumpBatches;
                }
                const bool logThis = m_dumpBatchesLeft > 0;
                const bool dumpTargets = logThis && m_dumpBatchesLeft == 1;
                for (auto &frame : frames)
                {
                    if (logThis)
                        std::fprintf(stderr, "[draw] --- frame (%zu draws, %zu native, 3d %d) ---\n", frame->draws.size(),
                                     frame->nativeDraws.size(), frame->had3d ? 1 : 0);
                    m_rhi->execute(*frame, logThis);
                }
                if (m_dumpBatchesLeft > 0)
                    --m_dumpBatchesLeft;
                m_rhi->composeDisplay(*frames.back());
                {
                    // [gta-recomp] lastFrameInfo: per frame of the batch: draws / native draws / clears / rt drops,
                    // 3D, displayed buffer
                    std::string info = "batch " + std::to_string(frames.size()) + ":";
                    for (auto &fr : frames)
                    {
                        char b[160];
                        std::snprintf(b, sizeof(b), " [draws %zu native %zu clears %zu drops %zu 3d %d disp fbp %u rt %d]",
                                      fr->draws.size(), fr->nativeDraws.size(), fr->clears.size(), fr->rtDrops.size(),
                                      fr->had3d ? 1 : 0, fr->displayFbp, fr->displayFromRt ? 1 : 0);
                        info += b;
                    }
                    std::lock_guard<std::mutex> lock(m_infoMutex);
                    m_lastInfo = std::move(info);
                }
                // 3D or not over the last 32 frames (the game draws 3D in every other VBlank at 30 fps): 3D in 4 or
                // more of them shows 3D, none shows 2D, in between keeps the current shape (fades, loading flashes)
                for (auto &frame : frames)
                {
                    m_3dHistory = (m_3dHistory << 1) | (frame->had3d ? 1u : 0u);
                    const int n = std::popcount(m_3dHistory);
                    if (n >= 4)
                        m_showing3d = true;
                    else if (n == 0)
                        m_showing3d = false;
                }
                m_executedFrames += frames.size();
                if (dumpTargets)
                {
                    m_dumped = true;
                    m_rhi->dump();
                }
                static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
                if (debug)
                {
                    static auto since = std::chrono::steady_clock::now();
                    static uint64_t nFrames = 0, nDraws = 0, nVerts = 0, nTex = 0;
                    for (auto &fr : frames)
                    {
                        ++nFrames;
                        nDraws += fr->draws.size();
                        nVerts += fr->vertices.size();
                        nTex += fr->textures.size();
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration<double>(now - since).count() >= 1.0)
                    {
                        const Frame &l = *frames.back();
                        std::fprintf(stderr, "[gs-hw] %llu frames, %llu draws, %llu verts, %llu tex | display fbp=%u %ux%u at %u,%u fromRt=%d cpu=%zu | rts:%s",
                                     (unsigned long long)nFrames, (unsigned long long)nDraws, (unsigned long long)nVerts,
                                     (unsigned long long)nTex, l.displayFbp, l.displayW, l.displayH, l.displayX, l.displayY,
                                     l.displayFromRt ? 1 : 0, l.cpuDisplay.size(), m_rhi->describeTargets().c_str());
                        std::fprintf(stderr, " pmode=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                                     (unsigned long long)l.privRegs[0], (unsigned long long)l.privRegs[7], (unsigned long long)l.privRegs[8],
                                     (unsigned long long)l.privRegs[9], (unsigned long long)l.privRegs[10]);
                        nFrames = nDraws = nVerts = nTex = 0;
                        since = now;
                    }
                }
                std::lock_guard<std::mutex> lock(m_queueMutex);
                for (auto &frame : frames)
                    if (m_freeFrames.size() < 6u)
                        m_freeFrames.push_back(std::move(frame));
            }
            texture = m_rhi->display();
            if (!texture)
                return false;
            width = m_rhi->displayWidth();
            height = m_rhi->displayHeight();
            return true;
        }

    private:
        // ---- recording helpers (game thread) -----------------------------------------------------------------
        bool buildState(const GSPrimitiveBatch &batch, HwState &s)
        {
            const GSDrawState &d = batch.state;
            const GSContext &c = d.context;
            s.fbp = c.frame.fbp;
            s.fbw = std::max<uint32_t>(1u, c.frame.fbw);
            s.fpsm = c.frame.psm;
            s.fbmsk = c.frame.fbmsk;
            s.zbp = c.zbuf.zbp;
            s.zpsm = c.zbuf.psm;
            s.zmsk = c.zbuf.zmask ? 1u : 0u;
            s.scx0 = c.scissor.x0;
            s.scy0 = c.scissor.y0;
            s.scx1 = c.scissor.x1;
            s.scy1 = c.scissor.y1;
            const uint64_t test = c.test;
            s.ate = test & 1u;
            s.atst = (test >> 1) & 7u;
            s.aref = (test >> 4) & 0xFFu;
            s.afail = (test >> 12) & 3u;
            s.zte = (test >> 16) & 1u;
            s.ztst = (test >> 17) & 3u;
            if (s.zte && s.ztst == 0u)
                return false; // ZTST NEVER
            s.abe = d.prim.abe ? 1u : 0u;
            const uint64_t a = c.alpha;
            s.ba = a & 3u;
            s.bb = (a >> 2) & 3u;
            s.bc = (a >> 4) & 3u;
            s.bd = (a >> 6) & 3u;
            s.fix = (a >> 32) & 0xFFu;
            s.iip = d.prim.iip ? 1u : 0u;
            s.fge = d.prim.fge ? 1u : 0u;
            s.fogR = d.fogR;
            s.fogG = d.fogG;
            s.fogB = d.fogB;
            s.tme = d.prim.tme ? 1u : 0u;
            if (s.tme)
            {
                const GSTex0Reg &t = c.tex0;
                s.tfx = t.tfx;
                s.tcc = t.tcc;
                s.fst = d.prim.fst ? 1u : 0u;
                s.linear = d.linearFilter ? 1u : 0u;
                s.tw = d.textureWidth;
                s.th = d.textureHeight;
                const uint64_t cl = c.clamp;
                s.wms = cl & 3u;
                s.wmt = (cl >> 2) & 3u;
                s.minu = (cl >> 4) & 0x3FFu;
                s.maxu = (cl >> 14) & 0x3FFu;
                s.minv = (cl >> 24) & 0x3FFu;
                s.maxv = (cl >> 34) & 0x3FFu;
                if (!bindTexture(d, s))
                    s.tme = 0u;
            }
            else
            {
                s.fst = d.prim.fst ? 1u : 0u;
            }
            return true;
        }

        // Texture source: a render target (GPU) or decoded CPU VRAM (cached).
        bool bindTexture(const GSDrawState &d, HwState &s)
        {
            // [gta-recomp] Consecutive primitives mostly use the same texture: reuse the last binding while the texture
            // registers and everything the key depends on (page generations, CLUT, render targets) are unchanged.
            // Recomputing it per primitive (page-generation hash, CLUT hash, cache lookup) was ~4% of a busy frame.
            const GSTex0Reg &t = d.context.tex0;
            TexBindInput in;
            std::memset(&in, 0, sizeof(in)); // compared bytewise, padding included
            in.version = m_texStateVersion;
            in.tbp0 = t.tbp0;
            in.cbp = t.cbp;
            in.tbw = t.tbw;
            in.psm = t.psm;
            in.cpsm = t.cpsm;
            in.csm = t.csm;
            in.csa = t.csa;
            in.ta0 = d.texa.ta0;
            in.ta1 = d.texa.ta1;
            in.aem = d.texa.aem ? 1u : 0u;
            in.w = d.textureWidth;
            in.h = d.textureHeight;
            if (m_bindCacheValid && std::memcmp(&in, &m_bindIn, sizeof(in)) == 0)
            {
                s.texIsRt = m_bindOut.texIsRt;
                s.rtFbp = m_bindOut.rtFbp;
                s.rtOx = m_bindOut.rtOx;
                s.rtOy = m_bindOut.rtOy;
                s.texKey = m_bindOut.texKey;
                if (!s.texIsRt && m_bindTouchedFrame != m_frameIndex)
                {
                    m_texLru[s.texKey] = m_frameIndex;
                    m_bindTouchedFrame = m_frameIndex;
                }
                return m_bindResult;
            }
            const bool result = bindTextureUncached(d, s);
            m_bindIn = in;
            m_bindOut = s;
            m_bindResult = result;
            m_bindTouchedFrame = m_frameIndex;
            m_bindCacheValid = true;
            return result;
        }

        bool bindTextureUncached(const GSDrawState &d, HwState &s)
        {
            const GSTex0Reg &t = d.context.tex0;
            const uint32_t texStart = t.tbp0 * 256u;
            if (t.psm == GS_PSM_CT32 || t.psm == GS_PSM_CT24 || t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S)
            {
                // Targets can overlap in VRAM (a 640x448 buffer's tail is the next buffer's head): use the target
                // whose base is the closest at or below the texture.
                const RtInfo *best = nullptr;
                uint32_t bestFbp = 0;
                for (const auto &[fbp, rt] : m_rts)
                {
                    const uint32_t rtStart = fbp * 8192u;
                    const uint32_t rtEnd = rtStart + rt.pageRows * rt.fbw * 8192u;
                    if (texStart >= rtStart && texStart < rtEnd && (!best || fbp > bestFbp))
                    {
                        best = &rt;
                        bestFbp = fbp;
                    }
                }
                if (best)
                {
                    const uint32_t page = (texStart - bestFbp * 8192u) / 8192u;
                    s.texIsRt = 1u;
                    s.rtFbp = bestFbp;
                    s.rtOx = static_cast<uint16_t>((page % best->fbw) * 64u);
                    s.rtOy = static_cast<uint16_t>((page / best->fbw) * 32u);
                    return true;
                }
            }
            uint32_t first, last;
            pageRange(t.tbp0, t.tbw, t.psm, 0, 0, d.textureWidth, d.textureHeight, first, last);
            uint64_t key = 0x1234567ull;
            key = mix64(key, t.tbp0 | (uint64_t(t.tbw) << 16) | (uint64_t(t.psm) << 24) | (uint64_t(d.textureWidth) << 32) |
                                 (uint64_t(d.textureHeight) << 48));
            key = mix64(key, uint64_t(d.texa.ta0) | (uint64_t(d.texa.ta1) << 8) | (uint64_t(d.texa.aem) << 16));
            const bool paletted = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H || t.psm == GS_PSM_T4HL ||
                                  t.psm == GS_PSM_T4HH;
            if (paletted)
                key = mix64(key, m_cpu.ClutHash(t));
            // [gta-recomp] Texture = an intact whole upload (same base/format/width, covers it, pages untouched since):
            // key it by the upload's content instead of the page generations, so a texture that is streamed into a
            // shared VRAM area every frame finds its decoded GPU copy again.
            bool contentKeyed = false;
            if (auto up = m_uploadAt.find(t.tbp0); up != m_uploadAt.end())
            {
                const UploadInfo &u = up->second;
                if (u.dpsm == t.psm && u.dbw == t.tbw && u.x == 0u && u.y == 0u && u.w >= d.textureWidth && u.h >= d.textureHeight &&
                    first >= u.first && last <= u.first + u.gens.size())
                {
                    contentKeyed = true;
                    for (uint32_t p = first; p < last && contentKeyed; ++p)
                        contentKeyed = m_pageGen[p] == u.gens[p - u.first];
                    if (contentKeyed)
                        key = mix64(mix64(key ^ 0xC0FFEEull, u.hash), u.w | (uint64_t(u.h) << 16));
                }
            }
            if (!contentKeyed)
                for (uint32_t p = first; p < last && p < kPages; ++p)
                    key = mix64(key, m_pageGen[p]);
            s.texKey = key;
            auto it = m_texLru.find(key);
            if (it != m_texLru.end())
            {
                it->second = m_frameIndex;
                return true;
            }
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_loggedTextures < 60u)
            {
                ++m_loggedTextures;
                std::fprintf(stderr, "[gs-hw] texture tbp=%u tbw=%u psm=%u %ux%u cbp=%u cpsm=%u csm=%u csa=%u tfx=%u\n", t.tbp0, t.tbw, t.psm,
                             d.textureWidth, d.textureHeight, t.cbp, t.cpsm, t.csm, t.csa, t.tfx);
            }
            Frame &f = *m_frame;
            TexCreate tc;
            tc.key = key;
            tc.w = d.textureWidth;
            tc.h = d.textureHeight;
            if (materializeRectKeepTransfer(t.psm, t.tbp0, t.tbw, 0u, 0u, d.textureWidth, d.textureHeight))
                ++m_lazyWhy[1];
            m_cpu.DecodeTexture(d, tc.pixels);
            f.cmds.push_back({CmdType::TexCreate, static_cast<uint32_t>(f.textures.size())});
            f.textures.push_back(std::move(tc));
            m_texLru[key] = m_frameIndex;
            if (m_texLru.size() > kMaxTextures)
                evictTextures();
            return true;
        }

        void evictTextures()
        {
            std::vector<std::pair<uint64_t, uint64_t>> byAge(m_texLru.begin(), m_texLru.end());
            std::sort(byAge.begin(), byAge.end(), [](auto &a, auto &b) { return a.second < b.second; });
            Frame &f = *m_frame;
            for (size_t i = 0; i < byAge.size() / 4; ++i)
            {
                if (byAge[i].second == m_frameIndex)
                    break;
                f.cmds.push_back({CmdType::TexDelete, static_cast<uint32_t>(f.texDeletes.size())});
                f.texDeletes.push_back(byAge[i].first);
                ++m_texStateVersion;
                m_texLru.erase(byAge[i].first);
            }
        }

        // [gta-recomp] scissorMaxY: the GS writes nothing below the scissor, so a draw whose vertices reach further
        // (VCS's sky gradient goes to y 738 on a 448-line screen) must not grow the target past it. An oversized target
        // claims VRAM pages it never uses, and an upload there (the game puts a texture into the Z-buffer area at
        // night) dropped the whole picture: the evening black frames.
        void noteTarget(uint32_t fbp, uint32_t fbw, uint32_t psm, const HwVertex *v, uint32_t n, uint32_t scissorMaxY)
        {
            auto found = m_rts.find(fbp);
            if (found == m_rts.end())
                ++m_texStateVersion; // a new render target: textures there now come from it
            RtInfo &rt = found != m_rts.end() ? found->second : m_rts[fbp];
            rt.fbw = std::max(rt.fbw, fbw);
            rt.psm = psm;
            float maxY = 0.0f;
            for (uint32_t i = 0; i < n; ++i)
                maxY = std::max(maxY, v[i].y);
            maxY = std::min(maxY, static_cast<float>(scissorMaxY) + 1.0f); // vertex y is an edge: pixels up to maxY - 1
            uint32_t pw, ph;
            pageSize(psm, pw, ph);
            const uint32_t lastLine = static_cast<uint32_t>(std::max(maxY - 1.0f, 0.0f));
            const uint32_t rows = std::min<uint32_t>(32u, lastLine / ph + 1u);
            rt.recentRows = std::max(rt.recentRows, rows);
            if (rows > rt.pageRows)
            {
                ++m_texStateVersion;
                static int logged = 0;
                if (rows > 15u && logged < 40 && logDrops())
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] rt fbp=%u fbw=%u grows to %u page rows: draw of %u vertices, max y %.0f, scissor %u\n", fbp,
                                 fbw, rows, n, maxY, scissorMaxY);
                }
            }
            rt.pageRows = std::max(rt.pageRows, std::max<uint32_t>(rows, 1u));
        }

        void touchTransferDest()
        {
            ++m_texStateVersion;
            const GSTransferCommand &t = m_transfer;
            uint32_t first, last;
            pageRange(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                      t.trxreg.rrh, first, last);
            for (uint32_t p = first; p < last && p < kPages; ++p)
                ++m_pageGen[p];
            std::vector<uint16_t> written; // [gta-recomp] blocks this upload wrote (for patchTarget), filled on demand
            bool writtenKnown = false;
            auto uploadBlocks = [&]() -> const std::vector<uint16_t> & {
                if (!writtenKnown)
                {
                    rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                               t.trxreg.rrh, written, nullptr);
                    writtenKnown = true;
                }
                return written;
            };
            // An upload over a render target: copy the uploaded rect into it (same base), copy the blocks it wrote
            // (other base), or drop the target (formats without a block layout).
            for (auto it = m_rts.begin(); it != m_rts.end();)
            {
                const uint32_t rtFirst = it->first * 32u / 32u * 1u; // fbp is in 8 KiB pages already
                const uint32_t rtLast = it->first + it->second.pageRows * it->second.fbw;
                if (first < rtLast && last > rtFirst)
                {
                    Frame &f = *m_frame;
                    if (t.bitbltbuf.dbp == it->first * 32u && t.bitbltbuf.dbw == it->second.fbw && t.trxreg.rrw && t.trxreg.rrh)
                    {
                        RtUpload up;
                        up.fbp = it->first;
                        up.fbw = it->second.fbw;
                        up.x = t.trxpos.dsax;
                        up.y = t.trxpos.dsay;
                        up.w = std::min<uint32_t>(t.trxreg.rrw, 2048u);
                        up.h = std::min<uint32_t>(t.trxreg.rrh, 2048u);
                        up.pixels.resize(static_cast<size_t>(up.w) * up.h);
                        const uint32_t psm = it->second.psm;
                        for (uint32_t y = 0; y < up.h; ++y)
                            for (uint32_t x = 0; x < up.w; ++x)
                            {
                                uint32_t v = m_cpu.ReadVram(psm, it->first * 32u, up.fbw, up.x + x, up.y + y);
                                if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
                                    v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) |
                                        ((v & 0x8000u) ? 0x80000000u : 0u);
                                up.pixels[static_cast<size_t>(y) * up.w + x] = v;
                            }
                        f.cmds.push_back({CmdType::RtUpload, static_cast<uint32_t>(f.rtUploads.size())});
                        f.rtUploads.push_back(std::move(up));
                        ++it;
                    }
                    else if (patchTargets() && patchTarget(it->first, it->second, uploadBlocks()))
                    {
                        ++it; // [gta-recomp] the written blocks were copied into the target
                    }
                    else
                    {
                        static int logged = 0; // [gta-recomp] which uploads drop render targets (evening black frames)
                        if (logged < 200 && logDrops())
                        {
                            ++logged;
                            std::fprintf(stderr,
                                         "[gs-hw] upload dbp=%u dbw=%u psm=%u %ux%u at %u,%u (pages %u-%u) drops rt fbp=%u fbw=%u rows=%u "
                                         "(pages %u-%u)\n",
                                         t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxreg.rrw, t.trxreg.rrh, t.trxpos.dsax,
                                         t.trxpos.dsay, first, last, it->first, it->second.fbw, it->second.pageRows, rtFirst, rtLast);
                        }
                        f.cmds.push_back({CmdType::RtDrop, static_cast<uint32_t>(f.rtDrops.size())});
                        f.rtDrops.push_back(it->first);
                        it = m_rts.erase(it);
                    }
                }
                else
                {
                    ++it;
                }
            }
        }

        // ---- state --------------------------------------------------------------------------------------------
        struct RtInfo
        {
            uint32_t fbw = 1, psm = 0, pageRows = 1;
            uint32_t recentRows = 0; // [gta-recomp] rows drawn since the last trim (shrinkTargets)
        };

        // [gta-recomp] A target grows to the tallest draw ever made into it (VCS clears all of VRAM through FBP 0 at
        // boot: 1472 lines); every 300 frames trim each target to what was drawn in that time, so it stops claiming
        // pages other data lives in (an upload there would drop or rewrite it for nothing).
        void shrinkTargets()
        {
            if (m_frameIndex % 300u != 0u)
                return;
            for (auto &[fbp, rt] : m_rts)
            {
                if (rt.recentRows && rt.recentRows < rt.pageRows)
                {
                    if (logDrops())
                        std::fprintf(stderr, "[gs-hw] rt fbp=%u trimmed %u -> %u page rows\n", fbp, rt.pageRows, rt.recentRows);
                    rt.pageRows = rt.recentRows;
                    ++m_texStateVersion;
                }
                rt.recentRows = 0;
            }
        }

        // [gta-recomp] Block (8x8 / 16x8 pixels) position inside a page for each of its 32 blocks, per format; false for
        // formats GSMem::RectBlocks does not know.
        bool blockLayout(uint32_t psm, std::array<std::pair<uint8_t, uint8_t>, 32> &where, uint32_t &bw, uint32_t &bh)
        {
            uint32_t pw, ph;
            pageSize(psm, pw, ph);
            bw = pw / 8u;
            bh = ph / 4u;
            if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
            {
                bw = 16u;
                bh = 8u;
            }
            if (pw / bw * (ph / bh) != 32u)
                return false;
            std::vector<uint16_t> touched;
            for (uint32_t by = 0; by < ph / bh; ++by)
                for (uint32_t bx = 0; bx < pw / bw; ++bx)
                {
                    touched.clear();
                    if (!GSMem::RectBlocks(psm, 0u, pw / 64u, bx * bw, by * bh, bw, bh, touched, nullptr) || touched.size() != 1u ||
                        touched[0] >= 32u)
                        return false;
                    where[touched[0]] = {static_cast<uint8_t>(bx), static_cast<uint8_t>(by)};
                }
            return true;
        }

        // [gta-recomp] PS2X_HW_PATCH_TARGETS=1 turns patchTarget on. Off by default: VCS streams textures into the VRAM
        // around its bloom buffer (FBP 420) every frame - ~6000 small uploads a second - and patching each one costs
        // more than the game frame (3 fps). Doing it right needs lazy patching (dirty blocks resolved only when the
        // target is read); until then overlapped targets are dropped as before.
        static bool patchTargets()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_HW_PATCH_TARGETS");
                return e && *e && *e != '0';
            }();
            return on;
        }

        // [gta-recomp] An upload that overlaps a render target at another base: copy just the blocks it wrote into the
        // target (as the GS would - the rest of the target keeps what was drawn), read back from CPU VRAM in the
        // target's own layout. VCS puts a CLUT inside its bloom buffer and a texture in the Z-buffer area each frame;
        // dropping the whole target there lost the picture. False when the format is not supported (then drop).
        bool patchTarget(uint32_t fbp, const RtInfo &rt, const std::vector<uint16_t> &written)
        {
            std::array<std::pair<uint8_t, uint8_t>, 32> where{};
            uint32_t bw = 0, bh = 0, pw = 0, ph = 0;
            if (!blockLayout(rt.psm, where, bw, bh))
                return false;
            pageSize(rt.psm, pw, ph);
            const uint32_t firstBlock = fbp * 32u, lastBlock = (fbp + rt.pageRows * rt.fbw) * 32u;
            std::vector<uint32_t> perPage(rt.pageRows * rt.fbw, 0u); // bitmask of written blocks per target page
            uint32_t count = 0;
            for (uint16_t b : written)
                if (b >= firstBlock && b < lastBlock)
                {
                    perPage[(b - firstBlock) / 32u] |= 1u << ((b - firstBlock) % 32u);
                    ++count;
                }
            if (!count)
                return true;
            // Large overwrites (VCS: a 512x512 texture into the Z-buffer area every frame, seen through the Z-clear view
            // FBP 280) are dropped as before: reading them back per pixel cost ~200 ms a frame, and a target mostly
            // overwritten has little worth keeping. Small ones (a CLUT inside the bloom buffer) are patched.
            if (count > 256u)
                return false;
            static uint64_t patches = 0, blocks = 0;
            ++patches;
            blocks += count;
            if (logDrops() && (patches & (patches - 1)) == 0) // powers of two
                std::fprintf(stderr, "[gs-hw] patched %llu uploads into targets (%llu blocks; last: fbp %u, %u blocks)\n",
                             static_cast<unsigned long long>(patches), static_cast<unsigned long long>(blocks), fbp, count);
            Frame &f = *m_frame;
            auto emit = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                RtUpload up;
                up.fbp = fbp;
                up.fbw = rt.fbw;
                up.x = x;
                up.y = y;
                up.w = w;
                up.h = h;
                up.pixels.resize(static_cast<size_t>(w) * h);
                for (uint32_t yy = 0; yy < h; ++yy)
                    for (uint32_t xx = 0; xx < w; ++xx)
                    {
                        uint32_t v = m_cpu.ReadVram(rt.psm, fbp * 32u, rt.fbw, x + xx, y + yy);
                        if (rt.psm == GS_PSM_CT16 || rt.psm == GS_PSM_CT16S)
                            v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) |
                                ((v & 0x8000u) ? 0x80000000u : 0u);
                        up.pixels[static_cast<size_t>(yy) * w + xx] = v;
                    }
                f.cmds.push_back({CmdType::RtUpload, static_cast<uint32_t>(f.rtUploads.size())});
                f.rtUploads.push_back(std::move(up));
            };
            for (uint32_t pg = 0; pg < perPage.size(); ++pg)
            {
                const uint32_t mask = perPage[pg];
                if (!mask)
                    continue;
                const uint32_t px = (pg % rt.fbw) * pw, py = (pg / rt.fbw) * ph;
                if (mask == 0xFFFFFFFFu)
                {
                    emit(px, py, pw, ph);
                    continue;
                }
                for (uint32_t k = 0; k < 32u; ++k)
                    if (mask & (1u << k))
                        emit(px + where[k].first * bw, py + where[k].second * bh, bw, bh);
            }
            return true;
        }
        static constexpr uint32_t kPages = 512u; // 4 MiB / 8 KiB
        static constexpr size_t kMaxTextures = 3000u;
        static constexpr size_t kMaxMeshes = 12000u; // [gta-recomp] native draws' vertex data (~3-10 KB each)

        uint32_t m_scale;
        GSCpuBackend m_cpu;
        GSTransferCommand m_transfer{};
        std::unique_ptr<Frame> m_frame;
        uint64_t m_frameIndex = 0;
        uint32_t m_pageGen[kPages]{};
        struct UploadRecord // [gta-recomp] last whole-transfer upload per destination (see UploadImage)
        {
            uint64_t hash = 0;
            uint32_t first = 0;
            std::vector<uint32_t> gens;
        };
        std::unordered_map<uint64_t, UploadRecord> m_uploads;
        struct UploadInfo // [gta-recomp] last whole upload per base pointer (content-keyed textures)
        {
            uint64_t hash = 0;
            uint32_t dbw = 0, dpsm = 0, x = 0, y = 0, w = 0, h = 0, first = 0;
            std::vector<uint32_t> gens;
        };
        std::unordered_map<uint32_t, UploadInfo> m_uploadAt;
        uint64_t m_skippedUploads = 0;
        std::unordered_map<uint64_t, uint64_t> m_texLru;  // key -> last frame used
        std::unordered_map<uint64_t, uint64_t> m_meshLru; // [gta-recomp] native draws' meshes: key -> last frame used
        std::unordered_map<uint32_t, RtInfo> m_rts;       // fbp -> extent (game thread's view)
        struct TexBindInput // [gta-recomp] bindTexture cache (see there)
        {
            uint64_t version;
            uint32_t tbp0, cbp;
            uint16_t w, h;
            uint8_t tbw, psm, cpsm, csm, csa, ta0, ta1, aem;
        };
        uint64_t m_texStateVersion = 1;
        TexBindInput m_bindIn{};
        HwState m_bindOut{};
        bool m_bindResult = false, m_bindCacheValid = false;
        uint64_t m_bindTouchedFrame = 0;
        std::deque<PendingUpload> m_pendingUploads;       // [gta-recomp] lazy uploads, in submission order
        uint64_t m_pendingFrontId = 0;                    // id of m_pendingUploads.front()
        std::vector<uint16_t> m_blockPending = std::vector<uint16_t>(kBlocks, 0u); // queued uploads touching each block
        std::vector<std::vector<uint64_t>> m_blockWriters = std::vector<std::vector<uint64_t>>(kBlocks); // their ids
        std::vector<uint16_t> m_scratchTouched, m_scratchFull, m_scratchRead;
        std::vector<uint32_t> m_wantStamp = std::vector<uint32_t>(kBlocks, 0u);
        uint32_t m_wantGeneration = 0;
        std::vector<uint8_t> m_scratchTake;
        size_t m_pendingBytes = 0;
        uint64_t m_lazyDeferred = 0, m_lazyWritten = 0, m_lazyDropped = 0;
        uint64_t m_lazyWhy[4]{}; // what forced queued uploads out: clut load, texture decode, direct upload, other
        std::unordered_map<uint32_t, DepthInfo> m_depthBufs; // zbp -> format / width

        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::deque<std::unique_ptr<Frame>> m_queue;
        std::vector<std::unique_ptr<Frame>> m_freeFrames; // executed frames for reuse (m_queueMutex)

        // main thread
        std::unique_ptr<RhiExecutor> m_rhi; // [gta-recomp] the renderer (D3D11)
        uint64_t m_executedFrames = 0;
        uint32_t m_loggedTransfers = 0, m_loggedTextures = 0;
        bool m_dumped = false;
        bool m_f12Down = false; // [gta-recomp]
        bool m_dumpPending = false; // [gta-recomp] F12 / PS2X_HW_DUMP waiting for a frame with draws
        int m_dumpBatchesLeft = 0;  // [gta-recomp] PS2X_HW_DUMP_BATCHES
        uint32_t m_3dHistory = ~0u; // [gta-recomp] showing3d: had3d of the last 32 frames, newest in bit 0
        std::atomic<bool> m_showing3d{true}; // main thread writes, the game thread's Submit reads

    public:
        bool showing3d() const override { return m_showing3d.load(std::memory_order_relaxed); }
        std::string lastFrameInfo() const override
        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            return m_lastInfo;
        }
        mutable std::mutex m_infoMutex; // [gta-recomp] lastFrameInfo
        std::string m_lastInfo;
    };
}

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options)
{
    return std::make_unique<HwBackend>(options);
}
