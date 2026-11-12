/* The sound board, seen from the main processor: 8 MiB of sound RAM, a
 * register file, and the sound processor's driver program reduced to its
 * side of the mailbox in sound RAM.
 *
 * The real driver (aicadrv.bin, a program for the sound board's own
 * processor) is loaded into sound RAM by the game but not run. What the
 * game's sound library (sdk/libsnd) waits for is modelled instead, as the
 * decompiled library shows it:
 *
 *   - after the reset bit is released the driver reports ready (a non-zero
 *     word at 0x5c of sound RAM; aica_driver_boot waits for it);
 *   - it takes request words from a 64-word ring at 0x400 and clears each
 *     slot it has taken (aica_ring_push looks for a zero slot);
 *   - request 0x18a0 (the bank table changed) is answered by raising the
 *     word at 0xb4 until the library raises 0xb0
 *     (snd_ack_word_wait_nonzero, snd_flag_raise_wait_clear);
 *   - request 0x2000a0 asks for the block move described by the words at
 *     0x90 / 0x94 / 0x98 (source, destination, size); the size word is
 *     cleared when it is done (snd_ram_copy_request, snd_ram_copy_busy).
 *
 * Every other request is a sound command: counted, kept in a short history
 * for the status report, and dropped. Nothing is played. Sequences and
 * voices therefore always read as "not playing".
 *
 * recomp: with hw_sound_chip set, none of that: the registers are the chip
 * model's (runtime/sound/aica.c), whose ARM7 runs the driver the game
 * loaded, stepped in virtual time 32 samples at a time.
 */
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "observe.h"

#include "aica.h"                   /* recomp */

#include <stdlib.h>
#include <string.h>

#define SOUND_RAM_PHYS 0x00800000u
#define SOUND_RAM_SIZE 0x00800000u
#define STEP_CYCLES    20000u

typedef struct sound_state {
    u32 reg[0x2000];            /* 0x00700000-0x00707fff */
    u32 running;                /* the sound processor is out of reset */
    u32 ring_rp;                /* offset of the next ring slot to take */
    u32 ack_pending;
    u64 requests, sound_commands, block_moves;
    u32 last[16];
    u32 last_n;
    u32 stepping;
    u32 stall_until;            /* the driver side takes no request before this virtual frame (0: no stall) */
} sound_state;

static sound_state ss;

/* recomp: the chip */
int hw_sound_chip;
unsigned hw_sound_arm_budget;
#define CHIP_BLOCK 32u                  /* samples a step */
static u64 chip_samples;                /* samples run; the next step ends at sample chip_samples + CHIP_BLOCK */

/* sample n is due at virtual cycle n * CPU_HZ / AICA_RATE */
static u64 chip_cycle_of(u64 n)
{
    return n * (CPU_HZ / 100) / (AICA_RATE / 100);
}

/* the chip's interrupt to the main CPU: bit 1 of the system chip's external
 * interrupts (the driver's doorbell, MCIPD bit 5, is the one the game enables) */
#define EXT_IRQ_AICA 0x00000002u
static void chip_irq(sh4 *c)
{
    u32 want = aica_main_irq() ? EXT_IRQ_AICA : 0;
    if ((hw.irq_external & EXT_IRQ_AICA) != want) {
        hw.irq_external = (hw.irq_external & ~EXT_IRQ_AICA) | want;
        hw_irq_update(c);
    }
}

static void chip_step(sh4 *c, u32 arg)
{
    (void)arg;
    aica_run(CHIP_BLOCK);
    chip_samples += CHIP_BLOCK;
    event_add(chip_cycle_of(chip_samples + CHIP_BLOCK), chip_step, 0, "sound chip");
    chip_irq(c);
}

static u32 sr32(u32 off) { return g_r32(0xa0000000u + SOUND_RAM_PHYS + off); }
/* what the driver side writes into sound RAM is the model's: a planted value (mem.h) */
static void sw32(u32 off, u32 v)
{
    mem_planting++;
    g_w32(0xa0000000u + SOUND_RAM_PHYS + off, v);
    mem_planting--;
}

static void driver_start(void)
{
    ss.ring_rp = 0x400;
    ss.ack_pending = 0;
    for (u32 o = 0x400; o < 0x500; o += 4)
        sw32(o, 0);
    sw32(0xb0, 0);
    sw32(0xb4, 0);
    sw32(0x98, 0);
    /* free space for sound banks: from the bank area's start (the driver
     * image gives it at +0x84) to the end of sound RAM */
    u32 bank_start = sr32(0x84);
    if (bank_start && bank_start < SOUND_RAM_SIZE)
        sw32(0x88, SOUND_RAM_SIZE - bank_start);
    sw32(0x5c, 1);              /* ready */
}

static void driver_request(u32 w)
{
    ss.requests++;
    if (w == 0x18a0u) {
        sw32(0xb4, 1);
        ss.ack_pending = 1;
        return;
    }
    if (w == 0x2000a0u) {
        u32 src = sr32(0x90), dst = sr32(0x94), size = sr32(0x98);
        if (src < SOUND_RAM_SIZE && dst < SOUND_RAM_SIZE && size <= SOUND_RAM_SIZE - src &&
            size <= SOUND_RAM_SIZE - dst) {
            u8 *base = mem_ptr(0xa0000000u + SOUND_RAM_PHYS);
            memmove(base + dst, base + src, size);
        }
        sw32(0x98, 0);
        ss.block_moves++;
        return;
    }
    ss.sound_commands++;
    ss.last[ss.last_n++ & 15] = w;
    if (cfg.verbose > 2)
        fprintf(stderr, "[sound] vb %u request %08x\n", machine_frame, w);
}

