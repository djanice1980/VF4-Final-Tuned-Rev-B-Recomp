/* The game's own menu: see menu.h. */
#include "menu.h"

#include <string.h>

#include "cards.h"
#include "options.h"
#include "ui.h"
#include "updates.h"

enum { TAB_GAME, TAB_CARDS, TAB_PICTURE, TAB_SOUND, TAB_CONTROLS, TABS };
static const char *const tab_names[TABS] = { "Game", "Cards", "Picture", "Sound", "Controls" };

/* a row: an option (options.h), one of these actions, or a control */
enum { A_RESUME = OPT_COUNT, A_TEST, A_RESTART, A_UPDATES, A_KOFI, A_LAUNCHER, A_QUIT, A_CONTROL };

#define ROWS 11
static const int tab_rows[TABS][ROWS] = {
    [TAB_GAME] = { A_RESUME, OPT_CREDITS, OPT_NETWORK, A_TEST, A_RESTART, A_UPDATES, A_KOFI, A_LAUNCHER, A_QUIT, -1 },
    [TAB_CARDS] = { OPT_CARD1, OPT_CARD2, OPT_CARDS, -1 },
    [TAB_PICTURE] = { OPT_FULLSCREEN, OPT_WINDOW, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_SCALE, -1 },
    [TAB_SOUND] = { OPT_VOLUME, OPT_SOUND, -1 },
};
static int shown_rows[TABS][ROWS], nshown[TABS];   /* tab_rows without Updates and the launcher outside a release folder */

static settings *s;
static menu_run run;
static settings before;     /* as it was when opened */
static int tab, row, top, armed = -1; /* armed: Restart or Quit, chosen once (the second choice does it) */
static int notice, notice_focus;    /* the first start's Ko-fi notice; its buttons: 0 Support on Ko-fi, 1 Close */
static int kofi_said = -1;          /* the Ko-fi page was opened (1) or not (0) since the menu opened */
/* for the mouse: the last drawing's target (0 until it is drawn after
 * opening: the hit tests are its), the notice's buttons and Ko-fi's badge
 * (logical units; w 0 when not drawn) */
static int drawn_w, drawn_h;
static SDL_FRect notice_btn[2], badge_at;

void menu_open(settings *cfg, const menu_run *r)
{
    s = cfg;
    run = *r;
    before = *cfg;
    tab = TAB_GAME;
    row = top = 0;
    armed = -1;
    notice = 0;
    kofi_said = -1;
    drawn_w = drawn_h = 0;
    for (int t = 0; t < TABS; t++) {
        nshown[t] = 0;
        for (int i = 0; i < ROWS && tab_rows[t][i] >= 0; i++)
            if ((tab_rows[t][i] != A_UPDATES || gu_state() != GU_OFF) && (tab_rows[t][i] != A_LAUNCHER || gu_launcher()))
                shown_rows[t][nshown[t]++] = tab_rows[t][i];
    }
}

void menu_open_notice(settings *cfg, const menu_run *r)
{
    menu_open(cfg, r);
    notice = 1;
    notice_focus = 1;       /* Close: a button pressed as the notice comes up opens no browser */
}

void menu_kofi(int opened)
{
    kofi_said = opened != 0;
}

int menu_changed(void)
{
    return s && memcmp(s, &before, sizeof before) != 0;
}

static int rows_in(int t)
{
    return t == TAB_CONTROLS ? ACT_COUNT : nshown[t];
}

static int row_id(int r)
{
    return tab == TAB_CONTROLS ? A_CONTROL : shown_rows[tab][r];
}

/* Updates is a toggle once a newer release is out: "Update when I quit" */
static int updates_toggle(void)
{
    return gu_state() == GU_AVAILABLE || gu_state() == GU_ASKED;
}

static int wrap(int v, int d, int n)
{
    return ((v + d) % n + n) % n;
}

/* Left/Right (d -1, +1) on an option; MENU_LIVE when it applies at once */
static int change(int d)
{
    int id = row_id(row);
    if (id == A_UPDATES && updates_toggle())
        gu_press();
    if (id >= OPT_COUNT)
        return MENU_STAY;
    if (id == OPT_VOLUME && !run.sound)
        return MENU_STAY;
    if (id == OPT_SCALE && s->renderer != RENDERER_GPU)
        return MENU_STAY;
    option_change(s, id, d);
    return id == OPT_PICTURE || id == OPT_FILTER || id == OPT_FULLSCREEN || id == OPT_WINDOW || id == OPT_VOLUME
               ? MENU_LIVE
               : MENU_STAY;
}

