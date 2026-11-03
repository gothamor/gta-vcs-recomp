// The mod loader - see mods/modloader.h and docs/MODS.md "The mod loader".
//
// A mod is a folder in modloader/ (any name; a leading '.' hides it). modloader/modloader.ini is read in Mod Loader's
// own format, so its knobs mean the same here: [Folder.Config] Profile, and the profile's [Profiles.<P>.Config]
// IgnoreAllMods, [Profiles.<P>.Priority] Mod=1..100 (50 by default, 0 ignores it; a higher one wins a file two mods
// have) and [Profiles.<P>.IgnoreMods] (wildcards). F2 > Mods switches each mod, all of them, or their textures; a
// change is applied at the next host frame, while the game runs (gsHwSetTextureReplacements).
//
// Test runs (hidden) stay authentic: they load mods only with VCS_MODS=1, and the texture packs outside modloader/
// (VCS_TEXTURE_PACK, textures/SLUS-21590) only with VCS_TEXTURE_REPLACEMENTS=1 - or both with VCS_TEST_MODS=1, which
// leaves the switches free for the `setting` command (an environment value fixes its setting).
#include "mods/modloader.h"

#include "platform/platform.h"
#include "render/texture_replacements.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vcs::mods
{
    namespace
    {
        struct Mod
        {
            std::string name;    // the folder's
            std::string key;     // its switch in vcs.ini: mods.<the name in lower case, other characters as _>
            fs::path path;
            int priority = 50;   // modloader.ini
            size_t textures = 0; // .dds / .png files in it at start (dumps/ aside)
        };

        fs::path g_root, g_exeDir;
        std::vector<Mod> g_mods;
        bool g_dirty = true;

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        std::string trim(const std::string &s)
        {
            const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        std::string utf8(const fs::path &p)
        {
            const auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        // Mod Loader's wildcards: * and ?, case-insensitive
        bool matches(const char *pattern, const char *text)
        {
            for (; *pattern; ++pattern, ++text)
            {
                if (*pattern == '*')
                {
                    for (const char *t = text;; ++t)
                    {
                        if (matches(pattern + 1, t))
                            return true;
                        if (!*t)
                            return false;
                    }
                }
                if (!*text || (*pattern != '?' && std::tolower(static_cast<unsigned char>(*pattern)) !=
                                                       std::tolower(static_cast<unsigned char>(*text))))
                    return false;
            }
            return !*text;
        }

        struct Config
        {
            std::string profile = "default";
            std::map<std::string, int> priority; // lower-case mod name -> 0..100
            std::vector<std::string> ignore;     // wildcards
            bool ignoreAll = false;
        };

        Config readConfig(const fs::path &ini)
        {
            Config c;
            std::ifstream in(ini);
            std::vector<std::pair<std::string, std::string>> lines; // (section, line)
            std::string section, line;
            while (std::getline(in, line))
            {
                if (const size_t comment = line.find(';'); comment != std::string::npos)
                    line.erase(comment);
                line = trim(line);
                if (line.empty())
                    continue;
                if (line.front() == '[' && line.back() == ']')
                    section = lower(trim(line.substr(1, line.size() - 2)));
                else
                    lines.emplace_back(section, line);
            }
            auto field = [](const std::string &l, bool value) {
                const size_t eq = l.find('=');
                return eq == std::string::npos ? (value ? std::string() : trim(l)) : trim(value ? l.substr(eq + 1) : l.substr(0, eq));
            };
            for (const auto &[s, l] : lines)
                if (s == "folder.config" && lower(field(l, false)) == "profile")
                    c.profile = lower(field(l, true));
            const std::string p = "profiles." + c.profile + ".";
            for (const auto &[s, l] : lines)
            {
                if (s == p + "priority" && l.find('=') != std::string::npos)
                    c.priority[lower(field(l, false))] = std::atoi(field(l, true).c_str());
                else if (s == p + "ignoremods")
                    c.ignore.push_back(l);
                else if (s == p + "config" && lower(field(l, false)) == "ignoreallmods")
                    c.ignoreAll = lower(field(l, true)) == "true" || field(l, true) == "1";
            }
            return c;
        }

        size_t countTextures(const fs::path &folder)
        {
            size_t n = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                if (it->is_directory(ec) && it->path().filename() == "dumps")
                    it.disable_recursion_pending();
                else if (it->is_regular_file(ec))
                {
                    const std::string ext = lower(it->path().extension().string());
                    n += ext == ".dds" || ext == ".png";
                }
            }
            return n;
        }

        fs::path findRoot(const fs::path &exeDir)
        {
            if (const char *dir = std::getenv("VCS_MODLOADER_DIR"); dir && *dir)
                return fs::u8path(dir);
            std::error_code ec;
            for (const fs::path &candidate : {exeDir / "modloader", exeDir.parent_path() / "modloader"})
                if (fs::is_directory(candidate, ec))
                    return candidate;
            // none yet: where "Open" makes it - the checkout's root when vcs.exe is in its build/
            return fs::exists(exeDir.parent_path() / "tools" / "build.py", ec) ? exeDir.parent_path() / "modloader" : exeDir / "modloader";
        }

        std::string keyFor(const std::string &name)
        {
            std::string key = "mods.";
            for (const unsigned char c : name)
                key += std::isalnum(c) ? char(std::tolower(c)) : '_';
            return key;
        }

        void apply()
        {
            using Source = render::TextureReplacements::Source;
            // a test run: VCS_MODS / VCS_TEXTURE_REPLACEMENTS (which also fix the switch), or VCS_TEST_MODS=1 (switches free)
            const bool allowed = !ps2control::hiddenMode() || std::getenv("VCS_TEST_MODS");
            const bool textures = settings::value("graphics.texture_replacements") != 0 && (allowed || std::getenv("VCS_TEXTURE_REPLACEMENTS"));
            const bool mods = settings::value("mods.enabled") != 0 && (allowed || std::getenv("VCS_MODS"));
            std::vector<Source> sources;
            if (textures)
            {
                if (const char *pack = std::getenv("VCS_TEXTURE_PACK"); pack && *pack)
                    sources.push_back({fs::u8path(pack), 100, "VCS_TEXTURE_PACK"});
                if (fs::path ini = fs::u8path(settings::state("graphics.texture_pack")); !ini.empty())
                    sources.push_back({ini.is_relative() ? g_exeDir / ini : ini, 50, "graphics.texture_pack (vcs.ini)"});
                // a PCSX2 folder beside vcs.exe or in the checkout's root (upstream PR #8's places)
                for (const fs::path &candidate : {g_exeDir / "textures" / "SLUS-21590", g_exeDir.parent_path() / "textures" / "SLUS-21590",
                                                  g_exeDir / "SLUS-21590", g_exeDir.parent_path() / "SLUS-21590"})
                {
                    std::error_code ec;
                    if (fs::is_directory(candidate / "replacements", ec))
                    {
                        sources.push_back({candidate, 50, "textures/SLUS-21590"});
                        break;
                    }
                }
                if (mods)
                    for (const Mod &m : g_mods)
                        if (settings::value(m.key))
                            sources.push_back({m.path, m.priority, m.name});
            }
            const auto start = std::chrono::steady_clock::now();
            std::shared_ptr<render::TextureReplacements> pack;
            if (!sources.empty())
                pack = std::make_shared<render::TextureReplacements>(sources);
            gsHwSetTextureReplacements(pack);
            std::fprintf(stderr, "[mods] textures from %zu source(s)%s, indexed in %.0f ms\n", sources.size(),
                         !textures ? " (Replace textures off)" : (!mods ? " (Load mods off)" : ""),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }

    void registerSettings(const fs::path &exeDir)
    {
        g_exeDir = exeDir;
        g_root = findRoot(exeDir);
        const Config config = readConfig(g_root / "modloader.ini");
        std::vector<fs::path> folders;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(g_root, ec))
            if (entry.is_directory(ec))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end(), [](const fs::path &a, const fs::path &b) {
            return lower(utf8(a.filename())) < lower(utf8(b.filename()));
        });
        for (const fs::path &folder : folders)
        {
            const std::string name = utf8(folder.filename());
            if (name.empty() || name.front() == '.')
                continue;
            const auto ignored = std::find_if(config.ignore.begin(), config.ignore.end(),
                                              [&](const std::string &w) { return matches(w.c_str(), name.c_str()); });
            const auto set = config.priority.find(lower(name));
            const int priority = set == config.priority.end() ? 50 : std::min(set->second, 100);
            if (config.ignoreAll || ignored != config.ignore.end() || priority <= 0)
            {
                std::fprintf(stderr, "[mods] %s: ignored by modloader.ini\n", name.c_str());
                continue;
            }
            std::string key = keyFor(name);
            while (std::any_of(g_mods.begin(), g_mods.end(), [&](const Mod &m) { return m.key == key; }))
                key += '_';
            g_mods.push_back({name, key, folder, priority, countTextures(folder)});
        }
        std::fprintf(stderr, "[mods] %zu mod(s) in %s\n", g_mods.size(), utf8(g_root).c_str());

        using settings::Type;
        settings::Spec s;
        s.key = "mods.enabled";
        s.page = "Mods";
        s.section = "Mod loader";
        s.label = "Load mods";
        s.help = "Mods are the folders in " + utf8(g_root) + ", one folder a mod, as with PC GTA's Mod Loader: nothing of the "
                 "game's is changed. Off: the game as it is.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_MODS";
        s.onChange = [](int) { g_dirty = true; };
        settings::add(s);

        s = {};
        s.key = "graphics.texture_replacements";
        s.page = "Mods";
        s.section = "Mod loader";
        s.label = "Replace textures";
        s.help = "Texture packs - PCSX2 file names, DDS or PNG (docs/TEXTURE_PACKS.md) - from the mods, textures/SLUS-21590 and "
                 "graphics.texture_pack in vcs.ini. Off: the game's own textures, at once.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_TEXTURE_REPLACEMENTS";
        s.onChange = [](int) { g_dirty = true; };
        settings::add(s);

        for (const Mod &m : g_mods)
        {
            s = {};
            s.key = m.key;
            s.page = "Mods";
            s.section = "Installed mods";
            s.label = m.name;
            char what[160];
            if (m.textures)
                std::snprintf(what, sizeof(what), "%zu texture files (DDS / PNG), priority %d (modloader.ini). ", m.textures, m.priority);
            else
                std::snprintf(what, sizeof(what), "Nothing in it this version reads yet (texture packs so far). ");
            s.help = what + utf8(m.path);
            s.type = Type::Toggle;
            s.defaultValue = 1;
            s.onChange = [](int) { g_dirty = true; };
            settings::add(s);
        }

        s = {};
        s.key = "mods.rescan";
        s.page = "Mods";
        s.section = "Folder";
        s.label = "Read the mods again";
        s.help = "After adding or changing files in a mod. A new mod folder shows here after a restart.";
        s.type = Type::Action;
        s.button = "Rescan";
        s.onChange = [](int) { g_dirty = true; };
        settings::add(s);

        s = {};
        s.key = "mods.open";
        s.page = "Mods";
        s.section = "Folder";
        s.label = "Mods folder";
        s.help = "Opens " + utf8(g_root) + " (made if it isn't there): put each mod in a folder of its own inside it.";
        s.type = Type::Action;
        s.button = "Open";
        s.onChange = [](int) {
            std::error_code e;
            fs::create_directories(g_root, e);
            platform::openFolder(utf8(fs::absolute(g_root, e)));
        };
        settings::add(s);
    }

    void onHostFrame()
    {
        if (!g_dirty)
            return;
        g_dirty = false;
        apply();
    }
}
