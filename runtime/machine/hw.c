/* Accesses outside RAM: the processor's own control registers, the Holly
 * system registers, the store queues. Each access is recorded with the
 * routine that made it, modelled or not, so that the next replacement or
 * model is obvious from the report.
 *
 * What is modelled here is written from public hardware documentation
 * (the SH7750 hardware manual; the register maps of the Dreamcast/Naomi
 * system chip as documented by the homebrew community) and from what the
 * decompiled code does with each register. No emulator code was used.
 */
#include "hw.h"
#include "observe.h"
#include "mem.h"
#include "dispatch.h"
#include "machine.h"

#include <stdlib.h>
#include <string.h>

hw_state hw;
gfx_sink_fn gfx_sink;
u64 gfx_sq_blocks, gfx_sq_blocks_frame;

/* ---- the access report ---- */

typedef struct access_row {
    u32 addr;
    u32 fn;             /* entry of the routine, or the raw pc outside the image */
    u8 is_write, modelled, size, used;
    u32 count;
    u32 first_val, last_val;
    u32 first_frame;
} access_row;

#define ACCESS_ROWS 8192
static access_row access_tab[ACCESS_ROWS];
static int access_used, access_overflow;

int hw_sandbox_unmodelled;

static void note_access(sh4 *c, u32 addr, u32 val, int size, int is_write, int modelled)
{
    if (obs_sandbox) {
        /* run by the harness in a sandbox (the controller's "call"): not the game's doing,
         * and not in the table of what the game touched */
        hw_sandbox_unmodelled += !modelled;
        return;
    }
    u32 pc = c->pc - 2;
    routine *r = routine_containing(pc);
    u32 fn = r ? r->addr : pc;
    u32 h = (addr * 2654435761u) ^ (fn * 40503u) ^ (u32)is_write;
    h &= ACCESS_ROWS - 1;
    for (int probe = 0; probe < ACCESS_ROWS; probe++) {
        access_row *a = &access_tab[(h + (u32)probe) & (ACCESS_ROWS - 1)];
        if (!a->used) {
            if (access_used >= ACCESS_ROWS * 3 / 4) {
                access_overflow++;
                return;
            }
            a->used = 1;
            a->addr = addr;
            a->fn = fn;
            a->is_write = (u8)is_write;
            a->modelled = (u8)modelled;
            a->size = (u8)size;
            a->count = 1;
            a->first_val = a->last_val = val;
            a->first_frame = machine_frame;
            access_used++;
            if (cfg.hw_trace || (!modelled && cfg.verbose))
                fprintf(stderr, "[hw] %s %08x %s %0*x  by %s%s\n", is_write ? "write" : "read ", addr,
                        is_write ? "<-" : "->", size * 2, val, addr_text(pc), modelled ? "" : "  (no model)");
            return;
        }
        if (a->addr == addr && a->fn == fn && a->is_write == is_write) {
            a->count++;
            a->last_val = val;
            if (cfg.hw_trace > 1)
                fprintf(stderr, "[hw] %s %08x %s %0*x  by %s\n", is_write ? "write" : "read ", addr,
                        is_write ? "<-" : "->", size * 2, val, addr_text(pc));
            return;
        }
    }
}

static int access_cmp(const void *a, const void *b)
{
    const access_row *x = a, *y = b;
    if (x->modelled != y->modelled)
        return x->modelled - y->modelled;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    if (x->fn != y->fn)
        return x->fn < y->fn ? -1 : 1;
    return x->is_write - y->is_write;
}

int hw_unmodelled_count(void)
{
    int n = 0;
    for (int i = 0; i < ACCESS_ROWS; i++)
        if (access_tab[i].used && !access_tab[i].modelled)
            n++;
    return n;
}

