find_package(ZLIB REQUIRED)
find_package(zstd QUIET CONFIG)
add_subdirectory(libchdr/deps/lzma-24.05 EXCLUDE_FROM_ALL)

if(TARGET zstd::libzstd_shared)
    set(CHDR_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    set(CHDR_ZSTD_TARGET zstd::libzstd_static)
else()
    option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" OFF)
    option(ZSTD_BUILD_PROGRAMS "BUILD PROGRAMS" OFF)
    option(ZSTD_LEGACY_SUPPORT "LEGACY SUPPORT" OFF)
    if(NOT TARGET libzstd_static)
        add_subdirectory(libchdr/deps/zstd-1.5.6/build/cmake EXCLUDE_FROM_ALL)
    endif()
    set(CHDR_ZSTD_TARGET libzstd_static)
endif()

list(APPEND CHDR_INCLUDES ${CMAKE_CURRENT_BINARY_DIR}/include)

add_library(chdr-static STATIC
    libchdr/src/libchdr_bitstream.c
    libchdr/src/libchdr_cdrom.c
    libchdr/src/libchdr_chd.c
    libchdr/src/libchdr_flac.c
    libchdr/src/libchdr_huffman.c
)
target_include_directories(chdr-static PRIVATE ${CHDR_INCLUDES} PUBLIC libchdr/include)
target_link_libraries(chdr-static PRIVATE lzma ZLIB::ZLIB ${CHDR_ZSTD_TARGET})
if (MSVC)
    target_compile_definitions(chdr-static PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
