cmake_minimum_required(VERSION 3.21)

project(geozl
    DESCRIPTION "Geospatial codec extensions for OpenZL"
    LANGUAGES   C
)

option(GEOZL_BUILD_FULL           "Build the library that links OpenZL, off builds only the kernels" ON)
option(GEOZL_BUILD_KERNELS_SHARED "Build the pure C kernels shared library for Python" ON)
option(GEOZL_USE_SYSTEM_OPENZL    "Link an installed OpenZL instead of the submodule" OFF)
option(GEOZL_SANITIZE             "Build with ASan and UBSan, for tests and fuzzing" OFF)
option(GEOZL_WERROR               "Turn geozl warnings into errors, for CI" OFF)
option(GEOZL_BUILD_FUZZERS        "Build the libFuzzer harnesses, needs clang and FULL" OFF)
option(GEOZL_BUILD_WASM           "Build the wasm64 JavaScript module" OFF)
option(GEOZL_BUILD_GPU            "Build the internal Metal or CUDA runtime" OFF)
set(GEOZL_OPENZL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/openzl"
    CACHE PATH "Path to the OpenZL submodule source")

if(GEOZL_BUILD_WASM AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "GEOZL_BUILD_WASM requires an Emscripten toolchain")
endif()
if(GEOZL_BUILD_WASM AND NOT GEOZL_BUILD_FULL)
    message(FATAL_ERROR "GEOZL_BUILD_WASM requires GEOZL_BUILD_FULL=ON")
endif()
if(GEOZL_BUILD_GPU AND NOT GEOZL_BUILD_FULL)
    message(FATAL_ERROR "GEOZL_BUILD_GPU requires GEOZL_BUILD_FULL=ON")
endif()

# Freeze geozl's linkage before OpenZL touches BUILD_SHARED_LIBS. A standalone
# build is shared; a parent that explicitly sets OFF gets a static library.
# OpenZL may restore an initially undefined value as an empty cache entry, so
# empty still means the standalone default. Use a fresh build tree to change it.
if(GEOZL_BUILD_WASM)
    set(GEOZL_LIB_TYPE STATIC CACHE INTERNAL "geozl library type" FORCE)
elseif(NOT DEFINED GEOZL_LIB_TYPE)
    if(DEFINED BUILD_SHARED_LIBS
       AND NOT BUILD_SHARED_LIBS STREQUAL ""
       AND NOT BUILD_SHARED_LIBS)
        set(GEOZL_LIB_TYPE STATIC CACHE INTERNAL "geozl library type")
    else()
        set(GEOZL_LIB_TYPE SHARED CACHE INTERNAL "geozl library type")
    endif()
