// GTA: Vice City Stories (PS2) as a native PS5 app (docs/PS5.md).
//
// Finds the user's disc image, checks it holds the supported executable, maps it as cdrom0: and boots the recompiled
// game. The app's folder is /app0 (vcs.ini, userdata/ for the memory card, logs/vcs.log); the disc is "iso=" from
// vcs.ini, the first .iso in /app0/disc or /app0, or a "*Vice City Stories*.iso" on a USB drive.
#include "ps2_runtime.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_perf.h"

#include "debug/crash.h"
#include "debug/game_info.h"
#include "debug/rig.h"
#include "game/vcs_trainer.h"
#include "debug/selftest.h"
#include "debug/watchdog.h"
#include "disc/iso_image.h"
#include "disc/vcs_disc_id.h"
#include "game/vcs_display.h"
#include "game/vcs_framerate.h"
#include "game/vcs_movie.h"
#include "game/vcs_hud.h"
#include "game/vcs_input.h"
#include "game/vcs_volume.h"
#include "platform/platform.h"
#include "render/presenter.h"
#include "rhi/rhi.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"
#include "util/sha1.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <immintrin.h>

#if defined(PS2X_PGO_GEN)
extern "C" int __llvm_profile_write_file(void); // clang's profile runtime (instrumented build, PS2X_PGO=GEN)
#endif

namespace fs = std::filesystem;

namespace vcs
{
    void configureWatch(uint8_t *rdram); // debug/watch.cpp
}

namespace
{
    using vcs::disc::kElfName;
    using vcs::disc::kElfSha1;

    class MappedDisc final : public IPS2DiscImage
    {
    public:
        explicit MappedDisc(const vcs::IsoImage &iso) : m_iso(iso) {}

        bool findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const override
        {
            vcs::IsoEntry entry;
            if (!m_iso.find(ps2Path, entry))
                return false;
            lsn = entry.lba;
            sizeBytes = entry.size;
            isDirectory = entry.isDirectory;
            return true;
        }

        bool readBytes(uint64_t offset, size_t byteCount, void *destination) const override
        {
            return m_iso.readBytes(offset, byteCount, destination);
        }

        uint32_t totalSectors() const override { return m_iso.totalSectors(); }

        bool listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const override
        {
            const vcs::IsoImage::Children *kids = m_iso.children(ps2Path);
            if (!kids)
                return false;
            out.clear();
            out.reserve(kids->size());
            for (const auto &[name, entry] : *kids)
                out.push_back(DirEntry{name, entry.lba, entry.size, entry.isDirectory});
            return true;
        }

    private:
        const vcs::IsoImage &m_iso;
    };

    // The app's folder (writable: a homebrew app runs from /data/homebrew/<title id>, mounted as /app0).
    fs::path appDirectory() { return fs::path("/app0"); }

    fs::path dataDirectory()
    {
        // VCS_DATA_DIR: another save / cache folder (scripted test runs start from an empty memory card)
        if (const char *dir = std::getenv("VCS_DATA_DIR"); dir && *dir)
            return fs::path(dir);
        return appDirectory() / "userdata"; // the card and the cache live with the app
    }

