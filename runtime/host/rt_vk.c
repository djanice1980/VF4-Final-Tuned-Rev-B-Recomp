/* Ray traced light on Vulkan: see rt.h and docs/design.md section 74.
 *
 * A Vulkan device of its own on the GPU the renderer draws with (SDL's GPU layer does not hand
 * its own out), with VK_KHR_acceleration_structure and VK_KHR_ray_query. A frame is one command
 * buffer: the scene's buffers copied in, four bottom level structures and the top level one
 * built anew (the scene is in view space, so nothing of it stands still), the tracing shader
 * (shaders/rt_light.comp) run over the picture, its two results smoothed (shaders/rt_filter.comp)
 * and copied where the processor can read them. The call waits for it.
 *
 * Vulkan's functions come through SDL's loader (no import library, the same on Windows and
 * Linux); its declarations are Khronos's headers in runtime/vulkan. */
#include "rt.h"
#include "gpu_shaders.h"
#include "view.h"

#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <string.h>

#define INSTANCE_FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceFeatures2) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(GetBufferDeviceAddress) \
    X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdDispatch) X(CreateAccelerationStructureKHR) X(DestroyAccelerationStructureKHR) \
    X(GetAccelerationStructureBuildSizesKHR) X(CmdBuildAccelerationStructuresKHR) X(GetAccelerationStructureDeviceAddressKHR)
#define DECLARE(n) static PFN_vk##n vk##n;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkPhysicalDeviceMemoryProperties memp;
static VkDeviceSize scratch_align;
static VkDescriptorSetLayout dsl, fdsl;      /* the tracing shader's, the filter's */
static VkDescriptorPool dpool;
static VkDescriptorSet dset, fdset;
static VkPipelineLayout playout, fplayout;
static VkShaderModule module, fmodule;
static VkPipeline pipe, fpipe;
static int loaded;

typedef struct vbuf {
    VkBuffer b;
    VkDeviceMemory m;
    VkDeviceSize size;
    void *map;
    VkDeviceAddress addr;
} vbuf;

#define ADDRESS VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
#define STORAGE VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
#define INPUT VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR

/* the frame's data: for each kind a staging buffer the processor writes and the GPU's own copy */
enum { B_POS, B_TRI, B_STRIP, B_STATE, B_LIGHTS, B_INST, B_KINDS };
static vbuf stage[B_KINDS], gpu[B_KINDS];
static const VkBufferUsageFlags kind_usage[B_KINDS] = { INPUT | ADDRESS, STORAGE, STORAGE, STORAGE, STORAGE, INPUT | ADDRESS };
static vbuf arena_stage, arena_gpu;     /* the texel arena: all of it mapped; the ranges that changed are copied */
static struct { Uint32 at, n; } *dirty;
static size_t dirty_n, dirty_cap;
static vbuf consts;                     /* the shader's constants, mapped */
static vbuf out_gpu[2], out_back[3];    /* the two results, and where the processor reads them (and the eye rays' lengths) */
static vbuf raw_light, raw_refl, raw_depth;     /* the two before the filter, and the eye rays' lengths */
static vbuf as_store[5], as_scratch;    /* the acceleration structures' memory (4 groups and the top), and the builds' */
static VkAccelerationStructureKHR as[5];

static int fail(const char *what, VkResult r)
{
    SDL_SetError("ray tracing (Vulkan): %s failed (%d)", what, (int)r);
    return -1;
}

static void vbuf_free(vbuf *b)
{
    if (b->b)
        vkDestroyBuffer(dev, b->b, NULL);
    if (b->m)
        vkFreeMemory(dev, b->m, NULL);      /* unmaps too */
    SDL_zerop(b);
}

/* a buffer of at least size bytes (kept when it is large enough); host 1: the processor's memory,
 * mapped, to write into; 2: to read from (cached memory: reading the write-combined kind that
 * suits writing is many times slower) */
