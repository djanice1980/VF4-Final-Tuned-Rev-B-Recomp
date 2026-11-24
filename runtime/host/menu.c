/* The game's own menu: see menu.h. */
#include "menu.h"

#include <string.h>

#include "card_editor.h"
#include "cards.h"
#include "options.h"
#include "controls.h"
#include "ui.h"
#include "updates.h"

enum { TAB_GAME, TAB_CARDS, TAB_PICTURE, TAB_SOUND, TAB_PAD, TAB_KEYS, TABS };
static const char *const tab_names[TABS] = { "Game", "Cards", "Picture", "Sound", "Gamepad", "Keyboard" };

/* a row: an option (options.h), one of these actions, or a control */
enum { A_RESUME = OPT_COUNT, A_TEST, A_RESTART, A_UPDATES, A_KOFI, A_LAUNCHER, A_QUIT, A_EDIT1, A_EDIT2, A_CONTROL,
       A_CODE };

#define ROWS 13
#define ROWS_X 70.0f        /* the rows' place on the panel (menu_draw), for the mouse */
#define ROWS_W 640.0f
#define ROW_H 39.0f
static const int tab_rows[TABS][ROWS] = {
    [TAB_GAME] = { A_RESUME, A_CODE, OPT_CREDITS, OPT_NETWORK, OPT_REGION, OPT_LATENCY, A_TEST, A_RESTART, A_UPDATES,
                   A_KOFI, A_LAUNCHER, A_QUIT, -1 },
    [TAB_CARDS] = { OPT_CARD1, A_EDIT1, OPT_CARD2, A_EDIT2, OPT_CARDS, -1 },
    [TAB_PICTURE] = { OPT_FULLSCREEN, OPT_DISPLAY, OPT_WINDOW, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_GPU,
                      OPT_SCALE, OPT_LIGHTING, OPT_SKIP, OPT_PACING, -1 },
    [TAB_SOUND] = { OPT_VOLUME, OPT_SOUND, -1 },
};
static int shown_rows[TABS][ROWS], nshown[TABS];   /* tab_rows without Updates and the launcher outside a release folder */

static settings *s;
static menu_run run;
static settings before;     /* as it was when opened */
static int tab, row, top, armed = -1; /* armed: Restart or Quit, chosen once (the second choice does it) */
static int notice, notice_focus;    /* MENU_NOTICE_* (0 none); the Ko-fi notice's buttons: 0 Support on Ko-fi, 1 Close */
static int kofi_said = -1;          /* the Ko-fi page was opened (1) or not (0) since the menu opened */
/* for the mouse: the last drawing's target (0 until it is drawn after
 * opening: the hit tests are its), the notice's buttons and Ko-fi's badge
 * (logical units; w 0 when not drawn) */
static int drawn_w, drawn_h;
static SDL_FRect notice_btn[2], badge_at;

/* the card editor (docs/design.md section 42): in the tabs' place while it
 * is up, on the card in ed_name; its ring name note (typing) over it */
static int ed_on, ed_held;          /* ed_held: a game is using the card (its Save waits for the game) */
static card_editor ed;
static char ed_name[CARD_NAME_MAX];
static SDL_FRect ed_btn[2];         /* Save, Cancel (for the mouse) */
static int typing, ty_fresh, ty_focus;      /* ty_focus: 0 OK, 1 Cancel */
static unsigned edits;              /* Saves so far: the Cards tab reads its card again after one */
static char ty_text[CARD_NAME_LEN + 1];
static SDL_FRect ty_btn[2];         /* OK, Cancel */
#define HELD_NOTE "A game is using this card. Save keeps the changes, and they go onto the card when the game lets " \
                  "go of it; the game's own (the record, games left) stay."

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
    ed_on = typing = 0;
    for (int t = 0; t < TABS; t++) {
        nshown[t] = 0;
        for (int i = 0; i < ROWS && tab_rows[t][i] >= 0; i++) {
            int id = tab_rows[t][i];
            /* a two-PC game runs on both PCs alike: the test menu, a restart and the cards in the
             * units would change one of them only (the Cards tab keeps its Card units row) */
            if (run.session && (id == A_TEST || id == A_RESTART || id == OPT_CARD1 || id == OPT_CARD2 ||
                                id == A_EDIT1 || id == A_EDIT2))
                continue;
            if (id == A_CODE && !run.join_code)     /* the host of a two-PC game only */
                continue;
            if (id == OPT_LATENCY && !run.session)  /* a two-PC game only (the launcher's Settings always) */
                continue;
            if ((id != A_UPDATES || gu_state() != GU_OFF) && (id != A_LAUNCHER || gu_launcher()))
                shown_rows[t][nshown[t]++] = id;
        }
    }
}

