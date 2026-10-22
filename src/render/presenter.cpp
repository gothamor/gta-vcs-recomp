// See presenter.h.
#include "render/presenter.h"

#include "rhi/rhi.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

// fxc's /Fh headers declare `const BYTE name[]`
using BYTE = unsigned char;
#include "present_ps.h"
#include "present_vs.h"

namespace present
{
    namespace
    {
        struct Params
        {
            float rect[4];  // NDC left, top, right, bottom
            float uvMax[2];
            float pad[2];
        };

        std::unique_ptr<rhi::Device> g_device;
        std::unique_ptr<rhi::Pipeline> g_pipeline;
        std::unique_ptr<rhi::Buffer> g_params;
        std::unique_ptr<rhi::Sampler> g_nearest, g_linear;
        std::unique_ptr<rhi::Texture> g_captureTarget;
        std::unique_ptr<rhi::Texture> g_lumaTarget; // frameLuma
        std::vector<uint8_t> g_captureRgba;

        std::atomic<float> g_displayAspect{4.0f / 3.0f};

        // The bound pass's target is targetWidth x targetHeight; the src region is shown at display aspect `aspect`.
        void drawPicture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int targetWidth, int targetHeight)
        {
            if (!picture.texture || srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 || aspect <= 0.0f)
                return;
            float w = static_cast<float>(targetWidth), h = w / aspect;
            if (h > targetHeight)
            {
                h = static_cast<float>(targetHeight);
                w = h * aspect;
            }
            const float x0 = (targetWidth - w) * 0.5f, y0 = (targetHeight - h) * 0.5f;
            auto ndcX = [&](float px) { return px / targetWidth * 2.0f - 1.0f; };
            auto ndcY = [&](float py) { return 1.0f - py / targetHeight * 2.0f; };
            const rhi::TextureDesc &td = picture.texture->desc();
            const Params params{{ndcX(x0), ndcY(y0), ndcX(x0 + w), ndcY(y0 + h)},
                                {static_cast<float>(srcWidth) / td.width, static_cast<float>(srcHeight) / td.height},
                                {0.0f, 0.0f}};
            g_device->updateBuffer(*g_params, &params, sizeof(params));
            g_device->setPipeline(*g_pipeline);
            g_device->setConstants(0, *g_params);
            g_device->setTexture(0, picture.texture.get(), picture.linear ? g_linear.get() : g_nearest.get());
            g_device->draw(4);
            g_device->setTexture(0, nullptr, nullptr);
        }
    }

    bool init(void *nativeWindow, int width, int height)
    {
        rhi::DeviceDesc desc;
        desc.nativeWindow = nativeWindow;
        desc.width = static_cast<uint32_t>(std::max(width, 1));
        desc.height = static_cast<uint32_t>(std::max(height, 1));
        const char *debug = std::getenv("PS2X_D3D_DEBUG"); // D3D11 debug layer (Graphics Tools must be installed)
        desc.debug = debug && *debug && *debug != '0';
        std::string error;
        g_device = rhi::createD3D11Device(desc, error);
        if (!g_device)
        {
            std::fprintf(stderr, "[present] %s\n", error.c_str());
            return false;
        }
        std::fprintf(stderr, "[present] D3D11 on %s\n", g_device->adapterName().c_str());

        rhi::PipelineDesc pd;
        pd.vertex = {kPresentVS, sizeof(kPresentVS)};
        pd.pixel = {kPresentPS, sizeof(kPresentPS)};
        pd.topology = rhi::Topology::TriangleStrip;
        // alpha-blended over black, as raylib's DrawTexturePro did
        pd.blend.enable = true;
        pd.blend.srcColor = pd.blend.srcAlpha = rhi::BlendFactor::SrcAlpha;
        pd.blend.dstColor = pd.blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
        pd.name = "present";
        g_pipeline = g_device->createPipeline(pd);
        g_params = g_device->createBuffer({sizeof(Params), rhi::BufferUsage::Constant, true, "present params"});
        g_nearest = g_device->createSampler({rhi::Filter::Nearest, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
        g_linear = g_device->createSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
        if (!g_pipeline || !g_params || !g_nearest || !g_linear)
        {
            std::fprintf(stderr, "[present] could not create the present pipeline\n");
            return false;
        }
        if (!g_device->uiInit())
            std::fprintf(stderr, "[present] Dear ImGui renderer failed\n");
        return true;
    }

    void shutdown()
    {
        g_captureTarget.reset();
        g_pipeline.reset();
        g_params.reset();
        g_nearest.reset();
        g_linear.reset();
        g_device.reset();
    }

    rhi::Device *device() { return g_device.get(); }

    Picture createPicture(int width, int height, bool linear)
    {
        Picture p;
        rhi::TextureDesc desc;
        desc.width = static_cast<uint32_t>(std::max(width, 1));
        desc.height = static_cast<uint32_t>(std::max(height, 1));
        desc.usage = rhi::UsageSampled | rhi::UsageDynamic;
        desc.name = "picture";
        p.texture = g_device->createTexture(desc);
        p.width = width;
        p.height = height;
        p.linear = linear;
        return p;
    }

    void updatePicture(const Picture &picture, const void *rgba)
    {
        if (picture.texture)
            g_device->updateTexture(*picture.texture, 0, 0, static_cast<uint32_t>(picture.width), static_cast<uint32_t>(picture.height),
                                    rgba);
    }

    void beginFrame() { g_device->uiNewFrame(); }

    void setDisplayAspect(float aspect) { g_displayAspect.store(aspect, std::memory_order_relaxed); }

    float displayAspect(bool showing3d) { return showing3d ? g_displayAspect.load(std::memory_order_relaxed) : 4.0f / 3.0f; }

    void drawWindow(const Picture &picture, int srcWidth, int srcHeight, float aspect, int windowWidth, int windowHeight)
    {
        if (windowWidth > 0 && windowHeight > 0)
            g_device->resize(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight));
        uint32_t w = 0, h = 0;
        g_device->backbufferSize(w, h);
        rhi::PassDesc pass;
        pass.clearColor = true;
        pass.name = "window";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, aspect, static_cast<int>(w), static_cast<int>(h));
        g_device->uiRender(ImGui::GetDrawData());
        g_device->endPass();
    }

    void present() { g_device->present(false); }

    bool frameLuma(const Picture &picture, int srcWidth, int srcHeight, float &mean)
    {
        constexpr int kW = 32, kH = 24;
        if (!picture.texture || srcWidth <= 0 || srcHeight <= 0)
            return false;
        if (!g_lumaTarget)
        {
            rhi::TextureDesc desc;
            desc.width = kW;
            desc.height = kH;
            desc.usage = rhi::UsageRenderTarget | rhi::UsageSampled;
            desc.name = "luma probe";
            g_lumaTarget = g_device->createTexture(desc);
            if (!g_lumaTarget)
                return false;
        }
        rhi::PassDesc pass;
        pass.color = g_lumaTarget.get();
        pass.clearColor = true;
        pass.name = "luma probe";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, static_cast<float>(kW) / kH, kW, kH);
        g_device->endPass();
        uint8_t rgba[kW * kH * 4];
        if (!g_device->readTexture(*g_lumaTarget, 0, 0, kW, kH, rgba))
            return false;
        // the middle cells only (20%..80% across, 25%..80% down): the HUD stays out
        double sum = 0.0;
        int n = 0;
        for (int y = kH / 4; y < kH * 4 / 5; ++y)
            for (int x = kW / 5; x < kW * 4 / 5; ++x, ++n)
            {
                const uint8_t *px = rgba + (y * kW + x) * 4;
                sum += 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
            }
        mean = static_cast<float>(sum / n);
        return true;
    }

    bool capture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int width, int height, std::vector<uint8_t> &rgb)
    {
        if (width <= 0 || height <= 0)
            return false;
        if (!g_captureTarget || g_captureTarget->desc().width != static_cast<uint32_t>(width) ||
            g_captureTarget->desc().height != static_cast<uint32_t>(height))
        {
            rhi::TextureDesc desc;
            desc.width = static_cast<uint32_t>(width);
            desc.height = static_cast<uint32_t>(height);
            desc.usage = rhi::UsageRenderTarget | rhi::UsageSampled;
            desc.name = "capture";
            g_captureTarget = g_device->createTexture(desc);
            if (!g_captureTarget)
                return false;
        }
        rhi::PassDesc pass;
        pass.color = g_captureTarget.get();
        pass.clearColor = true;
        pass.name = "capture";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, aspect, width, height);
        if (ImDrawData *data = ImGui::GetDrawData(); data && data->Valid && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f)
        {
            ImDrawData scaled = *data; // the window's UI, stretched to the capture size
            scaled.FramebufferScale = ImVec2(width / data->DisplaySize.x, height / data->DisplaySize.y);
            g_device->uiRender(&scaled);
        }
        g_device->endPass();
        g_captureRgba.resize(static_cast<size_t>(width) * height * 4);
        if (!g_device->readTexture(*g_captureTarget, 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                   g_captureRgba.data()))
            return false;
        rgb.resize(static_cast<size_t>(width) * height * 3);
        for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
        {
            rgb[i * 3 + 0] = g_captureRgba[i * 4 + 0];
            rgb[i * 3 + 1] = g_captureRgba[i * 4 + 1];
            rgb[i * 3 + 2] = g_captureRgba[i * 4 + 2];
        }
        return true;
    }
}
