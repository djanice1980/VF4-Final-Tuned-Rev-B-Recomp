/* The drawing kit the launcher and the game's menu share: see ui.h. */
#include "ui.h"

#include <string.h>

/* stb_truetype (runtime/ui/stb_truetype.h, v1.26, public domain or MIT):
 * reads the fonts and draws their glyphs into the sheets */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ((void)(u), SDL_malloc(x))
#define STBTT_free(x, u) ((void)(u), SDL_free(x))
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* fonts.c (tools/make_fonts.py) */
extern const unsigned char ui_font_regular[], ui_font_medium[], ui_font_bold[], ui_font_mono[];

static SDL_Renderer *ren;
static float sc = 1, ox, oy;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float px(float logical_x)
{
    return SDL_roundf(ox + logical_x * sc);
}

static float py(float logical_y)
{
    return SDL_roundf(oy + logical_y * sc);
}

/* ---- the fonts ---- */

static const unsigned char *const face_data[UI_FACES] = { ui_font_regular, ui_font_medium, ui_font_bold, ui_font_mono };
static stbtt_fontinfo face_info[UI_FACES];
static float face_cap[UI_FACES];        /* the capitals' height, in ems */
static float face_line[UI_FACES];       /* ascent - descent + line gap, in ems */

/* the characters the sheets have: tools/make_fonts.py's TEXT */
static const struct { int first, n; } ranges[] = {
    { 0x20, 95 }, { 0xA0, 96 }, { 0x2010, 24 }, { 0x2030, 11 }, { 0x20AC, 1 }, { 0x2122, 1 },
};
#define NRANGES ((int)SDL_arraysize(ranges))
#define NCHARS (95 + 96 + 24 + 11 + 1 + 1)

static int char_index(int cp)
{
    int base = 0;
    for (int i = 0; i < NRANGES; i++) {
        if (cp >= ranges[i].first && cp < ranges[i].first + ranges[i].n)
            return base + cp - ranges[i].first;
        base += ranges[i].n;
    }
    return -1;
}

/* a sheet: one face's glyphs at one pixel size */
typedef struct sheet {
    int face, size;
    float em;                   /* stb_truetype's scale for this size */
    SDL_Texture *tex;
    stbtt_packedchar ch[NCHARS];
} sheet;

#define SHEETS 20
static sheet *sheets[SHEETS];
static int nsheets;

static sheet *bake(int face, int size)
{
    sheet *s = SDL_calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->face = face;
    s->size = size;
    s->em = stbtt_ScaleForMappingEmToPixels(&face_info[face], (float)size);
    int side = 128;
    while ((float)side * (float)side < 1.1f * (float)size * (float)size * NCHARS)
        side *= 2;
    unsigned char *bm = NULL;
    for (;; side *= 2) {
        SDL_free(bm);
        bm = SDL_calloc((size_t)side, (size_t)side);
        if (!bm)
            break;
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, bm, side, side, 0, 1, NULL))
            break;
        stbtt_PackSetOversampling(&pc, 1, 1);
        stbtt_pack_range rr[NRANGES];
        int base = 0;
        for (int i = 0; i < NRANGES; i++) {
            SDL_zero(rr[i]);
            rr[i].font_size = STBTT_POINT_SIZE((float)size);
            rr[i].first_unicode_codepoint_in_range = ranges[i].first;
            rr[i].num_chars = ranges[i].n;
            rr[i].chardata_for_range = s->ch + base;
            base += ranges[i].n;
        }
        int ok = stbtt_PackFontRanges(&pc, face_data[face], 0, rr, NRANGES);
        stbtt_PackEnd(&pc);
        if (ok || side >= 4096)
            break;
    }
    SDL_Surface *sf = bm ? SDL_CreateSurface(side, side, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (sf) {
        for (int y = 0; y < side; y++) {
            Uint8 *d = (Uint8 *)sf->pixels + (size_t)y * sf->pitch;
            const unsigned char *a = bm + (size_t)y * side;
            for (int x = 0; x < side; x++) {
                d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = 255;
                d[4 * x + 3] = a[x];
            }
        }
        s->tex = SDL_CreateTextureFromSurface(ren, sf);
        SDL_DestroySurface(sf);
    }
    SDL_free(bm);
    if (!s->tex) {
        SDL_free(s);
        return NULL;
    }
    SDL_SetTextureBlendMode(s->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(s->tex, SDL_SCALEMODE_NEAREST);
    return s;
}

static sheet *get_sheet(int face, float size)
{
    int p = (int)SDL_lroundf(size * sc);
    if (p < 5)
        p = 5;
    if (p > 160)
        p = 160;
    for (int i = 0; i < nsheets; i++)
        if (sheets[i]->face == face && sheets[i]->size == p)
            return sheets[i];
    if (nsheets == SHEETS) {            /* the oldest goes */
        SDL_DestroyTexture(sheets[0]->tex);
        SDL_free(sheets[0]);
        memmove(sheets, sheets + 1, sizeof sheets[0] * (SHEETS - 1));
        nsheets--;
    }
    sheet *s = bake(face, p);
    if (s)
        sheets[nsheets++] = s;
    return s;
}

/* the next character of UTF-8 text (a bad byte is U+FFFD) */
static int next_cp(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    int c = s[0], n = 0, cp;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
    else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else { cp = 0xFFFD; n = 1; }
    *ps += n;
    return cp;
}

/* draws (when draw) n bytes of s; the width in pixels */
static float run_text(sheet *sh, float x_px, float base_px, const char *s, size_t n, int draw)
{
    const char *end = s + n;
    float pen = x_px;
    int prev = 0;
    while (s < end && *s) {
        int cp = next_cp(&s);
        int idx = char_index(cp);
        if (idx < 0) {
            cp = '?';
            idx = char_index('?');
        }
        if (prev)
            pen += (float)stbtt_GetCodepointKernAdvance(&face_info[sh->face], prev, cp) * sh->em;
        const stbtt_packedchar *q = &sh->ch[idx];
        if (draw && q->x1 > q->x0) {
            SDL_FRect src = { q->x0, q->y0, (float)(q->x1 - q->x0), (float)(q->y1 - q->y0) };
            SDL_FRect dst = { SDL_roundf(pen + q->xoff), base_px + SDL_roundf(q->yoff), src.w, src.h };
            SDL_RenderTexture(ren, sh->tex, &src, &dst);
        }
        pen += q->xadvance;
        prev = cp;
    }
    return pen - x_px;
}

static float baseline(int face, sheet *sh, float y)
{
    return SDL_roundf(oy + y * sc + face_cap[face] * (float)sh->size / 2);
}

float ui_text(int face, float size, float x, float y, SDL_Color c, const char *s)
{
    sheet *sh = get_sheet(face, size);
    if (!sh || !s)
        return 0;
    SDL_SetTextureColorMod(sh->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(sh->tex, c.a);
    return run_text(sh, px(x), baseline(face, sh, y), s, strlen(s), 1) / sc;
}

float ui_text_w(int face, float size, const char *s)
{
    sheet *sh = get_sheet(face, size);
    return sh && s ? run_text(sh, 0, 0, s, strlen(s), 0) / sc : 0;
}

float ui_line_h(int face, float size)
{
    return face_line[face] * size;
}

void ui_text_in(int face, float size, float x, float y, float w, int align, int cut_start, SDL_Color c,
                const char *s)
{
    sheet *sh = get_sheet(face, size);
    if (!sh || !s)
        return;
    size_t n = strlen(s);
    float wp = w * sc, tw = run_text(sh, 0, 0, s, n, 0);
    SDL_SetTextureColorMod(sh->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(sh->tex, c.a);
    float b = baseline(face, sh, y), x0 = px(x);
    if (tw <= wp) {
        float at = align == 1 ? x0 + SDL_roundf((wp - tw) / 2) : align == 2 ? x0 + wp - tw : x0;
        run_text(sh, at, b, s, n, 1);
        return;
    }
    static const char ell[] = "\xE2\x80\xA6";
    float ew = run_text(sh, 0, 0, ell, 3, 0);
    if (cut_start) {
        /* the longest end that fits after the ellipsis */
        const char *t = s;
        while (*t && run_text(sh, 0, 0, t, strlen(t), 0) + ew > wp)
            next_cp(&t);
        float at = x0 + run_text(sh, x0, b, ell, 3, 1);
        run_text(sh, at, b, t, strlen(t), 1);
    } else {
        const char *t = s, *fit = s;
        while (*t) {
            const char *u = t;
            next_cp(&u);
            if (run_text(sh, 0, 0, s, (size_t)(u - s), 0) + ew > wp)
                break;
            fit = t = u;
        }
        float at = x0 + run_text(sh, x0, b, s, (size_t)(fit - s), 1);
        run_text(sh, at, b, ell, 3, 1);
    }
}

static int wrap(int face, float size, float x, float y, float w, float line_h, SDL_Color c, const char *s,
                int draw)
{
    sheet *sh = get_sheet(face, size);
    if (!sh || !s)
        return 0;
    if (draw) {
        SDL_SetTextureColorMod(sh->tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(sh->tex, c.a);
    }
    float wp = w * sc;
    int lines = 0;
    while (*s) {
        /* the most words that fit (at least one) */
        const char *end = s, *p = s;
        while (*p && *p != '\n') {
            const char *q = p;
            while (*q == ' ')
                q++;
            while (*q && *q != ' ' && *q != '\n')
                q++;
            if (end != s && run_text(sh, 0, 0, s, (size_t)(q - s), 0) > wp)
                break;
            end = p = q;
        }
        if (end == s)           /* an empty line */
            end = p;
        if (draw)
            run_text(sh, px(x), baseline(face, sh, y + (float)lines * line_h), s, (size_t)(end - s), 1);
        lines++;
        s = end;
        while (*s == ' ')
            s++;
        if (*s == '\n')
            s++;
    }
    return lines;
}

int ui_wrap(int face, float size, float x, float y, float w, float line_h, SDL_Color c, const char *s)
{
    return wrap(face, size, x, y, w, line_h, c, s, 1);
}

int ui_wrap_lines(int face, float size, float w, const char *s)
{
    return wrap(face, size, 0, 0, w, 0, (SDL_Color){ 0, 0, 0, 0 }, s, 0);
}

/* ---- shapes ---- */

static void px_fill(float x0, float y0, float x1, float y1, SDL_Color c)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren, &(SDL_FRect){ x0, y0, x1 - x0, y1 - y0 });
}

void ui_fill(float x, float y, float w, float h, SDL_Color c)
{
    px_fill(px(x), py(y), px(x + w), py(y + h), c);
}

/* disks of r pixels: their quarters are the corners of rounded boxes */
#define DISKS 32
static struct { int r; SDL_Texture *t; } disks[DISKS];
static int ndisks;

/* a texture from a coverage function over w x h pixels */
static SDL_Texture *coverage(int w, int h, float (*f)(int i, int j, const void *ctx), const void *ctx)
{
    SDL_Surface *sf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!sf)
        return NULL;
    for (int j = 0; j < h; j++) {
        Uint8 *d = (Uint8 *)sf->pixels + (size_t)j * sf->pitch;
        for (int i = 0; i < w; i++) {
            d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = 255;
            d[4 * i + 3] = (Uint8)SDL_lroundf(255 * clampf(f(i, j, ctx), 0, 1));
        }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf);
    SDL_DestroySurface(sf);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

static float disk_cov(int i, int j, const void *ctx)
{
    float r = (float)*(const int *)ctx;
    float dx = (float)i + 0.5f - r, dy = (float)j + 0.5f - r;
    return r - SDL_sqrtf(dx * dx + dy * dy) + 0.5f;
}

static SDL_Texture *disk(int r)
{
    for (int i = 0; i < ndisks; i++)
        if (disks[i].r == r)
            return disks[i].t;
    if (ndisks == DISKS) {
        SDL_DestroyTexture(disks[0].t);
        memmove(disks, disks + 1, sizeof disks[0] * (DISKS - 1));
        ndisks--;
    }
    SDL_Texture *t = coverage(2 * r, 2 * r, disk_cov, &r);
    if (t) {
        disks[ndisks].r = r;
        disks[ndisks++].t = t;
    }
    return t;
}

static void px_round(float x0, float y0, float x1, float y1, float rf, SDL_Color c)
{
    float w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    int r = (int)SDL_lroundf(rf);
    if (r > (int)(w / 2))
        r = (int)(w / 2);
    if (r > (int)(h / 2))
        r = (int)(h / 2);
    SDL_Texture *t = r >= 1 ? disk(r) : NULL;
    if (!t) {
        px_fill(x0, y0, x1, y1, c);
        return;
    }
    float R = (float)r;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_RenderTexture(ren, t, &(SDL_FRect){ 0, 0, R, R }, &(SDL_FRect){ x0, y0, R, R });
    SDL_RenderTexture(ren, t, &(SDL_FRect){ R, 0, R, R }, &(SDL_FRect){ x1 - R, y0, R, R });
    SDL_RenderTexture(ren, t, &(SDL_FRect){ 0, R, R, R }, &(SDL_FRect){ x0, y1 - R, R, R });
    SDL_RenderTexture(ren, t, &(SDL_FRect){ R, R, R, R }, &(SDL_FRect){ x1 - R, y1 - R, R, R });
    px_fill(x0 + R, y0, x1 - R, y1, c);
    px_fill(x0, y0 + R, x0 + R, y1 - R, c);
    px_fill(x1 - R, y0 + R, x1, y1 - R, c);
}

void ui_round(float x, float y, float w, float h, float r, SDL_Color c)
{
    px_round(px(x), py(y), px(x + w), py(y + h), r * sc, c);
}

void ui_round_line(float x, float y, float w, float h, float r, float t, SDL_Color line, SDL_Color fill)
{
    float x0 = px(x), y0 = py(y), x1 = px(x + w), y1 = py(y + h);
    float T = SDL_roundf(t * sc);
    if (T < 1)
        T = 1;
    px_round(x0, y0, x1, y1, r * sc, line);
    px_round(x0 + T, y0 + T, x1 - T, y1 - T, r * sc - T > 0 ? r * sc - T : 0, fill);
}

void ui_image(SDL_Texture *t, float x, float y, float w, float h)
{
    float x0 = px(x), y0 = py(y);
    SDL_RenderTexture(ren, t, NULL, &(SDL_FRect){ x0, y0, px(x + w) - x0, py(y + h) - y0 });
}

/* PNGs made into textures, by their data (a failed one is kept as NULL, so
 * it is not tried again every frame) */
static struct { const unsigned char *png; SDL_Texture *t; } pngs[4];
static int npngs;

int ui_png(const unsigned char *png, size_t n, float x, float y, float w, float h)
{
    int i = 0;
    while (i < npngs && pngs[i].png != png)
        i++;
    if (i == npngs) {
        if (npngs == (int)SDL_arraysize(pngs))
            return 0;
        SDL_Texture *t = NULL;
#if SDL_VERSION_ATLEAST(3, 4, 0)
        SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(png, n), true);
        if (s) {
            t = SDL_CreateTextureFromSurface(ren, s);
            SDL_DestroySurface(s);
            if (t)
                SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        }
#else
        (void)n;
#endif
        pngs[npngs].png = png;
        pngs[npngs++].t = t;
    }
    if (!pngs[i].t)
        return 0;
    ui_image(pngs[i].t, x, y, w, h);
    return 1;
}

/* ---- icons: distance functions, in units of the icon's box (0..1) ---- */

typedef struct { float x, y; } v2;

static float len2(float x, float y)
{
    return SDL_sqrtf(x * x + y * y);
}

static float seg(v2 p, v2 a, v2 b)
{
    float ex = b.x - a.x, ey = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
    return len2(wx - ex * t, wy - ey * t);
}

/* a polygon's signed distance (negative inside): Inigo Quilez's method */
static float poly(v2 p, const v2 *v, int n)
{
    float d = (p.x - v[0].x) * (p.x - v[0].x) + (p.y - v[0].y) * (p.y - v[0].y), s = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = v[j].x - v[i].x, ey = v[j].y - v[i].y, wx = p.x - v[i].x, wy = p.y - v[i].y;
        float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
        float bx = wx - ex * t, by = wy - ey * t;
        if (bx * bx + by * by < d)
            d = bx * bx + by * by;
        int c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
            s = -s;
    }
    return s * SDL_sqrtf(d);
}

