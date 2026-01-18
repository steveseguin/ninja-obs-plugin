cmake_minimum_required(VERSION 3.22...3.28)

# Determine if building standalone or as part of OBS
if(NOT DEFINED OBS_CMAKE_VERSION)
  set(STANDALONE_BUILD TRUE)
endif()

project(obs-vdoninja VERSION 1.0.0 LANGUAGES C CXX)

# Plugin configuration
set(PLUGIN_AUTHOR "VDO.Ninja Community")
set(PLUGIN_WEBSITE "https://vdo.ninja")
set(PLUGIN_SUPPORT_URL "https://github.com/steveseguin/obs-vdoninja/issues")

# Build configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Platform-specific settings
if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# Find OBS SDK
if(STANDALONE_BUILD)
  # Try to find OBS using pkg-config first (Linux)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBOBS libobs)
    pkg_check_modules(OBS_FRONTEND obs-frontend-api)
  endif()

  # If pkg-config didn't work, try CMake config
  if(NOT LIBOBS_FOUND)
    find_package(libobs QUIET)
  endif()

  # Manual fallback for include directories
  if(NOT LIBOBS_FOUND AND NOT libobs_FOUND)
    # Check common locations
    find_path(OBS_INCLUDE_DIR
      NAMES obs-module.h obs.h
      PATHS
        ${OBS_INCLUDE_DIRS}
        /usr/include/obs
        /usr/local/include/obs
        /opt/homebrew/include/obs
        "C:/Program Files/obs-studio/include"
      PATH_SUFFIXES obs libobs
    )

    find_library(OBS_LIB
      NAMES obs libobs
      PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
        "C:/Program Files/obs-studio/bin/64bit"
    )

    find_library(OBS_FRONTEND_LIB
      NAMES obs-frontend-api
      PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
        "C:/Program Files/obs-studio/bin/64bit"
    )

    if(OBS_INCLUDE_DIR AND OBS_LIB)
      set(LIBOBS_FOUND TRUE)
      set(LIBOBS_INCLUDE_DIRS ${OBS_INCLUDE_DIR})
      set(LIBOBS_LIBRARIES ${OBS_LIB})
      if(OBS_FRONTEND_LIB)
        set(OBS_FRONTEND_LIBRARIES ${OBS_FRONTEND_LIB})
      endif()
    endif()
  endif()

  if(NOT LIBOBS_FOUND AND NOT libobs_FOUND)
    message(FATAL_ERROR "OBS SDK not found. Please set OBS_INCLUDE_DIRS and ensure OBS libraries are in your path.")
  endif()
else()
  # Building as part of OBS - use provided targets
  if(NOT TARGET OBS::libobs)
    message(FATAL_ERROR "OBS::libobs target not found")
  endif()
endif()

# Find libdatachannel
find_package(LibDataChannel QUIET CONFIG)
if(NOT LibDataChannel_FOUND)
  # Try pkg-config
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBDATACHANNEL libdatachannel)
  endif()

  if(NOT LIBDATACHANNEL_FOUND)
    # Manual search
    find_path(LIBDATACHANNEL_INCLUDE_DIRS
      NAMES rtc/rtc.hpp
      PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
        ${CMAKE_PREFIX_PATH}/include
    )

    find_library(LIBDATACHANNEL_LIBRARIES
      NAMES datachannel datachannel-static
      PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
        ${CMAKE_PREFIX_PATH}/lib
    )

    if(LIBDATACHANNEL_INCLUDE_DIRS AND LIBDATACHANNEL_LIBRARIES)
      set(LIBDATACHANNEL_FOUND TRUE)
    endif()
  endif()
endif()

if(NOT LibDataChannel_FOUND AND NOT LIBDATACHANNEL_FOUND)
  message(FATAL_ERROR "libdatachannel not found. Please install it or set CMAKE_PREFIX_PATH.")
endif()

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find threads
find_package(Threads REQUIRED)

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

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add OBS includes
if(STANDALONE_BUILD)
  if(LIBOBS_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBOBS_INCLUDE_DIRS})
  endif()
  if(libobs_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${libobs_INCLUDE_DIRS})
  endif()
endif()

# Add libdatachannel includes
if(LibDataChannel_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
else()
  target_include_directories(${PROJECT_NAME} PRIVATE ${LIBDATACHANNEL_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBDATACHANNEL_LIBRARIES})
endif()

# Link OBS libraries
if(STANDALONE_BUILD)
  if(TARGET OBS::libobs)
    target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
  elseif(LIBOBS_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBOBS_LIBRARIES})
  endif()

  if(TARGET OBS::obs-frontend-api)
    target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  elseif(OBS_FRONTEND_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OBS_FRONTEND_LIBRARIES})
  endif()
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api)
endif()

# Link other dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

# Platform-specific linking
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0601)
elseif(APPLE)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  find_library(SECURITY_LIBRARY Security)
  target_link_libraries(${PROJECT_NAME} PRIVATE
    ${COREFOUNDATION_LIBRARY}
    ${SECURITY_LIBRARY}
  )
endif()

# Compiler warnings
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Set output properties
set_target_properties(${PROJECT_NAME} PROPERTIES
  OUTPUT_NAME ${PROJECT_NAME}
  PREFIX ""
)

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    BUNDLE FALSE
    MACOSX_BUNDLE FALSE
    SUFFIX ".so"
  )
elseif(WIN32)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    SUFFIX ".dll"
  )
endif()

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

# CPack configuration for creating packages
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "${PLUGIN_AUTHOR}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VDO.Ninja WebRTC Plugin for OBS Studio")

if(WIN32)
  set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ;DEB")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${PLUGIN_AUTHOR}")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio (>= 28.0)")
endif()

include(CPack)

# Print configuration summary
message(STATUS "")
message(STATUS "=== OBS VDO.Ninja Plugin Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
if(LibDataChannel_FOUND)
  message(STATUS "libdatachannel: Found (CMake package)")
elseif(LIBDATACHANNEL_FOUND)
  message(STATUS "libdatachannel: Found at ${LIBDATACHANNEL_LIBRARIES}")
endif()
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
message(STATUS "")
