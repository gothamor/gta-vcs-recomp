#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
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

// [gta-recomp] Keyboard, mouse and gamepad all feed one DualShock 2. Keyboard layout (see docs/CONTROLS.md):
//   WASD left stick, IJKL right stick, arrows D-pad, Shift Cross, Space Square, F Triangle, Ctrl/LMB Circle,
//   RMB/E R1, Q L1, 1/Z L2, 3/X R2, C L3, V R3, Enter Start, Backspace Select.
bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;

    uint16_t btns = 0xFFFFu;
    auto press = [&btns](bool down, uint16_t mask)
    {
        if (down)
            btns &= ~mask;
    };
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    constexpr int kGamepad = 0;
    if (IsGamepadAvailable(kGamepad))
    {
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP), PAD_UP);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN), PAD_DOWN);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT), PAD_LEFT);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT), PAD_RIGHT);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN), PAD_CROSS);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT), PAD_CIRCLE);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT), PAD_SQUARE);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP), PAD_TRIANGLE);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1), PAD_L1);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1), PAD_R1);
        // Analog triggers report through the axes on most XInput pads
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2) ||
                  GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_TRIGGER) > 0.3f,
              PAD_L2);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2) ||
                  GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.3f,
              PAD_R2);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT), PAD_START);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT), PAD_SELECT);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB), PAD_L3);
        press(IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB), PAD_R3);
        // small radial deadzone: worn sticks rest a few percent off centre
        auto deadzone = [](float &x, float &y)
        {
            if (x * x + y * y < 0.12f * 0.12f)
                x = y = 0.0f;
        };
        lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        deadzone(lx, ly);
        deadzone(rx, ry);
    }

    auto key = [](int k)
    { return IsKeyDown(k); };
    press(key(KEY_UP), PAD_UP);
    press(key(KEY_DOWN), PAD_DOWN);
    press(key(KEY_LEFT), PAD_LEFT);
    press(key(KEY_RIGHT), PAD_RIGHT);
    press(key(KEY_LEFT_SHIFT) || key(KEY_RIGHT_SHIFT), PAD_CROSS);
    press(key(KEY_SPACE), PAD_SQUARE);
    press(key(KEY_F), PAD_TRIANGLE);
    press(key(KEY_LEFT_CONTROL) || key(KEY_RIGHT_CONTROL) || IsMouseButtonDown(MOUSE_BUTTON_LEFT), PAD_CIRCLE);
    press(key(KEY_Q), PAD_L1);
    press(key(KEY_E) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT), PAD_R1);
    press(key(KEY_ONE) || key(KEY_Z), PAD_L2);
    press(key(KEY_THREE) || key(KEY_X), PAD_R2);
    press(key(KEY_C), PAD_L3);
    press(key(KEY_V), PAD_R3);
    press(key(KEY_ENTER) || key(KEY_KP_ENTER), PAD_START);
    press(key(KEY_BACKSPACE), PAD_SELECT);

    auto axis = [](float current, bool negative, bool positive)
    {
        if (negative != positive)
            return negative ? -1.0f : 1.0f;
        return current;
    };
    lx = axis(lx, key(KEY_A), key(KEY_D));
    ly = axis(ly, key(KEY_W), key(KEY_S));
    rx = axis(rx, key(KEY_J), key(KEY_L));
    ry = axis(ry, key(KEY_I), key(KEY_K));

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
