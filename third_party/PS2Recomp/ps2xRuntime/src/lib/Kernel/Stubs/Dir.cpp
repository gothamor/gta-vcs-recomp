// [gta-recomp] sceDopen/sceDread/sceDclose (libfileio directory API) for cdrom0:,
// served from the mapped disc image. VCS walks the whole disc at boot with these
// to build its own file table.
#include "Common.h"
#include "runtime/ps2_disc_image.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2_stubs
{
    namespace
    {
        struct OpenDir
        {
            std::vector<IPS2DiscImage::DirEntry> entries;
            size_t next = 0;
        };

        std::mutex g_dirMutex;
        std::unordered_map<int32_t, OpenDir> g_dirs;
        int32_t g_nextDirFd = 0x100; // separate range from the VFS's file descriptors

        void writeLe32(uint8_t *p, uint32_t v)
        {
            std::memcpy(p, &v, 4);
        }
    }

    void sceDopen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const char *path = reinterpret_cast<const char *>(getConstMemPtr(rdram, getRegU32(ctx, 4)));
        IPS2DiscImage *disc = ps2_disc::image();
        if (!path || !disc)
        {
            setReturnS32(ctx, -1);
            return;
        }
        std::string_view p(path);
        const size_t colon = p.find(':');
        if (colon == std::string_view::npos)
        {
            setReturnS32(ctx, -1);
            return;
        }
        std::string device(p.substr(0, colon));
        for (char &c : device)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        OpenDir dir;
        if ((device != "cdrom0" && device != "cdrom") || !disc->listDirectory(p.substr(colon + 1), dir.entries))
        {
            setReturnS32(ctx, -1);
            return;
        }
        std::lock_guard<std::mutex> lock(g_dirMutex);
        const int32_t fd = g_nextDirFd++;
        g_dirs.emplace(fd, std::move(dir));
        setReturnS32(ctx, fd);
    }

    void sceDread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        uint8_t *out = getMemPtr(rdram, getRegU32(ctx, 5));
        std::lock_guard<std::mutex> lock(g_dirMutex);
        auto it = g_dirs.find(fd);
        if (it == g_dirs.end() || !out)
        {
            setReturnS32(ctx, -1);
            return;
        }
        OpenDir &dir = it->second;
        if (dir.next >= dir.entries.size())
        {
            setReturnS32(ctx, 0);
            return;
        }
        const IPS2DiscImage::DirEntry &e = dir.entries[dir.next++];
        // struct sce_dirent { sce_stat d_stat (64 bytes); char d_name[256]; void *d_private; }
        // sce_stat: mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6]
        std::memset(out, 0, 64 + 256 + 4);
        constexpr uint32_t kStmFdir = 0x1000, kStmFreg = 0x2000, kStmRead = 0x0124;
        writeLe32(out + 0, (e.isDirectory ? kStmFdir : kStmFreg) | kStmRead);
        writeLe32(out + 8, e.sizeBytes);
        // Like the real cdrom0: driver, files keep their ISO 9660 version suffix
        // ("NAME.EXT;1"). VCS's file table lookups depend on it.
        const std::string name = e.isDirectory ? e.name : e.name + ";1";
        std::memcpy(out + 64, name.data(), std::min<size_t>(255, name.size()));
        setReturnS32(ctx, static_cast<int32_t>(std::min<size_t>(255, name.size())));
    }

    void sceDclose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_dirMutex);
        setReturnS32(ctx, g_dirs.erase(static_cast<int32_t>(getRegU32(ctx, 4))) ? 0 : -1);
    }
}
