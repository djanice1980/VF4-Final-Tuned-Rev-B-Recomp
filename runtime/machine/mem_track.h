/* recomp: which 64 KiB pages of the machine's memory a frame writes, shown
 * before the first write so their bytes can be kept (rollback.c; docs/
 * design.md section 55).
 *
 * Translated code and the interpreter store through mem_wpage (mem.h). While
 * tracking, a page not yet written this frame is "armed": NULL in mem_wpage at
 * every address it appears at, so its first store takes the slow path
 * (rt_io_write, mem_code_write, rt_sq_write), which calls mem_track; that
 * shows the page to the tracker's function and puts the page back in
 * mem_wpage. The fast path is unchanged. Device code that writes guest memory
 * through host pointers (the sound chip, the renderer's passes into
 * textures, block moves) calls mem_track_host first, and the host's own
 * stores (g_write and the rest) call mem_track.
 *
 * Kept free of the machine's other headers: the sound chip includes it. */
#ifndef VF4_MEM_TRACK_H
#define VF4_MEM_TRACK_H

#include <stddef.h>
#include <stdint.h>

#define MEM_TRACK_PAGE 0x10000u

extern int mem_tracking;

/* page: a tracked page's number (0 .. mem_track_count() - 1); host: its bytes,
 * still as they were before this frame's first store */
typedef void (*mem_track_fn)(uint32_t page, uint8_t *host);

/* starts tracking every memory area (after all are added); 0 on success */
int mem_track_begin(mem_track_fn fn);
void mem_track_end(void);
/* a new frame: the pages written since the last call are armed again; with
 * all set, every page is (after the memory was changed behind the tracker) */
void mem_track_arm(int all);
uint32_t mem_track_count(void);
uint8_t *mem_track_host_of(uint32_t page);

void mem_track_slow(uint32_t addr);
void mem_track_host_slow(const void *host, size_t n);

/* the guest address addr is about to be stored to (a store never crosses a
 * 64 KiB page: it is aligned, and a store-queue block is 32 aligned bytes) */
static inline void mem_track(uint32_t addr)
{
    if (__builtin_expect(mem_tracking, 0))
        mem_track_slow(addr);
}
/* n bytes at host are about to be written by device code */
static inline void mem_track_host(const void *host, size_t n)
{
    if (__builtin_expect(mem_tracking, 0))
        mem_track_host_slow(host, n);
}

#endif
