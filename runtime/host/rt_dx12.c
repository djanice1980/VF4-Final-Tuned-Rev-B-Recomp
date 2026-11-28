/* Ray traced light on DirectX 12: see rt.h and docs/design.md section 75. Windows only (the
 * Windows game on Windows, or under Proton, where DirectX 12 is vkd3d-proton's).
 *
 * rt_vk.c's twin. A D3D12 device of its own on the GPU the renderer draws with, with inline ray
 * queries (ray tracing tier 1.1). A frame is one command list: the scene's buffers copied in,
 * four bottom level structures and the top level one built anew, the tracing shader
 * (shaders/rt_light.hlsl) run over the picture, its two results smoothed
 * (shaders/rt_filter.hlsl) and copied where the processor can read them. The call waits for it.
 *
 * Every buffer the shaders read or write is a root descriptor, so there are no descriptor
 * heaps. The buffers in the GPU's own memory are made in the common state and left to
 * Direct3D's rule for buffers: the first use in a command list promotes one, a barrier takes it
 * on within the list, and after the list has run it is in the common state again. */
#include "rt.h"
#include "dx_shaders.h"
#include "view.h"

#define COBJMACROS
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <string.h>

static ID3D12Device5 *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList4 *list;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_value;
static ID3D12RootSignature *rs_trace, *rs_filter;
static ID3D12PipelineState *pso_trace, *pso_filter;

typedef struct dbuf {
    ID3D12Resource *r;
    UINT64 size;
    void *map;
} dbuf;

/* the frame's data: for each kind a staging buffer the processor writes and the GPU's own copy */
enum { B_POS, B_TRI, B_STRIP, B_STATE, B_LIGHTS, B_INST, B_KINDS };
static dbuf stage[B_KINDS], gpu[B_KINDS];
static dbuf arena_stage, arena_gpu;     /* the texel arena: all of it mapped; the ranges that changed are copied */
static struct { Uint32 at, n; } *dirty;
static size_t dirty_n, dirty_cap;
static dbuf consts;                     /* the shaders' constants, mapped */
static dbuf raw_light, raw_refl, raw_depth;     /* the two results before the filter, and the eye rays' lengths */
static dbuf out_gpu[2], out_back[3];    /* the two results, and where the processor reads them (and the eye rays' lengths) */
static dbuf as_store[5], as_scratch[5]; /* the acceleration structures (4 groups and the top), and the builds' memory */
/* The buffers a frame's command list took out of the common state by a barrier, and the state
 * each is in: the list's end puts them back (settle), so that every list finds every buffer in
 * the common state whatever Direct3D's rule for buffers after a list does on its own. */
static struct { dbuf *b; D3D12_RESOURCE_STATES s; } moved[16];
static int nmoved;

static int fail(const char *what, HRESULT hr)
{
    SDL_SetError("ray tracing (DirectX 12): %s failed (%08lx)", what, (unsigned long)hr);
    return -1;
}

static void dbuf_free(dbuf *b)
{
    if (b->r)
        ID3D12Resource_Release(b->r);
    SDL_zerop(b);
}

/* a buffer of at least size bytes (kept when it is large enough). heap: the GPU's own memory
 * (default), the processor's to write (upload: mapped, never read from) or to read (readback:
 * mapped) */
static int dbuf_room(dbuf *b, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    if (b->r && b->size >= size)
        return 0;
    dbuf_free(b);
    UINT64 s = 65536;
    while (s < size)
        s *= 2;
    D3D12_HEAP_PROPERTIES hp;
    SDL_zero(hp);
    hp.Type = heap;
    D3D12_RESOURCE_DESC d;
    SDL_zero(d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = s;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    HRESULT hr = ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&b->r);
    if (FAILED(hr)) {
        b->r = NULL;
        return fail("making a buffer", hr);
    }
    if (heap != D3D12_HEAP_TYPE_DEFAULT && FAILED(hr = ID3D12Resource_Map(b->r, 0, NULL, &b->map)))
        return fail("mapping a buffer", hr);
    b->size = s;
    return 0;
}

#define VA(b) ID3D12Resource_GetGPUVirtualAddress((b).r)