static int vbuf_room(vbuf *b, VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    if (b->b && b->size >= size)
        return 0;
    vbuf_free(b);
    VkDeviceSize s = 65536;
    while (s < size)
        s *= 2;
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkResult r = vkCreateBuffer(dev, &ci, NULL, &b->b);
    if (r != VK_SUCCESS)
        return fail("vkCreateBuffer", r);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b->b, &mr);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = UINT32_MAX;
    if (host == 2)
        for (uint32_t i = 0; i < memp.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits >> i & 1) &&
                (memp.memoryTypes[i].propertyFlags & (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                type = i;
    for (uint32_t i = 0; i < memp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (memp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    if (type == UINT32_MAX)
        return fail("finding memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    VkMemoryAllocateFlagsInfo fi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = usage & ADDRESS ? &fi : NULL,
                                .allocationSize = mr.size, .memoryTypeIndex = type };
    if ((r = vkAllocateMemory(dev, &mi, NULL, &b->m)) != VK_SUCCESS || (r = vkBindBufferMemory(dev, b->b, b->m, 0)) != VK_SUCCESS)
        return fail("buffer memory", r);
    if (host && (r = vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->map)) != VK_SUCCESS)
        return fail("vkMapMemory", r);
    if (usage & ADDRESS) {
        VkBufferDeviceAddressInfo a = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->b };
        b->addr = vkGetBufferDeviceAddress(dev, &a);
    }
    b->size = s;
    return 0;
}

static void rt_vk_stop(void)
{
    if (dev) {
        vkDeviceWaitIdle(dev);
        for (int i = 0; i < 5; i++) {
            if (as[i])
                vkDestroyAccelerationStructureKHR(dev, as[i], NULL);
            as[i] = VK_NULL_HANDLE;
            vbuf_free(&as_store[i]);
        }
        for (int i = 0; i < B_KINDS; i++) {
            vbuf_free(&stage[i]);
            vbuf_free(&gpu[i]);
        }
        for (int i = 0; i < 2; i++)
            vbuf_free(&out_gpu[i]);
        for (int i = 0; i < 3; i++)
            vbuf_free(&out_back[i]);
        vbuf_free(&raw_light);
        vbuf_free(&raw_refl);
        vbuf_free(&raw_depth);
        vbuf_free(&arena_stage);
        vbuf_free(&arena_gpu);
        vbuf_free(&consts);
        vbuf_free(&as_scratch);
        if (pipe) vkDestroyPipeline(dev, pipe, NULL);
        if (fpipe) vkDestroyPipeline(dev, fpipe, NULL);
        if (playout) vkDestroyPipelineLayout(dev, playout, NULL);
        if (fplayout) vkDestroyPipelineLayout(dev, fplayout, NULL);
        if (module) vkDestroyShaderModule(dev, module, NULL);
        if (fmodule) vkDestroyShaderModule(dev, fmodule, NULL);
        if (dpool) vkDestroyDescriptorPool(dev, dpool, NULL);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
        if (fdsl) vkDestroyDescriptorSetLayout(dev, fdsl, NULL);
        if (pool) vkDestroyCommandPool(dev, pool, NULL);
        vkDestroyDevice(dev, NULL);
    }
    if (inst)
        vkDestroyInstance(inst, NULL);
    if (loaded)
        SDL_Vulkan_UnloadLibrary();
    dev = VK_NULL_HANDLE;
    inst = VK_NULL_HANDLE;
    pipe = VK_NULL_HANDLE; playout = VK_NULL_HANDLE; module = VK_NULL_HANDLE; dpool = VK_NULL_HANDLE; dsl = VK_NULL_HANDLE;
    fpipe = VK_NULL_HANDLE; fplayout = VK_NULL_HANDLE; fmodule = VK_NULL_HANDLE; fdsl = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    dirty_n = 0;
    loaded = 0;
}

static int rt_vk_start(const char *gpu_name)
{
    if (dev)
        return 0;
    if (!SDL_Vulkan_LoadLibrary(NULL))
        return -1;
    loaded = 1;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!vkGetInstanceProcAddr || !(vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance"))) {
        rt_vk_stop();
        return SDL_SetError("ray tracing (Vulkan): no Vulkan loader"), -1;
    }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vf4ft ray tracing", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        inst = VK_NULL_HANDLE;
        rt_vk_stop();
        return fail("vkCreateInstance", r);
    }
#define LOAD(n) if (!(vk##n = (PFN_vk##n)vkGetInstanceProcAddr(inst, "vk" #n))) { rt_vk_stop(); return SDL_SetError("ray tracing (Vulkan): no vk" #n), -1; }
    INSTANCE_FUNCS(LOAD)
#undef LOAD
    /* the GPU the renderer draws with, by its name; it must have the two extensions */
    VkPhysicalDevice pds[16];
    uint32_t npd = 16;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < npd && !pd; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (!gpu_name || !gpu_name[0] || !strcmp(props.deviceName, gpu_name))
            pd = pds[i];
    }
    if (!pd) {
        rt_vk_stop();
        return SDL_SetError("ray tracing (Vulkan): no GPU named \"%s\"", gpu_name ? gpu_name : ""), -1;
    }
    static VkExtensionProperties have[512];
    uint32_t nhave = 512;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &nhave, have);
    const char *exts[] = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                           VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME };
    for (int e = 0; e < 3; e++) {
        int found = 0;
        for (uint32_t i = 0; i < nhave && !found; i++)
            found = !strcmp(have[i].extensionName, exts[e]);
        if (!found) {
            rt_vk_stop();
            return SDL_SetError("ray tracing (Vulkan): this GPU or its driver has no %s", exts[e]), -1;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &memp);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &asp };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    scratch_align = asp.minAccelerationStructureScratchOffsetAlignment ? asp.minAccelerationStructureScratchOffsetAlignment : 256;
    VkQueueFamilyProperties qf[16];
    uint32_t nqf = 16, fam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    for (uint32_t i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            fam = i;
            break;
        }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = fam, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = VK_TRUE };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                                                             .pNext = &f12, .accelerationStructure = VK_TRUE };
    VkPhysicalDeviceRayQueryFeaturesKHR frq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &fas,
                                                .rayQuery = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &frq };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };
    if ((r = vkCreateDevice(pd, &dci, NULL, &dev)) != VK_SUCCESS) {
        dev = VK_NULL_HANDLE;
        rt_vk_stop();
        return fail("vkCreateDevice", r);
    }
