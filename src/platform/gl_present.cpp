// See gl_present.h.
#include "platform/gl_present.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace glpresent
{
    namespace
    {
        using GLenum = unsigned int;
        using GLuint = unsigned int;
        using GLint = int;
        using GLsizei = int;
        using GLfloat = float;
        using GLchar = char;
        using GLbitfield = unsigned int;
        using GLboolean = unsigned char;

        constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_RGBA8 = 0x8058,
                         GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                         GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
                         GL_CLAMP_TO_EDGE = 0x812F, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0,
                         GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
                         GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_TRIANGLE_STRIP = 0x0005,
                         GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44,
                         GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_FUNC_ADD = 0x8006,
                         GL_COLOR_BUFFER_BIT = 0x4000, GL_TEXTURE0 = 0x84C0, GL_UNPACK_ALIGNMENT = 0x0CF5,
                         GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ROW_LENGTH = 0x0CF2;

#define GLP_FUNCS(X)                                                                                                   \
    X(void, glGenTextures, (GLsizei, GLuint *))                                                                        \
    X(void, glDeleteTextures, (GLsizei, const GLuint *))                                                               \
    X(void, glBindTexture, (GLenum, GLuint))                                                                           \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))               \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))            \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                                  \
    X(void, glPixelStorei, (GLenum, GLint))                                                                            \
    X(void, glActiveTexture, (GLenum))                                                                                 \
    X(GLuint, glCreateShader, (GLenum))                                                                                \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                                   \
    X(void, glCompileShader, (GLuint))                                                                                 \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                                  \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                                \
    X(void, glDeleteShader, (GLuint))                                                                                  \
    X(GLuint, glCreateProgram, ())                                                                                     \
    X(void, glAttachShader, (GLuint, GLuint))                                                                          \
    X(void, glLinkProgram, (GLuint))                                                                                   \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                                 \
    X(void, glDeleteProgram, (GLuint))                                                                                 \
    X(void, glUseProgram, (GLuint))                                                                                    \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                                           \
    X(void, glUniform1i, (GLint, GLint))                                                                               \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                                    \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                                  \
    X(void, glGenVertexArrays, (GLsizei, GLuint *))                                                                    \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint *))                                                           \
    X(void, glBindVertexArray, (GLuint))                                                                               \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                                    \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                              \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                        \
    X(void, glClear, (GLbitfield))                                                                                     \
    X(void, glEnable, (GLenum))                                                                                        \
    X(void, glDisable, (GLenum))                                                                                       \
    X(void, glBlendFunc, (GLenum, GLenum))                                                                             \
    X(void, glBlendEquation, (GLenum))                                                                                 \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                 \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                                    \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                                                           \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                                       \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                           \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                                      \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

#if defined(_WIN32)
#define GLP_API __stdcall
#else
#define GLP_API
#endif
#define GLP_DECLARE(ret, name, args) ret(GLP_API *name) args = nullptr;
        struct Gl
        {
            GLP_FUNCS(GLP_DECLARE)
        } gl;
#undef GLP_DECLARE

        GLuint g_program = 0, g_vao = 0;
        GLint g_uRect = -1, g_uUvMax = -1, g_uTex = -1;

        const char *kVertex = R"(#version 330 core
