/* The frame's scene packed for the tracing shader: see rt.h. Nothing of any graphics API. */
#include "rt.h"
#include "view.h"

#include <math.h>
#include <string.h>

/* what a triangle is to the rays, from its strip and its state; -1: nothing (a zero triangle, or
 * a blend that only adds light) */
static int classify(const view_gpu_frame *f, const view_rt_tri *t, Uint32 *flags)
{
    const view_rt_strip *s = &f->rt_strips[t->strip];
    const view_gpu_state *st = &f->st[s->state];
    Uint32 list = VIEW_RT_LIST(s->flags), src = st->tsp >> 29, dst = st->tsp >> 26 & 7;
    /* the translucent list: source alpha over, or premultiplied, counts by its alpha; a plain
     * replace is solid; anything else (added light) is not in the rays' way */
    int blend = list == 0 ? 0 : list == 1 ? 2 : dst == 5 && (src == 4 || src == 1) ? 1 : dst == 0 && src == 1 ? 0 : -1;
    if (blend < 0)
        return -1;
    const float *a = t->v[0].p, *b = t->v[1].p, *c = t->v[2].p;
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    if (!(nx * nx + ny * ny + nz * nz > 1e-24f))
        return -1;
    int baked = (t->v[0].col | t->v[1].col | t->v[2].col) || (s->flags & (VIEW_RT_UNLIT | VIEW_RT_BUMPED));
    int fighter = !baked && !(st->pcw >> 7 & 1);
    /* a one-sided triangle is seen from O when dot(its normal by its winding, O - corner) * front
     * is above 0: the screen's test (view.c, tri_view) in three dimensions */
    float side = f->proj[0] * f->proj[2] < 0 ? -1.0f : 1.0f;
    float front = s->flags & VIEW_RT_ONE_SIDED ? -side * (s->flags & VIEW_RT_BACK ? -1.0f : 1.0f) : 0;
    *flags = (Uint32)blend | (baked ? RT_TRI_BAKED : 0) | (fighter ? RT_TRI_FIGHTER : 0) | (front != 0 ? RT_TRI_ONE_SIDED : 0) |
             (front < 0 ? RT_TRI_BACK : 0);
    /* water: the game's own rule, the polygons it gives a reflection pass's picture to, in a frame
     * no such pass was drawn for (view.h, rt_mirror) */
    if (f->rt_mirror && st->target && blend == 0)
        *flags |= RT_TRI_MIRROR;
    return (fighter ? 0 : 2) + (blend ? 1 : 0);
}

static SDL_AtomicInt water_over = { 100 };

void rt_scene_water_over(int percent)
{
    SDL_SetAtomicInt(&water_over, percent < 0 ? 0 : percent > 100 ? 100 : percent);
}

void rt_scene_count(const view_gpu_frame *f, rt_scene *s)
{
    SDL_zerop(s);
    for (Uint32 i = 0; i < f->nrt_tris; i++) {
        Uint32 fl;
        int g = classify(f, &f->rt_tris[i], &fl);
        if (g >= 0)
            s->count[g]++;
    }
    for (int g = 1; g < 4; g++)
        s->base[g] = s->base[g - 1] + s->count[g - 1];
    s->ntri = s->base[3] + s->count[3];
}

/* The way the game's shadows fall, in two steps. Its shadow volumes are the fighters' outlines on
 * the floor drawn out straight up and down: their long edges give the floor's upright, and their
 * corners the outline's ground plan. From the middle of the fighters (their box is gathered as
 * the triangles are packed) to the middle of the ground plan at the height of the fighters'
 * lowest point is the way the light travels. (The game's own shadow light, read from its code,
 * is to replace this estimate: section 74.) */
typedef struct shadow_frame {
    int ok;
    double ax[3], up[3], bx[3];         /* across the floor, upright, across */
    double vlo[2], vhi[2];              /* the volumes' ground plan, along ax and bx */
    double flo[3], fhi[3];              /* the fighters' box, along ax, up, bx */
} shadow_frame;

