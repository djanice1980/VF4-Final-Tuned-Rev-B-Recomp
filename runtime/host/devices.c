/* The PC's GPUs and displays: see devices.h. */
#include "devices.h"

#include <string.h>

#include <SDL3/SDL_vulkan.h>

/* ---- the GPUs: Vulkan's own list ----
 * Only the few Vulkan calls and the start of the structures they use, so
 * that no Vulkan headers are needed: an instance with no layers asked for
 * and no extensions, its physical devices, and each one's properties (the
 * fields up to deviceName, which are the same on every system). */

#if defined(_WIN32)
#define VKAPI __stdcall
#else
#define VKAPI
#endif

typedef struct vk_app_info {
    int sType;                  /* VK_STRUCTURE_TYPE_APPLICATION_INFO (0) */
    const void *pNext;
    const char *pApplicationName;
    Uint32 applicationVersion;
    const char *pEngineName;
    Uint32 engineVersion;
    Uint32 apiVersion;
} vk_app_info;

typedef struct vk_instance_info {
    int sType;                  /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO (1) */
    const void *pNext;
    Uint32 flags;
    const vk_app_info *pApplicationInfo;
    Uint32 enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    Uint32 enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} vk_instance_info;

/* VkPhysicalDeviceProperties as far as deviceName; the whole is 824 bytes */
typedef struct vk_props_head {
    Uint32 apiVersion, driverVersion, vendorID, deviceID;
    Sint32 deviceType;
    char deviceName[256];
} vk_props_head;

typedef void (VKAPI *vk_void_fn)(void);
typedef vk_void_fn (VKAPI *vk_get_proc_fn)(void *instance, const char *name);
typedef Sint32 (VKAPI *vk_create_instance_fn)(const vk_instance_info *info, const void *alloc, void **instance);
typedef void (VKAPI *vk_destroy_instance_fn)(void *instance, const void *alloc);
typedef Sint32 (VKAPI *vk_enum_devices_fn)(void *instance, Uint32 *count, void **devices);
typedef void (VKAPI *vk_device_props_fn)(void *device, void *props);

static void say(char *why, size_t n, const char *fmt, const char *arg)
{
    if (why && n)
        SDL_snprintf(why, n, fmt, arg);
}

int gpus_list(gpu_info *out, int max, char *why, size_t n)
{
    say(why, n, "%s", "");
    const char *lib = SDL_GetHint(SDL_HINT_VULKAN_LIBRARY);
#if defined(_WIN32)
    if (!lib)
        lib = "vulkan-1.dll";
#else
    if (!lib)
        lib = "libvulkan.so.1";
#endif
    SDL_SharedObject *so = SDL_LoadObject(lib);
    if (!so) {
        say(why, n, "no Vulkan library (%s)", lib);
        return 0;
    }
    int count = 0;
    void *inst = NULL;
    vk_get_proc_fn get = (vk_get_proc_fn)SDL_LoadFunction(so, "vkGetInstanceProcAddr");
    vk_create_instance_fn create = get ? (vk_create_instance_fn)get(NULL, "vkCreateInstance") : NULL;
    vk_app_info app = { 0, NULL, "vf4ft", 1, "vf4ft", 1, 1u << 22 };    /* Vulkan 1.0 */
    vk_instance_info ii = { 1, NULL, 0, &app, 0, NULL, 0, NULL };
    Sint32 r = create ? create(&ii, NULL, &inst) : -1;
    if (!create || r != 0 || !inst) {
        say(why, n, "%s", create ? "Vulkan cannot start (no driver for it)" : "not a Vulkan library");
        SDL_UnloadObject(so);
        return 0;
    }
    vk_destroy_instance_fn destroy = (vk_destroy_instance_fn)get(inst, "vkDestroyInstance");
    vk_enum_devices_fn enumerate = (vk_enum_devices_fn)get(inst, "vkEnumeratePhysicalDevices");
    vk_device_props_fn props = (vk_device_props_fn)get(inst, "vkGetPhysicalDeviceProperties");
    void *dev[16];
    Uint32 nd = SDL_arraysize(dev);
    r = enumerate && props ? enumerate(inst, &nd, dev) : -1;
    if (r < 0)          /* VK_INCOMPLETE (5): the first 16 */
        nd = 0;
    for (Uint32 i = 0; i < nd && count < max; i++) {
        union {
            vk_props_head h;
            Uint64 room[512];   /* 4 KB, past the structure's end */
        } p;
        SDL_zero(p);
        props(dev[i], &p);
        if (p.h.deviceType != GPU_INTEGRATED && p.h.deviceType != GPU_DISCRETE && p.h.deviceType != GPU_VIRTUAL)
            continue;
        p.h.deviceName[sizeof p.h.deviceName - 1] = 0;
        gpu_info g;
        settings_name_clean(g.name, sizeof g.name, p.h.deviceName);
        g.type = p.h.deviceType;
        g.vendor = p.h.vendorID;
        g.device = p.h.deviceID;
        if (!g.name[0] || gpus_find(out, count, g.name))
            continue;
        out[count++] = g;
    }
    if (!count)
        say(why, n, "%s", nd ? "Vulkan lists no GPU (only ones that draw on the processor)" : "Vulkan lists no GPU");
    if (destroy)
        destroy(inst, NULL);
    SDL_UnloadObject(so);
    return count;
}

