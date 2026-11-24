/* The controls pages: see controls.h. */
#include "controls.h"

#include <string.h>

#include "ui.h"

/* the Gamepad page's first row is each player's controller; then the actions */
static int first_of(const ctl_page *p)
{
    return p->pad ? 1 : 0;
}

void ctl_open(ctl_page *p, int pad)
{
    p->pad = pad;
    p->row = p->top = 0;
    p->capturing = 0;
    if (p->col < 0 || p->col > 1)
        p->col = 0;
}

int ctl_count(const ctl_page *p)
{
    return ACT_COUNT + first_of(p);
}

void ctl_move(ctl_page *p, int d, int wrap)
{
    int n = ctl_count(p), r = p->row + d;
    if (wrap)
        r = ((r % n) + n) % n;
    else
        r = r < 0 ? 0 : r >= n ? n - 1 : r;
    p->row = r;
}

void ctl_side(ctl_page *p, int col)
{
    p->col = col ? 1 : 0;
}

/* the chosen player's binding of the chosen row's action, or NULL on the Controller row */
static binding *cell(settings *s, const ctl_page *p)
{
    int act = p->row - first_of(p);
    if (act < 0 || act >= ACT_COUNT)
        return NULL;
    return p->pad ? &s->pad[p->col][act] : &s->key[p->col][act];
}

/* the Controller row: the next gamepad connected now for player c, one the
 * other player has not been given; past the last, Auto again */
static void controller_next(settings *s, int c)
{
    int n = 0, at = -1;
    SDL_JoystickID *ids = ui_gamepads(&n);
    char w[UI_PAD_WISH_MAX];
    for (int i = 0; ids && i < n && at < 0; i++) {
        ui_pad_wish_of(ids[i], w, sizeof w);
        if (!SDL_strcmp(w, s->pad_wish[c]))
            at = i;
    }
    s->pad_wish[c][0] = 0;
    for (int i = at + 1; ids && i < n; i++) {
        ui_pad_wish_of(ids[i], w, sizeof w);
        if (SDL_strcmp(w, s->pad_wish[!c])) {
            SDL_strlcpy(s->pad_wish[c], w, sizeof s->pad_wish[c]);
            break;
        }
    }
    SDL_free(ids);
}

void ctl_set(settings *s, ctl_page *p)
{
    if (cell(s, p))
        p->capturing = 1;
    else
        controller_next(s, p->col);
}

void ctl_clear(settings *s, ctl_page *p)
{
    binding *b = cell(s, p);
    if (b)
        memset(b, 0, sizeof *b);
    else
        s->pad_wish[p->col][0] = 0;
}

void ctl_default(settings *s, ctl_page *p)
{
    binding *b = cell(s, p);
    if (!b) {
        s->pad_wish[p->col][0] = 0;
        return;
    }
    static settings d;
    settings_defaults(&d);
    int act = p->row - first_of(p);
    *b = p->pad ? d.pad[p->col][act] : d.key[p->col][act];
}

/* what a cell takes replaces what it had */
static int capture(settings *s, ctl_page *p, const char *name)
{
    binding *b = cell(s, p);
    p->capturing = 0;
    if (!b || !name || !name[0])
        return 1;
    memset(b, 0, sizeof *b);
    settings_binding_add(b, name);
    return 2;
}

/* while a cell waits: Delete empties it, Backspace gives it its default
 * back; the bar under the page offers both, so they must do it here too */
static int wait_key(settings *s, ctl_page *p, SDL_Keycode key)
{
    if (key == SDLK_ESCAPE) {
        p->capturing = 0;
        return 1;
    }
    if (key == SDLK_DELETE || key == SDLK_BACKSPACE) {
        p->capturing = 0;
        if (key == SDLK_DELETE)
            ctl_clear(s, p);
        else
            ctl_default(s, p);
        return 2;
    }
    return 0;
}

int ctl_waiting(const ctl_page *p)
{
    if (!p->capturing)
        return 0;
    return p->pad && ui_device() != UI_DEV_KEYS ? 2 : 1;
}

int ctl_event(settings *s, ctl_page *p, const SDL_Event *e)
{
    if (!p->capturing)
        return 0;
    if (p->pad) {
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            return capture(s, p, SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button));
        if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            if (e->gaxis.value > 16000 &&
                (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                return capture(s, p, SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e->gaxis.axis));
            return 1;               /* a stick on its way: not the menu's while the cell waits */
        }
        if (e->type == SDL_EVENT_KEY_DOWN) {
            int took = e->key.repeat ? 0 : wait_key(s, p, e->key.key);
            return took ? took : 1;         /* another key: nothing, the cell waits on */
        }
        return 0;
    }
    if (e->type == SDL_EVENT_KEY_DOWN) {
        if (e->key.repeat)
            return 1;
        int took = wait_key(s, p, e->key.key);
        return took ? took : capture(s, p, SDL_GetScancodeName(e->key.scancode));
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        /* a keyboard cell cannot take a pad's button: B leaves it as it
         * was, X empties it, Y gives its default back, as the bar says */
        int face = ui_pad_face(e->gbutton.which, e->gbutton.button);
        if (face == UI_IN_BACK)
            return wait_key(s, p, SDLK_ESCAPE);
        if (face == UI_IN_CLEAR)
            return wait_key(s, p, SDLK_DELETE);
        if (face == UI_IN_DEFAULT)
            return wait_key(s, p, SDLK_BACKSPACE);
        return 1;
    }
    return 0;
}

