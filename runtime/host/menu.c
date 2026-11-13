/* The game's own menu: see menu.h. */
#include "menu.h"

#include <stdio.h>
#include <string.h>

enum {
    R_RESUME, R_CREDITS, R_PICTURE, R_FILTER, R_FULLSCREEN, R_VOLUME, R_RENDERER, R_SCALE, R_CARDS,
    R_TEST, R_RESTART, R_QUIT, R_COUNT
};

static settings *s;
static menu_run run;
static settings before;     /* as it was when opened */
static int row, armed = -1; /* armed: Restart or Quit, chosen once (the second choice does it) */

void menu_open(settings *cfg, const menu_run *r)
{
    s = cfg;
    run = *r;
    before = *cfg;
    row = R_RESUME;
    armed = -1;
}

int menu_changed(void)
{
    return s && memcmp(s, &before, sizeof before) != 0;
}

static int wrap(int v, int d, int n)
{
    return ((v + d) % n + n) % n;
}

/* Left/Right (d -1, +1) on a setting; MENU_LIVE when it applies at once */
static int change(int d)
{
    switch (row) {
    case R_CREDITS: s->coins = wrap(s->coins, d, 2); return MENU_STAY;
    case R_PICTURE: s->picture = wrap(s->picture, d, 2); return MENU_LIVE;
    case R_FILTER: s->filter = wrap(s->filter, d, 2); return MENU_LIVE;
    case R_FULLSCREEN: s->fullscreen = wrap(s->fullscreen + 1, d, 3) - 1; return MENU_LIVE;
    case R_VOLUME:
        if (!run.sound)
            return MENU_STAY;
        s->volume += 10 * d;
        if (s->volume < 0) s->volume = 0;
        if (s->volume > 100) s->volume = 100;
        return MENU_LIVE;
    case R_RENDERER: s->renderer = wrap(s->renderer, d, 2); return MENU_STAY;
    case R_SCALE: s->scale = wrap(s->scale - 1, d, 4) + 1; return MENU_STAY;
    case R_CARDS: s->cards = !s->cards; return MENU_STAY;
    }
    return MENU_STAY;
}

int menu_command(int cmd)
{
    int was_armed = armed;
    armed = -1;
    switch (cmd) {
    case MENU_UP: row = wrap(row, -1, R_COUNT); return MENU_STAY;
    case MENU_DOWN: row = wrap(row, 1, R_COUNT); return MENU_STAY;
    case MENU_LEFT: return change(-1);
    case MENU_RIGHT: return change(1);
    case MENU_BACK: return MENU_RESUME;
    case MENU_OK:
        switch (row) {
        case R_RESUME: return MENU_RESUME;
        case R_TEST: return run.test_program ? MENU_OPERATOR : MENU_TEST;
        case R_RESTART:
        case R_QUIT:
            if (was_armed != row) {
                armed = row;        /* asks once more */
                return MENU_STAY;
            }
            return row == R_RESTART ? MENU_RESTART : MENU_QUIT;
        default:
            return change(1);
        }
    }
    return MENU_STAY;
}

/* ---- drawing ---- */

static SDL_Renderer *ren;

static void text(float x, float y, const char *t, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderDebugText(ren, x, y, t);
}

static void fill(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderFillRect(ren, &(SDL_FRect){ x, y, w, h });
}

static const char *row_label(int r)
{
    static const char *const labels[R_COUNT] = {
        "Resume", "Credits", "Picture", "Scaling", "Fullscreen", "Volume", "Renderer", "Resolution", "Card units",
        "Operator test menu", "Restart the game", "Quit",
    };
    if (r == R_TEST && run.test_program)
        return "Operator buttons";
    if (r == armed)
        return r == R_RESTART ? "Restart the game? A again" : "Quit? A again";
    return labels[r];
}

