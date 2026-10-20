cmake_minimum_required(VERSION 3.12)
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
    if(CHESS_NATIVE_ARCH)
        target_compile_options(chess_flags INTERFACE -march=native)
    endif()
endif()

function(chess_configure tgt)
    target_include_directories(${tgt} PRIVATE include)
    target_link_libraries(${tgt} PRIVATE chess_flags)
endfunction()

# ---- Sources ---------------------------------------------------------------
find_package(SDL2 REQUIRED)
set(CHESS_SDL2_TARGET SDL2::SDL2)
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS "src/*.cpp")

# ---- Main engine -----------------------------------------------------------
add_executable(chess ${SRC_FILES})
chess_configure(chess)
target_link_libraries(chess PRIVATE ${CHESS_SDL2_TARGET})

# ---- SPSA search-constant tuning build (tools/spsa.py) ---------------------
# The same engine with every SEARCH_PARAM exposed as a UCI spin option.
# cmake --build <dir> --target chess_spsa
add_executable(chess_spsa EXCLUDE_FROM_ALL ${SRC_FILES})
chess_configure(chess_spsa)
target_compile_definitions(chess_spsa PRIVATE SPSA)
target_link_libraries(chess_spsa PRIVATE ${CHESS_SDL2_TARGET})

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