/* a box's signed distance: center c, half sizes hx hy, corners rounded by r */
static float box(v2 p, v2 c, float hx, float hy, float r)
{
    float qx = SDL_fabsf(p.x - c.x) - hx + r, qy = SDL_fabsf(p.y - c.y) - hy + r;
    return len2(SDL_max(qx, 0), SDL_max(qy, 0)) + SDL_min(SDL_max(qx, qy), 0) - r;
}

static float icon_dist(int icon, v2 p)
{
    switch (icon) {
    case UI_I_LEFT: { v2 v[] = { { 0.70f, 0.20f }, { 0.70f, 0.80f }, { 0.22f, 0.50f } }; return poly(p, v, 3); }
    case UI_I_RIGHT: { v2 v[] = { { 0.30f, 0.20f }, { 0.78f, 0.50f }, { 0.30f, 0.80f } }; return poly(p, v, 3); }
    case UI_I_UP: { v2 v[] = { { 0.20f, 0.70f }, { 0.50f, 0.22f }, { 0.80f, 0.70f } }; return poly(p, v, 3); }
    case UI_I_DOWN: { v2 v[] = { { 0.20f, 0.30f }, { 0.80f, 0.30f }, { 0.50f, 0.78f } }; return poly(p, v, 3); }
    case UI_I_PLAY: { v2 v[] = { { 0.30f, 0.17f }, { 0.84f, 0.50f }, { 0.30f, 0.83f } }; return poly(p, v, 3); }
    case UI_I_CHECK:
        return SDL_min(seg(p, (v2){ 0.20f, 0.52f }, (v2){ 0.42f, 0.73f }),
                       seg(p, (v2){ 0.42f, 0.73f }, (v2){ 0.81f, 0.29f })) - 0.07f;
    case UI_I_CHEVRON:
        return SDL_min(seg(p, (v2){ 0.38f, 0.20f }, (v2){ 0.64f, 0.50f }),
                       seg(p, (v2){ 0.64f, 0.50f }, (v2){ 0.38f, 0.80f })) - 0.055f;
    case UI_I_FAIL:
        return SDL_min(seg(p, (v2){ 0.27f, 0.27f }, (v2){ 0.73f, 0.73f }),
                       seg(p, (v2){ 0.73f, 0.27f }, (v2){ 0.27f, 0.73f })) - 0.07f;
    case UI_I_DOT: return len2(p.x - 0.5f, p.y - 0.5f) - 0.22f;
    case UI_I_PS_CROSS:
        return SDL_min(seg(p, (v2){ 0.25f, 0.25f }, (v2){ 0.75f, 0.75f }),
                       seg(p, (v2){ 0.75f, 0.25f }, (v2){ 0.25f, 0.75f })) - 0.045f;
    case UI_I_PS_CIRCLE: return SDL_fabsf(len2(p.x - 0.5f, p.y - 0.5f) - 0.26f) - 0.045f;
    case UI_I_PS_SQUARE: {
        float qx = SDL_fabsf(p.x - 0.5f) - 0.24f, qy = SDL_fabsf(p.y - 0.5f) - 0.24f;
        float d = len2(SDL_max(qx, 0), SDL_max(qy, 0)) + SDL_min(SDL_max(qx, qy), 0);
        return SDL_fabsf(d) - 0.045f;
    }
    case UI_I_PS_TRIANGLE: {
        v2 v[] = { { 0.50f, 0.20f }, { 0.81f, 0.73f }, { 0.19f, 0.73f } };
        return SDL_fabsf(poly(p, v, 3)) - 0.045f;
    }
    case UI_I_VIEW: {               /* two windows, the back one behind the front one */
        float f = box(p, (v2){ 0.585f, 0.585f }, 0.195f, 0.155f, 0.04f), k = box(p, (v2){ 0.415f, 0.415f }, 0.195f, 0.155f, 0.04f);
        return SDL_min(SDL_fabsf(f) - 0.04f, SDL_max(SDL_fabsf(k) - 0.04f, -(f + 0.08f)));
    }
    case UI_I_MENU:
        return SDL_min(SDL_min(seg(p, (v2){ 0.27f, 0.32f }, (v2){ 0.73f, 0.32f }),
                               seg(p, (v2){ 0.27f, 0.50f }, (v2){ 0.73f, 0.50f })),
                       seg(p, (v2){ 0.27f, 0.68f }, (v2){ 0.73f, 0.68f })) - 0.045f;
    case UI_I_PLUS:
        return SDL_min(seg(p, (v2){ 0.26f, 0.50f }, (v2){ 0.74f, 0.50f }),
                       seg(p, (v2){ 0.50f, 0.26f }, (v2){ 0.50f, 0.74f })) - 0.05f;
    case UI_I_MINUS: return seg(p, (v2){ 0.26f, 0.50f }, (v2){ 0.74f, 0.50f }) - 0.05f;
    case UI_I_HOME: {
        v2 v[] = { { 0.50f, 0.17f }, { 0.83f, 0.48f }, { 0.735f, 0.48f }, { 0.735f, 0.80f },
                   { 0.265f, 0.80f }, { 0.265f, 0.48f }, { 0.17f, 0.48f } };
        return poly(p, v, 7);
    }
    case UI_I_CAPTURE:
        return SDL_min(SDL_fabsf(box(p, (v2){ 0.5f, 0.5f }, 0.32f, 0.32f, 0.08f)) - 0.05f,
                       len2(p.x - 0.5f, p.y - 0.5f) - 0.14f);
    case UI_I_DOTS:
        return SDL_min(SDL_min(len2(p.x - 0.22f, p.y - 0.5f), len2(p.x - 0.5f, p.y - 0.5f)),
                       len2(p.x - 0.78f, p.y - 0.5f)) - 0.09f;
    }
    return 1;
}

