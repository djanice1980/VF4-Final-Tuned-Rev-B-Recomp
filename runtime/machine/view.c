/* The view: a picture of what the game sends to the graphics hardware,
 * drawn by the harness itself so that a person can watch a run.
 *
 * It reads the display lists the ELAN is told to run, the compiled models
 * in the ELAN's memory they link to, the textures in the first rendering
 * chip's video memory and the chips' registers. The game's state does not
 * depend on it: a run with the view on ends in the same state as one with
 * it off. One thing is written into the machine: when the game renders
 * into a texture (the frame buffer registers name a place in texture
 * memory), the picture of that pass is stored there in the pixel format
 * the game asked for, so that the later pass that uses the texture finds
 * it. The game never reads those bytes (see docs/hle.md).
 *
 * What the streams mean is taken from the game's own code as the
 * decompilation shows it, and from the public documentation of the
 * rendering chip's list input, registers and control words:
 *
 *   - sdk/naomi2_library/model_draw.c (model_list_link_queue,
 *     model_volume_link_queue, model_draw_all_as_translucent,
 *     matrix_cmds_build_*): the link command 0x08000800 with the words
 *     that change a model's polygons, the matrix commands 0x08000100
 *     (vertex) and 0x08000200 (normal, with the environment map offsets),
 *     the no-op 0x08000000, and how a model is sent as a modifier volume
 *     with its closing triangle;
 *   - sdk/naomi2_library/naomi2_library.c (proj_cmd_queue): the projection
 *     command 0x08000300;
 *   - sdk/naomi2_library/elan_light_cmds.c and sdk/kamui/elan_commands.c
 *     (elan_light_masks_queue, elan_light_queue, elan_light_pack_*): the
 *     light commands 0x08000400;
 *   - sdk/naomi2_library/model_compile.c (model_list_compile, the dummy
 *     triangles, vertex_bytes_from_format, strip_vertex_code_1/2): a
 *     compiled model is material commands 0x08000500 (64 bytes: the
 *     specular byte, the lighting flags, base and offset colour) and
 *     strips 0x08000700 (a 32-byte header with the chip's four control
 *     words, the vertex format and count, then the vertices); the same
 *     routine's own environment mapping gives the rule the ELAN's is
 *     drawn by;
 *   - sdk/naomi2_library/object_put.c (object_put_init) and the library's
 *     own bump sender (bump_strip_send): the specular curve and the bump
 *     map's light of the library's own lighting, used for the ELAN's;
 *   - sdk/kamui/strip_context.c (context_hw_words_compute,
 *     pvr_shadow_scale_write) and the libsprite senders: the polygon,
 *     sprite and vertex blocks that pass through the ELAN to the chips
 *     unchanged, the control words, the shadow scale register;
 *   - sdk/naomi_library/fog.c, sdk/kamui/pvr_settings.c and the game's
 *     fog_table_build: the fog table, its index and the density register;
 *   - gfxlib/tex_render.c, tex_passes.c (tex_target_regs_load,
 *     texture_fb_write_regs_build, ta_continue_into_target,
 *     tex_target_bg_plane_write): how a pass is drawn into a texture;
 *   - sdk/kamui/chip_access.c (vram64_write): where textures are.
 *
 * The picture is a plain software rasterizer with a depth buffer. The
 * lists that arrive between two "list init / list continue" writes are a
 * scene; a scene is drawn when the game starts the render that names its
 * object lists: opaque, then punch-through with its alpha test, then the
 * modifier volumes of the opaque list (the shadows), then the translucent
 * list from the back. Transform, lighting and rasterizing run on worker
 * threads while the game waits; the picture does not depend on them.
 *
 * Where the game's code cannot say how the chips turn a word into pixels
 * (what lies inside a volume, the ELAN's specular light, the mip-map
 * level, how a link command's words combine with a polygon's) the rule is
 * reasoned out and checked by eye; docs/hle.md lists those.
 *
 * No emulator code was used.
 */
#define _GNU_SOURCE
#include "view.h"
#include "mem.h"
#include "machine.h"
#include "observe.h"
#include "tables.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VW 640                          /* the screen */
#define VH 480
#define FBW 1024                        /* the largest target a pass can have */
#define FBH 1024
#define VRAM1 0x04000000u               /* the first chip's video memory, texture addresses as offsets */
#define VRAM1_32 0x05000000u            /* its second window: where the library writes region arrays and planes */
#define VRAM_SIZE 0x01000000u
#define ELAN_RAM 0x0a000000u

int view_on;

/* ---- what was asked for ---- */

static const char *png_dir;             /* --view DIR */
static int every;                       /* draw every Nth frame the game renders; 0 = not set */
static const char *pipe_cmd;            /* --view-cmd / --live / --video: raw RGB frames to a command */
static FILE *pipe_f;
static int pipe_fps = 60;
static int realtime;                    /* --realtime: 60 vertical blanks a second of real time */
static int marks;                       /* --view-marks: where the game itself says the fighters are */
static u32 view_from;                   /* --view-from N: nothing before this vertical blank */
static int threads_wanted;              /* --view-threads N */
static char pic_path[1024];             /* the script's "picture FILE" */
static int pic_request;
static u32 pic_targets_seen, pic_targets_drawn, pic_waited;
static int verbose_once[16];

static u32 *fb;                         /* 0x00RRGGBB, FBW a row */
static float *zb;                       /* 1/w: larger is nearer */
static u8 *pfb;                         /* per pixel: bit 0 the polygon on top takes shadows */
static u8 *stn;                         /* per pixel: bit 0 inside a modifier volume of the opaque list */
static u8 *shown;                       /* the newest finished picture of the screen, RGB */
static int shown_valid;
static u32 gen = 1;                     /* counts renders: a texture is looked at again after each */
static u32 screens, screen_renders, pictures, target_pictures, png_written, pipe_frames;
static u64 tri_count;
static double draw_seconds, tl_seconds, raster_seconds;

/* ---- worker threads ---- */

#define MAX_THREADS 16
static struct {
    pthread_t th[MAX_THREADS];
    int n, made;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    void (*fn)(int item, void *arg);
    void *arg;
    int items, next;
    int tickets, started, finished;     /* workers still wanted for this turn; that took part; that are back */
} pool = { .mu = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER };

static void pool_work(void)
{
    for (;;) {
        int i = __sync_fetch_and_add(&pool.next, 1);
        if (i >= pool.items)
            break;
        pool.fn(i, pool.arg);
    }
}

static void *pool_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&pool.mu);
    for (;;) {
        while (pool.tickets == 0)
            pthread_cond_wait(&pool.go, &pool.mu);
        pool.tickets--;
        pool.started++;
        pthread_mutex_unlock(&pool.mu);
        pool_work();
        pthread_mutex_lock(&pool.mu);
        if (++pool.finished == pool.started)
            pthread_cond_signal(&pool.done);
    }
    return NULL;
}

/* fn(0 .. items-1), each item once, on this thread and on the workers that come in time
 * (a worker the host keeps waiting is not waited for); back when all items are done */
static void pool_run(void (*fn)(int, void *), int items, void *arg)
{
    if (items <= 0)
        return;
    if (!pool.made) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int want = threads_wanted ? threads_wanted : cpus > 8 ? 8 : (int)cpus;
        if (want > MAX_THREADS)
            want = MAX_THREADS;
        pool.made = 1;
        for (int i = 0; i < want - 1; i++)
            if (!pthread_create(&pool.th[pool.n], NULL, pool_main, NULL))
                pool.n++;
    }
    pool.fn = fn;
    pool.arg = arg;
    pool.items = items;
    pool.next = 0;
    if (pool.n == 0 || items == 1) {
        pool_work();
        return;
    }
    pthread_mutex_lock(&pool.mu);
    pool.tickets = pool.n < items - 1 ? pool.n : items - 1;
    pool.started = pool.finished = 0;
    pthread_cond_broadcast(&pool.go);
    pthread_mutex_unlock(&pool.mu);
    pool_work();
    pthread_mutex_lock(&pool.mu);
    pool.tickets = 0;
    while (pool.finished < pool.started)
        pthread_cond_wait(&pool.done, &pool.mu);
    pthread_mutex_unlock(&pool.mu);
}

/* ---- what a scene is made of ---- */

typedef struct light {
    int on, point, linear;
    float col[3], dir[3], pos[3], ka, kb;
} light;

/* a vertex ready for the rasterizer: screen x, y, 1/w, texture
 * coordinates, base colour and offset colour 0..255 */
typedef struct rv {
    float x, y, z, u, v, r, g, b, a, or_, og, ob, oa;
} rv;
#define RV_FLOATS 13

/* a vertex in the ELAN's view space */
typedef struct vv {
    float X, Y, Z, u, v, r, g, b, a, or_, og, ob, oa;
} vv;

struct tex;
/* how a polygon is drawn: the chip's four control words, its texture, the user clip */
typedef struct rstate {
    u32 pcw, isp, tsp, tcw;
    const struct tex *t;                /* looked up when the scene is drawn: what video memory holds then */
    s16 clip[4];                        /* x0, y0, x1, y1 in pixels, when pcw bits 16-17 say 2 (inside) or 3 (outside) */
    int textured;
} rstate;

typedef struct dtri {
    rv v[3];
    u32 st;                             /* which rstate of the scene */
    float key;
    s16 ylo, yhi;                       /* the rows it can touch */
} dtri;

typedef struct run {                    /* a stretch of a list's triangles */
    u32 first, n;
} run;

/* the ELAN's matrices and projection as a strip found them */
typedef struct xform {
    float m[12], near_z;                /* vertex matrix, by columns of the game's matrix */
    float n[9];                         /* normal matrix */
    float env_u, env_v;                 /* what is added to environment-mapped coordinates */
    float sx, cx, sy, cy;               /* projection: x = cx + sx * X / Z */
} xform;

typedef struct lightset {
    float amb[3];
    light l[16];
} lightset;

/* a strip of a model, kept (with a copy of its vertices) until the scene is drawn */
typedef struct sjob {
    u32 voff, count;                    /* the vertices: offset in the scene's copy, how many */
    u32 first, run;                     /* its triangles: the first of the room kept for them, and its run */
    u32 st, xf, ls;
    u32 base, offset;                   /* the material's colours */
    u16 size, format;
    u8 k;                               /* 0 opaque, 1 punch-through, 2 translucent */
    u8 unlit, env, spec;                /* from the material: no lights; generated coordinates; the specular power */
    u8 flat, use_ofs;
    s8 keep;
} sjob;

/* modifier volumes: triangles on the screen, and which of them make a volume */
typedef struct vtri {
    float x[3], y[3], z[3];
} vtri;
typedef struct vol {
    u32 first, n;
    int kind;                           /* 1: inside it is shadow; 2: inside it is not */
    s16 x0, y0, x1, y1;
} vol;

/* the ELAN's state while a scene's lists run */
typedef struct estate {
    int list;                           /* 0 opaque .. 4 punch-through; -1 between lists */
    xform xf;
    lightset ls;
    u32 light_mask;
    int xf_new, ls_new;                 /* changed since a strip last used them */
    u32 xf_at, ls_at;
    u32 base, offset, mat_flags, mat_spec;      /* the material */
    u32 link_cull, link_or0, link_or1, link_or2;       /* the link command's words */
    s16 clip[4];                        /* the user clip rectangle */
    /* blocks that pass through to the chips */
    u32 pcw;
    rstate st;
    int kind;                           /* 0 none, 1 polygon, 2 sprite, 3 modifier volume */
    int vbytes;
    float face[4], face_ofs[4];
    u32 sprite_base, sprite_ofs;
    rv strip[2];
    int strip_n;
    u32 vol_first[2];                   /* where the volume being built starts */
} estate;

#define GROW(p, cap, need) do { \
        if ((need) > (cap)) { \
            u32 c_ = (cap) ? (cap) : 256; \
            while (c_ < (need)) c_ *= 2; \
            void *n_ = realloc((p), (size_t)c_ * sizeof *(p)); \
            if (!n_) { fprintf(stderr, "view: no memory\n"); exit(1); } \
            (p) = n_; \
            (cap) = c_; \
        } \
    } while (0)

typedef struct scene {
    int used, capture;
    int regular;                        /* one of the frames drawn by turns (not only for the script's picture) */
    u32 key;                            /* the object list base the tile accelerator had when its lists came */
    u32 born;
    estate es;
    dtri *tris[3];
    u32 tris_n[3], tris_cap[3];
    run *runs[3];
    u32 runs_n[3], runs_cap[3];
    int run_open[3];                    /* the last run takes triangles sent straight to the chips */
    rstate *st;
    u32 st_n, st_cap;
    xform *xf;
    u32 xf_n, xf_cap;
    lightset *ls;
    u32 ls_n, ls_cap;
    sjob *jobs;
    u32 jobs_n, jobs_cap;
    u8 *vcopy;
    u32 vcopy_n, vcopy_cap;
    vtri *vt[2];                        /* 0: the opaque list's volumes, 1: the translucent list's */
    u32 vt_n[2], vt_cap[2];
    vol *vols[2];
    u32 vols_n[2], vols_cap[2];
    u32 *order;                         /* the translucent triangles from the back */
    u32 order_n, order_cap;
} scene;

#define SCENES 12
static scene scenes[SCENES];
static scene *cur;                      /* the scene the arriving lists belong to */
static estate es;                       /* its ELAN state */

static u32 bytes_lo, bytes_hi;          /* the game's vertex size tables */
static u8 spec_power[256];              /* material byte to the row of the library's specular table */

/* the target of the scene being drawn, and the registers that matter to all its pixels */
static struct {
    int w, h;
    int to_texture;
    u32 addr, stride, pack;             /* in video memory: where, bytes a row, the pixel format */
    u32 alpha_ref;
    int fog_on;
    u32 fog_tab[129];
    float fog_density;
    u32 fog_col, fog_col_vert;
    int shadow_scale;                   /* 0..256 */
} tg;

/* ---- textures ---- */

typedef struct tex {
    u32 tcw, size_bits, stride;
    int w, h, used, levels;
    u32 *pix;                           /* 0xAARRGGBB, every level in one block */
    u32 *lv[11];                        /* lv[0] the largest */
    u32 sum, checked;
} tex;
#define TEX_SLOTS 4096
static tex texc[TEX_SLOTS];
static u32 tex_used;
static u32 tw_tab[1024];                /* bit k of x at bit 2k */
static u32 tex_decodes;

static u32 tex_sum(const u8 *p, u32 n)
{
    u32 s = n, step = n / 1024 + 1;
    step = (step + 3) & ~3u;
    for (u32 i = 0; i + 4 <= n; i += step)
        s = s * 31 + *(const u32 *)(p + i);
    if (n >= 8)
        s = s * 31 + *(const u32 *)(p + n - 4);
    return s;
}

