#include "disc_source.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

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

#if VCS_DISC_CHD
#include <libchdr/chd.h>
#endif

namespace vcs
{
    std::string pathText(const std::filesystem::path &path)
    {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }

    namespace
    {
        bool isChdMagic(const char (&first8)[8]) { return std::memcmp(first8, "MComprHD", 8) == 0; } // a CHD's first bytes

        // ---- an .iso, mapped ------------------------------------------------------------------------------------
        class MappedSource final : public DiscSource
        {
        public:
            ~MappedSource() override
            {
#if defined(_WIN32)
                if (m_data)
                    UnmapViewOfFile(m_data);
                if (m_mapping)
                    CloseHandle(m_mapping);
                if (m_file != INVALID_HANDLE_VALUE)
                    CloseHandle(m_file);
#else
                if (m_data)
                    munmap(const_cast<uint8_t *>(m_data), m_size);
#endif
            }

            bool open(const std::filesystem::path &path, std::string &error)
            {
#if defined(_WIN32)
                m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
                if (m_file == INVALID_HANDLE_VALUE)
                {
                    error = "cannot open " + pathText(path);
                    return false;
                }
                LARGE_INTEGER size{};
                GetFileSizeEx(m_file, &size);
                m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (!m_mapping)
                {
                    error = "cannot map " + pathText(path);
                    return false;
                }
                const void *view = MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0);
                if (!view)
                {
                    error = "cannot map view of " + pathText(path);
                    return false;
                }
                m_data = static_cast<const uint8_t *>(view);
                m_size = static_cast<uint64_t>(size.QuadPart);
#else
                const int fd = ::open(path.c_str(), O_RDONLY);
                if (fd < 0)
                {
                    error = "cannot open " + pathText(path);
                    return false;
                }
                struct stat st{};
                fstat(fd, &st);
                void *view = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
                ::close(fd);
                if (view == MAP_FAILED)
                {
                    error = "cannot map " + pathText(path);
                    return false;
                }
                m_data = static_cast<const uint8_t *>(view);
                m_size = static_cast<uint64_t>(st.st_size);
#endif
                return true;
            }

            uint64_t size() const override { return m_size / kSectorSize * kSectorSize; }

            bool read(uint64_t offset, size_t byteCount, void *dst) const override
            {
                if (offset > size() || byteCount > size() - offset)
                    return false;
                std::memcpy(dst, m_data + offset, byteCount);
                return true;
            }

            const uint8_t *data() const override { return m_data; }
            bool plainSectors() const override { return true; }
            const char *kind() const override { return "iso"; }

        private:
            const uint8_t *m_data = nullptr;
            uint64_t m_size = 0;
#if defined(_WIN32)
            HANDLE m_file = INVALID_HANDLE_VALUE;
            HANDLE m_mapping = nullptr;
#endif
        };

#if VCS_DISC_CHD
        // ---- a .chd, through libchdr -------------------------------------------------------------------------
        // The file, read at positions: several decoders read it at once, each through its own Cursor.
        class SharedFile
        {
        public:
            SharedFile(const SharedFile &) = delete;
            SharedFile &operator=(const SharedFile &) = delete;
            ~SharedFile()
            {
#if defined(_WIN32)
                if (m_handle != INVALID_HANDLE_VALUE)
                    CloseHandle(m_handle);
#else
                if (m_fd >= 0)
                    ::close(m_fd);
#endif
            }

            static std::shared_ptr<SharedFile> open(const std::filesystem::path &path)
            {
                std::shared_ptr<SharedFile> file(new SharedFile);
#if defined(_WIN32)
                file->m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
#else
                file->m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
                return file->measure() ? file : nullptr;
            }

            uint64_t size() const { return m_size; }

