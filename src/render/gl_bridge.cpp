// Stage 2 stopgap (docs/NATIVE_RENDERER.md), deleted in stage 3: the OpenGL HW renderer still draws into the
// platform layer's hidden GL context; its picture is read back here and shown through the RHI. The readback is
// asynchronous (two pixel-pack buffers, used one host frame later): a synchronous glReadPixels stalled the main
// thread and cost the game thread ~4% (NVIDIA spins while it waits).
#include "render/presenter.h"

#include "platform/platform.h"

#include <cstdio>
#include <type_traits>
#include <cstdint>

namespace present
{
    namespace
    {
        using GLenum = unsigned int;
        using GLuint = unsigned int;
        using GLint = int;
        using GLsizei = int;

        constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908,
                         GL_UNSIGNED_BYTE = 0x1401, GL_PACK_ALIGNMENT = 0x0D05, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                         GL_PIXEL_PACK_BUFFER = 0x88EB, GL_STREAM_READ = 0x88E1, GL_MAP_READ_BIT = 0x0001;
        using GLsizeiptr = intptr_t;
        using GLintptr = intptr_t;
        using GLbitfield = unsigned int;
        using GLboolean = unsigned char;

#if defined(_WIN32)
#define GLB_API __stdcall
#else
#define GLB_API
#endif
        struct Gl
        {
            void(GLB_API *glGenFramebuffers)(GLsizei, GLuint *) = nullptr;
            void(GLB_API *glBindFramebuffer)(GLenum, GLuint) = nullptr;
            void(GLB_API *glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
            GLenum(GLB_API *glCheckFramebufferStatus)(GLenum) = nullptr;
            void(GLB_API *glPixelStorei)(GLenum, GLint) = nullptr;
            void(GLB_API *glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;
            void(GLB_API *glGenBuffers)(GLsizei, GLuint *) = nullptr;
            void(GLB_API *glBindBuffer)(GLenum, GLuint) = nullptr;
            void(GLB_API *glBufferData)(GLenum, GLsizeiptr, const void *, GLenum) = nullptr;
            void *(GLB_API *glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
            GLboolean(GLB_API *glUnmapBuffer)(GLenum) = nullptr;
        } gl;

        bool g_loaded = false, g_failed = false;
        GLuint g_fbo = 0;
        GLuint g_pbo[2] = {};
        int g_pboWidth[2] = {}, g_pboHeight[2] = {}; // what each buffer holds (0 = nothing)
        size_t g_pboBytes[2] = {};
        int g_next = 0;

        bool load()
        {
            if (g_loaded || g_failed)
                return g_loaded;
            auto get = [](auto &fn, const char *name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(platform::glProc(name));
                return fn != nullptr;
            };
            g_loaded = get(gl.glGenFramebuffers, "glGenFramebuffers") && get(gl.glBindFramebuffer, "glBindFramebuffer") &&
                       get(gl.glFramebufferTexture2D, "glFramebufferTexture2D") &&
                       get(gl.glCheckFramebufferStatus, "glCheckFramebufferStatus") && get(gl.glPixelStorei, "glPixelStorei") &&
                       get(gl.glReadPixels, "glReadPixels") && get(gl.glGenBuffers, "glGenBuffers") &&
                       get(gl.glBindBuffer, "glBindBuffer") && get(gl.glBufferData, "glBufferData") &&
                       get(gl.glMapBufferRange, "glMapBufferRange") && get(gl.glUnmapBuffer, "glUnmapBuffer");
            g_failed = !g_loaded;
            if (g_failed)
                std::fprintf(stderr, "[present] no OpenGL to read the HW renderer's picture from\n");
            else
            {
                gl.glGenFramebuffers(1, &g_fbo);
                gl.glGenBuffers(2, g_pbo);
            }
            return g_loaded;
        }
    }

    bool pictureFromGl(Picture &picture, uint32_t glTexture, int width, int height)
    {
        if (!glTexture || width <= 0 || height <= 0 || !load())
            return false;
        // start this frame's copy into one buffer...
        const int cur = g_next;
        g_next ^= 1;
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexture, 0);
        if (gl.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            const size_t bytes = static_cast<size_t>(width) * height * 4;
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[cur]);
            if (g_pboBytes[cur] != bytes)
            {
                gl.glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
                g_pboBytes[cur] = bytes;
            }
            gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g_pboWidth[cur] = width;
            g_pboHeight[cur] = height;
        }
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        // ...and show the one started a frame ago (texture row 0 is the top of the picture, as D3D wants)
        const int prev = cur ^ 1;
        bool shown = false;
        if (g_pboWidth[prev] > 0)
        {
            if (!picture.texture || picture.width != g_pboWidth[prev] || picture.height != g_pboHeight[prev])
                picture = createPicture(g_pboWidth[prev], g_pboHeight[prev], true);
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[prev]);
            if (const void *pixels = gl.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(g_pboBytes[prev]), GL_MAP_READ_BIT))
            {
                updatePicture(picture, pixels);
                gl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                shown = true;
            }
        }
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return shown;
    }
}