void hw_report(FILE *f, int unmodelled_only)
{
    access_row *rows = malloc(sizeof access_tab);
    int n = 0;
    for (int i = 0; i < ACCESS_ROWS; i++)
        if (access_tab[i].used && !(unmodelled_only && access_tab[i].modelled))
            rows[n++] = access_tab[i];
    qsort(rows, (size_t)n, sizeof *rows, access_cmp);
    fprintf(f, "model\taddress\taccess\tsize\tcount\tfirst value\tlast value\tfirst frame\troutine\n");
    for (int i = 0; i < n; i++) {
        access_row *a = &rows[i];
        fprintf(f, "%s\t%08x\t%s\t%d\t%u\t%08x\t%08x\t%u\t%s\n", a->modelled ? "yes" : "NO", a->addr,
                a->is_write ? "write" : "read", a->size, a->count, a->first_val, a->last_val, a->first_frame,
                addr_text(a->fn));
    }
    if (access_overflow)
        fprintf(f, "# %d accesses not recorded: table full\n", access_overflow);
    free(rows);
}

/* ---- the generic register store ---- */

static u32 *reg_slot(u32 addr, int make)
{
    u32 n = sizeof hw.reg / sizeof hw.reg[0];
    u32 h = (addr * 2654435761u) >> 20;
    for (u32 probe = 0; probe < n; probe++) {
        u32 i = (h + probe) & (n - 1);
        if (hw.reg[i].used && hw.reg[i].addr == addr)
            return &hw.reg[i].val;
        if (!hw.reg[i].used) {
            if (!make || hw.reg_count >= n * 3 / 4)
                return NULL;
            hw.reg[i].used = 1;
            hw.reg[i].addr = addr;
            hw.reg[i].val = 0;
            hw.reg_count++;
            return &hw.reg[i].val;
        }
    }
    return NULL;
}

/* Registers that are kept as written and read back, on purpose: nothing
 * behind them matters to a machine without the real buses, and the game
 * only sets them up. An access outside these ranges and outside the models
 * is reported as having no model. */
static const struct {
    u32 lo, hi;
    int keep;           /* 1: reads give back what was written; 0: reads give 0, writes vanish */
    const char *what;
} plain_regions[] = {
    { 0x005f6800u, 0x005f68ffu, 1, "system bus control" },
    { 0x005f7000u, 0x005f70ffu, 1, "cartridge board control" },
    { 0x005f7400u, 0x005f74ffu, 1, "G1 bus timing" },
    { 0x005f7800u, 0x005f78ffu, 1, "G2 bus DMA" },
    { 0x025f6800u, 0x025f68ffu, 1, "second chip: system bus control" },
    { 0x025f6900u, 0x025f695fu, 1, "second chip: external interrupt masks and DMA triggers (what its model leaves)" },
    { 0x00600000u, 0x006fffffu, 0, "G2 expansion port: nothing fitted" },
    { 0x01000000u, 0x01ffffffu, 0, "G2 expansion port: nothing fitted" },
    { 0x10000000u, 0x13ffffffu, 0, "list and texture input of the first rendering chip" },
    { 0xf0000000u, 0xf7ffffffu, 0, "cache address and data arrays: there are no caches" },
    { 0xff000000u, 0xff0000ffu, 1, "processor: MMU and cache control" },
    { 0xff200000u, 0xff2000ffu, 1, "processor: user break controller (no breaks are raised)" },
    { 0xff800000u, 0xff80ffffu, 1, "processor: bus state controller" },
    { 0xffa00000u, 0xffa000ffu, 1, "processor: DMA controller" },
    { 0xffc00000u, 0xffc8ffffu, 1, "processor: clock, watchdog, real-time clock" },
    { 0xffe00000u, 0xffe8ffffu, 1, "processor: serial ports" },
};

static int plain_region(u32 addr)
{
    for (unsigned i = 0; i < sizeof plain_regions / sizeof plain_regions[0]; i++)
        if (addr >= plain_regions[i].lo && addr <= plain_regions[i].hi)
            return (int)i;
    return -1;
}

static u32 reg_get(u32 addr)
{
    u32 *p = reg_slot(addr, 0);
    return p ? *p : 0;
}

static void reg_set(u32 addr, u32 val)
{
    u32 *p = reg_slot(addr, 1);
    if (p)
        *p = val;
}

/* ---- interrupts ---- */

