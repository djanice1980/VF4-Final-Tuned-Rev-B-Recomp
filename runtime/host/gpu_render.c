/* The GPU renderer: see gpu_render.h.
 *
 * A frame is one render pass into a target of the scene's size times the
 * scale, with a depth and stencil buffer:
 *
 *   1. the background plane over the whole target (depth cleared to 0, the
 *      far end: z is 1/w and larger is nearer, as in view.c);
 *   2. the opaque and punch-through lists in their order, each batch with
 *      the depth compare of its polygons; they write stencil bit 0 when
 *      their polygons take shadows (view.c's per-pixel "pfb");
 *   3. the opaque list's modifier volumes, one at a time: the volume's
 *      triangles invert stencil bit 1 where they lie in front of what is
 *      drawn (an odd count is inside), then a rectangle over the volume
 *      moves that into bit 2 (an inclusion volume sets it, an exclusion
 *      volume clears it) and clears bit 1; then every pixel with bits 0 and
 *      2 is multiplied by the shadow scale (view.c's band_item);
 *   4. the translucent list from the back, blended.
 *
 * Depth is z times 1 / (the frame's largest z), so it fits 0..1 and the
 * compares are the rasterizer's. The texture texels live in one storage
 * buffer (view.c's arena); the triangles' states in another; the fragment
 * shader fetches and filters texels itself, by the rasterizer's rules.
 *
 * Not drawn: the translucent list's volumes (shadows on translucent
 * polygons); a frame that has them is counted (gpu_render_stats).
 *
 * With ambient occlusion on (gpu_render_occlusion, docs/design.md section
 * 73) the pass ends after step 3 and the depth buffer is read: one pass
 * works the occlusion out of it, one smooths it, and the pass that goes on
 * with step 4 first multiplies it into the picture where stencil bit 3
 * says the 3D scene is (the opaque and punch-through lists write that bit
 * for a model's strips, and clear it for what the game sends straight to
 * the chips: its 2D layer keeps its colors).
 *
 * With ray tracing on (gpu_render_raytracing, section 74) step 3 is left out, and after step 2
 * the frame's scene is traced on a device of the ray tracing's own (rt.h): its two pictures,
 * the light's ratio and the glass's reflections, come back through the processor's memory, and
 * a pass into a second color target copies the picture and, where stencil bit 3 is set, puts
 * them in (color * ratio * (1 - share) + mirrored color). Step 4 goes on there. Ambient
 * occlusion out of the depth buffer is left out then: the rays bring their own.
 *
 * Ray tracing quality (section 76) says how much is traced: at High every pixel of the picture,
 * as above. At Medium and Low the traced pictures are half the picture's size each way, and with
 * them comes how far each traced pixel's eye ray went. A pass that reads the depth buffer then
 * fills the picture in from them (rt_fill.frag: each pixel from the traced pixels around it that
 * lie about as deep), and the pass that goes on first puts the picture as it was back where
 * stencil bit 3 is not set.
 *
 * A pass into a texture (section 78: a stage's reflection in its water) comes from the machine
 * before the frame that uses it, and is kept for it (pend). It is drawn first, by steps 1 to 4
 * at one pixel a pixel, into a target picture of its own (t_pass), with buffers of its own; the
 * frame's states that name that picture take their texels from it (scene.frag). With ray tracing
 * wanted the machine's thread draws such passes itself, into texture memory, as it did before:
 * the ray tracing's device has the texel arena and not these pictures.
 */
#include "gpu_render.h"
#include "view.h"
#include "gpu_shaders.h"
#include "rt.h"

#include <string.h>

#define ARENA_BYTES (VIEW_GPU_ARENA * 4u)

/* ---- what the machine's thread hands over ---- */

typedef struct packet {
    view_gpu_frame f;
    view_gpu_vertex *v;
    size_t v_cap;
    view_gpu_state *st;
    size_t st_cap;
    view_gpu_draw *d;
    size_t d_cap;
    float *vol;
    size_t vol_cap;
    view_gpu_volume *vols;
    size_t vols_cap;
    float *lit, *lights;    /* lighting on the GPU: the strip records and the light sets */
    size_t lit_cap, lights_cap;
    view_rt_tri *rt_t;      /* ray tracing: the scene's triangles and strips */
    size_t rt_t_cap;
    view_rt_strip *rt_s;
    size_t rt_s_cap;
    Uint64 pts;         /* when it is to be shown, by the game's clock (0: as it comes) */
    size_t up_end;      /* the texel uploads up to here are this frame's and those before it */
    struct held *pass;  /* the passes into textures made for it (section 78), in their order: VIEW_GPU_TARGETS */
    int npass;
} packet;

/* a pass into a texture as handed over, in memory of its own */
typedef struct held {
    view_gpu_frame f;
    view_gpu_vertex *v;
    size_t v_cap;
    view_gpu_state *st;
    size_t st_cap;
    view_gpu_draw *d;
    size_t d_cap;
    float *vol;
    size_t vol_cap;
    view_gpu_volume *vols;
    size_t vols_cap;
    float *lit, *lights;
    size_t lit_cap, lights_cap;
} held;

/* made once and kept: the machine's thread may hand a frame over while the
 * window's thread stops the renderer or starts it again (docs/design.md
 * section 53) */
static SDL_Mutex *lock;
static packet pk[4];
static held pk_pass[4][VIEW_GPU_TARGETS];
static held pend[VIEW_GPU_TARGETS];     /* the passes into textures since the last frame, for the next one */
static int npend;
static int pk_wait[2], pk_nwait;        /* the packets waiting to be drawn, oldest first (2 at most) */
static int pk_reading = -1;             /* the one the window draws */
static unsigned stat_submitted, stat_replaced;  /* frames from the machine; those replaced before they were drawn */
/* texel uploads waiting: [at, n, n texels] after one another */
static Uint32 *up;
static size_t up_n, up_cap;
/* the renderer has just started (again): the next frame sends the whole
 * arena, as the device's copy of it is new and empty */
static int up_all;
/* the uploads waiting begin with the whole arena (a device of the ray tracing's own waits for that) */
static int up_whole;
static unsigned stat_frames, stat_max_tris, stat_tvol_frames;

static int grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap : 1024;
    while (c < need)
        c *= 2;
    void *n = SDL_realloc(*p, c * elem);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}
#define GROW(p, cap, need) grow((void **)&(p), &(cap), (need), sizeof *(p))

void gpu_render_submit(const view_gpu_frame *f)
{
    gpu_render_submit_at(f, 0);
}

/* a pass into a texture into memory of its own (the lock is held) */
static int hold(held *h, const view_gpu_frame *f)
{
    if (GROW(h->v, h->v_cap, f->nv) || GROW(h->st, h->st_cap, f->nst) || GROW(h->d, h->d_cap, f->ndraws) ||
        GROW(h->vol, h->vol_cap, (size_t)f->nvol_xyz * 3) || GROW(h->vols, h->vols_cap, f->nvols) ||
        GROW(h->lit, h->lit_cap, (size_t)f->nlit + 1) || GROW(h->lights, h->lights_cap, (size_t)f->nlights + 1))
        return -1;
    h->f = *f;
    SDL_memcpy(h->v, f->v, (size_t)f->nv * sizeof *h->v);
    SDL_memcpy(h->st, f->st, (size_t)f->nst * sizeof *h->st);
    SDL_memcpy(h->d, f->draws, (size_t)f->ndraws * sizeof *h->d);
    SDL_memcpy(h->vol, f->vol_xyz, (size_t)f->nvol_xyz * 3 * sizeof(float));
    SDL_memcpy(h->vols, f->vols, (size_t)f->nvols * sizeof *h->vols);
    if (f->nlit)
        SDL_memcpy(h->lit, f->lit, (size_t)f->nlit * sizeof(float));
    if (f->nlights)
        SDL_memcpy(h->lights, f->lights, (size_t)f->nlights * sizeof(float));
    h->f.v = h->v;
    h->f.st = h->st;
    h->f.draws = h->d;
    h->f.vol_xyz = h->vol;
    h->f.vols = h->vols;
    h->f.lit = h->lit;
    h->f.lights = h->lights;
    h->f.rt_tris = NULL;
    h->f.rt_strips = NULL;
    h->f.nrt_tris = h->f.nrt_strips = 0;
    h->f.dirty = NULL;
    h->f.ndirty = 0;
    h->f.arena = NULL;
    return 0;
}

