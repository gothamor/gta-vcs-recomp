// See vcs_input.h.
//
// The game reads the pad once per game frame (scePadRead, game thread). The bindings live in the settings registry
// (main thread); every change rebuilds one Map, which the game thread copies under a mutex at each read.
#include "game/vcs_input.h"

#include "game/vcs_freecam.h"
#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_pad.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vcs::input
{
    namespace
    {
        // DualShock 2 buttons in the report's 16-bit word (active low)
        constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020,
                           kDown = 0x0040, kLeft = 0x0080, kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800,
                           kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;

        // DualSense sources for a PS2 button (the Controller page's choices): platform::GamepadButton, or a trigger.
        constexpr int kPadNone = -1, kPadL2 = 20, kPadR2 = 21;

        struct Control
        {
            const char *id;    // vcs.ini: pad.<id>
            const char *label;
            uint16_t mask;
            int pad;           // the default DualSense source: the same button as on the PS2
        };

        using namespace platform;
        const Control kControls[] = {
            {"cross", "Cross (sprint, accelerate)", kCross, GAMEPAD_SOUTH},
            {"square", "Square (jump, brake)", kSquare, GAMEPAD_WEST},
            {"triangle", "Triangle (enter vehicle)", kTriangle, GAMEPAD_NORTH},
            {"circle", "Circle (attack)", kCircle, GAMEPAD_EAST},
            {"l1", "L1", kL1, GAMEPAD_LEFT_SHOULDER},
            {"r1", "R1 (target)", kR1, GAMEPAD_RIGHT_SHOULDER},
            {"l2", "L2", kL2, kPadL2},
            {"r2", "R2", kR2, kPadR2},
            {"l3", "L3 (left stick click)", kL3, GAMEPAD_LEFT_STICK},
            {"r3", "R3 (right stick click)", kR3, GAMEPAD_RIGHT_STICK},
            {"start", "Start (pause menu)", kStart, GAMEPAD_START},
            {"select", "Select", kSelect, GAMEPAD_BACK},
            {"dpad_up", "D-pad up", kUp, GAMEPAD_DPAD_UP},
            {"dpad_down", "D-pad down", kDown, GAMEPAD_DPAD_DOWN},
            {"dpad_left", "D-pad left", kLeft, GAMEPAD_DPAD_LEFT},
            {"dpad_right", "D-pad right", kRight, GAMEPAD_DPAD_RIGHT},
        };
        constexpr size_t kControlCount = sizeof(kControls) / sizeof(kControls[0]);

        // Everything a pad read needs, rebuilt from the settings on every change.
        struct Map
        {
            std::array<int, kControlCount> pad{};
            bool swapSticks = false, invertX = false, invertY = false;
            float deadzoneLeft = 0.12f, deadzoneRight = 0.12f, cameraScale = 1.0f, trigger = 0.3f;
            float vibration = 1.0f;
        };

        std::mutex g_mapMutex;
        Map g_map;
        bool g_enabled = true; // VCS_INPUT_MAP
        std::atomic<uint64_t> g_lastReport{0}; // buttons | rx << 16 | ry << 24 | lx << 32 | ly << 40 ("padstate")
        std::atomic<uint32_t> g_reads{0};      // pad reads so far ("padstate")

        std::string padId(const Control &c) { return std::string("pad.") + c.id; }

        void rebuild()
        {
            Map m;
            for (size_t i = 0; i < kControlCount; ++i)
                m.pad[i] = settings::value(padId(kControls[i]));
            m.swapSticks = settings::value("pad.swap_sticks") != 0;
            m.invertX = settings::value("pad.invert_x") != 0;
            m.invertY = settings::value("pad.invert_y") != 0;
            m.deadzoneLeft = settings::value("pad.deadzone_left") / 100.0f;
            m.deadzoneRight = settings::value("pad.deadzone_right") / 100.0f;
            m.cameraScale = settings::value("pad.camera_speed") / 100.0f;
            m.trigger = settings::value("pad.trigger") / 100.0f;
            m.vibration = settings::value("pad.vibration") / 100.0f;
            std::lock_guard<std::mutex> lock(g_mapMutex);
            g_map = m;
        }

        // ---- The menu chords (ui/: touchpad + Options the settings, + R1 the trainer, + L1 the free camera) ----------
        // The touchpad click is the PS2's Select, and a chord starts with it: neither it nor the chord's second button
        // may reach the game on the way (Select would change the view, Start pause behind the menu - the menu blocks the
        // pad only from the host frame that opens it). So Select reaches the game once the touchpad has been held
        // kChordGrace on its own, or as a short press when it is let go sooner; while it is held, Options / L1 / R1 do
        // not, and a hold that saw one of them is a chord to the end. The game reads the pad on its own thread only.
        using Clock = std::chrono::steady_clock;
        constexpr auto kChordGrace = std::chrono::milliseconds(150);
        constexpr auto kSelectTap = std::chrono::milliseconds(100); // > 2 game frames at 30 fps: one read sees it
        struct ChordGuard
        {
            Clock::time_point backSince{}; // the touchpad went down (epoch: it is up)
            bool chord = false;            // Options, L1 or R1 went down during this hold
            Clock::time_point tapUntil{};  // a quick touchpad press played out as Select until then
        } g_chord;

        std::array<bool, GAMEPAD_BUTTON_COUNT> gameButtons(Clock::time_point now)
        {
            std::array<bool, GAMEPAD_BUTTON_COUNT> b{};
            for (int i = 0; i < GAMEPAD_BUTTON_COUNT; ++i)
                b[i] = gamepadButton(static_cast<GamepadButton>(i));
            if (b[GAMEPAD_BACK])
            {
                if (g_chord.backSince == Clock::time_point{})
                {
                    g_chord.backSince = now;
                    g_chord.chord = false;
                }
                g_chord.chord = g_chord.chord || b[GAMEPAD_START] || b[GAMEPAD_LEFT_SHOULDER] || b[GAMEPAD_RIGHT_SHOULDER];
                b[GAMEPAD_BACK] = !g_chord.chord && now - g_chord.backSince >= kChordGrace;
                b[GAMEPAD_START] = b[GAMEPAD_LEFT_SHOULDER] = b[GAMEPAD_RIGHT_SHOULDER] = false;
            }
            else
            {
                if (g_chord.backSince != Clock::time_point{} && !g_chord.chord && now - g_chord.backSince < kChordGrace)
                    g_chord.tapUntil = now + kSelectTap;
                g_chord.backSince = {};
                b[GAMEPAD_BACK] = now < g_chord.tapUntil;
            }
            return b;
        }

        bool padSource(int source, float trigger, const std::array<bool, GAMEPAD_BUTTON_COUNT> &buttons)
        {
            if (source >= 0 && source < GAMEPAD_BUTTON_COUNT)
                return buttons[source];
            if (source == kPadL2)
                return gamepadAxis(AXIS_LEFT_TRIGGER) > trigger;
            if (source == kPadR2)
                return gamepadAxis(AXIS_RIGHT_TRIGGER) > trigger;
            return false;
        }

        // Radial deadzone, the rest of the range stretched to 0..1 so a small tilt past it still counts.
        void deadzone(float &x, float &y, float dz)
        {
            const float m = std::sqrt(x * x + y * y);
            if (m <= dz || dz >= 0.99f)
            {
                x = y = 0.0f;
                return;
            }
            const float k = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
            x *= k;
            y *= k;
        }

        uint8_t toByte(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return static_cast<uint8_t>(v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f);
        }

        bool readPad(uint8_t *data)
        {
            Map m;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                m = g_map;
            }
            uint16_t buttons = 0;
            float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
            // the free camera has the controls (takeFreeCameraInput): the game gets a pad left alone
            const bool freeCamera = freecam::flying();
            const auto now = Clock::now();

            if (ps2control::physicalInputOn() && gamepadConnected() && !freeCamera)
            {
                const auto pad = gameButtons(now);
                for (size_t i = 0; i < kControlCount; ++i)
                    if (padSource(m.pad[i], m.trigger, pad))
                        buttons |= kControls[i].mask;
                lx = gamepadAxis(AXIS_LEFT_X);
                ly = gamepadAxis(AXIS_LEFT_Y);
                rx = gamepadAxis(AXIS_RIGHT_X);
                ry = gamepadAxis(AXIS_RIGHT_Y);
                if (m.swapSticks)
                {
                    std::swap(lx, rx);
                    std::swap(ly, ry);
                }
                deadzone(lx, ly, m.deadzoneLeft);
                deadzone(rx, ry, m.deadzoneRight);
                rx *= (m.invertX ? -1.0f : 1.0f) * m.cameraScale;
                ry *= (m.invertY ? -1.0f : 1.0f) * m.cameraScale;
            }
            // buttons held through the control channel (runtime/ps2_control.h), unless a menu has the input
            if (!ps2control::gameInputBlocked() && !freeCamera)
                buttons |= ps2control::injectedButtons();

            const uint16_t word = static_cast<uint16_t>(~buttons);
            data[2] = static_cast<uint8_t>(word & 0xFF);
            data[3] = static_cast<uint8_t>(word >> 8);
            data[4] = toByte(rx);
            data[5] = toByte(ry);
            data[6] = toByte(lx);
            data[7] = toByte(ly);
            g_reads.fetch_add(1, std::memory_order_relaxed);
            g_lastReport.store(static_cast<uint64_t>(word) | static_cast<uint64_t>(data[4]) << 16 | static_cast<uint64_t>(data[5]) << 24 |
                                   static_cast<uint64_t>(data[6]) << 32 | static_cast<uint64_t>(data[7]) << 40,
                               std::memory_order_relaxed);
            return true;
        }

        // The game's motors: the small one is on / off (the DualSense's fast, high-frequency motor), the large one
        // 0..255 (the slow one).
        void actuators(uint8_t small, uint8_t large)
        {
            float strength = 1.0f;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                strength = g_map.vibration;
            }
            static bool logged = false;
            if ((small || large) && !logged)
            {
                logged = true;
                std::fprintf(stderr, "[input] the game vibrates the pad (small %u, large %u)\n", small, large);
            }
            if (!ps2control::physicalInputOn())
                strength = 0.0f;
            gamepadRumble(std::min(1.0f, large / 255.0f * strength), small ? std::min(1.0f, strength) : 0.0f);
        }

        // The DualSense's buttons for a PS2 button.
        std::vector<settings::Choice> padChoices()
        {
            return {{"Cross", GAMEPAD_SOUTH},
                    {"Circle", GAMEPAD_EAST},
                    {"Square", GAMEPAD_WEST},
                    {"Triangle", GAMEPAD_NORTH},
                    {"L1", GAMEPAD_LEFT_SHOULDER},
                    {"R1", GAMEPAD_RIGHT_SHOULDER},
                    {"L2", kPadL2},
                    {"R2", kPadR2},
                    {"L3 (left stick click)", GAMEPAD_LEFT_STICK},
                    {"R3 (right stick click)", GAMEPAD_RIGHT_STICK},
                    {"Options", GAMEPAD_START},
                    {"Touchpad click", GAMEPAD_BACK}, // the Create button is the system's (platform_ps5.cpp)
                    {"D-pad up", GAMEPAD_DPAD_UP},
                    {"D-pad down", GAMEPAD_DPAD_DOWN},
                    {"D-pad left", GAMEPAD_DPAD_LEFT},
                    {"D-pad right", GAMEPAD_DPAD_RIGHT},
                    {"None", kPadNone}};
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        auto onChange = [](int) { rebuild(); };
        const std::string kPage = "Controller";

        for (const Control &c : kControls)
        {
            s = {};
            s.key = padId(c);
            s.page = kPage;
            s.section = "Buttons";
            s.label = c.label;
            s.help = std::string("The DualSense button for the PS2's ") + c.label + ". Several PS2 buttons can share one.";
            s.type = Type::Choice;
            s.choices = padChoices();
            s.defaultValue = c.pad;
            s.onChange = onChange;
            settings::add(s);
        }

        struct Percent
        {
            const char *key, *label, *help;
            int min, max, def;
        };
        const Percent sticks[] = {
            {"pad.deadzone_left", "Left stick deadzone", "How far the left stick moves before it counts (worn sticks rest a "
                                                          "little off centre). The rest of its range still reaches full tilt.",
             0, 40, 12},
            {"pad.deadzone_right", "Right stick deadzone", "The same for the right stick (the camera).", 0, 40, 12},
            {"pad.camera_speed", "Camera speed", "How fast the right stick turns the camera (above 100% full speed comes before "
                                                 "full tilt).",
             25, 300, 100},
            {"pad.trigger", "Trigger press point", "How far L2 / R2 go down before they press (they are buttons on the PS2).",
             5, 95, 30},
        };
        for (const Percent &p : sticks)
        {
            s = {};
            s.key = p.key;
            s.page = kPage;
            s.section = "Sticks and triggers";
            s.label = p.label;
            s.help = p.help;
            s.type = Type::Integer;
            s.min = p.min;
            s.max = p.max;
            s.format = "%d%%";
            s.defaultValue = p.def;
            s.onChange = onChange;
            settings::add(s);
        }
        struct Flag
        {
            const char *key, *label, *help;
        };
        const Flag flags[] = {
            {"pad.swap_sticks", "Swap sticks", "Move with the right stick and turn the camera with the left (left-handed)."},
            {"pad.invert_x", "Invert camera left / right", "The right stick turns the camera the other way."},
            {"pad.invert_y", "Invert camera up / down", "Pushing the right stick up looks down."},
        };
        for (const Flag &f : flags)
        {
            s = {};
            s.key = f.key;
            s.page = kPage;
            s.section = "Sticks and triggers";
            s.label = f.label;
            s.help = f.help;
            s.type = Type::Toggle;
            s.onChange = onChange;
            settings::add(s);
        }

        s = {};
        s.key = "pad.vibration";
        s.page = kPage;
        s.section = "Vibration";
        s.label = "Vibration";
        s.help = "The DualShock 2's rumble on the DualSense when the game asks for it (crashes, gunfire, rough ground): as "
                 "strong as on the PS2 at 100%, weaker or stronger; 0% is off.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 200;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = onChange;
        settings::add(s);
    }

    void install()
    {
        const char *env = std::getenv("VCS_INPUT_MAP");
        g_enabled = !(env && env[0] == '0');
        rebuild();
        if (g_enabled)
            PSPadBackend::setReader(&readPad);
        PSPadBackend::setActuatorSink(&actuators);
        std::fprintf(stderr, "[input] %s\n", g_enabled ? "player bindings" : "built-in mapping (VCS_INPUT_MAP=0)");
        // padstate -> ok buttons=<pressed, hex> rx= ry= lx= ly= reads= (the last report the bindings made, the reads so
        // far)
        ps2control::addCommand("padstate", [](const std::string &, ps2control::Reply reply) {
            const uint64_t r = g_lastReport.load(std::memory_order_relaxed);
            char text[128];
            std::snprintf(text, sizeof(text), "ok buttons=%04x rx=%u ry=%u lx=%u ly=%u reads=%u", static_cast<unsigned>(~r & 0xFFFF),
                          static_cast<unsigned>(r >> 16 & 0xFF), static_cast<unsigned>(r >> 24 & 0xFF),
                          static_cast<unsigned>(r >> 32 & 0xFF), static_cast<unsigned>(r >> 40 & 0xFF),
                          g_reads.load(std::memory_order_relaxed));
            reply(text);
        });
    }

    void stopVibration() { gamepadRumble(0.0f, 0.0f); }

    FreeCamInput takeFreeCameraInput()
    {
        Map m;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            m = g_map;
        }
        FreeCamInput in;
        if (ps2control::gameInputBlocked() || !ps2control::physicalInputOn() || !gamepadConnected())
            return in; // a menu has the input: the camera holds still

        // left stick moves, right stick looks, R2 up, L2 down, Cross fast, Circle slow, D-pad up / down zoom
        float lx = gamepadAxis(AXIS_LEFT_X), ly = gamepadAxis(AXIS_LEFT_Y);
        float rx = gamepadAxis(AXIS_RIGHT_X), ry = gamepadAxis(AXIS_RIGHT_Y);
        if (m.swapSticks)
        {
            std::swap(lx, rx);
            std::swap(ly, ry);
        }
        deadzone(lx, ly, m.deadzoneLeft);
        deadzone(rx, ry, m.deadzoneRight);
        in.strafe = std::clamp(lx, -1.0f, 1.0f);
        in.forward = std::clamp(-ly, -1.0f, 1.0f);
        in.turnRate = -rx * (m.invertX ? -1.0f : 1.0f) * m.cameraScale;
        in.tiltRate = -ry * (m.invertY ? -1.0f : 1.0f) * m.cameraScale;
        in.rise = std::clamp(gamepadAxis(AXIS_RIGHT_TRIGGER) - gamepadAxis(AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
        in.zoom = (gamepadButton(GAMEPAD_DPAD_UP) ? 1.0f : 0.0f) - (gamepadButton(GAMEPAD_DPAD_DOWN) ? 1.0f : 0.0f);
        in.fast = gamepadButton(GAMEPAD_SOUTH);
        in.slow = gamepadButton(GAMEPAD_EAST);
        return in;
    }
}
