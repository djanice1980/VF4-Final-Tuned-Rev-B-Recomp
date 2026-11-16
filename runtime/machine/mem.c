/* The address space: RAM areas mapped at their own addresses, a page table
 * for the interpreter, watchpoints, host-side accessors. */
#define _GNU_SOURCE
#include "mem.h"
#include "machine.h"

#include <errno.h>
#include <stdlib.h>          /* recomp: mem_track tables */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32                          /* recomp: Windows, docs/design.md section 14 */
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

u8 *mem_page[65536];
u8 *mem_page_ram[65536];
u8 mem_always_slow[65536];
u8 *mem_wpage[65536];                   /* recomp: see mem.h */
u8 mem_code_page[65536];                /* recomp */
void (*mem_code_store)(u32 addr, u32 n, const void *bytes);    /* recomp */

static u8 track_armed[65536];           /* recomp: mem_track.h; per 64 KiB of the address space */

/* recomp: mem_wpage from mem_page, the code pages and the pages armed for tracking */
void mem_wpage_sync(void)
{
    for (u32 p = 0; p < 65536; p++)
        mem_wpage[p] = mem_code_page[p] || track_armed[p] ? NULL : mem_page[p];
}

/* ---- recomp: tracking the pages a frame writes (mem_track.h) ---- */

int mem_tracking;
static mem_track_fn track_fn;
static u32 track_n;                     /* tracked pages: every area's, 64 KiB each */
static u16 track_of[65536];             /* per 64 KiB of the address space: its page + 1, or 0 */
static u8 **track_host;                 /* per page: its bytes */
static u32 *track_first, *track_cnt;    /* per page: where its addresses' list starts, and how long it is */
static u16 *track_idx;                  /* the 64 KiB address pages each page appears at */
static u8 *track_shown;                 /* per page: shown this frame */
static u32 *track_list, track_list_n;   /* the pages shown this frame */

static void track_free(void)
{
    free(track_host);
    free(track_first);
    free(track_cnt);
    free(track_idx);
    free(track_shown);
    free(track_list);
    track_host = NULL;
    track_first = track_cnt = track_list = NULL;
    track_idx = NULL;
    track_shown = NULL;
    track_n = track_list_n = 0;
}

int mem_track_begin(mem_track_fn fn)
{
    mem_track_end();
    u32 base[MEM_MAX_AREAS];
    for (int i = 0; i < mem_area_count; i++) {
        base[i] = track_n;
        track_n += mem_areas[i].size / MEM_TRACK_PAGE;
    }
    if (!track_n || track_n > 65535)
        return -1;
    track_host = malloc(track_n * sizeof *track_host);
    track_first = calloc(track_n, sizeof *track_first);
    track_cnt = calloc(track_n, sizeof *track_cnt);
    track_idx = malloc(65536 * sizeof *track_idx);
    track_shown = calloc(track_n, 1);
    track_list = malloc(track_n * sizeof *track_list);
    if (!track_host || !track_first || !track_cnt || !track_idx || !track_shown || !track_list) {
        track_free();
        return -1;
    }
    for (int i = 0; i < mem_area_count; i++)
        for (u32 off = 0; off < mem_areas[i].size; off += MEM_TRACK_PAGE)
            track_host[base[i] + off / MEM_TRACK_PAGE] = mem_areas[i].host + off;
    /* every address page that reaches an area's bytes: the mirrors and the aliases */
    memset(track_of, 0, sizeof track_of);
    for (u32 p = 0; p < 65536; p++) {
        u8 *h = mem_page_ram[p];
        for (int i = 0; h && i < mem_area_count; i++)
            if (h >= mem_areas[i].host && h < mem_areas[i].host + mem_areas[i].size) {
                u32 page = base[i] + (u32)(h - mem_areas[i].host) / MEM_TRACK_PAGE;
                track_of[p] = (u16)(page + 1);
                track_cnt[page]++;
                break;
            }
    }
    u32 at = 0;
    for (u32 page = 0; page < track_n; page++) {
        track_first[page] = at;
        at += track_cnt[page];
        track_cnt[page] = 0;
    }
    for (u32 p = 0; p < 65536; p++)
        if (track_of[p]) {
            u32 page = track_of[p] - 1u;
            track_idx[track_first[page] + track_cnt[page]++] = (u16)p;
        }
    track_fn = fn;
    mem_tracking = 1;
    mem_track_arm(1);
    return 0;
}

void mem_track_end(void)
{
    if (!track_n)
        return;
    mem_tracking = 0;
    memset(track_armed, 0, sizeof track_armed);
    mem_wpage_sync();
    track_free();
    memset(track_of, 0, sizeof track_of);
    track_fn = NULL;
}

