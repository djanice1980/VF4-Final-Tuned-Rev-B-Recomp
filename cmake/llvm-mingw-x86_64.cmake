# Cross build for Windows x86-64 with llvm-mingw (clang + lld, UCRT).
# The toolchain lives in <repo>/toolchain/llvm-mingw (tools/fetch_toolchain.sh);
# set LLVM_MINGW to use one somewhere else.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{LLVM_MINGW})
    set(_llvm_mingw "$ENV{LLVM_MINGW}")
else()
    get_filename_component(_llvm_mingw "${CMAKE_CURRENT_LIST_DIR}/../toolchain/llvm-mingw" ABSOLUTE)
endif()

set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   "${_llvm_mingw}/bin/${_triple}-clang")
set(CMAKE_CXX_COMPILER "${_llvm_mingw}/bin/${_triple}-clang++")
set(CMAKE_RC_COMPILER  "${_llvm_mingw}/bin/${_triple}-windres")
set(CMAKE_AR           "${_llvm_mingw}/bin/llvm-ar")
set(CMAKE_RANLIB       "${_llvm_mingw}/bin/llvm-ranlib")

set(CMAKE_FIND_ROOT_PATH "${_llvm_mingw}/${_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
