cmake_minimum_required(VERSION 3.16)

project(obs-vdoninja VERSION 1.1.68 LANGUAGES C CXX)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/ninja-obs-plugin/issues")
set(PLUGIN_LICENSE "AGPL-3.0-only")

# Options
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_PLUGIN "Build the OBS plugin (requires OBS SDK)" ON)
option(BUNDLE_LINUX_RUNTIME "Bundle libdatachannel privately in Linux release packages" OFF)
option(BUILD_PUBLISHER_TOOL "Build the VP9 alpha publisher test tool (requires BUILD_PLUGIN deps)" OFF)
option(BUILD_NATIVE_MEDIA_LINKED_GATE
    "Build the OBS/FFmpeg-linked native receiver packet-path validation gate"
    OFF)
option(BUILD_MODULE_LIFECYCLE_LINKED_GATE
    "Build the statically-linked libdatachannel module lifecycle validation gate"
    OFF)
set(LIBDATACHANNEL_PLOG_INCLUDE_DIR "" CACHE PATH
    "Explicit plog include directory for the linked module lifecycle gate")

# Build configuration
# Use C++20 on Windows for designated initializers support (MSVC requires /std:c++20)
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    )
else()
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find OpenSSL (optional; libdatachannel may provide transitive crypto deps)
find_package(OpenSSL QUIET)
find_package(Threads REQUIRED)

# Find Qt for the custom dock UI (only needed when building the plugin)
if(BUILD_PLUGIN)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(NOT Qt6_FOUND)
        find_package(Qt5 COMPONENTS Widgets REQUIRED)
    endif()
endif()

if(BUILD_PLUGIN)
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES include
    )
    find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec libavcodec
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib lib64 bin
    )
    find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil libavutil
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib lib64 bin
    )
    find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale libswscale
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib lib64 bin
    )
    find_library(FFMPEG_SWRESAMPLE_LIBRARY NAMES swresample libswresample
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib lib64 bin
    )

    if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC_LIBRARY OR NOT FFMPEG_AVUTIL_LIBRARY OR
       NOT FFMPEG_SWSCALE_LIBRARY OR NOT FFMPEG_SWRESAMPLE_LIBRARY)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG_AVCODEC QUIET libavcodec)
            pkg_check_modules(FFMPEG_AVUTIL QUIET libavutil)
            pkg_check_modules(FFMPEG_SWSCALE QUIET libswscale)
            pkg_check_modules(FFMPEG_SWRESAMPLE QUIET libswresample)

            if(NOT FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC_INCLUDE_DIRS)
                list(GET FFMPEG_AVCODEC_INCLUDE_DIRS 0 FFMPEG_INCLUDE_DIR)
            endif()
            if(NOT FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVCODEC_LIBRARIES)
                list(GET FFMPEG_AVCODEC_LIBRARIES 0 FFMPEG_AVCODEC_LIBRARY)
            endif()
            if(NOT FFMPEG_AVUTIL_LIBRARY AND FFMPEG_AVUTIL_LIBRARIES)
                list(GET FFMPEG_AVUTIL_LIBRARIES 0 FFMPEG_AVUTIL_LIBRARY)
            endif()
            if(NOT FFMPEG_SWSCALE_LIBRARY AND FFMPEG_SWSCALE_LIBRARIES)
                list(GET FFMPEG_SWSCALE_LIBRARIES 0 FFMPEG_SWSCALE_LIBRARY)
            endif()
            if(NOT FFMPEG_SWRESAMPLE_LIBRARY AND FFMPEG_SWRESAMPLE_LIBRARIES)
                list(GET FFMPEG_SWRESAMPLE_LIBRARIES 0 FFMPEG_SWRESAMPLE_LIBRARY)
            endif()
        endif()
    endif()

    if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC_LIBRARY OR NOT FFMPEG_AVUTIL_LIBRARY OR
       NOT FFMPEG_SWSCALE_LIBRARY OR NOT FFMPEG_SWRESAMPLE_LIBRARY)
        message(FATAL_ERROR "FFmpeg development libraries (avcodec, avutil, swscale, swresample) are required")
    endif()
endif()

if(DEFINED OBS_SDK_PATH)
    list(PREPEND CMAKE_PREFIX_PATH "${OBS_SDK_PATH}")
    if(EXISTS "${OBS_SDK_PATH}/include/obs/obs-module.h")
        set(LIBOBS_INCLUDE_DIRS "${OBS_SDK_PATH}/include/obs")
    endif()
    if(EXISTS "${OBS_SDK_PATH}/include/obs-frontend-api/obs-frontend-api.h")
        set(OBS_FRONTEND_INCLUDE_DIRS "${OBS_SDK_PATH}/include/obs-frontend-api")
    endif()
endif()