typedef struct { int icon, size; } icon_ctx;

static float icon_cov(int i, int j, const void *ctx)
{
    const icon_ctx *c = ctx;
    float s = (float)c->size;
    v2 p = { ((float)i + 0.5f) / s, ((float)j + 0.5f) / s };
    return 0.5f - icon_dist(c->icon, p) * s;
}

#define ICONS 64
static struct { int icon, size; SDL_Texture *t; } icons[ICONS];
static int nicons;

void ui_icon(int icon, float cx, float cy, float size, SDL_Color c)
{
    int s = (int)SDL_lroundf(size * sc);
    if (s < 4)
        s = 4;
    SDL_Texture *t = NULL;
    for (int i = 0; i < nicons && !t; i++)
        if (icons[i].icon == icon && icons[i].size == s)
            t = icons[i].t;
    if (!t) {
        if (nicons == ICONS) {
            SDL_DestroyTexture(icons[0].t);
            memmove(icons, icons + 1, sizeof icons[0] * (ICONS - 1));
            nicons--;
        }
        icon_ctx ctx = { icon, s };
        t = coverage(s, s, icon_cov, &ctx);
        if (!t)
            return;
        icons[nicons].icon = icon;
        icons[nicons].size = s;
        icons[nicons++].t = t;
    }
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    float x0 = SDL_roundf(ox + cx * sc - (float)s / 2), y0 = SDL_roundf(oy + cy * sc - (float)s / 2);
    SDL_RenderTexture(ren, t, NULL, &(SDL_FRect){ x0, y0, (float)s, (float)s });
}

/* ---- set-up ---- */

