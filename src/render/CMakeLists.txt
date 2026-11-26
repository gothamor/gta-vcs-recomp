# Renderer pieces on top of the RHI: the window presenter and the HW GS renderer's shaders (docs/NATIVE_RENDERER.md).
# One HLSL source per shader (the list: shaders/shaders.txt), as headers for each backend that is built:
#   D3D11: DXBC compiled now by D3DCompile (tools/hlsl_dxbc.py) -> shaders/dxbc/<header>, array <name>
#   Vulkan: the committed SPIR-V (shaders/compiled/spirv) -> shaders/spirv/<header>, array <name>_spv
#   Metal: the committed MSL text (shaders/compiled/msl) -> shaders/msl/<header>, array <name>_msl
# The SPIR-V and MSL are made once by `python tools/shaders.py build` (glslang + SPIRV-Cross) and committed: no shader
# compiler on a player's machine (DECISIONS.md 2026-11-27).
set(VCS_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(VCS_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
file(MAKE_DIRECTORY "${VCS_SHADER_OUT}/dxbc" "${VCS_SHADER_OUT}/spirv" "${VCS_SHADER_OUT}/msl")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# D3D11: Windows' own d3dcompiler_47.dll through tools/hlsl_dxbc.py, the same DXBC as the SDK's fxc (checked
# byte for byte; DECISIONS.md 2026-11-03) - no Windows SDK. VCS_DXBC_CHECK_FXC=<fxc.exe> compares every shader with
# that fxc's output and fails on a difference.
set(VCS_DXBC_CHECK_FXC "" CACHE FILEPATH "fxc.exe to compare tools/hlsl_dxbc.py's DXBC with (empty: no check)")
set(VCS_SHADER_HEADERS "")
# vcs_shader(<file.hlsl> <entry> <profile> <array name> <output header> [NAME=VALUE defines...])
function(vcs_shader source entry profile name header)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    if(VCS_RHI_D3D11)
        set(out "${VCS_SHADER_OUT}/dxbc/${header}")
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines -D ${d})
        endforeach()
        set(check "")
        if(VCS_DXBC_CHECK_FXC)
            set(check --check-fxc "${VCS_DXBC_CHECK_FXC}")
        endif()
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/hlsl_dxbc.py" --profile ${profile}
                    --entry ${entry} --name ${name} --out "${out}" ${defines} ${check} "${src}"
            DEPENDS "${src}" "${CMAKE_SOURCE_DIR}/tools/hlsl_dxbc.py"
            COMMENT "hlsl_dxbc ${source} ${entry}"
            VERBATIM)
        list(APPEND VCS_SHADER_HEADERS "${out}")
    endif()
    string(REGEX REPLACE "\\.h$" "" base "${header}")
    if(VCS_RHI_VULKAN)
        set(out "${VCS_SHADER_OUT}/spirv/${header}")
        set(compiled "${VCS_SHADER_DIR}/compiled/spirv/${base}.spv")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/shaders.py" header --spirv "${compiled}"
                    --name ${name}_spv --source "${src}" --out "${out}"
            DEPENDS "${compiled}" "${CMAKE_SOURCE_DIR}/tools/shaders.py" "${CMAKE_SOURCE_DIR}/tools/hlsl_spirv.py"
            COMMENT "spirv ${header}"
            VERBATIM)
        list(APPEND VCS_SHADER_HEADERS "${out}")
    endif()
    if(VCS_RHI_METAL)
        set(out "${VCS_SHADER_OUT}/msl/${header}")
        set(compiled "${VCS_SHADER_DIR}/compiled/msl/${base}.metal")
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines -D ${d})
        endforeach()
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/shaders.py" header --msl "${compiled}"
                    --name ${name}_msl --source "${src}" --entry ${entry} ${defines} --out "${out}"
            DEPENDS "${compiled}" "${CMAKE_SOURCE_DIR}/tools/shaders.py" "${CMAKE_SOURCE_DIR}/tools/hlsl_msl.py"
            COMMENT "msl ${header}"
            VERBATIM)
        list(APPEND VCS_SHADER_HEADERS "${out}")
    endif()
    set(VCS_SHADER_HEADERS ${VCS_SHADER_HEADERS} PARENT_SCOPE)
endfunction()
# the list (shaders/shaders.txt): file entry profile name header [DEFINE=VALUE ...]
file(STRINGS "${VCS_SHADER_DIR}/shaders.txt" _vcs_shader_lines)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS_SHADER_DIR}/shaders.txt")
foreach(line IN LISTS _vcs_shader_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^target-env ")
        continue()
    endif()
    separate_arguments(parts UNIX_COMMAND "${line}")
    list(GET parts 0 file)
    list(GET parts 1 entry)
    list(GET parts 2 profile)
    list(GET parts 3 name)
    list(GET parts 4 header)
    set(defines "")
    list(LENGTH parts _count)
    if(_count GREATER 5)
        list(SUBLIST parts 5 -1 defines)
    endif()
    vcs_shader(shaders/${file} ${entry} ${profile} ${name} ${header} ${defines})
endforeach()

# The committed SPIR-V / MSL against the HLSL they were made from: a warning when a shader changed without
# `python tools/shaders.py build` (tools/build.py runs it itself when it sees the change)
file(GLOB _vcs_hlsl CONFIGURE_DEPENDS "${VCS_SHADER_DIR}/*.hlsl")
set(_vcs_shader_stamp "${VCS_SHADER_OUT}/compiled_check.txt")
add_custom_command(
    OUTPUT "${_vcs_shader_stamp}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/shaders.py" check --warn
            --manifest src/render/shaders/shaders.txt --stamp "${_vcs_shader_stamp}"
    DEPENDS ${_vcs_hlsl} "${VCS_SHADER_DIR}/shaders.txt" "${VCS_SHADER_DIR}/compiled/manifest.json"
            "${CMAKE_SOURCE_DIR}/tools/hlsl_spirv.py" "${CMAKE_SOURCE_DIR}/tools/hlsl_msl.py"
            "${CMAKE_SOURCE_DIR}/tools/shader_tools/CMakeLists.txt"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "shaders.py check"
    VERBATIM)
list(APPEND VCS_SHADER_HEADERS "${_vcs_shader_stamp}")

add_library(vcs_render STATIC
    presenter.cpp
    gs_shaders.cpp
    ui_renderer.cpp
    ${VCS_SHADER_HEADERS}
)
target_include_directories(vcs_render PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${VCS_SHADER_OUT}")
target_link_libraries(vcs_render PUBLIC vcs_rhi vcs_platform imgui)
