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

/* address of a named global, or 0 */
uint32_t global_addr(const char *name);
/* "name+0x10" for a data address (static buffer ring), or NULL when none is near */
const char *global_text(uint32_t addr);

#endif