static inline u32 texel16(u32 v, int fmt)
{
    switch (fmt) {
    case 0: {
        u32 r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
        return (v & 0x8000 ? 0xff000000u : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    case 1: {
        u32 r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
        return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case 2: {
        u32 a = v >> 12 & 15, r = v >> 8 & 15, g = v >> 4 & 15, b = v & 15;
        return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
    case 4:                             /* a bump map: the two angles, kept as they are for the rasterizer */
        return 0xff000000u | (v & 0xffffu);
    default:
        return 0xff808080u;
    }
}

/* the position of texel (x, y) in a twiddled texture: bits interleaved, y
 * in the low bit; a non-square one goes on with the longer side's bits */
static inline u32 twiddle(u32 x, u32 y, u32 w, u32 h)
{
    if (w == h)
        return (u32)tw_tab[x] << 1 | tw_tab[y];
    u32 m = w < h ? w : h, bits = 0;
    while ((1u << bits) < m)
        bits++;
    u32 low = (u32)tw_tab[x & (m - 1)] << 1 | tw_tab[y & (m - 1)];
    return low | ((w > h ? x : y) >> bits) << (2 * bits);
}

/* where the level of side 2^k of a mip-mapped texture starts, in texels
 * (twiddled) or, for k - 1, in index bytes (compressed): the smaller
 * levels come first */
static u32 mip_skip(int k)
{
    u32 s = 1;
    for (int i = 1; i <= k; i++)
        s += i == 1 ? 1 : 1u << (2 * (i - 1));
    return s;
}

static void tex_decode(tex *t, const u8 *src, u32 avail)
{
    u32 tcw = t->tcw;
    int fmt = tcw >> 27 & 7, vq = tcw >> 30 & 1, mip = tcw >> 31, raster = tcw >> 26 & 1;
    int w = t->w, h = t->h;
    tex_decodes++;
    if (fmt > 2 && fmt != 4) {
        /* palette and YUV textures: none seen; a signal colour */
        for (int l = 0; l < t->levels; l++)
            for (int i = 0; i < (w >> l) * (h >> l); i++)
                t->lv[l][i] = 0xffff00ffu;
        return;
    }
    int k = 0;
    while ((1 << k) < w)
        k++;
    if (vq) {
        u32 need = 2048 + (u32)(w / 2) * (h / 2) + (mip && k ? mip_skip(k - 1) : 0);
        if (need > avail) {
            memset(t->pix, 0x80, (size_t)w * h * 4);
            t->levels = 1;
            return;
        }
        u32 book[1024];
        for (int i = 0; i < 1024; i++)
            book[i] = texel16(*(const u16 *)(src + 2 * i), fmt);
        for (int l = 0; l < t->levels; l++) {
            int lw = w >> l, lh = h >> l, j = k - l;
            const u8 *idx = src + 2048 + (mip ? (j ? mip_skip(j - 1) : 0) : 0);
            u32 *out = t->lv[l];
            if (lw < 2) {
                out[0] = book[idx[0] * 4];
                continue;
            }
            for (int y = 0; y < lh; y += 2)
                for (int x = 0; x < lw; x += 2) {
                    const u32 *e = &book[idx[twiddle(x / 2, y / 2, lw / 2, lh / 2)] * 4];
                    u32 *o = &out[y * lw + x];
                    o[0] = e[0];
                    o[lw] = e[1];
                    o[1] = e[2];
                    o[lw + 1] = e[3];
                }
        }
        return;
    }
    if (raster && !mip) {
        /* rows, with the texture's own width or the one the stride register gives */
        u32 stride = t->stride ? t->stride : (u32)w;
        if (((u32)(h - 1) * stride + w) * 2 > avail) {
            memset(t->pix, 0x80, (size_t)w * h * 4);
            return;
        }
        const u16 *px = (const u16 *)src;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                t->pix[y * w + x] = texel16(px[(u32)y * stride + x], fmt);
        return;
    }
    if (((u32)w * h + (mip ? mip_skip(k) : 0)) * 2 > avail) {
        memset(t->pix, 0x80, (size_t)w * h * 4);
        t->levels = 1;
        return;
    }
    for (int l = 0; l < t->levels; l++) {
        int lw = w >> l, lh = h >> l;
        const u16 *px = (const u16 *)(src + (mip ? mip_skip(k - l) * 2 : 0));
        u32 *out = t->lv[l];
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++)
                out[y * lw + x] = texel16(px[twiddle(x, y, lw, lh)], fmt);
    }
}

static void tex_flush(void)
{
    for (u32 i = 0; i < TEX_SLOTS; i++) {
        free(texc[i].pix);
        memset(&texc[i], 0, sizeof texc[i]);
    }
    tex_used = 0;
}

static const tex *tex_find(u32 tsp, u32 tcw)
{
    u32 size_bits = tsp & 0x3f;
    u32 stride = (tcw >> 25 & 1) && (tcw >> 26 & 1) ? (view_core_reg(0, 0xe4) & 31) * 32 : 0;
    u32 hsh = (tcw * 2654435761u ^ size_bits * 40503u) >> 16;
    tex *t = NULL;
    for (u32 i = 0; i < TEX_SLOTS; i++) {
        tex *c = &texc[(hsh + i) & (TEX_SLOTS - 1)];
        if (!c->used || (c->tcw == tcw && c->size_bits == size_bits && c->stride == stride)) {
            t = c;
            break;
        }
    }
    if (!t || (!t->used && tex_used >= TEX_SLOTS * 3 / 4))
        return NULL;                    /* full: emptied after this render (render_started) */
    if (t->used && t->checked == gen)
        return t;
    int w = 8 << (size_bits >> 3 & 7), h = 8 << (size_bits & 7);
    u32 addr = (tcw & 0x1fffffu) << 3;
    const u8 *src = mem_ptr(VRAM1 + addr);
    if (!src || addr >= VRAM_SIZE)
        return NULL;
    u32 avail = VRAM_SIZE - addr;
    u32 n = (tcw >> 30 & 1) ? 2048 + (u32)(w / 2) * (h / 2) : (stride ? (u32)h * stride : (u32)w * h) * 2;
    if (tcw >> 31)
        n += n / 3;
    if (n > avail)
        n = avail;
    u32 sum = tex_sum(src, n);
    if (!t->used) {
        int mip = tcw >> 31 && w == h, levels = 1;
        size_t texels = (size_t)w * h;
        if (mip)
            for (int s = w >> 1; s >= 1; s >>= 1, levels++)
                texels += (size_t)s * s;
        t->pix = malloc(texels * 4);
        if (!t->pix)
            return NULL;
        u32 *p = t->pix;
        for (int l = 0; l < levels; l++) {
            t->lv[l] = p;
            p += (size_t)(w >> l) * (h >> l);
        }
        t->levels = levels;
        t->used = 1;
        t->tcw = tcw;
        t->size_bits = size_bits;
        t->stride = stride;
        t->w = w;
        t->h = h;
        t->sum = ~sum;
        tex_used++;
    }
    if (t->sum != sum) {
        tex_decode(t, src, avail);
        t->sum = sum;
    }
    t->checked = gen;
    return t;
}

static const tex *tex_get(u32 tsp, u32 tcw)
{
    /* the strips of a model ask for the same one over and over */
    static const tex *last;
    static u32 last_tsp, last_tcw, last_gen;
    if (last && last_tcw == tcw && ((last_tsp ^ tsp) & 0x3f) == 0 && last_gen == gen)
        return last;
    last = tex_find(tsp, tcw);
    last_tsp = tsp;
    last_tcw = tcw;
    last_gen = gen;
    return last;
}

/* ---- the rasterizer ---- */

static inline u32 blend_factor(int f, u32 other, u32 a)
{
    /* per channel, 0..255, packed as 0x00RRGGBB weights */
    switch (f) {
    case 0: return 0;
    case 1: return 0x00ffffffu;
    case 2: return other & 0x00ffffffu;
    case 3: return ~other & 0x00ffffffu;
    case 4: return a * 0x010101u;
    case 5: return (255 - a) * 0x010101u;
    case 6: return 0x00ffffffu;         /* the frame's own alpha: kept as opaque */
    default: return 0;
    }
}

static inline u32 mul_add(u32 s, u32 sf, u32 d, u32 df)
{
    u32 r = ((s >> 16 & 255) * (sf >> 16 & 255) + (d >> 16 & 255) * (df >> 16 & 255) + 127) / 255;
    u32 g = ((s >> 8 & 255) * (sf >> 8 & 255) + (d >> 8 & 255) * (df >> 8 & 255) + 127) / 255;
    u32 b = ((s & 255) * (sf & 255) + (d & 255) * (df & 255) + 127) / 255;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return r << 16 | g << 8 | b;
}

/* a texel coordinate brought into the texture: clamped, mirrored or repeated */
static inline int tex_at(int t, int size, int clamp, int flip)
{
    if (clamp)
        return t < 0 ? 0 : t >= size ? size - 1 : t;
    if (flip && (t & size))
        return ~t & (size - 1);
    return t & (size - 1);
}

/* How much fog a pixel at depth 1/w takes, 0..255. The table is the game's
 * (fog_table_build): 128 steps, the step number the power of two of
 * density / w in its upper three bits and the next four bits of it in the
 * lower four; each register holds a step and the next one, and the bits
 * after those four go between the two. */
static inline u32 fog_at(float z)
{
    float s = z * tg.fog_density;
    if (!(s > 1.0f))
        s = 1.0f;
    else if (s > 255.9999f)
        s = 255.9999f;
    union { float f; u32 u; } b = { s };
    u32 i = (b.u >> 19) - (127u << 4), fr = b.u >> 11 & 255;
    return (tg.fog_tab[i] * (256 - fr) + tg.fog_tab[i + 1] * fr) >> 8;
}

/* what a thread keeps while it draws its bands */
typedef struct frag {
    float z;
    s32 next;
    u32 vol;
} frag;
static __thread u8 *th_tog;             /* one bit of work per pixel of a band */
static __thread s32 *th_head;           /* per pixel of a band: its chain of translucent volume faces */
static __thread frag *th_frag;
static __thread u32 th_frag_n, th_frag_cap;
static __thread int th_band_lo;
static __thread int th_tvol;            /* the band has translucent volumes */

/* whether a translucent pixel at depth z lies in one of the translucent list's
 * volumes: an odd number of the volume's faces in front of it */
static int tvol_inside(int x, int y, float z)
{
    s32 f = th_head[(y - th_band_lo) * FBW + x];
    u32 v = 0xffffffffu, odd = 0;
    while (f >= 0) {
        const frag *g = &th_frag[f];
        if (g->vol != v) {
            if (odd)
                return 1;
            v = g->vol;
        }
        if (g->z > z)
            odd ^= 1;
        f = g->next;
    }
    return (int)odd;
}

/* the three edges of a triangle on a grid of sixteenths of a pixel: e(p) =
 * A * (px - xa) + B * (py - ya), inside where all are >= 0; a pixel centre
 * exactly on an edge belongs to one of the two triangles that share it */
typedef struct edges {
    double x0, y0, x1, y1, x2, y2, area;
    double ea[3], eb[3], ec[3];
    int xmin, xmax, ymin, ymax, swapped;
} edges;

static int edges_set(edges *e, float ax, float ay, float bx, float by, float cx, float cy, int lo, int hi)
{
#define SNAP(v) ((int)((double)(v) * 16.0 + 16777216.5) - 16777216)
    int ix0 = SNAP(ax), iy0 = SNAP(ay), ix1 = SNAP(bx), iy1 = SNAP(by), ix2 = SNAP(cx), iy2 = SNAP(cy);
#undef SNAP
    e->x0 = ix0 / 16.0; e->y0 = iy0 / 16.0; e->x1 = ix1 / 16.0; e->y1 = iy1 / 16.0; e->x2 = ix2 / 16.0; e->y2 = iy2 / 16.0;
    e->area = (e->x1 - e->x0) * (e->y2 - e->y0) - (e->x2 - e->x0) * (e->y1 - e->y0);
    e->swapped = 0;
    if (e->area == 0)
        return 0;
    if (e->area < 0) {
        double d = e->x1; e->x1 = e->x2; e->x2 = d;
        d = e->y1; e->y1 = e->y2; e->y2 = d;
        e->area = -e->area;
        e->swapped = 1;
    }
    int ilx = ix0 < ix1 ? ix0 : ix1, ihx = ix0 > ix1 ? ix0 : ix1, ily = iy0 < iy1 ? iy0 : iy1, ihy = iy0 > iy1 ? iy0 : iy1;
    if (ix2 < ilx) ilx = ix2;
    if (ix2 > ihx) ihx = ix2;
    if (iy2 < ily) ily = iy2;
    if (iy2 > ihy) ihy = iy2;
    /* the pixels whose centres can be inside */
    e->xmin = (ilx + 7) >> 4; e->xmax = (ihx - 8) >> 4; e->ymin = (ily + 7) >> 4; e->ymax = (ihy - 8) >> 4;
    if (e->xmin < 0) e->xmin = 0;
    if (e->xmax > tg.w - 1) e->xmax = tg.w - 1;
    if (e->ymin < lo) e->ymin = lo;
    if (e->ymax > hi) e->ymax = hi;
    if (e->xmin > e->xmax || e->ymin > e->ymax)
        return 0;
    const double ex[3] = { e->x0, e->x1, e->x2 }, ey[3] = { e->y0, e->y1, e->y2 };
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        e->ea[i] = -(ey[j] - ey[i]);
        e->eb[i] = ex[j] - ex[i];
        double bias = (e->ea[i] > 0 || (e->ea[i] == 0 && e->eb[i] > 0)) ? 0 : -1.0 / 512;
        e->ec[i] = e->ea[i] * (e->xmin + 0.5 - ex[i]) + e->eb[i] * (e->ymin + 0.5 - ey[i]) + bias;
    }
    return 1;
}

/* the pixels of a row the triangle covers: xl..xr, or 0. er: the edge values at the
 * row's first pixel; qr: where each edge crosses the row, the exact value settling the
 * last pixel */
static inline int edges_span(const edges *e, const double *er, const double *qr, int *xl_out, int *xr_out)
{
    int xl = 0, xr = e->xmax - e->xmin;
    for (int i = 0; i < 3; i++) {
        double v = er[i], A = e->ea[i];
        if (A > 0) {
            if (v >= 0)
                continue;
            double at = qr[i];
            if (at > xr + 1)
                return 0;
            int k = (int)at;
            while (v + A * k < 0)
                k++;
            while (k > 0 && v + A * (k - 1) >= 0)
                k--;
            if (k > xl)
                xl = k;
        } else if (A < 0) {
            if (v < 0)
                return 0;
            double at = qr[i];
            if (at > xr + 1)
                continue;
            int k = (int)at;
            while (k >= 0 && v + A * k < 0)
                k--;
            while (k < xr && v + A * (k + 1) >= 0)
                k++;
            if (k < xr)
                xr = k;
        } else if (v < 0)
            return 0;
    }
    if (xl > xr)
        return 0;
    *xl_out = xl + e->xmin;
    *xr_out = xr + e->xmin;
    return 1;
}

/* mode: 0 opaque, 1 punch-through (alpha test), 2 translucent (blend, no depth write) */
static void raster(const dtri *d, const rstate *st, int mode, int band_lo, int band_hi)
{
    const rv *p0 = &d->v[0], *p1 = &d->v[1], *p2 = &d->v[2];
    const tex *t = st->t;
    u32 tsp = st->tsp, isp = st->isp, pcw = st->pcw;
    int depth_mode = isp >> 29;
    if (depth_mode == 0)
        return;
    int clip_mode = pcw >> 16 & 3;
    if (clip_mode == 2) {
        if (band_lo < st->clip[1]) band_lo = st->clip[1];
        if (band_hi > st->clip[3]) band_hi = st->clip[3];
    }
    edges e;
    if (!edges_set(&e, p0->x, p0->y, p1->x, p1->y, p2->x, p2->y, band_lo, band_hi))
        return;
    if (e.swapped) {
        const rv *sw = p1; p1 = p2; p2 = sw;
    }
    int ymin = e.ymin, ymax = e.ymax;
    double x0 = e.x0, y0 = e.y0, x1 = e.x1, y1 = e.y1, x2 = e.x2, y2 = e.y2;

    double inv = 1.0 / e.area;
#define GRAD(f0, f1, f2, dx, dy) do { \
        double d1 = (double)(f1) - (f0), d2 = (double)(f2) - (f0); \
        dx = (float)((d1 * (y2 - y0) - d2 * (y1 - y0)) * inv); \
        dy = (float)((d2 * (x1 - x0) - d1 * (x2 - x0)) * inv); \
    } while (0)
    float dzx, dzy, dux = 0, duy = 0, dvx = 0, dvy = 0;
    GRAD(p0->z, p1->z, p2->z, dzx, dzy);
    float u0z = 0, v0z = 0;
    if (t) {
        u0z = p0->u * p0->z;
        v0z = p0->v * p0->z;
        GRAD(u0z, p1->u * p1->z, p2->u * p2->z, dux, duy);
        GRAD(v0z, p1->v * p1->z, p2->v * p2->z, dvx, dvy);
    }
    int flat = p0->r == p1->r && p0->r == p2->r && p0->g == p1->g && p0->g == p2->g && p0->b == p1->b &&
               p0->b == p2->b && p0->a == p1->a && p0->a == p2->a;
    float drx = 0, dry = 0, dgx = 0, dgy = 0, dbx = 0, dby = 0, dax = 0, day = 0;
    if (!flat) {
        GRAD(p0->r, p1->r, p2->r, drx, dry);
        GRAD(p0->g, p1->g, p2->g, dgx, dgy);
        GRAD(p0->b, p1->b, p2->b, dbx, dby);
        GRAD(p0->a, p1->a, p2->a, dax, day);
    }
    /* the offset colour: added after the texture; its alpha is the fog of a polygon with fog by vertex */
    int fog_mode = tsp >> 22 & 3;
    int bump = t && (st->tcw >> 27 & 7) == 4;
    int has_ofs = (pcw >> 2 & 1) && t &&
                  (p0->or_ != 0 || p0->og != 0 || p0->ob != 0 || p1->or_ != 0 || p1->og != 0 || p1->ob != 0 ||
                   p2->or_ != 0 || p2->og != 0 || p2->ob != 0 || fog_mode == 1 || bump);
    float dorx = 0, dory = 0, dogx = 0, dogy = 0, dobx = 0, doby = 0, doax = 0, doay = 0;
    if (has_ofs) {
        GRAD(p0->or_, p1->or_, p2->or_, dorx, dory);
        GRAD(p0->og, p1->og, p2->og, dogx, dogy);
        GRAD(p0->ob, p1->ob, p2->ob, dobx, doby);
        GRAD(p0->oa, p1->oa, p2->oa, doax, doay);
    }
#undef GRAD

    int zwrite = !(isp >> 26 & 1) && mode != 2;
    int pass_gt = depth_mode == 4 || depth_mode == 5 || depth_mode == 6 || depth_mode == 7;
    int pass_eq = depth_mode == 2 || depth_mode == 3 || depth_mode == 6 || depth_mode == 7;
    int pass_lt = depth_mode == 1 || depth_mode == 3 || depth_mode == 5 || depth_mode == 7;
    int shade = tsp >> 6 & 3, use_alpha = tsp >> 20 & 1, no_tex_alpha = tsp >> 19 & 1;
    int clamp_u = tsp >> 16 & 1, clamp_v = tsp >> 15 & 1, flip_u = tsp >> 18 & 1, flip_v = tsp >> 17 & 1;
    int sf = tsp >> 29, df = tsp >> 26 & 7;
    u32 alpha_ref = tg.alpha_ref;
    int lv = 0, tw = t ? t->w : 0, th = t ? t->h : 0;
    const u32 *tpix = t ? t->pix : NULL;
    float twf = (float)tw, thf = (float)th, tw0 = twf, th0 = thf;
    /* the smaller levels of a mip-mapped texture: by how many texels a pixel covers,
     * times the adjustment the polygon asks for (bits 8-11 of the third word, in quarters) */
    int maxl = t ? t->levels - 1 : 0;
    float dadj = (float)(tsp >> 8 & 15) * 0.25f, dadj2 = dadj * dadj;
    if (dadj == 0)
        maxl = 0;
    /* four texels blended where the chip filters (bits 13-14 of the third word) */
    int smooth = t && (tsp >> 13 & 3) && !bump;
    int plain_depth = pass_gt && !pass_lt, wrap_uv = !clamp_u && !clamp_v && !flip_u && !flip_v;
    int icr = (int)(drx * 65536.0f), icg = (int)(dgx * 65536.0f), icb = (int)(dbx * 65536.0f), ica = (int)(dax * 65536.0f);
    int ior = (int)(dorx * 65536.0f), iog = (int)(dogx * 65536.0f), iob = (int)(dobx * 65536.0f), ioa = (int)(doax * 65536.0f);
    int fog_lut = tg.fog_on && (fog_mode == 0 || fog_mode == 3), fog_vert = fog_mode == 1 && has_ofs;
    u32 fog_col = fog_vert ? tg.fog_col_vert : tg.fog_col;
    int fcr = fog_col >> 16 & 255, fcg = fog_col >> 8 & 255, fcb = fog_col & 255;
    u8 takes_shadow = pcw >> 7 & 1;
    int tshadow = mode == 2 && takes_shadow && th_tvol && tg.shadow_scale < 256;
    int clip_out = clip_mode == 3;
    int cx0 = st->clip[0], cy0 = st->clip[1], cx1 = st->clip[2], cy1 = st->clip[3];

    double q0[3] = { 0, 0, 0 }, qs[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++)
        if (e.ea[i] != 0) {
            q0[i] = -e.ec[i] / e.ea[i];
            qs[i] = -e.eb[i] / e.ea[i];
        }
    double er[3] = { e.ec[0], e.ec[1], e.ec[2] }, qr[3] = { q0[0], q0[1], q0[2] };
    for (int y = ymin; y <= ymax; y++, er[0] += e.eb[0], er[1] += e.eb[1], er[2] += e.eb[2], qr[0] += qs[0], qr[1] += qs[1],
             qr[2] += qs[2]) {
        int xl, xr;
        if (!edges_span(&e, er, qr, &xl, &xr))
            continue;
        if (clip_mode == 2) {
            if (xl < cx0) xl = cx0;
            if (xr > cx1) xr = cx1;
            if (xl > xr)
                continue;
        }
        int row_out = clip_out && y >= cy0 && y <= cy1;
        float px = (float)(xl + 0.5 - x0), py = (float)(y + 0.5 - y0);
        float z = p0->z + dzx * px + dzy * py;
        float uz = u0z + dux * px + duy * py, vz = v0z + dvx * px + dvy * py;
        /* colours as 16.16 fixed point, 0..255 */
        int cr = (int)((p0->r + drx * px + dry * py) * 65536.0f), cg = (int)((p0->g + dgx * px + dgy * py) * 65536.0f);
        int cb = (int)((p0->b + dbx * px + dby * py) * 65536.0f), ca = (int)((p0->a + dax * px + day * py) * 65536.0f);
        int co_r = 0, co_g = 0, co_b = 0, co_a = 0;
        if (has_ofs) {
            co_r = (int)((p0->or_ + dorx * px + dory * py) * 65536.0f);
            co_g = (int)((p0->og + dogx * px + dogy * py) * 65536.0f);
            co_b = (int)((p0->ob + dobx * px + doby * py) * 65536.0f);
            co_a = (int)((p0->oa + doax * px + doay * py) * 65536.0f);
        }
        u32 *frow = fb + y * FBW;
        float *zrow = zb + y * FBW;
        u8 *prow = pfb + y * FBW;
        /* texture coordinates: exact every eighth pixel, straight between */
        float ua = 0, va = 0, dua = 0, dva = 0;
        int seg = 0;
        for (int x = xl; x <= xr; x++, z += dzx, uz += dux, vz += dvx, ua += dua, va += dva, seg--, cr += icr, cg += icg,
                 cb += icb, ca += ica, co_r += ior, co_g += iog, co_b += iob, co_a += ioa) {
            if (tpix && seg <= 0) {
                static const float inv_n[8] = { 0, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7 };
                int len = xr - x + 1;
                if (len > 8)
                    len = 8;
                float w = 1.0f / z;
                ua = uz * w;
                va = vz * w;
                dua = dva = 0;
                if (len > 1) {
                    float n = (float)(len - 1), w1 = 1.0f / (z + dzx * n);
                    dua = ((uz + dux * n) * w1 - ua) * inv_n[len - 1];
                    dva = ((vz + dvx * n) * w1 - va) * inv_n[len - 1];
                }
                seg = len;
                if (maxl) {
                    float ax = (dux - ua * dzx) * w * tw0, bx = (dvx - va * dzx) * w * th0;
                    float ay = (duy - ua * dzy) * w * tw0, by = (dvy - va * dzy) * w * th0;
                    float r2 = ax * ax + bx * bx, r2y = ay * ay + by * by;
                    if (r2y > r2)
                        r2 = r2y;
                    r2 *= dadj2;
                    int l = 0;
                    if (r2 > 2.0f) {
                        union { float f; u32 u; } q = { r2 };
                        l = (int)((q.u >> 23) - 126) >> 1;
                        if (l > maxl)
                            l = maxl;
                    }
                    if (l != lv) {
                        lv = l;
                        tpix = t->lv[l];
                        tw = t->w >> l;
                        th = t->h >> l;
                        twf = (float)tw;
                        thf = (float)th;
                    }
                }
            }
            float dz = zrow[x];
            if (plain_depth) {
                if (pass_eq ? z < dz : z <= dz)
                    continue;
            } else if (!((z > dz && pass_gt) || (z == dz && pass_eq) || (z < dz && pass_lt)))
                continue;
            if (row_out && x >= cx0 && x <= cx1)
                continue;
            int ir = cr >> 16, ig = cg >> 16, ib = cb >> 16, ia = use_alpha ? ca >> 16 : 255;
            if ((unsigned)ir > 255) ir = ir < 0 ? 0 : 255;
            if ((unsigned)ig > 255) ig = ig < 0 ? 0 : 255;
            if ((unsigned)ib > 255) ib = ib < 0 ? 0 : 255;
            if ((unsigned)ia > 255) ia = ia < 0 ? 0 : 255;
            if (tpix) {
                u32 c;
                if (smooth) {
                    float fu = ua * twf + 16383.5f, fv = va * thf + 16383.5f;
                    int tx = (int)fu, ty = (int)fv;
                    u32 ax = (u32)((fu - (float)tx) * 256.0f), ay = (u32)((fv - (float)ty) * 256.0f);
                    int x0i = tex_at(tx - 16384, tw, clamp_u, flip_u), x1i = tex_at(tx - 16383, tw, clamp_u, flip_u);
                    int y0i = tex_at(ty - 16384, th, clamp_v, flip_v) * tw, y1i = tex_at(ty - 16383, th, clamp_v, flip_v) * tw;
                    u32 c00 = tpix[y0i + x0i], c10 = tpix[y0i + x1i], c01 = tpix[y1i + x0i], c11 = tpix[y1i + x1i];
                    u32 rb0 = ((c00 & 0xff00ffu) * (256 - ax) + (c10 & 0xff00ffu) * ax) >> 8 & 0xff00ffu;
                    u32 ag0 = ((c00 >> 8 & 0xff00ffu) * (256 - ax) + (c10 >> 8 & 0xff00ffu) * ax) >> 8 & 0xff00ffu;
                    u32 rb1 = ((c01 & 0xff00ffu) * (256 - ax) + (c11 & 0xff00ffu) * ax) >> 8 & 0xff00ffu;
                    u32 ag1 = ((c01 >> 8 & 0xff00ffu) * (256 - ax) + (c11 >> 8 & 0xff00ffu) * ax) >> 8 & 0xff00ffu;
                    c = ((rb0 * (256 - ay) + rb1 * ay) >> 8 & 0xff00ffu) | ((ag0 * (256 - ay) + ag1 * ay) & 0xff00ff00u);
                } else {
                    int tx = (int)(ua * twf + 16384.0f), ty = (int)(va * thf + 16384.0f);
                    if (wrap_uv) {
                        tx &= tw - 1;
                        ty &= th - 1;
                    } else {
                        tx = tex_at(tx - 16384, tw, clamp_u, flip_u);
                        ty = tex_at(ty - 16384, th, clamp_v, flip_v);
                    }
                    c = tpix[ty * tw + tx];
                }
                if (bump) {
                    /* a bump map: the texel is two angles (up from the surface, round it), the
                     * offset colour the light: brightness = K1 + K2 sin S + K3 cos S cos (R - Q) */
                    float s_ang = (float)(c >> 8 & 255) * (float)(M_PI / 2 / 256), r_ang = (float)(c & 255) * (float)(2 * M_PI / 256);
                    float k1 = (float)(co_a >> 16) / 255.0f, k2 = (float)(co_r >> 16) / 255.0f, k3 = (float)(co_g >> 16) / 255.0f;
                    float q_ang = (float)(co_b >> 16) * (float)(2 * M_PI / 256);
                    float i_ = k1 + k2 * sinf(s_ang) + k3 * cosf(s_ang) * cosf(r_ang - q_ang);
                    int v = (int)(i_ * 255.0f);
                    v = v < 0 ? 0 : v > 255 ? 255 : v;
                    c = (u32)v << 24 | (u32)v << 16 | (u32)v << 8 | (u32)v;
                }
                int ta = no_tex_alpha ? 255 : (int)(c >> 24);
                int tr = c >> 16 & 255, tg_ = c >> 8 & 255, tb = c & 255;
                /* x * y / 255 as (x * (y + y / 128)) / 256 */
                if (shade & 1) {
                    ir = tr * (ir + (ir >> 7)) >> 8;
                    ig = tg_ * (ig + (ig >> 7)) >> 8;
                    ib = tb * (ib + (ib >> 7)) >> 8;
                    ia = shade == 1 ? ta : ta * (ia + (ia >> 7)) >> 8;
                } else if (shade == 0) {
                    ir = tr;
                    ig = tg_;
                    ib = tb;
                    ia = ta;
                } else {
                    int k = ta + (ta >> 7);
                    ir = (tr * k + ir * (256 - k)) >> 8;
                    ig = (tg_ * k + ig * (256 - k)) >> 8;
                    ib = (tb * k + ib * (256 - k)) >> 8;
                }
                if (has_ofs && !bump) {
                    ir += co_r >> 16;
                    ig += co_g >> 16;
                    ib += co_b >> 16;
                    if ((unsigned)ir > 255) ir = ir < 0 ? 0 : 255;
                    if ((unsigned)ig > 255) ig = ig < 0 ? 0 : 255;
                    if ((unsigned)ib > 255) ib = ib < 0 ? 0 : 255;
                }
            }
            if (mode == 1 && (u32)ia < alpha_ref)
                continue;
            if (fog_lut) {
                int f = (int)fog_at(z);
                if (fog_mode == 3) {
                    ir = fcr;
                    ig = fcg;
                    ib = fcb;
                    ia = f;
                } else if (f) {
                    f += f >> 7;
                    ir += (fcr - ir) * f >> 8;
                    ig += (fcg - ig) * f >> 8;
                    ib += (fcb - ib) * f >> 8;
                }
            } else if (fog_vert) {
                int f = co_a >> 16;
                f = f < 0 ? 0 : f > 255 ? 256 : f + (f >> 7);
                ir += (fcr - ir) * f >> 8;
                ig += (fcg - ig) * f >> 8;
                ib += (fcb - ib) * f >> 8;
            }
            if (tshadow && tvol_inside(x, y, z)) {
                ir = ir * tg.shadow_scale >> 8;
                ig = ig * tg.shadow_scale >> 8;
                ib = ib * tg.shadow_scale >> 8;
            }
            u32 src = (u32)ir << 16 | (u32)ig << 8 | (u32)ib;
            if (mode == 2) {
                u32 dst = frow[x];
                if (sf == 4 && df == 5) {
                    if (ia == 0)
                        continue;
                    int k = ia + (ia >> 7), nk = 256 - k;
                    src = (((u32)ir * k + (dst >> 16 & 255) * nk) >> 8) << 16 |
                          (((u32)ig * k + (dst >> 8 & 255) * nk) >> 8) << 8 | (((u32)ib * k + (dst & 255) * nk) >> 8);
                } else if (sf == 4 && df == 1) {
                    int k = ia + (ia >> 7);
                    u32 or_ = ((u32)ir * k >> 8) + (dst >> 16 & 255), og = ((u32)ig * k >> 8) + (dst >> 8 & 255);
                    u32 ob = ((u32)ib * k >> 8) + (dst & 255);
                    src = (or_ > 255 ? 255 : or_) << 16 | (og > 255 ? 255 : og) << 8 | (ob > 255 ? 255 : ob);
                } else {
                    src = mul_add(src, blend_factor(sf, dst, (u32)ia), dst, blend_factor(df, src, (u32)ia));
                }
            } else
                prow[x] = takes_shadow;
            frow[x] = src;
            if (zwrite)
                zrow[x] = z;
        }
    }
}

/* a volume triangle's depth as a plane over the screen, and the rows it covers */
typedef struct vspan {
    edges e;
    float dzx, dzy;
    double er[3], qr[3], qs[3];
} vspan;

static int vspan_set(vspan *s, const vtri *t, int lo, int hi)
{
    if (!edges_set(&s->e, t->x[0], t->y[0], t->x[1], t->y[1], t->x[2], t->y[2], lo, hi))
        return 0;
    double d1x = (double)t->x[1] - t->x[0], d1y = (double)t->y[1] - t->y[0], d2x = (double)t->x[2] - t->x[0],
           d2y = (double)t->y[2] - t->y[0];
    double det = d1x * d2y - d2x * d1y;
    if (det == 0)
        return 0;
    double z1 = (double)t->z[1] - t->z[0], z2 = (double)t->z[2] - t->z[0];
    s->dzx = (float)((z1 * d2y - z2 * d1y) / det);
    s->dzy = (float)((z2 * d1x - z1 * d2x) / det);
    for (int k = 0; k < 3; k++) {
        s->er[k] = s->e.ec[k];
        s->qr[k] = s->e.ea[k] != 0 ? -s->e.ec[k] / s->e.ea[k] : 0;
        s->qs[k] = s->e.ea[k] != 0 ? -s->e.eb[k] / s->e.ea[k] : 0;
    }
    return 1;
}

static inline void vspan_next(vspan *s)
{
    for (int k = 0; k < 3; k++) {
        s->er[k] += s->e.eb[k];
        s->qr[k] += s->qs[k];
    }
}

/* One modifier volume in a band. The chip's rule, as its public
 * description gives it: a pixel is inside a volume when an odd number of
 * the volume's triangles lie in front of what is drawn there. */
static void volume_band(const scene *sc, const vol *v, int lo, int hi)
{
    int x0 = v->x0 < 0 ? 0 : v->x0, x1 = v->x1 > tg.w - 1 ? tg.w - 1 : v->x1;
    int y0 = v->y0 < lo ? lo : v->y0, y1 = v->y1 > hi ? hi : v->y1;
    if (x0 > x1 || y0 > y1)
        return;
    for (int y = y0; y <= y1; y++)
        memset(th_tog + (y - lo) * FBW + x0, 0, (size_t)(x1 - x0 + 1));
    const vtri *t = sc->vt[0] + v->first;
    for (u32 i = 0; i < v->n; i++, t++) {
        vspan s;
        if (!vspan_set(&s, t, y0, y1))
            continue;
        for (int y = s.e.ymin; y <= s.e.ymax; y++, vspan_next(&s)) {
            int xl, xr;
            if (!edges_span(&s.e, s.er, s.qr, &xl, &xr))
                continue;
            float z = t->z[0] + s.dzx * (float)(xl + 0.5 - t->x[0]) + s.dzy * (float)(y + 0.5 - t->y[0]);
            const float *zrow = zb + y * FBW;
            u8 *trow = th_tog + (y - lo) * FBW;
            for (int x = xl; x <= xr; x++, z += s.dzx)
                if (z > zrow[x])
                    trow[x] ^= 1;
        }
    }
    for (int y = y0; y <= y1; y++) {
        const u8 *trow = th_tog + (y - lo) * FBW;
        u8 *srow = stn + y * FBW;
        if (v->kind == 1) {
            for (int x = x0; x <= x1; x++)
                srow[x] |= trow[x];
        } else
            for (int x = x0; x <= x1; x++)
                srow[x] &= (u8)~trow[x];
    }
}

/* the faces of the translucent list's volumes, chained per pixel of the band */
static void tvolumes_band(const scene *sc, int lo, int hi)
{
    th_tvol = 0;
    if (!sc->vols_n[1])
        return;
    for (int i = 0; i < (hi - lo + 1) * FBW; i++)
        th_head[i] = -1;
    th_frag_n = 0;
    for (u32 n = 0; n < sc->vols_n[1]; n++) {
        const vol *v = &sc->vols[1][n];
        if (v->kind != 1 || v->y1 < lo || v->y0 > hi)
            continue;
        const vtri *t = sc->vt[1] + v->first;
        for (u32 i = 0; i < v->n; i++, t++) {
            vspan s;
            if (!vspan_set(&s, t, lo, hi))
                continue;
            for (int y = s.e.ymin; y <= s.e.ymax; y++, vspan_next(&s)) {
                int xl, xr;
                if (!edges_span(&s.e, s.er, s.qr, &xl, &xr))
                    continue;
                float z = t->z[0] + s.dzx * (float)(xl + 0.5 - t->x[0]) + s.dzy * (float)(y + 0.5 - t->y[0]);
                const float *zrow = zb + y * FBW;
                s32 *hrow = th_head + (y - lo) * FBW;
                for (int x = xl; x <= xr; x++, z += s.dzx) {
                    if (z <= zrow[x])
                        continue;       /* behind what is opaque there: no translucent pixel can be behind it */
                    if (th_frag_n == th_frag_cap) {
                        u32 cap = th_frag_cap ? th_frag_cap * 2 : 16384;
                        frag *nf = realloc(th_frag, (size_t)cap * sizeof *nf);
                        if (!nf)
                            return;
                        th_frag = nf;
                        th_frag_cap = cap;
                    }
                    frag *g = &th_frag[th_frag_n];
                    g->z = z;
                    g->vol = n;
                    g->next = hrow[x];
                    hrow[x] = (s32)th_frag_n++;
                    th_tvol = 1;
                }
            }
        }
    }
}

/* ---- from the ELAN's view space to the screen ---- */

typedef struct tlctx {
    const xform *xf;
    dtri *out;
    u32 n, st;
    int flat;
    float near_z;
} tlctx;

static inline void project(const xform *xf, const vv *p, rv *o)
{
    float z = 1.0f / p->Z;
    o->x = xf->cx + xf->sx * p->X * z;
    o->y = xf->cy + xf->sy * p->Y * z;
    o->z = z;
    memcpy(&o->u, &p->u, (RV_FLOATS - 3) * sizeof(float));
}

/* fills a triangle's record; 0 when it cannot be seen */
static inline int tri_fill(dtri *d, const rv *a, const rv *b, const rv *c, u32 st)
{
    float lox = a->x < b->x ? a->x : b->x, hix = a->x > b->x ? a->x : b->x;
    float loy = a->y < b->y ? a->y : b->y, hiy = a->y > b->y ? a->y : b->y;
    if (c->x < lox) lox = c->x;
    if (c->x > hix) hix = c->x;
    if (c->y < loy) loy = c->y;
    if (c->y > hiy) hiy = c->y;
    /* off every target, or not a number */
    if (!(hix >= 0 && lox < FBW && hiy >= 0 && loy < FBH && lox > -1e6f && hix < 1e6f && loy > -1e6f && hiy < 1e6f))
        return 0;
    d->v[0] = *a;
    d->v[1] = *b;
    d->v[2] = *c;
    d->st = st;
    d->key = (a->z + b->z + c->z) * (1.0f / 3);
    d->ylo = (s16)(loy < 0 ? 0 : (int)loy);
    d->yhi = (s16)(hiy > FBH - 1 ? FBH - 1 : (int)hiy);
    return 1;
}

/* keep: 0 both faces; +1 or -1 only the triangles that turn that way on
 * the screen (culling by the strip's control word) */
static void tri_view(tlctx *c, const vv *a, const vv *b, const vv *cc, int keep)
{
    float nz = c->near_z;
    vv fa, fb_;
    if (c->flat) {
        /* one colour for the triangle: its last vertex's */
        fa = *a;
        fb_ = *b;
        memcpy(&fa.r, &cc->r, 8 * sizeof(float));
        memcpy(&fb_.r, &cc->r, 8 * sizeof(float));
        a = &fa;
        b = &fb_;
    }
    const vv *in[3] = { a, b, cc };
    int inside = (a->Z >= nz) + (b->Z >= nz) + (cc->Z >= nz);
    if (inside == 0)
        return;
    rv o[4];
    if (inside == 3) {
        project(c->xf, a, &o[0]);
        project(c->xf, b, &o[1]);
        project(c->xf, cc, &o[2]);
        if (keep && ((o[1].x - o[0].x) * (o[2].y - o[0].y) - (o[2].x - o[0].x) * (o[1].y - o[0].y)) * (float)keep <= 0)
            return;
        c->n += (u32)tri_fill(&c->out[c->n], &o[0], &o[1], &o[2], c->st);
        return;
    }
    vv cl[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const vv *p = in[i], *q = in[(i + 1) % 3];
        int pin = p->Z >= nz, qin = q->Z >= nz;
        if (pin)
            cl[n++] = *p;
        if (pin != qin) {
            float t = (nz - p->Z) / (q->Z - p->Z);
            vv *r = &cl[n++];
            const float *fp = &p->X, *fq = &q->X;
            float *fr = &r->X;
            for (int k = 0; k < RV_FLOATS; k++)
                fr[k] = fp[k] + (fq[k] - fp[k]) * t;
            r->Z = nz;
        }
    }
    for (int i = 0; i < n; i++)
        project(c->xf, &cl[i], &o[i]);
    if (keep && n >= 3 &&
        ((o[1].x - o[0].x) * (o[2].y - o[0].y) - (o[2].x - o[0].x) * (o[1].y - o[0].y)) * (float)keep <= 0)
        return;
    for (int i = 2; i < n; i++)
        c->n += (u32)tri_fill(&c->out[c->n], &o[0], &o[i - 1], &o[i], c->st);
}

static int vertex_bytes(u32 format)
{
    if (bytes_lo && bytes_hi)
        return (int)(g_r32(bytes_lo + 4 * (format & 0x1f)) + g_r32(bytes_hi + 4 * (format >> 5 & 0x1f)));
    return 16 + (format & 4 ? 16 : 0) + (format & 8 ? 8 : 0) + (format & 0x40 ? 8 : 0) + (format & 0x100 ? 16 : 0);
}

/* One strip of a compiled model: its vertices through the vertex matrix,
 * the lights, the near plane and the projection, into the room kept for
 * its triangles. Runs on any thread. */
static void strip_job(scene *sc, sjob *j)
{
    const xform *xf = &sc->xf[j->xf];
    const lightset *ls = &sc->ls[j->ls];
    const u8 *p = sc->vcopy + j->voff;
    u32 format = j->format;
    int size = j->size;
    int has_uv = format >> 3 & 1, has_col = format >> 6 & 1, bumped = format >> 8 & 1;
    int uv_at = 16 + (format & 4 ? 16 : 0), col_at = uv_at + (has_uv ? 8 : 0);
    float br = (float)(j->base >> 16 & 255), bg = (float)(j->base >> 8 & 255), bb = (float)(j->base & 255);
    float ba = (float)(j->base >> 24);
    float sr = (float)(j->offset >> 16 & 255), sg = (float)(j->offset >> 8 & 255), sb = (float)(j->offset & 255);
    int spec = j->use_ofs && j->spec && (j->offset & 0xffffff) && !j->unlit;
    const float *m = xf->m, *nm = xf->n;
    tlctx c = { xf, sc->tris[j->k] + j->first, 0, j->st, j->flat, xf->near_z > 0 ? xf->near_z : 0.05f };
    int keep = j->keep, odd = 0;
    vv first, before, prev;             /* the strip's first vertex, and the last two */
    memset(&first, 0, sizeof first);
    before = prev = first;
    int have = 0;
    for (u32 i = 0; i < j->count; i++, p += size) {
        u32 ctl = *(const u32 *)p;
        const float *f = (const float *)(p + 4);
        vv v;
        v.X = f[0] * m[0] + f[1] * m[3] + f[2] * m[6] + m[9];
        v.Y = f[0] * m[1] + f[1] * m[4] + f[2] * m[7] + m[10];
        v.Z = f[0] * m[2] + f[1] * m[5] + f[2] * m[8] + m[11];
        if (has_uv) {
            v.u = *(const float *)(p + uv_at);
            v.v = *(const float *)(p + uv_at + 4);
            if (!(fabsf(v.u) < 1e4f)) v.u = 0;
            if (!(fabsf(v.v) < 1e4f)) v.v = 0;
        } else
            v.u = v.v = 0;
        v.or_ = v.og = v.ob = v.oa = 0;
        u32 cb = has_col && !bumped ? *(const u32 *)(p + col_at) : 0, co = cb ? *(const u32 *)(p + col_at + 4) : 0;
        if (cb) {
            /* colours of its own, base and offset (the two words are zero in the fighters' skinned strips) */
            v.r = (float)(cb >> 16 & 255);
            v.g = (float)(cb >> 8 & 255);
            v.b = (float)(cb & 255);
            v.a = (float)(cb >> 24);
            v.or_ = (float)(co >> 16 & 255);
            v.og = (float)(co >> 8 & 255);
            v.ob = (float)(co & 255);
            v.oa = (float)(co >> 24);
        } else if (j->unlit && !j->env) {
            v.r = br;
            v.g = bg;
            v.b = bb;
            v.a = ba;
        } else {
            /* the lights, per vertex, in the game's view space (the normal
             * matrix goes there; the vertex matrix has x and z turned round) */
            float nx = (float)(s8)ctl, ny = (float)(s8)(ctl >> 8), nzz = (float)(s8)(ctl >> 16);
            float gx = nx * nm[0] + ny * nm[1] + nzz * nm[2];
            float gy = nx * nm[3] + ny * nm[4] + nzz * nm[5];
            float gz = nx * nm[6] + ny * nm[7] + nzz * nm[8];
            float len = sqrtf(gx * gx + gy * gy + gz * gz);
            float lr = ls->amb[0], lg = ls->amb[1], lb = ls->amb[2];
            float pr = 0, pg = 0, pb = 0;       /* the specular sum */
            float ex = 0, ey = 0, ez = 0;       /* from the vertex to the eye */
            float k2 = 0, k3 = 1, q_ang = 0;    /* a bump map's light */
            if (len > 0) {
                float il = 1.0f / len;
                gx *= il; gy *= il; gz *= il;
                if (spec) {
                    float el = sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
                    if (el > 0) {
                        ex = v.X / el;
                        ey = -v.Y / el;
                        ez = v.Z / el;
                    }
                }
                for (int k = 0; k < 15; k++) {
                    const light *l = &ls->l[k];
                    if (!l->on)
                        continue;
                    float d, at = 1, tx, ty, tz;        /* t: towards the light */
                    if (l->point) {
                        float dx = l->pos[0] + v.X, dy = l->pos[1] - v.Y, dz = l->pos[2] + v.Z;
                        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (dist <= 0)
                            continue;
                        tx = dx / dist; ty = dy / dist; tz = dz / dist;
                        at = l->linear ? l->ka * dist + l->kb : l->ka / dist + l->kb;
                        at = at < 0 ? 0 : at > 1 ? 1 : at;
                    } else {
                        tx = -l->dir[0]; ty = -l->dir[1]; tz = -l->dir[2];     /* dir: the way the light travels */
                    }
                    d = (gx * tx + gy * ty + gz * tz) * at;
                    if (d > 0) {
                        lr += l->col[0] * d;
                        lg += l->col[1] * d;
                        lb += l->col[2] * d;
                    }
                    if (spec && d > 0) {
                        /* the library's own curve (object_put_init): with c the cosine between the
                         * normal and the direction half way between light and eye, (2c^2 - 1)^2 to
                         * the material's power, nothing below c = 0.7071 */
                        float hx = tx + ex, hy = ty + ey, hz = tz + ez;
                        float hl = sqrtf(hx * hx + hy * hy + hz * hz);
                        if (hl > 0) {
                            float cs = (gx * hx + gy * hy + gz * hz) / hl;
                            if (cs > 0.70710678f) {
                                float w2 = 2 * cs * cs - 1;
                                float s = powf(w2 * w2, (float)j->spec) * at;
                                pr += l->col[0] * s;
                                pg += l->col[1] * s;
                                pb += l->col[2] * s;
                            }
                        }
                    }
                    if (bumped && k == 0) {
                        /* the light a bump map takes, as the library's own bump sender packs it
                         * (bump_strip_send): how high the light stands over the surface, and where
                         * round it in the plane of the two vectors each vertex carries */
                        u32 tw_ = *(const u32 *)(p + 28), bw_ = *(const u32 *)(p + 32);
                        float t0 = (float)(s8)tw_, t1 = (float)(s8)(tw_ >> 8), t2 = (float)(s8)(tw_ >> 16);
                        float b0 = (float)(s8)bw_, b1 = (float)(s8)(bw_ >> 8), b2 = (float)(s8)(bw_ >> 16);
                        float ttx = t0 * nm[0] + t1 * nm[1] + t2 * nm[2], tty = t0 * nm[3] + t1 * nm[4] + t2 * nm[5];
                        float ttz = t0 * nm[6] + t1 * nm[7] + t2 * nm[8];
                        float bbx = b0 * nm[0] + b1 * nm[1] + b2 * nm[2], bby = b0 * nm[3] + b1 * nm[4] + b2 * nm[5];
                        float bbz = b0 * nm[6] + b1 * nm[7] + b2 * nm[8];
                        float nl = gx * tx + gy * ty + gz * tz;
                        nl = nl < 0 ? 0 : nl > 1 ? 1 : nl;
                        k2 = nl;
                        k3 = sqrtf(1 - nl * nl);
                        q_ang = atan2f(bbx * tx + bby * ty + bbz * tz, ttx * tx + tty * ty + ttz * tz);
                    }
                }
            }
            if (j->unlit)
                lr = lg = lb = 1;
            v.r = br * (lr > 1 ? 1 : lr);
            v.g = bg * (lg > 1 ? 1 : lg);
            v.b = bb * (lb > 1 ? 1 : lb);
            v.a = ba;
            if (spec) {
                v.or_ = sr * (pr > 1 ? 1 : pr);
                v.og = sg * (pg > 1 ? 1 : pg);
                v.ob = sb * (pb > 1 ? 1 : pb);
            }
            if (bumped) {
                /* K1 the part that does not depend on the light, the rest shared out by K2 and K3 */
                u32 kw = *(const u32 *)(p + 24);
                float k1 = (float)(kw >> 8 & 255) / 255.0f, depth = (float)(kw & 255) / 255.0f;
                float q = q_ang < 0 ? q_ang + (float)(2 * M_PI) : q_ang;
                v.r = v.g = v.b = 255;
                v.oa = k1 * 255.0f;
                v.or_ = depth * k2 * 255.0f;
                v.og = depth * k3 * 255.0f;
                v.ob = q * (float)(256 / (2 * M_PI));
            }
            if (j->env && len > 0) {
                /* environment mapping as model_list_compile does it itself when the ELAN is not
                 * asked to: the normal in view space, halved and centred, plus the two offsets */
                v.u = (gx + 1.0f) * 0.5f + xf->env_u;
                v.v = (gy + 1.0f) * 0.5f + xf->env_v;
            }
        }
        /* bits 29-30: 0 the first two vertices of a strip (or of a triangle), 3 the
         * third, 1 a strip vertex, 2 a fan vertex; bit 31 the last (strip_vertex_code_1/2) */
        int code = ctl >> 29 & 3;
        if (have == 0) {
            first = prev = v;
            have = 1;
        } else if (have == 1) {
            before = prev;
            prev = v;
            have = 2;
        } else {
            if (code == 3)
                odd = 0;
            else if (code == 1)
                odd ^= 1;
            if (code == 2)
                tri_view(&c, &first, &prev, &v, keep);
            else if (code)
                tri_view(&c, &before, &prev, &v, odd ? -keep : keep);
            if (code == 3)
                first = before;
            before = prev;
            prev = v;
        }
        if (ctl >> 31)
            have = 0;
    }
    sc->runs[j->k][j->run].n = c.n;
}

static void strip_job_item(int i, void *arg)
{
    scene *sc = arg;
    strip_job(sc, &sc->jobs[i]);
}

/* ---- collecting a scene ---- */

static u32 state_add(scene *sc, const rstate *st)
{
    if (sc->st_n && !memcmp(&sc->st[sc->st_n - 1], st, sizeof *st))
        return sc->st_n - 1;
    GROW(sc->st, sc->st_cap, sc->st_n + 1);
    sc->st[sc->st_n] = *st;
    return sc->st_n++;
}

static inline int list_slot(int list)
{
    return list == 0 ? 0 : list == 4 ? 1 : list == 2 ? 2 : -1;
}

/* a triangle sent to the chips already transformed */
static void emit(const rv *a, const rv *b, const rv *c, const rstate *st)
{
    scene *sc = cur;
    int k = list_slot(es.list);
    if (k < 0 || (st->isp >> 29) == 0)
        return;
    GROW(sc->tris[k], sc->tris_cap[k], sc->tris_n[k] + 1);
    dtri *d = &sc->tris[k][sc->tris_n[k]];
    rv fa, fb_;
    if (!(st->pcw >> 1 & 1)) {
        fa = *a;
        fb_ = *b;
        memcpy(&fa.r, &c->r, 8 * sizeof(float));
        memcpy(&fb_.r, &c->r, 8 * sizeof(float));
        a = &fa;
        b = &fb_;
    }
    if (!tri_fill(d, a, b, c, state_add(sc, st)))
        return;
    if (!sc->run_open[k]) {
        GROW(sc->runs[k], sc->runs_cap[k], sc->runs_n[k] + 1);
        sc->runs[k][sc->runs_n[k]].first = sc->tris_n[k];
        sc->runs[k][sc->runs_n[k]++].n = 0;
        sc->run_open[k] = 1;
    }
    sc->runs[k][sc->runs_n[k] - 1].n++;
    sc->tris_n[k]++;
}

/* a triangle of a modifier volume, on the screen */
static void vol_tri(int which, const float *x, const float *y, const float *z)
{
    scene *sc = cur;
    for (int i = 0; i < 3; i++)
        if (!(x[i] > -1e6f && x[i] < 1e6f && y[i] > -1e6f && y[i] < 1e6f))
            return;
    GROW(sc->vt[which], sc->vt_cap[which], sc->vt_n[which] + 1);
    vtri *t = &sc->vt[which][sc->vt_n[which]++];
    memcpy(t->x, x, sizeof t->x);
    memcpy(t->y, y, sizeof t->y);
    memcpy(t->z, z, sizeof t->z);
}

/* the triangle that closes a volume has come: what was collected since the last one is a volume */
static void vol_close(int which, int kind)
{
    scene *sc = cur;
    u32 first = es.vol_first[which], n = sc->vt_n[which] - first;
    es.vol_first[which] = sc->vt_n[which];
    if (!n)
        return;
    GROW(sc->vols[which], sc->vols_cap[which], sc->vols_n[which] + 1);
    vol *v = &sc->vols[which][sc->vols_n[which]++];
    v->first = first;
    v->n = n;
    v->kind = kind;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 i = first; i < first + n; i++)
        for (int k = 0; k < 3; k++) {
            const vtri *t = &sc->vt[which][i];
            if (t->x[k] < x0) x0 = t->x[k];
            if (t->x[k] > x1) x1 = t->x[k];
            if (t->y[k] < y0) y0 = t->y[k];
            if (t->y[k] > y1) y1 = t->y[k];
        }
    v->x0 = (s16)(x0 < 0 ? 0 : x0 > FBW ? FBW : x0);
    v->y0 = (s16)(y0 < 0 ? 0 : y0 > FBH ? FBH : y0);
    v->x1 = (s16)(x1 < 0 ? -1 : x1 > FBW - 1 ? FBW - 1 : x1);
    v->y1 = (s16)(y1 < 0 ? -1 : y1 > FBH - 1 ? FBH - 1 : y1);
}

