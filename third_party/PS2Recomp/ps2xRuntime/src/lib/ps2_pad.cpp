#include "runtime/ps2_pad.h"
#include <atomic>
#include "ps2_host_backend.h"
#include "runtime/ps2_control.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

namespace
{
    PSPadBackend::Reader g_reader = nullptr;             // [gta-recomp] setReader
    PSPadBackend::ActuatorSink g_actuatorSink = nullptr; // [gta-recomp] setActuatorSink
}

void PSPadBackend::setReader(Reader reader) { g_reader = reader; }

// [gta-recomp] lastInputNs
static std::atomic<int64_t> g_padInputNs{0};
int64_t PSPadBackend::lastInputNs() { return g_padInputNs.load(std::memory_order_relaxed); }
void PSPadBackend::setActuatorSink(ActuatorSink sink) { g_actuatorSink = sink; }

void PSPadBackend::actuators(uint8_t small, uint8_t large)
{
    if (g_actuatorSink)
        g_actuatorSink(small, large);
}

// [gta-recomp] The DualSense feeds one DualShock 2: the game layer's bindings (setReader), else this fixed mapping by
// position (the reference: VCS_INPUT_MAP=0; docs/CONTROLS.md).
bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    g_padInputNs.store(platform::inputSnapshotNs(), std::memory_order_relaxed); // [gta-recomp] lastInputNs
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    if (g_reader) // [gta-recomp] the player's bindings (setReader)
    {
        data[2] = data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        if (g_reader(data))
            return true;
    }

    uint16_t btns = 0xFFFFu;
    auto press = [&btns](bool down, uint16_t mask)
    {
        if (down)
            btns &= ~mask;
    };
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    // [gta-recomp] off in hidden test runs and while an in-game UI (settings / trainer menu) has the input
    const bool physical = ps2control::physicalInputOn();
    if (physical && platform::gamepadConnected())
    {
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_UP), PAD_UP);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_DOWN), PAD_DOWN);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_LEFT), PAD_LEFT);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_RIGHT), PAD_RIGHT);
        press(platform::gamepadButton(platform::GAMEPAD_SOUTH), PAD_CROSS);
        press(platform::gamepadButton(platform::GAMEPAD_EAST), PAD_CIRCLE);
        press(platform::gamepadButton(platform::GAMEPAD_WEST), PAD_SQUARE);
        press(platform::gamepadButton(platform::GAMEPAD_NORTH), PAD_TRIANGLE);
        press(platform::gamepadButton(platform::GAMEPAD_LEFT_SHOULDER), PAD_L1);
        press(platform::gamepadButton(platform::GAMEPAD_RIGHT_SHOULDER), PAD_R1);
        // Triggers are analog axes (0..1)
        press(platform::gamepadAxis(platform::AXIS_LEFT_TRIGGER) > 0.3f, PAD_L2);
        press(platform::gamepadAxis(platform::AXIS_RIGHT_TRIGGER) > 0.3f, PAD_R2);
        press(platform::gamepadButton(platform::GAMEPAD_START), PAD_START);
        press(platform::gamepadButton(platform::GAMEPAD_BACK), PAD_SELECT);
        press(platform::gamepadButton(platform::GAMEPAD_LEFT_STICK), PAD_L3);
        press(platform::gamepadButton(platform::GAMEPAD_RIGHT_STICK), PAD_R3);
        // small radial deadzone: worn sticks rest a few percent off centre
        auto deadzone = [](float &x, float &y)
        {
            if (x * x + y * y < 0.12f * 0.12f)
                x = y = 0.0f;
        };
        lx = platform::gamepadAxis(platform::AXIS_LEFT_X);
        ly = platform::gamepadAxis(platform::AXIS_LEFT_Y);
        rx = platform::gamepadAxis(platform::AXIS_RIGHT_X);
        ry = platform::gamepadAxis(platform::AXIS_RIGHT_Y);
        deadzone(lx, ly);
        deadzone(rx, ry);
    }

    // [gta-recomp] buttons held through the control channel (runtime/ps2_control.h), unless a menu has the input
    if (!ps2control::gameInputBlocked())
        btns = static_cast<uint16_t>(btns & ~ps2control::injectedButtons());

    auto toByte = [](float v)
    {
        v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint8_t>(v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f);
    };
    data[4] = toByte(rx);
    data[5] = toByte(ry);
    data[6] = toByte(lx);
    data[7] = toByte(ly);
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
