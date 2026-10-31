// PS5 implementation of platform.h (docs/PS5.md). There is no window and no SDL: the picture goes to the TV through
// VK_KHR_display (PS5_Mesa's RADV, linked into the game), the DualSense is read with scePad, sound goes out through
// sceAudioOut, and the Dear ImGui frame (the settings menu, the trainer) is fed from the pad. No keyboard or mouse:
// keyDown / mouseDown are always false, and the menus open with the PC port's pad chords (touchpad + Options: settings;
// touchpad + R1: trainer; touchpad + L1: free camera), the touchpad click being the PS2's Select.
//
// What the console's libraries need, and where it was found (the gta4-ps5 port and ps5-plume-triangle, both run on
// consoles; docs/PS5.md):
//   - scePad: the head of ScePadData (buttons, sticks, triggers) as ps5-plume-triangle verified it on a console. The
//     Create button is the system's (apps never see it); rumble needs the pad's vibration mode 2 or it is dropped.
//   - sceAudioOut: 48 kHz, 256-frame buffers of 16-bit stereo; sceAudioOutOutput blocks until the previous buffer has
//     started playing, so the output thread runs at the speakers' pace.
//   - the TV: VK_KHR_display's first display that reports modes, a plane that can show it, and
//     sceSystemServiceHideSplashScreen first (plume-ps5's createPs5DisplaySurface).
//   - clock_gettime is a system call on the PS5: frame pacing reads the TSC (sceKernelReadTsc) instead.
#include "platform/platform.h"

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <pthread.h>
#include <pthread_np.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C"
{
    int sceUserServiceInitialize(void *params);
    int sceUserServiceGetInitialUser(int32_t *userId);
    int sceUserServiceGetForegroundUser(int32_t *userId);

    int scePadInit(void);
    int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param);
    int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
    int scePadReadState(int32_t handle, void *data);
    int scePadClose(int32_t handle);
    int scePadSetVibrationMode(int32_t handle, int32_t mode);
    struct ScePadVibrationParam
    {
        uint8_t largeMotor;
        uint8_t smallMotor;
    };
    int scePadSetVibration(int32_t handle, const ScePadVibrationParam *param);

    int32_t sceAudioOutInit(void);
    int32_t sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t length, uint32_t frequency, uint32_t format);
    int32_t sceAudioOutOutput(int32_t handle, const void *samples);
    int32_t sceAudioOutClose(int32_t handle);

    int sceSystemServiceHideSplashScreen(void);
    uint64_t sceKernelReadTsc(void);
    uint64_t sceKernelGetTscFrequency(void);

    // the system's notification (the payload SDK's samples/hello_world)
    struct Ps5NotifyRequest
    {
        char reserved[45];
        char message[3075];
    };
    int sceKernelSendNotificationRequest(int device, Ps5NotifyRequest *request, size_t size, int blocking);

    // RADV's ICD entry point (PS5_Mesa, linked in by tools/ps5_link.py); weak, so the platform layer links without it
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
        __attribute__((weak));
}

namespace platform
{
    namespace
    {
        // ---- time ---------------------------------------------------------------------------------------------
        uint64_t tscFrequency()
        {
            static const uint64_t frequency = sceKernelGetTscFrequency();
            return frequency;
        }

        // nanoseconds on the TSC (invariant, in step across the cores); clock_gettime when the kernel gives no rate
        uint64_t nowNs()
        {
            const uint64_t f = tscFrequency();
            if (!f)
            {
                timespec ts{};
                clock_gettime(CLOCK_MONOTONIC, &ts);
                return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
            }
            const uint64_t t = sceKernelReadTsc();
            return t / f * 1000000000ull + t % f * 1000000000ull / f; // (the remainder times 1e9 fits: f < 2^34)
        }

        int64_t steadyNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ---- state --------------------------------------------------------------------------------------------
        bool g_ready = false;
        bool g_imgui = false;
        bool g_hidden = false;
        bool g_fullscreen = true;
        uint64_t g_lastUiNs = 0;

        std::atomic<uint64_t> g_framePeriodNs{1000000000ull / 60u}; // setFrameRate
        uint64_t g_nextFrameNs = 0;
        uint64_t g_leadNs = 0;
        FrameLead g_atLead = nullptr;
        bool g_leadRan = false, g_slept = false;
        std::atomic<int64_t> g_snapshotNs{0};