# Build plugin only if requested
if(BUILD_PLUGIN)
    # When OBS_SDK_PATH is set, use find_library to get full paths (not pkg-config which returns -lobs)
    if(DEFINED OBS_SDK_PATH)
        # On macOS, check for app bundle framework/dylib structure first
        if(APPLE)
            # macOS app bundle structure: OBS.app/Contents/Frameworks/
            set(_OBS_FRAMEWORK_PATH "${OBS_SDK_PATH}/OBS.app/Contents/Frameworks")

            # Check for libobs framework
            if(EXISTS "${_OBS_FRAMEWORK_PATH}/libobs.framework")
                set(LIBOBS_FOUND TRUE)
                set(LIBOBS_LIBRARIES "${_OBS_FRAMEWORK_PATH}/libobs.framework")
                # Headers: prefer SDK include dir if exists (manually copied), otherwise framework headers
                # OBS headers expect: include/obs.h, include/util/c99defs.h (flat structure)
                if(EXISTS "${OBS_SDK_PATH}/include/obs.h")
                    set(LIBOBS_INCLUDE_DIRS "${OBS_SDK_PATH}/include")
                    message(STATUS "Found libobs framework with SDK headers: ${LIBOBS_LIBRARIES}")
                else()
                    set(LIBOBS_INCLUDE_DIRS "${_OBS_FRAMEWORK_PATH}/libobs.framework/Headers")
                    message(STATUS "Found libobs framework: ${LIBOBS_LIBRARIES}")
                endif()
            endif()

            # Check for obs-frontend-api - it can be a framework or dylib
            if(EXISTS "${_OBS_FRAMEWORK_PATH}/libobs-frontend-api.framework")
                set(OBS_FRONTEND_FOUND TRUE)
                set(OBS_FRONTEND_LIBRARIES "${_OBS_FRAMEWORK_PATH}/libobs-frontend-api.framework")
                set(OBS_FRONTEND_INCLUDE_DIRS "${_OBS_FRAMEWORK_PATH}/libobs-frontend-api.framework/Headers")
                message(STATUS "Found obs-frontend-api framework: ${OBS_FRONTEND_LIBRARIES}")
            elseif(EXISTS "${_OBS_FRAMEWORK_PATH}/obs-frontend-api.dylib")
                # OBS 31+ installs obs-frontend-api as a dylib, not a framework
                set(OBS_FRONTEND_FOUND TRUE)
                set(OBS_FRONTEND_LIBRARIES "${_OBS_FRAMEWORK_PATH}/obs-frontend-api.dylib")
                # Headers should be in the source or SDK include directory
                find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
                    HINTS "${OBS_SDK_PATH}"
                    PATH_SUFFIXES include/obs include obs include/obs-frontend-api
                    NO_DEFAULT_PATH
                )
                if(OBS_FRONTEND_INCLUDE_DIR)
                    set(OBS_FRONTEND_INCLUDE_DIRS "${OBS_FRONTEND_INCLUDE_DIR}")
                endif()
                message(STATUS "Found obs-frontend-api dylib: ${OBS_FRONTEND_LIBRARIES}")
            elseif(EXISTS "${_OBS_FRAMEWORK_PATH}/libobs-frontend-api.1.dylib")
                # Versioned dylib variant
                set(OBS_FRONTEND_FOUND TRUE)
                set(OBS_FRONTEND_LIBRARIES "${_OBS_FRAMEWORK_PATH}/libobs-frontend-api.1.dylib")
                find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
                    HINTS "${OBS_SDK_PATH}"
                    PATH_SUFFIXES include/obs include obs include/obs-frontend-api
                    NO_DEFAULT_PATH
                )
                if(OBS_FRONTEND_INCLUDE_DIR)
                    set(OBS_FRONTEND_INCLUDE_DIRS "${OBS_FRONTEND_INCLUDE_DIR}")
                endif()
                message(STATUS "Found obs-frontend-api versioned dylib: ${OBS_FRONTEND_LIBRARIES}")
            endif()
        endif()

        # Standard lib search (Linux, Windows, and macOS fallback)
        if(NOT LIBOBS_FOUND)
            find_path(LIBOBS_INCLUDE_DIR obs.h
                HINTS "${OBS_SDK_PATH}"
                PATH_SUFFIXES include/obs include obs
                NO_DEFAULT_PATH
            )
            find_library(LIBOBS_LIBRARY NAMES obs libobs
                HINTS "${OBS_SDK_PATH}"
                PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu bin
                NO_DEFAULT_PATH
            )
            if(LIBOBS_INCLUDE_DIR AND LIBOBS_LIBRARY)
                set(LIBOBS_FOUND TRUE)
                set(LIBOBS_INCLUDE_DIRS "${LIBOBS_INCLUDE_DIR}")
                set(LIBOBS_LIBRARIES "${LIBOBS_LIBRARY}")
                message(STATUS "Found libobs via OBS_SDK_PATH: ${LIBOBS_LIBRARY}")
            endif()
        endif()

        if(NOT OBS_FRONTEND_FOUND)
            find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
                HINTS "${OBS_SDK_PATH}"
                PATH_SUFFIXES include/obs include obs include/obs-frontend-api
                NO_DEFAULT_PATH
            )
            find_library(OBS_FRONTEND_LIBRARY NAMES obs-frontend-api libobs-frontend-api
                HINTS "${OBS_SDK_PATH}"
                PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu bin
                NO_DEFAULT_PATH
            )
            if(OBS_FRONTEND_INCLUDE_DIR AND OBS_FRONTEND_LIBRARY)
                set(OBS_FRONTEND_FOUND TRUE)
                set(OBS_FRONTEND_INCLUDE_DIRS "${OBS_FRONTEND_INCLUDE_DIR}")
                set(OBS_FRONTEND_LIBRARIES "${OBS_FRONTEND_LIBRARY}")
                message(STATUS "Found obs-frontend-api via OBS_SDK_PATH: ${OBS_FRONTEND_LIBRARY}")
            endif()
        endif()
    endif()

    # Fallback to pkg-config if not found via OBS_SDK_PATH
    if(NOT LIBOBS_FOUND OR NOT OBS_FRONTEND_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            if(NOT LIBOBS_FOUND)
                pkg_check_modules(LIBOBS libobs)
                # pkg-config returns library names, add library directory
                if(LIBOBS_FOUND AND LIBOBS_LIBRARY_DIRS)
                    link_directories(${LIBOBS_LIBRARY_DIRS})
                endif()
            endif()
            if(NOT OBS_FRONTEND_FOUND)
                pkg_check_modules(OBS_FRONTEND obs-frontend-api)
                if(OBS_FRONTEND_FOUND AND OBS_FRONTEND_LIBRARY_DIRS)
                    link_directories(${OBS_FRONTEND_LIBRARY_DIRS})
                endif()
            endif()
        endif()
    endif()

    # Try find_package as another fallback
    if(NOT LIBOBS_FOUND)
        find_package(libobs QUIET)
        if(libobs_FOUND)
            set(LIBOBS_FOUND TRUE)
        endif()
    endif()

    if(NOT OBS_FRONTEND_FOUND)
        find_package(obs-frontend-api QUIET)
        if(obs-frontend-api_FOUND)
            set(OBS_FRONTEND_FOUND TRUE)
        endif()
    endif()

    # Final fallback: manual search
    if(NOT LIBOBS_FOUND)
        find_path(LIBOBS_INCLUDE_DIR obs.h
            HINTS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include/obs include obs
        )
        find_library(LIBOBS_LIBRARY NAMES obs libobs
            HINTS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
        )
        if(LIBOBS_INCLUDE_DIR AND LIBOBS_LIBRARY)
            set(LIBOBS_FOUND TRUE)
            set(LIBOBS_INCLUDE_DIRS ${LIBOBS_INCLUDE_DIR})
            set(LIBOBS_LIBRARIES ${LIBOBS_LIBRARY})
        endif()
    endif()

    if(NOT OBS_FRONTEND_FOUND)
        find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
            HINTS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include/obs include obs include/obs-frontend-api include/obs-frontend-api/Headers
        )
        find_library(OBS_FRONTEND_LIBRARY NAMES obs-frontend-api libobs-frontend-api
            HINTS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
        )
        if(OBS_FRONTEND_INCLUDE_DIR AND OBS_FRONTEND_LIBRARY)
            set(OBS_FRONTEND_FOUND TRUE)
            set(OBS_FRONTEND_INCLUDE_DIRS ${OBS_FRONTEND_INCLUDE_DIR})
            set(OBS_FRONTEND_LIBRARIES ${OBS_FRONTEND_LIBRARY})
        endif()
    endif()

    if(NOT LIBOBS_FOUND OR NOT OBS_FRONTEND_FOUND)
        if(BUILD_TESTS)
            message(STATUS "OBS SDK not found - building tests only")
            set(BUILD_PLUGIN OFF)
        else()
            message(FATAL_ERROR "OBS SDK not found. Install libobs-dev or set CMAKE_PREFIX_PATH.")
        endif()
    endif()
