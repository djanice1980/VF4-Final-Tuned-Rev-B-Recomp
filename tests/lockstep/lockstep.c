/* The translation against VF4debug's interpreter (docs/design.md, section 9).
 *
 * Built by tests/lockstep/build.sh next to VF4debug's core (read from its
 * checkout, never copied), so both run on the same guest memory. Part 1
 * runs every ordinary instruction the translator can emit, in each FPU mode,
 * from random states: one step of the interpreter against the translated
 * code, comparing every register and the memory it may touch. Part 2 calls
 * routines of the game image both ways and compares the whole state and all
 * of RAM afterwards.
 *
 * usage: lockstep [-s states-per-case] [-r runs-per-routine] [-v] <routine-list>...
 */
#include "sh4.h"
#include "mem.h"
#include "hw.h"
#include "dispatch.h"
#include "machine.h"
#include "control.h"

#include "lockstep_cases.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void write_reports(void) {}

#define BUF     0x0c910000u     /* scratch memory the instructions address */
#define BUF_LEN 0x00010000u
#define STACK   0x0c9f0000u

static int verbose;
static long checks, failures, skipped, traps, too_long;
#define BUDGET 20000000ull       /* instructions a routine run may take */

static u32 rng_state = 0x9e3779b9u;
static u32 rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static u32 rnd_edge(void)
{
    static const u32 edges[] = { 0, 1, 2, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xffffffffu, 0xfffffffeu,
                                 0xffff, 0x10000, 0x8000, 0xff, 0x100, 31, 32, 33 };
    u32 r = rnd();
    if ((r & 7) == 0)
        return edges[(r >> 3) % (sizeof edges / sizeof edges[0])];
    if ((r & 7) == 1)
        return rnd() >> (rnd() & 31);
    return rnd();
}

/* float bit patterns: ordinary numbers most of the time, and the values
 * that take the FPU's uncommon paths */
