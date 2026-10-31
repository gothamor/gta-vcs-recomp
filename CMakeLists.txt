cmake_minimum_required(VERSION 3.21)
project(vcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Two kinds of build tree from this file:
#   - the PS5 build (docs/PS5.md): a cross build with ps5/cmake/ps5-toolchain.cmake (it sets VCS_PS5), the game -
#     tools/ps5.py configures it in build/ps5;
#   - the build machine's tools: the recompiler (and its analyzer), which turn the user's disc into generated C++ -
#     tools/recompile.py configures it in build/release. The game is never built for the build machine.

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.py). Never committed.
set(VCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")

set(PS2X_LTO "OFF" CACHE STRING "Link-time optimisation for the game: OFF (default), THIN (tools/ps5.py build --lto), FULL")
set(PS2X_PGO "" CACHE STRING "Profile-guided optimisation: empty (off), GEN (instrumented), or a .profdata path")

option(VCS_PS5 "PS5 build (set by ps5/cmake/ps5-toolchain.cmake)" OFF)
if(VCS_PS5)
    include("${CMAKE_SOURCE_DIR}/ps5/cmake/ps5.cmake")
endif()

if(VCS_PS5)
    set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
else()
    set(PS2X_BUILD_RECOMP ON CACHE BOOL "" FORCE)
    set(PS2X_BUILD_ANALYZER ON CACHE BOOL "" FORCE)
endif()
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE) # upstream's runtime debugger (F1 on a keyboard): not on the PS5

# clang 19+ only (the same compiler for the tools and the PS5's cross build, and PS2_GUEST_CC needs it), linked with
# lld. Compile and ThinLTO jobs follow the memory: a unity batch of generated code takes up to ~2 GB to compile.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    message(FATAL_ERROR "The build needs clang 19 or newer (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}): "
                        "sudo apt install clang-20 lld-20")
endif()
add_link_options(-fuse-ld=lld)
cmake_host_system_information(RESULT _vcs_mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT _vcs_cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR _vcs_jobs "${_vcs_mem_mb} / 2048")
if(_vcs_jobs LESS 1)
    set(_vcs_jobs 1)
endif()
if(_vcs_jobs GREATER _vcs_cores)
    set(_vcs_jobs ${_vcs_cores})
endif()
set(VCS_COMPILE_JOBS ${_vcs_jobs} CACHE STRING "Parallel compile jobs (default: one per 2 GB of memory, at most one per core)")
set(PS2X_LTO_JOBS ${_vcs_jobs} CACHE STRING "Parallel ThinLTO backend jobs at the game's link")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS vcs_compile=${VCS_COMPILE_JOBS})
set(CMAKE_JOB_POOL_COMPILE vcs_compile)
message(STATUS "${VCS_COMPILE_JOBS} compile jobs, ${PS2X_LTO_JOBS} ThinLTO jobs (${_vcs_mem_mb} MB memory)")

set(VCS_HAVE_GENERATED OFF)
if(VCS_PS5 AND EXISTS "${VCS_GENERATED_DIR}/register_functions.cpp")
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
    vcs_ps5_game(ps2EntryRunner) # ps5/cmake/ps5.cmake: the console's start-up pieces, the link, the eboot's name
    # Our own sources aren't generated code; keep them out of the unity batches.
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
    # Line tables for the crash report (src/debug/crash.cpp: offsets into vcs.elf, symbolized with llvm-symbolizer) and
    # frame pointers for its stack walk - the runtime and our code, not the 240 MB of generated code
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -gline-tables-only -fno-omit-frame-pointer)
        endif()
    endforeach()
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-gline-tables-only;-fno-omit-frame-pointer")
elseif(VCS_PS5)
    message(STATUS "No generated code in ${VCS_GENERATED_DIR}: run python3 tools/recompile.py <your .iso> first.")
endif()
