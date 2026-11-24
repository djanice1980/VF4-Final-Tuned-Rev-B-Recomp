/* recomp: the game's view planes, for a wide picture (docs/design.md section
 * 68). Not VF4debug's: see ORIGIN.md.
 *
 * The game leaves out of its lists some of what its 4:3 view does not hold.
 * proj_from_vfov and proj_from_hfov leave the view's four side planes at
 * view_side_planes, and two routines test spheres against them:
 * stage_far_objs_draw (a stage's far objects) and stage_obj_visible (the
 * water stages' patches). Nothing else in the game reads the planes.
 *
 * A wide picture shows what lies left and right of that view (view.c,
 * view_host_wide), so there those objects were missing, and came and went
 * as the camera turned. With a width above 640 the left and right planes
 * are set wider by the picture's share as soon as the game has made them.
 * The tests are the game's own code still. At 640 nothing here does
 * anything: the machine is the game's.
 *
 * (obj_sphere_visible, the game's third test, is against the target
 * rectangle. It is left alone: in a survey of every stage it turned away
 * nothing that a 21:9 picture shows.)
 */
#include "hle.h"
#include "mem.h"
#include "tables.h"

#include <xmmintrin.h>

void rt_continue(sh4 *c);       /* runtime/native/rt_native.c */

static u32 a_planes;
static int cull_w = 640;

static float rf(u32 a)
{
    union { u32 u; float f; } v = { g_r32(a) };
    return v.f;
}

static void wf(u32 a, float f)
{
    union { float f; u32 u; } v = { f };
    g_w32(a, v.u);
}

/* Runs the routine itself (rt_continue: its own code from its start to its
 * return; the dispatcher then sees the processor past it and goes on from
 * there) and mends what it left. No event is served on the way: the
 * routine is sums on registers and memory, a few hundred instructions, and
 * a snapshot taken inside it would resume without this frame and leave the
 * planes the game's. An event that falls due waits for the routine's end.
 *
 * The planes are (cos e, 0, sin e) and (-cos e, 0, sin e) for the left and
 * right sides, e the angle from the view's axis to the side. The tangent
 * grows by the picture's share; the sum uses +, *, / and the square root
 * only, which give the same bits on every PC. */
static void hook_proj(sh4 *c, routine *r)
{
    (void)r;
    int w = __atomic_load_n(&cull_w, __ATOMIC_RELAXED);
    if (w <= 640)
        return;
    u64 next = c->next_event;
    c->next_event = c->cycles + 100000000ull;
    rt_continue(c);
    c->next_event = next < c->cycles ? 0 : next;
    float co = rf(a_planes), si = rf(a_planes + 8);
    if (!(co > 0.0f))
        return;
    float t = si / co * ((float)w / 640.0f);
    float k = 1.0f / _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(1.0f + t * t)));
    wf(a_planes, k);
    wf(a_planes + 16, -k);
    wf(a_planes + 8, t * k);
    wf(a_planes + 24, t * k);
}

void hle_cull_wide(int width)
{
    __atomic_store_n(&cull_w, width < 640 ? 640 : width, __ATOMIC_RELAXED);
}

void hle_cull_install(sh4 *c)
{
    (void)c;
    a_planes = 0;
    if (disp_program != PROGRAM_MAIN)
        return;                 /* the test program draws no stage */
    a_planes = global_addr("view_side_planes");
    if (!a_planes)
        return;
    disp_hook("proj_from_vfov", hook_proj, "wide picture: the side planes are set wider after it");
    disp_hook("proj_from_hfov", hook_proj, "wide picture: the side planes are set wider after it");
}