void menu_open_notice(settings *cfg, const menu_run *r, int kind)
{
    menu_open(cfg, r);
    notice = kind;
    notice_focus = 1;       /* Close (Ko-fi's): a button pressed as the notice comes up opens no browser */
}

void menu_kofi(int opened)
{
    kofi_said = opened != 0;
}

int menu_changed(void)
{
    return s && memcmp(s, &before, sizeof before) != 0;
}

/* the Gamepad and Keyboard tabs: the controls pages (runtime/ui/controls.h),
 * as the launcher's Settings has them (docs/design.md section 72) */
static ctl_page ctl;
static int controls_tab(void)
{
    return tab == TAB_PAD || tab == TAB_KEYS;
}

static int rows_in(int t)
{
    return t == TAB_PAD || t == TAB_KEYS ? ctl_count(&ctl) : nshown[t];
}

static int row_id(int r)
{
    return controls_tab() ? A_CONTROL : shown_rows[tab][r];
}

/* to tab t: its first row, or the controls page it is */
static void tab_to(int t)
{
    tab = t;
    row = top = 0;
    if (controls_tab())
        ctl_open(&ctl, tab == TAB_PAD);
}

int menu_capturing(void)
{
    return s && !notice && !ed_on && !typing && controls_tab() && ctl.capturing;
}

int menu_capture(const SDL_Event *e)
{
    return menu_capturing() ? ctl_event(s, &ctl, e) : 0;
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
    if ((id == OPT_SCALE || id == OPT_GPU) && s->renderer != RENDERER_GPU)
        return MENU_STAY;
    option_change(s, id, d);
    return id == OPT_PICTURE || id == OPT_FILTER || id == OPT_FULLSCREEN || id == OPT_WINDOW || id == OPT_VOLUME ||
                   id == OPT_DISPLAY || id == OPT_RENDERER || id == OPT_GPU || id == OPT_SCALE || id == OPT_PACING ||
                   id == OPT_LIGHTING || id == OPT_SKIP
               ? MENU_LIVE
               : MENU_STAY;
}

/* ---- the card editor ---- */

/* player side's card as the menu shows it (a Saved edit in): 1 a card (in
 * card and v), 0 not made yet, -1 none or not a card; *held */
static int card_state(int side, Uint8 *card, card_view *v, int *held)
{
    SDL_zerop(v);
    *held = 0;
    if (!s->card[side][0] || !run.card_peek)
        return -1;
    int file = run.card_peek(s->card[side], card, held);
    if (file > 0)
        card_view_of(card, v);
    return file;
}

/* why player side's card cannot be edited now, or NULL; *held */
static const char *edit_why(int side, int *held)
{
    Uint8 c[CARD_SIZE];
    card_view v;
    if (!s->card[side][0]) {
        *held = 0;
        return side ? "Player 2 has no card: choose one above first." : "Player 1 has no card: choose one above first.";
    }
    int file = card_state(side, c, &v, held);
    if (ced_editable(&v, file))
        return NULL;
    if (file < 0)
        return "Its file is not a card (it is not 128 bytes), so it is left alone.";
    if (v.format != 4)
        return "A card from an older version of the game: the game brings it up to date at its next game, and it "
               "can be edited then.";
    if (!v.header_ok || !v.body_ok)
        return "Its checks do not hold, so the game would not read it.";
    return "It is Dural's, whom no player can be.";
}

static void editor_open(int side)
{
    Uint8 card[CARD_SIZE];
    card_view v;
    int held, file = card_state(side, card, &v, &held);
    if (!s->card[side][0] || !ced_editable(&v, file))
        return;
    int fresh = file == 0 || v.blank;
    if (fresh)      /* as Save will make it (its number is the machine thread's to give) */
        card_starter(card, file > 0 ? (Uint32)card[4] << 24 | (Uint32)card[5] << 16 | (Uint32)card[6] << 8 | card[7]
                                    : 0x20050000u);
    ced_open(&ed, card, fresh);
    SDL_strlcpy(ed_name, s->card[side], sizeof ed_name);
    ed_held = held;
    ed_on = 1;
    typing = 0;
}

/* Save (or Cancel): back to the Cards tab; a Saved change goes to main.c,
 * which puts it onto the card as the menu closes */
static void editor_close(int save)
{
    if (save && (ed.fresh || ced_changed(&ed)) && run.card_edit) {
        run.card_edit(ed_name, &ed.f0, &ed.f, ed.fresh);
        edits++;
    }
    ed_on = typing = 0;
}

static void typing_open(void)
{
    ced_name_text(&ed, ty_text, sizeof ty_text);
    ty_fresh = 1;           /* chosen: the first letter typed replaces it */
    ty_focus = 0;
    typing = 1;
}

int menu_typing(void)
{
    return s && ed_on && typing;
}

