/* The observer: the shadow call stack, the exact counts, the sampling rule
 * and the call log. See observe.h for what it keeps and README.md ("The
 * call log") for the log's keys.
 *
 * How a call gets into the log. Every entry into a routine pushes a frame
 * (a few words); a frame is "captured" (registers, bytes behind pointers,
 * the store journal, the callee list) when at its entry it shows something
 * the routine's kept calls do not have yet: it is one of the routine's
 * first calls, it comes from a new call site, the game is in a new main
 * mode, its declared arguments are a new tuple. What only shows at the
 * return (a new return value, a new path through the routine's own
 * branches) is checked for every call, captured or not: when an uncaptured
 * call shows one, a short "late" record is written from what the frame
 * holds (argument registers, results, the path) and the routine's next
 * calls are captured until one of them shows it again in full.
 */
#include "observe.h"
#include "machine.h"
#include "tables.h"
#include "control.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

obs_rule obs_rules = { .hits = 3, .max_hits = 56, .ret = -1, .peek = 64, .peek_max = 256, .writes = 8192, .quiet = 0 };
FILE *obs_log;
int obs_on, obs_jr_on, obs_rd_on;
u32 obs_sandbox;
u8 *obs_map;
u16 *obs_slot_routine;
u32 obs_pc;
obs_frame *obs_top;

#define OBS_BARRIER 0xffffffffu
#define LVL_SANDBOX 0x80        /* in a frame's or a journal entry's level: made inside a sandbox */
#define OBS_NONE    0xfffffffeu

/* how a frame ended */
enum { H_RET, H_LEFT, H_RTE, H_END, H_JUMPED, H_SANDBOX };

/* why a call is kept */
#define W_FIRST 0x01
#define W_SITE  0x02
#define W_MODE  0x04
#define W_ARGS  0x08
#define W_RET   0x10
#define W_PATH  0x20
#define W_TRACE 0x40
#define W_HUNT  0x80
#define W_STORE 0x100           /* its own stores put values where its kept calls had not put them */
#define W_SPREAD 0x200          /* one of the calls kept at growing distances through the run */
#define W_HCALL 0x400           /* run by the controller's "call" */

static char run_name[64] = "run";
static u32 seq;
static int last_mode = -1;      /* MODE_PACK, -1 before the first look */
static u32 a_main_mode, a_sub_mode, a_frame_count, a_vblank_count, a_mode_data;
static u32 heap_lo = 0x0c618000u;

/* ---- the shadow stack ---- */

#define STACK_MAX 2048
static obs_frame stack[STACK_MAX + 2];      /* stack[0] is a floor that is never popped */
static int sp_n;
static int cur_lvl;
static u32 obs_entered;                     /* the entry the last call or jump went to */
static u32 hw_reads;                        /* reads outside RAM and calls out of the image, so far */
static u32 pl_reads;                        /* reads of planted values (mem.h) in slow-path memory, so far */
static u32 exc_count;                       /* exceptions and interrupts taken, so far */
static u64 stack_overflows, edge_overflows;
static routine *harness_top;                /* the routine a "call" of the controller is about to enter */

/* ---- what is known of each routine's arguments (tools/gen_tables.py) ---- */

#define ARG_MAX 12
#define LOC_STACK 0x40
#define LOC_FR 0x20
typedef struct aspec {
    u8 known, n, variadic, nstack;
    u8 imask, fmask;            /* which of r4-r7 and of fr4-fr11 carry declared parameters */
    u8 cls[ARG_MAX];            /* i p f d q s */
    u8 loc[ARG_MAX];            /* 4-7: r4-r7; LOC_FR + n: fr n; LOC_STACK + k: stack word k */
    u16 psz[ARG_MAX];           /* bytes a pointer parameter points at (0: not known) */
    u8 ret;                     /* v i p f d q s, 0 when not known */
    u16 rsz;
} aspec;
static aspec *specs;

/* ---- per routine: what its kept calls already show ---- */

#define SITE_N 64
#define MODE_N 8
#define STH_N 12                /* classes of own stores remembered per routine */
#define K_SPREAD 24             /* calls kept for being far into the run */
#define K_NORET 2
#define ARGH_N 8
#define RETV_N 8
#define PATH_N 24
#define K_RET 6
#define K_PATH 16
#define LATE_MAX 12
#define HUNT_LEN 12             /* calls recorded in full after one that showed something while it was not */
#define HUNT_MAX 480
typedef struct rstat {
    u32 kept, late;
    u8 k_first, k_ret, k_path, k_noret, k_spread, k_store, n_sth;
    u8 n_site, n_mode, n_argh, n_retv, n_path, last_path, last_ret;
    u8 paths_over, rets_over;
    u32 hunt, hunts;
    u64 next_spread;            /* the call number of the next call kept for being far into the run */
    u64 store_gap, store_last;  /* a new class of own stores is kept when this many calls have passed since the last */
    u32 store_quick;            /* a cheap mark of the last call's own stores: the same again is not looked at */
    u32 sth[STH_N];
    u32 path_lost;              /* returns whose path did not fit the table */
    u32 site[SITE_N];
    u8 mode[MODE_N];
    u32 argh[ARGH_N];
    u32 retv[RETV_N], retc[RETV_N];
    u8 retk[RETV_N];
    u64 path[PATH_N];
    u32 pathc[PATH_N];
    u8 pathk[PATH_N];           /* 0: no kept call took it; 1: a late record; 2: a full record */
} rstat;
static rstat *rst;

/* ---- edges: (origin instruction, target, kind) -> count ---- */

#define EDGE_SLOTS (1u << 18)
#define EK_INTO 0x10            /* into the body of a routine, not its entry */
#define EK_OUT  0x20            /* out of the image */
#define EK_SW   0x40            /* a computed jump inside a routine (a switch): from the jump to the case */
typedef struct edge {
    u32 from, to;
    u64 n;
    u8 kind, used;
} edge;
static edge *edges;
static u32 edges_used;

/* ---- captures ---- */

#define PEEK_N 12
#define PEEK_BYTES 256
#define CALLEE_N 64
#define HWL_N 12
#define CL_N 40                 /* direct callees kept with their arguments and results, per record */
#define RD_N 32                 /* globals whose first value read is kept, per record */
#define WO_N 64                 /* own stores kept one by one, in order, per record */
#define OWN_MAX 65536           /* own stores a record's lists are made from */
#define SNAP_N 16
typedef struct peek {
    u32 addr;
    u16 len;
    u8 slot;                    /* 4-7: r4-r7; 16 + k: stack word k */
    u8 data[PEEK_BYTES];
} peek;
typedef struct callee {
    u32 key;                    /* routine index, or the address for a call out of the image */
    u32 n;
    u8 kind;
} callee;
typedef struct strbuf {
    char *s;
    size_t n, cap;
} strbuf;
typedef struct clrec {
    u32 ridx;
    u32 a[4], f[2];
    u32 r0, f0;
    u8 kind, done;
} clrec;
typedef struct rdrec {
    u32 addr, val;
    u8 size, planted;
} rdrec;
typedef struct ownrec {
    u32 addr, old, val;
    u32 size;
} ownrec;
typedef struct obs_capture {
    struct obs_capture *next;
    u32 why;
    u32 r[16], fr[16], xf[16], fpscr;
    u32 stk[16];
    int nstk;
    peek pk[PEEK_N];
    int npk;
    callee cal[CALLEE_N];
    int ncal;
    u32 cal_more;
    u32 hwr[HWL_N][2], hww[HWL_N][2], hwc[HWL_N][3];
    int nhwr, nhww, nhwc;
    u32 plr[HWL_N][2];
    int nplr;
    clrec cl[CL_N];
    int ncl;
    u32 cl_more;
    rdrec rd[RD_N];
    int nrd, rd_more;
    ownrec *own;                        /* its own stores, in order (kept across uses of the capture) */
    u32 nown, own_cap, own_lost;
    u32 sn[SNAP_N];                     /* the watched records' contents at the entry, as ids */
    u32 snmask;
    u32 jr_start;
    int capped;
    strbuf w_own, w_cal, w_host;        /* the write lists, once made */
    u32 ws_own[3], ws_cal[3], ws_host[3];   /* stores, bytes, ranges */
    int w_done;
    u32 exc0;
    int mode;
    u32 fc, vb, vf;
    u64 cycles;
} obs_capture;
static obs_capture *cap_free;
static int capstack[STACK_MAX + 2];        /* stack indices of the frames being captured */
static int capstack_n, cap_lo;             /* captures below cap_lo have had their write list cut */

/* ---- the store journal ---- */

#define JR_RING (1u << 16)
#define JR_HOST 0xffffu
#define STACK_SPAN 0x10000u
typedef struct jent {
    u32 addr, old;
    u16 depth;
    u8 size, lvl;
} jent;
static jent *jr;
static u32 jr_head, jr_outer_start, jr_sp;

/* ---- game state ---- */

static void state_addrs(void)
{
    if (a_main_mode)
        return;
    a_main_mode = global_addr("main_mode_no");
    a_sub_mode = global_addr("sub_mode_no");
    a_frame_count = global_addr("game_frame_count");
    a_vblank_count = global_addr("vblank_count");
    a_mode_data = global_addr("mode_data");
    if (global_addr("main_heap_area"))
        heap_lo = global_addr("main_heap_area");
}

/* recomp (test mode): the other program runs now; its globals are elsewhere
 * (the test program has no mode words: the game's modes read 0 there) */
void obs_program_changed(void)
{
    a_main_mode = a_sub_mode = a_frame_count = a_vblank_count = a_mode_data = 0;
}

int game_mode_main(void) { state_addrs(); return a_main_mode ? (int)g_r32(a_main_mode) : 0; }
int game_mode_sub(void) { state_addrs(); return a_sub_mode ? (int)g_r32(a_sub_mode) : 0; }
u32 game_frames(void) { state_addrs(); return a_frame_count ? g_r32(a_frame_count) : 0; }
u32 game_vblanks(void) { state_addrs(); return a_vblank_count ? g_r32(a_vblank_count) : 0; }

const char *game_mode_name(int m, int s, char *buf, size_t n)
{
    state_addrs();
    char a[64] = "", b[64] = "";
    if (a_mode_data && m >= 0 && m < 8) {
        u32 base = a_mode_data + (u32)m * 0xcc;
        g_str(g_r32(base + 4), a, sizeof a);
        if (s >= 0 && s < 16)
            g_str(g_r32(base + 12 + (u32)s * 12 + 4), b, sizeof b);
    }
    if (a[0] && b[0])
        snprintf(buf, n, "%s.%s", a, b);
    else if (a[0])
        snprintf(buf, n, "%s.%d", a, s);
    else
        snprintf(buf, n, "%d.%d", m, s);
    return buf;
}

/* ---- small helpers ---- */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20 || ch >= 0x7f)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static void json_float(FILE *f, u32 bits)
{
    float v;
    memcpy(&v, &bits, 4);
    if (isfinite(v))
        fprintf(f, "%.9g", (double)v);
    else
        fputs("null", f);
}

static void put_hex(FILE *f, const u8 *p, u32 n)
{
    static const char digits[] = "0123456789abcdef";
    char buf[2 * PEEK_BYTES + 1];
    if (n > PEEK_BYTES)
        n = PEEK_BYTES;
    for (u32 i = 0; i < n; i++) {
        buf[2 * i] = digits[p[i] >> 4];
        buf[2 * i + 1] = digits[p[i] & 15];
    }
    fwrite(buf, 1, 2 * n, f);
}

static double vtime(u64 cycles)
{
    return (double)cycles / (double)CPU_HZ;
}

static inline u32 phys(u32 a)
{
    return a & 0x1fffffffu;
}

/* a value that can be read as a pointer into the machine's memory (the
 * low 2 MiB, the synthetic ROM, are left out: small numbers land there) */
static int ptr_ok(u32 v)
{
    u32 top = v >> 29;
    if (!(top == 0 || top == 4 || top == 5))
        return 0;
    if (phys(v) < 0x00200000u)
        return 0;
    return mem_page_ram[v >> 16] != NULL;
}

static u32 read_bytes(u32 a, u8 *dst, u32 n)
{
    u32 got = 0;
    while (got < n) {
        u8 *p = mem_ptr(a + got);
        if (!p)
            break;
        u32 chunk = 0x10000 - ((a + got) & 0xffff);
        if (chunk > n - got)
            chunk = n - got;
        memcpy(dst + got, p, chunk);
        got += chunk;
    }
    return got;
}