static void track_page_arm(u32 page, int armed)
{
    for (u32 j = 0; j < track_cnt[page]; j++) {
        u16 p = track_idx[track_first[page] + j];
        track_armed[p] = (u8)armed;
        mem_wpage[p] = armed || mem_code_page[p] ? NULL : mem_page[p];
    }
}

void mem_track_arm(int all)
{
    if (!track_n)
        return;
    if (all) {
        memset(track_shown, 0, track_n);
        for (u32 page = 0; page < track_n; page++)
            track_page_arm(page, 1);
    } else
        for (u32 i = 0; i < track_list_n; i++) {
            track_shown[track_list[i]] = 0;
            track_page_arm(track_list[i], 1);
        }
    track_list_n = 0;
}

u32 mem_track_count(void)
{
    return track_n;
}

u8 *mem_track_host_of(u32 page)
{
    return page < track_n ? track_host[page] : NULL;
}

static void track_show(u32 page)
{
    track_shown[page] = 1;
    track_list[track_list_n++] = page;
    track_fn(page, track_host[page]);
    track_page_arm(page, 0);
}

void mem_track_slow(u32 addr)
{
    u32 page = track_of[addr >> 16];
    if (page && !track_shown[page - 1])
        track_show(page - 1);
}

void mem_track_host_slow(const void *host, size_t n)
{
    const u8 *h = host;
    if (!n)
        return;
    for (int i = 0; i < mem_area_count; i++) {
        const mem_area *a = &mem_areas[i];
        if (h < a->host || h >= a->host + a->size)
            continue;
        u32 page0 = 0;
        for (int k = 0; k < i; k++)
            page0 += mem_areas[k].size / MEM_TRACK_PAGE;
        size_t lo = (size_t)(h - a->host), hi = lo + n - 1;
        if (hi >= a->size)
            hi = a->size - 1;
        for (size_t o = lo / MEM_TRACK_PAGE; o <= hi / MEM_TRACK_PAGE; o++)
            if (!track_shown[page0 + o])
                track_show(page0 + (u32)o);
        return;
    }
}

/* recomp: a store mem_wpage sends to the slow path: a code page that is
 * plain RAM here (mem_page has it) is written as the fast path would, after
 * mem_code_store has seen the bytes; anything else is io_write's */
void mem_code_write(sh4 *c, u32 a, u32 v, int size)
{
    mem_track(a);                       /* recomp: before the store; see mem_track.h */
    u8 *p = mem_page[a >> 16];
    if (!p) {
        io_write(c, a, v, size);
        return;
    }
    mem_code_note(a, (u32)size, &v);    /* the host is little-endian: v's low bytes first */
    p += a & 0xffff;
    if (size == 4)
        *(u32 *)p = v;
    else if (size == 2)
        *(u16 *)p = (u16)v;
    else
        *p = (u8)v;
}
mem_area mem_areas[MEM_MAX_AREAS];
int mem_area_count;
mem_watch mem_watches[MEM_MAX_WATCH];
int mem_watch_count;

int mem_init(void)
{
    memset(mem_page, 0, sizeof mem_page);
    memset(mem_page_ram, 0, sizeof mem_page_ram);
    mem_area_count = 0;
    mem_watch_count = 0;
    mem_wpage_sync();                   /* recomp */
    return 0;
}

#ifdef _WIN32
/* recomp: on Windows an area is one allocation with a spare 64 KiB after it
 * (the guard), at whatever address the host gives; the mirrors and aliases
 * are in the page table only, which is all the interpreter and the
 * translated code use */
static u8 *map_at(int fd, u32 addr, u32 size)
{
    (void)fd, (void)addr, (void)size;
    return NULL;
}
#else
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static u8 *map_at(int fd, u32 addr, u32 size)
{
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (p == MAP_FAILED)
        return NULL;
    if ((uintptr_t)p != addr) {      /* an old kernel ignoring the flag */
        munmap(p, size);
        return NULL;
    }
    return p;
}
#endif

/* A page of the host's own after the end of a mapping, where nothing else
 * is. The interpreter moves a word or a long word with one host access at
 * the page table's pointer; a guest access that is not aligned and starts
 * in the last bytes of an area would run past the host mapping's end. The
 * game never does that (the processor would take an address error); a
 * routine the harness calls with made-up arguments can. With the page
 * there the bytes that spill over go nowhere, and the harness lives. */
static void guard_after(u8 *end)
{
#ifdef _WIN32
    (void)end;                          /* recomp: allocated with the area */
#else
    void *p = mmap(end, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && p != (void *)end)
        munmap(p, 4096);
#endif
}