            // up to `n` bytes at `at`; the count read (short at the end of the file or on an error)
            size_t readAt(uint64_t at, void *dst, size_t n) const
            {
                uint8_t *out = static_cast<uint8_t *>(dst);
                size_t done = 0;
                while (done < n)
                {
                    const uint64_t pos = at + done;
#if defined(_WIN32)
                    OVERLAPPED where{};
                    where.Offset = static_cast<DWORD>(pos);
                    where.OffsetHigh = static_cast<DWORD>(pos >> 32);
                    DWORD got = 0;
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 30));
                    if (!ReadFile(m_handle, out + done, want, &got, &where) || got == 0)
                        break;
#else
                    const ssize_t got = pread(m_fd, out + done, n - done, static_cast<off_t>(pos));
                    if (got < 0 && errno == EINTR)
                        continue;
                    if (got <= 0)
                        break;
#endif
                    done += static_cast<size_t>(got);
                }
                return done;
            }

        private:
            SharedFile() = default;

            bool measure()
            {
#if defined(_WIN32)
                LARGE_INTEGER size{};
                if (m_handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(m_handle, &size))
                    return false;
                m_size = static_cast<uint64_t>(size.QuadPart);
#else
                struct stat st{};
                if (m_fd < 0 || fstat(m_fd, &st) != 0)
                    return false;
                m_size = static_cast<uint64_t>(st.st_size);
#endif
                return true;
            }

#if defined(_WIN32)
            HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
            int m_fd = -1;
