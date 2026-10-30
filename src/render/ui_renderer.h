// Dear ImGui drawn through the RHI (shaders/ui.hlsl), for backends without an ImGui renderer of their own (Vulkan; the
// D3D11 backend keeps imgui_impl_dx11). Its pipeline works on any target the backend draws to, the swapchain and the
// RGBA capture targets alike. Main thread.
#pragma once

#include <memory>
#include <vector>

struct ImDrawData;

namespace rhi
{
    class Buffer;
    class Device;
    class Pipeline;
    class Sampler;
    class Texture;
}

namespace render
{
    class UiRenderer
    {
    public:
        bool init(rhi::Device &device);
        void shutdown();
        // Into the current pass (its target is FramebufferScale * DisplaySize); creates / updates / destroys the
        // textures ImGui asks for first.
        void render(ImDrawData *data);

    private:
        rhi::Device *m_device = nullptr;
        std::unique_ptr<rhi::Pipeline> m_pipeline;
        std::unique_ptr<rhi::Sampler> m_sampler;
        std::unique_ptr<rhi::Buffer> m_constants, m_vertices, m_indices;
        size_t m_vertexCapacity = 0, m_indexCapacity = 0; // in vertices / indices
        std::vector<unsigned char> m_vertexData, m_indexData;
        std::vector<std::unique_ptr<rhi::Texture>> m_textures; // ImTextureID = index + 1
    };
}