int ui_init(SDL_Renderer *r)
{
    ren = r;
    for (int f = 0; f < UI_FACES; f++) {
        if (!stbtt_InitFont(&face_info[f], face_data[f], stbtt_GetFontOffsetForIndex(face_data[f], 0)))
            return SDL_SetError("the UI font %d is damaged", f), -1;
        int asc, desc, gap, x0, y0, x1, y1;
        stbtt_GetFontVMetrics(&face_info[f], &asc, &desc, &gap);
        float em = stbtt_ScaleForMappingEmToPixels(&face_info[f], 1);
        face_line[f] = (float)(asc - desc + gap) * em;
        face_cap[f] = stbtt_GetCodepointBox(&face_info[f], 'H', &x0, &y0, &x1, &y1) ? (float)y1 * em : 0.7f;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    return 0;
}

void ui_quit(void)
{
    for (int i = 0; i < nsheets; i++) {
        SDL_DestroyTexture(sheets[i]->tex);
        SDL_free(sheets[i]);
    }
    for (int i = 0; i < ndisks; i++)
        SDL_DestroyTexture(disks[i].t);
    for (int i = 0; i < nicons; i++)
        SDL_DestroyTexture(icons[i].t);
    for (int i = 0; i < npngs; i++)
        if (pngs[i].t)
            SDL_DestroyTexture(pngs[i].t);
    nsheets = ndisks = nicons = npngs = 0;
    ren = NULL;
}

void ui_set_scale(float scale, float x, float y)
{
    sc = scale > 0.05f ? scale : 0.05f;
    ox = x;
    oy = y;
}

float ui_get_scale(void)
{
    return sc;
}

void ui_fit(float w, float h, int pw, int ph)
{
    float s = SDL_min((float)pw / w, (float)ph / h);
    ui_set_scale(s, SDL_floorf(((float)pw - w * s) / 2), SDL_floorf(((float)ph - h * s) / 2));
}

void ui_to_logical(float x, float y, float *lx, float *ly)
{
    *lx = (x - ox) / sc;
    *ly = (y - oy) / sc;
}

/* ---- input glyphs ---- */

/* how each style names a gamepad's inputs (UI_DEV_*): an icon (UI_I_*) or
 * a label; the face buttons take their labels from SDL by the style's type */
typedef struct pad_style {
    const char *name;
    SDL_GamepadType type;       /* for SDL_GetGamepadButtonLabelForType */
    int colors;                 /* face buttons: 0 plain, 1 Xbox letters, 2 PlayStation shapes, 3 GameCube */
    const char *shoulder[2], *trigger[2], *stick[2];
    const char *back, *start, *guide, *misc1, *touchpad;
    int back_icon, start_icon, guide_icon, misc1_icon;
    const char *paddle[4];      /* SDL's paddle1..4: right upper, left upper, right lower, left lower */
} pad_style;

#define NO -1
static const pad_style styles[UI_DEVS] = {
    [UI_DEV_XBOX] = { "xbox", SDL_GAMEPAD_TYPE_XBOXONE, 1, { "LB", "RB" }, { "LT", "RT" }, { "LS", "RS" },
                      NULL, NULL, "Guide", "Share", NULL, UI_I_VIEW, UI_I_MENU, NO, NO, { "P1", "P3", "P2", "P4" } },
    [UI_DEV_XBOX360] = { "xbox360", SDL_GAMEPAD_TYPE_XBOX360, 1, { "LB", "RB" }, { "LT", "RT" }, { "LS", "RS" },
                         "Back", "Start", "Guide", "Misc", NULL, NO, NO, NO, NO, { "P1", "P3", "P2", "P4" } },
    [UI_DEV_PS] = { "ps5", SDL_GAMEPAD_TYPE_PS5, 0, { "L1", "R1" }, { "L2", "R2" }, { "L3", "R3" },
                    "Create", "Options", "PS", "Mute", "Touchpad", NO, NO, NO, NO, { "RB", "LB", "R Fn", "L Fn" } },
    [UI_DEV_PS4] = { "ps4", SDL_GAMEPAD_TYPE_PS4, 2, { "L1", "R1" }, { "L2", "R2" }, { "L3", "R3" },
                     "Share", "Options", "PS", "Misc", "Touchpad", NO, NO, NO, NO, { "P1", "P3", "P2", "P4" } },
    [UI_DEV_PS3] = { "ps3", SDL_GAMEPAD_TYPE_PS3, 2, { "L1", "R1" }, { "L2", "R2" }, { "L3", "R3" },
                     "Select", "Start", "PS", "Misc", NULL, NO, NO, NO, NO, { "P1", "P3", "P2", "P4" } },
    [UI_DEV_NINTENDO] = { "switch", SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, 0, { "L", "R" }, { "ZL", "ZR" },
                          { "LS", "RS" }, NULL, NULL, NULL, NULL, NULL, UI_I_MINUS, UI_I_PLUS, UI_I_HOME,
                          UI_I_CAPTURE, { "SR", "SL", "SL", "SR" } },
    [UI_DEV_GAMECUBE] = { "gamecube", SDL_GAMEPAD_TYPE_GAMECUBE, 3, { "ZL", "Z" }, { "L", "R" }, { "LS", "RS" },
                          "Back", "Start", NULL, NULL, NULL, NO, NO, UI_I_HOME, UI_I_CAPTURE,
                          { "P1", "P3", "P2", "P4" } },
    [UI_DEV_STEAM] = { "steam", SDL_GAMEPAD_TYPE_STEAM, 0, { "L1", "R1" }, { "L2", "R2" }, { "L3", "R3" },
                       NULL, NULL, "Steam", NULL, NULL, UI_I_VIEW, UI_I_MENU, NO, UI_I_DOTS,
                       { "R4", "L4", "R5", "L5" } },
    [UI_DEV_GENERIC] = { "generic", SDL_GAMEPAD_TYPE_STANDARD, 0, { "LB", "RB" }, { "LT", "RT" }, { "LS", "RS" },
                         "Back", "Start", "Guide", "Misc", "Pad", NO, NO, NO, NO, { "P1", "P3", "P2", "P4" } },
};

int ui_device_named(const char *name)
{
    if (!SDL_strcasecmp(name, "keys"))
        return UI_DEV_KEYS;
    if (!SDL_strcasecmp(name, "ps"))
        return UI_DEV_PS;
    for (int i = 1; i < UI_DEVS; i++)
        if (!SDL_strcasecmp(name, styles[i].name))
            return i;
    return -1;
}

static int dev = UI_DEV_KEYS, last_pad_style = -1;
/* the last pad's own face labels (its mapping can differ from its type's,
 * as an 8BitDo pad in its Switch layout); 0 when not known */
static SDL_GamepadButtonLabel last_face[4];

/* ---- Steam Input's twin pads (docs/design.md s30) ----
 * With Steam Input on for the game, Steam reads the real pad itself and
 * gives the game a virtual pad of its own (Valve's 28de:11ff, an XInput
 * pad), and names the real pad behind each of its slots in a file whose
 * path is in SteamVirtualGamepadInfo: "[slot N]", then name=, VID=, PID=,
 * type= (SDL's names: ps5, xboxone...). Run natively, SDL reads the file
 * and gives the virtual pad the real one's ids and type. Under Proton it
 * does not (the path is a Linux one, and SDL takes it that Wine leaves out
 * the real pads, SDL_gamepad.c's SDL_ShouldIgnoreGamepad), but Proton also
 * shows the real pad (a DualSense through its HID driver): one press was
 * player 1's (the virtual pad) and player 2's (the real one).
 *
 * So a real pad with the ids the file gives for a virtual pad's slot is
 * left out, one per virtual pad. A virtual pad's slot is its player index
 * (SDL's: Steam's slot when XInput tells it, else XInput's user index),
 * else its place among the virtual pads. If the file cannot be read and
 * Steam says it filters (SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD),
 * every real pad is left out while a virtual one is there. The pads are
 * looked at again when SDL's list changes, and every second. */

#define STEAM_SLOTS 8
#define TWIN_MAX 16
typedef struct steam_slot {
    int set;
    Uint16 vendor, product;
    SDL_GamepadType type;
    char name[64];
} steam_slot;
static steam_slot steam_info[STEAM_SLOTS];
static char steam_path[1024];           /* the file as this program opens it; "" when none */
static int steam_tried, steam_read;     /* steam_read: the file was read */
static SDL_Time steam_mtime;
static SDL_JoystickID twin_seen[TWIN_MAX], twin_hidden[TWIN_MAX], twin_by[TWIN_MAX];
static int twin_by_slot[TWIN_MAX];      /* twin_hidden's: the virtual pad and its slot (0: none) */
static int twin_slot[TWIN_MAX];         /* a virtual pad's slot in steam_info (twin_seen's order), or -1 */
static int twin_seen_n = -1, twin_hidden_n;
static Uint64 twin_at;
static void (*twin_log)(const char *);

void ui_pad_logger(void (*log)(const char *line))
{
    twin_log = log;
}

static void twin_say(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void twin_say(const char *fmt, ...)
{
    if (!twin_log)
        return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    twin_log(line);
}

/* Steam's own pad: its ids, or (when SDL took the real pad's ids for it from
 * Steam's file, run natively) a Steam handle */
static int steam_virtual(SDL_JoystickID id)
{
    if (SDL_GetGamepadVendorForID(id) == 0x28DE && SDL_GetGamepadProductForID(id) == 0x11FF)
        return 1;
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);
    return g && SDL_GetGamepadSteamHandle(g) != 0;
}

/* Steam's file into steam_info, when it is new: 1 when it was read */
static int steam_load(void)
{
    if (!steam_tried) {
        steam_tried = 1;
        const char *p = SDL_getenv("SteamVirtualGamepadInfo");
        if (p && *p) {
            SDL_strlcpy(steam_path, p, sizeof steam_path);
#ifdef _WIN32
            /* under Proton the path is the Linux one; Wine's Z: drive is / */
            if (p[0] == '/' && !SDL_GetPathInfo(steam_path, NULL))
                SDL_snprintf(steam_path, sizeof steam_path, "Z:%s", p);
#endif
            twin_say("Steam's pad list: %s", steam_path);
        }
    }
    if (!steam_path[0])
        return 0;
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(steam_path, &pi)) {
        if (steam_read)
            twin_say("Steam's pad list: cannot be read now: %s", SDL_GetError());
        steam_read = 0;
        return 0;
    }
    if (steam_read && pi.modify_time == steam_mtime)
        return 1;
    size_t size = 0;
    char *data = SDL_LoadFile(steam_path, &size);      /* ends with a 0 */
    if (!data) {
        if (steam_read || !steam_mtime)
            twin_say("Steam's pad list: cannot be read: %s", SDL_GetError());
        steam_read = 0;
        steam_mtime = pi.modify_time;
        return 0;
    }
    SDL_zeroa(steam_info);
    int slot = -1;
    char *save = NULL;
    for (char *line = SDL_strtok_r(data, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        int n;
        if (SDL_sscanf(line, "[slot %d]", &n) == 1) {
            slot = n >= 0 && n < STEAM_SLOTS ? n : -1;
            if (slot >= 0)
                steam_info[slot].set = 1;
            continue;
        }
        char *v = SDL_strchr(line, '=');
        if (slot < 0 || !v)
            continue;
        *v++ = 0;
        steam_slot *s = &steam_info[slot];
        if (!SDL_strcmp(line, "VID"))
            s->vendor = (Uint16)SDL_strtoul(v, NULL, 0);
        else if (!SDL_strcmp(line, "PID"))
            s->product = (Uint16)SDL_strtoul(v, NULL, 0);
        else if (!SDL_strcmp(line, "type"))
            s->type = SDL_GetGamepadTypeFromString(v);
        else if (!SDL_strcmp(line, "name"))
            SDL_strlcpy(s->name, v, sizeof s->name);
    }
    SDL_free(data);
    steam_read = 1;
    steam_mtime = pi.modify_time;
    for (int i = 0; i < STEAM_SLOTS; i++)
        if (steam_info[i].set)
            twin_say("Steam's pad list: slot %d \"%s\" %04x:%04x %s", i, steam_info[i].name, steam_info[i].vendor,
                     steam_info[i].product,
                     steam_info[i].type ? SDL_GetGamepadStringForType(steam_info[i].type) : "(no type)");
    return 1;
}

static int twin_is_hidden(SDL_JoystickID id)
{
    for (int i = 0; i < twin_hidden_n; i++)
        if (twin_hidden[i] == id)
            return 1;
    return 0;
}

/* looks at the pads again when SDL's list (ids) changed, or a second on */
static void twin_update(const SDL_JoystickID *ids, int n)
{
    if (n > TWIN_MAX)
        n = TWIN_MAX;
    Uint64 now = SDL_GetTicks();
    if (n == twin_seen_n && !SDL_memcmp(ids, twin_seen, (size_t)n * sizeof *ids) && now - twin_at < 1000)
        return;
    twin_at = now;
    SDL_JoystickID was[TWIN_MAX];
    int was_n = twin_hidden_n;
    SDL_memcpy(was, twin_hidden, sizeof was);
    if (n)
        SDL_memcpy(twin_seen, ids, (size_t)n * sizeof *ids);
    twin_seen_n = n;
    twin_hidden_n = 0;
    int virtuals = 0;
    for (int i = 0; i < n; i++) {
        twin_slot[i] = -1;
        if (!steam_virtual(ids[i]))
            continue;
        if (!virtuals++)
            steam_load();
        if (!steam_read)
            continue;
        int s = SDL_GetGamepadPlayerIndexForID(ids[i]);
        if (s < 0 || s >= STEAM_SLOTS || !steam_info[s].set)       /* else its place among the virtual pads */
            for (int k = 0, at = virtuals - 1; k < STEAM_SLOTS; k++)
                if (steam_info[k].set && !at--) {
                    s = k;
                    break;
                }
        if (s < 0 || s >= STEAM_SLOTS || !steam_info[s].set)
            continue;
        twin_slot[i] = s;
        for (int j = 0; j < n; j++)
            if (!steam_virtual(ids[j]) && !twin_is_hidden(ids[j]) && steam_info[s].vendor &&
                SDL_GetGamepadVendorForID(ids[j]) == steam_info[s].vendor &&
                SDL_GetGamepadProductForID(ids[j]) == steam_info[s].product) {
                twin_by[twin_hidden_n] = ids[i];
                twin_by_slot[twin_hidden_n] = s;
                twin_hidden[twin_hidden_n++] = ids[j];
                break;
            }
    }
    const char *allow = SDL_getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD");   /* Steam sets "1" */
    if (virtuals && !steam_read && allow && (!SDL_strcmp(allow, "1") || !SDL_strcasecmp(allow, "true")))
        for (int j = 0; j < n; j++)
            if (!steam_virtual(ids[j])) {
                twin_by[twin_hidden_n] = 0;
                twin_hidden[twin_hidden_n++] = ids[j];
            }
    /* the log says what changed */
    for (int i = 0; i < twin_hidden_n; i++) {
        int old = 0;
        for (int k = 0; k < was_n; k++)
            old |= was[k] == twin_hidden[i];
        if (old)
            continue;
        SDL_JoystickID r = twin_hidden[i], v = twin_by[i];
        if (v)
            twin_say("gamepad %u \"%s\" %04x:%04x left out: Steam Input gives it as its pad %u (slot %d)",
                     (unsigned)r, SDL_GetGamepadNameForID(r), SDL_GetGamepadVendorForID(r),
                     SDL_GetGamepadProductForID(r), (unsigned)v, twin_by_slot[i]);
        else
            twin_say("gamepad %u \"%s\" %04x:%04x left out: a Steam Input pad is there and Steam's pad list "
                     "cannot be read", (unsigned)r, SDL_GetGamepadNameForID(r), SDL_GetGamepadVendorForID(r),
                     SDL_GetGamepadProductForID(r));
    }
    for (int k = 0; k < was_n; k++)
        if (!twin_is_hidden(was[k])) {
            int there = 0;
            for (int j = 0; j < n; j++)
                there |= ids[j] == was[k];
            if (there)
                twin_say("gamepad %u is used again", (unsigned)was[k]);
        }
}

static Uint64 twin_polled;
static int twin_dirty = 1;

static void twin_poll(void)
{
    Uint64 now = SDL_GetTicks();
    if (!twin_dirty && now - twin_polled < 250)
        return;
    twin_dirty = 0;
    twin_polled = now;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    twin_update(ids, ids ? n : 0);
    SDL_free(ids);
}

SDL_JoystickID *ui_gamepads(int *count)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) {
        *count = 0;
        return NULL;
    }
    twin_update(ids, n);
    twin_polled = SDL_GetTicks();
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!twin_is_hidden(ids[i]))
            ids[k++] = ids[i];
    ids[k] = 0;
    *count = k;
    return ids;
}

int ui_pad_hidden(SDL_JoystickID id)
{
    twin_poll();
    return twin_is_hidden(id);
}

