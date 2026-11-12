/* SH-4 processor state, as translated code sees it.
 *
 * The layout is kept identical to VF4debug's (src/sh4.h, same maintainer,
 * MIT), field for field, so that translated routines can also be linked
 * into VF4debug and run there in place of its interpreter: that is how the
 * translation is checked against the interpreter (docs/design.md, section 9).
 * Change it only together with VF4debug's.
 */
#ifndef VF4RT_SH4_H
#define VF4RT_SH4_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* SR bits */
#define SR_T   0x00000001u
#define SR_S   0x00000002u
#define SR_IMASK 0x000000f0u
#define SR_Q   0x00000100u
#define SR_M   0x00000200u
#define SR_FD  0x00008000u
#define SR_BL  0x10000000u
#define SR_RB  0x20000000u
#define SR_MD  0x40000000u
#define SR_VALID 0x700083f3u

/* FPSCR bits */
#define FPSCR_RM  0x00000003u
#define FPSCR_DN  0x00040000u
#define FPSCR_PR  0x00080000u
#define FPSCR_SZ  0x00100000u
#define FPSCR_FR  0x00200000u
#define FPSCR_VALID 0x003fffffu
#define FPSCR_CAUSE  0x0003f000u
#define FPSCR_ENABLE 0x00000f80u
#define FPSCR_FLAG   0x0000007cu
#define FPC_E 0x00020000u
#define FPC_V 0x00010000u
#define FPC_Z 0x00008000u
#define FPC_O 0x00004000u
#define FPC_U 0x00002000u
#define FPC_I 0x00001000u
/* the not-a-number the chip gives as a result (never a payload) */
#define SH4_QNAN_S 0x7fbfffffu
#define SH4_QNAN_D 0x7ff7ffffffffffffull

typedef union {
    float f;
    u32 u;
} fpreg;

typedef struct sh4 {
    u32 r[16];        /* the general registers in use (r0-r7 of the bank SR selects) */
    u32 r_bank[8];    /* r0-r7 of the other bank */
    u32 pc;
    u32 pr;
    u32 sr;           /* every bit but T, which lives in t */
    u32 t;            /* 0 or 1 */
    u32 gbr, vbr, ssr, spc, sgr, dbr;
    u32 mach, macl;
    u32 fpul;
    u32 fpscr;
    fpreg fr[16];     /* the bank FPSCR.FR selects */
    fpreg xf[16];     /* the other bank */
    u32 sq[2][8];     /* the two store queues */

    u64 cycles;       /* virtual time: one per instruction, plus skipped idle time */
    u64 insns;        /* instructions executed */
    u64 next_event;   /* run loop leaves when cycles reaches this */

    u32 pending_irq;  /* highest pending interrupt level (0: none) */
    int stop;         /* set to leave the run loop */
    u32 stop_arg;
    int in_slot;      /* executing a delay slot (interpreter only) */
} sh4;

#endif