        // the TV: the display mode the surface was made for (1080p until then)
        std::atomic<int> g_width{1920}, g_height{1080};
        std::atomic<double> g_refreshHz{60.0};

        // the DualSense
        constexpr int32_t kNoUser = -1;
        int32_t g_user = kNoUser;
        int32_t g_pad = -1;
        uint64_t g_padRetryNs = 0;
        bool g_padLogged = false;
        std::array<std::atomic<bool>, GAMEPAD_BUTTON_COUNT> g_padButtons{};
        std::array<std::atomic<float>, AXIS_COUNT> g_padAxes{};
        std::atomic<bool> g_padConnected{false};
        std::atomic<uint32_t> g_rumbleWanted{0}; // low << 8 | high, 0..255 each
        uint32_t g_rumbleSent = ~0u;
        uint64_t g_rumbleSentNs = 0;

        // ScePadData's head (the rest - motion, touch, ... - unused); room for the whole structure
        struct PadData
        {
            uint32_t buttons;
            uint8_t leftX, leftY, rightX, rightY; // 0..255, 128 the centre, Y down
            uint8_t l2, r2;                       // 0..255
            uint8_t rest[246];
        };
        constexpr uint32_t kPadL3 = 0x2, kPadR3 = 0x4, kPadOptions = 0x8, kPadUp = 0x10, kPadRight = 0x20, kPadDown = 0x40,
                           kPadLeft = 0x80, kPadL1 = 0x400, kPadR1 = 0x800, kPadTriangle = 0x1000, kPadCircle = 0x2000,
                           kPadCross = 0x4000, kPadSquare = 0x8000, kPadTouchpad = 0x100000;

        // sound
        std::atomic<bool> g_audioRunning{false};
        int32_t g_audioPort = -1;
        std::thread g_audioThread;
        AudioFill g_audioFill = nullptr;
        constexpr uint32_t kAudioRate = 48000, kAudioFrames = 256;

        void openPad()
        {
            if (g_pad >= 0)
                return;
            const uint64_t now = nowNs();
            if (now < g_padRetryNs)
                return;
            g_padRetryNs = now + 1000000000ull; // a controller can be switched on later: try once a second
            if (g_user == kNoUser)
            {
                int32_t user = kNoUser;
                if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
                    if (sceUserServiceGetForegroundUser(&user) != 0)
                        user = kNoUser;
                g_user = user;
            }
            if (g_user == kNoUser)
                return;
            int32_t handle = scePadOpen(g_user, 0, 0, nullptr);
            if (handle < 0)
                handle = scePadGetHandle(g_user, 0, 0); // opened before (0x80920004: already opened)
            if (handle < 0)
            {
                if (!g_padLogged)
                    std::fprintf(stderr, "[platform] no controller yet (scePadOpen 0x%08x)\n", static_cast<uint32_t>(handle));
                g_padLogged = true;
                return;
            }
            g_pad = handle;
            const int mode = scePadSetVibrationMode(handle, 2); // the dual-motor mode: rumble is dropped without it
            std::fprintf(stderr, "[platform] DualSense of user 0x%x (handle %d, vibration mode 0x%x)\n", static_cast<uint32_t>(g_user),
                         handle, static_cast<uint32_t>(mode));
            g_rumbleSent = ~0u;
        }

        float stick(uint8_t v)
        {
            const float f = (static_cast<int>(v) - 128) / 127.0f;
            return std::clamp(f, -1.0f, 1.0f);
        }

