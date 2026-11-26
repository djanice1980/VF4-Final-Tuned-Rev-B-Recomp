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
/* recomp: the game window's frame skip (docs/design.md section 71): the
 * screen is drawn for every nth frame the game renders (1: all of them; 2:
 * every second one). The others' lists are not taken, nor those of the
 * passes into textures the game makes for them (section 78). Any thread,
 * at any time. */
void view_host_every(int n);

/* recomp: rollback's re-run frames (docs/design.md section 56) were shown
 * the first time. VIEW_RERUN: a screen's lists are not taken and no screen
 * is drawn; set it as the machine goes back (the textures are looked at
 * again). VIEW_RERUN_LAST: the last re-run frame, whose lists the frame
 * after it shows: they are taken, still no screen drawn. VIEW_LIVE: as
 * ever. A pass into a texture goes as the screen it is made for (section
 * 78): taken when that one's lists are. */
enum { VIEW_LIVE, VIEW_RERUN_LAST, VIEW_RERUN };
void view_rerun(int mode);

/* recomp (M6): the GPU path. With view_host_gpu set, a pass to the screen
 * is not rasterized here: after transform, lighting and the translucent
 * sort, what the rasterizer would draw is handed to submit, on the
 * machine's thread, and the game goes on. The host draws it on the GPU with
 * the rasterizer's rules (runtime/host/gpu_render.c). Everything in the
 * frame is valid only during the call.
 *
 * A pass into a texture (docs/design.md section 78: a stage's reflection
 * in its water) is drawn here, into texture memory, unless
 * view_host_gpu_passes is on: then it is handed to submit as well, before
 * the pass to the screen that uses it, with `target` saying which of the
 * host's VIEW_GPU_TARGETS target pictures it is for. Nothing goes into
 * texture memory then, and a state whose texture is such a pass's says so
 * (its `target`): the host takes its texels from that picture. */
#define VIEW_GPU_TARGETS 4
typedef struct view_gpu_vertex {
    float x, y, z, u, v;                /* the target's pixels; z = 1/w, larger is nearer; texture coordinates */
    u32 col, ofs;                       /* base and offset colour, 0xAABBGGRR (bytes R, G, B, A in memory) */
    u32 state;                          /* which view_gpu_state */
    /* lighting on the GPU (view_host_gpu_lighting, docs/design.md section 67): lit not 0 says the
     * vertex was not lit here. Its colours are to be made from n, its normal, and l, the point it
     * is lit at (both in the game's view space), by the strip record lit - 1 and that record's
     * light set; col and ofs are 0 */
    float n[3], l[3];
    u32 lit;
} view_gpu_vertex;
/* a strip record, VIEW_GPU_LIT_FLOATS floats: the material's base colour (r, g, b, a: 0..255), its
 * specular colour (r, g, b) and power (0: none), the light set's number, 1 when the material takes
 * no lights, two spare. A light set, VIEW_GPU_LIGHTSET_FLOATS floats: the ambient colour (r, g, b,
 * 0), then 15 lights of 16 floats: colour (r, g, b), flags (1 on, 2 point, 4 linear fall);
 * direction (x, y, z), fall a; position (x, y, z), fall b; cone k, cone b, diffuse mode, routing */
#define VIEW_GPU_LIT_FLOATS 12
#define VIEW_GPU_LIGHTSET_FLOATS 244
typedef struct view_gpu_state {
    u32 pcw, isp, tsp, tcw;             /* the chip's control words */
    u32 tex_at, tex_w, tex_h, tex_levels;   /* the texture in the texel arena (tex_w 0: none) */
    s32 clip[4];                        /* x0, y0, x1, y1: the user clip, pixels, inclusive */
    /* section 78: target not 0: the texture is the host's target picture target - 1, not the arena's
     * (tex_w x tex_h its size, one level); texel (x, y) is the picture's pixel (x + target_ox, y), and
     * where u is clamped it may run target_ox texels past each side (the wide screen) */
    u32 target, target_ox;
    u32 pad[2];
} view_gpu_state;
typedef struct view_gpu_draw {
    u32 first, count;                   /* vertices: count / 3 triangles */
    u32 list;                           /* 0 opaque, 1 punch-through, 2 translucent */
    u32 depth;                          /* the depth compare: bit 0 less, 1 equal, 2 greater (new against stored) */
    u32 zwrite, src, dst;               /* the blend factors (translucent): the chip's 0..7 */
    u32 shadow;                         /* the polygons take shadows (opaque, punch-through) */
    u32 solid;                          /* a model's strips, the 3D scene (opaque, punch-through); 0: what the game
                                         * sends straight to the chips, its 2D layer (docs/design.md section 73) */
} view_gpu_draw;
typedef struct view_gpu_volume {
    u32 first, count;                   /* in vol_xyz: triangles' corners, 3 floats each */
    int kind;                           /* 1 inside is shadow; 2 inside is not */
    float x0, y0, x1, y1;               /* the pixels it can touch, inclusive */
} view_gpu_volume;
typedef struct view_gpu_range {
    u32 at, n;                          /* texels of the arena to upload */
} view_gpu_range;
/* recomp (ray tracing, docs/design.md section 74): with view_host_rt on, a frame also carries the
 * scene as the models' strips made it: every triangle, before the near plane, the culling and the
 * screen's edges took any away, with what the lights need. A position is in the vertex matrix's
 * space (the eye at the origin; the game's view space has x and z turned round against it), a
 * normal in the game's view space, as the lights are. */
