cmake_minimum_required(VERSION 3.24)
project(vf4ft_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Windows (llvm-mingw cross build): SDL3 from toolchain/sdl3-win64 (tools/fetch_toolchain.sh).
# Linux: the system's SDL3 (pacman: sdl3).
if(WIN32 AND NOT DEFINED SDL3_DIR)
    set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/toolchain/sdl3-win64/lib/cmake/SDL3")
endif()
find_package(SDL3 3.4 REQUIRED CONFIG COMPONENTS SDL3-shared)

add_executable(vf4ft runtime/host/main.c)
target_link_libraries(vf4ft PRIVATE SDL3::SDL3)
target_compile_options(vf4ft PRIVATE -Wall -Wextra)

if(WIN32)
    # A windowed program: no console window when started from Steam or Explorer.
    set_target_properties(vf4ft PROPERTIES WIN32_EXECUTABLE ON)
    # Nothing from the toolchain's own DLLs: only system DLLs and SDL3.dll beside the exe.
    target_link_options(vf4ft PRIVATE -static)
    add_custom_command(TARGET vf4ft POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:vf4ft>)
endif()
