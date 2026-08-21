cmake_minimum_required(VERSION 3.12)
project(ChessGame LANGUAGES CXX)

# Force C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable strict warnings across different compilers
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
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
