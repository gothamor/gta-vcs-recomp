// See settings.h.
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace settings
{
    namespace
    {
        std::vector<Spec> g_specs;
        std::unordered_map<std::string, size_t> g_index;
        std::unordered_map<std::string, int> g_values, g_startValues;
        std::mutex g_mutex; // g_values (read from other threads)
        std::filesystem::path g_ini;
        std::unordered_set<std::string> g_overridden; // by their environment variable, at load()
        std::unordered_map<std::string, std::string> g_loaded; // every key = value of vcs.ini at load()
        std::map<std::string, std::string> g_state;            // setState() values, written by save()

        bool valid(const Spec &s, int v)
        {
            switch (s.type)
            {
            case Type::Toggle: return v == 0 || v == 1;
            case Type::Integer: return v >= s.min && v <= s.max;
            case Type::Choice:
                return std::any_of(s.choices.begin(), s.choices.end(), [&](const Choice &c) { return c.value == v; });
            case Type::Key: return v > 0 && v < 512;
            }
            return false;
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            return s.substr(i);
        }

        // Rewrites vcs.ini: our keys updated in place, missing ones appended, every other line kept as it was.
        void save()
        {
            if (g_ini.empty())
                return;
            std::vector<std::string> lines;
            {
                std::ifstream in(g_ini);
                std::string line;
                while (std::getline(in, line))
                    lines.push_back(line);
            }
            std::unordered_map<std::string, bool> written;
            std::lock_guard<std::mutex> lock(g_mutex);
            for (std::string &line : lines)
            {
                const std::string t = trim(line);
                const size_t eq = t.find('=');
                if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos)
                    continue;
                const std::string key = trim(t.substr(0, eq));
                if (g_overridden.count(key))
                    continue; // the environment's value is for this run only
                if (auto it = g_values.find(key); it != g_values.end() && g_index.count(key))
                {
                    line = key + "=" + std::to_string(it->second);
                    written[key] = true;
                }
                else if (auto st = g_state.find(key); st != g_state.end())
                {
                    line = key + "=" + st->second;
                    written[key] = true;
                }
            }
            for (const Spec &s : g_specs)
            {
                const int v = g_values[s.key];
                if (!written[s.key] && !g_overridden.count(s.key) && v != s.defaultValue)
                    lines.push_back(s.key + "=" + std::to_string(v));
            }
            for (const auto &[key, text] : g_state)
                if (!written[key])
                    lines.push_back(key + "=" + text);
            std::filesystem::path tmp = g_ini;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                for (const std::string &line : lines)
                    out << line << "\n";
                if (!out)
                    return;
            }
            std::error_code ec;
            std::filesystem::rename(tmp, g_ini, ec);
            if (ec)
                std::fprintf(stderr, "[settings] could not write %s\n", g_ini.string().c_str());
        }
    }

    void add(Spec spec)
    {
        g_index[spec.key] = g_specs.size();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_values[spec.key] = spec.defaultValue;
        }
        g_specs.push_back(std::move(spec));
    }

    const std::vector<Spec> &all() { return g_specs; }

    std::vector<std::string> pages()
    {
        std::vector<std::string> out;
        for (const Spec &s : g_specs)
            if (std::find(out.begin(), out.end(), s.page) == out.end())
                out.push_back(s.page);
        return out;
    }

    int value(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_values.find(key);
        return it != g_values.end() ? it->second : 0;
    }

    bool overridden(const std::string &key) { return g_overridden.count(key) != 0; }

    bool changedSinceStart(const std::string &key)
    {
        auto it = g_startValues.find(key);
        return it != g_startValues.end() && it->second != value(key);
    }

    bool set(const std::string &key, int v)
    {
        auto it = g_index.find(key);
        if (it == g_index.end() || overridden(key))
            return false;
        const Spec &s = g_specs[it->second];
        if (!valid(s, v))
            return false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_values[key] == v)
                return true;
            g_values[key] = v;
        }
        save();
        if (s.apply == Apply::Live && s.onChange)
            s.onChange(v);
        return true;
    }

    void load(const std::filesystem::path &ini)
    {
        g_ini = ini;
        std::unordered_map<std::string, std::string> found; // vcs.ini key -> value text
        {
            std::ifstream in(ini);
            std::string line;
            while (std::getline(in, line))
            {
                const std::string t = trim(line);
                const size_t eq = t.find('=');
                if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos)
                    continue;
                found[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
            }
        }
        auto take = [](const Spec &s, const std::string &text, const char *from) {
            const int v = std::atoi(text.c_str());
            if (!valid(s, v))
            {
                std::fprintf(stderr, "[settings] %s: ignoring %s=%s\n", from, s.key.c_str(), text.c_str());
                return;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_values[s.key] = v;
        };
        const std::string iniName = ini.string();
        for (const Spec &s : g_specs)
        {
            if (auto it = found.find(s.key); it != found.end())
                take(s, it->second, iniName.c_str());
            else if (auto old = found.find(s.alias); !s.alias.empty() && old != found.end())
                take(s, old->second, iniName.c_str());
            if (const char *e = s.env.empty() ? nullptr : std::getenv(s.env.c_str()); e && *e)
            {
                take(s, e, s.env.c_str());
                g_overridden.insert(s.key);
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_startValues = g_values;
        g_loaded = std::move(found);
    }

    std::string state(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_state.find(key); it != g_state.end())
            return it->second;
        auto it = g_loaded.find(key);
        return it != g_loaded.end() ? it->second : std::string();
    }

    void setState(const std::vector<std::pair<std::string, std::string>> &values)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[key, text] : values)
                g_state[key] = text;
        }
        save();
    }

    void applyAll()
    {
        for (const Spec &s : g_specs)
            if (s.onChange)
                s.onChange(value(s.key));
    }

    void refresh()
    {
        bool changed = false;
        for (const Spec &s : g_specs)
        {
            if (!s.read)
                continue;
            const int v = s.read();
            std::lock_guard<std::mutex> lock(g_mutex);
            if (valid(s, v) && g_values[s.key] != v)
            {
                g_values[s.key] = v;
                changed = true;
            }
        }
        if (changed)
            save();
    }
}
