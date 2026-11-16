/* recomp: rollback: see rollback.h and docs/design.md section 55. */
#include "rollback.h"
#include "machine.h"
#include "mem.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct rb_record {
    u32 frame;
    u8 *dev;                /* the models at the frame's end (snapshot_dev_save) */
    u32 n, cap;
    u32 *page;              /* the tracked pages the next frame wrote (mem_track.h numbers) */
    u8 **old;               /* each one's bytes at the frame's end */
} rb_record;

static int on, tracking, broken;
static int slots;           /* depth + 1: the newest record is still taking pages */
static rb_record *rec;
static int head, count;     /* rec[head] is the newest of count */
static size_t dev_size;
static u8 **pool;           /* spare 64 KiB buffers */
static u32 pool_n, pool_total;
static u32 pages_most;
static rb_stats stats;

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void rb_stats_take(rb_stats *s)
{
    *s = stats;
    memset(&stats, 0, sizeof stats);
}

static u8 *page_get(void)
{
    if (pool_n)
        return pool[--pool_n];
    u8 **grown = realloc(pool, (pool_total + 1) * sizeof *pool);
    u8 *b = malloc(MEM_TRACK_PAGE);
    if (!grown || !b) {
        if (grown)
            pool = grown;
        free(b);
        return NULL;
    }
    pool = grown;
    pool_total++;
    return b;
}

static void record_empty(rb_record *r)
{
    for (u32 i = 0; i < r->n; i++)
        pool[pool_n++] = r->old[i];
    r->n = 0;
}

/* mem_track_fn: the page is about to be written for the first time since the
 * newest frame's end */
static void on_first_store(uint32_t page, uint8_t *host)
{
    rb_record *r = &rec[head];
    if (broken)
        return;
    if (r->n == r->cap) {
        u32 cap = r->cap ? 2 * r->cap : 64;
        u32 *pg = realloc(r->page, cap * sizeof *pg);
        if (pg)
            r->page = pg;
        u8 **old = realloc(r->old, cap * sizeof *old);
        if (old)
            r->old = old;
        if (!pg || !old) {
            broken = 1;
            return;
        }
        r->cap = cap;
    }
    double t0 = now_seconds();
    u8 *b = page_get();
    if (!b) {
        broken = 1;
        return;
    }
    memcpy(b, host, MEM_TRACK_PAGE);
    stats.copy_s += now_seconds() - t0;
    stats.pages++;
    r->page[r->n] = page;
    r->old[r->n] = b;
    r->n++;
    if (r->n > pages_most)
        pages_most = r->n;
}

int rb_begin(int depth)
{
    rb_end();
    if (depth < 1)
        return -1;
    slots = depth + 1;
    dev_size = snapshot_dev_size();
    rec = calloc((size_t)slots, sizeof *rec);
    if (!rec)
        return -1;
    for (int i = 0; i < slots; i++)
        if (!(rec[i].dev = malloc(dev_size))) {
            rb_end();
            return -1;
        }
    head = slots - 1;
    count = 0;
    broken = 0;
    pages_most = 0;
    on = 1;
    return 0;
}

void rb_end(void)
{
    if (tracking)
        mem_track_end();
    tracking = 0;
    if (rec) {
        for (int i = 0; i < slots; i++) {
            record_empty(&rec[i]);
            free(rec[i].dev);
            free(rec[i].page);
            free(rec[i].old);
        }
        free(rec);
    }
    rec = NULL;
    for (u32 i = 0; i < pool_n; i++)
        free(pool[i]);
    free(pool);
    pool = NULL;
    pool_n = pool_total = 0;
    count = 0;
    on = 0;
}

int rb_on(void)
{
    return on && !broken;
}

void rb_frame(uint32_t frame)
{
    if (!on || broken)
        return;
    double t0 = now_seconds();
    int next = (head + 1) % slots;
    if (count == slots) {
        record_empty(&rec[next]);       /* the oldest goes */
        count--;
    }
    head = next;
    count++;
    rb_record *r = &rec[head];
    r->frame = frame;
    r->n = 0;
    snapshot_dev_save(r->dev);
    if (!tracking) {
        if (mem_track_begin(on_first_store)) {
            fprintf(stderr, "vf4ft: rollback: the memory cannot be tracked\n");
            broken = 1;
            return;
        }
        tracking = 1;
    } else {
        mem_track_arm(0);
    }
    stats.frames++;
    stats.save_s += now_seconds() - t0;
}

int rb_restore(uint32_t frame)
{
    if (!on || broken || !count)
        return -1;
    int back = 0, at = head;
    while (rec[at].frame != frame) {
        if (++back >= count)
            return -1;
        at = (at - 1 + slots) % slots;
    }
    double t0 = now_seconds();
    /* newest first: what is left in memory is the oldest copy of each page,
     * the one from the end of frame */
    for (int k = 0, i = head; k <= back; k++, i = (i - 1 + slots) % slots) {
        rb_record *r = &rec[i];
        for (u32 j = r->n; j-- > 0;)
            memcpy(mem_track_host_of(r->page[j]), r->old[j], MEM_TRACK_PAGE);
        record_empty(r);
    }
    snapshot_dev_load(rec[at].dev);
    head = at;
    count -= back;
    mem_track_arm(1);
    stats.restores++;
    stats.restore_s += now_seconds() - t0;
    return 0;
}