static void rt_dx_stop(void)
{
    /* every frame is waited for; one that was given up on (a GPU that hung) gets a little longer
     * before its buffers go */
    if (fence && fence_event && ID3D12Fence_GetCompletedValue(fence) < fence_value &&
        SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event)))
        WaitForSingleObject(fence_event, 5000);
    nmoved = 0;
    if (dev) {
        dbuf *all[] = { &arena_stage, &arena_gpu, &consts, &raw_light, &raw_refl, &raw_depth, &out_gpu[0], &out_gpu[1],
                        &out_back[0], &out_back[1], &out_back[2] };
        for (size_t i = 0; i < SDL_arraysize(all); i++)
            dbuf_free(all[i]);
        for (int i = 0; i < B_KINDS; i++) {
            dbuf_free(&stage[i]);
            dbuf_free(&gpu[i]);
        }
        for (int i = 0; i < 5; i++) {
            dbuf_free(&as_store[i]);
            dbuf_free(&as_scratch[i]);
        }
    }
    if (pso_trace) ID3D12PipelineState_Release(pso_trace);
    if (pso_filter) ID3D12PipelineState_Release(pso_filter);
    if (rs_trace) ID3D12RootSignature_Release(rs_trace);
    if (rs_filter) ID3D12RootSignature_Release(rs_filter);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList4_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device5_Release(dev);
    if (fence_event) CloseHandle(fence_event);
    pso_trace = pso_filter = NULL;
    rs_trace = rs_filter = NULL;
    fence = NULL;
    list = NULL;
    alloc = NULL;
    queue = NULL;
    dev = NULL;
    fence_event = NULL;
    dirty_n = 0;
}

/* a root signature of root descriptors: the constants (b0), nsrv buffers to read (t0 on), nuav to write (u0 on) */
static ID3D12RootSignature *root_signature(int nsrv, int nuav)
{
    D3D12_ROOT_PARAMETER rp[12];
    SDL_zeroa(rp);
    int n = 1 + nsrv + nuav;
    for (int i = 0; i < n; i++) {
        rp[i].ParameterType = i == 0 ? D3D12_ROOT_PARAMETER_TYPE_CBV : i <= nsrv ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[i].Descriptor.ShaderRegister = i == 0 ? 0 : i <= nsrv ? (UINT)(i - 1) : (UINT)(i - 1 - nsrv);
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC d;
    SDL_zero(d);
    d.NumParameters = (UINT)n;
    d.pParameters = rp;
    ID3D10Blob *blob = NULL, *err = NULL;
    ID3D12RootSignature *rs = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (SUCCEEDED(hr))
        hr = ID3D12Device5_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                               &IID_ID3D12RootSignature, (void **)&rs);
    if (blob) ID3D10Blob_Release(blob);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) {
        fail("a root signature", hr);
        return NULL;
    }
    return rs;
}

static ID3D12PipelineState *compute(ID3D12RootSignature *rs, const unsigned char *code, size_t n)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d;
    SDL_zero(d);
    d.pRootSignature = rs;
    d.CS.pShaderBytecode = code;
    d.CS.BytecodeLength = n;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device5_CreateComputePipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr)) {
        fail("a shader", hr);
        return NULL;
    }
    return pso;
}

