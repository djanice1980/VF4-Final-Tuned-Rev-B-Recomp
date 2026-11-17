/* The GPU renderer (M6, docs/design.md section 6): draws the scenes that
 * runtime/machine/view.c hands over (view.h, view_gpu_frame) with SDL_GPU,
 * at a multiple of the game's 640 x 480, by the software rasterizer's
 * rules. The shaders are SPIR-V (runtime/host/shaders, gpu_shaders.h), so
 * the device must be Vulkan's: natively, or through Proton's winevulkan. */
#ifndef VF4FT_GPU_RENDER_H
#define VF4FT_GPU_RENDER_H

#include <SDL3/SDL.h>

struct view_gpu_frame;

/* after the device is made; scale 1..4. 0, or -1 with a message in the SDL error */
int gpu_render_init(SDL_GPUDevice *dev, int scale);

/* view.c's hook (view_host_gpu), on the machine's thread: the frame is
 * copied, the textures that changed are kept for the next upload */
void gpu_render_submit(const struct view_gpu_frame *f);

/* on the window's thread, before the swapchain is drawn: draws the newest
 * frame if one came since the last call, unless take is 0 (this present
 * shows the frame before again: vsync.h). Returns the picture (NULL before
 * the first) with its size in pixels and the game's vertical blank. */
SDL_GPUTexture *gpu_render_frame(SDL_GPUCommandBuffer *cmd, Uint32 *w, Uint32 *h, unsigned *frame, int take);

/* any thread: the frames the machine handed over, and those of them never
 * drawn (a newer one came first) */
void gpu_render_counts(unsigned *submitted, unsigned *replaced);

/* frames drawn, and the most triangles in one, for the log */
void gpu_render_stats(unsigned *frames, unsigned *max_tris, unsigned *translucent_volume_frames);

void gpu_render_quit(void);

#endif