static void sb_put(strbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_put(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int need = vsnprintf(b->s ? b->s + b->n : NULL, b->s ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (b->s && (size_t)need < b->cap - b->n) {
            b->n += (size_t)need;
            return;
        }
        b->cap = (b->cap ? b->cap * 2 : 256) + (size_t)need;
        b->s = realloc(b->s, b->cap);
    }
}

/* ---- argument descriptions ---- */

static void spec_parse(aspec *sp, const char *text)
{
    memset(sp, 0, sizeof *sp);
    if (!text)
        return;
    sp->known = 1;
    int ireg = 4, freg = 4, stk = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (*p == '>') {
            p++;
            sp->ret = (u8)*p;
            if (*p)
                p++;
            sp->rsz = (u16)strtoul(p, (char **)&p, 10);
            if (*p == '!') {
                sp->variadic = 1;
                p++;
            }
            continue;
        }
        char cls = *p++;
        u32 size = strtoul(p, (char **)&p, 10);
        if (sp->n >= ARG_MAX)
            continue;
        int i = sp->n++;
        sp->cls[i] = (u8)cls;
        sp->psz[i] = (u16)(size > 0xffff ? 0xffff : size);
        switch (cls) {
        case 'f':
            if (freg <= 11) {
                sp->loc[i] = (u8)(LOC_FR + freg);
                sp->fmask |= (u8)(1u << (freg - 4));
                freg++;
            } else {
                sp->loc[i] = (u8)(LOC_STACK + stk++);
            }
            break;
        case 'd':
            freg = (freg + 1) & ~1;
            if (freg <= 10) {
                sp->loc[i] = (u8)(LOC_FR + freg);
                sp->fmask |= (u8)(3u << (freg - 4));
                freg += 2;
            } else {
                sp->loc[i] = (u8)(LOC_STACK + stk);
                stk += 2;
            }
            break;
        case 'q':
            if (ireg <= 6) {
                sp->loc[i] = (u8)ireg;
                sp->imask |= (u8)(3u << (ireg - 4));
                ireg += 2;
            } else {
                ireg = 8;
                sp->loc[i] = (u8)(LOC_STACK + stk);
                stk += 2;
            }
            break;
        case 's': {
            /* a structure by value: in r4-r7 when all of it fits into the
             * registers that are left, else all of it on the stack (and the
             * registers stay free for what follows) */
            int words = (int)((size + 3) / 4);
            if (!words)
                words = 1;
            if (ireg + words - 1 <= 7) {
                sp->loc[i] = (u8)ireg;
                for (int w = 0; w < words; w++)
                    sp->imask |= (u8)(1u << (ireg + w - 4));
                ireg += words;
            } else {
                sp->loc[i] = (u8)(LOC_STACK + stk);
                stk += words;
            }
            break;
        }
        default:
            if (ireg <= 7) {
                sp->loc[i] = (u8)ireg;
                sp->imask |= (u8)(1u << (ireg - 4));
                ireg++;
            } else {
                sp->loc[i] = (u8)(LOC_STACK + stk++);
            }
            break;
        }
    }
    sp->nstack = (u8)(stk > 16 ? 16 : stk);
}

/* ---- edges ---- */

static void edge_count(u32 from, u32 to, int kind)
{
    u32 h = ((from * 0x9e3779b1u) ^ (to * 0x85ebca6bu) ^ ((u32)kind * 0xc2b2ae35u)) >> 14;
    for (;;) {
        edge *e = &edges[h & (EDGE_SLOTS - 1)];
        if (e->used) {
            if (e->from == from && e->to == to && e->kind == (u8)kind) {
                e->n++;
                return;
            }
            h++;
            continue;
        }
        if (edges_used >= EDGE_SLOTS / 4 * 3) {
            edge_overflows++;
            return;
        }
        e->used = 1;
        e->from = from;
        e->to = to;
        e->kind = (u8)kind;
        e->n = 1;
        edges_used++;
        return;
    }
}

/* ---- the journal and the captures ---- */

static int jr_ring_on;           /* some recorded call's list of its callees' stores is still being filled */

/* after the top of the shadow stack changed: stores are offered to the
 * observer while the journal is being filled or the running routine itself
 * is being recorded (its own stores are kept whatever the journal does) */
static inline void rd_update(void)
{
    obs_capture *cap = obs_top->cap;
    obs_rd_on = cap != NULL && cap->nrd < RD_N;
    obs_jr_on = jr_ring_on || cap != NULL;
}

static void jr_update(void)
{
    jr_ring_on = 0;
    if (capstack_n > 0) {
        obs_frame *in = &stack[capstack[capstack_n - 1]];
        jr_sp = phys(in->sp);
        if (cap_lo < capstack_n) {
            jr_outer_start = stack[capstack[cap_lo]].cap->jr_start;
            jr_ring_on = in->lvl == cur_lvl;
        }
    }
    rd_update();
}

typedef struct wbyte {
    u32 addr, seq;
    u8 old;
} wbyte;
static wbyte *wb_own, *wb_cal, *wb_host;
static u32 wb_cap;

