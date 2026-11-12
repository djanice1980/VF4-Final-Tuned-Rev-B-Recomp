/* The machine: virtual time and its events, the run loop, stop reports,
 * snapshots. */
#include "machine.h"
#include "mem.h"
#include "hw.h"
#include "observe.h"
#include "control.h"
#include "hle.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

volatile int machine_interrupted;
machine_config cfg;
sh4 cpu;
u32 machine_frame;
char machine_stop_text[512];

/* ---- events ---- */

typedef struct event {
    u64 at;
    event_fn fn;
    u32 arg;
    const char *name;
} event;

#define EVENT_MAX 64
static event events[EVENT_MAX];
static int event_n;

#define EVENT_FN_MAX 32
static event_fn event_fns[EVENT_FN_MAX];
static const char *event_fn_names[EVENT_FN_MAX];
static int event_fn_n;

static int event_fn_id(event_fn fn, const char *name)
{
    for (int i = 0; i < event_fn_n; i++)
        if (event_fns[i] == fn)
            return i;
    if (event_fn_n < EVENT_FN_MAX) {
        event_fns[event_fn_n] = fn;
        event_fn_names[event_fn_n] = name;
        return event_fn_n++;
    }
    return -1;
}

void event_register(event_fn fn, const char *name)
{
    event_fn_id(fn, name);
}

void event_add(u64 at, event_fn fn, u32 arg, const char *name)
{
    event_fn_id(fn, name);
    if (event_n >= EVENT_MAX) {
        fprintf(stderr, "machine: event table full (adding %s)\n", name);
        return;
    }
    events[event_n].at = at;
    events[event_n].fn = fn;
    events[event_n].arg = arg;
    events[event_n].name = name;
    event_n++;
    if (at < cpu.next_event)
        cpu.next_event = at;
}

u64 event_next(void)
{
    u64 best = ~0ull;
    for (int i = 0; i < event_n; i++)
        if (events[i].at < best)
            best = events[i].at;
    return best;
}

/* recomp: not static: the native runtime's events (runtime/native) fire them too */
void events_fire(sh4 *c);
void events_fire(sh4 *c)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < event_n; i++)
            if (events[i].at <= c->cycles && (best < 0 || events[i].at < events[best].at))
                best = i;
        if (best < 0)
            return;
        event e = events[best];
        events[best] = events[--event_n];
        e.fn(c, e.arg);
    }
}

/* ---- stops ---- */

void machine_stop(sh4 *c, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(machine_stop_text, sizeof machine_stop_text, fmt, ap);
    va_end(ap);
    c->stop = code;
    c->next_event = 0;
}

void machine_idle_skip(sh4 *c)
{
    u64 next = event_next();
    if (next != ~0ull && next > c->cycles)
        c->cycles = next;
    c->next_event = 0;
}

void machine_where(sh4 *c, FILE *f)
{
    char mode[160];
    fprintf(f, "  pc %08x (%s)  pr %08x (%s)  sr %08x  fpscr %08x\n", c->pc, addr_text(c->pc), c->pr,
            addr_text(c->pr), sh4_get_sr(c), c->fpscr);
    for (int i = 0; i < 16; i += 4)
        fprintf(f, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1], i + 2,
                c->r[i + 2], i + 3, c->r[i + 3]);
    fprintf(f, "  mode %s  game frame %u  vblank_count %u  virtual vblanks %u  instructions %llu\n",
            game_mode_name(game_mode_main(), game_mode_sub(), mode, sizeof mode), game_frames(), game_vblanks(),
            machine_frame, (unsigned long long)c->insns);
}

void machine_backtrace(sh4 *c, FILE *f)
{
    /* no frame pointers to follow in general: list the stack words that are
     * return addresses (the instruction two before is a call) */
    u32 sp = c->r[15];
    int shown = 0;
    fprintf(f, "  call chain (stack words that follow a call instruction):\n");
    fprintf(f, "    at   %s\n", addr_text(c->pc));
    fprintf(f, "    pr   %s\n", addr_text(c->pr));
    for (u32 a = sp; a < sp + 0x800 && shown < 24; a += 4) {
        if (!mem_ptr(a))
            break;
        u32 v = g_r32(a);
        if (!routine_containing(v) || (v & 1))
            continue;
        u32 op = g_r16(v - 4);
        int is_call = (op & 0xf000) == 0xb000 || (op & 0xf0ff) == 0x400b || (op & 0xf0ff) == 0x0003;
        if (!is_call)
            continue;
        fprintf(f, "    +%03x %s\n", a - sp, addr_text(v));
        shown++;
    }
}

