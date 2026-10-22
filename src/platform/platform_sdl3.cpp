// SDL3 implementation of platform.h.
#include "platform/platform.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace platform
{
    namespace
    {
        SDL_Window *g_window = nullptr;
        SDL_GLContext g_gl = nullptr;
        bool g_quit = false;
        bool g_imgui = false;
        uint64_t g_nextFrameNs = 0;

        std::array<std::atomic<bool>, KEY_COUNT> g_keys{};
        std::array<bool, KEY_COUNT> g_keysPrev{}; // main thread
        std::array<bool, KEY_COUNT> g_keysPressed{};
        std::array<std::atomic<bool>, MOUSE_BUTTON_COUNT> g_mouse{};
        std::array<std::atomic<bool>, GAMEPAD_BUTTON_COUNT> g_padButtons{};
        std::array<std::atomic<float>, AXIS_COUNT> g_padAxes{};
        std::atomic<bool> g_padConnected{false};
        SDL_Gamepad *g_gamepad = nullptr;

        SDL_AudioStream *g_audio = nullptr;
        AudioFill g_audioFill = nullptr;
        std::vector<int16_t> g_audioScratch;

        struct ScanKey
        {
            SDL_Scancode scancode;
            int key;
        };

        // SDL scancodes (physical positions) for the keys platform.h names.
        const std::vector<ScanKey> &scanKeys()
        {
            static const std::vector<ScanKey> keys = [] {
                std::vector<ScanKey> k = {
                    {SDL_SCANCODE_SPACE, KEY_SPACE}, {SDL_SCANCODE_APOSTROPHE, KEY_APOSTROPHE}, {SDL_SCANCODE_COMMA, KEY_COMMA},
                    {SDL_SCANCODE_MINUS, KEY_MINUS}, {SDL_SCANCODE_PERIOD, KEY_PERIOD}, {SDL_SCANCODE_SLASH, KEY_SLASH},
                    {SDL_SCANCODE_SEMICOLON, KEY_SEMICOLON}, {SDL_SCANCODE_EQUALS, KEY_EQUAL},
                    {SDL_SCANCODE_LEFTBRACKET, KEY_LEFT_BRACKET}, {SDL_SCANCODE_BACKSLASH, KEY_BACKSLASH},
                    {SDL_SCANCODE_RIGHTBRACKET, KEY_RIGHT_BRACKET}, {SDL_SCANCODE_GRAVE, KEY_GRAVE},
                    {SDL_SCANCODE_ESCAPE, KEY_ESCAPE}, {SDL_SCANCODE_RETURN, KEY_ENTER}, {SDL_SCANCODE_TAB, KEY_TAB},
                    {SDL_SCANCODE_BACKSPACE, KEY_BACKSPACE}, {SDL_SCANCODE_INSERT, KEY_INSERT}, {SDL_SCANCODE_DELETE, KEY_DELETE},
                    {SDL_SCANCODE_RIGHT, KEY_RIGHT}, {SDL_SCANCODE_LEFT, KEY_LEFT}, {SDL_SCANCODE_DOWN, KEY_DOWN},
                    {SDL_SCANCODE_UP, KEY_UP}, {SDL_SCANCODE_PAGEUP, KEY_PAGE_UP}, {SDL_SCANCODE_PAGEDOWN, KEY_PAGE_DOWN},
                    {SDL_SCANCODE_HOME, KEY_HOME}, {SDL_SCANCODE_END, KEY_END}, {SDL_SCANCODE_KP_ENTER, KEY_KP_ENTER},
                    {SDL_SCANCODE_LSHIFT, KEY_LEFT_SHIFT}, {SDL_SCANCODE_LCTRL, KEY_LEFT_CONTROL}, {SDL_SCANCODE_LALT, KEY_LEFT_ALT},
                    {SDL_SCANCODE_RSHIFT, KEY_RIGHT_SHIFT}, {SDL_SCANCODE_RCTRL, KEY_RIGHT_CONTROL}, {SDL_SCANCODE_RALT, KEY_RIGHT_ALT},
                    {SDL_SCANCODE_0, KEY_ZERO},
                };
                for (int i = 0; i < 26; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_A + i), KEY_A + i});
                for (int i = 0; i < 9; ++i) // SDL orders 1..9 then 0
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_1 + i), KEY_ONE + i});
                for (int i = 0; i < 12; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + i), KEY_F1 + i});
                k.push_back({SDL_SCANCODE_KP_0, KEY_KP_0});
                for (int i = 0; i < 9; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + i), KEY_KP_1 + i});
                return k;
            }();
            return keys;
        }

        void openGamepad(SDL_JoystickID id)
        {
            if (g_gamepad)
                return;
            g_gamepad = SDL_OpenGamepad(id);
            if (g_gamepad)
                std::fprintf(stderr, "[platform] gamepad: %s\n", SDL_GetGamepadName(g_gamepad));
        }

        void snapshotGamepad()
        {
            static constexpr SDL_GamepadButton kButtons[GAMEPAD_BUTTON_COUNT] = {
                SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
                SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
                SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
                SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
            };
            static constexpr SDL_GamepadAxis kAxes[AXIS_COUNT] = {
                SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
            };
            g_padConnected.store(g_gamepad != nullptr, std::memory_order_relaxed);
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
                g_padButtons[b].store(g_gamepad && SDL_GetGamepadButton(g_gamepad, kButtons[b]), std::memory_order_relaxed);
            for (int a = 0; a < AXIS_COUNT; ++a)
            {
                const float v = g_gamepad ? SDL_GetGamepadAxis(g_gamepad, kAxes[a]) / 32767.0f : 0.0f;
                g_padAxes[a].store(v < -1.0f ? -1.0f : v, std::memory_order_relaxed);
            }
        }

        void SDLCALL audioCallback(void *, SDL_AudioStream *stream, int additional, int)
        {
            const uint32_t frames = static_cast<uint32_t>(additional) / 4u;
            if (!frames || !g_audioFill)
                return;
            g_audioScratch.resize(frames * 2u);
            g_audioFill(g_audioScratch.data(), frames);
            SDL_PutAudioStreamData(stream, g_audioScratch.data(), static_cast<int>(frames * 4u));
        }

        // ---- PNG (stored deflate blocks: no compressor needed) ------------------------------------------------
        uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            while (n--)
                crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x >> 24));
            v.push_back(static_cast<uint8_t>(x >> 16));
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(0, out.data() + start, out.size() - start));
        }
    }

    bool init(const WindowDesc &desc)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[platform] SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            std::fprintf(stderr, "[platform] no audio: %s\n", SDL_GetError());

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
        if (desc.hidden)
            flags |= SDL_WINDOW_HIDDEN;
        g_window = SDL_CreateWindow(desc.title, desc.width, desc.height, flags);
        if (!g_window)
        {
            std::fprintf(stderr, "[platform] SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        g_gl = SDL_GL_CreateContext(g_window);
        if (!g_gl)
        {
            std::fprintf(stderr, "[platform] OpenGL 3.3 context: %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(g_window, g_gl);
        SDL_GL_SetSwapInterval(0); // paced by present() instead, also when the window is hidden

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        g_imgui = ImGui_ImplSDL3_InitForOpenGL(g_window, g_gl) && ImGui_ImplOpenGL3_Init("#version 330");
        if (!g_imgui)
            std::fprintf(stderr, "[platform] Dear ImGui backends failed\n");
        return true;
    }

    void shutdown()
    {
        if (!g_window)
            return;
        audioClose();
        if (g_imgui)
        {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            g_imgui = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (g_gamepad)
            SDL_CloseGamepad(g_gamepad);
        g_gamepad = nullptr;
        if (g_gl)
            SDL_GL_DestroyContext(g_gl);
        g_gl = nullptr;
        if (g_window)
            SDL_DestroyWindow(g_window);
        g_window = nullptr;
        SDL_Quit();
    }

    bool ready() { return g_window != nullptr; }

    void pumpEvents()
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (g_imgui)
                ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_quit = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_gamepad && SDL_GetGamepadID(g_gamepad) == e.gdevice.which)
                {
                    SDL_CloseGamepad(g_gamepad);
                    g_gamepad = nullptr;
                    int count = 0;
                    if (SDL_JoystickID *ids = SDL_GetGamepads(&count))
                    {
                        if (count > 0)
                            openGamepad(ids[0]);
                        SDL_free(ids);
                    }
                }
                break;
            default:
                break;
            }
        }

        int count = 0;
        const bool *state = SDL_GetKeyboardState(&count);
        std::array<bool, KEY_COUNT> now{};
        for (const ScanKey &k : scanKeys())
            if (k.scancode < count && state[k.scancode])
                now[k.key] = true;
        for (int k = 0; k < KEY_COUNT; ++k)
        {
            g_keysPressed[k] = now[k] && !g_keysPrev[k];
            g_keysPrev[k] = now[k];
            g_keys[k].store(now[k], std::memory_order_relaxed);
        }
        const SDL_MouseButtonFlags mouse = SDL_GetMouseState(nullptr, nullptr);
        g_mouse[MOUSE_LEFT].store((mouse & SDL_BUTTON_LMASK) != 0, std::memory_order_relaxed);
        g_mouse[MOUSE_RIGHT].store((mouse & SDL_BUTTON_RMASK) != 0, std::memory_order_relaxed);
        g_mouse[MOUSE_MIDDLE].store((mouse & SDL_BUTTON_MMASK) != 0, std::memory_order_relaxed);
        snapshotGamepad();
    }

    bool quitRequested() { return g_quit; }

    void beginFrame()
    {
        if (!g_imgui)
            return;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void renderUi()
    {
        if (!g_imgui)
            return;
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void renderUiScaled(int width, int height)
    {
        ImDrawData *data = g_imgui ? ImGui::GetDrawData() : nullptr;
        if (!data || !data->Valid || data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f)
            return;
        ImDrawData scaled = *data;
        scaled.FramebufferScale = ImVec2(width / data->DisplaySize.x, height / data->DisplaySize.y);
        ImGui_ImplOpenGL3_RenderDrawData(&scaled);
    }

    void present()
    {
        SDL_GL_SwapWindow(g_window);
        // 60 host frames per second, as raylib's SetTargetFPS(60) did
        constexpr uint64_t kPeriodNs = 1000000000ull / 60u;
        const uint64_t now = SDL_GetTicksNS();
        g_nextFrameNs = g_nextFrameNs ? g_nextFrameNs + kPeriodNs : now + kPeriodNs;
        if (g_nextFrameNs < now)
            g_nextFrameNs = now; // fell behind: don't try to catch up
        else
            SDL_DelayPrecise(g_nextFrameNs - now);
    }

    void windowSize(int &width, int &height)
    {
        width = height = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &width, &height);
    }

    bool keyDown(int key) { return key >= 0 && key < KEY_COUNT && g_keys[key].load(std::memory_order_relaxed); }
    bool keyPressed(int key) { return key >= 0 && key < KEY_COUNT && g_keysPressed[key]; }
    bool mouseDown(MouseButton button) { return g_mouse[button].load(std::memory_order_relaxed); }
    bool gamepadConnected() { return g_padConnected.load(std::memory_order_relaxed); }
    bool gamepadButton(GamepadButton button) { return g_padButtons[button].load(std::memory_order_relaxed); }
    float gamepadAxis(GamepadAxis axis) { return g_padAxes[axis].load(std::memory_order_relaxed); }

    bool audioAvailable() { return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0; }

    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill)
    {
        if (g_audio || !audioAvailable())
            return false;
        char frames[16];
        std::snprintf(frames, sizeof(frames), "%u", bufferFrames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        g_audioFill = fill;
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCallback, nullptr);
        if (!g_audio)
        {
            std::fprintf(stderr, "[platform] audio device: %s\n", SDL_GetError());
            return false;
        }
        SDL_ResumeAudioStreamDevice(g_audio);
        return true;
    }

    void audioClose()
    {
        if (g_audio)
            SDL_DestroyAudioStream(g_audio);
        g_audio = nullptr;
    }

    void *glProc(const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }

    bool writePng(const char *path, const uint8_t *rgb, int width, int height)
    {
        std::vector<uint8_t> raw; // filter byte 0 + RGB per row
        raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgb + static_cast<size_t>(y) * width * 3, rgb + static_cast<size_t>(y + 1) * width * 3);
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0; // adler32
        for (size_t pos = 0; pos < raw.size() || pos == 0;)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - pos);
            z.push_back(pos + n == raw.size() ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>(~n >> 8));
            z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            for (size_t i = pos; i < pos + n;)
            {
                const size_t end = std::min(pos + n, i + 5552); // the most bytes before b can overflow
                for (; i < end; ++i)
                {
                    a += raw[i];
                    b += a;
                }
                a %= 65521u;
                b %= 65521u;
            }
            pos += n;
            if (n == 0)
                break;
        }
        put32(z, (b << 16) | a);

        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        put32(ihdr, static_cast<uint32_t>(width));
        put32(ihdr, static_cast<uint32_t>(height));
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
        chunk(png, "IHDR", ihdr);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && ok;
    }
}
