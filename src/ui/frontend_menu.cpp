#include "ui/frontend_menu.h"

#include "game/vcs_input.h"
#include "ps2_runtime.h"
#include "ui/settings_menu.h"

#include "imgui.h"

// ImGui 1.92 implements this user-texture path (used by both the D3D11 and RHI backends) but still leaves its
// experimental declaration out of imgui.h.
namespace ImGui
{
    IMGUI_API void RegisterUserTexture(ImTextureData *texture);
}

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../third_party/stb/stb_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

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
        ImTextureData *g_background = nullptr;
        bool g_backgroundTried = false;

        constexpr const char *kPalmBackground = "db7fe6220bfa99e-400b40212c17643-00006213.png";
        constexpr ImU32 kCyan = IM_COL32(18, 211, 215, 255);
        constexpr ImU32 kPink = IM_COL32(194, 91, 142, 255);
        constexpr ImU32 kWhite = IM_COL32(225, 225, 225, 255);

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

        void loadBackground()
        {
            if (g_backgroundTried)
                return;
            g_backgroundTried = true;
            const char *pack = std::getenv("VCS_TEXTURE_PACK");
            if (!pack || !*pack)
                return;
            const std::filesystem::path path = std::filesystem::u8path(pack) / "replacements" / kPalmBackground;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return;
            std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            int width = 0, height = 0, channels = 0;
            unsigned char *pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
            if (!pixels || width <= 0 || height <= 0)
            {
                std::fprintf(stderr, "[frontend] could not decode %s\n", path.string().c_str());
                stbi_image_free(pixels);
                return;
            }
            g_background = new ImTextureData();
            g_background->Create(ImTextureFormat_RGBA32, width, height);
            std::memcpy(g_background->Pixels, pixels, static_cast<size_t>(width) * height * 4u);
            g_background->UseColors = true;
            stbi_image_free(pixels);
            ImGui::RegisterUserTexture(g_background);
            std::fprintf(stderr, "[frontend] menu artwork %s (%dx%d)\n", path.string().c_str(), width, height);
        }

        bool menuItem(const char *label, float width, float height = 34.0f)
        {
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - width) * 0.5f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(label, ImVec2(width, height));
            const bool selected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImVec2 text = ImGui::CalcTextSize(label);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (width - text.x) * 0.5f, p.y + (height - text.y) * 0.5f),
                                                selected ? kPink : kCyan, label);
            return ImGui::IsItemActivated();
        }

        void footer(const ImGuiViewport &vp)
        {
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float y = vp.WorkPos.y + vp.WorkSize.y - 53.0f;
            draw->AddText(ImVec2(vp.WorkPos.x + 30.0f, y), kCyan, "<");
            draw->AddText(ImVec2(vp.WorkPos.x + 52.0f, y), kWhite, "BACK");
            draw->AddText(ImVec2(vp.WorkPos.x + 30.0f, y + 25.0f), kPink, "X");
            draw->AddText(ImVec2(vp.WorkPos.x + 52.0f, y + 25.0f), kWhite, "SELECT");
            const char *move = "ARROWS / STICK  MOVE";
            const ImVec2 size = ImGui::CalcTextSize(move);
            draw->AddText(ImVec2(vp.WorkPos.x + vp.WorkSize.x - size.x - 26.0f, y + 25.0f), kWhite, move);
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
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        loadBackground();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin(startup ? "VCS Front End" : "VCS Pause Actions", nullptr, flags))
        {
            ImDrawList *draw = ImGui::GetWindowDrawList();
            if (g_background)
                draw->AddImage(g_background->GetTexRef(), vp->WorkPos,
                               ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y));
            else
                draw->AddRectFilled(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
                                    IM_COL32(25, 0, 35, 255));

            const float scale = std::clamp(vp->WorkSize.y / 448.0f, 0.85f, 1.8f);
            ImGui::SetWindowFontScale(1.35f * scale);
            const float contentWidth = std::min(420.0f * scale, vp->WorkSize.x * 0.72f);
            ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - contentWidth) * 0.5f, vp->WorkSize.y * 0.25f));
            if (startup)
            {
                if (!g_startSubmenu)
                {
                    if (menuItem("START GAME", contentWidth)) { g_startSubmenu = true; g_quitConfirm = false; }
                    if (menuItem("OPTIONS", contentWidth)) { openSettingsMenu(""); g_quitConfirm = false; }
                    if (menuItem(g_quitConfirm ? "QUIT GAME - ARE YOU SURE?" : "QUIT GAME", contentWidth)) requestQuit(runtime);
                }
                else
                {
                    if (menuItem("LOAD GAME", contentWidth)) begin(StartupAction::LoadGame);
                    if (menuItem("NEW GAME", contentWidth)) begin(StartupAction::NewGame);
                    if (menuItem("DELETE GAME", contentWidth)) begin(StartupAction::DeleteGame);
                    if (menuItem("BACK", contentWidth)) g_startSubmenu = false;
                }
            }
            else
            {
                if (menuItem("OPTIONS", contentWidth)) { openSettingsMenu(""); g_quitConfirm = false; }
                if (menuItem(g_quitConfirm ? "QUIT - ARE YOU SURE?" : "QUIT GAME", contentWidth)) requestQuit(runtime);
                if (menuItem("BACK TO PAUSE MENU", contentWidth)) { g_pauseDismissed = true; g_quitConfirm = false; }
            }
            ImGui::SetWindowFontScale(1.0f);
            draw->AddText(ImVec2(vp->WorkPos.x + 28.0f, vp->WorkPos.y + 24.0f), kPink,
                          startup ? "Game" : "Options");
            footer(*vp);
        }
        ImGui::End();
        ImGui::PopStyleVar();
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