#endif
            uint64_t m_size = 0;
        };

        // One decoder's view of the file, handed to libchdr, which owns it from then on (chd_close deletes it, also
        // when chd_open fails after taking it).
        struct Cursor
        {
            std::shared_ptr<const SharedFile> file;
            uint64_t pos = 0;
        };

        uint64_t chdFileSize(void *c) { return static_cast<Cursor *>(c)->file->size(); }
        size_t chdFileRead(void *dst, size_t size, size_t count, void *c)
        {
            Cursor *cursor = static_cast<Cursor *>(c);
            if (size == 0 || count == 0)
                return 0;
            const size_t got = cursor->file->readAt(cursor->pos, dst, size * count);
            cursor->pos += got;
            return got / size;
        }
        int chdFileClose(void *c)
        {
            delete static_cast<Cursor *>(c);
            return 0;
        }
        int chdFileSeek(void *c, int64_t offset, int whence)
        {
            Cursor *cursor = static_cast<Cursor *>(c);
            const int64_t base = whence == SEEK_SET   ? 0
                                 : whence == SEEK_CUR ? static_cast<int64_t>(cursor->pos)
                                 : whence == SEEK_END ? static_cast<int64_t>(cursor->file->size())
                                                      : -1;
            if (base < 0 || base + offset < 0)
                return -1;
            cursor->pos = static_cast<uint64_t>(base + offset);
            return 0;
        }
        const core_file_callbacks kChdFileCallbacks = {chdFileSize, chdFileRead, chdFileClose, chdFileSeek};

        // null with `err` set on failure
        chd_file *openChdHandle(const std::shared_ptr<const SharedFile> &file, chd_error &err)
        {
            Cursor *cursor = new Cursor{file, 0};
            chd_file *chd = nullptr;
            err = chd_open_core_file_callbacks(&kChdFileCallbacks, cursor, CHD_OPEN_READ, nullptr, &chd);
            // on a failure libchdr has closed (deleted) the cursor through chd_close, except for an out-of-memory
            // before its setup - accepted
            return err == CHDERR_NONE ? chd : nullptr;
        }

        constexpr uint32_t kCdFrameBytes = 2448; // a CD frame in a CHD: 2352 sector + 96 subcode (cdrom.h)

        // Where the 2048 data bytes sit in a CD frame, by the track's type (chd.h's CDROM_TRACK_METADATA formats)
        int cdDataOffset(const std::string &type)
        {
            if (type == "MODE1" || type == "MODE1/2048" || type == "MODE2_FORM1" || type == "MODE2/2048")
                return 0;
            if (type == "MODE1_RAW" || type == "MODE1/2352")
                return 16;
            if (type == "MODE2_RAW" || type == "MODE2/2352")
                return 24;
            return -1;
        }

        // A read's hunks decode on several decoders at once (docs/DISC.md "CHD images"): a CD hunk takes 1-2 ms on
        // a slower CPU, and one streaming read holds 15-20 of them.
        //   - Decoders: libchdr handles on the same file, each with its own position. The reading thread decodes with
        //     the first; VCS_CHD_DECODERS - 1 worker threads hold one each (default: half the cores, at most 4; 1 is
        //     the single-threaded reader).
        //   - Hunks: decoded once, shared (shared_ptr) by the cache and every read waiting for them, so an eviction
        //     never takes a hunk from a read that still needs it. The cache: ~16 MB (VCS_CHD_CACHE_HUNKS for tests).
        class ChdSource final : public DiscSource
        {
        public:
            ~ChdSource() override
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_work.notify_all();
                for (std::thread &t : m_workers)
                    t.join();
                if (m_chd)
                    chd_close(m_chd);
            }

            bool open(const std::filesystem::path &path, std::string &error)
            {
                std::shared_ptr<SharedFile> file = SharedFile::open(path);
                if (!file)
                {
                    error = "cannot open " + pathText(path);
                    return false;
                }
                return openFile(std::move(file), error);
            }

            bool openFile(std::shared_ptr<const SharedFile> file, std::string &error)
            {
                chd_error err = CHDERR_NONE;
                m_chd = openChdHandle(file, err);
                if (!m_chd)
                {
                    error = std::string("cannot read the CHD: ") + chd_error_string(err);
                    return false;
                }
                const chd_header *h = chd_get_header(m_chd);
                m_hunkBytes = h->hunkbytes;
                char meta[256] = {};
                uint32_t length = 0;
                if (chd_get_metadata(m_chd, DVD_METADATA_TAG, 0, meta, sizeof(meta) - 1, &length, nullptr, nullptr) == CHDERR_NONE)
                {
                    if (h->unitbytes != kSectorSize)
                    {
                        error = "DVD CHD with " + std::to_string(h->unitbytes) + "-byte units";
                        return false;
                    }
                    m_layout = {"chd (DVD)", kSectorSize, 0, h->logicalbytes / kSectorSize};
                }
                else if (chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &length, nullptr, nullptr) == CHDERR_NONE ||
                         chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &length, nullptr, nullptr) == CHDERR_NONE)
                {
                    // the first track: a PS2 disc's data (CD-mode CHDs of PS2 DVDs have exactly one)
                    int track = 0, frames = 0;
                    char type[64] = {}, subtype[64] = {};
                    if (std::sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d", &track, type, subtype, &frames) != 4 ||
                        frames <= 0)
                    {
                        error = std::string("unreadable CD track metadata: ") + meta;
                        return false;
                    }
                    const int dataOffset = cdDataOffset(type);
                    if (dataOffset < 0)
                    {
                        error = std::string("the CHD's first track is ") + type + ", not a data track";
                        return false;
                    }
                    // CHT2: pregap frames stored in the track (PGTYPE starting with V). PS2 discs have none; where
                    // sector 0 then sits is not checked against chdman here, so refuse rather than guess
                    int pregap = 0;
                    char pgtype[64] = {};
                    if (const char *p = std::strstr(meta, "PREGAP:"); p && std::sscanf(p, "PREGAP:%d PGTYPE:%63s", &pregap, pgtype) == 2 &&
                        pgtype[0] == 'V' && pregap > 0)
                    {
                        error = "a CD track with " + std::to_string(pregap) + " pregap frames stored: not supported";
                        return false;
                    }
                    m_layout = {"chd (CD)", kCdFrameBytes, dataOffset, uint64_t(frames)};
                }
                else
                {
                    error = "not a disc CHD (neither DVD nor CD metadata)";
                    return false;
                }
                if (m_hunkBytes == 0 || m_hunkBytes % m_layout.frameBytes != 0)
                {
                    error = "CHD hunks of " + std::to_string(m_hunkBytes) + " bytes";
                    return false;
                }
                m_framesPerHunk = m_hunkBytes / m_layout.frameBytes;
                if ((m_layout.sectors + m_framesPerHunk - 1) / m_framesPerHunk > h->totalhunks)
                {
                    error = "the CHD's track is longer than its data (" + std::to_string(m_layout.sectors) + " sectors, " +
                            std::to_string(h->totalhunks) + " hunks)";
                    return false;
                }
                m_maxCached = (16u << 20) / m_hunkBytes + 2; // ~16 MB of decompressed hunks
                if (const char *n = std::getenv("VCS_CHD_CACHE_HUNKS"); n && std::atoi(n) > 0)
                    m_maxCached = size_t(std::atoi(n));
                const unsigned decoders = decoderCount();
                // the workers open their own handles: decoding the map (tens of ms) does not delay the boot
                for (unsigned i = 1; i < decoders; ++i)
                    m_workers.emplace_back([this, file] { work(file); });
                std::fprintf(stderr, "[disc] chd: decoders %u, cache %zu hunks\n", decoders, m_maxCached);
                return true;
            }

            uint64_t size() const override { return m_layout.sectors * kSectorSize; }

            bool read(uint64_t offset, size_t byteCount, void *dst) const override
            {
                if (offset > size() || byteCount > size() - offset)
                    return false;
                uint8_t *out = static_cast<uint8_t *>(dst);
                while (byteCount)
                {
                    // a window of hunks at a time: what one read holds decoded stays bounded
                    const uint64_t firstSector = offset / kSectorSize;
                    const uint64_t lastSector = (offset + byteCount - 1) / kSectorSize;
                    const uint32_t first = uint32_t(firstSector / m_framesPerHunk);
                    const uint32_t last = std::min<uint32_t>(uint32_t(lastSector / m_framesPerHunk), first + kWindowHunks - 1);
                    std::vector<HunkPtr> hunks;
                    if (!fetch(first, last, hunks))
                        return false;
                    for (const HunkPtr &hunk : hunks)
                    {
                        // the hunk's sectors in the request
                        while (byteCount && offset / kSectorSize / m_framesPerHunk == hunk->number)
                        {
                            const uint64_t sector = offset / kSectorSize;
                            const uint32_t within = uint32_t(offset % kSectorSize);
                            const size_t at = size_t(sector % m_framesPerHunk) * m_layout.frameBytes + size_t(m_layout.dataOffset) + within;
                            const size_t n = std::min<size_t>(byteCount, kSectorSize - within);
                            std::memcpy(out, hunk->data.data() + at, n);
                            out += n;
                            offset += n;
                            byteCount -= n;
                        }
                    }
                }
                return true;
            }

            const char *kind() const override { return m_layout.kind; }

        private:
            static constexpr uint32_t kWindowHunks = 64;

            struct Hunk
            {
                uint32_t number = 0;
                std::vector<uint8_t> data;
                bool done = false, failed = false;
            };
            using HunkPtr = std::shared_ptr<Hunk>;

            static unsigned decoderCount()
            {
                if (const char *n = std::getenv("VCS_CHD_DECODERS"); n && std::atoi(n) > 0)
                    return unsigned(std::min(std::atoi(n), 16));
                const unsigned cores = std::thread::hardware_concurrency();
                return std::clamp(cores / 2, 1u, 4u);
            }

            // hunks [first, last], decoded, into `out`; false on a decode error
            bool fetch(uint32_t first, uint32_t last, std::vector<HunkPtr> &out) const
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                out.clear();
                bool queued = false;
                for (uint32_t number = first; number <= last; ++number)
                {
                    if (auto it = m_index.find(number); it != m_index.end())
                    {
                        m_lru.splice(m_lru.begin(), m_lru, it->second);
                        out.push_back(*it->second);
                    }
                    else if (auto f = m_inFlight.find(number); f != m_inFlight.end())
                        out.push_back(f->second);
                    else
                    {
                        HunkPtr hunk = std::make_shared<Hunk>();
                        hunk->number = number;
                        m_inFlight.emplace(number, hunk);
                        m_queue.push_back(hunk);
                        out.push_back(std::move(hunk));
                        queued = true;
                    }
                }
                if (queued && !m_workers.empty())
                    m_work.notify_all();
                for (const HunkPtr &hunk : out)
                {
                    // help with the queue while the first handle is free, else wait for the workers
                    while (!hunk->done)
                    {
                        if (!m_queue.empty() && !m_mainBusy)
                        {
                            m_mainBusy = true;
                            decodeOne(lock, m_chd);
                            m_mainBusy = false;
                        }
                        else
                            m_done.wait(lock);
                    }
                    if (hunk->failed)
                        return false;
                }
                return true;
            }

            // the queue's front decoded on `chd`; m_mutex held on entry and on return, not while decoding
            void decodeOne(std::unique_lock<std::mutex> &lock, chd_file *chd) const
            {
                HunkPtr hunk = std::move(m_queue.front());
                m_queue.pop_front();
                lock.unlock();
                // out of memory fails the hunk like a decode error: no hunk stays in flight with readers waiting
                chd_error err = CHDERR_OUT_OF_MEMORY;
                try
                {
                    hunk->data.resize(m_hunkBytes);
                    err = chd_read(chd, hunk->number, hunk->data.data());
                }
                catch (const std::bad_alloc &)
                {
                }
                lock.lock();
                m_inFlight.erase(hunk->number);
                if (err != CHDERR_NONE)
                {
                    if (!m_reported)
                        std::fprintf(stderr, "[disc] CHD hunk %u: %s\n", hunk->number, chd_error_string(err));
                    m_reported = true;
                    hunk->failed = true;
                }
                else
                {
                    if (m_lru.size() >= m_maxCached)
                    {
                        m_index.erase(m_lru.back()->number);
                        m_lru.pop_back();
                    }
                    m_lru.push_front(hunk);
                    m_index[hunk->number] = m_lru.begin();
                }
                hunk->done = true;
                m_done.notify_all();
            }

            void work(std::shared_ptr<const SharedFile> file) const
            {
                chd_error err = CHDERR_NONE;
                chd_file *chd = openChdHandle(file, err);
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!chd)
                {
                    // the reader goes on with the decoders it has
                    std::fprintf(stderr, "[disc] chd: a decoder failed to open: %s\n", chd_error_string(err));
                    return;
                }
                for (;;)
                {
                    m_work.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                    if (m_stop)
                        break;
                    decodeOne(lock, chd);
                }
                lock.unlock();
                chd_close(chd);
            }

            chd_file *m_chd = nullptr; // the first decoder: the reading thread's
            // where the sectors' 2048 data bytes sit: a DVD CHD's units are the sectors, a CD-mode CHD's are frames
            struct Layout
            {
                const char *kind = "chd";
                uint32_t frameBytes = 0; // a unit: 2048 (DVD) or 2448 (a CD frame)
                int dataOffset = 0;      // the data's offset in a unit
                uint64_t sectors = 0;
            };
            Layout m_layout;
            uint32_t m_hunkBytes = 0, m_framesPerHunk = 1;
            size_t m_maxCached = 0;
            std::vector<std::thread> m_workers;
            mutable std::mutex m_mutex;
            mutable std::condition_variable m_work, m_done;
            mutable std::deque<HunkPtr> m_queue;                      // hunks to decode, in the order asked
            mutable std::unordered_map<uint32_t, HunkPtr> m_inFlight; // queued or decoding
            mutable std::list<HunkPtr> m_lru;
            mutable std::unordered_map<uint32_t, std::list<HunkPtr>::iterator> m_index;
            mutable bool m_mainBusy = false, m_reported = false, m_stop = false;
        };
