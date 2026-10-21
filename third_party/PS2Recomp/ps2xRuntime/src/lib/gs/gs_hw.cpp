// [gta-recomp] Hardware (OpenGL 3.3) GS renderer. See runtime/gs/gs_hw.h.
#include "runtime/gs/gs_hw.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

    // Everything that selects GL state for a draw. Compared bytewise to merge consecutive primitives.
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
        uint64_t privRegs[15]{};
        // Display from CPU VRAM when no render target holds the displayed buffer.
        std::vector<uint32_t> cpuDisplay;
        uint32_t cpuDisplayW = 0, cpuDisplayH = 0;
        bool displayFromRt = false;
        uint32_t displayFbp = 0, displayX = 0, displayY = 0, displayW = 0, displayH = 0;
    };

    // GS page geometry per PSM: 8 KiB pages.
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
    // GL (main thread)
    // ------------------------------------------------------------------------------------------------------
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;
    using GLboolean = unsigned char;
    using GLfloat = float;
    using GLchar = char;
    using GLsizeiptr = intptr_t;
    using GLintptr = intptr_t;
    using GLbitfield = unsigned int;

    constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
                     GL_FLOAT = 0x1406, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                     GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
                     GL_CLAMP_TO_EDGE = 0x812F, GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8,
                     GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00,
                     GL_DEPTH_COMPONENT32F = 0x8CAC, GL_DEPTH_COMPONENT = 0x1902, GL_ARRAY_BUFFER = 0x8892,
                     GL_STREAM_DRAW = 0x88E0, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
                     GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_TRIANGLES = 0x0004, GL_LINES = 0x0001,
                     GL_POINTS = 0x0000, GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11,
                     GL_CULL_FACE = 0x0B44, GL_ALWAYS = 0x0207, GL_GEQUAL = 0x0206, GL_GREATER = 0x0204,
                     GL_NEVER = 0x0200, GL_ONE = 1, GL_ZERO = 0, GL_SRC1_ALPHA = 0x8589,
                     GL_ONE_MINUS_SRC1_ALPHA = 0x88FB, GL_FUNC_ADD = 0x8006, GL_FUNC_SUBTRACT = 0x800A,
                     GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
                     GL_TEXTURE0 = 0x84C0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_UNPACK_ALIGNMENT = 0x0CF5,
                     GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_VIEWPORT = 0x0BA2,
                     GL_PROGRAM_POINT_SIZE = 0x8642, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305;