int menu_command(int cmd)
{
    if (notice) {
        switch (cmd) {
        case MENU_UP:
        case MENU_DOWN:
        case MENU_LEFT:
        case MENU_RIGHT: notice_focus ^= 1; return MENU_STAY;
        case MENU_OK: return notice_focus == 0 ? MENU_KOFI_CLOSE : MENU_RESUME;
        case MENU_BACK: return MENU_RESUME;
        default: return MENU_STAY;
        }
    }
    int was_armed = armed;
    armed = -1;
    switch (cmd) {
    case MENU_UP: row = wrap(row, -1, rows_in(tab)); return MENU_STAY;
    case MENU_DOWN: row = wrap(row, 1, rows_in(tab)); return MENU_STAY;
    case MENU_PREV:
    case MENU_NEXT:
        tab = wrap(tab, cmd == MENU_PREV ? -1 : 1, TABS);
        row = top = 0;
        return MENU_STAY;
    case MENU_LEFT: return change(-1);
    case MENU_RIGHT: return change(1);
    case MENU_BACK: return MENU_RESUME;
    case MENU_OK:
        switch (row_id(row)) {
        case A_RESUME: return MENU_RESUME;
        case A_TEST: return run.test_program ? MENU_OPERATOR : MENU_TEST;
        case A_RESTART:
        case A_LAUNCHER:
        case A_QUIT:
            if (was_armed != row_id(row)) {
                armed = row_id(row);    /* asks once more */
                return MENU_STAY;
            }
            return row_id(row) == A_RESTART ? MENU_RESTART : row_id(row) == A_LAUNCHER ? MENU_LAUNCHER : MENU_QUIT;
        case A_UPDATES: gu_press(); return MENU_STAY;
        case A_KOFI: return MENU_KOFI;
        case A_CONTROL: return MENU_STAY;
        default: return change(1);
        }
    }
    return MENU_STAY;
}

/* ---- the mouse ---- */

