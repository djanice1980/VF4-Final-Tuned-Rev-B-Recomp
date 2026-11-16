/* The PC's GPUs and displays: see devices.h. */
#include "devices.h"

#include <string.h>

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