void menu_text(const char *t)
{
    if (!menu_typing())
        return;
    if (ty_fresh)
        ty_text[0] = 0;
    ty_fresh = 0;
    size_t len = strlen(ty_text);
    for (; *t; t++)
        if (ced_name_char((unsigned char)*t, len)) {
            ty_text[len++] = *t;
            ty_text[len] = 0;
        }
}

/* the note's OK (focus 0) or Cancel */
static void typing_done(int ok)
{
    if (ok)
        ced_name_set(&ed, ty_text);
    typing = 0;
}

static int typing_command(int cmd)
{
    switch (cmd) {
    case MENU_LEFT:
    case MENU_RIGHT: ty_focus ^= 1; break;
    case MENU_OK: typing_done(ty_focus == 0); break;
    case MENU_BACK:
    case MENU_ESCAPE: typing_done(0); break;
    case MENU_ERASE:
    case MENU_CLEAR: {
        size_t len = strlen(ty_text);
        ty_text[ty_fresh || !len ? 0 : len - 1] = 0;
        ty_fresh = 0;
        break;
    }
    }
    return MENU_STAY;
}

static int editor_command(int cmd)
{
    switch (cmd) {
    case MENU_UP: ced_move(&ed, -1); break;
    case MENU_DOWN: ced_move(&ed, 1); break;
    case MENU_PREV: ced_tab_to(&ed, ed.tab - 1); break;
    case MENU_NEXT: ced_tab_to(&ed, ed.tab + 1); break;
    case MENU_LEFT: ced_change(&ed, -1); break;
    case MENU_RIGHT: ced_change(&ed, 1); break;
    case MENU_OK:
        if (ced_on_name(&ed))
            typing_open();
        else
            ced_change(&ed, 1);
        break;
    case MENU_ERASE:
    case MENU_UNDO: ced_undo(&ed); break;
    case MENU_BACK:
    case MENU_ESCAPE: editor_close(1); break;
    }
    return MENU_STAY;
}