static int wbyte_cmp(const void *a, const void *b)
{
    const wbyte *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void wb_room(u32 bytes)
{
    if (wb_cap < bytes + 64) {
        wb_cap = bytes + 64;
        wb_own = realloc(wb_own, wb_cap * sizeof *wb_own);
        wb_cal = realloc(wb_cal, wb_cap * sizeof *wb_cal);
        wb_host = realloc(wb_host, wb_cap * sizeof *wb_host);
    }
}

/* a range that lies in the game's own data (not on the heap, not on a stack) */
static int in_globals(u32 addr)
{
    return addr >= RAM_BASE && addr < heap_lo;
}

/* One class of stores as a list of byte ranges: [address, length, before,
 * after]. "before" is the byte before the first store of the class to it,
 * "after" the byte as it is now. At most max_ranges ranges are listed, each
 * with at most per_range bytes and all together at most budget bytes; when
 * there are more ranges than that and prefer_globals is set, the ranges in
 * the game's own data are listed before any on the heap. stats: stores,
 * bytes, ranges (all exact, whatever is listed). */
static void write_list(strbuf *out, u32 *stats, wbyte *w, u32 n, u32 stores, u32 max_ranges, u32 per_range,
                       u32 budget, int prefer_globals)
{
    static u32 *rs, *rl;        /* range starts (indices into w) and lengths */
    static u32 r_cap;
    qsort(w, n, sizeof *w, wbyte_cmp);
    u32 bytes = 0, ranges = 0, in_g = 0;
    for (u32 i = 0; i < n;) {
        u32 start = w[i].addr, len = 0, first = i;
        while (i < n && w[i].addr == start + len) {
            len++;
            u32 a = w[i].addr;
            while (i < n && w[i].addr == a)     /* later stores to the same byte */
                i++;
        }
        if (ranges >= r_cap) {
            r_cap = r_cap ? r_cap * 2 : 256;
            rs = realloc(rs, r_cap * sizeof *rs);
            rl = realloc(rl, r_cap * sizeof *rl);
        }
        rs[ranges] = first;
        rl[ranges] = len;
        ranges++;
        bytes += len;
        in_g += in_globals(start);
    }
    /* which ranges are listed when there are too many: the game's own data first */
    u32 take_g = ranges, take_o = ranges;
    if (ranges > max_ranges) {
        if (prefer_globals) {
            take_g = in_g < max_ranges ? in_g : max_ranges;
            take_o = max_ranges - take_g;
        } else {
            take_g = take_o = 0;        /* (the first max_ranges by address: counted below) */
        }
    }
    out->n = 0;
    sb_put(out, "[");
    u32 listed = 0;
    for (u32 r = 0; r < ranges; r++) {
        u32 start = w[rs[r]].addr, len = rl[r];
        if (ranges > max_ranges) {
            if (prefer_globals) {
                u32 *left = in_globals(start) ? &take_g : &take_o;
                if (!*left)
                    continue;
                (*left)--;
            } else if (listed >= max_ranges) {
                break;
            }
        }
        static const char digits[] = "0123456789abcdef";
        char hb[2 * PEEK_BYTES + 1], ha[2 * PEEK_BYTES + 1];
        u32 shown = len < per_range ? len : per_range;
        if (shown > budget)
            shown = budget;
        if (shown > PEEK_BYTES)
            shown = PEEK_BYTES;
        budget -= shown;
        /* the first store to each byte of the range gives its "before" */
        u32 i = rs[r];
        for (u32 k = 0; k < shown; k++) {
            u8 before = w[i].old, after = (u8)g_r8(start + k);
            u32 a = w[i].addr;
            while (i < n && w[i].addr == a)
                i++;
            hb[2 * k] = digits[before >> 4];
            hb[2 * k + 1] = digits[before & 15];
            ha[2 * k] = digits[after >> 4];
            ha[2 * k + 1] = digits[after & 15];
        }
        hb[2 * shown] = ha[2 * shown] = 0;
        sb_put(out, "%s[\"%08x\",%u,\"%s\",\"%s\"]", listed ? "," : "", start, len, hb, ha);
        listed++;
    }
    sb_put(out, "]");
    stats[0] = stores;
    stats[1] = bytes;
    stats[2] = ranges;
}

/* turns the journal span of a captured frame into the lists of what its
 * callees and host code stored (its own stores are kept apart, in the
 * capture itself, and never cut: own_list_make) */
static void writes_make(int fi)
{
    obs_frame *f = &stack[fi];
    obs_capture *cap = f->cap;
    u32 n = jr_head - cap->jr_start;
    if (n > JR_RING)
        n = JR_RING;
    wb_room(n * 4);
    u32 nc = 0, nh = 0, sc = 0, sh = 0;
    u32 fsp = phys(f->sp);
    for (u32 k = 0; k < n; k++) {
        jent *e = &jr[(cap->jr_start + k) & (JR_RING - 1)];
        wbyte *dst;
        u32 *cnt;
        if (e->depth == JR_HOST) {
            if ((e->lvl ^ f->lvl) & LVL_SANDBOX)
                continue;                   /* a store made for a sandbox is not the game's call's, and the other way round */
            dst = wb_host, cnt = &nh;
            sh++;
        } else {
            if (e->lvl != f->lvl)
                continue;                   /* an interrupt's stores are not this call's */
            if (fsp - e->addr - 1 < STACK_SPAN)
                continue;                   /* below the frame's entry stack pointer: its own locals */
            if (e->depth == (u16)fi)
                continue;                   /* its own: in the capture */
            dst = wb_cal, cnt = &nc;
            sc++;
        }
        for (u32 b = 0; b < e->size; b++) {
            dst[*cnt].addr = e->addr + b;
            dst[*cnt].seq = k;
            dst[*cnt].old = (u8)(e->old >> (8 * b));
            (*cnt)++;
        }
    }
    write_list(&cap->w_cal, cap->ws_cal, wb_cal, nc, sc, 32, 64, 1280, 1);
    write_list(&cap->w_host, cap->ws_host, wb_host, nh, sh, 8, 64, 512, 0);
    cap->w_done = 1;
}

/* the routine's own stores as ranges (w) */
static void own_list_make(obs_capture *cap)
{
    u32 nb = 0;
    wb_room(cap->nown * 4);
    for (u32 k = 0; k < cap->nown; k++) {
        ownrec *e = &cap->own[k];
        for (u32 b = 0; b < e->size; b++) {
            wb_own[nb].addr = e->addr + b;
            wb_own[nb].seq = k;
            wb_own[nb].old = (u8)(e->old >> (8 * b));
            nb++;
        }
    }
    write_list(&cap->w_own, cap->ws_own, wb_own, nb, cap->nown + cap->own_lost, 160, 256, 3072, 1);
}

static void own_put(obs_capture *cap, u32 addr, u32 old, u32 val, u32 size)
{
    if (cap->nown >= cap->own_cap) {
        if (cap->own_cap >= OWN_MAX) {
            cap->own_lost++;
            return;
        }
        cap->own_cap = cap->own_cap ? cap->own_cap * 4 : 64;
        cap->own = realloc(cap->own, cap->own_cap * sizeof *cap->own);
    }
    ownrec *e = &cap->own[cap->nown++];
    e->addr = addr;
    e->old = old;
    e->val = val;
    e->size = size;
}

static inline void jr_put(u32 addr, u32 old, u32 size, u32 depth)
{
    while (cap_lo < capstack_n && jr_head - jr_outer_start >= (u32)obs_rules.writes) {
        /* the outermost capture has its fill: its lists of callees' and
         * host stores are made now, from what the journal holds, and
         * marked as cut (its own stores go on being kept) */
        writes_make(capstack[cap_lo]);
        stack[capstack[cap_lo]].cap->capped = 1;
        cap_lo++;
        jr_update();
    }
    if (cap_lo >= capstack_n)
        return;
    jent *e = &jr[jr_head++ & (JR_RING - 1)];
    e->addr = addr;
    e->old = old;
    e->size = (u8)size;
    e->depth = (u16)depth;
    e->lvl = (u8)cur_lvl;
}

/* a store of the running routine outside its own stack frame, while some
 * call is being recorded (the interpreter's OBS_STORE has counted it) */
void obs_store(sh4 *c, u32 addr, u32 n, u32 val)
{
    (void)c;
    u32 p = phys(addr);
    u32 old = n == 4 ? g_r32(addr) : n == 2 ? g_r16(addr) : g_r8(addr);
    obs_capture *cap = obs_top->cap;
    if (cap)
        own_put(cap, p, old, val, n);
    if (!jr_ring_on || jr_sp - p - 1 < STACK_SPAN)
        return;                         /* (the innermost recorded routine's own stack frame) */
    jr_put(p, old, n, (u32)sp_n);
}

/* the same for a store that did not come through OBS_STORE (RAM on the slow path) */
static void store_note(sh4 *c, u32 addr, u32 n, u32 val)
{
    obs_frame *t = obs_top;
    if (t->spp - 1 - phys(addr) < STACK_SPAN)
        return;
    u32 k = t->own_n++;
    if (k < LW_N) {
        t->lw[k].addr = addr;
        t->lw[k].old = n == 4 ? g_r32(addr) : n == 2 ? g_r16(addr) : g_r8(addr);
        t->lw[k].val = val;
        t->lw[k].size = n;
    }
    if (obs_jr_on)
        obs_store(c, addr, n, val);
}

void obs_store_block(sh4 *c, u32 addr, const u32 *words)
{
    addr &= ~31u;
    for (u32 k = 0; k < 8; k++)
        store_note(c, addr + 4 * k, 4, words[k]);
    if (mem_plant_near(addr))
        mem_unplant(addr, 32);
}

/* a store by host code (a hardware model, a BIOS service, the controller) */
static void host_store(u32 addr, u32 n)
{
    if (cap_lo >= capstack_n)
        return;
    u32 p = phys(addr);
    while (n) {
        u32 size = n >= 4 && !(p & 3) ? 4 : 1;
        u32 old = size == 4 ? g_r32(p) : g_r8(p);
        jr_put(p, old, size, JR_HOST);
        if (cap_lo >= capstack_n)
            return;
        p += size;
        n -= size;
    }
}

/* a read of RAM by the running routine while it is being recorded: the
 * first value it reads from each place in the game's own data */
void obs_read(u32 addr, u32 val, u32 n)
{
    u32 p = phys(addr);
    if (p - RAM_BASE >= heap_lo - RAM_BASE)
        return;                         /* the heap, a stack, other memory */
    u32 slot = (p - IMAGE_BASE) >> 1;
    if (slot < disp_slots && obs_slot_routine[slot])
        return;                         /* code, or a routine's own constants */
    obs_capture *cap = obs_top->cap;
    if (!cap)
        return;
    for (int i = 0; i < cap->nrd; i++)
        if (cap->rd[i].addr == p)
            return;
    if (cap->nrd >= RD_N) {
        cap->rd_more = 1;
        obs_rd_on = 0;
        return;
    }
    rdrec *r = &cap->rd[cap->nrd++];
    r->addr = p;
    r->val = val;
    r->size = (u8)n;
    r->planted = (u8)(mem_plant_near(p) && mem_planted(p, n));
    if (cap->nrd >= RD_N) {
        cap->rd_more = 1;
        obs_rd_on = 0;
    }
}

void obs_io_read(sh4 *c, u32 addr, u32 val, int size)
{
    (void)c;
    if (mem_page_ram[addr >> 16]) {
        /* RAM on the slow path: low RAM and the sound driver's mailbox,
         * where the models plant values, and whatever is being watched */
        if (mem_plant_near(addr) && mem_planted(addr, (u32)size)) {
            pl_reads++;
            obs_capture *cp = obs_top->cap;
            if (cp) {
                int i = 0;
                while (i < cp->nplr && cp->plr[i][0] != phys(addr))
                    i++;
                if (i == cp->nplr && cp->nplr < HWL_N) {
                    cp->plr[i][0] = phys(addr);
                    cp->plr[i][1] = val;
                    cp->nplr++;
                }
            }
        }
        if (obs_rd_on)
            obs_read(addr, val, (u32)size);
        return;
    }
    if (addr >= 0xe0000000u && addr < 0xe4000000u)
        return;                         /* the store queues */
    hw_reads++;
    obs_capture *cap = obs_top->cap;
    if (cap) {
        for (int i = 0; i < cap->nhwr; i++)
            if (cap->hwr[i][0] == addr) {
                cap->hwr[i][1] = val;
                return;
            }
        if (cap->nhwr < HWL_N) {
            cap->hwr[cap->nhwr][0] = addr;
            cap->hwr[cap->nhwr][1] = val;
            cap->nhwr++;
        }
    }
}

void obs_io_write(sh4 *c, u32 addr, u32 val, int size)
{
    if (mem_page_ram[addr >> 16]) {
        store_note(c, addr, (u32)size, val);
        return;
    }
    if (addr >= 0xe0000000u && addr < 0xe4000000u)
        return;
    obs_capture *cap = obs_top->cap;
    if (cap) {
        int i = 0;
        while (i < cap->nhww && cap->hww[i][0] != addr)
            i++;
        if (i < cap->nhww) {
            cap->hww[i][1] = val;
        } else if (cap->nhww < HWL_N) {
            cap->hww[cap->nhww][0] = addr;
            cap->hww[cap->nhww][1] = val;
            cap->nhww++;
        }
    }
    /* the calls being recorded further up see it too, with the routine
     * that made it: a register written through a shared helper is the
     * caller's doing */
    for (int k = capstack_n - 1; k >= 0; k--) {
        obs_frame *f = &stack[capstack[k]];
        if (f == obs_top || f->lvl != cur_lvl)
            continue;
        obs_capture *up = f->cap;
        int i = 0;
        while (i < up->nhwc && up->hwc[i][0] != addr)
            i++;
        if (i == up->nhwc) {
            if (up->nhwc >= HWL_N)
                continue;
            up->nhwc++;
            up->hwc[i][0] = addr;
        }
        up->hwc[i][1] = val;
        up->hwc[i][2] = obs_top->ridx;
    }
}

void obs_computed(sh4 *c, u32 target)
{
    /* a jmp or braf that stays inside its routine is a switch: each target
     * is counted, and is part of the call's path */
    (void)c;
    u32 fs = disp_slot(obs_pc), ts = disp_slot(target);
    if (fs >= disp_slots || ts >= disp_slots || !obs_slot_routine[fs] || obs_slot_routine[fs] != obs_slot_routine[ts] ||
        (obs_map[ts] & OM_ENTRY))
        return;
    edge_count(obs_pc, phys(target), DISP_JUMP | EK_SW);
    obs_top->sig |= 1ull << (((ts << 1) * 0x9e3779b1u + 0x7f4a7c15u) >> 26);
}

static void callee_add(obs_capture *cap, u32 key, int kind)
{
    for (int i = 0; i < cap->ncal; i++)
        if (cap->cal[i].key == key && cap->cal[i].kind == (u8)kind) {
            cap->cal[i].n++;
            return;
        }
    if (cap->ncal < CALLEE_N) {
        cap->cal[cap->ncal].key = key;
        cap->cal[cap->ncal].kind = (u8)kind;
        cap->cal[cap->ncal].n = 1;
        cap->ncal++;
    } else {
        cap->cal_more++;
    }
}

static void peek_add(obs_capture *cap, int slot, u32 v, u32 len)
{
    if (cap->npk >= PEEK_N || !ptr_ok(v))
        return;
    if (disp_slot(v) < disp_slots && obs_slot_routine[disp_slot(v)])
        return;                         /* a pointer to code: the instructions say nothing */
    if (len > (u32)obs_rules.peek_max)
        len = (u32)obs_rules.peek_max;
    if (len > PEEK_BYTES)
        len = PEEK_BYTES;
    peek *p = &cap->pk[cap->npk];
    p->addr = v;
    p->slot = (u8)slot;
    p->len = (u16)read_bytes(v, p->data, len);
    if (p->len)
        cap->npk++;
}

static u32 peek_len(u8 cls, u16 psz)
{
    if (cls == 'p' && psz > 8)
        return psz;
    return (u32)obs_rules.peek;
}

/* ---- watched records (the controller's "snap") ----
 *
 * A range of memory named by the script is kept with every full record of
 * the routines its pattern names: as it is at the entry ("sn") and, when it
 * changed, at the return ("snr"). The bytes themselves go into the log
 * once per distinct content, as a "snap" line with a number; the records
 * carry the numbers. */

typedef struct snapdef {
    char name[48];
    char pattern[80];
    u32 addr, len;
    int deref;                  /* addr holds a pointer to the range */
} snapdef;
static snapdef snaps[SNAP_N];
static int snap_n;
static u16 *snap_mask;          /* per routine: which snaps go with its records */
#define SNAP_SEEN (1u << 16)
static u64 *snap_seen;          /* contents already written: hash -> id (open addressing) */
static u32 *snap_seen_id;
static u32 snap_ids;

static int glob_match(const char *pat, const char *text)
{
    if (!*pat)
        return !*text;
    if (*pat == '*')
        return glob_match(pat + 1, text) || (*text && glob_match(pat, text + 1));
    return *text && (*pat == '?' || *pat == *text) && glob_match(pat + 1, text + 1);
}

static void snap_masks(void)
{
    if (!snap_mask)
        snap_mask = calloc((size_t)routine_count, sizeof *snap_mask);
    for (int i = 0; i < routine_count; i++) {
        char key[400];
        snprintf(key, sizeof key, "%s/%s", routines[i].file ? routines[i].file : "", routines[i].name);
        u16 m = 0;
        for (int k = 0; k < snap_n; k++)
            if (glob_match(snaps[k].pattern, key))
                m |= (u16)(1u << k);
        snap_mask[i] = m;
    }
}

int obs_snap_add(const char *name, u32 addr, int deref, u32 len, const char *pattern)
{
    if (snap_n >= SNAP_N || !len)
        return -1;
    snapdef *d = &snaps[snap_n];
    snprintf(d->name, sizeof d->name, "%s", name);
    if (strchr(pattern, '/') || strchr(pattern, '*'))
        snprintf(d->pattern, sizeof d->pattern, "%s", pattern);
    else
        snprintf(d->pattern, sizeof d->pattern, "*/%s", pattern);   /* a bare name: that routine */
    d->addr = addr;
    d->len = len > 0x2000 ? 0x2000 : len;
    d->deref = deref;
    snap_n++;
    if (obs_on)
        snap_masks();
    return 0;
}

/* the contents of one watched record as a number, written to the log when new; 0: not readable */
static u32 snap_id(int k)
{
    static u8 buf[0x2000];
    snapdef *d = &snaps[k];
    u32 a = d->deref ? g_r32(d->addr) : d->addr;
    if (!a || !mem_ptr(a) || read_bytes(a, buf, d->len) != d->len)
        return 0;
    u64 h = 1469598103934665603ull ^ (u64)(k + 1) * 0x9e3779b97f4a7c15ull ^ a;
    for (u32 i = 0; i < d->len; i++)
        h = (h ^ buf[i]) * 1099511628211ull;
    if (!h)
        h = 1;
    if (!snap_seen) {
        snap_seen = calloc(SNAP_SEEN, sizeof *snap_seen);
        snap_seen_id = calloc(SNAP_SEEN, sizeof *snap_seen_id);
    }
    u32 i = (u32)(h >> 40) & (SNAP_SEEN - 1);
    for (u32 tries = 0; tries < SNAP_SEEN / 2; tries++, i = (i + 1) & (SNAP_SEEN - 1)) {
        if (snap_seen[i] == h)
            return snap_seen_id[i];
        if (!snap_seen[i]) {
            snap_seen[i] = h;
            break;
        }
    }
    u32 id = ++snap_ids;
    snap_seen_id[i] = id;       /* (a full table: the slot is taken over) */
    snap_seen[i] = h;
    if (obs_log) {
        static const char digits[] = "0123456789abcdef";
        static char hex[2 * 0x2000 + 1];
        for (u32 b = 0; b < d->len; b++) {
            hex[2 * b] = digits[buf[b] >> 4];
            hex[2 * b + 1] = digits[buf[b] & 15];
        }
        hex[2 * d->len] = 0;
        fprintf(obs_log, "{\"id\":%u,\"name\":\"%s\",\"addr\":\"%08x\",\"len\":%u,\"bytes\":\"%s\",\"vf\":%u,"
                "\"k\":\"snap\"}\n", id, d->name, a, d->len, hex, machine_frame);
    }
    return id;
}

static void snaps_take(obs_capture *cap, u32 ridx, u32 *ids, int entry)
{
    u16 m = snap_mask ? snap_mask[ridx] : 0;
    if (entry)
        cap->snmask = m;
    for (int k = 0; m && k < snap_n; k++)
        if (m & (1u << k))
            ids[k] = snap_id(k);
}

static void capture_begin(sh4 *c, obs_frame *f, routine *r, u32 why)
{
    obs_capture *cap = cap_free;
    if (cap)
        cap_free = cap->next;
    else
        cap = calloc(1, sizeof *cap);
    strbuf so = cap->w_own, sc = cap->w_cal, sh = cap->w_host;
    memset(cap, 0, offsetof(obs_capture, pk));
    cap->npk = cap->ncal = cap->nhwr = cap->nhww = cap->nhwc = cap->nplr = 0;
    cap->cal_more = 0;
    cap->ncl = cap->nrd = cap->rd_more = 0;
    cap->cl_more = 0;
    cap->nown = cap->own_lost = 0;
    cap->snmask = 0;
    cap->capped = cap->w_done = 0;
    cap->w_own = so, cap->w_cal = sc, cap->w_host = sh;
    cap->why = why;
    memcpy(cap->r, c->r, sizeof cap->r);
    for (int i = 0; i < 16; i++) {
        cap->fr[i] = c->fr[i].u;
        cap->xf[i] = c->xf[i].u;
    }
    cap->fpscr = c->fpscr;
    obs_mode_check(c);
    cap->mode = last_mode;
    f->mode = (u16)last_mode;
    cap->fc = game_frames();
    cap->vb = game_vblanks();
    cap->vf = machine_frame;
    cap->cycles = c->cycles;
    cap->exc0 = exc_count;

    aspec *sp = &specs[f->ridx];
    u32 sp15 = c->r[15];
    cap->nstk = 0;
    if (mem_ptr(sp15)) {
        int want = sp->variadic ? 8 : sp->nstack > 4 ? sp->nstack : 4;
        for (int i = 0; i < want && mem_ptr(sp15 + 4u * (u32)i); i++)
            cap->stk[cap->nstk++] = g_r32(sp15 + 4u * (u32)i);
    }
    /* the bytes behind pointers: declared pointer parameters with the size
     * of what they point at, and any other argument register or declared
     * stack word that holds an address in the machine's memory */
    u8 done = 0;
    for (int i = 0; i < sp->n; i++) {
        u8 loc = sp->loc[i];
        if (sp->cls[i] != 'p' && sp->cls[i] != 'i')
            continue;
        if (loc >= 4 && loc <= 7) {
            peek_add(cap, loc, c->r[loc], peek_len(sp->cls[i], sp->psz[i]));
            done |= (u8)(1u << (loc - 4));
        } else if (loc >= LOC_STACK && loc - LOC_STACK < cap->nstk) {
            peek_add(cap, 16 + (loc - LOC_STACK), cap->stk[loc - LOC_STACK], peek_len(sp->cls[i], sp->psz[i]));
        }
    }
    for (int i = 4; i <= 7; i++)
        if (!(done & (1u << (i - 4))))
            peek_add(cap, i, c->r[i], (u32)obs_rules.peek);
    if (sp->variadic)
        for (int i = sp->nstack; i < cap->nstk; i++)
            peek_add(cap, 16 + i, cap->stk[i], (u32)obs_rules.peek);

    /* the stores it made before it was known to be worth recording (none, when recording starts at its entry) */
    for (u32 k = 0; k < f->own_n && k < LW_N; k++)
        own_put(cap, phys(f->lw[k].addr), f->lw[k].old, f->lw[k].val, f->lw[k].size);
    snaps_take(cap, f->ridx, cap->sn, 1);
    cap->jr_start = jr_head;
    f->cap = cap;
    capstack[capstack_n++] = (int)(f - stack);
    jr_update();
    (void)r;
}

static void capture_free(obs_frame *f)
{
    obs_capture *cap = f->cap;
    f->cap = NULL;
    capstack_n--;
    if (cap_lo > capstack_n)
        cap_lo = capstack_n;
    jr_update();
    cap->next = cap_free;
    cap_free = cap;
}

/* ---- log ---- */

int obs_enable(void)
{
    if (obs_on)
        return 0;
    state_addrs();
    obs_map = calloc(disp_slots, 1);
    obs_slot_routine = calloc(disp_slots, sizeof *obs_slot_routine);
    specs = calloc((size_t)routine_count, sizeof *specs);
    rst = calloc((size_t)routine_count, sizeof *rst);
    edges = calloc(EDGE_SLOTS, sizeof *edges);
    jr = calloc(JR_RING, sizeof *jr);
    if (!obs_map || !obs_slot_routine || !specs || !rst || !edges || !jr)
        return -1;
    for (int i = 0; i < routine_count; i++) {
        u32 s = disp_slot(routines[i].addr);
        u32 e = i + 1 < routine_count ? disp_slot(routines[i + 1].addr)
                                      : disp_slot((routines[i].addr + routines[i].size + 0x1f) & ~0x1fu);
        if (s < disp_slots)
            obs_map[s] |= OM_ENTRY;
        for (u32 k = s; k < e && k < disp_slots; k++)
            obs_slot_routine[k] = (u16)(i + 1);
        spec_parse(&specs[i], gen_routines[i].spec);
    }
    if (obs_rules.writes > (int)(JR_RING / 2))
        obs_rules.writes = JR_RING / 2;
    memset(stack, 0, sizeof stack);
    stack[0].ridx = OBS_NONE;
    sp_n = 0;
    obs_top = &stack[0];
    mem_host_store = host_store;
    disp_set_flag_all(DF_LOG, 1);
    obs_on = 1;
    if (snap_n)
        snap_masks();
    return 0;
}

int obs_open(const char *path, const char *name)
{
    if (obs_enable())
        return -1;
    obs_log = fopen(path, "w");
    if (!obs_log) {
        perror(path);
        return -1;
    }
    static char buf[1 << 16];
    setvbuf(obs_log, buf, _IOFBF, sizeof buf);
    /* the globals the frame line carries unless the script names others
     * (names of the decompilation; one that is gone is left out) */
    static const struct { const char *name; int size; } dflt[] = {
        { "input_buttons", 2 }, { "input_presses", 2 }, { "game_flags", 4 }, { "round_number", 1 },
        { "round_end_kind", 1 }, { "round_winner", 1 }, { "rounds_won", 2 }, { "round_time", 4 },
        { "rank_round", 1 }, { "cpu_on_off", 4 }, { "match_stage", 4 }, { "player_chara", 4 },
        { "player_chara+4", 4 }, { "robs+0x10", 2 }, { "robs+0x87c", 2 }
    };
    for (unsigned i = 0; i < sizeof dflt / sizeof dflt[0]; i++)
        obs_frame_global(dflt[i].name, dflt[i].size);
    if (name)
        snprintf(run_name, sizeof run_name, "%s", name);

    FILE *f = obs_log;
    fprintf(f, "{\"run\":");
    json_str(f, run_name);
    fprintf(f, ",\"format\":2,\"functions\":%d,\"skipped\":0,\"resumed\":0,\"rule\":{\"hits\":%d,\"variety\":1,"
            "\"max_hits\":%d,\"mode_quota\":%d,\"ret\":\"all\",\"ret_frames\":0,\"peek\":%d,\"peek_max\":%d,"
            "\"writes\":%d},\"format_m4\":1,\"mode\":\"%d.%d\",\"fc\":%u,\"vb\":%u,\"country\":%d,\"mode_names\":{", routine_count,
            obs_rules.hits, obs_rules.max_hits, obs_rules.hits, obs_rules.peek, obs_rules.peek_max, obs_rules.writes,
            game_mode_main(), game_mode_sub(), game_frames(), game_vblanks(), cfg.country);
    int first = 1;
    for (int m = 0; m < 8 && a_mode_data; m++) {
        u32 base = a_mode_data + (u32)m * 0xcc;
        char mn[64], sn[64];
        g_str(g_r32(base + 4), mn, sizeof mn);
        if (!mn[0])
            continue;
        fprintf(f, "%s\"%d\":", first ? "" : ",", m);
        json_str(f, mn);
        first = 0;
        for (int k = 0; k < 16; k++) {
            u32 e = base + 12 + (u32)k * 12;
            if (!g_r32(e + 4))
                break;
            g_str(g_r32(e + 4), sn, sizeof sn);
            char full[160];
            snprintf(full, sizeof full, "%s.%s", mn, sn);
            fprintf(f, ",\"%d.%d\":", m, k);
            json_str(f, full);
        }
    }
    fprintf(f, "},\"image\":");
    json_str(f, cfg.image ? cfg.image : "");
    fprintf(f, ",\"image_bytes\":%u,\"source\":\"vf4debug harness\",\"k\":\"start\",\"t\":0.0}\n", 0x340000u);
    return 0;
}

void obs_note(const char *what)
{
    if (!obs_log)
        return;
    fprintf(obs_log, "{\"what\":");
    json_str(obs_log, what);
    fprintf(obs_log, ",\"vf\":%u,\"fc\":%u,\"k\":\"note\",\"t\":%.6f}\n", machine_frame, game_frames(),
            vtime(cpu.cycles));
}

void obs_poke(u32 addr, u32 value, int size, const char *text)
{
    if (!obs_log)
        return;
    fprintf(obs_log, "{\"addr\":\"%08x\",\"value\":%u,\"size\":%d,\"text\":", addr, value, size);
    json_str(obs_log, text ? text : "");
    fprintf(obs_log, ",\"mode\":\"%d.%d\",\"vf\":%u,\"fc\":%u,\"k\":\"poke\",\"t\":%.6f}\n", game_mode_main(),
            game_mode_sub(), machine_frame, game_frames(), vtime(cpu.cycles));
}

void obs_mode_check(sh4 *c)
{
    int mode = MODE_PACK(game_mode_main(), game_mode_sub());
    if (mode == last_mode)
        return;
    if (obs_log && last_mode != -1) {
        char name[160];
        game_mode_name(MODE_MAIN(mode), MODE_SUB(mode), name, sizeof name);
        fprintf(obs_log, "{\"from\":\"%d.%d\",\"to\":\"%d.%d\",\"name\":", MODE_MAIN(last_mode), MODE_SUB(last_mode),
                MODE_MAIN(mode), MODE_SUB(mode));
        json_str(obs_log, name);
        fprintf(obs_log, ",\"fc\":%u,\"vf\":%u,\"k\":\"mode\",\"t\":%.6f}\n", game_frames(), machine_frame,
                vtime(c->cycles));
    }
    last_mode = mode;
    machine_mode_changed(c, MODE_MAIN(mode), MODE_SUB(mode));
}

static const char *kind_name(int kind)
{
    switch (kind & 15) {
    case DISP_CALL: return "call";
    case DISP_JUMP: return "jump";
    case DISP_FALL: return "fall";
    case DISP_EXCEPTION: return "exc";
    default: return "?";
    }
}

static void put_why(FILE *f, u32 why)
{
    static const char *const names[] = { "first", "site", "mode", "args", "ret", "path", "trace", "hunt", "stores",
                                         "spread", "hcall" };
    int any = 0;
    fputs(",\"why\":[", f);
    for (int i = 0; i < 11; i++)
        if (why & (1u << i)) {
            fprintf(f, "%s\"%s\"", any ? "," : "", names[i]);
            any = 1;
        }
    fputc(']', f);
}

static void put_slot_key(FILE *f, int slot)
{
    if (slot < 16)
        fprintf(f, "\"%d\":", slot);
    else
        fprintf(f, "\"s%d\":", slot - 16);
}

/* the head both kinds of record share */
static void put_head(FILE *f, obs_frame *fr, routine *r, rstat *rs)
{
    routine *who = routine_containing(fr->pr);
    fprintf(f, "{\"id\":\"%s:%u\",\"h\":%u,\"n\":%u,\"fn\":\"%08x\",\"name\":\"%s\",\"pr\":\"%08x\",", run_name, seq,
            rs->kept + rs->late, fr->n, r->addr, r->name, fr->pr);
    if (who && fr->kind != DISP_EXCEPTION)
        fprintf(f, "\"caller\":\"%s\",\"coff\":%u,", who->name, phys(fr->pr) - who->addr);
    else
        fprintf(f, "\"caller\":null,\"coff\":null,");
    fprintf(f, "\"via\":\"%s\"", kind_name(fr->kind));
    if (obs_sandbox)
        fprintf(f, ",\"sbx\":%u", obs_sandbox);     /* run by the harness in a sandbox, not by the game */
    if (fr->from) {
        routine *org = routine_containing(fr->from);
        fprintf(f, ",\"from\":\"%08x\"", fr->from);
        if (org)
            fprintf(f, ",\"org\":\"%s\",\"ooff\":%u", org->name, phys(fr->from) - org->addr);
    }
}

static void emit_capture(sh4 *c, obs_frame *fr, routine *r, rstat *rs, int how, u32 why, int path_i)
{
    FILE *f = obs_log;
    obs_capture *cap = fr->cap;
    aspec *sp = &specs[fr->ridx];
    int fi = (int)(fr - stack);
    seq++;
    rs->kept++;
    /* the watched records as they are now (a new content writes a line of its own: before this record's) */
    u32 sn_now[SNAP_N];
    if (cap->snmask)
        snaps_take(cap, fr->ridx, sn_now, 0);

    /* ---- the call ---- */
    put_head(f, fr, r, rs);
    fprintf(f, ",\"r\":[%u,%u,%u,%u],\"frb\":[%u,%u,%u,%u],\"fr\":[", cap->r[4], cap->r[5], cap->r[6], cap->r[7],
            cap->fr[4], cap->fr[5], cap->fr[6], cap->fr[7]);
    for (int i = 4; i < 8; i++) {
        json_float(f, cap->fr[i]);
        fputc(i < 7 ? ',' : ']', f);
    }
    fprintf(f, ",\"fr8\":[%u,%u,%u,%u],\"r03\":[%u,%u,%u,%u],\"r8\":[%u,%u,%u,%u,%u,%u,%u],\"sp\":%u,"
            "\"mode\":\"%d.%d\",\"fc\":%u,\"vb\":%u,\"vf\":%u,", cap->fr[8], cap->fr[9], cap->fr[10], cap->fr[11],
            cap->r[0], cap->r[1], cap->r[2], cap->r[3], cap->r[8], cap->r[9], cap->r[10], cap->r[11], cap->r[12],
            cap->r[13], cap->r[14], cap->r[15], MODE_MAIN(cap->mode), MODE_SUB(cap->mode), cap->fc, cap->vb, cap->vf);
    if (how == H_RET)
        fprintf(f, "\"ret\":1");
    else
        fprintf(f, "\"ret\":\"%s\"", fr->kind == DISP_EXCEPTION ? "entered by an exception: PR is not its return address"
                                                                 : "no return seen");
    if (fr->kind == DISP_EXCEPTION)
        fprintf(f, ",\"exc\":1");
    if (fr->kind == DISP_JUMP || fr->kind == DISP_FALL)
        fprintf(f, ",\"tail\":1");
    if (sp->variadic)
        fprintf(f, ",\"variadic\":1");
    if (cap->nstk) {
        fprintf(f, ",\"stk\":[");
        for (int i = 0; i < cap->nstk; i++)
            fprintf(f, "%s%u", i ? "," : "", cap->stk[i]);
        fputc(']', f);
    }
    int any = 0;
    for (int i = 0; i < cap->npk; i++) {
        if (cap->pk[i].slot >= 16)
            continue;
        fprintf(f, "%s\"%d\":\"", any ? "," : ",\"pk\":{", cap->pk[i].slot);
        put_hex(f, cap->pk[i].data, cap->pk[i].len);
        fputc('"', f);
        any = 1;
    }
    if (any)
        fputc('}', f);
    any = 0;
    for (int i = 0; i < cap->npk; i++) {
        if (cap->pk[i].slot < 16)
            continue;
        fprintf(f, "%s\"%d\":\"", any ? "," : ",\"spk\":{", cap->pk[i].slot - 16);
        put_hex(f, cap->pk[i].data, cap->pk[i].len);
        fputc('"', f);
        any = 1;
    }
    if (any)
        fputc('}', f);
    int xf = (fr->flags & (OF_XF | OF_XFC)) != 0;
    if (xf) {
        fprintf(f, ",\"xf\":[");
        for (int i = 0; i < 16; i++)
            fprintf(f, "%s%u", i ? "," : "", cap->xf[i]);
        fprintf(f, "],\"xfby\":\"%s\"", (fr->flags & OF_XF) ? "itself" : "a callee");
    }
    if (cap->snmask) {
        any = 0;
        for (int k = 0; k < snap_n; k++)
            if ((cap->snmask & (1u << k)) && cap->sn[k]) {
                fprintf(f, "%s\"%s\":%u", any ? "," : ",\"sn\":{", snaps[k].name, cap->sn[k]);
                any = 1;
            }
        if (any)
            fputc('}', f);
    }
    put_why(f, why);
    fprintf(f, ",\"ic\":%llu,\"k\":\"hit\",\"t\":%.6f}\n", (unsigned long long)fr->ic, vtime(cap->cycles));

    /* ---- how it ended ---- */
    fprintf(f, "{\"id\":\"%s:%u\",\"fn\":\"%08x\",\"name\":\"%s\"", run_name, seq, r->addr, r->name);
    if (how == H_RET) {
        fprintf(f, ",\"r0\":%u,\"r1\":%u,\"fr0b\":%u,\"fr0\":", c->r[0], c->r[1], c->fr[0].u);
        json_float(f, c->fr[0].u);
        fprintf(f, ",\"fr1b\":%u,\"fpul\":%u,\"sp\":%u,\"dsp\":%d", c->fr[1].u, c->fpul, c->r[15],
                (int)(c->r[15] - fr->sp));
        u8 buf[PEEK_BYTES];
        u32 n = 0;
        if (sp->ret != 'v' && sp->ret != 'f' && sp->ret != 'd' && ptr_ok(c->r[0]))
            n = read_bytes(c->r[0], buf, sp->ret == 'p' && sp->rsz > 8 ? (sp->rsz > PEEK_BYTES ? PEEK_BYTES : sp->rsz)
                                                                      : (u32)obs_rules.peek);
        if (n) {
            fprintf(f, ",\"pk\":\"");
            put_hex(f, buf, n);
            fputc('"', f);
        } else {
            fprintf(f, ",\"pk\":null");
        }
    } else {
        static const char *const whys[] = { "", "its frame was left without the return being seen",
                                            "ended by the return from the exception", "the run ended first",
                                            "the controller loaded a snapshot", "the sandbox was closed first" };
        fprintf(f, ",\"why\":\"%s\"", whys[how]);
    }
    fprintf(f, ",\"mode\":\"%d.%d\",\"fc\":%u,\"dfc\":%d,\"vf\":%u,\"dic\":%llu", MODE_MAIN(last_mode),
            MODE_SUB(last_mode), game_frames(), (int)(game_frames() - cap->fc), machine_frame,
            (unsigned long long)(c->insns - fr->ic));
    /* the pointers of the call, read again */
    int same_n = 0;
    any = 0;
    for (int i = 0; i < cap->npk; i++) {
        peek *p = &cap->pk[i];
        u8 now[PEEK_BYTES];
        u32 n = read_bytes(p->addr, now, p->len);
        if (n == p->len && !memcmp(now, p->data, n)) {
            same_n++;
            continue;
        }
        fputs(any ? "," : ",\"apk\":{", f);
        put_slot_key(f, p->slot);
        fputc('"', f);
        put_hex(f, now, n);
        fputc('"', f);
        any = 1;
    }
    if (any)
        fputc('}', f);
    if (same_n) {
        any = 0;
        for (int i = 0; i < cap->npk; i++) {
            peek *p = &cap->pk[i];
            u8 now[PEEK_BYTES];
            u32 n = read_bytes(p->addr, now, p->len);
            if (n == p->len && !memcmp(now, p->data, n)) {
                fprintf(f, p->slot < 16 ? "%s\"%d\"" : "%s\"s%d\"", any ? "," : ",\"same\":[",
                        p->slot < 16 ? p->slot : p->slot - 16);
                any = 1;
            }
        }
        fputc(']', f);
    }
    /* the routines it called itself, in the order of their first call */
    fprintf(f, ",\"callees\":[");
    for (int i = 0; i < cap->ncal; i++) {
        callee *k = &cap->cal[i];
        if (k->kind & EK_OUT)
            fprintf(f, "%s[\"@%08x\",%u,\"outside\"]", i ? "," : "", k->key, k->n);
        else if ((k->kind & 15) == DISP_CALL)
            fprintf(f, "%s[\"%s\",%u]", i ? "," : "", routines[k->key].name, k->n);
        else
            fprintf(f, "%s[\"%s\",%u,\"%s\"]", i ? "," : "", routines[k->key].name, k->n, kind_name(k->kind));
    }
    fputc(']', f);
    if (cap->cal_more)
        fprintf(f, ",\"cmore\":%u", cap->cal_more);
    /* its first calls one by one, in order: [routine, [r4-r7], r0] with the float registers
     * [fr4, fr5] and fr0 as bit patterns when the callee's prototype uses them */
    if (cap->ncl) {
        fprintf(f, ",\"cl\":[");
        for (int i = 0; i < cap->ncl; i++) {
            clrec *k = &cap->cl[i];
            aspec *ks = &specs[k->ridx];
            fprintf(f, "%s[\"%s\",[%u,%u,%u,%u],", i ? "," : "", routines[k->ridx].name, k->a[0], k->a[1], k->a[2],
                    k->a[3]);
            if (k->done)
                fprintf(f, "%u", k->r0);
            else
                fputs("null", f);
            if (ks->fmask || ks->ret == 'f' || ks->ret == 'd') {
                fprintf(f, ",[%u,%u],", k->f[0], k->f[1]);
                if (k->done)
                    fprintf(f, "%u", k->f0);
                else
                    fputs("null", f);
            }
            fputc(']', f);
        }
        fputc(']', f);
        if (cap->cl_more)
            fprintf(f, ",\"clmore\":%u", cap->cl_more);
    }
    /* what was written: its own stores (never cut by what its callees store) */
    if (!cap->w_done)
        writes_make(fi);
    own_list_make(cap);
    fprintf(f, ",\"w\":%s,\"ws\":[%u,%u,%u]", cap->w_own.s, cap->ws_own[0], cap->ws_own[1], cap->ws_own[2]);
    if (cap->own_lost)
        fprintf(f, ",\"wocap\":%u", OWN_MAX);
    /* The same one by one, in order: [address, size, bytes before, bytes stored]. Left out ("wou")
     * when it would say nothing the ranges do not: every store a whole word, none to a place stored
     * to before, in rising order of address, each value still there at the return. */
    int uniform = cap->nown > 0 && cap->nown <= WO_N;
    for (u32 i = 0; uniform && i < cap->nown; i++) {
        ownrec *e = &cap->own[i];
        if (e->size != 4 || g_r32(e->addr) != e->val || (i && e->addr <= cap->own[i - 1].addr))
            uniform = 0;
    }
    if (uniform) {
        fprintf(f, ",\"wou\":1");
    } else if (cap->nown) {
        static const char digits[] = "0123456789abcdef";
        fprintf(f, ",\"wo\":[");
        for (u32 i = 0; i < cap->nown && i < WO_N; i++) {
            ownrec *e = &cap->own[i];
            char hb[9], hv[9];
            for (u32 b = 0; b < e->size && b < 4; b++) {
                u8 x = (u8)(e->old >> (8 * b)), y = (u8)(e->val >> (8 * b));
                hb[2 * b] = digits[x >> 4], hb[2 * b + 1] = digits[x & 15];
                hv[2 * b] = digits[y >> 4], hv[2 * b + 1] = digits[y & 15];
            }
            hb[2 * e->size] = hv[2 * e->size] = 0;
            fprintf(f, "%s[\"%08x\",%u,\"%s\",\"%s\"]", i ? "," : "", e->addr, e->size, hb, hv);
        }
        fputc(']', f);
    }
    /* the first value it read from each place in the game's own data: [address, size, value]
     * ("p" after it: a value a model planted) */
    if (cap->nrd) {
        fprintf(f, ",\"rd\":[");
        for (int i = 0; i < cap->nrd; i++)
            fprintf(f, "%s[\"%08x\",%u,%u%s]", i ? "," : "", cap->rd[i].addr, cap->rd[i].size, cap->rd[i].val,
                    cap->rd[i].planted ? ",\"p\"" : "");
        fputc(']', f);
        if (cap->rd_more)
            fprintf(f, ",\"rdmore\":1");
    }
    fprintf(f, ",\"wc\":%s,\"wcs\":[%u,%u,%u]", cap->w_cal.s, cap->ws_cal[0], cap->ws_cal[1], cap->ws_cal[2]);
    if (cap->ws_host[0])
        fprintf(f, ",\"wh\":%s,\"whs\":[%u,%u,%u]", cap->w_host.s, cap->ws_host[0], cap->ws_host[1],
                cap->ws_host[2]);
    if (cap->capped)
        fprintf(f, ",\"wcap\":%d,\"wcut\":\"callees\"", obs_rules.writes);   /* (wc and wh were cut; w is whole) */
    if (how == H_RET) {
        fprintf(f, ",\"path\":\"%016llx\"", (unsigned long long)fr->sig);
        if (path_i >= 0)
            fprintf(f, ",\"pathn\":%d", path_i);
    }
    if (exc_count != cap->exc0)
        fprintf(f, ",\"irq\":%u", exc_count - cap->exc0);
    if (hw_reads != fr->hw0)
        fprintf(f, ",\"hw\":%u", hw_reads - fr->hw0);
    if (cap->nhwr) {
        fprintf(f, ",\"hwr\":[");
        for (int i = 0; i < cap->nhwr; i++)
            fprintf(f, "%s[\"%08x\",%u]", i ? "," : "", cap->hwr[i][0], cap->hwr[i][1]);
        fputc(']', f);
    }
    if (cap->nhww) {
        fprintf(f, ",\"hww\":[");
        for (int i = 0; i < cap->nhww; i++)
            fprintf(f, "%s[\"%08x\",%u]", i ? "," : "", cap->hww[i][0], cap->hww[i][1]);
        fputc(']', f);
    }
    if (cap->nhwc) {
        /* registers its callees wrote: [address, last value, the routine that wrote it] */
        fprintf(f, ",\"hwc\":[");
        for (int i = 0; i < cap->nhwc; i++)
            fprintf(f, "%s[\"%08x\",%u,\"%s\"]", i ? "," : "", cap->hwc[i][0], cap->hwc[i][1],
                    cap->hwc[i][2] < (u32)routine_count ? routines[cap->hwc[i][2]].name : "?");
        fputc(']', f);
    }
    if (pl_reads != fr->pl0)
        fprintf(f, ",\"pl\":%u", pl_reads - fr->pl0);
    if (cap->nplr) {
        /* values a model planted that it read itself (low RAM, the sound driver's words): [address, value] */
        fprintf(f, ",\"plr\":[");
        for (int i = 0; i < cap->nplr; i++)
            fprintf(f, "%s[\"%08x\",%u]", i ? "," : "", cap->plr[i][0], cap->plr[i][1]);
        fputc(']', f);
    }
    if (cap->snmask) {
        any = 0;
        for (int k = 0; k < snap_n; k++)
            if ((cap->snmask & (1u << k)) && sn_now[k] && sn_now[k] != cap->sn[k]) {
                fprintf(f, "%s\"%s\":%u", any ? "," : ",\"snr\":{", snaps[k].name, sn_now[k]);
                any = 1;
            }
        if (any)
            fputc('}', f);
    }
    if (how == H_RET && (gen_routines[fr->ridx].is_asm || !sp->known)) {
        /* hand-written assembly hands results back where it likes: every
         * register that differs from its value at the entry */
        fprintf(f, ",\"chg\":{");
        any = 0;
        for (int i = 0; i < 15; i++)
            if (c->r[i] != cap->r[i]) {
                fprintf(f, "%s\"r%d\":%u", any ? "," : "", i, c->r[i]);
                any = 1;
            }
        for (int i = 0; i < 16; i++)
            if (c->fr[i].u != cap->fr[i]) {
                fprintf(f, "%s\"fr%d\":%u", any ? "," : "", i, c->fr[i].u);
                any = 1;
            }
        fputc('}', f);
    }
    if (xf) {
        fprintf(f, ",\"xf\":[");
        for (int i = 0; i < 16; i++)
            fprintf(f, "%s%u", i ? "," : "", c->xf[i].u);
        fputc(']', f);
    }
    if (c->fpscr != cap->fpscr)
        fprintf(f, ",\"fpscr\":[%u,%u]", cap->fpscr, c->fpscr);
    fprintf(f, ",\"k\":\"%s\",\"t\":%.6f}\n", how == H_RET ? "ret" : "noret", vtime(c->cycles));
}

/* A call that was not being recorded showed something new at its return
 * (or never returned): what its frame still holds. Besides the argument
 * registers and the result that is the count of its own stores with the
 * first of them one by one ("own", "lw"), and the first routines it called
 * with counts ("lc"). */
static void emit_late(sh4 *c, obs_frame *fr, routine *r, rstat *rs, u32 why, int path_i, int how)
{
    FILE *f = obs_log;
    aspec *sp = &specs[fr->ridx];
    seq++;
    rs->late++;
    put_head(f, fr, r, rs);
    fprintf(f, ",\"late\":1,\"r\":[%u,%u,%u,%u],\"frb\":[%u,%u,%u,%u],\"fr\":[", fr->a[0], fr->a[1], fr->a[2], fr->a[3],
            fr->f[0], fr->f[1], fr->f[2], fr->f[3]);
    for (int i = 0; i < 4; i++) {
        json_float(f, fr->f[i]);
        fputc(i < 3 ? ',' : ']', f);
    }
    fprintf(f, ",\"sp\":%u,\"mode\":\"%d.%d\",\"fc\":%u,\"vb\":%u,\"vf\":%u", fr->sp, MODE_MAIN(fr->mode),
            MODE_SUB(fr->mode), game_frames(), game_vblanks(), machine_frame);
    if (how == H_RET)
        fprintf(f, ",\"ret\":1");
    else
        fprintf(f, ",\"ret\":\"no return seen\"");
    if (fr->kind == DISP_JUMP || fr->kind == DISP_FALL)
        fprintf(f, ",\"tail\":1");
    put_why(f, why);
    fprintf(f, ",\"ic\":%llu,\"k\":\"hit\",\"t\":%.6f}\n", (unsigned long long)fr->ic, vtime(c->cycles));

    fprintf(f, "{\"id\":\"%s:%u\",\"fn\":\"%08x\",\"name\":\"%s\",\"late\":1", run_name, seq, r->addr, r->name);
    if (how == H_RET) {
        fprintf(f, ",\"r0\":%u,\"r1\":%u,\"fr0b\":%u,\"fr0\":", c->r[0], c->r[1], c->fr[0].u);
        json_float(f, c->fr[0].u);
        fprintf(f, ",\"fr1b\":%u,\"fpul\":%u,\"sp\":%u,\"dsp\":%d", c->fr[1].u, c->fpul, c->r[15],
                (int)(c->r[15] - fr->sp));
    } else {
        static const char *const whys[] = { "", "its frame was left without the return being seen",
                                            "ended by the return from the exception", "the run ended first",
                                            "the controller loaded a snapshot", "the sandbox was closed first" };
        fprintf(f, ",\"why\":\"%s\"", whys[how]);
    }
    fprintf(f, ",\"mode\":\"%d.%d\",\"fc\":%u,\"dfc\":0,\"vf\":%u,\"dic\":%llu,\"pk\":null", MODE_MAIN(last_mode),
            MODE_SUB(last_mode), game_frames(), machine_frame, (unsigned long long)(c->insns - fr->ic));
    /* the bytes behind the argument registers as they are now, after the call */
    int any = 0;
    for (int i = 0; i < 4; i++) {
        u8 buf[PEEK_BYTES];
        u32 len = (u32)obs_rules.peek;
        for (int k = 0; k < sp->n; k++)
            if (sp->loc[k] == 4 + i)
                len = peek_len(sp->cls[k], sp->psz[k]);
        if (len > PEEK_BYTES)
            len = PEEK_BYTES;
        if (!ptr_ok(fr->a[i]))
            continue;
        if (disp_slot(fr->a[i]) < disp_slots && obs_slot_routine[disp_slot(fr->a[i])])
            continue;
        u32 n = read_bytes(fr->a[i], buf, len);
        if (!n)
            continue;
        fprintf(f, "%s\"%d\":\"", any ? "," : ",\"pka\":{", 4 + i);
        put_hex(f, buf, n);
        fputc('"', f);
        any = 1;
    }
    if (any)
        fputc('}', f);
    /* its own stores: how many, and the first ones as [address, size, bytes before, bytes stored] */
    fprintf(f, ",\"own\":%u", fr->own_n);
    if (fr->own_n) {
        static const char digits[] = "0123456789abcdef";
        fprintf(f, ",\"lw\":[");
        for (u32 i = 0; i < fr->own_n && i < LW_N; i++) {
            obs_lstore *e = &fr->lw[i];
            char hb[9], hv[9];
            for (u32 b = 0; b < e->size && b < 4; b++) {
                u8 x = (u8)(e->old >> (8 * b)), y = (u8)(e->val >> (8 * b));
                hb[2 * b] = digits[x >> 4], hb[2 * b + 1] = digits[x & 15];
                hv[2 * b] = digits[y >> 4], hv[2 * b + 1] = digits[y & 15];
            }
            hb[2 * e->size] = hv[2 * e->size] = 0;
            fprintf(f, "%s[\"%08x\",%u,\"%s\",\"%s\"]", i ? "," : "", phys(e->addr), e->size, hb, hv);
        }
        fputc(']', f);
    }
    /* the routines it called itself, in the order of their first call, with counts */
    fprintf(f, ",\"lc\":[");
    for (int i = 0; i < fr->lc_n; i++)
        fprintf(f, "%s[\"%s\",%u]", i ? "," : "", routines[fr->lc[i].ridx].name, fr->lc[i].n);
    fputc(']', f);
    if (fr->lc_more)
        fprintf(f, ",\"lcmore\":%u", fr->lc_more);
    if (how == H_RET) {
        fprintf(f, ",\"path\":\"%016llx\"", (unsigned long long)fr->sig);
        if (path_i >= 0)
            fprintf(f, ",\"pathn\":%d", path_i);
    }
    if (hw_reads != fr->hw0)
        fprintf(f, ",\"hw\":%u", hw_reads - fr->hw0);
    if (pl_reads != fr->pl0)
        fprintf(f, ",\"pl\":%u", pl_reads - fr->pl0);
    fprintf(f, ",\"k\":\"%s\",\"t\":%.6f}\n", how == H_RET ? "ret" : "noret", vtime(c->cycles));
}

/* ---- frames ---- */

static inline int path_note(rstat *rs, u64 sig, int *is_new)
{
    if (rs->n_path && rs->path[rs->last_path] == sig) {
        rs->pathc[rs->last_path]++;
        return rs->last_path;
    }
    for (int i = 0; i < rs->n_path; i++)
        if (rs->path[i] == sig) {
            rs->pathc[i]++;
            rs->last_path = (u8)i;
            return i;
        }
    if (rs->n_path < PATH_N) {
        int i = rs->n_path++;
        rs->path[i] = sig;
        rs->pathc[i] = 1;
        rs->pathk[i] = 0;
        rs->last_path = (u8)i;
        *is_new = 1;
        return i;
    }
    rs->paths_over = 1;
    rs->path_lost++;
    return -1;
}

static inline int ret_note(rstat *rs, u32 v, int *is_new)
{
    if (rs->rets_over)
        return -1;
    if (rs->n_retv && rs->retv[rs->last_ret] == v) {
        rs->retc[rs->last_ret]++;
        return rs->last_ret;
    }
    for (int i = 0; i < rs->n_retv; i++)
        if (rs->retv[i] == v) {
            rs->retc[i]++;
            rs->last_ret = (u8)i;
            return i;
        }
    if (rs->n_retv < RETV_N) {
        int i = rs->n_retv++;
        rs->retv[i] = v;
        rs->retc[i] = 1;
        rs->retk[i] = 0;
        rs->last_ret = (u8)i;
        *is_new = 1;
        return i;
    }
    rs->rets_over = 1;
    return -1;
}

/* the class of a call's own stores: where its first ones went (heap and
 * stack addresses count as one place each) and what they put there */
static u32 store_class(obs_frame *f)
{
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < f->own_n && i < LW_N; i++) {
        u32 p = phys(f->lw[i].addr);
        if (p >= heap_lo && p < RAM_BASE + RAM_SIZE)
            p = 0x4ea90000u | (p & 3);
        h = (h ^ p) * 16777619u;
        h = (h ^ f->lw[i].val) * 16777619u;
    }
    return h | 1;
}

