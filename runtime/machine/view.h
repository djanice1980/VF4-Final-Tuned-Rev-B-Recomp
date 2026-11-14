/* The view: a picture of what the game sends to the graphics hardware,
 * drawn by the harness so that a person can watch a run. The game's state
 * does not depend on it; the one thing it writes into the machine is the
 * picture of a pass the game renders into a texture, in texture memory,
 * which the game never reads. See view.c. */
#ifndef VF4_VIEW_H
#define VF4_VIEW_H

#include "sh4.h"
#include <stdio.h>

extern int view_on;

/* the command line: 0 when the option is not the view's, 1 when it was
 * taken, 2 when it was taken with its value, -1 when the value is bad */
int view_option(const char *option, const char *value);
int view_init(void);
void view_close(void);
void view_stats(FILE *f);
/* the script's "picture FILE": the next frame the game renders goes there */
void view_picture(const char *path);

/* recomp: the game window (runtime/host) draws every frame and takes the
 * newest picture at each vertical blank: *w x VIEW_H, 3 bytes a pixel
 * (red, green, blue), or NULL before the game has rendered one. *w is
 * VIEW_W, or the wide screen's width */
#define VIEW_W 640
#define VIEW_H 480
#define VIEW_W_MAX 1280                 /* the widest wide screen */
void view_host_start(void);
const u8 *view_host_picture(int *w);
/* recomp: the wide screen: the pass to the screen is drawn `width` pixels
 * wide (even, VIEW_W to VIEW_W_MAX), the game's VIEW_W in its middle and
 * what the camera sees past its sides around it. VIEW_W: no wide screen.
 * Any thread; the next pass drawn takes it */
void view_host_wide(int width);

/* recomp (M6): the GPU path. With view_host_gpu set, a pass to the screen
 * is not rasterized here: after transform, lighting and the translucent
 * sort, what the rasterizer would draw is handed to submit, on the
 * machine's thread, and the game goes on (passes into textures are still
 * drawn here, into texture memory). The host draws it on the GPU with the
 * rasterizer's rules (runtime/host/gpu_render.c). Everything in the frame
 * is valid only during the call. */
typedef struct view_gpu_vertex {
    float x, y, z, u, v;                /* the target's pixels; z = 1/w, larger is nearer; texture coordinates */
    u32 col, ofs;                       /* base and offset colour, 0xAABBGGRR (bytes R, G, B, A in memory) */
    u32 state;                          /* which view_gpu_state */
} view_gpu_vertex;
typedef struct view_gpu_state {
    u32 pcw, isp, tsp, tcw;             /* the chip's control words */
    u32 tex_at, tex_w, tex_h, tex_levels;   /* the texture in the texel arena (tex_w 0: none) */
    s32 clip[4];                        /* x0, y0, x1, y1: the user clip, pixels, inclusive */
    u32 pad[4];
} view_gpu_state;
typedef struct view_gpu_draw {
    u32 first, count;                   /* vertices: count / 3 triangles */
    u32 list;                           /* 0 opaque, 1 punch-through, 2 translucent */
    u32 depth;                          /* the depth compare: bit 0 less, 1 equal, 2 greater (new against stored) */
    u32 zwrite, src, dst;               /* the blend factors (translucent): the chip's 0..7 */
    u32 shadow;                         /* the polygons take shadows (opaque, punch-through) */
} view_gpu_draw;
typedef struct view_gpu_volume {
    u32 first, count;                   /* in vol_xyz: triangles' corners, 3 floats each */
    int kind;                           /* 1 inside is shadow; 2 inside is not */
    float x0, y0, x1, y1;               /* the pixels it can touch, inclusive */
} view_gpu_volume;
typedef struct view_gpu_range {
    u32 at, n;                          /* texels of the arena to upload */
} view_gpu_range;
typedef struct view_gpu_frame {
    unsigned frame;                     /* the vertical blank */
    int w, h;                           /* the target, pixels */
    float zmax;                         /* the largest z of anything drawn */
    u32 alpha_ref;
    int fog_on;
    float fog_density;
    u32 fog_tab[129], fog_col, fog_col_vert;
    int shadow_scale;                   /* 256: no shadows */
    /* the background plane: colour = c0 + ga (x - x0) + gb (y - y0), then fog_amount (0..256) of the fog colour */
    float bg_c0[3], bg_ga[3], bg_gb[3], bg_x0, bg_y0;
    int bg_fog;
    const view_gpu_vertex *v;
    u32 nv;
    const view_gpu_state *st;
    u32 nst;
    const view_gpu_draw *draws;
    u32 ndraws;
    const float *vol_xyz;
    u32 nvol_xyz;                       /* corners */
    const view_gpu_volume *vols;
    u32 nvols;
    u32 translucent_volumes;            /* volumes of the translucent list, not drawn by the GPU path */
    /* the texel arena (0xAARRGGBB): the ranges that changed since the last frame; reset: drop everything */
    const u32 *arena;
    u32 arena_texels;
    const view_gpu_range *dirty;
    u32 ndirty;
    int arena_reset;
} view_gpu_frame;
#define VIEW_GPU_ARENA (16u << 20)      /* texels */
void view_program_changed(void);   /* recomp (test mode): look the program's tables up again */
void view_host_gpu(void (*submit)(const view_gpu_frame *f));

/* called from hw_gfx.c, only while view_on is set */
void view_list_segment(sh4 *c, u32 addr, u32 len);       /* a display list segment the ELAN is told to run */
void view_reg_write(sh4 *c, int chip, u32 off, u32 v);   /* a write to a rendering chip's core register */
void view_frame(sh4 *c);                                 /* once per vertical blank */
/* a rendering chip's core register as it stands (hw_gfx.c) */
u32 view_core_reg(int chip, u32 off);

#endif
