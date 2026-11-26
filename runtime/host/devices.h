/* The PC's GPUs and displays, by name, for the GPU and Display settings
 * (docs/design.md section 51). Shared by the game (runtime/host/main.c)
 * and the launcher (runtime/launcher), which list them in their Picture
 * tabs; names are as settings.txt keeps them (settings_name_clean). */
#ifndef VF4FT_DEVICES_H
#define VF4FT_DEVICES_H

#include <stddef.h>

#include <SDL3/SDL.h>

#include "settings.h"

/* Vulkan's device types (VkPhysicalDeviceType) */
enum { GPU_OTHER, GPU_INTEGRATED, GPU_DISCRETE, GPU_VIRTUAL, GPU_CPU };

#define GPUS_MAX 8
typedef struct gpu_info {
    char name[GPU_NAME_MAX];    /* Vulkan's deviceName */
    int type;                   /* GPU_INTEGRATED, GPU_DISCRETE or GPU_VIRTUAL */
    Uint32 vendor, device;      /* its PCI ids (vendorID, deviceID) */
} gpu_info;

/* The Vulkan GPUs here, in Vulkan's order: their number (at most max), 0
 * when there is no Vulkan or no GPU (*why, n bytes, says why; why may be
 * NULL). It loads the Vulkan library and makes an instance of its own, so
 * it works with any video driver and before a window. GPUs that draw on
 * the processor (llvmpipe, lavapipe: Vulkan's CPU type) are left out, as
 * the Software renderer is that; two of one name are listed once (neither
 * SDL nor Vulkan's loader can choose between them) */
int gpus_list(gpu_info *out, int max, char *why, size_t n);

/* the GPU of that name in list (n of them), or NULL */
const gpu_info *gpus_find(const gpu_info *list, int n, const char *name);

/* SDL's Vulkan renderer on a GPU of our choosing (the launcher: docs/design.md
 * section 53). SDL picks the first GPU that can show the window and takes
 * no name, but it takes an instance, a surface and a GPU made elsewhere:
 * this is them. The instance is made with SDL's Vulkan library and the
 * extensions SDL's renderer asks for; the window's surface is on it; the
 * GPUs are the ones that can draw and show the window (a queue for
 * drawing, one that presents to the surface, VK_KHR_swapchain), in
 * Vulkan's order, without those that draw on the processor (as gpus_list).
 * The renderer does not destroy what it is given: vk_window_close, after
 * SDL_DestroyRenderer. */
typedef struct vk_gpu {
    char name[GPU_NAME_MAX];    /* as gpus_list names it */
    void *physical;             /* VkPhysicalDevice */
    Uint32 graphics, present;   /* the queue families */
} vk_gpu;
typedef struct vk_window {
    void *instance;             /* VkInstance; NULL: not open */
    Uint64 surface;             /* VkSurfaceKHR */
    vk_gpu gpu[GPUS_MAX];
    int n;
} vk_window;

/* 0, or -1 with why (n bytes) saying what failed. Opening it may turn the
 * window into a Vulkan one (SDL_WINDOW_VULKAN): the window must have no
 * renderer then */
int vk_window_open(SDL_Window *win, vk_window *v, char *why, size_t n);
void vk_window_close(vk_window *v);

/* the create properties of SDL's Vulkan renderer on v's GPU i
 * (SDL_CreateRendererWithProperties); destroy them after */
SDL_PropertiesID vk_window_props(const vk_window *v, int i, SDL_Window *win);

/* the GPU that renderer r draws on when it is SDL's Vulkan renderer, as
 * gpus_list names it, in buf (n bytes); NULL when it is another */
const char *vk_renderer_gpu(SDL_Renderer *r, char *buf, size_t n);

#define DISPLAYS_MAX 16
/* The displays SDL lists now, in its order: their number (at most max),
 * each one's name in names and its id in ids (ids may be NULL). A name is
 * SDL_GetDisplayName's ("Display N" when it has none), and a second display
 * of the same name is "NAME (2)" */
int displays_list(char names[][DISPLAY_NAME_MAX], SDL_DisplayID *ids, int max);

/* the display of that name now, or 0 (not connected, or "": Auto) */
SDL_DisplayID displays_find(const char *name);

#define SCREEN_MODES_MAX 96
/* The display modes an exclusive fullscreen can set on display d (0: the primary one), as
 * SDL lists them, the largest first (docs/design.md section 86): their number (at most max),
 * each as the settings write it ("WIDTHxHEIGHT@HZ", settings.h), one of a kind, none under
 * 640 x 480 */
int screen_modes_list(SDL_DisplayID d, char names[][SCREENMODE_MAX], int max);

/* the display mode an exclusive fullscreen on d (0: the primary one) takes for the settings'
 * text: the display's nearest to it; for "" (Auto), for a text that is no mode and for one
 * the display has nothing near, the desktop's own. 0 when SDL cannot say (mode is not set) */
int screen_mode_find(SDL_DisplayID d, const char *text, SDL_DisplayMode *mode);

#endif