/* is this call's class of own stores one the routine's kept calls have not
 * shown, and has the routine been called often enough since the last one
 * that was kept for that? (The distance doubles each time, so that such
 * keeps spread over the whole run instead of being used up by a counter
 * in the first frames.) */
static int store_news(rstat *rs, obs_frame *f, int take)
{
    if (!f->own_n || rs->k_store >= STH_N)
        return 0;
    if (!take) {
        if ((u64)f->n - rs->store_last < rs->store_gap)
            return 0;
        u32 quick = f->own_n * 0x9e3779b1u ^ f->lw[0].addr ^ f->lw[0].val * 0x85ebca6bu;
        if (quick == rs->store_quick)
            return 0;                   /* as the call before it: looked at already */
        rs->store_quick = quick;
    }
    u32 h = store_class(f);
    for (int i = 0; i < rs->n_sth; i++)
        if (rs->sth[i] == h)
            return 0;
    if ((u64)f->n - rs->store_last < rs->store_gap)
        return 0;
    if (take) {
        if (rs->n_sth < STH_N)
            rs->sth[rs->n_sth++] = h;
        rs->k_store++;
        rs->store_last = f->n;
        rs->store_gap = rs->store_gap ? rs->store_gap * 2 : 1;
    }
    return 1;
}

static void frame_done(sh4 *c, int how)
{
    obs_frame *f = &stack[sp_n];
    if (f->ridx != OBS_BARRIER) {
        routine *r = &routines[f->ridx];
        rstat *rs = &rst[f->ridx];
        aspec *sp = &specs[f->ridx];
        int pi = -1, ri = -1, new_path = 0, new_ret = 0;
        if (how == H_RET) {
            pi = path_note(rs, f->sig, &new_path);
            if (sp->ret != 'v')
                ri = ret_note(rs, sp->ret == 'f' ? c->fr[0].u : c->r[0], &new_ret);
            /* its result into its caller's list of calls */
            if (f->clx != 0xffff && sp_n > 0) {
                obs_capture *up = stack[sp_n - 1].cap;
                if (up && f->clx < up->ncl && up->cl[f->clx].ridx == f->ridx && !up->cl[f->clx].done) {
                    up->cl[f->clx].r0 = c->r[0];
                    up->cl[f->clx].f0 = c->fr[0].u;
                    up->cl[f->clx].done = 1;
                }
            }
        }
        if (f->cap) {
            u32 why = f->cap->why;
            if (how == H_RET) {
                if (pi >= 0 && rs->pathk[pi] != 2 && rs->k_path < K_PATH)
                    why |= W_PATH;
                if (ri >= 0 && rs->retk[ri] != 2 && rs->k_ret < K_RET)
                    why |= W_RET;
                if (store_news(rs, f, 1))
                    why |= W_STORE;
            }
            if (why & ~W_HUNT) {
                if (why & W_PATH)
                    rs->k_path++;
                if (why & W_RET)
                    rs->k_ret++;
                if (pi >= 0)
                    rs->pathk[pi] = 2;
                if (ri >= 0)
                    rs->retk[ri] = 2;
                if (how != H_RET && rs->k_noret < 255)
                    rs->k_noret++;
                emit_capture(c, f, r, rs, how, why, pi);
            }
            capture_free(f);
        } else if (obs_log && how == H_RET) {
            u32 why = 0;
            if (pi >= 0 && rs->pathk[pi] == 0)
                why |= W_PATH;
            if (ri >= 0 && rs->retk[ri] == 0 && rs->k_ret < K_RET)
                why |= W_RET;
            if (!why && store_news(rs, f, 0))
                why |= W_STORE;
            if (why) {
                if (rs->late < LATE_MAX || ((why & W_STORE) && rs->late < LATE_MAX + STH_N)) {
                    if (why & W_STORE)
                        store_news(rs, f, 1);
                    emit_late(c, f, r, rs, why, pi, H_RET);
                    if (why & W_PATH)
                        rs->pathk[pi] = 1;
                    if (why & W_RET)
                        rs->retk[ri] = 1;
                }
                if (!rs->hunt && rs->hunts < HUNT_MAX) {
                    rs->hunt = HUNT_LEN;
                    rs->hunts += HUNT_LEN;
                }
            } else if (pi >= 0 && rs->pathk[pi] == 1 && rs->k_path < K_PATH && !rs->hunt && rs->hunts < HUNT_MAX) {
                rs->hunt = HUNT_LEN;    /* still no full record of this path */
                rs->hunts += HUNT_LEN;
            }
        } else if (obs_log && how != H_JUMPED && rs->k_noret < K_NORET) {
            /* a call that never returned and was not being recorded: say so,
             * with what its frame holds (once or twice per routine) */
            rs->k_noret++;
            emit_late(c, f, r, rs, 0, -1, how);
        }
        if (f->flags & OF_XF)
            stack[sp_n - 1].flags |= OF_XFC;
    }
    sp_n--;
    obs_top = &stack[sp_n];
    rd_update();
}

