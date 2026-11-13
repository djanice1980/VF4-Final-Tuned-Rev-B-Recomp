/* The cartridge port, behind which the DIMM board holds the game's files.
 *
 * The game's own file layer (vf4lib/libcore/src/sffs.c) drives this port by
 * hand: an address register pair and a data register that steps the
 * address by two for every 16-bit read (dimm_read_unlocked), and a block
 * read by DMA that ends with an interrupt (dimm_dma_start, dimm_dma_irq).
 * Modelling the port, and not replacing those routines, keeps all of the
 * game's file code running: the volume mount, the directory, the read
 * queue, the decompression.
 *
 * The data is the decrypted DIMM image of the decompilation's work
 * directory (work/vf41_s.dec), opened read-only; the game's rare writes to
 * the board's memory (its network log) go to a private copy in memory.
 *
 * Register layout as the decompiled sffs.c uses it (and as the board's
 * public documentation gives it): 0x5f7000 address high bits with mode
 * bits, 0x5f7004 address low, 0x5f7008 data, 0x5f700c / 0x5f7010 the DMA
 * address pair, 0x5f7404 / 0x5f7408 DMA destination and length, 0x5f7414
 * enable, 0x5f7418 start and busy.
 */
#define _GNU_SOURCE
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "net.h"

#include <errno.h>
#include <string.h>
#ifdef _WIN32                          /* recomp: Windows, docs/design.md section 14 */
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct cart_state {
    u32 ctrl, addr_lo;          /* 0x5f7000, 0x5f7004 */
    u32 pio_offset;
    u32 dma_hi, dma_lo;         /* 0x5f700c, 0x5f7010 */
    u32 g1_dst, g1_len, g1_dir, g1_enable, g1_busy;
    u64 pio_reads, dma_reads, dma_bytes, writes;
} cart_state;

static cart_state cs;
static u8 *image;
static u32 image_size;

static void dma_end_event(sh4 *c, u32 arg)
{
    (void)arg;
    cs.g1_busy = 0;
    hw_raise(c, IRQ_CART_DMA_END);
}

static u32 port_offset(u32 hi, u32 lo)
{
    /* twelve address bits in the high register; the bank bit above them
     * selects a second bank this board does not have */
    return ((hi & 0x0fffu) << 16) | (lo & 0xffffu) | ((hi & 0x1000u) ? 0x10000000u : 0);
}

int hw_cart_read(sh4 *c, u32 p, u32 *out)
{
    if (hw_net_reg_read(c, p, out))     /* the board's network registers, when that model is on (hw_dimm_net.c) */
        return 1;
    switch (p) {
    case 0x005f7000: *out = cs.ctrl; return 1;
    case 0x005f7004: *out = cs.addr_lo; return 1;
    case 0x005f7008: {
        u32 o = cs.pio_offset;
        cs.pio_reads++;
        if (!hw_net_pio_read(o, out))   /* (the network window above the image) */
            *out = (image && o + 1 < image_size) ? (u32)(image[o] | (image[o + 1] << 8)) : 0xffffu;
        cs.pio_offset = o + 2;
        return 1;
    }
    case 0x005f700c: *out = cs.g1_busy ? 0 : 0x4000u; return 1;     /* bit 14: no DMA under way */
    case 0x005f703c: *out = 0; return 1;        /* DIMM board command register: a board answers (not 0xffff) */
    case 0x005f7010: *out = cs.dma_lo; return 1;
    case 0x005f7404: *out = cs.g1_dst; return 1;
    case 0x005f7408: *out = cs.g1_len; return 1;
    case 0x005f740c: *out = cs.g1_dir; return 1;
    case 0x005f7414: *out = cs.g1_enable; return 1;
    case 0x005f7418: *out = cs.g1_busy; return 1;
    }
    return 0;
}

