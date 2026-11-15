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

# The launcher (M5): the builder's window, Build / Update / Play / Add to
# Steam. It holds no game code, so it builds without the translation.
# The drawing kit it shares with the game's menu (runtime/ui).
set(VF4FT_UI runtime/ui/ui.c runtime/ui/fonts.c runtime/ui/options.c runtime/ui/kofi_badge.c runtime/ui/card_editor.c)
add_executable(vf4ft-launcher runtime/launcher/launcher.c runtime/host/settings.c runtime/host/cards.c
               ${VF4FT_UI})
target_include_directories(vf4ft-launcher PRIVATE runtime/host runtime/ui)
target_link_libraries(vf4ft-launcher PRIVATE SDL3::SDL3)
target_compile_options(vf4ft-launcher PRIVATE -Wall -Wextra)
if(WIN32)
    set_target_properties(vf4ft-launcher PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(vf4ft-launcher PRIVATE -static)
endif()

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
# and the seam to the translated main program (runtime/native). Its routine
# and global tables (docs/design.md, section 5) come from meta/
# (tools/make_tables.py, as in the player's build), or, with
# -DVF4FT_TABLES=decomp, from the decompilation with VF4debug's own
# tools/gen_tables.py, which adds the declarations the observer's call log
# shows. The default is decomp when both checkouts are beside this one.
# The test program's tables (tables_test.c, test mode) always come from
# meta/: gen_tables.py makes only the main program's.
if(TARGET vf4ft_gen_main AND TARGET vf4ft_gen_test)
    set(VF4_DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../vf4ft-decomp" CACHE PATH "the vf4ft-decomp checkout")
    set(VF4DEBUG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../VF4debug" CACHE PATH "the VF4debug checkout (its tools/)")
    if(EXISTS "${VF4DEBUG_DIR}/tools/gen_tables.py" AND EXISTS "${VF4_DECOMP}/ghidra/source_map.tsv")
        set(tables_default decomp)
    else()
        set(tables_default meta)
    endif()
    set(VF4FT_TABLES ${tables_default} CACHE STRING "where the machine's routine and global tables come from: meta or decomp")
    set(machine_gen "${CMAKE_CURRENT_BINARY_DIR}/machine_gen")
    if(VF4FT_TABLES STREQUAL "decomp")
        add_custom_command(OUTPUT "${machine_gen}/tables.c"
            COMMAND python3 -I "${VF4DEBUG_DIR}/tools/gen_tables.py" "${VF4_DECOMP}" "${machine_gen}"
            DEPENDS "${VF4DEBUG_DIR}/tools/gen_tables.py"
            COMMENT "Routine and global tables from the decompilation"
            VERBATIM)
        add_custom_command(OUTPUT "${machine_gen}/tables_test.c"
            COMMAND python3 -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_tables.py" "${machine_gen}" --test-only
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_tables.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/meta/test/routines.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/meta/test/globals.tsv"
            COMMENT "The test program's routine and global tables from meta/"
            VERBATIM)
    else()
        add_custom_command(OUTPUT "${machine_gen}/tables.c" "${machine_gen}/tables_test.c"
            COMMAND python3 -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_tables.py" "${machine_gen}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_tables.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/meta/main/routines.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/meta/main/globals.tsv"
                    "${CMAKE_CURRENT_SOURCE_DIR}/meta/test/routines.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/meta/test/globals.tsv"
            COMMENT "Routine and global tables from meta/"
            VERBATIM)
    endif()
    file(GLOB machine_src CONFIGURE_DEPENDS runtime/machine/*.c)
    # objects, not an archive: the translated code calls back into rt_native.c
    add_library(vf4ft_machine OBJECT runtime/native/rt_native.c ${machine_src} "${machine_gen}/tables.c"
                "${machine_gen}/tables_test.c" runtime/sound/arm7.c runtime/sound/aica.c)
    # runtime/machine first: its sh4.h is the one the seam sees (runtime/cpu/sh4.h)
    target_include_directories(vf4ft_machine BEFORE PUBLIC runtime/machine runtime/native runtime/sound)
    target_compile_options(vf4ft_machine PRIVATE -std=gnu11 -Wall -Wextra -Wno-unused-parameter ${VF4FT_FP_FLAGS})
    set_source_files_properties("${machine_gen}/tables.c" "${machine_gen}/tables_test.c" PROPERTIES COMPILE_OPTIONS "-O0;-g0")
    target_link_libraries(vf4ft_machine PUBLIC vf4ft_gen_main vf4ft_gen_test m)
    if(WIN32)
        target_link_libraries(vf4ft_machine PUBLIC pthread)    # winpthreads, from llvm-mingw (view.c's workers)
    endif()

    # The game (milestone 3): the machine on its own thread, the window, the
    # controls. Installed by the builder (tools/make_game.py), it reads the
    # disc's file and its key from data/ beside itself; a development build
    # gets data/main.bin and data/vf41_s.dec as links to the decompilation's
    # files.
    add_executable(vf4ft runtime/host/main.c runtime/host/game_machine.c runtime/host/dimm_image.c
                         runtime/host/settings.c runtime/host/gpu_render.c runtime/host/menu.c runtime/host/updates.c
                         runtime/host/art.c runtime/host/cards.c runtime/host/udp.c runtime/host/session.c
                         runtime/host/snap_patch.c runtime/host/secure.c runtime/crypto/monocypher.c
                         runtime/crypto/monocypher-ed25519.c ${VF4FT_UI})
    target_include_directories(vf4ft PRIVATE runtime/host runtime/ui runtime/crypto)
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
        target_link_libraries(vf4ft PRIVATE ws2_32 bcrypt)    # the two-PC game's sockets and random numbers (Windows' own)
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
    message(WARNING "no translated code for both programs in ${VF4FT_GEN_DIR}: run translator/translate.py "
                    "for main and test first (README.md); "
                    "only the CPU runtime is built")
endif()
