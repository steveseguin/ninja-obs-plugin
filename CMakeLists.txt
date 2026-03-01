cmake_minimum_required(VERSION 3.16)

project(obs-vdoninja VERSION 1.1.16 LANGUAGES C CXX)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/ninja-obs-plugin/issues")
set(PLUGIN_LICENSE "AGPL-3.0-only")

# Options
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_PLUGIN "Build the OBS plugin (requires OBS SDK)" ON)

# Build configuration
# Use C++20 on Windows for designated initializers support (MSVC requires /std:c++20)
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
    add_compile_options(/Zc:__cplusplus)
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
        src/vdoninja-output.cpp
        src/vdoninja-source.cpp
        src/vdoninja-signaling.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-auto-scene-manager.cpp
        src/vdoninja-layout.cpp
        src/vdoninja-peer-manager.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-utils.cpp
        src/vdoninja-dock.cpp
    )

    set(PLUGIN_HEADERS
        src/plugin-main.h
        src/vdoninja-common.h
        src/vdoninja-output.h
        src/vdoninja-source.h
        src/vdoninja-signaling.h
        src/vdoninja-signaling-protocol.h
        src/vdoninja-auto-scene-manager.h
        src/vdoninja-layout.h
        src/vdoninja-peer-manager.h
        src/vdoninja-data-channel.h
        src/vdoninja-utils.h
        src/vdoninja-dock.h
    )

    # Create the plugin library
    add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
    set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

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
        if(WIN32)
            if(TARGET LibDataChannel::LibDataChannelStatic)
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
    if(OpenSSL_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()

    # Platform-specific settings
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32)
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
        src/vdoninja-utils.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-signaling-protocol.cpp
        src/vdoninja-layout.cpp
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

    target_compile_definitions(vdoninja-testable PRIVATE TESTING_BUILD)

    # Unit tests
    add_executable(vdoninja-tests
        tests/test-utils.cpp
        tests/test-json.cpp
        tests/test-data-channel.cpp
        tests/test-signaling-protocol.cpp
        tests/test-layout.cpp
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

    include(GoogleTest)
    gtest_discover_tests(vdoninja-tests)
endif()
