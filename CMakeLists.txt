cmake_minimum_required(VERSION 3.20)
project(ubersdr-dsp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── Optional feature flags ────────────────────────────────────────────────────
option(WITH_FFTW3      "Use FFTW3 for NR2 FFT (recommended)"        ON)
option(WITH_SPECBLEACH "Build NR4 (libspecbleach)"                  ON)
option(WITH_DFNR       "Build DFNR (DeepFilterNet3)"                OFF)
option(WITH_GRPC_SERVER "Build gRPC server (replaces stdin/stdout)" ON)

# Path to the third_party directory (defaults to local third_party/)
set(AETHER_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    CACHE PATH "Path to third_party directory containing r8brain, rnnoise, libspecbleach, deepfilter")

# ── Proto / gRPC code generation ──────────────────────────────────────────────
if(WITH_GRPC_SERVER)
    find_package(Protobuf REQUIRED)
    find_package(gRPC QUIET)

    if(NOT gRPC_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    endif()

    # Locate protoc and the gRPC C++ plugin
    if(gRPC_FOUND)
        get_target_property(GRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin LOCATION)
    else()
        find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
    endif()

    set(PROTO_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    set(PROTO_FILE  "${PROTO_DIR}/ubersdr_dsp.proto")
    set(PROTO_OUT   "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
    file(MAKE_DIRECTORY "${PROTO_OUT}")

    # Generated file paths
    set(PROTO_SRCS
        "${PROTO_OUT}/ubersdr_dsp.pb.cc"
        "${PROTO_OUT}/ubersdr_dsp.grpc.pb.cc"
    )
    set(PROTO_HDRS
        "${PROTO_OUT}/ubersdr_dsp.pb.h"
        "${PROTO_OUT}/ubersdr_dsp.grpc.pb.h"
    )

    add_custom_command(
        OUTPUT  ${PROTO_SRCS} ${PROTO_HDRS}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
                --grpc_out=${PROTO_OUT}
                --cpp_out=${PROTO_OUT}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
                -I ${PROTO_DIR}
                ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating protobuf/gRPC stubs from ubersdr_dsp.proto"
        VERBATIM
    )

    add_custom_target(proto_gen DEPENDS ${PROTO_SRCS} ${PROTO_HDRS})
endif()

# ── Core filter sources (always built) ────────────────────────────────────────
set(FILTER_SOURCES
    SpectralNR.cpp
    RNNoiseFilter.cpp
    Resampler.cpp
)

# ── r8brain (header-only) ─────────────────────────────────────────────────────
set(R8BRAIN_DIR "${AETHER_THIRD_PARTY}/r8brain")
if(NOT EXISTS "${R8BRAIN_DIR}/CDSPResampler.h")
    message(FATAL_ERROR "r8brain not found at ${R8BRAIN_DIR}. "
            "Set AETHER_THIRD_PARTY to the AetherSDR third_party directory.")
endif()

# ── RNNoise sources ───────────────────────────────────────────────────────────
set(RNNOISE_DIR "${AETHER_THIRD_PARTY}/rnnoise")
if(NOT EXISTS "${RNNOISE_DIR}/include/rnnoise.h")
    message(FATAL_ERROR "rnnoise not found at ${RNNOISE_DIR}")
endif()

set(RNNOISE_SOURCES
    ${RNNOISE_DIR}/src/celt_lpc.c
    ${RNNOISE_DIR}/src/denoise.c
    ${RNNOISE_DIR}/src/kiss_fft.c
    ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c
    ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/rnn.c
    ${RNNOISE_DIR}/src/rnnoise_data.c
    ${RNNOISE_DIR}/src/rnnoise_tables.c
)

# x86 RTCD sources are only meaningful on x86; including them on ARM64 pulls in
# x86 CPU-detection code that has no ARM equivalent and may fail to compile or link.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    list(APPEND RNNOISE_SOURCES
        ${RNNOISE_DIR}/src/x86/x86cpu.c
        ${RNNOISE_DIR}/src/x86/x86_dnn_map.c
    )
endif()

set(RNNOISE_AVX2_SOURCES  ${RNNOISE_DIR}/src/x86/nnet_avx2.c)
set(RNNOISE_SSE41_SOURCES ${RNNOISE_DIR}/src/x86/nnet_sse4_1.c)

include(CheckCCompilerFlag)

# ── Architecture-specific optimisation flags for rnnoise ──────────────────────
# On ARM64 use a safe ARMv8-A baseline so that Docker images built on a powerful
# ARM64 host (Graviton3, Apple M2 CI) don't emit dot-product instructions
# (vdotq_s32 / FEAT_DotProd) that SIGILL on older targets (Cortex-A53, Pi 4,
# Graviton1/2).  NEON is mandatory in all ARMv8-A so vec_neon.h still activates
# and rnnoise uses the safe vpadalq_s16 fallback path.
# On x86 keep -march=native for maximum performance.
# (Same pattern as ka9q-radio/src/Makefile.)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set_source_files_properties(${RNNOISE_SOURCES}
        PROPERTIES COMPILE_FLAGS "-march=armv8-a")
else()
    check_c_compiler_flag(-mavx2   COMPILER_HAS_AVX2)
    check_c_compiler_flag(-msse4.1 COMPILER_HAS_SSE41)

    if(COMPILER_HAS_AVX2)
        list(APPEND RNNOISE_SOURCES ${RNNOISE_AVX2_SOURCES})
        set_source_files_properties(${RNNOISE_AVX2_SOURCES}
            PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
    endif()

    if(COMPILER_HAS_SSE41)
        list(APPEND RNNOISE_SOURCES ${RNNOISE_SSE41_SOURCES})
        set_source_files_properties(${RNNOISE_SSE41_SOURCES}
            PROPERTIES COMPILE_FLAGS "-msse4.1")
    endif()

    check_c_compiler_flag(-march=native COMPILER_HAS_MARCH_NATIVE)
    if(COMPILER_HAS_MARCH_NATIVE)
        set_source_files_properties(${RNNOISE_SOURCES}
            PROPERTIES COMPILE_FLAGS "-march=native")
        if(COMPILER_HAS_AVX2)
            set_source_files_properties(${RNNOISE_AVX2_SOURCES}
                PROPERTIES COMPILE_FLAGS "-march=native -mavx2 -mfma")
        endif()
        if(COMPILER_HAS_SSE41)
            set_source_files_properties(${RNNOISE_SSE41_SOURCES}
                PROPERTIES COMPILE_FLAGS "-march=native -msse4.1")
        endif()
    endif()
endif()

# ── FFTW3 ─────────────────────────────────────────────────────────────────────
if(WITH_FFTW3)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3 fftw3)
    endif()
    if(NOT FFTW3_FOUND)
        find_library(FFTW3_LIBRARIES NAMES fftw3)
        find_path(FFTW3_INCLUDE_DIRS NAMES fftw3.h)
        if(FFTW3_LIBRARIES AND FFTW3_INCLUDE_DIRS)
            set(FFTW3_FOUND TRUE)
        endif()
    endif()
    if(FFTW3_FOUND)
        message(STATUS "FFTW3 found: ${FFTW3_LIBRARIES}")
    else()
        message(WARNING "FFTW3 not found — NR2 will use built-in radix-2 FFT fallback")
        set(WITH_FFTW3 OFF)
    endif()
endif()

# ── libspecbleach (NR4) ───────────────────────────────────────────────────────
if(WITH_SPECBLEACH)
    set(SPECBLEACH_DIR "${AETHER_THIRD_PARTY}/libspecbleach")
    if(NOT EXISTS "${SPECBLEACH_DIR}/include/specbleach_denoiser.h")
        message(WARNING "libspecbleach not found at ${SPECBLEACH_DIR} — NR4 disabled")
        set(WITH_SPECBLEACH OFF)
    endif()
endif()

if(WITH_SPECBLEACH)
    file(GLOB_RECURSE SPECBLEACH_SOURCES
        "${AETHER_THIRD_PARTY}/libspecbleach/src/*.c"
    )
    list(APPEND FILTER_SOURCES SpecbleachFilter.cpp ${SPECBLEACH_SOURCES})
endif()

# ── DeepFilterNet3 (DFNR) ────────────────────────────────────────────────────
if(WITH_DFNR)
    set(DEEPFILTER_DIR "${AETHER_THIRD_PARTY}/deepfilter")
    if(NOT EXISTS "${DEEPFILTER_DIR}/include/deep_filter.h")
        message(WARNING "DeepFilter not found at ${DEEPFILTER_DIR} — DFNR disabled")
        set(WITH_DFNR OFF)
    else()
        find_library(DEEPFILTER_LIB
            NAMES deepfilter deep_filter df deep_filter_lib
            PATHS "${DEEPFILTER_DIR}/lib"
                  "${DEEPFILTER_DIR}/lib/linux-x86_64"
                  "${DEEPFILTER_DIR}/lib/linux-aarch64"
            NO_DEFAULT_PATH
        )
        if(NOT DEEPFILTER_LIB)
            message(WARNING "DeepFilter shared library not found in ${DEEPFILTER_DIR}/lib — DFNR disabled")
            set(WITH_DFNR OFF)
        else()
            list(APPEND FILTER_SOURCES DeepFilterFilter.cpp DeepFilterPool.cpp)
        endif()
    endif()
endif()

# ── gRPC server sources ───────────────────────────────────────────────────────
if(WITH_GRPC_SERVER)
    set(SERVER_SOURCES
        src/server.cpp
        src/DspServiceImpl.cpp
        src/FilterFactory.cpp
        src/filters/Nr2FilterWrapper.cpp
        src/filters/Rn2FilterWrapper.cpp
        src/filters/Nr4FilterWrapper.cpp
        src/filters/DfnrFilterWrapper.cpp
        ${PROTO_SRCS}
    )
    set(ENTRY_SOURCES ${SERVER_SOURCES})
else()
    # Legacy stdin/stdout mode
    set(ENTRY_SOURCES main.cpp)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(ubersdr-dsp
    ${ENTRY_SOURCES}
    ${FILTER_SOURCES}
    ${RNNOISE_SOURCES}
)

if(WITH_GRPC_SERVER)
    add_dependencies(ubersdr-dsp proto_gen)
endif()

# ── Include paths ─────────────────────────────────────────────────────────────
target_include_directories(ubersdr-dsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # SpectralNR.h, RNNoiseFilter.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/src      # DspServiceImpl.h, FilterFactory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/filters  # IFilter.h, *Wrapper.h
    ${R8BRAIN_DIR}
    ${RNNOISE_DIR}/include
    ${RNNOISE_DIR}/src
)

if(WITH_GRPC_SERVER)
    target_include_directories(ubersdr-dsp PRIVATE
        ${PROTO_OUT}                     # generated ubersdr_dsp.pb.h etc.
        ${Protobuf_INCLUDE_DIRS}
    )
    if(GRPC_INCLUDE_DIRS)
        target_include_directories(ubersdr-dsp PRIVATE ${GRPC_INCLUDE_DIRS})
    endif()
endif()

# ── Compile definitions ───────────────────────────────────────────────────────
if(WITH_FFTW3)
    target_compile_definitions(ubersdr-dsp PRIVATE HAVE_FFTW3)
    target_include_directories(ubersdr-dsp PRIVATE ${FFTW3_INCLUDE_DIRS})
    target_link_libraries(ubersdr-dsp PRIVATE ${FFTW3_LIBRARIES})
endif()

if(WITH_SPECBLEACH)
    target_compile_definitions(ubersdr-dsp PRIVATE HAVE_SPECBLEACH)
    target_include_directories(ubersdr-dsp PRIVATE
        "${AETHER_THIRD_PARTY}/libspecbleach/include"
        "${AETHER_THIRD_PARTY}/libspecbleach/src"
        "${AETHER_THIRD_PARTY}/libspecbleach/src/shared"
    )
    find_library(FFTW3F_LIB NAMES fftw3f)
    if(FFTW3F_LIB)
        target_link_libraries(ubersdr-dsp PRIVATE ${FFTW3F_LIB})
    else()
        message(WARNING "fftw3f (single-precision) not found — NR4 may fail to link.")
    endif()
endif()

if(WITH_DFNR)
    target_compile_definitions(ubersdr-dsp PRIVATE HAVE_DFNR)
    target_include_directories(ubersdr-dsp PRIVATE "${AETHER_THIRD_PARTY}/deepfilter/include")
    target_link_libraries(ubersdr-dsp PRIVATE ${DEEPFILTER_LIB})
endif()

if(WITH_GRPC_SERVER)
    target_compile_definitions(ubersdr-dsp PRIVATE HAVE_GRPC_SERVER)
    if(gRPC_FOUND)
        target_link_libraries(ubersdr-dsp PRIVATE
            gRPC::grpc++
            gRPC::grpc
            gRPC::grpc++_reflection
        )
    else()
        target_link_libraries(ubersdr-dsp PRIVATE ${GRPC_LIBRARIES})
    endif()
    target_link_libraries(ubersdr-dsp PRIVATE ${Protobuf_LIBRARIES})
endif()

# ── Math / threading ──────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(ubersdr-dsp PRIVATE Threads::Threads m)

# ── Compiler flags ────────────────────────────────────────────────────────────
target_compile_options(ubersdr-dsp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-O2 -Wall -Wextra -Wno-unused-parameter>
)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "ubersdr-dsp build configuration:")
if(WITH_GRPC_SERVER)
    message(STATUS "  Mode:  gRPC server (WITH_GRPC_SERVER=ON)")
else()
    message(STATUS "  Mode:  stdin/stdout (WITH_GRPC_SERVER=OFF)")
endif()
message(STATUS "  NR2  (SpectralNR)      always on")
message(STATUS "  RN2  (RNNoise)         always on")
message(STATUS "  NR4  (libspecbleach)   ${WITH_SPECBLEACH}")
message(STATUS "  DFNR (DeepFilterNet3)  ${WITH_DFNR}")
message(STATUS "  FFTW3                  ${WITH_FFTW3}")
message(STATUS "")
message(STATUS "  r8brain:               ${R8BRAIN_DIR}")
message(STATUS "  rnnoise:               ${RNNOISE_DIR}")
message(STATUS "")