int menu_command(int cmd)
{
    if (notice == MENU_NOTICE_REFRESH) {    /* its one button, OK */
        switch (cmd) {
        case MENU_OK:
        case MENU_BACK:
        case MENU_ESCAPE:
        case MENU_ERASE: return MENU_RESUME;
        default: return MENU_STAY;
        }
    }
    if (notice) {
        switch (cmd) {
        case MENU_UP:
        case MENU_DOWN:
        case MENU_LEFT:
        case MENU_RIGHT: notice_focus ^= 1; return MENU_STAY;
        case MENU_OK: return notice_focus == 0 ? MENU_KOFI_CLOSE : MENU_RESUME;
        case MENU_BACK:
        case MENU_ESCAPE:
        case MENU_ERASE: return MENU_RESUME;
        default: return MENU_STAY;
        }
    }
    if (typing)
        return typing_command(cmd);
    if (ed_on)
        return editor_command(cmd);
    if (controls_tab()) {
        /* the controls page: Left and Right are the two players' cells; a
         * cell is set, emptied, or given its default back */
        armed = -1;
        switch (cmd) {
        case MENU_UP: ctl_move(&ctl, -1, 1); return MENU_STAY;
        case MENU_DOWN: ctl_move(&ctl, 1, 1); return MENU_STAY;
        case MENU_LEFT: ctl_side(&ctl, 0); return MENU_STAY;
        case MENU_RIGHT: ctl_side(&ctl, 1); return MENU_STAY;
        case MENU_OK: ctl_set(s, &ctl); return MENU_LIVE;
        case MENU_CLEAR: ctl_clear(s, &ctl); return MENU_LIVE;
        case MENU_UNDO:
        case MENU_ERASE: ctl_default(s, &ctl); return MENU_LIVE;
        case MENU_PREV:
        case MENU_NEXT: tab_to(wrap(tab, cmd == MENU_PREV ? -1 : 1, TABS)); return MENU_STAY;
        default: return MENU_RESUME;        /* Back, Escape */
        }
    }
    if (cmd == MENU_UNDO || cmd == MENU_CLEAR)
        return MENU_STAY;
    if (cmd == MENU_ESCAPE || cmd == MENU_ERASE)
        cmd = MENU_BACK;
    int was_armed = armed;
    armed = -1;
    switch (cmd) {
    case MENU_UP: row = wrap(row, -1, rows_in(tab)); return MENU_STAY;
    case MENU_DOWN: row = wrap(row, 1, rows_in(tab)); return MENU_STAY;
    case MENU_PREV:
    case MENU_NEXT:
        tab_to(wrap(tab, cmd == MENU_PREV ? -1 : 1, TABS));
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
        case A_CODE:
            if (run.join_code && run.join_code(NULL, 0) != MENU_CODE_NONE)
                run.join_code_new();
            return MENU_STAY;
        case A_EDIT1:
        case A_EDIT2: editor_open(row_id(row) - A_EDIT1); return MENU_STAY;
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
    if (notice == MENU_NOTICE_REFRESH) {
        int b = inside(&notice_btn[0], x, y);
        switch (what) {
        case MENU_MOUSE_LEFT: return b ? MENU_RESUME : -1;
        case MENU_MOUSE_RIGHT: return MENU_RESUME;
        default: return -1;
        }
    }
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
    if (typing) {           /* the ring name's note: its buttons only */
        int b = inside(&ty_btn[0], x, y) ? 0 : inside(&ty_btn[1], x, y) ? 1 : -1;
        switch (what) {
        case MENU_MOUSE_MOVE:
            if (b < 0 || b == ty_focus)
                return -1;
            ty_focus = b;
            return MENU_STAY;
        case MENU_MOUSE_LEFT:
            if (b < 0)
                return -1;
            typing_done(b == 0);
            return MENU_STAY;
        case MENU_MOUSE_RIGHT: typing_done(0); return MENU_STAY;
        default: return -1;
        }
    }
    int r = controls_tab() ? -1 : ui_rows_hit(x, y), t = ui_tabs_hit(x, y);
    if (!ed_on && controls_tab() && t < 0 && t != UI_TAB_PREV && t != UI_TAB_NEXT) {
        /* the controls page: the row under the mouse, a click sets its cell,
         * a right click closes as elsewhere, the wheel moves the choice */
        int was_row = ctl.row, was_col = ctl.col;
        switch (what) {
        case MENU_MOUSE_MOVE:
            if (ctl.capturing)
                return -1;
            return ctl_point(s, &ctl, x, y, 0) && (ctl.row != was_row || ctl.col != was_col) ? MENU_STAY : -1;
        case MENU_MOUSE_WHEEL:
            ctl_move(&ctl, d, 0);
            return ctl.row != was_row ? MENU_STAY : -1;
        case MENU_MOUSE_LEFT:
            if (inside(&badge_at, x, y))
                return -1;
            return ctl_point(s, &ctl, x, y, 1) == 2 ? MENU_LIVE : MENU_STAY;
        case MENU_MOUSE_RIGHT:
            return menu_command(MENU_BACK);
        default:
            return -1;
        }
    }
    if (ed_on) {
        switch (what) {
        case MENU_MOUSE_MOVE:
            if (r < 0 || r == ed.row)
                return -1;
            ed.row = r;
            return MENU_STAY;
        case MENU_MOUSE_WHEEL: {
            int to = SDL_clamp(ed.row + d, 0, ced_rows(&ed) - 1);
            if (to == ed.row)
                return -1;
            ed.row = to;
            return MENU_STAY;
        }
        case MENU_MOUSE_LEFT:
            if (t == UI_TAB_PREV || t == UI_TAB_NEXT || t >= 0) {
                if (t == ed.tab)
                    return -1;
                ced_tab_to(&ed, t >= 0 ? t : ed.tab + (t == UI_TAB_PREV ? -1 : 1));
                return MENU_STAY;
            }
            if (inside(&ed_btn[0], x, y) || inside(&ed_btn[1], x, y)) {
                editor_close(inside(&ed_btn[0], x, y));
                return MENU_STAY;
            }
            if (r < 0)
                return -1;
            {
                /* a choice's left half goes back, its right half on (ui_rows:
                 * the value's box ends 0.35 rows in from the right) */
                float right = ROWS_X + ROWS_W - ROW_H * 0.35f, bw = SDL_min(ROWS_W * 0.42f, ROW_H * 5.6f);
                int was = ed.row;
                ed.row = r;
                if (ced_on_name(&ed))
                    typing_open();
                else if (x >= right - bw && x <= right + 4)
                    ced_change(&ed, x < right - bw / 2 ? -1 : 1);
                else if (r == was)
                    ced_change(&ed, 1);
            }
            return MENU_STAY;
        case MENU_MOUSE_RIGHT:
            if (r < 0) {
                editor_close(1);
                return MENU_STAY;
            }
            ed.row = r;
            ced_change(&ed, -1);
            return MENU_STAY;
        default:
            return -1;
        }
    }
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
            tab_to(t);
            armed = -1;
            return MENU_STAY;
        }
        if (inside(&badge_at, x, y))
            return MENU_KOFI;
        if (r < 0)
            return -1;
        choose(r);
        if (row_id(r) < OPT_COUNT) {
            /* a choice's left half goes back, its right half on (ui_rows: the value's box ends 0.35
             * rows in from the right), as in the card editor: Window size does not wrap */
            float right = ROWS_X + ROWS_W - ROW_H * 0.35f, bw = SDL_min(ROWS_W * 0.42f, ROW_H * 5.6f);
            if (x >= right - bw && x < right - bw / 2)
                return menu_command(MENU_LEFT);
        }
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

/* what the screen's refresh rate does to the game's motion, for the refresh
 * notice and the Fullscreen row (docs/design.md section 44); NULL when it
 * is even (or not known, off a Steam Deck OLED) */
static const char *refresh_text(void)
{
    static char t[520];
    if (run.deck == STEAM_DECK_OLED)
        SDL_snprintf(t, sizeof t,
                     "The game shows 60 pictures a second, and a Steam Deck OLED's screen can refresh 90 times a "
                     "second: then the motion is uneven. Press the Quick Access button (the one with three dots), "
                     "open Performance, turn on Use per-game profile, and set Refresh rate to 60. At 60 already, "
                     "nothing needs to change.");
    else if (settings_refresh_uneven(run.refresh) && run.refresh_own)
        SDL_snprintf(t, sizeof t,
                     "The game shows 60 pictures a second, and this screen seems to refresh about %.0f times a "
                     "second, which is not a multiple of 60: the motion may look uneven. If it does, set the "
                     "screen to a multiple of 60 Hz (60, 120, 180, 240) in the system's display settings. "
                     "FreeSync or G-Sync, where the screen has it, can smooth it in fullscreen too.",
                     (double)run.refresh);
    else if (settings_refresh_uneven(run.refresh))
        SDL_snprintf(t, sizeof t,
                     "The game shows 60 pictures a second, and this screen refreshes %.0f times a second: the "
                     "motion is uneven. Set the screen to a multiple of 60 Hz (60, 120, 180, 240) in the "
                     "system's display settings. FreeSync or G-Sync, where the screen has it, can smooth it in "
                     "fullscreen too.",
                     (double)run.refresh);
    else
        return NULL;
    return t;
}

/* the refresh rate's notice (menu_open_notice: MENU_NOTICE_REFRESH), with OK */
static void draw_refresh_notice(void)
{
    const char *t = refresh_text();
    if (!t)
        t = "";
    const float W = 620, P = 34;
    /* the text's lines from 84 down, the faint line, then the button and the hints (128 at the foot) */
    const float H = 84 + 23 * (float)ui_wrap_lines(UI_REGULAR, 15.5f, W - 2 * P, t) + 58 + 128;
    const float X = (1180 - W) / 2, Y = (680 - H) / 2;
    ui_round_line(X, Y, W, H, 20, 1, UI_LINE, UI_PANEL);
    ui_text(UI_BOLD, 21, X + P, Y + 44, UI_TEXT,
            run.deck == STEAM_DECK_OLED ? "Set the screen to 60 Hz"
            : run.refresh_own           ? "The screen may not be at a multiple of 60 Hz"
                                        : "Set the screen to a multiple of 60 Hz");
    float y = Y + 84;
    y += 23 * (float)ui_wrap(UI_REGULAR, 15.5f, X + P, y, W - 2 * P, 23, UI_MUTED, t);
    ui_text_in(UI_REGULAR, 13.5f, X, y + 16, W, 1, 0, UI_FAINT,
               "The Picture tab's Fullscreen row says it too. This notice won't show again.");
    const char *l0 = "OK";
    float w0 = ui_text_w(UI_MEDIUM, 16, l0) + 52 + 40;
    float bx = X + (W - w0) / 2, by = Y + H - 62 - 66;
    notice_btn[0] = (SDL_FRect){ bx, by, w0, 46 };
    notice_btn[1] = (SDL_FRect){ 0, 0, 0, 0 };
    ui_round(bx, by, w0, 46, 12, UI_ACCENT);
    ui_text_in(UI_MEDIUM, 16, bx, by + 23, w0, 1, 0, UI_ON_ACCENT, l0);
    ui_fill(X + 30, Y + H - 62, W - 60, 1, UI_LINE);
    static const ui_hint hints[] = {
        { "Close", { UI_IN_OK, UI_IN_NONE } },
    };
    ui_hints(X + W - 32, Y + H - 31, 26, hints, (int)SDL_arraysize(hints), 2);
}

/* the first start's notice (menu_open_notice), like the author's pcbview's */
static void draw_notice(void)
{
    if (notice == MENU_NOTICE_REFRESH) {
        draw_refresh_notice();
        return;
    }
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
    case OPT_REGION: return s->region != run.start.region;
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
    static unsigned seen;
    Uint64 now = SDL_GetTicks();
    if (strcmp(name, s->card[side]) || now - at >= 500 || !body[0] || edits != seen) {
        Uint8 c[CARD_SIZE];
        card_view v;
        int held;
        SDL_strlcpy(name, s->card[side], sizeof name);
        at = now;
        seen = edits;
        int file = card_state(side, c, &v, &held);     /* with a Saved edit in */
        card_view_text(&v, file, body, sizeof body);
    }
    *title = name;
    *text = body;
    *note = !s->cards ? "Card units are off (below): the game uses none."
                      : "Left and Right choose the card (None: no card, every character free); the other player's "
                        "card swaps the two. Cards change as the menu closes.";
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
        if (id == OPT_CREDITS || id == OPT_NETWORK || id == OPT_REGION)
            *note = "Applies when the game restarts (Restart the game).";
        else if (id == OPT_VOLUME && run.no_device)
            *note = "No sound: no audio device could be opened when the game started. Check the PC's sound "
                    "output, then quit and start the game again.";
        else if (id == OPT_VOLUME && !run.sound)
            *note = "Sound is off: turn it on below, for the next start.";
        else if ((id == OPT_SCALE || id == OPT_GPU) && s->renderer != RENDERER_GPU)
            *note = "The Software renderer does not use it.";
        else if ((id == OPT_RENDERER || id == OPT_GPU || id == OPT_SCALE) && s->renderer == RENDERER_GPU &&
                 !(run.gpu_now && run.gpu_now[0]))
            *note = "The GPU renderer could not start on any GPU: the game is drawn with Software for now. The "
                    "game's log (vf4ft.log) says why.";
        else if (id == OPT_GPU) {
            /* the whole names: the row cuts a long one short (section 53: as it is now) */
            static char gn[2 * GPU_NAME_MAX + 100];
            if (s->gpu[0] && SDL_strcasecmp(s->gpu, run.gpu_now))
                SDL_snprintf(gn, sizeof gn, "%s could not be used: drawing on %s.", s->gpu, run.gpu_now);
            else if (s->gpu[0])
                SDL_snprintf(gn, sizeof gn, "Drawing on %s now.", run.gpu_now);
            else
                SDL_snprintf(gn, sizeof gn, "Auto: drawing on %s.", run.gpu_now);
            *note = gn;
        } else if (id == OPT_DISPLAY) {
            static char dn[DISPLAY_NAME_MAX + 32];
            *note = option_note(s, id, dn, sizeof dn);
        } else if (id == OPT_LATENCY && run.latency_now) {
            static char ln[240];
            run.latency_now(ln, sizeof ln);
            if (ln[0])
                *note = ln;
        } else if (id == OPT_PACING && run.pacing_now) {
            static char pn[240];
            run.pacing_now(pn, sizeof pn);
            if (pn[0])
                *note = pn;
        }
        else if (id == OPT_FULLSCREEN && refresh_text())
            *note = refresh_text();
        else if (options[id].at_start)
            *note = later(id) ? "Changed: applies the next time the game starts." : "Applies the next time the game starts.";
        return;
    }
    switch (id) {
    case A_RESUME: *title = "Resume"; *text = "Back to the game, where it paused."; break;
    case A_EDIT1:
    case A_EDIT2: {
        int held;
        *title = id == A_EDIT1 ? "Edit player 1's card" : "Edit player 2's card";
        *text = "The card editor, as in the launcher: the ring name, character, rank, games left and items, and "
                "each costume's colors and worn items. A card holds its player to its character at the select "
                "screen. Saved changes go onto the card as the menu closes.";
        *note = edit_why(id - A_EDIT1, &held);
        if (!*note && held)
            *note = HELD_NOTE;
        break;
    }
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
    case A_CODE: {
        int st = run.join_code ? run.join_code(NULL, 0) : MENU_CODE_NONE;
        *title = "Join code";
        *text = "Another PC joins this game with this PC's address and this code (on the launcher's Join "
                "page). A code works once, and a new one is made each time a player leaves. Choose this for a "
                "new code: the one before no longer works.";
        if (st == MENU_CODE_NONE)
            *note = "No other PC can join now: a player is here, or joining.";
        else if (st == MENU_CODE_GONE)
            *note = "Three wrong codes were typed, so the code no longer works. Choose this for a new one.";
        break;
    }
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
        *text = "";
        break;
    }
}

