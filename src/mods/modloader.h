// The mod loader (docs/MODS.md "The mod loader"): mods are folders in modloader/, as with PC GTA's Mod Loader - nothing
// of the game's is changed on disc, mods are read while the game runs, and each can be switched off in F2 > Mods.
// What it reads from a mod so far: texture packs (PCSX2-named .dds / .png, render/texture_replacements.h).
#pragma once

#include <filesystem>

namespace vcs::mods
{
    // Before settings::load: finds modloader/ (VCS_MODLOADER_DIR; else beside vcs.exe, or the checkout's root when vcs.exe
    // is in build/), lists its mods (modloader.ini: Mod Loader's priorities and ignore lists) and registers the Mods page.
    void registerSettings(const std::filesystem::path &exeDir);
    // Main thread, every host frame: applies what changed since the last one (one re-index for any number of switches).
    void onHostFrame();
}