endif()

if(BUILD_PLUGIN)
    # Some distro pkg-config files expose /usr/include while the public OBS
    # headers are installed below /usr/include/obs. Resolve that layout for
    # both the plugin and linked tests instead of requiring an SDK override.
    foreach(_include_dir IN LISTS LIBOBS_INCLUDE_DIRS)
        if(EXISTS "${_include_dir}/obs/obs-module.h")
            list(APPEND LIBOBS_INCLUDE_DIRS "${_include_dir}/obs")
        endif()
    endforeach()
    foreach(_include_dir IN LISTS OBS_FRONTEND_INCLUDE_DIRS)
        if(EXISTS "${_include_dir}/obs/obs-frontend-api.h")
            list(APPEND OBS_FRONTEND_INCLUDE_DIRS "${_include_dir}/obs")
        endif()
    endforeach()

    # Find libdatachannel
    find_package(LibDataChannel QUIET CONFIG)
    if(NOT LibDataChannel_FOUND)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBDATACHANNEL libdatachannel)
        endif()
    endif()

    if(NOT LibDataChannel_FOUND AND NOT LIBDATACHANNEL_FOUND)
        message(FATAL_ERROR "libdatachannel not found. Please install it or set CMAKE_PREFIX_PATH.")
    endif()

    # Plugin sources
    set(PLUGIN_SOURCES
        src/plugin-main.cpp
        src/vdoninja-alpha-sync.cpp
        src/vdoninja-audio-red.cpp
        src/vdoninja-bitrate-controller.cpp
        src/vdoninja-h264-profile.cpp
        src/vdoninja-ice-candidate-queue.cpp
        src/vdoninja-loss-protection.cpp
        src/vdoninja-output.cpp
        src/vdoninja-reliability.cpp
        src/vdoninja-rtcp-feedback.cpp
        src/vdoninja-rtp-pacer.cpp
        src/vdoninja-rtp-repair.cpp
        src/vdoninja-rtp-utils.cpp
        src/vdoninja-source.cpp
        src/vdoninja-signaling.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-auto-scene-manager.cpp
        src/vdoninja-layout.cpp
        src/vdoninja-module-lifecycle.cpp
        src/vdoninja-peer-manager.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-system-cpu.cpp
        src/vdoninja-utils.cpp
        src/vdoninja-dock.cpp
    )

    set(PLUGIN_HEADERS
        src/plugin-main.h
        src/vdoninja-alpha-sync.h
        src/vdoninja-audio-red.h
        src/vdoninja-bitrate-controller.h
        src/vdoninja-h264-profile.h
        src/vdoninja-auto-inbound-state.h
        src/vdoninja-ice-candidate-queue.h
        src/vdoninja-loss-protection.h
        src/vdoninja-common.h
        src/vdoninja-output.h
        src/vdoninja-reliability.h
        src/vdoninja-rtcp-feedback.h
        src/vdoninja-rtp-pacer.h
        src/vdoninja-rtp-repair.h
        src/vdoninja-source.h
        src/vdoninja-signaling.h
        src/vdoninja-signaling-protocol.h
        src/vdoninja-auto-scene-manager.h
        src/vdoninja-layout.h
        src/vdoninja-module-lifecycle.h
        src/vdoninja-peer-manager.h
        src/vdoninja-data-channel.h
        src/vdoninja-system-cpu.h
        src/vdoninja-utils.h
        src/vdoninja-video-keyframe-gate.h
        src/vdoninja-dock.h
    )

    # Create the plugin library
    add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        AUTOMOC ON
        NO_SYSTEM_FROM_IMPORTED ON
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    if(DEFINED OBS_SDK_PATH)
        if(EXISTS "${OBS_SDK_PATH}/include/obs")
            target_include_directories(${PROJECT_NAME} PRIVATE "${OBS_SDK_PATH}/include/obs")
        endif()
        if(EXISTS "${OBS_SDK_PATH}/include/obs-frontend-api")
            target_include_directories(${PROJECT_NAME} PRIVATE "${OBS_SDK_PATH}/include/obs-frontend-api")
        endif()
    endif()

    # Link libdatachannel
    if(LibDataChannel_FOUND)
        get_target_property(LIBDATACHANNEL_INCLUDE_DIRS LibDataChannel::LibDataChannel INTERFACE_INCLUDE_DIRECTORIES)
        if(LIBDATACHANNEL_INCLUDE_DIRS)
            target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
        endif()
        if(WIN32)
            if(TARGET LibDataChannel::LibDataChannelStatic)
                get_target_property(LIBDATACHANNEL_STATIC_INCLUDE_DIRS LibDataChannel::LibDataChannelStatic INTERFACE_INCLUDE_DIRECTORIES)
                if(LIBDATACHANNEL_STATIC_INCLUDE_DIRS)
                    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${LIBDATACHANNEL_STATIC_INCLUDE_DIRS})
                endif()
                target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannelStatic)
            else()
                if(TARGET LibDataChannel::LibDataChannel)
                    get_target_property(_ldc_defs LibDataChannel::LibDataChannel INTERFACE_COMPILE_DEFINITIONS)
                    if(_ldc_defs)
                        list(JOIN _ldc_defs ";" _ldc_defs_joined)
                    else()
                        set(_ldc_defs_joined "")
                    endif()
                    if(_ldc_defs_joined MATCHES "(^|;)RTC_STATIC($|;)")
                        target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
                    else()
                        message(FATAL_ERROR
                            "Windows build resolved LibDataChannel::LibDataChannel without RTC_STATIC and without "
                            "LibDataChannel::LibDataChannelStatic. This would import datachannel.dll at runtime.")
                    endif()
                else()
                    message(FATAL_ERROR "LibDataChannel target missing")
                endif()
            endif()
        else()
            target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
        endif()
    else()
        target_include_directories(${PROJECT_NAME} PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBDATACHANNEL_LIBRARIES})
    endif()

    # OBS deps includes also contain an rtc/ tree. Keep them behind libdatachannel
    # so rtc/*.hpp always matches the static library we link.
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIR})

    # Link OBS libraries
    if(TARGET OBS::libobs)
        target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api)
    elseif(LIBOBS_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBOBS_LIBRARIES})
        if(OBS_FRONTEND_LIBRARIES)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${OBS_FRONTEND_LIBRARIES})
        endif()
        if(LIBOBS_INCLUDE_DIRS)
            target_include_directories(${PROJECT_NAME} PRIVATE ${LIBOBS_INCLUDE_DIRS})
        endif()
        if(OBS_FRONTEND_INCLUDE_DIRS)
            target_include_directories(${PROJECT_NAME} PRIVATE ${OBS_FRONTEND_INCLUDE_DIRS})
        endif()
    endif()

    # Link Qt
    if(Qt6_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Widgets)
    endif()

    # Link other dependencies
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${FFMPEG_AVCODEC_LIBRARY}
        ${FFMPEG_AVUTIL_LIBRARY}
        ${FFMPEG_SWSCALE_LIBRARY}
        ${FFMPEG_SWRESAMPLE_LIBRARY}
    )
    if(OpenSSL_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()

    # Platform-specific settings
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32 bcrypt)
        target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
    elseif(APPLE)
        find_library(COREFOUNDATION_LIBRARY CoreFoundation)
        find_library(SECURITY_LIBRARY Security)
        if(COREFOUNDATION_LIBRARY AND SECURITY_LIBRARY)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${COREFOUNDATION_LIBRARY} ${SECURITY_LIBRARY})
        endif()
    elseif(UNIX)
        target_link_libraries(${PROJECT_NAME} PRIVATE pthread)
    endif()

    # Compiler warnings
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Set output properties
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${PROJECT_NAME} PREFIX "")

    # Installation
    include(GNUInstallDirs)

    if(WIN32)
        set(OBS_PLUGIN_DIR "obs-plugins/64bit")
        set(OBS_DATA_DIR "data/obs-plugins/${PROJECT_NAME}")
    elseif(APPLE)
        set(OBS_PLUGIN_DIR "lib/obs-plugins")
        set(OBS_DATA_DIR "share/obs/obs-plugins/${PROJECT_NAME}")
    else()
        set(OBS_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
        set(OBS_DATA_DIR "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PROJECT_NAME}")
    endif()

    install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION ${OBS_PLUGIN_DIR}
        RUNTIME DESTINATION ${OBS_PLUGIN_DIR}
    )

    if(BUNDLE_LINUX_RUNTIME)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET LibDataChannel::LibDataChannel)
            message(FATAL_ERROR "BUNDLE_LINUX_RUNTIME requires Linux and the LibDataChannel CMake package")
        endif()
        get_target_property(_ldc_type LibDataChannel::LibDataChannel TYPE)
        if(NOT _ldc_type STREQUAL "SHARED_LIBRARY")
            message(FATAL_ERROR "BUNDLE_LINUX_RUNTIME requires the shared LibDataChannel target")
        endif()
        # Keep the runtime beside the plugin in both system and per-user installs.
        # Do not expose it as another OBS module or require a global ldconfig change.
        set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN/obs-vdoninja")
        install(FILES "$<TARGET_FILE:LibDataChannel::LibDataChannel>"
            DESTINATION "${OBS_PLUGIN_DIR}/obs-vdoninja"
            RENAME "$<TARGET_SONAME_FILE_NAME:LibDataChannel::LibDataChannel>")
    endif()

    install(DIRECTORY data/
        DESTINATION ${OBS_DATA_DIR}
    )

    # CPack configuration
    set(CPACK_PACKAGE_NAME "obs-vdoninja")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_VENDOR ${PLUGIN_AUTHOR})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VDO.Ninja integration plugin for OBS Studio")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    include(CPack)
