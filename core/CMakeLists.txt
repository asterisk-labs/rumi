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
option(RUMI_USE_SYSTEM_OPENZL "Link an installed OpenZL instead of the submodule" OFF)

# Path to the vendored OpenZL. The VERSION file sits one level up, so the
# submodule at the repo root is one level up from this file too.
set(RUMI_OPENZL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/openzl"
    CACHE PATH "Path to the OpenZL submodule source")

# e.g. -DRUMI_SANITIZE=thread, or address,undefined. Empty = off. Use a
# throwaway build dir for this, it's a sanitizer-only configuration.
set(RUMI_SANITIZE "" CACHE STRING "Sanitizer to compile and link with")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD              23)
set(CMAKE_CXX_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_EXTENSIONS            OFF)

# The codec bindings, kernels and the registration shim are C and rely on
# compound literals in designated initializers, which is C11.
set(CMAKE_C_STANDARD                11)
set(CMAKE_C_STANDARD_REQUIRED       ON)
set(CMAKE_C_EXTENSIONS              OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide everything by default. The C++ surface is re-exported with RUMI_API and
# the extern "C" entry points with GDAL's CPL_DLL. Hiding C symbols too keeps
# the vendored OpenZL and its bundled zstd out of the plugin's dynamic symbol
# table, which matters because GDAL also links a zstd.
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_C_VISIBILITY_PRESET       hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

find_package(GDAL 3.12 CONFIG REQUIRED)

# OpenZL provides the tile codec (compression 60000). Vendored as a submodule
# and built static so it folds into rumi with no extra runtime dependency,
# carrying its own zstd and lz4. rumi links no separate compression library.
if(RUMI_USE_SYSTEM_OPENZL)
    find_package(openzl CONFIG REQUIRED)        # OpenZL::openzl
    set(RUMI_OPENZL_TARGET OpenZL::openzl)
else()
    set(OPENZL_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_BENCHMARKS     OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_CLI            OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_EXAMPLES       OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_TOOLS          OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_PYTHON_EXT     OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_CUSTOM_PARSERS OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_CPP            OFF CACHE BOOL "" FORCE)  # only the C decoder is needed
    set(OPENZL_ALLOW_INTROSPECTION  OFF CACHE BOOL "" FORCE)  # off for production
    set(OPENZL_INSTALL              OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)  # static even when rumi is shared
    add_subdirectory(${RUMI_OPENZL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/openzl EXCLUDE_FROM_ALL)
    set(RUMI_OPENZL_TARGET openzl)               # the OpenZL:: alias only exists on install
endif()

# Experimental rumi codecs. register_experimental.c wires the decoders into
# OpenZL, kernels.c re-exports the kernels under rumi_ names for cffi.
set(RUMI_SOURCES
    src/parser.cpp
    src/builder.cpp
    src/geokeys.cpp
    src/plan.cpp
    src/read.cpp
    src/pattern.cpp
    src/image.cpp
    src/capi.cpp

    src/register_experimental.c
    src/kernels.c
    src/delta_w/decode_delta_w_binding.c
    src/delta_w/decode_delta_w_kernel.c
    src/delta_w/encode_delta_w_kernel.c
    src/delta_n/decode_delta_n_binding.c
    src/delta_n/decode_delta_n_kernel.c
    src/delta_n/encode_delta_n_kernel.c
    src/planar/decode_planar_binding.c
    src/planar/decode_planar_kernel.c
    src/planar/encode_planar_kernel.c
)

# Compile and link settings shared by every target.
add_library(rumi_config INTERFACE)
target_include_directories(rumi_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# The registration shim, kernel wrapper and codec bindings include each other
# by folder relative path (e.g. "delta_w/graph_delta_w.h"), so src is on the
# include path.
target_include_directories(rumi_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(rumi_config INTERFACE
    GDAL::GDAL
    ${RUMI_OPENZL_TARGET}
)
target_compile_options(rumi_config INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->>
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
    target_compile_definitions(gdal_RUMI PRIVATE
        RUMI_BUILD
        RUMI_VERSION_STRING="${RUMI_VERSION_FULL}"
    )
    set_target_properties(gdal_RUMI PROPERTIES
        PREFIX      ""
        OUTPUT_NAME "gdal_RUMI"
    )

    install(TARGETS gdal_RUMI
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gdalplugins
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/gdalplugins
    )
endif()

# For apps that link directly and call rumi::register_driver() themselves
# instead of relying on plugin auto-discovery.
if(RUMI_BUILD_SHARED_LIB)
    add_library(rumi SHARED ${RUMI_SOURCES})
    add_library(rumi::rumi ALIAS rumi)
    target_link_libraries(rumi PUBLIC rumi_config)
    target_compile_definitions(rumi PRIVATE
        RUMI_BUILD
        RUMI_VERSION_STRING="${RUMI_VERSION_FULL}"
    )
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
message(STATUS "  OpenZL:         ${RUMI_OPENZL_TARGET}")
message(STATUS "  Plugin:         ${RUMI_BUILD_PLUGIN}")
message(STATUS "  Standalone lib: ${RUMI_BUILD_SHARED_LIB}")
message(STATUS "  Sanitize:       ${RUMI_SANITIZE}")
message(STATUS "")