const gpu_info *gpus_find(const gpu_info *list, int n, const char *name)
{
    for (int i = 0; i < n && name && name[0]; i++)
        if (!SDL_strcasecmp(list[i].name, name))
            return &list[i];
    return NULL;
}

/* ---- SDL's Vulkan renderer on a GPU of our choosing (devices.h) ----
 * More of Vulkan, the same way: the calls to list the GPUs that can show a
 * window. VkQueueFamilyProperties and VkExtensionProperties are whole here
 * (24 and 260 bytes on every system). */

typedef struct vk_queue_family {
    Uint32 queueFlags;          /* VK_QUEUE_GRAPHICS_BIT: 1 */
    Uint32 queueCount, timestampValidBits;
    Uint32 granularity[3];
} vk_queue_family;

typedef struct vk_extension {
    char extensionName[256];
    Uint32 specVersion;
} vk_extension;

typedef Sint32 (VKAPI *vk_enum_instance_ext_fn)(const char *layer, Uint32 *count, vk_extension *out);
typedef void (VKAPI *vk_queue_families_fn)(void *device, Uint32 *count, vk_queue_family *out);
typedef Sint32 (VKAPI *vk_surface_support_fn)(void *device, Uint32 family, VkSurfaceKHR surface, Uint32 *supported);
typedef Sint32 (VKAPI *vk_enum_device_ext_fn)(void *device, const char *layer, Uint32 *count, vk_extension *out);