void machine_first_seen(sh4 *c, routine *r)
{
    (void)c;
    if (cfg.verbose > 1)
        fprintf(stderr, "[new] vb %u  %s  (%s)\n", machine_frame, r->name, r->file);
}

void machine_break(sh4 *c, routine *r)
{
    control_break(c, r);
}

int machine_outside(sh4 *c, u32 target, int kind)
{
    return hle_outside(c, target, kind);
}

void machine_trapa(sh4 *c, u32 imm)
{
    hw.tra = imm << 2;
    hw.expevt = 0x160;
    sh4_exception(c, 0x100);
}

void machine_fpu_exception(sh4 *c)
{
    /* an FPU exception that traps: event code 0x120, through VBR + 0x100 */
    hw.expevt = 0x120;
    sh4_exception(c, 0x100);
}

void machine_watch_report(sh4 *c, mem_watch *w, u32 addr, u32 val, int size, int is_write)
{
    control_watch_hit(c, w, addr, val, size, is_write);
}

void machine_mode_changed(sh4 *c, int main_mode, int sub_mode)
{
    control_mode_changed(c, main_mode, sub_mode);
}

/* ---- the run loop ---- */

static void vblank_event(sh4 *c, u32 arg)
{
    (void)arg;
    machine_frame++;
    event_add(c->cycles - (c->cycles % FRAME_CYCLES) + FRAME_CYCLES, vblank_event, 0, "vertical blank");
    gfx_sq_blocks_frame = 0;
    hw_gfx_frame(c);
    hw_raise(c, IRQ_VBLANK_IN);         /* the first chip's vertical blank interrupt */
    control_frame(c);
    obs_mode_check(c);
    obs_frame_line(c);
    if (machine_interrupted && !c->stop)
        machine_stop(c, STOP_LIMIT, "interrupted (Ctrl-C)");
    if (cfg.max_frames && machine_frame >= cfg.max_frames && !c->stop)
        machine_stop(c, STOP_LIMIT, "frame limit (%u virtual vertical blanks)", cfg.max_frames);
}

int machine_init(void)
{
    event_n = 0;
    machine_frame = 0;
    event_fn_id(vblank_event, "vertical blank");
    event_add(FRAME_CYCLES, vblank_event, 0, "vertical blank");
    return 0;
}

int machine_run(sh4 *c)
{
    int sleeping = 0;
    c->stop = 0;
    for (;;) {
        if (c->stop == STOP_SLEEP) {
            c->stop = 0;
            sleeping = 1;
        }
        if (c->stop)
            return c->stop;
        if (sleeping) {
            u64 next = event_next();
            if (next == ~0ull) {
                machine_stop(c, STOP_SLEEP, "sleep with no event scheduled");
                return c->stop;
            }
            if (next > c->cycles)
                c->cycles = next;
        }
        events_fire(c);
        if (c->stop && c->stop != STOP_SLEEP)
            return c->stop;
        u32 code;
        int level = hw_irq_pending(c, &code);
        if (level && !(c->sr & SR_BL) && (u32)level > ((c->sr & SR_IMASK) >> 4)) {
            hw.intevt = code;
            sleeping = 0;
            sh4_exception(c, 0x600);
        }
        if (sleeping)
            continue;
        if (cfg.max_insns && c->insns >= cfg.max_insns) {
            machine_stop(c, STOP_LIMIT, "instruction limit (%llu)", (unsigned long long)cfg.max_insns);
            return c->stop;
        }
        u64 next = event_next();
        if (cfg.max_insns && cfg.max_insns - c->insns < next - c->cycles)
            next = c->cycles + (cfg.max_insns - c->insns);
        c->next_event = next;
        sh4_run(c);
    }
}

/* ---- snapshots ---- */

