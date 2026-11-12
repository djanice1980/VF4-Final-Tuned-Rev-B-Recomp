/* The controller: a script (a file, or lines on standard input) that feeds
 * inputs frame by frame, pokes named globals, runs to a mode or a routine,
 * saves and restores snapshots and stops on conditions. The command list
 * is in README.md and in control.c's help text. */
#ifndef VF4_CONTROL_H
#define VF4_CONTROL_H

#include "sh4.h"
#include "dispatch.h"
#include "mem.h"

/* the cabinet's switches, in the harness's own layout; the input
 * board model (hw_maple.c) turns them into what the game reads */
enum {
    BTN_UP = 0x01, BTN_DOWN = 0x02, BTN_LEFT = 0x04, BTN_RIGHT = 0x08,
    BTN_P = 0x10, BTN_K = 0x20, BTN_G = 0x40, BTN_E = 0x80,
    BTN_START = 0x100
};
enum { SYS_TEST = 0x01, SYS_SERVICE = 0x02 };

typedef struct input_state {
    u32 player[2];       /* BTN_ bits held */
    u32 sys;             /* SYS_ bits held */
    u32 coin_switch[2];  /* coin chute switches closed right now */
    u32 coins[2];        /* coins inserted in all, per chute */
} input_state;

/* Memory of the harness's own in the machine's address space, for the
 * arguments of the controller's "call": eight buffers of 8 KiB ("buf:0" to
 * "buf:7") and 64 KiB for strings ("str:TEXT"). Nothing of the game is
 * there and the game never looks there. */
#define CONTROL_SCRATCH 0x0f000000u
#define CONTROL_SCRATCH_SIZE 0x20000u

extern input_state inputs;
extern int control_failed;      /* a command failed in a way that ends a script (a snapshot that did not load) */

int control_init(const char *script_path);
/* runs commands until one needs the machine to run; 0 when the script is over */
int control_next(sh4 *c);
/* tells the controller why the machine stopped; returns 0 when the run should end */
int control_stopped(sh4 *c, int code);
void control_frame(sh4 *c);
void control_break(sh4 *c, routine *r);
void control_watch_hit(sh4 *c, mem_watch *w, u32 addr, u32 val, int size, int is_write);
void control_mode_changed(sh4 *c, int main_mode, int sub_mode);
/* a guest address from "name", "name+0x10" or a number; 0 when unknown */
u32 control_resolve(const char *text);

#endif
