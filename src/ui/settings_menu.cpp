// See settings_menu.h.
#include "ui/settings_menu.h"

#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        bool g_open = false;
        bool g_releaseWait = false;  // closed, but the keys / buttons that closed it are still down
        bool g_chordWasDown = false; // Back + Start
        bool g_popupWasOpen = false; // a combo's list: Escape / B close it, not the menu
        bool g_focusWindow = false;
        int g_page = 0, g_selectPage = -1;
        std::string g_capturing; // the Key setting waiting for a key press

        const char *keyName(int key)
        {
            static char buffer[16];
            using namespace platform;
            if (key >= KEY_A && key <= KEY_Z)
                return (buffer[0] = static_cast<char>(key), buffer[1] = 0, buffer);
            if (key >= KEY_ZERO && key <= KEY_NINE)
                return (buffer[0] = static_cast<char>(key), buffer[1] = 0, buffer);
            if (key >= KEY_F1 && key <= KEY_F12)
                return (std::snprintf(buffer, sizeof(buffer), "F%d", key - KEY_F1 + 1), buffer);
            if (key >= KEY_KP_0 && key <= KEY_KP_9)
                return (std::snprintf(buffer, sizeof(buffer), "Keypad %d", key - KEY_KP_0), buffer);
            switch (key)
            {
            case KEY_SPACE: return "Space";
            case KEY_GRAVE: return "`";
            case KEY_MINUS: return "-";
            case KEY_EQUAL: return "=";
            case KEY_LEFT_BRACKET: return "[";
            case KEY_RIGHT_BRACKET: return "]";
            case KEY_BACKSLASH: return "\\";
            case KEY_SEMICOLON: return ";";
            case KEY_APOSTROPHE: return "'";
            case KEY_COMMA: return ",";
            case KEY_PERIOD: return ".";
            case KEY_SLASH: return "/";
            case KEY_TAB: return "Tab";
            case KEY_INSERT: return "Insert";
            case KEY_DELETE: return "Delete";
            case KEY_HOME: return "Home";
            case KEY_END: return "End";
            case KEY_PAGE_UP: return "Page Up";
            case KEY_PAGE_DOWN: return "Page Down";
            default: return "?";
            }
        }

        // Keys the menu key may not take: the menu's own (Escape, Enter, arrows, Q / E), the game's (ps2_pad.cpp) and
        // the modifiers.
        bool bindable(int key)
        {
            using namespace platform;
            static constexpr int kTaken[] = {KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_BACKSPACE, KEY_SPACE, KEY_W, KEY_A, KEY_S,
                                             KEY_D, KEY_Q, KEY_E, KEY_F, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_I, KEY_J, KEY_K, KEY_L,
                                             KEY_ONE, KEY_THREE};
            if (std::find(std::begin(kTaken), std::end(kTaken), key) != std::end(kTaken) || (key >= KEY_RIGHT && key <= KEY_UP) ||
                key >= KEY_LEFT_SHIFT)
                return false;
            return std::string(keyName(key)) != "?";
        }

        bool gamepadDown(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        bool anyInputHeld()
        {
            for (int k = 1; k < platform::KEY_COUNT; ++k)
                if (platform::keyDown(k))
                    return true;
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (gamepadDown(static_cast<platform::GamepadButton>(b)))
                    return true;
            return platform::mouseDown(platform::MOUSE_LEFT) || platform::mouseDown(platform::MOUSE_RIGHT);
        }

        void open()
        {
            settings::refresh();
            g_open = true;
            g_focusWindow = true;
            g_capturing.clear();
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        }

        void close()
        {
            g_open = false;
            g_releaseWait = true; // the game would otherwise see Escape / B / Start held
            g_capturing.clear();
            ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        }

        // One row: the label, then the setting's control. Returns the help text when the row is selected or hovered.
        void row(const settings::Spec &s, std::string &help)
        {
            const bool locked = settings::overridden(s.key);
            int v = settings::value(s.key);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.label.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(s.key.c_str());
            ImGui::BeginDisabled(locked);
            ImGui::SetNextItemWidth(-FLT_MIN);
            switch (s.type)
            {
            case settings::Type::Toggle:
            {
                bool on = v != 0;
                if (ImGui::Checkbox(on ? "On###v" : "Off###v", &on))
                    settings::set(s.key, on ? 1 : 0);
                break;
            }
            case settings::Type::Choice:
            {
                auto current = std::find_if(s.choices.begin(), s.choices.end(), [&](const settings::Choice &c) { return c.value == v; });
                if (ImGui::BeginCombo("##v", current != s.choices.end() ? current->label.c_str() : "?"))
                {
                    for (const settings::Choice &c : s.choices)
                    {
                        if (ImGui::Selectable(c.label.c_str(), c.value == v))
                            settings::set(s.key, c.value);
                        if (c.value == v)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            case settings::Type::Integer:
                if (ImGui::SliderInt("##v", &v, s.min, s.max, s.format.c_str(), ImGuiSliderFlags_AlwaysClamp))
                    settings::set(s.key, v);
                break;
            case settings::Type::Key:
                if (g_capturing == s.key)
                {
                    ImGui::Button("Press a key... (Esc: cancel)", ImVec2(-FLT_MIN, 0));
                    if (platform::keyPressed(platform::KEY_ESCAPE))
                        g_capturing.clear();
                    else
                        for (int k = 1; k < platform::KEY_COUNT; ++k)
                            if (platform::keyPressed(k) && bindable(k))
                            {
                                settings::set(s.key, k);
                                g_capturing.clear();
                                break;
                            }
                }
                else if (ImGui::Button(keyName(v), ImVec2(-FLT_MIN, 0)))
                    g_capturing = s.key;
                break;
            }
            const bool selected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImGui::EndDisabled();
            ImGui::PopID();
            if (selected || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                help = s.help;
                if (locked)
                    help += " (set by " + s.env + " for this run)";
            }
            if (s.apply == settings::Apply::Restart && settings::changedSinceStart(s.key))
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("applies after a restart");
            }
        }
    }

    bool settingsMenuOpen() { return g_open; }

    void drawSettingsMenu()
    {
        if (ps2control::hiddenMode())
            return;

        const bool chord = gamepadDown(platform::GAMEPAD_BACK) && gamepadDown(platform::GAMEPAD_START);
        const bool chordPressed = chord && !g_chordWasDown;
        g_chordWasDown = chord;
        // the physical key, or one injected over the control channel (scripted checks; physical input to the game is
        // blocked while the menu is open, so ps2control::keyPressed alone would not close it)
        const int key = settings::value("controls.menu_key");
        const bool menuKey = g_capturing.empty() && (platform::keyPressed(key) || ps2control::keyPressed(key));
        if (menuKey || chordPressed)
            g_open ? close() : open();
        else if (g_open && g_capturing.empty() && !g_popupWasOpen &&
                 (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            close();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;
        ps2control::setGameInputBlocked(g_open || g_releaseWait);
        if (!g_open)
            return;

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float scale = std::max(1.0f, vp->Size.y / 720.0f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 1.25f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.9f, 640.0f * scale), 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        if (g_focusWindow)
        {
            ImGui::SetNextWindowFocus();
            g_focusWindow = false;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("Settings", nullptr, flags))
        {
            const std::vector<std::string> pages = settings::pages();
            // LB / RB (or Q / E with the keyboard) switch pages
            const int n = static_cast<int>(pages.size());
            if (n > 1 && g_capturing.empty())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    g_selectPage = (g_page + n - 1) % n;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                    g_selectPage = (g_page + 1) % n;
            }
            std::string help;
            if (ImGui::BeginTabBar("pages"))
            {
                for (int i = 0; i < n; ++i)
                {
                    const ImGuiTabItemFlags tf = g_selectPage == i ? ImGuiTabItemFlags_SetSelected : 0;
                    if (!ImGui::BeginTabItem(pages[i].c_str(), nullptr, tf))
                        continue;
                    g_page = i;
                    if (ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                        for (const settings::Spec &s : settings::all())
                            if (s.page == pages[i])
                                row(s, help);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Reset page to defaults"))
                        for (const settings::Spec &s : settings::all())
                            if (s.page == pages[i])
                                settings::set(s.key, s.defaultValue);
                    ImGui::EndTabItem();
                }
                g_selectPage = -1;
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(help.empty() ? " " : help.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s or Back+Start: close    Q / E, LB / RB: pages", keyName(settings::value("controls.menu_key")));
        }
        ImGui::End();
        ImGui::PopFont();
        g_popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
}
