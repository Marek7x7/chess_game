# Cross-compile a Windows .exe from Linux with GCC MinGW-w64
# (Fedora: dnf install mingw64-gcc-c++; Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix).
#   cmake -S . -B build-windows -DCHESS_PORTABLE=ON -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
# The engine needs std::thread and std::async, so the POSIX thread model is required: Debian and Ubuntu
# ship both models and their plain x86_64-w64-mingw32-g++ is the "win32" one, which has neither.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(CHESS_MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++)
find_program(CHESS_MINGW_CC NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc)
find_program(CHESS_MINGW_RC NAMES x86_64-w64-mingw32-windres)
if(NOT CHESS_MINGW_CXX OR NOT CHESS_MINGW_CC)
    message(FATAL_ERROR "MinGW-w64 GCC not found (x86_64-w64-mingw32-g++-posix); see cmake/mingw-w64.cmake")
endif()
set(CMAKE_CXX_COMPILER ${CHESS_MINGW_CXX})
set(CMAKE_C_COMPILER ${CHESS_MINGW_CC})
if(CHESS_MINGW_RC)
    set(CMAKE_RC_COMPILER ${CHESS_MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
