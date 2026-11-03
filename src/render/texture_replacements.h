// PCSX2-named texture packs (DDS and PNG). Index and requests: GS stage; loading: one worker; GPU upload: main thread.
#pragma once

#include "rhi/rhi.h"

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
        std::vector<uint32_t> pixels; // PNG: level 0, unmodified RGBA8 (PCSX2 packs keep GS alpha: 128 = opaque)
        // DDS: the file's levels as stored (RGBA8, BGRA8 or block-compressed), level 0 first, nothing decoded
        rhi::Format format = rhi::Format::RGBA8;
        std::vector<uint8_t> levels;
        std::vector<size_t> levelOffsets; // into `levels`, one per level
        bool dds() const { return !levelOffsets.empty(); }
        // its texture's size on the GPU: a DDS's own levels (level 0 alone without a chain), a PNG's chain made by the GPU
        size_t gpuBytes(bool mipChain) const;
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
        // Whether the device can make block-compressed textures (rhi::Device::blockCompression): set before the first
        // request; without it a block-compressed DDS keeps the original texture.
        void setBlockCompression(bool supported);
        static constexpr uint64_t key(uint32_t id) { return 0xFFFF545800000000ull | id; }

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