#define SNAP_MAGIC 0x34465632u   /* "2VF4" */

typedef struct snap_blob {
    const char *name;
    void *ptr;
    u32 size;
} snap_blob;
static snap_blob blobs[64];
static int blob_n;

void snapshot_register(const char *name, void *ptr, u32 size)
{
    if (blob_n < 64) {
        blobs[blob_n].name = name;
        blobs[blob_n].ptr = ptr;
        blobs[blob_n].size = size;
        blob_n++;
    }
}

/* The file: the magic word; the processor's state and the hardware
 * registers, each with its size before it; the frame count; the scheduled
 * events; the modules' own state (name, size, bytes each); the memory
 * areas (address, size, then each 4 KiB page as one tag byte, 0 for a page
 * of zeros, 1 followed by the page). tools/snapdiff.py reads the same. */
static int snapshot_write(FILE *f);

int snapshot_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    int bad = snapshot_write(f);
    if (fclose(f))
        bad = 1;
    return bad ? -1 : 0;
}

/* the same into memory (for the controller's sandbox); the caller frees *data */
int snapshot_save_mem(u8 **data, size_t *size)
{
    char *buf = NULL;
    FILE *f = open_memstream(&buf, size);
    if (!f)
        return -1;
    int bad = snapshot_write(f);
    if (fclose(f))
        bad = 1;
    if (bad) {
        free(buf);
        return -1;
    }
    *data = (u8 *)buf;
    return 0;
}

static int snapshot_write(FILE *f)
{
    u32 magic = SNAP_MAGIC, n;
    fwrite(&magic, 4, 1, f);
    n = sizeof cpu;
    fwrite(&n, 4, 1, f);
    fwrite(&cpu, sizeof cpu, 1, f);
    n = sizeof hw;
    fwrite(&n, 4, 1, f);
    fwrite(&hw, sizeof hw, 1, f);
    fwrite(&machine_frame, 4, 1, f);
    n = (u32)event_n;
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < event_n; i++) {
        int id = event_fn_id(events[i].fn, events[i].name);
        fwrite(&events[i].at, 8, 1, f);
        fwrite(&id, 4, 1, f);
        fwrite(&events[i].arg, 4, 1, f);
    }
    n = (u32)blob_n;
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < blob_n; i++) {
        char name[32] = "";
        snprintf(name, sizeof name, "%s", blobs[i].name);
        fwrite(name, sizeof name, 1, f);
        fwrite(&blobs[i].size, 4, 1, f);
        fwrite(blobs[i].ptr, blobs[i].size, 1, f);
    }
    n = (u32)mem_area_count;
    fwrite(&n, 4, 1, f);
    static const u8 zero[4096];
    for (int i = 0; i < mem_area_count; i++) {
        mem_area *a = &mem_areas[i];
        fwrite(&a->phys, 4, 1, f);
        fwrite(&a->size, 4, 1, f);
        /* pages that are all zero are stored as one byte */
        for (u32 off = 0; off < a->size; off += 4096) {
            u8 tag = memcmp(a->host + off, zero, 4096) != 0;
            fputc(tag, f);
            if (tag)
                fwrite(a->host + off, 4096, 1, f);
        }
    }
    return ferror(f);
}

/* a reader over the file's bytes that never runs past the end */
typedef struct snap_in {
    const u8 *p, *end;
    int bad;
} snap_in;

static const u8 *snap_take(snap_in *in, size_t n)
{
    if (in->bad || (size_t)(in->end - in->p) < n) {
        in->bad = 1;
        return NULL;
    }
    const u8 *q = in->p;
    in->p += n;
    return q;
}

static u32 snap_u32(snap_in *in)
{
    const u8 *q = snap_take(in, 4);
    u32 v = 0;
    if (q)
        memcpy(&v, q, 4);
    return v;
}

/* Walks the whole file: with apply 0 nothing of the machine is touched and
 * the result says whether the file is a snapshot of this build, whole to
 * its last page; with apply 1 the same walk loads it. */
