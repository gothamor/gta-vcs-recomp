// Where the Vulkan loader is on macOS (docs/MACOS.md). Header only: the platform layer (SDL's Vulkan library hint) and
// the Vulkan RHI (volk) both need it, and neither links the other.
//
// macOS has no system Vulkan loader. The game uses the Khronos loader with MoltenVK behind it (Vulkan on Metal), from
// next to the game (a folder given to players) or from Homebrew (brew install vulkan-loader molten-vk). dlopen() does
// not look in Homebrew's lib folder on its own, so the full path is found here. Elsewhere this returns "" (the
// system's loader by name, as before).
#pragma once

#include <string>
#include <vector>

#if defined(__APPLE__)
#include <cstdlib>
#include <filesystem>
#include <mach-o/dyld.h>
#endif

namespace vcs
{
    inline std::string vulkanLoaderPath()
    {
#if defined(__APPLE__)
        namespace fs = std::filesystem;
        std::error_code ec;
        // VCS_VULKAN_LIBRARY: another loader (or MoltenVK's own libMoltenVK.dylib, which is a Vulkan ICD and a loader)
        if (const char *given = std::getenv("VCS_VULKAN_LIBRARY"); given && *given)
            return given;
        std::vector<fs::path> candidates;
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string self(size, '\0');
        if (size > 0 && _NSGetExecutablePath(self.data(), &size) == 0)
        {
            const fs::path dir = fs::path(self.c_str()).parent_path();
            candidates.push_back(dir / "libvulkan.1.dylib");
            candidates.push_back(dir / "../Frameworks/libvulkan.1.dylib"); // an .app bundle
        }
        if (const char *brew = std::getenv("HOMEBREW_PREFIX"); brew && *brew)
            candidates.push_back(fs::path(brew) / "lib/libvulkan.1.dylib");
        candidates.push_back("/opt/homebrew/lib/libvulkan.1.dylib"); // Homebrew on Apple silicon
        candidates.push_back("/usr/local/lib/libvulkan.1.dylib");    // Homebrew on Intel, the LunarG SDK
        for (const fs::path &p : candidates)
            if (fs::is_regular_file(p, ec))
                return p.string();
#endif
        return {};
    }
}
