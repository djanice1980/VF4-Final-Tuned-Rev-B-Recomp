/* The graphics hardware, as little of it as the game's own graphics
 * libraries need to keep running unchanged: two rendering chips (their
 * register files, the render-end and list-end interrupt bits, the video
 * memory), and the ELAN geometry processor (its command stream, its
 * interrupt status, its memory).
 *
 * Nothing is drawn here (the picture view.c makes on request reads what
 * passes; it stores a pass the game renders into a texture in texture
 * memory, which the game never reads). What is modelled is the protocol
 * the libraries drive:
 *
 *   - register files that keep what is written (the window at 0x085fxxxx
 *     writes both chips);
 *   - "start render" answers with the render-end interrupt bits on both
 *     chips a little later;
 *   - the ELAN command stream is executed far enough to follow its links
 *     from segment to segment, to copy plain data transfers, to answer the
 *     "wait for the end of list" commands by raising the list-end bits, and
 *     to raise its two status bits (transfer finished, list set finished)
 *     where the link commands ask for them.
 *
 * The command layouts are the ones the decompiled library writes
 * (sdk/kamui/transfer_queue.c: xfer_job_start, list_seg_link_to,
 * list_seg_end_mark; sdk/kamui/elan_status.c: elan_status_service,
 * elan_cmd_wait_list_end); the register meanings are those the library's
 * own code shows (sdk/kamui/pvr_irq.c) together with the public
 * documentation of the rendering chip's register map. No emulator code was
 * used.
 *
 * Display lists pass through here: each list segment the ELAN is told to
 * run is handed to gfx_list_sink and, when a picture is wanted, to the view
 * (and each 32-byte block written straight to a chip's list input to
 * gfx_sink). Texture transfers to the first chip's texture input land in
 * its video memory.
 */
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "dispatch.h"
#include "view.h"

#include <string.h>

#define RENDER_CYCLES 200000u            /* a render "takes" one millisecond */

typedef struct gfx_state {
    u32 core[2][0x800];                  /* each chip's core registers, 0x5f8000-0x5f9fff */
    u32 irq2_normal, irq2_error, mask6_normal2, mask6_error2, mask4_normal2, mask4_error2, mask2_normal2, mask2_error2;
    u32 elan_reg[0x40];
    u32 elan_status;                     /* bit 0: transfer finished; bit 1: list set finished */
    u32 dmac[17];                        /* SH-4 DMA controller, 0xffa00000-0xffa00043 */
    u64 renders, list_segments, list_bytes, plain_transfers, elan_blocks;
    u32 frame_segments, frame_list_bytes;
} gfx_state;

static gfx_state gs;
gfx_list_sink_fn gfx_list_sink;

static void render_done_event(sh4 *c, u32 arg)
{
    (void)arg;
    hw.irq_normal |= IRQ_RENDER_END_VIDEO | IRQ_RENDER_END_DEPTH | IRQ_RENDER_END_TEXTURE;
    gs.irq2_normal |= IRQ_RENDER_END_VIDEO | IRQ_RENDER_END_DEPTH | IRQ_RENDER_END_TEXTURE;
    hw_irq_update(c);
}

int hw_gfx_irq_level(void)
{
    /* the second chip and the ELAN share the first chip's request line */
    if ((gs.irq2_normal & gs.mask6_normal2) || (gs.irq2_error & gs.mask6_error2))
        return 6;
    if (gs.elan_status & 3)
        return 6;
    return 0;
}

/* ---- rendering chip core registers ---- */

/* The line the sync generator is at. A frame is 525 lines of virtual time;
 * the vertical blank interrupt marks the line the game has programmed for
 * it (the low field of the register at +0xcc; 480 until it has). */
static u32 vblank_line(void)
{
    u32 line = gs.core[0][0xcc / 4] & 0x3ff;
    return line ? line : 480;
}

static u32 scan_line(sh4 *c)
{
    return (u32)(((c->cycles % FRAME_CYCLES) * 525 / FRAME_CYCLES + vblank_line()) % 525);
}

static void hblank_event(sh4 *c, u32 arg)
{
    (void)arg;
    hw_raise(c, IRQ_HBLANK);
}