static int inside(const SDL_FRect *r, float x, float y)
{
    return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* chooses row r (another row: Restart or Quit asks from the start again) */
static void choose(int r)
{
    if (r != row)
        armed = -1;
    row = r;
}

int menu_mouse(int what, float px, float py, int d)
{
    if (!drawn_w)
        return -1;
    float x, y;
    ui_fit(1180, 680, drawn_w, drawn_h);      /* menu_draw's: the operator bar draws at its own scale */
    ui_to_logical(px, py, &x, &y);
    if (notice) {
        int b = inside(&notice_btn[0], x, y) || inside(&badge_at, x, y) ? 0 : inside(&notice_btn[1], x, y) ? 1 : -1;
        switch (what) {
        case MENU_MOUSE_MOVE:
            if (b < 0 || b == notice_focus)
                return -1;
            notice_focus = b;
            return MENU_STAY;
        case MENU_MOUSE_LEFT: return b == 0 ? MENU_KOFI_CLOSE : b == 1 ? MENU_RESUME : -1;
        case MENU_MOUSE_RIGHT: return MENU_RESUME;
        default: return -1;
        }
    }
    int r = ui_rows_hit(x, y), t = ui_tabs_hit(x, y);
    switch (what) {
    case MENU_MOUSE_MOVE:
        if (r < 0 || r == row)
            return -1;
        choose(r);
        return MENU_STAY;
    case MENU_MOUSE_WHEEL: {
        int to = SDL_clamp(row + d, 0, rows_in(tab) - 1);
        if (to == row)
            return -1;
        choose(to);
        return MENU_STAY;
    }
    case MENU_MOUSE_LEFT:
        if (t == UI_TAB_PREV || t == UI_TAB_NEXT)
            return menu_command(t == UI_TAB_PREV ? MENU_PREV : MENU_NEXT);
        if (t >= 0) {
            if (t == tab)
                return -1;
            tab = t;
            row = top = 0;
            armed = -1;
            return MENU_STAY;
        }
        if (inside(&badge_at, x, y))
            return MENU_KOFI;
        if (r < 0)
            return -1;
        choose(r);
        return menu_command(MENU_OK);   /* Restart, Quit and Open the launcher: a second click */
    case MENU_MOUSE_RIGHT:
        if (r < 0)
            return menu_command(MENU_BACK);
        choose(r);
        return menu_command(MENU_LEFT); /* an option one back; nothing on an action */
    default:
        return -1;
    }
}

/* ---- drawing ---- */

/* Ko-fi's badge, w wide (its own shape: 672 x 356); a plain button in its
 * color when SDL cannot read PNGs (before 3.4) */
static float kofi_badge(float x, float y, float w)
{
    float h = w * 356 / 672;
    if (!ui_png(ui_kofi_badge_png, ui_kofi_badge_png_size, x, y, w, h)) {
        ui_round(x, y, w, h, h * 0.2f, (SDL_Color){ 255, 90, 22, 255 });
        ui_text_in(UI_BOLD, h * 0.2f, x, y + h / 2, w, 1, 0, (SDL_Color){ 255, 255, 255, 255 },
                   "Support me on Ko-fi");
    }
    return h;
}

/* a button of the notice; focused, it is in the accent */
static float notice_button(float x, float y, const char *label, int on)
{
    float w = ui_text_w(UI_MEDIUM, 16, label) + 52;
    ui_round(x, y, w, 46, 12, on ? UI_ACCENT : UI_RAISED);
    ui_text_in(UI_MEDIUM, 16, x, y + 23, w, 1, 0, on ? UI_ON_ACCENT : UI_TEXT, label);
    return w;
}

/* the first start's notice (menu_open_notice), like the author's pcbview's */
static void draw_notice(void)
{
    const float W = 620, H = 512, X = (1180 - W) / 2, Y = (680 - H) / 2, P = 34;
    ui_round_line(X, Y, W, H, 20, 1, UI_LINE, UI_PANEL);
    ui_text(UI_BOLD, 21, X + P, Y + 44, UI_TEXT, "Support this recomp");
    float y = Y + 84;
    y += 23 * (float)ui_wrap(UI_REGULAR, 15.5f, X + P, y, W - 2 * P, 23, UI_MUTED,
                             "This recomp of Virtua Fighter 4 Final Tuned is free. If you enjoy it, you can "
                             "support its development on Ko-fi.");
    float bw = 250, bh = kofi_badge(X + (W - bw) / 2, y, bw);
    badge_at = (SDL_FRect){ X + (W - bw) / 2, y, bw, bh };
    y += bh + 26;
    ui_text_in(UI_MEDIUM, 15, X, y, W, 1, 0, UI_TEXT, MENU_KOFI_SHOWN);
    y += 34;
    ui_text_in(UI_REGULAR, 13.5f, X, y, W, 1, 0, UI_FAINT,
               "Support on Ko-fi stays in the game's menu, on the Game tab.");
    ui_text_in(UI_REGULAR, 13.5f, X, y + 20, W, 1, 0, UI_FAINT, "This notice won't show again.");
    /* the buttons, in the middle */
    const char *l0 = "Support on Ko-fi", *l1 = "Close";
    float w0 = ui_text_w(UI_MEDIUM, 16, l0) + 52, w1 = ui_text_w(UI_MEDIUM, 16, l1) + 52;
    float bx = X + (W - w0 - 14 - w1) / 2, by = Y + H - 62 - 66;
    notice_btn[0] = (SDL_FRect){ bx, by, w0, 46 };
    notice_btn[1] = (SDL_FRect){ bx + w0 + 14, by, w1, 46 };
    bx += notice_button(bx, by, l0, notice_focus == 0) + 14;
    notice_button(bx, by, l1, notice_focus == 1);
    ui_fill(X + 30, Y + H - 62, W - 60, 1, UI_LINE);
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_LEFTRIGHT, UI_IN_NONE } }, { "Select", { UI_IN_OK, UI_IN_NONE } },
        { "Close", { UI_IN_BACK, UI_IN_NONE } },
    };
    ui_hints(X + W - 32, Y + H - 31, 26, hints, (int)SDL_arraysize(hints), 2);
}

/* the option's value differs from the run's and it applies at the next start */
static int later(int id)
{
    switch (id) {
    case OPT_CREDITS: return s->coins != run.start.coins;
    case OPT_CARDS: return s->cards != run.start.cards;
    case OPT_NETWORK: return s->network != run.start.network;
    case OPT_RENDERER: return s->renderer != run.start.renderer;
    case OPT_SCALE: return s->scale != run.start.scale;
    case OPT_SOUND: return s->sound != run.start.sound;
    }
    return 0;
}

