cmake_minimum_required(VERSION 3.16)

project(obs-vdoninja VERSION 1.0.2 LANGUAGES C CXX)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/obs-vdoninja/issues")
set(PLUGIN_LICENSE "AGPL-3.0")

# Options
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_PLUGIN "Build the OBS plugin (requires OBS SDK)" ON)

# Build configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find OpenSSL (required for both plugin and tests)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

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
    # Try to find OBS SDK
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBOBS libobs)
        pkg_check_modules(OBS_FRONTEND obs-frontend-api)
    endif()

    if(NOT LIBOBS_FOUND)
        find_package(libobs QUIET)
        if(libobs_FOUND)
            set(LIBOBS_FOUND TRUE)
        endif()
    endif()

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
        find_package(obs-frontend-api QUIET)
        if(obs-frontend-api_FOUND)
            set(OBS_FRONTEND_FOUND TRUE)
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
        src/vdoninja-peer-manager.cpp
        src/vdoninja-data-channel.cpp
        src/vdoninja-utils.cpp
    )

    set(PLUGIN_HEADERS
        src/plugin-main.h
        src/vdoninja-common.h
        src/vdoninja-output.h
        src/vdoninja-source.h
        src/vdoninja-signaling.h
        src/vdoninja-peer-manager.h
        src/vdoninja-data-channel.h
        src/vdoninja-utils.h
    )

    # Create the plugin library
    add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

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
        target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
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

    # Link other dependencies
    target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )

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
    )

    # Create a static library for testable code (with OBS stubs)
    add_library(vdoninja-testable STATIC
        ${TESTABLE_SOURCES}
        tests/stubs/obs-stubs.cpp
    )

    target_include_directories(vdoninja-testable PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs
        ${OPENSSL_INCLUDE_DIR}
    )

    target_link_libraries(vdoninja-testable PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
    )

    target_compile_definitions(vdoninja-testable PRIVATE TESTING_BUILD)

    # Unit tests
    add_executable(vdoninja-tests
        tests/test-utils.cpp
        tests/test-json.cpp
        tests/test-data-channel.cpp
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
