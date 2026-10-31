// PCSX2-named PNG packs. Index and requests: GS stage; decoding: one worker; GPU upload: main thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace render
{
    uint64_t textureHash(const void *data, size_t bytes);

    struct ReplacementImage
    {
        uint32_t id = 0, width = 0, height = 0;
        std::vector<uint32_t> pixels; // unmodified RGBA8: PCSX2 packs keep GS alpha (128 = opaque)
    };

    class TextureReplacements
    {
    public:
        TextureReplacements();
        ~TextureReplacements();
        TextureReplacements(const TextureReplacements &) = delete;
        TextureReplacements &operator=(const TextureReplacements &) = delete;

        bool candidate(uint32_t bits) const;
        uint32_t find(uint64_t pixels, uint64_t palette, uint32_t bits);
        void request(uint32_t id);
        std::vector<std::shared_ptr<const ReplacementImage>> takeReady();
        size_t budget() const;
        static constexpr uint64_t key(uint32_t id) { return 0xFFFF545800000000ull | id; }

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