int hw_irq_pending(sh4 *c, u32 *intevt)
{
    (void)c;
    int level = 0;
    u32 code = 0;
    /* the system chip drives three request lines; by the hardware manual's
     * table for the encoded IRL inputs they are levels 6, 4 and 2 */
    if ((hw.irq_normal & hw.mask6_normal) || (hw.irq_external & hw.mask6_external) || (hw.irq_error & hw.mask6_error)) {
        level = 6;
        code = 0x320;
    } else if ((hw.irq_normal & hw.mask4_normal) || (hw.irq_external & hw.mask4_external) || (hw.irq_error & hw.mask4_error)) {
        level = 4;
        code = 0x360;
    } else if ((hw.irq_normal & hw.mask2_normal) || (hw.irq_external & hw.mask2_external) || (hw.irq_error & hw.mask2_error)) {
        level = 2;
        code = 0x3a0;
    }
    if (level < 6 && hw_gfx_irq_level() == 6) {
        level = 6;
        code = 0x320;
    }
    /* timer underflows: priority from IPRA */
    for (int ch = 0; ch < 3; ch++) {
        if ((hw.tmu[ch].tcr & 0x120) == 0x120) {       /* UNF and UNIE */
            int pri = (int)((hw.ipra >> (12 - 4 * ch)) & 15);
            if (pri > level) {
                level = pri;
                code = 0x400 + 0x20 * (u32)ch;
            }
        }
    }
    if (intevt)
        *intevt = code;
    return level;
}

void hw_irq_update(sh4 *c)
{
    c->pending_irq = (u32)hw_irq_pending(c, NULL);
    if (c->pending_irq)
        c->next_event = 0;
}

void hw_raise(sh4 *c, u32 bits)
{
    hw.irq_normal |= bits;
    hw_irq_update(c);
}

void hw_raise_ext(sh4 *c, u32 bits)
{
    hw.irq_external |= bits;
    hw_irq_update(c);
}

/* ---- timers ---- */

static u32 tmu_divisor(u32 tcr)
{
    /* the peripheral clock is a quarter of the processor clock */
    switch (tcr & 7) {
    case 0: return 4 * 4;
    case 1: return 4 * 16;
    case 2: return 4 * 64;
    case 3: return 4 * 256;
    default: return 4 * 1024;
    }
}

static void tmu_event(sh4 *c, u32 ch);

static void tmu_sync(sh4 *c, int ch)
{
    if (!hw.tmu[ch].running) {
        hw.tmu[ch].synced = c->cycles;
        return;
    }
    u32 div = tmu_divisor(hw.tmu[ch].tcr);
    u64 ticks = (c->cycles - hw.tmu[ch].synced) / div;
    hw.tmu[ch].synced += ticks * div;
    if (ticks <= hw.tmu[ch].tcnt) {
        hw.tmu[ch].tcnt -= (u32)ticks;
    } else {
        u64 over = ticks - hw.tmu[ch].tcnt - 1;
        u64 period = (u64)hw.tmu[ch].tcor + 1;
        hw.tmu[ch].tcnt = hw.tmu[ch].tcor - (u32)(over % period);
        hw.tmu[ch].tcr |= 0x100;                       /* UNF */
    }
}

static void tmu_schedule(sh4 *c, int ch)
{
    if (hw.tmu[ch].running && (hw.tmu[ch].tcr & 0x20)) {
        u64 at = hw.tmu[ch].synced + ((u64)hw.tmu[ch].tcnt + 1) * tmu_divisor(hw.tmu[ch].tcr);
        event_add(at, tmu_event, (u32)ch, "timer underflow");
    }
    (void)c;
}

static void tmu_event(sh4 *c, u32 ch)
{
    tmu_sync(c, (int)ch);
    hw_irq_update(c);
    if (!(hw.tmu[ch].tcr & 0x100))
        return;                 /* stale: the timer was reprogrammed */
    tmu_schedule(c, (int)ch);
}

u32 hw_tmu_count(sh4 *c, int ch)
{
    tmu_sync(c, ch);
    return hw.tmu[ch].tcnt;
}

/* ---- the processor's own registers (0xff000000 and up) ---- */

