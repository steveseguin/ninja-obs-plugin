cmake_minimum_required(VERSION 3.22...3.25)

project(obs-vdoninja VERSION 1.0.0)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/obs-vdoninja/issues")
set(PLUGIN_LICENSE "AGPL-3.0")

# Option to build tests
option(BUILD_TESTS "Build unit tests" OFF)

# Build configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find dependencies
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

# Try to find libdatachannel
find_package(LibDataChannel QUIET)
if(NOT LibDataChannel_FOUND)
    # Use pkg-config as fallback
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDATACHANNEL libdatachannel)
    endif()
endif()

# Find OpenSSL for SHA256 hashing
find_package(OpenSSL REQUIRED)

# Find websocketpp or use bundled
find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/config/asio_client.hpp)
find_package(Boost COMPONENTS system QUIET)

# Plugin sources
set(PLUGIN_SOURCES
    src/plugin-main.cpp
    src/vdoninja-signaling.cpp
    src/vdoninja-peer-manager.cpp
    src/vdoninja-output.cpp
    src/vdoninja-source.cpp
    src/vdoninja-utils.cpp
    src/vdoninja-data-channel.cpp
)

set(PLUGIN_HEADERS
    src/plugin-main.h
    src/vdoninja-signaling.h
    src/vdoninja-peer-manager.h
    src/vdoninja-output.h
    src/vdoninja-source.h
    src/vdoninja-utils.h
    src/vdoninja-data-channel.h
    src/vdoninja-common.h
)

# Create the plugin module
add_library(obs-vdoninja MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

# Include directories
target_include_directories(obs-vdoninja PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
)

if(LIBDATACHANNEL_FOUND OR LibDataChannel_FOUND)
    target_include_directories(obs-vdoninja PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
endif()

if(WEBSOCKETPP_INCLUDE_DIR)
    target_include_directories(obs-vdoninja PRIVATE ${WEBSOCKETPP_INCLUDE_DIR})
endif()

# Link libraries
target_link_libraries(obs-vdoninja PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(LIBDATACHANNEL_FOUND OR LibDataChannel_FOUND)
    target_link_libraries(obs-vdoninja PRIVATE ${LIBDATACHANNEL_LIBRARIES})
else()
    target_link_libraries(obs-vdoninja PRIVATE datachannel)
endif()

if(Boost_FOUND)
    target_link_libraries(obs-vdoninja PRIVATE Boost::system)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(obs-vdoninja PRIVATE ws2_32 crypt32)
    target_compile_definitions(obs-vdoninja PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
    )
elseif(APPLE)
    set_target_properties(obs-vdoninja PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION plugin
        MACOSX_BUNDLE_GUI_IDENTIFIER com.vdoninja.obs-plugin
    )
elseif(UNIX)
    target_link_libraries(obs-vdoninja PRIVATE pthread)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(obs-vdoninja PRIVATE /W4)
else()
    target_compile_options(obs-vdoninja PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Installation
include(GNUInstallDirs)

if(WIN32)
    set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
    set(OBS_DATA_DESTINATION "data/obs-plugins/obs-vdoninja")
elseif(APPLE)
    set(OBS_PLUGIN_DESTINATION "Library/Application Support/obs-studio/plugins")
    set(OBS_DATA_DESTINATION "Library/Application Support/obs-studio/plugins/obs-vdoninja/data")
else()
    set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-vdoninja")
endif()

install(TARGETS obs-vdoninja
    LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION}
    RUNTIME DESTINATION ${OBS_PLUGIN_DESTINATION}
)

install(DIRECTORY data/locale
    DESTINATION ${OBS_DATA_DESTINATION}
)

# Testing
if(BUILD_TESTS)
    enable_testing()

    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
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

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "obs-vdoninja")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR ${PLUGIN_AUTHOR})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VDO.Ninja integration plugin for OBS Studio")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