/* the value; *later: it applies at the next start and differs from this run */
static void row_value(int r, char *v, size_t n, int *later)
{
    static const char *const fs[] = { "Auto", "Off", "On" };
    *later = 0;
    v[0] = 0;
    switch (r) {
    case R_CREDITS: SDL_strlcpy(v, s->coins == COINS_ARCADE ? "Arcade coins" : "Free play", n); break;
    case R_PICTURE: SDL_strlcpy(v, s->picture == PICTURE_STRETCH ? "Stretch" : "4:3", n); break;
    case R_FILTER: SDL_strlcpy(v, s->filter == FILTER_SHARP ? "Sharp" : "Smooth", n); break;
    case R_FULLSCREEN: SDL_strlcpy(v, fs[s->fullscreen + 1], n); break;
    case R_VOLUME:
        if (run.sound)
            SDL_snprintf(v, n, "%d%%", s->volume);
        else
            SDL_strlcpy(v, "(sound off)", n);
        break;
    case R_RENDERER:
        SDL_strlcpy(v, s->renderer == RENDERER_GPU ? "GPU (Vulkan)" : "Software", n);
        *later = s->renderer != run.renderer;
        break;
    case R_SCALE:
        SDL_snprintf(v, n, "%d x %d", 640 * s->scale, 480 * s->scale);
        *later = s->scale != run.scale;
        break;
    case R_CARDS:
        SDL_strlcpy(v, s->cards ? "On" : "Off", n);
        *later = s->cards != run.cards;
        break;
    }
}

static const char *row_note(int r)
{
    switch (r) {
    case R_RESUME: return "Back to the game";
    case R_CREDITS: return "Applies when the game restarts";
    case R_PICTURE: return "4:3, or stretched to the screen";
    case R_FILTER: return "How the picture is scaled up";
    case R_FULLSCREEN: return "Auto: on for Steam Deck, Big Picture";
    case R_VOLUME: return run.sound ? "The game's sound" : "Sound is off: the launcher's Settings";
    case R_RENDERER: return "* applies the next time it starts";
    case R_SCALE: return s->renderer == RENDERER_GPU ? "* GPU only; applies at the next start"
                                                     : "The GPU renderer's (not in use)";
    case R_CARDS: return "* VF.NET cards; applies at next start";
    case R_TEST: return run.test_program ? "A: Test, X: Service (F2, F3)" : "The cabinet's Test switch";
    case R_RESTART: return "Off and on again; records are kept";
    case R_QUIT: return "Saves, then closes the game";
    }
    return "";
}

void menu_draw(SDL_Renderer *r)
{
    ren = r;
    fill(0, 0, MENU_W, MENU_H, 90, 100, 130);           /* the border */
    fill(1, 1, MENU_W - 2, MENU_H - 2, 20, 22, 30);
    text(8, 7, "Virtua Fighter 4 Final Tuned", 255, 200, 80);
    text(MENU_W - 8 - 6 * 8, 7, "PAUSED", 140, 150, 170);
    fill(6, 18, MENU_W - 12, 1, 60, 66, 86);
    for (int i = 0; i < R_COUNT; i++) {
        float y = 24.0f + 12.0f * (float)i;
        if (i == row)
            fill(4, y - 2, MENU_W - 8, 12, 50, 70, 110);
        char v[40];
        int later;
        row_value(i, v, sizeof v, &later);
        int red = i == armed;
        text(12, y, row_label(i), 255, red ? 140 : 255, red ? 120 : 255);
        if (v[0]) {
            text(164, y, v, 255, 220, 140);
            if (later)
                text(164 + 8.0f * (float)strlen(v), y, "*", 255, 140, 120);
        }
    }
    fill(6, 170, MENU_W - 12, 1, 60, 66, 86);
    text(12, 176, row_note(row), 170, 200, 230);
    text(8, 192, "Up/Down choose, Left/Right change", 150, 170, 150);
    text(8, 203, "A or Enter: select  B or Esc: back", 150, 170, 150);
}

void menu_draw_bar(SDL_Renderer *r)
{
    ren = r;
    fill(0, 0, MENU_W, MENU_BAR_H, 90, 100, 130);
    fill(1, 1, MENU_W - 2, MENU_BAR_H - 2, 20, 22, 30);
    text(6, 2, "Test: A/F2  Service: X/F3  Close: B", 255, 220, 140);
}
