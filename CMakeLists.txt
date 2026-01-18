cmake_minimum_required(VERSION 3.22...3.25)

project(obs-vdoninja VERSION 1.0.0)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/obs-vdoninja/issues")

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

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "obs-vdoninja")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR ${PLUGIN_AUTHOR})
include(CPack)
