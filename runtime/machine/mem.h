/* The Naomi 2 address space inside this process.
 *
 * RAM areas are mapped at their own addresses (and at their 0x8... and
 * 0xa... mirrors), so every absolute pointer in the game image is also a
 * valid host pointer. Everything else (hardware registers, the processor's
 * own control registers, the store queues) goes through io_read / io_write
 * in hw.c.
 */
#ifndef VF4_MEM_H
#define VF4_MEM_H

#include "sh4.h"
#include <stddef.h>

#define RAM_BASE   0x0c000000u
#define RAM_SIZE   0x02000000u
#define IMAGE_BASE 0x0c020000u

/* One entry per 64 KiB of the 4 GiB space: host address of that page, or 0
 * when accesses there are handled by hw.c (io_read, io_write). */
extern u8 *mem_page[65536];
/* Same table without the watchpoint holes (for the slow path). */
extern u8 *mem_page_ram[65536];
/* Pages that take the slow path whatever the watchpoints say. */
extern u8 mem_always_slow[65536];
/* recomp: stores look in mem_wpage: mem_page, with NULL also at the pages
 * mem_code_page marks (where the recomp's runtime has translated code). A
 * store there goes to mem_code_write, which shows mem_code_store the new
 * bytes before it makes them (runtime/native/rt_native.c: changed code). With
 * no page marked, mem_wpage is mem_page and nothing changes. Every change to
 * mem_page or mem_code_page is followed by mem_wpage_sync. */
extern u8 *mem_wpage[65536];
extern u8 mem_code_page[65536];
extern void (*mem_code_store)(u32 addr, u32 n, const void *bytes);
void mem_wpage_sync(void);
void mem_code_write(sh4 *c, u32 a, u32 v, int size);
static inline void mem_code_note(u32 a, u32 n, const void *bytes)
{
    if (__builtin_expect(mem_code_store != NULL, 0) &&
        (mem_code_page[a >> 16] || mem_code_page[(a + n - 1) >> 16]))
        mem_code_store(a, n, bytes);
}

typedef struct mem_area {
    const char *name;
    u32 phys;         /* physical base (29-bit) */
    u32 size;
    int fd;           /* memfd backing it */
    u8 *host;         /* where it is mapped */
} mem_area;

#define MEM_MAX_AREAS 16
extern mem_area mem_areas[MEM_MAX_AREAS];
extern int mem_area_count;

int mem_init(void);
/* adds zeroed RAM at a physical address, mapped at the address itself and
 * its mirrors when the host allows; returns 0 on success */
int mem_add_ram(const char *name, u32 phys, u32 size);
/* a second window onto RAM already added */
int mem_alias(u32 phys_alias, u32 phys_src, u32 size);
/* a host page after each area, once all areas are there: see mem.c */
void mem_guards(void);
int mem_load_file(const char *path, u32 addr, u32 max, u32 *loaded);

/* slow paths (hw.c) */
u32 io_read(sh4 *c, u32 addr, int size);
void io_write(sh4 *c, u32 addr, u32 val, int size);
/* a 32-byte store-queue block sent to an address outside RAM */
void io_sq_write(sh4 *c, u32 dst, const u32 *words);

/* watchpoints: accesses to the pages they cover take the slow path */
typedef struct mem_watch {
    u32 addr, len;
    int on_read, on_write;
    int stop;         /* stop the run when hit */
    char name[48];
} mem_watch;
#define MEM_MAX_WATCH 32
extern mem_watch mem_watches[MEM_MAX_WATCH];
extern int mem_watch_count;
int mem_watch_add(u32 addr, u32 len, int on_read, int on_write, int stop, const char *name);
void mem_watch_clear(void);
void mem_watch_hit(sh4 *c, u32 addr, u32 val, int size, int is_write);

static inline u8 *mem_ptr(u32 a)
{
    u8 *p = mem_page_ram[a >> 16];
    return p ? p + (a & 0xffff) : NULL;
}

static inline u32 mem_r32(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1))
        return *(u32 *)(p + (a & 0xffff));
    return io_read(c, a, 4);
}
static inline u32 mem_r16(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1))
        return *(u16 *)(p + (a & 0xffff));
    return io_read(c, a, 2);
}
static inline u32 mem_r8(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1))
        return p[a & 0xffff];
    return io_read(c, a, 1);
}
static inline void mem_w32(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];         /* recomp: mem_wpage */
    if (__builtin_expect(p != NULL, 1))
        *(u32 *)(p + (a & 0xffff)) = v;
    else
        mem_code_write(c, a, v, 4);     /* recomp: io_write when not a code page */
}
static inline void mem_w16(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];         /* recomp: mem_wpage */
    if (__builtin_expect(p != NULL, 1))
        *(u16 *)(p + (a & 0xffff)) = (u16)v;
    else
        mem_code_write(c, a, v, 2);     /* recomp */
}
static inline void mem_w8(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];         /* recomp: mem_wpage */
    if (__builtin_expect(p != NULL, 1))
        p[a & 0xffff] = (u8)v;
    else
        mem_code_write(c, a, v, 1);     /* recomp */
}

/* access for host code (replacements, the controller): RAM only, no side
 * effects; reads of unmapped addresses give 0 */
u32 g_r32(u32 a);
u32 g_r16(u32 a);
u32 g_r8(u32 a);
void g_w32(u32 a, u32 v);
void g_w16(u32 a, u32 v);
void g_w8(u32 a, u32 v);
/* called before host code stores into guest memory through the accessors
 * above (the observer's journal of who wrote what); NULL when unused */
extern void (*mem_host_store)(u32 addr, u32 n);
/* Planted values: bytes a model put into the machine's memory in place of
 * what the real machine's BIOS, DIMM board or sound driver would have left
 * there (the BIOS's words in low RAM and what its services hand back, the
 * sound driver's words in sound RAM). A byte stays marked until the game
 * stores to it itself; the call log flags a value read from a marked byte
 * as the model's, as it does a value read from a hardware register.
 * Low RAM and the first 64 KiB of sound RAM are exact always (they take the
 * slow path); elsewhere the marks follow the game's stores only while the
 * observer is on. */
#define PLANT_PAGES 48
extern u8 mem_plant_page[8192];       /* per 64 KiB of the 29-bit space: 0, or which map + 1 */
extern int mem_planting;              /* while not 0, what host code stores through g_w* is planted */
void mem_plant(u32 addr, u32 len);
void mem_unplant(u32 addr, u32 len);
int mem_planted(u32 addr, u32 len);
void mem_plant_register(void);      /* makes the marks part of snapshots */
static inline int mem_plant_near(u32 addr) { return mem_plant_page[(addr >> 16) & 0x1fff] != 0; }
/* copies; return 0 when the whole range is RAM */
int g_read(u32 a, void *dst, u32 n);
int g_write(u32 a, const void *src, u32 n);
/* a NUL-terminated guest string into buf (always terminated) */
void g_str(u32 a, char *buf, size_t n);

#endif