    fs::path isoFromIni(const fs::path &ini)
    {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.rfind("iso=", 0) == 0)
            {
                std::string value = line.substr(4);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '"'))
                    value.pop_back();
                if (!value.empty() && value.front() == '"')
                    value.erase(0, 1);
                return fs::u8path(value);
            }
        }
        return {};
    }

    void setEnv(const char *name, const std::string &value) { setenv(name, value.c_str(), 1); }

    // Display aspect limits: narrower than 4:3 would cut the original view (Hor+ only widens), wider than 32:9 is not
    // checked (the horizontal view reaches 124 degrees at 32:9 with the game's 70-degree FOV).
    constexpr float kNarrowestAspect = 4.0f / 3.0f, kWidestAspect = 32.0f / 9.0f;
    float g_appliedAspect = 0.0f;

    // The TV mode's shape for "Match the TV".
    bool outputAspect(float &aspect)
    {
        int w = 0, h = 0;
        platform::outputSize(w, h);
        if (w <= 0 || h <= 0)
            return false;
        aspect = std::clamp(static_cast<float>(w) / static_cast<float>(h), kNarrowestAspect, kWidestAspect);
        return true;
    }

    // display.aspect: 43, 1610, 169, 219, 329, or 0 = the TV's shape
    float aspectFor(int code)
    {
        switch (code)
        {
        case 1610:
            return 16.0f / 10.0f;
        case 169:
            return 16.0f / 9.0f;
        case 219:
            return 21.0f / 9.0f;
        case 329:
            return 32.0f / 9.0f;
        case 0:
        {
            float a = g_appliedAspect > 0.0f ? g_appliedAspect : 4.0f / 3.0f;
            outputAspect(a);
            return a;
        }
        default:
            return 4.0f / 3.0f;
        }
    }

    // Display aspect: the presenter's ratio, the game camera's Hor+ widening and the HUD squeeze (game/vcs_display.h).
    void applyAspect()
    {
        const float aspect = aspectFor(settings::value("display.aspect"));
        g_appliedAspect = aspect;
        present::setDisplayAspect(aspect);
        vcs::setAspectWidening(aspect / (4.0f / 3.0f));
        gsHwSetAspectWidening(aspect / (4.0f / 3.0f)); // post effects keep their 4:3 proportions
        const int hud = settings::value("display.hud");
        gsHwSetHudSqueeze(hud != 1 ? (4.0f / 3.0f) / aspect : 1.0f);
        vcs::setHudAnchoring(hud == 2); // per-element placement (game/vcs_hud.h)
        std::fprintf(stderr, "[settings] display aspect %.3f, HUD %s\n", aspect, hud == 0 ? "4:3" : hud == 2 ? "screen edges" : "stretched");
    }

    // Each host frame: "Match the TV" follows the TV mode (known once the picture's surface is made).
    void followOutputAspect()
    {
        float aspect = 0.0f;
        if (settings::value("display.aspect") == 0 && outputAspect(aspect) && std::fabs(aspect - g_appliedAspect) > 1e-3f)
            applyAspect();
    }

    // Display / Graphics presets: the settings each one sets (the rest stay as they are). Authentic is their defaults,
    // except the frame rate: the default is 60 fps with frame-rate independent timing (DECISIONS.md 2026-10-30), the
    // PS2's is 30 with its own timing.
    struct Preset
    {
        const char *key;
        int value;
    };
    const Preset kEnhanced[] = {{"display.aspect", 0},   {"display.hud", 2},    {"display.scale", 0},
                                {"display.msaa", 4},     {"display.aniso", 16}, {"display.timing", 1},
                                {"display.fps", vcs::kCapRefresh}, {"display.vsync", 0}};
    const Preset kLight[] = {{"display.aspect", 0}, {"display.hud", 2},    {"display.scale", 1},  {"display.msaa", 1},
                             {"display.aniso", 4},  {"display.timing", 1}, {"display.fps", 60}, {"display.vsync", 0}};
    const char *const kPresetKeys[] = {"display.aspect", "display.hud", "display.scale", "display.msaa",
                                       "display.aniso",  "display.timing", "display.fps", "display.vsync"};

    template <size_t N> void applyPreset(const char *name, const Preset (&values)[N])
    {
        for (const Preset &p : values)
            settings::set(p.key, p.value);
        std::fprintf(stderr, "[settings] preset %s\n", name);
    }

    const Preset kAuthenticTiming[] = {{"display.timing", 0}, {"display.fps", 30}};

    void applyAuthentic()
    {
        for (const char *key : kPresetKeys)
            for (const settings::Spec &s : settings::all())
                if (s.key == key)
                    settings::set(s.key, s.defaultValue);
        for (const Preset &p : kAuthenticTiming)
            settings::set(p.key, p.value);
        std::fprintf(stderr, "[settings] preset Authentic\n");
    }

    // Each host frame: the frame-rate counter (Display > Show frame rate) - game frames per second, measured over half
    // a second, in the corner the setting names.
    void drawFrameRateCounter()
    {
        const int corner = settings::value("display.fps_counter");
        if (corner == 0 || ps2control::hiddenMode())
            return;
        static auto since = std::chrono::steady_clock::now();
        static uint64_t framesThen = vcs::rig::gameFrames();
        static float fps = 0.0f;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - since).count();
        if (seconds >= 0.5)
        {
            const uint64_t frames = vcs::rig::gameFrames();
            fps = static_cast<float>((frames - framesThen) / seconds);
            framesThen = frames;
            since = now;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f fps", fps);
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float size = std::max(16.0f, vp->Size.y / 40.0f);
        ImFont *font = ImGui::GetFont();
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const float margin = size * 0.6f;
        const float x = corner == 1 || corner == 3 ? vp->Pos.x + margin : vp->Pos.x + vp->Size.x - margin - extent.x;
        const float y = corner == 1 || corner == 2 ? vp->Pos.y + margin : vp->Pos.y + vp->Size.y - margin - extent.y;
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        draw->AddText(font, size, ImVec2(x + 1.5f, y + 1.5f), IM_COL32(0, 0, 0, 200), text);
        draw->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 160, 255), text);
    }

    // The player settings (settings/settings.h). The menu shows pages (tabs) in the order they first appear here, each
    // page's sections likewise, and the rows of a section in this order. vcs.ini in the app's folder holds them along
    // with iso=; a setting's vcs.ini key stays the same when it moves to another page.
    void registerSettings()
    {
        using settings::Apply;
        using settings::Type;
        settings::Spec s;

        s = {};
        s.key = "presets.authentic";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Authentic (PS2)";
        s.help = "The PS2's picture and timing: 4:3, original resolution, no anti-aliasing, 30 fps. Sets the Display and "
                 "Graphics pages; controls, sound and the TV output stay as they are.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyAuthentic(); };
        settings::add(s);

        s = {};
        s.key = "presets.enhanced";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Enhanced";
        s.help = "Widescreen in the TV's shape with the HUD at the screen edges, rendered at the TV's resolution, 4x "
                 "anti-aliasing, 16x texture filtering and frame-rate independent timing at the TV's refresh rate. "
                 "Resolution, anti-aliasing and timing apply after a restart.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyPreset("Enhanced", kEnhanced); };
        settings::add(s);

        s = {};
        s.key = "presets.light";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Widescreen, light";
        s.help = "Widescreen at 60 fps with the most frame time to spare: the original resolution, no anti-aliasing, 4x "
                 "texture filtering. Resolution and timing apply after a restart.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyPreset("Widescreen, light", kLight); };
        settings::add(s);

        // The TV's mode (src/platform/platform_ps5.cpp): read when the picture starts
        s = {};
        s.key = "display.ps5_output";
        s.page = "Display";
        s.section = "Screen";
        s.label = "TV output";
        s.help = "The resolution the PS5 sends to the TV. Automatic is 4K when the TV takes it; 1080p suits 1080p TVs that "
                 "also report 4K. The game's own picture is scaled to it (Graphics > Internal resolution).";
        s.type = Type::Choice;
        s.choices = {{"Automatic (4K when the TV has it)", 0}, {"1080p", 1080}, {"1440p", 1440}, {"4K (2160p)", 2160}};
        s.defaultValue = 0;
        s.apply = Apply::Restart;
        s.env = "VCS_PS5_OUTPUT";
        s.onChange = [](int v) { setEnv("VCS_PS5_OUTPUT", std::to_string(v)); };
        settings::add(s);
        s = {};
        s.key = "display.aspect";
        s.page = "Display";
        s.section = "Screen";
        s.label = "Aspect ratio";
        s.help = "Widescreen shows more to the sides (Hor+); 4:3 is the original picture. Match the TV takes the TV "
                 "mode's shape (16:9). The wider ratios show the picture in a strip between bars.";
        s.type = Type::Choice;
        s.choices = {{"4:3 (original)", 43}, {"16:10", 1610}, {"16:9", 169}, {"21:9", 219}, {"32:9", 329}, {"Match the TV", 0}};
        s.defaultValue = 43;
        s.env = "VCS_ASPECT";
        s.onChange = [](int) { applyAspect(); };
        settings::add(s);

        s = {};
        s.key = "display.hud";
        s.page = "Display";
        s.section = "Screen";
        s.label = "HUD";
        s.help = "In widescreen: keep the HUD, text and menus at 4:3 in the middle, move each HUD element to the screen "
                 "edge it is near (keeping its 4:3 shape), or stretch them to the screen.";
        s.type = Type::Choice;
        s.choices = {{"Keep 4:3", 0}, {"Screen edges", 2}, {"Stretch", 1}};
        s.env = "VCS_HUD";
        s.defaultValue = 0;
        s.onChange = [](int) { applyAspect(); };
        settings::add(s);

        s = {};
        s.key = "display.scale";
        s.alias = "scale";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Internal resolution";
        s.help = "Renders the game at a multiple of 640x448 (in widescreen as many times wider as the view: 1x at 16:9 "
                 "is 853x448), or at the TV output's own resolution (sharpest; the effects keep the original's look). "
                 "Costs GPU time.";
        s.type = Type::Choice;
        s.choices = {{"1x (original)", 1}, {"2x", 2}, {"3x", 3}, {"4x", 4}, {"6x", 6}, {"8x", 8}, {"Match the TV", 0}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_SCALE";
        s.onChange = [](int v) { setEnv("PS2X_HW_SCALE", std::to_string(v)); }; // read when the renderer starts
        settings::add(s);

        auto applyVolume = [](int) {
            auto gain = [](int percent) { return (percent / 100.0f) * (percent / 100.0f); }; // even steps to the ear
            ps2AudioSetVolume(gain(settings::value("audio.volume")), gain(settings::value("audio.movie_volume")));
        };
        s = {};
        s.key = "audio.volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Master volume";
        s.help = "Everything the game plays, movies included.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = applyVolume;
        settings::add(s);

        vcs::volume::registerSettings(); // music / sound effects: the game's own levels (game/vcs_volume.h)

        s = {};
        s.key = "audio.movie_volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Movie volume";
        s.help = "The intro movie and the other full-motion videos, relative to the master volume.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = applyVolume;
        settings::add(s);

        s = {};
        s.key = "audio.output";
        s.page = "Audio";
        s.section = "Output";
        s.label = "Speakers";
        s.help = "Mono mixes left and right together (one speaker or ear); Swapped exchanges them (headphones worn the other "
                 "way round).";
        s.type = Type::Choice;
        s.choices = {{"Stereo", 0}, {"Mono", 1}, {"Stereo, swapped", 2}};
        s.defaultValue = 0;
        s.onChange = [](int v) { ps2AudioSetOutputMode(v); };
        settings::add(s);

        s = {};
        s.key = "display.timing";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Game timing";
        s.help = "Frame-rate independent (the default): paced by the console's clock to the frame rate cap, game time "
                 "follows real time. Original: the game paces itself on the PS2's 60 Hz VBlank (30 fps, or 60 with a cap "
                 "of 60 or more) and slows down when the console can't keep up.";
        s.type = Type::Choice;
        s.choices = {{"Original (PS2)", 0}, {"Frame-rate independent", 1}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "VCS_TIMING";
        s.onChange = [](int v) { vcs::setTiming(v == 1 ? vcs::Timing::Independent : vcs::Timing::Original); };
        settings::add(s);

        s = {};
        s.key = "display.fps";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Frame rate cap";
        s.help = "60 is the default, 30 the original. With original timing anything above 30 runs at 60 (the VBlank "
                 "allows no more); with frame-rate independent timing the game runs up to the cap - at most 240 (above "
                 "that the game's shortest time step makes it run slightly fast). A 60 Hz TV shows at most 60.";
        s.type = Type::Choice;
        s.choices = {{"30 (original)", 30},
                     {"60", 60},
                     {"100", 100},
                     {"120", 120},
                     {"144", 144},
                     {"165", 165},
                     {"240", 240},
                     {"TV refresh (up to 240)", vcs::kCapRefresh},
                     {"Unlimited (up to 240)", vcs::kCapUnlimited}};
        s.defaultValue = 60;
        s.env = "VCS_FPS";
        s.onChange = [](int v) { vcs::setFrameCap(v); };
        settings::add(s);

        s = {};
        s.key = "display.vsync";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Vertical sync";
        s.help = "Off: each frame is shown as soon as it is ready, never torn. On: frames wait for the TV's refresh "
                 "(smoothest with a cap of TV refresh, at a little more input delay). Off, allow tearing: the lowest "
                 "delay, frames shown mid-refresh.";
        s.type = Type::Choice;
        s.choices = {{"Off", 0}, {"On", 1}, {"Off, allow tearing", 2}};
        s.defaultValue = 0;
        s.env = "VCS_VSYNC";
        s.onChange = [](int v) { present::setPresentMode(v); };
        settings::add(s);

        s = {};
        s.key = "display.fps_counter";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Show frame rate";
        s.help = "The game's frames per second in a corner of the screen (Advanced > Performance overlay shows more).";
        s.type = Type::Choice;
        s.choices = {{"Off", 0}, {"Top left", 1}, {"Top right", 2}, {"Bottom left", 3}, {"Bottom right", 4}};
        s.defaultValue = 0;
        settings::add(s);

        s = {};
        s.key = "display.skip_intro";
        s.page = "Game";
        s.section = "Start-up";
        s.label = "Skip intro movies";
        s.help = "The Rockstar logos and the intro movie at start-up. Off plays them as on the PS2 (Options or Cross skips "
                 "the intro, not the logos); on goes straight to the game.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_SKIP_MOVIES";
        s.onChange = [](int v) { vcs::setSkipIntroMovies(v != 0); };
        settings::add(s);

        s = {};
        s.key = "display.msaa";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Anti-aliasing (MSAA)";
        s.help = "Smooths the edges of the 3D picture. Off is the original. Costs GPU time.";
        s.type = Type::Choice;
        s.choices = {{"Off (original)", 1}, {"2x", 2}, {"4x", 4}, {"8x", 8}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_MSAA";
        s.onChange = [](int v) { setEnv("PS2X_HW_MSAA", std::to_string(v)); };
        settings::add(s);

        s = {};
        s.key = "display.aniso";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Texture filtering";
        s.help = "Anisotropic filtering keeps textures sharp at a slant (roads, walls into the distance). Off is the "
                 "original bilinear filtering.";
        s.type = Type::Choice;
        s.choices = {{"Bilinear (original)", 0}, {"Anisotropic 2x", 2}, {"Anisotropic 4x", 4}, {"Anisotropic 8x", 8},
                     {"Anisotropic 16x", 16}};
        s.defaultValue = 0;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_ANISO";
        s.onChange = [](int v) { setEnv("PS2X_HW_ANISO", std::to_string(v)); };
        settings::add(s);

        vcs::input::registerSettings(); // the Controller page (game/vcs_input.h)

        s = {};
        s.key = "graphics.perf_overlay";
        s.page = "Advanced";
        s.section = "Diagnostics";
        s.label = "Performance overlay";
        s.help = "Frame times, the frame-time graph and the renderer's counters in the corner.";
        s.type = Type::Toggle;
        s.env = "VCS_PERF_OVERLAY";
        s.onChange = [](int v) { ps2perf::setOverlay(v != 0); };
        s.read = [] { return ps2perf::overlayOn() ? 1 : 0; };
        settings::add(s);

        s = {};
        s.key = "graphics.perf_record";
        s.page = "Advanced";
        s.section = "Diagnostics";
        s.label = "Record performance";
        s.help = "Starts or stops writing the overlay's numbers for every frame to logs/perf_<date>_<time>.csv in the app's "
                 "folder (tools/perf_report.py summarises it). The overlay shows REC while it runs.";
        s.type = Type::Action;
        s.button = "Start / stop";
        s.onChange = [](int) { ps2perf::setRecording(!ps2perf::recording(), (appDirectory() / "logs").string()); };
        settings::add(s);

        s = {};
        s.key = "advanced.reset_all";
        s.page = "Advanced";
        s.section = "Reset";
        s.label = "Reset all settings";
        s.help = "Every setting on every page back to its default (the PS2's picture at 60 fps with frame-rate independent "
                 "timing, the default controls). The disc path is kept.";
        s.type = Type::Action;
        s.button = "Reset everything";
        s.confirm = true;
        s.onChange = [](int) {
            for (const settings::Spec &each : settings::all())
                if (each.type != Type::Action)
                    settings::set(each.key, each.defaultValue);
            std::fprintf(stderr, "[settings] all reset to defaults\n");
        };
        settings::add(s);
    }

    fs::path firstIsoIn(const fs::path &dir)
    {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec))
        {
            std::string ext = entry.path().extension().string();
            for (char &c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (entry.is_regular_file() && ext == ".iso")
                return entry.path();
        }
        return {};
    }

    // The first "*Vice City Stories*.iso" in `dir` (any case), for folders that hold other games too (USB drives).
    fs::path namedIsoIn(const fs::path &dir)
    {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec))
        {
            std::string name = entry.path().filename().string();
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find("vice city stories") != std::string::npos && name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".iso") == 0 && entry.is_regular_file(ec))
                return entry.path();
        }
        return {};
    }

    fs::path locateIso(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0])
            return fs::u8path(argv[1]);
        if (fs::path p = isoFromIni(appDirectory() / "vcs.ini"); !p.empty())
            return p;
        if (fs::path p = firstIsoIn(appDirectory() / "disc"); !p.empty())
            return p;
        if (fs::path p = firstIsoIn(appDirectory()); !p.empty())
            return p;
        for (int usb = 0; usb < 8; ++usb) // USB drives (exFAT / FAT32), the top folder and a games/ folder
            for (const char *sub : {"", "games"})
                if (fs::path p = namedIsoIn(fs::path("/mnt/usb" + std::to_string(usb)) / sub); !p.empty())
                    return p;
        return {};
    }

    // No console to print to: stdout and stderr go to /app0/logs/vcs.log (the run before kept as vcs.prev.log),
    // line by line so a crash keeps the end.
    void ps5Log()
    {
        const fs::path dir = appDirectory() / "logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::rename(dir / "vcs.log", dir / "vcs.prev.log", ec);
        const std::string log = (dir / "vcs.log").string();
        if (FILE *f = std::fopen(log.c_str(), "w")) // a new, empty log; both streams then append to it (one offset)
            std::fclose(f);
        if (std::freopen(log.c_str(), "a", stderr))
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
        if (std::freopen(log.c_str(), "a", stdout))
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    // The app's start-up code sets the IEEE floating-point state (MXCSR 0x1f80, the x86-64 default) before main; the
    // console's own is flush-to-zero with denormals-are-zero (0x9fe0), which the game's numerics were never checked
    // against.
    void ps5FloatingPoint()
    {
        const unsigned csr = _mm_getcsr();
        if ((csr & ~0x3Fu) != 0x1F80u) // (the low six bits are sticky exception flags)
        {
            std::fprintf(stderr, "[main] MXCSR 0x%04x at start: set to 0x1f80 (IEEE, the game's)\n", csr);
            _mm_setcsr(0x1F80u);
        }
    }

    // A start-up failure, in the log and for the player (the system's notification).
    void reportStartFailure(const std::string &message)
    {
        std::cerr << message << std::endl;
        platform::notify(("Vice City Stories - " + message).c_str());
    }

    // Writes the guest ELF from the disc into the cache folder (runtime loads it from a file). 0, or the exit code for
    // the failure (vcs_disc_id.h) with `error` set.
    int stageElf(const vcs::IsoImage &iso, const fs::path &out, std::string &error)
    {
        vcs::IsoEntry entry;
        if (!iso.find(kElfName, entry))
        {
            error = std::string(kElfName) + " not found: this is not GTA: Vice City Stories (USA)";
            return vcs::disc::kExitWrongDisc;
        }
        std::vector<uint8_t> elf(entry.size);
        iso.readBytes(uint64_t(entry.lba) * vcs::IsoImage::kSectorSize, elf.size(), elf.data());
        const std::string sha1 = vcs::sha1Hex(elf.data(), elf.size());
        if (sha1 != kElfSha1)
        {
            error = "unsupported game build (executable sha1 " + sha1 + ")";
            return vcs::disc::kExitWrongDisc;
        }
        std::error_code ec;
        if (fs::exists(out, ec) && fs::file_size(out, ec) == elf.size())
            return 0;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        if (!file.good())
        {
            error = "cannot write " + out.string();
            return vcs::disc::kExitError;
        }
        return 0;
    }
}

