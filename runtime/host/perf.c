/* The frame rate display: perf.h. */
#include "perf.h"

#include "ui.h"

/* the panel in the kit's units, of which 720 are the window's height. 152 wide: with its margin it
 * stays inside the black side of a 4:3 picture on a 16:9 screen (160 units), and left of player
 * 1's card signs when the picture fills the window */
#define PW 152.0f
#define PH_NUMBERS 60.0f
#define PH_GRAPH 98.0f
#define GRAPH_MS 33.4f      /* the graph's height: two frames of a 60 a second game */
#define FRAME_MS 16.6833f   /* one */
#define SLOW_MS 20.0f       /* a bar longer than this is marked */

static float span[PERF_SPAN];   /* ms from each picture to the next, a ring: at is the next to write */
static int at, held;
static Uint64 last_ns;
static float game_avg = -1, rays_avg = -1;  /* smoothed (below 0: nothing yet) */
static Uint64 said_at;                      /* when the numbers were last written (ms) */
static char said_fps[16], said_ms[24], said_work[48], said_slow[32];

void perf_reset(void)
{
    at = held = 0;
    last_ns = 0;
    game_avg = rays_avg = -1;
    said_at = 0;
    said_fps[0] = said_ms[0] = said_work[0] = said_slow[0] = 0;
}

void perf_picture(Uint64 ns)
{
    if (last_ns && ns > last_ns) {
        float ms = (float)((double)(ns - last_ns) / 1e6);
        if (ms >= 1000) {
            at = held = 0;      /* a pause: what was before it says nothing of what comes after */
        } else {
            span[at] = ms;
            at = (at + 1) % PERF_SPAN;
            if (held < PERF_SPAN)
                held++;
        }
    }
    last_ns = ns;
}

static float scale_of(int ph)
{
    float k = (float)ph / 720.0f;
    return k < 1 ? 1 : k;
}

void perf_size(int graph, int ph, int *w, int *h)
{
    float k = scale_of(ph);
    *w = (int)(PW * k + 0.5f);
    *h = (int)((graph ? PH_GRAPH : PH_NUMBERS) * k + 0.5f);
}

/* the i-th newest time (0: the newest) */
static float newest(int i)
{
    return span[((at - 1 - i) % PERF_SPAN + PERF_SPAN) % PERF_SPAN];
}

static void smooth(float *avg, float now)
{
    if (now < 0)
        *avg = -1;
    else
        *avg = *avg < 0 ? now : *avg + (now - *avg) * 0.1f;
}

void perf_draw(int graph, int w, int h, float game_ms, float rays_ms)
{
    Uint64 now = SDL_GetTicks();
    float k = (float)w / PW, H = graph ? PH_GRAPH : PH_NUMBERS;
    (void)h;
    smooth(&game_avg, game_ms);
    smooth(&rays_avg, rays_ms);
    if (!said_at || now - said_at >= 250) {
        /* the last half second's pictures (30 of a 60 a second game), and the slowest the graph holds */
        int n = held < 30 ? held : 30;
        float sum = 0, worst = 0;
        for (int i = 0; i < n; i++)
            sum += newest(i);
        for (int i = 0; i < held; i++)
            if (newest(i) > worst)
                worst = newest(i);
        said_at = now;
        if (n && sum > 0) {
            SDL_snprintf(said_fps, sizeof said_fps, "%.1f", (double)(1000.0f * (float)n / sum));
            SDL_snprintf(said_ms, sizeof said_ms, "%.1f ms", (double)(sum / (float)n));
        } else {
            SDL_strlcpy(said_fps, "-", sizeof said_fps);
            SDL_strlcpy(said_ms, "- ms", sizeof said_ms);
        }
        if (game_avg >= 0 && rays_avg >= 0)
            SDL_snprintf(said_work, sizeof said_work, "game %.1f   rays %.1f", (double)game_avg, (double)rays_avg);
        else if (game_avg >= 0)
            SDL_snprintf(said_work, sizeof said_work, "game %.1f ms", (double)game_avg);
        else
            said_work[0] = 0;
        SDL_snprintf(said_slow, sizeof said_slow, "slowest %.1f ms", (double)worst);
    }
    ui_set_scale(k, 0, 0);
    ui_fill(0, 0, PW, H, (SDL_Color){ 14, 16, 22, 255 });
    ui_fill(0, 0, PW, 1, UI_LINE);
    ui_fill(PW - 1, 0, 1, H, UI_LINE);
    float x = 8 + ui_text(UI_BOLD, 17, 8, 15, UI_TEXT, said_fps);
    ui_text(UI_REGULAR, 12, x + 4, 16, UI_MUTED, "fps");
    ui_text_in(UI_MEDIUM, 13, 0, 15, PW - 8, 2, 0, UI_TEXT, said_ms);
    ui_text_in(UI_REGULAR, 11.5f, 8, 33, PW - 16, 0, 0, UI_MUTED, said_work);
    ui_text_in(UI_REGULAR, 11.5f, 8, 48, PW - 16, 0, 0, UI_MUTED, said_slow);
    if (!graph)
        return;
    /* the graph: a bar a picture, the newest at the right, GRAPH_MS high, a line at one frame's time */
    const float gx = 8, gy = 58, gw = PW - 16, gh = 32, bw = gw / PERF_SPAN;
    ui_fill(gx, gy, gw, gh, (SDL_Color){ 24, 27, 36, 255 });
    for (int i = 0; i < held; i++) {
        float ms = newest(i), t = ms > GRAPH_MS ? GRAPH_MS : ms;
        float bh = t / GRAPH_MS * gh;
        if (bh < 1)
            bh = 1;
        /* a pixel wider than its share (but for the newest): a bar's width is not a whole number of
         * pixels, and a gap between two would look like a missing picture */
        ui_fill(gx + gw - (float)(i + 1) * bw, gy + gh - bh, i ? bw + 1.0f / k : bw, bh, ms > SLOW_MS ? UI_ACCENT : UI_GOOD);
    }
    ui_fill(gx, gy + gh - FRAME_MS / GRAPH_MS * gh, gw, 1, UI_FAINT);
}
