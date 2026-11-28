# Host platform layer (docs/NATIVE_RENDERER.md): SDL3 window / input / audio and the Dear ImGui context with its
# input backend. The runtime links it (with src/render) through ps2_host_backend (PS2X_HOST_PLATFORM_TARGET).
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Linux: XTest only serves SDL's global mouse warp (the game captures the mouse through XInput2's relative motion);
# off, so the build needs one package less (SDL stops at a missing one it was asked for)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
# SDL asks CMake whether <spawn.h> has posix_spawn_file_actions_addchdir (POSIX 2024) and its _np spelling, and CMake
# keeps the answer in the cache. glibc 2.44's rolling snapshots had the POSIX name for a while and then reverted it
# (BZ 34437; 2.45 exports it for real), so a build folder made before such an update keeps a stale "found" and SDL's
# SDL_posixprocess.c stops compiling - an AppImage update on CachyOS hit it on 2026-11-27. Test this system every
# configure instead (two try_compiles): with today's glibc only _np exists, and SDL's own shim then uses it.
if(UNIX AND NOT APPLE)
    unset(HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR CACHE)
    unset(HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR_NP CACHE)
endif()
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
    GIT_TAG "release-3.4.18"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(SDL3)

# Same version the runtime's debug panel was written against; the runtime reuses this target.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
    GIT_TAG "v1.92.7-docking"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
)
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}")
set(VCS_IMGUI_BACKENDS_DIR "${imgui_SOURCE_DIR}/backends" CACHE INTERNAL "")

# deflate.cpp compresses the screenshots (writePng): no zlib since 2026-11-27 (DECISIONS.md)
add_library(vcs_platform STATIC
    platform_sdl3.cpp
    deflate.cpp
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
)
target_include_directories(vcs_platform PUBLIC "${CMAKE_SOURCE_DIR}/src"
                           PRIVATE "${imgui_SOURCE_DIR}/backends")
target_link_libraries(vcs_platform PUBLIC imgui PRIVATE SDL3::SDL3-static)