static int rt_dx_start(const char *gpu_name)
{
    if (dev)
        return 0;
    /* The GPU the renderer draws with, by its name: DXGI's description is as a rule the name
     * Vulkan gives it, on Windows and under Proton. So the GPUs are tried in this order: the one
     * of that very name, those whose name holds the other's or is held by it, then the rest; and
     * the first with ray queries is taken (the traced light comes back through the processor, so
     * another GPU than the renderer's gives the same picture). */
    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr))
        return fail("CreateDXGIFactory1", hr);
    enum { MAXA = 8 };
    IDXGIAdapter1 *ad[MAXA], *a = NULL;
    char names[MAXA][256];
    int rank[MAXA], na = 0;
    for (UINT i = 0; na < MAXA && IDXGIFactory1_EnumAdapters1(factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(a, &desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            IDXGIAdapter1_Release(a);
            continue;
        }
        names[na][0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, names[na], sizeof names[na], NULL, NULL);
        names[na][sizeof names[na] - 1] = 0;
        rank[na] = !gpu_name || !names[na][0] ? 2 : !strcmp(names[na], gpu_name) ? 0
                   : strstr(names[na], gpu_name) || strstr(gpu_name, names[na]) ? 1 : 2;
        ad[na++] = a;
    }
    IDXGIFactory1_Release(factory);
    char why[256] = "no GPU";
    for (int r = 0; r < 3 && !dev; r++)
        for (int i = 0; i < na && !dev; i++) {
            if (rank[i] != r)
                continue;
            hr = D3D12CreateDevice((IUnknown *)ad[i], D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev);
            if (FAILED(hr)) {
                dev = NULL;
                SDL_snprintf(why, sizeof why, "%s: no DirectX 12 with ray tracing (%08lx)", names[i], (unsigned long)hr);
                continue;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
            SDL_zero(o5);
            ID3D12Device5_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
            if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
                SDL_snprintf(why, sizeof why, "%s or its driver has no ray queries (ray tracing tier %d.%d)", names[i],
                             (int)o5.RaytracingTier / 10, (int)o5.RaytracingTier % 10);
                ID3D12Device5_Release(dev);
                dev = NULL;
                continue;
            }
            if (r)      /* not the renderer's by its name: said, since it may be another GPU */
                SDL_Log("ray tracing (DirectX 12): traced on %s (DirectX lists no GPU named as the renderer's, %s)", names[i],
                        gpu_name ? gpu_name : "which has no name");
        }
    for (int i = 0; i < na; i++)
        IDXGIAdapter1_Release(ad[i]);
    if (!dev)
        return SDL_SetError("ray tracing (DirectX 12): %s", why), -1;
    D3D12_COMMAND_QUEUE_DESC qd;
    SDL_zero(qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)) ||
        FAILED(hr = ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc)) ||
        FAILED(hr = ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList4,
                                                    (void **)&list)) ||
        FAILED(hr = ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence))) {
        rt_dx_stop();
        return fail("setting up", hr);
    }
    fence_value = 0;
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!fence_event) {
        rt_dx_stop();
        return SDL_SetError("ray tracing (DirectX 12): no event to wait on"), -1;
    }
    /* the tracing shader: the constants; the top level structure and 5 buffers to read; 3 to write.
     * The filter: the constants; 3 to read; 2 to write */
    if (!(rs_trace = root_signature(6, 3)) || !(rs_filter = root_signature(3, 2)) ||
        !(pso_trace = compute(rs_trace, dxil_rt_light_hlsl, sizeof dxil_rt_light_hlsl)) ||
        !(pso_filter = compute(rs_filter, dxil_rt_filter_hlsl, sizeof dxil_rt_filter_hlsl)) ||
        dbuf_room(&arena_stage, (UINT64)VIEW_GPU_ARENA * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, 0) ||
        dbuf_room(&arena_gpu, (UINT64)VIEW_GPU_ARENA * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON) ||
        dbuf_room(&consts, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, 0)) {
        rt_dx_stop();
        return -1;
    }
    return 0;
}

static void rt_dx_texels(Uint32 at, Uint32 n, const Uint32 *data)
{
    if (!dev || (size_t)at + n > VIEW_GPU_ARENA || !n)
        return;
    memcpy((Uint32 *)arena_stage.map + at, data, (size_t)n * 4);
    if (dirty_n == dirty_cap) {
        size_t c = dirty_cap ? dirty_cap * 2 : 64;
        void *p = SDL_realloc(dirty, c * sizeof *dirty);
        if (!p)
            return;
        dirty = p;
        dirty_cap = c;
    }
    dirty[dirty_n].at = at;
    dirty[dirty_n++].n = n;
}

static void barrier(dbuf *b, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER rb;
    SDL_zero(rb);
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = b->r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = before;
    rb.Transition.StateAfter = after;
    ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &rb);
}

/* the buffer taken from the state its first use in the list gave it (or a barrier before) to another */
static void transition(dbuf *b, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    barrier(b, before, after);
    int i = 0;
    while (i < nmoved && moved[i].b != b)
        i++;
    if (i == (int)SDL_arraysize(moved))
        return;
    moved[i].b = b;
    moved[i].s = after;
    if (i == nmoved)
        nmoved++;
}