/* Every controller SDL reports goes to the log, a gamepad or not (issue 4: a
 * controller neither the launcher nor the game took, and a log that could not
 * say whether SDL saw it). A controller SDL has no button layout for is a
 * joystick only: the game and the launcher use gamepads */
static void pad_log_seen(SDL_JoystickID id)
{
    char guid[40];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, (int)sizeof guid);
    const char *name = SDL_GetJoystickNameForID(id), *path = SDL_GetJoystickPathForID(id);
    int pad = SDL_IsGamepad(id);
    twin_say("controller seen: id %u \"%s\" %04x:%04x, %s%s, guid %s, path %s", (unsigned)id, name ? name : "?",
             SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
             pad ? "a gamepad: " : "NOT usable: SDL has no gamepad layout for it",
             pad ? SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id)) : "", guid, path ? path : "-");
}

int ui_pad_event_hidden(const SDL_Event *e)
{
    SDL_JoystickID id;
    switch (e->type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        pad_log_seen(e->jdevice.which);
        return 0;
    case SDL_EVENT_JOYSTICK_REMOVED:
        twin_say("controller gone: id %u", (unsigned)e->jdevice.which);
        return 0;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        twin_dirty = 1;
        return 0;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: id = e->gbutton.which; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: id = e->gaxis.which; break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: id = e->gtouchpad.which; break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: id = e->gsensor.which; break;
    default: return 0;
    }
    return ui_pad_hidden(id);
}

/* the real pad behind a Steam virtual pad, from Steam's file; NULL when not known */
static const steam_slot *twin_info(SDL_JoystickID id)
{
    twin_poll();
    for (int i = 0; i < twin_seen_n; i++)
        if (twin_seen[i] == id && twin_slot[i] >= 0)
            return &steam_info[twin_slot[i]];
    return NULL;
}

static int style_of_type(SDL_GamepadType type)
{
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: return UI_DEV_XBOX360;
    case SDL_GAMEPAD_TYPE_XBOXONE: return UI_DEV_XBOX;
    case SDL_GAMEPAD_TYPE_PS3: return UI_DEV_PS3;
    case SDL_GAMEPAD_TYPE_PS4: return UI_DEV_PS4;
    case SDL_GAMEPAD_TYPE_PS5: return UI_DEV_PS;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return UI_DEV_NINTENDO;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return UI_DEV_GAMECUBE;
    case SDL_GAMEPAD_TYPE_STEAM: return UI_DEV_STEAM;
    default: return UI_DEV_GENERIC;
    }
}

/* the style of a gamepad SDL has */
static int style_of(SDL_JoystickID id)
{
    if (SDL_GetGamepadVendorForID(id) == 0x28DE && SDL_GetGamepadProductForID(id) == 0x11FF) {
        /* Steam Input's own pad: the real pad behind it from Steam's file
         * (s30), else a guess: on a Steam Deck most likely the Deck's own
         * controls, elsewhere an Xbox pad */
        const steam_slot *s = twin_info(id);
        if (s && s->type != SDL_GAMEPAD_TYPE_UNKNOWN)
            return style_of_type(s->type);
        const char *deck = SDL_getenv("SteamDeck");
        return deck && !SDL_strcmp(deck, "1") ? UI_DEV_STEAM : UI_DEV_XBOX;
    }
    return style_of_type(SDL_GetGamepadTypeForID(id));
}

/* a pad's four face labels: a Steam virtual pad's own are an Xbox pad's (A
 * at the bottom), so the real pad's by place, from its type in Steam's file */
static void pad_faces(SDL_JoystickID id, SDL_GamepadButtonLabel l[4])
{
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);     /* opened by the program already */
    const steam_slot *s = SDL_GetGamepadVendorForID(id) == 0x28DE && SDL_GetGamepadProductForID(id) == 0x11FF
                              ? twin_info(id) : NULL;
    for (int b = 0; b < 4; b++)
        l[b] = s && s->type != SDL_GAMEPAD_TYPE_UNKNOWN
                   ? SDL_GetGamepadButtonLabelForType(s->type, (SDL_GamepadButton)b)
               : g ? SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b)
                   : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
}

static void note_pad(SDL_JoystickID id)
{
    dev = last_pad_style = style_of(id);
    pad_faces(id, last_face);
}

int ui_note_event(const SDL_Event *e)
{
    int dev_was = dev;
    SDL_GamepadButtonLabel face_was[4];
    SDL_memcpy(face_was, last_face, sizeof face_was);
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        dev = UI_DEV_KEYS;
    else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        note_pad(e->gbutton.which);
    else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 20000 || e->gaxis.value < -20000))
        note_pad(e->gaxis.which);
    /* two pads of one style can label their face buttons differently (an
     * Xbox pad and an 8BitDo pad in its Switch layout) */
    return dev != dev_was || SDL_memcmp(face_was, last_face, sizeof face_was) != 0;
}

int ui_device(void)
{
    return dev;
}

void ui_set_device(int d)
{
    if (d < 0 || d >= UI_DEVS)
        return;
    dev = d;
    if (d != UI_DEV_KEYS) {
        last_pad_style = d;
        SDL_zeroa(last_face);
    }
}

int ui_pad_style(void)
{
    if (last_pad_style >= 0)
        return last_pad_style;
    int n = 0, s = UI_DEV_XBOX;
    SDL_JoystickID *ids = ui_gamepads(&n);
    if (ids && n > 0)
        s = style_of(ids[0]);
    SDL_free(ids);
    return s;
}

/* ---- a controller for each player (docs/design.md section 69) ---- */

/* the caps of gamepad inputs drawn as one pad's, not the last one used */
static int as_on, as_style;
static SDL_GamepadButtonLabel as_face[4];

void ui_pad_caps_as(SDL_JoystickID id)
{
    as_on = id != 0;
    if (!as_on)
        return;
    as_style = style_of(id);
    pad_faces(id, as_face);
}

/* a serial number as a wish keeps it: letters, digits, '-', '_', ':', '.' */
static void serial_clean(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++)
        if (SDL_isalnum((unsigned char)*in) || *in == '-' || *in == '_' || *in == ':' || *in == '.')
            out[k++] = *in;
    out[k] = 0;
}

static void pad_serial(SDL_JoystickID id, char *out, size_t n)
{
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);
    serial_clean(g ? SDL_GetGamepadSerial(g) : NULL, out, n);
}

/* "VVVV:PPPP[/SERIAL][@N] NAME": the maker's and the model's numbers, the
 * serial number when the pad tells one, else its place among the pads of
 * that model (from 1; left out for the first), and its name for showing */
static int wish_parse(const char *w, unsigned *vid, unsigned *pid, char *serial, size_t sn, int *nth)
{
    serial[0] = 0;
    *nth = 1;
    if (!w || !w[0])
        return 0;
    char *end = NULL;
    unsigned long v = SDL_strtoul(w, &end, 16);
    if (end != w + 4 || *end != ':')
        return 0;
    const char *q = end + 1;
    unsigned long p = SDL_strtoul(q, &end, 16);
    if (end != q + 4)
        return 0;
    *vid = (unsigned)v;
    *pid = (unsigned)p;
    q = end;
    if (*q == '/') {
        size_t k = 0;
        for (q++; *q && *q != ' ' && *q != '@'; q++)
            if (k + 1 < sn)
                serial[k++] = *q;
        serial[k] = 0;
    }
    if (*q == '@') {
        int n = SDL_atoi(q + 1);
        if (n > 1)
            *nth = n;
    }
    return 1;
}

void ui_pad_wish_of(SDL_JoystickID id, char *out, size_t n)
{
    unsigned vid = SDL_GetGamepadVendorForID(id), pid = SDL_GetGamepadProductForID(id);
    char serial[48], name[96];
    pad_serial(id, serial, sizeof serial);
    const char *nm = SDL_GetGamepadNameForID(id);
    size_t k = 0;
    for (; nm && *nm && k + 1 < sizeof name; nm++)      /* a '#' would end the settings file's line */
        name[k++] = *nm == '#' || (unsigned char)*nm < 0x20 ? ' ' : *nm;
    name[k] = 0;
    int nth = 1, count = 0;
    SDL_JoystickID *ids = ui_gamepads(&count);
    for (int i = 0; ids && i < count && ids[i] != id; i++)
        if (SDL_GetGamepadVendorForID(ids[i]) == vid && SDL_GetGamepadProductForID(ids[i]) == pid)
            nth++;
    SDL_free(ids);
    if (serial[0])
        SDL_snprintf(out, n, "%04x:%04x/%s %s", vid, pid, serial, name);
    else if (nth > 1)
        SDL_snprintf(out, n, "%04x:%04x@%d %s", vid, pid, nth, name);
    else
        SDL_snprintf(out, n, "%04x:%04x %s", vid, pid, name);
}

const char *ui_pad_wish_name(const char *wish)
{
    const char *sp = wish ? SDL_strchr(wish, ' ') : NULL;
    return sp ? sp + 1 : "";
}

int ui_pad_wish_fits(const char *wish, SDL_JoystickID id)
{
    unsigned vid = 0, pid = 0;
    char serial[48];
    int nth = 1;
    return wish_parse(wish, &vid, &pid, serial, sizeof serial, &nth) && SDL_GetGamepadVendorForID(id) == vid &&
           SDL_GetGamepadProductForID(id) == pid;
}

void ui_pad_players(const char *const wish[2], SDL_JoystickID out[2])
{
    out[0] = out[1] = 0;
    int n = 0;
    SDL_JoystickID *ids = ui_gamepads(&n);
    if (!ids)
        return;
    /* a player who asks for one controller gets it when it is there: by its
     * serial number, else by its place among its model, else the first of
     * its model that is free */
    for (int p = 0; p < 2; p++) {
        unsigned vid = 0, pid = 0;
        char serial[48], have[48];
        int nth = 1;
        if (!wish_parse(wish[p], &vid, &pid, serial, sizeof serial, &nth))
            continue;
        SDL_JoystickID first = 0, by_serial = 0, by_place = 0;
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (SDL_GetGamepadVendorForID(ids[i]) != vid || SDL_GetGamepadProductForID(ids[i]) != pid)
                continue;
            k++;
            if (p == 1 && ids[i] == out[0])
                continue;               /* player 1 has it */
            if (!first)
                first = ids[i];
            if (k == nth)
                by_place = ids[i];
            if (serial[0]) {
                pad_serial(ids[i], have, sizeof have);
                if (!SDL_strcmp(have, serial))
                    by_serial = ids[i];
            }
        }
        out[p] = by_serial ? by_serial : by_place ? by_place : first;
    }
    /* the others, and a player whose controller is not there: the pads left,
     * in the order they connected, player 1 first */
    for (int p = 0; p < 2; p++) {
        if (out[p])
            continue;
        for (int i = 0; i < n; i++)
            if (ids[i] != out[0] && ids[i] != out[1]) {
                out[p] = ids[i];
                break;
            }
    }
    SDL_free(ids);
}