#endif

        // Is this file a CHD? (Its first 8 bytes.)
        bool isChdFile(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            FILE *file = _wfopen(path.c_str(), L"rb");
#else
            FILE *file = std::fopen(path.c_str(), "rb");
#endif
            if (!file)
                return false;
            char magic[8] = {};
            const bool chd = std::fread(magic, 1, 8, file) == 8 && isChdMagic(magic);
            std::fclose(file);
            return chd;
        }

        // VCS_DISC_TIME=<s>: for each window of s seconds with reads, how much the game read and how long it took, from
        // the disc's opening (`at`; docs/DISC.md "CHD images"). data() is null, so a mapped .iso's reads come here too.
        class TimedSource final : public DiscSource
        {
        public:
            TimedSource(std::unique_ptr<DiscSource> inner, double window)
                : m_inner(std::move(inner)), m_window(window), m_opened(std::chrono::steady_clock::now())
            {
            }

            uint64_t size() const override { return m_inner->size(); }
            bool plainSectors() const override { return m_inner->plainSectors(); }
            const char *kind() const override { return m_inner->kind(); }

            bool read(uint64_t offset, size_t byteCount, void *dst) const override
            {
                using Clock = std::chrono::steady_clock;
                const auto start = Clock::now();
                const bool ok = m_inner->read(offset, byteCount, dst);
                const auto end = Clock::now();
                const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_reads == 0)
                    m_since = start;
                ++m_reads;
                m_bytes += byteCount;
                m_us += us;
                m_longestUs = std::max(m_longestUs, us);
                m_slow += us > 4000 ? 1u : 0u;
                if (std::chrono::duration<double>(end - m_since).count() >= m_window)
                {
                    std::fprintf(stderr, "[disc-time] at %.1f s, %s: %llu reads, %.2f MB, %.1f ms in reads (longest %.1f ms, "
                                         "%llu over 4 ms) in %.1f s\n",
                                 std::chrono::duration<double>(m_since - m_opened).count(), m_inner->kind(),
                                 static_cast<unsigned long long>(m_reads), m_bytes / 1048576.0, m_us / 1000.0,
                                 m_longestUs / 1000.0, static_cast<unsigned long long>(m_slow),
                                 std::chrono::duration<double>(end - m_since).count());
                    m_reads = m_bytes = m_us = m_longestUs = m_slow = 0;
                }
                return ok;
            }

        private:
            std::unique_ptr<DiscSource> m_inner;
            const double m_window;
            const std::chrono::steady_clock::time_point m_opened;
            mutable std::mutex m_mutex;
            mutable std::chrono::steady_clock::time_point m_since{};
            mutable uint64_t m_reads = 0, m_bytes = 0, m_us = 0, m_longestUs = 0, m_slow = 0;
        };

        std::unique_ptr<DiscSource> timed(std::unique_ptr<DiscSource> source)
        {
            const char *on = std::getenv("VCS_DISC_TIME");
            const double window = on ? std::atof(on) : 0.0;
            if (!source || !(window > 0.0))
                return source;
            return std::make_unique<TimedSource>(std::move(source), window);
        }

        std::unique_ptr<DiscSource> openUntimed(const std::filesystem::path &path, std::string &error)
        {
            if (isChdFile(path))
            {
#if VCS_DISC_CHD
                auto chd = std::make_unique<ChdSource>();
                if (!chd->open(path, error))
                    return nullptr;
                return chd;
#else
                error = "a .chd: this program reads .iso images only";
                return nullptr;
#endif
            }
            auto mapped = std::make_unique<MappedSource>();
            if (!mapped->open(path, error))
                return nullptr;
            return mapped;
        }
    }

    std::unique_ptr<DiscSource> openDiscSource(const std::filesystem::path &path, std::string &error)
    {
        return timed(openUntimed(path, error));
    }

}