/* Player 1 card or Player 2 card with a card chosen: the card's details
 * (card_view_text, as the launcher's Cards tab shows them), read from its
 * file again when another card is chosen or half a second has gone by (the
 * game writes the units' cards as the menu opens) */
static void card_about(int side, const char **title, const char **text, const char **note)
{
    static char name[CARD_NAME_MAX], body[600];
    static Uint64 at;
    Uint64 now = SDL_GetTicks();
    if (strcmp(name, s->card[side]) || now - at >= 500 || !body[0]) {
        char path[4300];
        card_view v;
        int file = 0;
        SDL_zero(v);
        SDL_strlcpy(name, s->card[side], sizeof name);
        at = now;
        cards_path(path, sizeof path, run.user_dir ? run.user_dir : "", name);
        if (SDL_GetPathInfo(path, NULL)) {
            size_t len = 0;
            Uint8 *c = SDL_LoadFile(path, &len);
            file = c && len == CARD_SIZE ? 1 : -1;
            if (file > 0)
                card_view_of(c, &v);
            SDL_free(c);
        }
        card_view_text(&v, file, body, sizeof body);
    }
    *title = name;
    *text = body;
    *note = !s->cards ? "Card units are off (below): the game uses none."
                      : "Left and Right choose the card; the other player's card swaps the two. Cards change as the "
                        "menu closes.";
}

static void about_of(int id, const char **title, const char **text, const char **note)
{
    *note = NULL;
    if ((id == OPT_CARD1 || id == OPT_CARD2) && s->card[id - OPT_CARD1][0]) {
        card_about(id - OPT_CARD1, title, text, note);
        return;
    }
    if (id < OPT_COUNT) {
        *title = options[id].label;
        *text = options[id].about;
        if (id == OPT_CREDITS || id == OPT_NETWORK)
            *note = "Applies when the game restarts (Restart the game).";
        else if (id == OPT_VOLUME && !run.sound)
            *note = "Sound is off: turn it on below, for the next start.";
        else if (id == OPT_SCALE && s->renderer != RENDERER_GPU)
            *note = "The Software renderer does not use it.";
        else if (options[id].at_start)
            *note = later(id) ? "Changed: applies the next time the game starts." : "Applies the next time the game starts.";
        return;
    }
    switch (id) {
    case A_RESUME: *title = "Resume"; *text = "Back to the game, where it paused."; break;
    case A_TEST:
        if (run.test_program) {
            *title = "Operator buttons";
            *text = "The test menu runs. While the operator bar shows, A is the Test switch (it chooses) "
                    "and X is Service (it moves). B gives the buttons back to the game.";
        } else {
            *title = "Operator test menu";
            *text = "The cabinet's Test switch: the game's own test menu, for its settings, records and "
                    "checks. Then A is Test (it chooses) and X is Service (it moves), as F2 and F3 are.";
        }
        break;
    case A_RESTART:
        *title = "Restart the game";
        *text = "Off and on again, as the cabinet's power switch. Records and settings are kept.";
        break;
    case A_QUIT:
        *title = "Quit";
        *text = "Saves the records and closes the game, and the launcher too when it started the game (Open the "
                "launcher goes back to it).";
        break;
    case A_LAUNCHER:
        *title = "Open the launcher";
        *text = "Saves the records, closes the game and opens the launcher: its settings, the install folder, "
                "your game files, updates, and Start the game directly.";
        break;
    case A_KOFI:
        *title = "Support on Ko-fi";
        *text = "This recomp is free. If you enjoy it, you can support its development on Ko-fi: "
                MENU_KOFI_SHOWN;
        if (kofi_said == 1)
            *note = "Sent to your browser.";
        else if (kofi_said == 0)
            *note = "No browser opened: the address is above.";
        break;
    case A_UPDATES: {
        static char t[600], mb[48];
        *title = "Check for updates";
        switch (gu_state()) {
        case GU_CHECKING: *text = "Asking GitHub\xE2\x80\xA6"; break;
        case GU_NONE: SDL_snprintf(t, sizeof t, "%s is the newest release.", gu_version()); *text = t; break;
        case GU_FAILED: SDL_snprintf(t, sizeof t, "The check did not work: %s", gu_error()); *text = t; break;
        case GU_AVAILABLE:
        case GU_ASKED:
            *title = "Update when I quit";
            mb[0] = 0;
            if (gu_size() > 0)
                SDL_snprintf(mb, sizeof mb, " (%lld MB)", SDL_max(gu_size() / 1000000, 1));
            SDL_snprintf(t, sizeof t,
                         gu_by_launcher()
                             ? "%s is out (this is %s). With this on, when you quit, the launcher downloads it%s, "
                               "puts it in place and builds the game again. Your records and settings stay."
                             : "%s is out (this is %s). With this on, quitting the game starts the launcher: it "
                               "downloads the update%s, puts it in place, builds the game again and closes. Your "
                               "records and settings stay.",
                         gu_newer(), gu_version(), mb);
            *text = t;
            if (gu_state() == GU_ASKED)
                *note = "On: it goes in when you quit.";
            break;
        default:
            SDL_snprintf(t, sizeof t, "Asks GitHub whether a newer release of the launcher and builder is out. "
                         "This is %s.", gu_version());
            *text = t;
            break;
        }
        break;
    }
    default:
        *title = "Controls";
        *text = ui_device() == UI_DEV_KEYS
                    ? "The keyboard's keys for player 1. Change them in the launcher's Settings."
                    : "Every gamepad's buttons (the first is player 1, the second player 2). Change them in "
                      "the launcher's Settings; the left stick always moves.";
        break;
    }
}

