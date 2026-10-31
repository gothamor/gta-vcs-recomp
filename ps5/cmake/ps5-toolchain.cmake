# The PS5 (jailbroken consoles, homebrew in /data/homebrew): a cross build of the game for the PS5's FreeBSD-based
# system (docs/PS5.md). The host's own clang 19+ compiles for clang's x86_64-sie-ps5 target against the PS5 payload
# SDK's headers and libc++; the final link is tools/ps5_link.py (the RADV driver linked in, the direct-memory heap,
# emulated TLS: the link recipe of ps5-plume-triangle, which the gta4-ps5 port ships with).
#
#   python3 tools/ps5.py build          (configures build/ps5 with this file; the whole command is in docs/PS5.md)
#
# PS5_PAYLOAD_SDK: the SDK root (bin/, target/include, target/lib). PS5_CLANG: the clang to use (default: the newest
# clang 19+ that tools/toolchain.py finds, passed in as CMAKE_C_COMPILER by tools/ps5.py).

if(NOT PS5_PAYLOAD_SDK)
    set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
endif()
if(NOT PS5_PAYLOAD_SDK OR NOT EXISTS "${PS5_PAYLOAD_SDK}/target/include/c++/v1")
    message(FATAL_ERROR "Set PS5_PAYLOAD_SDK to the PS5 payload SDK root (it has target/include/c++/v1): docs/PS5.md")
endif()
get_filename_component(PS5_PAYLOAD_SDK "${PS5_PAYLOAD_SDK}" ABSOLUTE)
set(PS5_PAYLOAD_SDK "${PS5_PAYLOAD_SDK}" CACHE PATH "PS5 payload SDK root" FORCE)
# try_compile projects see it too (CheckIPOSupported's project gets no platform variables: the environment then)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK)
set(ENV{PS5_PAYLOAD_SDK} "${PS5_PAYLOAD_SDK}")

set(VCS_PS5 ON CACHE BOOL "The PS5 build (ps5/cmake/ps5-toolchain.cmake)" FORCE)
set(PS5 TRUE)

# The console runs a FreeBSD kernel on Zen 2 cores.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 9)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING ON)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_ASM_COMPILER_TARGET x86_64-sie-ps5)

# No sysroot for CMake's own searches: everything the build needs is named explicitly (the SDK's prefix holds no CMake
# packages), and the host's programs (python, glslang) must stay findable.
set(CMAKE_FIND_ROOT_PATH "${PS5_PAYLOAD_SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler checks build a static library: an executable needs the PS5 link (tools/ps5_link.py), which a check can't do.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # the eboot is a PIE

# The payload SDK's own compiler wrappers' flags (bin/prospero-clang++), and:
#   -march=znver2: the PS5's CPU (a superset of the x86-64-v3 the PC builds use)
#   -fdenormal-fp-math=ieee: the target's default, preserve-sign, assumes a flush-to-zero MXCSR; the app's start-up
#       code sets the IEEE state the PC builds compute in (MXCSR 0x1f80; main.cpp checks it)
#   -femulated-tls: the PS5's ABI for thread_local (ps5/runtime/ps5_emutls.c)
#   -fexperimental-library: libc++ 18 keeps std::jthread / stop_token behind it
#   -isysroot: clang's PS5 driver then finds <sdk>/target/include and target/include_common itself
set(_vcs_ps5_flags "-isysroot ${PS5_PAYLOAD_SDK} -march=znver2 -fvisibility-nodllstorageclass=default -fno-stack-protector -fno-plt -femulated-tls -fdenormal-fp-math=ieee")
set(CMAKE_C_FLAGS_INIT "${_vcs_ps5_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_vcs_ps5_flags} -stdlib++-isystem ${PS5_PAYLOAD_SDK}/target/include/c++/v1 -frtti -fexceptions -fexperimental-library")
set(CMAKE_ASM_FLAGS_INIT "${_vcs_ps5_flags}")