/* a volume's triangle in view space: cut at the near plane, projected */
static void vol_tri_view(int which, const float (*p)[3])
{
    const xform *xf = &es.xf;
    float nz = xf->near_z > 0 ? xf->near_z : 0.05f;
    float cl[4][3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const float *a = p[i], *b = p[(i + 1) % 3];
        int ain = a[2] >= nz, bin = b[2] >= nz;
        if (ain)
            memcpy(cl[n++], a, sizeof cl[0]);
        if (ain != bin) {
            float t = (nz - a[2]) / (b[2] - a[2]);
            cl[n][0] = a[0] + (b[0] - a[0]) * t;
            cl[n][1] = a[1] + (b[1] - a[1]) * t;
            cl[n++][2] = nz;
        }
    }
    float x[4], y[4], z[4];
    for (int i = 0; i < n; i++) {
        z[i] = 1.0f / cl[i][2];
        x[i] = xf->cx + xf->sx * cl[i][0] * z[i];
        y[i] = xf->cy + xf->sy * cl[i][1] * z[i];
    }
    for (int i = 2; i < n; i++) {
        float tx[3] = { x[0], x[i - 1], x[i] }, ty[3] = { y[0], y[i - 1], y[i] }, tz[3] = { z[0], z[i - 1], z[i] };
        vol_tri(which, tx, ty, tz);
    }
}

