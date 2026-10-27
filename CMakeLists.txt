cmake_minimum_required(VERSION 3.21)
project(vcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.py). Never committed.
set(VCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")

set(PS2X_LTO "THIN" CACHE STRING "Link-time optimisation for the game: THIN (default, the game to play), OFF (quick dev builds), FULL")
set(PS2X_PGO "" CACHE STRING "Profile-guided optimisation: empty (off), GEN (instrumented), or a .profdata path (tools/build.py --opt)")
option(VCS_BUILD_TOOLS "Build the PS2Recomp recompiler/analyzer" ON)

set(PS2X_BUILD_RECOMP ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

set(VCS_HAVE_GENERATED OFF)
if(EXISTS "${VCS_GENERATED_DIR}/register_functions.cpp")
    set(VCS_HAVE_GENERATED ON)
endif()
set(PS2X_BUILD_RUNTIME ${VCS_HAVE_GENERATED} CACHE BOOL "" FORCE)

file(GLOB_RECURSE VCS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM VCS_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
# The platform layer, RHI and renderer are their own libraries (src/*/CMakeLists.txt), linked into the runtime.
list(FILTER VCS_SOURCES EXCLUDE REGEX "/src/(platform|rhi|render)/")
# VU1 microprograms compiled to C++ by tools/vu1_recomp.py (from the user's ELF; see game/vcs/vu1_uploads.txt)
if(EXISTS "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
    list(APPEND VCS_SOURCES "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
endif()

set(PS2X_RUNNER_SOURCE_DIR "${VCS_GENERATED_DIR}" CACHE PATH "" FORCE)
set(PS2X_RUNNER_EXTRA_SOURCES "${VCS_SOURCES}" CACHE STRING "" FORCE)
set(PS2X_RUNNER_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp" CACHE FILEPATH "" FORCE)

set(PS2X_PARALLEL_GS_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs" CACHE PATH "" FORCE)
if(VCS_HAVE_GENERATED)
    add_subdirectory(src/platform)
    add_subdirectory(src/rhi)
    add_subdirectory(src/render)
    add_library(vcs_host INTERFACE)
    target_link_libraries(vcs_host INTERFACE vcs_platform vcs_render)
    set(PS2X_HOST_PLATFORM_TARGET vcs_host CACHE STRING "" FORCE)
endif()
add_subdirectory(third_party/PS2Recomp)

if(VCS_HAVE_GENERATED)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "vcs")
    target_include_directories(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(ps2EntryRunner PRIVATE vcs_host) # main.cpp: settings menu (Dear ImGui), presenter aspect
    # Our own sources aren't generated code; keep them out of the unity batches.
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
else()
    message(STATUS "No generated code in ${VCS_GENERATED_DIR}: building tools only. Run python tools/build.py --recompile.")
endif()

# Debug symbols for the runtime and our code (not the 240 MB of generated code),
# so the sampling profiler and debuggers can name host functions.
option(VCS_RUNTIME_DEBUG_INFO "Emit CodeView debug info for runtime + launcher" ON)
if(VCS_RUNTIME_DEBUG_INFO AND MSVC)
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /Z7)
        endif()
    endforeach()
    if(VCS_HAVE_GENERATED)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "/Z7")
        target_link_options(ps2EntryRunner PRIVATE /DEBUG)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
endif()
