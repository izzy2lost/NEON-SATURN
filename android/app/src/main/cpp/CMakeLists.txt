find_package(SDL3 REQUIRED CONFIG)
find_package(cereal CONFIG REQUIRED)

add_library(neonsaturn-main SHARED
    bootstrap.cpp
    emulator_main.cpp
    ipl_select.cpp
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

# Identifies BIOS files for the launcher, which should not have to load SDL and the emulator to do it
add_library(neonsaturn-romid SHARED
    ipl_select.cpp
    rom_identify.cpp
)

set_target_properties(neonsaturn-romid PROPERTIES OUTPUT_NAME romid)

target_compile_features(neonsaturn-romid PRIVATE cxx_std_20)

target_link_libraries(neonsaturn-romid PRIVATE
    ymir-core
)