static u32 core_read(sh4 *c, int chip, u32 off)
{
    switch (off) {
    case 0x10c: {                        /* sync generator status: the line being scanned */
        u32 line = scan_line(c);
        u32 blank_from = gs.core[0][0xdc / 4] & 0x3ff, blank_to = (gs.core[0][0xdc / 4] >> 16) & 0x3ff;
        if (!blank_from)
            blank_from = 480;
        int blank = blank_to < blank_from ? (line >= blank_from || line < blank_to)
                                          : (line >= blank_from && line < blank_to);
        return line | (blank ? 0x2000u : 0);
    }
    case 0x000: return 0x17fd11db;       /* chip identification */
    case 0x004: return 0x00000011;       /* revision */
    case 0x138:                          /* where the list input stands: nothing was added */
        return gs.core[chip][0x128 / 4];
    case 0x134:
        return gs.core[chip][0x124 / 4];
    default:
        return gs.core[chip][(off & 0x1ffc) / 4];
    }
}

static void core_write(sh4 *c, int chip, u32 off, u32 v)
{
    gs.core[chip][(off & 0x1ffc) / 4] = v;
    if (view_on)
        view_reg_write(c, chip, off, v);
    if (off == 0x014 && chip == 0) {     /* start render */
        gs.renders++;
        event_add(c->cycles + RENDER_CYCLES, render_done_event, 0, "render end");
    }
}

u32 view_core_reg(int chip, u32 off)
{
    return gs.core[chip & 1][(off & 0x1ffc) / 4];
}

/* ---- the ELAN ---- */

#define ELAN_RAM_PHYS 0x0a000000u
#define ELAN_RAM_SIZE 0x02000000u

static void elan_block(sh4 *c, const u32 *w);

static void elan_link(sh4 *c, u32 w1, u32 w2, u32 w3)
{
    for (int guard = 0; guard < 4096; guard++) {
        u32 flags = w1 & 0x60000000u;
        u32 len = w3;
        u32 src = (w1 & 0x80000000u) ? gs.dmac[0x20 / 4] : (ELAN_RAM_PHYS | (w1 & 0x01fffff8u));
        int has_data = (w1 & 0x80000000u) || (w1 & 0x01fffff8u) || len;
        u32 next[8];
        int chained = 0;
        if (has_data && len >= 32 && w2 == 0x09000000u) {
            /* a list segment: its body is display list, its last 32 bytes a command block */
            gs.list_segments++;
            gs.list_bytes += len - 32;
            gs.frame_segments++;
            gs.frame_list_bytes += len - 32;
            if (gfx_list_sink)
                gfx_list_sink(c, src, len - 32);
            if (view_on)
                view_list_segment(c, src, len - 32);
            if (g_read(src + len - 32, next, 32) == 0)
                chained = 1;
        } else if (has_data && len) {
            /* a plain transfer to memory */
            u32 dst = w2 & 0x1ffffff8u;
            /* the first chip's texture input (0x11000000): the bytes land in its
             * video memory at the same offset (vram64_write, sdk/kamui/chip_access.c) */
            if ((dst & 0xff000000u) == 0x11000000u)
                dst = 0x04000000u | (dst & 0x00ffffffu);
            gs.plain_transfers++;
            for (u32 off = 0; off < len; off += 4) {
                if (mem_ptr(src + off) && mem_ptr(dst + off))
                    g_w32(dst + off, g_r32(src + off));
            }
        }
        if (w1 & 0x80000000u) {
            /* the DMA channel ran to its end */
            gs.dmac[0x20 / 4] += len;
            gs.dmac[0x28 / 4] = 0;
            gs.dmac[0x2c / 4] |= 2;                     /* transfer end */
        }
        if (flags & 0x20000000u)
            gs.elan_status |= 1;
        if (flags & 0x40000000u)
            gs.elan_status |= 2;
        if (!chained)
            break;
        /* the block at the end of the segment: go on if it starts with a link */
        if (next[0] != 0x08000f00u) {
            elan_block(c, next);
            break;
        }
        w1 = next[1];
        w2 = next[2];
        w3 = next[3];
    }
    hw_irq_update(c);
}

static void elan_block(sh4 *c, const u32 *w)
{
    gs.elan_blocks++;
    for (int i = 0; i < 8;) {
        u32 op = w[i];
        if (op == 0x08000f00u && i + 3 < 8) {           /* link */
            elan_link(c, w[i + 1], w[i + 2], w[i + 3]);
            i += 4;
        } else if (op == 0x08000e00u && i + 3 < 8) {    /* wait for a chip's status bits */
            u32 reg = w[i + 1] & 0x1ffffffcu, bits = w[i + 3];
            if (reg == 0x005f6900u)
                hw.irq_normal |= bits;
            else if (reg == 0x025f6900u)
                gs.irq2_normal |= bits;
            i += 4;
        } else {
            i += 2;                                     /* 0x08000000 and blank words: nothing */
        }
    }
    hw_irq_update(c);
}