/* a button at the panel's foot, its middle at y, ending at x; returns its box */
static SDL_FRect foot_button(float x, float y, const char *label, int accent)
{
    float w = ui_text_w(UI_MEDIUM, 15, label) + 40, h = 38;
    ui_round(x - w, y - h / 2, w, h, 10, accent ? UI_ACCENT : UI_RAISED);
    ui_text_in(UI_MEDIUM, 15, x - w, y, w, 1, 0, accent ? UI_ON_ACCENT : UI_TEXT, label);
    return (SDL_FRect){ x - w, y - h / 2, w, h };
}

/* the card editor, in the tabs' place: its tabs (Card, Costume 1, Costume
 * 2), its rows and the chosen row's description; Save and Cancel at the foot */
static void draw_editor(float X, float Y, float W, float H)
{
    ui_tabs(X + 30, Y + 70, W - 60, 46, ced_tab_names, CED_TABS, ed.tab);
    ui_fill(X + 30, Y + 132, W - 60, 1, UI_LINE);
    ui_row rows[CED_COSTUME_ROWS];
    static char vals[CED_COSTUME_ROWS][64];
    const char *abouts[CED_COSTUME_ROWS];
    int n = ced_rows(&ed);
    for (int i = 0; i < n; i++)
        ced_row(&ed, i, &rows[i], vals[i], sizeof vals[i], &abouts[i]);
    float ry = Y + 150, rh = H - 150 - 76;
    ui_rows(ROWS_X, ry, ROWS_W, rh, ROW_H, rows, n, ed.row, &ed.top);
    ui_about(X + 694, ry, W - 694 - 30, rh, rows[ed.row].label, abouts[ed.row], ed_held ? HELD_NOTE : ced_note(&ed));

    ui_fill(X + 30, Y + H - 62, W - 60, 1, UI_LINE);
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Change", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } }, { "Undo", { UI_IN_DEFAULT, UI_IN_NONE } },
    };
    ui_hints(X + 34, Y + H - 31, 26, hints, (int)SDL_arraysize(hints), 0);
    static const ui_hint save_hint[] = { { "Save", { UI_IN_BACK, UI_IN_NONE } } };
    ed_btn[0] = foot_button(X + W - 34, Y + H - 31, "Save", 1);
    ed_btn[1] = foot_button(ed_btn[0].x - 12, Y + H - 31, "Cancel", 0);
    ui_hints(ed_btn[1].x - 20, Y + H - 31, 26, save_hint, 1, 2);
}

