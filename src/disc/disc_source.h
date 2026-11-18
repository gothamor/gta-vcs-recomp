// Where a disc image's 2048-byte sectors come from (docs/DISC.md "CHD images"):
//   - an .iso, memory-mapped once (reads are memcpy; the game's streaming costs page faults only);
//   - a .chd (MAME's compressed image, both kinds: DVD CHDs and CD-mode ones made by chdman createcd), through
//     libchdr with a cache of decompressed hunks. Only in the game (VCS_DISC_CHD); the launcher reads .iso only.
// IsoImage reads through one of these; nothing else opens the disc file (main.cpp hands the runtime an
// IPS2DiscImage, ps2_disc_image.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace vcs
{
    class DiscSource
    {
    public:
        static constexpr uint32_t kSectorSize = 2048;

        virtual ~DiscSource() = default;

        // Bytes of sector data (sectors * 2048).
        virtual uint64_t size() const = 0;

        // Copies [offset, offset + byteCount) of the sector data. Returns false (dst unspecified) on a read or
        // decode error, or when the range runs past size(). Safe to call from several threads.
        virtual bool read(uint64_t offset, size_t byteCount, void *dst) const = 0;

        // The whole image in memory when it is mapped, else null.
        virtual const uint8_t *data() const { return nullptr; }

        // The file holds the sectors one after another as they are (an .iso, not a .chd), so it can be read raw.
        virtual bool plainSectors() const { return false; }

        // "iso", "chd (DVD)", "chd (CD)": for the log.
        virtual const char *kind() const = 0;
    };

    // Opens an .iso, or a .chd when this build reads them (by the file's first bytes, not its name).
    // Null with `error` set on failure.
    std::unique_ptr<DiscSource> openDiscSource(const std::filesystem::path &path, std::string &error);

    // A path as UTF-8 text, for messages (path::string() can throw for a name outside the code page)
    std::string pathText(const std::filesystem::path &path);
}