endif()
message(STATUS "geozl linkage: ${GEOZL_LIB_TYPE}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD                11)
set(CMAKE_C_STANDARD_REQUIRED       ON)
set(CMAKE_C_EXTENSIONS              OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)

# Hide everything but GEOZL_API so vendored OpenZL stays off the symbol table.
set(CMAKE_C_VISIBILITY_PRESET       hidden)
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# OpenZL requires 64-bit pointers. Its flags are scoped to its subdirectory,
# so apply the same ABI to GeoZL and the final module here.
if(EMSCRIPTEN)
    add_compile_options(-sMEMORY64=1)
    add_link_options(-sMEMORY64=1 -sWASM_BIGINT=1)
endif()

if(GEOZL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Coverage instrumentation has to reach the library and OpenZL, not just the
# harness, or libFuzzer has no signal to steer on and mutates blind. Set here,
# before add_subdirectory(openzl), because the decoders are only reachable once
# OpenZL has parsed the frame around them. The harness itself links
# -fsanitize=fuzzer below, which is what brings in libFuzzer's main.
if(GEOZL_BUILD_FUZZERS)
    add_compile_options(-fsanitize=fuzzer-no-link)
    add_link_options(-fsanitize=fuzzer-no-link)
endif()

# A frame is written on one machine and read on another, so the decoder has to
# reconstruct the same bits everywhere. Contraction breaks that: a compiler with
# FMA available folds w*w - offset into one instruction that rounds once instead
# of twice, and whether it does depends on the target and the compiler version.
# On arm64 it always does. Fallin and Burtscher hit the same thing between CPU
# and GPU builds of LC and reached for the same switch (arXiv:2407.15037).
# clang-cl ignores -ffp-contract=off with a warning and keeps contracting, so it
# gets the flag through /clang:, and /W4 because its -Wall means -Weverything.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(GEOZL_FP /clang:-ffp-contract=off)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(GEOZL_FP -ffp-contract=off)
endif()

# Applied to the geozl targets only, so vendored OpenZL keeps its own warnings.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(GEOZL_WARN /W4)
    if(GEOZL_WERROR)
        list(APPEND GEOZL_WARN /WX)
    endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(GEOZL_WARN -Wall -Wextra)
    if(GEOZL_WERROR)
        list(APPEND GEOZL_WARN -Werror)
    endif()
endif()

include(GNUInstallDirs)
find_library(GEOZL_LIBM m)   # the wp_static trainer and the quantizers use libm

# clang-cl turns __builtin_cpu_supports into a call into compiler-rt, which the
# MSVC linker does not bring in on its own.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} /clang:--rtlib=compiler-rt /clang:-print-libgcc-file-name
        OUTPUT_VARIABLE GEOZL_CLANG_RT OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(TO_CMAKE_PATH "${GEOZL_CLANG_RT}" GEOZL_CLANG_RT)
    if(NOT EXISTS "${GEOZL_CLANG_RT}")
        message(FATAL_ERROR "clang-cl has no compiler-rt builtins at ${GEOZL_CLANG_RT}")
    endif()
endif()

# Discover encoder and decoder sides independently. Retired writers keep their
# decoders so frames already on disk remain readable.
file(GLOB GEOZL_ENCODER_BINDINGS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*/encode_*_binding.c)
foreach(binding IN LISTS GEOZL_ENCODER_BINDINGS)
    get_filename_component(dir ${binding} DIRECTORY)
    get_filename_component(name ${dir} NAME)
    list(APPEND GEOZL_BINDINGS src/${name}/encode_${name}_binding.c)
    list(APPEND GEOZL_KERNELS src/${name}/encode_${name}_kernel.c)
endforeach()

file(GLOB GEOZL_DECODER_BINDINGS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*/decode_*_binding.c)
foreach(binding IN LISTS GEOZL_DECODER_BINDINGS)
    get_filename_component(dir ${binding} DIRECTORY)
    get_filename_component(name ${dir} NAME)
    list(APPEND GEOZL_BINDINGS src/${name}/decode_${name}_binding.c)
    list(APPEND GEOZL_KERNELS src/${name}/decode_${name}_kernel.c)
endforeach()
list(APPEND GEOZL_BINDINGS src/pivco_lanes/pivco_lanes.c)
# The rest of the kernels library, which the glob above cannot find because none
# of it is a codec. It is here so a Python caller reaches it without OpenZL: the
# coefficient blob's serializer, the wp_static trainer, the recipe parsers that
# turn an error string into a bound, the noise fit the sqrt quantizer needs, and
# the report of which vector paths this build carries.
list(APPEND GEOZL_KERNELS
    src/coeffs/coeffs.c
    src/wp_static/train_wp_static.c
    src/lossy/lossy_recipe.c
    src/quant_linear/quant_linear_spec.c
    src/quant_log/quant_log_spec.c
    src/quant_sqrt/quant_sqrt_spec.c
    src/quant_sqrt/quant_sqrt_fit.c
    src/common/simd.c)

if(GEOZL_BUILD_FULL)
    if(GEOZL_USE_SYSTEM_OPENZL)
        find_package(openzl CONFIG REQUIRED)
        set(GEOZL_OPENZL_TARGET OpenZL::openzl)
    else()
        enable_language(CXX)
        set(CMAKE_CXX_STANDARD          17)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
        foreach(off TESTS BENCHMARKS CLI EXAMPLES TOOLS PYTHON_EXT CUSTOM_PARSERS CPP)
            set(OPENZL_BUILD_${off} OFF CACHE BOOL "" FORCE)
        endforeach()
        set(OPENZL_ALLOW_INTROSPECTION OFF CACHE BOOL "" FORCE)
        set(OPENZL_INSTALL             OFF CACHE BOOL "" FORCE)
        set(OPENZL_BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
        add_subdirectory(${GEOZL_OPENZL_DIR}
                         ${CMAKE_CURRENT_BINARY_DIR}/openzl EXCLUDE_FROM_ALL)
        set(GEOZL_OPENZL_TARGET openzl)
    endif()

    add_library(geozl ${GEOZL_LIB_TYPE}
        ${GEOZL_BINDINGS}
        ${GEOZL_KERNELS}
        src/2d/2d.c
        src/encoder_registry.c
        src/decoder_registry.c)
    add_library(geozl::geozl ALIAS geozl)

    # OpenZL is PUBLIC, a C consumer runs the graph itself. src is private for the
    # relative includes between folders.
    target_include_directories(geozl
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(geozl PUBLIC ${GEOZL_OPENZL_TARGET})
    # planar_zigzag_transpose_pivco calls PivCo's kernels, which OpenZL does not install.
    if(GEOZL_USE_SYSTEM_OPENZL)
        target_include_directories(geozl PRIVATE ${GEOZL_OPENZL_DIR}/src)
    endif()
    target_compile_definitions(geozl PRIVATE GEOZL_BUILD)
    target_compile_options(geozl PRIVATE ${GEOZL_WARN} ${GEOZL_FP})
    if(GEOZL_BUILD_GPU)
        find_package(Threads REQUIRED)
        target_sources(geozl PRIVATE
            src/gpu/frame_batch.c
            src/gpu/pfor_capture.c
            src/gpu/pfor_decode.c
            src/gpu/pfor_plan.c
            src/gpu/pfor_plan_parallel.c
            src/gpu/pivco_capture.c
            src/gpu/pivco_decode.c
            src/gpu/pivco_plan.c
            src/gpu/pivco_plan_parallel.c
            src/gpu/planar_scan.c
            src/gpu/runtime.c
            src/gpu/staging_arena.c
            src/gpu/decode.c)
        target_link_libraries(geozl PRIVATE Threads::Threads)
        # The kernels ship inside the library, PTX on CUDA and Metal source on
        # Metal, so neither building nor running needs Slang.
        if(APPLE)
            set(GEOZL_GPU_MODULE_KIND metal)
        else()
            set(GEOZL_GPU_MODULE_KIND ptx)
        endif()
        set(GEOZL_GPU_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/modules)
        set(GEOZL_GPU_MODULES_C ${CMAKE_CURRENT_BINARY_DIR}/gpu_modules.c)
        file(GLOB GEOZL_GPU_MODULE_FILES CONFIGURE_DEPENDS
             ${GEOZL_GPU_MODULE_DIR}/*.${GEOZL_GPU_MODULE_KIND})
        add_custom_command(
            OUTPUT ${GEOZL_GPU_MODULES_C}
            COMMAND ${CMAKE_COMMAND} -DMODULE_DIR=${GEOZL_GPU_MODULE_DIR}
                    -DEXTENSION=${GEOZL_GPU_MODULE_KIND}
                    -DOUTPUT=${GEOZL_GPU_MODULES_C}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/embed_modules.cmake
            DEPENDS ${GEOZL_GPU_MODULE_FILES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/embed_modules.cmake
            VERBATIM)
        target_sources(geozl PRIVATE ${GEOZL_GPU_MODULES_C})
        if(APPLE)
            enable_language(OBJC)
            target_sources(geozl PRIVATE src/gpu/runtime_metal.m)
            target_compile_definitions(geozl PRIVATE GEOZL_GPU_METAL)
            target_compile_options(geozl PRIVATE
                $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
                $<$<COMPILE_LANGUAGE:OBJC>:-fvisibility=hidden>)
            target_link_libraries(geozl PRIVATE
                "-framework Metal" "-framework Foundation")
        elseif(UNIX)
            target_sources(geozl PRIVATE src/gpu/runtime_cuda.c)
            target_compile_definitions(geozl PRIVATE GEOZL_GPU_CUDA)
            target_link_libraries(geozl PRIVATE ${CMAKE_DL_LIBS})
        else()
            message(FATAL_ERROR
                "GEOZL_BUILD_GPU is not supported on this platform")
        endif()
    else()
        target_sources(geozl PRIVATE src/gpu/unavailable.c)
    endif()
    if(GEOZL_LIBM)
        target_link_libraries(geozl PRIVATE ${GEOZL_LIBM})
    endif()
    if(GEOZL_CLANG_RT)
        target_link_libraries(geozl PRIVATE ${GEOZL_CLANG_RT})
    endif()

    if(GEOZL_BUILD_WASM)
        add_executable(geozl_wasm_module
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/geozl_wasm.c)
        target_link_libraries(geozl_wasm_module PRIVATE geozl)
        target_compile_options(geozl_wasm_module PRIVATE -O3)
        target_link_options(geozl_wasm_module PRIVATE
            -O3
            --no-entry
            -sWASM=1
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=32MB
            -sMAXIMUM_MEMORY=16GB
            -sSTACK_SIZE=4MB
            -sMODULARIZE=1
            -sEXPORT_ES6=1
            -sEXPORT_NAME=geozlModule
            -sEXPORTED_FUNCTIONS=
            -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU64,UTF8ToString
            -sENVIRONMENT=web,worker,node)
        set_target_properties(geozl_wasm_module PROPERTIES
            OUTPUT_NAME "geozl"
            SUFFIX ".js"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wasm")
        # The output directory is the npm package, so it also carries the
        # README and the licences of what the module links.
        add_custom_target(geozl_wasm ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/js
                    $<TARGET_FILE_DIR:geozl_wasm_module>
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/README.md
                    ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
                    ${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE
                    $<TARGET_FILE_DIR:geozl_wasm_module>
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/../licenses
                    $<TARGET_FILE_DIR:geozl_wasm_module>/licenses
            DEPENDS geozl_wasm_module)

        install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/wasm/geozl.js
            ${CMAKE_CURRENT_BINARY_DIR}/wasm/geozl.wasm
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/js/geozl_api.js
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/js/geozl_api.d.ts
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/wasm/js/package.json
            DESTINATION wasm
            OPTIONAL)
    endif()

    install(TARGETS geozl ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/geozl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # libFuzzer harness over the decode path. Needs clang, and GEOZL_SANITIZE=ON
    # so ASan and UBSan see the decoders. The instrumentation itself is set
    # globally above, this only pulls in libFuzzer's main.
    if(GEOZL_BUILD_FUZZERS)
        foreach(fz decode binding)
            add_executable(geozl_${fz}_fuzzer
                ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/${fz}_fuzzer.c)
            target_link_libraries(geozl_${fz}_fuzzer PRIVATE geozl)
            target_link_options(geozl_${fz}_fuzzer PRIVATE -fsanitize=fuzzer)
        endforeach()

        # A mutator hardly ever produces a frame that still parses, so the
        # decode fuzzer spends its budget in the OpenZL frame reader and rarely
        # reaches a quantizer at all. These drive the codecs directly and assert
        # the bound on the way back out. Kernels only, no OpenZL.
        foreach(fz roundtrip lossy_recipe quant_linear quant_log quant_sqrt pfor coeffs)
            add_executable(geozl_${fz}_fuzzer
                ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/${fz}_fuzzer.c
                ${GEOZL_KERNELS})
            target_include_directories(geozl_${fz}_fuzzer PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
            # Without GEOZL_FP the kernels here contract where the library does
            # not, and the fuzzer asserts a bound about arithmetic nobody ships.
            target_compile_options(geozl_${fz}_fuzzer PRIVATE ${GEOZL_FP})
            target_link_options(geozl_${fz}_fuzzer PRIVATE -fsanitize=fuzzer)
            if(GEOZL_LIBM)
                target_link_libraries(geozl_${fz}_fuzzer PRIVATE ${GEOZL_LIBM})
            endif()
        endforeach()
    endif()
endif()

if(GEOZL_BUILD_KERNELS_SHARED)
    add_library(geozl_kernels SHARED ${GEOZL_KERNELS})
    # The kernel headers carry no export attribute, so a Windows DLL needs every
    # symbol exported to match what default visibility gives elsewhere.
    set_target_properties(geozl_kernels PROPERTIES C_VISIBILITY_PRESET default
                          WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(geozl_kernels PRIVATE GEOZL_BUILD)
    # include/ too: the kernels take their parameter blocks from geozl/, which is
    # public because the node builders over them are.
    target_include_directories(geozl_kernels PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(geozl_kernels PRIVATE ${GEOZL_WARN} ${GEOZL_FP})
    if(GEOZL_LIBM)
        target_link_libraries(geozl_kernels PRIVATE ${GEOZL_LIBM})
    endif()
    if(GEOZL_CLANG_RT)
        target_link_libraries(geozl_kernels PRIVATE ${GEOZL_CLANG_RT})
    endif()
    install(TARGETS geozl_kernels LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
