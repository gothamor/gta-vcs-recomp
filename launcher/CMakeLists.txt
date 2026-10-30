# The launcher (docs/LAUNCHER.md): finds and checks the disc, installs what the build needs, runs tools/build.py and
# starts the game. A project of its own - no generated code, no disc - so CI can build it for a release:
#   python tools/build.py --launcher        (build/launcher, VCSLauncher.exe in the repository root)
# Same SDL3 and Dear ImGui as the game (src/platform); a release's deps/ replaces their downloads
# (tools/toolchain.py deps_defines).
cmake_minimum_required(VERSION 3.21)
project(vcs_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
# the same pins as src/platform/CMakeLists.txt (tools/deps.py fetches each once for both)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
    GIT_TAG "release-3.4.18"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(SDL3)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
    GIT_TAG "v1.92.7-docking"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_executable(VCSLauncher WIN32
    main.cpp
    ui.cpp
    tasks.cpp
    process.cpp
    disc_check.cpp
    system.cpp
    json.cpp
    "${VCS_ROOT}/src/disc/iso_image.cpp"
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)
# the version (VERSION in the repository root, the one number of a release) in the exe's version information
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS_ROOT}/VERSION")
file(STRINGS "${VCS_ROOT}/VERSION" VCS_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _vcs_version_numbers "${VCS_VERSION}")
if(NOT _vcs_version_numbers)
    message(FATAL_ERROR "VERSION must start with major.minor.patch (it holds '${VCS_VERSION}')")
endif()
target_compile_definitions(VCSLauncher PRIVATE VCS_VERSION_MAJOR=${CMAKE_MATCH_1} VCS_VERSION_MINOR=${CMAKE_MATCH_2}
                           VCS_VERSION_PATCH=${CMAKE_MATCH_3} "VCS_VERSION_TEXT=\"${VCS_VERSION}\"")

if(WIN32)
    target_sources(VCSLauncher PRIVATE launcher.rc)
    target_link_libraries(VCSLauncher PRIVATE ole32 shell32 uuid)
    target_compile_definitions(VCSLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_include_directories(VCSLauncher PRIVATE "${VCS_ROOT}/src" "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(VCSLauncher PRIVATE SDL3::SDL3-static)
# next to tools/ and the source, where a release has it (the launcher finds the repository from its own folder)
set_target_properties(VCSLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_ROOT}")
if(MSVC)
    # one self-contained exe: the static C++ runtime, nothing to install
    set_property(TARGET VCSLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_property(TARGET SDL3-static PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