/* one strip of a compiled model: 32 bytes of header, then the vertices */
static u32 model_strip(u32 addr, u32 end)
{
    scene *sc = cur;
    u32 h[8];
    if (g_read(addr, h, 32))
        return end;
    u32 format = h[6], count = h[7];
    int size = vertex_bytes(format);
    if (size < 16 || count > 0x100000 || addr + 32 + (u64)size * count > end)
        return end;
    u32 next = addr + 32 + (u32)size * count;
    const u8 *p = mem_ptr(addr + 32);
    if (!p || count < 3)
        return next;
    if (es.list == 1 || es.list == 3) {
        /* a model sent as a modifier volume (model_volume_link_queue): only where its
         * triangles are counts; the triangle with 1 or 2 in the top bits of its second
         * word closes the volume (the dummy triangle the library links after it) */
        int which = es.list == 3, kind = (h[1] ^ es.link_or1) >> 29 & 3;
        const float *m = es.xf.m;
        float first[3] = { 0, 0, 0 }, before[3] = { 0, 0, 0 }, prev[3] = { 0, 0, 0 };
        int have = 0;
        for (u32 i = 0; i < count; i++, p += size) {
            u32 ctl = *(const u32 *)p;
            const float *f = (const float *)(p + 4);
            float v[3] = { f[0] * m[0] + f[1] * m[3] + f[2] * m[6] + m[9], f[0] * m[1] + f[1] * m[4] + f[2] * m[7] + m[10],
                           f[0] * m[2] + f[1] * m[5] + f[2] * m[8] + m[11] };
            int code = ctl >> 29 & 3;
            if (have == 0) {
                memcpy(first, v, sizeof v);
                memcpy(prev, v, sizeof v);
                have = 1;
            } else if (have == 1) {
                memcpy(before, prev, sizeof v);
                memcpy(prev, v, sizeof v);
                have = 2;
            } else {
                float t[3][3];
                memcpy(t[0], code == 2 ? first : before, sizeof v);
                memcpy(t[1], prev, sizeof v);
                memcpy(t[2], v, sizeof v);
                if (code)
                    vol_tri_view(which, (const float (*)[3])t);
                if (code == 3)
                    memcpy(first, before, sizeof v);
                memcpy(before, prev, sizeof v);
                memcpy(prev, v, sizeof v);
            }
            if (ctl >> 31)
                have = 0;
        }
        if (kind == 1 || kind == 2)
            vol_close(which, kind);
        return next;
    }
    int k = list_slot(es.list);
    if (k < 0)
        return next;
    /* The words of the link command change the model's polygons: the shadow bit and the
     * user clip in the first, fog, filter and blend in the third. The ELAN combines them
     * bit by bit, a set bit turning the polygon's bit over: the library sends the bits it
     * wants XORed with the ones it knows the polygon has (model_volume_link_queue: the
     * polygon's own depth bits to make them 0, and for the closing triangle the model's
     * clip bits XOR the wanted ones; the translucent draw of an opaque model: its blend
     * XOR the opaque one, m ^ 0x20000000). */
    rstate st;
    memset(&st, 0, sizeof st);
    st.pcw = h[0] ^ (es.link_or0 & 0x000300c0u);
    st.isp = h[1] ^ (es.link_or1 & 0xe0000000u);
    st.tsp = h[2] ^ es.link_or2;
    st.tcw = h[3];
    int has_uv = format >> 3 & 1;
    /* textured with coordinates of its own, or with the ones the ELAN makes from the normals */
    st.textured = (h[0] >> 3 & 1) && (has_uv || (es.mat_flags >> 11 & 3));
    memcpy(st.clip, es.clip, sizeof st.clip);
    if ((st.isp >> 29) == 0)
        return next;
    if (es.xf_new) {
        GROW(sc->xf, sc->xf_cap, sc->xf_n + 1);
        sc->xf[sc->xf_n] = es.xf;
        es.xf_at = sc->xf_n++;
        es.xf_new = 0;
    }
    if (es.ls_new) {
        GROW(sc->ls, sc->ls_cap, sc->ls_n + 1);
        sc->ls[sc->ls_n] = es.ls;
        for (int i = 0; i < 16; i++)
            if (!(es.light_mask >> i & 1))
                sc->ls[sc->ls_n].l[i].on = 0;
        es.ls_at = sc->ls_n++;
        es.ls_new = 0;
    }
    u32 st_at = state_add(sc, &st);
    /* culling: bits 27-28 of the strip's second word say which way its triangles
     * turn (2 or 3; 0 and 1 keep both faces); a link command of a mirrored model
     * turns them round; the triangles of a strip turn alternately */
    int cull = h[1] >> 27 & 3, keep = cull < 2 ? 0 : cull == 2 ? 1 : -1;
    if (es.link_cull == 2)
        keep = -keep;
    if (es.xf.sx * es.xf.sy < 0)
        keep = -keep;
    u32 bytes = (u32)size * count;
    GROW(sc->vcopy, sc->vcopy_cap, sc->vcopy_n + bytes);
    memcpy(sc->vcopy + sc->vcopy_n, p, bytes);
    /* a block holds many strips, each ended by bit 31 of its last vertex: every 256
     * vertices or so the block is cut at such an end, so that a large model is shared
     * out among the threads */
    for (u32 from = 0; from < count;) {
        u32 n = count - from;
        if (n > 384) {
            n = 256;
            while (from + n < count && !(*(const u32 *)(p + (size_t)(from + n - 1) * size) >> 31))
                n++;
        }
        GROW(sc->jobs, sc->jobs_cap, sc->jobs_n + 1);
        sjob *j = &sc->jobs[sc->jobs_n++];
        memset(j, 0, sizeof *j);
        j->voff = sc->vcopy_n + from * (u32)size;
        j->count = n;
        j->size = (u16)size;
        j->format = (u16)format;
        j->st = st_at;
        j->xf = es.xf_at;
        j->ls = es.ls_at;
        j->base = es.base;
        j->offset = es.offset;
        j->k = (u8)k;
        /* the material's second word (model_list_compile): bit 9 no lights, bits 11-12
         * coordinates from the normal; its first the specular power */
        j->unlit = es.mat_flags >> 9 & 1;
        j->env = (es.mat_flags >> 11 & 3) != 0;
        j->spec = spec_power[es.mat_spec & 255];
        j->flat = !(st.pcw >> 1 & 1);
        j->use_ofs = st.pcw >> 2 & 1;
        j->keep = (s8)keep;
        /* room for its triangles: n vertices make at most n - 2, each cut by the near plane into at most two */
        u32 room = n > 2 ? 2 * (n - 2) : 0;
        GROW(sc->tris[k], sc->tris_cap[k], sc->tris_n[k] + room);
        GROW(sc->runs[k], sc->runs_cap[k], sc->runs_n[k] + 1);
        j->first = sc->tris_n[k];
        j->run = sc->runs_n[k];
        sc->runs[k][sc->runs_n[k]].first = j->first;
        sc->runs[k][sc->runs_n[k]++].n = 0;
        sc->tris_n[k] += room;
        from += n;
    }
    sc->run_open[k] = 0;
    sc->vcopy_n += bytes;
    return next;
}

