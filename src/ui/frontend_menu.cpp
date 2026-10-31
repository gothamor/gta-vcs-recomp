#include "ui/frontend_menu.h"

#include "game/vcs_input.h"
#include "ps2_runtime.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"

#include "imgui.h"
#include "imgui_internal.h"

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
#include <cctype>
#include <cmath>
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
        bool g_optionsOpen = false;
        int g_optionsPage = -1;
        bool g_quitConfirm = false;
        std::atomic<bool> g_pauseDismissed{false};
        ImTextureData *g_background = nullptr, *g_font = nullptr, *g_scriptFont = nullptr;
        ImTextureData *g_pricedown = nullptr, *g_backIcon = nullptr, *g_selectIcon = nullptr;
        ImTextureData *g_moveIcons[4] = {};
        ImFont *g_optionsFont = nullptr;
        bool g_backgroundTried = false;
        bool g_sawBootLoading = false;

        constexpr const char *kPalmBackground = "db7fe6220bfa99e-400b40212c17643-00006213.png";
        constexpr const char *kMenuFont = "3ece7b57667e958b-f663bcea3bca5023-00006213.png";
        constexpr const char *kScriptFont = "87ad416320b92240-59ab262d214c676c-00006214.png";
        constexpr ImU32 kCyan = IM_COL32(18, 211, 215, 255);
        constexpr ImU32 kPink = IM_COL32(194, 91, 142, 255);
        constexpr ImU32 kYellow = IM_COL32(190, 181, 108, 255);
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
            g_scriptFont = loadTexture(replacements / kScriptFont, false);
            g_pricedown = loadTexture(replacements / "64e22c1534c81341-d49e156c51372d1c-00006214.png", false);
            g_backIcon = loadTexture(replacements / "d0d9f396c36b7411-bd20758f94447ef8-00005113.png", false);
            g_selectIcon = loadTexture(replacements / "60a3a644467171e5-acbe9ae672fed823-00005113.png", false);
            static constexpr const char *arrows[] = {
                "784c33b4a3241b19-b97a81b80c22e4c8-00005113.png",
                "a1b7cc92002291c5-b97a81b80c22e4c8-00005113.png",
                "3c60f292c5015937-b97a81b80c22e4c8-00005113.png",
                "47e8e408d74357d8-b97a81b80c22e4c8-00005113.png"};
            for (unsigned i = 0; i < 4; ++i)
                g_moveIcons[i] = loadTexture(replacements / arrows[i], false);
            // These PS2 font masks store full opacity as 0x80, rather than PNG's 0xff.
            for (ImTextureData *font : {g_font, g_scriptFont, g_pricedown, g_backIcon, g_selectIcon,
                                       g_moveIcons[0], g_moveIcons[1], g_moveIcons[2], g_moveIcons[3]})
                if (font)
                    for (size_t i = 3, n = static_cast<size_t>(font->Width) * font->Height * 4; i < n; i += 4)
                        font->Pixels[i] = static_cast<unsigned char>(std::min(255, font->Pixels[i] * 2));
        }

        struct Glyph
        {
            float x0, y0, x1, y1;
        };

        bool glyph(unsigned char c, Glyph &out)
        {
            static constexpr float an[] = {100, 112, 124, 136, 148, 160, 170, 182, 195, 200, 207, 219, 224, 240, 253};
            static constexpr float oz[] = {0, 12, 24, 36, 46, 58, 70, 83, 95, 112, 124, 136, 147};
            if (c >= 'a' && c <= 'n')
            {
                const unsigned i = c - 'a';
                out = {an[i], 70, an[i + 1], 93};
                return true;
            }
            if (c >= 'o' && c <= 'z')
            {
                const unsigned i = c - 'o';
                out = {oz[i], 94, oz[i + 1], 117};
                return true;
            }
            static constexpr float digits[] = {164, 178, 190, 204, 218, 232, 246};
            if (c >= '0' && c <= '5')
            {
                const unsigned i = c - '0';
                out = {digits[i], 0, digits[i + 1], 23};
                return true;
            }
            if (c >= '6' && c <= '9')
            {
                const unsigned i = c - '6';
                out = {i * 14.0f, 24, (i + 1) * 14.0f, 46};
                return true;
            }
            switch (c)
            {
            case '!': out = {7, 0, 12, 23}; return true;
            case '"': out = {13, 0, 21, 23}; return true;
            case '#': out = {22, 0, 35, 23}; return true;
            case '$': out = {35, 0, 49, 23}; return true;
            case '%': out = {51, 0, 67, 23}; return true;
            case '&': out = {70, 0, 86, 23}; return true;
            case '\'': out = {87, 0, 91, 23}; return true;
            case '(': out = {92, 0, 99, 23}; return true;
            case ')': out = {99, 0, 107, 23}; return true;
            case '*': out = {108, 0, 118, 23}; return true;
            case '+': out = {120, 0, 131, 23}; return true;
            case ',': out = {133, 0, 139, 23}; return true;
            case '-': case '_': out = {141, 0, 148, 23}; return true;
            case '.': out = {148, 0, 154, 23}; return true;
            case '/': case '\\': out = {155, 0, 164, 23}; return true;
            case ':': out = {56, 24, 62, 46}; return true;
            case ';': out = {62, 24, 69, 46}; return true;
            case '=': out = {84, 24, 97, 46}; return true;
            case '>': out = {99, 24, 112, 46}; return true;
            case '?': out = {112, 24, 125, 46}; return true;
            case '@': out = {124, 24, 143, 46}; return true;
            case '[': out = {92, 0, 99, 23}; return true;
            case ']': out = {99, 0, 107, 23}; return true;
            }
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

        ImFont *optionsFont()
        {
            if (g_optionsFont || !g_font)
                return g_optionsFont;
            // Supply bitmap glyphs to ImGui itself so widgets, popups, wrapped help and numeric editing all use
            // the game's texture. F2 continues to use ImGui's default font.
            static ImFontLoader loader;
            loader.Name = "VCS texture atlas";
            loader.FontSrcInit = [](ImFontAtlas *, ImFontConfig *) { return true; };
            loader.FontSrcContainsGlyph = [](ImFontAtlas *, ImFontConfig *, ImWchar c) {
                Glyph g;
                return c == ' ' || (c < 128 && glyph(static_cast<unsigned char>(c), g));
            };
            loader.FontBakedInit = [](ImFontAtlas *, ImFontConfig *, ImFontBaked *b, void *) {
                b->Ascent = b->Size;
                b->Descent = 0;
                return true;
            };
            loader.FontBakedLoadGlyph = [](ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *b, void *,
                                           ImWchar c, ImFontGlyph *out, float *advance) {
                Glyph g{};
                if (c != ' ' && (c >= 128 || !glyph(static_cast<unsigned char>(c), g))) return false;
                const float scale = b->Size / 23.0f;
                const float width = c == ' ' ? 7.0f : g.x1 - g.x0 + 1.0f;
                if (advance) { *advance = width * scale; return true; }
                out->Codepoint = c;
                out->AdvanceX = width * scale;
                out->Visible = c != ' ';
                if (c == ' ') return true;
                const float density = b->RasterizerDensity * src->RasterizerDensity;
                const int w = std::max(1, static_cast<int>(std::ceil((g.x1 - g.x0) * scale * density)));
                const int h = std::max(1, static_cast<int>(std::ceil((g.y1 - g.y0) * scale * density)));
                const ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
                if (id == ImFontAtlasRectId_Invalid) return false;
                ImTextureRect *rect = ImFontAtlasPackGetRect(atlas, id);
                std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const int sx = std::clamp(static_cast<int>((g.x0 + (x + 0.5f) / w * (g.x1 - g.x0)) * g_font->Width / 256), 0, g_font->Width - 1);
                        const int sy = std::clamp(static_cast<int>((g.y0 + (y + 0.5f) / h * (g.y1 - g.y0)) * g_font->Height / 256), 0, g_font->Height - 1);
                        std::memcpy(pixels.data() + (static_cast<size_t>(y) * w + x) * 4,
                                    g_font->Pixels + (static_cast<size_t>(sy) * g_font->Width + sx) * 4, 4);
                    }
                out->X0 = out->Y0 = 0;
                out->X1 = w / density;
                out->Y1 = h / density;
                out->Colored = false;
                out->PackId = id;
                ImFontAtlasBakedSetFontGlyphBitmap(atlas, b, src, out, rect, pixels.data(), ImTextureFormat_RGBA32, w * 4);
                return true;
            };
            ImFontConfig config;
            config.FontLoader = &loader;
            config.SizePixels = 20;
            ImStrncpy(config.Name, "VCS menu texture", IM_ARRAYSIZE(config.Name));
            g_optionsFont = ImGui::GetIO().Fonts->AddFont(&config);
            return g_optionsFont;
        }

        float glyphAdvance(unsigned char c, float scale)
        {
            if (c == ' ')
                return 7.0f * scale;
            Glyph g;
            return glyph(c, g) ? ((g.x1 - g.x0) + 1.0f) * scale : 8.0f * scale;
        }

        ImVec2 textSize(const char *text, float scale, ImTextureData *font = nullptr, float xScale = 1.0f)
        {
            font = font ? font : g_font;
            if (!font)
                return ImGui::CalcTextSize(text);
            float width = 0.0f;
            for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
                width += glyphAdvance(*p, scale) * xScale;
            return ImVec2(width, 22.0f * scale);
        }

        void drawText(ImDrawList *draw, ImVec2 pos, const char *text, ImU32 color, float scale,
                      ImTextureData *font = nullptr, float xScale = 1.0f)
        {
            font = font ? font : g_font;
            if (!font)
            {
                draw->AddText(pos, color, text);
                return;
            }
            for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
            {
                if (*p == ' ')
                {
                    pos.x += glyphAdvance(*p, scale) * xScale;
                    continue;
                }
                Glyph g;
                if (!glyph(*p, g))
                {
                    pos.x += glyphAdvance(*p, scale) * xScale;
                    continue;
                }
                const float width = (g.x1 - g.x0) * scale * xScale, height = (g.y1 - g.y0) * scale;
                draw->AddImage(font->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + height),
                               ImVec2(g.x0 / 256.0f, g.y0 / 256.0f), ImVec2(g.x1 / 256.0f, g.y1 / 256.0f), color);
                pos.x += glyphAdvance(*p, scale) * xScale;
            }
        }

        bool menuItem(const char *label, float width, float textScale, float xScale)
        {
            const float height = 48.0f * textScale;
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - width) * 0.5f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(label, ImVec2(width, height));
            const bool selected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImVec2 text = textSize(label, textScale, nullptr, xScale);
            drawText(ImGui::GetWindowDrawList(), ImVec2(p.x + (width - text.x) * 0.5f, p.y + (height - text.y) * 0.5f),
                     label, selected ? kYellow : kCyan, textScale, nullptr, xScale);
            return ImGui::IsItemActivated();
        }

        struct MenuCanvas
        {
            ImVec2 pos, size;
            float scale, xScale;
        };

        MenuCanvas menuCanvas(const ImGuiViewport &vp)
        {
            const int hud = settings::value("display.hud");
            if (hud != 0)
                return {vp.WorkPos, vp.WorkSize, vp.WorkSize.y / 480.0f,
                        hud == 1 ? (vp.WorkSize.x / vp.WorkSize.y) / (4.0f / 3.0f) : 1.0f};
            const float width = std::min(vp.WorkSize.x, vp.WorkSize.y * (4.0f / 3.0f));
            const float height = width * (3.0f / 4.0f);
            return {ImVec2(vp.WorkPos.x + (vp.WorkSize.x - width) * 0.5f,
                           vp.WorkPos.y + (vp.WorkSize.y - height) * 0.5f),
                    ImVec2(width, height), height / 480.0f, 1.0f};
        }

        bool pricedownGlyph(char c, Glyph &g)
        {
            static constexpr float ag[] = {246, 284, 322, 360, 398, 436, 474, 511};
            static constexpr float ht[] = {0, 38, 56, 94, 132, 162, 220, 258, 296, 334, 372, 410, 448, 488};
            static constexpr float uz[] = {0, 39, 82, 148, 186, 224, 261};
            if (c >= 'a' && c <= 'g') { unsigned i = c - 'a'; g = {ag[i], 94, ag[i + 1] - 1, 142}; return true; }
            if (c >= 'h' && c <= 't') { unsigned i = c - 'h'; g = {ht[i], 174, ht[i + 1] - 1, 222}; return true; }
            if (c >= 'u' && c <= 'z') { unsigned i = c - 'u'; g = {uz[i], 254, uz[i + 1] - 1, 302}; return true; }
            return false;
        }

        void footerText(ImDrawList *draw, ImVec2 pos, const char *text, float scale, float xScale)
        {
            if (!g_pricedown)
                return drawText(draw, pos, text, kWhite, scale, nullptr, xScale);
            for (const char *p = text; *p; ++p)
            {
                Glyph g;
                if (!pricedownGlyph(*p, g)) { pos.x += 7 * scale * xScale; continue; }
                const float width = (g.x1 - g.x0) * 0.32f * scale * xScale;
                draw->AddImage(g_pricedown->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + 15.36f * scale),
                               ImVec2(g.x0 / 512, g.y0 / 512), ImVec2(g.x1 / 512, g.y1 / 512), kWhite);
                pos.x += width + scale * xScale;
            }
        }

        void footerIcon(ImDrawList *draw, ImTextureData *icon, ImVec2 pos, float size, float xScale, const char *fallback)
        {
            if (icon)
                draw->AddImage(icon->GetTexRef(), pos, ImVec2(pos.x + size * xScale, pos.y + size));
            else
                drawText(draw, pos, fallback, kWhite, size / 23, nullptr, xScale);
        }

        bool footer(const MenuCanvas &canvas, bool showBack)
        {
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float scale = canvas.scale;
            const float y = canvas.pos.y + canvas.size.y - 53.0f * canvas.scale;
            const float left = canvas.pos.x + 30.0f * canvas.scale;
            if (showBack)
            {
                footerIcon(draw, g_backIcon, ImVec2(left, y - 2 * scale), 20 * scale, canvas.xScale, "<");
                footerText(draw, ImVec2(left + 26 * scale * canvas.xScale, y), "back", scale, canvas.xScale);
            }
            footerIcon(draw, g_selectIcon, ImVec2(left, y + 23 * scale), 20 * scale, canvas.xScale, "X");
            footerText(draw, ImVec2(left + 26 * scale * canvas.xScale, y + 25 * scale), "select", scale, canvas.xScale);
            const float right = canvas.pos.x + canvas.size.x - 26 * scale;
            const float arrowSize = 16 * scale;
            for (unsigned i = 0; i < 4; ++i)
                footerIcon(draw, g_moveIcons[i], ImVec2(right - (4 - i) * arrowSize * canvas.xScale, y),
                           arrowSize, canvas.xScale, i == 0 ? "<" : i == 2 ? ">" : " ");
            footerText(draw, ImVec2(right - 64 * scale * canvas.xScale, y + 25 * scale), "move", scale, canvas.xScale);
            if (showBack)
            {
                ImGui::SetCursorScreenPos(ImVec2(left, y));
                return ImGui::InvisibleButton("footer back", ImVec2(100 * canvas.scale, 23 * canvas.scale));
            }
            return false;
        }

        void drawBackground(ImDrawList *draw, const ImGuiViewport &vp, const MenuCanvas &canvas)
        {
            const ImVec2 viewportEnd(vp.WorkPos.x + vp.WorkSize.x, vp.WorkPos.y + vp.WorkSize.y);
            draw->AddRectFilled(vp.WorkPos, viewportEnd, IM_COL32(25, 0, 35, 255));
            if (!g_background || g_background->Width <= 0 || g_background->Height <= 0)
                return;
            draw->AddImage(g_background->GetTexRef(), canvas.pos,
                           ImVec2(canvas.pos.x + canvas.size.x, canvas.pos.y + canvas.size.y));
        }

        void drawHeading(ImDrawList *draw, const MenuCanvas &canvas, bool options)
        {
            if (!g_scriptFont)
            {
                drawText(draw, ImVec2(canvas.pos.x + 28.0f * canvas.scale, canvas.pos.y + 24.0f * canvas.scale),
                         options ? "Options" : "Game", kPink, canvas.scale * 1.7f, nullptr, canvas.xScale);
                return;
            }
            // The common baseline is y=36 in 512-atlas units. G has a descender, so its bottom must not be
            // mistaken for its baseline. Render both titles 70% larger than the earlier half-scale crops.
            struct Letter { Glyph uv; float top, advance; };
            // Advance widths account for the script's slanted strokes; visible bounds must overlap slightly.
            static constexpr Letter game[] = {{{232, 69, 259, 117}, 6, 22}, {{1, 205, 23, 220}, 22, 19},
                                               {{274, 205, 305, 220}, 22, 27}, {{93, 205, 111, 220}, 22, 18}};
            static constexpr Letter opts[] = {{{40, 128, 69, 163}, 2, 22}, {{352, 204, 384, 238}, 21, 22},
                                               {{452, 190, 481, 221}, 7, 19}, {{187, 195, 203, 220}, 12, 12},
                                               {{332, 205, 348, 220}, 22, 14}, {{306, 205, 331, 220}, 22, 22},
                                               {{432, 205, 451, 220}, 22, 19}};
            const Letter *letters = options ? opts : game;
            const unsigned count = options ? 7 : 4;
            ImVec2 pos(canvas.pos.x + 28.0f * canvas.scale, canvas.pos.y + 24.0f * canvas.scale);
            for (unsigned i = 0; i < count; ++i)
            {
                const Glyph &g = letters[i].uv;
                const float width = (g.x1 - g.x0) * 0.85f * canvas.scale * canvas.xScale;
                const float height = (g.y1 - g.y0) * 0.85f * canvas.scale;
                const float y = pos.y + letters[i].top * 0.85f * canvas.scale;
                draw->AddImage(g_scriptFont->GetTexRef(), ImVec2(pos.x, y), ImVec2(pos.x + width, y + height),
                               ImVec2(g.x0 / 512.0f, g.y0 / 512.0f), ImVec2(g.x1 / 512.0f, g.y1 / 512.0f), kPink);
                pos.x += letters[i].advance * 0.85f * canvas.scale * canvas.xScale;
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

        void openOptions()
        {
            resetEmbeddedSettings();
            g_optionsOpen = true;
            g_optionsPage = -1;
            g_quitConfirm = false;
        }

        void drawOptions(const MenuCanvas &canvas, const ImGuiViewport &vp)
        {
            const float scale = canvas.scale;
            drawHeading(ImGui::GetWindowDrawList(), canvas, true);
            const auto pages = settings::pages();
            ImFont *font = optionsFont();
            if (font)
                ImGui::PushFont(font, std::max(12.0f, 15.0f * scale));
            ImGui::SetCursorPos(ImVec2(canvas.pos.x - vp.WorkPos.x + 28 * scale,
                                       canvas.pos.y - vp.WorkPos.y + 75 * scale));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.07f, 0.83f, 0.84f, 1));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0, 0.07f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.02f, 0.16f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.28f, 0.12f, 1));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.28f, 0.12f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.40f, 0.38f, 0.18f, 1));
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(kYellow));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(kYellow));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(kYellow));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.38f, 0.18f, 1));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.40f, 0.38f, 0.18f, 1));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.30f, 0.28f, 0.12f, 1));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.40f, 0.38f, 0.18f, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4 * scale, 7 * scale));
            const ImVec2 area(canvas.size.x - 56 * scale, canvas.size.y - 155 * scale);
            if (ImGui::BeginChild("native options", area, ImGuiChildFlags_NavFlattened))
            {
                auto label = [&](const std::string &text) {
                    // Preserve numbers and punctuation in registry labels whose glyphs are not mapped in this face.
                    if (std::any_of(text.begin(), text.end(), [](unsigned char c) {
                            return !std::isalpha(c) && c != ' ';
                        }))
                    {
                        ImGui::TextUnformatted(text.c_str());
                        return;
                    }
                    std::string upper = text;
                    for (char &c : upper)
                    {
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (c != ' ' && (c < 'A' || c > 'Z')) c = ' ';
                    }
                    float size = scale * 0.65f;
                    const float width = textSize(upper.c_str(), size, nullptr, canvas.xScale).x;
                    if (width > ImGui::GetContentRegionAvail().x && width > 0)
                        size *= ImGui::GetContentRegionAvail().x / width;
                    drawText(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), upper.c_str(), ImGui::GetColorU32(ImGuiCol_Text),
                             size, nullptr, canvas.xScale);
                    ImGui::Dummy(ImVec2(1, std::max(ImGui::GetTextLineHeight(), 22 * size)));
                };
                if (g_optionsPage < 0)
                {
                    for (size_t i = 0; i < pages.size(); ++i)
                    {
                        std::string upper = pages[i];
                        for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (menuItem(upper.c_str(), area.x * 0.8f, scale * 0.85f, canvas.xScale))
                            g_optionsPage = static_cast<int>(i);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
                        g_optionsOpen = false;
                }
                else if (g_optionsPage < static_cast<int>(pages.size()))
                {
                    label(pages[g_optionsPage]);
                    if (drawEmbeddedSettings(pages[g_optionsPage], label))
                    {
                        resetEmbeddedSettings();
                        g_optionsPage = -1;
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(13);
            if (font)
                ImGui::PopFont();
            if (footer(canvas, true))
            {
                resetEmbeddedSettings();
                if (g_optionsPage >= 0) g_optionsPage = -1;
                else g_optionsOpen = false;
            }
        }
    }

    void armStartup() { g_startupArmed.store(true, std::memory_order_release); }

    void openOptionsPage(const std::string &page)
    {
        openOptions();
        const auto pages = settings::pages();
        for (size_t i = 0; i < pages.size(); ++i)
            if (pages[i] == page)
                g_optionsPage = static_cast<int>(i);
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
        loadArtwork();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::SetNextWindowFocus();
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin(startup ? "VCS Front End" : "VCS Pause Actions", nullptr, flags))
        {
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const MenuCanvas canvas = menuCanvas(*vp);
            drawBackground(draw, *vp, canvas);

            if (g_optionsOpen)
            {
                drawOptions(canvas, *vp);
                ImGui::End();
                ImGui::PopStyleVar();
                return;
            }

            const float scale = std::clamp(canvas.scale, 0.70f, 1.8f);
            ImGui::SetWindowFontScale(1.35f * scale);
            const float contentWidth = std::min(420.0f * scale, canvas.size.x * 0.72f);
            ImGui::SetCursorPos(ImVec2(canvas.pos.x - vp->WorkPos.x + (canvas.size.x - contentWidth) * 0.5f,
                                       canvas.pos.y - vp->WorkPos.y + canvas.size.y * 0.25f));
            if (startup)
            {
                if (!g_startSubmenu)
                {
                    if (menuItem("START GAME", contentWidth, scale, canvas.xScale)) { g_startSubmenu = true; g_quitConfirm = false; }
                    if (menuItem("OPTIONS", contentWidth, scale, canvas.xScale)) openOptions();
                    if (menuItem(g_quitConfirm ? "ARE YOU SURE" : "QUIT GAME", contentWidth, scale, canvas.xScale)) requestQuit(runtime);
                }
                else
                {
                    if (menuItem("LOAD GAME", contentWidth, scale, canvas.xScale)) begin(StartupAction::LoadGame);
                    if (menuItem("NEW GAME", contentWidth, scale, canvas.xScale)) begin(StartupAction::NewGame);
                    if (menuItem("DELETE GAME", contentWidth, scale, canvas.xScale)) begin(StartupAction::DeleteGame);
                    if (menuItem("BACK", contentWidth, scale, canvas.xScale)) g_startSubmenu = false;
                }
            }
            else
            {
                if (menuItem("OPTIONS", contentWidth, scale, canvas.xScale)) openOptions();
                if (menuItem(g_quitConfirm ? "ARE YOU SURE" : "QUIT GAME", contentWidth, scale, canvas.xScale)) requestQuit(runtime);
                if (menuItem("BACK TO PAUSE MENU", contentWidth, scale, canvas.xScale)) { g_pauseDismissed = true; g_quitConfirm = false; }
            }
            ImGui::SetWindowFontScale(1.0f);
            drawHeading(draw, canvas, !startup);
            footer(canvas, !startup || g_startSubmenu);
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
