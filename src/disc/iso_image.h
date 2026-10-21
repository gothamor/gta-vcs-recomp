// Read-only access to the user's PS2 disc image (ISO 9660, 2048-byte sectors).
// The whole image is memory-mapped once; reads are plain memcpy, so the game's
// sector streaming costs nothing beyond page faults.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

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
        static constexpr uint32_t kSectorSize = 2048;

        IsoImage() = default;
        ~IsoImage();
        IsoImage(const IsoImage &) = delete;
        IsoImage &operator=(const IsoImage &) = delete;

        // Maps the image and indexes every file. Returns false with error() set on failure.
        bool open(const std::filesystem::path &path);
        void close();
        bool isOpen() const { return m_data != nullptr; }
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

        // Direct pointer into the mapping (valid while open).
        const uint8_t *sectorPtr(uint32_t lba) const;

        const std::unordered_map<std::string, IsoEntry> &entries() const { return m_entries; }

        static std::string normalizePath(std::string_view path);

    private:
        bool indexDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);

        const uint8_t *m_data = nullptr;
        uint64_t m_size = 0;
        void *m_file = nullptr;
        void *m_mapping = nullptr;
        std::string m_error;
        std::unordered_map<std::string, IsoEntry> m_entries; // key: normalizePath()
    };
}
