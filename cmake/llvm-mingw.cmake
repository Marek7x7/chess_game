# Cross-compile a Windows .exe with llvm-mingw (clang, lld, libc++ and a UCRT sysroot in one tarball that
# needs no installation: https://github.com/mstorsjo/llvm-mingw/releases, the "ucrt-ubuntu" x86_64 build).
# Put its bin/ directory on PATH, then:
#   cmake -S . -B build-windows -DCHESS_PORTABLE=ON -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(CHESS_MINGW_CXX NAMES x86_64-w64-mingw32-clang++)
find_program(CHESS_MINGW_CC NAMES x86_64-w64-mingw32-clang)
find_program(CHESS_MINGW_RC NAMES x86_64-w64-mingw32-windres)
if(NOT CHESS_MINGW_CXX OR NOT CHESS_MINGW_CC)
    message(FATAL_ERROR "x86_64-w64-mingw32-clang++ not found on PATH; see cmake/llvm-mingw.cmake")
endif()
set(CMAKE_CXX_COMPILER ${CHESS_MINGW_CXX})
set(CMAKE_C_COMPILER ${CHESS_MINGW_CC})
if(CHESS_MINGW_RC)
    set(CMAKE_RC_COMPILER ${CHESS_MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