        void snapshotPad()
        {
            openPad();
            PadData d{};
            bool ok = g_pad >= 0 && scePadReadState(g_pad, &d) == 0;
            if (g_pad >= 0 && !ok)
            {
                scePadClose(g_pad); // gone: opened again when it comes back
                g_pad = -1;
                g_padLogged = false;
            }
            // all zero: no controller behind the handle (both sticks exactly at the top-left corner is not a real state)
            if (ok && !d.buttons && !d.leftX && !d.leftY && !d.rightX && !d.rightY)
                ok = false;
            g_padConnected.store(ok, std::memory_order_relaxed);
            const uint32_t b = ok ? d.buttons : 0;
            auto set = [&](GamepadButton button, uint32_t mask) { g_padButtons[button].store((b & mask) != 0, std::memory_order_relaxed); };
            set(GAMEPAD_SOUTH, kPadCross);
            set(GAMEPAD_EAST, kPadCircle);
            set(GAMEPAD_WEST, kPadSquare);
            set(GAMEPAD_NORTH, kPadTriangle);
            set(GAMEPAD_BACK, kPadTouchpad); // the PS2's Select (the Create button is the system's)
            set(GAMEPAD_START, kPadOptions);
            set(GAMEPAD_LEFT_STICK, kPadL3);
            set(GAMEPAD_RIGHT_STICK, kPadR3);
            set(GAMEPAD_LEFT_SHOULDER, kPadL1);
            set(GAMEPAD_RIGHT_SHOULDER, kPadR1);
            set(GAMEPAD_DPAD_UP, kPadUp);
            set(GAMEPAD_DPAD_DOWN, kPadDown);
            set(GAMEPAD_DPAD_LEFT, kPadLeft);
            set(GAMEPAD_DPAD_RIGHT, kPadRight);
            g_padAxes[AXIS_LEFT_X].store(ok ? stick(d.leftX) : 0.0f, std::memory_order_relaxed);
            g_padAxes[AXIS_LEFT_Y].store(ok ? stick(d.leftY) : 0.0f, std::memory_order_relaxed);
            g_padAxes[AXIS_RIGHT_X].store(ok ? stick(d.rightX) : 0.0f, std::memory_order_relaxed);
            g_padAxes[AXIS_RIGHT_Y].store(ok ? stick(d.rightY) : 0.0f, std::memory_order_relaxed);
            g_padAxes[AXIS_LEFT_TRIGGER].store(ok ? d.l2 / 255.0f : 0.0f, std::memory_order_relaxed);
            g_padAxes[AXIS_RIGHT_TRIGGER].store(ok ? d.r2 / 255.0f : 0.0f, std::memory_order_relaxed);

            // vibration: sent on a change, and renewed while it lasts
            const uint32_t rumble = g_rumbleWanted.load(std::memory_order_relaxed);
            const uint64_t now = nowNs();
            if (g_pad >= 0 && (rumble != g_rumbleSent || (rumble && now - g_rumbleSentNs > 500000000ull)))
            {
                const ScePadVibrationParam v{static_cast<uint8_t>(rumble >> 8), static_cast<uint8_t>(rumble & 0xFF)};
                scePadSetVibration(g_pad, &v);
                g_rumbleSent = rumble;
                g_rumbleSentNs = now;
            }
        }

        // The pad as Dear ImGui's gamepad keys (the settings menu turns on gamepad navigation while it is open).
        void feedImGuiPad(ImGuiIO &io)
        {
            const bool connected = g_padConnected.load(std::memory_order_relaxed);
            if (connected)
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            else
                io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
            auto button = [&](ImGuiKey key, GamepadButton b) { io.AddKeyEvent(key, g_padButtons[b].load(std::memory_order_relaxed)); };
            button(ImGuiKey_GamepadStart, GAMEPAD_START);
            button(ImGuiKey_GamepadBack, GAMEPAD_BACK);
            button(ImGuiKey_GamepadFaceDown, GAMEPAD_SOUTH);
            button(ImGuiKey_GamepadFaceRight, GAMEPAD_EAST);
            button(ImGuiKey_GamepadFaceLeft, GAMEPAD_WEST);
            button(ImGuiKey_GamepadFaceUp, GAMEPAD_NORTH);
            button(ImGuiKey_GamepadDpadLeft, GAMEPAD_DPAD_LEFT);
            button(ImGuiKey_GamepadDpadRight, GAMEPAD_DPAD_RIGHT);
            button(ImGuiKey_GamepadDpadUp, GAMEPAD_DPAD_UP);
            button(ImGuiKey_GamepadDpadDown, GAMEPAD_DPAD_DOWN);
            button(ImGuiKey_GamepadL1, GAMEPAD_LEFT_SHOULDER);
            button(ImGuiKey_GamepadR1, GAMEPAD_RIGHT_SHOULDER);
            button(ImGuiKey_GamepadL3, GAMEPAD_LEFT_STICK);
            button(ImGuiKey_GamepadR3, GAMEPAD_RIGHT_STICK);
            // analog: as imgui_impl_sdl3 does (a dead zone, then the rest of the range)
            auto analog = [&](ImGuiKey key, float v, float v0, float v1) {
                const float t = std::clamp((v - v0) / (v1 - v0), 0.0f, 1.0f);
                io.AddKeyAnalogEvent(key, t > 0.1f, t);
            };
            auto axis = [](GamepadAxis a) { return g_padAxes[a].load(std::memory_order_relaxed); };
            analog(ImGuiKey_GamepadL2, axis(AXIS_LEFT_TRIGGER), 0.0f, 1.0f);
            analog(ImGuiKey_GamepadR2, axis(AXIS_RIGHT_TRIGGER), 0.0f, 1.0f);
            constexpr float kDead = 0.25f;
            analog(ImGuiKey_GamepadLStickLeft, -axis(AXIS_LEFT_X), kDead, 1.0f);
            analog(ImGuiKey_GamepadLStickRight, axis(AXIS_LEFT_X), kDead, 1.0f);
            analog(ImGuiKey_GamepadLStickUp, -axis(AXIS_LEFT_Y), kDead, 1.0f);
            analog(ImGuiKey_GamepadLStickDown, axis(AXIS_LEFT_Y), kDead, 1.0f);
            analog(ImGuiKey_GamepadRStickLeft, -axis(AXIS_RIGHT_X), kDead, 1.0f);
            analog(ImGuiKey_GamepadRStickRight, axis(AXIS_RIGHT_X), kDead, 1.0f);
            analog(ImGuiKey_GamepadRStickUp, -axis(AXIS_RIGHT_Y), kDead, 1.0f);
            analog(ImGuiKey_GamepadRStickDown, axis(AXIS_RIGHT_Y), kDead, 1.0f);
        }