SDL_JoystickID ui_test_pad(const char *type, const char *name)
{
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.name = name;
    /* a real pad's vendor and product ids, so that SDL takes it for one and
     * its glyphs show */
    static const struct { const char *name; Uint16 vendor, product; const char *real; } types[] = {
        { "xbox", 0x045E, 0x0B12, "Xbox Wireless Controller" },             /* Xbox Series X|S */
        { "xbox360", 0x045E, 0x028E, "Xbox 360 Controller" },
        { "ps5", 0x054C, 0x0CE6, "DualSense Wireless Controller" },
        { "ps4", 0x054C, 0x09CC, "DualShock 4 Wireless Controller" },
        { "ps3", 0x054C, 0x0268, "DualShock 3 Controller" },
        { "switch", 0x057E, 0x2009, "Nintendo Switch Pro Controller" },
        { "gamecube", 0x057E, 0x2073, "Nintendo GameCube Controller" },     /* the Switch 2's */
        { "deck", 0x28DE, 0x1205, "Steam Deck" },
        { "steamvirtual", 0x28DE, 0x11FF, "Steam Virtual Gamepad" },        /* Steam Input's own pad */
    };
    for (size_t k = 0; type && k < SDL_arraysize(types); k++)
        if (!SDL_strcmp(type, types[k].name)) {
            d.vendor_id = types[k].vendor, d.product_id = types[k].product;
            if (!name)
                d.name = types[k].real;
        }
    if (!d.name)
        d.name = "Test gamepad";
    return SDL_AttachVirtualJoystick(&d);
}

void ui_pad_title(SDL_JoystickID id, char *out, size_t n)
{
    const char *name = SDL_GetGamepadNameForID(id);
    if (!name || !name[0])
        name = "Gamepad";
    int count = 0, same = 0, place = 0;
    SDL_JoystickID *ids = ui_gamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        const char *other = SDL_GetGamepadNameForID(ids[i]);
        if (other && !SDL_strcmp(other, name)) {
            same++;
            if (ids[i] == id)
                place = same;
        }
    }
    SDL_free(ids);
    if (same > 1 && place)
        SDL_snprintf(out, n, "%s (%d)", name, place);
    else
        SDL_strlcpy(out, name, n);
}

/* ---- face buttons in menus: by their labels ---- */

static const int face_in[4] = { UI_IN_OK, UI_IN_BACK, UI_IN_CLEAR, UI_IN_DEFAULT };

/* a face button's job in a menu from its label (0 select, 1 back, 2 clear,
 * 3 default), or -1 */
static int face_job(SDL_GamepadButtonLabel l)
{
    switch (l) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 0;
    case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return 1;
    case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return 2;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return 3;
    default: return -1;
    }
}

/* the jobs of the four face buttons (SDL_GAMEPAD_BUTTON_SOUTH..NORTH) from
 * their labels; by place (south select, east back, west clear, north
 * default) when the labels are not known or do not give each job once */
static void face_jobs(const SDL_GamepadButtonLabel l[4], int job[4])
{
    int seen = 0;
    for (int b = 0; b < 4; b++) {
        job[b] = face_job(l[b]);
        if (job[b] >= 0)
            seen |= 1 << job[b];
    }
    if (seen != 15)
        for (int b = 0; b < 4; b++)
            job[b] = b;
}

int ui_pad_face(SDL_JoystickID pad, int button)
{
    if (button < SDL_GAMEPAD_BUTTON_SOUTH || button > SDL_GAMEPAD_BUTTON_NORTH)
        return UI_IN_NONE;
    SDL_Gamepad *g = SDL_GetGamepadFromID(pad);
    SDL_GamepadType type = SDL_GetGamepadTypeForID(pad);
    SDL_GamepadButtonLabel l[4];
    int job[4];
    for (int b = 0; b < 4; b++)
        l[b] = g ? SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b)
                 : SDL_GetGamepadButtonLabelForType(type, (SDL_GamepadButton)b);
    face_jobs(l, job);
    return face_in[job[button]];
}

const char *ui_pad_face_name(int in)
{
    const pad_style *ps = &styles[ui_pad_style()];
    SDL_GamepadButtonLabel l[4];
    int job[4];
    for (int b = 0; b < 4; b++)
        l[b] = last_face[b] ? last_face[b] : SDL_GetGamepadButtonLabelForType(ps->type, (SDL_GamepadButton)b);
    face_jobs(l, job);
    for (int b = 0; b < 4; b++)
        if (face_in[job[b]] == in)
            return SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    return NULL;
}

/* a cap: draw 0 measures it */
static float cap_box(float x, float y, float h, const char *label, int icon, int round, SDL_Color c, int draw)
{
    float size = h * 0.5f, tw = label ? ui_text_w(UI_MEDIUM, size, label) : 0;
    float w = round || icon >= 0 ? h : SDL_max(h, tw + h * 0.62f);
    if (!draw)
        return w;
    ui_round_line(x, y - h / 2, w, h, round ? h / 2 : h * 0.24f, 1, UI_LINE, UI_HOVER);
    if (icon >= 0)
        ui_icon(icon, x + w / 2, y, h * (round ? 0.78f : 0.62f), c);
    else if (label)
        ui_text(UI_MEDIUM, size, x + (w - tw) / 2, y, c, label);
    return w;
}

float ui_cap(float x, float y, float h, const char *label)
{
    return cap_box(x, y, h, label, -1, 0, UI_TEXT, 1);
}

float ui_cap_icon(float x, float y, float h, int icon)
{
    return cap_box(x, y, h, NULL, icon, 0, UI_TEXT, 1);
}

/* a face button's look by its printed label */
static void face_look(int st, SDL_GamepadButtonLabel l, const char **label, int *icon, SDL_Color *c)
{
    static const struct { const char *text; int icon; SDL_Color xbox, ps, cube; } faces[] = {
        [SDL_GAMEPAD_BUTTON_LABEL_A] = { "A", NO, { 104, 200, 96, 255 }, { 0 }, { 92, 204, 156, 255 } },
        [SDL_GAMEPAD_BUTTON_LABEL_B] = { "B", NO, { 240, 98, 92, 255 }, { 0 }, { 240, 98, 92, 255 } },
        [SDL_GAMEPAD_BUTTON_LABEL_X] = { "X", NO, { 92, 158, 242, 255 }, { 0 }, { 0 } },
        [SDL_GAMEPAD_BUTTON_LABEL_Y] = { "Y", NO, { 244, 200, 74, 255 }, { 0 }, { 0 } },
        [SDL_GAMEPAD_BUTTON_LABEL_CROSS] = { NULL, UI_I_PS_CROSS, { 0 }, { 132, 166, 242, 255 }, { 0 } },
        [SDL_GAMEPAD_BUTTON_LABEL_CIRCLE] = { NULL, UI_I_PS_CIRCLE, { 0 }, { 242, 106, 112, 255 }, { 0 } },
        [SDL_GAMEPAD_BUTTON_LABEL_SQUARE] = { NULL, UI_I_PS_SQUARE, { 0 }, { 230, 136, 206, 255 }, { 0 } },
        [SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE] = { NULL, UI_I_PS_TRIANGLE, { 0 }, { 94, 206, 174, 255 }, { 0 } },
    };
    if (l <= SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN || l >= (int)SDL_arraysize(faces))
        return;
    *label = faces[l].text;
    *icon = faces[l].icon;
    SDL_Color k = styles[st].colors == 1 ? faces[l].xbox : styles[st].colors == 2 ? faces[l].ps
                : styles[st].colors == 3 ? faces[l].cube : (SDL_Color){ 0 };
    if (k.a)
        *c = k;
}

/* how a key or a gamepad input looks: a label or an icon, round or not, its color */
static void input_look(const char *name, int pad, const char **label, int *icon, int *round, SDL_Color *c)
{
    *label = name;
    *icon = -1;
    *round = 0;
    *c = UI_TEXT;
    if (!pad) {
        static const struct { const char *name, *label; int icon; } keys[] = {
            { "Up", NULL, UI_I_UP }, { "Down", NULL, UI_I_DOWN }, { "Left", NULL, UI_I_LEFT },
            { "Right", NULL, UI_I_RIGHT }, { "Return", "Enter", -1 }, { "Escape", "Esc", -1 },
            { "Delete", "Del", -1 }, { "Backspace", "Backspace", -1 },
        };
        for (size_t i = 0; i < SDL_arraysize(keys); i++)
            if (!SDL_strcasecmp(name, keys[i].name)) {
                *label = keys[i].label;
                *icon = keys[i].icon;
            }
        return;
    }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: *icon = UI_I_UP; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: *icon = UI_I_DOWN; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: *icon = UI_I_LEFT; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: *icon = UI_I_RIGHT; break;
    default: break;
    }
    if (*icon >= 0) {
        *label = NULL;
        return;
    }
    int st = as_on ? as_style : ui_pad_style();
    const pad_style *ps = &styles[st];
    if (b >= SDL_GAMEPAD_BUTTON_SOUTH && b <= SDL_GAMEPAD_BUTTON_NORTH) {
        SDL_GamepadButtonLabel l = as_on ? as_face[b] : last_face[b];
        if (!l)
            l = SDL_GetGamepadButtonLabelForType(ps->type, b);
        *round = 1;
        face_look(st, l, label, icon, c);
        if (*icon >= 0)
            *label = NULL;
        return;
    }
    const char *t = NULL;
    int i = NO, rnd = 0;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: t = ps->shoulder[0]; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: t = ps->shoulder[1]; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: t = ps->stick[0]; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: t = ps->stick[1]; break;
    case SDL_GAMEPAD_BUTTON_BACK: t = ps->back, i = ps->back_icon, rnd = 1; break;
    case SDL_GAMEPAD_BUTTON_START: t = ps->start, i = ps->start_icon, rnd = 1; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: t = ps->guide, i = ps->guide_icon, rnd = 1; break;
    case SDL_GAMEPAD_BUTTON_MISC1: t = ps->misc1, i = ps->misc1_icon; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: t = ps->touchpad ? ps->touchpad : "Pad"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: t = ps->paddle[0]; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: t = ps->paddle[1]; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: t = ps->paddle[2]; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: t = ps->paddle[3]; break;
    default:
        /* the triggers are axes in SDL: their names are not buttons */
        if (!SDL_strcmp(name, "lefttrigger"))
            t = ps->trigger[0];
        else if (!SDL_strcmp(name, "righttrigger"))
            t = ps->trigger[1];
        break;
    }
    if (i >= 0) {
        *icon = i;
        *label = NULL;
        *round = rnd;
    } else if (t) {
        *label = t;
    }
}

