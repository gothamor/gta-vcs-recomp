// See gs_shaders.h. The headers come from src/render/CMakeLists.txt: DXBC from fxc (shaders/dxbc), SPIR-V from
// tools/hlsl_spirv.py (shaders/spirv).
#include "render/gs_shaders.h"

#if defined(VCS_HAVE_D3D11)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/gs_draw_vs.h"
#include "dxbc/gs_world_vs.h"
#include "dxbc/gs_lit_vs.h"
#include "dxbc/gs_draw_ps.h"
#include "dxbc/gs_draw_ps_dual.h"
#include "dxbc/gs_draw_ps_gather.h"
#include "dxbc/gs_draw_ps_gather_dual.h"
#include "dxbc/gs_quad_vs.h"
#include "dxbc/gs_blit_ps.h"
#include "dxbc/gs_fill_color_ps.h"
#include "dxbc/gs_fill_depth_ps.h"
#include "dxbc/present_vs.h"
#include "dxbc/present_ps.h"
#include "dxbc/ui_vs.h"
#include "dxbc/ui_ps.h"
#endif
#if defined(VCS_HAVE_VULKAN)
#include "spirv/gs_draw_vs.h"
#include "spirv/gs_world_vs.h"
#include "spirv/gs_lit_vs.h"
#include "spirv/gs_draw_ps.h"
#include "spirv/gs_draw_ps_dual.h"
#include "spirv/gs_draw_ps_gather.h"
#include "spirv/gs_draw_ps_gather_dual.h"
#include "spirv/gs_quad_vs.h"
#include "spirv/gs_blit_ps.h"
#include "spirv/gs_fill_color_ps.h"
#include "spirv/gs_fill_depth_ps.h"
#include "spirv/present_vs.h"
#include "spirv/present_ps.h"
#include "spirv/ui_vs.h"
#include "spirv/ui_ps.h"
#endif

namespace render
{
    const GsShaders &gsShaders(rhi::Backend backend)
    {
#if defined(VCS_HAVE_D3D11)
        static const GsShaders dxbc{
            {kGsDrawVS, sizeof(kGsDrawVS)},
            {kGsWorldVS, sizeof(kGsWorldVS)},
            {kGsLitVS, sizeof(kGsLitVS)},
            {kGsDrawPS, sizeof(kGsDrawPS)},
            {kGsDrawPSDual, sizeof(kGsDrawPSDual)},
            {kGsDrawPSGather, sizeof(kGsDrawPSGather)},
            {kGsDrawPSGatherDual, sizeof(kGsDrawPSGatherDual)},
            {kGsQuadVS, sizeof(kGsQuadVS)},
            {kGsBlitPS, sizeof(kGsBlitPS)},
            {kGsFillColorPS, sizeof(kGsFillColorPS)},
            {kGsFillDepthPS, sizeof(kGsFillDepthPS)},
            {kPresentVS, sizeof(kPresentVS)},
            {kPresentPS, sizeof(kPresentPS)},
            {kUiVS, sizeof(kUiVS)},
            {kUiPS, sizeof(kUiPS)},
        };
        if (backend == rhi::Backend::D3D11)
            return dxbc;
#endif
#if defined(VCS_HAVE_VULKAN)
        static const GsShaders spirv{
            {kGsDrawVS_spv, sizeof(kGsDrawVS_spv)},
            {kGsWorldVS_spv, sizeof(kGsWorldVS_spv)},
            {kGsLitVS_spv, sizeof(kGsLitVS_spv)},
            {kGsDrawPS_spv, sizeof(kGsDrawPS_spv)},
            {kGsDrawPSDual_spv, sizeof(kGsDrawPSDual_spv)},
            {kGsDrawPSGather_spv, sizeof(kGsDrawPSGather_spv)},
            {kGsDrawPSGatherDual_spv, sizeof(kGsDrawPSGatherDual_spv)},
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
#endif
        (void)backend;
        static const GsShaders none{};
        return none;
    }
}
