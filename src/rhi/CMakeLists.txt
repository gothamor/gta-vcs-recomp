# Render hardware interface (rhi.h) and its backends: Vulkan (rhi/vulkan, the PS5's RADV; only it sees Vulkan) and the
# null backend (VCS_RHI=null: no GPU work, for bring-up and checks of the CPU side).
include(FetchContent)

# Headers, loader (volk: RADV's vkGetInstanceProcAddr is handed over by the platform layer) and allocator, all from
# source
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    volk
    GIT_REPOSITORY "https://github.com/zeux/volk.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR "no-cmake" # sources only: volk.c is built into vcs_rhi below
)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git"
    GIT_TAG "v3.3.0"
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR "no-cmake" # header only
)
FetchContent_MakeAvailable(VulkanHeaders volk VulkanMemoryAllocator)

# Dear ImGui is drawn through the RHI (render/ui_renderer.cpp); the TV's surface comes from the platform layer
# (rhi::VulkanWindow)
add_library(vcs_rhi STATIC rhi.cpp null/rhi_null.cpp vulkan/rhi_vulkan.cpp "${volk_SOURCE_DIR}/volk.c")
target_include_directories(vcs_rhi PUBLIC "${CMAKE_SOURCE_DIR}/src"
                           PRIVATE "${volk_SOURCE_DIR}" "${vulkanmemoryallocator_SOURCE_DIR}/include")
target_compile_definitions(vcs_rhi PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(vcs_rhi PUBLIC imgui PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