/* once every area is there (an area's place must not be taken by its neighbour's guard) */
void mem_guards(void)
{
    for (int i = 0; i < mem_area_count; i++)
        guard_after(mem_areas[i].host + mem_areas[i].size);
}

int mem_add_ram(const char *name, u32 phys, u32 size)
{
    if (mem_area_count >= MEM_MAX_AREAS || (phys & 0xffff) || (size & 0xffff))
        return -1;
    mem_area *a = &mem_areas[mem_area_count];
    a->name = name;
    a->phys = phys;
    a->size = size;
#ifdef _WIN32
    /* recomp: one allocation, the guard included */
    a->fd = -1;
    a->host = VirtualAlloc(NULL, (SIZE_T)size + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!a->host) {
        fprintf(stderr, "mem: cannot make %s: Windows error %lu\n", name, (unsigned long)GetLastError());
        return -1;
    }
#else
    a->fd = memfd_create(name, 0);
    if (a->fd < 0 || ftruncate(a->fd, size) < 0) {
        fprintf(stderr, "mem: cannot make %s: %s\n", name, strerror(errno));
        return -1;
    }
    a->host = NULL;
    if (phys >= 0x10000)
        a->host = map_at(a->fd, phys, size);
    if (!a->host) {
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mem: cannot map %s: %s\n", name, strerror(errno));
            return -1;
        }
        a->host = p;
        if (phys >= 0x10000)
            fprintf(stderr, "mem: %s could not be mapped at its own address %08x (host has %p there); "
                    "the interpreter is not affected\n", name, phys, (void *)a->host);
    }
    /* the mirrors that also are host mappings: P1 and P2 */
    if (!map_at(a->fd, 0x80000000u | phys, size) || !map_at(a->fd, 0xa0000000u | phys, size))
        fprintf(stderr, "mem: a mirror of %s could not be mapped; the interpreter is not affected\n", name);
#endif
    /* the page table: every mirror of the 29-bit physical address below P3 */
    for (u32 top = 0; top < 6; top++) {
        u32 base = (top << 29) | phys;
        for (u32 off = 0; off < size; off += 0x10000) {
            mem_page[(base + off) >> 16] = a->host + off;
            mem_page_ram[(base + off) >> 16] = a->host + off;
        }
    }
    mem_area_count++;
    mem_wpage_sync();                   /* recomp */
    return 0;
}

int mem_alias(u32 phys_alias, u32 phys_src, u32 size)
{
    mem_area *a = NULL;
    for (int i = 0; i < mem_area_count; i++)
        if (mem_areas[i].phys == phys_src && mem_areas[i].size >= size)
            a = &mem_areas[i];
    if (!a)
        return -1;
    /* host mappings of the same memory, where the host allows */
    map_at(a->fd, phys_alias, size);
    map_at(a->fd, 0x80000000u | phys_alias, size);
    map_at(a->fd, 0xa0000000u | phys_alias, size);
    for (u32 top = 0; top < 6; top++) {
        u32 base = (top << 29) | phys_alias;
        for (u32 off = 0; off < size; off += 0x10000) {
            mem_page[(base + off) >> 16] = a->host + off;
            mem_page_ram[(base + off) >> 16] = a->host + off;
        }
    }
    mem_wpage_sync();                   /* recomp */
    return 0;
}

int mem_load_file(const char *path, u32 addr, u32 max, u32 *loaded)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mem: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    u8 *p = mem_ptr(addr);
    if (!p) {
        fclose(f);
        return -1;
    }
    size_t n = fread(p, 1, max, f);
    fclose(f);
    if (loaded)
        *loaded = (u32)n;
    return 0;
}

/* ---- watchpoints ---- */

static void watch_pages(u32 addr, u32 len, int hole)
{
    u32 phys = addr & 0x1fffffffu;
    for (u32 top = 0; top < 6; top++) {
        u32 first = ((top << 29) | phys) >> 16;
        u32 last = (((top << 29) | phys) + len - 1) >> 16;
        for (u32 p = first; p <= last; p++)
            mem_page[p] = (hole || mem_always_slow[p]) ? NULL : mem_page_ram[p];
    }
    mem_wpage_sync();                   /* recomp */
}

int mem_watch_add(u32 addr, u32 len, int on_read, int on_write, int stop, const char *name)
{
    if (mem_watch_count >= MEM_MAX_WATCH || !mem_ptr(addr) || len == 0)
        return -1;
    mem_watch *w = &mem_watches[mem_watch_count++];
    w->addr = addr & 0x1fffffffu;
    w->len = len;
    w->on_read = on_read;
    w->on_write = on_write;
    w->stop = stop;
    snprintf(w->name, sizeof w->name, "%s", name ? name : "");
    watch_pages(addr, len, 1);
    return 0;
}

