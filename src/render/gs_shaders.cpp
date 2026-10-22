// See gs_shaders.h. DXBC from fxc (src/render/CMakeLists.txt).
#include "render/gs_shaders.h"

using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "gs_draw_ps.h"
#include "gs_draw_ps_dual.h"
#include "gs_draw_vs.h"
#include "gs_world_vs.h"
#include "gs_quad_vs.h"
#include "gs_blit_ps.h"
#include "gs_fill_color_ps.h"
#include "gs_fill_depth_ps.h"

namespace render
{
    const GsShaders &gsShaders()
    {
        static const GsShaders shaders{
            {kGsDrawVS, sizeof(kGsDrawVS)},
            {kGsWorldVS, sizeof(kGsWorldVS)},
            {kGsDrawPS, sizeof(kGsDrawPS)},
            {kGsDrawPSDual, sizeof(kGsDrawPSDual)},
            {kGsQuadVS, sizeof(kGsQuadVS)},
            {kGsBlitPS, sizeof(kGsBlitPS)},
            {kGsFillColorPS, sizeof(kGsFillColorPS)},
            {kGsFillDepthPS, sizeof(kGsFillDepthPS)},
        };
        return shaders;
    }
}