void gpu_render_submit_at(const view_gpu_frame *f, Uint64 pts)
{
    SDL_LockMutex(lock);
    /* the texels first: every frame's, in order, whether it is drawn or not.
     * After a start, all of the arena that is in use */
    if (up_all && !GROW(up, up_cap, (size_t)f->arena_texels + 2)) {
        up_n = 0;
        if (f->arena_texels) {
            up[up_n++] = 0;
            up[up_n++] = f->arena_texels;
            SDL_memcpy(up + up_n, f->arena, (size_t)f->arena_texels * 4);
            up_n += f->arena_texels;
        }
        up_all = 0;
        up_whole = 1;
    }
    for (Uint32 i = 0; i < f->ndirty; i++) {
        Uint32 at = f->dirty[i].at, n = f->dirty[i].n;
        if (GROW(up, up_cap, up_n + 2 + n))
            break;
        up[up_n++] = at;
        up[up_n++] = n;
        SDL_memcpy(up + up_n, f->arena + at, (size_t)n * 4);
        up_n += n;
    }
    if (f->target) {
        /* a pass into a texture (section 78): kept for the frame that comes next and uses it. One
         * for the same picture that no frame came for goes; the order they came in stays */
        int k = 0;
        while (k < npend && pend[k].f.target != f->target)
            k++;
        if (k < npend) {
            held old = pend[k];
            for (int i = k; i + 1 < npend; i++)
                pend[i] = pend[i + 1];
            pend[--npend] = old;
        }
        if (npend < VIEW_GPU_TARGETS && !hold(&pend[npend], f))
            npend++;
        SDL_UnlockMutex(lock);
        return;
    }
    stat_submitted++;
    int w = 0;
    while (w == pk_reading || (pk_nwait > 0 && w == pk_wait[0]) || (pk_nwait > 1 && w == pk_wait[1]))
        w++;
    packet *p = &pk[w];
    if (GROW(p->v, p->v_cap, f->nv) || GROW(p->st, p->st_cap, f->nst) || GROW(p->d, p->d_cap, f->ndraws) ||
        GROW(p->vol, p->vol_cap, (size_t)f->nvol_xyz * 3) || GROW(p->vols, p->vols_cap, f->nvols) ||
        GROW(p->lit, p->lit_cap, (size_t)f->nlit + 1) || GROW(p->lights, p->lights_cap, (size_t)f->nlights + 1) ||
        GROW(p->rt_t, p->rt_t_cap, (size_t)f->nrt_tris + 1) || GROW(p->rt_s, p->rt_s_cap, (size_t)f->nrt_strips + 1)) {
        SDL_UnlockMutex(lock);
        return;
    }
    p->f = *f;
    SDL_memcpy(p->v, f->v, (size_t)f->nv * sizeof *p->v);
    SDL_memcpy(p->st, f->st, (size_t)f->nst * sizeof *p->st);
    SDL_memcpy(p->d, f->draws, (size_t)f->ndraws * sizeof *p->d);
    SDL_memcpy(p->vol, f->vol_xyz, (size_t)f->nvol_xyz * 3 * sizeof(float));
    SDL_memcpy(p->vols, f->vols, (size_t)f->nvols * sizeof *p->vols);
    if (f->nlit)
        SDL_memcpy(p->lit, f->lit, (size_t)f->nlit * sizeof(float));
    if (f->nlights)
        SDL_memcpy(p->lights, f->lights, (size_t)f->nlights * sizeof(float));
    if (f->nrt_tris) {
        SDL_memcpy(p->rt_t, f->rt_tris, (size_t)f->nrt_tris * sizeof *p->rt_t);
        SDL_memcpy(p->rt_s, f->rt_strips, (size_t)f->nrt_strips * sizeof *p->rt_s);
    }
    p->f.rt_tris = p->rt_t;
    p->f.rt_strips = p->rt_s;
    p->f.lit = p->lit;
    p->f.lights = p->lights;
    p->f.v = p->v;
    p->f.st = p->st;
    p->f.draws = p->d;
    p->f.vol_xyz = p->vol;
    p->f.vols = p->vols;
    p->f.dirty = NULL;
    p->f.ndirty = 0;
    p->f.arena = NULL;
    p->pts = pts;
    p->up_end = up_n;
    p->pass = pk_pass[w];
    for (int i = 0; i < npend; i++) {
        held t = p->pass[i];
        p->pass[i] = pend[i];
        pend[i] = t;
    }
    p->npass = npend;
    npend = 0;
    if (pk_nwait == 2) {        /* no room: the oldest waiting is never drawn */
        stat_replaced++;
        pk_wait[0] = pk_wait[1];
        pk_nwait = 1;
    }
    pk_wait[pk_nwait++] = w;
    SDL_UnlockMutex(lock);
}

int gpu_render_waiting(unsigned frame[2], Uint64 pts[2])
{
    if (!lock)
        return 0;
    SDL_LockMutex(lock);
    int n = pk_nwait;
    for (int i = 0; i < n; i++) {
        frame[i] = pk[pk_wait[i]].f.frame;
        pts[i] = pk[pk_wait[i]].pts;
    }
    SDL_UnlockMutex(lock);
    return n;
}

/* ---- the GPU's side ---- */

static SDL_GPUDevice *gpu;
static int scale = 1;
static SDL_GPUShader *sh_scene_v, *sh_scene_f, *sh_vol_v, *sh_rect_v, *sh_bg_f, *sh_const_f;
static SDL_GPUTextureFormat ds_format;
static SDL_GPUGraphicsPipeline *pl_scene[3][8][2][64], *pl_bg, *pl_vol, *pl_resolve, *pl_shadow;
static SDL_GPUBuffer *b_tex, *b_vert, *b_state, *b_vol, *b_lit, *b_lights;
static Uint32 b_vert_size, b_state_size, b_vol_size, b_lit_size, b_lights_size;
static SDL_GPUTransferBuffer *xfer;
static Uint32 xfer_size;
static SDL_GPUTexture *t_color, *t_ds;
static Uint32 t_w, t_h;
static unsigned last_frame;
static int have_picture;
/* ambient occlusion (section 73): asked for (the window's thread sets it and draws); what this
 * device can do; the targets as they were last made */
static int ao_want, ao_can, t_ao_made;
/* ray tracing (section 74): the backend asked for (NULL: off); the one started; its arena is
 * whole (it traces only then); the targets as they were last made (t_rt_made: 0 none, else the
 * traced picture's divisor) */
static const rt_backend *rt_want, *rt_run;
/* Ray tracing quality (section 76), low to high: rays a traced pixel for the light around,
 * towards each light and in a mirror; bounced light; the traced picture's size, the renderer's
 * over that number each way; and how far around the filter smooths, in traced pixels. Low and
 * Medium trace a quarter of the pixels, and a pass of the renderer's fills the picture in from
 * them by its own depth (rt_fill.frag). Medium's filter reaches 2 traced pixels, which are 4 of
 * the picture's: about High's 3, so a shadow's edge is about as soft. */
static const rt_quality rt_levels[3] = { { 1, 1, 1, 1, 2, 3 }, { 4, 4, 2, 1, 2, 2 }, { 4, 4, 2, 1, 1, 3 } };
static int rt_level = 1;        /* asked for: Medium, as the setting's default */
static int rt_said_level = -1, rt_said_w, rt_said_h;    /* what the log last said was traced */
static int rt_whole, rt_failed, t_rt_made;
static int ds_sampled;          /* the depth buffer can be one a shader reads (ambient occlusion and the fill need it) */
static SDL_GPUShader *sh_copy_f, *sh_rt_apply_f, *sh_rt_fill_f;
static SDL_GPUGraphicsPipeline *pl_copy, *pl_rt_apply, *pl_rt_fill, *pl_copy_flat;
static SDL_GPUTexture *t_color2, *t_light, *t_refl, *t_reach, *t_final;
static SDL_GPUTransferBuffer *xfer_rt;
static Uint32 xfer_rt_size;
/* frames traced and their time, but for the first after each start, which holds one-off work (a
 * driver may compile its own shaders for the acceleration structures then): that one's time */
static unsigned stat_rt_frames;
static double stat_rt_ms, stat_rt_first_ms, stat_rt_last_ms;
static int rt_first;
/* passes into textures (section 78): asked for (gpu_render_passes: on, but for a test); each
 * target picture with its depth buffer, as last made; a texture to bind where there is none; the
 * buffers a pass's triangles and states go through, apart from the frame's */
static int pass_want = 1;
static SDL_GPUTexture *t_pass[VIEW_GPU_TARGETS], *t_pass_ds[VIEW_GPU_TARGETS], *t_none;
static Uint32 t_pass_w[VIEW_GPU_TARGETS], t_pass_h[VIEW_GPU_TARGETS];
static Uint8 t_pass_new[VIEW_GPU_TARGETS];      /* just made: nothing is known of what it holds */
/* traced water in the picture's place (section 81): a black picture of t_pass's size, which the
 * water of a traced frame is drawn over (mirror_under) */
static SDL_GPUTexture *t_under[VIEW_GPU_TARGETS];
static Uint32 t_under_w[VIEW_GPU_TARGETS], t_under_h[VIEW_GPU_TARGETS];
static SDL_GPUBuffer *bp_vert, *bp_state, *bp_vol, *bp_lit, *bp_lights;
static Uint32 bp_vert_size, bp_state_size, bp_vol_size, bp_lit_size, bp_lights_size;
static SDL_GPUTransferBuffer *xfer_p;
static Uint32 xfer_p_size;
static unsigned stat_passes, stat_pass_stale;
static void passes_follow(void);
static SDL_GPUShader *sh_ao_f, *sh_ao_blur_f, *sh_ao_apply_f;
static SDL_GPUGraphicsPipeline *pl_ao, *pl_ao_blur, *pl_ao_apply;
static SDL_GPUTexture *t_ao, *t_ao_smooth;
static SDL_GPUSampler *smp_texel;
#define AO_RADIUS 0.45f         /* in the scene's units: a fighter is about 1.7 tall */
#define AO_STRENGTH 1.0f

static SDL_GPUShader *shader(const unsigned char *code, size_t n, SDL_GPUShaderStage stage, Uint32 storage, Uint32 uniforms,
                             Uint32 samplers)
{
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code = code;
    ci.code_size = n;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = samplers;
    ci.num_storage_buffers = storage;
    ci.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(gpu, &ci);
}

static SDL_GPUBlendFactor blend_of(Uint32 f, int src)
{
    switch (f) {
    case 0: return SDL_GPU_BLENDFACTOR_ZERO;
    case 1: return SDL_GPU_BLENDFACTOR_ONE;
    case 2: return src ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_SRC_COLOR;   /* the other's color */
    case 3: return src ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_ONE;     /* the frame's own alpha: opaque, as view.c keeps it */
    default: return SDL_GPU_BLENDFACTOR_ZERO;
    }
}

static SDL_GPUCompareOp compare_of(Uint32 depth)
{
    static const SDL_GPUCompareOp ops[8] = {
        SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS,
    };
    return ops[depth & 7];
}

