// See ui_renderer.h.
#include "render/ui_renderer.h"

#include "render/gs_shaders.h"
#include "rhi/rhi.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace render
{
    namespace
    {
        // ImTextureData as RGBA rows (Alpha8 atlases become white with that alpha)
        void rgbaRows(const ImTextureData &t, int x, int y, int w, int h, std::vector<unsigned char> &out)
        {
            out.resize(static_cast<size_t>(w) * h * 4);
            for (int row = 0; row < h; ++row)
            {
                const unsigned char *src = t.Pixels + (static_cast<size_t>(y + row) * t.Width + x) * t.BytesPerPixel;
                unsigned char *dst = out.data() + static_cast<size_t>(row) * w * 4;
                if (t.Format == ImTextureFormat_RGBA32)
                    std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                else
                    for (int i = 0; i < w; ++i)
                    {
                        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
                        dst[i * 4 + 3] = src[i];
                    }
            }
        }
    }

    bool UiRenderer::init(rhi::Device &device)
    {
        m_device = &device;
        const GsShaders &shaders = gsShaders(device.backend());
        rhi::PipelineDesc pd;
        pd.vertex = shaders.uiVS;
        pd.pixel = shaders.uiPS;
        pd.attributes = {
            {"POSITION", 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(ImDrawVert, pos))},
            {"TEXCOORD", 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(ImDrawVert, uv))},
            {"COLOR", 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(ImDrawVert, col))},
        };
        pd.stride = sizeof(ImDrawVert);
        pd.blend.enable = true;
        pd.blend.srcColor = rhi::BlendFactor::SrcAlpha;
        pd.blend.dstColor = rhi::BlendFactor::InvSrcAlpha;
        pd.blend.srcAlpha = rhi::BlendFactor::One;
        pd.blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
        pd.scissor = true;
        pd.name = "dear imgui";
        m_pipeline = device.createPipeline(pd);
        m_sampler = device.createSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
        m_constants = device.createBuffer({sizeof(float) * 4, rhi::BufferUsage::Constant, true, "imgui constants"});
        if (!m_pipeline || !m_sampler || !m_constants)
            return false;
        ImGuiIO &io = ImGui::GetIO();
        io.BackendRendererName = "vcs_rhi_ui";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        return true;
    }

    void UiRenderer::shutdown()
    {
        if (ImGui::GetCurrentContext())
        {
            // ImGui's textures lose their device copies (made again if a renderer starts later)
            for (ImTextureData *t : ImGui::GetPlatformIO().Textures)
                if (t->RefCount == 1)
                {
                    t->SetTexID(ImTextureID_Invalid);
                    t->SetStatus(ImTextureStatus_Destroyed);
                }
            ImGuiIO &io = ImGui::GetIO();
            io.BackendRendererName = nullptr;
            io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        }
        m_textures.clear();
        m_pipeline.reset();
        m_sampler.reset();
        m_constants.reset();
        m_vertices.reset();
        m_indices.reset();
        m_device = nullptr;
    }

    void UiRenderer::render(ImDrawData *data)
    {
        if (!m_device || !data)
            return;
        rhi::Device &dev = *m_device;
        std::vector<unsigned char> rows;
        if (data->Textures)
            for (ImTextureData *t : *data->Textures)
            {
                if (t->Status == ImTextureStatus_WantCreate)
                {
                    rhi::TextureDesc desc;
                    desc.width = static_cast<uint32_t>(t->Width);
                    desc.height = static_cast<uint32_t>(t->Height);
                    desc.name = "imgui texture";
                    rgbaRows(*t, 0, 0, t->Width, t->Height, rows);
                    std::unique_ptr<rhi::Texture> texture = dev.createTexture(desc, rows.data());
                    if (!texture)
                        continue;
                    size_t slot = 0;
                    while (slot < m_textures.size() && m_textures[slot])
                        ++slot;
                    if (slot == m_textures.size())
                        m_textures.emplace_back();
                    m_textures[slot] = std::move(texture);
                    t->SetTexID(static_cast<ImTextureID>(slot + 1));
                    t->SetStatus(ImTextureStatus_OK);
                }
                else if (t->Status == ImTextureStatus_WantUpdates)
                {
                    const size_t slot = static_cast<size_t>(t->GetTexID()) - 1;
                    if (slot < m_textures.size() && m_textures[slot])
                        for (const ImTextureRect &r : t->Updates)
                        {
                            rgbaRows(*t, r.x, r.y, r.w, r.h, rows);
                            dev.updateTexture(*m_textures[slot], r.x, r.y, r.w, r.h, rows.data());
                        }
                    t->SetStatus(ImTextureStatus_OK);
                }
                else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0)
                {
                    // the backend keeps a texture alive until the GPU is done with it
                    const size_t slot = static_cast<size_t>(t->GetTexID()) - 1;
                    if (slot < m_textures.size())
                        m_textures[slot].reset();
                    t->SetTexID(ImTextureID_Invalid);
                    t->SetStatus(ImTextureStatus_Destroyed);
                }
            }

        const float fbWidth = data->DisplaySize.x * data->FramebufferScale.x;
        const float fbHeight = data->DisplaySize.y * data->FramebufferScale.y;
        if (fbWidth <= 0.0f || fbHeight <= 0.0f || data->TotalVtxCount <= 0 || data->TotalIdxCount <= 0)
            return;

        // one vertex and one index buffer for every list (RendererHasVtxOffset: per-command base vertex)
        const size_t vertexCount = static_cast<size_t>(data->TotalVtxCount), indexCount = static_cast<size_t>(data->TotalIdxCount);
        if (!m_vertices || m_vertexCapacity < vertexCount)
        {
            m_vertexCapacity = vertexCount + 5000;
            m_vertices = dev.createBuffer({m_vertexCapacity * sizeof(ImDrawVert), rhi::BufferUsage::Vertex, true, "imgui vertices"});
        }
        if (!m_indices || m_indexCapacity < indexCount)
        {
            m_indexCapacity = indexCount + 10000;
            m_indices = dev.createBuffer({m_indexCapacity * sizeof(ImDrawIdx), rhi::BufferUsage::Index, true, "imgui indices"});
        }
        if (!m_vertices || !m_indices)
            return;
        m_vertexData.resize(vertexCount * sizeof(ImDrawVert));
        m_indexData.resize(indexCount * sizeof(ImDrawIdx));
        size_t vOff = 0, iOff = 0;
        for (const ImDrawList *list : data->CmdLists)
        {
            std::memcpy(m_vertexData.data() + vOff, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
            std::memcpy(m_indexData.data() + iOff, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
            vOff += list->VtxBuffer.Size * sizeof(ImDrawVert);
            iOff += list->IdxBuffer.Size * sizeof(ImDrawIdx);
        }
        dev.updateBuffer(*m_vertices, m_vertexData.data(), m_vertexData.size());
        dev.updateBuffer(*m_indices, m_indexData.data(), m_indexData.size());

        // display coordinates (DisplayPos .. + DisplaySize) to NDC, y up
        const float l = data->DisplayPos.x, t = data->DisplayPos.y, w = data->DisplaySize.x, h = data->DisplaySize.y;
        const float scaleOffset[4] = {2.0f / w, -2.0f / h, -1.0f - l * 2.0f / w, 1.0f + t * 2.0f / h};
        dev.updateBuffer(*m_constants, scaleOffset, sizeof(scaleOffset));

        dev.setViewport(0.0f, 0.0f, fbWidth, fbHeight);
        dev.setPipeline(*m_pipeline);
        dev.setVertexBuffer(*m_vertices);
        dev.setIndexBuffer(*m_indices, sizeof(ImDrawIdx) == 4);
        dev.setConstants(0, *m_constants);
        const ImVec2 clipOff = data->DisplayPos, clipScale = data->FramebufferScale;
        int baseVertex = 0;
        uint32_t baseIndex = 0;
        for (const ImDrawList *list : data->CmdLists)
        {
            for (const ImDrawCmd &cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback)
                {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                        cmd.UserCallback(list, &cmd);
                    continue;
                }
                const float x0 = std::max((cmd.ClipRect.x - clipOff.x) * clipScale.x, 0.0f);
                const float y0 = std::max((cmd.ClipRect.y - clipOff.y) * clipScale.y, 0.0f);
                const float x1 = std::min((cmd.ClipRect.z - clipOff.x) * clipScale.x, fbWidth);
                const float y1 = std::min((cmd.ClipRect.w - clipOff.y) * clipScale.y, fbHeight);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                const size_t slot = static_cast<size_t>(cmd.GetTexID()) - 1;
                rhi::Texture *texture = slot < m_textures.size() ? m_textures[slot].get() : nullptr;
                if (!texture)
                    continue;
                dev.setScissor(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(std::ceil(x1 - x0)),
                               static_cast<int>(std::ceil(y1 - y0)));
                dev.setTexture(0, texture, m_sampler.get());
                dev.drawIndexed(cmd.ElemCount, baseIndex + cmd.IdxOffset, baseVertex + static_cast<int>(cmd.VtxOffset));
            }
            baseVertex += list->VtxBuffer.Size;
            baseIndex += static_cast<uint32_t>(list->IdxBuffer.Size);
        }
        dev.setTexture(0, nullptr, nullptr);
        dev.setScissor(0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight));
    }
}
