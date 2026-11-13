/* The drawing kit the launcher and the game's menu share (docs/design.md
 * section 22): text in Noto Sans (runtime/ui/fonts.c, baked by
 * stb_truetype into glyph sheets for each size it is drawn at), rounded
 * boxes and small icons with smooth edges, key caps, and the widgets the
 * two are made of: tabs, setting rows, a description panel, button hints.
 *
 * It draws with one SDL_Renderer (the launcher's window, or the software
 * renderer the game draws its menu with) in "logical" units: a point is
 * drawn at offset + logical * scale, in the target's own pixels, so that
 * text is sharp at any window size. Colors are the palette below. */
#ifndef VF4FT_UI_H
#define VF4FT_UI_H

#include <SDL3/SDL.h>

/* the palette */
#define UI_BG       ((SDL_Color){ 14, 16, 22, 255 })
#define UI_PANEL    ((SDL_Color){ 21, 24, 33, 255 })
#define UI_CARD     ((SDL_Color){ 28, 32, 43, 255 })
#define UI_RAISED   ((SDL_Color){ 38, 43, 57, 255 })
#define UI_HOVER    ((SDL_Color){ 48, 54, 71, 255 })
#define UI_LINE     ((SDL_Color){ 50, 56, 74, 255 })
#define UI_TEXT     ((SDL_Color){ 236, 238, 243, 255 })
#define UI_MUTED    ((SDL_Color){ 154, 161, 178, 255 })
#define UI_FAINT    ((SDL_Color){ 99, 106, 124, 255 })
#define UI_ACCENT   ((SDL_Color){ 242, 179, 61, 255 })
#define UI_ON_ACCENT ((SDL_Color){ 30, 22, 6, 255 })
#define UI_GOOD     ((SDL_Color){ 76, 195, 138, 255 })
#define UI_BAD      ((SDL_Color){ 240, 96, 93, 255 })

enum { UI_REGULAR, UI_MEDIUM, UI_BOLD, UI_MONO, UI_FACES };

/* icons, drawn from distance functions so that their edges are smooth */
enum {
    UI_I_LEFT, UI_I_RIGHT, UI_I_UP, UI_I_DOWN,      /* filled triangles */
    UI_I_CHECK, UI_I_CHEVRON, UI_I_FAIL, UI_I_DOT,
    UI_I_PS_CROSS, UI_I_PS_CIRCLE, UI_I_PS_SQUARE, UI_I_PS_TRIANGLE,
    UI_I_PLAY,
    UI_I_VIEW, UI_I_MENU,                           /* Xbox and Steam: two windows, three lines */
    UI_I_PLUS, UI_I_MINUS, UI_I_HOME, UI_I_CAPTURE, /* Nintendo */
    UI_I_DOTS,                                      /* Steam's quick access button */
    UI_ICONS
};

/* 0, or -1 (SDL_GetError). Again after the renderer changes (ui_quit first). */
int ui_init(SDL_Renderer *r);
void ui_quit(void);

/* logical to pixels: pixel = offset + logical * scale (the glyph sheets are
 * made again when the scale changes) */
void ui_set_scale(float scale, float ox, float oy);
float ui_get_scale(void);
/* the scale and offset that fit a logical w x h in the middle of a pixel
 * target pw x ph */
void ui_fit(float w, float h, int pw, int ph);
/* logical coordinates of a pixel point (mouse) */
void ui_to_logical(float px, float py, float *x, float *y);

/* ---- text: UTF-8; y is the middle of the capitals (so a line can be
 * centered in a box), x the left edge; each returns the width drawn ---- */
float ui_text(int face, float size, float x, float y, SDL_Color c, const char *s);
float ui_text_w(int face, float size, const char *s);
/* align 0 left, 1 center, 2 right of x .. x + w; cut with an ellipsis to fit
 * (cut_start: the start is cut, for paths) */
void ui_text_in(int face, float size, float x, float y, float w, int align, int cut_start, SDL_Color c,
                const char *s);
