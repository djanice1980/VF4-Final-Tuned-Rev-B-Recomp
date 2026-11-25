/* The GPU renderer (M6, docs/design.md section 6): draws the scenes that
 * runtime/machine/view.c hands over (view.h, view_gpu_frame) with SDL_GPU,
 * at a multiple of the game's 640 x 480, by the software rasterizer's
 * rules. The shaders are SPIR-V (runtime/host/shaders, gpu_shaders.h), so
 * the device must be Vulkan's: natively, or through Proton's winevulkan. */
#ifndef VF4FT_GPU_RENDER_H
#define VF4FT_GPU_RENDER_H

#include <SDL3/SDL.h>

#include "rt.h"

struct view_gpu_frame;

/* after the device is made; scale 1..4. 0, or -1 with a message in the SDL error */
int gpu_render_init(SDL_GPUDevice *dev, int scale);

/* view.c's hook (view_host_gpu), on the machine's thread: the frame is
 * copied, the textures that changed are kept for the next upload */
void gpu_render_submit(const struct view_gpu_frame *f);

/* the same, with when the frame is to be shown by the game's clock (ns;
 * vsync.h's vsync_pick). Two frames can wait to be drawn; a third puts the
 * oldest out */
void gpu_render_submit_at(const struct view_gpu_frame *f, Uint64 pts);

/* any thread: the frames waiting, oldest first: their vertical blanks and
 * times. Returns how many (0 to 2) */
int gpu_render_waiting(unsigned frame[2], Uint64 pts[2]);

/* on the window's thread, before the swapchain is drawn: draws the newest
 * frame waiting whose vertical blank is no more than upto (0: none, this
 * present shows the picture before again; ~0u: the newest), passing over
 * the ones before it. Returns the picture (NULL before the first) with its
 * size in pixels and the game's vertical blank. */
SDL_GPUTexture *gpu_render_frame(SDL_GPUCommandBuffer *cmd, Uint32 *w, Uint32 *h, unsigned *frame, unsigned upto);

/* on the window's thread, at any time (before gpu_render_init too; it stays
 * over a start again): ambient occlusion out of the depth buffer (SSAO,
 * docs/design.md section 73) is multiplied into the 3D scene's solid
 * surfaces; the game's 2D layer and what is translucent are as before. Off
 * (the default), a frame is drawn as ever. A device that cannot draws as
 * with it off. */
void gpu_render_occlusion(int on);

/* on the window's thread, at any time (before gpu_render_init too; it stays over a start again):
 * ray traced light (docs/design.md section 74) by this backend (rt.h), or NULL for none, the
 * default. The backend is started on the renderer's GPU with the next frame; one that cannot
 * start (no ray tracing on the GPU) is said in the log, and the picture is drawn as without.
 * quality (section 76): 0 low, 1 medium, 2 high (settings.h RAYQUALITY_*): how many of the
 * picture's pixels are traced, and by how many rays; a change is taken up with the next frame. */
void gpu_render_raytracing(const rt_backend *backend, int quality);

/* frames drawn with traced light and the mean time one took to trace and bring back, ms; but for
 * the first frame after each start of the ray tracing, whose time is first_ms (the last start's):
 * it holds work done once (a driver may compile shaders of its own then) */
void gpu_render_rt_stats(unsigned *frames, double *ms, double *first_ms);

/* any thread: the frames the machine handed over, and those of them never
 * drawn (a newer one came first) */
void gpu_render_counts(unsigned *submitted, unsigned *replaced);

/* frames drawn, and the most triangles in one, for the log */
void gpu_render_stats(unsigned *frames, unsigned *max_tris, unsigned *translucent_volume_frames);

void gpu_render_quit(void);

#endif
