#include "iso_image.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
#if defined(_WIN32)
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            m_error = "cannot open " + path.string();
            return false;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(file, &size);
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping)
        {
            CloseHandle(file);
            m_error = "cannot map " + path.string();
            return false;
        }
        void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!view)
        {
            CloseHandle(mapping);
            CloseHandle(file);
            m_error = "cannot map view of " + path.string();
            return false;
        }
        m_file = file;
        m_mapping = mapping;
        m_data = static_cast<const uint8_t *>(view);
        m_size = static_cast<uint64_t>(size.QuadPart);
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            m_error = "cannot open " + path.string();
            return false;
        }
        struct stat st{};
        fstat(fd, &st);
        void *view = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (view == MAP_FAILED)
        {
            m_error = "cannot map " + path.string();
            return false;
        }
        m_data = static_cast<const uint8_t *>(view);
        m_size = static_cast<uint64_t>(st.st_size);
#endif

        if (m_size < 17 * kSectorSize)
        {
            m_error = "image too small";
            close();
            return false;
        }
        const uint8_t *pvd = m_data + 16 * kSectorSize;
        if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
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
        if (depth > 16 || uint64_t(lba) * kSectorSize + size > m_size)
        {
            m_error = "corrupt directory tree";
            return false;
        }
        const uint8_t *dir = m_data + uint64_t(lba) * kSectorSize;
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
#if defined(_WIN32)
        if (m_data)
        {
            UnmapViewOfFile(m_data);
        }
        if (m_mapping)
        {
            CloseHandle(static_cast<HANDLE>(m_mapping));
        }
        if (m_file)
        {
            CloseHandle(static_cast<HANDLE>(m_file));
        }
#else
        if (m_data)
        {
            munmap(const_cast<uint8_t *>(m_data), m_size);
        }
#endif
        m_data = nullptr;
        m_mapping = nullptr;
        m_file = nullptr;
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
        if (!m_data || offset >= m_size)
        {
            std::memset(dst, 0, byteCount);
            return false;
        }
        const uint64_t available = m_size - offset;
        const size_t n = available < byteCount ? static_cast<size_t>(available) : byteCount;
        std::memcpy(dst, m_data + offset, n);
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