static u32 rnd_float(void)
{
    static const u32 edges[] = { 0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
                                 0x7fc00000u, 0x7fbfffffu, 0x7f800001u, 0x00000001u, 0x807fffffu, 0x00800000u,
                                 0x7f7fffffu, 0x4f000000u, 0xcf000000u, 0x4b800000u, 0x3f000000u, 0x40490fdbu };
    u32 r = rnd();
    if ((r & 7) == 0)
        return edges[(r >> 3) % (sizeof edges / sizeof edges[0])];
    if ((r & 7) < 5) {               /* a number of moderate size */
        u32 e = 100 + (rnd() % 56);
        return (rnd() & 0x80000000u) | (e << 23) | (rnd() & 0x7fffffu);
    }
    return rnd();
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    failures++;
    if (failures > 60 && !verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- the translated code's host side ------------------------------------ */

static int trapped;

void rt_fpu_trap(sh4 *c, u32 at)
{
    trapped = 1;
    c->pc = at;
    machine_fpu_exception(c);
}

void rt_host_fault(sh4 *c, int what, u32 at, u32 detail);
void rt_host_fault(sh4 *c, int what, u32 at, u32 detail)
{
    if (!c->stop)
        fail("translated code faulted (%d) at %08x (%08x)", what, at, detail);
    c->stop = STOP_ILLEGAL;
    c->pc = 0xdeadbeefu;
}

void rt_trapa(sh4 *c, u32 imm, u32 at);
void rt_trapa(sh4 *c, u32 imm, u32 at) { rt_host_fault(c, 100, at, imm); }
void rt_sleep(sh4 *c, u32 at);
void rt_sleep(sh4 *c, u32 at) { rt_host_fault(c, 101, at, 0); }
/* the only event in these tests: the translated run's cycle budget is spent */
static jmp_buf escape;
static int escape_set;
void rt_event(sh4 *c, u32 at);
void rt_event(sh4 *c, u32 at)
{
    (void)at;
    if (escape_set && c->cycles >= c->next_event)
        longjmp(escape, 1);
}

/* routines the translated side entered (RT_ENTRY, lockstep_hooks.h) */
static unsigned char *entered;
static long entered_count;
void lockstep_entered(unsigned addr)
{
    if (!entered || rt_table != M_routines)
        return;
    unsigned lo = 0, hi = M_routine_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (M_routines[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < M_routine_count && M_routines[lo].addr == addr && !entered[lo]) {
        entered[lo] = 1;
        entered_count++;
    }
}

/* ---- state ---------------------------------------------------------------- */

/* the fields that must agree (not the interpreter's bookkeeping: next_event,
 * pending_irq, stop, stop_arg, in_slot) */
static int compare(const sh4 *a, const sh4 *b, char *why, size_t n)
{
#define F(x) do { if (a->x != b->x) { snprintf(why, n, #x " %08x/%08x", (u32)a->x, (u32)b->x); return 0; } } while (0)
    for (int i = 0; i < 16; i++)
        if (a->r[i] != b->r[i]) { snprintf(why, n, "r%d %08x/%08x", i, a->r[i], b->r[i]); return 0; }
    for (int i = 0; i < 8; i++)
        if (a->r_bank[i] != b->r_bank[i]) { snprintf(why, n, "r%d_bank %08x/%08x", i, a->r_bank[i], b->r_bank[i]); return 0; }
    F(pc); F(pr); F(sr); F(t); F(gbr); F(vbr); F(ssr); F(spc); F(sgr); F(dbr); F(mach); F(macl); F(fpul); F(fpscr);
    for (int i = 0; i < 16; i++) {
        if (a->fr[i].u != b->fr[i].u) { snprintf(why, n, "fr%d %08x/%08x", i, a->fr[i].u, b->fr[i].u); return 0; }
        if (a->xf[i].u != b->xf[i].u) { snprintf(why, n, "xf%d %08x/%08x", i, a->xf[i].u, b->xf[i].u); return 0; }
    }
    for (int q = 0; q < 2; q++)
        for (int i = 0; i < 8; i++)
            if (a->sq[q][i] != b->sq[q][i]) { snprintf(why, n, "sq%d[%d] %08x/%08x", q, i, a->sq[q][i], b->sq[q][i]); return 0; }
    F(cycles); F(insns);
#undef F
    return 1;
}

static void random_state(sh4 *c, const insn_case *k)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 16; i++)
        c->r[i] = k && k->mem ? BUF + 0x100 + (rnd() & 0x7ff8) : rnd_edge();
    for (int i = 0; i < 8; i++)
        c->r_bank[i] = k && k->mem ? BUF + 0x100 + (rnd() & 0x7ff8) : rnd_edge();
    if (k && k->indexed)
        c->r[0] = rnd() & 0xf8;
    u32 sr = SR_MD | (rnd() & (SR_IMASK | SR_Q | SR_M | SR_S | SR_RB));
    c->sr = sr;
    c->t = rnd() & 1;
    c->gbr = k && k->mem ? BUF + (rnd() & 0x3ff0) : rnd();
    c->vbr = 0x8c000000u;
    c->ssr = rnd(); c->spc = rnd(); c->sgr = rnd(); c->dbr = rnd();
    c->mach = rnd_edge(); c->macl = rnd_edge(); c->fpul = (rnd() & 1) ? rnd_float() : rnd_edge();
    u32 fp = rnd() & 1;                 /* round to nearest or to zero */
    fp |= rnd() & (FPSCR_FLAG | FPSCR_CAUSE);
    fp |= (rnd() & 1) ? FPSCR_DN : 0;
    fp |= (rnd() & 1) ? FPSCR_FR : 0;
    if ((rnd() & 7) == 0)
        fp |= rnd() & FPSCR_ENABLE;     /* sometimes an exception that traps */
    int sz = rnd() & 1, pr = sz ? 0 : rnd() & 1;    /* SZ and PR both set is reserved */
    if (k && k->mode >= 0 && k->mode <= 1) {
        if (k->field == 1) { sz = k->mode; if (sz) pr = 0; }
        else               { pr = k->mode; if (pr) sz = 0; }
    }
    fp |= (sz ? FPSCR_SZ : 0) | (pr ? FPSCR_PR : 0);
    c->fpscr = fp;
    for (int i = 0; i < 16; i++) {
        c->fr[i].u = rnd_float();
        c->xf[i].u = rnd_float();
    }
    for (int q = 0; q < 2; q++)
        for (int i = 0; i < 8; i++)
            c->sq[q][i] = rnd();
    c->next_event = ~0ull;
}

/* ---- part 1: single instructions ----------------------------------------- */

static u8 buf_before[BUF_LEN], buf_interp[BUF_LEN];

static void test_insns(int states)
{
    u32 code = insn_code_addr;
    for (u32 i = 4; i < insn_pool_size; i++)
        g_w8(code + i, insn_pool[i]);
    long before = failures;
    for (unsigned n = 0; n < insn_case_count; n++) {
        const insn_case *k = &insn_cases[n];
        int shown = 0;
        g_w16(code, k->op);
        g_w16(code + 2, 0x0009);
        for (int s = 0; s < states; s++) {
            sh4 init, a, b;
            random_state(&init, k);
            for (u32 i = 0; i < BUF_LEN; i += 4) {
                u32 v = rnd();
                memcpy(buf_before + i, &v, 4);
            }
            memcpy(mem_ptr(BUF), buf_before, BUF_LEN);

            a = init;
            a.pc = code;
            sh4_step(&a);
            memcpy(buf_interp, mem_ptr(BUF), BUF_LEN);
            int a_stopped = a.stop;

            memcpy(mem_ptr(BUF), buf_before, BUF_LEN);
            b = init;
            case_fpscr_loaded((struct sh4 *)&b);
            trapped = 0;
            k->fn((struct sh4 *)&b);
            if (!trapped && !b.stop)
                b.pc = code + 2;
            checks++;
            if (a_stopped) {
                /* the interpreter refused (an instruction it does not model
                 * here): not a comparison */
                checks--;
                skipped++;
                continue;
            }
            traps += trapped;
            char why[96];
            if (!compare(&a, &b, why, sizeof why)) {
                if (!shown++)
                    fail("%04x %-28s mode %d state %d: %s (interpreter/translation)", k->op, k->text, k->mode, s, why);
                continue;
            }
            if (memcmp(buf_interp, mem_ptr(BUF), BUF_LEN)) {
                u32 o = 0;
                while (buf_interp[o] == mem_ptr(BUF)[o]) o++;
                if (!shown++)
                    fail("%04x %-28s mode %d state %d: memory at %08x %02x/%02x", k->op, k->text, k->mode, s,
                         BUF + o, buf_interp[o], mem_ptr(BUF)[o]);
                failures++;
            }
        }
    }
    printf("instructions: %u cases x %d states, %ld failed (%ld FPU traps compared, %ld states the interpreter "
           "stopped on, not compared)\n", insn_case_count, states, failures - before, traps, skipped);
}

/* ---- part 2: routines of the image ---------------------------------------- */

static u8 *ram_init, *ram_interp;

static const lockstep_routine *find_routine(const char *name)
{
    for (unsigned i = 0; i < M_routine_count; i++)
        if (!strcmp(M_routines[i].name, name))
            return &M_routines[i];
    return NULL;
}

/* an argument of a kind routines.txt names after the routine:
 *   p  an address in the scratch buffer (sometimes unaligned)
 *   n  a length, at most 2 KiB
 *   b  a byte
 *   i  any number (edge values often)
 * no kinds: each argument is one of those at random */
static u32 arg_of(char kind)
{
    switch (kind) {
    case 'p': return BUF + (rnd() & 0x7ff0) + ((rnd() & 1) ? (rnd() & 3) : 0);
    case 'n': return (rnd() & 1) ? rnd() % 2048 : rnd() % 17;
    case 'b': return rnd() & 0xff;
    case 'i': return rnd_edge();
    default:  return 0;
    }
}

static void routine_state(sh4 *c, const char *kinds)
{
    random_state(c, NULL);
    /* the game's own conditions: privileged, bank 0, interrupts masked,
     * FPSCR as its start-up code leaves it (DN, round to nearest), no traps */
    c->sr = SR_MD | SR_IMASK;
    c->fpscr = FPSCR_DN | (rnd() & FPSCR_FLAG);
    c->r[15] = STACK;
    /* arguments: small numbers, numbers, addresses of scratch memory, floats */
    size_t nk = strlen(kinds);
    for (int i = 4; i < 8; i++) {
        if (nk) {
            c->r[i] = arg_of((size_t)(i - 4) < nk ? kinds[i - 4] : 'i');
            continue;
        }
        switch (rnd() & 3) {
        case 0: c->r[i] = rnd() & 0xff; break;
        case 1: c->r[i] = rnd_edge(); break;
        default: c->r[i] = BUF + (rnd() & 0xfff0); break;
        }
    }
}

static void test_routine(const lockstep_routine *r, const char *kinds, int runs)
{
    volatile int shown = 0;
    for (volatile int n = 0; n < runs; n++) {
        static sh4 init, a, b;      /* static: b is read after a longjmp */
        routine_state(&init, kinds);
        for (u32 i = 0; i < BUF_LEN; i += 4) {
            u32 v = (rnd() & 1) ? rnd() & 0x7f7f7f7fu : rnd();
            g_w32(BUF + i, v);
        }
        memcpy(ram_init, mem_ptr(RAM_BASE), RAM_SIZE);

        a = init;
        a.pc = r->addr;
        a.pr = SH4_RETURN_MAGIC;
        a.stop = 0;
        while (!a.stop && a.cycles < BUDGET) {
            a.next_event = a.cycles + 1000000ull;
            sh4_run(&a);
        }
        a.next_event = ~0ull;
        if (!a.stop) {
            too_long++;
            memcpy(mem_ptr(RAM_BASE), ram_init, RAM_SIZE);
            continue;
        }
        memcpy(ram_interp, mem_ptr(RAM_BASE), RAM_SIZE);
        if (a.stop != STOP_RETURN) {
            if (verbose)
                printf("  %s run %d: the interpreter stopped: %s %s\n", r->name, n, sh4_stop_name(a.stop), machine_stop_text);
            memcpy(mem_ptr(RAM_BASE), ram_init, RAM_SIZE);
            continue;
        }

        memcpy(mem_ptr(RAM_BASE), ram_init, RAM_SIZE);
        b = init;
        b.pc = r->addr;
        b.pr = SH4_RETURN_MAGIC;
        b.stop = 0;
        case_fpscr_loaded((struct sh4 *)&b);
        trapped = 0;
        b.next_event = a.cycles + 1000;         /* past where the interpreter returned: diverged */
        escape_set = 1;
        if (setjmp(escape)) {
            escape_set = 0;
            checks++;
            if (!shown++)
                fail("%s run %d: the translation ran past %llu instructions, the interpreter returned after %llu",
                     r->name, n, (unsigned long long)b.cycles, (unsigned long long)a.cycles);
            memcpy(mem_ptr(RAM_BASE), ram_init, RAM_SIZE);
            continue;
        }
        r->fn((struct sh4 *)&b);
        escape_set = 0;
        b.next_event = ~0ull;
        checks++;
        char why[96];
        a.stop = b.stop = 0;
        if (!compare(&a, &b, why, sizeof why)) {
            if (!shown++)
                fail("%s run %d: %s (interpreter/translation)", r->name, n, why);
        } else if (memcmp(ram_interp, mem_ptr(RAM_BASE), RAM_SIZE)) {
            u32 o = 0;
            while (ram_interp[o] == mem_ptr(RAM_BASE)[o]) o++;
            if (!shown++)
                fail("%s run %d: RAM at %08x %02x/%02x", r->name, n, RAM_BASE + o, ram_interp[o], mem_ptr(RAM_BASE)[o]);
            failures++;
        }
        memcpy(mem_ptr(RAM_BASE), ram_init, RAM_SIZE);
    }
}

static int test_routines(const char *list, int runs)
{
    FILE *f = fopen(list, "r");
    if (!f) {
        printf("cannot read %s\n", list);
        return 1;
    }
    char line[256];
    long before = failures, before_checks = checks;
    int count = 0, missing = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line + strspn(line, " \t");
        if (!*p || *p == '#' || *p == '\n')
            continue;
        char *k = p + strcspn(p, " \t\r\n");
        if (*k && *k != '\n' && *k != '\r') {
            *k++ = 0;
            k += strspn(k, " \t");
        }
        k[strcspn(k, " \t\r\n")] = 0;
        p[strcspn(p, " \t\r\n")] = 0;
        const lockstep_routine *r = find_routine(p);
        if (!r) {
            printf("  no routine %s\n", p);
            missing++;
            continue;
        }
        long f0 = failures;
        test_routine(r, k, runs);
        if (verbose)
            printf("  %-32s %s\n", p, failures > f0 ? "FAILED" : "ok");
        count++;
    }
    fclose(f);
    printf("routines of %s: %d routines, %ld runs compared, %ld failed%s; runs over the budget so far: %ld\n", list,
           count, checks - before_checks, failures - before, missing ? " (some names unknown)" : "", too_long);
    return 0;
}