static void shadow_start(const view_gpu_frame *f, shadow_frame *sf)
{
    SDL_zerop(sf);
    if (f->nvol_xyz < 3 || f->proj[0] == 0 || f->proj[2] == 0)
        return;
    double M[3][3] = { { 0 } };
    static float (*corner)[3];
    static size_t corner_cap;
    if (f->nvol_xyz > corner_cap) {
        void *n = SDL_realloc(corner, (size_t)f->nvol_xyz * sizeof *corner);
        if (!n)
            return;
        corner = n;
        corner_cap = f->nvol_xyz;
    }
    Uint32 nc = 0;
    for (Uint32 i = 0; i + 2 < f->nvol_xyz; i += 3) {
        float p[3][3];
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            const float *c = f->vol_xyz + (size_t)(i + (Uint32)k) * 3;
            if (!(c[2] > 1e-6f)) {
                ok = 0;
                break;
            }
            float Z = 1 / c[2];
            p[k][0] = -(c[0] - f->proj[1]) / f->proj[0] * Z;
            p[k][1] = (c[1] - f->proj[3]) / f->proj[2] * Z;
            p[k][2] = -Z;
        }
        if (!ok)
            continue;
        for (int k = 0; k < 3; k++) {
            memcpy(corner[nc++], p[k], sizeof p[k]);
            const float *a = p[k], *b = p[(k + 1) % 3];
            float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            float l = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            if (!(l > 0.3f))
                continue;
            for (int x = 0; x < 3; x++)
                for (int y = 0; y < 3; y++)
                    M[x][y] += (double)(e[x] * e[y] / l);
        }
    }
    if (!nc || !(M[0][0] + M[1][1] + M[2][2] > 0))
        return;
    /* the upright: the direction most of the edges' length shares */
    double *up = sf->up, *ax = sf->ax, *bx = sf->bx;
    up[0] = 0.1; up[1] = 1.0; up[2] = 0.1;
    for (int it = 0; it < 64; it++) {
        double y[3], n = 0;
        for (int a = 0; a < 3; a++) {
            y[a] = M[a][0] * up[0] + M[a][1] * up[1] + M[a][2] * up[2];
            n += y[a] * y[a];
        }
        n = sqrt(n);
        if (!(n > 0))
            return;
        for (int a = 0; a < 3; a++)
            up[a] = y[a] / n;
    }
    if (up[1] < 0)
        for (int a = 0; a < 3; a++)
            up[a] = -up[a];
    ax[0] = 1; ax[1] = 0; ax[2] = 0;
    double d = up[0];
    for (int a = 0; a < 3; a++)
        ax[a] -= d * up[a];
    double n = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (!(n > 1e-6))
        return;
    for (int a = 0; a < 3; a++)
        ax[a] /= n;
    bx[0] = up[1] * ax[2] - up[2] * ax[1];
    bx[1] = up[2] * ax[0] - up[0] * ax[2];
    bx[2] = up[0] * ax[1] - up[1] * ax[0];
    for (int a = 0; a < 2; a++) {
        sf->vlo[a] = 1e30;
        sf->vhi[a] = -1e30;
    }
    for (int a = 0; a < 3; a++) {
        sf->flo[a] = 1e30;
        sf->fhi[a] = -1e30;
    }
    for (Uint32 i = 0; i < nc; i++) {
        const float *p = corner[i];
        double c[2] = { p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2], p[0] * bx[0] + p[1] * bx[1] + p[2] * bx[2] };
        for (int a = 0; a < 2; a++) {
            if (c[a] < sf->vlo[a]) sf->vlo[a] = c[a];
            if (c[a] > sf->vhi[a]) sf->vhi[a] = c[a];
        }
    }
    sf->ok = 1;
}

/* a fighter's corner */
static inline void shadow_point(shadow_frame *sf, const float *p)
{
    double c[3] = { p[0] * sf->ax[0] + p[1] * sf->ax[1] + p[2] * sf->ax[2], p[0] * sf->up[0] + p[1] * sf->up[1] + p[2] * sf->up[2],
                    p[0] * sf->bx[0] + p[1] * sf->bx[1] + p[2] * sf->bx[2] };
    for (int a = 0; a < 3; a++) {
        if (c[a] < sf->flo[a]) sf->flo[a] = c[a];
        if (c[a] > sf->fhi[a]) sf->fhi[a] = c[a];
    }
}

/* 1 with the way the light travels */
static int shadow_end(const shadow_frame *sf, float *dir)
{
    if (!sf->ok || !(sf->fhi[1] >= sf->flo[1]))
        return 0;
    double way[3] = { (sf->vlo[0] + sf->vhi[0]) / 2 - (sf->flo[0] + sf->fhi[0]) / 2, sf->flo[1] - (sf->flo[1] + sf->fhi[1]) / 2,
                      (sf->vlo[1] + sf->vhi[1]) / 2 - (sf->flo[2] + sf->fhi[2]) / 2 };
    double w[3], l = 0;
    for (int a = 0; a < 3; a++) {
        w[a] = way[0] * sf->ax[a] + way[1] * sf->up[a] + way[2] * sf->bx[a];
        l += w[a] * w[a];
    }
    l = sqrt(l);
    if (!(l > 1e-6))
        return 0;
    for (int a = 0; a < 3; a++)
        dir[a] = (float)(w[a] / l);
    return 1;
}