static void driver_step(sh4 *c, u32 arg)
{
    (void)arg;
    if (!ss.running) {
        ss.stepping = 0;
        return;
    }
    if (ss.stall_until && machine_frame < ss.stall_until) {
        /* a script asked for a driver that takes nothing (the controller's "sound stall"): the
         * requests stay in the ring, the handshakes are not answered */
        event_add(c->cycles + STEP_CYCLES, driver_step, 0, "sound driver step");
        return;
    }
    if (ss.ack_pending && sr32(0xb0) != 0) {
        sw32(0xb4, 0);
        ss.ack_pending = 0;
    }
    for (int n = 0; n < 64; n++) {
        u32 w = sr32(ss.ring_rp);
        if (w == 0)
            break;
        sw32(ss.ring_rp, 0);
        ss.ring_rp += 4;
        if (ss.ring_rp >= 0x500)
            ss.ring_rp = 0x400;
        driver_request(w);
    }
    event_add(c->cycles + STEP_CYCLES, driver_step, 0, "sound driver step");
}

int hw_sound_read(sh4 *c, u32 p, u32 *out)
{
    if (hw_sound_chip && p >= 0x00700000u && p < 0x00708000u) {    /* recomp */
        *out = aica_read(p - 0x00700000u);
        return 1;
    }
    if (p >= 0x00700000u && p < 0x00708000u) {
        *out = ss.reg[(p - 0x00700000u) / 4];
        return 1;
    }
    if (p == 0x00710000u || p == 0x00710004u) {
        /* the board's clock: seconds, in two 16-bit halves */
        u32 now = hw.rtc_base + (u32)(c->cycles / CPU_HZ);
        *out = p == 0x00710000u ? now >> 16 : now & 0xffff;
        return 1;
    }
    if (p == 0x00710008u) {
        *out = 0;
        return 1;
    }
    return 0;
}

int hw_sound_write(sh4 *c, u32 p, u32 v)
{
    if (hw_sound_chip && p >= 0x00700000u && p < 0x00708000u) {    /* recomp */
        aica_write(p - 0x00700000u, v);
        chip_irq(c);
        return 1;
    }
    if (p >= 0x00700000u && p < 0x00708000u) {
        ss.reg[(p - 0x00700000u) / 4] = v;
        if (p == 0x00702c00u) {
            u32 run = !(v & 1);
            if (run && !ss.running) {
                ss.running = 1;
                driver_start();
                if (!ss.stepping) {
                    ss.stepping = 1;
                    event_add(c->cycles + STEP_CYCLES, driver_step, 0, "sound driver step");
                }
            } else if (!run) {
                ss.running = 0;
            }
        }
        return 1;
    }
    if (p >= 0x00710000u && p < 0x00710010u)
        return 1;               /* setting the clock: ignored */
    return 0;
}

void hw_sound_stats(FILE *f)
{
    if (hw_sound_chip) {                /* recomp */
        aica_stats(f);
        return;
    }
    fprintf(f, "  sound: driver %s; %llu requests taken (%llu sound commands dropped, %llu block moves)\n",
            ss.running ? "running" : "not started", (unsigned long long)ss.requests,
            (unsigned long long)ss.sound_commands, (unsigned long long)ss.block_moves);
}

/* The controller's "sound stall FRAMES | off": the sound driver's side stops taking requests from
 * the ring for so many frames (the driver hung, or busy), so that the game's sound library meets a full
 * ring and unanswered handshakes; what it does then is its own code's (sdk/libsnd). */
int hw_sound_command(char **tok, int n, char *msg, size_t len)
{
    if (n >= 2 && !strcmp(tok[0], "stall")) {
        ss.stall_until = !strcmp(tok[1], "off") ? 0 : machine_frame + (u32)strtoul(tok[1], NULL, 0);
        char note[100];
        snprintf(note, sizeof note, "the sound driver model: takes no request until frame %u", ss.stall_until);
        obs_note(note);
        return 0;
    }
    snprintf(msg, len, "sound stall FRAMES | sound stall off");
    return -1;
}

void hw_sound_init(void)
{
    memset(&ss, 0, sizeof ss);
    snapshot_register("sound", &ss, sizeof ss);
    event_register(driver_step, "sound driver step");
    mem_add_ram("sound-ram", SOUND_RAM_PHYS, SOUND_RAM_SIZE);
    if (hw_sound_chip) {                /* recomp: the chip, from the first sample */
        unsigned n;
        aica_init(mem_ptr(0xa0000000u + SOUND_RAM_PHYS), hw_sound_arm_budget);
        snapshot_register("sound chip", aica_state(&n), n);
        snapshot_register("sound chip samples", &chip_samples, sizeof chip_samples);
        event_register(chip_step, "sound chip");
        chip_samples = 0;
        event_add(chip_cycle_of(CHIP_BLOCK), chip_step, 0, "sound chip");
    }
    /* the first 64 KiB, where the driver's mailbox is, take the slow path:
     * the marks on the words the model plants there (ready, free space,
     * the handshake words, the emptied ring) are then exact in every run */
    for (u32 top = 0; top < 6; top++) {
        mem_page[((top << 29) | SOUND_RAM_PHYS) >> 16] = NULL;
        mem_always_slow[((top << 29) | SOUND_RAM_PHYS) >> 16] = 1;
    }
    mem_wpage_sync();                   /* recomp */
}