/* the height between lines */
float ui_line_h(int face, float size);
/* wrapped to w, the first line's middle at y; returns the lines' count */
int ui_wrap(int face, float size, float x, float y, float w, float line_h, SDL_Color c, const char *s);
/* how many lines ui_wrap makes of s, drawing nothing */
int ui_wrap_lines(int face, float size, float w, const char *s);

/* ---- shapes ---- */
void ui_fill(float x, float y, float w, float h, SDL_Color c);
void ui_round(float x, float y, float w, float h, float r, SDL_Color c);
/* a rounded box with a border t wide (fill must be opaque) */
void ui_round_line(float x, float y, float w, float h, float r, float t, SDL_Color line, SDL_Color fill);
/* an icon in a size x size box centered at cx, cy */
void ui_icon(int icon, float cx, float cy, float size, SDL_Color c);
/* a texture over a logical rectangle */
void ui_image(SDL_Texture *t, float x, float y, float w, float h);
/* a PNG in the program over a logical rectangle (SDL 3.4's PNG loader; the
 * texture is kept until ui_quit). 0 when it cannot be drawn (an older SDL,
 * or a damaged PNG): the caller then draws something else */
int ui_png(const unsigned char *png, size_t n, float x, float y, float w, float h);
/* kofi_badge.c (tools/make_kofi.py): Ko-fi's "Support me on Ko-fi" button, 672 x 356 */
extern const unsigned char ui_kofi_badge_png[];
extern const size_t ui_kofi_badge_png_size;

/* ---- input glyphs ---- */
/* what the hints show: the keyboard's keys or a gamepad's buttons, in the
 * style of its maker (SDL's gamepad type; Steam Input's own pad on a Steam
 * Deck is the Deck) */
enum {
    UI_DEV_KEYS,
    UI_DEV_XBOX,            /* Xbox One and Series: View, Menu */
    UI_DEV_PS,              /* PS5: Create, Options */
    UI_DEV_NINTENDO,        /* Switch, Joy-Con: B A Y X by position, -, + */
    UI_DEV_XBOX360,         /* Back, Start */
    UI_DEV_PS4,             /* Share, Options; shapes in color */
    UI_DEV_PS3,             /* Select, Start; shapes in color */
    UI_DEV_GAMECUBE,        /* A X B Y by position, Z, L, R */
    UI_DEV_STEAM,           /* Steam Deck and Steam Controller: L1 R1 L2 R2, L4 L5 R4 R5 */
    UI_DEV_GENERIC,         /* any other: A B X Y, LB RB LT RT, Back, Start */
    UI_DEVS
};
/* a style by its name (keys, xbox, xbox360, ps5 or ps, ps4, ps3, switch,
 * gamecube, steam, generic), or -1: for tests */
int ui_device_named(const char *name);
/* follows the last key or button pressed (call it with every event);
 * returns 1 when the caps' names changed with it: another style, or another
 * pad of the same style whose face buttons are labeled otherwise */
int ui_note_event(const SDL_Event *e);
int ui_device(void);
void ui_set_device(int dev);
/* the style of gamepad glyphs: the last pad used, else the first one there */
int ui_pad_style(void);

/* Steam Input's twin pads (docs/design.md s30). Under Proton with Steam
 * Input on, SDL sees Steam's virtual pad and also the real pad behind it, so
 * one press would be two players'. The real pad Steam names for a virtual
 * pad's slot (its SteamVirtualGamepadInfo file) is left out; when the file
 * cannot be read, every real pad is while a virtual one is there.
 * ui_gamepads: SDL_GetGamepads without the pads left out (SDL_free it).
 * ui_pad_event_hidden: 1 for an event of a pad left out (skip it; call it
 * with every event, before ui_note_event). ui_pad_logger: where the lines
 * saying which pad is left out go. */