#define GLFN(ret, name, args) using PFN_##name = ret(__stdcall *) args; PFN_##name name = nullptr;
    struct Gl
    {
        GLFN(void, glGenTextures, (GLsizei, GLuint *))
        GLFN(void, glDeleteTextures, (GLsizei, const GLuint *))
        GLFN(void, glBindTexture, (GLenum, GLuint))
        GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
        GLFN(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
        GLFN(void, glTexParameteri, (GLenum, GLenum, GLint))
        GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
        GLFN(void, glScissor, (GLint, GLint, GLsizei, GLsizei))
        GLFN(void, glEnable, (GLenum))
        GLFN(void, glDisable, (GLenum))
        GLFN(void, glDepthFunc, (GLenum))
        GLFN(void, glDepthMask, (GLboolean))
        GLFN(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
        GLFN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
        GLFN(void, glClearDepth, (double))
        GLFN(void, glClear, (GLbitfield))
        GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei))
        GLFN(void, glPixelStorei, (GLenum, GLint))
        GLFN(void, glGetIntegerv, (GLenum, GLint *))
        GLFN(void, glGenFramebuffers, (GLsizei, GLuint *))
        GLFN(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
        GLFN(void, glBindFramebuffer, (GLenum, GLuint))
        GLFN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
        GLFN(GLenum, glCheckFramebufferStatus, (GLenum))
        GLFN(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
        GLFN(void, glActiveTexture, (GLenum))
        GLFN(void, glGenBuffers, (GLsizei, GLuint *))
        GLFN(void, glBindBuffer, (GLenum, GLuint))
        GLFN(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
        GLFN(void, glGenVertexArrays, (GLsizei, GLuint *))
        GLFN(void, glBindVertexArray, (GLuint))
        GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
        GLFN(void, glEnableVertexAttribArray, (GLuint))
        GLFN(GLuint, glCreateShader, (GLenum))
        GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
        GLFN(void, glCompileShader, (GLuint))
        GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint *))
        GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
        GLFN(GLuint, glCreateProgram, ())
        GLFN(void, glAttachShader, (GLuint, GLuint))
        GLFN(void, glLinkProgram, (GLuint))
        GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint *))
        GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
        GLFN(void, glUseProgram, (GLuint))
        GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar *))
        GLFN(void, glUniform1i, (GLint, GLint))
        GLFN(void, glUniform1f, (GLint, GLfloat))
        GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
        GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
        GLFN(void, glUniform2i, (GLint, GLint, GLint))
        GLFN(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint))
        GLFN(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
        GLFN(void, glBlendEquationSeparate, (GLenum, GLenum))
        GLFN(void, glBindFragDataLocationIndexed, (GLuint, GLuint, GLuint, const GLchar *))

        bool load()
        {
#if defined(_WIN32)
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            using WglGet = PROC(WINAPI *)(LPCSTR);
            auto wglGet = reinterpret_cast<WglGet>(GetProcAddress(gl, "wglGetProcAddress"));
            auto get = [&](const char *n) -> void * {
                void *p = wglGet ? reinterpret_cast<void *>(wglGet(n)) : nullptr;
                if (!p || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) || p == reinterpret_cast<void *>(3) ||
                    p == reinterpret_cast<void *>(-1))
                    p = reinterpret_cast<void *>(GetProcAddress(gl, n));
                return p;
            };
#define GLLOAD(name) name = reinterpret_cast<PFN_##name>(get(#name)); if (!name) { std::fprintf(stderr, "[gs-hw] missing GL function %s\n", #name); return false; }
            GLLOAD(glGenTextures) GLLOAD(glDeleteTextures) GLLOAD(glBindTexture) GLLOAD(glTexImage2D) GLLOAD(glTexSubImage2D)
            GLLOAD(glTexParameteri) GLLOAD(glViewport) GLLOAD(glScissor) GLLOAD(glEnable) GLLOAD(glDisable) GLLOAD(glDepthFunc)
            GLLOAD(glDepthMask) GLLOAD(glColorMask) GLLOAD(glClearColor) GLLOAD(glClearDepth) GLLOAD(glClear) GLLOAD(glDrawArrays)
            GLLOAD(glPixelStorei) GLLOAD(glGetIntegerv) GLLOAD(glGenFramebuffers) GLLOAD(glDeleteFramebuffers) GLLOAD(glBindFramebuffer)
            GLLOAD(glFramebufferTexture2D) GLLOAD(glCheckFramebufferStatus) GLLOAD(glBlitFramebuffer) GLLOAD(glActiveTexture)
            GLLOAD(glGenBuffers) GLLOAD(glBindBuffer) GLLOAD(glBufferData) GLLOAD(glGenVertexArrays) GLLOAD(glBindVertexArray)
            GLLOAD(glVertexAttribPointer) GLLOAD(glEnableVertexAttribArray) GLLOAD(glCreateShader) GLLOAD(glShaderSource)
            GLLOAD(glCompileShader) GLLOAD(glGetShaderiv) GLLOAD(glGetShaderInfoLog) GLLOAD(glCreateProgram) GLLOAD(glAttachShader)
            GLLOAD(glLinkProgram) GLLOAD(glGetProgramiv) GLLOAD(glGetProgramInfoLog) GLLOAD(glUseProgram) GLLOAD(glGetUniformLocation)
            GLLOAD(glUniform1i) GLLOAD(glUniform1f) GLLOAD(glUniform2f) GLLOAD(glUniform3f) GLLOAD(glUniform2i) GLLOAD(glUniform4i)
            GLLOAD(glBlendFuncSeparate) GLLOAD(glBlendEquationSeparate) GLLOAD(glBindFragDataLocationIndexed)
#undef GLLOAD
            return true;
#else
            return false;
#endif
        }
    };
#undef GLFN

    const char *kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aStq;
layout(location = 2) in vec4 aCol;
layout(location = 3) in float aFog;
uniform vec2 uTargetSize;
noperspective out vec3 vStq;
noperspective out vec4 vCol;
flat out vec4 vColFlat;
noperspective out float vFog;
void main()
{
    vec2 p = aPos.xy / uTargetSize * 2.0 - 1.0;
    gl_Position = vec4(p, aPos.z * 2.0 - 1.0, 1.0);
    gl_PointSize = 1.0;
    vStq = aStq;
    vCol = aCol;
    vColFlat = aCol;
    vFog = aFog;
}
)";

    const char *kFragmentShader = R"(#version 330 core