void menu_draw(SDL_Renderer *r, int pw, int ph, SDL_Texture *backdrop)
{
    (void)r;
    ui_set_scale(1, 0, 0);
    if (backdrop)
        ui_image(backdrop, 0, 0, (float)pw, (float)ph);     /* blurred and darkened already (main.c) */
    else
        ui_fill(0, 0, (float)pw, (float)ph, UI_BG);

    /* the panel: 1100 x 620 units in the middle */
    ui_fit(1180, 680, pw, ph);
    drawn_w = pw;
    drawn_h = ph;
    badge_at.w = 0;
    if (notice) {
        draw_notice();
        return;
    }
    const float X = 40, Y = 30, W = 1100, H = 620;
    ui_round_line(X, Y, W, H, 20, 1, UI_LINE, UI_PANEL);
    ui_text(UI_BOLD, 21, X + 34, Y + 40, UI_TEXT, "Virtua Fighter 4 Final Tuned");
    float cw = ui_text_w(UI_MEDIUM, 13.5f, "PAUSED") + 26;
    ui_round(X + W - 34 - cw, Y + 27, cw, 26, 13, UI_RAISED);
    ui_text(UI_MEDIUM, 13.5f, X + W - 34 - cw + 13, Y + 40, UI_MUTED, "PAUSED");
    ui_tabs(X + 30, Y + 70, W - 60, 46, tab_names, TABS, tab);
    ui_fill(X + 30, Y + 132, W - 60, 1, UI_LINE);

    /* the rows */
    ui_row rows[ACT_COUNT];
    char vals[ACT_COUNT][160];
    int n = rows_in(tab);
    for (int i = 0; i < n; i++) {
        int id = row_id(i);
        ui_row *w = &rows[i];
        if (id < OPT_COUNT) {
            option_row(s, id, w, vals[i], sizeof vals[i]);
            w->later = options[id].at_start && later(id);
            if (id == OPT_CREDITS || id == OPT_NETWORK)
                w->later = later(id);
            if (id == OPT_VOLUME && !run.sound)
                w->off = 1;
            continue;
        }
        SDL_zerop(w);
        w->kind = UI_ROW_ACTION;
        switch (id) {
        case A_RESUME: w->label = "Resume"; break;
        case A_TEST: w->label = run.test_program ? "Operator buttons" : "Operator test menu"; break;
        case A_RESTART: w->label = armed == A_RESTART ? "Restart the game? Choose it again" : "Restart the game"; break;
        case A_QUIT: w->label = armed == A_QUIT ? "Quit? Choose it again" : "Quit"; break;
        case A_KOFI: w->label = "Support on Ko-fi"; break;
        case A_LAUNCHER: w->label = armed == A_LAUNCHER ? "Open the launcher? Choose it again" : "Open the launcher"; break;
        case A_UPDATES: {
            static char label[96];
            int st = gu_state();
            w->label = "Check for updates";
            if (updates_toggle()) {
                SDL_snprintf(label, sizeof label, "Update to %s when I quit", gu_newer());
                w->label = label;
                w->kind = UI_ROW_TOGGLE;
                w->on = st == GU_ASKED;
            } else if (st != GU_IDLE) {
                w->kind = UI_ROW_TEXT;
                w->value = st == GU_CHECKING ? "Checking\xE2\x80\xA6" : st == GU_NONE ? "Up to date" : "Could not check";
            }
            break;
        }
        default: {
            int keys = ui_device() == UI_DEV_KEYS;
            w->label = settings_action_text[i];
            w->kind = keys ? UI_ROW_KEYS : UI_ROW_PAD;
            settings_binding_text(keys ? &s->key[0][i] : &s->pad[i], vals[i], sizeof vals[i]);
            w->value = vals[i];
            break;
        }
        }
        w->warn = armed >= 0 && armed == id;
    }
    /* 39 units a row: a release's Game tab (ten rows, with Network, Check for
     * updates and Open the launcher) fits in rh = 394 without scrolling; at 46
     * Quit was below the fold (nine rows then), at 43 the tenth was. An
     * eleventh row there needs this looked at again. The Controls tab
     * scrolls. */
    float ry = Y + 150, rh = H - 150 - 76;
    ui_rows(X + 30, ry, 640, rh, 39, rows, n, row, &top);

    const char *title, *text, *note;
    about_of(row_id(row), &title, &text, &note);
    ui_about(X + 694, ry, W - 694 - 30, rh, title, text, note);
    if (row_id(row) == A_KOFI) {         /* the badge at the foot of the panel */
        float bw = 220, aw = W - 694 - 30, bh = bw * 356 / 672;
        badge_at = (SDL_FRect){ X + 694 + (aw - bw) / 2, ry + rh - 24 - bh, bw, bh };
        kofi_badge(badge_at.x, badge_at.y, bw);
    }

    ui_fill(X + 30, Y + H - 62, W - 60, 1, UI_LINE);
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Change", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } }, { "Select", { UI_IN_OK, UI_IN_NONE } },
        { "Close", { UI_IN_BACK, UI_IN_NONE } },
    };
    static const ui_hint view_hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Tabs", { UI_IN_PREV, UI_IN_NEXT } },
        { "Close", { UI_IN_BACK, UI_IN_NONE } },
    };
    if (tab == TAB_CONTROLS)
        ui_hints(X + W - 32, Y + H - 31, 26, view_hints, (int)SDL_arraysize(view_hints), 2);
    else
        ui_hints(X + W - 32, Y + H - 31, 26, hints, (int)SDL_arraysize(hints), 2);
    if (tab != TAB_CONTROLS) {
        int any = 0;
        for (int i = 0; i < n; i++)
            any |= rows[i].later;
        if (any) {
            ui_icon(UI_I_DOT, X + 40, Y + H - 31, 14, UI_ACCENT);
            ui_text(UI_REGULAR, 14, X + 52, Y + H - 31, UI_MUTED, "Applies at the next start");
        }
    }
}

