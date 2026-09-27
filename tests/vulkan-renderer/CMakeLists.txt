# Desktop test harness for the Vulkan VDP renderer: compares it against the software renderer on synthetic scenes.
# Standalone project (it builds ymir-core with the Vulkan renderer enabled and needs DXC for the shaders):
#   cmake -S tests/vulkan-renderer -B build-vktest -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDXC_EXECUTABLE=<dxc>
#   ninja -C build-vktest vktest && ./build-vktest/vktest
cmake_minimum_required(VERSION 3.28)
project(YmirVulkanRendererTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
get_filename_component(YMIR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(Ymir_VERSION 0.3.2)
set(Ymir_VERSION_MAJOR 0)
set(Ymir_VERSION_MINOR 3)
set(Ymir_VERSION_PATCH 2)
set(Ymir_core_VERSION 0.3.2)
set(Ymir_core_VERSION_MAJOR 0)
set(Ymir_LIBRARY_ONLY ON CACHE BOOL "" FORCE)
set(Ymir_DEV_BUILD ON CACHE BOOL "" FORCE)
set(Ymir_ENABLE_DEVLOG ON CACHE BOOL "" FORCE)
set(Ymir_ENABLE_VULKAN_RENDERER ON CACHE BOOL "" FORCE)
set(Ymir_FEATUREFLAG_DEFAULT ON)

include(${YMIR_ROOT}/cmake/CMakeRC.cmake)
include(${YMIR_ROOT}/cmake/CompileShaders.cmake)

set(FMT_MODULE OFF)
add_subdirectory(${YMIR_ROOT}/vendor/fmt vendor/fmt)
foreach (dir mio stb dr_libs concurrentqueue xxHash lz4)
    add_subdirectory(${YMIR_ROOT}/vendor/${dir} vendor/${dir} EXCLUDE_FROM_ALL)
endforeach ()
add_subdirectory(${YMIR_ROOT}/vendor/libchdr vendor/libchdr)
add_subdirectory(${YMIR_ROOT}/libs/ymir-core libs/ymir-core)

add_executable(vktest vktest.cpp)
target_link_libraries(vktest PRIVATE ymir-core)