int hw_cart_write(sh4 *c, u32 p, u32 v)
{
    if (hw_net_reg_write(c, p, v))
        return 1;
    switch (p) {
    case 0x005f7000:
        cs.ctrl = v;
        cs.pio_offset = port_offset(cs.ctrl, cs.addr_lo);
        return 1;
    case 0x005f7004:
        cs.addr_lo = v & 0xffff;
        cs.pio_offset = port_offset(cs.ctrl, cs.addr_lo);
        return 1;
    case 0x005f7008: {
        u32 o = cs.pio_offset;
        cs.writes++;
        if (hw_net_pio_write(o, v)) {
        } else if (image && o + 1 < image_size) {
            image[o] = (u8)v;
            image[o + 1] = (u8)(v >> 8);
        }
        cs.pio_offset = o + 2;
        return 1;
    }
    case 0x005f700c: cs.dma_hi = v; return 1;
    case 0x005f7010: cs.dma_lo = v & 0xffff; return 1;
    case 0x005f7404: cs.g1_dst = v; return 1;
    case 0x005f7408: cs.g1_len = v; return 1;
    case 0x005f740c: cs.g1_dir = v; return 1;
    case 0x005f7414: cs.g1_enable = v & 1; return 1;
    case 0x005f7418:
        if ((v & 1) && cs.g1_enable && !cs.g1_busy) {
            u32 o = port_offset(cs.dma_hi, cs.dma_lo), n = cs.g1_len;
            cs.dma_reads++;
            cs.dma_bytes += n;
            for (u32 i = 0; i < n; i += 4) {
                u32 w = 0xffffffffu;
                if (cs.g1_dir == 0 && hw_net_is_on()) {         /* memory to the board: only its network window takes it */
                    w = g_r32(cs.g1_dst + i);
                    hw_net_block_write(o + i, (const u8 *)&w, 4);
                    continue;
                }
                if (hw_net_block_read(o + i, (u8 *)&w, 4)) {
                } else if (image && o + i + 3 < image_size)
                    memcpy(&w, image + o + i, 4);
                g_w32(cs.g1_dst + i, w);
            }
            cs.g1_busy = 1;
            /* about 10 MB a second: fast enough not to be felt, slow enough
             * for the game's "is it still running" tests to see it run */
            event_add(c->cycles + 2000 + (u64)n * 20, dma_end_event, 0, "cartridge DMA end");
        }
        return 1;
    }
    return 0;
}

u32 hw_cart_image_size(void)
{
    return image_size;
}

/* recomp (test mode): the image as the port has it, for the BIOS stand-in's
 * loading of the test program (rt_native.c); NULL without one */
const u8 *hw_cart_image(void)
{
    return image;
}

void hw_cart_stats(FILE *f)
{
    fprintf(f, "  files: %llu words read from the cartridge port, %llu block reads (%llu bytes), %llu words written\n",
            (unsigned long long)cs.pio_reads, (unsigned long long)cs.dma_reads, (unsigned long long)cs.dma_bytes,
            (unsigned long long)cs.writes);
}

int hw_cart_init(const char *path)
{
    memset(&cs, 0, sizeof cs);
    snapshot_register("cartridge", &cs, sizeof cs);
    hw_net_init();                      /* the board's network side (hw_dimm_net.c): off unless asked for */
    event_register(dma_end_event, "cartridge DMA end");
    image = NULL;
    image_size = 0;
#ifdef _WIN32
    /* recomp: a copy-on-write view of the file, as the private mmap below */
    wchar_t wpath[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024)) {
        fprintf(stderr, "vf4debug: the DIMM image's path is too long: %s\n", path);
        return -1;
    }
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "vf4debug: cannot open the DIMM image %s: Windows error %lu\n", path,
                (unsigned long)GetLastError());
        return -1;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0x1000000 || sz.QuadPart > 0xffffffffll) {
        fprintf(stderr, "vf4debug: %s is too small to be the DIMM image\n", path);
        CloseHandle(h);
        return -1;
    }
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    void *m = map ? MapViewOfFile(map, FILE_MAP_COPY, 0, 0, 0) : NULL;
    if (map)
        CloseHandle(map);
    CloseHandle(h);
    if (!m) {
        fprintf(stderr, "vf4debug: cannot map %s: Windows error %lu\n", path, (unsigned long)GetLastError());
        return -1;
    }
    image_size = (u32)sz.QuadPart;
    image = m;
    return 0;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "vf4debug: cannot open the DIMM image %s: %s\n"
                "  (the decompilation's extraction step makes work/vf41_s.dec; see README.md)\n", path,
                strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0x1000000) {
        fprintf(stderr, "vf4debug: %s is too small to be the DIMM image\n", path);
        close(fd);
        return -1;
    }
    image_size = (u32)st.st_size;
    void *m = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        fprintf(stderr, "vf4debug: cannot map %s: %s\n", path, strerror(errno));
        image_size = 0;
        return -1;
    }
    image = m;
    return 0;
#endif
}

/* recomp (M5): the image already in memory, decrypted from the disc's file
 * (runtime/host/dimm_image.c); the cartridge port owns it from here */
int hw_cart_init_memory(u8 *buf, u32 size)
{
    memset(&cs, 0, sizeof cs);
    snapshot_register("cartridge", &cs, sizeof cs);
    hw_net_init();
    event_register(dma_end_event, "cartridge DMA end");
    image = buf;
    image_size = size;
    return 0;
}
