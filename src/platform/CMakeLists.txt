# Host platform layer (docs/NATIVE_RENDERER.md): SDL3 window / input / audio and the Dear ImGui context with its
# input backend. The runtime links it (with src/render) through ps2_host_backend (PS2X_HOST_PLATFORM_TARGET).
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
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

# zlib for compressed screenshots (writePng); only the static library is built for us.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY "https://github.com/madler/zlib.git"
    GIT_TAG "v1.3.1"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib)

add_library(vcs_platform STATIC
    platform_sdl3.cpp
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
)
target_include_directories(vcs_platform PUBLIC "${CMAKE_SOURCE_DIR}/src"
                           PRIVATE "${imgui_SOURCE_DIR}/backends" "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
target_link_libraries(vcs_platform PUBLIC imgui PRIVATE SDL3::SDL3-static zlibstatic)
