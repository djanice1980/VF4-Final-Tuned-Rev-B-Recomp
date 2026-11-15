/* A snapshot of the machine as the difference from another one both PCs
 * have (docs/design.md section 49): the joining PC of a two-PC game has
 * the host's machine as it was at the end of its start-up, so the host
 * sends the machine as it is now as copies from that one, copies from what
 * came before, and the bytes that are new. Nothing here knows about the
 * machine: both are bytes. */
#ifndef VF4FT_SNAP_PATCH_H
#define VF4FT_SNAP_PATCH_H

#include <stddef.h>

/* a fingerprint of bytes (64 bits; not for secrets) */
unsigned long long snap_hash(const unsigned char *p, size_t n);

/* cur as a patch against ref into *out (*out_n bytes, freed with free);
 * ref NULL (ref_n 0): from nothing, cur against itself only. 0, or -1 when
 * memory ran out */
int snap_patch_make(const unsigned char *ref, size_t ref_n, const unsigned char *cur, size_t cur_n,
                    unsigned char **out, size_t *out_n);

/* the bytes a patch was made of, from the same ref, into *out (*out_n bytes,
 * freed with free); a patch made from nothing takes no ref (any given is
 * not used). 0, or -1 when the patch is damaged, is not of this ref, or the
 * result's fingerprint is not the one it was made with */
int snap_patch_apply(const unsigned char *ref, size_t ref_n, const unsigned char *patch, size_t patch_n,
                     unsigned char **out, size_t *out_n);

#endif