static u32 arg_hash(sh4 *c, aspec *sp)
{
    u32 h = 0x811c9dc5u;
    u32 sp15 = phys(c->r[15]);
    for (int i = 0; i < 4; i++) {
        if (!(sp->imask & (1u << i)))
            continue;
        u32 v = c->r[4 + i];
        u32 p = phys(v);
        if ((v >> 29) == 0 || (v >> 29) == 4 || (v >> 29) == 5) {
            if (p - (sp15 - STACK_SPAN) < 2 * STACK_SPAN)
                v = 0x57ac0000u;                    /* somewhere on the stack */
            else if (p >= heap_lo && p < RAM_BASE + RAM_SIZE)
                v = 0x4ea90000u;                    /* somewhere on the heap */
        }
        h = (h ^ v) * 16777619u;
    }
    for (int i = 0; i < 8; i++)
        if (sp->fmask & (1u << i))
            h = (h ^ c->fr[4 + i].u) * 16777619u;
    return h | 1;
}

int obs_call(sh4 *c, routine *r, int kind)
{
    if (!obs_on)
        return 1;
    u32 ridx = (u32)(r - routines);
    u32 from = kind == DISP_EXCEPTION ? 0 : obs_pc;
    if (kind == DISP_JUMP || kind == DISP_FALL) {
        u32 fs = disp_slot(from);
        if (fs < disp_slots && obs_slot_routine[fs] == ridx + 1)
            return 0;                   /* back to its own first instruction: a loop */
    }
    obs_entered = r->addr;
    u32 sp15 = c->r[15];
    /* frames whose stack has been given back were left without a return */
    while (sp_n > 0 && stack[sp_n].ridx != OBS_BARRIER && stack[sp_n].sp < sp15)
        frame_done(c, H_LEFT);
    edge_count(from, r->addr, kind);
    if (sp_n >= STACK_MAX) {
        stack_overflows++;
        return 1;
    }
    obs_frame *par = &stack[sp_n];
    u16 clx = 0xffff;
    if (par->ridx != OBS_BARRIER) {
        obs_capture *pc = par->cap;
        if (pc) {
            callee_add(pc, ridx, kind);
            if (pc->ncl < CL_N) {
                clrec *k = &pc->cl[pc->ncl];
                k->ridx = ridx;
                k->a[0] = c->r[4], k->a[1] = c->r[5], k->a[2] = c->r[6], k->a[3] = c->r[7];
                k->f[0] = c->fr[4].u, k->f[1] = c->fr[5].u;
                k->kind = (u8)kind;
                k->done = 0;
                clx = (u16)pc->ncl++;
            } else {
                pc->cl_more++;
            }
        }
        /* every frame keeps the first routines it called, with counts (the one called last is tried first) */
        int i = par->lc_n ? par->lc_n - 1 : 0;
        if (!(i < par->lc_n && par->lc[i].ridx == (u16)ridx))
            for (i = 0; i < par->lc_n && par->lc[i].ridx != (u16)ridx; i++)
                ;
        if (i < par->lc_n) {
            if (par->lc[i].n < 0xffff)
                par->lc[i].n++;
        } else if (par->lc_n < LC_N) {
            par->lc[par->lc_n].ridx = (u16)ridx;
            par->lc[par->lc_n].n = 1;
            par->lc_n++;
        } else {
            par->lc_more++;
        }
    }
    obs_frame *f = &stack[++sp_n];
    obs_top = f;
    f->spp = phys(sp15);
    f->own_n = 0;
    f->pl0 = pl_reads;
    f->lc_n = 0;
    f->lc_more = 0;
    f->clx = clx;
    f->sig = 0;
    f->ic = c->insns;
    f->flags = 0;
    f->ridx = ridx;
    f->pr = c->pr;
    f->sp = sp15;
    f->from = from;
    f->hw0 = hw_reads;
    f->n = (u32)(obs_sandbox ? r->hcalls : r->calls) + 1;
    f->a[0] = c->r[4], f->a[1] = c->r[5], f->a[2] = c->r[6], f->a[3] = c->r[7];
    f->f[0] = c->fr[4].u, f->f[1] = c->fr[5].u, f->f[2] = c->fr[6].u, f->f[3] = c->fr[7].u;
    f->cap = NULL;
    f->mode = (u16)last_mode;
    f->kind = (u8)kind;
    f->lvl = (u8)cur_lvl;
    obs_rd_on = 0;
    obs_jr_on = jr_ring_on;
    if (!obs_log)
        return 1;

    rstat *rs = &rst[ridx];
    u32 why = 0;
    if (harness_top == r) {
        why = W_HCALL;                  /* the routine the controller's "call" names: always in full */
        harness_top = NULL;
    } else if (disp_flags[disp_slot(r->addr)] & DF_TRACE) {
        why = W_TRACE;
    } else {
        /* a call site not seen before is always kept (up to the table's size), whatever else has been */
        if (rs->n_site < SITE_N) {
            u32 site = from ? from : 1;
            int i = 0;
            while (i < rs->n_site && rs->site[i] != site)
                i++;
            if (i == rs->n_site) {
                rs->site[rs->n_site++] = site;
                why |= W_SITE;
            }
        }
        /* and one call in full at growing distances through the run: the
         * 1,000th, the 2,000th, the 4,000th ... */
        if (!rs->next_spread)
            rs->next_spread = 1000;
        if ((u64)f->n >= rs->next_spread) {
            rs->next_spread = (u64)f->n * 2;
            if (rs->k_spread < K_SPREAD) {
                rs->k_spread++;
                why |= W_SPREAD;
            }
        }
    }
    if (!(why & (W_TRACE | W_HCALL)) && rs->kept < (u32)obs_rules.max_hits) {
        if (rs->k_first < obs_rules.hits) {
            rs->k_first++;
            why |= W_FIRST;
        }
        if (rs->n_mode < MODE_N) {
            u8 m = (u8)(last_mode >> 8);
            int i = 0;
            while (i < rs->n_mode && rs->mode[i] != m)
                i++;
            if (i == rs->n_mode) {
                rs->mode[rs->n_mode++] = m;
                why |= W_MODE;
            }
        }
        aspec *sp = &specs[ridx];
        if (rs->n_argh < ARGH_N && (sp->imask | sp->fmask)) {
            u32 h = arg_hash(c, sp);
            int i = 0;
            while (i < rs->n_argh && rs->argh[i] != h)
                i++;
            if (i == rs->n_argh) {
                rs->argh[rs->n_argh++] = h;
                why |= W_ARGS;
            }
        }
    }
    if (rs->hunt) {
        rs->hunt--;
        why |= W_HUNT;
    }
    if (why)
        capture_begin(c, f, r, why);
    return 1;
}

