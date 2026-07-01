cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" RUMI_VERSION_FULL)
string(STRIP "${RUMI_VERSION_FULL}" RUMI_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" RUMI_VERSION_NUMERIC
    "${RUMI_VERSION_FULL}")

project(rumi
    VERSION     ${RUMI_VERSION_NUMERIC}
    DESCRIPTION "A GeoTIFF profile for AI training data"
    LANGUAGES   CXX C
)

option(RUMI_BUILD_PLUGIN      "Build the GDAL auto-loadable plugin"               ON)
option(RUMI_BUILD_SHARED_LIB  "Build standalone librumi for direct link"          OFF)

# geozl owns OpenZL and brings it in transitively, so rumi vendors geozl only.
# The VERSION file sits one level up, so the submodule at the repo root is one
# level up from this file too.
set(RUMI_GEOZL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/geozl"
    CACHE PATH "Path to the geozl submodule source")

# e.g. -DRUMI_SANITIZE=thread, or address,undefined. Empty = off. Use a
# throwaway build dir for this, it's a sanitizer-only configuration.
set(RUMI_SANITIZE "" CACHE STRING "Sanitizer to compile and link with")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD              23)
set(CMAKE_CXX_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_EXTENSIONS            OFF)

# C stays enabled for the geozl sub-build, which is C and carries OpenZL. rumi
# itself is C++ only.
set(CMAKE_C_STANDARD                11)
set(CMAKE_C_STANDARD_REQUIRED       ON)
set(CMAKE_C_EXTENSIONS              OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# compile_commands.json for clangd and friends, so the warnings show in editor.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hide everything by default. The C++ surface is re-exported with RUMI_API and
# the extern "C" entry points with GDAL's CPL_DLL. Hiding keeps the vendored
# OpenZL and its bundled zstd out of the plugin's dynamic symbol table, which
# matters because GDAL also links a zstd.
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_C_VISIBILITY_PRESET       hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

find_package(GDAL 3.12 CONFIG REQUIRED)

# geozl is the OpenZL frame codec (compression 60000) plus rumi's spatial
# decoders. Vendored static so it folds into rumi with no extra runtime
# dependency, and GEOZL_STATIC keeps its entry points from being re-exported.
# geozl exposes OpenZL PUBLIC, so rumi links one OpenZL, the same one geozl
# registers its decoders against.
set(BUILD_SHARED_LIBS OFF)
set(GEOZL_BUILD_KERNELS OFF CACHE BOOL "" FORCE)   # the cffi kernels lib is a Python concern
add_subdirectory(${RUMI_GEOZL_DIR}/core ${CMAKE_CURRENT_BINARY_DIR}/geozl EXCLUDE_FROM_ALL)
target_compile_definitions(geozl PUBLIC GEOZL_STATIC)

set(RUMI_SOURCES
    src/parser.cpp
    src/builder.cpp
    src/geokeys.cpp
    src/plan.cpp
    src/read.cpp
    src/pattern.cpp
    src/image.cpp
    src/capi.cpp
)

# Compile and link settings shared by every target. rumi_config is internal,
# linked PRIVATE, never installed, so the build defines here do not leak to
# consumers of the installed library.
add_library(rumi_config INTERFACE)
target_include_directories(rumi_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rumi_config INTERFACE
    GDAL::GDAL
    geozl::geozl
)
target_compile_definitions(rumi_config INTERFACE
    RUMI_BUILD
    RUMI_VERSION_STRING="${RUMI_VERSION_FULL}"
)
# Warnings on rumi's own C++. geozl and OpenZL build with their own flags through
# add_subdirectory, so this does not touch them.
target_compile_options(rumi_config INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /permissive->
)

# A sanitizer applies to every target through this interface: lib and plugin.
# It has to be on both the compile and the link line.
if(RUMI_SANITIZE)
    target_compile_options(rumi_config INTERFACE
        -fsanitize=${RUMI_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(rumi_config INTERFACE
        -fsanitize=${RUMI_SANITIZE})
endif()

# GDAL's loader scans for gdal_<NAME>.{so,dll,dylib} and calls
# GDALRegister_<NAME>(). The empty PREFIX strips the "lib" CMake would prepend.
if(RUMI_BUILD_PLUGIN)
    add_library(gdal_RUMI MODULE ${RUMI_SOURCES})
    target_link_libraries(gdal_RUMI PRIVATE rumi_config)
    set_target_properties(gdal_RUMI PROPERTIES
        PREFIX      ""
        OUTPUT_NAME "gdal_RUMI"
    )

    # GDAL's compiled-in plugin path is <prefix>/lib/gdalplugins on every
    # platform. A MODULE .dll is a RUNTIME artifact on Windows, so it also goes
    # to lib/gdalplugins, not bin, or GDAL would not find it.
    install(TARGETS gdal_RUMI
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gdalplugins
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/gdalplugins
    )
endif()

# For apps that link directly and call rumi::register_driver() themselves
# instead of relying on plugin auto-discovery.
if(RUMI_BUILD_SHARED_LIB)
    add_library(rumi SHARED ${RUMI_SOURCES})
    add_library(rumi::rumi ALIAS rumi)
    target_link_libraries(rumi PUBLIC rumi_config)
    set_target_properties(rumi PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )

    install(TARGETS rumi
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES include/rumi.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/rumi
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )
endif()

message(STATUS "")
message(STATUS "rumi ${RUMI_VERSION_FULL}")
message(STATUS "  GDAL:           ${GDAL_VERSION}")
message(STATUS "  geozl:          ${RUMI_GEOZL_DIR}")
message(STATUS "  OpenZL:         via geozl")
message(STATUS "  Plugin:         ${RUMI_BUILD_PLUGIN}")
message(STATUS "  Standalone lib: ${RUMI_BUILD_SHARED_LIB}")
message(STATUS "  Sanitize:       ${RUMI_SANITIZE}")
message(STATUS "")