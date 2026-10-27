// Host platform layer: window, events, keyboard / mouse / gamepad, audio output, frame pacing and the Dear ImGui
// frame. The runtime and the renderer talk to the host only through this header (docs/NATIVE_RENDERER.md);
// platform_sdl3.cpp implements it with SDL3. No SDL, windows.h or graphics-API types appear here.
//
// Threads: init / shutdown / pumpEvents / the frame calls and keyPressed belong to the main thread. The input
// queries (keyDown, mouseDown, gamepad*) read a snapshot taken by pumpEvents and may be called from any thread
// (the game thread reads the pad).
#pragma once

#include <cstdint>
#include <string>

namespace platform
{
    // Key codes: the values raylib / GLFW use, so existing call sites and the control channel keep their numbers.
    enum Key : int
    {
        KEY_NULL = 0,
        KEY_SPACE = 32,
        KEY_APOSTROPHE = 39,
        KEY_COMMA = 44,
        KEY_MINUS = 45,
        KEY_PERIOD = 46,
        KEY_SLASH = 47,
        KEY_ZERO = 48, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE,
        KEY_SEMICOLON = 59,
        KEY_EQUAL = 61,
        KEY_A = 65, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
        KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
        KEY_LEFT_BRACKET = 91,
        KEY_BACKSLASH = 92,
        KEY_RIGHT_BRACKET = 93,
        KEY_GRAVE = 96,
        KEY_ESCAPE = 256,
        KEY_ENTER = 257,
        KEY_TAB = 258,
        KEY_BACKSPACE = 259,
        KEY_INSERT = 260,
        KEY_DELETE = 261,
        KEY_RIGHT = 262,
        KEY_LEFT = 263,
        KEY_DOWN = 264,
        KEY_UP = 265,
        KEY_PAGE_UP = 266,
        KEY_PAGE_DOWN = 267,
        KEY_HOME = 268,
        KEY_END = 269,
        KEY_F1 = 290, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        KEY_KP_0 = 320, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
        KEY_KP_ENTER = 335,
        KEY_LEFT_SHIFT = 340,
        KEY_LEFT_CONTROL = 341,
        KEY_LEFT_ALT = 342,
        KEY_RIGHT_SHIFT = 344,
        KEY_RIGHT_CONTROL = 345,
        KEY_RIGHT_ALT = 346,
        KEY_COUNT = 512,
    };

    enum MouseButton : int
    {
        MOUSE_LEFT,
        MOUSE_RIGHT,
        MOUSE_MIDDLE,
        MOUSE_BUTTON_COUNT,
    };

    // Positions, not labels: SOUTH = Cross / A, EAST = Circle / B, WEST = Square / X, NORTH = Triangle / Y.
    enum GamepadButton : int
    {
        GAMEPAD_SOUTH,
        GAMEPAD_EAST,
        GAMEPAD_WEST,
        GAMEPAD_NORTH,
        GAMEPAD_BACK,
        GAMEPAD_START,
        GAMEPAD_LEFT_STICK,
        GAMEPAD_RIGHT_STICK,
        GAMEPAD_LEFT_SHOULDER,
        GAMEPAD_RIGHT_SHOULDER,
        GAMEPAD_DPAD_UP,
        GAMEPAD_DPAD_DOWN,
        GAMEPAD_DPAD_LEFT,
        GAMEPAD_DPAD_RIGHT,
        GAMEPAD_BUTTON_COUNT,
    };

    // Sticks -1..1 (down / right positive), triggers 0..1.
    enum GamepadAxis : int
    {
        AXIS_LEFT_X,
        AXIS_LEFT_Y,
        AXIS_RIGHT_X,
        AXIS_RIGHT_Y,
        AXIS_LEFT_TRIGGER,
        AXIS_RIGHT_TRIGGER,
        AXIS_COUNT,
    };

    struct WindowDesc
    {
        const char *title = "";
        int width = 640;
        int height = 448;
        bool hidden = false; // not on the desktop or taskbar (scripted test runs)
    };

    // Where the window was on the desktop, to open it there again (main.cpp keeps it in vcs.ini as window.*).
    // x / y / width / height: the restored (not maximized) client area, in screen coordinates relative to the
    // top-left of the monitor it is on. The monitor is found again by name, and by its desktop origin between two of
    // the same model; when it is gone the window opens at the default place.
    struct WindowPlacement
    {
        std::string display;            // monitor name
        int displayX = 0, displayY = 0; // that monitor's desktop origin
        int x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
    };
    using PlacementChanged = void (*)(const WindowPlacement &placement);

    // Before init: open the window at `placement` instead of centred on the primary monitor at the default size.
    void setStartPlacement(const WindowPlacement &placement);
    // Called on the main thread once the window has settled after a move / resize / maximize, and at shutdown. Not
    // used for hidden windows (they are neither placed nor remembered).
    void onPlacementChanged(PlacementChanged callback);

    // Main thread. Full screen: borderless at the desktop's resolution on the monitor the window is on (no display mode
    // change, so Alt+Tab and other monitors stay as they are); off: the window as it was before. Before init: how the
    // window opens. A hidden window only remembers it. In full screen the mouse cursor hides after 2 s without moving.
    void setFullscreen(bool on);
    bool fullscreen();

    // Main thread. Creates the window (no graphics API attached: the renderer makes its swapchain on
    // nativeWindow()) and the Dear ImGui context with its input backend; the ImGui renderer belongs to the RHI.
    bool init(const WindowDesc &desc);
    void shutdown();
    bool ready();
    void *nativeWindow(); // HWND on Windows

    // Main thread, once per host frame, in this order:
    //   pumpEvents()      events + input snapshot
    //   beginFrame()      new Dear ImGui frame (overlays / debug UI may draw from here on; the renderer's ImGui
    //                     backend starts its frame just before)
    //   endUiFrame()      Dear ImGui output finished (ImGui::GetDrawData), for the renderer to draw
    //   ...the renderer draws and presents...
    //   waitNextFrame()   sleeps until the next host frame (1/60 s unless setFrameRate says otherwise)
    void pumpEvents();
    bool quitRequested(); // window closed
    void beginFrame();
    void endUiFrame();
    void waitNextFrame();
    void setFrameRate(double hz); // host frames per second for waitNextFrame (any thread; 60 by default)
    double displayRefreshHz();    // the window's monitor's refresh rate (60 when unknown)
    void delayPrecise(uint64_t ns); // sleep with sub-millisecond precision (any thread)

    void windowSize(int &width, int &height); // drawable pixels

    // Input (any thread; snapshot from the last pumpEvents).
    bool keyDown(int key);
    bool keyPressed(int key); // main thread: went down at the last pumpEvents
    bool mouseDown(MouseButton button);
    bool gamepadConnected();
    bool gamepadButton(GamepadButton button);
    float gamepadAxis(GamepadAxis axis);

    // Audio output: interleaved 16-bit stereo at `rate`, pulled by `fill` on the audio thread.
    using AudioFill = void (*)(int16_t *stereo, uint32_t frames);
    bool audioAvailable(); // a playback device exists
    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill);
    void audioClose();

    // PNG (8-bit RGB, rows top-down) - screenshots.
    bool writePng(const char *path, const uint8_t *rgb, int width, int height);
}