void rt_scene_fill(const view_gpu_frame *f, rt_scene *s, rt_tri *tris, float *pos, int w, int h, Uint32 texels, const rt_quality *q)
{
    /* tris and pos may be memory that is slow to read (a GPU's staging buffer): written only */
    Uint32 fill[4];
    memcpy(fill, s->base, sizeof fill);
    shadow_frame sf;
    shadow_start(f, &sf);
    double mirror_n[3] = { 0, 0, 0 };   /* the water's triangles' normals, each towards the eye, by their areas */
    double mirror_c[3] = { 0, 0, 0 }, mirror_a = 0;     /* their middles by their areas, and the areas' sum */
    for (Uint32 i = 0; i < f->nrt_tris; i++) {
        const view_rt_tri *t = &f->rt_tris[i];
        Uint32 fl;
        int g = classify(f, t, &fl);
        if (g < 0)
            continue;
        Uint32 k = fill[g]++;
        rt_tri *o = &tris[k];
        float *pp[3] = { o->p0, o->p1, o->p2 }, *nn[3] = { o->n0, o->n1, o->n2 };
        for (int c = 0; c < 3; c++) {
            const view_rt_vertex *v = &t->v[c];
            /* into the game's view space: x and z turned round */
            float q[3] = { -v->p[0], v->p[1], -v->p[2] };
            pp[c][0] = q[0]; pp[c][1] = q[1]; pp[c][2] = q[2]; pp[c][3] = 0;
            nn[c][0] = v->n[0]; nn[c][1] = v->n[1]; nn[c][2] = v->n[2]; nn[c][3] = 0;
            memcpy(pos + (size_t)k * 9 + (size_t)c * 3, q, sizeof q);
            o->col[c] = v->col;
            if (g < 2 && sf.ok)
                shadow_point(&sf, q);
        }
        o->col[3] = 0;
        o->uv01[0] = t->v[0].uv[0]; o->uv01[1] = t->v[0].uv[1]; o->uv01[2] = t->v[1].uv[0]; o->uv01[3] = t->v[1].uv[1];
        o->uv2[0] = t->v[2].uv[0]; o->uv2[1] = t->v[2].uv[1];
        o->strip = t->strip;
        o->flags = fl;
        if (fl & RT_TRI_MIRROR) {
            /* from the frame's vertices again (o is written only) */
            const float a[3] = { -t->v[0].p[0], t->v[0].p[1], -t->v[0].p[2] }, b[3] = { -t->v[1].p[0], t->v[1].p[1], -t->v[1].p[2] },
                        c[3] = { -t->v[2].p[0], t->v[2].p[1], -t->v[2].p[2] };
            double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            double side = n[0] * a[0] + n[1] * a[1] + n[2] * a[2] > 0 ? -1.0 : 1.0;    /* the eye is at the origin */
            double area = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int x = 0; x < 3; x++) {
                mirror_n[x] += side * n[x];
                mirror_c[x] += area * ((double)a[x] + b[x] + c[x]) / 3;
            }
            mirror_a += area;
            o->col[3] = t->v[0].ofs;    /* the color its polygons add (one for the strip) */
        }
    }
    rt_consts *u = &s->consts;
    SDL_zerop(u);
    memcpy(u->proj, f->proj, sizeof u->proj);
    u->misc[0] = f->rt_near;
    u->misc[1] = (float)w / (float)f->w;
    u->misc[2] = (float)f->alpha_ref;
    u->misc[3] = (float)f->shadow_scale / 256.0f;
    float dir[3];
    if (f->shadow_scale < 256 && shadow_end(&sf, dir)) {
        memcpy(u->shadow_dir, dir, sizeof dir);
        u->shadow_dir[3] = 1;
    }
    u->size[0] = w; u->size[1] = h; u->size[2] = q->ambient; u->size[3] = q->shadow;
    for (int g = 0; g < 4; g++)
        u->base[g] = (Sint32)s->base[g];
    u->dist[0] = 3.0f;          /* the ambient rays' reach, in the scene's units (a fighter is about 1.7 tall) */
    u->dist[1] = 12.0f;         /* the shadow rays' */
    u->dist[2] = 0.03f;         /* the shadow light's radius: a soft edge that sharpens at the feet */
    u->dist[3] = 0.12f;         /* a lamp's */
    u->tune[0] = 0.003f;        /* how far off the surface a ray starts */
    u->tune[1] = 0.7f;          /* how much of the occlusion a surface with painted light takes */
    u->tune[2] = 1.25f;         /* the most a pixel is brightened (bounced light) */
    u->tune[3] = 1.0f;
    u->opt[0] = q->reflection;
    u->opt[1] = q->bounce;
    u->opt[2] = 1;
    u->opt[3] = 1;              /* the same rays every frame: what is still does not flicker */
    u->tune2[0] = 0.01f;
    u->tune2[1] = (float)SDL_GetAtomicInt(&water_over) / 100.0f;    /* section 90 */
    u->counts[0] = (Sint32)texels;
    u->counts[1] = q->smooth;
    double ml = sqrt(mirror_n[0] * mirror_n[0] + mirror_n[1] * mirror_n[1] + mirror_n[2] * mirror_n[2]);
    if (ml > 0) {
        for (int x = 0; x < 3; x++) {
            u->mirror[x] = (float)(mirror_n[x] / ml);
            u->mirror_bg[x] = f->rt_mirror_bg[x];
        }
        u->mirror[3] = (float)f->rt_mirror;
        u->mirror_fog[0] = (float)f->rt_fog_at;
        if (mirror_a > 0)
            u->mirror_bg[3] = (float)((u->mirror[0] * mirror_c[0] + u->mirror[1] * mirror_c[1] + u->mirror[2] * mirror_c[2]) /
                                      mirror_a);
    }
}