/* ---- blocks that pass through to the chips ---- */

static inline float f32(u32 w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static inline void rgba_of(u32 argb, rv *o)
{
    o->a = (float)(argb >> 24);
    o->r = (float)(argb >> 16 & 255);
    o->g = (float)(argb >> 8 & 255);
    o->b = (float)(argb & 255);
}

static inline void ofs_of(u32 argb, rv *o)
{
    o->oa = (float)(argb >> 24);
    o->or_ = (float)(argb >> 16 & 255);
    o->og = (float)(argb >> 8 & 255);
    o->ob = (float)(argb & 255);
}

static inline float clamp255(float f)
{
    f *= 255.0f;
    return f < 0 ? 0 : f > 255 ? 255 : f;
}

static inline void uv16(u32 w, rv *o)
{
    o->u = f32(w & 0xffff0000u);
    o->v = f32(w << 16);
}

static void chip_header(const u32 *w, int para)
{
    es.pcw = w[0];
    if (es.list < 0)
        es.list = w[0] >> 24 & 7;      /* the list is the one its first block names, until the end of list */
    es.strip_n = 0;
    memset(&es.st, 0, sizeof es.st);
    es.st.pcw = w[0];
    es.st.isp = w[1];
    es.st.tsp = w[2];
    es.st.tcw = w[3];
    memcpy(es.st.clip, es.clip, sizeof es.st.clip);
    if (es.list == 1 || es.list == 3) {
        es.kind = 3;
        es.vbytes = 64;
        return;
    }
    es.st.textured = w[0] >> 3 & 1;
    if (para == 5) {
        es.kind = 2;
        es.vbytes = 64;
        es.sprite_base = w[4];
        es.sprite_ofs = w[5];
        es.st.pcw |= 2;                 /* one colour: nothing to pick */
        return;
    }
    es.kind = 1;
    int txd = w[0] >> 3 & 1, col = w[0] >> 4 & 3, two = w[0] >> 6 & 1;
    es.vbytes = txd && (col == 1 || two) ? 64 : 32;
    if (col == 2 && !(txd && (w[0] >> 2 & 1)) && !two) {
        for (int i = 0; i < 4; i++)
            es.face[i] = f32(w[4 + i]);
        memset(es.face_ofs, 0, sizeof es.face_ofs);
    }
}

static void chip_vertex(const u32 *w)
{
    u32 pcw = es.pcw;
    int txd = pcw >> 3 & 1, col = pcw >> 4 & 3, uv_short = pcw & 1, two = pcw >> 6 & 1, ofs = pcw >> 2 & 1;
    if (es.kind == 3) {
        /* a modifier volume's triangle, three corners; the header's top bits close the volume */
        int which = es.list == 3, kind = es.st.isp >> 29 & 3;
        float x[3] = { f32(w[1]), f32(w[4]), f32(w[7]) }, y[3] = { f32(w[2]), f32(w[5]), f32(w[8]) };
        float z[3] = { f32(w[3]), f32(w[6]), f32(w[9]) };
        vol_tri(which, x, y, z);
        if (kind == 1 || kind == 2)
            vol_close(which, kind);
        return;
    }
    if (es.kind == 2) {
        /* a sprite: four corners, three texture coordinates */
        rv q[4];
        float base[3][3] = { { f32(w[1]), f32(w[2]), f32(w[3]) }, { f32(w[4]), f32(w[5]), f32(w[6]) },
                             { f32(w[7]), f32(w[8]), f32(w[9]) } };
        for (int i = 0; i < 3; i++) {
            q[i].x = base[i][0];
            q[i].y = base[i][1];
            q[i].z = base[i][2];
            rgba_of(es.sprite_base, &q[i]);
            ofs_of(ofs ? es.sprite_ofs : 0, &q[i]);
            uv16(w[13 + i], &q[i]);
        }
        q[3] = q[0];
        q[3].x = f32(w[10]);
        q[3].y = f32(w[11]);
        q[3].u = q[0].u + q[2].u - q[1].u;
        q[3].v = q[0].v + q[2].v - q[1].v;
        /* the fourth corner's depth lies in the plane of the other three */
        double ax = q[1].x - q[0].x, ay = q[1].y - q[0].y, bx = q[2].x - q[0].x, by = q[2].y - q[0].y;
        double det = ax * by - ay * bx;
        if (det != 0) {
            double dx = q[3].x - q[0].x, dy = q[3].y - q[0].y;
            double s = (dx * by - dy * bx) / det, t = (ax * dy - ay * dx) / det;
            q[3].z = (float)(q[0].z + s * (q[1].z - q[0].z) + t * (q[2].z - q[0].z));
        }
        emit(&q[0], &q[1], &q[2], &es.st);
        emit(&q[0], &q[2], &q[3], &es.st);
        return;
    }
    if (es.kind != 1 || two)
        return;                         /* a polygon with two sets of parameters: not drawn (none seen) */
    rv v;
    memset(&v, 0, sizeof v);
    v.x = f32(w[1]);
    v.y = f32(w[2]);
    v.z = f32(w[3]);
    if (!txd) {
        if (col == 0)
            rgba_of(w[6], &v);
        else if (col == 1) {
            v.a = clamp255(f32(w[4])); v.r = clamp255(f32(w[5])); v.g = clamp255(f32(w[6])); v.b = clamp255(f32(w[7]));
        } else {
            float k = f32(w[6]);
            v.a = clamp255(es.face[0]); v.r = clamp255(es.face[1] * k); v.g = clamp255(es.face[2] * k);
            v.b = clamp255(es.face[3] * k);
        }
    } else {
        if (uv_short)
            uv16(w[4], &v);
        else {
            v.u = f32(w[4]);
            v.v = f32(w[5]);
        }
        if (col == 0) {
            rgba_of(w[6], &v);
            if (ofs)
                ofs_of(w[7], &v);
        } else if (col == 1) {
            v.a = clamp255(f32(w[8])); v.r = clamp255(f32(w[9])); v.g = clamp255(f32(w[10])); v.b = clamp255(f32(w[11]));
            if (ofs) {
                v.oa = clamp255(f32(w[12])); v.or_ = clamp255(f32(w[13])); v.og = clamp255(f32(w[14]));
                v.ob = clamp255(f32(w[15]));
            }
        } else {
            float k = f32(w[6]);
            v.a = clamp255(es.face[0]); v.r = clamp255(es.face[1] * k); v.g = clamp255(es.face[2] * k);
            v.b = clamp255(es.face[3] * k);
            if (ofs) {
                float ko = f32(w[7]);
                v.oa = clamp255(es.face_ofs[0]); v.or_ = clamp255(es.face_ofs[1] * ko);
                v.og = clamp255(es.face_ofs[2] * ko); v.ob = clamp255(es.face_ofs[3] * ko);
            }
        }
    }
    if (es.strip_n >= 2)
        emit(&es.strip[0], &es.strip[1], &v, &es.st);
    es.strip[0] = es.strip[1];
    es.strip[1] = v;
    if (es.strip_n < 2)
        es.strip_n++;
    if (w[0] >> 28 & 1)
        es.strip_n = 0;
}

/* ---- the command stream ---- */

static void light_cmd(const u32 *w)
{
    if (w[1] & 0x10) {
        /* the light set's header (elan_light_masks_queue): which lights are on, the
         * ambient colour; the lights follow */
        es.ls.amb[0] = (float)(w[3] >> 16 & 255) / 255.0f;
        es.ls.amb[1] = (float)(w[3] >> 8 & 255) / 255.0f;
        es.ls.amb[2] = (float)(w[3] & 255) / 255.0f;
        es.light_mask = (w[2] | w[2] >> 16) & 0xffff;
        for (int i = 0; i < 16; i++)
            es.ls.l[i].on = 0;
        es.ls_new = 1;
        return;
    }
    light *l = &es.ls.l[w[1] & 15];
    if ((w[1] & 15) == 15)
        return;                         /* the block the matrix writer sends before each model */
    l->on = 1;
    l->col[0] = (float)(w[1] >> 24) / 127.0f;
    l->col[1] = (float)(w[1] >> 16 & 255) / 127.0f;
    l->col[2] = (float)(w[1] >> 8 & 255) / 127.0f;
    l->dir[0] = (float)(s8)(w[2] >> 16) / 127.0f;
    l->dir[1] = (float)(s8)(w[2] >> 8) / 127.0f;
    l->dir[2] = (float)(s8)w[2] / 127.0f;
    l->point = w[2] >> 30 & 1;
    l->linear = w[2] >> 31;
    l->pos[0] = f32(w[3]);
    l->pos[1] = f32(w[4]);
    l->pos[2] = f32(w[5]);
    l->ka = f32(w[6] & 0xffff0000u);
    l->kb = f32(w[6] << 16);
    es.ls_new = 1;
}

static void run_list(u32 addr, u32 len, int depth)
{
    u32 end = addr + len;
    while (addr + 8 <= end) {
        u32 w[16];
        u32 op = g_r32(addr);
        if (op == 0x08000000u) {
            addr += 8;
            continue;
        }
        int cmd = op >> 8 & 0xf, para = op >> 29;
        u32 left = end - addr, size = cmd == 1 || cmd == 2 || cmd == 5 ? 64 : cmd == 0xe || cmd == 0xf ? 16 : 32;
        if (cmd == 0 && para == 7)
            size = es.kind ? (u32)es.vbytes : 32;
        if (cmd == 0 && para == 4) {
            int txd = op >> 3 & 1, col = op >> 4 & 3, ofs = op >> 2 & 1, two = op >> 6 & 1, lst = op >> 24 & 7;
            if (lst != 1 && lst != 3 && col == 2 && ((txd && ofs) || two))
                size = 64;
        }
        if (size > left)
            break;
        memset(w, 0, sizeof w);
        g_read(addr, w, size);
        switch (cmd) {
        case 0:
            if (para == 0) {
                es.list = -1;
                es.kind = 0;
            } else if (para == 1) {
                /* the user clip rectangle, in tiles of 32 pixels */
                es.clip[0] = (s16)(w[4] * 32);
                es.clip[1] = (s16)(w[5] * 32);
                es.clip[2] = (s16)(w[6] * 32 + 31);
                es.clip[3] = (s16)(w[7] * 32 + 31);
            } else if (para == 4 || para == 5) {
                chip_header(w, para);
                if (size == 64)
                    for (int i = 0; i < 4; i++) {
                        es.face[i] = f32(w[8 + i]);
                        es.face_ofs[i] = f32(w[12 + i]);
                    }
            } else if (para == 7)
                chip_vertex(w);
            break;
        case 1:
            es.xf.near_z = f32(w[1]);
            for (int i = 0; i < 12; i++)
                es.xf.m[i] = f32(w[2 + i]);
            es.xf_new = 1;
            break;
        case 2:
            es.xf.env_u = f32(w[1]);
            for (int i = 0; i < 9; i++)
                es.xf.n[i] = f32(w[2 + i]);
            es.xf.env_v = f32(w[11]);
            es.xf_new = 1;
            break;
        case 3:
            es.xf.sx = f32(w[2]);
            es.xf.cx = f32(w[3]);
            es.xf.sy = f32(w[4]);
            es.xf.cy = f32(w[5]);
            es.xf_new = 1;
            break;
        case 4:
            light_cmd(w);
            break;
        case 5:
            es.mat_spec = w[1];
            es.mat_flags = w[2];
            es.base = w[3];
            es.offset = w[4];
            break;
        case 7:
            addr = model_strip(addr, end);
            continue;
        case 8:
            if (depth < 4 && (w[4] & 0x80000000u)) {
                u32 keep = es.link_cull, or0 = es.link_or0, or1 = es.link_or1, or2 = es.link_or2;
                es.link_cull = w[1] >> 27 & 3;
                es.link_or0 = w[0];
                es.link_or1 = w[1];
                es.link_or2 = w[2];
                run_list(ELAN_RAM | (w[4] & 0x01fffff8u), w[6], depth + 1);
                es.link_cull = keep;
                es.link_or0 = or0;
                es.link_or1 = or1;
                es.link_or2 = or2;
            }
            break;
        case 0xe:
        case 0xf:
            break;
        default:
            if (!verbose_once[cmd] && cfg.verbose) {
                verbose_once[cmd] = 1;
                fprintf(stderr, "view: a command the view does not know, %08x at %08x\n", op, addr);
            }
            return;
        }
        addr += size;
    }
}

/* ---- pictures out ---- */

static u32 crc_tab[256];

static void png_tables(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ c >> 1 : c >> 1;
        crc_tab[i] = c;
    }
}