uniform vec4 uRect;  // NDC: left, top, right, bottom
uniform vec2 uUvMax;
out vec2 vUv;
void main()
{
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(uRect.x, uRect.z, c.x), mix(uRect.y, uRect.w, c.y), 0.0, 1.0);
    vUv = c * uUvMax;
}
)";
        const char *kFragment = R"(#version 330 core
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

        GLuint compile(GLenum type, const char *source)
        {
            const GLuint s = gl.glCreateShader(type);
            gl.glShaderSource(s, 1, &source, nullptr);
            gl.glCompileShader(s);
            GLint ok = 0;
            gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024] = {};
                gl.glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                std::fprintf(stderr, "[present] shader: %s\n", log);
            }
            return s;
        }
    }

    bool init()
    {
#define GLP_LOAD(ret, name, args)                                                                                      \
    gl.name = reinterpret_cast<decltype(gl.name)>(platform::glProc(#name));                                            \
    if (!gl.name)                                                                                                      \
    {                                                                                                                  \
        std::fprintf(stderr, "[present] missing GL function %s\n", #name);                                             \
        return false;                                                                                                  \
    }
        GLP_FUNCS(GLP_LOAD)
#undef GLP_LOAD
        const GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
        g_program = gl.glCreateProgram();
        gl.glAttachShader(g_program, vs);
        gl.glAttachShader(g_program, fs);
        gl.glLinkProgram(g_program);
        gl.glDeleteShader(vs);
        gl.glDeleteShader(fs);
        GLint ok = 0;
        gl.glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            std::fprintf(stderr, "[present] program link failed\n");
            return false;
        }
        g_uRect = gl.glGetUniformLocation(g_program, "uRect");
        g_uUvMax = gl.glGetUniformLocation(g_program, "uUvMax");
        g_uTex = gl.glGetUniformLocation(g_program, "uTex");
        gl.glGenVertexArrays(1, &g_vao);
        return true;
    }

    void shutdown()
    {
        if (g_program)
            gl.glDeleteProgram(g_program);
        if (g_vao)
            gl.glDeleteVertexArrays(1, &g_vao);
        g_program = g_vao = 0;
    }

    Texture createTexture(int width, int height, bool linear)
    {
        Texture t;
        t.width = width;
        t.height = height;
        gl.glGenTextures(1, &t.id);
        gl.glBindTexture(GL_TEXTURE_2D, t.id);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.glBindTexture(GL_TEXTURE_2D, 0);
        return t;
    }

    void updateTexture(const Texture &texture, const void *rgba)
    {
        if (!texture.id)
            return;
        gl.glBindTexture(GL_TEXTURE_2D, texture.id);
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        gl.glBindTexture(GL_TEXTURE_2D, 0);
    }

    void destroyTexture(Texture &texture)
    {
        if (texture.id)
            gl.glDeleteTextures(1, &texture.id);
        texture = Texture{};
    }

    void drawFrame(uint32_t textureId, int textureWidth, int textureHeight, int srcWidth, int srcHeight, int targetWidth,
                   int targetHeight)
    {
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glColorMask(1, 1, 1, 1);
        gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        if (!textureId || textureWidth <= 0 || textureHeight <= 0 || srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 ||
            targetHeight <= 0)
            return;
        const float scale = std::min(static_cast<float>(targetWidth) / srcWidth, static_cast<float>(targetHeight) / srcHeight);
        const float w = srcWidth * scale, h = srcHeight * scale;
        const float x0 = (targetWidth - w) * 0.5f, y0 = (targetHeight - h) * 0.5f;
        auto ndcX = [&](float px) { return px / targetWidth * 2.0f - 1.0f; };
        auto ndcY = [&](float py) { return 1.0f - py / targetHeight * 2.0f; };

        gl.glDisable(GL_DEPTH_TEST);
        gl.glDisable(GL_CULL_FACE);
        gl.glEnable(GL_BLEND);
        gl.glBlendEquation(GL_FUNC_ADD);
        gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.glUseProgram(g_program);
        gl.glUniform4f(g_uRect, ndcX(x0), ndcY(y0), ndcX(x0 + w), ndcY(y0 + h));
        gl.glUniform2f(g_uUvMax, static_cast<float>(srcWidth) / textureWidth, static_cast<float>(srcHeight) / textureHeight);
        gl.glUniform1i(g_uTex, 0);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D, textureId);
        gl.glBindVertexArray(g_vao);
        gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl.glBindVertexArray(0);
        gl.glBindTexture(GL_TEXTURE_2D, 0);
        gl.glUseProgram(0);
    }

    void bindWindow(int width, int height)
    {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glViewport(0, 0, width, height);
    }

    bool capture(int width, int height, const std::function<void(int, int)> &draw, std::vector<uint8_t> &rgb)
    {
        GLuint tex = 0, fbo = 0;
        gl.glGenTextures(1, &tex);
        gl.glBindTexture(GL_TEXTURE_2D, tex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.glBindTexture(GL_TEXTURE_2D, 0);
        gl.glGenFramebuffers(1, &fbo);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        const bool ok = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok)
        {
            gl.glViewport(0, 0, width, height);
            draw(width, height);
            std::vector<uint8_t> bottomUp(static_cast<size_t>(width) * height * 3);
            gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
            gl.glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bottomUp.data());
            rgb.resize(bottomUp.size());
            const size_t row = static_cast<size_t>(width) * 3;
            for (int y = 0; y < height; ++y)
                std::memcpy(rgb.data() + y * row, bottomUp.data() + (height - 1 - y) * row, row);
        }
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glDeleteFramebuffers(1, &fbo);
        gl.glDeleteTextures(1, &tex);
        return ok;
    }
}