SDL_JoystickID *ui_gamepads(int *count);
int ui_pad_hidden(SDL_JoystickID id);
int ui_pad_event_hidden(const SDL_Event *e);
void ui_pad_logger(void (*log)(const char *line));

/* a key cap with a label (middle at y, h high); returns its width */
float ui_cap(float x, float y, float h, const char *label);
/* a cap with an icon in it */
float ui_cap_icon(float x, float y, float h, int icon);
/* a key's or a gamepad input's cap by its SDL name (settings.h's names):
 * pad 1 for a gamepad name (drawn in the style of ui_pad_style) */
float ui_cap_input(float x, float y, float h, const char *name, int pad);
float ui_cap_input_w(float h, const char *name, int pad);

/* what a hint is for: drawn as the device's own key or button */
enum {
    UI_IN_UPDOWN, UI_IN_LEFTRIGHT, UI_IN_OK, UI_IN_BACK, UI_IN_PREV, UI_IN_NEXT,
    UI_IN_CLEAR, UI_IN_DEFAULT, UI_IN_NONE
};
typedef struct ui_hint {
    const char *text;
    int in[2];              /* UI_IN_*; UI_IN_NONE for none */
} ui_hint;
/* a row of hints, its middle at y; align 0 from x rightwards, 2 ending at x;
 * returns the width */
float ui_hints(float x, float y, float h, const ui_hint *hints, int n, int align);

/* a gamepad face button's job in a menu. Menus go by the buttons' labels,
 * the game by their places: select is the button labeled A or ✕, back B or
 * ○, the launcher's clear X or □ and default Y or △. So on a Switch pad
 * select is the right button and back the bottom one, and on a GameCube pad
 * back is the left one. Returns UI_IN_OK, _BACK, _CLEAR or _DEFAULT, or
 * UI_IN_NONE for a button that is not a face button. */
int ui_pad_face(SDL_JoystickID pad, int button);
/* the SDL name of the face button that does UI_IN_OK, _BACK, _CLEAR or
 * _DEFAULT on the pad last used ("a", "b", "x" or "y": names by place),
 * for its cap */
const char *ui_pad_face_name(int in);

/* ---- widgets ---- */
/* the tabs bar: the previous and next caps at its ends */
void ui_tabs(float x, float y, float w, float h, const char *const *names, int n, int cur);
/* after ui_tabs: the tab at a logical point, UI_TAB_PREV or UI_TAB_NEXT for
 * the caps, or -1 */
#define UI_TAB_PREV (-2)
#define UI_TAB_NEXT (-3)
int ui_tabs_hit(float x, float y);

enum { UI_ROW_ACTION, UI_ROW_CHOICE, UI_ROW_TOGGLE, UI_ROW_SLIDER, UI_ROW_PAD, UI_ROW_KEYS, UI_ROW_TEXT };
typedef struct ui_row {
    const char *label;
    int kind;               /* UI_ROW_* */
    const char *value;      /* CHOICE, TEXT: the value; PAD, KEYS: the binding's names, ", " between */
    int on;                 /* TOGGLE */
    int level;              /* SLIDER: 0..100 */
    int later;              /* a mark: it applies at the next start */
    int warn;               /* the label in red (a choice that asks again) */
    int off;                /* greyed: does nothing now */
    int waiting;            /* PAD, KEYS: waits for its key or button */
} ui_row;
/* rows from y down, each row_h high; the list scrolls to keep sel in view
 * (*top: the first row shown, kept between calls) */
void ui_rows(float x, float y, float w, float h, float row_h, const ui_row *rows, int n, int sel, int *top);
/* the row at a logical point after ui_rows, or -1 */
int ui_rows_hit(float x, float y);
/* the description panel: a title, wrapped text and a note in the accent */
void ui_about(float x, float y, float w, float h, const char *title, const char *text, const char *note);
/* a progress bar: frac 0..1, or < 0 for one that moves (t: seconds) */
void ui_bar(float x, float y, float w, float h, float frac, double t);

#endif
