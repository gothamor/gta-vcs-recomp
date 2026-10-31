// See gs_shaders.h. The headers come from src/render/CMakeLists.txt: SPIR-V from tools/hlsl_spirv.py (shaders/spirv).
#include "render/gs_shaders.h"

#include "spirv/gs_draw_vs.h"
#include "spirv/gs_world_vs.h"
#include "spirv/gs_lit_vs.h"
#include "spirv/gs_draw_ps0.h"
#include "spirv/gs_draw_ps0_dual.h"
#include "spirv/gs_draw_ps1.h"
#include "spirv/gs_draw_ps1_dual.h"
#include "spirv/gs_draw_ps2.h"
#include "spirv/gs_draw_ps2_dual.h"
#include "spirv/gs_draw_ps3.h"
#include "spirv/gs_draw_ps3_dual.h"
#include "spirv/gs_draw_ps4.h"
#include "spirv/gs_draw_ps4_dual.h"
#include "spirv/gs_draw_ps5.h"
#include "spirv/gs_draw_ps5_dual.h"
#include "spirv/gs_draw_ps6.h"
#include "spirv/gs_draw_ps6_dual.h"
#include "spirv/gs_quad_vs.h"
#include "spirv/gs_blit_ps.h"
#include "spirv/gs_fill_color_ps.h"
#include "spirv/gs_fill_depth_ps.h"
#include "spirv/present_vs.h"
#include "spirv/present_ps.h"
#include "spirv/ui_vs.h"
#include "spirv/ui_ps.h"

namespace render
{
    const GsShaders &gsShaders(rhi::Backend backend)
    {
        static const GsShaders spirv{
            {kGsDrawVS_spv, sizeof(kGsDrawVS_spv)},
            {kGsWorldVS_spv, sizeof(kGsWorldVS_spv)},
            {kGsLitVS_spv, sizeof(kGsLitVS_spv)},
            {
                {{kGsDrawPS0_spv, sizeof(kGsDrawPS0_spv)}, {kGsDrawPS0Dual_spv, sizeof(kGsDrawPS0Dual_spv)}},
                {{kGsDrawPS1_spv, sizeof(kGsDrawPS1_spv)}, {kGsDrawPS1Dual_spv, sizeof(kGsDrawPS1Dual_spv)}},
                {{kGsDrawPS2_spv, sizeof(kGsDrawPS2_spv)}, {kGsDrawPS2Dual_spv, sizeof(kGsDrawPS2Dual_spv)}},
                {{kGsDrawPS3_spv, sizeof(kGsDrawPS3_spv)}, {kGsDrawPS3Dual_spv, sizeof(kGsDrawPS3Dual_spv)}},
                {{kGsDrawPS4_spv, sizeof(kGsDrawPS4_spv)}, {kGsDrawPS4Dual_spv, sizeof(kGsDrawPS4Dual_spv)}},
                {{kGsDrawPS5_spv, sizeof(kGsDrawPS5_spv)}, {kGsDrawPS5Dual_spv, sizeof(kGsDrawPS5Dual_spv)}},
                {{kGsDrawPS6_spv, sizeof(kGsDrawPS6_spv)}, {kGsDrawPS6Dual_spv, sizeof(kGsDrawPS6Dual_spv)}},
            },
            {kGsQuadVS_spv, sizeof(kGsQuadVS_spv)},
            {kGsBlitPS_spv, sizeof(kGsBlitPS_spv)},
            {kGsFillColorPS_spv, sizeof(kGsFillColorPS_spv)},
            {kGsFillDepthPS_spv, sizeof(kGsFillDepthPS_spv)},
            {kPresentVS_spv, sizeof(kPresentVS_spv)},
            {kPresentPS_spv, sizeof(kPresentPS_spv)},
            {kUiVS_spv, sizeof(kUiVS_spv)},
            {kUiPS_spv, sizeof(kUiPS_spv)},
        };
        if (backend == rhi::Backend::Vulkan)
            return spirv;
        static const GsShaders none{};
        return none;
    }
}
