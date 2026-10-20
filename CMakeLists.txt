cmake_minimum_required(VERSION 3.15)
project(ChessGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # -std=c++17, not gnu++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # clangd / clang-tidy support

# ---- Options ---------------------------------------------------------------
# -march=native tunes for the CPU that builds the binary; the result can
# SIGILL on a different CPU. Fine (and wanted) for this project's actual use
# case -- building and SPRT-testing on the same machine.
option(CHESS_NATIVE_ARCH "Build with -march=native (binary may SIGILL on other CPUs)" ON)
option(CHESS_WERROR      "Treat warnings as errors" ON)
option(CHESS_LTO         "Link-time optimisation in Release builds" ON)
option(CHESS_GUI         "Build the SDL2 GUI (OFF: a UCI/command-line engine that needs no SDL2)" ON)

# Distribution builds. Neither needs SDL2 installed on the target machine (with CHESS_GUI=OFF SDL2 is not used at all).
#   CHESS_BUNDLE_SDL2: download SDL2 and link it statically into the executable
#                      (needs network access at configure time).
#   CHESS_PORTABLE:    runs on any CPU of the baseline level below instead of
#                      this one, links the C++ runtime statically, and implies
#                      CHESS_BUNDLE_SDL2. Overrides CHESS_NATIVE_ARCH.
# Linux:   cmake -S . -B build-portable -DCHESS_PORTABLE=ON
# Windows: cross-compile from Linux with MinGW-w64:
#   cmake -S . -B build-windows -DCHESS_PORTABLE=ON \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
#   or with MSVC, from a Developer prompt:  cmake -S . -B build -DCHESS_PORTABLE=ON
option(CHESS_BUNDLE_SDL2 "Build SDL2 from source and link it statically" OFF)
option(CHESS_PORTABLE    "Self-contained executable for machines without SDL2" OFF)
set(CHESS_PORTABLE_ARCH "x86-64-v2" CACHE STRING
    "-march baseline for CHESS_PORTABLE on x86-64 (x86-64-v3 = AVX2/BMI2, 2013+ CPUs)")
if(CHESS_PORTABLE)
    set(CHESS_BUNDLE_SDL2 ON)
    # Static C runtime for MSVC (must be set before the targets are created).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- Default build type (single-config generators only) --------------------
# Only applies to a fresh, never-configured build directory -- an existing
# build dir's cached CMAKE_BUILD_TYPE (e.g. Debug, for ASan/TSan variants)
# always wins over this default.
get_property(CHESS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT CHESS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- LTO -------------------------------------------------------------------
# Inlines across source files (e.g. pieceValue() into the move-ordering code)
# when the toolchain supports it. Must be set before the targets are created.
if(CHESS_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CHESS_IPO_OK OUTPUT CHESS_IPO_MESSAGE)
    if(CHESS_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(STATUS "LTO unavailable: ${CHESS_IPO_MESSAGE}")
    endif()
endif()

# ---- Shared compile flags (applied per target, not globally) ---------------
add_library(chess_flags INTERFACE)
if(MSVC)
    target_compile_options(chess_flags INTERFACE /W4
        $<$<BOOL:${CHESS_WERROR}>:/WX>)
else()
    target_compile_options(chess_flags INTERFACE -Wall -Wextra
        $<$<BOOL:${CHESS_WERROR}>:-Werror>)
    if(CHESS_PORTABLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            target_compile_options(chess_flags INTERFACE -march=${CHESS_PORTABLE_ARCH})
        endif()
        # No libstdc++/libgcc .so to ship; MinGW also drops libwinpthread-1.dll.
        if(MINGW)
            target_link_options(chess_flags INTERFACE -static)
        else()
            target_link_options(chess_flags INTERFACE -static-libstdc++ -static-libgcc)
        endif()
    elseif(CHESS_NATIVE_ARCH)
        target_compile_options(chess_flags INTERFACE -march=native)
    endif()
endif()

if(NOT CHESS_GUI)
    target_compile_definitions(chess_flags INTERFACE CHESS_NO_GUI)
endif()

# `git describe` at configure time, shown by --version and in the UCI id.
set(CHESS_VERSION "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE CHESS_GIT_DESCRIBE
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE CHESS_GIT_RESULT)
    if(CHESS_GIT_RESULT EQUAL 0 AND CHESS_GIT_DESCRIBE)
        set(CHESS_VERSION "${CHESS_GIT_DESCRIBE}")
    endif()
endif()
target_compile_definitions(chess_flags INTERFACE CHESS_VERSION="${CHESS_VERSION}")

function(chess_configure tgt)
    target_include_directories(${tgt} PRIVATE include)
    target_link_libraries(${tgt} PRIVATE chess_flags)
endfunction()

# ---- Sources ---------------------------------------------------------------
if(NOT CHESS_GUI)
    message(STATUS "CHESS_GUI=OFF: building without the SDL2 GUI")
elseif(CHESS_BUNDLE_SDL2)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    # The GUI only calls SDL_Init(SDL_INIT_VIDEO); skipping the rest also avoids
    # SDL 2.30's PipeWire backend, which does not compile against current headers.
    foreach(sub AUDIO JOYSTICK HAPTIC SENSOR)
        set(SDL_${sub} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(SDL2
        URL      https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
        URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4)
    FetchContent_MakeAvailable(SDL2)
    set(CHESS_SDL2_TARGET SDL2::SDL2-static)
else()
    find_package(SDL2 REQUIRED)
    set(CHESS_SDL2_TARGET SDL2::SDL2)
endif()
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS "src/*.cpp")
if(NOT CHESS_GUI)
    list(FILTER SRC_FILES EXCLUDE REGEX ".*/gui\\.cpp$")
endif()

# ---- Main engine -----------------------------------------------------------
add_executable(chess ${SRC_FILES})
chess_configure(chess)
if(CHESS_GUI)
    target_link_libraries(chess PRIVATE ${CHESS_SDL2_TARGET})
endif()

# ---- SPSA search-constant tuning build (tools/spsa.py) ---------------------
# The same engine with every SEARCH_PARAM exposed as a UCI spin option.
# cmake --build <dir> --target chess_spsa
add_executable(chess_spsa EXCLUDE_FROM_ALL ${SRC_FILES})
chess_configure(chess_spsa)
target_compile_definitions(chess_spsa PRIVATE SPSA)
if(CHESS_GUI)
    target_link_libraries(chess_spsa PRIVATE ${CHESS_SDL2_TARGET})
endif()

# ---- Texel tuner (tools/tune.cpp) ------------------------------------------
# Not part of the default build: cmake --build <dir> --target chess_tune.
# Compiled with -DTUNING so every TUNE_ weight is a registered, mutable variable.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    set(TUNE_SRC ${SRC_FILES})
    list(FILTER TUNE_SRC EXCLUDE REGEX ".*/(main|gui)\\.cpp$")
    add_executable(chess_tune EXCLUDE_FROM_ALL ${TUNE_SRC} tools/tune.cpp)
    chess_configure(chess_tune)
    target_compile_definitions(chess_tune PRIVATE TUNING)
    target_link_libraries(chess_tune PRIVATE OpenMP::OpenMP_CXX)
endif()

# ---- Correctness gates as tests (ctest) -----------------------------------
# CLAUDE.md rule 3 in one command:  ctest --test-dir <build-dir> --output-on-failure
# Perft against published node counts, the incremental-eval/hash self-check, the bench signature
# (tools/bench_signature.txt) and the UCI robustness cases (tools/uci_conformance.py, needs python3).
option(CHESS_BUILD_TESTS "Register the correctness gates with CTest" ON)
if(CHESS_BUILD_TESTS)
    enable_testing()
    add_test(NAME perft COMMAND chess --perft-suite 5)
    add_test(NAME verify_eval COMMAND chess --verify-eval 4)
    set_tests_properties(perft verify_eval PROPERTIES PASS_REGULAR_EXPRESSION "ALL PASS" TIMEOUT 900)
    add_test(NAME bench_signature
             COMMAND ${CMAKE_COMMAND} -DCHESS=$<TARGET_FILE:chess>
                     -DEXPECTED=${CMAKE_SOURCE_DIR}/tools/bench_signature.txt -P ${CMAKE_SOURCE_DIR}/tools/check_bench.cmake)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME uci_conformance
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/uci_conformance.py $<TARGET_FILE:chess>)
        set_tests_properties(uci_conformance PROPERTIES TIMEOUT 300)
    endif()
endif()