uint32_t rb_oldest(void)
{
    return count ? rec[(head - count + 1 + slots) % slots].frame : 0;
}

uint32_t rb_newest(void)
{
    return count ? rec[head].frame : 0;
}

size_t rb_bytes(void)
{
    return (size_t)slots * dev_size + (size_t)pool_total * MEM_TRACK_PAGE;
}

uint32_t rb_pages_now(void)
{
    return count ? rec[head].n : 0;
}

uint32_t rb_pages_most(void)
{
    return pages_most;
}

/* ---- the sync test's hashes ---- */

/* four lanes of FNV-1a over 64-bit words (as game_machine_hash) */
static u64 hash_bytes(const u8 *p, size_t n)
{
    const u64 m = 0x100000001b3ull;
    u64 h[4] = { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0x9e3779b97f4a7c15ull, 0x7f4a7c159e3779b9ull };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int j = 0; j < 4; j++) {
            u64 w;
            memcpy(&w, p + i + 8 * j, 8);
            h[j] = (h[j] ^ w) * m;
        }
    for (; i < n; i++)
        h[0] = (h[0] ^ p[i]) * m;
    u64 r = h[0];
    for (int j = 1; j < 4; j++)
        r = (r ^ h[j]) * m;
    return (r ^ n) * m;
}

void rb_digest_take(rb_digest *d)
{
    u32 pages = 0;
    for (int i = 0; i < mem_area_count; i++)
        pages += mem_areas[i].size / 4096;
    if (!d->page || d->pages != pages) {
        free(d->page);
        d->page = calloc(pages, sizeof *d->page);
        d->pages = d->page ? pages : 0;
    }
    const u64 m = 0x100000001b3ull;
    u64 all = 0xcbf29ce484222325ull;
    u32 k = 0;
    for (int i = 0; i < mem_area_count && d->page; i++)
        for (u32 off = 0; off < mem_areas[i].size; off += 4096, k++) {
            d->page[k] = hash_bytes(mem_areas[i].host + off, 4096);
            all = (all ^ d->page[k]) * m;
        }
    static u8 *dev;
    static size_t dev_n;
    size_t need = snapshot_dev_size();
    if (dev_n != need) {
        free(dev);
        dev = malloc(need);
        dev_n = dev ? need : 0;
    }
    d->parts = 0;
    if (dev) {
        snapshot_dev_save(dev);
        const char *name;
        size_t off, len;
        for (int i = 0; i < 96 && snapshot_dev_part(i, &name, &off, &len); i++) {
            d->part[i] = hash_bytes(dev + off, len);
            all = (all ^ d->part[i]) * m;
            d->parts = i + 1;
        }
    }
    d->all = all;
}

void rb_digest_free(rb_digest *d)
{
    free(d->page);
    d->page = NULL;
    d->pages = 0;
}

int rb_digest_diff(const rb_digest *a, const rb_digest *b, FILE *f, const char *what)
{
    int bad = 0, shown = 0;
    if (a->pages == b->pages && a->page && b->page) {
        u32 k = 0;
        for (int i = 0; i < mem_area_count; i++) {
            u32 n = mem_areas[i].size / 4096, run = 0, from = 0, area_bad = 0;
            for (u32 j = 0; j <= n; j++) {
                int differs = j < n && a->page[k + j] != b->page[k + j];
                if (differs) {
                    if (!run)
                        from = j;
                    run++;
                    area_bad++;
                    continue;
                }
                if (run && shown < 24) {
                    fprintf(f, "%s: %s %08x-%08x differs (%u KiB)\n", what, mem_areas[i].name,
                            mem_areas[i].phys + from * 4096, mem_areas[i].phys + (from + run) * 4096 - 1, run * 4);
                    shown++;
                }
                run = 0;
            }
            if (area_bad)
                fprintf(f, "%s: %s: %u of %u 4 KiB pages differ\n", what, mem_areas[i].name, area_bad, n);
            bad += (int)area_bad;
            k += n;
        }
    } else if (a->all != b->all) {
        fprintf(f, "%s: the memory's hashes cannot be compared\n", what);
        bad++;
    }
    for (int i = 0; i < a->parts && i < b->parts; i++)
        if (a->part[i] != b->part[i]) {
            const char *name = "?";
            size_t off, len;
            snapshot_dev_part(i, &name, &off, &len);
            fprintf(f, "%s: the models' state \"%s\" differs (%zu bytes)\n", what, name, len);
            bad++;
        }
    return bad;
}