void mem_watch_clear(void)
{
    for (int i = 0; i < mem_watch_count; i++)
        watch_pages(mem_watches[i].addr, mem_watches[i].len, 0);
    mem_watch_count = 0;
}

/* ---- host-side access ---- */

u32 g_r32(u32 a) { u8 *p = mem_ptr(a); return p ? *(u32 *)p : 0; }
u32 g_r16(u32 a) { u8 *p = mem_ptr(a); return p ? *(u16 *)p : 0; }
u32 g_r8(u32 a) { u8 *p = mem_ptr(a); return p ? *p : 0; }
void (*mem_host_store)(u32 addr, u32 n);
int mem_planting;
#define HOST_STORE(a, n) do { \
        mem_track(a);   /* recomp: n never crosses a 64 KiB page here */ \
        if (__builtin_expect(mem_host_store != NULL, 0)) mem_host_store(a, n); \
        if (__builtin_expect(mem_planting, 0)) mem_plant(a, n); \
        else if (__builtin_expect(mem_plant_near(a), 0)) mem_unplant(a, n); \
    } while (0)

/* ---- planted values ---- */

u8 mem_plant_page[8192];
static u8 plant_bits[PLANT_PAGES][0x10000 / 8];
static u32 plant_pages;

void mem_plant(u32 addr, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 a = (addr + i) & 0x1fffffffu;
        u8 ix = mem_plant_page[a >> 16];
        if (!ix) {
            if (plant_pages >= PLANT_PAGES)
                return;
            ix = mem_plant_page[a >> 16] = (u8)++plant_pages;
        }
        plant_bits[ix - 1][(a & 0xffff) >> 3] |= (u8)(1u << (a & 7));
    }
}

void mem_unplant(u32 addr, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 a = (addr + i) & 0x1fffffffu;
        u8 ix = mem_plant_page[a >> 16];
        if (ix)
            plant_bits[ix - 1][(a & 0xffff) >> 3] &= (u8)~(1u << (a & 7));
    }
}

int mem_planted(u32 addr, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 a = (addr + i) & 0x1fffffffu;
        u8 ix = mem_plant_page[a >> 16];
        if (ix && (plant_bits[ix - 1][(a & 0xffff) >> 3] & (1u << (a & 7))))
            return 1;
    }
    return 0;
}

void mem_plant_register(void)
{
    snapshot_register("planted-pages", mem_plant_page, sizeof mem_plant_page);
    snapshot_register("planted-bytes", plant_bits, sizeof plant_bits);
    snapshot_register("planted-count", &plant_pages, sizeof plant_pages);
}
/* recomp: mem_code_note in each */
void g_w32(u32 a, u32 v) { u8 *p = mem_ptr(a); if (p) { HOST_STORE(a, 4); mem_code_note(a, 4, &v); *(u32 *)p = v; } }
void g_w16(u32 a, u32 v) { u8 *p = mem_ptr(a); if (p) { HOST_STORE(a, 2); mem_code_note(a, 2, &v); *(u16 *)p = (u16)v; } }
void g_w8(u32 a, u32 v) { u8 *p = mem_ptr(a); if (p) { HOST_STORE(a, 1); mem_code_note(a, 1, &v); *p = (u8)v; } }

int g_read(u32 a, void *dst, u32 n)
{
    u8 *d = dst;
    while (n) {
        u8 *p = mem_ptr(a);
        u32 chunk = 0x10000 - (a & 0xffff);
        if (chunk > n)
            chunk = n;
        if (!p) {
            memset(d, 0, n);
            return -1;
        }
        memcpy(d, p, chunk);
        d += chunk;
        a += chunk;
        n -= chunk;
    }
    return 0;
}

int g_write(u32 a, const void *src, u32 n)
{
    const u8 *s = src;
    while (n) {
        u8 *p = mem_ptr(a);
        u32 chunk = 0x10000 - (a & 0xffff);
        if (chunk > n)
            chunk = n;
        if (!p)
            return -1;
        HOST_STORE(a, chunk);
        mem_code_note(a, chunk, s);     /* recomp */
        memcpy(p, s, chunk);
        s += chunk;
        a += chunk;
        n -= chunk;
    }
    return 0;
}

void g_str(u32 a, char *buf, size_t n)
{
    size_t i = 0;
    while (i + 1 < n) {
        u8 *p = mem_ptr(a + i);
        if (!p || !*p)
            break;
        buf[i] = (char)*p;
        i++;
    }
    buf[i] = 0;
}