#define LOAD(n) if (!(vk##n = (PFN_vk##n)vkGetDeviceProcAddr(dev, "vk" #n))) { rt_vk_stop(); return SDL_SetError("ray tracing (Vulkan): no vk" #n), -1; }
    vkDestroyDevice = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
    vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)vkGetDeviceProcAddr(dev, "vkDeviceWaitIdle");
    DEVICE_FUNCS(LOAD)
#undef LOAD
    vkGetDeviceQueue(dev, fam, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = fam };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if ((r = vkCreateCommandPool(dev, &pci, NULL, &pool)) != VK_SUCCESS)
        goto failed;
    cai.commandPool = pool;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd)) != VK_SUCCESS)
        goto failed;
    /* the tracing shader and what it reads: 0 the top level structure, 1 the constants, 2 to 9 buffers;
     * the filter: 0 the constants, 1 to 5 buffers */
    VkDescriptorSetLayoutBinding lb[10], flb[6];
    SDL_zeroa(lb);
    SDL_zeroa(flb);
    for (uint32_t i = 0; i < 6; i++) {
        flb[i].binding = i;
        flb[i].descriptorCount = 1;
        flb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        flb[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    for (uint32_t i = 0; i < 10; i++) {
        lb[i].binding = i;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lb[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                               : i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 10, .pBindings = lb };
    VkDescriptorSetLayoutCreateInfo flci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = flb };
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 13 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = ps };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1 };
    if ((r = vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl)) != VK_SUCCESS ||
        (r = vkCreateDescriptorSetLayout(dev, &flci, NULL, &fdsl)) != VK_SUCCESS ||
        (r = vkCreateDescriptorPool(dev, &dpi, NULL, &dpool)) != VK_SUCCESS)
        goto failed;
    dai.descriptorPool = dpool;
    dai.pSetLayouts = &dsl;
    if ((r = vkAllocateDescriptorSets(dev, &dai, &dset)) != VK_SUCCESS)
        goto failed;
    dai.pSetLayouts = &fdsl;
    if ((r = vkAllocateDescriptorSets(dev, &dai, &fdset)) != VK_SUCCESS)
        goto failed;
    /* the shader's words, where words may be read (the header has them as bytes) */
    static uint32_t code[(sizeof spv_rt_light_comp + 3) / 4];
    memcpy(code, spv_rt_light_comp, sizeof spv_rt_light_comp);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv_rt_light_comp,
                                     .pCode = code };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    if ((r = vkCreateShaderModule(dev, &smi, NULL, &module)) != VK_SUCCESS || (r = vkCreatePipelineLayout(dev, &pli, NULL, &playout)) != VK_SUCCESS)
        goto failed;
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = playout };
    cpi.stage = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" };
    if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe)) != VK_SUCCESS)
        goto failed;
    static uint32_t fcode[(sizeof spv_rt_filter_comp + 3) / 4];
    memcpy(fcode, spv_rt_filter_comp, sizeof spv_rt_filter_comp);
    smi.codeSize = sizeof spv_rt_filter_comp;
    smi.pCode = fcode;
    pli.pSetLayouts = &fdsl;
    if ((r = vkCreateShaderModule(dev, &smi, NULL, &fmodule)) != VK_SUCCESS || (r = vkCreatePipelineLayout(dev, &pli, NULL, &fplayout)) != VK_SUCCESS)
        goto failed;
    cpi.layout = fplayout;
    cpi.stage.module = fmodule;
    if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &fpipe)) != VK_SUCCESS)
        goto failed;
    /* the texel arena, whole, and the constants */
    if (vbuf_room(&arena_stage, (VkDeviceSize)VIEW_GPU_ARENA * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1) ||
        vbuf_room(&arena_gpu, (VkDeviceSize)VIEW_GPU_ARENA * 4, STORAGE | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0) ||
        vbuf_room(&consts, sizeof(rt_consts), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1)) {
        rt_vk_stop();
        return -1;
    }
    return 0;
