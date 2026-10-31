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
        std::atomic<bool> g_startupArmed{false};
        std::atomic<bool> g_pauseOpen{false};
        std::atomic<int> g_pendingAction{-1};
        bool g_startSubmenu = false;
        bool g_quitConfirm = false;
        std::atomic<bool> g_pauseDismissed{false};
        ImTextureData *g_background = nullptr, *g_font = nullptr;
        bool g_backgroundTried = false;
        bool g_sawBootLoading = false;

        constexpr const char *kPalmBackground = "db7fe6220bfa99e-400b40212c17643-00006213.png";
        constexpr const char *kMenuFont = "3ece7b57667e958b-f663bcea3bca5023-00006213.png";
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

        ImTextureData *loadTexture(const std::filesystem::path &path, bool opaque)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return nullptr;
            std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            int width = 0, height = 0, channels = 0;
            unsigned char *pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
            if (!pixels || width <= 0 || height <= 0)
            {
                std::fprintf(stderr, "[frontend] could not decode %s\n", path.string().c_str());
                stbi_image_free(pixels);
                return nullptr;
            }
            ImTextureData *texture = new ImTextureData();
            texture->Create(ImTextureFormat_RGBA32, width, height);
            std::memcpy(texture->Pixels, pixels, static_cast<size_t>(width) * height * 4u);
            if (opaque)
                for (size_t i = 3, n = static_cast<size_t>(width) * height * 4u; i < n; i += 4)
                    texture->Pixels[i] = 255;
            texture->UseColors = true;
            stbi_image_free(pixels);
            ImGui::RegisterUserTexture(texture);
            std::fprintf(stderr, "[frontend] menu artwork %s (%dx%d)\n", path.string().c_str(), width, height);
            return texture;
        }

        void loadArtwork()
        {
            if (g_backgroundTried)
                return;
            g_backgroundTried = true;
            const char *pack = std::getenv("VCS_TEXTURE_PACK");
            if (!pack || !*pack)
                return;
            const std::filesystem::path replacements = std::filesystem::u8path(pack) / "replacements";
            g_background = loadTexture(replacements / kPalmBackground, true);
            g_font = loadTexture(replacements / kMenuFont, false);
        }

        struct Glyph
        {
            float x0, y0, x1, y1;
        };

        bool glyph(unsigned char c, Glyph &out)
        {
            static constexpr float ag[] = {144, 158, 173, 188, 203, 217, 230, 246};
            static constexpr float hw[] = {0, 15, 22, 34, 49, 61, 82, 98, 114, 128, 144, 159, 173, 188, 204, 220, 242};
            static constexpr float xz[] = {0, 16, 31, 45};
            if (c >= 'A' && c <= 'G')
            {
                const unsigned i = c - 'A';
                out = {ag[i], 24, ag[i + 1], 46};
                return true;
            }
            if (c >= 'H' && c <= 'W')
            {
                const unsigned i = c - 'H';
                out = {hw[i], 47, hw[i + 1], 69};
                return true;
            }
            if (c >= 'X' && c <= 'Z')
            {
                const unsigned i = c - 'X';
                out = {xz[i], 70, xz[i + 1], 93};
                return true;
            }
            if (c == '<')
            {
                out = {69, 24, 82, 46};
                return true;
            }
            return false;
        }

        float glyphAdvance(unsigned char c, float scale)
        {
            if (c == ' ')
                return 7.0f * scale;
            Glyph g;
            return glyph(c, g) ? ((g.x1 - g.x0) + 1.0f) * scale : 8.0f * scale;
        }

        ImVec2 textSize(const char *text, float scale)
        {
            if (!g_font)
                return ImGui::CalcTextSize(text);
            float width = 0.0f;
            for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
                width += glyphAdvance(*p, scale);
            return ImVec2(width, 22.0f * scale);
        }

        void drawText(ImDrawList *draw, ImVec2 pos, const char *text, ImU32 color, float scale)
        {
            if (!g_font)
            {
                draw->AddText(pos, color, text);
                return;
            }
            for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
            {
                if (*p == ' ')
                {
                    pos.x += glyphAdvance(*p, scale);
                    continue;
                }
                Glyph g;
                if (!glyph(*p, g))
                {
                    pos.x += glyphAdvance(*p, scale);
                    continue;
                }
                const float width = (g.x1 - g.x0) * scale, height = (g.y1 - g.y0) * scale;
                draw->AddImage(g_font->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + height),
                               ImVec2(g.x0 / 256.0f, g.y0 / 256.0f), ImVec2(g.x1 / 256.0f, g.y1 / 256.0f), color);
                pos.x += glyphAdvance(*p, scale);
            }
        }

        bool menuItem(const char *label, float width, float textScale, float height = 34.0f)
        {
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - width) * 0.5f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(label, ImVec2(width, height));
            const bool selected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImVec2 text = textSize(label, textScale);
            drawText(ImGui::GetWindowDrawList(), ImVec2(p.x + (width - text.x) * 0.5f, p.y + (height - text.y) * 0.5f),
                     label, selected ? kPink : kCyan, textScale);
            return ImGui::IsItemActivated();
        }

        void footer(const ImGuiViewport &vp, float scale)
        {
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float y = vp.WorkPos.y + vp.WorkSize.y - 53.0f;
            drawText(draw, ImVec2(vp.WorkPos.x + 30.0f, y), "<", kCyan, scale);
            drawText(draw, ImVec2(vp.WorkPos.x + 52.0f, y), "BACK", kWhite, scale);
            drawText(draw, ImVec2(vp.WorkPos.x + 30.0f, y + 25.0f), "X", kPink, scale);
            drawText(draw, ImVec2(vp.WorkPos.x + 52.0f, y + 25.0f), "SELECT", kWhite, scale);
            const char *move = "ARROWS  STICK  MOVE";
            const ImVec2 size = textSize(move, scale);
            drawText(draw, ImVec2(vp.WorkPos.x + vp.WorkSize.x - size.x - 26.0f, y + 25.0f), move, kWhite, scale);
        }

        void drawBackground(ImDrawList *draw, const ImGuiViewport &vp)
        {
            const ImVec2 viewportEnd(vp.WorkPos.x + vp.WorkSize.x, vp.WorkPos.y + vp.WorkSize.y);
            draw->AddRectFilled(vp.WorkPos, viewportEnd, IM_COL32(25, 0, 35, 255));
            if (!g_background || g_background->Width <= 0 || g_background->Height <= 0)
                return;

            // Keep replacement artwork at its authored aspect ratio. Fit the complete image inside the viewport and
            // use the menu purple for any side/top bars instead of deforming it to the current display mode.
            const float fit = std::min(vp.WorkSize.x / static_cast<float>(g_background->Width),
                                       vp.WorkSize.y / static_cast<float>(g_background->Height));
            const ImVec2 size(g_background->Width * fit, g_background->Height * fit);
            const ImVec2 start(vp.WorkPos.x + (vp.WorkSize.x - size.x) * 0.5f,
                               vp.WorkPos.y + (vp.WorkSize.y - size.y) * 0.5f);
            draw->AddImage(g_background->GetTexRef(), start, ImVec2(start.x + size.x, start.y + size.y));
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

    void armStartup() { g_startupArmed.store(true, std::memory_order_release); }

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
        loadArtwork();
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
            drawBackground(draw, *vp);

            const float scale = std::clamp(vp->WorkSize.y / 448.0f, 0.85f, 1.8f);
            ImGui::SetWindowFontScale(1.35f * scale);
            const float contentWidth = std::min(420.0f * scale, vp->WorkSize.x * 0.72f);
            ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - contentWidth) * 0.5f, vp->WorkSize.y * 0.25f));
            if (startup)
            {
                if (!g_startSubmenu)
                {
                    if (menuItem("START GAME", contentWidth, scale)) { g_startSubmenu = true; g_quitConfirm = false; }
                    if (menuItem("OPTIONS", contentWidth, scale)) { openSettingsMenu(""); g_quitConfirm = false; }
                    if (menuItem(g_quitConfirm ? "ARE YOU SURE" : "QUIT GAME", contentWidth, scale)) requestQuit(runtime);
                }
                else
                {
                    if (menuItem("LOAD GAME", contentWidth, scale)) begin(StartupAction::LoadGame);
                    if (menuItem("NEW GAME", contentWidth, scale)) begin(StartupAction::NewGame);
                    if (menuItem("DELETE GAME", contentWidth, scale)) begin(StartupAction::DeleteGame);
                    if (menuItem("BACK", contentWidth, scale)) g_startSubmenu = false;
                }
            }
            else
            {
                if (menuItem("OPTIONS", contentWidth, scale)) { openSettingsMenu(""); g_quitConfirm = false; }
                if (menuItem(g_quitConfirm ? "ARE YOU SURE" : "QUIT GAME", contentWidth, scale)) requestQuit(runtime);
                if (menuItem("BACK TO PAUSE MENU", contentWidth, scale)) { g_pauseDismissed = true; g_quitConfirm = false; }
            }
            ImGui::SetWindowFontScale(1.0f);
            drawText(draw, ImVec2(vp->WorkPos.x + 28.0f, vp->WorkPos.y + 24.0f), startup ? "GAME" : "OPTIONS", kPink, scale);
            footer(*vp, scale * 0.7f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void onGameFrame(uint8_t *rdram, bool loadingScreen, PS2Runtime *runtime)
    {
        if (g_startupArmed.load(std::memory_order_acquire))
        {
            g_sawBootLoading |= loadingScreen;
            if (g_sawBootLoading && !loadingScreen)
            {
                g_startupArmed.store(false, std::memory_order_release);
                g_startupOpen.store(true, std::memory_order_release);
                while (g_startupOpen.load(std::memory_order_acquire) && !runtime->isStopRequested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
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