static u32 elan_read(u32 off)
{
    /* the status register also shows the two chips' interrupt requests, as
     * two pairs of active-low bits (elan_status_service inverts bits 2-5
     * and takes the first pair for chip 1, the second for chip 2) */
    u32 chip1 = (hw.irq_normal & hw.mask6_normal) || (hw.irq_error & hw.mask6_error);
    u32 chip2 = (gs.irq2_normal & gs.mask6_normal2) || (gs.irq2_error & gs.mask6_error2);
    switch (off) {
    case 0x00: return 0xe1ad0000u;                      /* identification the library looks for */
    case 0x04: return 2;                                /* revision: not the first one */
    case 0x0c: return 0;                                /* command queue level: always room */
    case 0x74: return (gs.elan_status & 3) | (chip1 ? 0 : 0x0c) | (chip2 ? 0 : 0x30);
    default: return off < 0x100 ? gs.elan_reg[off / 4] : 0;
    }
}

static void elan_write(sh4 *c, u32 off, u32 v)
{
    if (off == 0x74) {
        gs.elan_status &= ~v;
        hw_irq_update(c);
        return;
    }
    if (off < 0x100)
        gs.elan_reg[off / 4] = v;
}

/* ---- entry points from hw.c ---- */

int hw_gfx_read(sh4 *c, u32 p, u32 *out)
{
    if (p >= 0x005f8000u && p < 0x005fa000u) { *out = core_read(c, 0, p - 0x005f8000u); return 1; }
    if (p >= 0x025f8000u && p < 0x025fa000u) { *out = core_read(c, 1, p - 0x025f8000u); return 1; }
    if (p >= 0x085f8000u && p < 0x085fa000u) { *out = core_read(c, 0, p - 0x085f8000u); return 1; }
    if (p >= 0x08800000u && p < 0x08800100u) { *out = elan_read(p - 0x08800000u); return 1; }
    switch (p) {
    case 0x025f6900: *out = gs.irq2_normal | (gs.irq2_error ? 0x80000000u : 0); return 1;
    case 0x025f6908: *out = gs.irq2_error; return 1;
    case 0x025f6910: *out = gs.mask2_normal2; return 1;
    case 0x025f6918: *out = gs.mask2_error2; return 1;
    case 0x025f6920: *out = gs.mask4_normal2; return 1;
    case 0x025f6928: *out = gs.mask4_error2; return 1;
    case 0x025f6930: *out = gs.mask6_normal2; return 1;
    case 0x025f6938: *out = gs.mask6_error2; return 1;
    case 0x005f6808: *out = 0; return 1;                /* channel-2 DMA: never busy */
    case 0x005f688c: *out = 0; return 1;                /* bus FIFO state: empty */
    case 0x005f689c: *out = 0x0b; return 1;             /* system chip revision */
    }
    return 0;
}

static int chip2_sys_write(sh4 *c, u32 off, u32 v)
{
    switch (off) {
    case 0x6900: gs.irq2_normal &= ~v; break;
    case 0x6908: gs.irq2_error &= ~v; break;
    case 0x6910: gs.mask2_normal2 = v; break;
    case 0x6918: gs.mask2_error2 = v; break;
    case 0x6920: gs.mask4_normal2 = v; break;
    case 0x6928: gs.mask4_error2 = v; break;
    case 0x6930: gs.mask6_normal2 = v; break;
    case 0x6938: gs.mask6_error2 = v; break;
    default: return 0;
    }
    hw_irq_update(c);
    return 1;
}

int hw_gfx_write(sh4 *c, u32 p, u32 v)
{
    if (p >= 0x005f8000u && p < 0x005fa000u) { core_write(c, 0, p - 0x005f8000u, v); return 1; }
    if (p >= 0x025f8000u && p < 0x025fa000u) { core_write(c, 1, p - 0x025f8000u, v); return 1; }
    if (p >= 0x085f8000u && p < 0x085fa000u) {
        core_write(c, 0, p - 0x085f8000u, v);
        core_write(c, 1, p - 0x085f8000u, v);
        return 1;
    }
    if (p >= 0x08800000u && p < 0x08800100u) { elan_write(c, p - 0x08800000u, v); return 1; }
    if (p >= 0x025f6900u && p < 0x025f6940u)
        return chip2_sys_write(c, p - 0x025f0000u, v);
    if (p >= 0x085f6900u && p < 0x085f6940u) {
        /* the window that writes both chips' interrupt registers */
        io_write(c, 0xa05f0000u + (p - 0x085f0000u), v, 4);
        return chip2_sys_write(c, p - 0x085f0000u, v);
    }
    return 0;
}

