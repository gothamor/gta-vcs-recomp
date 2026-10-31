# The PS5 platform layer (docs/PS5.md, docs/NATIVE_RENDERER.md): the TV's Vulkan surface, the DualSense, the audio
# output and the Dear ImGui context, on the console's own libraries (platform_ps5.cpp). The runtime links it (with
# src/render) through ps2_host_backend (PS2X_HOST_PLATFORM_TARGET).
include(FetchContent)

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

# zlib for compressed screenshots (writePng); only the static library is built for us.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY "https://github.com/madler/zlib.git"
    GIT_TAG "v1.3.1"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib)

# The Vulkan headers (the TV's surface is VK_KHR_display; the driver, RADV, is linked into the game by
# tools/ps5_link.py). src/rhi uses the same target.
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(VulkanHeaders)

add_library(vcs_platform STATIC platform_ps5.cpp platform_common.cpp)
target_compile_definitions(vcs_platform PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(vcs_platform PUBLIC imgui PRIVATE Vulkan::Headers zlibstatic)
target_include_directories(vcs_platform PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
