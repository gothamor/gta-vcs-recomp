// The PS5 platform layer (platform_ps5.cpp, docs/PS5.md): the TV's Vulkan surface, the DualSense, the audio output,
// frame pacing and the Dear ImGui frame. The runtime and the renderer reach the console only through this header
// (docs/NATIVE_RENDERER.md); platform_common.cpp holds the PNG writer. No graphics-API types appear here.
//
// Threads: init / shutdown / pumpEvents / the frame calls belong to the main thread. The controller queries
// (gamepad*) read a snapshot taken by pumpEvents and may be called from any thread (the game thread reads the pad).
#pragma once

#include <cstdint>

namespace platform
{
    // The DualSense's buttons by position, as the DualShock 2's: SOUTH = Cross, EAST = Circle, WEST = Square,
    // NORTH = Triangle; BACK is the touchpad click (the PS2's Select: the Create button belongs to the system),
    // START is Options.
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

    // Sticks -1..1 (down / right positive), triggers (L2 / R2) 0..1.
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

    // Main thread. Opens the controller and makes the Dear ImGui context (fed from the pad); the renderer makes its
    // swapchain on the TV through vulkanCreateSurface.
    bool init();
    void shutdown();
    bool ready();

    // For the Vulkan renderer (rhi::VulkanWindow). Main thread.
    const char *const *vulkanInstanceExtensions(uint32_t *count); // the instance extensions the surface needs
    bool vulkanCreateSurface(void *instance, uint64_t *surface);  // VkInstance -> VkSurfaceKHR on the TV
    // The Vulkan driver's vkGetInstanceProcAddr (a PFN_vkGetInstanceProcAddr): RADV is linked into the game, there is
    // no loader library.
    void *vulkanGetInstanceProcAddr();

    // A short message for the player outside the game's picture, for when the game cannot start (no disc...): the
    // system's notification.
    void notify(const char *message);

    // Main thread, once per host frame, in this order:
    //   pumpEvents()      the controller snapshot
    //   beginFrame()      new Dear ImGui frame (overlays / menus may draw from here on; the renderer's ImGui backend
    //                     starts its frame just before)
    //   endUiFrame()      Dear ImGui output finished (ImGui::GetDrawData), for the renderer to draw
    //   ...the renderer draws and presents...
    //   waitNextFrame()   sleeps until the next host frame (1/60 s unless setFrameRate says otherwise)
    // frameStart false: a controller refresh between frames (frame lead)
    void pumpEvents(bool frameStart = true);
    // When the controller snapshot the queries read was taken (std::chrono::steady_clock, ns): the start of a frame's
    // input delay (any thread).
    int64_t inputSnapshotNs();
    void beginFrame();
    void endUiFrame();
    void waitNextFrame();
    // Frame lead (main thread): waitNextFrame wakes `leadNs` before the next host frame and calls `atLead` (a controller
    // refresh and the start of the game's next frame, so it is ready by then: docs/PERFORMANCE.md 19); 0 = none.
    // frameLeadRan(): whether the last waitNextFrame did (then the frame start need not start the game's frame).
    using FrameLead = void (*)();
    void setFrameLead(uint64_t leadNs, FrameLead atLead);
    bool frameLeadRan();
    bool frameSlept(); // the last waitNextFrame waited for its time (false: the host frame took the whole period or more)
    void setFrameRate(double hz); // host frames per second for waitNextFrame (any thread; 60 by default)
    double displayRefreshHz();    // the TV mode's refresh rate (60 until the surface is made)
    void delayPrecise(uint64_t ns); // sleep with sub-millisecond precision (any thread)

    void outputSize(int &width, int &height); // the TV mode's pixels (1920x1080 until the surface is made)

    // The DualSense (any thread; snapshot from the last pumpEvents).
    bool gamepadConnected();
    bool gamepadButton(GamepadButton button);
    float gamepadAxis(GamepadAxis axis);
    // Vibration, motors 0..1 (low = the large, slow motor; high = the small, fast one). Any thread; applied at the next
    // pumpEvents and held until changed (0, 0 stops).
    void gamepadRumble(float low, float high);

    // Audio output: interleaved 16-bit stereo at `rate` (48000), pulled by `fill` on the audio thread.
    using AudioFill = void (*)(int16_t *stereo, uint32_t frames);
    bool audioAvailable(); // the main output port is there
    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill);
    void audioClose();

    // PNG (8-bit RGB, rows top-down) - screenshots.
    bool writePng(const char *path, const uint8_t *rgb, int width, int height);
}
