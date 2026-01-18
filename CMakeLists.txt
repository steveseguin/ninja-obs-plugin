cmake_minimum_required(VERSION 3.16)

project(obs-vdoninja VERSION 1.0.0)

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

# Find OpenSSL (required for both plugin and tests)
find_package(OpenSSL REQUIRED)

# Build plugin only if requested and OBS SDK is available
if(BUILD_PLUGIN)
    # Try to find OBS SDK
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBOBS libobs)
        pkg_check_modules(OBS_FRONTEND obs-frontend-api)
    endif()

    # Also try CMake find_package
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

    if(NOT LIBOBS_FOUND OR NOT OBS_FRONTEND_FOUND)
        if(BUILD_TESTS)
            message(STATUS "OBS SDK not found - building tests only")
            set(BUILD_PLUGIN OFF)
        else()
            message(FATAL_ERROR "OBS SDK not found. Please set OBS_INCLUDE_DIRS and ensure OBS libraries are in your path.")
        endif()
    endif()
endif()

if(BUILD_PLUGIN)
    # Try to find libdatachannel
    find_package(LibDataChannel QUIET)
    if(NOT LibDataChannel_FOUND)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBDATACHANNEL libdatachannel)
        endif()
    endif()

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

    if(LIBOBS_INCLUDE_DIRS)
        target_include_directories(obs-vdoninja PRIVATE ${LIBOBS_INCLUDE_DIRS})
    endif()

    if(LIBDATACHANNEL_FOUND OR LibDataChannel_FOUND)
        target_include_directories(obs-vdoninja PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
    endif()

    if(WEBSOCKETPP_INCLUDE_DIR)
        target_include_directories(obs-vdoninja PRIVATE ${WEBSOCKETPP_INCLUDE_DIR})
    endif()

    # Link libraries
    target_link_libraries(obs-vdoninja PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
    )

    # Link OBS libraries
    if(TARGET OBS::libobs)
        target_link_libraries(obs-vdoninja PRIVATE OBS::libobs OBS::obs-frontend-api)
    elseif(LIBOBS_LIBRARIES)
        target_link_libraries(obs-vdoninja PRIVATE ${LIBOBS_LIBRARIES} ${OBS_FRONTEND_LIBRARIES})
    endif()

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

    # CPack configuration for packaging
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
