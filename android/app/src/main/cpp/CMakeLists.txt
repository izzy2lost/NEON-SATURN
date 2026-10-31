find_package(SDL3 REQUIRED CONFIG)
find_package(cereal CONFIG REQUIRED)

set(NEONSATURN_VERSION "0.0" CACHE STRING "App version reported to RetroAchievements")

# RetroAchievements (rcheevos, vendored). Whole runtime directory: its translation units reference each other freely.
# Not built: rcheevos' disc/encrypted/zip hashing (the Saturn hash is read from the disc Ymir already loaded, see
# cheevos_session.cpp), rc_client_raintegration.c (Windows) and rc_libretro.c.
set(RC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../rcheevos")
file(GLOB RC_RUNTIME_SRC "${RC_DIR}/src/rcheevos/*.c")
file(GLOB RC_API_SRC "${RC_DIR}/src/rapi/*.c")
set(RC_SRC
    ${RC_RUNTIME_SRC}
    ${RC_API_SRC}
    "${RC_DIR}/src/rc_client.c"
    "${RC_DIR}/src/rhash/hash.c"
    "${RC_DIR}/src/rhash/hash_rom.c"
    "${RC_DIR}/src/rhash/md5.c"
    "${RC_DIR}/src/rc_util.c"
    "${RC_DIR}/src/rc_compat.c"
    "${RC_DIR}/src/rc_version.c"
)
# Its private headers stay off the emulator's include path.
set_source_files_properties(${RC_SRC} PROPERTIES
    COMPILE_DEFINITIONS "RC_HASH_NO_DISC;RC_HASH_NO_ENCRYPTED;RC_HASH_NO_ZIP;RC_DISABLE_LUA"
    INCLUDE_DIRECTORIES "${RC_DIR}/src;${RC_DIR}/src/rhash;${RC_DIR}/src/rapi"
)
# Only the User-Agent needs these: rc_version.h is not a public header, and the app version stays off every other
# file so a bump rebuilds one.
set_source_files_properties(cheevos_client.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${RC_DIR}/src"
    COMPILE_DEFINITIONS "NEONSATURN_VERSION=\"${NEONSATURN_VERSION}\""
)
set_source_files_properties(cheevos_session.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${RC_DIR}/src/rhash"
)

add_library(neonsaturn-main SHARED
    bootstrap.cpp
    cheevos_client.cpp
    cheevos_session.cpp
    emulator_main.cpp
    ipl_select.cpp
    savestate_file.cpp
    ${RC_SRC}
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/app/audio_system.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/app/rewind_buffer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/util/file_loader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/util/rom_loader.cpp"
)

set_target_properties(neonsaturn-main PROPERTIES OUTPUT_NAME main)

target_compile_features(neonsaturn-main PRIVATE cxx_std_20)

target_include_directories(neonsaturn-main PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/app"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../apps/ymir-sdl3/src/util"
    "${RC_DIR}/include"
)

target_link_libraries(neonsaturn-main PRIVATE
    cereal::cereal
    lz4::lz4
    SDL3::SDL3
    ymir-core
    android
    EGL
    GLESv3
    log
)

# Identifies BIOS files and discs for the launcher, which should not have to load SDL and the emulator to do it
add_library(neonsaturn-romid SHARED
    disc_hash.cpp
    ipl_select.cpp
    rom_identify.cpp
)

set_target_properties(neonsaturn-romid PROPERTIES OUTPUT_NAME romid)

target_compile_features(neonsaturn-romid PRIVATE cxx_std_20)

target_link_libraries(neonsaturn-romid PRIVATE
    chdr-static
    ymir-core
    z
)