static int cpu_reg_read(sh4 *c, u32 a, u32 *out)
{
    switch (a) {
    case 0xff00001c: *out = hw.ccr; return 1;
    case 0xff000020: *out = hw.tra; return 1;
    case 0xff000024: *out = hw.expevt; return 1;
    case 0xff000028: *out = hw.intevt; return 1;
    case 0xff000038: *out = hw.qacr[0]; return 1;
    case 0xff00003c: *out = hw.qacr[1]; return 1;
    case 0xffd00000: *out = hw.icr; return 1;
    case 0xffd00004: *out = hw.ipra; return 1;
    case 0xffd00008: *out = hw.iprb; return 1;
    case 0xffd0000c: *out = hw.iprc; return 1;
    case 0xffd80000: *out = hw.tocr; return 1;
    case 0xffd80004: *out = hw.tstr; return 1;
    }
    if (a >= 0xffd80008 && a < 0xffd8002c) {
        int ch = (int)((a - 0xffd80008) / 12), which = (int)((a - 0xffd80008) % 12) / 4;
        tmu_sync(c, ch);
        *out = which == 0 ? hw.tmu[ch].tcor : which == 1 ? hw.tmu[ch].tcnt : hw.tmu[ch].tcr;
        return 1;
    }
    return hw_dmac_read(a, out);
}

static int cpu_reg_write(sh4 *c, u32 a, u32 v)
{
    switch (a) {
    case 0xff00001c: hw.ccr = v & ~0x808u; return 1;     /* the two cache-flush bits read as 0 */
    case 0xff000020: hw.tra = v; return 1;
    case 0xff000024: hw.expevt = v; return 1;
    case 0xff000028: hw.intevt = v; return 1;
    case 0xff000038: hw.qacr[0] = v; return 1;
    case 0xff00003c: hw.qacr[1] = v; return 1;
    case 0xffd00000: hw.icr = v; return 1;
    case 0xffd00004: hw.ipra = v; hw_irq_update(c); return 1;
    case 0xffd00008: hw.iprb = v; return 1;
    case 0xffd0000c: hw.iprc = v; return 1;
    case 0xffd80000: hw.tocr = v; return 1;
    case 0xffd80004:
        for (int ch = 0; ch < 3; ch++) {
            tmu_sync(c, ch);
            hw.tmu[ch].running = (v >> ch) & 1;
            hw.tmu[ch].synced = c->cycles;
            tmu_schedule(c, ch);
        }
        hw.tstr = v & 7;
        return 1;
    }
    if (a >= 0xffd80008 && a < 0xffd8002c) {
        int ch = (int)((a - 0xffd80008) / 12), which = (int)((a - 0xffd80008) % 12) / 4;
        tmu_sync(c, ch);
        if (which == 0)
            hw.tmu[ch].tcor = v;
        else if (which == 1)
            hw.tmu[ch].tcnt = v;
        else
            hw.tmu[ch].tcr = (v & 0x3f) | (hw.tmu[ch].tcr & v & 0x100);   /* UNF is cleared by writing 0 */
        hw.tmu[ch].synced = c->cycles;
        tmu_schedule(c, ch);
        hw_irq_update(c);
        return 1;
    }
    return hw_dmac_write(a, v);
}

/* ---- the system chip (physical 0x005f6800-0x005f9fff) ---- */

static int holly_read(sh4 *c, u32 p, u32 *out)
{
    switch (p) {
    case 0x005f6900:
        *out = hw.irq_normal | (hw.irq_external ? 0x40000000u : 0) | (hw.irq_error ? 0x80000000u : 0);
        return 1;
    case 0x005f6904: *out = hw.irq_external; return 1;
    case 0x005f6908: *out = hw.irq_error; return 1;
    case 0x005f6910: *out = hw.mask2_normal; return 1;
    case 0x005f6914: *out = hw.mask2_external; return 1;
    case 0x005f6918: *out = hw.mask2_error; return 1;
    case 0x005f6920: *out = hw.mask4_normal; return 1;
    case 0x005f6924: *out = hw.mask4_external; return 1;
    case 0x005f6928: *out = hw.mask4_error; return 1;
    case 0x005f6930: *out = hw.mask6_normal; return 1;
    case 0x005f6934: *out = hw.mask6_external; return 1;
    case 0x005f6938: *out = hw.mask6_error; return 1;
    }
    if (p >= 0x005f6c00 && p < 0x005f6d00)
        return hw_maple_read(c, p, out);
    if (hw_cart_read(c, p, out) || hw_sound_read(c, p, out))
        return 1;
    return hw_gfx_read(c, p, out);
}