static const char *snapshot_walk(const u8 *data, size_t size, int apply)
{
    snap_in in = { data, data + size, 0 };
    const u8 *q;
    if (snap_u32(&in) != SNAP_MAGIC)
        return "it is not a snapshot of this version of the harness";
    if (snap_u32(&in) != sizeof cpu || !(q = snap_take(&in, sizeof cpu)))
        return "the processor state has another size (another build made it)";
    if (apply)
        memcpy(&cpu, q, sizeof cpu);
    if (snap_u32(&in) != sizeof hw || !(q = snap_take(&in, sizeof hw)))
        return "the hardware state has another size (another build made it)";
    if (apply)
        memcpy(&hw, q, sizeof hw);
    u32 frame = snap_u32(&in), n = snap_u32(&in);
    if (in.bad || n > EVENT_MAX)
        return "the event list is damaged";
    if (apply) {
        machine_frame = frame;
        event_n = (int)n;
    }
    for (u32 i = 0; i < n; i++) {
        q = snap_take(&in, 16);
        int id = -1;
        if (q)
            memcpy(&id, q + 8, 4);
        if (!q || id < 0 || id >= event_fn_n)
            return "it has an event this build does not know";
        if (apply) {
            memcpy(&events[i].at, q, 8);
            memcpy(&events[i].arg, q + 12, 4);
            events[i].fn = event_fns[id];
            events[i].name = event_fn_names[id];
        }
    }
    if (snap_u32(&in) != (u32)blob_n || in.bad)
        return "it holds the state of another set of models (another build made it)";
    for (int i = 0; i < blob_n; i++) {
        const u8 *name = snap_take(&in, 32);
        u32 bsize = snap_u32(&in);
        if (!name || strncmp((const char *)name, blobs[i].name, 31) || bsize != blobs[i].size ||
            !(q = snap_take(&in, bsize)))
            return "the state of one of the models has another layout (another build made it)";
        if (apply)
            memcpy(blobs[i].ptr, q, bsize);
    }
    if (snap_u32(&in) != (u32)mem_area_count || in.bad)
        return "it has another set of memory areas";
    for (int i = 0; i < mem_area_count; i++) {
        mem_area *a = &mem_areas[i];
        u32 phys = snap_u32(&in), asize = snap_u32(&in);
        if (in.bad || phys != a->phys || asize != a->size)
            return "its memory areas are not this build's";
        for (u32 off = 0; off < a->size; off += 4096) {
            q = snap_take(&in, 1);
            if (!q || *q > 1)
                return "it ends early or is damaged (a memory page is missing)";
            if (*q == 0) {
                if (apply)
                    memset(a->host + off, 0, 4096);
                continue;
            }
            const u8 *page = snap_take(&in, 4096);
            if (!page)
                return "it ends early (a memory page is cut off)";
            if (apply)
                memcpy(a->host + off, page, 4096);
        }
    }
    if (in.p != in.end)
        return "there are bytes after its end";
    return NULL;
}

char snapshot_error[256];

int snapshot_load_mem(const u8 *data, size_t size)
{
    const char *why = snapshot_walk(data, size, 0);
    if (!why)
        why = snapshot_walk(data, size, 1);
    if (why) {
        snprintf(snapshot_error, sizeof snapshot_error, "the snapshot in memory: %s", why);
        return -1;
    }
    cpu.stop = 0;
    cpu.next_event = 0;
    return 0;
}

/* All or nothing: the file is read and checked from end to end before a
 * byte of the machine is changed, so a file that cannot be loaded leaves
 * the machine as it was (and the caller with a reason in snapshot_error). */
int snapshot_load(const char *path)
{
    snapshot_error[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(snapshot_error, sizeof snapshot_error, "%s: %s", path, strerror(errno));
        return -1;
    }
    u8 *data = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET)) {
        data = malloc((size_t)size + 1);
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (!data) {
        snprintf(snapshot_error, sizeof snapshot_error, "%s: cannot be read", path);
        return -1;
    }
    const char *why = snapshot_walk(data, (size_t)size, 0);
    if (!why)
        why = snapshot_walk(data, (size_t)size, 1);
    free(data);
    if (why) {
        snprintf(snapshot_error, sizeof snapshot_error, "%s: %s", path, why);
        return -1;
    }
    cpu.stop = 0;
    cpu.next_event = 0;
    return 0;
}
