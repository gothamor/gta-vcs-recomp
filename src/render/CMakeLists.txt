# Renderer pieces on top of the RHI: the window presenter and the HW GS renderer's shaders (docs/NATIVE_RENDERER.md).
# HLSL shaders are compiled at build time with fxc into headers (DXBC for the D3D11 backend).
file(GLOB VCS_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT VCS_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
find_program(VCS_FXC fxc HINTS ${VCS_FXC_CANDIDATES})
if(NOT VCS_FXC AND VCS_FXC_CANDIDATES)
    list(GET VCS_FXC_CANDIDATES 0 VCS_FXC)
endif()
if(NOT VCS_FXC)
    message(FATAL_ERROR "fxc.exe (Windows SDK) not found: install the Windows 10/11 SDK")
endif()

set(VCS_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VCS_SHADER_OUT}")
set(VCS_SHADER_HEADERS "")
# vcs_shader(<file.hlsl> <entry> <profile> <array name> <output header> [NAME=VALUE defines...])
function(vcs_shader source entry profile name header)
    set(out "${VCS_SHADER_OUT}/${header}")
    set(defines "")
    foreach(d IN LISTS ARGN)
        list(APPEND defines /D ${d})
    endforeach()
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${VCS_FXC}" /nologo /O3 /T ${profile} /E ${entry} /Vn ${name} ${defines} /Fh "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        COMMENT "fxc ${source} ${entry}"
        VERBATIM)
    set(VCS_SHADER_HEADERS ${VCS_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
vcs_shader(shaders/present.hlsl vs_main vs_5_0 kPresentVS present_vs.h)
vcs_shader(shaders/present.hlsl ps_main ps_5_0 kPresentPS present_ps.h)
vcs_shader(shaders/gs_draw.hlsl vs_main vs_5_0 kGsDrawVS gs_draw_vs.h)
vcs_shader(shaders/gs_draw.hlsl vs_world vs_5_0 kGsWorldVS gs_world_vs.h)
vcs_shader(shaders/gs_draw.hlsl vs_lit vs_5_0 kGsLitVS gs_lit_vs.h)
vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPS gs_draw_ps.h DUAL_SOURCE=0)
vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPSDual gs_draw_ps_dual.h DUAL_SOURCE=1)
vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPSGather gs_draw_ps_gather.h DUAL_SOURCE=0 GATHER_ONLY=1)
vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPSGatherDual gs_draw_ps_gather_dual.h DUAL_SOURCE=1 GATHER_ONLY=1)
vcs_shader(shaders/gs_util.hlsl vs_main vs_5_0 kGsQuadVS gs_quad_vs.h)
vcs_shader(shaders/gs_util.hlsl ps_blit ps_5_0 kGsBlitPS gs_blit_ps.h)
vcs_shader(shaders/gs_util.hlsl ps_fill_color ps_5_0 kGsFillColorPS gs_fill_color_ps.h)
vcs_shader(shaders/gs_util.hlsl ps_fill_depth ps_5_0 kGsFillDepthPS gs_fill_depth_ps.h)

add_library(vcs_render STATIC
    presenter.cpp
    gs_shaders.cpp
    ${VCS_SHADER_HEADERS}
)
target_include_directories(vcs_render PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${VCS_SHADER_OUT}")
target_link_libraries(vcs_render PUBLIC vcs_rhi vcs_platform imgui)