failed:
    rt_vk_stop();
    return fail("setting up", r);
}

static void rt_vk_texels(Uint32 at, Uint32 n, const Uint32 *data)
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

/* everything written before is done before anything after reads it */
static void barrier(void)
{
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                                           VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR };
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    vkCmdPipelineBarrier(cmd, stages, stages, 0, 1, &b, 0, NULL, 0, NULL);
}

/* an acceleration structure of the frame in slot i (the last frame's there goes), recorded */
static int build(int i, const VkAccelerationStructureGeometryKHR *geom, uint32_t prims, VkAccelerationStructureTypeKHR type,
                 VkDeviceAddress *addr)
{
    VkAccelerationStructureBuildGeometryInfoKHR bi = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                       .type = type,
                                                       .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
                                                       .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                       .geometryCount = 1, .pGeometries = geom };
    VkAccelerationStructureBuildSizesInfoKHR sz = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
    if (as[i])
        vkDestroyAccelerationStructureKHR(dev, as[i], NULL);
    as[i] = VK_NULL_HANDLE;
    if (vbuf_room(&as_store[i], sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | ADDRESS, 0))
        return -1;
    VkAccelerationStructureCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                .buffer = as_store[i].b, .size = sz.accelerationStructureSize, .type = type };
    VkResult r = vkCreateAccelerationStructureKHR(dev, &ci, NULL, &as[i]);
    if (r != VK_SUCCESS)
        return fail("vkCreateAccelerationStructureKHR", r);
    bi.dstAccelerationStructure = as[i];
    /* each build its own part of the scratch memory: they are in one command buffer */
    static const VkDeviceSize part = 16u << 20;
    if (sz.buildScratchSize + scratch_align > part)
        return SDL_SetError("ray tracing (Vulkan): a build needs %llu bytes to work in", (unsigned long long)sz.buildScratchSize), -1;
    VkDeviceAddress a = as_scratch.addr + (VkDeviceAddress)i * part;
    bi.scratchData.deviceAddress = (a + scratch_align - 1) / scratch_align * scratch_align;
    VkAccelerationStructureBuildRangeInfoKHR range = { .primitiveCount = prims };
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);
    VkAccelerationStructureDeviceAddressInfoKHR ai = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                       .accelerationStructure = as[i] };
    *addr = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
    return 0;
}

