/* Per-routine dispatch: every routine of the game image has a row that says
 * how it runs.
 *
 *   RK_ORIGINAL  the game's own bytes, under the interpreter (the default)
 *   RK_HOST      a host replacement (src/hle_*.c): called with the SH-4
 *                state, takes its arguments from the registers by the ABI
 *                and leaves the result there
 *   RK_NATIVE    reserved: the decompilation's C compiled for the host
 *                (milestone 2); the row and the seam exist, nothing uses it
 *
 * The interpreter looks at one byte per target on every call and jump
 * (disp_flags); a non-zero byte brings it here.
 */
#ifndef VF4_DISPATCH_H
#define VF4_DISPATCH_H

#include "sh4.h"

/* how a target is reached; DISP_FALL: execution ran into a routine's entry
 * without a call or a jump (only the observer tells) */
enum { DISP_CALL = 0, DISP_JUMP = 1, DISP_RETURN = 2, DISP_EXCEPTION = 3, DISP_FALL = 4 };
enum { RK_ORIGINAL = 0, RK_HOST = 1, RK_NATIVE = 2 };

/* disp_flags bits */
#define DF_FIRST 0x01   /* not entered yet */
#define DF_HOST  0x02   /* has a host replacement or a hook */
#define DF_LOG   0x04   /* the observer wants to see calls */
#define DF_COUNT 0x08   /* call-count profile is on */
#define DF_BREAK 0x10   /* the controller wants to stop here */
#define DF_TRACE 0x20   /* full trace of this routine */

struct routine;
typedef void (*host_fn)(sh4 *c, struct routine *r);

typedef struct routine {
    u32 addr;
    u32 size;
    const char *name;
    const char *file;
    const char *proto;      /* C declaration from the decompilation, or NULL */
    u8 kind;                /* RK_ */
    u8 seen;
    u8 exc;                 /* entered by an exception at least once */
    host_fn host;           /* RK_HOST: the replacement */
    host_fn hook;           /* runs before the routine, whatever its kind */
    const char *why;        /* one line: why it is replaced or hooked */
    u64 calls;              /* entries; exact with the observer on (else only while a flag brings calls here,
                             * and a jump back to the routine's own start counts too) */
    u32 first_frame;        /* vblank count when first entered */
    s32 first_mode;         /* main * 256 + sub when first entered */
    u64 first_insn;
    u32 host_calls;
    u64 hcalls;             /* entries while the harness ran it in a sandbox (the controller's "call"): not the game's */
} routine;

extern routine *routines;       /* sorted by address */
extern int routine_count;
extern u8 *disp_flags;          /* one byte per halfword of the image */
extern u32 *disp_index;         /* routine index + 1 at an entry, else 0 */
extern u32 disp_slots;

int disp_init(void);
routine *routine_by_name(const char *name);
routine *routine_at(u32 addr);          /* exact entry */
routine *routine_containing(u32 addr);  /* the routine an address lies in */
/* "name+0x12" (or the address when outside the image), in a static buffer ring */
const char *addr_text(u32 addr);
static inline u32 disp_slot(u32 addr) { return ((addr & 0x1fffffffu) - 0x0c020000u) >> 1; }
void disp_set_flag(routine *r, u8 flag, int on);
void disp_set_flag_all(u8 flag, int on);

/* from the interpreter */
void disp_enter(sh4 *c, u32 target, int kind);
void disp_outside(sh4 *c, u32 target, int kind);
void disp_enter_exception(sh4 *c, u32 pc);
void disp_leave_exception(sh4 *c);
void disp_trapa(sh4 *c, u32 imm);
extern int disp_exc_depth;

/* host replacements register themselves with these */
int disp_replace(const char *name, host_fn fn, const char *why);
int disp_hook(const char *name, host_fn fn, const char *why);

/* arguments by the ABI: integers and pointers in r4-r7 then the stack,
 * floats in fr4-fr11 */
static inline u32 arg(sh4 *c, int i) { return c->r[4 + i]; }
u32 arg_stack(sh4 *c, int i);
static inline void ret(sh4 *c, u32 v) { c->r[0] = v; }

/* runs a guest routine to its return from host code (nested); returns r0 */
u32 guest_call(sh4 *c, u32 addr, u32 a0, u32 a1, u32 a2, u32 a3);

#endif
