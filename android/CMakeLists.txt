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

# ymir-core links fmt::fmt but does not look it up itself (upstream gets it from
# vendor/CMakeLists.txt, which this build does not use), so resolve it from vcpkg here
find_package(fmt CONFIG REQUIRED)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/mio" "${CMAKE_BINARY_DIR}/vendor/mio" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/concurrentqueue" "${CMAKE_BINARY_DIR}/vendor/concurrentqueue" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/xxHash" "${CMAKE_BINARY_DIR}/vendor/xxHash" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/lz4" "${CMAKE_BINARY_DIR}/vendor/lz4" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/libchdr" "${CMAKE_BINARY_DIR}/vendor/libchdr")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../libs/ymir-core" "${CMAKE_BINARY_DIR}/libs/ymir-core")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/app/src/main/cpp" "${CMAKE_BINARY_DIR}/app/src/main/cpp")