float ui_cap_input(float x, float y, float h, const char *name, int pad)
{
    const char *label;
    int icon, round;
    SDL_Color c;
    input_look(name, pad, &label, &icon, &round, &c);
    return cap_box(x, y, h, label, icon, round, c, 1);
}

float ui_cap_input_w(float h, const char *name, int pad)
{
    const char *label;
    int icon, round;
    SDL_Color c;
    input_look(name, pad, &label, &icon, &round, &c);
    return cap_box(0, 0, h, label, icon, round, c, 0);
}

/* a hint's inputs as names: keyboard (pad 0) or gamepad */
static int hint_names(int in, int pad, const char **out)
{
    static const char *const keys[][2] = {
        { "Up", "Down" }, { "Left", "Right" }, { "Return", NULL }, { "Escape", NULL }, { "Q", NULL }, { "E", NULL },
        { "Delete", NULL }, { "Backspace", NULL },
    };
    static const char *const pads[][2] = {      /* the face buttons go by label: ui_pad_face_name */
        { "dpup", "dpdown" }, { "dpleft", "dpright" }, { NULL, NULL }, { NULL, NULL }, { "leftshoulder", NULL },
        { "rightshoulder", NULL }, { NULL, NULL }, { NULL, NULL },
    };
    if (in < 0 || in >= UI_IN_NONE)
        return 0;
    if (pad && (in == UI_IN_OK || in == UI_IN_BACK || in == UI_IN_CLEAR || in == UI_IN_DEFAULT)) {
        out[0] = ui_pad_face_name(in);
        out[1] = NULL;
        return out[0] ? 1 : 0;
    }
    const char *const *t = pad ? pads[in] : keys[in];
    out[0] = t[0];
    out[1] = t[1];
    return t[1] ? 2 : 1;
}

static float hint_w(float h, const ui_hint *hint, int pad, int draw, float x, float y)
{
    float size = h * 0.56f, gap = h * 0.22f, w = 0;
    if (hint->text && hint->text[0]) {
        w = draw ? ui_text(UI_REGULAR, size, x, y, UI_MUTED, hint->text) : ui_text_w(UI_REGULAR, size, hint->text);
        w += gap * 1.6f;
    }
    for (int k = 0; k < 2; k++) {
        const char *names[2];
        int n = hint_names(hint->in[k], pad, names);
        for (int i = 0; i < n; i++) {
            w += draw ? ui_cap_input(x + w, y, h, names[i], pad) : ui_cap_input_w(h, names[i], pad);
            w += gap;
        }
    }
    return w - gap;
}

float ui_hints(float x, float y, float h, const ui_hint *hints, int n, int align)
{
    int pad = dev != UI_DEV_KEYS;
    float space = h * 1.1f, total = 0;
    for (int i = 0; i < n; i++)
        total += hint_w(h, &hints[i], pad, 0, 0, 0) + (i ? space : 0);
    float at = align == 2 ? x - total : x;
    for (int i = 0; i < n; i++)
        at += hint_w(h, &hints[i], pad, 1, at, y) + space;
    return total;
}

/* ---- widgets ---- */

#define TABS_MAX 8
static struct { float y, h, left, prev_end, next_x, right, x[TABS_MAX + 1]; int n; } last_tabs;

int ui_tabs_hit(float x, float y)
{
    if (y < last_tabs.y || y >= last_tabs.y + last_tabs.h || x < last_tabs.left || x >= last_tabs.right)
        return -1;
    if (x < last_tabs.prev_end)
        return UI_TAB_PREV;
    if (x >= last_tabs.next_x)
        return UI_TAB_NEXT;
    for (int i = 0; i < last_tabs.n; i++)
        if (x >= last_tabs.x[i] && x < last_tabs.x[i + 1])
            return i;
    return -1;
}

void ui_tabs(float x, float y, float w, float h, const char *const *names, int n, int cur)
{
    int pad = dev != UI_DEV_KEYS;
    const char *prev[2], *next[2];
    hint_names(UI_IN_PREV, pad, prev);
    hint_names(UI_IN_NEXT, pad, next);
    float ch = h * 0.62f, mid = y + h / 2;
    ui_cap_input(x, mid, ch, prev[0], pad);
    ui_cap_input(x + w - ui_cap_input_w(ch, next[0], pad), mid, ch, next[0], pad);
    float size = h * 0.4f, padx = h * 0.55f, total = 0;
    for (int i = 0; i < n; i++)
        total += ui_text_w(UI_MEDIUM, size, names[i]) + 2 * padx;
    float at = x + (w - total) / 2;
    last_tabs.y = y;
    last_tabs.h = h;
    last_tabs.n = n < TABS_MAX ? n : TABS_MAX;
    last_tabs.left = x;
    last_tabs.right = x + w;
    last_tabs.prev_end = x + ui_cap_input_w(ch, prev[0], pad);
    last_tabs.next_x = x + w - ui_cap_input_w(ch, next[0], pad);
    for (int i = 0; i < n; i++) {
        float tw = ui_text_w(UI_MEDIUM, size, names[i]);
        if (i < TABS_MAX) {
            last_tabs.x[i] = at;
            last_tabs.x[i + 1] = at + tw + 2 * padx;
        }
        if (i == cur)
            ui_round(at, y + h * 0.14f, tw + 2 * padx, h * 0.72f, h * 0.36f, UI_ACCENT);
        ui_text(UI_MEDIUM, size, at + padx, mid, i == cur ? UI_ON_ACCENT : UI_MUTED, names[i]);
        at += tw + 2 * padx;
    }
}

static struct { float x, y, w, row_h; int top, shown, n; } last_rows;

void ui_rows(float x, float y, float w, float h, float row_h, const ui_row *rows, int n, int sel, int *top)
{
    int shown = (int)(h / row_h);
    if (shown < 1)
        shown = 1;
    if (sel < *top)
        *top = sel;
    if (sel >= *top + shown)
        *top = sel - shown + 1;
    if (*top > n - shown)
        *top = n - shown;
    if (*top < 0)
        *top = 0;
    last_rows.x = x;
    last_rows.y = y;
    last_rows.w = w;
    last_rows.row_h = row_h;
    last_rows.top = *top;
    last_rows.shown = shown;
    last_rows.n = n;
    float size = row_h * 0.39f, vsize = row_h * 0.37f, ch = row_h * 0.58f;
    for (int i = *top; i < n && i < *top + shown; i++) {
        const ui_row *r = &rows[i];
        float ry = y + (float)(i - *top) * row_h, mid = ry + row_h / 2, right = x + w - row_h * 0.35f;
        int on = i == sel;
        if (on) {
            ui_round(x, ry + 2, w, row_h - 4, row_h * 0.2f, UI_RAISED);
            ui_round(x, ry + row_h * 0.26f, 3, row_h * 0.48f, 1.5f, UI_ACCENT);
        }
        SDL_Color lc = r->off ? UI_FAINT : r->warn ? UI_BAD : UI_TEXT;
        ui_text_in(UI_REGULAR, size, x + row_h * 0.4f, mid, w * 0.5f, 0, 0, lc, r->label);
        SDL_Color arrow = r->off ? UI_LINE : on ? UI_ACCENT : UI_FAINT;
        switch (r->kind) {
        case UI_ROW_CHOICE: {
            float bw = SDL_min(w * 0.42f, row_h * 5.6f), bx = right - bw, as = row_h * 0.34f;
            ui_icon(UI_I_LEFT, bx + as / 2, mid, as, r->ends & UI_END_LEFT ? UI_LINE : arrow);
            ui_icon(UI_I_RIGHT, right - as / 2, mid, as, r->ends & UI_END_RIGHT ? UI_LINE : arrow);
            ui_text_in(UI_MEDIUM, vsize, bx + as, mid, bw - 2 * as, 1, 0, r->off ? UI_FAINT : UI_TEXT, r->value);
            if (r->later)
                ui_icon(UI_I_DOT, bx - row_h * 0.25f, mid, row_h * 0.36f, UI_ACCENT);
            break;
        }
        case UI_ROW_TOGGLE: {
            float tw = row_h * 2.9f, th = row_h * 0.62f, tx = right - tw, ty = mid - th / 2;
            ui_round_line(tx, ty, tw, th, th / 2, 1, UI_LINE, UI_BG);
            float hx = r->on ? tx + tw / 2 : tx;
            SDL_Color fill = r->off ? UI_LINE : on ? UI_ACCENT : UI_HOVER;
            ui_round(hx + 2, ty + 2, tw / 2 - 4, th - 4, (th - 4) / 2, fill);
            SDL_Color sel_text = r->off ? UI_MUTED : on ? UI_ON_ACCENT : UI_TEXT;
            ui_text_in(UI_MEDIUM, vsize * 0.92f, tx, mid, tw / 2, 1, 0, r->on ? UI_FAINT : sel_text, "Off");
            ui_text_in(UI_MEDIUM, vsize * 0.92f, tx + tw / 2, mid, tw / 2, 1, 0, r->on ? sel_text : UI_FAINT, "On");
            if (r->later)
                ui_icon(UI_I_DOT, tx - row_h * 0.25f, mid, row_h * 0.36f, UI_ACCENT);
            break;
        }
        case UI_ROW_SLIDER: {
            float tw = ui_text_w(UI_MEDIUM, vsize, "100%") + 4, bw = row_h * 3.6f, bx = right - tw - row_h * 0.3f - bw;
            float bh = row_h * 0.14f;
            ui_round(bx, mid - bh / 2, bw, bh, bh / 2, UI_LINE);
            if (!r->off && r->level > 0)
                ui_round(bx, mid - bh / 2, SDL_max(bh, bw * (float)r->level / 100), bh, bh / 2,
                         on ? UI_ACCENT : UI_MUTED);
            char t[16];
            SDL_snprintf(t, sizeof t, "%d%%", r->level);
            ui_text_in(UI_MEDIUM, vsize, right - tw, mid, tw, 2, 0, r->off ? UI_FAINT : UI_TEXT, r->off ? "-" : t);
            break;
        }
        case UI_ROW_ACTION:
            ui_icon(UI_I_CHEVRON, right - row_h * 0.2f, mid, row_h * 0.42f, arrow);
            break;
        case UI_ROW_TEXT:
            ui_text_in(r->strong ? UI_MEDIUM : UI_REGULAR, vsize, x + w * 0.45f, mid, right - x - w * 0.45f, 2, 0,
                       r->strong ? UI_TEXT : UI_MUTED, r->value);
            break;
        case UI_ROW_PAD:
        case UI_ROW_KEYS: {
            int pad = r->kind == UI_ROW_PAD;
            if (r->waiting) {
                ui_text_in(UI_MEDIUM, vsize, x + w * 0.45f, mid, right - x - w * 0.45f, 2, 0, UI_ACCENT,
                           pad ? "Press a button\xE2\x80\xA6" : "Press a key\xE2\x80\xA6");
                break;
            }
            /* the names, right to left */
            char names[160];
            SDL_strlcpy(names, r->value ? r->value : "", sizeof names);
            const char *list[8];
            int k = 0;
            for (char *t = names, *save = NULL; k < 8 && (t = SDL_strtok_r(t, ",", &save)); t = NULL) {
                while (*t == ' ')
                    t++;
                if (*t && SDL_strcmp(t, "-"))
                    list[k++] = t;
            }
            if (!k) {
                ui_text_in(UI_REGULAR, vsize, x + w * 0.45f, mid, right - x - w * 0.45f, 2, 0, UI_FAINT, "Not set");
                break;
            }
            float at = right;
            for (int j = k - 1; j >= 0; j--) {
                at -= ui_cap_input_w(ch, list[j], pad);
                ui_cap_input(at, mid, ch, list[j], pad);
                at -= ch * 0.3f;
            }
            break;
        }
        }
    }
    /* more above or below */
    if (*top > 0)
        ui_icon(UI_I_UP, x + w - row_h * 0.25f, y - row_h * 0.08f, row_h * 0.3f, UI_FAINT);
    if (*top + shown < n)
        ui_icon(UI_I_DOWN, x + w - row_h * 0.25f, y + (float)shown * row_h + row_h * 0.08f, row_h * 0.3f, UI_FAINT);
}

