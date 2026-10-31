#include "render/texture_replacements.h"

#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include "../../third_party/stb/stb_image.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace render
{
    uint64_t textureHash(const void *data, size_t bytes) { return XXH3_64bits(data, bytes); }

    namespace
    {
        struct Name
        {
            uint64_t pixels = 0, palette = 0;
            uint32_t bits = 0;
            bool operator==(const Name &) const = default;
        };
        struct NameHash
        {
            size_t operator()(const Name &n) const { return size_t(n.pixels ^ (n.palette * 31) ^ n.bits); }
        };
        bool parseName(const std::string &stem, Name &n)
        {
            // Full-texture base images; reject region/mip filenames instead of accidentally aliasing them.
            const size_t a = stem.find('-'), b = a == stem.npos ? a : stem.find('-', a + 1);
            auto hex = [](std::string_view s, auto &v) {
                if (s.empty()) return false;
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v, 16);
                return r.ec == std::errc{} && r.ptr == s.data() + s.size();
            };
            if (a == stem.npos || !hex(std::string_view(stem).substr(0, a), n.pixels)) return false;
            if (b != stem.npos && !hex(std::string_view(stem).substr(a + 1, b - a - 1), n.palette)) return false;
            if (!hex(std::string_view(stem).substr((b == stem.npos ? a : b) + 1), n.bits)) return false;
            n.bits &= ~(1u << 14); // old PCSX2 TCC flag; modern PCSX2 ignores it too
            return true;
        }
        size_t memoryBudget()
        {
            const char *e = std::getenv("VCS_TEXTURE_CACHE_MB");
            return size_t(e ? std::clamp(std::atoi(e), 64, 4096) : 512) * 1024 * 1024;
        }
        std::shared_ptr<ReplacementImage> load(const std::filesystem::path &path, uint32_t id)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            const auto bytes = f ? f.tellg() : std::streampos(-1);
            if (bytes <= 0 || bytes > 128 * 1024 * 1024) return {};
            std::vector<uint8_t> encoded(static_cast<size_t>(bytes));
            f.seekg(0);
            if (!f.read(reinterpret_cast<char *>(encoded.data()), encoded.size())) return {};
            int w = 0, h = 0, channels = 0;
            if (!stbi_info_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels) ||
                w <= 0 || h <= 0 || w > 8192 || h > 8192 || uint64_t(w) * h > 64 * 1024 * 1024) return {};
            auto *rgba = stbi_load_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels, 4);
            if (!rgba) return {};
            auto image = std::make_shared<ReplacementImage>();
            image->id = id;
            image->width = uint32_t(w);
            image->height = uint32_t(h);
            image->pixels.resize(size_t(w) * h);
            std::memcpy(image->pixels.data(), rgba, image->pixels.size() * 4);
            stbi_image_free(rgba);
            return image;
        }
    }

    struct TextureReplacements::Impl
    {
        struct File { std::filesystem::path path; bool pending = false, failed = false, matched = false; };
        std::vector<File> files{1}; // id 0 = no match
        std::unordered_map<Name, uint32_t, NameHash> names;
        std::unordered_set<uint32_t> shapes;
        size_t gpuBudget = memoryBudget(), readyBytes = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<uint32_t> queue;
        std::vector<std::shared_ptr<const ReplacementImage>> ready;
        std::thread worker;
        bool stopping = false;
        uint32_t matched = 0, loaded = 0;

        void run()
        {
            for (;;)
            {
                uint32_t id;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping) return;
                    id = queue.front(); queue.pop_front();
                }
                auto image = load(files[id].path, id);
                std::unique_lock lock(mutex);
                if (!image)
                {
                    files[id].failed = true;
                    files[id].pending = false;
                    std::fprintf(stderr, "[textures] failed to load %s; keeping original\n", files[id].path.string().c_str());
                    continue;
                }
                // Bound decoded images waiting for the GS stage. One large image may exceed 128 MiB on its own.
                const size_t bytes = image->pixels.size() * 4;
                cv.wait(lock, [&] { return stopping || ready.empty() || readyBytes + bytes <= 128 * 1024 * 1024; });
                if (stopping) return;
                readyBytes += bytes;
                ++loaded;
                std::fprintf(stderr, "[textures] loaded %s (%ux%u, %.1f MiB)\n", files[id].path.filename().string().c_str(),
                             image->width, image->height, bytes / 1048576.0);
                ready.push_back(std::move(image));
            }
        }
    };

    TextureReplacements::TextureReplacements() : m(std::make_unique<Impl>())
    {
        const char *enabled = std::getenv("VCS_TEXTURE_REPLACEMENTS");
        if (!enabled || enabled[0] != '1') return;
        const char *dir = std::getenv("VCS_TEXTURE_PACK");
        if (!dir || !*dir)
        {
            std::fprintf(stderr, "[textures] no pack directory configured; keeping originals\n");
            return;
        }
        const auto *utf8 = reinterpret_cast<const char8_t *>(dir);
        auto root = std::filesystem::path(std::u8string_view(utf8));
        std::error_code ec;
        if (std::filesystem::is_directory(root / "replacements", ec)) root /= "replacements";
        std::vector<std::filesystem::path> paths;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec)) paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end()); // deterministic duplicate handling
        uint32_t rejected = 0, duplicates = 0;
        for (const auto &path : paths)
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext != ".png") continue;
            Name n;
            if (!parseName(path.stem().string(), n)) { ++rejected; continue; }
            if (!m->names.emplace(n, uint32_t(m->files.size())).second) { ++duplicates; continue; }
            m->files.push_back({path});
            m->shapes.insert(n.bits & 0x3FFFu);
        }
        std::fprintf(stderr, "[textures] indexed %zu PNG replacements in %s (%u unsupported names, %u duplicates); cache %zu MiB%s\n",
                     m->names.size(), root.string().c_str(), rejected, duplicates, m->gpuBudget >> 20, ec ? "; directory scan failed" : "");
        if (!m->names.empty()) m->worker = std::thread([this] { m->run(); });
    }
    TextureReplacements::~TextureReplacements()
    {
        { std::lock_guard lock(m->mutex); m->stopping = true; }
        m->cv.notify_all();
        if (m->worker.joinable()) m->worker.join();
        if (!m->names.empty())
            std::fprintf(stderr, "[textures] summary: %u/%zu pack files matched, %u image loads\n", m->matched, m->names.size(), m->loaded);
    }
    bool TextureReplacements::candidate(uint32_t bits) const { return m->shapes.contains(bits & 0x3FFFu); }
    uint32_t TextureReplacements::find(uint64_t pixels, uint64_t palette, uint32_t bits)
    {
        // Opt-in diagnostic: exact names computed by the renderer (never texture data).
        static const bool trace = std::getenv("VCS_TEXTURE_TRACE") != nullptr;
        if (trace)
            std::fprintf(stderr, "[textures] lookup %llx-%llx-%08x\n", (unsigned long long)pixels,
                         (unsigned long long)palette, bits);
        const auto it = m->names.find({pixels, palette, bits});
        if (it == m->names.end()) return 0;
        auto &file = m->files[it->second];
        if (!file.matched)
        {
            file.matched = true; ++m->matched;
            std::fprintf(stderr, "[textures] matched %s (%u/%zu)\n", file.path.filename().string().c_str(), m->matched, m->names.size());
        }
        return it->second;
    }
    void TextureReplacements::request(uint32_t id)
    {
        std::lock_guard lock(m->mutex);
        auto &file = m->files.at(id);
        if (file.pending || file.failed) return;
        file.pending = true;
        m->queue.push_back(id);
        m->cv.notify_all();
    }
    std::vector<std::shared_ptr<const ReplacementImage>> TextureReplacements::takeReady()
    {
        std::vector<std::shared_ptr<const ReplacementImage>> result;
        if (!m->worker.joinable()) return result;
        {
            std::lock_guard lock(m->mutex);
            result.swap(m->ready);
            m->readyBytes = 0;
            for (const auto &image : result) m->files[image->id].pending = false;
        }
        m->cv.notify_all();
        return result;
    }
    size_t TextureReplacements::budget() const { return m->gpuBudget; }
}
