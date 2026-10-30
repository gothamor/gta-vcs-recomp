#include "ui.h"

#include "system.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        // ---- the look ------------------------------------------------------------------------------------------
        // Vice City at dusk, quietly: a deep violet base, one pink accent for what to do next, teal for done.
        ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
        const ImU32 kSidebar = rgb(16, 13, 23);
        const ImU32 kBase = rgb(22, 18, 31);
        const ImU32 kCard = rgb(30, 25, 42);
        const ImU32 kCardHover = rgb(38, 32, 53);
        const ImU32 kLine = rgb(46, 39, 63);
        const ImU32 kText = rgb(238, 234, 246);
        const ImU32 kDim = rgb(160, 152, 180);
        const ImU32 kFaint = rgb(108, 100, 128);
        const ImU32 kPink = rgb(240, 80, 143);
        const ImU32 kPinkHover = rgb(255, 106, 163);
        const ImU32 kTeal = rgb(61, 214, 198);
        const ImU32 kAmber = rgb(245, 181, 68);
        const ImU32 kRed = rgb(240, 99, 99);

        ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
        ImU32 alpha(ImU32 c, float a)
        {
            ImVec4 v = v4(c);
            v.w = a;
            return ImGui::ColorConvertFloat4ToU32(v);
        }

        Ui *g_ui = nullptr;
        float S(float v) { return v * g_ui->scale; }
        ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
        // the right edge of the current window's content, in window coordinates (for right-aligned items)
        float contentRight() { return ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x; }

        enum Status
        {
            kUnknown,
            kGood,
            kWarning,
            kMissing
        };

        std::string duration(double seconds)
        {
            const int s = int(seconds);
            char text[32];
            if (s >= 3600)
                std::snprintf(text, sizeof(text), "%dh %02dm", s / 3600, s / 60 % 60);
            else if (s >= 60)
                std::snprintf(text, sizeof(text), "%dm %02ds", s / 60, s % 60);
            else
                std::snprintf(text, sizeof(text), "%ds", s);
            return text;
        }

        // `text` cut to `width` pixels with "..." (a whole UTF-8 character at a time)
        std::string ellipsize(const std::string &text, float width, ImFont *font = nullptr, float size = 0)
        {
            auto measure = [&](const std::string &t) {
                return font ? font->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x : ImGui::CalcTextSize(t.c_str()).x;
            };
            if (measure(text) <= width)
                return text;
            std::string cut = text;
            while (!cut.empty() && measure(cut + "...") > width)
                while (!cut.empty())
                {
                    const unsigned char c = static_cast<unsigned char>(cut.back());
                    cut.pop_back();
                    if ((c & 0xC0) != 0x80) // the lead byte (or ASCII): the character is gone
                        break;
                }
            return cut + "...";
        }

        // ---- text --------------------------------------------------------------------------------------------
        void text(const std::string &t, ImU32 colour = kText, float size = 16, bool bold = false)
        {
            ImGui::PushFont(bold ? g_ui->bold : g_ui->body, size);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        void para(const std::string &t, ImU32 colour = kDim, float size = 15, float maxWidth = 640)
        {
            ImGui::PushFont(g_ui->body, size);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            const float wrap = ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, S(maxWidth));
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        void gap(float h) { ImGui::Dummy(ImVec2(1, S(h))); }

        void header(const char *eyebrow, const std::string &title, const std::string &description)
        {
            if (eyebrow && *eyebrow)
            {
                text(eyebrow, kPink, 12.5f, true);
                gap(2);
            }
            text(title, kText, 30, true);
            if (!description.empty())
            {
                gap(4);
                para(description, kDim, 15.5f);
            }
            gap(22);
        }

        // ---- shapes ------------------------------------------------------------------------------------------
        void checkMark(ImDrawList *dl, ImVec2 c, float r, ImU32 colour, float thick)
        {
            const ImVec2 pts[3] = {c + ImVec2(-r * 0.45f, 0.0f), c + ImVec2(-r * 0.12f, r * 0.33f), c + ImVec2(r * 0.48f, -r * 0.32f)};
            dl->AddPolyline(pts, 3, colour, ImDrawFlags_None, thick);
        }

        void cross(ImDrawList *dl, ImVec2 c, float r, ImU32 colour, float thick)
        {
            dl->AddLine(c + ImVec2(-r * 0.35f, -r * 0.35f), c + ImVec2(r * 0.35f, r * 0.35f), colour, thick);
            dl->AddLine(c + ImVec2(r * 0.35f, -r * 0.35f), c + ImVec2(-r * 0.35f, r * 0.35f), colour, thick);
        }

        void spinner(ImDrawList *dl, ImVec2 c, float r, ImU32 colour, float thick)
        {
            const float t = float(ImGui::GetTime()) * 5.0f;
            dl->PathClear();
            dl->PathArcTo(c, r, t, t + 4.4f, 24);
            dl->PathStroke(colour, ImDrawFlags_None, thick);
        }

        void centredText(ImDrawList *dl, ImVec2 c, const char *t, ImU32 colour, float size, bool bold = true)
        {
            ImFont *font = bold ? g_ui->bold : g_ui->body;
            const float px = S(size);
            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0, t);
            dl->AddText(font, px, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), colour, t);
        }

        // a status badge: done (teal check), attention (amber !), missing (number in a ring), checking (spinner)
        void badge(ImDrawList *dl, ImVec2 c, float r, Status s, int number)
        {
            if (s == kGood)
            {
                dl->AddCircleFilled(c, r, kTeal, 32);
                checkMark(dl, c, r, kSidebar, S(2.2f));
            }
            else if (s == kWarning)
            {
                dl->AddCircleFilled(c, r, alpha(kAmber, 0.18f), 32);
                dl->AddCircle(c, r, kAmber, 32, S(1.6f));
                centredText(dl, c, "!", kAmber, 14);
            }
            else if (s == kMissing)
            {
                dl->AddCircle(c, r, kFaint, 32, S(1.6f));
                const std::string n = std::to_string(number);
                centredText(dl, c, n.c_str(), kDim, 13);
            }
            else
                spinner(dl, c, r * 0.7f, kFaint, S(2));
        }

        void pill(const std::string &t, ImU32 colour)
        {
            ImFont *font = g_ui->bold;
            const float px = S(12.5f);
            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0, t.c_str());
            const ImVec2 pad(S(10), S(4));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 size(sz.x + pad.x * 2, sz.y + pad.y * 2);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + size, alpha(colour, 0.16f), size.y * 0.5f);
            dl->AddText(font, px, p + pad, colour, t.c_str());
            ImGui::Dummy(size);
        }

        void progressBar(float fraction, float height = 8)
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = S(height);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + ImVec2(w, h), kLine, h * 0.5f);
            if (fraction >= 0)
            {
                if (fraction > 0)
                    dl->AddRectFilled(p, p + ImVec2(std::max(h, w * std::min(fraction, 1.0f)), h), kTeal, h * 0.5f);
            }
            else // indeterminate: a segment sliding along
            {
                const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
                const float seg = w * 0.28f;
                const float x0 = -seg + (w + seg) * t;
                dl->PushClipRect(p, p + ImVec2(w, h), true);
                dl->AddRectFilled(p + ImVec2(x0, 0), p + ImVec2(x0 + seg, h), kTeal, h * 0.5f);
                dl->PopClipRect();
            }
            ImGui::Dummy(ImVec2(w, h));
        }

        // ---- buttons -----------------------------------------------------------------------------------------
        enum class Kind
        {
            Primary,
            Secondary,
            Ghost
        };

        bool button(const char *label, Kind kind = Kind::Secondary, ImVec2 size = ImVec2(0, 0), float fontSize = 15)
        {
            ImGui::PushFont(kind == Kind::Primary ? g_ui->bold : g_ui->body, fontSize);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(9));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(16), S(9)));
            const ImU32 base = kind == Kind::Primary ? kPink : kind == Kind::Secondary ? kCardHover : 0;
            const ImU32 hover = kind == Kind::Primary ? kPinkHover : kind == Kind::Secondary ? rgb(50, 42, 69) : alpha(kText, 0.06f);
            ImGui::PushStyleColor(ImGuiCol_Button, base);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kind == Kind::Primary ? rgb(214, 62, 124) : alpha(kText, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_Text, kind == Kind::Primary ? rgb(255, 255, 255) : kind == Kind::Ghost ? kDim : kText);
            const bool pressed = ImGui::Button(label, size);
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
            ImGui::PopFont();
            return pressed;
        }

        // a card: rounded, no border, its own padding; grows with its content
        void beginCard(const char *id, ImU32 bg = kCard)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(20)));
            ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
        }

        void endCard(ImU32 accent = 0)
        {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            if (accent) // a bar down the card's left edge
            {
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + S(4), b.y), accent, S(14), ImDrawFlags_RoundCornersLeft);
            }
            gap(14);
        }

        // a label / value row inside a card
        void row(const char *label, const std::string &value, ImU32 colour = kText)
        {
            ImGui::PushFont(g_ui->body, 14.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::SameLine(S(150));
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
            gap(4);
        }

        // ---- state of the steps --------------------------------------------------------------------------------
        Status folderStatus(const App &app)
        {
            if (!app.doctorDone)
                return kUnknown;
            if (app.doctorFailed())
                return kWarning;
            return app.doctor["folder"]["warnings"].size() ? kWarning : kGood;
        }

        Status discStatus(const App &app) { return app.discChecked && app.disc.ok ? kGood : kMissing; }

        Status toolsStatus(const App &app)
        {
            if (!app.doctorDone)
                return kUnknown;
            if (app.doctorFailed())
                return kWarning;
            const Json &d = app.doctor;
            if (!d["compiler"]["ok"].boolean() || !d["downloads"]["ok"].boolean())
                return d["build"]["current"].boolean() ? kWarning : kMissing; // only matters for the next build
            return d["memory"]["ok"].boolean() ? kGood : kWarning;
        }

        Status gameStatus(const App &app)
        {
            if (!app.doctorDone)
                return kUnknown;
            if (app.doctorFailed())
                return kWarning;
            if (app.doctor["build"]["current"].boolean())
                return kGood;
            return app.doctor["build"]["built"].boolean() ? kWarning : kMissing;
        }

        std::string folderLine(const App &app)
        {
            if (!app.doctorDone)
                return "Checking";
            if (app.doctorFailed())
                return "Couldn't check";
            const size_t n = app.doctor["folder"]["warnings"].size();
            if (n)
                return n == 1 ? "1 thing to look at" : std::to_string(n) + " things to look at";
            char t[64];
            std::snprintf(t, sizeof(t), "%.0f GB free", app.doctor["folder"]["free_bytes"].num() / (1 << 30));
            return t;
        }

        std::string discLine(const App &app)
        {
            if (!app.discChecked)
                return "Not chosen yet";
            if (app.disc.ok)
                return "USA v1.00, verified";
            return app.disc.code == "not_found" ? "File not found" : app.disc.code == "not_iso" ? "Not an .iso"
                                                                                              : "Not the supported disc";
        }

        std::string toolsLine(const App &app)
        {
            if (!app.doctorDone)
                return "Checking";
            if (app.doctorFailed())
                return "Couldn't check";
            if (!app.doctor["compiler"]["ok"].boolean())
                return "Needs installing";
            if (!app.doctor["downloads"]["ok"].boolean())
                return "Git missing";
            return "Ready";
        }

        std::string gameLine(const App &app)
        {
            if (!app.doctorDone)
                return "Checking";
            if (app.doctorFailed())
                return "Couldn't check";
            const Json &b = app.doctor["build"];
            if (b["current"].boolean())
                return "Built, up to date";
            return b["built"].boolean() ? "Needs an update" : "Not built yet";
        }

        // ---- the sidebar ---------------------------------------------------------------------------------------
        void logo(ImDrawList *dl, ImVec2 c, float r)
        {
            // the app icon (launcher/make_icon.py), small: a rounded square of dusk sky, a striped sun, the sea
            const ImVec2 a = c - ImVec2(r, r), b = c + ImVec2(r, r);
            const ImU32 sky = rgb(58, 24, 78);
            dl->AddRectFilled(a, b, sky, r * 0.42f);
            dl->PushClipRect(a, b, true);
            dl->AddCircleFilled(c + ImVec2(0, r * 0.08f), r * 0.56f, rgb(255, 170, 92), 32);
            for (int i = 0; i < 3; ++i)
            {
                const float y = c.y + r * (0.14f + 0.15f * i);
                dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + r * (0.05f + 0.03f * i)), sky);
            }
            dl->AddRectFilled(ImVec2(a.x, c.y + r * 0.58f), b, rgb(28, 98, 110), r * 0.42f, ImDrawFlags_RoundCornersBottom);
            dl->PopClipRect();
        }

        bool stepRow(int index, const char *name, const std::string &line, Status status, bool selected, bool last)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = S(56);
            ImGui::PushID(index);
            const bool clicked = ImGui::InvisibleButton("step", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (selected || hovered)
                dl->AddRectFilled(p, p + ImVec2(w, h), selected ? kCard : alpha(kText, 0.03f), S(10));
            const ImVec2 c = p + ImVec2(S(24), h * 0.5f);
            const float r = S(12);
            if (!last) // the line to the next step's badge
                dl->AddLine(ImVec2(c.x, c.y + r + S(5)), ImVec2(c.x, c.y + h + S(6) - r - S(5)),
                            status == kGood ? alpha(kTeal, 0.45f) : kLine, S(2));
            badge(dl, c, r, status, index + 1);
            dl->AddText(g_ui->bold, S(15.5f), p + ImVec2(S(48), S(9)), selected ? kText : rgb(214, 208, 228), name);
            const std::string l = ellipsize(line, w - S(58), g_ui->body, S(13.5f));
            dl->AddText(g_ui->body, S(13.5f), p + ImVec2(S(48), S(30)),
                        status == kWarning ? kAmber : kDim, l.c_str());
            return clicked;
        }

        bool navRow(const char *label, bool selected, const char *note = nullptr)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = S(38);
            const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            if (selected || hovered)
                dl->AddRectFilled(p, p + ImVec2(w, h), selected ? kCard : alpha(kText, 0.03f), S(10));
            dl->AddText(g_ui->body, S(15), p + ImVec2(S(16), S(9)), selected ? kText : kDim, label);
            if (note)
            {
                const ImVec2 sz = g_ui->bold->CalcTextSizeA(S(11.5f), FLT_MAX, 0, note);
                const ImVec2 a = p + ImVec2(w - sz.x - S(26), S(10)), b = a + ImVec2(sz.x + S(14), sz.y + S(6));
                dl->AddRectFilled(a, b, alpha(kPink, 0.2f), S(9));
                dl->AddText(g_ui->bold, S(11.5f), a + ImVec2(S(7), S(3)), kPink, note);
            }
            return clicked;
        }

        void drawSidebar(App &app, Ui &ui, int shown)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(22)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(6)));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
            ImGui::BeginChild("sidebar", ImVec2(S(264), 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            logo(dl, p + ImVec2(S(26), S(22)), S(20));
            dl->AddText(g_ui->bold, S(18), p + ImVec2(S(56), S(4)), kText, "Vice City Stories");
            const std::string sub = "PC port  \xC2\xB7  " + app.sourceVersion;
            dl->AddText(g_ui->body, S(13), p + ImVec2(S(56), S(26)), kFaint,
                        ellipsize(sub, S(190), g_ui->body, S(13)).c_str());
            gap(64);

            text("   SET UP", kFaint, 11.5f, true);
            gap(2);
            const char *names[] = {"Folders", "Disc", "Build tools", "Game"};
            const Status states[] = {folderStatus(app), discStatus(app), toolsStatus(app), gameStatus(app)};
            const std::string lines[] = {folderLine(app), discLine(app), toolsLine(app), gameLine(app)};
            for (int i = 0; i < 4; ++i)
                if (stepRow(i, names[i], lines[i], states[i], shown == i, i == 3))
                {
                    ui.page = i;
                    if (app.task && app.task->finished)
                        app.dismissTask();
                }
            gap(18);
            text("   MORE", kFaint, 11.5f, true);
            gap(2);
            const char *more[] = {"Saves", "Settings", "Help"};
            for (int i = 0; i < 3; ++i)
            {
                const char *note = i == 1 && app.updateInfo["newer"].boolean() ? "UPDATE" : nullptr;
                if (navRow(more[i], shown == kSaves + i, note))
                {
                    ui.page = kSaves + i;
                    if (app.task && app.task->finished)
                        app.dismissTask();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }

        // ---- the pages -----------------------------------------------------------------------------------------
        // The check (tools/doctor.py) gave no answer: say so and why, offer to run it again. True when it failed - the
        // page shows nothing else, since everything on it would be a guess.
        bool checkFailed(App &app)
        {
            if (!app.doctorFailed())
                return false;
            beginCard("checkfailed", rgb(48, 26, 36));
            text("Couldn't check", kText, 17, true);
            gap(4);
            para(app.doctorError, kDim, 14, 900);
            gap(10);
            ImGui::BeginDisabled(app.doctorRunning);
            if (button(app.doctorRunning ? "Checking..." : "Check again", Kind::Secondary))
                app.refresh();
            ImGui::EndDisabled();
            endCard(kRed);
            return true;
        }

        void openBuildDirDialog(Ui &ui, const fs::path &start)
        {
            SDL_ShowOpenFolderDialog(
                [](void *user, const char *const *files, int) {
                    auto *self = static_cast<Ui *>(user);
                    if (files && files[0])
                    {
                        std::lock_guard<std::mutex> lock(self->dialogLock);
                        self->pickedBuildDir.push_back(files[0]);
                    }
                },
                &ui, ui.window, pathText(start).c_str(), false);
        }

        void drawFolder(App &app, Ui &ui)
        {
            header("STEP 1 OF 4", "Folders",
                   "Where the game is built. It can go on another drive with more room; a short local folder works best.");

            // the build folder: the launcher knows it without the check
            beginCard("buildfolder");
            text("Build folder", kText, 16, true);
            const bool custom = app.buildCustom();
            const char *label = custom ? "CHOSEN" : "DEFAULT";
            const float pw = g_ui->bold->CalcTextSizeA(S(12.5f), FLT_MAX, 0, label).x + S(20);
            ImGui::SameLine(contentRight() - pw);
            pill(label, custom ? kPink : kFaint);
            para("The built game, its settings and logs: about 1 to 3 GB.", kFaint, 14);
            gap(8);
            row("Location", pathText(app.build));
            if (app.doctor.isObject())
            {
                char t[96];
                std::snprintf(t, sizeof(t), "%.1f GB on that drive  (a build needs about %.0f GB)",
                              app.doctor["folder"]["free_bytes"].num() / (1 << 30),
                              app.doctor["folder"]["need_bytes"].num() / (1 << 30));
                row("Free space", t);
            }
            gap(6);
            ImGui::BeginDisabled(app.busy() || app.gameRunning);
            if (button("Choose another folder", Kind::Secondary))
                openBuildDirDialog(ui, app.build.parent_path());
            if (custom)
            {
                ImGui::SameLine();
                if (button("Use the default", Kind::Ghost))
                {
                    app.startBuildDirCheck(app.root / "build");
                    ui.awaitingBuildDir = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (button("Open", Kind::Ghost))
                openFolder(app.build);
            if (app.gameRunning)
                para("Quit the game to change it.", kAmber, 14);
            endCard();

            beginCard("source");
            text("Source folder", kText, 16, true);
            para("The launcher, its scripts and the recompiled code (0.2 GB). Move it by moving the whole folder.", kFaint, 14);
            gap(8);
            row("Location", pathText(app.root));
            if (app.doctor.isObject())
            {
                const Json &m = app.doctor["memory"];
                row("PC memory", m["message"].str(), m["ok"].boolean() ? kText : kAmber);
            }
            gap(6);
            if (button("Open in Explorer", Kind::Ghost))
                openFolder(app.root);
            endCard();

            if (!app.doctorDone)
            {
                para("Checking...");
                return;
            }
            if (checkFailed(app))
                return;
            const Json &f = app.doctor["folder"];
            for (size_t i = 0; i < f["warnings"].size(); ++i)
            {
                beginCard(("warn" + std::to_string(i)).c_str(), rgb(44, 34, 30));
                text("Worth fixing", kAmber, 13, true);
                gap(2);
                para(f["warnings"][i]["message"].str(), kText, 15, 900);
                endCard(kAmber);
            }
        }

        void drawDrives(App &app, Ui &ui);

        void discIcon(ImDrawList *dl, ImVec2 c, float r, ImU32 colour)
        {
            dl->AddCircle(c, r, colour, 40, S(2));
            dl->AddCircle(c, r * 0.32f, colour, 24, S(2));
            dl->PathClear();
            dl->PathArcTo(c, r * 0.68f, -2.4f, -1.4f, 12);
            dl->PathStroke(alpha(colour, 0.6f), ImDrawFlags_None, S(1.6f));
        }

        void drawDisc(App &app, Ui &ui)
        {
            header("STEP 2 OF 4", "Your disc",
                   "Your own copy of GTA: Vice City Stories for the PS2 (USA, SLUS-21590) as a disc image (.iso). The "
                   "game is recompiled from it and reads its files from it while you play.");
            beginCard("disc");
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool ok = app.discChecked && app.disc.ok;
            discIcon(dl, p + ImVec2(S(22), S(22)), S(20), app.discChecked ? (ok ? kTeal : kRed) : kFaint);
            ImGui::SetCursorScreenPos(p + ImVec2(S(60), 0));
            ImGui::BeginGroup();
            const float width = ImGui::GetContentRegionAvail().x - S(140);
            if (app.discChecked)
            {
                text(ellipsize(pathText(app.iso.filename()), width, g_ui->bold, S(16)), kText, 16, true);
                text(ellipsize(pathText(app.iso.parent_path()), width, g_ui->body, S(13.5f)), kFaint, 13.5f);
            }
            else
            {
                text("No disc image yet", kText, 16, true);
                text("Choose it, or drop the .iso onto this window", kFaint, 13.5f);
            }
            ImGui::EndGroup();
            if (app.discChecked)
            {
                const std::string &code = app.disc.code;
                const char *label = ok ? "VERIFIED"
                                    : code == "not_found" ? "NOT FOUND"
                                    : code == "not_iso"   ? "NOT AN ISO"
                                                          : "NOT SUPPORTED";
                const float pw = g_ui->bold->CalcTextSizeA(S(12.5f), FLT_MAX, 0, label).x + S(20);
                ImGui::SameLine(contentRight() - pw);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(8));
                pill(label, ok ? kTeal : kRed);
            }
            gap(10);
            if (app.discChecked)
            {
                para(app.disc.message + (ok && app.discFound ? "  -  found automatically" : ""), ok ? kDim : kRed, 14.5f, 900);
                gap(8);
            }
            if (button(app.discChecked ? "Choose another file" : "Choose your .iso", Kind::Secondary))
                openIsoDialog(ui);
            ImGui::SameLine();
            if (button("Copy from a DVD drive", Kind::Ghost) && !app.busy())
            {
                app.drives = Json();
                app.startRipList();
                ui.showDrives = true;
            }
            ImGui::SameLine();
            if (button("Search again", Kind::Ghost))
            {
                app.scanForDisc();
                if (!app.disc.ok)
                    app.say("No Vice City Stories .iso in Downloads, Desktop, Documents or this folder.");
            }
            endCard();
            if (ui.showDrives)
                drawDrives(app, ui);
        }

        void ripTargetDialog(Ui &ui, const fs::path &suggested)
        {
            static const SDL_DialogFileFilter filters[] = {{"Disc image", "iso"}};
            SDL_ShowSaveFileDialog(
                [](void *user, const char *const *files, int) {
                    auto *self = static_cast<Ui *>(user);
                    if (files && files[0])
                    {
                        std::lock_guard<std::mutex> lock(self->dialogLock);
                        self->pickedRipTarget.push_back(files[0]);
                    }
                },
                &ui, ui.window, filters, 1, pathText(suggested).c_str());
        }

        void drawDrives(App &app, Ui &ui)
        {
            beginCard("drives");
            text("Copy from a DVD drive", kText, 16, true);
            gap(2);
            para("Put the disc in a DVD drive of this PC. It is copied to an .iso (about 3.9 GB, a few minutes) and "
                 "used from then on.", kDim, 14.5f, 900);
            gap(8);
            if (app.busy() && app.task->kind == "rip_list")
                para("Looking for drives...");
            else if (!app.drives.size())
                para("No DVD drive found on this PC.", kAmber);
            else
                for (size_t i = 0; i < app.drives.size(); ++i)
                {
                    const Json &d = app.drives[i];
                    const std::string drive = d["drive"].str();
                    const std::string label = drive + "   " + (d["ok"].boolean() ? d["message"].str() + "  (" + d["boot"].str() + ")"
                                                                                   : d["message"].str());
                    ImGui::BeginDisabled(!d["ok"].boolean());
                    ImGui::PushFont(g_ui->body, 15);
                    if (ImGui::Selectable(label.c_str(), ui.ripDrive == drive))
                        ui.ripDrive = drive;
                    ImGui::PopFont();
                    ImGui::EndDisabled();
                }
            gap(8);
            ImGui::BeginDisabled(ui.ripDrive.empty() || app.busy());
            if (button("Copy to an .iso...", Kind::Secondary))
                ripTargetDialog(ui, knownFolder(Folder::Documents) / "Grand Theft Auto - Vice City Stories (USA).iso");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (button("Look again", Kind::Ghost) && !app.busy())
                app.startRipList();
            ImGui::SameLine();
            if (button("Close", Kind::Ghost))
                ui.showDrives = false;
            endCard();
        }

        void drawTools(App &app)
        {
            header("STEP 3 OF 4", "Build tools",
                   "The game is compiled on your PC with Microsoft's free C++ tools and clang. Nothing of the game is "
                   "downloaded - only the compiler.");
            if (!app.doctorDone)
            {
                para("Checking...");
                return;
            }
            if (checkFailed(app))
                return;
            const Json &c = app.doctor["compiler"];
            if (!c["ok"].boolean())
            {
                const std::string code = c["code"].str();
                beginCard("install", rgb(40, 27, 46));
                text(errorHelp(code).title, kText, 17, true);
                gap(4);
                para(c["message"].str() + ". The launcher can run Microsoft's installer with the right options ticked; "
                     "Windows asks for permission first, and the installer shows its own progress.", kDim, 14.5f, 900);
                gap(10);
                const char *label = code == "no_clang"   ? "Add the Clang tools"
                                    : code == "old_clang" ? "Update Visual Studio"
                                                          : "Install build tools  (about 6 GB)";
                if (button(label, Kind::Primary) && !app.busy())
                    app.startInstallTools();
                endCard();
            }
            beginCard("tools");
            row("Compiler", c["ok"].boolean() ? c["message"].str() : "missing", c["ok"].boolean() ? kText : kRed);
            const Json &d = app.doctor["downloads"];
            row("Libraries", d["bundled"].boolean() ? "Bundled with this release" : d["message"].str(),
                d["ok"].boolean() ? kText : kRed);
            row("Python", app.doctor["python"]["version"].str() +
                              (app.doctor["python"]["bundled"].boolean() ? "  (bundled)" : "  (" + app.pythonText + ")"));
            if (!d["ok"].boolean())
            {
                gap(6);
                if (button("Get Git", Kind::Secondary))
                    openUrl("https://git-scm.com/download/win");
            }
            endCard();
        }

        const char *optLabel(const std::string &opt)
        {
            if (opt == "lto")
                return "Standard";
            if (opt == "none")
                return "Quick to build";
            if (opt == "pgo")
                return "Fastest game";
            return "Keep the current type";
        }

        const char *optNote(const std::string &opt)
        {
            if (opt == "lto")
                return "Optimised (ThinLTO). What most players want.";
            if (opt == "none")
                return "Builds faster, the game runs a little slower.";
            if (opt == "pgo")
                return "Adds about 30 minutes: the game is played once, hidden, to tune the build.";
            return "A first build is Standard.";
        }

        void drawGame(App &app)
        {
            header("STEP 4 OF 4", "The game", "Recompiled from your disc and compiled into build\\vcs.exe.");
            if (!app.doctorDone)
            {
                para("Checking...");
                return;
            }
            if (checkFailed(app))
                return;
            const Json &b = app.doctor["build"];
            beginCard("state");
            if (b["current"].boolean())
            {
                pill("UP TO DATE", kTeal);
                gap(6);
                std::string opt = b["info"]["opt"].str();
                opt = opt.substr(0, opt.find(" - ")); // build.py's hint for developers stays out
                para("Built from this version (" + opt + "). Press Play.", kText, 15, 900);
            }
            else if (b["built"].boolean())
            {
                pill("NEEDS AN UPDATE", kAmber);
                gap(6);
                for (size_t i = 0; i < b["reasons"].size(); ++i)
                    para("\xE2\x80\xA2  " + b["reasons"][i].str(), kDim, 14.5f, 900);
                gap(4);
                para("Updating takes minutes: only what changed is compiled again.", kText, 14.5f, 900);
            }
            else
            {
                pill("NOT BUILT YET", kFaint);
                gap(6);
                para("The first build takes 10 to 60 minutes depending on the PC, once. The PC stays usable meanwhile.",
                     kText, 15, 900);
            }
            endCard();

            beginCard("type");
            text("Build type", kText, 15.5f, true);
            gap(6);
            ImGui::PushFont(g_ui->body, 15);
            ImGui::SetNextItemWidth(S(280));
            if (ImGui::BeginCombo("##opt", optLabel(app.settings.opt)))
            {
                for (const char *opt : {"", "lto", "none", "pgo"})
                    if (ImGui::Selectable(optLabel(opt), app.settings.opt == opt))
                    {
                        app.settings.opt = opt;
                        app.saveSettings();
                    }
                ImGui::EndCombo();
            }
            ImGui::PopFont();
            gap(4);
            para(optNote(app.settings.opt), kFaint, 14);
            endCard();

            if (button(b["current"].boolean() ? "Build again" : "Build now", Kind::Secondary) && !app.busy() && app.disc.ok)
                app.startBuild();
            ImGui::SameLine();
            if (button("Check again", Kind::Ghost) && !app.doctorRunning)
                app.refresh();
            gap(16);
            para("The game's own settings - graphics, widescreen, controls, sound - are in the game: press F2 while "
                 "playing.", kFaint, 14);
        }

        void drawSaves(App &app, Ui &ui)
        {
            header(nullptr, "Saves", "Your memory card, kept apart from the game so updates and rebuilds never touch it.");
            if (!ui.backupsAsked && !app.busy())
            {
                ui.backupsAsked = true;
                app.startBackupList();
            }
            beginCard("card");
            row("Memory card", pathText(app.savesFolder()));
            gap(6);
            ImGui::BeginDisabled(app.busy());
            if (button("Back up now", Kind::Secondary))
            {
                app.startBackup();
                ui.backupsAsked = false; // the list again afterwards
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (button("Open the folder", Kind::Ghost))
                openFolder(app.savesFolder());
            endCard();

            beginCard("backups");
            text("Backups", kText, 16, true);
            gap(6);
            if (app.busy() && (app.task->kind == "backup_list" || app.task->kind == "backup"))
                para("Loading...");
            else if (!app.backups.size())
                para("None yet. A backup is a zip of the card, with the order the game needs.", kFaint, 14.5f);
            else
                for (size_t i = 0; i < app.backups.size(); ++i)
                {
                    const Json &bk = app.backups[i];
                    const std::string name = bk["name"].str();
                    ImGui::PushID(int(i));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float w = ImGui::GetContentRegionAvail().x;
                    if (i)
                        ImGui::GetWindowDrawList()->AddLine(p + ImVec2(0, -S(4)), p + ImVec2(w, -S(4)), kLine);
                    ImGui::BeginGroup();
                    text(name, kText, 15);
                    text(bk["saves"].str(), kFaint, 13);
                    ImGui::EndGroup();
                    ImGui::SameLine(w - S(110));
                    ImGui::BeginDisabled(app.busy() || app.gameRunning);
                    if (button("Restore", Kind::Ghost))
                    {
                        ui.restoreName = name;
                        ui.confirmRestore = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    gap(6);
                }
            if (app.gameRunning)
                para("Quit the game to restore a backup.", kAmber, 14);
            gap(4);
            if (button("Open the backups folder", Kind::Ghost))
                openFolder(app.savesFolder() / "backups");
            endCard();
        }

        void drawSettings(App &app, Ui &ui)
        {
            header(nullptr, "Settings", "The launcher's own options. The game's are in the game (F2).");
            bool changed = false;
            beginCard("launch");
            text("When the game starts", kText, 15.5f, true);
            gap(6);
            ImGui::PushFont(g_ui->body, 15);
            const char *values[] = {"minimize", "close", "stay"};
            const char *labels[] = {"Minimize the launcher - it comes back when the game ends", "Close the launcher",
                                    "Keep the launcher open"};
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(3), S(3)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(10)));
            for (int i = 0; i < 3; ++i)
                if (ImGui::RadioButton(labels[i], app.settings.afterLaunch == values[i]))
                {
                    app.settings.afterLaunch = values[i];
                    changed = true;
                }
            ImGui::PopStyleVar(2);
            ImGui::PopFont();
            endCard();

            beginCard("updates");
            text("Updates", kText, 15.5f, true);
            gap(6);
            if (app.updateInfo["newer"].boolean())
            {
                pill("VERSION " + app.updateInfo["latest"].str() + " AVAILABLE", kPink);
                gap(6);
                ImGui::BeginDisabled(app.busy() || app.gameRunning);
                if (button("Update now", Kind::Primary))
                    app.startUpdate();
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (button("What's new", Kind::Ghost))
                    openUrl(app.updateInfo["url"].str());
                gap(6);
            }
            ImGui::PushFont(g_ui->body, 15);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(3), S(3)));
            changed |= ImGui::Checkbox("Check for a new release when the launcher starts", &app.settings.checkUpdates);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            para("One request to GitHub; nothing about you or your PC is sent.", kFaint, 14);
            gap(6);
            ImGui::BeginDisabled(app.busy());
            if (button("Check now", Kind::Secondary))
                app.startUpdateCheck(false);
            ImGui::EndDisabled();
            endCard();

            beginCard("shortcut");
            text("Desktop shortcut", kText, 15.5f, true);
            gap(6);
            if (button("Add a shortcut to the desktop", Kind::Secondary))
            {
                std::string error;
                app.say(app.shortcut(error) ? "Shortcut \"GTA Vice City Stories\" added to the desktop" : "No shortcut: " + error);
            }
            endCard();
            if (changed)
                app.saveSettings();
            (void)ui;
        }

        void drawHelp(App &app)
        {
            header(nullptr, "Help", "When something goes wrong, the logs say why.");
            beginCard("report");
            text("Report a problem", kText, 15.5f, true);
            gap(4);
            para("Makes a zip of the build's and the game's logs and what this PC has - no game files, no saves - to "
                 "attach to your bug report.", kDim, 14.5f, 900);
            gap(8);
            ImGui::BeginDisabled(app.busy());
            if (button("Make a report", Kind::Secondary))
                app.startReport();
            ImGui::EndDisabled();
            endCard();

            beginCard("files");
            text("Files", kText, 15.5f, true);
            gap(6);
            if (button("The game's last log", Kind::Ghost))
                openFile(app.build / "run" / "launcher_game.log");
            ImGui::SameLine();
            if (button("Build log", Kind::Ghost))
                openFile(app.build / "build.log");
            ImGui::SameLine();
            if (button("README", Kind::Ghost))
                openFile(app.root / "README.md");
            endCard();

            beginCard("about");
            row("Version", app.sourceVersion);
            row("Folder", pathText(app.root));
            row("Python", app.pythonText);
            gap(4);
            para("Unofficial. Grand Theft Auto is property of Rockstar Games / Take-Two Interactive; this port contains "
                 "no game code or data.", kFaint, 13.5f, 900);
            endCard();
        }

        // ---- a running task ------------------------------------------------------------------------------------
        bool takesOver(const Task &t)
        {
            return t.kind == "build" || t.kind == "tools" || t.kind == "update" || t.kind == "rip" || t.kind == "report" ||
                   t.kind == "restore" || (t.finished && !t.ok());
        }

        void drawTask(App &app, Ui &ui, double now)
        {
            Task &t = *app.task;
            const bool failed = t.finished && !t.ok() && !t.cancelled;
            const char *eyebrow = !t.finished ? "IN PROGRESS" : t.cancelled ? "STOPPED" : failed ? "DIDN'T WORK" : "DONE";
            header(eyebrow, t.title, "");
            const double elapsed = (t.finished ? t.endTime : now) - t.startTime;

            if (!t.finished)
            {
                beginCard("progress");
                const std::string stageTitle = t.stages.empty() ? "Starting" : t.stages.back().title;
                if (t.total > 0)
                {
                    char pct[16];
                    std::snprintf(pct, sizeof(pct), "%d%%", int(100.0 * t.done / t.total));
                    text(pct, kText, 40, true);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(16));
                    text(stageTitle, kDim, 15);
                    gap(6);
                    progressBar(float(t.done) / float(t.total));
                }
                else
                {
                    text(stageTitle, kText, 18, true);
                    gap(10);
                    progressBar(-1);
                }
                gap(8);
                std::string line = duration(elapsed) + " so far";
                if (t.total > 0 && t.note == "compiling")
                    line += "   \xC2\xB7   " + std::to_string(t.done) + " of " + std::to_string(t.total) + " steps";
                if (t.eta >= 0)
                    line += "   \xC2\xB7   about " + duration(t.eta) + " left in this step";
                text(line, kFaint, 13.5f);
                endCard();
            }
            else if (failed)
            {
                const ErrorHelp &help = errorHelp(t.errorCode);
                beginCard("error", rgb(48, 26, 36));
                text(help.title, kText, 18, true);
                gap(4);
                if (!t.errorMessage.empty())
                    para(t.errorMessage, kDim, 14.5f, 900);
                gap(6);
                para(help.fix, kText, 15, 900);
                gap(10);
                if (!t.errorLog.empty() && button("Open the log", Kind::Secondary))
                    openFile(app.root / pathFrom(t.errorLog));
                if (!t.errorLog.empty())
                    ImGui::SameLine();
                const bool report = button("Make a report", Kind::Ghost);
                endCard(kRed);
                if (report)
                {
                    app.dismissTask();
                    app.startReport();
                    return;
                }
            }
            else
            {
                beginCard("done", t.cancelled ? kCard : rgb(24, 44, 46));
                if (t.cancelled)
                    para("Stopped. Starting again continues where it stopped.", kText, 15.5f);
                else
                {
                    std::string message = "Done in " + duration(elapsed) + ".";
                    if (t.kind == "build")
                        message += " The game is ready.";
                    if (t.kind == "tools")
                        message = t.result["message"].str().empty() ? "The build tools are installed." : t.result["message"].str();
                    if (t.kind == "update")
                        message = "Updated to " + t.result["version"].str() + ". Restart the launcher, then update the build.";
                    if (t.kind == "rip")
                        message = "Copied to " + t.result["iso"].str() + ".";
                    if (t.kind == "report")
                        message = "The report is ready: a zip of the logs, no game files, no saves.";
                    if (t.kind == "restore")
                        message = "Saves restored. The card as it was is in a backup too.";
                    para(message, kText, 15.5f, 900);
                    if (t.kind == "report")
                    {
                        gap(8);
                        if (button("Show the report", Kind::Secondary))
                            revealFile(pathFrom(t.result["path"].str()));
                    }
                }
                endCard();
            }

            // the stages as a timeline
            if (!t.stages.empty())
            {
                beginCard("stages");
                ImDrawList *dl = ImGui::GetWindowDrawList();
                for (size_t i = 0; i < t.stages.size(); ++i)
                {
                    const Task::Stage &s = t.stages[i];
                    const bool last = i + 1 == t.stages.size();
                    const bool running = !t.finished && last;
                    const bool broke = failed && last;
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const ImVec2 c = p + ImVec2(S(9), S(11));
                    if (running)
                        spinner(dl, c, S(7), kPink, S(2.2f));
                    else if (broke)
                        cross(dl, c, S(10), kRed, S(2.2f));
                    else if (t.cancelled && last)
                        dl->AddCircle(c, S(6), kFaint, 16, S(1.6f));
                    else
                        checkMark(dl, c, S(10), kTeal, S(2.2f));
                    ImGui::SetCursorScreenPos(p + ImVec2(S(30), 0));
                    text(s.title, running ? kText : broke ? kRed : kDim, 15);
                    if (s.end > 0 || running)
                    {
                        const std::string d = duration(std::max(0.0, (s.end > 0 ? s.end : now) - s.start));
                        const float w = g_ui->body->CalcTextSizeA(S(14), FLT_MAX, 0, d.c_str()).x;
                        ImGui::SameLine(contentRight() - w);
                        text(d, kFaint, 14);
                    }
                    gap(2);
                }
                endCard();
            }

            if (button(ui.showLog ? "Hide details" : "Show details", Kind::Ghost))
                ui.showLog = !ui.showLog;
            if (ui.showLog || failed)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
                ImGui::BeginChild("log", ImVec2(0, std::max(S(160), ImGui::GetContentRegionAvail().y)),
                                  ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
                const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
                ImGui::PushFont(g_ui->body, 13.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGuiListClipper clipper;
                clipper.Begin(int(t.log.size()));
                while (clipper.Step())
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        ImGui::TextUnformatted(t.log[size_t(i)].c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (atBottom)
                    ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
            }
        }

        // ---- the footer ----------------------------------------------------------------------------------------
        void drawFooter(App &app, Ui &ui, double now, const std::string &next)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            dl->AddLine(wp, wp + ImVec2(ImGui::GetWindowWidth(), 0), kLine);

            // left: what's happening
            const float midY = ImGui::GetWindowHeight() * 0.5f;
            ImGui::SetCursorPosY(midY - S(10));
            if (app.gameEnded && app.gameExit != 0)
            {
                text(ellipsize(app.gameExitText(), ImGui::GetContentRegionAvail().x - S(420)), kRed, 14.5f);
                ImGui::SameLine();
                ImGui::SetCursorPosY(midY - S(17));
                if (button("Log", Kind::Ghost))
                    openFile(app.gameLog);
                ImGui::SameLine();
                ImGui::SetCursorPosY(midY - S(17));
                if (button("Dismiss", Kind::Ghost))
                    app.gameEnded = false;
            }
            else if (app.gameRunning)
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float pulse = 0.55f + 0.45f * float(std::sin(ImGui::GetTime() * 3.0));
                dl->AddCircleFilled(p + ImVec2(S(5), S(10)), S(4.5f), alpha(kTeal, pulse));
                ImGui::SetCursorScreenPos(p + ImVec2(S(18), 0));
                text("Playing  \xC2\xB7  started " + duration(now - app.gameStart) + " ago", kDim, 14.5f);
            }
            else if (!app.toast.empty() && now - app.toastTime < 8.0)
                text(ellipsize(app.toast, ImGui::GetContentRegionAvail().x - S(260)), kTeal, 14.5f);
            else if (app.busy() && takesOver(*app.task))
                text(app.task->title + "...", kDim, 14.5f);

            // right: the next thing to do
            const ImVec2 big(S(220), S(50));
            const float right = contentRight();
            ImGui::SetCursorPos(ImVec2(right - big.x, midY - big.y * 0.5f));
            auto disabledPrimary = [&](const char *label) {
                ImGui::BeginDisabled();
                button(label, Kind::Primary, big, 17);
                ImGui::EndDisabled();
            };
            if (app.busy() && takesOver(*app.task))
            {
                ImGui::SetCursorPos(ImVec2(right - S(130), midY - S(19)));
                if (button(app.task->cancelled ? "Stopping..." : "Cancel", Kind::Secondary, ImVec2(S(130), S(38))))
                    app.cancelTask();
            }
            else if (app.task && app.task->finished && takesOver(*app.task) && !app.task->ok() && !app.task->cancelled)
            {
                // a failure: try again (the build and the installer continue where they stopped), or close it
                const bool retry = app.task->kind == "build" || app.task->kind == "tools";
                ImGui::SetCursorPos(ImVec2(right - big.x - S(142), midY - S(19)));
                if (button("Close", Kind::Ghost, ImVec2(S(130), S(38))))
                    app.dismissTask();
                ImGui::SetCursorPos(ImVec2(right - big.x, midY - big.y * 0.5f));
                if (button(retry ? "Try again" : "OK", Kind::Primary, big, 17))
                {
                    const std::string kind = app.task->kind;
                    app.dismissTask();
                    if (kind == "build")
                        app.startBuild();
                    else if (kind == "tools")
                        app.startInstallTools();
                }
            }
            else if (app.task && app.task->finished && takesOver(*app.task) && app.next() != "play")
            {
                if (button("OK", Kind::Primary, big, 17))
                    app.dismissTask();
            }
            else if (app.gameRunning)
                disabledPrimary("Playing");
            else if (next == "disc")
            {
                if (button("Choose your disc", Kind::Primary, big, 17))
                    openIsoDialog(ui);
            }
            else if (next == "tools")
            {
                if (button("Install build tools", Kind::Primary, big, 17))
                {
                    ui.page = kTools;
                    app.startInstallTools();
                }
            }
            else if (next == "build")
            {
                if (button(app.doctor["build"]["built"].boolean() ? "Update the build" : "Build the game", Kind::Primary, big, 17))
                    app.startBuild();
            }
            else if (next == "recheck")
            {
                if (button(app.doctorRunning ? "Checking..." : "Check again", Kind::Primary, big, 17) && !app.doctorRunning)
                {
                    ui.page = kTools;
                    app.refresh();
                }
            }
            else if (next == "play")
            {
                if (button("Play", Kind::Primary, big, 19))
                {
                    app.dismissTask();
                    std::string error;
                    if (!app.play(error))
                        app.say(error, now);
                    else if (app.settings.afterLaunch == "close")
                        ui.quitNow = true;
                    else if (app.settings.afterLaunch == "minimize")
                    {
                        SDL_MinimizeWindow(ui.window);
                        ui.minimizedForGame = true;
                    }
                }
            }
            else
                disabledPrimary("Checking...");
        }

        // ---- dialogs -------------------------------------------------------------------------------------------
        bool beginDialog(const char *id)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(24)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16));
            ImGui::SetNextWindowSize(ImVec2(S(460), 0));
            const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                                      ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PopStyleVar(2);
            return open;
        }

        void drawDialogs(App &app, Ui &ui)
        {
            // a build folder checked by tools/build_dir.py: what it means, then Build here
            if (ui.awaitingBuildDir && !app.busy() && app.buildDirCheck.has("path"))
            {
                ImGui::OpenPopup("builddir");
                ui.awaitingBuildDir = false;
            }
            else if (ui.awaitingBuildDir && !app.busy() && (!app.task || app.task->finished))
                ui.awaitingBuildDir = false; // the check itself failed: its error shows on the page
            if (beginDialog("builddir"))
            {
                const Json &c = app.buildDirCheck;
                const bool ok = c["ok"].boolean();
                const fs::path target = pathFrom(c["path"].str());
                text(!ok ? "Can't build there" : c["current"].boolean() ? "Already building there" : "Build here?", kText, 20, true);
                gap(6);
                text(c["path"].str(), kText, 15, true);
                gap(4);
                if (!ok)
                    para("Because " + c["problem"].str() + ".", kRed, 15, 400);
                else
                {
                    char t[96];
                    std::snprintf(t, sizeof(t), "%.1f GB free on that drive.", c["free_bytes"].num() / (1 << 30));
                    para(std::string(c["default"].boolean() ? "The default, next to the source. " : "") +
                             (c["reused"].boolean() ? "A build folder already there is used again. " : "") + t,
                         kDim, 15, 400);
                    for (size_t i = 0; i < c["warnings"].size(); ++i)
                        para("! " + c["warnings"][i].str(), kAmber, 14.5f, 400);
                    if (!c["current"].boolean())
                    {
                        gap(6);
                        std::error_code ec;
                        const bool built = fs::is_regular_file(app.build / "vcs.exe", ec);
                        para(std::string("The game is built again there (10 to 60 minutes, once); your game settings and "
                                         "disc choice come along.") +
                                 (built ? " The current build stays in " + pathText(app.build) +
                                              " until you delete it."
                                        : ""),
                             kDim, 15, 400);
                    }
                }
                gap(16);
                if (ok && !c["current"].boolean())
                {
                    if (button("Build here", Kind::Primary))
                    {
                        app.startBuildDirSet(target);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                }
                if (button("Choose another", Kind::Secondary))
                {
                    openBuildDirDialog(ui, target.parent_path());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (button(ok && c["current"].boolean() ? "OK" : "Cancel", Kind::Ghost))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (ui.confirmRestore)
            {
                ImGui::OpenPopup("restore");
                ui.confirmRestore = false;
            }
            if (beginDialog("restore"))
            {
                text("Restore this backup?", kText, 20, true);
                gap(6);
                para(ui.restoreName + " replaces your memory card. The card as it is now is backed up first.", kDim,
                     15, 400);
                gap(16);
                if (button("Restore", Kind::Primary))
                {
                    app.startRestore(ui.restoreName);
                    ui.backupsAsked = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (button("Cancel", Kind::Ghost))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (ui.wantQuit)
            {
                ui.wantQuit = false;
                if (app.busy() && takesOver(*app.task))
                    ImGui::OpenPopup("quit");
                else
                    ui.quitNow = true;
            }
            if (beginDialog("quit"))
            {
                text("Stop and close?", kText, 20, true);
                gap(6);
                para((app.task ? app.task->title : std::string("A task")) +
                         " is still running. Closing stops it; starting it again later continues where it stopped.",
                     kDim, 15, 400);
                gap(16);
                if (button("Stop and close", Kind::Primary))
                {
                    app.cancelTask();
                    ui.quitNow = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (button("Keep it running", Kind::Ghost))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }

    void openIsoDialog(Ui &ui)
    {
        static const SDL_DialogFileFilter filters[] = {{"PS2 disc image (*.iso)", "iso"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(
            [](void *user, const char *const *files, int) {
                auto *self = static_cast<Ui *>(user);
                if (files && files[0])
                {
                    std::lock_guard<std::mutex> lock(self->dialogLock);
                    self->pickedIso.push_back(files[0]);
                }
            },
            &ui, ui.window, filters, 2, nullptr, false);
    }

    void applyStyle(Ui &ui)
    {
        g_ui = &ui;
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);
        style.WindowRounding = 0;
        style.ChildRounding = 14;
        style.FrameRounding = 8;
        style.PopupRounding = 12;
        style.GrabRounding = 8;
        style.ScrollbarRounding = 8;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 0;
        style.PopupBorderSize = 0;
        style.FrameBorderSize = 0;
        style.FramePadding = ImVec2(12, 8);
        style.ItemSpacing = ImVec2(10, 8);
        style.WindowPadding = ImVec2(0, 0);
        style.ScrollbarSize = 10;
        style.SelectableTextAlign = ImVec2(0, 0.5f);
        ImVec4 *c = style.Colors;
        c[ImGuiCol_WindowBg] = v4(kBase);
        c[ImGuiCol_ChildBg] = v4(kBase);
        c[ImGuiCol_PopupBg] = v4(rgb(32, 27, 45));
        c[ImGuiCol_Border] = v4(kLine);
        c[ImGuiCol_Text] = v4(kText);
        c[ImGuiCol_TextDisabled] = v4(kFaint);
        c[ImGuiCol_FrameBg] = v4(kCardHover);
        c[ImGuiCol_FrameBgHovered] = v4(rgb(50, 42, 69));
        c[ImGuiCol_FrameBgActive] = v4(rgb(58, 49, 80));
        c[ImGuiCol_Button] = v4(kCardHover);
        c[ImGuiCol_ButtonHovered] = v4(rgb(50, 42, 69));
        c[ImGuiCol_ButtonActive] = v4(rgb(58, 49, 80));
        c[ImGuiCol_Header] = v4(alpha(kPink, 0.22f));
        c[ImGuiCol_HeaderHovered] = v4(alpha(kPink, 0.14f));
        c[ImGuiCol_HeaderActive] = v4(alpha(kPink, 0.30f));
        c[ImGuiCol_CheckMark] = v4(kPink);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = v4(kLine);
        c[ImGuiCol_ScrollbarGrabHovered] = v4(kFaint);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.02f, 0.05f, 0.7f);
        c[ImGuiCol_NavCursor] = v4(alpha(kPink, 0.6f));
        style.ScaleAllSizes(ui.scale);
        style.FontScaleDpi = ui.scale;
    }

    void drawUi(App &app, Ui &ui, double now)
    {
        g_ui = &ui;
        // the file dialogs' answers
        {
            std::lock_guard<std::mutex> lock(ui.dialogLock);
            for (const std::string &file : ui.pickedIso)
            {
                app.chooseDisc(pathFrom(file));
                ui.page = kDisc;
            }
            ui.pickedIso.clear();
            for (const std::string &file : ui.pickedBuildDir)
            {
                app.startBuildDirCheck(pathFrom(file));
                ui.awaitingBuildDir = true;
            }
            ui.pickedBuildDir.clear();
            for (const std::string &file : ui.pickedRipTarget)
                app.startRip(ui.ripDrive, pathFrom(file));
            ui.pickedRipTarget.clear();
        }

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        const std::string next = app.next();
        const int followed = next == "disc" ? kDisc : next == "tools" || next == "recheck" ? kTools : kGame;
        const bool taskView = app.task && takesOver(*app.task);
        const int shown = taskView ? -2 : ui.page >= 0 ? ui.page : followed;

        drawSidebar(app, ui, shown);
        ImGui::SameLine(0, 0);

        ImGui::BeginGroup();
        const float footerH = S(84);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(44), S(38)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(6)));
        ImGui::BeginChild("main", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (taskView)
            drawTask(app, ui, now);
        else
            switch (shown)
            {
            case kFolder: drawFolder(app, ui); break;
            case kDisc: drawDisc(app, ui); break;
            case kTools: drawTools(app); break;
            case kSaves: drawSaves(app, ui); break;
            case kSettings: drawSettings(app, ui); break;
            case kHelp: drawHelp(app); break;
            default: drawGame(app); break;
            }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(44), 0));
        ImGui::BeginChild("footer", ImVec2(0, footerH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        drawFooter(app, ui, now, next);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        drawDialogs(app, ui);
        ImGui::End();
    }
}
