# The PS5 build's own pieces (docs/PS5.md), included by the top-level CMakeLists.txt when VCS_PS5 is on.
#
# The game is linked by tools/ps5_link.py instead of the compiler driver: the PS5 link needs the RADV Vulkan driver
# (PS5_Mesa's libvulkan_radeon.ps5.a), the payload SDK's platform layer (a heap in direct memory through --wrap=malloc,
# thread stacks there, the libc functions the console lacks), the start-up code and linker script of
# ps5-plume-triangle, and the SDK's system-library stubs. tools/ps5.py then turns the ELF into the app folder
# (eboot.bin signed as a fake SELF, sce_sys/param.json, sce_module/libc.prx).

set(VCS_PS5_TRIANGLE "$ENV{VCS_PS5_TRIANGLE}" CACHE PATH
    "ps5-plume-triangle checkout, built once (its build/ and .deps/: start-up code, RADV, ps5-native-tool)")
set(VCS_PS5_RADV "" CACHE FILEPATH "libvulkan_radeon.ps5.a (default: the triangle's .deps/native/radv-release)")
set(VCS_PS5_FFMPEG "" CACHE PATH "FFmpeg built for the PS5 (tools/ps5.py ffmpeg; include/ and lib/): the movies")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The game's link: tools/ps5_link.py takes the objects and libraries CMake would hand the linker (response files
# included) and links them with the PS5 recipe. Set here, before the runtime's subdirectory makes the executable.
set(_vcs_ps5_link "\"${Python3_EXECUTABLE}\" -I \"${CMAKE_SOURCE_DIR}/tools/ps5_link.py\" --sdk \"${PS5_PAYLOAD_SDK}\"")
string(APPEND _vcs_ps5_link " --triangle \"${VCS_PS5_TRIANGLE}\" --clang \"${CMAKE_C_COMPILER}\"")
if(VCS_PS5_RADV)
    string(APPEND _vcs_ps5_link " --radv \"${VCS_PS5_RADV}\"")
endif()
if(PS2X_LTO STREQUAL "THIN")
    string(APPEND _vcs_ps5_link " --lto-cache \"${CMAKE_BINARY_DIR}/lto-cache\"")
endif()
set(CMAKE_CXX_LINK_EXECUTABLE "${_vcs_ps5_link} -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")

# FFmpeg for the movies (src/game/vcs_movie.cpp; MPEG-2 video and swscale only). Without it the runtime's MPEG stub
# and the movie player skip the movies (the game goes on as after a skip).
if(VCS_PS5_FFMPEG AND EXISTS "${VCS_PS5_FFMPEG}/include/libavcodec/avcodec.h")
    set(PS2X_ENABLE_FFMPEG ON CACHE BOOL "" FORCE)
    message(STATUS "PS5: FFmpeg from ${VCS_PS5_FFMPEG}")
else()
    set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
    message(STATUS "PS5: no FFmpeg (VCS_PS5_FFMPEG; tools/ps5.py ffmpeg builds it): the movies are skipped")
endif()

# The console's start-up pieces that come with the game: emulated TLS that runs C++ thread_local destructors before
# freeing the thread's storage, and stat("") answering ENOENT (ps5/runtime/).
add_library(vcs_ps5_runtime OBJECT
    "${CMAKE_SOURCE_DIR}/ps5/runtime/ps5_emutls.c"
    "${CMAKE_SOURCE_DIR}/ps5/runtime/ps5_stat.c"
)
set_target_properties(vcs_ps5_runtime PROPERTIES C_STANDARD 11)
target_compile_options(vcs_ps5_runtime PRIVATE -O2 -fPIC)

function(vcs_ps5_game target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:vcs_ps5_runtime>)
    if(PS2X_ENABLE_FFMPEG)
        target_include_directories(${target} PRIVATE "${VCS_PS5_FFMPEG}/include")
    endif()
    # build/ps5/vcs.elf: the PIE that tools/ps5.py packages
    set_target_properties(${target} PROPERTIES SUFFIX ".elf" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/tools/ps5_link.py")
endfunction()
