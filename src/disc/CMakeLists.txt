# The game's disc source as a library of its own: disc_source.cpp with .chd support (libchdr; src/disc/disc_source.cpp,
# docs/DISC.md "CHD images"). libchdr is BSD-3 (its LICENSE.txt); its zlib (miniz), LZMA and zstd are its
# own copies, so nothing clashes with ours.
#
# Why a library: libchdr's include folder and VCS_DISC_CHD must reach disc_source.cpp only. Linked PRIVATE here,
# CMake puts $<LINK_ONLY:chdr-static> into vcs_disc's link interface, so the game links it without its usage
# requirements on every CMake version. Linking chdr-static to the game target directly leaked its include folder into
# every compile command (LINK_ONLY there needs policy CMP0131, CMake 3.24; we allow 3.21) and rebuilt the generated
# code. The launcher compiles disc_source.cpp itself, without this file: .iso only, no third-party code (AGENTS.md).
include(FetchContent)

set(BUILD_SHARED_LIBS OFF)         # chdr-static only (a normal variable: libchdr's option() honours it, CMP0077)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZER OFF CACHE BOOL "" FORCE)

# pinned by tag like every other library here: tools/deps.py clones with --branch <tag>
FetchContent_Declare(
    libchdr
    GIT_REPOSITORY "https://github.com/rtissera/libchdr.git"
    GIT_TAG "v0.3.0"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libchdr)
# built only for the game's link (FetchContent_Declare's EXCLUDE_FROM_ALL needs CMake 3.28; Linux allows 3.21)
foreach(_chd_target chdr-static chdr-benchmark)
    if(TARGET ${_chd_target})
        set_property(TARGET ${_chd_target} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

add_library(vcs_disc STATIC disc_source.cpp)
target_compile_definitions(vcs_disc PRIVATE VCS_DISC_CHD=1)
target_link_libraries(vcs_disc PRIVATE chdr-static)
