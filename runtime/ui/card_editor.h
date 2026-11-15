/* The card editor's rows (docs/design.md sections 37 and 42): a card's ring
 * name, character, rank, games and items, and each costume's colors and
 * worn items, as rows of the UI kit. Shared by the launcher's editor page
 * and the game menu's, which each draw it and take its keys in their own
 * way; the card itself is cards.h's (card_fields_get, card_fields_put). */
#ifndef VF4FT_CARD_EDITOR_H
#define VF4FT_CARD_EDITOR_H

#include <stddef.h>

#include "cards.h"
#include "ui.h"

#define CED_TABS 3
extern const char *const ced_tab_names[CED_TABS];      /* "Card", "Costume 1", "Costume 2" */

/* the rows: the Card tab's, then each Costume tab's */
enum { CED_NAME, CED_CHARA, CED_RANK, CED_PLAYS, CED_ITEMS, CED_CARD_ROWS };
enum { CED_COLOR = 0, CED_HAIR = 4, CED_EYES = 5, CED_SLOT = 6, CED_COSTUME_ROWS = 10 };

typedef struct card_editor {
    card_fields f, f0;      /* as edited, as it was */
    int fresh;              /* a card never used, or not made yet: Save makes it a ready card */
    int tab, row, top;      /* the tab, the chosen row, the first row shown */
} card_editor;

/* opens it on card (CARD_SIZE bytes) at its Card tab; fresh: see above */
void ced_open(card_editor *e, const Uint8 *card, int fresh);
int ced_rows(const card_editor *e);
void ced_tab_to(card_editor *e, int t);             /* wraps; the first row */
void ced_move(card_editor *e, int d);               /* the row d down (wraps) */
void ced_change(card_editor *e, int d);             /* the chosen row's value: Left (-1), Right (1) */
int ced_on_name(const card_editor *e);              /* the chosen row is the ring name (OK types it) */
int ced_changed(const card_editor *e);
void ced_undo(card_editor *e);                      /* every change undone */

/* the row i, its value written to v (n bytes), its description in *about */
void ced_row(const card_editor *e, int i, ui_row *r, char *v, size_t n, const char **about);
/* the note under the description: a fresh card, a change, or NULL */
const char *ced_note(const card_editor *e);

/* the ring name: as text for its note to start from ("" when it has
 * characters outside ASCII: it is typed anew), and the typed text back
 * (spaces at its ends left out). ced_name_char: whether c can be added to a
 * name len bytes long (ASCII's letters, numbers and marks, up to 20) */
void ced_name_text(const card_editor *e, char *out, size_t n);
void ced_name_set(card_editor *e, const char *text);
int ced_name_char(int c, size_t len);

/* whether a card can be edited: a Final Tuned card whose checks hold, a
 * card never used, or one not made yet. file as card_view_text's (1 its
 * file is there, v read from it; 0 none; -1 not a card) */
int ced_editable(const card_view *v, int file);

#endif