/* did execution come to pc from the instruction before it: by running on
 * past it, or by a conditional branch? (Anything else that lands on a
 * routine's first instruction without a call or a jump is a resumption:
 * the return from an interrupt that was taken there.) */
static int came_by_fall(u32 pc)
{
    u32 d = pc - obs_pc;
    if (d > 0x204 && d < 0xfffffe00u)
        return 0;
    if (!mem_ptr(obs_pc))
        return 0;
    u32 op = g_r16(obs_pc);
    if ((op & 0xf900) == 0x8900) {          /* bt, bf, bt/s, bf/s */
        int delayed = (op & 0x0400) != 0;
        u32 target = obs_pc + 4 + (u32)((s32)(s8)(op & 0xff) * 2);
        return target == pc || obs_pc + (delayed ? 4u : 2u) == pc;
    }
    if (d != 2)
        return 0;
    /* the instruction right before: unless it is a branch with a delay
     * slot (then the slot, not it, was the last thing to run) */
    if ((op & 0xe000) == 0xa000)            /* bra, bsr */
        return 0;
    if ((op & 0xf0df) == 0x400b || (op & 0xf0df) == 0x0003)   /* jsr, jmp, bsrf, braf */
        return 0;
    if (op == 0x000b || op == 0x002b)       /* rts, rte */
        return 0;
    return 1;
}