static int has_extension(const vk_extension *e, Uint32 n, const char *name)
{
    for (Uint32 i = 0; i < n; i++)
        if (!SDL_strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

int vk_window_open(SDL_Window *win, vk_window *v, char *why, size_t n)
{
    SDL_zerop(v);
    say(why, n, "%s", "");
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        say(why, n, "Vulkan cannot start (%s)", SDL_GetError());
        return -1;
    }
    vk_get_proc_fn get = (vk_get_proc_fn)SDL_Vulkan_GetVkGetInstanceProcAddr();
    Uint32 nsdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&nsdl);
    vk_create_instance_fn create = get ? (vk_create_instance_fn)get(NULL, "vkCreateInstance") : NULL;
    vk_enum_instance_ext_fn inst_ext = get ? (vk_enum_instance_ext_fn)get(NULL, "vkEnumerateInstanceExtensionProperties")
                                           : NULL;
    if (!create || !inst_ext || !sdl_ext) {
        say(why, n, "%s", "not a Vulkan library");
        SDL_Vulkan_UnloadLibrary();
        return -1;
    }
    /* SDL's extensions, and VK_KHR_get_physical_device_properties2 when
     * there is one: SDL's renderer looks for it (as it would on an instance
     * of its own) and turns on the device's YCbCr ones with it */
    const char *ext[24];
    Uint32 next = 0;
    for (Uint32 i = 0; i < nsdl && next < SDL_arraysize(ext) - 1; i++)
        ext[next++] = sdl_ext[i];
    static const char props2[] = "VK_KHR_get_physical_device_properties2";
    Uint32 nav = 0;
    vk_extension *av = NULL;
    if (inst_ext(NULL, &nav, NULL) == 0 && nav && (av = SDL_calloc(nav, sizeof *av)) && inst_ext(NULL, &nav, av) >= 0 &&
        has_extension(av, nav, props2))
        ext[next++] = props2;
    SDL_free(av);
    vk_app_info app = { 0, NULL, "vf4ft", 1, "vf4ft", 1, 1u << 22 };    /* Vulkan 1.0, as SDL's own */
    vk_instance_info ii = { 1, NULL, 0, &app, 0, NULL, next, ext };
    void *inst = NULL;
    if (create(&ii, NULL, &inst) != 0 || !inst) {
        say(why, n, "%s", "Vulkan cannot start (no driver for it)");
        SDL_Vulkan_UnloadLibrary();
        return -1;
    }
    v->instance = inst;
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(win, (VkInstance)inst, NULL, &surface)) {
        say(why, n, "no Vulkan surface for the window (%s)", SDL_GetError());
        vk_window_close(v);
        return -1;
    }
    SDL_COMPILE_TIME_ASSERT(surface_fits, sizeof surface <= sizeof v->surface);
    SDL_memcpy(&v->surface, &surface, sizeof surface);
    vk_enum_devices_fn enumerate = (vk_enum_devices_fn)get(inst, "vkEnumeratePhysicalDevices");
    vk_device_props_fn props = (vk_device_props_fn)get(inst, "vkGetPhysicalDeviceProperties");
    vk_queue_families_fn families = (vk_queue_families_fn)get(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    vk_surface_support_fn support = (vk_surface_support_fn)get(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    vk_enum_device_ext_fn dev_ext = (vk_enum_device_ext_fn)get(inst, "vkEnumerateDeviceExtensionProperties");
    void *dev[16];
    Uint32 nd = SDL_arraysize(dev);
    if (!enumerate || !props || !families || !support || !dev_ext || enumerate(inst, &nd, dev) < 0)
        nd = 0;
    int cpu = 0;
    for (Uint32 i = 0; i < nd && v->n < GPUS_MAX; i++) {
        union {
            vk_props_head h;
            Uint64 room[512];
        } p;
        SDL_zero(p);
        props(dev[i], &p);
        if (p.h.deviceType != GPU_INTEGRATED && p.h.deviceType != GPU_DISCRETE && p.h.deviceType != GPU_VIRTUAL) {
            cpu++;
            continue;
        }
        vk_queue_family qf[16];
        Uint32 nq = SDL_arraysize(qf), graphics = ~0u, present = ~0u;
        families(dev[i], &nq, qf);
        for (Uint32 q = 0; q < nq; q++) {   /* as SDL's own choice: one that does both if there is one */
            Uint32 can = 0;
            if (!qf[q].queueCount)
                continue;
            if (qf[q].queueFlags & 1)
                graphics = q;
            if (support(dev[i], q, surface, &can) == 0 && can) {
                present = q;
                if (qf[q].queueFlags & 1)
                    break;
            }
        }
        Uint32 ne = 0;
        vk_extension *de = NULL;
        int swapchain = graphics != ~0u && present != ~0u && dev_ext(dev[i], NULL, &ne, NULL) == 0 && ne &&
                        (de = SDL_calloc(ne, sizeof *de)) && dev_ext(dev[i], NULL, &ne, de) >= 0 &&
                        has_extension(de, ne, "VK_KHR_swapchain");
        SDL_free(de);
        if (!swapchain)
            continue;
        vk_gpu *g = &v->gpu[v->n++];
        p.h.deviceName[sizeof p.h.deviceName - 1] = 0;
        settings_name_clean(g->name, sizeof g->name, p.h.deviceName);
        g->physical = dev[i];
        g->graphics = graphics;
        g->present = present;
    }
    if (!v->n)
        say(why, n, "%s", !nd ? "Vulkan lists no GPU" : cpu == (int)nd ? "Vulkan lists no GPU (only ones that draw on the processor)"
                                                                     : "no GPU can show the window with Vulkan");
    return 0;
}

void vk_window_close(vk_window *v)
{
    if (!v->instance)
        return;
    VkSurfaceKHR surface = 0;
    SDL_memcpy(&surface, &v->surface, sizeof surface);
    if (surface)
        SDL_Vulkan_DestroySurface((VkInstance)v->instance, surface, NULL);
    vk_get_proc_fn get = (vk_get_proc_fn)SDL_Vulkan_GetVkGetInstanceProcAddr();
    vk_destroy_instance_fn destroy = get ? (vk_destroy_instance_fn)get(v->instance, "vkDestroyInstance") : NULL;
    if (destroy)
        destroy(v->instance, NULL);
    SDL_Vulkan_UnloadLibrary();
    SDL_zerop(v);
}

SDL_PropertiesID vk_window_props(const vk_window *v, int i, SDL_Window *win)
{
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, win);
    SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER, v->instance);
    SDL_SetNumberProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_SURFACE_NUMBER, (Sint64)v->surface);
    SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_PHYSICAL_DEVICE_POINTER, v->gpu[i].physical);
    SDL_SetNumberProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER, v->gpu[i].graphics);
    SDL_SetNumberProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_PRESENT_QUEUE_FAMILY_INDEX_NUMBER, v->gpu[i].present);
    return p;
}

