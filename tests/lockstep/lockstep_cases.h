/* Shared by the generated cases (which see the processor state through the
 * recomp's runtime/cpu/sh4.h) and the harness (through VF4debug's src/sh4.h):
 * the two describe the same struct sh4, field for field. */
#ifndef LOCKSTEP_CASES_H
#define LOCKSTEP_CASES_H

struct sh4;

typedef struct insn_case {
    unsigned short op;
    signed char mode;        /* -1: mode does not matter; 0/1: the bit fixed at translation; 2: tested at run time */
    signed char field;       /* the bit the mode is: 1 SZ, 2 PR, 0 none */
    signed char mem;         /* addresses memory through registers */
    signed char indexed;     /* @(r0,rn) or @(r0,gbr): r0 is an offset */
    void (*fn)(struct sh4 *c);
    const char *text;
} insn_case;

extern const insn_case insn_cases[];
extern const unsigned insn_case_count;
extern const unsigned char insn_pool[];
extern const unsigned insn_pool_size;
extern const unsigned insn_code_addr;
void case_fpscr_loaded(struct sh4 *c);

/* the translated programs' routine tables (rt_dispatch.h) */
typedef struct lockstep_routine {
    unsigned addr;
    void (*fn)(struct sh4 *c);
    const char *name;
} lockstep_routine;
extern const lockstep_routine M_routines[];
extern const unsigned M_routine_count;
extern const lockstep_routine *rt_table;
extern unsigned rt_table_count;
extern const unsigned rt_sh4_size;

#endif
