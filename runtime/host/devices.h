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

#define DISPLAYS_MAX 16
/* The displays SDL lists now, in its order: their number (at most max),
 * each one's name in names and its id in ids (ids may be NULL). A name is
 * SDL_GetDisplayName's ("Display N" when it has none), and a second display
 * of the same name is "NAME (2)" */
int displays_list(char names[][DISPLAY_NAME_MAX], SDL_DisplayID *ids, int max);

/* the display of that name now, or 0 (not connected, or "": Auto) */
SDL_DisplayID displays_find(const char *name);

#endif