int menu_bar_height(int ph)
{
    int h = ph * 6 / 100;
    return h < 30 ? 30 : h;
}

void menu_draw_bar(SDL_Renderer *r, int pw, int ph)
{
    (void)r;
    float k = (float)ph / 44;
    ui_set_scale(k, 0, 0);
    float w = (float)pw / k;
    ui_fill(0, 0, w, 44, UI_PANEL);
    ui_fill(0, 43, w, 1, UI_LINE);
    float x = 16 + ui_text(UI_BOLD, 16, 16, 22, UI_ACCENT, "Operator");
    x += 20;
    float h = 26;
    x += ui_text(UI_REGULAR, 15, x, 22, UI_MUTED, "Test") + 8;
    x += ui_cap_input(x, 22, h, ui_pad_face_name(UI_IN_OK), 1) + 6;        /* A, X and B by label */
    x += ui_cap_input(x, 22, h, "F2", 0) + 22;
    x += ui_text(UI_REGULAR, 15, x, 22, UI_MUTED, "Service") + 8;
    x += ui_cap_input(x, 22, h, ui_pad_face_name(UI_IN_CLEAR), 1) + 6;
    x += ui_cap_input(x, 22, h, "F3", 0) + 22;
    x += ui_text(UI_REGULAR, 15, x, 22, UI_MUTED, "Done") + 8;
    ui_cap_input(x, 22, h, ui_pad_face_name(UI_IN_BACK), 1);
}
