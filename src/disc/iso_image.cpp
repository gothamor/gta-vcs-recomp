#include "iso_image.h"

#include <cstring>
#include <vector>

namespace vcs
{
    namespace
    {
        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
    }

    IsoImage::~IsoImage()
    {
        close();
    }

    std::string IsoImage::normalizePath(std::string_view path)
    {
        if (path.size() >= 7 && (path.substr(0, 7) == "cdrom0:" || path.substr(0, 7) == "CDROM0:"))
        {
            path.remove_prefix(7);
        }
        std::string out;
        out.reserve(path.size());
        for (char c : path)
        {
            if (c == ';')
            {
                break;
            }
            if (c == '\\')
            {
                c = '/';
            }
            if (c == '/' && (out.empty() || out.back() == '/'))
            {
                continue;
            }
            if (c >= 'a' && c <= 'z')
            {
                c = static_cast<char>(c - 'a' + 'A');
            }
            out.push_back(c);
        }
        while (!out.empty() && out.back() == '/')
        {
            out.pop_back();
        }
        return out;
    }

    bool IsoImage::open(const std::filesystem::path &path)
    {
        close();
        return adopt(openDiscSource(path, m_error));
    }

    bool IsoImage::adopt(std::unique_ptr<DiscSource> source)
    {
        m_source = std::move(source);
        if (!m_source)
            return false;
        m_data = m_source->data();
        m_size = m_source->size();

        if (m_size < 17 * kSectorSize)
        {
            m_error = "image too small";
            close();
            return false;
        }
        uint8_t pvd[kSectorSize];
        if (!m_source->read(16 * uint64_t(kSectorSize), kSectorSize, pvd) || pvd[0] != 1 ||
            std::memcmp(pvd + 1, "CD001", 5) != 0)
        {
            m_error = "not an ISO 9660 image";
            close();
            return false;
        }
        const uint8_t *root = pvd + 156;
        if (!indexDirectory(le32(root + 2), le32(root + 10), "", 0))
        {
            close();
            return false;
        }
        return true;
    }

    bool IsoImage::indexDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth)
    {
        if (depth > 16 || size > (16u << 20) || uint64_t(lba) * kSectorSize + size > m_size)
        {
            m_error = "corrupt directory tree";
            return false;
        }
        std::vector<uint8_t> buffer(size);
        if (!m_source->read(uint64_t(lba) * kSectorSize, size, buffer.data()))
        {
            m_error = "cannot read a directory";
            return false;
        }
        const uint8_t *dir = buffer.data();
        Children &kids = m_children[normalizePath(prefix)];
        uint32_t pos = 0;
        while (pos < size)
        {
            const uint8_t length = dir[pos];
            if (length == 0)
            {
                // Records never cross a sector boundary; zero means skip to the next sector.
                pos = (pos / kSectorSize + 1) * kSectorSize;
                continue;
            }
            const uint8_t *rec = dir + pos;
            // a player's file may be anything: a record must hold its fixed part and its name, inside the buffer
            if (length < 34 || uint64_t(pos) + length > size || 33u + rec[32] > length)
            {
                m_error = "corrupt directory record";
                return false;
            }
            pos += length;
            const uint8_t nameLen = rec[32];
            if (nameLen == 1 && (rec[33] == 0 || rec[33] == 1))
            {
                continue; // "." and ".."
            }
            std::string name(reinterpret_cast<const char *>(rec + 33), nameLen);
            if (const size_t semi = name.find(';'); semi != std::string::npos)
                name.resize(semi);
            IsoEntry entry;
            entry.lba = le32(rec + 2);
            entry.size = le32(rec + 10);
            entry.isDirectory = (rec[25] & 2) != 0;
            const std::string full = normalizePath(prefix.empty() ? name : prefix + "/" + name);
            m_entries[full] = entry;
            kids.emplace_back(name, entry);
            if (entry.isDirectory && !indexDirectory(entry.lba, entry.size, full, depth + 1))
            {
                return false;
            }
        }
        return true;
    }

    void IsoImage::close()
    {
        m_source.reset();
        m_data = nullptr;
        m_size = 0;
        m_entries.clear();
        m_children.clear();
    }

    const IsoImage::Children *IsoImage::children(std::string_view dirPath) const
    {
        auto it = m_children.find(normalizePath(dirPath));
        return it == m_children.end() ? nullptr : &it->second;
    }

    bool IsoImage::find(std::string_view path, IsoEntry &out) const
    {
        auto it = m_entries.find(normalizePath(path));
        if (it == m_entries.end())
        {
            return false;
        }
        out = it->second;
        return true;
    }

    bool IsoImage::readBytes(uint64_t offset, size_t byteCount, void *dst) const
    {
        if (!m_source || offset >= m_size)
        {
            std::memset(dst, 0, byteCount);
            return false;
        }
        const uint64_t available = m_size - offset;
        const size_t n = available < byteCount ? static_cast<size_t>(available) : byteCount;
        if (m_data)
            std::memcpy(dst, m_data + offset, n); // mapped: the hot path as before
        else if (!m_source->read(offset, n, dst))
        {
            std::memset(dst, 0, byteCount);
            return false;
        }
        if (n < byteCount)
        {
            std::memset(static_cast<uint8_t *>(dst) + n, 0, byteCount - n);
            return false;
        }
        return true;
    }

    bool IsoImage::readSectors(uint32_t lba, uint32_t count, void *dst) const
    {
        return readBytes(uint64_t(lba) * kSectorSize, size_t(count) * kSectorSize, dst);
    }

    const uint8_t *IsoImage::sectorPtr(uint32_t lba) const
    {
        const uint64_t offset = uint64_t(lba) * kSectorSize;
        return (m_data && offset < m_size) ? m_data + offset : nullptr;
    }
}
