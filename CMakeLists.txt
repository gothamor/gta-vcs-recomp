cmake_minimum_required(VERSION 3.21)
project(vcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.sh). Never committed.
set(VCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")

set(PS2X_LTO "OFF" CACHE STRING "Link-time optimisation for the game: OFF (dev), THIN (release), FULL")
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

set(PS2X_RUNNER_SOURCE_DIR "${VCS_GENERATED_DIR}" CACHE PATH "" FORCE)
set(PS2X_RUNNER_EXTRA_SOURCES "${VCS_SOURCES}" CACHE STRING "" FORCE)
set(PS2X_RUNNER_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp" CACHE FILEPATH "" FORCE)

add_subdirectory(third_party/PS2Recomp)

if(VCS_HAVE_GENERATED)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "vcs")
    target_include_directories(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # Our own sources aren't generated code; keep them out of the unity batches.
    # (windows.h in them also clashes with raylib inside a unity TU.)
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
else()
    message(STATUS "No generated code in ${VCS_GENERATED_DIR}: building tools only. Run tools/recompile.sh first.")
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
