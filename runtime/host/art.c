/* Steam's artwork from the player's own build (art.h; docs/design.md
 * section 36). The scenes are vertical blanks of the attract sequence of a
 * blank cabinet with no input, which is the same run on every machine. */
#include "art.h"

#include <math.h>
#include <string.h>

/* a picture to make: a crop of one vertical blank's picture, in the game's
 * 640 x 480 units, scaled to Steam's size */
typedef struct {
    unsigned frame;
    const char *name;           /* the file in the --art folder */
    float x, y, w, h;
    int out_w, out_h;
} art_piece;

/* Steam's slots (steam_shortcut.py --art puts them in place): the library's
 * capsule (600 x 900), the wide capsule (920 x 430), the hero banner behind
 * the game's page (1920 x 620) and the shortcut's icon */
static const art_piece pieces[] = {
    /* the first demo's close-up of Akira (ADVERTISE.DEMO_DISP): the crops
     * keep clear of FREE PLAY above, PRESS START BUTTON below and his name
     * to the right */
    { 5300, "capsule.png", 118.0f, 40.0f, 240.0f, 360.0f, 600, 900 },
    { 5300, "icon.png", 202.0f, 32.0f, 150.0f, 150.0f, 256, 256 },
    /* the title over its flames (ADVERTISE.TITLE_DISP) */
    { 12050, "wide.png", 0.0f, 80.0f, 640.0f, 299.1f, 920, 430 },
    { 12050, "hero.png", 0.0f, 124.0f, 640.0f, 206.6f, 1920, 620 },
};

#define FRAMES_MAX 256
static unsigned list[FRAMES_MAX];
static int nlist, whole;        /* whole: --art-frames, whole pictures */

int art_frames_parse(const char *spec)
{
    char buf[2048];
    SDL_strlcpy(buf, spec, sizeof buf);
    char *save = NULL;
    for (char *it = SDL_strtok_r(buf, ",", &save); it; it = SDL_strtok_r(NULL, ",", &save)) {
        char *end = NULL;
        unsigned a = (unsigned)SDL_strtoul(it, &end, 10), b = a, step = 1;
        if (end == it)
            return -1;
        if (*end == '-') {
            char *e2 = NULL;
            b = (unsigned)SDL_strtoul(end + 1, &e2, 10);
            if (e2 == end + 1 || b < a)
                return -1;
            end = e2;
            if (*end == '/') {
                step = (unsigned)SDL_strtoul(end + 1, &e2, 10);
                if (e2 == end + 1 || !step)
                    return -1;
                end = e2;
            }
        }
        if (*end)
            return -1;
        for (unsigned f = a; f <= b; f += step) {
            if (nlist == FRAMES_MAX)
                return -1;
            list[nlist++] = f;
        }
    }
    whole = 1;
    return 0;
}

