/* The sound board's processor: an ARM7 (ARM state only) interpreter for
 * the game's sound driver, aicadrv.bin (docs/design.md section 16). Our
 * own code, written from the ARM architecture's rules. The state holds no
 * pointers, so the machine's snapshots can keep it as plain data. */
#ifndef VF4FT_ARM7_H
#define VF4FT_ARM7_H

#include <stdint.h>

typedef struct arm7 {
    uint32_t r[16];             /* the current mode's view; r[15] is the next instruction */
    uint32_t cpsr, spsr;        /* spsr: the current mode's (0 in user and system mode) */
    uint32_t fiq_r8_12[5], usr_r8_12[5];    /* the two banks of r8-r12 */
    uint32_t r13_14[6][2];      /* r13, r14 of user/system, FIQ, IRQ, supervisor, abort, undefined */
    uint32_t spsrs[6];
    uint32_t fiq;               /* the FIQ input: taken while set and the F bit is clear */
    uint32_t undefined_seen;    /* undefined instructions met (each one is reported once) */
    uint64_t insns;
} arm7;

/* the bus: sound RAM below 0x00800000, the chip's registers above; given by
 * the chip (aica.c) */
uint32_t arm7_bus_r32(uint32_t a);
uint32_t arm7_bus_r16(uint32_t a);
uint32_t arm7_bus_r8(uint32_t a);
void arm7_bus_w32(uint32_t a, uint32_t v);
void arm7_bus_w16(uint32_t a, uint32_t v);
void arm7_bus_w8(uint32_t a, uint32_t v);

/* sound RAM for the fast path (the program and its data); mask is its size - 1 */
void arm7_set_ram(uint8_t *ram, uint32_t mask);

void arm7_reset(arm7 *s);
/* runs n instructions (an interrupt taken counts as one) */
void arm7_run(arm7 *s, uint32_t n);

#endif
