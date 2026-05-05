find_package(ZLIB QUIET)
find_package(zstd QUIET CONFIG)
add_subdirectory(libchdr/deps/lzma-25.01 EXCLUDE_FROM_ALL)

if(TARGET ZLIB::ZLIB)
    set(CHDR_ZLIB_TARGET ZLIB::ZLIB)
    set(CHDR_ZLIB_DEFINE CHDR_SYSTEM_ZLIB)
else()
    if(NOT TARGET miniz)
        add_subdirectory(libchdr/deps/miniz-3.1.1 EXCLUDE_FROM_ALL)
    endif()
    set(CHDR_ZLIB_TARGET miniz)
endif()

if(TARGET zstd::libzstd_shared)
    set(CHDR_ZSTD_TARGET zstd::libzstd_shared)
    set(CHDR_ZSTD_DEFINE CHDR_SYSTEM_ZSTD)
elseif(TARGET zstd::libzstd_static)
    set(CHDR_ZSTD_TARGET zstd::libzstd_static)
    set(CHDR_ZSTD_DEFINE CHDR_SYSTEM_ZSTD)
else()
    if(NOT TARGET zstd)
        add_subdirectory(libchdr/deps/zstd-1.5.7 EXCLUDE_FROM_ALL)
    endif()
    set(CHDR_ZSTD_TARGET zstd)
endif()

list(APPEND CHDR_INCLUDES ${CMAKE_CURRENT_BINARY_DIR}/include)

add_library(chdr-static STATIC
    libchdr/src/libchdr_bitstream.c
    libchdr/src/libchdr_cdrom.c
    libchdr/src/libchdr_chd.c
    libchdr/src/libchdr_codec_cdfl.c
    libchdr/src/libchdr_codec_cdlz.c
    libchdr/src/libchdr_codec_cdzl.c
    libchdr/src/libchdr_codec_cdzs.c
    libchdr/src/libchdr_codec_flac.c
    libchdr/src/libchdr_codec_huff.c
    libchdr/src/libchdr_codec_lzma.c
    libchdr/src/libchdr_codec_zlib.c
    libchdr/src/libchdr_codec_zstd.c
    libchdr/src/libchdr_flac.c
    libchdr/src/libchdr_huffman.c
)
target_include_directories(chdr-static PRIVATE ${CHDR_INCLUDES} PUBLIC libchdr/include)
target_link_libraries(chdr-static PRIVATE chdr-lzma ${CHDR_ZLIB_TARGET} ${CHDR_ZSTD_TARGET})
target_compile_definitions(chdr-static PRIVATE ${CHDR_ZLIB_DEFINE} ${CHDR_ZSTD_DEFINE})
if (MSVC)
    target_compile_definitions(chdr-static PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
