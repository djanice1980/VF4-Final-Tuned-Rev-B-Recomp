/* SH-4 interpreter: processor state and entry points.
 *
 * Written from the processor's public documentation (the SH-4 software
 * manual's instruction descriptions) and from what the decompilation shows
 * the game uses. No emulator code was consulted or copied.
 */
#ifndef VF4_SH4_H
#define VF4_SH4_H

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
/* the exception fields: cause (bits 17-12: E V Z O U I), enable (bits 11-7:
 * V Z O U I) and flag (bits 6-2: V Z O U I). cause >> 5 is the enable bit
 * of the same exception, cause >> 10 its flag bit. */
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

    u32 pending_irq;  /* highest pending interrupt level (0: none), kept by hw.c */
    int stop;         /* set to leave the run loop: a STOP_ code */
    u32 stop_arg;
    int in_slot;      /* executing a delay slot */
} sh4;

enum {
    STOP_NONE = 0,
    STOP_EVENT,       /* next_event reached (normal) */
    STOP_RETURN,      /* a nested host->guest call came back */
    STOP_ILLEGAL,     /* unimplemented or illegal instruction */
    STOP_BADPC,       /* PC left the mapped code */
    STOP_BREAK,       /* controller breakpoint or condition */
    STOP_FATAL,       /* the game called its fatal-error routine, or a model gave up */
    STOP_LIMIT,       /* instruction or frame limit */
    STOP_SLEEP        /* sleep with nothing scheduled */
};

/* where a nested host->guest call returns: never a real code address */
#define SH4_RETURN_MAGIC 0xfffffff0u

void sh4_reset(sh4 *c, u32 pc);
/* runs until c->stop is set or cycles reaches next_event; returns the stop code */
int sh4_run(sh4 *c);
/* executes one instruction at c->pc (used by the tests) */
void sh4_step(sh4 *c);

u32 sh4_get_sr(const sh4 *c);
void sh4_set_sr(sh4 *c, u32 v);
void sh4_set_fpscr(sh4 *c, u32 v);
/* enters an exception or interrupt: SPC/SSR/SGR, SR.BL/MD/RB, PC = VBR + offset */
void sh4_exception(sh4 *c, u32 vector_offset);

const char *sh4_stop_name(int code);

/* Diagnosis only (--diag): each bit puts back one thing as it was before
 * milestone 4, so that a difference between two runs can be laid at one
 * change's door. Never set in a normal run. */
#define DIAG_HOST_NAN    1u   /* a NaN result keeps the host's bit pattern (FFC00000) */
#define DIAG_NO_FP_FLAGS 2u   /* the FPSCR cause and flag fields stay as written */
#define DIAG_OLD_VECTORS 4u   /* the vector stubs load r0 with the entry's address and leave it */
#define DIAG_OLD_BOARD_WORD 8u /* the DIMM board's word without the size of what is loaded */
extern u32 sh4_diag;
/* counts for the closing lines: results that were made the chip's NaN, and
 * FPU exceptions an enable bit turned into a trap */
extern u64 sh4_nan_results, sh4_fpu_traps;

#endif
