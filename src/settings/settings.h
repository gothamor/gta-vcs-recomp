// Player settings (docs/SETTINGS_MENU_WIDESCREEN.md): a registry of typed settings, their values (vcs.ini next to
// vcs.exe), and change notifications. The settings menu (ui/settings_menu.h) is generated from the registry: a new
// setting is one add() plus its apply callback.
//
// Order at start-up: add() everything, load(ini), applyAll(). Values are ints (toggles, choices, integers, keys).
// Threads: add / load / applyAll / set from the main thread (menu); value() from any thread.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace settings
{
    enum class Type
    {
        Toggle,  // 0 / 1
        Choice,  // one of `choices` (their values)
        Integer, // min..max
        Key,     // a platform::Key (the menu offers the keys it knows)
    };

    enum class Apply
    {
        Live,    // onChange runs at once
        Restart, // takes effect at the next start
    };

    struct Choice
    {
        std::string label;
        int value = 0;
    };

    struct Spec
    {
        std::string key;     // vcs.ini key, e.g. "display.aspect"
        std::string alias;   // an older vcs.ini key, read when `key` is absent (e.g. "scale")
        std::string page;    // menu page (a tab), e.g. "Display"; independent of `key`, so settings can move pages
        std::string section; // heading inside the page, e.g. "Screen" (rows of one section are registered together)
        std::string label;   // "Aspect ratio"
        std::string help;    // one line, shown for the selected setting
        Type type = Type::Toggle;
        int defaultValue = 0;
        int min = 0, max = 1;          // Integer
        std::string format = "%d";     // Integer, as shown
        std::vector<Choice> choices;   // Choice
        Apply apply = Apply::Live;
        std::string env;               // an environment variable that overrides it (testing), shown as such
        std::function<void(int)> onChange; // the new value; also called once by applyAll()
        std::function<int()> read;     // optional: the live value, when something else changes it too (a hotkey)
    };

    void add(Spec spec);
    const std::vector<Spec> &all();
    std::vector<std::string> pages(); // in registration order

    int value(const std::string &key);         // the default for unknown keys / before load
    bool set(const std::string &key, int v);   // validates, saves vcs.ini, calls onChange (Live); false if refused
    bool overridden(const std::string &key);   // set by its environment variable at load (the menu shows it read-only)
    bool changedSinceStart(const std::string &key); // Restart settings: "applies after restart"

    void load(const std::filesystem::path &ini);
    void applyAll();
    void refresh(); // takes the live values of settings with `read` (the menu calls it when it opens)

    // Saved state that is not a setting (no menu entry, no validation): text values in vcs.ini, e.g. the window
    // placement (window.*). Main thread, after load().
    std::string state(const std::string &key); // "" when absent
    void setState(const std::vector<std::pair<std::string, std::string>> &values); // one rewrite of vcs.ini
}