static u32 crc32_of(u32 crc, const u8 *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_tab[(crc ^ *p++) & 255] ^ crc >> 8;
    return ~crc;
}

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
    u8 head[8] = { (u8)(n >> 24), (u8)(n >> 16), (u8)(n >> 8), (u8)n, (u8)type[0], (u8)type[1], (u8)type[2], (u8)type[3] };
    fwrite(head, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    u32 crc = crc32_of(crc32_of(0, head + 4, 4), data, n);
    u8 tail[4] = { (u8)(crc >> 24), (u8)(crc >> 16), (u8)(crc >> 8), (u8)crc };
    fwrite(tail, 1, 4, f);
}

/* A PNG with no library. Each row takes the filter (none, left, up,
 * average, Paeth) that leaves the smallest bytes; the result is packed as
 * deflate: repeats found through a table of three-byte starts, and for
 * every 32,768 symbols a block with its own Huffman codes. */
typedef struct bitw {
    u8 *out;
    size_t n;
    u32 acc;
    int bits;
} bitw;

static inline void put_bits(bitw *b, u32 v, int n)
{
    b->acc |= v << b->bits;
    b->bits += n;
    while (b->bits >= 8) {
        b->out[b->n++] = (u8)b->acc;
        b->acc >>= 8;
        b->bits -= 8;
    }
}

