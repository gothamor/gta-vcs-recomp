#include "ui/frontend_menu.h"

#include "game/vcs_input.h"
#include "ps2_runtime.h"
#include "ui/settings_menu.h"

#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>
#include <thread>

namespace ui::frontend
{
    namespace
    {
        constexpr uint32_t kPlayerPed = 0x004E4910u;
        constexpr uint32_t kUserPause = 0x004CD2ACu;
        constexpr uint16_t kStart = 0x0008u, kRight = 0x0020u, kDown = 0x0040u, kCross = 0x4000u;

        std::atomic<bool> g_startupOpen{false};
        std::atomic<bool> g_pauseOpen{false};
        std::atomic<int> g_pendingAction{-1};
        bool g_startSubmenu = false;
        bool g_quitConfirm = false;
        std::atomic<bool> g_pauseDismissed{false};

        struct Step
        {
            uint16_t button;
            int waitFrames;
        };
        constexpr Step kLoadSteps[] = {{kStart, 180}, {kRight, 30}, {kRight, 30}, {kCross, 1}};
        constexpr Step kDeleteSteps[] = {{kStart, 180}, {kRight, 30}, {kRight, 30}, {kCross, 30},
                                         {kDown, 30}, {kDown, 30}, {kCross, 1}};
        int g_step = 0, g_wait = 0;

        void begin(StartupAction action)
        {
            g_step = 0;
            g_wait = 0;
            g_pendingAction.store(action == StartupAction::NewGame ? -1 : static_cast<int>(action), std::memory_order_release);
            g_startupOpen.store(false, std::memory_order_release);
            g_startSubmenu = false;
        }

        bool menuButton(const char *label)
        {
            const float width = ImGui::GetContentRegionAvail().x;
            return ImGui::Button(label, ImVec2(width, 42.0f));
        }

        void style(bool push)
        {
            if (push)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.055f, 0.075f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.13f, 0.75f, 0.72f, 0.75f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.28f, 0.32f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.19f, 0.48f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.94f, 0.35f, 0.61f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.98f, 0.96f, 1.0f));
            }
            else
            {
                ImGui::PopStyleColor(6);
                ImGui::PopStyleVar(3);
            }
        }

        void requestQuit(PS2Runtime &runtime)
        {
            if (!g_quitConfirm)
            {
                g_quitConfirm = true;
                return;
            }
            runtime.requestStop();
        }
    }

    void waitAtStartup(PS2Runtime *runtime)
    {
        g_startupOpen.store(true, std::memory_order_release);
        while (g_startupOpen.load(std::memory_order_acquire) && !runtime->isStopRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    bool selectStartup(StartupAction action)
    {
        if (!g_startupOpen.load(std::memory_order_acquire))
            return false;
        begin(action);
        return true;
    }

    bool blocksInput()
    {
        return g_startupOpen.load(std::memory_order_relaxed) ||
               (g_pauseOpen.load(std::memory_order_relaxed) && !g_pauseDismissed && !settingsMenuOpen());
    }

    void draw(PS2Runtime &runtime)
    {
        const bool startup = g_startupOpen.load(std::memory_order_acquire);
        const bool pause = g_pauseOpen.load(std::memory_order_acquire);
        if ((!startup && (!pause || g_pauseDismissed)) || settingsMenuOpen())
            return;

        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        style(true);
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const ImVec2 size(startup ? 430.0f : 245.0f, startup ? 390.0f : 190.0f);
        const ImVec2 pos = startup ? ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
                                            vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f)
                                   : ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 28.0f,
                                            vp->WorkPos.y + vp->WorkSize.y - size.y - 28.0f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
        if (ImGui::Begin(startup ? "VCS Front End" : "VCS Pause Actions", nullptr, flags))
        {
            if (startup)
            {
                ImGui::SetWindowFontScale(1.55f);
                ImGui::TextUnformatted("VICE CITY STORIES");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::TextColored(ImVec4(0.94f, 0.35f, 0.61f, 1.0f), "NATIVE PORT");
                ImGui::Separator();
                ImGui::Spacing();
                if (!g_startSubmenu)
                {
                    if (menuButton("START GAME")) { g_startSubmenu = true; g_quitConfirm = false; }
                    if (menuButton("OPTIONS")) { openSettingsMenu(""); g_quitConfirm = false; }
                    if (menuButton(g_quitConfirm ? "QUIT GAME - ARE YOU SURE?" : "QUIT GAME")) requestQuit(runtime);
                }
                else
                {
                    ImGui::TextUnformatted("START GAME");
                    if (menuButton("NEW GAME")) begin(StartupAction::NewGame);
                    if (menuButton("LOAD GAME")) begin(StartupAction::LoadGame);
                    if (menuButton("DELETE GAME")) begin(StartupAction::DeleteGame);
                    if (menuButton("BACK")) g_startSubmenu = false;
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.94f, 0.35f, 0.61f, 1.0f), "PORT MENU");
                ImGui::Separator();
                if (menuButton("OPTIONS")) { openSettingsMenu(""); g_quitConfirm = false; }
                if (menuButton(g_quitConfirm ? "QUIT - ARE YOU SURE?" : "QUIT GAME")) requestQuit(runtime);
                if (menuButton("BACK TO PAUSE MENU")) { g_pauseDismissed = true; g_quitConfirm = false; }
            }
        }
        ImGui::End();
        style(false);
    }

    void onGameFrame(uint8_t *rdram)
    {
        const bool paused = rdram[kUserPause] != 0; // only the player's pause menu, never a loading/code pause
        g_pauseOpen.store(paused, std::memory_order_release);
        if (!paused)
            g_pauseDismissed = false;
        const int action = g_pendingAction.load(std::memory_order_acquire);
        if (action < 0)
            return;
        uint32_t ped = 0;
        std::memcpy(&ped, rdram + kPlayerPed, sizeof(ped));
        if (!ped)
            return;
        const Step *steps = action == static_cast<int>(StartupAction::LoadGame) ? kLoadSteps : kDeleteSteps;
        const int count = action == static_cast<int>(StartupAction::LoadGame) ? static_cast<int>(std::size(kLoadSteps))
                                                                              : static_cast<int>(std::size(kDeleteSteps));
        if (g_wait > 0)
        {
            --g_wait;
            return;
        }
        if (g_step >= count)
        {
            g_pendingAction.store(-1, std::memory_order_release);
            g_step = 0;
            return;
        }
        vcs::input::injectButton(steps[g_step].button, 4);
        g_wait = steps[g_step].waitFrames;
        ++g_step;
    }
}
