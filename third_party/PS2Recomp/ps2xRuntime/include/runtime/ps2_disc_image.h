// [gta-recomp] Pluggable disc image. When set, cdrom0: file lookups and all
// sector reads (EE sceCd*, cdrom0: VFS, IOP cdvdman) go through it with the
// real on-disc sector numbers, instead of a pseudo-ISO built from a folder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class IPS2DiscImage
{
public:
    virtual ~IPS2DiscImage() = default;
    // ps2Path may carry a "cdrom0:" prefix, '\' separators and a ";1" suffix.
    virtual bool findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const = 0;
    virtual bool readBytes(uint64_t offset, size_t byteCount, void *destination) const = 0;
    virtual uint32_t totalSectors() const = 0;

    struct DirEntry
    {
        std::string name; // leaf name as on disc (upper case, no ";1")
        uint32_t lsn = 0;
        uint32_t sizeBytes = 0;
        bool isDirectory = false;
    };
    // Children of a directory ("" or "/" = root). False if not a directory.
    virtual bool listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const = 0;
};

namespace ps2_disc
{
    void setImage(IPS2DiscImage *image);
    IPS2DiscImage *image();
}