noperspective in vec3 vStq;
noperspective in vec4 vCol;
flat in vec4 vColFlat;
noperspective in float vFog;
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;
uniform int uTme, uTfx, uTcc, uFst, uLinear, uIip, uFge;
uniform int uAte, uAtst, uAref, uAfailPass;
uniform ivec2 uTexSize;
uniform ivec2 uWrap;
uniform ivec4 uRegion;
uniform int uTexIsRt;
uniform vec2 uRtOrigin;
uniform float uRtScale;
uniform sampler2D uTex;
uniform vec3 uFogCol;
uniform int uBlendC;     // 0 As, 1 Ad, 2 FIX
uniform float uFix;
uniform int uPremul;     // 0: none, 1: rgb *= C', 2: rgb *= (1 + C')
int wrapc(int c, int size, int mode, int mn, int mx)
{
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return clamp(c, mn, mx);
    return (c & mn) | mx;
}
vec4 fetchTexel(ivec2 t)
{
    t.x = wrapc(t.x, uTexSize.x, uWrap.x, uRegion.x, uRegion.y);
    t.y = wrapc(t.y, uTexSize.y, uWrap.y, uRegion.z, uRegion.w);
    if (uTexIsRt == 1)
    {
        vec2 p = (vec2(t) + uRtOrigin + vec2(0.5)) * uRtScale;
        return texelFetch(uTex, ivec2(p), 0) * 255.0;
    }
    return texelFetch(uTex, t, 0) * 255.0;
}
vec4 sampleTex(vec2 uv)
{
    if (uLinear == 0)
        return fetchTexel(ivec2(floor(uv)));
    vec2 p = uv - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 c00 = fetchTexel(i), c10 = fetchTexel(i + ivec2(1, 0));
    vec4 c01 = fetchTexel(i + ivec2(0, 1)), c11 = fetchTexel(i + ivec2(1, 1));
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}
void main()
{
    vec4 vc = floor((uIip == 1 ? vCol : vColFlat) * 255.0 + 0.5);
    vec4 c = vc;
    if (uTme == 1)
    {
        vec2 uv = (uFst == 1) ? vStq.xy : (vStq.xy / vStq.z) * vec2(uTexSize);
        vec4 t = sampleTex(uv);
        if (uTfx == 0) { c.rgb = t.rgb * vc.rgb / 128.0; c.a = (uTcc == 1) ? t.a * vc.a / 128.0 : vc.a; }
        else if (uTfx == 1) { c.rgb = t.rgb; c.a = (uTcc == 1) ? t.a : vc.a; }
        else if (uTfx == 2) { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = (uTcc == 1) ? t.a + vc.a : vc.a; }
        else { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = (uTcc == 1) ? t.a : vc.a; }
    }
    c = clamp(floor(c), 0.0, 255.0);
    if (uFge == 1)
    {
        float f = floor(vFog * 255.0 + 0.5);
        c.rgb = floor((c.rgb * f + uFogCol * (255.0 - f)) / 255.0);
    }
    bool pass = true;
    if (uAte == 1)
    {
        float a = c.a, r = float(uAref);
        if (uAtst == 0) pass = false;
        else if (uAtst == 2) pass = a < r;
        else if (uAtst == 3) pass = a <= r;
        else if (uAtst == 4) pass = a == r;
        else if (uAtst == 5) pass = a >= r;
        else if (uAtst == 6) pass = a > r;
        else if (uAtst == 7) pass = a != r;
    }
    if (uAfailPass == 0 && !pass) discard;
    if (uAfailPass == 1 && pass) discard;
    float cf = (uBlendC == 0) ? c.a / 128.0 : ((uBlendC == 2) ? uFix / 128.0 : 1.0);
    vec3 rgb = c.rgb;
    if (uPremul == 1) rgb *= cf;
    else if (uPremul == 2) rgb *= (1.0 + cf);
    oColor = vec4(rgb / 255.0, c.a / 255.0);
    oBlend = vec4(min(cf, 1.0));
}
)";

    const char *kBlitFragmentShader = R"(#version 330 core
