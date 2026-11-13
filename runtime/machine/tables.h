/* The tables tools/gen_tables.py builds from the decompilation at build
 * time (build/gen/tables.c). */
#ifndef VF4_TABLES_H
#define VF4_TABLES_H

#include <stdint.h>

typedef struct gen_routine {
    uint32_t addr;
    uint32_t size;
    const char *name;
    const char *file;
    const char *proto;
    const char *spec;     /* where the arguments are: see tools/gen_tables.py; NULL without a declaration */
    int is_asm;           /* hand-written assembly (source/asm of the decompilation) */
} gen_routine;

typedef struct gen_global {
    uint32_t addr;
    const char *name;
} gen_global;

extern const gen_routine gen_routines[];
extern const int gen_routine_count;
extern const gen_global gen_globals[];
extern const int gen_global_count;

/* recomp (test mode): the test program's tables (tables_test.c, from meta/
 * by tools/make_tables.py), and the program the machine runs now, whose
 * tables dispatch.c, observe.c and control.c read: main.bin's (gen_*) until
 * the BIOS stand-in starts the test program (disp_program_set) */
extern const gen_routine gen_test_routines[];
extern const int gen_test_routine_count;
extern const gen_global gen_test_globals[];
extern const int gen_test_global_count;
enum { PROGRAM_MAIN = 0, PROGRAM_TEST = 1 };
extern int disp_program;
extern const gen_routine *prog_routines;
extern int prog_routine_count;
extern const gen_global *prog_globals;
extern int prog_global_count;

/* address of a named global, or 0 */
uint32_t global_addr(const char *name);
/* "name+0x10" for a data address (static buffer ring), or NULL when none is near */
const char *global_text(uint32_t addr);

#endif