int obs_step(sh4 *c, u32 pc, u32 slot, u8 m)
{
    if (!(m & OM_RUN))
        obs_map[slot] = m | OM_RUN;
    if (m & OM_ENTRY) {
        if (obs_entered == phys(pc)) {
            obs_entered = 0;
        } else if (!obs_pc || came_by_fall(pc)) {
            /* reached without a call or a jump: execution ran into the
             * routine from the code before it, or a conditional branch
             * came here (or it is the very first instruction: reset) */
            disp_enter(c, pc, obs_pc ? DISP_FALL : DISP_EXCEPTION);
            if (c->pc != pc || c->stop)
                return 1;               /* (obs_entered stays, so that coming back here is not a second entry) */
            obs_entered = 0;
        }
    }
    return 0;
}

void obs_into(sh4 *c, u32 target, int kind)
{
    (void)c;
    edge_count(obs_pc, phys(target), kind | EK_INTO);
}

void obs_outside_call(sh4 *c, u32 target)
{
    (void)c;
    edge_count(obs_pc, target, DISP_CALL | EK_OUT);
    hw_reads++;
    if (obs_top->cap)
        callee_add(obs_top->cap, target, DISP_CALL | EK_OUT);
}

void obs_on_return(sh4 *c)
{
    u32 pc = c->pc, sp = c->r[15];
    int i = sp_n;
    if (i <= 0)
        return;
    obs_frame *f = &stack[i];
    if (__builtin_expect(f->ridx == OBS_BARRIER, 0))
        return;
    if (__builtin_expect(!(f->pr == pc && f->sp == sp), 0)) {
        /* not the top frame's return. A frame further down whose return
         * this is? Then the ones above it are done with: jumped into and
         * finished on its behalf, or left behind. */
        int scan = 0;
        while (i > 0 && !(stack[i].pr == pc && stack[i].sp == sp)) {
            if (stack[i].ridx == OBS_BARRIER || stack[i].sp > sp || ++scan > 32) {
                i = 0;
                break;
            }
            i--;
        }
        if (i <= 0) {
            while (sp_n > 0 && stack[sp_n].ridx != OBS_BARRIER && stack[sp_n].sp < sp)
                frame_done(c, H_LEFT);
            return;
        }
        while (sp_n > i) {
            obs_frame *t = &stack[sp_n];
            frame_done(c, t->pr == pc && (t->kind == DISP_JUMP || t->kind == DISP_FALL) ? H_RET : H_LEFT);
        }
    }
    /* a chain of jumps from routine to routine comes back through one rts */
    for (;;) {
        int kind = stack[sp_n].kind;
        frame_done(c, H_RET);
        if (!(kind == DISP_JUMP || kind == DISP_FALL) || sp_n <= 0)
            break;
        obs_frame *t = &stack[sp_n];
        if (t->ridx == OBS_BARRIER || t->pr != pc || t->sp != sp)
            break;
    }
}

void obs_host_returned(sh4 *c, routine *r)
{
    if (!obs_on || sp_n <= 0)
        return;
    obs_frame *f = &stack[sp_n];
    if (f->ridx == (u32)(r - routines) && f->sp == c->r[15])
        frame_done(c, H_RET);
}

void obs_exception_level(int d)
{
    if (!obs_on)
        return;
    d |= cur_lvl & LVL_SANDBOX;
    if (d > cur_lvl) {
        exc_count++;
        if (sp_n < STACK_MAX) {
            obs_frame *f = &stack[++sp_n];
            memset(f, 0, sizeof *f);
            f->ridx = OBS_BARRIER;
            f->sp = cpu.r[15];
            f->lvl = (u8)d;
            obs_top = f;
        }
    } else if (d < cur_lvl) {
        int i = sp_n;
        while (i > 0 && stack[i].ridx != OBS_BARRIER)
            i--;
        if (i > 0) {
            while (sp_n > i)
                frame_done(&cpu, H_RTE);
            frame_done(&cpu, H_RTE);        /* the boundary itself */
        }
    }
    cur_lvl = d;
    jr_update();
}

/* ---- sandboxes ----
 *
 * What the controller runs between "sandbox begin" and "sandbox end" (a
 * "call" makes its own) happens in a copy of the machine that is thrown
 * away. The observer keeps it apart from the game's own run: its own
 * coverage map, its own counts and edges, its own sampling state; every
 * record made inside carries "sbx" with the sandbox's number. On the shadow
 * stack the sandbox is a level of its own above a boundary, like an
 * interrupt: the calls of the game that were under way stay as they are and
 * go on when the sandbox is closed. */

static u8 *h_map, *g_map;
static rstat *h_rst, *g_rst;
static edge *h_edges, *g_edges;
static u32 h_edges_used, g_edges_used;
static int sbx_base, sbx_lvl;

void obs_sandbox_begin(u32 id)
{
    obs_sandbox = id;
    if (!obs_on)
        return;
    if (!h_map) {
        h_map = calloc(disp_slots, 1);
        h_rst = calloc((size_t)routine_count, sizeof *h_rst);
        h_edges = calloc(EDGE_SLOTS, sizeof *h_edges);
        for (u32 i = 0; i < disp_slots; i++)
            h_map[i] = obs_map[i] & OM_ENTRY;
    }
    g_map = obs_map, obs_map = h_map;
    g_rst = rst, rst = h_rst;
    g_edges = edges, edges = h_edges;
    g_edges_used = edges_used, edges_used = h_edges_used;
    sbx_lvl = cur_lvl;
    cur_lvl |= LVL_SANDBOX;
    sbx_base = sp_n;
    if (sp_n < STACK_MAX) {
        obs_frame *f = &stack[++sp_n];
        memset(f, 0, sizeof *f);
        f->ridx = OBS_BARRIER;
        f->sp = cpu.r[15];
        f->lvl = (u8)cur_lvl;
        obs_top = f;
    }
    obs_entered = 0;
    jr_update();
    if (obs_log)
        fprintf(obs_log, "{\"sbx\":%u,\"what\":\"begin\",\"vf\":%u,\"fc\":%u,\"mode\":\"%d.%d\",\"k\":\"sandbox\","
                "\"t\":%.6f}\n", id, machine_frame, game_frames(), game_mode_main(), game_mode_sub(), vtime(cpu.cycles));
}