int ui_rows_hit(float x, float y)
{
    if (x < last_rows.x || x >= last_rows.x + last_rows.w || y < last_rows.y)
        return -1;
    int i = (int)((y - last_rows.y) / last_rows.row_h);
    if (i >= last_rows.shown || last_rows.top + i >= last_rows.n)
        return -1;
    return last_rows.top + i;
}

/* ---- the controls table: an action a row, a cell for each player ---- */

static struct { float x, y, w, row_h, cx[2], cw; int top, shown, n; } last_table;

/* a cell's inputs as caps from its left, or what it says instead */
static void bind_cell(const ui_bind_row *r, int c, int keys, float x, float mid, float w, float row_h, int waiting)
{
    float vsize = row_h * 0.37f, ch = row_h * 0.58f;
    if (waiting) {
        ui_text_in(UI_MEDIUM, vsize, x, mid, w, 0, 0, UI_ACCENT, keys ? "Press a key\xE2\x80\xA6" : "Press a button\xE2\x80\xA6");
        return;
    }
    const char *v = r->value[c] ? r->value[c] : "";
    if (r->text) {
        ui_text_in(r->faint[c] ? UI_REGULAR : UI_MEDIUM, vsize, x, mid, w, 0, 0, r->faint[c] ? UI_MUTED : UI_TEXT, v);
        return;
    }
    char names[160];
    SDL_strlcpy(names, v, sizeof names);
    int k = 0;
    float at = x;
    for (char *t = names, *save = NULL; (t = SDL_strtok_r(t, ",", &save)); t = NULL) {
        while (*t == ' ')
            t++;
        if (!*t || !SDL_strcmp(t, "-"))
            continue;
        float cw = ui_cap_input_w(ch, t, !keys);
        if (at + cw > x + w)
            break;                      /* no room for more: the row's own page shows them all */
        ui_cap_input(at, mid, ch, t, !keys);
        at += cw + ch * 0.3f;
        k++;
    }
    if (!k)
        ui_text_in(UI_REGULAR, vsize, x, mid, w, 0, 0, UI_FAINT, "Not set");
}

void ui_bind_table(float x, float y, float w, float h, float row_h, const char *const head[2],
                   const SDL_JoystickID pad[2], int keys, const ui_bind_row *rows, int n, int sel, int col, int *top)
{
    float head_h = row_h * 0.9f, label_w = w * 0.3f, cw = (w - label_w) / 2;
    float cx[2] = { x + label_w, x + label_w + cw };
    /* the heads: whose column it is */
    for (int c = 0; c < 2; c++)
        ui_text_in(UI_BOLD, row_h * 0.36f, cx[c] + row_h * 0.3f, y + head_h / 2, cw - row_h * 0.6f, 0, 0,
                   c == col || col < 0 ? UI_TEXT : UI_MUTED, head[c]);
    ui_fill(x, y + head_h - 1, w, 1, UI_LINE);
    y += head_h;
    h -= head_h;
    int shown = (int)(h / row_h);
    if (shown < 1)
        shown = 1;
    if (sel < *top)
        *top = sel;
    if (sel >= *top + shown)
        *top = sel - shown + 1;
    if (*top > n - shown)
        *top = n - shown;
    if (*top < 0)
        *top = 0;
    last_table.x = x;
    last_table.y = y;
    last_table.w = w;
    last_table.row_h = row_h;
    last_table.cx[0] = cx[0];
    last_table.cx[1] = cx[1];
    last_table.cw = cw;
    last_table.top = *top;
    last_table.shown = shown;
    last_table.n = n;
    float size = row_h * 0.39f;
    for (int i = *top; i < n && i < *top + shown; i++) {
        const ui_bind_row *r = &rows[i];
        float ry = y + (float)(i - *top) * row_h, mid = ry + row_h / 2;
        int on = i == sel;
        if (on) {
            ui_round(x, ry + 2, w, row_h - 4, row_h * 0.2f, UI_RAISED);
            ui_round(x, ry + row_h * 0.26f, 3, row_h * 0.48f, 1.5f, UI_ACCENT);
            if (col >= 0)       /* the cell chosen */
                ui_round_line(cx[col] + 3, ry + 4, cw - 6, row_h - 8, row_h * 0.18f, 1.5f, UI_ACCENT, UI_RAISED);
        }
        ui_text_in(UI_REGULAR, size, x + row_h * 0.4f, mid, label_w - row_h * 0.5f, 0, 0, UI_TEXT, r->label);
        for (int c = 0; c < 2; c++) {
            if (!keys)
                ui_pad_caps_as(pad[c]);
            bind_cell(r, c, keys, cx[c] + row_h * 0.3f, mid, cw - row_h * 0.6f, row_h, on && r->waiting == c + 1);
        }
        ui_pad_caps_as(0);
    }
    if (*top > 0)
        ui_icon(UI_I_UP, x + w - row_h * 0.25f, y - row_h * 0.08f, row_h * 0.3f, UI_FAINT);
    if (*top + shown < n)
        ui_icon(UI_I_DOWN, x + w - row_h * 0.25f, y + (float)shown * row_h + row_h * 0.08f, row_h * 0.3f, UI_FAINT);
}

int ui_bind_table_hit(float x, float y, int *col)
{
    *col = -1;
    if (x < last_table.x || x >= last_table.x + last_table.w || y < last_table.y || last_table.row_h <= 0)
        return -1;
    int i = (int)((y - last_table.y) / last_table.row_h);
    if (i >= last_table.shown || last_table.top + i >= last_table.n)
        return -1;
    for (int c = 0; c < 2; c++)
        if (x >= last_table.cx[c] && x < last_table.cx[c] + last_table.cw)
            *col = c;
    return last_table.top + i;
}

void ui_about(float x, float y, float w, float h, const char *title, const char *text, const char *note)
{
    ui_round(x, y, w, h, 14, UI_CARD);
    float pad = 22, ty = y + pad + 10;
    if (title && title[0]) {
        ui_text_in(UI_BOLD, 19, x + pad, ty, w - 2 * pad, 0, 0, UI_TEXT, title);
        ty += 38;
    }
    if (text && text[0])
        ty += 23.0f * (float)ui_wrap(UI_REGULAR, 15.5f, x + pad, ty, w - 2 * pad, 23, UI_MUTED, text) + 12;
    if (note && note[0]) {
        ui_icon(UI_I_DOT, x + pad + 4, ty, 14, UI_ACCENT);
        ui_wrap(UI_MEDIUM, 15, x + pad + 18, ty, w - 2 * pad - 18, 22, UI_ACCENT, note);
    }
}

void ui_bar(float x, float y, float w, float h, float frac, double t)
{
    ui_round(x, y, w, h, h / 2, UI_RAISED);
    if (frac >= 0) {
        float fw = w * clampf(frac, 0, 1);
        if (fw > 0)
            ui_round(x, y, SDL_max(fw, h), h, h / 2, UI_ACCENT);
        return;
    }
    /* a piece a quarter long that moves across and comes back in */
    float pos = (float)SDL_fmod(t * 0.55, 1.3) - 0.3f;
    float a = SDL_max(pos, 0), b = SDL_min(pos + 0.3f, 1);
    if (b > a)
        ui_round(x + w * a, y, SDL_max(w * (b - a), h), h, h / 2, UI_ACCENT);
}