/* code lengths for n symbols with these counts, none longer than limit */
static void huff_lengths(const u32 *freq, int n, int limit, u8 *len)
{
    int sym[288], par[576], used = 0;
    u32 wt[576];
    memset(len, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        if (freq[i])
            sym[used++] = i;
    if (used == 0)
        return;
    if (used == 1) {
        len[sym[0]] = 1;
        return;
    }
    /* the tree: the two lightest nodes joined, until one is left */
    int nodes = used, alive[288], alive_n = used;
    for (int i = 0; i < used; i++) {
        wt[i] = freq[sym[i]];
        par[i] = -1;
        alive[i] = i;
    }
    while (alive_n > 1) {
        int a = 0, b = 1;
        if (wt[alive[b]] < wt[alive[a]]) {
            a = 1;
            b = 0;
        }
        for (int i = 2; i < alive_n; i++) {
            if (wt[alive[i]] < wt[alive[a]]) {
                b = a;
                a = i;
            } else if (wt[alive[i]] < wt[alive[b]])
                b = i;
        }
        wt[nodes] = wt[alive[a]] + wt[alive[b]];
        par[nodes] = -1;
        par[alive[a]] = par[alive[b]] = nodes;
        int lo = a < b ? a : b, hi = a < b ? b : a;
        alive[lo] = nodes++;
        alive[hi] = alive[--alive_n];
    }
    int count[64], order[288];
    memset(count, 0, sizeof count);
    for (int i = 0; i < used; i++) {
        int d = 0;
        for (int p = par[i]; p >= 0; p = par[p])
            d++;
        count[d > 63 ? 63 : d]++;
    }
    /* too long: codes of the longest lengths are brought up to the limit, and one shorter
     * code is lengthened for every two that need room */
    for (int d = limit + 1; d < 64; d++) {
        count[limit] += count[d];
        count[d] = 0;
    }
    u32 total = 0;
    for (int d = 1; d <= limit; d++)
        total += (u32)count[d] << (limit - d);
    while (total > 1u << limit) {
        count[limit]--;
        for (int d = limit - 1; d >= 1; d--)
            if (count[d]) {
                count[d]--;
                count[d + 1] += 2;
                break;
            }
        total--;
    }
    /* the commonest symbols take the shortest codes */
    for (int i = 0; i < used; i++)
        order[i] = sym[i];
    for (int i = 1; i < used; i++) {
        int s = order[i], k = i;
        while (k > 0 && freq[order[k - 1]] < freq[s]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = s;
    }
    int at = 0;
    for (int d = 1; d <= limit; d++)
        for (int c = count[d]; c > 0 && at < used; c--)
            len[order[at++]] = (u8)d;
}

/* the codes of those lengths, turned round: deflate sends them high bit first */
static void huff_codes(const u8 *len, int n, u16 *code)
{
    int count[16], next[16];
    memset(count, 0, sizeof count);
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    int c = 0;
    next[0] = 0;
    for (int d = 1; d < 16; d++) {
        c = (c + count[d - 1]) << 1;
        next[d] = c;
    }
    for (int i = 0; i < n; i++) {
        int v = len[i] ? next[len[i]]++ : 0, r = 0;
        for (int k = 0; k < len[i]; k++)
            r |= (v >> k & 1) << (len[i] - 1 - k);
        code[i] = (u16)r;
    }
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                  99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                   12, 13, 13 };
static u8 len_sym[259], dist_sym_lo[257];

static inline int dist_sym(u32 d)
{
    return d <= 256 ? dist_sym_lo[d] : 16 + dist_sym_lo[((d - 1) >> 7) + 1] - 2;
}

/* one block: n symbols (a literal when its distance is 0, else length and distance) */
static void deflate_block(bitw *b, const u16 *sl, const u16 *sd, u32 n, int last)
{
    u32 lf[288], df[30], cf[19];
    u8 ll[288], dl[30], cl[19], all[288 + 30], rle[288 + 30], rle_x[288 + 30];
    u16 lc[288], dc[30], cc[19];
    memset(lf, 0, sizeof lf);
    memset(df, 0, sizeof df);
    for (u32 i = 0; i < n; i++)
        if (sd[i]) {
            lf[257 + len_sym[sl[i]]]++;
            df[dist_sym(sd[i])]++;
        } else
            lf[sl[i]]++;
    lf[256] = 1;
    huff_lengths(lf, 288, 15, ll);
    huff_lengths(df, 30, 15, dl);
    int nl = 286, nd = 30;
    while (nl > 257 && !ll[nl - 1])
        nl--;
    while (nd > 1 && !dl[nd - 1])
        nd--;
    memcpy(all, ll, (size_t)nl);
    memcpy(all + nl, dl, (size_t)nd);
    /* the lengths themselves, with runs: 16 the one before again 3-6 times, 17 and 18 runs of zeros */
    int rn = 0;
    for (int i = 0; i < nl + nd;) {
        int v = all[i], rep = 1;
        while (i + rep < nl + nd && all[i + rep] == v)
            rep++;
        if (v == 0 && rep >= 3) {
            int take = rep > 138 ? 138 : rep;
            rle[rn] = take >= 11 ? 18 : 17;
            rle_x[rn++] = (u8)(take >= 11 ? take - 11 : take - 3);
            i += take;
        } else if (v && rep >= 4) {
            int take = rep - 1 > 6 ? 6 : rep - 1;
            rle[rn] = (u8)v;
            rle_x[rn++] = 0;
            rle[rn] = 16;
            rle_x[rn++] = (u8)(take - 3);
            i += 1 + take;
        } else {
            rle[rn] = (u8)v;
            rle_x[rn++] = 0;
            i++;
        }
    }
    memset(cf, 0, sizeof cf);
    for (int i = 0; i < rn; i++)
        cf[rle[i]]++;
    huff_lengths(cf, 19, 7, cl);
    huff_codes(ll, 288, lc);
    huff_codes(dl, 30, dc);
    huff_codes(cl, 19, cc);
    static const u8 cl_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nc = 19;
    while (nc > 4 && !cl[cl_order[nc - 1]])
        nc--;
    put_bits(b, (u32)last, 1);
    put_bits(b, 2, 2);                  /* codes of its own */
    put_bits(b, (u32)(nl - 257), 5);
    put_bits(b, (u32)(nd - 1), 5);
    put_bits(b, (u32)(nc - 4), 4);
    for (int i = 0; i < nc; i++)
        put_bits(b, cl[cl_order[i]], 3);
    for (int i = 0; i < rn; i++) {
        put_bits(b, cc[rle[i]], cl[rle[i]]);
        if (rle[i] == 16)
            put_bits(b, rle_x[i], 2);
        else if (rle[i] == 17)
            put_bits(b, rle_x[i], 3);
        else if (rle[i] == 18)
            put_bits(b, rle_x[i], 7);
    }
    for (u32 i = 0; i < n; i++) {
        if (!sd[i]) {
            put_bits(b, lc[sl[i]], ll[sl[i]]);
            continue;
        }
        int ls = len_sym[sl[i]], ds = dist_sym(sd[i]);
        put_bits(b, lc[257 + ls], ll[257 + ls]);
        if (len_extra[ls])
            put_bits(b, (u32)(sl[i] - len_base[ls]), len_extra[ls]);
        put_bits(b, dc[ds], dl[ds]);
        if (dist_extra[ds]) {
            u32 x = (u32)sd[i] - dist_base[ds];
            if (dist_extra[ds] > 8) {
                put_bits(b, x & 255, 8);
                put_bits(b, x >> 8, dist_extra[ds] - 8);
            } else
                put_bits(b, x, dist_extra[ds]);
        }
    }
    put_bits(b, lc[256], ll[256]);
}

#define PNG_BLOCK 32768
#define PNG_HASH 15

static size_t deflate_all(const u8 *raw, size_t n, u8 *out)
{
    static s32 head[1 << PNG_HASH], prev[32768];
    static u16 sl[PNG_BLOCK], sd[PNG_BLOCK];
    if (!len_sym[258]) {
        for (int i = 0; i < 29; i++)
            for (int l = len_base[i]; l <= 258 && (i == 28 || l < len_base[i + 1]); l++)
                len_sym[l] = (u8)i;
        len_sym[258] = 28;
        for (int i = 0; i < 16; i++)
            for (int d = dist_base[i]; d <= 256 && d < dist_base[i + 1]; d++)
                dist_sym_lo[d] = (u8)i;
    }
    bitw b = { out, 0, 0, 0 };
    out[b.n++] = 0x78;
    out[b.n++] = 0x9c;
    memset(head, 0xff, sizeof head);
    u32 sn = 0;
#define HASH3(p) ((((u32)(p)[0] << 10) ^ ((u32)(p)[1] << 5) ^ (p)[2]) * 2654435761u >> (32 - PNG_HASH))
    for (size_t i = 0; i < n;) {
        int best = 0;
        u32 best_d = 0;
        if (i + 3 <= n) {
            u32 h = HASH3(raw + i);
            size_t max = n - i < 258 ? n - i : 258;
            int chain = 24;
            for (s32 p = head[h]; p >= 0 && i - (size_t)p <= 32768 && chain--; p = prev[p & 32767]) {
                const u8 *a = raw + i, *c = raw + p;
                if (c[best] == a[best] && c[0] == a[0]) {
                    size_t l = 1;
                    while (l < max && c[l] == a[l])
                        l++;
                    if ((int)l > best) {
                        best = (int)l;
                        best_d = (u32)(i - (size_t)p);
                        if (l == max)
                            break;
                    }
                }
                if (prev[p & 32767] >= p)
                    break;
            }
            prev[i & 32767] = head[h];
            head[h] = (s32)i;
        }
        if (best >= 3 && !(best == 3 && best_d > 4096)) {
            sl[sn] = (u16)best;
            sd[sn++] = (u16)best_d;
            /* the bytes of the repeat can start repeats too */
            for (size_t k = i + 1; k < i + (size_t)best && k + 3 <= n; k++) {
                u32 h = HASH3(raw + k);
                prev[k & 32767] = head[h];
                head[h] = (s32)k;
            }
            i += (size_t)best;
        } else {
            sl[sn] = raw[i++];
            sd[sn++] = 0;
        }
        if (sn == PNG_BLOCK || i >= n) {
            deflate_block(&b, sl, sd, sn, i >= n);
            sn = 0;
        }
    }
#undef HASH3
    if (n == 0)
        deflate_block(&b, sl, sd, 0, 1);
    if (b.bits)
        put_bits(&b, 0, 8 - b.bits);
    u32 s1 = 1, s2 = 0;
    for (size_t i = 0; i < n;) {
        size_t stop = i + 5000 < n ? i + 5000 : n;
        for (; i < stop; i++) {
            s1 += raw[i];
            s2 += s1;
        }
        s1 %= 65521;
        s2 %= 65521;
    }
    out[b.n++] = (u8)(s2 >> 8);
    out[b.n++] = (u8)s2;
    out[b.n++] = (u8)(s1 >> 8);
    out[b.n++] = (u8)s1;
    return b.n;
}

static int png_write(const char *path, const u8 *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    size_t stride = 3 * (size_t)w, raw_n = (size_t)h * (1 + stride);
    u8 *raw = malloc(raw_n), *out = malloc(raw_n + raw_n / 2 + 1024), *zero = calloc(1, stride), *try_ = malloc(stride);
    if (!raw || !out || !zero || !try_) {
        free(raw);
        free(out);
        free(zero);
        free(try_);
        fclose(f);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        const u8 *cur_ = rgb + (size_t)y * stride, *up = y ? cur_ - stride : zero;
        u8 *dst = raw + (size_t)y * (1 + stride);
        u32 best = 0xffffffffu;
        for (int ft = 0; ft < 5; ft++) {
            u32 sum = 0;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= 3 ? cur_[i - 3] : 0, bb = up[i], c = i >= 3 ? up[i - 3] : 0, pr;
                switch (ft) {
                case 0: pr = 0; break;
                case 1: pr = a; break;
                case 2: pr = bb; break;
                case 3: pr = (a + bb) >> 1; break;
                default: {
                    int p = a + bb - c, pa = abs(p - a), pb = abs(p - bb), pc = abs(p - c);
                    pr = pa <= pb && pa <= pc ? a : pb <= pc ? bb : c;
                }
                }
                u8 v = (u8)(cur_[i] - pr);
                try_[i] = v;
                sum += v < 128 ? v : 256u - v;
            }
            if (sum < best) {
                best = sum;
                dst[0] = (u8)ft;
                memcpy(dst + 1, try_, stride);
            }
        }
    }
    size_t zn = deflate_all(raw, raw_n, out);
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    u8 ihdr[13] = { (u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h,
                    8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", out, (u32)zn);
    png_chunk(f, "IEND", NULL, 0);
    free(raw);
    free(out);
    free(zero);
    free(try_);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

static void pipe_open(void)
{
    if (!pipe_cmd || pipe_f)
        return;
    signal(SIGPIPE, SIG_IGN);
    fflush(stdout);
    fflush(stderr);
    pipe_f = popen(pipe_cmd, "w");
    if (!pipe_f) {
        perror(pipe_cmd);
        pipe_cmd = NULL;
    }
}

/* ---- a scene, from its lists to its picture ---- */

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static scene *scene_open(u32 key)
{
    static u32 births;
    scene *s = NULL;
    for (int i = 0; i < SCENES && !s; i++)
        if (scenes[i].used && scenes[i].key == key)
            s = &scenes[i];
    for (int i = 0; i < SCENES && !s; i++)
        if (!scenes[i].used)
            s = &scenes[i];
    if (!s) {
        /* lists that were never rendered: the oldest goes */
        s = &scenes[0];
        for (int i = 1; i < SCENES; i++)
            if (scenes[i].born < s->born)
                s = &scenes[i];
    }
    for (int k = 0; k < 3; k++)
        s->tris_n[k] = s->runs_n[k] = 0, s->run_open[k] = 0;
    s->st_n = s->xf_n = s->ls_n = s->jobs_n = s->vcopy_n = s->order_n = 0;
    s->vt_n[0] = s->vt_n[1] = s->vols_n[0] = s->vols_n[1] = 0;
    s->used = 1;
    s->key = key;
    s->born = ++births;
    return s;
}

/* The tile accelerator is told to take lists: from the start (a frame for
 * the screen) or going on into other object lists (a pass into a
 * texture, ta_continue_into_target). What arrives from here on is one
 * scene, known by the object list base. */
static void lists_begin(int pass)
{
    u32 key = view_core_reg(0, 0x124) & 0x00fffffcu;
    int active = (png_dir || pipe_cmd) && machine_frame >= view_from, want, regular = 0;
    if (!pass) {
        regular = active && screens % (u32)every == 0;
        want = pic_request || regular;
        screens++;
    } else {
        /* its texture is used by the screens that follow: wanted when one of the next few is */
        want = pic_request;
        for (u32 j = 0; active && j < 4; j++)
            if ((screens + j) % (u32)every == 0)
                want = 1;
        if (want && pic_request)
            pic_targets_seen++;
    }
    /* a scene that is not wanted is noted all the same: its render must find it, and nothing else */
    cur = scene_open(key);
    cur->capture = want;
    cur->regular = regular;
    if (!want) {
        cur = NULL;
        return;
    }
    memset(&es, 0, sizeof es);
    es.list = -1;
    es.base = 0xffffffffu;
    es.xf.sx = es.xf.sy = -VH / 2.0f;
    es.xf.cx = VW / 2.0f;
    es.xf.cy = VH / 2.0f;
    es.xf_new = es.ls_new = 1;
    es.ls.amb[0] = es.ls.amb[1] = es.ls.amb[2] = 1;
    es.light_mask = 0xffff;
    es.clip[2] = FBW - 1;
    es.clip[3] = FBH - 1;
}

/* The scene a render is of. The registers name a region array (+0x2c), and the
 * library lays each one out right after the object lists it points into (the screen's
 * banks; tex_target_bank_lay_out for a texture's): the scene whose object list base is
 * the nearest below it. */
static scene *scene_of_render(void)
{
    u32 ra = view_core_reg(0, 0x2c) & 0x00fffffcu;
    scene *s = NULL;
    for (int i = 0; i < SCENES; i++)
        if (scenes[i].used && scenes[i].key <= ra && ra - scenes[i].key < 0x40000 && (!s || scenes[i].key > s->key))
            s = &scenes[i];
    return s;
}

/* The background plane: three corners with a colour each, where the
 * register at +0x8c says (bg_plane_defaults_set, tex_target_bg_plane_write);
 * and everything else a new picture starts from. */
static void target_clear(void)
{
    u32 tag = view_core_reg(0, 0x8c), base = view_core_reg(0, 0x20) & 0x00f00000u;
    u32 at = VRAM1_32 + ((base + ((tag >> 3 & 0x1fffffu) << 2)) & 0x00fffffcu);
    u32 skip = tag >> 24 & 7, w[3 + 3 * 10];
    float x[3] = { 0, 0, 0 }, y[3] = { 0, 0, 0 }, c[3][3];
    memset(w, 0, sizeof w);
    memset(c, 0, sizeof c);
    int plane_ok = 0;
    float plane_z = 0;
    if (!g_read(at, w, (3 + 3 * (3 + skip)) * 4) && !(w[0] >> 25 & 1) && skip >= 1) {
        plane_ok = 1;
        plane_z = f32(w[3 + 2]);
        for (int i = 0; i < 3; i++) {
            const u32 *v = &w[3 + i * (3 + skip)];
            x[i] = f32(v[0]);
            y[i] = f32(v[1]);
            c[i][0] = (float)(v[3] >> 16 & 255);
            c[i][1] = (float)(v[3] >> 8 & 255);
            c[i][2] = (float)(v[3] & 255);
        }
    }
    /* the colour over the plane: c0 + a (x - x0) + b (y - y0) */
    double d1x = (double)x[1] - x[0], d1y = (double)y[1] - y[0], d2x = (double)x[2] - x[0], d2y = (double)y[2] - y[0];
    double det = d1x * d2y - d2x * d1y;
    float ga[3] = { 0, 0, 0 }, gb[3] = { 0, 0, 0 };
    int flat = det == 0 || (!memcmp(c[0], c[1], sizeof c[0]) && !memcmp(c[0], c[2], sizeof c[0]));
    if (!flat)
        for (int k = 0; k < 3; k++) {
            double e1 = (double)c[1][k] - c[0][k], e2 = (double)c[2][k] - c[0][k];
            ga[k] = (float)((e1 * d2y - e2 * d1y) / det);
            gb[k] = (float)((e2 * d1x - e1 * d2x) / det);
        }
    /* the plane takes the table's fog like any polygon whose third word asks for it: it
     * lies at the far end, so with the fog the game fades a scene out by, it fades too */
    int fog = tg.fog_on && plane_ok && (w[1] >> 22 & 3) == 0 ? (int)fog_at(plane_z) : 0;
    u32 fc[3] = { tg.fog_col >> 16 & 255, tg.fog_col >> 8 & 255, tg.fog_col & 255 };
    fog += fog >> 7;
    u32 one = 0;
    for (int k = 0; k < 3; k++)
        one = one << 8 | (u32)((int)c[0][k] + (((int)fc[k] - (int)c[0][k]) * fog >> 8));
    for (int yy = 0; yy < tg.h; yy++) {
        u32 *frow = fb + yy * FBW;
        if (flat) {
            for (int xx = 0; xx < tg.w; xx++)
                frow[xx] = one;
        } else
            for (int xx = 0; xx < tg.w; xx++) {
                u32 px = 0;
                for (int k = 0; k < 3; k++) {
                    float v = c[0][k] + ga[k] * ((float)xx + 0.5f - x[0]) + gb[k] * ((float)yy + 0.5f - y[0]);
                    int iv = v < 0 ? 0 : v > 255 ? 255 : (int)v;
                    px = px << 8 | (u32)(iv + (((int)fc[k] - iv) * fog >> 8));
                }
                frow[xx] = px;
            }
        memset(zb + yy * FBW, 0, (size_t)tg.w * sizeof(float));
        memset(pfb + yy * FBW, 0, (size_t)tg.w);
        memset(stn + yy * FBW, 0, (size_t)tg.w);
    }
}

static const dtri *sort_base;

static int by_depth(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa, b = *(const u32 *)pb;
    float ka = sort_base[a].key, kb = sort_base[b].key;
    if (ka != kb)
        return ka < kb ? -1 : 1;
    return a < b ? -1 : a > b;
}

/* The picture is drawn in horizontal bands, each by one thread: a band has
 * its own rows and takes the triangles in their order, so the picture
 * does not depend on the threads. The game does not run meanwhile. */
#define BANDS 16

static void band_item(int b, void *arg)
{
    const scene *sc = arg;
    int lo = b * tg.h / BANDS, hi = (b + 1) * tg.h / BANDS - 1;
    if (hi < lo)
        return;
    if (!th_tog) {
        th_tog = malloc((size_t)FBW * (FBH / BANDS + 1));
        th_head = malloc((size_t)FBW * (FBH / BANDS + 1) * sizeof(s32));
        if (!th_tog || !th_head) {
            fprintf(stderr, "view: no memory\n");
            exit(1);
        }
    }
    th_band_lo = lo;
    th_tvol = 0;
    for (int k = 0; k < 2; k++)
        for (u32 r = 0; r < sc->runs_n[k]; r++) {
            const dtri *d = sc->tris[k] + sc->runs[k][r].first;
            for (u32 i = sc->runs[k][r].n; i; i--, d++)
                if (d->ylo <= hi && d->yhi >= lo)
                    raster(d, &sc->st[d->st], k, lo, hi);
        }
    /* the shadows: where a pixel lies in a volume and its polygon takes shadows, the
     * colour times the scale the game wrote (pvr_shadow_scale_write), in 256ths */
    if (sc->vols_n[0] && tg.shadow_scale < 256) {
        int any = 0;
        for (u32 n = 0; n < sc->vols_n[0]; n++) {
            const vol *v = &sc->vols[0][n];
            if (v->y1 >= lo && v->y0 <= hi) {
                volume_band(sc, v, lo, hi);
                any = 1;
            }
        }
        for (int y = lo; any && y <= hi; y++) {
            u32 *frow = fb + y * FBW;
            const u8 *srow = stn + y * FBW, *prow = pfb + y * FBW;
            for (int x = 0; x < tg.w; x++)
                if (srow[x] & prow[x] & 1) {
                    u32 c = frow[x], s = (u32)tg.shadow_scale;
                    frow[x] = ((c >> 16 & 255) * s >> 8) << 16 | ((c >> 8 & 255) * s >> 8) << 8 | ((c & 255) * s >> 8);
                }
        }
    }
    if (tg.shadow_scale < 256)
        tvolumes_band(sc, lo, hi);
    for (u32 i = 0; i < sc->order_n; i++) {
        const dtri *d = sc->tris[2] + sc->order[i];
        if (d->ylo <= hi && d->yhi >= lo)
            raster(d, &sc->st[d->st], 2, lo, hi);
    }
}

/* A check that can be seen: the game keeps each fighter's place on the
 * screen in its own record (rob_rec.screen_xy, robs + side * 0x86c + 0xcc,
 * computed by the game's code from its camera). A cross is put there; it
 * has to sit on the fighter the rasterizer drew from the display lists. */
static void marks_draw(void)
{
    static u32 robs;
    if (!robs)
        robs = global_addr("robs");
    if (!robs || game_mode_main() != 3)
        return;
    for (u32 side = 0; side < 2; side++) {
        u32 at = robs + side * 0x86c + 0xcc, wx = g_r32(at), wy = g_r32(at + 4);
        float fx, fy;
        memcpy(&fx, &wx, 4);
        memcpy(&fy, &wy, 4);
        if (!(fx > -1000 && fx < 2000 && fy > -1000 && fy < 2000))
            continue;
        int cx = (int)fx, cy = (int)fy;
        u32 col = side ? 0x0040c0ffu : 0x00ff4040u;
        for (int d = -9; d <= 9; d++)
            for (int t = -1; t <= 1; t++) {
                int px[2] = { cx + d, cx + t }, py[2] = { cy + t, cy + d };
                for (int k = 0; k < 2; k++)
                    if (px[k] >= 0 && px[k] < VW && py[k] >= 0 && py[k] < VH)
                        fb[py[k] * FBW + px[k]] = t == 0 && d > -9 && d < 9 ? col : 0;
            }
    }
}

/* The pass was into a texture: its pixels go where the frame buffer
 * registers say, in the format they say (texture_fb_write_regs_build: the
 * write start in texture memory, 8-byte units a row, the pack mode), so
 * that the polygons that use the texture find them. */
static void target_store(void)
{
    static const u8 bytes_of[8] = { 2, 2, 2, 2, 3, 4, 4, 0 };
    u32 bpp = bytes_of[tg.pack & 7];
    if (!bpp || !tg.stride || (u64)tg.addr + (u64)(tg.h - 1) * tg.stride + (u64)tg.w * bpp > VRAM_SIZE)
        return;
    u8 *base = mem_ptr(VRAM1 + tg.addr);
    if (!base)
        return;
    for (int y = 0; y < tg.h; y++) {
        const u32 *frow = fb + y * FBW;
        u8 *out = base + (size_t)y * tg.stride;
        for (int x = 0; x < tg.w; x++) {
            u32 c = frow[x], r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
            switch (tg.pack & 7) {
            case 0: *(u16 *)(out + 2 * x) = (u16)(r >> 3 << 10 | g >> 3 << 5 | b >> 3); break;
            case 1: *(u16 *)(out + 2 * x) = (u16)(r >> 3 << 11 | g >> 2 << 5 | b >> 3); break;
            case 2: *(u16 *)(out + 2 * x) = (u16)(0xf000u | r >> 4 << 8 | g >> 4 << 4 | b >> 4); break;
            case 3: *(u16 *)(out + 2 * x) = (u16)(0x8000u | r >> 3 << 10 | g >> 3 << 5 | b >> 3); break;
            case 4: out[3 * x] = (u8)b; out[3 * x + 1] = (u8)g; out[3 * x + 2] = (u8)r; break;
            case 5: *(u32 *)(out + 4 * x) = c & 0x00ffffffu; break;
            default: *(u32 *)(out + 4 * x) = c | 0xff000000u; break;
            }
        }
    }
}

static void tables_once(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    bytes_lo = global_addr("vertex_bytes_lo");
    bytes_hi = global_addr("vertex_bytes_hi");
    /* the specular byte of a material command is material_param_byte_table[type]
     * (model_list_compile); the type is the row of the library's own specular table */
    u32 tab = global_addr("material_param_byte_table");
    for (u32 b = 0; b < 256; b++) {
        spec_power[b] = (u8)(b / 8 ? b / 8 : 1);
        for (u32 j = 1; tab && j < 96; j++)
            if ((g_r32(tab + 4 * j) & 255) == b) {
                spec_power[b] = (u8)j;
                break;
            }
    }
}

static void scene_draw(scene *sc, int to_texture)
{
    u32 xc = view_core_reg(0, 0x68), yc = view_core_reg(0, 0x6c), sof = view_core_reg(0, 0x60);
    tg.w = (int)(xc >> 16 & 0x7ff) + 1;
    tg.h = (int)(yc >> 16 & 0x3ff) + 1;
    if (tg.w > FBW) tg.w = FBW;
    if (tg.h > FBH) tg.h = FBH;
    if (!to_texture && (tg.w > VW || tg.h > VH)) {
        tg.w = VW;
        tg.h = VH;
    }
    tg.to_texture = to_texture;
    tg.addr = sof & 0x00fffffcu;
    tg.stride = (view_core_reg(0, 0x4c) & 0x1ff) * 8;
    tg.pack = view_core_reg(0, 0x48) & 7;
    tg.alpha_ref = view_core_reg(0, 0x11c) & 0xff;
    /* fog: the density register is a mantissa (0x80 = 1) and a power of two
     * (fog_density_pack_float); each table register holds a step and the next */
    u32 dens = view_core_reg(0, 0xb8);
    tg.fog_density = (float)(dens >> 8 & 255) / 128.0f * powf(2.0f, (float)(s8)(dens & 255));
    tg.fog_on = 0;
    for (u32 i = 0; i < 128; i++) {
        u32 r = view_core_reg(0, 0x200 + 4 * i);
        tg.fog_tab[i] = r >> 8 & 255;
        tg.fog_tab[i + 1] = r & 255;
        tg.fog_on |= (r & 0xffff) != 0;
    }
    tg.fog_col = view_core_reg(0, 0xb0) & 0x00ffffffu;
    tg.fog_col_vert = view_core_reg(0, 0xb4) & 0x00ffffffu;
    /* shadows: bit 8 says a shadowed pixel is its colour times the low byte in 256ths
     * (pvr_shadow_scale_write); without it a polygon would have a second set of
     * parameters for its shadowed part, which this game never sends */
    u32 shad = view_core_reg(0, 0x74);
    tg.shadow_scale = shad & 0x100 ? (int)(shad & 0xff) : 256;

    tables_once();
    target_clear();
    /* the textures as video memory has them now: a pass into a texture that the game
     * rendered after these lists were sent is in them, as it would be for the chip */
    for (u32 i = 0; i < sc->st_n; i++)
        sc->st[i].t = sc->st[i].textured ? tex_get(sc->st[i].tsp, sc->st[i].tcw) : NULL;
    double t0 = now_seconds();
    pool_run(strip_job_item, (int)sc->jobs_n, sc);
    /* the translucent list from the back to the front */
    sc->order_n = 0;
    for (u32 r = 0; r < sc->runs_n[2]; r++) {
        GROW(sc->order, sc->order_cap, sc->order_n + sc->runs[2][r].n);
        for (u32 i = 0; i < sc->runs[2][r].n; i++)
            sc->order[sc->order_n++] = sc->runs[2][r].first + i;
    }
    if (sc->order_n > 1) {
        sort_base = sc->tris[2];
        qsort(sc->order, sc->order_n, sizeof(u32), by_depth);
    }
    for (int k = 0; k < 3; k++)
        for (u32 r = 0; r < sc->runs_n[k]; r++)
            tri_count += sc->runs[k][r].n;
    double t1 = now_seconds();
    pool_run(band_item, BANDS, sc);
    raster_seconds += now_seconds() - t1;
    tl_seconds += t1 - t0;
    if (to_texture)
        target_store();
}

/* "start render": the scene whose object lists the registers name is drawn */
static void render_started(void)
{
    double t0 = now_seconds();
    int to_texture = view_core_reg(0, 0x60) >> 24 & 1;
    scene *s = scene_of_render();
    int drawn = 0;
    if (s == cur)
        cur = NULL;
    if (s && s->capture) {
        scene_draw(s, to_texture);
        drawn = 1;
    }
    if (s)
        s->used = 0;
    gen++;
    if (to_texture) {
        if (drawn) {
            target_pictures++;
            if (pic_request)
                pic_targets_drawn++;
        }
    } else {
        if (drawn) {
            if (marks)
                marks_draw();
            for (int y = 0; y < VH; y++) {
                const u32 *frow = fb + y * FBW;
                u8 *out = shown + (size_t)y * VW * 3;
                for (int x = 0; x < VW; x++) {
                    out[3 * x] = (u8)(frow[x] >> 16);
                    out[3 * x + 1] = (u8)(frow[x] >> 8);
                    out[3 * x + 2] = (u8)frow[x];
                }
            }
            shown_valid = 1;
            pictures++;
        }
        draw_seconds += now_seconds() - t0;
        if (drawn && pic_request && pic_path[0]) {
            /* a frame that uses a texture the game renders: the first one whose texture the view has drawn */
            if (pic_targets_seen && !pic_targets_drawn && pic_waited < 3)
                pic_waited++;
            else {
                if (!png_write(pic_path, shown, VW, VH))
                    png_written++;
                pic_request = 0;
            }
        }
        if (drawn && png_dir && s->regular) {
            char path[1200];
            snprintf(path, sizeof path, "%s/f%06u.png", png_dir, machine_frame);
            if (!png_write(path, shown, VW, VH))
                png_written++;
        }
        screen_renders++;
        t0 = now_seconds();
    }
    draw_seconds += now_seconds() - t0;
    if (tex_used >= TEX_SLOTS / 2)
        tex_flush();                    /* nothing holds a texture between two renders */
}

void view_list_segment(sh4 *c, u32 addr, u32 len)
{
    (void)c;
    if (!cur)
        return;
    double t0 = now_seconds();
    run_list(addr, len, 0);
    draw_seconds += now_seconds() - t0;
}

void view_reg_write(sh4 *c, int chip, u32 off, u32 v)
{
    (void)c;
    if (chip != 0)
        return;
    if (off == 0x014)
        render_started();
    else if ((off == 0x144 || off == 0x160) && v >> 31)
        lists_begin(off == 0x160);
}

/* once per vertical blank: the newest picture goes to the viewer, and the
 * run keeps the game's pace when asked to */
void view_frame(sh4 *c)
{
    (void)c;
    if (machine_frame < view_from)
        return;
    if (pipe_cmd && shown_valid && machine_frame % (u32)every == 0) {
        pipe_open();
        if (pipe_f && (fwrite(shown, 1, VW * VH * 3, pipe_f) != VW * VH * 3 || fflush(pipe_f))) {
            /* the viewer was closed: the run ends at this vertical blank */
            pclose(pipe_f);
            pipe_f = NULL;
            pipe_cmd = NULL;
            machine_interrupted = 1;
            fprintf(stderr, "view: the viewer has gone; stopping\n");
        } else
            pipe_frames++;
    }
    if (realtime) {
        static double due;
        double t = now_seconds();
        if (due == 0 || t - due > 0.25)
            due = t;
        due += 1.0 / 60;
        if (due > t) {
            struct timespec ts = { (time_t)(due - t), (long)(fmod(due - t, 1.0) * 1e9) };
            nanosleep(&ts, NULL);
        }
    }
}

/* ---- set-up ---- */

static void view_start(void)
{
    if (fb)
        return;
    fb = malloc((size_t)FBW * FBH * sizeof(u32));
    zb = malloc((size_t)FBW * FBH * sizeof(float));
    pfb = calloc((size_t)FBW, FBH);
    stn = calloc((size_t)FBW, FBH);
    shown = calloc(VW * VH, 3);
    if (!fb || !zb || !pfb || !stn || !shown) {
        fprintf(stderr, "view: no memory\n");
        exit(1);
    }
    png_tables();
    for (u32 i = 0; i < 1024; i++) {
        u32 r = 0;
        for (int k = 0; k < 10; k++)
            r |= (i >> k & 1) << (2 * k);
        tw_tab[i] = r;
    }
    view_on = 1;
}

int view_option(const char *a, const char *v)
{
    static char cmd[1600];
    if (!strcmp(a, "--view")) {
        if (!v) return -1;
        png_dir = v;
        return 2;
    }
    if (!strcmp(a, "--view-every")) {
        if (!v || atoi(v) < 1) return -1;
        every = atoi(v);
        return 2;
    }
    if (!strcmp(a, "--view-cmd")) {
        if (!v) return -1;
        pipe_cmd = v;
        return 2;
    }
    if (!strcmp(a, "--live")) {
        pipe_cmd = "live";
        return 1;
    }
    if (!strcmp(a, "--video")) {
        if (!v) return -1;
        snprintf(cmd, sizeof cmd, "video:%s", v);
        pipe_cmd = cmd;
        return 2;
    }
    if (!strcmp(a, "--view-from")) {
        if (!v) return -1;
        view_from = (u32)strtoul(v, NULL, 0);
        return 2;
    }
    if (!strcmp(a, "--view-threads")) {
        if (!v || atoi(v) < 1) return -1;
        threads_wanted = atoi(v);
        return 2;
    }
    if (!strcmp(a, "--view-marks")) {
        marks = 1;
        return 1;
    }
    if (!strcmp(a, "--realtime")) {
        realtime = 1;
        return 1;
    }
    return 0;
}

int view_init(void)
{
    static char cmd[2048];
    if (!png_dir && !pipe_cmd && !realtime)
        return 0;
    if (!every)
        every = !pipe_cmd ? 60 : !strcmp(pipe_cmd, "live") ? 2 : 1;
    pipe_fps = 60 / every > 0 ? 60 / every : 1;
    if (pipe_cmd && !strcmp(pipe_cmd, "live")) {
        snprintf(cmd, sizeof cmd, "ffplay -loglevel error -autoexit -f rawvideo -pixel_format rgb24 -video_size %dx%d "
                 "-framerate 60 -window_title vf4debug -i - >/dev/null", VW, VH);
        pipe_cmd = cmd;
    } else if (pipe_cmd && !strncmp(pipe_cmd, "video:", 6)) {
        char file[1600];
        snprintf(file, sizeof file, "%s", pipe_cmd + 6);
        snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
                 "-framerate %d -i - -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p '%s' >/dev/null", VW, VH,
                 pipe_fps, file);
        pipe_cmd = cmd;
    }
    if (png_dir && mkdir(png_dir, 0777) && errno != EEXIST) {
        perror(png_dir);
        return -1;
    }
    view_start();
    return 0;
}

/* the script's "picture FILE": the next frame the game renders is drawn
 * and written there (on a stage with a pass into a texture, the frame
 * after it, when the view has drawn that texture too) */
void view_picture(const char *path)
{
    view_start();
    if (!every)
        every = 1;
    snprintf(pic_path, sizeof pic_path, "%s", path);
    pic_request = 1;
    pic_targets_seen = pic_targets_drawn = pic_waited = 0;
}

void view_close(void)
{
    if (pipe_f) {
        pclose(pipe_f);
        pipe_f = NULL;
    }
}

void view_stats(FILE *f)
{
    if (!fb)
        return;
    fprintf(f, "  view: %u frames rendered by the game, %u drawn (%.1f ms each: %.1f transform and light, %.1f "
            "rasterize; %llu triangles a frame), %u passes into textures drawn, %u PNG files, %u frames to the "
            "viewer, %u textures decoded\n", screen_renders, pictures, pictures ? draw_seconds * 1000 / pictures : 0.0,
            pictures ? tl_seconds * 1000 / pictures : 0.0, pictures ? raster_seconds * 1000 / pictures : 0.0,
            (unsigned long long)(pictures ? tri_count / pictures : 0), target_pictures, png_written, pipe_frames,
            tex_decodes);
}
