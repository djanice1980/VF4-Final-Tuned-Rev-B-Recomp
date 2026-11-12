/* Host replacements and hooks on named routines, and the BIOS services.
 * Every replacement and hook is registered with a one-line reason and
 * listed in docs/hle.md (build/vf4debug --list-hle prints the table the
 * build has). The hardware models are in hw*.c. */
#ifndef VF4_HLE_H
#define VF4_HLE_H

#include "sh4.h"
#include "dispatch.h"
#include <stdio.h>

int hle_init(sh4 *c);
/* a jump or call to an address outside the image (BIOS entry points and
 * the like); returns 1 when it was handled */
int hle_outside(sh4 *c, u32 target, int kind);
void hle_list(FILE *f);

/* the parts */
void hle_boot_install(sh4 *c);      /* hle_boot.c: what the BIOS leaves and offers */
void hle_vectors_write(void);       /* hle_boot.c: the code at the exception vectors */
u32 hle_board_id_word(int with_network);    /* hle_boot.c: the word the DIMM board leaves in low RAM */
void hle_waits_install(sh4 *c);     /* hle_waits.c: the game's waits as jumps of virtual time */

/* shared helpers for replacements */
void hle_return_zero(sh4 *c, routine *r);
void hle_return_one(sh4 *c, routine *r);
void hle_nothing(sh4 *c, routine *r);

#endif
