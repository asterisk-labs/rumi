cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" RUMI_VERSION_FULL)
string(STRIP "${RUMI_VERSION_FULL}" RUMI_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" RUMI_VERSION_NUMERIC
    "${RUMI_VERSION_FULL}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/rumi/rumi.h"
    RUMI_API_VERSION_LINE REGEX "^#define RUMI_API_VERSION [0-9]+$")
if(NOT RUMI_API_VERSION_LINE)
    message(FATAL_ERROR "rumi.h has no numeric RUMI_API_VERSION")
endif()
string(REGEX REPLACE ".*RUMI_API_VERSION ([0-9]+).*" "\\1"
    RUMI_API_VERSION "${RUMI_API_VERSION_LINE}")

project(rumi
    VERSION     ${RUMI_VERSION_NUMERIC}
    DESCRIPTION "A GeoTIFF-inspired raster format for AI training data"
    LANGUAGES   CXX C
)

option(RUMI_BUILD_SHARED_LIB  "Build standalone librumi for direct link"          OFF)
option(RUMI_BUILD_TESTS       "Build the C++ component tests"                     OFF)
option(RUMI_BUILD_FUZZERS     "Build libFuzzer harnesses (requires Clang)"         OFF)
option(RUMI_WERROR            "Treat warnings in rumi C++ as errors"              OFF)

# geozl supplies OpenZL transitively from the repository root.
set(RUMI_GEOZL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/geozl"
    CACHE PATH "Path to the geozl submodule source")

# Examples: -DRUMI_SANITIZE=thread or -DRUMI_SANITIZE=address,undefined.
set(RUMI_SANITIZE "" CACHE STRING "Sanitizer to compile and link with")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD              23)
set(CMAKE_CXX_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_EXTENSIONS            OFF)

# geozl and OpenZL require C; rumi itself is C++.
set(CMAKE_C_STANDARD                11)
set(CMAKE_C_STANDARD_REQUIRED       ON)
set(CMAKE_C_EXTENSIONS              OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile_commands.json for editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Export only RUMI_API symbols. Vendored codec symbols remain private.
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_C_VISIBILITY_PRESET       hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

# Link geozl and OpenZL statically into rumi without exporting their APIs.
set(BUILD_SHARED_LIBS OFF)
set(GEOZL_BUILD_KERNELS_SHARED OFF CACHE BOOL "" FORCE)   # the cffi kernels lib is a Python concern
add_subdirectory(${RUMI_GEOZL_DIR}/core ${CMAKE_CURRENT_BINARY_DIR}/geozl EXCLUDE_FROM_ALL)
target_compile_definitions(geozl PUBLIC GEOZL_STATIC)

set(RUMI_SOURCES
    src/parser.cpp
    src/builder.cpp
    src/geokeys.cpp
    src/write.cpp
    src/plan.cpp
    src/read.cpp
    src/pattern.cpp
    src/time.cpp
    src/capi.cpp
    src/dlpack.cpp
)

# Internal compile and link settings shared by rumi targets.
add_library(rumi_config INTERFACE)
target_include_directories(rumi_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rumi_config INTERFACE
    geozl::geozl
)
target_compile_definitions(rumi_config INTERFACE
    RUMI_BUILD
    RUMI_VERSION_STRING="${RUMI_VERSION_FULL}"
)
# Warning flags for rumi sources only.
target_compile_options(rumi_config INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /permissive->
)
if(RUMI_WERROR)
    target_compile_options(rumi_config INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>
    )
endif()

# Sanitizers must be enabled for both compilation and linking.
if(RUMI_SANITIZE)
    target_compile_options(rumi_config INTERFACE
        -fsanitize=${RUMI_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(rumi_config INTERFACE
        -fsanitize=${RUMI_SANITIZE})
endif()

if(RUMI_BUILD_SHARED_LIB)
    add_library(rumi SHARED ${RUMI_SOURCES})
    add_library(rumi::rumi ALIAS rumi)
    target_link_libraries(rumi PUBLIC rumi_config)
    set_target_properties(rumi PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${RUMI_API_VERSION}
    )

    install(TARGETS rumi
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    # Install the C ABI and its generated dtype and DLPack dependencies.
    install(FILES
        include/rumi/rumi.h
        include/rumi/rumi_dtypes.def
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rumi
    )
    install(FILES include/dlpack/dlpack.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dlpack
    )
endif()

# Component tests compile internal sources directly.
if(RUMI_BUILD_TESTS)
    enable_testing()
    add_executable(rumi_tests tests/test_core.cpp ${RUMI_SOURCES})
    target_link_libraries(rumi_tests PRIVATE rumi_config)
    if(RUMI_SANITIZE STREQUAL "thread")
        # TSAN cannot run the post-fork thread-creation test.
        target_compile_definitions(rumi_tests PRIVATE RUMI_TSAN)
    endif()
    add_test(NAME core COMMAND rumi_tests)
endif()

if(RUMI_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RUMI_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()
    foreach(surface IN ITEMS header index pattern read)
        add_executable(rumi_${surface}_fuzzer
            ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/${surface}_fuzzer.cpp
            ${RUMI_SOURCES})
        target_link_libraries(rumi_${surface}_fuzzer PRIVATE rumi_config)
        target_compile_options(rumi_${surface}_fuzzer PRIVATE
            -fsanitize=fuzzer-no-link)
        target_link_options(rumi_${surface}_fuzzer PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

message(STATUS "")
message(STATUS "rumi ${RUMI_VERSION_FULL}")
message(STATUS "  C API version:   ${RUMI_API_VERSION}")
message(STATUS "  geozl:          ${RUMI_GEOZL_DIR}")
message(STATUS "  OpenZL:         via geozl")
message(STATUS "  Standalone lib: ${RUMI_BUILD_SHARED_LIB}")
message(STATUS "  Tests:          ${RUMI_BUILD_TESTS}")
message(STATUS "  Fuzzers:        ${RUMI_BUILD_FUZZERS}")
message(STATUS "  Warnings error: ${RUMI_WERROR}")
message(STATUS "  Sanitize:       ${RUMI_SANITIZE}")
message(STATUS "")