void obs_sandbox_end(void)
{
    u32 id = obs_sandbox;
    if (obs_on && h_map && obs_map == h_map) {
        while (sp_n > sbx_base)
            frame_done(&cpu, H_SANDBOX);
        cur_lvl = sbx_lvl;
        h_edges_used = edges_used;
        obs_map = g_map, rst = g_rst, edges = g_edges, edges_used = g_edges_used;
        obs_entered = phys(cpu.pc);     /* where the machine stands again is not a new entry */
        harness_top = NULL;
        jr_update();
        if (obs_log)
            fprintf(obs_log, "{\"sbx\":%u,\"what\":\"end\",\"vf\":%u,\"k\":\"sandbox\",\"t\":%.6f}\n", id,
                    machine_frame, vtime(cpu.cycles));
    }
    obs_sandbox = 0;
}

void obs_harness_call(routine *r)
{
    harness_top = r;
    obs_pc = 0;                         /* no instruction of the game makes this call */
    obs_entered = 0;
}

/* ---- the frame line ----
 *
 * Once a frame, when the cabinet's switches or one of a handful of the
 * game's globals changed: {"k":"frame","vf","vb","fc","ic","pad":[player 1,
 * player 2, system, coins of chute 1, of chute 2],"g":{name:value}}. Only
 * what changed is in a line ("pad" and each global carry forward); every
 * 600th frame a line has everything ("full"). The switch bits are the
 * controller's (src/control.h): 1 up, 2 down, 4 left, 8 right, 0x10 punch,
 * 0x20 kick, 0x40 guard, 0x80 the fourth button, 0x100 start. */

#define FG_N 32
static struct {
    char name[44];
    u32 addr, last;
    int size;
} fg[FG_N];
static int fg_n;
static u32 pad_last[5];
static u32 fl_last_full = 0xffffffffu;

int obs_frame_global(const char *name, int size)
{
    u32 off = 0;
    char base[64];
    snprintf(base, sizeof base, "%s", name);
    char *plus = strchr(base, '+');
    if (plus) {
        *plus = 0;
        off = (u32)strtoul(plus + 1, NULL, 0);
    }
    u32 a = global_addr(base);
    if (!a || fg_n >= FG_N)
        return 0;
    for (int i = 0; i < fg_n; i++)
        if (!strcmp(fg[i].name, name))
            return 1;
    snprintf(fg[fg_n].name, sizeof fg[fg_n].name, "%s", name);
    fg[fg_n].addr = a + off;
    fg[fg_n].size = size == 1 || size == 2 ? size : 4;
    fg[fg_n].last = 0;
    fg_n++;
    fl_last_full = 0xffffffffu;
    return 1;
}

void obs_frame_line(sh4 *c)
{
    if (!obs_log || obs_sandbox)
        return;
    u32 pad[5] = { inputs.player[0], inputs.player[1], inputs.sys, inputs.coins[0], inputs.coins[1] };
    int full = fl_last_full == 0xffffffffu || machine_frame - fl_last_full >= 600;
    int pad_new = full || memcmp(pad, pad_last, sizeof pad) != 0;
    u32 val[FG_N];
    int changed = 0;
    for (int i = 0; i < fg_n; i++) {
        val[i] = fg[i].size == 1 ? g_r8(fg[i].addr) : fg[i].size == 2 ? g_r16(fg[i].addr) : g_r32(fg[i].addr);
        changed |= val[i] != fg[i].last;
    }
    if (!pad_new && !changed)
        return;
    FILE *f = obs_log;
    fprintf(f, "{\"vf\":%u,\"vb\":%u,\"fc\":%u,\"ic\":%llu,\"mode\":\"%d.%d\"", machine_frame, game_vblanks(),
            game_frames(), (unsigned long long)c->insns, game_mode_main(), game_mode_sub());
    if (full)
        fprintf(f, ",\"full\":1");
    if (pad_new)
        fprintf(f, ",\"pad\":[%u,%u,%u,%u,%u]", pad[0], pad[1], pad[2], pad[3], pad[4]);
    int any = 0;
    for (int i = 0; i < fg_n; i++)
        if (full || val[i] != fg[i].last) {
            fprintf(f, "%s\"%s\":%u", any ? "," : ",\"g\":{", fg[i].name, val[i]);
            fg[i].last = val[i];
            any = 1;
        }
    if (any)
        fputc('}', f);
    fprintf(f, ",\"k\":\"frame\"}\n");
    memcpy(pad_last, pad, sizeof pad);
    if (full)
        fl_last_full = machine_frame;
}

void obs_machine_jumped(const char *why)
{
    if (!obs_on)
        return;
    if (obs_sandbox)
        obs_sandbox_end();
    while (sp_n > 0)
        frame_done(&cpu, H_JUMPED);
    fl_last_full = 0xffffffffu;
    cur_lvl = disp_exc_depth;
    obs_entered = phys(cpu.pc);         /* where the machine now stands is not a new entry */
    jr_update();
    obs_note(why);
}

void obs_trace_call(sh4 *c, routine *r, int kind)
{
    (void)kind;
    if (cfg.quiet)
        return;
    char mode[160];
    fprintf(stderr, "[trace] vb %u %s  %s(%08x, %08x, %08x, %08x; %g, %g)  from %s\n", machine_frame,
            game_mode_name(game_mode_main(), game_mode_sub(), mode, sizeof mode), r->name, c->r[4], c->r[5],
            c->r[6], c->r[7], (double)c->fr[4].f, (double)c->fr[5].f, addr_text(c->pr));
}

/* ---- the end of the run: totals into the log ---- */

static u32 routine_slots(int i, u32 *first)
{
    u32 s = disp_slot(routines[i].addr);
    u32 e = i + 1 < routine_count ? disp_slot(routines[i + 1].addr)
                                  : disp_slot((routines[i].addr + routines[i].size + 0x1f) & ~0x1fu);
    if (e > disp_slots)
        e = disp_slots;
    *first = s;
    return e > s ? e - s : 0;
}

static void put_bits(FILE *f, u32 first, u32 n, u8 bit)
{
    static const char digits[] = "0123456789abcdef";
    for (u32 k = 0; k < n; k += 4) {
        u32 v = 0;
        for (u32 j = 0; j < 4 && k + j < n; j++)
            if (obs_map[first + k + j] & bit)
                v |= 1u << j;
        fputc(digits[v], f);
    }
}

static void stat_lines(FILE *f, const u8 *map, rstat *tab, int harness)
{
    for (int i = 0; i < routine_count; i++) {
        routine *r = &routines[i];
        rstat *rs = &tab[i];
        u32 first, n = routine_slots(i, &first), run = 0;
        for (u32 k = 0; k < n; k++)
            run += map[first + k] & OM_RUN;
        unsigned long long calls = harness ? r->hcalls : r->calls;
        if (harness ? !calls && !run : !r->seen && !run)
            continue;
        fprintf(f, "{\"fn\":\"%08x\",\"name\":\"%s\",\"calls\":%llu,\"kept\":%u,\"late\":%u,\"first_vf\":%u,"
                "\"first_mode\":\"%d.%d\",\"retk\":\"%c\",\"paths\":[", r->addr, r->name, calls, rs->kept, rs->late,
                harness ? 0 : r->first_frame, harness ? -1 : MODE_MAIN(r->first_mode),
                harness ? -1 : MODE_SUB(r->first_mode), specs[i].ret ? specs[i].ret : '?');
        for (int k = 0; k < rs->n_path; k++)
            fprintf(f, "%s[\"%016llx\",%u,%u]", k ? "," : "", (unsigned long long)rs->path[k], rs->pathc[k],
                    rs->pathk[k]);
        fprintf(f, "],\"paths_lost\":%u,\"rets\":[", rs->path_lost);
        for (int k = 0; k < rs->n_retv; k++)
            fprintf(f, "%s[%u,%u]", k ? "," : "", rs->retv[k], rs->retc[k]);
        fprintf(f, "],\"rets_more\":%d,\"size\":%u,\"run\":\"", rs->rets_over, n);
        const u8 *save = obs_map;
        obs_map = (u8 *)map;
        put_bits(f, first, n, OM_RUN);
        fprintf(f, "\",\"bt\":\"");
        put_bits(f, first, n, OM_BT);
        fprintf(f, "\",\"bn\":\"");
        put_bits(f, first, n, OM_BN);
        obs_map = (u8 *)save;
        fprintf(f, "\",\"k\":\"%s\"}\n", harness ? "hstat" : "stat");
    }
}

static void edge_lines(FILE *f, edge *tab, const char *k)
{
    for (u32 i = 0; i < EDGE_SLOTS; i++) {
        edge *e = &tab[i];
        if (!e->used)
            continue;
        /* a switch's edge (from the computed jump to the case it went to) has its own kind of line,
         * so that readers that add up the calls into a routine never meet it */
        fprintf(f, "{\"from\":\"%08x\",\"to\":\"%08x\",\"via\":\"%s\"%s%s,\"n\":%llu,\"k\":\"%s%s\"}\n", e->from, e->to,
                kind_name(e->kind), (e->kind & EK_INTO) ? ",\"into\":1" : "", (e->kind & EK_OUT) ? ",\"out\":1" : "",
                (unsigned long long)e->n, (e->kind & EK_SW) ? "sw" : "", (e->kind & EK_SW) ? "itch" : k);
    }
}

void obs_close(void)
{
    if (!obs_on)
        return;
    if (obs_sandbox)
        obs_sandbox_end();
    while (sp_n > 0)
        frame_done(&cpu, H_END);
    if (!obs_log)
        return;
    FILE *f = obs_log;
    stat_lines(f, obs_map, rst, 0);
    edge_lines(f, edges, "edge");
    if (h_map) {
        /* what ran only because the harness called it (sandboxes): lines of their own */
        stat_lines(f, h_map, h_rst, 1);
        edge_lines(f, h_edges, "hedge");
    }
    fprintf(f, "{\"insns\":%llu,\"vf\":%u,\"fc\":%u,\"entered\":%d,\"records\":%u,\"edges\":%u,\"edges_lost\":%llu,"
            "\"stack_overflows\":%llu,\"k\":\"end\",\"t\":%.6f}\n", (unsigned long long)cpu.insns, machine_frame,
            game_frames(), obs_seen_count(), seq, edges_used, (unsigned long long)edge_overflows,
            (unsigned long long)stack_overflows, vtime(cpu.cycles));
    fclose(obs_log);
    obs_log = NULL;
}

/* ---- reports ---- */

int obs_seen_count(void)
{
    int n = 0;
    for (int i = 0; i < routine_count; i++)
        n += routines[i].seen;
    return n;
}

void obs_write_profile(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "address\tname\tfile\trun as\tcalls\tfirst vblank\tfirst mode\tfirst instruction\tkept\tlate\tpaths"
            "\tharness calls\n");
    for (int i = 0; i < routine_count; i++) {
        routine *r = &routines[i];
        char mode[160] = "";
        if (r->seen)
            game_mode_name(MODE_MAIN(r->first_mode), MODE_SUB(r->first_mode), mode, sizeof mode);
        fprintf(f, "%08x\t%s\t%s\t%s\t%llu\t%u\t%s\t%llu\t%u\t%u\t%u\t%llu\n", r->addr, r->name, r->file,
                r->kind == RK_HOST ? "host replacement" : r->hook ? "original + hook" : "original",
                (unsigned long long)r->calls, r->seen ? r->first_frame : 0, mode,
                (unsigned long long)r->first_insn, rst ? rst[i].kept : 0, rst ? rst[i].late : 0,
                rst ? rst[i].n_path : 0, (unsigned long long)r->hcalls);
    }
    fclose(f);
}

void obs_write_coverage(const char *path)
{
    if (!obs_on)
        return;
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "address\tname\tfile\thalfwords\texecuted\tbranches\tboth ways\n");
    for (int i = 0; i < routine_count; i++) {
        routine *r = &routines[i];
        u32 first, n = routine_slots(i, &first), hit = 0, br = 0, both = 0;
        for (u32 k = 0; k < n; k++) {
            u8 m = obs_map[first + k];
            hit += m & OM_RUN;
            if (m & (OM_BT | OM_BN)) {
                br++;
                both += (m & (OM_BT | OM_BN)) == (OM_BT | OM_BN);
            }
        }
        fprintf(f, "%08x\t%s\t%s\t%u\t%u\t%u\t%u\n", r->addr, r->name, r->file, n, hit, br, both);
    }
    fclose(f);
}

void obs_write_edges(const char *path)
{
    if (!obs_on)
        return;
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "from\tfrom routine\tto\tto routine\tvia\tcount\n");
    for (u32 i = 0; i < EDGE_SLOTS; i++) {
        edge *e = &edges[i];
        if (!e->used)
            continue;
        routine *a = routine_containing(e->from), *b = routine_containing(e->to);
        fprintf(f, "%08x\t%s\t%08x\t%s\t%s%s%s\t%llu\n", e->from, a && e->from ? a->name : "", e->to, b ? b->name : "",
                (e->kind & EK_SW) ? "switch" : kind_name(e->kind), (e->kind & EK_INTO) ? " into" : "",
                (e->kind & EK_OUT) ? " outside" : "", (unsigned long long)e->n);
    }
    fclose(f);
}
