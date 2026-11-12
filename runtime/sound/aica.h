/* The AICA sound chip: 64 channels, timers, the two interrupt controllers,
 * and the ARM7 that runs the game's sound driver (docs/design.md section
 * 16). Our own code, from the chip's public descriptions and the
 * decompilation's findings. Nothing here knows about the main CPU's
 * machine: hw_sound.c connects it. The state holds no pointers (snapshots
 * keep it as plain data). */
#ifndef VF4FT_AICA_H
#define VF4FT_AICA_H

#include <stdint.h>
#include <stdio.h>

#define AICA_RATE 44100
#define AICA_RAM_SIZE 0x00800000u

/* sound_ram: the 8 MiB the main CPU also sees; arm_budget: ARM
 * instructions per sample (0 = the default) */
void aica_init(uint8_t *sound_ram, unsigned arm_budget);

/* the register file, 0x0000-0x7fff, as the main CPU sees it (32-bit
 * accesses, 16 bits of data) */
uint32_t aica_read(uint32_t off);
void aica_write(uint32_t off, uint32_t v);

/* n samples: the ARM, the timers, the interrupts, the channels; the output
 * goes to the audio buffer */
void aica_run(unsigned n);

/* the main CPU's interrupt from the chip (MCIPD & MCIEB) is asserted */
int aica_main_irq(void);

/* the audio buffer: up to max stereo frames (left, right) taken from it;
 * returns how many. It holds about 1.5 s; older frames are dropped when no
 * one takes them. */
unsigned aica_audio_take(int16_t *dst, unsigned max);
void aica_audio_discard(void);

/* the state, for the machine's snapshots: pointer and size */
void *aica_state(unsigned *size);

void aica_stats(FILE *f);

#endif
