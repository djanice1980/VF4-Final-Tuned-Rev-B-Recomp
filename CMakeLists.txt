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
# Floating point as the interpreter does it: SSE, no fused multiply-add.
set(VF4FT_FP_FLAGS -msse2 -ffp-contract=off -fno-strict-aliasing)

add_library(vf4ft_cpu STATIC runtime/cpu/rt_core.c runtime/cpu/sh4rt_fpu.c)
target_include_directories(vf4ft_cpu PUBLIC runtime/cpu)
target_compile_options(vf4ft_cpu PRIVATE -Wall -Wextra ${VF4FT_FP_FLAGS})

set(VF4FT_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/gen" CACHE PATH "translator output (one folder per program)")
foreach(prog main test)
    file(GLOB gen_src CONFIGURE_DEPENDS "${VF4FT_GEN_DIR}/${prog}/*.c")
    if(gen_src)
        add_library(vf4ft_gen_${prog} STATIC ${gen_src})
        target_include_directories(vf4ft_gen_${prog} PRIVATE "${VF4FT_GEN_DIR}/${prog}")
        target_link_libraries(vf4ft_gen_${prog} PUBLIC vf4ft_cpu)
        target_compile_options(vf4ft_gen_${prog} PRIVATE -Wall -Wextra -Werror ${VF4FT_FP_FLAGS})
    endif()
endforeach()

# The machine (milestone 2 and on): VF4debug's device models (runtime/machine)
# and the seam to the translated main program (runtime/native). Development
# builds make its routine and global tables from the decompilation, with
# VF4debug's own tools/gen_tables.py (docs/design.md, section 5); the player's
# build will make them from meta/ (M5).
if(TARGET vf4ft_gen_main)
    set(VF4_DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../vf4ft-decomp" CACHE PATH "the vf4ft-decomp checkout")
    set(VF4DEBUG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../VF4debug" CACHE PATH "the VF4debug checkout (its tools/)")
    set(machine_gen "${CMAKE_CURRENT_BINARY_DIR}/machine_gen")
    add_custom_command(OUTPUT "${machine_gen}/tables.c"
        COMMAND python3 -I "${VF4DEBUG_DIR}/tools/gen_tables.py" "${VF4_DECOMP}" "${machine_gen}"
        DEPENDS "${VF4DEBUG_DIR}/tools/gen_tables.py"
        COMMENT "Routine and global tables from the decompilation"
        VERBATIM)
    file(GLOB machine_src CONFIGURE_DEPENDS runtime/machine/*.c)
    # objects, not an archive: the translated code calls back into rt_native.c
    add_library(vf4ft_machine OBJECT runtime/native/rt_native.c ${machine_src} "${machine_gen}/tables.c")
    # runtime/machine first: its sh4.h is the one the seam sees (runtime/cpu/sh4.h)
    target_include_directories(vf4ft_machine BEFORE PUBLIC runtime/machine runtime/native)
    target_compile_options(vf4ft_machine PRIVATE -std=gnu11 -Wall -Wextra -Wno-unused-parameter ${VF4FT_FP_FLAGS})
    set_source_files_properties("${machine_gen}/tables.c" PROPERTIES COMPILE_OPTIONS "-O0;-g0")
    target_link_libraries(vf4ft_machine PUBLIC vf4ft_gen_main m)
    if(WIN32)
        target_link_libraries(vf4ft_machine PUBLIC pthread)    # winpthreads, from llvm-mingw (view.c's workers)
    endif()

    # The game (milestone 3): the machine on its own thread, the window, the
    # controls. Until the install layout of M5 it reads data/main.bin and
    # data/vf41_s.dec beside itself: links to the decompilation's files here.
    add_executable(vf4ft runtime/host/main.c runtime/host/game_machine.c)
    target_link_libraries(vf4ft PRIVATE SDL3::SDL3 vf4ft_machine)
    target_compile_options(vf4ft PRIVATE -Wall -Wextra ${VF4FT_FP_FLAGS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/data")
    foreach(f main.bin vf41_s.dec)
        if(EXISTS "${VF4_DECOMP}/work/${f}" AND NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/data/${f}")
            file(CREATE_LINK "${VF4_DECOMP}/work/${f}" "${CMAKE_CURRENT_BINARY_DIR}/data/${f}" SYMBOLIC)
        endif()
    endforeach()
    if(WIN32)
        # A windowed program: no console window when started from Steam or Explorer.
        set_target_properties(vf4ft PROPERTIES WIN32_EXECUTABLE ON)
        # Nothing from the toolchain's own DLLs: only system DLLs and SDL3.dll beside the exe.
        target_link_options(vf4ft PRIVATE -static)
        add_custom_command(TARGET vf4ft POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:vf4ft>)
    endif()

    # Milestone 2: the same machine headless, with VF4debug's options and
    # scripts, for the comparisons (tools/compare_runs.py). Linux only.
    if(NOT WIN32)
        add_executable(vf4ft_headless runtime/headless/main.c)
        target_compile_definitions(vf4ft_headless PRIVATE "DEFAULT_DECOMP=\"${VF4_DECOMP}\"")
        target_compile_options(vf4ft_headless PRIVATE -std=gnu11 -Wall -Wextra -Wno-unused-parameter ${VF4FT_FP_FLAGS})
        target_link_libraries(vf4ft_headless PRIVATE vf4ft_machine)
    endif()
else()
    message(WARNING "no translated code in ${VF4FT_GEN_DIR}: run translator/translate.py first (README.md); "
                    "only the CPU runtime is built")
endif()
