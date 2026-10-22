// GTA: Vice City Stories (PS2) native launcher.
//
// Finds the user's disc image, checks it holds the supported executable,
// maps it as cdrom0: and boots the recompiled game.
//
//   vcs.exe [path\to\Grand Theft Auto - Vice City Stories (USA).iso]
//
// Without an argument it uses "iso=" from vcs.ini next to the exe, then the
// first *.iso in the exe's folder or the current folder.
#include "ps2_runtime.h"
#include "runtime/ps2_disc_image.h"

#include "debug/crash.h"
#include "debug/profiler.h"
#include "debug/selftest.h"
#include "debug/watchdog.h"
#include "disc/iso_image.h"
#include "util/sha1.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace vcs
{
    void configureWatch(); // debug/watch.cpp
}

namespace
{
    constexpr const char *kElfName = "SLUS_215.90";
    constexpr const char *kElfSha1 = "d1832508caf0990c2291c0f255c96a889cce9f88";

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

    fs::path exeDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return fs::path(buffer).parent_path();
#endif
        return fs::current_path();
    }

    fs::path dataDirectory()
    {
        // VCS_DATA_DIR: another save / cache folder (scripted test runs start from an empty memory card)
        if (const char *dir = std::getenv("VCS_DATA_DIR"); dir && *dir)
            return fs::path(dir);
#if defined(_WIN32)
        if (const char *local = std::getenv("LOCALAPPDATA"))
            return fs::path(local) / "VCSRecomp";
#endif
        if (const char *home = std::getenv("HOME"))
            return fs::path(home) / ".local/share/VCSRecomp";
        return exeDirectory() / "userdata";
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

    fs::path locateIso(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0])
            return fs::u8path(argv[1]);
        if (fs::path p = isoFromIni(exeDirectory() / "vcs.ini"); !p.empty())
            return p;
        if (fs::path p = firstIsoIn(exeDirectory()); !p.empty())
            return p;
        return firstIsoIn(fs::current_path());
    }

    // Writes the guest ELF from the disc into the cache folder (runtime loads it from a file).
    bool stageElf(const vcs::IsoImage &iso, const fs::path &out, std::string &error)
    {
        vcs::IsoEntry entry;
        if (!iso.find(kElfName, entry))
        {
            error = std::string(kElfName) + " not found: this is not GTA: Vice City Stories (USA)";
            return false;
        }
        std::vector<uint8_t> elf(entry.size);
        iso.readBytes(uint64_t(entry.lba) * vcs::IsoImage::kSectorSize, elf.size(), elf.data());
        const std::string sha1 = vcs::sha1Hex(elf.data(), elf.size());
        if (sha1 != kElfSha1)
        {
            error = "unsupported game build (executable sha1 " + sha1 + ")";
            return false;
        }
        std::error_code ec;
        if (fs::exists(out, ec) && fs::file_size(out, ec) == elf.size())
            return true;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        return file.good();
    }
}

int main(int argc, char *argv[])
{
    vcs::installCrashHandlers();
    try
    {
        const fs::path isoPath = locateIso(argc, argv);
        if (isoPath.empty())
        {
            std::cerr << "No disc image found. Pass your Vice City Stories (USA) .iso as the first argument,\n"
                         "put iso=<path> in vcs.ini next to vcs.exe, or place the .iso next to vcs.exe."
                      << std::endl;
            return 1;
        }

        vcs::IsoImage iso;
        if (!iso.open(isoPath))
        {
            std::cerr << "Cannot read disc image " << isoPath.string() << ": " << iso.error() << std::endl;
            return 1;
        }

        const fs::path data = dataDirectory();
        const fs::path elfPath = data / "cache" / kElfName;
        std::string error;
        if (!stageElf(iso, elfPath, error))
        {
            std::cerr << error << std::endl;
            return 1;
        }

        MappedDisc disc(iso);
        ps2_disc::setImage(&disc);

        PS2Runtime runtime;
        if (!runtime.initialize("GTA: Vice City Stories"))
        {
            std::cerr << "Failed to initialize runtime" << std::endl;
            return 1;
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

        vcs::configureWatch();
        vcs::runSelfTest(runtime);
        vcs::startWatchdog(runtime);
        vcs::startProfiler(runtime);
        runtime.run();
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
