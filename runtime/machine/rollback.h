/* recomp: rollback: the machine as it was at the end of each of the last few
 * frames, and the way back to any of them (docs/design.md section 55).
 *
 * Each frame's end keeps the models' state whole (snapshot_dev_save, about
 * 2.3 MB) and, as the next frame runs, the 64 KiB pages of memory it is
 * about to write, before their first store (mem_track.h). Going back to a
 * frame puts the pages back newest first, then that frame's models; the run
 * then goes on from there (rt_native_rewind). */
#ifndef VF4_ROLLBACK_H
#define VF4_ROLLBACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* keeps the last depth frames (after all memory areas are added); 0 on
 * success. Tracking starts at the first rb_frame */
int rb_begin(int depth);
void rb_end(void);
int rb_on(void);

/* the end of a frame: the machine as it is now is that frame's */
void rb_frame(uint32_t frame);

/* memory and the models as they were at the end of frame (one rb_frame was
 * given and no more than depth frames came after it); 0, or -1 when that
 * frame is not kept (nothing changes) */
int rb_restore(uint32_t frame);

/* kept frames, oldest and newest; bytes held (pages and models) */
uint32_t rb_oldest(void);
uint32_t rb_newest(void);
size_t rb_bytes(void);
/* pages kept at the newest frame's end so far, and the most at any one frame */
uint32_t rb_pages_now(void);
uint32_t rb_pages_most(void);

/* what it cost since the last call: frames ended, pages kept, times gone
 * back, and the seconds spent keeping the models, copying pages and going
 * back */
typedef struct rb_stats {
    unsigned frames, pages, restores;
    double save_s, copy_s, restore_s;
} rb_stats;
void rb_stats_take(rb_stats *s);

/* the sync test's hashes: every 4 KiB page of memory, each part of the
 * models' state (snapshot_dev_part), and all of it in one */
typedef struct rb_digest {
    uint64_t all;
    uint32_t pages;
    uint64_t *page;
    int parts;
    uint64_t part[96];
} rb_digest;
void rb_digest_take(rb_digest *d);
void rb_digest_free(rb_digest *d);
/* the differences between a and b to f, each line starting with what; the
 * number of differing pages and parts */
int rb_digest_diff(const rb_digest *a, const rb_digest *b, FILE *f, const char *what);

#endif