static int rt_vk_trace(const view_gpu_frame *f, int w, int h, const rt_quality *q, const Uint32 **light, const Uint32 **refl,
                       const float **reach)
{
    if (!dev)
        return SDL_SetError("ray tracing (Vulkan): not started"), -1;
    rt_scene sc;
    rt_scene_count(f, &sc);
    if (!sc.ntri)
        return SDL_SetError("ray tracing: the frame has no scene"), -1;
    size_t bytes[B_KINDS] = { (size_t)sc.ntri * 36, (size_t)sc.ntri * sizeof(rt_tri), (size_t)f->nrt_strips * sizeof *f->rt_strips,
                              (size_t)f->nst * sizeof *f->st, (size_t)f->nlights * sizeof(float),
                              4 * sizeof(VkAccelerationStructureInstanceKHR) };
    for (int k = 0; k < B_KINDS; k++)
        if (vbuf_room(&stage[k], bytes[k] + 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1) ||
            vbuf_room(&gpu[k], bytes[k] + 64, kind_usage[k] | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0))
            return -1;
    VkDeviceSize pixels = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    for (int i = 0; i < 2; i++)
        if (vbuf_room(&out_gpu[i], pixels, STORAGE | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
            vbuf_room(&out_back[i], pixels, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2))
            return -1;
    if (vbuf_room(&as_scratch, 5u * (16u << 20), STORAGE | ADDRESS, 0) || vbuf_room(&raw_light, pixels, STORAGE, 0) ||
        vbuf_room(&raw_refl, pixels, STORAGE, 0) || vbuf_room(&raw_depth, pixels, STORAGE | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
        (reach && vbuf_room(&out_back[2], pixels, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2)))
        return -1;
    rt_scene_fill(f, &sc, stage[B_TRI].map, stage[B_POS].map, w, h, VIEW_GPU_ARENA, q);
    memcpy(stage[B_STRIP].map, f->rt_strips, bytes[B_STRIP]);
    memcpy(stage[B_STATE].map, f->st, bytes[B_STATE]);
    memcpy(stage[B_LIGHTS].map, f->lights, bytes[B_LIGHTS]);
    memcpy(consts.map, &sc.consts, sizeof sc.consts);

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult r;
    if ((r = vkResetCommandBuffer(cmd, 0)) != VK_SUCCESS || (r = vkBeginCommandBuffer(cmd, &bi)) != VK_SUCCESS)
        return fail("starting the command buffer", r);
    /* the frame's data and the texels that changed */
    for (int k = 0; k < B_KINDS - 1; k++)
        if (bytes[k]) {
            VkBufferCopy c = { 0, 0, bytes[k] };
            vkCmdCopyBuffer(cmd, stage[k].b, gpu[k].b, 1, &c);
        }
    for (size_t i = 0; i < dirty_n; i++) {
        VkBufferCopy c = { (VkDeviceSize)dirty[i].at * 4, (VkDeviceSize)dirty[i].at * 4, (VkDeviceSize)dirty[i].n * 4 };
        vkCmdCopyBuffer(cmd, arena_stage.b, arena_gpu.b, 1, &c);
    }
    dirty_n = 0;
    barrier();
    /* the four groups' structures */
    VkAccelerationStructureInstanceKHR *insts = stage[B_INST].map;
    uint32_t ninst = 0;
    memset(insts, 0, 4 * sizeof *insts);
    for (int g = 0; g < 4; g++) {
        if (!sc.count[g])
            continue;
        VkAccelerationStructureGeometryKHR geo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = g & 1 ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR };
        geo.geometry.triangles = (VkAccelerationStructureGeometryTrianglesDataKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR, .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData.deviceAddress = gpu[B_POS].addr + (VkDeviceAddress)sc.base[g] * 36, .vertexStride = 12,
            .maxVertex = sc.count[g] * 3 - 1, .indexType = VK_INDEX_TYPE_NONE_KHR };
        VkAccelerationStructureInstanceKHR *in = &insts[ninst++];
        if (build(g, &geo, sc.count[g], VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &in->accelerationStructureReference))
            return -1;
        in->transform.matrix[0][0] = in->transform.matrix[1][1] = in->transform.matrix[2][2] = 1;
        in->instanceCustomIndex = (uint32_t)g;
        in->mask = g < 2 ? 1 : 2;
        in->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    }
    VkBufferCopy ic = { 0, 0, 4 * sizeof *insts };
    vkCmdCopyBuffer(cmd, stage[B_INST].b, gpu[B_INST].b, 1, &ic);
    barrier();
    VkAccelerationStructureGeometryKHR tgeo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR };
    tgeo.geometry.instances = (VkAccelerationStructureGeometryInstancesDataKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR, .data.deviceAddress = gpu[B_INST].addr };
    VkDeviceAddress top;
    if (build(4, &tgeo, ninst, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &top))
        return -1;
    barrier();
    /* the shader over the picture */
    VkWriteDescriptorSetAccelerationStructureKHR was = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                         .accelerationStructureCount = 1, .pAccelerationStructures = &as[4] };
    const vbuf *bufs[10] = { NULL, &consts, &gpu[B_TRI], &gpu[B_STRIP], &gpu[B_STATE], &gpu[B_LIGHTS], &arena_gpu, &raw_light, &raw_refl,
                             &raw_depth };
    VkDescriptorBufferInfo binfo[10];
    VkWriteDescriptorSet wr[10];
    SDL_zeroa(wr);
    for (uint32_t i = 0; i < 10; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = dset;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                               : i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0)
            wr[i].pNext = &was;
        else {
            binfo[i] = (VkDescriptorBufferInfo){ bufs[i]->b, 0, VK_WHOLE_SIZE };
            wr[i].pBufferInfo = &binfo[i];
        }
    }
    vkUpdateDescriptorSets(dev, 10, wr, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL);
    vkCmdDispatch(cmd, (uint32_t)(w + 7) / 8, (uint32_t)(h + 7) / 8, 1);
    barrier();
    /* the two smoothed */
    const vbuf *fbufs[6] = { &consts, &raw_light, &raw_depth, &out_gpu[0], &raw_refl, &out_gpu[1] };
    VkDescriptorBufferInfo finfo[6];
    VkWriteDescriptorSet fwr[6];
    SDL_zeroa(fwr);
    for (uint32_t i = 0; i < 6; i++) {
        finfo[i] = (VkDescriptorBufferInfo){ fbufs[i]->b, 0, VK_WHOLE_SIZE };
        fwr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fwr[i].dstSet = fdset;
        fwr[i].dstBinding = i;
        fwr[i].descriptorCount = 1;
        fwr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fwr[i].pBufferInfo = &finfo[i];
    }
    vkUpdateDescriptorSets(dev, 6, fwr, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fpipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fplayout, 0, 1, &fdset, 0, NULL);
    vkCmdDispatch(cmd, (uint32_t)(w + 7) / 8, (uint32_t)(h + 7) / 8, 1);
    barrier();
    for (int i = 0; i < 2; i++) {
        VkBufferCopy c = { 0, 0, pixels };
        vkCmdCopyBuffer(cmd, out_gpu[i].b, out_back[i].b, 1, &c);
    }
    if (reach) {
        VkBufferCopy c = { 0, 0, pixels };
        vkCmdCopyBuffer(cmd, raw_depth.b, out_back[2].b, 1, &c);
    }
    if ((r = vkEndCommandBuffer(cmd)) != VK_SUCCESS)
        return fail("vkEndCommandBuffer", r);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    if ((r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) != VK_SUCCESS || (r = vkQueueWaitIdle(queue)) != VK_SUCCESS)
        return fail("running the frame", r);
    *light = out_back[0].map;
    *refl = out_back[1].map;
    if (reach)
        *reach = out_back[2].map;
    return 0;
}

const rt_backend rt_vulkan = { "Vulkan", rt_vk_start, rt_vk_stop, rt_vk_texels, rt_vk_trace };