static int by_value(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

const unsigned *art_frames(int *n)
{
    if (!whole && !nlist) {
        for (size_t i = 0; i < SDL_arraysize(pieces); i++) {
            int seen = 0;
            for (int j = 0; j < nlist; j++)
                seen |= list[j] == pieces[i].frame;
            if (!seen)
                list[nlist++] = pieces[i].frame;
        }
    }
    SDL_qsort(list, (size_t)nlist, sizeof list[0], by_value);
    *n = nlist;
    return list;
}

/* sRGB to linear light and back: the average of a pixel's area is taken in
 * linear light, so a downscale keeps its brightness */
static float lin[256];

static void lin_init(void)
{
    if (lin[255] > 0.0f)
        return;
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        lin[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
}

static Uint8 to_srgb(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    float c = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (Uint8)(c * 255.0f + 0.5f);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* the area (x0, y0, cw, ch) of src (sw x sh RGBA, in its own pixels) to dst
 * (dw x dh RGB, opaque): each output pixel is the average of the source area
 * it covers, by how much of each source pixel it covers */
static int area_scale(const Uint8 *src, int sw, int sh, double x0, double y0, double cw, double ch, Uint8 *dst,
                      int dw, int dh)
{
    int ry0 = (int)floor(y0), ry1 = (int)ceil(y0 + ch);
    int rows = ry1 - ry0;
    float *tmp = SDL_malloc((size_t)rows * (size_t)dw * 3 * sizeof(float));
    if (!tmp)
        return -1;
    double fx = cw / dw, fy = ch / dh;
    for (int ox = 0; ox < dw; ox++) {
        double a = x0 + ox * fx, b = a + fx;
        for (int r = 0; r < rows; r++) {
            const Uint8 *row = src + (size_t)clampi(ry0 + r, 0, sh - 1) * (size_t)sw * 4;
            float acc[3] = { 0, 0, 0 };
            double wsum = 0;
            for (int sx = (int)floor(a); sx < (int)ceil(b); sx++) {
                double wgt = fmin(b, sx + 1.0) - fmax(a, (double)sx);
                if (wgt <= 0)
                    continue;
                const Uint8 *p = row + (size_t)clampi(sx, 0, sw - 1) * 4;
                for (int c = 0; c < 3; c++)
                    acc[c] += lin[p[c]] * (float)wgt;
                wsum += wgt;
            }
            for (int c = 0; c < 3; c++)
                tmp[((size_t)r * (size_t)dw + (size_t)ox) * 3 + (size_t)c] = wsum > 0 ? acc[c] / (float)wsum : 0.0f;
        }
    }
    for (int oy = 0; oy < dh; oy++) {
        double a = y0 + oy * fy, b = a + fy;
        for (int ox = 0; ox < dw; ox++) {
            float acc[3] = { 0, 0, 0 };
            double wsum = 0;
            for (int sy = (int)floor(a); sy < (int)ceil(b); sy++) {
                double wgt = fmin(b, sy + 1.0) - fmax(a, (double)sy);
                if (wgt <= 0)
                    continue;
                const float *p = tmp + ((size_t)clampi(sy - ry0, 0, rows - 1) * (size_t)dw + (size_t)ox) * 3;
                for (int c = 0; c < 3; c++)
                    acc[c] += p[c] * (float)wgt;
                wsum += wgt;
            }
            Uint8 *o = dst + ((size_t)oy * (size_t)dw + (size_t)ox) * 3;
            for (int c = 0; c < 3; c++)
                o[c] = to_srgb(wsum > 0 ? acc[c] / (float)wsum : 0.0f);
        }
    }
    SDL_free(tmp);
    return 0;
}

static int save_png(const char *dir, const char *name, Uint8 *rgb, int w, int h, void (*say)(const char *))
{
    char path[4300], msg[4500];
    size_t dl = strlen(dir);
    SDL_snprintf(path, sizeof path, "%s%s%s", dir, dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\') ? "" : "/", name);
    SDL_Surface *sf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB24, rgb, w * 3);
    int ok = sf && SDL_SavePNG(sf, path);
    if (ok)
        SDL_snprintf(msg, sizeof msg, "art: %s (%dx%d)", path, w, h);
    else
        SDL_snprintf(msg, sizeof msg, "art: cannot write %s: %s", path, SDL_GetError());
    say(msg);
    SDL_DestroySurface(sf);
    return ok;
}

int art_take(const char *dir, unsigned frame, const void *rgba, Uint32 w, Uint32 h, void (*say)(const char *msg))
{
    int made = 0;
    if (!rgba) {
        say("art: the picture could not be read back");
        return 0;
    }
    lin_init();
    if (whole) {
        /* the whole picture, opaque, for choosing scenes */
        Uint8 *rgb = SDL_malloc((size_t)w * h * 3);
        if (!rgb)
            return 0;
        const Uint8 *s = rgba;
        for (size_t i = 0; i < (size_t)w * h; i++)
            memcpy(rgb + i * 3, s + i * 4, 3);
        char name[64];
        SDL_snprintf(name, sizeof name, "frame-%05u.png", frame);
        made += save_png(dir, name, rgb, (int)w, (int)h, say);
        SDL_free(rgb);
        return made;
    }
    double sx = w / 640.0, sy = h / 480.0;
    for (size_t i = 0; i < SDL_arraysize(pieces); i++) {
        const art_piece *p = &pieces[i];
        if (p->frame != frame)
            continue;
        Uint8 *rgb = SDL_malloc((size_t)p->out_w * (size_t)p->out_h * 3);
        if (rgb && !area_scale(rgba, (int)w, (int)h, p->x * sx, p->y * sy, p->w * sx, p->h * sy, rgb, p->out_w,
                               p->out_h))
            made += save_png(dir, p->name, rgb, p->out_w, p->out_h, say);
        SDL_free(rgb);
    }
    return made;
}
