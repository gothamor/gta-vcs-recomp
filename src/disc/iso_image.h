// Read-only access to the user's PS2 disc image (ISO 9660, 2048-byte sectors).
// The sectors come from a DiscSource (disc_source.h): an .iso, memory-mapped once (reads are plain memcpy, so the
// game's sector streaming costs nothing beyond page faults), or in the game a .chd through libchdr.
#pragma once

#include "disc_source.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vcs
{
    struct IsoEntry
    {
        uint32_t lba = 0;
        uint32_t size = 0;
        bool isDirectory = false;
    };

    class IsoImage
    {
    public:
        static constexpr uint32_t kSectorSize = DiscSource::kSectorSize;

        IsoImage() = default;
        ~IsoImage();
        IsoImage(const IsoImage &) = delete;
        IsoImage &operator=(const IsoImage &) = delete;

        // Opens the image (.iso, or .chd where this build reads them) and indexes every file. Returns false with
        // error() set on failure.
        bool open(const std::filesystem::path &path);
        void close();
        bool isOpen() const { return m_source != nullptr; }
        // "iso", "chd (DVD)", "chd (CD)": for the log
        const char *kind() const { return m_source ? m_source->kind() : ""; }
        // The image file can be read raw at lba * 2048 (an .iso; DiscSource::plainSectors)
        bool plainSectors() const { return m_source && m_source->plainSectors(); }
        const std::string &error() const { return m_error; }

        // Path lookup is case-insensitive, accepts '/' or '\', an optional
        // "cdrom0:" prefix, and a trailing ";1" version suffix.
        bool find(std::string_view path, IsoEntry &out) const;

        uint32_t totalSectors() const { return static_cast<uint32_t>(m_size / kSectorSize); }

        // Copies sectors [lba, lba+count) into dst. Out-of-range sectors are zero-filled
        // and make the call return false.
        bool readSectors(uint32_t lba, uint32_t count, void *dst) const;

        // Copies byteCount bytes starting at a byte offset into the image.
        bool readBytes(uint64_t offset, size_t byteCount, void *dst) const;

        // Direct pointer into the mapping (valid while open); null when the image is not mapped (a .chd).
        const uint8_t *sectorPtr(uint32_t lba) const;

        const std::unordered_map<std::string, IsoEntry> &entries() const { return m_entries; }

        // Children of a directory in on-disc order (leaf name, entry). Null if not a directory.
        using Children = std::vector<std::pair<std::string, IsoEntry>>;
        const Children *children(std::string_view dirPath) const;

        static std::string normalizePath(std::string_view path);

    private:
        bool indexDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);
        bool adopt(std::unique_ptr<DiscSource> source); // open: the checks and the index

        std::unique_ptr<DiscSource> m_source;
        const uint8_t *m_data = nullptr; // m_source->data(): the mapping, or null
        uint64_t m_size = 0;
        std::string m_error;
        std::unordered_map<std::string, IsoEntry> m_entries; // key: normalizePath()
        std::unordered_map<std::string, Children> m_children; // key: normalizePath() of the directory, "" = root
    };
}
