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

int ui_wrap(int face, float size, float x, float y, float w, float line_h, SDL_Color c, const char *s)
{
    sheet *sh = get_sheet(face, size);
    if (!sh || !s)
        return 0;
    SDL_SetTextureColorMod(sh->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(sh->tex, c.a);
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

#define ICONS 48
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
    nsheets = ndisks = nicons = 0;
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

static int dev = UI_DEV_KEYS, last_pad_style = -1;

static int style_of(SDL_GamepadType t)
{
    switch (t) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return UI_DEV_PS;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return UI_DEV_NINTENDO;
    default: return UI_DEV_XBOX;
    }
}

void ui_note_event(const SDL_Event *e)
{
    SDL_JoystickID which = 0;
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        dev = UI_DEV_KEYS;
    else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        which = e->gbutton.which;
    else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 20000 || e->gaxis.value < -20000))
        which = e->gaxis.which;
    if (which)
        dev = last_pad_style = style_of(SDL_GetGamepadTypeForID(which));
}

int ui_device(void)
{
    return dev;
}

void ui_set_device(int d)
{
    dev = d;
    if (d != UI_DEV_KEYS)
        last_pad_style = d;
}

int ui_pad_style(void)
{
    if (last_pad_style >= 0)
        return last_pad_style;
    int n = 0, s = UI_DEV_XBOX;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0)
        s = style_of(SDL_GetGamepadTypeForID(ids[0]));
    SDL_free(ids);
    return s;
}

/* a cap: draw 0 measures it */
static float cap_box(float x, float y, float h, const char *label, int icon, int round, int draw)
{
    float size = h * 0.5f, tw = label ? ui_text_w(UI_MEDIUM, size, label) : 0;
    float w = round || icon >= 0 ? h : SDL_max(h, tw + h * 0.62f);
    if (!draw)
        return w;
    ui_round_line(x, y - h / 2, w, h, round ? h / 2 : h * 0.24f, 1, UI_LINE, UI_HOVER);
    if (icon >= 0)
        ui_icon(icon, x + w / 2, y, h * (round ? 0.78f : 0.62f), UI_TEXT);
    else if (label)
        ui_text(UI_MEDIUM, size, x + (w - tw) / 2, y, UI_TEXT, label);
    return w;
}

float ui_cap(float x, float y, float h, const char *label)
{
    return cap_box(x, y, h, label, -1, 0, 1);
}

float ui_cap_icon(float x, float y, float h, int icon)
{
    return cap_box(x, y, h, NULL, icon, 0, 1);
}

/* how a key or a gamepad input looks: a label or an icon, round or not */
static void input_look(const char *name, int pad, const char **label, int *icon, int *round)
{
    *label = name;
    *icon = -1;
    *round = 0;
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
    static const struct { const char *name, *xbox, *ps, *nin; int psicon; } pads[] = {
        { "a", "A", NULL, "B", UI_I_PS_CROSS }, { "b", "B", NULL, "A", UI_I_PS_CIRCLE },
        { "x", "X", NULL, "Y", UI_I_PS_SQUARE }, { "y", "Y", NULL, "X", UI_I_PS_TRIANGLE },
        { "leftshoulder", "LB", "L1", "L", -1 }, { "rightshoulder", "RB", "R1", "R", -1 },
        { "lefttrigger", "LT", "L2", "ZL", -1 }, { "righttrigger", "RT", "R2", "ZR", -1 },
        { "back", "View", "Share", "\xE2\x80\x93", -1 }, { "start", "Menu", "Options", "+", -1 },
        { "guide", "Guide", "PS", "Home", -1 }, { "leftstick", "LS", "L3", "LS", -1 },
        { "rightstick", "RS", "R3", "RS", -1 }, { "misc1", "Share", "Mute", "Capture", -1 },
        { "touchpad", "Pad", "Touchpad", "Pad", -1 },
    };
    if (!SDL_strcmp(name, "dpup")) *icon = UI_I_UP;
    else if (!SDL_strcmp(name, "dpdown")) *icon = UI_I_DOWN;
    else if (!SDL_strcmp(name, "dpleft")) *icon = UI_I_LEFT;
    else if (!SDL_strcmp(name, "dpright")) *icon = UI_I_RIGHT;
    if (*icon >= 0) {
        *label = NULL;
        return;
    }
    int st = ui_pad_style();
    for (size_t i = 0; i < SDL_arraysize(pads); i++)
        if (!SDL_strcmp(name, pads[i].name)) {
            *round = i < 4;
            if (st == UI_DEV_PS && pads[i].psicon >= 0) {
                *icon = pads[i].psicon;
                *label = NULL;
            } else {
                *label = st == UI_DEV_PS ? pads[i].ps : st == UI_DEV_NINTENDO ? pads[i].nin : pads[i].xbox;
            }
        }
}

float ui_cap_input(float x, float y, float h, const char *name, int pad)
{
    const char *label;
    int icon, round;
    input_look(name, pad, &label, &icon, &round);
    return cap_box(x, y, h, label, icon, round, 1);
}

float ui_cap_input_w(float h, const char *name, int pad)
{
    const char *label;
    int icon, round;
    input_look(name, pad, &label, &icon, &round);
    return cap_box(0, 0, h, label, icon, round, 0);
}

/* a hint's inputs as names: keyboard (pad 0) or gamepad */
static int hint_names(int in, int pad, const char **out)
{
    static const char *const keys[][2] = {
        { "Up", "Down" }, { "Left", "Right" }, { "Return", NULL }, { "Escape", NULL }, { "Q", NULL }, { "E", NULL },
        { "Delete", NULL }, { "Backspace", NULL },
    };
    static const char *const pads[][2] = {
        { "dpup", "dpdown" }, { "dpleft", "dpright" }, { "a", NULL }, { "b", NULL }, { "leftshoulder", NULL },
        { "rightshoulder", NULL }, { "x", NULL }, { "y", NULL },
    };
    if (in < 0 || in >= UI_IN_NONE)
        return 0;
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
            ui_icon(UI_I_LEFT, bx + as / 2, mid, as, arrow);
            ui_icon(UI_I_RIGHT, right - as / 2, mid, as, arrow);
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
            ui_text_in(UI_REGULAR, vsize, x + w * 0.45f, mid, right - x - w * 0.45f, 2, 0, UI_MUTED, r->value);
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