        // ---- Vulkan -------------------------------------------------------------------------------------------
        // RADV's entry point, with the core 1.1 names it may only export under their KHR aliases (plume-ps5 maps them)
        VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ps5GetInstanceProcAddr(VkInstance instance, const char *name)
        {
            if (!vk_icdGetInstanceProcAddr || !name)
                return nullptr;
            if (PFN_vkVoidFunction fn = vk_icdGetInstanceProcAddr(instance, name))
                return fn;
            static const char *const kAliases[][2] = {
                {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"},
                {"vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR"},
                {"vkGetPhysicalDeviceMemoryProperties2", "vkGetPhysicalDeviceMemoryProperties2KHR"},
                {"vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceFormatProperties2KHR"},
                {"vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2KHR"},
            };
            for (const auto &a : kAliases)
                if (std::strcmp(name, a[0]) == 0)
                    return vk_icdGetInstanceProcAddr(instance, a[1]);
            return nullptr;
        }

        // the output height asked for (Display > TV output: VCS_PS5_OUTPUT, read when the surface is made); 0 = 4K when
        // the TV has it, else its largest mode
        int wantedOutputHeight()
        {
            const char *v = std::getenv("VCS_PS5_OUTPUT");
            return v && *v ? std::atoi(v) : 0;
        }

        // how well a mode fits: the height asked for (or 2160, else the larger), then a refresh rate near 60 Hz (the
        // game paces to 60; 120 Hz modes also need the app to declare them in param.json)
        int64_t modeScore(const VkDisplayModeParametersKHR &p, int wanted)
        {
            const int64_t h = p.visibleRegion.height;
            const int64_t refreshPenalty = std::llabs(static_cast<int64_t>(p.refreshRate) - 60000) / 100; // 0.1 Hz steps
            const int64_t heightScore = wanted > 0 ? (h == wanted ? 1000000 : 0) : (h == 2160 ? 1000000 : 0);
            return heightScore + h * 100 - refreshPenalty;
        }
    }

    void setStartPlacement(const WindowPlacement &) {} // the TV: no window to place
    void onPlacementChanged(PlacementChanged) {}

    void setFullscreen(bool on) { g_fullscreen = on; } // always the whole TV; remembered for the setting's sake
    bool fullscreen() { return true; }

    bool init(const WindowDesc &desc)
    {
        g_hidden = desc.hidden;
        sceUserServiceInitialize(nullptr); // (an error when it already was: fine)
        const int pad = scePadInit();
        if (pad < 0)
            std::fprintf(stderr, "[platform] scePadInit failed (0x%08x): no controller\n", static_cast<uint32_t>(pad));
        snapshotPad();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendPlatformName = "vcs_ps5";
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // no mouse
        ImGui::StyleColorsDark();
        g_imgui = true;
        g_ready = true;
        std::fprintf(stderr, "[platform] PS5 (TSC %.3f MHz)\n", tscFrequency() / 1e6);
        return true;
    }