noperspective in vec3 vStq;
noperspective in vec4 vCol;
flat in vec4 vColFlat;
noperspective in float vFog;
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;
uniform sampler2D uTex;
void main()
{
    oColor = vec4(texture(uTex, vStq.xy).rgb, 1.0);
    oBlend = vec4(1.0);
}
)";

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
            noteTarget(s.fbp, s.fbw, s.fpsm, out, n);
            Frame &f = *m_frame;
            if (f.draws.empty() || f.cmds.empty() || f.cmds.back().type != CmdType::Draw ||
                std::memcmp(&f.draws.back().s, &s, sizeof(HwState)) != 0)
            {
                f.cmds.push_back({CmdType::Draw, static_cast<uint32_t>(f.draws.size())});
                f.draws.push_back({s, static_cast<uint32_t>(f.vertices.size()), 0u});
            }
            f.vertices.insert(f.vertices.end(), out, out + n);
            f.draws.back().count += n;
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override { m_cpu.LoadClut(tex0, texclut); }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            m_transfer = command;
            m_cpu.BeginTransfer(command);
            if (command.direction == 2u) // local -> local: destination pages change
                touchTransferDest();
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            m_cpu.UploadImage(data, sizeBytes);
            touchTransferDest();
        }

        void Flush() override { m_cpu.Flush(); }
        void TextureFlush() override { m_cpu.TextureFlush(); }
        void Sync(GSSyncReason reason) override { m_cpu.Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &request) override { return m_cpu.Present(request); }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
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
            return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            return m_cpu.ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            m_cpu.WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_cpu.SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_cpu.GetTransferSnapshot(); }

        // ---- frame hand-over (game thread) -------------------------------------------------------------------
        void endFrame(const uint64_t (&privRegs)[15]) override
        {
            Frame &f = *m_frame;
            std::memcpy(f.privRegs, privRegs, sizeof(f.privRegs));
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
            ++m_frameIndex;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                // Don't let the game run more than a few frames ahead of the main thread.
                m_queueCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return m_queue.size() < 3u; });
                m_queue.push_back(std::move(m_frame));
            }
            m_frame = std::make_unique<Frame>();
        }

        GSHwPresenter &presenter() override { return *this; }

        // ---- GSHwPresenter (main thread) ---------------------------------------------------------------------
        bool executeAndGetDisplay(uint32_t &glTexture, uint32_t &width, uint32_t &height) override
        {
            if (!m_glReady && !initGl())
                return false;
            std::deque<std::unique_ptr<Frame>> frames;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                frames.swap(m_queue);
            }
            m_queueCv.notify_all();
            if (!frames.empty())
            {
                GLint prevFbo = 0, prevViewport[4] = {};
                m_gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                m_gl.glGetIntegerv(GL_VIEWPORT, prevViewport);
                for (auto &frame : frames)
                    execute(*frame);
                composeDisplay(*frames.back());
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
                        std::fprintf(stderr, "[gs-hw] %llu frames, %llu draws, %llu verts, %llu tex | display fbp=%u %ux%u at %u,%u fromRt=%d cpu=%zu | rts:",
                                     (unsigned long long)nFrames, (unsigned long long)nDraws, (unsigned long long)nVerts,
                                     (unsigned long long)nTex, l.displayFbp, l.displayW, l.displayH, l.displayX, l.displayY,
                                     l.displayFromRt ? 1 : 0, l.cpuDisplay.size());
                        for (const auto &[fbp, rt] : m_glRts)
                            std::fprintf(stderr, " %u(%ux%u)", fbp, rt.w, rt.h);
                        std::fprintf(stderr, " pmode=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                                     (unsigned long long)l.privRegs[0], (unsigned long long)l.privRegs[7], (unsigned long long)l.privRegs[8],
                                     (unsigned long long)l.privRegs[9], (unsigned long long)l.privRegs[10]);
                        nFrames = nDraws = nVerts = nTex = 0;
                        since = now;
                    }
                }
                restoreRaylibState(static_cast<GLuint>(prevFbo), prevViewport);
            }
            if (!m_displayTex)
                return false;
            glTexture = m_displayTex;
            width = m_displayW * m_scale;
            height = m_displayH * m_scale;
            return true;
        }

        void releaseGl() override {}

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
            const GSTex0Reg &t = d.context.tex0;
            const uint32_t texStart = t.tbp0 * 256u;
            for (const auto &[fbp, rt] : m_rts)
            {
                const uint32_t rtStart = fbp * 8192u;
                const uint32_t rtEnd = rtStart + rt.pageRows * rt.fbw * 8192u;
                if (texStart >= rtStart && texStart < rtEnd &&
                    (t.psm == GS_PSM_CT32 || t.psm == GS_PSM_CT24 || t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S))
                {
                    const uint32_t page = (texStart - rtStart) / 8192u;
                    s.texIsRt = 1u;
                    s.rtFbp = fbp;
                    s.rtOx = static_cast<uint16_t>((page % rt.fbw) * 64u);
                    s.rtOy = static_cast<uint16_t>((page / rt.fbw) * 32u);
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
            for (uint32_t p = first; p < last && p < kPages; ++p)
                key = mix64(key, m_pageGen[p]);
            s.texKey = key;
            auto it = m_texLru.find(key);
            if (it != m_texLru.end())
            {
                it->second = m_frameIndex;
                return true;
            }
            Frame &f = *m_frame;
            TexCreate tc;
            tc.key = key;
            tc.w = d.textureWidth;
            tc.h = d.textureHeight;
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
                m_texLru.erase(byAge[i].first);
            }
        }

        void noteTarget(uint32_t fbp, uint32_t fbw, uint32_t psm, const HwVertex *v, uint32_t n)
        {
            RtInfo &rt = m_rts[fbp];
            rt.fbw = std::max(rt.fbw, fbw);
            rt.psm = psm;
            float maxY = 0.0f;
            for (uint32_t i = 0; i < n; ++i)
                maxY = std::max(maxY, v[i].y);
            uint32_t pw, ph;
            pageSize(psm, pw, ph);
            const uint32_t rows = std::min<uint32_t>(32u, (static_cast<uint32_t>(std::max(maxY, 0.0f)) + ph) / ph);
            rt.pageRows = std::max(rt.pageRows, std::max<uint32_t>(rows, 1u));
        }

        void touchTransferDest()
        {
            const GSTransferCommand &t = m_transfer;
            uint32_t first, last;
            pageRange(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                      t.trxreg.rrh, first, last);
            for (uint32_t p = first; p < last && p < kPages; ++p)
                ++m_pageGen[p];
            // An upload over a render target: copy the uploaded rect into it (same base), or drop the target.
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
                    else
                    {
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

        // ---- GL execution (main thread) ------------------------------------------------------------------------
        struct GlRt
        {
            GLuint color = 0;
            uint32_t w = 0, h = 0; // PS2 pixels
        };

        bool initGl()
        {
            if (m_glFailed)
                return false;
            if (!m_gl.load())
            {
                m_glFailed = true;
                return false;
            }
            m_prog = buildProgram(kVertexShader, kFragmentShader);
            m_blitProg = buildProgram(kVertexShader, kBlitFragmentShader);
            if (!m_prog || !m_blitProg)
            {
                m_glFailed = true;
                return false;
            }
            m_gl.glGenVertexArrays(1, &m_vao);
            m_gl.glGenBuffers(1, &m_vbo);
            m_gl.glBindVertexArray(m_vao);
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
            const GLsizei stride = sizeof(HwVertex);
            m_gl.glVertexAttribPointer(0, 3, GL_FLOAT, 0, stride, reinterpret_cast<void *>(offsetof(HwVertex, x)));
            m_gl.glVertexAttribPointer(1, 3, GL_FLOAT, 0, stride, reinterpret_cast<void *>(offsetof(HwVertex, s)));
            m_gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, stride, reinterpret_cast<void *>(offsetof(HwVertex, r)));
            m_gl.glVertexAttribPointer(3, 1, GL_FLOAT, 0, stride, reinterpret_cast<void *>(offsetof(HwVertex, fog)));
            for (GLuint i = 0; i < 4; ++i)
                m_gl.glEnableVertexAttribArray(i);
            m_gl.glBindVertexArray(0);
            m_gl.glGenFramebuffers(1, &m_fbo);
            m_glReady = true;
            std::fprintf(stderr, "[gs-hw] OpenGL renderer ready, scale %ux\n", m_scale);
            return true;
        }

        GLuint buildProgram(const char *vs, const char *fs)
        {
            auto compile = [&](GLenum type, const char *src) -> GLuint {
                GLuint sh = m_gl.glCreateShader(type);
                m_gl.glShaderSource(sh, 1, &src, nullptr);
                m_gl.glCompileShader(sh);
                GLint ok = 0;
                m_gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
                if (!ok)
                {
                    char log[2048];
                    m_gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                    std::fprintf(stderr, "[gs-hw] shader compile failed: %s\n", log);
                    return 0u;
                }
                return sh;
            };
            const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
            if (!v || !f)
                return 0u;
            GLuint p = m_gl.glCreateProgram();
            m_gl.glAttachShader(p, v);
            m_gl.glAttachShader(p, f);
            m_gl.glLinkProgram(p);
            GLint ok = 0;
            m_gl.glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[2048];
                m_gl.glGetProgramInfoLog(p, sizeof(log), nullptr, log);
                std::fprintf(stderr, "[gs-hw] program link failed: %s\n", log);
                return 0u;
            }
            return p;
        }

        GLuint makeTexture(uint32_t w, uint32_t h, GLenum internal, GLenum format, GLenum type, const void *data, bool linear)
        {
            GLuint t = 0;
            m_gl.glGenTextures(1, &t);
            m_gl.glBindTexture(GL_TEXTURE_2D, t);
            m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            m_gl.glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                              format, type, data);
            return t;
        }

        GlRt &rtFor(uint32_t fbp, uint32_t fbw, uint32_t minH)
        {
            GlRt &rt = m_glRts[fbp];
            const uint32_t w = std::max<uint32_t>(fbw, 1u) * 64u;
            const uint32_t h = std::max<uint32_t>(minH, 512u);
            if (rt.color && rt.w >= w && rt.h >= h)
                return rt;
            GlRt grown;
            grown.w = std::max(rt.w, w);
            grown.h = std::max(rt.h, h);
            grown.color = makeTexture(grown.w * m_scale, grown.h * m_scale, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, grown.color, 0);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glColorMask(1, 1, 1, 1);
            m_gl.glClearColor(0, 0, 0, 0);
            m_gl.glClear(GL_COLOR_BUFFER_BIT);
            if (rt.color)
            {
                // keep the old contents
                GLuint readFbo = 0;
                m_gl.glGenFramebuffers(1, &readFbo);
                m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                m_gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);
                m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
                m_gl.glBlitFramebuffer(0, 0, rt.w * m_scale, rt.h * m_scale, 0, 0, rt.w * m_scale, rt.h * m_scale,
                                       GL_COLOR_BUFFER_BIT, GL_NEAREST);
                m_gl.glDeleteFramebuffers(1, &readFbo);
                m_gl.glDeleteTextures(1, &rt.color);
            }
            rt = grown;
            return rt;
        }

        GLuint depthFor(uint32_t zbp, uint32_t w, uint32_t h)
        {
            auto &d = m_glDepth[zbp];
            if (d.tex && d.w >= w && d.h >= h)
                return d.tex;
            if (d.tex)
                m_gl.glDeleteTextures(1, &d.tex);
            d.w = std::max(d.w, w);
            d.h = std::max(d.h, h);
            d.tex = makeTexture(d.w * m_scale, d.h * m_scale, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr, false);
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d.tex, 0);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDepthMask(1);
            m_gl.glClearDepth(0.0);
            m_gl.glClear(GL_DEPTH_BUFFER_BIT);
            return d.tex;
        }

        void execute(Frame &f)
        {
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
            m_gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(f.vertices.size() * sizeof(HwVertex)), f.vertices.data(),
                              GL_STREAM_DRAW);
            m_gl.glBindVertexArray(m_vao);
            m_gl.glDisable(GL_CULL_FACE);
            for (const Cmd &c : f.cmds)
            {
                switch (c.type)
                {
                case CmdType::TexCreate:
                {
                    const TexCreate &t = f.textures[c.index];
                    GLuint &tex = m_glTextures[t.key];
                    if (tex)
                        m_gl.glDeleteTextures(1, &tex);
                    tex = makeTexture(t.w, t.h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, t.pixels.data(), false);
                    break;
                }
                case CmdType::TexDelete:
                {
                    auto it = m_glTextures.find(f.texDeletes[c.index]);
                    if (it != m_glTextures.end())
                    {
                        m_gl.glDeleteTextures(1, &it->second);
                        m_glTextures.erase(it);
                    }
                    break;
                }
                case CmdType::RtDrop:
                {
                    auto it = m_glRts.find(f.rtDrops[c.index]);
                    if (it != m_glRts.end())
                    {
                        m_gl.glDeleteTextures(1, &it->second.color);
                        m_glRts.erase(it);
                    }
                    break;
                }
                case CmdType::RtUpload:
                {
                    const RtUpload &u = f.rtUploads[c.index];
                    GlRt &rt = rtFor(u.fbp, u.fbw, u.y + u.h);
                    // Upload at 1x into a scratch texture, then scale-blit into the target.
                    GLuint tmp = makeTexture(u.w, u.h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, u.pixels.data(), false);
                    GLuint readFbo = 0;
                    m_gl.glGenFramebuffers(1, &readFbo);
                    m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                    m_gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
                    m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
                    m_gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);
                    m_gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                    m_gl.glDisable(GL_SCISSOR_TEST);
                    m_gl.glBlitFramebuffer(0, 0, u.w, u.h, u.x * m_scale, u.y * m_scale, (u.x + u.w) * m_scale, (u.y + u.h) * m_scale,
                                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    m_gl.glDeleteFramebuffers(1, &readFbo);
                    m_gl.glDeleteTextures(1, &tmp);
                    break;
                }
                case CmdType::Clear:
                {
                    const ClearCmd &cl = f.clears[c.index];
                    GlRt &rt = rtFor(cl.s.fbp, cl.s.fbw, cl.s.scy1 + 1u);
                    bindTarget(rt, 0u, false);
                    m_gl.glEnable(GL_SCISSOR_TEST);
                    m_gl.glScissor(cl.s.scx0 * m_scale, cl.s.scy0 * m_scale, (cl.s.scx1 - cl.s.scx0 + 1) * m_scale,
                                   (cl.s.scy1 - cl.s.scy0 + 1) * m_scale);
                    m_gl.glColorMask(1, 1, 1, 1);
                    m_gl.glClearColor((cl.rgba & 0xFF) / 255.0f, ((cl.rgba >> 8) & 0xFF) / 255.0f, ((cl.rgba >> 16) & 0xFF) / 255.0f,
                                      ((cl.rgba >> 24) & 0xFF) / 255.0f);
                    m_gl.glClear(GL_COLOR_BUFFER_BIT);
                    break;
                }
                case CmdType::Draw:
                    draw(f.draws[c.index]);
                    break;
                }
            }
        }

        void bindTarget(GlRt &rt, GLuint depth, bool withDepth)
        {
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, withDepth ? depth : 0u, 0);
            m_gl.glViewport(0, 0, rt.w * m_scale, rt.h * m_scale);
        }

        GLint loc(const char *name)
        {
            auto it = m_uniforms.find(name);
            if (it != m_uniforms.end())
                return it->second;
            const GLint l = m_gl.glGetUniformLocation(m_prog, name);
            m_uniforms[name] = l;
            return l;
        }

        // GS blend (A - B) * C / 128 + D  ->  GL blend; false when not expressible (drawn unblended).
        bool setBlend(const HwState &s, int &premul)
        {
            premul = 0;
            if (!s.abe)
            {
                m_gl.glDisable(GL_BLEND);
                return true;
            }
            const int A = s.ba, B = s.bb, D = s.bd;
            GLenum eq = GL_FUNC_ADD, src = GL_ONE, dst = GL_ZERO;
            if (A == B)
            {
                // (X - X) * C + D = D
                if (D == 0) { src = GL_ONE; dst = GL_ZERO; }
                else if (D == 1) { src = GL_ZERO; dst = GL_ONE; }
                else { src = GL_ZERO; dst = GL_ZERO; }
            }
            else if (A == 0 && B == 1)
            {
                if (D == 1) { src = GL_SRC1_ALPHA; dst = GL_ONE_MINUS_SRC1_ALPHA; }      // Cs*C + Cd*(1-C)
                else if (D == 0) { premul = 2; eq = GL_FUNC_SUBTRACT; src = GL_ONE; dst = GL_SRC1_ALPHA; } // Cs*(1+C) - Cd*C
                else { src = GL_SRC1_ALPHA; dst = GL_SRC1_ALPHA; eq = GL_FUNC_SUBTRACT; } // (Cs - Cd)*C
            }
            else if (A == 0 && B == 2)
            {
                premul = 1;                                       // Cs*C + D
                src = GL_ONE;
                dst = (D == 1) ? GL_ONE : GL_ZERO;
                if (D == 0) { premul = 2; src = GL_ONE; dst = GL_ZERO; }
            }
            else if (A == 1 && B == 0)
            {
                if (D == 0) { src = GL_ONE_MINUS_SRC1_ALPHA; dst = GL_SRC1_ALPHA; } // Cs*(1-C) + Cd*C
                else return false;
            }
            else if (A == 2 && B == 0)
            {
                premul = 1;                                        // D - Cs*C
                if (D == 1) { eq = GL_FUNC_REVERSE_SUBTRACT; src = GL_ONE; dst = GL_ONE; }
                else { src = GL_ZERO; dst = GL_ZERO; }
            }
            else if (A == 2 && B == 1)
            {
                if (D == 1) { src = GL_ZERO; dst = GL_ONE_MINUS_SRC1_ALPHA; }      // Cd*(1-C)
                else if (D == 0) { eq = GL_FUNC_REVERSE_SUBTRACT; src = GL_ONE; dst = GL_SRC1_ALPHA; } // Cs - Cd*C
                else return false;
            }
            else if (A == 1 && B == 2)
            {
                if (D == 0) { src = GL_ONE; dst = GL_SRC1_ALPHA; }                // Cs + Cd*C
                else if (D == 2) { src = GL_ZERO; dst = GL_SRC1_ALPHA; }          // Cd*C
                else return false;
            }
            else
            {
                return false;
            }
            m_gl.glEnable(GL_BLEND);
            m_gl.glBlendEquationSeparate(eq, GL_FUNC_ADD);
            m_gl.glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
            return true;
        }

        void draw(const DrawCmd &dc)
        {
            const HwState &s = dc.s;
            GlRt &rt = rtFor(s.fbp, s.fbw, s.scy1 + 1u);
            const bool depthOn = s.zte != 0u;
            const GLuint depth = depthOn ? depthFor(s.zbp, rt.w, rt.h) : 0u;
            bindTarget(rt, depth, depthOn);
            m_gl.glUseProgram(m_prog);
            m_gl.glUniform2f(loc("uTargetSize"), static_cast<float>(rt.w), static_cast<float>(rt.h));
            m_gl.glEnable(GL_SCISSOR_TEST);
            m_gl.glScissor(s.scx0 * m_scale, s.scy0 * m_scale, (s.scx1 - s.scx0 + 1) * m_scale, (s.scy1 - s.scy0 + 1) * m_scale);

            // depth
            if (depthOn)
            {
                m_gl.glEnable(GL_DEPTH_TEST);
                m_gl.glDepthFunc(s.ztst == 1 ? GL_ALWAYS : s.ztst == 2 ? GL_GEQUAL : GL_GREATER);
            }
            else
            {
                m_gl.glDisable(GL_DEPTH_TEST);
            }
            const bool zwrite = depthOn && !s.zmsk;

            // texture
            m_gl.glUniform1i(loc("uTme"), s.tme);
            if (s.tme)
            {
                GLuint tex = 0;
                float rtScale = 1.0f;
                if (s.texIsRt)
                {
                    auto it = m_glRts.find(s.rtFbp);
                    tex = it != m_glRts.end() ? it->second.color : 0u;
                    rtScale = static_cast<float>(m_scale);
                }
                else
                {
                    auto it = m_glTextures.find(s.texKey);
                    tex = it != m_glTextures.end() ? it->second : 0u;
                }
                m_gl.glActiveTexture(GL_TEXTURE0);
                m_gl.glBindTexture(GL_TEXTURE_2D, tex);
                m_gl.glUniform1i(loc("uTex"), 0);
                m_gl.glUniform1i(loc("uTfx"), s.tfx);
                m_gl.glUniform1i(loc("uTcc"), s.tcc);
                m_gl.glUniform1i(loc("uFst"), s.fst);
                m_gl.glUniform1i(loc("uLinear"), s.linear);
                m_gl.glUniform2i(loc("uTexSize"), s.tw, s.th);
                m_gl.glUniform2i(loc("uWrap"), s.wms, s.wmt);
                m_gl.glUniform4i(loc("uRegion"), s.minu, s.maxu, s.minv, s.maxv);
                m_gl.glUniform1i(loc("uTexIsRt"), s.texIsRt);
                m_gl.glUniform2f(loc("uRtOrigin"), s.rtOx, s.rtOy);
                m_gl.glUniform1f(loc("uRtScale"), rtScale);
            }
            m_gl.glUniform1i(loc("uIip"), s.iip);
            m_gl.glUniform1i(loc("uFge"), s.fge);
            m_gl.glUniform3f(loc("uFogCol"), s.fogR, s.fogG, s.fogB);

            int premul = 0;
            if (!setBlend(s, premul))
            {
                m_gl.glDisable(GL_BLEND);
                if (m_reportedBlend.insert(s.ba | (s.bb << 2) | (s.bd << 6)).second)
                    std::fprintf(stderr, "[gs-hw] unsupported blend A=%u B=%u C=%u D=%u (drawn opaque)\n", s.ba, s.bb, s.bc, s.bd);
            }
            m_gl.glUniform1i(loc("uBlendC"), s.bc);
            m_gl.glUniform1f(loc("uFix"), s.fix);
            m_gl.glUniform1i(loc("uPremul"), premul);

            // frame write mask (whole channels only)
            const GLboolean mr = (s.fbmsk & 0xFFu) != 0xFFu, mg = (s.fbmsk & 0xFF00u) != 0xFF00u,
                            mb = (s.fbmsk & 0xFF0000u) != 0xFF0000u, ma = (s.fbmsk & 0xFF000000u) != 0xFF000000u;

            m_gl.glUniform1i(loc("uAte"), s.ate);
            m_gl.glUniform1i(loc("uAtst"), s.atst);
            m_gl.glUniform1i(loc("uAref"), s.aref);
            const GLenum mode = s.mode == 1 ? GL_LINES : s.mode == 2 ? GL_POINTS : GL_TRIANGLES;
            // pass 1: pixels passing the alpha test (or all of them)
            m_gl.glUniform1i(loc("uAfailPass"), 0);
            m_gl.glColorMask(mr, mg, mb, ma);
            m_gl.glDepthMask(zwrite);
            if (s.ate && s.atst != 1u && s.afail != 0u)
            {
                // AFAIL other than KEEP: draw the passing pixels normally, the failing ones with the AFAIL mask
                m_gl.glDrawArrays(mode, static_cast<GLint>(dc.first), static_cast<GLsizei>(dc.count));
                m_gl.glUniform1i(loc("uAfailPass"), 1);
                if (s.afail == 1u) { m_gl.glDepthMask(0); }                               // FB_ONLY
                else if (s.afail == 2u) { m_gl.glColorMask(0, 0, 0, 0); }                 // ZB_ONLY
                else { m_gl.glColorMask(mr, mg, mb, 0); m_gl.glDepthMask(0); }           // RGB_ONLY
            }
            else if (s.ate && s.atst != 1u)
            {
                // KEEP: failing pixels discarded (pass 0 does it)
            }
            else
            {
                m_gl.glUniform1i(loc("uAte"), 0);
            }
            m_gl.glDrawArrays(mode, static_cast<GLint>(dc.first), static_cast<GLsizei>(dc.count));
        }

        void composeDisplay(const Frame &f)
        {
            m_displayW = f.displayW;
            m_displayH = f.displayH;
            const uint32_t w = m_displayW * m_scale, h = m_displayH * m_scale;
            if (!m_displayTex || m_displayTexW != w || m_displayTexH != h)
            {
                if (m_displayTex)
                    m_gl.glDeleteTextures(1, &m_displayTex);
                m_displayTex = makeTexture(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true);
                m_displayTexW = w;
                m_displayTexH = h;
            }
            m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
            m_gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_displayTex, 0);
            m_gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glColorMask(1, 1, 1, 1);
            auto it = m_glRts.find(f.displayFbp);
            if (f.displayFromRt && it != m_glRts.end())
            {
                GLuint readFbo = 0;
                m_gl.glGenFramebuffers(1, &readFbo);
                m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                m_gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.color, 0);
                const uint32_t x0 = f.displayX * m_scale, y0 = f.displayY * m_scale;
                m_gl.glBlitFramebuffer(x0, y0, x0 + w, y0 + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                m_gl.glDeleteFramebuffers(1, &readFbo);
                // the PS2 frame buffer's alpha is not shown: force it to 1
                m_gl.glColorMask(0, 0, 0, 1);
                m_gl.glClearColor(0, 0, 0, 1);
                m_gl.glClear(GL_COLOR_BUFFER_BIT);
                m_gl.glColorMask(1, 1, 1, 1);
            }
            else if (!f.cpuDisplay.empty())
            {
                GLuint tmp = makeTexture(f.cpuDisplayW, f.cpuDisplayH, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, f.cpuDisplay.data(), false);
                GLuint readFbo = 0;
                m_gl.glGenFramebuffers(1, &readFbo);
                m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                m_gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
                m_gl.glBlitFramebuffer(0, 0, f.cpuDisplayW, f.cpuDisplayH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                m_gl.glDeleteFramebuffers(1, &readFbo);
                m_gl.glDeleteTextures(1, &tmp);
            }
            else
            {
                m_gl.glClearColor(0, 0, 0, 1);
                m_gl.glClear(GL_COLOR_BUFFER_BIT);
            }
        }

        void restoreRaylibState(GLuint prevFbo, const GLint (&viewport)[4])
        {
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
            m_gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDisable(GL_DEPTH_TEST);
            m_gl.glDepthMask(1);
            m_gl.glColorMask(1, 1, 1, 1);
            m_gl.glEnable(GL_BLEND);
            m_gl.glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            m_gl.glBlendFuncSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/, GL_ONE, 0x0303);
            m_gl.glBindVertexArray(0);
            m_gl.glUseProgram(0);
            m_gl.glActiveTexture(GL_TEXTURE0);
            m_gl.glBindTexture(GL_TEXTURE_2D, 0);
        }

        // ---- state --------------------------------------------------------------------------------------------
        struct RtInfo
        {
            uint32_t fbw = 1, psm = 0, pageRows = 1;
        };
        static constexpr uint32_t kPages = 512u; // 4 MiB / 8 KiB
        static constexpr size_t kMaxTextures = 3000u;

        uint32_t m_scale;
        GSCpuBackend m_cpu;
        GSTransferCommand m_transfer{};
        std::unique_ptr<Frame> m_frame;
        uint64_t m_frameIndex = 0;
        uint32_t m_pageGen[kPages]{};
        std::unordered_map<uint64_t, uint64_t> m_texLru;  // key -> last frame used
        std::unordered_map<uint32_t, RtInfo> m_rts;       // fbp -> extent (game thread's view)

        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::deque<std::unique_ptr<Frame>> m_queue;

        // main thread
        Gl m_gl;
        bool m_glReady = false, m_glFailed = false;
        GLuint m_prog = 0, m_blitProg = 0, m_vao = 0, m_vbo = 0, m_fbo = 0;
        std::unordered_map<std::string, GLint> m_uniforms;
        std::unordered_map<uint32_t, GlRt> m_glRts;
        struct GlDepth
        {
            GLuint tex = 0;
            uint32_t w = 0, h = 0;
        };
        std::unordered_map<uint32_t, GlDepth> m_glDepth;
        std::unordered_map<uint64_t, GLuint> m_glTextures;
        std::unordered_set<uint32_t> m_reportedBlend;
        GLuint m_displayTex = 0;
        uint32_t m_displayTexW = 0, m_displayTexH = 0, m_displayW = 640, m_displayH = 448;
    };
}

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options)
{
    return std::make_unique<HwBackend>(options);
}