/* what the Controller row says for player c, whose gamepad now is id (0:
 * none): Auto and the pad that gives, or the controller chosen; when that
 * one is not connected, the pad the player has meanwhile. Returns 1 when
 * the chosen controller is missing */
static int controller_text(const settings *s, int c, SDL_JoystickID id, char *out, size_t n, int *faint)
{
    const char *w = s->pad_wish[c];
    char t[96];
    *faint = 1;
    if (id)
        ui_pad_title(id, t, sizeof t);
    if (!w[0]) {
        if (id)
            SDL_snprintf(out, n, "Auto: %s", t);
        else
            SDL_strlcpy(out, "Auto: none connected", n);
        return 0;
    }
    if (id && ui_pad_wish_fits(w, id)) {
        *faint = 0;
        SDL_strlcpy(out, t, n);
        return 0;
    }
    if (id)
        SDL_snprintf(out, n, "Meanwhile: %s", t);
    else
        SDL_snprintf(out, n, "%s (not connected)", ui_pad_wish_name(w));
    return 1;
}

void ctl_draw(const settings *s, ctl_page *p, float x, float y, float w, float h, float row_h)
{
    int first = first_of(p), n = ctl_count(p);
    static const char *const head[2] = { "Player 1", "Player 2" };
    const char *const wish[2] = { s->pad_wish[0], s->pad_wish[1] };
    SDL_JoystickID ids[2] = { 0, 0 };
    if (p->pad)
        ui_pad_players(wish, ids);
    ui_bind_row rows[ACT_COUNT + 1];
    static char vals[ACT_COUNT + 1][2][160];
    int missing[2] = { 0, 0 };
    SDL_zeroa(rows);
    if (p->pad) {
        rows[0].label = "Controller";
        rows[0].text = 1;
        for (int c = 0; c < 2; c++) {
            missing[c] = controller_text(s, c, ids[c], vals[0][c], sizeof vals[0][c], &rows[0].faint[c]);
            rows[0].value[c] = vals[0][c];
        }
    }
    for (int a = 0; a < ACT_COUNT; a++) {
        ui_bind_row *r = &rows[first + a];
        r->label = settings_action_text[a];
        for (int c = 0; c < 2; c++) {
            settings_binding_text(p->pad ? &s->pad[c][a] : &s->key[c][a], vals[first + a][c], sizeof vals[0][0]);
            r->value[c] = vals[first + a][c];
        }
        r->waiting = p->capturing && p->row == first + a ? p->col + 1 : 0;
    }
    if (p->row >= n)
        p->row = n - 1;
    float line = 30;
    ui_bind_table(x, y, w, h - line, row_h, head, ids, !p->pad, rows, n, p->row, p->col, &p->top);
    const char *note;
    static char gone[200];
    if (p->capturing)
        note = p->pad ? "Press the button for it. On the keyboard, Esc leaves it as it was, Del empties it and "
                        "Backspace gives its default back."
                      : "Press the key for it. Esc leaves it as it was, Del empties it and Backspace gives its "
                        "default back.";
    else if (p->pad && p->row < first && missing[p->col]) {
        SDL_snprintf(gone, sizeof gone, "Player %d's %s is not connected. It is this player's again when it is.",
                     p->col + 1, ui_pad_wish_name(s->pad_wish[p->col]));
        note = gone;
    } else if (p->pad && p->row < first)
        note = "Set: the next gamepad connected now. Clear: Auto, the gamepads in the order they connected (the first "
               "is player 1's).";
    else if (p->pad)
        note = "The left stick always moves. Each column names the buttons as that player's gamepad does.";
    else
        note = "Both players' keys, on the one keyboard. Player 2 has none by default.";
    ui_text_in(UI_REGULAR, 14.5f, x + 16, y + h - 12, w - 32, 0, 0, p->capturing ? UI_ACCENT : UI_MUTED, note);
}

int ctl_point(settings *s, ctl_page *p, float x, float y, int press)
{
    int col = -1, r = ui_bind_table_hit(x, y, &col);
    if (r < 0)
        return 0;
    p->row = r;
    if (col >= 0)
        p->col = col;               /* the cell under the mouse is the one Clear and Default act on */
    if (!press || col < 0)
        return 1;
    ctl_set(s, p);
    return 2;
}
