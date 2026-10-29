// Compiled shaders of the HW GS renderer (shaders/gs_draw.hlsl, shaders/gs_util.hlsl) for the current RHI backend.
#pragma once

#include "rhi/rhi.h"

namespace render
{
    struct GsShaders
    {
        rhi::ShaderCode drawVS;
        rhi::ShaderCode worldVS;      // native VCS world draws
        rhi::ShaderCode litVS;        // native VCS lit / skinned models
        rhi::ShaderCode drawPS;       // one colour output
        rhi::ShaderCode drawPSDual;   // + blend factor as the second output (Src1Alpha blending)
        rhi::ShaderCode drawPSGather; // drawPS for bilinear game textures fetched by Gather only (GATHER_ONLY: fewer registers)
        rhi::ShaderCode drawPSGatherDual;
        rhi::ShaderCode quadVS;
        rhi::ShaderCode blitPS;
        rhi::ShaderCode fillColorPS;
        rhi::ShaderCode fillDepthPS;
    };

    const GsShaders &gsShaders();
}