const char *vk_renderer_gpu(SDL_Renderer *r, char *buf, size_t n)
{
    SDL_PropertiesID p = SDL_GetRendererProperties(r);
    void *inst = SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER, NULL);
    void *dev = SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_PHYSICAL_DEVICE_POINTER, NULL);
    vk_get_proc_fn get = inst && dev ? (vk_get_proc_fn)SDL_Vulkan_GetVkGetInstanceProcAddr() : NULL;
    vk_device_props_fn props = get ? (vk_device_props_fn)get(inst, "vkGetPhysicalDeviceProperties") : NULL;
    if (!props)
        return NULL;
    union {
        vk_props_head h;
        Uint64 room[512];
    } d;
    SDL_zero(d);
    props(dev, &d);
    d.h.deviceName[sizeof d.h.deviceName - 1] = 0;
    settings_name_clean(buf, n, d.h.deviceName);
    return buf;
}

/* ---- the displays ---- */

int displays_list(char names[][DISPLAY_NAME_MAX], SDL_DisplayID *ids, int max)
{
    int nd = 0, count = 0;
    SDL_DisplayID *d = SDL_GetDisplays(&nd);
    for (int i = 0; d && i < nd && count < max; i++) {
        char base[DISPLAY_NAME_MAX], fallback[32];
        const char *sdl = SDL_GetDisplayName(d[i]);
        SDL_snprintf(fallback, sizeof fallback, "Display %d", i + 1);
        settings_name_clean(base, sizeof base, sdl && sdl[0] ? sdl : fallback);
        if (!base[0])
            SDL_strlcpy(base, fallback, sizeof base);
        int same = 0;
        for (int k = 0; k < i; k++) {
            const char *other = SDL_GetDisplayName(d[k]);
            char o[DISPLAY_NAME_MAX];
            SDL_snprintf(fallback, sizeof fallback, "Display %d", k + 1);
            settings_name_clean(o, sizeof o, other && other[0] ? other : fallback);
            same += !SDL_strcasecmp(o, base);
        }
        if (same) {
            char tail[16], whole[DISPLAY_NAME_MAX];
            SDL_snprintf(tail, sizeof tail, " (%d)", same + 1);
            SDL_strlcpy(whole, base, sizeof whole);
            settings_name_clean(base, sizeof base - SDL_strlen(tail), whole);     /* room for the tail */
            SDL_strlcat(base, tail, sizeof base);
        }
        SDL_strlcpy(names[count], base, DISPLAY_NAME_MAX);
        if (ids)
            ids[count] = d[i];
        count++;
    }
    SDL_free(d);
    return count;
}

SDL_DisplayID displays_find(const char *name)
{
    char names[DISPLAYS_MAX][DISPLAY_NAME_MAX];
    SDL_DisplayID ids[DISPLAYS_MAX];
    int n = name && name[0] ? displays_list(names, ids, DISPLAYS_MAX) : 0;
    for (int i = 0; i < n; i++)
        if (!SDL_strcasecmp(names[i], name))
            return ids[i];
    return 0;
}

int screen_modes_list(SDL_DisplayID d, char names[][SCREENMODE_MAX], int max)
{
    int count = 0, n = 0;
    if (!d)
        d = SDL_GetPrimaryDisplay();
    SDL_DisplayMode **modes = d ? SDL_GetFullscreenDisplayModes(d, &count) : NULL;
    for (int i = 0; modes && i < count && n < max; i++) {
        char t[SCREENMODE_MAX];
        int seen = 0;
        if (modes[i]->w < 640 || modes[i]->h < 480)
            continue;
        settings_screenmode_text(t, sizeof t, modes[i]->w, modes[i]->h, modes[i]->refresh_rate);
        for (int k = 0; k < n; k++)     /* the same size and rate in another pixel format or density */
            seen |= !SDL_strcmp(names[k], t);
        if (!seen)
            SDL_strlcpy(names[n++], t, SCREENMODE_MAX);
    }
    SDL_free(modes);
    return n;
}

int screen_mode_find(SDL_DisplayID d, const char *text, SDL_DisplayMode *mode)
{
    int w, h;
    float hz;
    const SDL_DisplayMode *desk;
    if (!d)
        d = SDL_GetPrimaryDisplay();
    if (!d)
        return 0;
    if (text && settings_screenmode_parse(text, &w, &h, &hz) &&
        SDL_GetClosestFullscreenDisplayMode(d, w, h, hz, false, mode))
        return 1;
    desk = SDL_GetDesktopDisplayMode(d);
    if (!desk)
        return 0;
    *mode = *desk;
    return 1;
}
