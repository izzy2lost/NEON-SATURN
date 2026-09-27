cmake_minimum_required(VERSION 3.28)
project(NeonSaturnAndroid LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ARCHITECTURES arm64)
set(Ymir_core_VERSION 0.3.2)
set(Ymir_core_VERSION_MAJOR 0)
set(Ymir_VERSION 0.3.2)
set(Ymir_VERSION_MAJOR 0)
set(Ymir_VERSION_MINOR 3)
set(Ymir_VERSION_PATCH 2)

# Build ymir-core and its vendored dependencies self-contained: no vcpkg packages
# (libchdr uses its bundled miniz/zstd instead of find_package)
set(Ymir_LIBRARY_ONLY ON CACHE BOOL "" FORCE)
set(Ymir_INCLUDE_PACKAGING OFF CACHE BOOL "" FORCE)
set(Ymir_DEV_BUILD ON CACHE BOOL "" FORCE)
set(Ymir_FEATUREFLAG_DEFAULT ${Ymir_DEV_BUILD} CACHE BOOL "" FORCE)
set(Ymir_FF_VIRTUA_GUN ${Ymir_FEATUREFLAG_DEFAULT} CACHE BOOL "" FORCE)
set(Ymir_EXTRA_INLINING ON CACHE BOOL "" FORCE)
set(Ymir_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_SANDBOX OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_YMDASM OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_DEVLOG OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_IMGUI_DEMO OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_UPDATE_CHECKS OFF CACHE BOOL "" FORCE)
set(Ymir_AVX2 OFF CACHE BOOL "" FORCE)

message(STATUS "NEON SATURN Android: using minimal vendored dependency set")

# Release optimization.
# Gradle maps the release variant to RelWithDebInfo, which the NDK compiles at -O2 with
# no cross-translation-unit optimization. ymir-core is split across ~90 files, so most of
# the emulator's hot path is invisible to the optimizer; ThinLTO is what lets the SH2 and
# VDP inner loops inline across those boundaries. Applied directory-wide so the vendored
# decompression libraries (used for CHD loading) benefit too.
# Debug builds are deliberately left at the NDK defaults to keep iteration fast.
#
# Note: Ymir_ENABLE_IPO above is inert in this build - it is gated on IPO_SUPPORTED, which
# is only set by the root CMakeLists.txt that the Android build does not include. LTO is
# applied here instead, which also covers the vendored libraries and the JNI layer.
set(neonsaturn_optimized "$<NOT:$<CONFIG:Debug>>")

add_compile_options(
    "$<${neonsaturn_optimized}:-O3>"
    "$<${neonsaturn_optimized}:-flto=thin>"
)
add_link_options(
    "$<${neonsaturn_optimized}:-flto=thin>"
    # Keeps incremental release links from redoing the whole ThinLTO backend
    "$<${neonsaturn_optimized}:-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache>"
    "$<${neonsaturn_optimized}:-Wl,--thinlto-cache-policy=cache_size_bytes=1g>"
    # --gc-sections already comes from the NDK toolchain; fold identical functions as well.
    # 'safe' honours Clang's address-significance table, so function-pointer identity holds.
    "$<${neonsaturn_optimized}:-Wl,--icf=safe>"
)

# ymir-core links fmt::fmt but does not look it up itself (upstream gets it from
# vendor/CMakeLists.txt, which this build does not use), so resolve it from vcpkg here
find_package(fmt CONFIG REQUIRED)

# The Vulkan VDP renderer runs ymir-core's HLSL compute shaders, compiled to SPIR-V at build time by DXC and
# embedded through CMakeRC. DXC comes from vcpkg as a host tool (see vcpkg.json).
set(Ymir_ENABLE_VULKAN_RENDERER ON CACHE BOOL "" FORCE)

# vcpkg installs host tools under vcpkg_installed/<host triplet>; the host triplet is only known to CMake when it's
# passed explicitly, so look the tool up instead.
file(GLOB NEONSATURN_DXC_TOOL "${VCPKG_INSTALLED_DIR}/*/tools/directx-dxc/dxc" "${VCPKG_INSTALLED_DIR}/*/tools/directx-dxc/dxc.exe")
if (NOT NEONSATURN_DXC_TOOL)
    message(FATAL_ERROR "DXC not found in ${VCPKG_INSTALLED_DIR}/*/tools/directx-dxc; it should be installed by vcpkg")
endif ()
list(GET NEONSATURN_DXC_TOOL 0 NEONSATURN_DXC_TOOL)
if (CMAKE_HOST_WIN32)
    # The Windows package places the DLLs next to dxc.exe
    set(NEONSATURN_DXC "${NEONSATURN_DXC_TOOL}")
else ()
    # Depending on the port version, the Linux dxc binary may look for libdxcompiler.so in ../lib relative to itself,
    # which doesn't match the vcpkg layout (tools/directx-dxc/dxc and lib/libdxcompiler.so), so run it through a
    # wrapper that sets the library path.
    get_filename_component(NEONSATURN_DXC_LIB_DIR "${NEONSATURN_DXC_TOOL}/../../../lib" ABSOLUTE)
    set(NEONSATURN_DXC "${CMAKE_BINARY_DIR}/dxc-wrapper.sh")
    file(WRITE "${NEONSATURN_DXC}"
        "#!/bin/sh\n"
        "LD_LIBRARY_PATH=\"${NEONSATURN_DXC_LIB_DIR}\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\" "
        "exec \"${NEONSATURN_DXC_TOOL}\" \"\$@\"\n"
    )
    file(CHMOD "${NEONSATURN_DXC}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endif ()
set(DXC_EXECUTABLE_VULKAN "${NEONSATURN_DXC}" CACHE FILEPATH "" FORCE)
set(DXC_EXECUTABLE "${NEONSATURN_DXC}" CACHE FILEPATH "" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CMakeRC.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CompileShaders.cmake")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/mio" "${CMAKE_BINARY_DIR}/vendor/mio" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/stb" "${CMAKE_BINARY_DIR}/vendor/stb" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/dr_libs" "${CMAKE_BINARY_DIR}/vendor/dr_libs" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/concurrentqueue" "${CMAKE_BINARY_DIR}/vendor/concurrentqueue" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/xxHash" "${CMAKE_BINARY_DIR}/vendor/xxHash" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/lz4" "${CMAKE_BINARY_DIR}/vendor/lz4" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/libchdr" "${CMAKE_BINARY_DIR}/vendor/libchdr")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../libs/ymir-core" "${CMAKE_BINARY_DIR}/libs/ymir-core")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/app/src/main/cpp" "${CMAKE_BINARY_DIR}/app/src/main/cpp")