/* the ring name's note over the editor: the name typed so far (chosen at
 * first: a letter typed replaces it), OK and Cancel */
static void draw_typing(void)
{
    ui_fill(-2000, -2000, 5180, 4680, (SDL_Color){ 6, 7, 10, 200 });    /* the whole window, past the panel's shape */
    const float W = 620, P = 36;
    const char *text = "The name the game shows for this player: up to 20 letters (A to Z, without accents), "
                       "numbers, spaces and marks.";
    int lines = ui_wrap_lines(UI_REGULAR, 15.5f, W - 2 * P, text);
    float H = 88 + 23 * (float)lines + 24 + 48 + 40 + 46 + 34, X = (1180 - W) / 2, Y = (680 - H) / 2;
    ui_round_line(X, Y, W, H, 16, 1, UI_LINE, UI_CARD);
    ui_text(UI_BOLD, 21, X + P, Y + 46, UI_TEXT, "Ring name");
    float y = Y + 84;
    y += 23 * (float)ui_wrap(UI_REGULAR, 15.5f, X + P, y, W - 2 * P, 23, UI_MUTED, text) + 12;
    ui_round_line(X + P, y, W - 2 * P, 48, 10, 2, UI_ACCENT, UI_BG);
    if (ty_fresh && ty_text[0])
        ui_round(X + P + 12, y + 10, ui_text_w(UI_MEDIUM, 18, ty_text) + 8, 28, 5, UI_HOVER);
    float tw = ui_text(UI_MEDIUM, 18, X + P + 16, y + 24, UI_TEXT, ty_text);
    ui_fill(X + P + 16 + tw + 2, y + 12, 2, 24, UI_ACCENT);       /* the caret (it does not blink: the menu is drawn
                                                                     * again only when something changes) */
    y += 48 + 24;
    ui_text_in(UI_REGULAR, 14, X + P, y, W - 2 * P, 0, 0, UI_FAINT, "On a Steam Deck, Steam + X shows the keyboard.");
    float by = Y + H - 34 - 23;
    ty_btn[1] = foot_button(X + W - P, by, "Cancel", ty_focus == 1);
    ty_btn[0] = foot_button(ty_btn[1].x - 12, by, "OK", ty_focus == 0);
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
    char heading[64];
    SDL_snprintf(heading, sizeof heading, "Card: %s", ed_name);
    ui_text(UI_BOLD, 21, X + 34, Y + 40, UI_TEXT, ed_on ? heading : "Virtua Fighter 4 Final Tuned");
    float cw = ui_text_w(UI_MEDIUM, 13.5f, "PAUSED") + 26;
    ui_round(X + W - 34 - cw, Y + 27, cw, 26, 13, UI_RAISED);
    ui_text(UI_MEDIUM, 13.5f, X + W - 34 - cw + 13, Y + 40, UI_MUTED, "PAUSED");
    if (ed_on) {
        draw_editor(X, Y, W, H);
        if (typing)
            draw_typing();
        return;
    }
    ui_tabs(X + 30, Y + 70, W - 60, 46, tab_names, TABS, tab);
    ui_fill(X + 30, Y + 132, W - 60, 1, UI_LINE);

    float ry = Y + 150, rh = H - 150 - 76;
    int any_later = 0;      /* a row is marked: it applies at the next start */
    if (controls_tab()) {
        ctl_draw(s, &ctl, X + 30, ry, W - 60, rh, ROW_H);
    } else {
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
                if (id == OPT_CREDITS || id == OPT_NETWORK || id == OPT_REGION)
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
            case A_CODE: {
                static char code[16];
                int st = run.join_code ? run.join_code(code, sizeof code) : MENU_CODE_NONE;
                w->label = "Join code";
                w->kind = UI_ROW_TEXT;
                w->value = st == MENU_CODE_SHOWN ? code : st == MENU_CODE_GONE ? "None: choose for a new one" : "None now";
                w->strong = st == MENU_CODE_SHOWN;
                break;
            }
            case A_EDIT1:
            case A_EDIT2: {
                int held;
                w->label = id == A_EDIT1 ? "Edit player 1's card" : "Edit player 2's card";
                w->off = edit_why(id - A_EDIT1, &held) != NULL;
                break;
            }
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
            default: break;
            }
            w->warn = armed >= 0 && armed == id;
        }
        /* 39 units a row: a release's Game tab (ten rows, with Network, Region,
         * Check for updates and Open the launcher) fits in rh = 394 without
         * scrolling; at 46 Quit was below the fold (nine rows then), at 43 the
         * tenth was. An eleventh row there needs this looked at again (Join
         * code and Latency optimization show only in a two-PC game, which has no
         * Test or Restart: ten rows). The Controls tab scrolls. */
        ui_rows(ROWS_X, ry, ROWS_W, rh, ROW_H, rows, n, row, &top);

        const char *title, *text, *note;
        about_of(row_id(row), &title, &text, &note);
        ui_about(X + 694, ry, W - 694 - 30, rh, title, text, note);
        if (row_id(row) == A_KOFI) {         /* the badge at the foot of the panel */
            float bw = 220, aw = W - 694 - 30, bh = bw * 356 / 672;
            badge_at = (SDL_FRect){ X + 694 + (aw - bw) / 2, ry + rh - 24 - bh, bw, bh };
            kofi_badge(badge_at.x, badge_at.y, bw);
        }

        for (int i = 0; i < n; i++)
            any_later |= rows[i].later;
    }

    ui_fill(X + 30, Y + H - 62, W - 60, 1, UI_LINE);
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Change", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } }, { "Select", { UI_IN_OK, UI_IN_NONE } },
        { "Close", { UI_IN_BACK, UI_IN_NONE } },
    };
    static const ui_hint ctl_hints[] = {
        { "Set", { UI_IN_OK, UI_IN_NONE } }, { "Clear", { UI_IN_CLEAR, UI_IN_NONE } },
        { "Default", { UI_IN_DEFAULT, UI_IN_NONE } }, { "Player", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } }, { "Close", { UI_IN_BACK, UI_IN_NONE } },
    };
    static const ui_hint wait_hints[] = {
        { "Leave it", { UI_IN_BACK, UI_IN_NONE } }, { "Clear", { UI_IN_CLEAR, UI_IN_NONE } },
        { "Default", { UI_IN_DEFAULT, UI_IN_NONE } },
    };
    if (controls_tab() && ctl_waiting(&ctl)) {
        if (ctl_waiting(&ctl) == 1)     /* a cell waits: these still do what they say (controls.h) */
            ui_hints(X + W - 32, Y + H - 31, 26, wait_hints, (int)SDL_arraysize(wait_hints), 2);
    } else if (controls_tab())
        ui_hints(X + W - 32, Y + H - 31, 26, ctl_hints, (int)SDL_arraysize(ctl_hints), 2);
    else
        ui_hints(X + W - 32, Y + H - 31, 26, hints, (int)SDL_arraysize(hints), 2);
    if (any_later) {
        ui_icon(UI_I_DOT, X + 40, Y + H - 31, 14, UI_ACCENT);
        ui_text(UI_REGULAR, 14, X + 52, Y + H - 31, UI_MUTED, "Applies at the next start");
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

void menu_draw_note(SDL_Renderer *r, int pw, int ph, const char *text)
{
    (void)r;
    float k = (float)ph / 44;
    ui_set_scale(k, 0, 0);
    float w = (float)pw / k;
    ui_fill(0, 0, w, 44, UI_PANEL);
    ui_fill(0, 43, w, 1, UI_LINE);
    float x = 16 + ui_text(UI_BOLD, 16, 16, 22, UI_ACCENT, "Two PCs") + 20;
    /* smaller when it would not fit */
    float size = 15;
    while (size > 9 && x + ui_text_w(UI_REGULAR, size, text) > w - 16)
        size -= 1;
    ui_text(UI_REGULAR, size, x, 22, UI_TEXT, text);
}
