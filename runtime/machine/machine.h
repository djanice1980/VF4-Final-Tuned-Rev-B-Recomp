/* The machine around the processor: virtual time, scheduled events, the
 * run loop, stop conditions, snapshots. */
#ifndef VF4_MACHINE_H
#define VF4_MACHINE_H

#include "sh4.h"
#include "dispatch.h"
#include <stddef.h>
#include <stdio.h>

/* Virtual time: one cycle per instruction, 200 MHz, 60 frames a second.
 * (The real chip needs more than one cycle per instruction, so the virtual
 * machine is faster than the real one; the game only ever sees it through
 * the timer and the vertical blank.) */
#define CPU_HZ        200000000ull
#define FRAME_CYCLES  (CPU_HZ / 60)

typedef struct machine_config {
    const char *decomp;       /* the vf4ft-decomp checkout */
    const char *image;        /* main.bin */
    const char *dimm;         /* the decrypted DIMM image (vf41_s.dec) */
    const char *log_path;     /* call log (JSON lines), or NULL */
    const char *run_name;
    const char *script;       /* controller script, "-" for stdin, or NULL */
    const char *report_dir;   /* where status/profile/coverage reports go */
    u64 max_insns;
    u32 max_frames;
    int verbose;
    int hw_trace;             /* print every hardware access */
    int profile;
    int coverage;
    int quiet;
    int country;              /* 0 Japan, 1 USA, 2 export, 3 Korea, 4 Australia */
} machine_config;

extern machine_config cfg;
extern sh4 cpu;
extern u32 machine_frame;         /* virtual vertical blanks since the start */
extern char machine_stop_text[512];
extern volatile int machine_interrupted;      /* set by Ctrl-C: the run ends at the next vertical blank */
/* recomp: the game window's hook (runtime/host), once per vertical blank after
 * the controller's frame step: it takes the picture and sets the inputs */
extern void (*machine_host_frame)(sh4 *c);

int machine_init(void);
void write_reports(void);
int machine_run(sh4 *c);
/* stops the run with a reason (printf style) */
void machine_stop(sh4 *c, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* jumps virtual time to the next scheduled event (an idle wait) */
void machine_idle_skip(sh4 *c);

/* from dispatch.c */
void machine_first_seen(sh4 *c, routine *r);
void machine_break(sh4 *c, routine *r);
int machine_outside(sh4 *c, u32 target, int kind);
void machine_trapa(sh4 *c, u32 imm);
void machine_fpu_exception(sh4 *c);

/* scheduled events */
typedef void (*event_fn)(sh4 *c, u32 arg);
void event_add(u64 at_cycle, event_fn fn, u32 arg, const char *name);
u64 event_next(void);
/* makes an event routine known to snapshots before it is first scheduled */
void event_register(event_fn fn, const char *name);

/* where things are in the call chain (for stop reports) */
void machine_where(sh4 *c, FILE *f);
void machine_backtrace(sh4 *c, FILE *f);

/* snapshots of the whole machine; modules register the state they keep
 * outside guest memory (plain data, no pointers). A load is all or nothing:
 * a file that is missing, cut short or another build's leaves the machine
 * untouched and returns -1. */
int snapshot_save(const char *path);
int snapshot_load(const char *path);
extern char snapshot_error[256];      /* why the last snapshot_load failed */
/* the same to and from memory (the controller's sandbox) */
int snapshot_save_mem(u8 **data, size_t *size);
int snapshot_load_mem(const u8 *data, size_t size);
void snapshot_register(const char *name, void *ptr, u32 size);

struct mem_watch;
void machine_watch_report(sh4 *c, struct mem_watch *w, u32 addr, u32 val, int size, int is_write);
void machine_mode_changed(sh4 *c, int main_mode, int sub_mode);

#endif
