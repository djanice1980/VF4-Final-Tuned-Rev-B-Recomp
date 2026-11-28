/* Ray traced light (docs/design.md section 74). The GPU renderer draws the game's picture as
 * ever; with ray tracing on, a second device of the same GPU traces the frame's scene (handed
 * over by runtime/machine/view.c, view_host_rt) and gives back two pictures the renderer puts
 * into its own after the solid lists: the light's ratio and the glass's reflections.
 *
 * rt_scene.c packs a frame's scene as the tracing shader reads it, whatever the API; a backend
 * (rt_vk.c: Vulkan) owns the device, the acceleration structures and the shader. */
#ifndef VF4FT_RT_H
#define VF4FT_RT_H

#include <SDL3/SDL.h>

struct view_gpu_frame;

/* a triangle as the shader has it (its Tri): corners and normals in the game's view space */
typedef struct rt_tri {
    float p0[4], p1[4], p2[4], n0[4], n1[4], n2[4];
    float uv01[4], uv2[2];
    Uint32 strip;               /* its view_rt_strip */
    Uint32 flags;               /* RT_TRI_* */
    Uint32 col[4];              /* its corners' own colors (0: the material's) */
} rt_tri;
#define RT_TRI_BLEND 3u         /* 0 opaque, 1 by its alpha (the translucent list), 2 punch-through */
#define RT_TRI_BAKED 4u         /* colors of its own, or a material that takes no lights */
#define RT_TRI_FIGHTER 8u       /* lit, and not marked to take the game's shadows */
#define RT_TRI_ONE_SIDED 16u
#define RT_TRI_BACK 32u         /* one-sided: its front is the other way */
#define RT_TRI_MIRROR 64u       /* water: the game gives it its reflection picture; the rays mirror the scene there */

/* the shader's constants (its P) */
typedef struct rt_consts {
    float proj[4];              /* sx, cx, sy, cy */
    float misc[4];              /* near z, picture pixels a game pixel, alpha reference, the game's shadow scale (of 1) */
    float shadow_dir[4];        /* the way the game's shadow light travels; w 1: known */
    Sint32 size[4];             /* picture w, h; ambient rays; shadow rays a light */
    Sint32 base[4];             /* the first triangle of each group */
    float dist[4];              /* ambient rays' reach, shadow rays' reach, the sun's radius, a bulb's radius */
    float tune[4];              /* eps, baked surfaces' share of the occlusion, the most a pixel is brightened, the glass's gain */
    Sint32 opt[4];              /* reflection rays, bounce on, point lights' shadows on, seed */
    float tune2[4];             /* the glass's roughness */
    Sint32 counts[4];           /* texels; the filter's neighborhood, in traced pixels each way */
    float mirror[4];            /* the water's plane: its normal towards the eye (view space); w: 0 no water, 1 the
                                 * mirror in the place of the game's picture, 2 over it by the angle's share */
    float mirror_bg[4];         /* what a mirror's ray that leaves the scene shows (0..1); w: the plane's place (normal . point) */
    float mirror_fog[4];        /* x: where the fog of the water's picture begins in the lights' floats (0: none) */
} rt_consts;

/* The triangles in four groups, each an acceleration structure of its own: 0 the fighters' solid
 * ones, 1 the fighters' by-alpha ones, 2 the stage's solid ones, 3 the stage's by-alpha ones. A
 * group's instance carries its number, and mask 1 (fighters) or 2 (stage). */
typedef struct rt_scene {
    Uint32 ntri, count[4], base[4];
    rt_consts consts;
} rt_scene;

/* how much of a frame is traced (the Ray tracing quality setting, section 76; the levels are
 * gpu_render.c's) */
typedef struct rt_quality {
    int ambient, shadow, reflection;    /* rays a traced pixel: for the light around, towards each light, in a mirror */
    int bounce;                 /* an ambient ray brings the light of what it meets (1), or only its shade (0) */
    int div;                    /* the traced picture is the renderer's over this each way (1: every pixel) */
    int smooth;                 /* the filter takes the traced pixels this far around each way (1 to 3) */
} rt_quality;

/* first: the frame's triangles sorted into the groups (s->ntri, count, base); the triangles
 * rt_scene_fill will write are counted here, so the caller can make room */
void rt_scene_count(const struct view_gpu_frame *f, rt_scene *s);
/* then: the triangles (s->ntri of them) and their corners (9 floats each) in group order, and the
 * constants for a picture w x h pixels (texels: the arena's size) */
void rt_scene_fill(const struct view_gpu_frame *f, rt_scene *s, rt_tri *tris, float *pos, int w, int h, Uint32 texels,
                   const rt_quality *q);
/* traced water over the game's own picture of it (docs/design.md sections 81 and 90): the mirror
 * takes percent of the share the eye's angle gives it (100: all of it, the start's; 0: none: the
 * game's water as it is). Any thread; rt_scene_fill takes it up with the next frame */
void rt_scene_water_over(int percent);

typedef struct rt_backend {
    const char *name;
    /* a device on the GPU of that name with ray queries; 0, or -1 with the reason as SDL's error */
    int (*start)(const char *gpu_name);
    void (*stop)(void);
    /* texels of the arena that changed (view.c's, as the GPU renderer uploads them) */
    void (*texels)(Uint32 at, Uint32 n, const Uint32 *data);
    /* traces the frame for a picture w x h; the results, w * h words each (rt_light.comp's two
     * outputs), stay good until the next call. reach, when not NULL: how far each pixel's eye
     * ray went as well (w * h floats; 0: it met nothing), which the renderer needs to fill a
     * traced picture smaller than its own in. 0; 1 when the frame holds nothing for the rays
     * (no results: the frame is drawn without traced light and the tracer goes on; docs/design.md
     * section 108); -1 for a fault, with SDL's error */
    int (*trace)(const struct view_gpu_frame *f, int w, int h, const rt_quality *q, const Uint32 **light, const Uint32 **refl,
                 const float **reach);
} rt_backend;

extern const rt_backend rt_vulkan;      /* rt_vk.c */
#ifdef _WIN32
extern const rt_backend rt_directx;     /* rt_dx12.c: DirectX 12, in the Windows game only (section 75) */
#endif

#endif
