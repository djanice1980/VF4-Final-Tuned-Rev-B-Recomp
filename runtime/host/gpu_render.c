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
 * the chips: its 2D layer keeps its colours).
 */
#include "gpu_render.h"
#include "view.h"
#include "gpu_shaders.h"

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
    Uint64 pts;         /* when it is to be shown, by the game's clock (0: as it comes) */
    size_t up_end;      /* the texel uploads up to here are this frame's and those before it */
} packet;

/* made once and kept: the machine's thread may hand a frame over while the
 * window's thread stops the renderer or starts it again (docs/design.md
 * section 53) */
static SDL_Mutex *lock;
static packet pk[4];
static int pk_wait[2], pk_nwait;        /* the packets waiting to be drawn, oldest first (2 at most) */
static int pk_reading = -1;             /* the one the window draws */
static unsigned stat_submitted, stat_replaced;  /* frames from the machine; those replaced before they were drawn */
/* texel uploads waiting: [at, n, n texels] after one another */
static Uint32 *up;
static size_t up_n, up_cap;
/* the renderer has just started (again): the next frame sends the whole
 * arena, as the device's copy of it is new and empty */
static int up_all;
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
    stat_submitted++;
    int w = 0;
    while (w == pk_reading || (pk_nwait > 0 && w == pk_wait[0]) || (pk_nwait > 1 && w == pk_wait[1]))
        w++;
    packet *p = &pk[w];
    if (GROW(p->v, p->v_cap, f->nv) || GROW(p->st, p->st_cap, f->nst) || GROW(p->d, p->d_cap, f->ndraws) ||
        GROW(p->vol, p->vol_cap, (size_t)f->nvol_xyz * 3) || GROW(p->vols, p->vols_cap, f->nvols) ||
        GROW(p->lit, p->lit_cap, (size_t)f->nlit + 1) || GROW(p->lights, p->lights_cap, (size_t)f->nlights + 1)) {
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
    case 2: return src ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_SRC_COLOR;   /* the other's colour */
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
    if (!p->occlusion_target) {
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
    SDL_UnlockMutex(lock);
    if (!(SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_SPIRV))
        return SDL_SetError("the GPU device takes no SPIR-V (it is not Vulkan)"), -1;
    ds_format = SDL_GPUTextureSupportsFormat(gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                    ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    sh_scene_v = shader(spv_scene_vert, sizeof spv_scene_vert, SDL_GPU_SHADERSTAGE_VERTEX, 2, 1, 0);
    sh_scene_f = shader(spv_scene_frag, sizeof spv_scene_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, 0);
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
    /* where bits 0 and 2 are set: the colour times the scale */
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
    ao_can = SDL_GPUTextureSupportsFormat(gpu, ds_format, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
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
            /* where bit 3 is set: the colour times the occlusion */
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
    SDL_GPUBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = ARENA_BYTES;
    b_tex = SDL_CreateGPUBuffer(gpu, &bi);
    if (!b_tex)
        return -1;
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

/* ao: the depth buffer is one a shader can read, and the occlusion's two targets are there */
static int targets(Uint32 w, Uint32 h, int ao)
{
    if (t_color && w == t_w && h == t_h && ao == t_ao_made)
        return 0;
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
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | (ao ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
    t_ds = SDL_CreateGPUTexture(gpu, &ti);
    if (ao) {
        ti.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        t_ao = SDL_CreateGPUTexture(gpu, &ti);
        t_ao_smooth = SDL_CreateGPUTexture(gpu, &ti);
    }
    t_w = w;
    t_h = h;
    t_ao_made = ao;
    return t_color && t_ds && (!ao || (t_ao && t_ao_smooth)) ? 0 : -1;
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

static void draw(SDL_GPUCommandBuffer *cmd, const packet *pkt)
{
    const view_gpu_frame *f = &pkt->f;
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
    }
    SDL_memmove(up, up + up_take, (up_n - up_take) * 4);
    up_n -= up_take;
    for (int i = 0; i < pk_nwait; i++)
        pk[pk_wait[i]].up_end = pk[pk_wait[i]].up_end > up_take ? pk[pk_wait[i]].up_end - up_take : 0;
    SDL_UnlockMutex(lock);
    Uint32 o_v = (Uint32)up_bytes, o_s = o_v + vb, o_vol = o_s + sb, o_lit = o_vol + volb, o_ls = o_lit + litb;
    SDL_memcpy(m + o_v, f->v, vb);
    SDL_memcpy(m + o_s, f->st, sb);
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

    /* ambient occlusion: asked for, the device can, and the frame has a 3D scene whose projection is known */
    int ao = ao_want && ao_can && f->proj[0] != 0 && f->proj[2] != 0;
    if (targets((Uint32)f->w * (Uint32)scale, (Uint32)f->h * (Uint32)scale, ao_want && ao_can))
        return;
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
    dt.store_op = ao ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    dt.stencil_store_op = ao ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    dt.clear_stencil = 0;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    float zs = f->zmax > 0 ? 1.0f / (f->zmax * 1.0001f) : 1.0f;
    float vu[4] = { (float)f->w, (float)f->h, zs, 0 };

    /* 1. the background plane */
    SDL_BindGPUGraphicsPipeline(rp, pl_bg);
    float bg[16] = { f->bg_c0[0], f->bg_c0[1], f->bg_c0[2], (float)scale, f->bg_ga[0], f->bg_ga[1], f->bg_ga[2], f->bg_x0,
                     f->bg_gb[0], f->bg_gb[1], f->bg_gb[2], f->bg_y0, (float)(f->fog_col >> 16 & 255),
                     (float)(f->fog_col >> 8 & 255), (float)(f->fog_col & 255), (float)f->bg_fog };
    SDL_PushGPUFragmentUniformData(cmd, 0, bg, sizeof bg);
    rect(cmd, rp, 0, 0, (float)f->w, (float)f->h, f);

    /* the scene's uniforms */
    struct {
        float p0[4];
        Uint32 p1[4];
        Uint32 fog[132];
    } fu;
    SDL_zero(fu);
    fu.p0[0] = (float)scale;
    fu.p0[1] = (float)f->alpha_ref;
    fu.p0[2] = f->fog_density;
    fu.p1[0] = (Uint32)f->fog_on;
    fu.p1[1] = f->fog_col;
    fu.p1[2] = f->fog_col_vert;
    SDL_memcpy(fu.fog, f->fog_tab, sizeof f->fog_tab);
    SDL_GPUBuffer *storage[2] = { b_state, b_tex };
    SDL_GPUBuffer *vstorage[2] = { b_lit, b_lights };   /* the vertex shader's: lighting on the GPU */
    SDL_GPUBufferBinding vbind = { b_vert, 0 };
    int list_pushed = -1;
    SDL_GPUGraphicsPipeline *bound = NULL;

    /* 2. opaque and punch-through; 4. translucent (after the volumes) */
    for (int phase = 0; phase < 2; phase++) {
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
                SDL_BindGPUFragmentStorageBuffers(rp, 0, storage, 2);
                SDL_BindGPUVertexStorageBuffers(rp, 0, vstorage, 2);
                SDL_PushGPUVertexUniformData(cmd, 0, vu, sizeof vu);
                bound = pl;
            }
            if ((int)d->list != list_pushed) {
                fu.p0[3] = (float)d->list;
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof fu);
                list_pushed = (int)d->list;
            }
            if (d->list != 2)
                SDL_SetGPUStencilReference(rp, (Uint8)((d->shadow ? 1 : 0) | (d->solid ? 8 : 0)));
            SDL_DrawGPUPrimitives(rp, d->count, 1, d->first, 0);
        }
        if (phase == 0 && f->nvols && f->shadow_scale < 256) {
            /* 3. the opaque list's volumes, then the shadow */
            SDL_GPUBufferBinding volbind = { b_vol, 0 };
            float zero[4] = { 0, 0, 0, 0 };
            for (Uint32 n = 0; n < f->nvols; n++) {
                const view_gpu_volume *v = &f->vols[n];
                if (!v->count)
                    continue;
                SDL_BindGPUGraphicsPipeline(rp, pl_vol);
                SDL_BindGPUVertexBuffers(rp, 0, &volbind, 1);
                SDL_PushGPUVertexUniformData(cmd, 0, vu, sizeof vu);
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
            bound = NULL;
            list_pushed = -1;
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
            bound = NULL;
            list_pushed = -1;
        }
    }
    SDL_EndGPURenderPass(rp);
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
    return have_picture ? t_color : NULL;
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
    SDL_GPUGraphicsPipeline *pls[] = { pl_bg, pl_vol, pl_resolve, pl_shadow, pl_ao, pl_ao_blur, pl_ao_apply };
    for (int i = 0; i < 7; i++)
        if (pls[i])
            SDL_ReleaseGPUGraphicsPipeline(gpu, pls[i]);
    SDL_GPUShader *shs[] = { sh_scene_v, sh_scene_f, sh_vol_v, sh_rect_v, sh_bg_f, sh_const_f, sh_ao_f, sh_ao_blur_f,
                             sh_ao_apply_f };
    for (int i = 0; i < 9; i++)
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
    pl_ao = pl_ao_blur = pl_ao_apply = NULL;
    sh_ao_f = sh_ao_blur_f = sh_ao_apply_f = NULL;
    t_ao = t_ao_smooth = NULL;
    smp_texel = NULL;
    ao_can = t_ao_made = 0;
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