int main(int argc, char *argv[])
{
    ps5Log();
    ps5FloatingPoint();
    vcs::installCrashHandlers();
    try
    {
        // the hardware (Vulkan) renderer unless PS2X_GS says otherwise: the runtime's own default is the software GS
        // (~5 fps)
        if (const char *gs = std::getenv("PS2X_GS"); !gs || !*gs)
            setEnv("PS2X_GS", "hw");
        registerSettings();
        settings::load(appDirectory() / "vcs.ini");
        if (ps2control::hiddenMode())
            settings::keepFile(); // a test run never changes the player's settings
        settings::applyAll();
        const fs::path isoPath = locateIso(argc, argv);
        if (isoPath.empty())
        {
            reportStartFailure("No disc image found. Copy your Vice City Stories (USA) .iso into the app's disc folder "
                               "(/data/homebrew/<title id>/disc/) or onto a USB drive.");
            return vcs::disc::kExitNoDisc;
        }

        vcs::IsoImage iso;
        if (!iso.open(isoPath))
        {
            reportStartFailure("Cannot read disc image " + isoPath.string() + ": " + iso.error());
            return vcs::disc::kExitDiscUnreadable;
        }

        const fs::path data = dataDirectory();
        const fs::path elfPath = data / "cache" / kElfName;
        std::string error;
        if (const int code = stageElf(iso, elfPath, error))
        {
            reportStartFailure(error);
            return code;
        }

        MappedDisc disc(iso);
        ps2_disc::setImage(&disc);

        // the trainer's mission list names each mission from MAIN.SCM (game/vcs_trainer.h)
        if (vcs::IsoEntry scm; iso.find("MAIN.SCM", scm) && !scm.isDirectory)
        {
            std::vector<uint8_t> bytes((scm.size + vcs::IsoImage::kSectorSize - 1) / vcs::IsoImage::kSectorSize *
                                       vcs::IsoImage::kSectorSize);
            if (iso.readSectors(scm.lba, static_cast<uint32_t>(bytes.size() / vcs::IsoImage::kSectorSize), bytes.data()))
            {
                bytes.resize(scm.size);
                vcs::trainer::setMissionScript(bytes);
            }
        }

        // the card in our data folder from the start: the runtime's default (the working directory) left empty mc0 / mc1
        // folders there, made before loadELF below set the paths
        {
            PS2Runtime::IoPaths early = PS2Runtime::getIoPaths();
            early.mcRoot = data / "mc0";
            PS2Runtime::setIoPaths(early);
        }

        PS2Runtime runtime;
        // the settings menu draws in the runtime's UI hook (registered before initialize(), which runs the init hook)
        runtime.setDebugUiCallbacks([](PS2Runtime &, void *) {},
                                    [](PS2Runtime &, void *) {
                                        followOutputAspect();
                                        ui::drawSettingsMenu();
                                        ui::drawTrainerMenu();
                                        ps2control::setGameInputBlocked(ui::settingsMenuBlocksInput() || ui::trainerMenuBlocksInput());
                                        vcs::volume::onHostFrame();
                                        drawFrameRateCounter();
                                    },
                                    nullptr, nullptr);
        if (!runtime.initialize("GTA: Vice City Stories"))
        {
            reportStartFailure("Failed to initialize runtime");
            return vcs::disc::kExitInitFailed;
        }
        if (!runtime.loadELF(elfPath.string()))
        {
            std::cerr << "Failed to load " << elfPath.string() << std::endl;
            return 1;
        }

        // loadELF points every root at the cache folder; keep saves in our data folder
        // and route cdrom0: through the mapped disc (cdImage is used by the IOP fallback).
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = isoPath;
        paths.mcRoot = data / "mc0";
        PS2Runtime::setIoPaths(paths);

        vcs::rig::install(runtime); // control-channel automation commands (debug/rig.h)
        vcs::trainer::install(runtime); // the trainer's control-channel command (game/vcs_trainer.h)
        vcs::input::install(runtime.memory().getRDRAM()); // the player's bindings feed the pad (game/vcs_input.h)
        vcs::volume::install(runtime);  // the game's music / effects levels follow the settings (game/vcs_volume.h)
        vcs::gameinfo::install(runtime); // the overlay's game-state panel, the `gameinfo` command (debug/game_info.h)
        // setting <key> [value] -> ok <value>: a setting read or changed (a test run never writes vcs.ini)
        ps2control::addCommand("setting", [](const std::string &args, ps2control::Reply reply) {
            const size_t space = args.find(' ');
            const std::string key = args.substr(0, space);
            const auto spec = std::find_if(settings::all().begin(), settings::all().end(), [&](const settings::Spec &s) { return s.key == key; });
            if (spec == settings::all().end())
                return reply("error unknown setting " + key);
            if (spec->type == settings::Type::Action) // a button: pressed (presets, reset)
            {
                spec->onChange(0);
                return reply("ok pressed");
            }
            if (space != std::string::npos && !settings::set(key, std::atoi(args.c_str() + space + 1)))
                return reply("error refused");
            reply("ok " + std::to_string(settings::value(key)));
        });
        // menu <page>|close|scroll <0..1> -> ok: the settings menu opened on a page (screenshots of it in test runs)
        ps2control::addCommand("menu", [](const std::string &args, ps2control::Reply reply) {
            if (args == "close")
                ui::closeSettingsMenu();
            else if (args.rfind("scroll ", 0) == 0)
                ui::scrollSettingsMenu(static_cast<float>(std::atof(args.c_str() + 7)));
            else
                ui::openSettingsMenu(args);
            reply("ok");
        });
        vcs::configureWatch(runtime.memory().getRDRAM());
        vcs::runSelfTest(runtime);
        vcs::startWatchdog(runtime);
        runtime.run();
#if defined(PS2X_PGO_GEN)
        __llvm_profile_write_file(); // instrumented build: _Exit skips the atexit handler that writes the profile
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