static int holly_write(sh4 *c, u32 p, u32 v)
{
    switch (p) {
    case 0x005f6900: hw.irq_normal &= ~v; break;            /* written ones acknowledge */
    case 0x005f6904: break;                             /* external lines: cleared at their source */
    case 0x005f6908: hw.irq_error &= ~v; break;
    case 0x005f6910: hw.mask2_normal = v; break;
    case 0x005f6914: hw.mask2_external = v; break;
    case 0x005f6918: hw.mask2_error = v; break;
    case 0x005f6920: hw.mask4_normal = v; break;
    case 0x005f6924: hw.mask4_external = v; break;
    case 0x005f6928: hw.mask4_error = v; break;
    case 0x005f6930: hw.mask6_normal = v; break;
    case 0x005f6934: hw.mask6_external = v; break;
    case 0x005f6938: hw.mask6_error = v; break;
    default:
        if (p >= 0x005f6c00 && p < 0x005f6d00)
            return hw_maple_write(c, p, v);
        if (hw_cart_write(c, p, v) || hw_sound_write(c, p, v))
            return 1;
        return hw_gfx_write(c, p, v);
    }
    hw_irq_update(c);
    return 1;
}

/* ---- entry points ---- */

/* The first 128 KiB of RAM belong to the BIOS. The game reads things the
 * BIOS left there; a read of a word nobody has written yet is reported as
 * an access without a model, so that what is missing shows up. */
#define LOW_RAM_SIZE 0x20000u
static u8 low_ram_written[LOW_RAM_SIZE / 4];

static int in_low_ram(u32 addr)
{
    return (addr & 0x1fffffffu) - RAM_BASE < LOW_RAM_SIZE;
}

/* what the BIOS or the DIMM board leaves in low RAM: planted values */
void low_ram_w32(u32 addr, u32 v)
{
    mem_planting++;
    g_w32(addr, v);
    mem_planting--;
    low_ram_written[((addr & 0x1fffffffu) - RAM_BASE) / 4] = 1;
}

void low_ram_w16(u32 addr, u32 v)
{
    mem_planting++;
    g_w16(addr, v);
    mem_planting--;
    low_ram_written[((addr & 0x1fffffffu) - RAM_BASE) / 4] = 1;
}

static u32 ram_slow_read(sh4 *c, u32 addr, int size)
{
    u8 *p = mem_ptr(addr);
    u32 v = size == 4 ? *(u32 *)p : size == 2 ? *(u16 *)p : *p;
    if (in_low_ram(addr) && !low_ram_written[((addr & 0x1fffffffu) - RAM_BASE) / 4])
        note_access(c, addr, v, size, 0, 0);
    if (mem_watch_count)
        mem_watch_hit(c, addr, v, size, 0);
    return v;
}

static void ram_slow_write(sh4 *c, u32 addr, u32 val, int size)
{
    u8 *p = mem_ptr(addr);
    mem_track(addr);                        /* recomp */
    if (in_low_ram(addr))
        low_ram_written[((addr & 0x1fffffffu) - RAM_BASE) / 4] = 1;
    if (mem_plant_near(addr))
        mem_unplant(addr, (u32)size);       /* the game's own value from here on */
    if (size == 4)
        *(u32 *)p = val;
    else if (size == 2)
        *(u16 *)p = (u16)val;
    else
        *p = (u8)val;
    mem_watch_hit(c, addr, val, size, 1);
}

u32 io_read(sh4 *c, u32 addr, int size)
{
    u32 v = 0;
    if (mem_page_ram[addr >> 16])
        return ram_slow_read(c, addr, size);
    if (addr >= 0xe0000000u && addr < 0xe4000000u) {
        v = c->sq[(addr >> 5) & 1][(addr >> 2) & 7];
        note_access(c, addr & 0xffffffe0u, v, size, 0, 1);
        return v;
    }
    int ok;
    if (addr >= 0xff000000u || (addr >= 0x1f000000u && addr < 0x20000000u)) {
        ok = cpu_reg_read(c, addr | 0xe0000000u, &v);
    } else {
        u32 p = addr & 0x1fffffffu;
        ok = holly_read(c, p, &v);
    }
    if (!ok) {
        u32 key = addr >= 0xe0000000u ? addr : (addr & 0x1fffffffu);
        int region = plain_region(key);
        if (region >= 0) {
            ok = 1;
            v = plain_regions[region].keep ? reg_get(key) : 0;
        } else {
            v = reg_get(key);
        }
    }
    if (size == 2)
        v &= 0xffff;
    else if (size == 1)
        v &= 0xff;
    note_access(c, addr, v, size, 0, ok);
    return v;
}

