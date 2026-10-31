# The controller test (python3 tools/ps5.py padtest; docs/PS5.md), built for the build machine: the PS5 platform layer
# (src/platform/platform_ps5.cpp) with the console's pad / user / TSC functions mocked in padtest.cpp, the real
# bindings (src/game/vcs_input.cpp), settings and the runtime's pad backend and control channel - the controller's
# whole path to the report the game reads, and nothing of the game itself.
cmake_minimum_required(VERSION 3.21)
project(vcs_padtest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(VCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RUNTIME "${VCS_ROOT}/third_party/PS2Recomp/ps2xRuntime")

include(FetchContent)
# the same pins as the game (src/platform/CMakeLists.txt, src/rhi/CMakeLists.txt)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
    GIT_TAG "v1.92.7-docking"
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui VulkanHeaders)

add_executable(padtest
    padtest.cpp
    "${VCS_ROOT}/src/platform/platform_ps5.cpp"
    "${VCS_ROOT}/src/game/vcs_input.cpp"
    "${VCS_ROOT}/src/settings/settings.cpp"
    "${RUNTIME}/src/lib/ps2_pad.cpp"
    "${RUNTIME}/src/lib/ps2_control.cpp"
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(padtest PRIVATE
    "${VCS_ROOT}/src" "${RUNTIME}/include" "${imgui_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/shim") # pthread_np.h: FreeBSD's, as the console has it
target_compile_definitions(padtest PRIVATE VK_NO_PROTOTYPES)
target_compile_options(padtest PRIVATE -g -O1 -fno-strict-aliasing)
find_package(Threads REQUIRED)
target_link_libraries(padtest PRIVATE Vulkan::Headers Threads::Threads)