typedef struct pipe_desc {
    SDL_GPUShader *vs, *fs;
    int scene_input, vol_input;
    int blend;
    SDL_GPUBlendFactor src, dst;
    int color_off;
    int depth_test, depth_write;
    SDL_GPUCompareOp depth_op;
    int stencil;
    SDL_GPUCompareOp st_op;
    SDL_GPUStencilOp st_pass, st_depth_fail;
    Uint8 st_cmask, st_wmask;
    int occlusion_target;       /* one channel of 8 bits and no depth: the occlusion's own targets */
    int no_depth;               /* the picture's format and no depth: a pass that reads the depth buffer */
} pipe_desc;

static SDL_GPUGraphicsPipeline *pipeline(const pipe_desc *p)
{
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = p->vs;
    ci.fragment_shader = p->fs;
    SDL_GPUVertexBufferDescription vb;
    SDL_zero(vb);
    SDL_GPUVertexAttribute va[8];
    SDL_zero(va);
    if (p->scene_input) {
        vb.pitch = sizeof(view_gpu_vertex);
        va[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
        va[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 };
        va[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20 };
        va[3] = (SDL_GPUVertexAttribute){ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24 };
        va[4] = (SDL_GPUVertexAttribute){ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 28 };
        va[5] = (SDL_GPUVertexAttribute){ 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 32 };     /* n, l, lit */
        va[6] = (SDL_GPUVertexAttribute){ 6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 44 };
        va[7] = (SDL_GPUVertexAttribute){ 7, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 56 };
        ci.vertex_input_state.num_vertex_attributes = 8;
    } else if (p->vol_input) {
        vb.pitch = 3 * sizeof(float);
        va[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
        ci.vertex_input_state.num_vertex_attributes = 1;
    }
    if (p->scene_input || p->vol_input) {
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        ci.vertex_input_state.vertex_buffer_descriptions = &vb;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = va;
    }
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.depth_stencil_state.enable_depth_test = p->depth_test;
    ci.depth_stencil_state.enable_depth_write = p->depth_write;
    ci.depth_stencil_state.compare_op = p->depth_test ? p->depth_op : SDL_GPU_COMPAREOP_ALWAYS;
    if (p->stencil) {
        SDL_GPUStencilOpState so = { SDL_GPU_STENCILOP_KEEP, p->st_pass, p->st_depth_fail, p->st_op };
        ci.depth_stencil_state.enable_stencil_test = true;
        ci.depth_stencil_state.front_stencil_state = so;
        ci.depth_stencil_state.back_stencil_state = so;
        ci.depth_stencil_state.compare_mask = p->st_cmask;
        ci.depth_stencil_state.write_mask = p->st_wmask;
    }
    SDL_GPUColorTargetDescription ct;
    SDL_zero(ct);
    ct.format = p->occlusion_target ? SDL_GPU_TEXTUREFORMAT_R8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (p->blend) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = ct.blend_state.src_alpha_blendfactor = p->src;
        ct.blend_state.dst_color_blendfactor = ct.blend_state.dst_alpha_blendfactor = p->dst;
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    if (p->color_off) {
        ct.blend_state.enable_color_write_mask = true;
        ct.blend_state.color_write_mask = 0;
    }
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    if (!p->occlusion_target && !p->no_depth) {
        ci.target_info.depth_stencil_format = ds_format;
        ci.target_info.has_depth_stencil_target = true;
    }
    return SDL_CreateGPUGraphicsPipeline(gpu, &ci);
}

static SDL_GPUGraphicsPipeline *scene_pipeline(const view_gpu_draw *d)
{
    Uint32 list = d->list > 2 ? 2 : d->list, blend = list == 2 ? (d->src & 7) * 8 + (d->dst & 7) : 0;
    SDL_GPUGraphicsPipeline **slot = &pl_scene[list][d->depth & 7][list != 2 && d->zwrite][blend];
    if (!*slot) {
        pipe_desc p;
        SDL_zero(p);
        p.vs = sh_scene_v;
        p.fs = sh_scene_f;
        p.scene_input = 1;
        p.depth_test = 1;
        p.depth_op = compare_of(d->depth);
        p.depth_write = list != 2 && d->zwrite;
        if (list == 2) {
            p.blend = 1;
            p.src = blend_of(d->src, 1);
            p.dst = blend_of(d->dst, 0);
        } else {
            /* bit 0: the polygon on top takes shadows; bit 3: it is of the 3D scene (the reference is the draw's) */
            p.stencil = 1;
            p.st_op = SDL_GPU_COMPAREOP_ALWAYS;
            p.st_pass = SDL_GPU_STENCILOP_REPLACE;
            p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
            p.st_wmask = 9;
            p.st_cmask = 0;
        }
        *slot = pipeline(&p);
        if (!*slot)
            SDL_Log("gpu_render: pipeline (list %u, depth %u, blend %u): %s", list, d->depth, blend, SDL_GetError());
    }
    return *slot;
}

int gpu_render_init(SDL_GPUDevice *dev, int scale_)
{
    gpu = dev;
    scale = scale_ < 1 ? 1 : scale_ > 4 ? 4 : scale_;
    if (!lock && !(lock = SDL_CreateMutex()))
        return -1;
    /* what came before is for a device gone: the arena again, whole, and no frame until the next */
    SDL_LockMutex(lock);
    up_n = 0;
    up_all = 1;
    pk_nwait = 0;
    npend = 0;
    SDL_UnlockMutex(lock);
    if (!(SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_SPIRV))
        return SDL_SetError("the GPU device takes no SPIR-V (it is not Vulkan)"), -1;
    ds_format = SDL_GPUTextureSupportsFormat(gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                    ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    sh_scene_v = shader(spv_scene_vert, sizeof spv_scene_vert, SDL_GPU_SHADERSTAGE_VERTEX, 2, 1, 0);
    sh_scene_f = shader(spv_scene_frag, sizeof spv_scene_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, VIEW_GPU_TARGETS);
    sh_vol_v = shader(spv_volume_vert, sizeof spv_volume_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    sh_rect_v = shader(spv_rect_vert, sizeof spv_rect_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    sh_bg_f = shader(spv_background_frag, sizeof spv_background_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    sh_const_f = shader(spv_constant_frag, sizeof spv_constant_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    if (!sh_scene_v || !sh_scene_f || !sh_vol_v || !sh_rect_v || !sh_bg_f || !sh_const_f)
        return -1;
    pipe_desc p;
    SDL_zero(p);
    p.vs = sh_rect_v;
    p.fs = sh_bg_f;
    pl_bg = pipeline(&p);
    /* a volume's triangles: bit 1 inverted where they are in front */
    SDL_zero(p);
    p.vs = sh_vol_v;
    p.fs = sh_const_f;
    p.vol_input = 1;
    p.color_off = 1;
    p.depth_test = 1;
    p.depth_op = SDL_GPU_COMPAREOP_GREATER;
    p.stencil = 1;
    p.st_op = SDL_GPU_COMPAREOP_ALWAYS;
    p.st_pass = SDL_GPU_STENCILOP_INVERT;
    p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
    p.st_wmask = 2;
    pl_vol = pipeline(&p);
    /* where bit 1 is set: bits 1 and 2 become the reference's (4: inside a shadow, 0: not) */
    SDL_zero(p);
    p.vs = sh_rect_v;
    p.fs = sh_const_f;
    p.color_off = 1;
    p.stencil = 1;
    p.st_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
    p.st_pass = SDL_GPU_STENCILOP_REPLACE;
    p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
    p.st_cmask = 2;
    p.st_wmask = 6;
    pl_resolve = pipeline(&p);
    /* where bits 0 and 2 are set: the color times the scale */
    SDL_zero(p);
    p.vs = sh_rect_v;
    p.fs = sh_const_f;
    p.blend = 1;
    p.src = SDL_GPU_BLENDFACTOR_ZERO;
    p.dst = SDL_GPU_BLENDFACTOR_SRC_COLOR;
    p.stencil = 1;
    p.st_op = SDL_GPU_COMPAREOP_EQUAL;
    p.st_pass = SDL_GPU_STENCILOP_KEEP;
    p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
    p.st_cmask = 5;
    p.st_wmask = 0;
    pl_shadow = pipeline(&p);
    if (!pl_bg || !pl_vol || !pl_resolve || !pl_shadow)
        return -1;
    /* Ambient occlusion: the depth buffer must be one a shader can read, and there must be a
     * target of one channel. A device without them draws as with the setting off. */
    ds_sampled = SDL_GPUTextureSupportsFormat(gpu, ds_format, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    ao_can = ds_sampled &&
             SDL_GPUTextureSupportsFormat(gpu, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (ao_can) {
        sh_ao_f = shader(spv_ssao_frag, sizeof spv_ssao_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 1);
        sh_ao_blur_f = shader(spv_ssao_blur_frag, sizeof spv_ssao_blur_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
        sh_ao_apply_f = shader(spv_ssao_apply_frag, sizeof spv_ssao_apply_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
        SDL_GPUSamplerCreateInfo si;
        SDL_zero(si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        smp_texel = SDL_CreateGPUSampler(gpu, &si);
        if (sh_ao_f && sh_ao_blur_f && sh_ao_apply_f && smp_texel) {
            SDL_zero(p);
            p.vs = sh_rect_v;
            p.fs = sh_ao_f;
            p.occlusion_target = 1;
            pl_ao = pipeline(&p);
            p.fs = sh_ao_blur_f;
            pl_ao_blur = pipeline(&p);
            /* where bit 3 is set: the color times the occlusion */
            SDL_zero(p);
            p.vs = sh_rect_v;
            p.fs = sh_ao_apply_f;
            p.blend = 1;
            p.src = SDL_GPU_BLENDFACTOR_ZERO;
            p.dst = SDL_GPU_BLENDFACTOR_SRC_COLOR;
            p.stencil = 1;
            p.st_op = SDL_GPU_COMPAREOP_EQUAL;
            p.st_pass = SDL_GPU_STENCILOP_KEEP;
            p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
            p.st_cmask = 8;
            p.st_wmask = 0;
            pl_ao_apply = pipeline(&p);
        }
        if (!pl_ao || !pl_ao_blur || !pl_ao_apply) {
            SDL_Log("gpu_render: no ambient occlusion on this device: %s", SDL_GetError());
            ao_can = 0;
        }
    }
    /* ray tracing's two passes over the picture: the copy into the second color target, and the
     * traced light where bit 3 is set (they need the sampler too) */
    if (!smp_texel) {
        SDL_GPUSamplerCreateInfo si;
        SDL_zero(si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        smp_texel = SDL_CreateGPUSampler(gpu, &si);
    }
    sh_copy_f = shader(spv_copy_frag, sizeof spv_copy_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
    sh_rt_apply_f = shader(spv_rt_apply_frag, sizeof spv_rt_apply_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 3);
    if (sh_copy_f && sh_rt_apply_f && smp_texel) {
        SDL_zero(p);
        p.vs = sh_rect_v;
        p.fs = sh_copy_f;
        pl_copy = pipeline(&p);
        p.fs = sh_rt_apply_f;
        p.stencil = 1;
        p.st_op = SDL_GPU_COMPAREOP_EQUAL;
        p.st_pass = SDL_GPU_STENCILOP_KEEP;
        p.st_depth_fail = SDL_GPU_STENCILOP_KEEP;
        p.st_cmask = 8;
        p.st_wmask = 0;
        pl_rt_apply = pipeline(&p);
        /* A traced picture smaller than the renderer's (section 76): a pass without the depth
         * buffer as its target, which reads it, fills the traced light in over the whole picture;
         * then the picture as it was comes back where bit 3 is not set (the copy, with the
         * stencil's reference 0). */
        p.fs = sh_copy_f;
        pl_copy_flat = pipeline(&p);
        sh_rt_fill_f = shader(spv_rt_fill_frag, sizeof spv_rt_fill_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 5);
        if (sh_rt_fill_f) {
            SDL_zero(p);
            p.vs = sh_rect_v;
            p.fs = sh_rt_fill_f;
            p.no_depth = 1;
            pl_rt_fill = pipeline(&p);
        }
    }
    SDL_GPUBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = ARENA_BYTES;
    b_tex = SDL_CreateGPUBuffer(gpu, &bi);
    if (!b_tex)
        return -1;
    /* the scene's shader reads four target pictures (section 78): this one stands where there is none */
    SDL_GPUTextureCreateInfo ni;
    SDL_zero(ni);
    ni.type = SDL_GPU_TEXTURETYPE_2D;
    ni.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ni.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ni.width = ni.height = 1;
    ni.layer_count_or_depth = 1;
    ni.num_levels = 1;
    t_none = SDL_CreateGPUTexture(gpu, &ni);
    if (!t_none || !smp_texel)
        return -1;
    passes_follow();
    return 0;
}

static int buffer_room(SDL_GPUBuffer **b, Uint32 *size, Uint32 need, SDL_GPUBufferUsageFlags usage)
{
    if (need <= *size && *b)
        return 0;
    Uint32 s = *size ? *size : 65536;
    while (s < need)
        s *= 2;
    if (*b)
        SDL_ReleaseGPUBuffer(gpu, *b);
    SDL_GPUBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = usage;
    bi.size = s;
    *b = SDL_CreateGPUBuffer(gpu, &bi);
    *size = *b ? s : 0;
    return *b ? 0 : -1;
}

/* ao: the depth buffer is one a shader can read, and the occlusion's two targets are there. rt: 0,
 * or the traced picture's divisor: the second color target and the traced pictures' textures
 * (w / rt x h / rt, rounded up) are there, and past 1 the depth buffer is one a shader can read */
static int targets(Uint32 w, Uint32 h, int ao, int rt)
{
    if (t_color && w == t_w && h == t_h && ao == t_ao_made && rt == t_rt_made)
        return 0;
    SDL_GPUTexture **more[] = { &t_color2, &t_light, &t_refl, &t_reach };
    for (int i = 0; i < 4; i++) {
        if (*more[i])
            SDL_ReleaseGPUTexture(gpu, *more[i]);
        *more[i] = NULL;
    }
    t_final = NULL;
    if (t_color)
        SDL_ReleaseGPUTexture(gpu, t_color);
    if (t_ds)
        SDL_ReleaseGPUTexture(gpu, t_ds);
    if (t_ao)
        SDL_ReleaseGPUTexture(gpu, t_ao);
    if (t_ao_smooth)
        SDL_ReleaseGPUTexture(gpu, t_ao_smooth);
    t_ao = t_ao_smooth = NULL;
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    t_color = SDL_CreateGPUTexture(gpu, &ti);
    ti.format = ds_format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | (ao || rt > 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
    t_ds = SDL_CreateGPUTexture(gpu, &ti);
    if (ao) {
        ti.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        t_ao = SDL_CreateGPUTexture(gpu, &ti);
        t_ao_smooth = SDL_CreateGPUTexture(gpu, &ti);
    }
    if (rt) {
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        t_color2 = SDL_CreateGPUTexture(gpu, &ti);
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = (w + (Uint32)rt - 1) / (Uint32)rt;
        ti.height = (h + (Uint32)rt - 1) / (Uint32)rt;
        t_refl = SDL_CreateGPUTexture(gpu, &ti);
        ti.format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
        t_light = SDL_CreateGPUTexture(gpu, &ti);
        if (rt > 1) {
            ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            t_reach = SDL_CreateGPUTexture(gpu, &ti);
        }
    }
    t_w = w;
    t_h = h;
    t_ao_made = ao;
    t_rt_made = rt;
    return t_color && t_ds && (!ao || (t_ao && t_ao_smooth)) && (!rt || (t_color2 && t_light && t_refl)) && (rt <= 1 || t_reach)
               ? 0 : -1;
}

void gpu_render_raytracing(const rt_backend *backend, int quality)
{
    rt_level = quality < 0 ? 0 : quality > 2 ? 2 : quality;
    if (backend == rt_want)
        return;
    rt_want = backend;
    rt_failed = 0;
    view_host_rt(backend != NULL);
    view_host_rt_mirror(0);     /* traced water: on again with the first frame the new one traces */
    passes_follow();
}

void gpu_render_passes(int on)
{
    pass_want = on;
    passes_follow();
}

void gpu_render_pass_stats(unsigned *passes, unsigned *stale)
{
    *passes = stat_passes;
    *stale = stat_pass_stale;
}

double gpu_render_rt_last_ms(void)
{
    return stat_rt_last_ms;
}

void gpu_render_rt_stats(unsigned *frames, double *ms, double *first_ms)
{
    *frames = stat_rt_frames;
    *ms = stat_rt_frames ? stat_rt_ms / stat_rt_frames : 0;
    *first_ms = stat_rt_first_ms;
}

/* the ray tracing's device follows the wish: started on the renderer's GPU, stopped when it is no
 * longer wanted. Started, it first waits for the whole texel arena (asked for here). */
static void rt_follow(void)
{
    if (rt_run && rt_run != rt_want) {
        rt_run->stop();
        rt_run = NULL;
    }
    if (!rt_want || rt_run || rt_failed)
        return;
    const char *name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(gpu), SDL_PROP_GPU_DEVICE_NAME_STRING, NULL);
    if (!pl_copy || !pl_rt_apply || !SDL_GPUTextureSupportsFormat(gpu, ds_format, SDL_GPU_TEXTURETYPE_2D,
                                                                  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) ||
        rt_want->start(name)) {
        SDL_Log("gpu_render: no ray tracing (%s): %s", rt_want->name, SDL_GetError());
        rt_failed = 1;
        return;
    }
    SDL_Log("gpu_render: ray tracing (%s) started; the picture is drawn on %s", rt_want->name, name ? name : "the GPU");
    rt_run = rt_want;
    rt_whole = 0;
    rt_first = 1;
    SDL_LockMutex(lock);
    up_all = 1;
    SDL_UnlockMutex(lock);
}

void gpu_render_occlusion(int on)
{
    ao_want = on;
}

typedef struct { float rect[4], screen[4]; } rect_u;

static void rect(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, float x0, float y0, float x1, float y1,
                 const view_gpu_frame *f)
{
    rect_u u = { { x0, y0, x1, y1 }, { (float)f->w, (float)f->h, 0, 0 } };
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
}

/* ---- a scene's lists, for a frame and for a pass into a texture ---- */

typedef struct scene_fu {
    float p0[4];
    Uint32 p1[4];
    Uint32 fog[132];
} scene_fu;

typedef struct scene_bufs {
    SDL_GPUBuffer *vert, *state, *vol, *lit, *lights;
    SDL_GPUTextureSamplerBinding pass[VIEW_GPU_TARGETS];    /* the target pictures its states may name */
    float scale;                        /* the target's pixels a pixel of the scene */
} scene_bufs;

static void scene_fu_set(scene_fu *fu, const view_gpu_frame *f, float scale_)
{
    SDL_zerop(fu);
    fu->p0[0] = scale_;
    fu->p0[1] = (float)f->alpha_ref;
    fu->p0[2] = f->fog_density;
    fu->p1[0] = (Uint32)f->fog_on;
    fu->p1[1] = f->fog_col;
    fu->p1[2] = f->fog_col_vert;
    SDL_memcpy(fu->fog, f->fog_tab, sizeof f->fog_tab);
}

/* 1. the background plane */
static void scene_background(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, const view_gpu_frame *f, float scale_)
{
    SDL_BindGPUGraphicsPipeline(rp, pl_bg);
    float bg[16] = { f->bg_c0[0], f->bg_c0[1], f->bg_c0[2], scale_, f->bg_ga[0], f->bg_ga[1], f->bg_ga[2], f->bg_x0,
                     f->bg_gb[0], f->bg_gb[1], f->bg_gb[2], f->bg_y0, (float)(f->fog_col >> 16 & 255),
                     (float)(f->fog_col >> 8 & 255), (float)(f->fog_col & 255), (float)f->bg_fog };
    SDL_PushGPUFragmentUniformData(cmd, 0, bg, sizeof bg);
    rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
}

/* 2. the opaque and punch-through lists (phase 0), or 4. the translucent list (phase 1) */
static void scene_lists(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, const view_gpu_frame *f, int phase,
                        const scene_bufs *b, scene_fu *fu, const float vu[4])
{
    SDL_GPUBuffer *storage[2] = { b->state, b_tex };
    SDL_GPUBuffer *vstorage[2] = { b->lit, b->lights };     /* the vertex shader's: lighting on the GPU */
    SDL_GPUBufferBinding vbind = { b->vert, 0 };
    int list_pushed = -1;
    SDL_GPUGraphicsPipeline *bound = NULL;
    for (Uint32 i = 0; i < f->ndraws; i++) {
        const view_gpu_draw *d = &f->draws[i];
        if ((d->list == 2) != phase)
            continue;
        SDL_GPUGraphicsPipeline *pl = scene_pipeline(d);
        if (!pl)
            continue;
        if (pl != bound) {
            SDL_BindGPUGraphicsPipeline(rp, pl);
            SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
            SDL_BindGPUFragmentSamplers(rp, 0, b->pass, VIEW_GPU_TARGETS);
            SDL_BindGPUFragmentStorageBuffers(rp, 0, storage, 2);
            SDL_BindGPUVertexStorageBuffers(rp, 0, vstorage, 2);
            SDL_PushGPUVertexUniformData(cmd, 0, vu, 4 * sizeof(float));
            bound = pl;
        }
        if ((int)d->list != list_pushed) {
            fu->p0[3] = (float)d->list;
            SDL_PushGPUFragmentUniformData(cmd, 0, fu, sizeof *fu);
            list_pushed = (int)d->list;
        }
        if (d->list != 2)
            SDL_SetGPUStencilReference(rp, (Uint8)((d->shadow ? 1 : 0) | (d->solid ? 8 : 0)));
        SDL_DrawGPUPrimitives(rp, d->count, 1, d->first, 0);
    }
}

/* 3. the opaque list's volumes, then the shadow */
static void scene_shadows(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, const view_gpu_frame *f, const scene_bufs *b,
                          const float vu[4])
{
    SDL_GPUBufferBinding volbind = { b->vol, 0 };
    float zero[4] = { 0, 0, 0, 0 };
    for (Uint32 n = 0; n < f->nvols; n++) {
        const view_gpu_volume *v = &f->vols[n];
        if (!v->count)
            continue;
        SDL_BindGPUGraphicsPipeline(rp, pl_vol);
        SDL_BindGPUVertexBuffers(rp, 0, &volbind, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, vu, 4 * sizeof(float));
        SDL_PushGPUFragmentUniformData(cmd, 0, zero, sizeof zero);
        SDL_DrawGPUPrimitives(rp, v->count, 1, v->first, 0);
        SDL_BindGPUGraphicsPipeline(rp, pl_resolve);
        SDL_SetGPUStencilReference(rp, v->kind == 1 ? 4 : 0);
        float x0 = v->x0 < 0 ? 0 : v->x0, y0 = v->y0 < 0 ? 0 : v->y0;
        float x1 = v->x1 + 1 > (float)f->w ? (float)f->w : v->x1 + 1, y1 = v->y1 + 1 > (float)f->h ? (float)f->h : v->y1 + 1;
        if (x1 > x0 && y1 > y0)
            rect(cmd, rp, x0, y0, x1, y1, f);
    }
    SDL_BindGPUGraphicsPipeline(rp, pl_shadow);
    SDL_SetGPUStencilReference(rp, 5);
    float s = (float)f->shadow_scale / 256.0f, col[4] = { s, s, s, 1 };
    SDL_PushGPUFragmentUniformData(cmd, 0, col, sizeof col);
    rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
}

/* ---- passes into textures (section 78) ---- */

/* Traced water (docs/design.md section 81): with ray tracing through Vulkan, the scene mirrored by
 * rays on the polygons the game gives its reflection pass's picture to. 1: in that picture's
 * place, and the pass is not drawn at all (view_host_rt_mirror); 2: over that picture, by the
 * angle's share, and the pass is drawn here as without ray tracing. Which of the two is the
 * stage's matter (gpu_render_water, any thread); the window's thread takes it up with a frame. */
static SDL_AtomicInt water_asked;
static int water_mode = 2;

void gpu_render_water(int mode, int over_percent)
{
    SDL_SetAtomicInt(&water_asked, mode == 1 ? 1 : 2);
    rt_scene_water_over(over_percent);
}

/* the machine's thread hands them over when they are wanted, the device can, and no ray tracing
 * is wanted (its device has the arena's texels, not these pictures; with the water traced over
 * the picture it needs none: rt_light.comp, water_seen) */
static void passes_follow(void)
{
    view_host_gpu_passes(gpu && t_none && pass_want && (!rt_want || (water_mode == 2 && rt_want == &rt_vulkan)));
}

/* A state names a target picture: it is there, at the size the state's texture says? If not (the
 * pass never came: a frame of the machine's that was never drawn, a renderer just started), the
 * state is drawn without a texture this once. states: the copy that goes to the GPU. drawn: bit k
 * set: picture k was drawn for this frame. Returns how many states took a picture not drawn for it */
static unsigned pass_states(view_gpu_state *states, Uint32 n, unsigned drawn, int self)
{
    unsigned stale = 0;
    for (Uint32 i = 0; i < n; i++) {
        view_gpu_state *s = &states[i];
        if (!s->target)
            continue;
        Uint32 k = s->target - 1;
        if (k >= VIEW_GPU_TARGETS || (int)k == self || !t_pass[k] || t_pass_w[k] != s->tex_w + 2 * s->target_ox ||
            t_pass_h[k] != s->tex_h) {
            s->target = s->target_ox = 0;
            s->tex_w = 0;
            stale++;
        } else if (!(drawn >> k & 1)) {
            stale++;
        }
    }
    return stale;
}

/* under: the pictures whose black twin stands in (mirror_under) */
static void scene_pass_pictures(scene_bufs *b, int self, unsigned under)
{
    for (int k = 0; k < VIEW_GPU_TARGETS; k++) {
        b->pass[k].texture = (under >> k & 1) && t_under[k] ? t_under[k] : t_pass[k] && k != self ? t_pass[k] : t_none;
        b->pass[k].sampler = smp_texel;
    }
}

/* target picture k at w x h, made (again) when it is not there at that size. Returns its bit, or
 * 0: none */
static unsigned pass_room(int k, Uint32 w, Uint32 h)
{
    if (k < 0 || k >= VIEW_GPU_TARGETS || w < 1 || h < 1 || w > 4096 || h > 4096)
        return 0;
    if (!t_pass[k] || t_pass_w[k] != w || t_pass_h[k] != h) {
        if (t_pass[k])
            SDL_ReleaseGPUTexture(gpu, t_pass[k]);
        if (t_pass_ds[k])
            SDL_ReleaseGPUTexture(gpu, t_pass_ds[k]);
        SDL_GPUTextureCreateInfo ti;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        t_pass[k] = SDL_CreateGPUTexture(gpu, &ti);
        ti.format = ds_format;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        t_pass_ds[k] = SDL_CreateGPUTexture(gpu, &ti);
        t_pass_w[k] = w;
        t_pass_h[k] = h;
        t_pass_new[k] = 1;
        if (!t_pass[k] || !t_pass_ds[k]) {
            if (t_pass[k])
                SDL_ReleaseGPUTexture(gpu, t_pass[k]);
            if (t_pass_ds[k])
                SDL_ReleaseGPUTexture(gpu, t_pass_ds[k]);
            t_pass[k] = t_pass_ds[k] = NULL;
            t_pass_w[k] = t_pass_h[k] = 0;
            return 0;
        }
    }
    return 1u << k;
}

/* the target picture a pass into a texture is drawn into. Returns its bit, or 0: none */
static unsigned pass_picture(const view_gpu_frame *f)
{
    return pass_room(f->target - 1, f->w > 0 ? (Uint32)f->w : 0, f->h > 0 ? (Uint32)f->h : 0);
}

/* Traced water in the picture's place (section 81). The game's reflection pass is not drawn any
 * more, and its target picture keeps what the last pass left. That showed under the mirror
 * wherever the mirror's share is not whole (along a waterline, where the traced pixels around a
 * picture pixel are part water and part bank), and what it showed depended on how many passes
 * had been drawn before the mirror took over. So the water of a traced frame is drawn over black
 * instead: nothing of the game's picture is under the mirror. The game's picture itself is left
 * alone: the frames that still come without a pass after ray tracing is switched off show it.
 *
 * mirror_named: the pictures the frame's states name that no pass was drawn into for it.
 * mirror_rooms: before the states are checked, those are made where there is none (a state whose
 * picture is not there is drawn without a texture, which would be the polygon's bare color).
 * mirror_under: for a frame that was traced, their black twins made and cleared where needed, and
 * a picture that was just made cleared too; returns the pictures to draw the twin for. */
static unsigned mirror_named(const view_gpu_frame *f, unsigned drawn)
{
    unsigned named = 0;
    for (Uint32 i = 0; i < f->nst; i++)
        if (f->st[i].target && f->st[i].target <= VIEW_GPU_TARGETS)
            named |= 1u << (f->st[i].target - 1);
    return named & ~drawn;
}

static void mirror_rooms(const view_gpu_frame *f, unsigned drawn)
{
    unsigned named = mirror_named(f, drawn);
    for (Uint32 i = 0; i < f->nst && named; i++) {
        const view_gpu_state *st = &f->st[i];
        if (st->target && st->target <= VIEW_GPU_TARGETS && (named >> (st->target - 1) & 1)) {
            pass_room((int)st->target - 1, st->tex_w + 2 * st->target_ox, st->tex_h);
            named &= ~(1u << (st->target - 1));
        }
    }
}

static int clear_black(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t)
{
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = t;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!rp)
        return -1;
    SDL_EndGPURenderPass(rp);
    return 0;
}

static unsigned mirror_under(SDL_GPUCommandBuffer *cmd, const view_gpu_frame *f, unsigned drawn, int traced)
{
    unsigned named = mirror_named(f, drawn), under = 0;
    for (int k = 0; k < VIEW_GPU_TARGETS; k++) {
        if (!(named >> k & 1) || !t_pass[k])
            continue;
        if (t_pass_new[k] && clear_black(cmd, t_pass[k]) == 0)
            t_pass_new[k] = 0;
        if (!traced)
            continue;
        if (!t_under[k] || t_under_w[k] != t_pass_w[k] || t_under_h[k] != t_pass_h[k]) {
            if (t_under[k])
                SDL_ReleaseGPUTexture(gpu, t_under[k]);
            SDL_GPUTextureCreateInfo ti;
            SDL_zero(ti);
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ti.width = t_pass_w[k];
            ti.height = t_pass_h[k];
            ti.layer_count_or_depth = 1;
            ti.num_levels = 1;
            t_under[k] = SDL_CreateGPUTexture(gpu, &ti);
            t_under_w[k] = t_pass_w[k];
            t_under_h[k] = t_pass_h[k];
            if (t_under[k] && clear_black(cmd, t_under[k])) {
                SDL_ReleaseGPUTexture(gpu, t_under[k]);
                t_under[k] = NULL;
            }
        }
        if (t_under[k])
            under |= 1u << k;
    }
    return under;
}

/* one pass into a texture: into its target picture, at one pixel a pixel. drawn: the pictures
 * drawn for this frame before it. Returns its picture's bit, or 0 */
static unsigned draw_pass(SDL_GPUCommandBuffer *cmd, const held *h, unsigned drawn)
{
    const view_gpu_frame *f = &h->f;
    int k = f->target - 1;
    if (!pass_picture(f))
        return 0;
    Uint32 vb = f->nv * (Uint32)sizeof(view_gpu_vertex), sb = f->nst * (Uint32)sizeof(view_gpu_state);
    Uint32 volb = f->nvol_xyz * 3 * (Uint32)sizeof(float);
    Uint32 litb = f->nlit * (Uint32)sizeof(float), lsb = f->nlights * (Uint32)sizeof(float);
    Uint32 need = vb + sb + volb + litb + lsb + 256;
    if (need > xfer_p_size) {
        if (xfer_p)
            SDL_ReleaseGPUTransferBuffer(gpu, xfer_p);
        xfer_p_size = 1u << 20;
        while (xfer_p_size < need)
            xfer_p_size *= 2;
        SDL_GPUTransferBufferCreateInfo ti;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = xfer_p_size;
        xfer_p = SDL_CreateGPUTransferBuffer(gpu, &ti);
        if (!xfer_p)
            xfer_p_size = 0;
    }
    Uint8 *m = xfer_p ? SDL_MapGPUTransferBuffer(gpu, xfer_p, true) : NULL;
    if (!m || buffer_room(&bp_vert, &bp_vert_size, vb + 16, SDL_GPU_BUFFERUSAGE_VERTEX) ||
        buffer_room(&bp_state, &bp_state_size, sb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        buffer_room(&bp_vol, &bp_vol_size, volb + 16, SDL_GPU_BUFFERUSAGE_VERTEX) ||
        buffer_room(&bp_lit, &bp_lit_size, litb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        buffer_room(&bp_lights, &bp_lights_size, lsb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) {
        if (m)
            SDL_UnmapGPUTransferBuffer(gpu, xfer_p);
        return 0;
    }
    Uint32 o_s = vb, o_vol = o_s + sb, o_lit = o_vol + volb, o_ls = o_lit + litb;
    SDL_memcpy(m, f->v, vb);
    SDL_memcpy(m + o_s, f->st, sb);
    pass_states((view_gpu_state *)(void *)(m + o_s), f->nst, drawn, k);
    SDL_memcpy(m + o_vol, f->vol_xyz, volb);
    if (litb)
        SDL_memcpy(m + o_lit, f->lit, litb);
    if (lsb)
        SDL_memcpy(m + o_ls, f->lights, lsb);
    SDL_UnmapGPUTransferBuffer(gpu, xfer_p);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    const struct { SDL_GPUBuffer *b; Uint32 at, n; } parts[5] = {
        { bp_vert, 0, vb }, { bp_state, o_s, sb }, { bp_vol, o_vol, volb }, { bp_lit, o_lit, litb }, { bp_lights, o_ls, lsb } };
    for (int i = 0; i < 5; i++)
        if (parts[i].n) {
            SDL_GPUTransferBufferLocation src = { xfer_p, parts[i].at };
            SDL_GPUBufferRegion dst = { parts[i].b, 0, parts[i].n };
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        }
    SDL_EndGPUCopyPass(cp);

    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = t_pass[k];
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_zero(dt);
    dt.texture = t_pass_ds[k];
    dt.clear_depth = 0;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.clear_stencil = 0;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    float zs = f->zmax > 0 ? 1.0f / (f->zmax * 1.0001f) : 1.0f;
    float vu[4] = { (float)f->w, (float)f->h, zs, 0 };
    scene_bufs b = { bp_vert, bp_state, bp_vol, bp_lit, bp_lights, { { NULL, NULL } }, 1.0f };
    scene_pass_pictures(&b, k, 0);
    scene_fu fu;
    scene_fu_set(&fu, f, 1.0f);
    scene_background(cmd, rp, f, 1.0f);
    scene_lists(cmd, rp, f, 0, &b, &fu, vu);
    if (f->nvols && f->shadow_scale < 256)
        scene_shadows(cmd, rp, f, &b, vu);
    scene_lists(cmd, rp, f, 1, &b, &fu, vu);
    SDL_EndGPURenderPass(rp);
    stat_passes++;
    t_pass_new[k] = 0;
    return 1u << k;
}

static void draw(SDL_GPUCommandBuffer *cmd, const packet *pkt)
{
    const view_gpu_frame *f = &pkt->f;
    rt_follow();
    /* everything to the GPU: the texels that changed up to this frame (those of frames still
     * waiting stay for their turn), the triangles, the states, the volumes */
    SDL_LockMutex(lock);
    size_t up_take = pkt->up_end < up_n ? pkt->up_end : up_n;
    size_t up_bytes = up_take * 4;
    Uint32 vb = f->nv * (Uint32)sizeof(view_gpu_vertex), sb = f->nst * (Uint32)sizeof(view_gpu_state);
    Uint32 volb = f->nvol_xyz * 3 * (Uint32)sizeof(float);
    Uint32 litb = f->nlit * (Uint32)sizeof(float), lsb = f->nlights * (Uint32)sizeof(float);
    Uint32 need = (Uint32)up_bytes + vb + sb + volb + litb + lsb + 256;
    if (need > xfer_size) {
        if (xfer)
            SDL_ReleaseGPUTransferBuffer(gpu, xfer);
        xfer_size = 1u << 20;
        while (xfer_size < need)
            xfer_size *= 2;
        SDL_GPUTransferBufferCreateInfo ti;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = xfer_size;
        xfer = SDL_CreateGPUTransferBuffer(gpu, &ti);
        if (!xfer)
            xfer_size = 0;
    }
    Uint8 *m = xfer ? SDL_MapGPUTransferBuffer(gpu, xfer, true) : NULL;
    if (!m || buffer_room(&b_vert, &b_vert_size, vb + 16, SDL_GPU_BUFFERUSAGE_VERTEX) ||
        buffer_room(&b_state, &b_state_size, sb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        buffer_room(&b_vol, &b_vol_size, volb + 16, SDL_GPU_BUFFERUSAGE_VERTEX) ||
        buffer_room(&b_lit, &b_lit_size, litb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        buffer_room(&b_lights, &b_lights_size, lsb + 64, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) {
        if (m)
            SDL_UnmapGPUTransferBuffer(gpu, xfer);
        SDL_UnlockMutex(lock);
        return;
    }
    SDL_memcpy(m, up, up_bytes);
    /* where each upload's texels are in the transfer buffer, taken while the list is ours */
    static Uint32 *regs;
    static size_t regs_cap;
    size_t nregs = 0;
    for (size_t i = 0; i < up_take; i += 2 + up[i + 1]) {
        if (GROW(regs, regs_cap, nregs + 3))
            break;
        regs[nregs++] = up[i];
        regs[nregs++] = up[i + 1];
        regs[nregs++] = (Uint32)((i + 2) * 4);
        if (rt_run)
            rt_run->texels(up[i], up[i + 1], up + i + 2);
    }
    if (up_take && up_whole) {
        up_whole = 0;
        if (rt_run)
            rt_whole = 1;
    }
    SDL_memmove(up, up + up_take, (up_n - up_take) * 4);
    up_n -= up_take;
    for (int i = 0; i < pk_nwait; i++)
        pk[pk_wait[i]].up_end = pk[pk_wait[i]].up_end > up_take ? pk[pk_wait[i]].up_end - up_take : 0;
    SDL_UnlockMutex(lock);
    Uint32 o_v = (Uint32)up_bytes, o_s = o_v + vb, o_vol = o_s + sb, o_lit = o_vol + volb, o_ls = o_lit + litb;
    SDL_memcpy(m + o_v, f->v, vb);
    SDL_memcpy(m + o_s, f->st, sb);
    /* the target pictures this frame's passes into textures are drawn into below (section 78): made
     * here, where the frame's states that name one are checked against them */
    unsigned pass_bits = 0;
    for (int i = 0; i < pkt->npass; i++)
        pass_bits |= pass_picture(&pkt->pass[i].f);
    if (f->rt_mirror == 1)
        mirror_rooms(f, pass_bits);
    if (pass_states((view_gpu_state *)(void *)(m + o_s), f->nst, pass_bits, -1) && f->rt_mirror != 1)
        stat_pass_stale++;      /* (traced water: the rays draw over those polygons) */
    SDL_memcpy(m + o_vol, f->vol_xyz, volb);
    if (litb)
        SDL_memcpy(m + o_lit, f->lit, litb);
    if (lsb)
        SDL_memcpy(m + o_ls, f->lights, lsb);
    SDL_UnmapGPUTransferBuffer(gpu, xfer);

    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    for (size_t i = 0; i < nregs; i += 3) {
        Uint32 at = regs[i], n = regs[i + 1];
        if ((size_t)at + n > VIEW_GPU_ARENA)
            continue;
        SDL_GPUTransferBufferLocation src = { xfer, regs[i + 2] };
        SDL_GPUBufferRegion dst = { b_tex, at * 4, n * 4 };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    if (vb) {
        SDL_GPUTransferBufferLocation src = { xfer, o_v };
        SDL_GPUBufferRegion dst = { b_vert, 0, vb };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    if (sb) {
        SDL_GPUTransferBufferLocation src = { xfer, o_s };
        SDL_GPUBufferRegion dst = { b_state, 0, sb };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    if (volb) {
        SDL_GPUTransferBufferLocation src = { xfer, o_vol };
        SDL_GPUBufferRegion dst = { b_vol, 0, volb };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    if (litb) {
        SDL_GPUTransferBufferLocation src = { xfer, o_lit };
        SDL_GPUBufferRegion dst = { b_lit, 0, litb };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    if (lsb) {
        SDL_GPUTransferBufferLocation src = { xfer, o_ls };
        SDL_GPUBufferRegion dst = { b_lights, 0, lsb };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(cp);

    /* the passes into textures made for this frame (section 78), before anything of the frame's is drawn */
    unsigned pass_done = 0;
    for (int i = 0; i < pkt->npass; i++)
        pass_done |= draw_pass(cmd, &pkt->pass[i], pass_done);

    /* ambient occlusion: asked for, the device can, and the frame has a 3D scene whose projection is known */
    int ao = ao_want && ao_can && f->proj[0] != 0 && f->proj[2] != 0;
    Uint32 tw = (Uint32)f->w * (Uint32)scale, th = (Uint32)f->h * (Uint32)scale;
    /* Ray tracing quality: the traced picture's size. A smaller one needs the depth buffer read
     * (the fill); a device that cannot traces every pixel, with the level's rays. */
    const rt_quality *rq = &rt_levels[rt_level];
    Uint32 rdiv = rt_run ? (Uint32)rq->div : 0;
    if (rdiv > 1 && !(ds_sampled && pl_rt_fill && pl_copy_flat))
        rdiv = 1;
    Uint32 rw = rdiv ? (tw + rdiv - 1) / rdiv : 0, rh = rdiv ? (th + rdiv - 1) / rdiv : 0;
    if (targets(tw, th, ao_want && ao_can, (int)rdiv))
        return;
    /* ray tracing: the frame's scene traced now, on its own device, and its pictures uploaded */
    int rt = 0;
    if (rt_run && rt_whole && f->nrt_tris && f->proj[0] != 0 && f->proj[2] != 0) {
        const Uint32 *light = NULL, *refl = NULL;
        const float *reach = NULL;
        Uint64 t0 = SDL_GetTicksNS();
        Uint32 bytes = rw * rh * 4, parts = rdiv > 1 ? 3 : 2;
        if (xfer_rt_size < parts * bytes) {
            if (xfer_rt)
                SDL_ReleaseGPUTransferBuffer(gpu, xfer_rt);
            SDL_GPUTransferBufferCreateInfo xi;
            SDL_zero(xi);
            xi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            xi.size = parts * bytes;
            xfer_rt = SDL_CreateGPUTransferBuffer(gpu, &xi);
            xfer_rt_size = xfer_rt ? parts * bytes : 0;
        }
        Uint8 *xm = xfer_rt ? SDL_MapGPUTransferBuffer(gpu, xfer_rt, true) : NULL;
        if (rt_level != rt_said_level || (int)rw != rt_said_w || (int)rh != rt_said_h) {
            static const char *const names[3] = { "low", "medium", "high" };
            SDL_Log("gpu_render: ray tracing quality %s: %u x %u traced for a picture %u x %u%s", names[rt_level], rw, rh, tw, th,
                    rdiv != (Uint32)rq->div ? " (every pixel: this GPU's depth buffer cannot be read for less)" : "");
            rt_said_level = rt_level;
            rt_said_w = (int)rw;
            rt_said_h = (int)rh;
        }
        if (xm && rt_run->trace(f, (int)rw, (int)rh, rq, &light, &refl, rdiv > 1 ? &reach : NULL) == 0) {
            SDL_memcpy(xm, light, bytes);
            SDL_memcpy(xm + bytes, refl, bytes);
            if (reach)
                SDL_memcpy(xm + 2 * bytes, reach, bytes);
            rt = 1;
        } else if (xm && !rt_failed) {
            SDL_Log("gpu_render: ray tracing stopped: %s", SDL_GetError());
            rt_failed = 1;      /* the wish stays; this run draws without */
        }
        if (xm)
            SDL_UnmapGPUTransferBuffer(gpu, xfer_rt);
        if (rt) {
            SDL_GPUCopyPass *rcp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src = { xfer_rt, 0, rw, rh };
            SDL_GPUTextureRegion dst = { t_light, 0, 0, 0, 0, 0, rw, rh, 1 };
            SDL_UploadToGPUTexture(rcp, &src, &dst, true);
            src.offset = bytes;
            dst.texture = t_refl;
            SDL_UploadToGPUTexture(rcp, &src, &dst, true);
            if (rdiv > 1) {
                src.offset = 2 * bytes;
                dst.texture = t_reach;
                SDL_UploadToGPUTexture(rcp, &src, &dst, true);
            }
            SDL_EndGPUCopyPass(rcp);
            double ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
            stat_rt_last_ms = ms;
            if (rt_first) {
                rt_first = 0;
                stat_rt_first_ms = ms;
            } else {
                stat_rt_frames++;
                stat_rt_ms += ms;
            }
        }
    }
    if (rt_failed && rt_run) {
        rt_run->stop();
        rt_run = NULL;
    }
    /* traced water: while frames are traced through Vulkan (the only shader that mirrors), the
     * machine's thread draws no reflection pass and marks the water for the rays instead */
    int asked = SDL_GetAtomicInt(&water_asked);
    if (asked && asked != water_mode) {
        water_mode = asked;
        passes_follow();
    }
    view_host_rt_mirror(water_mode == 1 && rt && rt_run == &rt_vulkan && pass_want);
    unsigned under = f->rt_mirror == 1 ? mirror_under(cmd, f, pass_done, rt) : 0;
    if (rt)
        ao = 0;
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = t_color;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_zero(dt);
    dt.texture = t_ds;
    dt.clear_depth = 0;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = ao || rt ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    dt.stencil_store_op = ao || rt ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    dt.clear_stencil = 0;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    float zs = f->zmax > 0 ? 1.0f / (f->zmax * 1.0001f) : 1.0f;
    float vu[4] = { (float)f->w, (float)f->h, zs, 0 };

    /* 1. the background plane */
    scene_background(cmd, rp, f, (float)scale);

    /* the scene's uniforms, its buffers, and the target pictures its states may name */
    scene_fu fu;
    scene_fu_set(&fu, f, (float)scale);
    scene_bufs sb_ = { b_vert, b_state, b_vol, b_lit, b_lights, { { NULL, NULL } }, (float)scale };
    scene_pass_pictures(&sb_, -1, under);

    /* 2. opaque and punch-through; 4. translucent (after the volumes) */
    for (int phase = 0; phase < 2; phase++) {
        scene_lists(cmd, rp, f, phase, &sb_, &fu, vu);
        if (phase == 0 && f->nvols && f->shadow_scale < 256 && !rt)
            scene_shadows(cmd, rp, f, &sb_, vu);        /* 3. the opaque list's volumes, then the shadow */
        if (phase == 0 && rt) {
            /* The traced light (section 74), where the ambient occlusion would go in: into the
             * second color target the picture as it is, then, where bit 3 is set, the picture
             * times the light's ratio with the reflections; the translucent list goes on there. */
            SDL_EndGPURenderPass(rp);
            ct.texture = t_color2;
            ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.load_op = SDL_GPU_LOADOP_LOAD;
            dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
            dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPUTextureSamplerBinding five[5] = { { t_color, smp_texel }, { t_light, smp_texel }, { t_refl, smp_texel },
                                                     { t_reach, smp_texel }, { t_ds, smp_texel } };
            if (rdiv > 1) {
                /* A traced picture smaller than this one (section 76). The depth buffer is read,
                 * so it is no target of the first pass: the traced light filled in over the whole
                 * picture (rt_fill.frag). The pass that goes on then puts the picture as it was
                 * back where bit 3 is not set. */
                rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
                SDL_BindGPUGraphicsPipeline(rp, pl_rt_fill);
                SDL_BindGPUFragmentSamplers(rp, 0, five, 5);
                float fu[8] = { f->proj[0], f->proj[1], f->proj[2], f->proj[3], (float)scale, zs, (float)rw / (float)tw,
                                (float)rh / (float)th };
                SDL_PushGPUFragmentUniformData(cmd, 0, fu, sizeof fu);
                rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
                SDL_EndGPURenderPass(rp);
                ct.load_op = SDL_GPU_LOADOP_LOAD;
                rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
                SDL_BindGPUGraphicsPipeline(rp, pl_copy_flat);
                SDL_SetGPUStencilReference(rp, 0);
                SDL_BindGPUFragmentSamplers(rp, 0, five, 1);
                rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
            } else {
                rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
                SDL_BindGPUGraphicsPipeline(rp, pl_copy);
                SDL_BindGPUFragmentSamplers(rp, 0, five, 1);
                rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
                SDL_BindGPUGraphicsPipeline(rp, pl_rt_apply);
                SDL_SetGPUStencilReference(rp, 8);
                SDL_BindGPUFragmentSamplers(rp, 0, five, 3);
                rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
            }
        }
        if (phase == 0 && ao) {
            /* The ambient occlusion (section 73), between the solid lists and the translucent one.
             * The pass ends so that the depth buffer can be read: the occlusion out of it, then
             * smoothed; the pass that goes on multiplies it into the picture where bit 3 is set. */
            SDL_EndGPURenderPass(rp);
            SDL_GPUColorTargetInfo at;
            SDL_zero(at);
            at.texture = t_ao;
            at.load_op = SDL_GPU_LOADOP_DONT_CARE;
            at.store_op = SDL_GPU_STOREOP_STORE;
            rp = SDL_BeginGPURenderPass(cmd, &at, 1, NULL);
            SDL_BindGPUGraphicsPipeline(rp, pl_ao);
            SDL_GPUTextureSamplerBinding depth = { t_ds, smp_texel };
            SDL_BindGPUFragmentSamplers(rp, 0, &depth, 1);
            float au[8] = { f->proj[0], f->proj[1], f->proj[2], f->proj[3], (float)scale, zs, AO_RADIUS, AO_STRENGTH };
            SDL_PushGPUFragmentUniformData(cmd, 0, au, sizeof au);
            rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
            SDL_EndGPURenderPass(rp);
            at.texture = t_ao_smooth;
            rp = SDL_BeginGPURenderPass(cmd, &at, 1, NULL);
            SDL_BindGPUGraphicsPipeline(rp, pl_ao_blur);
            SDL_GPUTextureSamplerBinding two[2] = { { t_ao, smp_texel }, { t_ds, smp_texel } };
            SDL_BindGPUFragmentSamplers(rp, 0, two, 2);
            rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
            SDL_EndGPURenderPass(rp);
            ct.load_op = SDL_GPU_LOADOP_LOAD;
            dt.load_op = SDL_GPU_LOADOP_LOAD;
            dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
            dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
            SDL_BindGPUGraphicsPipeline(rp, pl_ao_apply);
            SDL_SetGPUStencilReference(rp, 8);
            SDL_GPUTextureSamplerBinding smooth = { t_ao_smooth, smp_texel };
            SDL_BindGPUFragmentSamplers(rp, 0, &smooth, 1);
            rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);
        }
    }
    SDL_EndGPURenderPass(rp);
    t_final = rt ? t_color2 : t_color;
    have_picture = 1;
    last_frame = f->frame;
    stat_frames++;
    if (f->nv / 3 > stat_max_tris)
        stat_max_tris = f->nv / 3;
    if (f->translucent_volumes)
        stat_tvol_frames++;
}

SDL_GPUTexture *gpu_render_frame(SDL_GPUCommandBuffer *cmd, Uint32 *w, Uint32 *h, unsigned *frame, unsigned upto)
{
    SDL_LockMutex(lock);
    int r = -1, got = -1;
    for (int i = 0; i < pk_nwait; i++)
        if (pk[pk_wait[i]].f.frame <= upto)
            got = i;
    if (got >= 0) {
        stat_replaced += (unsigned)got;         /* the ones before it are passed over */
        r = pk_reading = pk_wait[got];
        for (int i = got + 1; i < pk_nwait; i++)
            pk_wait[i - got - 1] = pk_wait[i];
        pk_nwait -= got + 1;
    }
    SDL_UnlockMutex(lock);
    if (r >= 0) {
        draw(cmd, &pk[r]);
        SDL_LockMutex(lock);
        pk_reading = -1;
        SDL_UnlockMutex(lock);
    }
    *w = t_w;
    *h = t_h;
    *frame = last_frame;
    return have_picture ? t_final : NULL;
}

void gpu_render_counts(unsigned *submitted, unsigned *replaced)
{
    SDL_LockMutex(lock);
    *submitted = stat_submitted;
    *replaced = stat_replaced;
    SDL_UnlockMutex(lock);
}

void gpu_render_stats(unsigned *frames, unsigned *max_tris, unsigned *tvol)
{
    *frames = stat_frames;
    *max_tris = stat_max_tris;
    *tvol = stat_tvol_frames;
}

void gpu_render_quit(void)
{
    if (!gpu)
        return;
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 2; c++)
                for (int d = 0; d < 64; d++)
                    if (pl_scene[a][b][c][d])
                        SDL_ReleaseGPUGraphicsPipeline(gpu, pl_scene[a][b][c][d]);
    SDL_GPUGraphicsPipeline *pls[] = { pl_bg, pl_vol, pl_resolve, pl_shadow, pl_ao, pl_ao_blur, pl_ao_apply, pl_copy, pl_rt_apply,
                                       pl_rt_fill, pl_copy_flat };
    for (int i = 0; i < 11; i++)
        if (pls[i])
            SDL_ReleaseGPUGraphicsPipeline(gpu, pls[i]);
    SDL_GPUShader *shs[] = { sh_scene_v, sh_scene_f, sh_vol_v, sh_rect_v, sh_bg_f, sh_const_f, sh_ao_f, sh_ao_blur_f,
                             sh_ao_apply_f, sh_copy_f, sh_rt_apply_f, sh_rt_fill_f };
    for (int i = 0; i < 12; i++)
        if (shs[i])
            SDL_ReleaseGPUShader(gpu, shs[i]);
    SDL_GPUBuffer *bs[] = { b_tex, b_vert, b_state, b_vol, b_lit, b_lights };
    for (int i = 0; i < 6; i++)         /* was 4: the two of lighting on the GPU were never released */
        if (bs[i])
            SDL_ReleaseGPUBuffer(gpu, bs[i]);
    if (xfer)
        SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    if (t_color)
        SDL_ReleaseGPUTexture(gpu, t_color);
    if (t_ds)
        SDL_ReleaseGPUTexture(gpu, t_ds);
    if (t_ao)
        SDL_ReleaseGPUTexture(gpu, t_ao);
    if (t_ao_smooth)
        SDL_ReleaseGPUTexture(gpu, t_ao_smooth);
    if (smp_texel)
        SDL_ReleaseGPUSampler(gpu, smp_texel);
    /* the passes into textures: the machine's thread draws them itself until a renderer is there again */
    view_host_gpu_passes(0);
    view_host_rt_mirror(0);
    for (int k = 0; k < VIEW_GPU_TARGETS; k++) {
        if (t_pass[k])
            SDL_ReleaseGPUTexture(gpu, t_pass[k]);
        if (t_pass_ds[k])
            SDL_ReleaseGPUTexture(gpu, t_pass_ds[k]);
        t_pass[k] = t_pass_ds[k] = NULL;
        t_pass_w[k] = t_pass_h[k] = 0;
        if (t_under[k])
            SDL_ReleaseGPUTexture(gpu, t_under[k]);
        t_under[k] = NULL;
        t_under_w[k] = t_under_h[k] = 0;
    }
    if (t_none)
        SDL_ReleaseGPUTexture(gpu, t_none);
    t_none = NULL;
    SDL_GPUBuffer *pbs[] = { bp_vert, bp_state, bp_vol, bp_lit, bp_lights };
    for (int i = 0; i < 5; i++)
        if (pbs[i])
            SDL_ReleaseGPUBuffer(gpu, pbs[i]);
    bp_vert = bp_state = bp_vol = bp_lit = bp_lights = NULL;
    bp_vert_size = bp_state_size = bp_vol_size = bp_lit_size = bp_lights_size = 0;
    if (xfer_p)
        SDL_ReleaseGPUTransferBuffer(gpu, xfer_p);
    xfer_p = NULL;
    xfer_p_size = 0;
    pl_ao = pl_ao_blur = pl_ao_apply = NULL;
    sh_ao_f = sh_ao_blur_f = sh_ao_apply_f = NULL;
    t_ao = t_ao_smooth = NULL;
    smp_texel = NULL;
    ao_can = t_ao_made = 0;
    /* the ray tracing's device goes with the renderer's (the wish stays: the next start takes it up) */
    if (rt_run)
        rt_run->stop();
    rt_run = NULL;
    rt_failed = rt_whole = t_rt_made = ds_sampled = 0;
    rt_said_level = -1;
    SDL_GPUTexture *more[] = { t_color2, t_light, t_refl, t_reach };
    for (int i = 0; i < 4; i++)
        if (more[i])
            SDL_ReleaseGPUTexture(gpu, more[i]);
    t_color2 = t_light = t_refl = t_reach = t_final = NULL;
    if (xfer_rt)
        SDL_ReleaseGPUTransferBuffer(gpu, xfer_rt);
    xfer_rt = NULL;
    xfer_rt_size = 0;
    pl_copy = pl_rt_apply = pl_rt_fill = pl_copy_flat = NULL;
    sh_copy_f = sh_rt_apply_f = sh_rt_fill_f = NULL;
    /* all of it gone, for a start again on this device or another (the
     * lock, the frames handed over and the counts stay) */
    SDL_zeroa(pl_scene);
    pl_bg = pl_vol = pl_resolve = pl_shadow = NULL;
    sh_scene_v = sh_scene_f = sh_vol_v = sh_rect_v = sh_bg_f = sh_const_f = NULL;
    b_tex = b_vert = b_state = b_vol = b_lit = b_lights = NULL;
    b_vert_size = b_state_size = b_vol_size = b_lit_size = b_lights_size = 0;
    xfer = NULL;
    xfer_size = 0;
    t_color = t_ds = NULL;
    t_w = t_h = 0;
    have_picture = 0;
    gpu = NULL;
}