void io_write(sh4 *c, u32 addr, u32 val, int size)
{
    if (mem_page_ram[addr >> 16]) {
        ram_slow_write(c, addr, val, size);
        return;
    }
    if (addr >= 0xe0000000u && addr < 0xe4000000u) {
        if (size == 4)
            c->sq[(addr >> 5) & 1][(addr >> 2) & 7] = val;
        else
            note_access(c, addr, val, size, 1, 0);
        return;
    }
    int ok;
    if (addr >= 0xff000000u || (addr >= 0x1f000000u && addr < 0x20000000u)) {
        ok = cpu_reg_write(c, addr | 0xe0000000u, val);
    } else {
        u32 p = addr & 0x1fffffffu;
        ok = holly_write(c, p, val);
    }
    if (!ok) {
        u32 key = addr >= 0xe0000000u ? addr : (addr & 0x1fffffffu);
        int region = plain_region(key);
        if (region >= 0) {
            ok = 1;
            if (plain_regions[region].keep)
                reg_set(key, val);
        } else {
            reg_set(key, val);
        }
    }
    note_access(c, addr, val, size, 1, ok);
}

void io_sq_write(sh4 *c, u32 dst, const u32 *words)
{
    if (hw_gfx_sq(c, dst, words)) {
        note_access(c, dst & 0xff000000u, words[0], 4, 1, 1);
        return;
    }
    gfx_sq_blocks++;
    gfx_sq_blocks_frame++;
    if (gfx_sink)
        gfx_sink(c, dst, words);
    note_access(c, dst & 0xfff00000u, words[0], 4, 1, 1);
}

void mem_watch_hit(sh4 *c, u32 addr, u32 val, int size, int is_write)
{
    u32 p = addr & 0x1fffffffu;
    for (int i = 0; i < mem_watch_count; i++) {
        mem_watch *w = &mem_watches[i];
        if (p + (u32)size <= w->addr || p >= w->addr + w->len)
            continue;
        if (is_write ? !w->on_write : !w->on_read)
            continue;
        machine_watch_report(c, w, addr, val, size, is_write);
    }
}

void hw_init(sh4 *c)
{
    (void)c;
    memset(&hw, 0, sizeof hw);
    memset(access_tab, 0, sizeof access_tab);
    access_used = access_overflow = 0;
    for (int ch = 0; ch < 3; ch++) {
        hw.tmu[ch].tcor = 0xffffffffu;
        hw.tmu[ch].tcnt = 0xffffffffu;
    }
    /* 2004-11-10 07:18:24 in the clock chip's count of seconds from
     * 1950-01-01 00:00:00: a fixed date, so that runs repeat exactly. (The
     * value was meant to be noon and is 16,896 seconds short of it; it is
     * kept, because the card and network scenarios' closing times and
     * deadlines are worked out from it.) */
    hw.rtc_base = 0x67305e40u;
    event_register(tmu_event, "timer underflow");
    hw_maple_init();
    hw_gfx_init();
    hw_sound_init();
    /* low RAM takes the slow path (see ram_slow_read) */
    memset(low_ram_written, 0, sizeof low_ram_written);
    for (u32 top = 0; top < 6; top++)
        for (u32 off = 0; off < LOW_RAM_SIZE; off += 0x10000)
            mem_page[((top << 29) | (RAM_BASE + off)) >> 16] = NULL,
            mem_always_slow[((top << 29) | (RAM_BASE + off)) >> 16] = 1;
    mem_wpage_sync();                   /* recomp */
    snapshot_register("low-ram-written", low_ram_written, sizeof low_ram_written);
    mem_plant_register();
}
