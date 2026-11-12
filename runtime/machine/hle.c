/* Host replacements and hooks: the common part. Milestone 1 ended with no
 * routine replaced (the hardware models of hw*.c and the BIOS services of
 * hle_boot.c were enough) and three hooks (hle_waits.c). */
#include "hle.h"
#include "machine.h"

#include <stdio.h>

void hle_return_zero(sh4 *c, routine *r) { (void)r; c->r[0] = 0; }
void hle_return_one(sh4 *c, routine *r) { (void)r; c->r[0] = 1; }
void hle_nothing(sh4 *c, routine *r) { (void)c; (void)r; }

int hle_init(sh4 *c)
{
    hle_boot_install(c);
    hle_waits_install(c);
    return 0;
}

void hle_list(FILE *f)
{
    fprintf(f, "address\troutine\tfile\thow\tcalls\twhy\n");
    for (int i = 0; i < routine_count; i++) {
        routine *r = &routines[i];
        if (r->kind != RK_HOST && !r->hook)
            continue;
        fprintf(f, "%08x\t%s\t%s\t%s\t%u\t%s\n", r->addr, r->name, r->file,
                r->kind == RK_HOST ? "replaced" : "hooked", r->kind == RK_HOST ? r->host_calls : (u32)r->calls,
                r->why ? r->why : "");
    }
}
