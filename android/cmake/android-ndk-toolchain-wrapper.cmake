set(_neonsaturn_ndk "")

if(DEFINED ENV{ANDROID_NDK_HOME} AND EXISTS "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
    set(_neonsaturn_ndk "$ENV{ANDROID_NDK_HOME}")
elseif(DEFINED ENV{ANDROID_NDK_ROOT} AND EXISTS "$ENV{ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
    set(_neonsaturn_ndk "$ENV{ANDROID_NDK_ROOT}")
elseif(DEFINED ENV{ANDROID_NDK} AND EXISTS "$ENV{ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    set(_neonsaturn_ndk "$ENV{ANDROID_NDK}")
else()
    get_filename_component(_neonsaturn_android_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(_neonsaturn_local_properties "${_neonsaturn_android_root}/local.properties")

    if(EXISTS "${_neonsaturn_local_properties}")
        file(STRINGS "${_neonsaturn_local_properties}" _neonsaturn_sdk_lines REGEX "^sdk\\.dir=")
        list(LENGTH _neonsaturn_sdk_lines _neonsaturn_sdk_line_count)

        if(_neonsaturn_sdk_line_count GREATER 0)
            list(GET _neonsaturn_sdk_lines 0 _neonsaturn_sdk_line)
            string(REPLACE "sdk.dir=" "" _neonsaturn_sdk_dir "${_neonsaturn_sdk_line}")
            string(REPLACE "\\:" ":" _neonsaturn_sdk_dir "${_neonsaturn_sdk_dir}")
            string(REPLACE "\\\\" "\\" _neonsaturn_sdk_dir "${_neonsaturn_sdk_dir}")
            set(_neonsaturn_candidate_ndk "${_neonsaturn_sdk_dir}/ndk/30.0.15729638")

            if(EXISTS "${_neonsaturn_candidate_ndk}/build/cmake/android.toolchain.cmake")
                set(_neonsaturn_ndk "${_neonsaturn_candidate_ndk}")
            endif()
        endif()
    endif()
endif()

if(_neonsaturn_ndk STREQUAL "")
    message(FATAL_ERROR "Unable to locate Android NDK 30.0.15729638. Set ANDROID_NDK_HOME or update android/local.properties.")
endif()

set(ANDROID_NDK_HOME "${_neonsaturn_ndk}" CACHE PATH "" FORCE)
set(ENV{ANDROID_NDK_HOME} "${_neonsaturn_ndk}")

get_filename_component(_neonsaturn_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_neonsaturn_repo_root}/vcpkg/scripts/toolchains/android.cmake")