    void shutdown()
    {
        if (!g_ready)
            return;
        audioClose();
        if (g_pad >= 0)
        {
            const ScePadVibrationParam still{0, 0};
            scePadSetVibration(g_pad, &still);
            scePadClose(g_pad);
            g_pad = -1;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        g_imgui = false;
        g_ready = false;
    }

    bool ready() { return g_ready; }
    void *nativeWindow() { return nullptr; }
    bool windowHidden() { return g_hidden; }

    const char *const *vulkanInstanceExtensions(uint32_t *count)
    {
        static const char *const kExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
        if (count)
            *count = vk_icdGetInstanceProcAddr ? 2u : 0u;
        return vk_icdGetInstanceProcAddr ? kExtensions : nullptr;
    }

    // The TV as a VkSurfaceKHR: the first display that reports modes, the mode that fits best (modeScore), a plane that
    // can show that display. The surface's size is the mode's visible region (windowSize reports it from then on).
    bool vulkanCreateSurface(void *instancePtr, uint64_t *surface)
    {
        VkInstance instance = static_cast<VkInstance>(instancePtr);
        auto load = [&](const char *name) { return ps5GetInstanceProcAddr(instance, name); };
        auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(load("vkEnumeratePhysicalDevices"));
        auto displayProps =
            reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(load("vkGetPhysicalDeviceDisplayPropertiesKHR"));
        auto planeProps =
            reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(load("vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
        auto planeDisplays =
            reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(load("vkGetDisplayPlaneSupportedDisplaysKHR"));
        auto modeProps = reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(load("vkGetDisplayModePropertiesKHR"));
        auto createSurface = reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(load("vkCreateDisplayPlaneSurfaceKHR"));
        if (!enumerateDevices || !displayProps || !planeProps || !modeProps || !createSurface)
        {
            std::fprintf(stderr, "[platform] the Vulkan driver has no VK_KHR_display\n");
            return false;
        }
        uint32_t n = 0;
        enumerateDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        if (!n || enumerateDevices(instance, &n, gpus.data()) < 0)
        {
            std::fprintf(stderr, "[platform] no GPU\n");
            return false;
        }
        VkPhysicalDevice gpu = gpus[0];

        sceSystemServiceHideSplashScreen(); // the system's start-up picture gives way to ours

        uint32_t displayCount = 0;
        displayProps(gpu, &displayCount, nullptr);
        std::vector<VkDisplayPropertiesKHR> displays(displayCount);
        if (!displayCount || displayProps(gpu, &displayCount, displays.data()) < 0)
        {
            std::fprintf(stderr, "[platform] no display\n");
            return false;
        }
        uint32_t planeCount = 0;
        planeProps(gpu, &planeCount, nullptr);
        std::vector<VkDisplayPlanePropertiesKHR> planes(planeCount);
        if (!planeCount || planeProps(gpu, &planeCount, planes.data()) < 0)
        {
            std::fprintf(stderr, "[platform] no display plane\n");
            return false;
        }

        const int wanted = wantedOutputHeight();
        VkDisplayKHR display = VK_NULL_HANDLE;
        VkDisplayModePropertiesKHR mode{};
        for (const VkDisplayPropertiesKHR &d : displays)
        {
            uint32_t modeCount = 0;
            if (modeProps(gpu, d.display, &modeCount, nullptr) < 0 || !modeCount)
                continue;
            std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
            if (modeProps(gpu, d.display, &modeCount, modes.data()) < 0)
                continue;
            display = d.display;
            mode = *std::max_element(modes.begin(), modes.end(), [&](const auto &a, const auto &b) {
                return modeScore(a.parameters, wanted) < modeScore(b.parameters, wanted);
            });
            for (const VkDisplayModePropertiesKHR &m : modes)
                std::fprintf(stderr, "[platform] TV mode %ux%u at %.2f Hz%s\n", m.parameters.visibleRegion.width,
                             m.parameters.visibleRegion.height, m.parameters.refreshRate / 1000.0,
                             m.displayMode == mode.displayMode ? " (chosen)" : "");
            break;
        }
        if (display == VK_NULL_HANDLE)
        {
            std::fprintf(stderr, "[platform] the display reports no modes\n");
            return false;
        }
        uint32_t plane = 0;
        for (uint32_t i = 0; i < planeCount; ++i)
        {
            bool shows = planes[i].currentDisplay == display || planes[i].currentDisplay == VK_NULL_HANDLE;
            if (!shows && planeDisplays)
            {
                uint32_t count = 0;
                planeDisplays(gpu, i, &count, nullptr);
                std::vector<VkDisplayKHR> can(count);
                planeDisplays(gpu, i, &count, can.data());
                shows = std::find(can.begin(), can.end(), display) != can.end();
            }
            if (shows)
            {
                plane = i;
                break;
            }
        }
        VkDisplaySurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
        info.displayMode = mode.displayMode;
        info.planeIndex = plane;
        info.planeStackIndex = planes[plane].currentStackIndex;
        info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.globalAlpha = 1.0f;
        info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        info.imageExtent = mode.parameters.visibleRegion;
        VkSurfaceKHR made = VK_NULL_HANDLE;
        const VkResult r = createSurface(instance, &info, nullptr, &made);
        if (r != VK_SUCCESS)
        {
            std::fprintf(stderr, "[platform] vkCreateDisplayPlaneSurfaceKHR failed (%d)\n", static_cast<int>(r));
            return false;
        }
        g_width.store(static_cast<int>(info.imageExtent.width));
        g_height.store(static_cast<int>(info.imageExtent.height));
        if (mode.parameters.refreshRate > 1000)
            g_refreshHz.store(mode.parameters.refreshRate / 1000.0);
        std::fprintf(stderr, "[platform] TV: %ux%u at %.2f Hz on plane %u\n", info.imageExtent.width, info.imageExtent.height,
                     mode.parameters.refreshRate / 1000.0, plane);
        *surface = reinterpret_cast<uint64_t>(made);
        return true;
    }

    void *vulkanGetInstanceProcAddr()
    {
        return vk_icdGetInstanceProcAddr ? reinterpret_cast<void *>(&ps5GetInstanceProcAddr) : nullptr;
    }

    void notify(const char *message)
    {
        std::fprintf(stderr, "[platform] %s\n", message);
        Ps5NotifyRequest request{};
        std::snprintf(request.message, sizeof(request.message), "%s", message);
        sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    }

    void pumpEvents(bool frameStart)
    {
        (void)frameStart; // no keys: no press edges to keep between frames
        snapshotPad();
        g_snapshotNs.store(steadyNs(), std::memory_order_release);
    }

    int64_t inputSnapshotNs() { return g_snapshotNs.load(std::memory_order_acquire); }

    bool quitRequested() { return false; } // the system ends the app (PS button, Close Game)

    void beginFrame()
    {
        if (!g_imgui)
            return;
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(g_width.load()), static_cast<float>(g_height.load()));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        const uint64_t now = nowNs();
        io.DeltaTime = g_lastUiNs && now > g_lastUiNs ? std::max(1e-4f, static_cast<float>((now - g_lastUiNs) / 1e9)) : 1.0f / 60.0f;
        g_lastUiNs = now;
        feedImGuiPad(io);
        ImGui::NewFrame();
    }

    void endUiFrame()
    {
        if (g_imgui)
            ImGui::Render();
    }

    void setFrameRate(double hz)
    {
        g_framePeriodNs.store(static_cast<uint64_t>(1e9 / std::clamp(hz, 24.0, 1000.0)), std::memory_order_relaxed);
    }

    double displayRefreshHz() { return g_refreshHz.load(); }

    // nanosleep while more than the spin margin is left, then the TSC: the console's sleeps are not precise enough for
    // frame pacing on their own
    void delayPrecise(uint64_t ns)
    {
        constexpr uint64_t kSpinNs = 1500000;
        const uint64_t end = nowNs() + ns;
        for (;;)
        {
            const uint64_t now = nowNs();
            if (now >= end)
                return;
            const uint64_t left = end - now;
            if (left > kSpinNs)
            {
                const uint64_t sleep = left - kSpinNs;
                timespec ts{static_cast<time_t>(sleep / 1000000000ull), static_cast<long>(sleep % 1000000000ull)};
                nanosleep(&ts, nullptr);
            }
            else
                __builtin_ia32_pause();
        }
    }

    void waitNextFrame()
    {
        const uint64_t period = g_framePeriodNs.load(std::memory_order_relaxed);
        const uint64_t now = nowNs();
        g_nextFrameNs = g_nextFrameNs ? g_nextFrameNs + period : now + period;
        g_leadRan = false;
        g_slept = false;
        if (g_nextFrameNs < now)
        {
            g_nextFrameNs = now; // fell behind: don't try to catch up
            return;
        }
        g_slept = true;
        const uint64_t lead = std::min(g_leadNs, period);
        if (g_atLead && lead && g_nextFrameNs - lead > now)
        {
            delayPrecise(g_nextFrameNs - lead - now);
            g_atLead();
            g_leadRan = true;
            const uint64_t after = nowNs();
            if (g_nextFrameNs > after)
                delayPrecise(g_nextFrameNs - after);
            return;
        }
        delayPrecise(g_nextFrameNs - now);
    }

    void setFrameLead(uint64_t leadNs, FrameLead atLead)
    {
        g_leadNs = leadNs;
        g_atLead = atLead;
    }

    bool frameLeadRan() { return g_leadRan; }
    bool frameSlept() { return g_slept; }

    void windowSize(int &width, int &height)
    {
        width = g_width.load();
        height = g_height.load();
    }

    bool windowFocused() { return !g_hidden; } // the system suspends the app when it leaves the screen

    bool keyDown(int) { return false; }
    bool keyPressed(int) { return false; }
    bool mouseDown(MouseButton) { return false; }
    bool gamepadConnected() { return g_padConnected.load(std::memory_order_relaxed); }
    bool gamepadButton(GamepadButton button) { return g_padButtons[button].load(std::memory_order_relaxed); }
    float gamepadAxis(GamepadAxis axis) { return g_padAxes[axis].load(std::memory_order_relaxed); }
    const char *gamepadName() { return gamepadConnected() ? "DualSense" : ""; }

    void gamepadRumble(float low, float high)
    {
        auto level = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        g_rumbleWanted.store(level(low) << 8 | level(high), std::memory_order_relaxed);
    }

    void setMouseCaptured(bool) {}
    bool mouseCaptured() { return false; }

    void takeMouseMotion(float &dx, float &dy)
    {
        dx = 0.0f;
        dy = 0.0f;
    }

    bool audioAvailable() { return true; } // the system's main output port is always there

    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill)
    {
        (void)bufferFrames; // the port takes 256 frames at a time; the runtime's ring keeps the delay (ps2_audio_out.cpp)
        if (g_audioPort >= 0 || !fill)
            return false;
        if (rate != kAudioRate)
        {
            std::fprintf(stderr, "[platform] audio at %u Hz: the PS5's output is 48000 only\n", rate);
            return false;
        }
        sceAudioOutInit(); // (already initialized: fine)
        constexpr int32_t kSystemUser = 0xFF, kMainPort = 0;
        constexpr uint32_t kS16Stereo = 1;
        g_audioPort = sceAudioOutOpen(kSystemUser, kMainPort, 0, kAudioFrames, kAudioRate, kS16Stereo);
        if (g_audioPort < 0)
        {
            std::fprintf(stderr, "[platform] sceAudioOutOpen failed (0x%08x)\n", static_cast<uint32_t>(g_audioPort));
            g_audioPort = -1;
            return false;
        }
        g_audioFill = fill;
        g_audioRunning.store(true);
        g_audioThread = std::thread([] {
            pthread_set_name_np(pthread_self(), "AudioOut");
            int16_t samples[kAudioFrames * 2];
            while (g_audioRunning.load(std::memory_order_relaxed))
            {
                g_audioFill(samples, kAudioFrames);
                sceAudioOutOutput(g_audioPort, samples); // returns when the last buffer has started playing
            }
        });
        std::fprintf(stderr, "[platform] sound: port %d, 48000 Hz, %u-frame buffers\n", g_audioPort, kAudioFrames);
        return true;
    }

    void audioClose()
    {
        if (g_audioRunning.exchange(false) && g_audioThread.joinable())
            g_audioThread.join();
        if (g_audioPort >= 0)
        {
            sceAudioOutOutput(g_audioPort, nullptr); // waits for the last buffer
            sceAudioOutClose(g_audioPort);
            g_audioPort = -1;
        }
        g_audioFill = nullptr;
    }
}