int hw_dmac_read(u32 a, u32 *out)
{
    if (a >= 0xffa00000u && a < 0xffa00044u) { *out = gs.dmac[(a - 0xffa00000u) / 4]; return 1; }
    return 0;
}

int hw_dmac_write(u32 a, u32 v)
{
    if (a >= 0xffa00000u && a < 0xffa00044u) { gs.dmac[(a - 0xffa00000u) / 4] = v; return 1; }
    return 0;
}

/* a store-queue block sent outside RAM */
int hw_gfx_sq(sh4 *c, u32 dst, const u32 *words)
{
    u32 p = dst & 0x1fffffffu;
    if ((p & 0xff000000u) == 0x09000000u) {             /* the ELAN's command input */
        elan_block(c, words);
        return 1;
    }
    if ((p & 0xff000000u) == 0x11000000u) {             /* the first chip's texture input, by store queue */
        g_write(0x04000000u | (p & 0x00ffffe0u), words, 32);
        return 1;
    }
    return 0;
}

/* once per vertical blank */
void hw_gfx_frame(sh4 *c)
{
    if (view_on)
        view_frame(c);
    gs.frame_segments = 0;
    gs.frame_list_bytes = 0;
    /* The horizontal blank interrupt: the game programs one line a frame
     * (the register at +0xc8: line in the low field, mode 0 in bits 12-13).
     * The modes that interrupt on many lines are not modelled. */
    u32 reg = gs.core[0][0xc8 / 4];
    if (((reg >> 12) & 3) == 0 && (hw.mask6_normal | hw.mask4_normal | hw.mask2_normal) & IRQ_HBLANK) {
        u32 lines = ((reg & 0x3ff) + 525 - vblank_line()) % 525;
        event_add(c->cycles + (u64)lines * (FRAME_CYCLES / 525) + 1, hblank_event, 0, "horizontal blank");
    }
}

void hw_gfx_stats(FILE *f)
{
    fprintf(f, "  interrupt state: chip 1 status %08x mask6 %08x err %08x/%08x; chip 2 status %08x mask6 %08x err "
            "%08x/%08x; ELAN status %x\n", hw.irq_normal, hw.mask6_normal, hw.irq_error, hw.mask6_error, gs.irq2_normal, gs.mask6_normal2,
            gs.irq2_error, gs.mask6_error2, gs.elan_status);
    fprintf(f, "  graphics: %llu renders started, %llu list segments (%llu bytes) through the ELAN, %llu plain "
            "transfers, %llu store-queue blocks to the chips\n", (unsigned long long)gs.renders,
            (unsigned long long)gs.list_segments, (unsigned long long)gs.list_bytes,
            (unsigned long long)gs.plain_transfers, (unsigned long long)gfx_sq_blocks);
}

void hw_gfx_init(void)
{
    memset(&gs, 0, sizeof gs);
    snapshot_register("graphics", &gs, sizeof gs);
    event_register(render_done_event, "render end");
    event_register(hblank_event, "horizontal blank");
    /* Video memory of the two chips and the ELAN's memory. A chip's memory is seen
     * through two windows that lay its two halves out differently: the first (textures)
     * takes four bytes from each half in turn, the second (object lists, region arrays,
     * background planes, frame buffers) has one half after the other. The library places
     * what it writes through each so that nothing overlaps in the chip. Here each window
     * is memory of its own: with one memory behind both in the same order, a region array
     * written through the second window would land in the middle of a texture (and a
     * texture loaded later would wipe it), which the view would show. Nothing the game
     * does reads through one window what it wrote through the other. */
    mem_add_ram("video-ram-1", 0x04000000u, 0x01000000u);
    mem_add_ram("video-ram-1-lists", 0x05000000u, 0x01000000u);
    mem_add_ram("video-ram-2", 0x06000000u, 0x01000000u);
    mem_add_ram("video-ram-2-lists", 0x07000000u, 0x01000000u);
    mem_add_ram("elan-ram", ELAN_RAM_PHYS, ELAN_RAM_SIZE);
}