endif()

# Testing (can be built without OBS SDK)
if(BUILD_TESTS)
    enable_testing()

    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # Test sources that don't depend on OBS
    set(TESTABLE_SOURCES
        src/vdoninja-alpha-sync.cpp
        src/vdoninja-audio-red.cpp
        src/vdoninja-bitrate-controller.cpp
        src/vdoninja-h264-profile.cpp
        src/vdoninja-ice-candidate-queue.cpp
        src/vdoninja-loss-protection.cpp
        src/vdoninja-rtp-utils.cpp
        src/vdoninja-utils.cpp
        src/vdoninja-reliability.cpp
        src/vdoninja-rtcp-feedback.cpp
        src/vdoninja-rtp-pacer.cpp
        src/vdoninja-rtp-repair.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-signaling.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-layout.cpp
        src/vdoninja-module-lifecycle.cpp
        src/vdoninja-system-cpu.cpp
    )

    # Create a static library for testable code (with OBS stubs)
    add_library(vdoninja-testable STATIC
        ${TESTABLE_SOURCES}
        tests/stubs/obs-stubs.cpp
    )

    target_include_directories(vdoninja-testable PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs
    )

    if(OpenSSL_FOUND)
        target_link_libraries(vdoninja-testable PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(WIN32)
        target_link_libraries(vdoninja-testable PUBLIC bcrypt)
    endif()

    target_compile_definitions(vdoninja-testable PRIVATE TESTING_BUILD)
    if(MSVC)
        target_compile_options(vdoninja-testable PRIVATE /FS)
    endif()

    # Unit tests
    add_executable(vdoninja-tests
        tests/test-obs-clock.cpp
        tests/test-alpha-sync.cpp
        tests/test-audio-red.cpp
        tests/test-auto-inbound.cpp
        tests/test-bitrate-controller.cpp
        tests/test-h264-profile.cpp
        tests/test-ice-candidate-queue.cpp
        tests/test-loss-protection.cpp
        tests/test-module-lifecycle.cpp
        tests/test-peer-manager.cpp
        tests/test-utils.cpp
        tests/test-reliability.cpp
        tests/test-rtcp-feedback.cpp
        tests/test-rtp-audio.cpp
        tests/test-rtp-pacer.cpp
        tests/test-rtp-repair.cpp
        tests/test-rtc-stub-compat.cpp
        tests/test-json.cpp
        tests/test-data-channel.cpp
        tests/test-signaling-protocol.cpp
        tests/test-signaling-state.cpp
        tests/test-layout.cpp
        tests/test-system-cpu.cpp
        tests/test-vp9-rtp.cpp
    )

    target_include_directories(vdoninja-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs
    )

    target_link_libraries(vdoninja-tests PRIVATE
        vdoninja-testable
        GTest::gtest_main
        GTest::gmock
    )
    target_compile_definitions(vdoninja-tests PRIVATE TESTING_BUILD)
    if(MSVC)
        target_compile_options(vdoninja-tests PRIVATE /FS)
    endif()

    # Isolated module-boundary lifecycle gate. Keep this buildable while
    # peer/source integration work is staged in the shared test target.
    add_executable(vdoninja-module-lifecycle-tests
        tests/test-module-lifecycle.cpp
        src/vdoninja-module-lifecycle.cpp
    )
    target_include_directories(vdoninja-module-lifecycle-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(vdoninja-module-lifecycle-tests PRIVATE
        GTest::gtest_main
    )
    if(MSVC)
        target_compile_options(vdoninja-module-lifecycle-tests PRIVATE /FS)
    endif()

    # The documented and CI test build targets vdoninja-tests explicitly.
    # Keep the separately registered lifecycle executable in that build graph
    # so a clean CTest run never references a *_NOT_BUILT placeholder.
    add_dependencies(vdoninja-tests vdoninja-module-lifecycle-tests)

    include(GoogleTest)
    gtest_discover_tests(vdoninja-tests)
    gtest_discover_tests(vdoninja-module-lifecycle-tests TEST_PREFIX "ModuleBoundary.")
endif()

# ---------------------------------------------------------------------------
# Module-boundary lifecycle integration gate
# Exercises the real statically-linked libdatachannel logger and global RTC
# cleanup token. This is deliberately separate from the mock-based unit gate.
# ---------------------------------------------------------------------------
if(BUILD_MODULE_LIFECYCLE_LINKED_GATE)
    if(NOT BUILD_PLUGIN)
        message(FATAL_ERROR "BUILD_MODULE_LIFECYCLE_LINKED_GATE requires BUILD_PLUGIN=ON")
    endif()
    if(NOT EXISTS "${LIBDATACHANNEL_PLOG_INCLUDE_DIR}/plog/Log.h")
        message(FATAL_ERROR
            "LIBDATACHANNEL_PLOG_INCLUDE_DIR must contain plog/Log.h: ${LIBDATACHANNEL_PLOG_INCLUDE_DIR}")
    endif()

    add_executable(vdoninja-module-lifecycle-linked-gate
        tests/module-lifecycle-linked/main.cpp
        src/vdoninja-module-lifecycle.cpp
    )
    set_target_properties(vdoninja-module-lifecycle-linked-gate PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
    target_include_directories(vdoninja-module-lifecycle-linked-gate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LIBDATACHANNEL_PLOG_INCLUDE_DIR}
    )

    if(LibDataChannel_FOUND)
        get_target_property(_module_lifecycle_ldc_includes
            LibDataChannel::LibDataChannel INTERFACE_INCLUDE_DIRECTORIES)
        if(_module_lifecycle_ldc_includes)
            target_include_directories(vdoninja-module-lifecycle-linked-gate BEFORE PRIVATE
                ${_module_lifecycle_ldc_includes})
        endif()
        if(WIN32 AND TARGET LibDataChannel::LibDataChannelStatic)
            get_target_property(_module_lifecycle_ldc_static_includes
                LibDataChannel::LibDataChannelStatic INTERFACE_INCLUDE_DIRECTORIES)
            if(_module_lifecycle_ldc_static_includes)
                target_include_directories(vdoninja-module-lifecycle-linked-gate BEFORE PRIVATE
                    ${_module_lifecycle_ldc_static_includes})
            endif()
            target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE
                LibDataChannel::LibDataChannelStatic)
        else()
            target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE
                LibDataChannel::LibDataChannel)
        endif()
    else()
        target_include_directories(vdoninja-module-lifecycle-linked-gate PRIVATE
            ${LIBDATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE
            ${LIBDATACHANNEL_LIBRARIES})
    endif()

    target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE Threads::Threads)
    if(OpenSSL_FOUND)
        target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE
            OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(WIN32)
        target_compile_definitions(vdoninja-module-lifecycle-linked-gate PRIVATE
            _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
        target_link_libraries(vdoninja-module-lifecycle-linked-gate PRIVATE
            ws2_32 crypt32 bcrypt iphlpapi secur32 advapi32 user32 shell32)
    endif()
    if(MSVC)
        target_compile_options(vdoninja-module-lifecycle-linked-gate PRIVATE /W4 /FS)
    endif()

    enable_testing()
    add_test(NAME vdoninja-module-lifecycle-linked
        COMMAND $<TARGET_FILE:vdoninja-module-lifecycle-linked-gate>)
    set_tests_properties(vdoninja-module-lifecycle-linked PROPERTIES
        LABELS "module-lifecycle-linked;release-linked"
        TIMEOUT 60)

    message(STATUS "Static libdatachannel module lifecycle gate enabled")
endif()

# ---------------------------------------------------------------------------
# Native receiver media integration gate
# Compiles the production VDONinjaSource packet/decode/pair/output path against
# the same OBS, FFmpeg, and libdatachannel dependencies as the shipped plugin.
# It is intentionally a standalone executable: running it never launches OBS.
# ---------------------------------------------------------------------------
if(BUILD_NATIVE_MEDIA_LINKED_GATE)
    if(NOT BUILD_PLUGIN)
        message(FATAL_ERROR "BUILD_NATIVE_MEDIA_LINKED_GATE requires BUILD_PLUGIN=ON")
    endif()

    add_executable(vdoninja-native-media-linked-gate
        tests/native-media-linked/main.cpp
        src/vdoninja-alpha-sync.cpp
        src/vdoninja-audio-red.cpp
        src/vdoninja-bitrate-controller.cpp
        src/vdoninja-h264-profile.cpp
        src/vdoninja-ice-candidate-queue.cpp
        src/vdoninja-loss-protection.cpp
        src/vdoninja-reliability.cpp
        src/vdoninja-rtcp-feedback.cpp
        src/vdoninja-rtp-pacer.cpp
        src/vdoninja-rtp-repair.cpp
        src/vdoninja-rtp-utils.cpp
        src/vdoninja-source.cpp
        src/vdoninja-signaling.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-layout.cpp
        src/vdoninja-peer-manager.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-system-cpu.cpp
        src/vdoninja-utils.cpp
    )
    set_target_properties(vdoninja-native-media-linked-gate PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
    target_compile_definitions(vdoninja-native-media-linked-gate PRIVATE
        VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST
    )
    target_include_directories(vdoninja-native-media-linked-gate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    if(DEFINED OBS_SDK_PATH)
        if(EXISTS "${OBS_SDK_PATH}/include/obs")
            target_include_directories(vdoninja-native-media-linked-gate PRIVATE "${OBS_SDK_PATH}/include/obs")
        endif()
        if(EXISTS "${OBS_SDK_PATH}/include/obs-frontend-api")
            target_include_directories(vdoninja-native-media-linked-gate PRIVATE
                "${OBS_SDK_PATH}/include/obs-frontend-api")
        endif()
    endif()

    if(LibDataChannel_FOUND)
        get_target_property(_native_media_ldc_includes
            LibDataChannel::LibDataChannel INTERFACE_INCLUDE_DIRECTORIES)
        if(_native_media_ldc_includes)
            target_include_directories(vdoninja-native-media-linked-gate BEFORE PRIVATE
                ${_native_media_ldc_includes})
        endif()
        if(WIN32 AND TARGET LibDataChannel::LibDataChannelStatic)
            get_target_property(_native_media_ldc_static_includes
                LibDataChannel::LibDataChannelStatic INTERFACE_INCLUDE_DIRECTORIES)
            if(_native_media_ldc_static_includes)
                target_include_directories(vdoninja-native-media-linked-gate BEFORE PRIVATE
                    ${_native_media_ldc_static_includes})
            endif()
            target_link_libraries(vdoninja-native-media-linked-gate PRIVATE
                LibDataChannel::LibDataChannelStatic)
        else()
            target_link_libraries(vdoninja-native-media-linked-gate PRIVATE LibDataChannel::LibDataChannel)
        endif()
    else()
        target_include_directories(vdoninja-native-media-linked-gate PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(vdoninja-native-media-linked-gate PRIVATE ${LIBDATACHANNEL_LIBRARIES})
    endif()

    target_include_directories(vdoninja-native-media-linked-gate SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIR})
    if(TARGET OBS::libobs)
        target_link_libraries(vdoninja-native-media-linked-gate PRIVATE OBS::libobs)
    elseif(LIBOBS_LIBRARIES)
        target_link_libraries(vdoninja-native-media-linked-gate PRIVATE ${LIBOBS_LIBRARIES})
        if(LIBOBS_INCLUDE_DIRS)
            target_include_directories(vdoninja-native-media-linked-gate PRIVATE ${LIBOBS_INCLUDE_DIRS})
        endif()
    endif()
    target_link_libraries(vdoninja-native-media-linked-gate PRIVATE
        Threads::Threads
        ${FFMPEG_AVCODEC_LIBRARY}
        ${FFMPEG_AVUTIL_LIBRARY}
        ${FFMPEG_SWSCALE_LIBRARY}
        ${FFMPEG_SWRESAMPLE_LIBRARY}
    )
    if(OpenSSL_FOUND)
        target_link_libraries(vdoninja-native-media-linked-gate PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(WIN32)
        target_compile_definitions(vdoninja-native-media-linked-gate PRIVATE
            _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
        target_link_libraries(vdoninja-native-media-linked-gate PRIVATE ws2_32 crypt32 bcrypt)
    endif()
    if(MSVC)
        target_compile_options(vdoninja-native-media-linked-gate PRIVATE /W4 /wd4201 /FS)
    endif()

    # Keep the opt-in linked gate bounded even when a concurrency regression
    # strands an internal join. These registrations remain disabled unless the
    # linked gate itself is explicitly enabled at configure time.
    enable_testing()
    add_test(NAME vdoninja-native-media-linked-owner-lifetime-track
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "Track in-flight callback is safe during manager destruction")
    add_test(NAME vdoninja-native-media-linked-owner-lifetime-data-channel
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "DataChannel in-flight callback is safe during manager destruction")
    add_test(NAME vdoninja-native-media-linked-owner-order-peer-connection
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "PeerConnection description and feedback functions share one owner session")
    add_test(NAME vdoninja-native-media-linked-owner-order-same-handle-data-channel
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "same-handle DataChannel replacement drains before detaching functions")
    add_test(NAME vdoninja-native-media-linked-owner-hook-pre-permit
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "completion delayed until after owner shutdown is rejected")
    add_test(NAME vdoninja-native-media-linked-owner-hook-registration-failure
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "installed function registration failure detaches setter")
    add_test(NAME vdoninja-native-media-linked-owner-hook-track-kinds
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "video alpha audio Track completions share one owner session")
    add_test(NAME vdoninja-native-media-linked-owner-hook-feedback
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>
            "admitted feedback completion drains before owner state")
    add_test(NAME vdoninja-native-media-linked-full
        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>)
    set_tests_properties(
        vdoninja-native-media-linked-owner-lifetime-track
        vdoninja-native-media-linked-owner-lifetime-data-channel
        PROPERTIES
            LABELS "native-media-linked;owner-lifetime;release-linked"
            TIMEOUT 30)
    set_tests_properties(
        vdoninja-native-media-linked-owner-order-peer-connection
        vdoninja-native-media-linked-owner-order-same-handle-data-channel
        PROPERTIES
            LABELS "native-media-linked;owner-lifetime;release-linked"
            TIMEOUT 60)
    set_tests_properties(
        vdoninja-native-media-linked-owner-hook-pre-permit
        vdoninja-native-media-linked-owner-hook-registration-failure
        vdoninja-native-media-linked-owner-hook-track-kinds
        vdoninja-native-media-linked-owner-hook-feedback
        PROPERTIES
            LABELS "native-media-linked;owner-lifetime;release-linked"
            TIMEOUT 60)
    set_tests_properties(vdoninja-native-media-linked-full PROPERTIES
        LABELS "native-media-linked;release-linked"
        TIMEOUT 600)

    message(STATUS "OBS/FFmpeg-linked native receiver media gate enabled")
endif()

# ---------------------------------------------------------------------------
# VP9 alpha publisher test tool
# Requires the same dependency set as the plugin (libdatachannel + FFmpeg).
# Configure with: cmake -DBUILD_PUBLISHER_TOOL=ON -DBUILD_PLUGIN=ON ...
# ---------------------------------------------------------------------------
if(BUILD_PUBLISHER_TOOL)
    if(NOT BUILD_PLUGIN)
        message(FATAL_ERROR "BUILD_PUBLISHER_TOOL requires BUILD_PLUGIN=ON (needs libdatachannel + FFmpeg)")
    endif()

    add_executable(vp9-alpha-publisher
        tests/tools/vp9-alpha-publisher/main.cpp
        src/vdoninja-signaling.cpp
        src/vdoninja-utils.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-reliability.cpp
        src/vdoninja-rtp-utils.cpp
        tests/stubs/obs-stubs.cpp
    )
    set_target_properties(vp9-alpha-publisher PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)

    # Expose only the OBS stub: tests/stubs also contains fake rtc/ headers.
    # Include ordering cannot reliably override those when the real SDK lives
    # in a compiler implicit include directory such as /usr/local/include.
    set(_publisher_obs_stubs "${CMAKE_CURRENT_BINARY_DIR}/publisher-obs-stubs")
    configure_file(tests/stubs/obs-module.h "${_publisher_obs_stubs}/obs-module.h" COPYONLY)
    target_include_directories(vp9-alpha-publisher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_publisher_obs_stubs}
    )
    # Same rtc/ header collision as the plugin target.
    target_include_directories(vp9-alpha-publisher SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIR})

    # Re-use exactly the same libdatachannel link target resolved by the plugin section
    if(LibDataChannel_FOUND)
        if(TARGET LibDataChannel::LibDataChannelStatic)
            set(_publisher_ldc_target LibDataChannel::LibDataChannelStatic)
        else()
            set(_publisher_ldc_target LibDataChannel::LibDataChannel)
        endif()
        target_link_libraries(vp9-alpha-publisher PRIVATE ${_publisher_ldc_target})
    endif()

    target_link_libraries(vp9-alpha-publisher PRIVATE
        ${FFMPEG_AVCODEC_LIBRARY}
        ${FFMPEG_AVUTIL_LIBRARY}
        ${FFMPEG_SWSCALE_LIBRARY}
        Threads::Threads
    )

    if(OpenSSL_FOUND)
        target_link_libraries(vp9-alpha-publisher PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()

    if(WIN32)
        target_link_libraries(vp9-alpha-publisher PRIVATE bcrypt)
    endif()

    if(MSVC)
        target_compile_options(vp9-alpha-publisher PRIVATE /W3 /wd4201)
        # Static libdatachannel can cause COMDAT symbol conflicts with non-inline virtuals.
        target_link_options(vp9-alpha-publisher PRIVATE /FORCE:MULTIPLE)
    endif()

    message(STATUS "VP9 alpha publisher tool enabled")
endif()

# Opt-in real OBS rendering fixture; never installed in release packages.
option(BUILD_OBS_CLOCK_FIXTURE "Build the OBS-clocked frame-counter source for E2E isolation" OFF)
if(BUILD_OBS_CLOCK_FIXTURE)
    if(NOT BUILD_PLUGIN)
        message(FATAL_ERROR "BUILD_OBS_CLOCK_FIXTURE requires BUILD_PLUGIN=ON")
    endif()
    add_library(vdoninja-clock-fixture MODULE tests/obs-clock-source/main.cpp)
    set_target_properties(vdoninja-clock-fixture PROPERTIES PREFIX "")
    target_include_directories(vdoninja-clock-fixture PRIVATE ${LIBOBS_INCLUDE_DIRS})
    # ARM OBS graphics headers use SIMDe from the OBS dependency prefix.
    find_path(VDONINJA_OBS_SIMDE_INCLUDE_DIR simde/x86/sse2.h)
    if(VDONINJA_OBS_SIMDE_INCLUDE_DIR)
        target_include_directories(vdoninja-clock-fixture SYSTEM PRIVATE ${VDONINJA_OBS_SIMDE_INCLUDE_DIR})
    endif()
    if(TARGET OBS::libobs)
        target_link_libraries(vdoninja-clock-fixture PRIVATE OBS::libobs)
    else()
        target_link_libraries(vdoninja-clock-fixture PRIVATE ${LIBOBS_LIBRARIES})
    endif()
endif()

option(BUILD_RTP_TIMING_TRACE "Build bounded in-memory RTP/SR tracing for synthetic E2E isolation" OFF)
if(BUILD_RTP_TIMING_TRACE)
    if(NOT BUILD_PLUGIN)
        message(FATAL_ERROR "BUILD_RTP_TIMING_TRACE requires BUILD_PLUGIN=ON")
    endif()
    target_compile_definitions(obs-vdoninja PRIVATE VDONINJA_ENABLE_TIMING_TRACE)
endif()
if(BUILD_RTP_TIMING_TRACE AND BUILD_TESTS)
    add_executable(vdoninja-rtp-timing-trace-tests tests/test-rtp-timing-trace.cpp src/vdoninja-rtp-repair.cpp)
    target_link_libraries(vdoninja-rtp-timing-trace-tests PRIVATE GTest::gtest_main)
    if(TARGET LibDataChannel::LibDataChannel)
        target_link_libraries(vdoninja-rtp-timing-trace-tests PRIVATE LibDataChannel::LibDataChannel)
    else()
        target_include_directories(vdoninja-rtp-timing-trace-tests PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(vdoninja-rtp-timing-trace-tests PRIVATE ${LIBDATACHANNEL_LIBRARIES})
    endif()
    add_test(NAME rtp-timing-trace COMMAND vdoninja-rtp-timing-trace-tests)
    set_tests_properties(rtp-timing-trace PROPERTIES TIMEOUT 30 LABELS diagnostics)
endif()
