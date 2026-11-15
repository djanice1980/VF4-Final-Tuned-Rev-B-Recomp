/* FSCA's sine and cosine (docs/design.md section 45).
 *
 * FSCA takes a 16-bit angle (65,536 to a turn) and gives its sine and
 * cosine as floats. They come from a table, not the host's math library,
 * so the bits are the same on every PC: two PCs playing one game together
 * run the same machine and must never part by a last bit. The values are
 * what sin() and cos() in double precision, rounded to float, gave on
 * Linux (and under Wine and Proton, checked for every angle), so runs from
 * before keep their hashes.
 *
 * rt_fsca_quarter holds the sine of angles 0 to 16384 (sh4rt_fpu.c, from
 * fsca_table.inc, written by tools/make_fsca_table.py); every other value
 * follows by symmetry, except three tiny ones: the double-precision angles
 * for a half turn and for a quarter and three quarters are not exactly pi,
 * pi/2 and 3pi/2. Used by the translated code (sh4rt.h) and the
 * interpreter (runtime/machine/sh4_exec.inc) alike. */
#ifndef VF4FT_FSCA_H
#define VF4FT_FSCA_H

#include <stdint.h>

extern const uint32_t rt_fsca_quarter[16385];

#define RT_FSCA_SIN_HALF     0x250d3132u     /* sin(32768): 1.2246e-16 */
#define RT_FSCA_COS_QUARTER  0x248d3132u     /* cos(16384): 6.1232e-17 */
#define RT_FSCA_COS_3QUARTER 0xa553c9cau     /* cos(49152): -1.8370e-16 */

/* the sine of angle k (its low 16 bits) as float bits */
static inline uint32_t rt_fsca_sin(uint32_t k)
{
    uint32_t r = k & 0x3fff;
    switch ((k >> 14) & 3) {
    case 0: return rt_fsca_quarter[r];
    case 1: return rt_fsca_quarter[0x4000 - r];
    case 2: return r ? rt_fsca_quarter[r] ^ 0x80000000u : RT_FSCA_SIN_HALF;
    default: return rt_fsca_quarter[0x4000 - r] ^ 0x80000000u;
    }
}

/* the cosine of angle k as float bits */
static inline uint32_t rt_fsca_cos(uint32_t k)
{
    k &= 0xffff;
    if (k == 0x4000)
        return RT_FSCA_COS_QUARTER;
    if (k == 0xc000)
        return RT_FSCA_COS_3QUARTER;
    return rt_fsca_sin(k + 0x4000);
}

#endif