typedef struct view_rt_vertex {
    float p[3], n[3], uv[2];
    u32 col, ofs;                       /* colours of its own, 0xAARRGGBB (col 0: none, the material's) */
} view_rt_vertex;
typedef struct view_rt_tri {
    u32 strip;                          /* which view_rt_strip */
    view_rt_vertex v[3];
} view_rt_tri;
#define VIEW_RT_UNLIT 0x001u            /* the material takes no lights */
#define VIEW_RT_ENV 0x002u              /* its texture coordinates come from the normals */
#define VIEW_RT_BUMPED 0x008u
#define VIEW_RT_LIST(flags) ((flags) >> 4 & 3u)     /* 0 opaque, 1 punch-through, 2 translucent */
#define VIEW_RT_OFFSET 0x080u           /* its polygons take the offset (specular) colour */
#define VIEW_RT_ONE_SIDED 0x100u        /* only the triangles that turn one way on the screen are drawn */
#define VIEW_RT_BACK 0x800u             /* one-sided: the way that is kept is the other one */
typedef struct view_rt_strip {
    float base[4], spec[4];             /* the material: base colour (r, g, b, a: 0..255), specular colour and power */
    u32 lights, state;                  /* its light set and its view_gpu_state */
    u32 flags, pad;                     /* VIEW_RT_* */
} view_rt_strip;
typedef struct view_gpu_frame {
    unsigned frame;                     /* the vertical blank */
    int w, h;                           /* the target, pixels */
    /* section 78: 0: the pass to the screen. 1 + n: a pass into a texture, for the host's target
     * picture n, w x h pixels: the texture's own width in its middle and target_ox columns drawn
     * beyond each side (the wide screen). It comes before the pass to the screen that uses it. */
    int target, target_ox;
    float zmax;                         /* the largest z of anything drawn */
    /* the 3D scene's projection, its first strip's (section 73): a point (X, Y, Z) before the eye is
     * at x = proj[1] + proj[0] X / Z, y = proj[3] + proj[2] Y / Z, and its z is 1 / Z. proj[0] 0: no strip */
    float proj[4];
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
    const float *lit;                   /* the strip records and the light sets (lighting on the GPU) */
    u32 nlit;                           /* floats */
    const float *lights;
    u32 nlights;                        /* floats */
    /* ray tracing (section 74): the scene's triangles and strips, and the near plane's distance;
     * with them the light sets above are there whatever lights the vertices */
    const view_rt_tri *rt_tris;
    u32 nrt_tris;
    const view_rt_strip *rt_strips;
    u32 nrt_strips;
    float rt_near;
    /* recomp (traced water): rt_mirror not 0: the frame has states with a target, the water. 1: no
     * pass was drawn for them; the ray tracing mirrors the scene there in the pass's place
     * (view_host_rt_mirror). 2: the pass was handed to the host as ever. rt_mirror_bg: what that
     * pass had behind everything (0..1, its background plane with its fog), for a mirror's ray
     * that leaves the scene */
    int rt_mirror;
    float rt_mirror_bg[3];
    /* not 0: where in lights (a float's index) that pass's fog begins: the 129 steps of its table
     * (0..255), its color (r, g, b: 0..255), its density, and 1 when the fog is on */
    u32 rt_fog_at;
#define VIEW_RT_FOG_FLOATS 134
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
/* recomp (section 78): the passes into textures go to submit too (on), or are drawn here into
 * texture memory as ever (off, the default). Any thread; the next pass takes it */
void view_host_gpu_passes(int on);
/* recomp (section 67): the GPU path's lights are worked out by the host's vertex shader (on) or
 * here as ever (off, the default). Any thread; the next pass to the screen takes it */
void view_host_gpu_lighting(int on);
/* recomp (section 74): the GPU path's frames carry the scene for ray tracing (on) or not (off, the
 * default). Any thread; the next pass to the screen takes it */
void view_host_rt(int on);
/* recomp (traced water): while it is on and view_host_gpu_passes is off, a pass into a texture of
 * the kind the host could draw is neither drawn here nor handed on: the states that use its
 * picture are marked as with a pass the host draws (their target), and the frame says so
 * (rt_mirror). For a host whose ray tracing mirrors the scene on those polygons. Any thread; the
 * next pass takes it */
void view_host_rt_mirror(int on);

/* called from hw_gfx.c, only while view_on is set */
void view_list_segment(sh4 *c, u32 addr, u32 len);       /* a display list segment the ELAN is told to run */
void view_reg_write(sh4 *c, int chip, u32 off, u32 v);   /* a write to a rendering chip's core register */
void view_frame(sh4 *c);                                 /* once per vertical blank */
/* a rendering chip's core register as it stands (hw_gfx.c) */
u32 view_core_reg(int chip, u32 off);

#endif