int main(int argc, char **argv)
{
    int states = 32, runs = 64, argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (!strcmp(argv[argi], "-s") && argi + 1 < argc) states = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-r") && argi + 1 < argc) runs = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-v")) verbose = 1;
        else { printf("usage: lockstep [-s states] [-r runs] [-v] <routine-list>...\n"); return 2; }
    }
    if (rt_sh4_size != sizeof(sh4)) {
        printf("the recomp's struct sh4 (%u bytes) is not VF4debug's (%u)\n", rt_sh4_size, (unsigned)sizeof(sh4));
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *image = getenv("VF4_IMAGE");
    memset(&cfg, 0, sizeof cfg);
    cfg.quiet = 1;
    cfg.image = image;
    if (mem_init() || mem_add_ram("main-ram", RAM_BASE, RAM_SIZE))
        return 2;
    if (disp_init())
        return 2;
    machine_init();
    hw_init(&cpu);
    ram_init = malloc(RAM_SIZE);
    ram_interp = malloc(RAM_SIZE);
    if (!ram_init || !ram_interp)
        return 2;

    test_insns(states);
    if (argi < argc) {
        u32 loaded = 0;
        if (!image || mem_load_file(image, IMAGE_BASE, 0x340000, &loaded) || loaded != 0x340000) {
            printf("routines: need VF4_IMAGE, the main program image\n");
            return 2;
        }
        rt_table = M_routines;
        rt_table_count = M_routine_count;
        entered = calloc(M_routine_count, 1);
        for (; argi < argc; argi++)
            if (test_routines(argv[argi], runs))
                return 2;
    }
    if (entered)
        printf("routines the translation entered: %ld of %u\n", entered_count, M_routine_count);
    printf("%ld checks, %ld failed\n", checks, failures);
    return failures ? 1 : 0;
}
