cmake_minimum_required(VERSION 3.12)
project(ChessGame LANGUAGES CXX)

# Force C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build when the caller doesn't pick one explicitly.
# Only applies to a fresh, never-configured build directory -- an existing
# build dir's cached CMAKE_BUILD_TYPE (e.g. Debug, for ASan/TSan variants)
# always wins over this default.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Enable strict warnings across different compilers
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# -march=native tunes for the CPU that builds the binary; the result can
# SIGILL on a different CPU. Fine (and wanted) for this project's actual use
# case -- building and SPRT-testing on the same machine -- so on by default;
# override with -DCHESS_NATIVE_ARCH=OFF if portability ever matters.
option(CHESS_NATIVE_ARCH "Build with -march=native" ON)
if(CHESS_NATIVE_ARCH AND NOT MSVC)
    add_compile_options(-march=native)
endif()

# Link-time optimisation for release builds (inlines across source files, e.g.
# pieceValue() into the move-ordering code) when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT CHESS_IPO_OK OUTPUT CHESS_IPO_MESSAGE)
if(CHESS_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Find the SDL2 dependency portably
find_package(SDL2 REQUIRED)

# Collect all source files
file(GLOB_RECURSE SRC_FILES "src/*.cpp")

# Define the executable target (CMake automatically appends .exe on Windows)
add_executable(chess ${SRC_FILES})

# Include headers path
target_include_directories(chess PRIVATE include)

# Link SDL2 portably
target_link_libraries(chess PRIVATE SDL2::SDL2)

# Search-constant tuning build (tools/spsa.py): the same engine with every
# SEARCH_PARAM exposed as a UCI spin option. cmake --build <dir> --target chess_spsa
add_executable(chess_spsa EXCLUDE_FROM_ALL ${SRC_FILES})
target_include_directories(chess_spsa PRIVATE include)
target_compile_definitions(chess_spsa PRIVATE SPSA)
target_link_libraries(chess_spsa PRIVATE SDL2::SDL2)

# Texel tuner for the evaluation weights (tools/tune.cpp). Not part of the
# default build: cmake --build <dir> --target chess_tune. Compiled with
# -DTUNING so every TUNE_ weight is a registered, mutable variable.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    set(TUNE_SRC ${SRC_FILES})
    list(FILTER TUNE_SRC EXCLUDE REGEX ".*/(main|gui)\\.cpp$")
    add_executable(chess_tune EXCLUDE_FROM_ALL ${TUNE_SRC} tools/tune.cpp)
    target_include_directories(chess_tune PRIVATE include)
    target_compile_definitions(chess_tune PRIVATE TUNING)
    target_link_libraries(chess_tune PRIVATE OpenMP::OpenMP_CXX)
endif()
