# Render hardware interface (rhi.h) and its backends. Only rhi/d3d11 sees D3D11 / DXGI.
add_library(vcs_rhi STATIC
    d3d11/rhi_d3d11.cpp
    "${VCS_IMGUI_BACKENDS_DIR}/imgui_impl_dx11.cpp"
)
target_include_directories(vcs_rhi PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${VCS_IMGUI_BACKENDS_DIR}")
target_link_libraries(vcs_rhi PUBLIC imgui PRIVATE d3d11 dxgi d3dcompiler dxguid)