static void settle(void)
{
    for (int i = 0; i < nmoved; i++)
        barrier(moved[i].b, moved[i].s, D3D12_RESOURCE_STATE_COMMON);
    nmoved = 0;
}

/* what was written into the buffer (as an acceleration structure, or by a shader) is done before it is read */
static void written(dbuf *b)
{
    D3D12_RESOURCE_BARRIER rb;
    SDL_zero(rb);
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    rb.UAV.pResource = b->r;
    ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &rb);
}

/* an acceleration structure of the frame in slot i, recorded */
static int build(int i, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *in)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre;
    SDL_zero(pre);
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, in, &pre);
    if (!pre.ResultDataMaxSizeInBytes)
        return SDL_SetError("ray tracing (DirectX 12): an acceleration structure cannot be built"), -1;
    if (dbuf_room(&as_store[i], pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) ||
        dbuf_room(&as_scratch[i], pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COMMON))
        return -1;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd;
    SDL_zero(bd);
    bd.Inputs = *in;
    bd.DestAccelerationStructureData = VA(as_store[i]);
    bd.ScratchAccelerationStructureData = VA(as_scratch[i]);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list, &bd, 0, NULL);
    written(&as_store[i]);
    return 0;
}

static int rt_dx_trace(const view_gpu_frame *f, int w, int h, const rt_quality *q, const Uint32 **light, const Uint32 **refl,
                       const float **reach)
{
    if (!dev)
        return SDL_SetError("ray tracing (DirectX 12): not started"), -1;
    rt_scene sc;
    rt_scene_count(f, &sc);
    if (!sc.ntri)
        return 1;       /* nothing for the rays in this frame (rt.h): no fault */
    nmoved = 0;
    size_t bytes[B_KINDS] = { (size_t)sc.ntri * 36, (size_t)sc.ntri * sizeof(rt_tri), (size_t)f->nrt_strips * sizeof *f->rt_strips,
                              (size_t)f->nst * sizeof *f->st, (size_t)f->nlights * sizeof(float),
                              4 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC) };
    for (int k = 0; k < B_KINDS; k++)
        if (dbuf_room(&stage[k], bytes[k] + 64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, 0) ||
            dbuf_room(&gpu[k], bytes[k] + 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON))
            return -1;
    UINT64 pixels = (UINT64)w * (UINT64)h * 4;
    dbuf *uav[5] = { &raw_light, &raw_refl, &raw_depth, &out_gpu[0], &out_gpu[1] };
    for (int i = 0; i < 5; i++)
        if (dbuf_room(uav[i], pixels, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON))
            return -1;
    for (int i = 0; i < (reach ? 3 : 2); i++)
        if (dbuf_room(&out_back[i], pixels, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, 0))
            return -1;
    rt_scene_fill(f, &sc, stage[B_TRI].map, stage[B_POS].map, w, h, VIEW_GPU_ARENA, q);
    memcpy(stage[B_STRIP].map, f->rt_strips, bytes[B_STRIP]);
    memcpy(stage[B_STATE].map, f->st, bytes[B_STATE]);
    memcpy(stage[B_LIGHTS].map, f->lights, bytes[B_LIGHTS]);
    memcpy(consts.map, &sc.consts, sizeof sc.consts);

    /* the frame's data and the texels that changed */
    for (int k = 0; k < B_KINDS - 1; k++)
        if (bytes[k]) {
            ID3D12GraphicsCommandList4_CopyBufferRegion(list, gpu[k].r, 0, stage[k].r, 0, bytes[k]);
            transition(&gpu[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    for (size_t i = 0; i < dirty_n; i++)
        ID3D12GraphicsCommandList4_CopyBufferRegion(list, arena_gpu.r, (UINT64)dirty[i].at * 4, arena_stage.r, (UINT64)dirty[i].at * 4,
                                                    (UINT64)dirty[i].n * 4);
    if (dirty_n)
        transition(&arena_gpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dirty_n = 0;
    /* the four groups' structures, and the one over them */
    D3D12_RAYTRACING_INSTANCE_DESC *insts = stage[B_INST].map, one;
    UINT ninst = 0;
    for (int g = 0; g < 4; g++) {
        if (!sc.count[g])
            continue;
        D3D12_RAYTRACING_GEOMETRY_DESC geo;
        SDL_zero(geo);
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo.Flags = g & 1 ? D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geo.Triangles.VertexCount = sc.count[g] * 3;
        geo.Triangles.VertexBuffer.StartAddress = VA(gpu[B_POS]) + (UINT64)sc.base[g] * 36;
        geo.Triangles.VertexBuffer.StrideInBytes = 12;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
        SDL_zero(in);
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &geo;
        if (build(g, &in))
            return -1;
        SDL_zero(one);
        one.Transform[0][0] = one.Transform[1][1] = one.Transform[2][2] = 1;
        one.InstanceID = (UINT)g;
        one.InstanceMask = g < 2 ? 1 : 2;
        one.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        one.AccelerationStructure = VA(as_store[g]);
        insts[ninst++] = one;       /* written whole: the staging buffer is not read */
    }
    ID3D12GraphicsCommandList4_CopyBufferRegion(list, gpu[B_INST].r, 0, stage[B_INST].r, 0, (UINT64)ninst * sizeof one);
    transition(&gpu[B_INST], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin;
    SDL_zero(tin);
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tin.NumDescs = ninst;
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tin.InstanceDescs = VA(gpu[B_INST]);
    if (build(4, &tin))
        return -1;
    /* the tracing shader over the picture */
    ID3D12GraphicsCommandList4_SetComputeRootSignature(list, rs_trace);
    ID3D12GraphicsCommandList4_SetPipelineState(list, pso_trace);
    ID3D12GraphicsCommandList4_SetComputeRootConstantBufferView(list, 0, VA(consts));
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(list, 1, VA(as_store[4]));
    dbuf *srv[5] = { &gpu[B_TRI], &gpu[B_STRIP], &gpu[B_STATE], &gpu[B_LIGHTS], &arena_gpu };
    for (int i = 0; i < 5; i++)
        ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(list, (UINT)(2 + i), VA(*srv[i]));
    for (int i = 0; i < 3; i++)
        ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(list, (UINT)(7 + i), VA(*uav[i]));
    ID3D12GraphicsCommandList4_Dispatch(list, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
    for (int i = 0; i < 3; i++)
        transition(uav[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    /* the two smoothed */
    ID3D12GraphicsCommandList4_SetComputeRootSignature(list, rs_filter);
    ID3D12GraphicsCommandList4_SetPipelineState(list, pso_filter);
    ID3D12GraphicsCommandList4_SetComputeRootConstantBufferView(list, 0, VA(consts));
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(list, 1, VA(raw_light));
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(list, 2, VA(raw_depth));
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(list, 3, VA(raw_refl));
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(list, 4, VA(out_gpu[0]));
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(list, 5, VA(out_gpu[1]));
    ID3D12GraphicsCommandList4_Dispatch(list, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
    for (int i = 0; i < 2; i++) {
        transition(&out_gpu[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyBufferRegion(list, out_back[i].r, 0, out_gpu[i].r, 0, pixels);
    }
    if (reach) {
        transition(&raw_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyBufferRegion(list, out_back[2].r, 0, raw_depth.r, 0, pixels);
    }
    settle();
    /* run, and wait */
    HRESULT hr = ID3D12GraphicsCommandList4_Close(list);
    if (FAILED(hr))
        return fail("closing the command list", hr);
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, l);
    if (FAILED(hr = ID3D12CommandQueue_Signal(queue, fence, ++fence_value)) ||
        FAILED(hr = ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event)))
        return fail("running the frame", hr);
    if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0)
        return SDL_SetError("ray tracing (DirectX 12): the GPU did not finish the frame"), -1;
    if (FAILED(hr = ID3D12Device5_GetDeviceRemovedReason(dev)))
        return fail("the device", hr);
    if (FAILED(hr = ID3D12CommandAllocator_Reset(alloc)) || FAILED(hr = ID3D12GraphicsCommandList4_Reset(list, alloc, NULL)))
        return fail("the next command list", hr);
    *light = out_back[0].map;
    *refl = out_back[1].map;
    if (reach)
        *reach = out_back[2].map;
    return 0;
}

const rt_backend rt_directx = { "DirectX 12", rt_dx_start, rt_dx_stop, rt_dx_texels, rt_dx_trace };
