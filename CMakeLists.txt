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

# The translated CPU: runtime pieces, and the code the translator made from
# your copy of the game (work/gen, from translator/translate.py). Warnings in
# generated code are errors: they mean the translator emitted something wrong.
add_library(vf4ft_cpu STATIC runtime/cpu/rt_core.c runtime/cpu/sh4rt_fpu.c)
target_include_directories(vf4ft_cpu PUBLIC runtime/cpu)
target_compile_options(vf4ft_cpu PRIVATE -Wall -Wextra)

set(VF4FT_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/gen" CACHE PATH "translator output (one folder per program)")
foreach(prog main test)
    file(GLOB gen_src CONFIGURE_DEPENDS "${VF4FT_GEN_DIR}/${prog}/*.c")
    if(gen_src)
        add_library(vf4ft_gen_${prog} STATIC ${gen_src})
        target_include_directories(vf4ft_gen_${prog} PRIVATE "${VF4FT_GEN_DIR}/${prog}")
        target_link_libraries(vf4ft_gen_${prog} PUBLIC vf4ft_cpu)
        target_compile_options(vf4ft_gen_${prog} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

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
