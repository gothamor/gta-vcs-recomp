// See settings_menu.h.
//
// Layout: a fixed-size window (from the screen alone), the page tabs, the page's sections in a scrolling child, and a
// footer of fixed height (help text sized for the longest help of any setting, the restart note, the buttons). Nothing
// in the window changes size with the selected row, so the rows never move under the selection (2026-10-24).
// Navigation is Dear ImGui's gamepad navigation, fed from the DualSense (platform_ps5.cpp).
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
        bool g_releaseWait = false;  // closed, but the buttons that closed it are still down
        bool g_chordWasDown = false; // touchpad + Options
        bool g_popupWasOpen = false; // a combo's list: Circle closes it, not the menu
        bool g_wasFocused = false;   // the menu window had focus last frame
        bool g_focusWindow = false;
        int g_page = 0, g_selectPage = -1;
        std::string g_confirming;    // the Action setting waiting for its second press
        double g_confirmUntil = 0.0;
        float g_scrollTo = -1.0f;    // scrollSettingsMenu, at the next frame
        std::string g_helpKey;    // the row whose help shows: the last one navigated to (kept in between)
        std::string g_navFocused; // the row whose control had the navigation focus last frame

        bool gamepadDown(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        bool anyInputHeld()
        {
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (gamepadDown(static_cast<platform::GamepadButton>(b)))
                    return true;
            return false;
        }

        void open()
        {
            settings::refresh();
            g_open = true;
            g_focusWindow = true;
            g_confirming.clear();
            g_helpKey.clear();
            g_navFocused.clear();
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        }

        void close()
        {
            g_open = false;
            g_releaseWait = true; // the game would otherwise see Circle / Options held
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        }

        bool restartPending(const settings::Spec &s) { return s.apply == settings::Apply::Restart && settings::changedSinceStart(s.key); }

        // What the help area shows for a setting.
        std::string helpText(const settings::Spec &s)
        {
            std::string text = s.help;
            if (s.apply == settings::Apply::Restart)
                text += " Applies the next time the game starts.";
            if (settings::overridden(s.key))
                text += " Locked: the environment variable " + s.env + " sets it for this run; start the game without it "
                        "to change it here.";
            return text;
        }

        // A page's sections in the order their first setting was registered.
        std::vector<std::string> sections(const std::string &page)
        {
            std::vector<std::string> out;
            for (const settings::Spec &s : settings::all())
                if (s.page == page && !s.hidden && std::find(out.begin(), out.end(), s.section) == out.end())
                    out.push_back(s.section);
            return out;
        }

        // The label, then the setting's control.
        void row(const settings::Spec &s, std::string &focusedNow)
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            const bool locked = settings::overridden(s.key);
            int v = settings::value(s.key);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (locked) // greyed out with the reason beside it, so a press that does nothing doesn't look like a bug
                ImGui::TextDisabled("%s (%s)", s.label.c_str(), s.env.c_str());
            else
                ImGui::TextUnformatted(s.label.c_str());
            if (restartPending(s))
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "*");
            }
            ImGui::TableNextColumn();
            ImGui::PushID(s.key.c_str());
            ImGui::BeginDisabled(locked);
            ImGui::SetNextItemWidth(-FLT_MIN);
            auto noteItem = [&] { // right after the control itself (a combo's list is another window)
                if (ImGui::IsItemFocused())
                    focusedNow = s.key;
            };
            switch (s.type)
            {
            case settings::Type::Toggle:
            {
                bool on = v != 0;
                const bool changed = ImGui::Checkbox(on ? "On###v" : "Off###v", &on);
                noteItem();
                if (changed)
                    settings::set(s.key, on ? 1 : 0);
                break;
            }
            case settings::Type::Choice:
            {
                auto current = std::find_if(s.choices.begin(), s.choices.end(), [&](const settings::Choice &c) { return c.value == v; });
                const bool listOpen = ImGui::BeginCombo("##v", current != s.choices.end() ? current->label.c_str() : "?");
                noteItem();
                if (listOpen)
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
            {
                int edited = v;
                const bool changed = ImGui::SliderInt("##v", &edited, s.min, s.max, s.format.c_str(), ImGuiSliderFlags_AlwaysClamp);
                noteItem();
                if (changed)
                    settings::set(s.key, edited);
                break;
            }
            case settings::Type::Action:
            {
                const bool confirming = g_confirming == s.key && ImGui::GetTime() < g_confirmUntil;
                const std::string text = confirming ? "Press Cross again to confirm" : s.button.empty() ? s.label : s.button;
                const bool pressed = ImGui::Button((text + "###v").c_str(), ImVec2(-FLT_MIN, 0));
                noteItem();
                if (pressed)
                {
                    if (s.confirm && !confirming)
                    {
                        g_confirming = s.key;
                        g_confirmUntil = ImGui::GetTime() + 3.0;
                    }
                    else
                    {
                        g_confirming.clear();
                        if (s.onChange)
                            s.onChange(0);
                    }
                }
                break;
            }
            }
            ImGui::EndDisabled();
            ImGui::PopID();

            if (g_helpKey == s.key)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, 0.35f));
        }
    }

    bool settingsMenuOpen() { return g_open; }

    void openSettingsMenu(const std::string &page)
    {
        const std::vector<std::string> pages = settings::pages();
        for (size_t i = 0; i < pages.size(); ++i)
            if (pages[i] == page)
                g_selectPage = static_cast<int>(i);
        if (!g_open)
            open();
    }

    void scrollSettingsMenu(float fraction) { g_scrollTo = std::clamp(fraction, 0.0f, 1.0f); }

    void closeSettingsMenu()
    {
        if (g_open)
            close();
    }
    bool settingsMenuBlocksInput() { return g_open || g_releaseWait; }

    void drawSettingsMenu()
    {
        if (ps2control::hiddenMode() && !g_open) // a test run opens it only through openSettingsMenu
        {
            g_releaseWait = false; // no physical input to wait for
            return;
        }

        // touchpad + Options (the PS2's Select + Start, kept from the game: game/vcs_input.cpp)
        const bool chord = gamepadDown(platform::GAMEPAD_BACK) && gamepadDown(platform::GAMEPAD_START);
        const bool chordPressed = chord && !g_chordWasDown;
        g_chordWasDown = chord;
        if (chordPressed)
            g_open ? close() : open();
        else if (g_open && !g_popupWasOpen && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;
        if (!g_open)
            return;

        const std::vector<std::string> pages = settings::pages();
        const int n = static_cast<int>(pages.size());
        if (n == 0)
            return;
        g_page = std::clamp(g_page, 0, n - 1);

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float scale = std::max(1.0f, vp->Size.y / 720.0f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 1.25f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.92f, 720.0f * scale), std::min(vp->Size.y * 0.88f, 540.0f * scale)),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        // keeps the navigation focus (not while a combo list is open: focusing the window would close it)
        if (g_focusWindow || (!g_wasFocused && !g_popupWasOpen))
        {
            ImGui::SetNextWindowFocus();
            g_focusWindow = false;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::Begin("Settings", nullptr, flags))
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            // L1 / R1 switch pages
            if (n > 1 && !g_popupWasOpen)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                    g_selectPage = (g_page + n - 1) % n;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                    g_selectPage = (g_page + 1) % n;
            }

            // the help area fits the longest help of any page, so it is the same height whatever is selected - but at
            // most four lines (a quarter of the window or so), so the rows keep the room: a longer help is drawn smaller
            const float wrap = ImGui::GetContentRegionAvail().x;
            const float line = ImGui::GetTextLineHeightWithSpacing();
            float helpHeight = line * 2.0f;
            for (const settings::Spec &s : settings::all())
                helpHeight = std::max(helpHeight, ImGui::CalcTextSize(helpText(s).c_str(), nullptr, false, wrap).y);
            helpHeight = std::min(helpHeight, line * 4.0f);
            const float footer = (1.0f + style.ItemSpacing.y) + (helpHeight + style.ItemSpacing.y) +
                                 (ImGui::GetTextLineHeight() + style.ItemSpacing.y) + ImGui::GetFrameHeight() + style.ItemSpacing.y +
                                 2.0f;

            const int shownPage = g_page;
            if (ImGui::BeginTabBar("pages"))
            {
                for (int i = 0; i < n; ++i)
                {
                    const ImGuiTabItemFlags tf = g_selectPage == i ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(pages[i].c_str(), nullptr, tf))
                    {
                        g_page = i;
                        ImGui::EndTabItem();
                    }
                }
                g_selectPage = -1;
                ImGui::EndTabBar();
            }
            if (g_page != shownPage)
                g_helpKey.clear();
            const std::string &page = pages[g_page];

            std::string focusedNow;
            ImGui::PushID(page.c_str());
            if (ImGui::BeginChild("rows", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened))
            {
                if (g_scrollTo >= 0.0f && ImGui::GetScrollMaxY() > 0.0f)
                {
                    ImGui::SetScrollY(g_scrollTo * ImGui::GetScrollMaxY());
                    g_scrollTo = -1.0f;
                }
                for (const std::string &section : sections(page))
                {
                    if (!section.empty())
                        ImGui::SeparatorText(section.c_str());
                    if (ImGui::BeginTable(section.c_str(), 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                        for (const settings::Spec &s : settings::all())
                            if (s.page == page && s.section == section && !s.hidden)
                                row(s, focusedNow);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
            // the navigation moved to another row: its help shows
            if (!focusedNow.empty() && focusedNow != g_navFocused)
                g_helpKey = focusedNow;
            g_navFocused = focusedNow;

            ImGui::Separator();
            const settings::Spec *helpSpec = nullptr;
            bool anyRestart = false;
            for (const settings::Spec &s : settings::all())
            {
                if (s.page == page && s.key == g_helpKey)
                    helpSpec = &s;
                anyRestart = anyRestart || restartPending(s);
            }
            if (ImGui::BeginChild("help", ImVec2(0.0f, helpHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (helpSpec)
                {
                    const std::string text = helpText(*helpSpec);
                    const float base = ImGui::GetFontSize();
                    float size = base; // the largest size, down to 60%, at which the help fits
                    while (size > base * 0.6f &&
                           ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, ImGui::GetContentRegionAvail().x, text.c_str()).y > helpHeight)
                        size -= base * 0.05f;
                    ImGui::PushFont(nullptr, size);
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopFont();
                }
                else
                    ImGui::TextDisabled("Select a setting (d-pad) to see what it does.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            if (anyRestart)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "* changed: applies the next time the game starts");
            else
                ImGui::TextUnformatted(" ");

            if (ImGui::Button("Reset page to defaults"))
                for (const settings::Spec &s : settings::all())
                    if (s.page == page && s.type != settings::Type::Action)
                        settings::set(s.key, s.defaultValue);
            const char *hint = "Cross: change  Circle or touchpad+Options: close  L1/R1: page";
            ImGui::SameLine();
            if (const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x; room > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room); // right-aligned
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", hint);
        }
        g_wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
        ImGui::PopFont();
        g_popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
}
