/* The card editor's rows: see card_editor.h. */
#include "card_editor.h"

#include <string.h>

const char *const ced_tab_names[CED_TABS] = { "Card", "Costume 1", "Costume 2" };

void ced_open(card_editor *e, const Uint8 *card, int fresh)
{
    SDL_zerop(e);
    card_fields_get(card, &e->f);
    e->f0 = e->f;
    e->fresh = fresh;
}

int ced_rows(const card_editor *e)
{
    return e->tab == 0 ? CED_CARD_ROWS : CED_COSTUME_ROWS;
}

void ced_tab_to(card_editor *e, int t)
{
    e->tab = (t % CED_TABS + CED_TABS) % CED_TABS;
    e->row = e->top = 0;
}

void ced_move(card_editor *e, int d)
{
    int n = ced_rows(e);
    e->row = ((e->row + d) % n + n) % n;
}

int ced_on_name(const card_editor *e)
{
    return e->tab == 0 && e->row == CED_NAME;
}

int ced_changed(const card_editor *e)
{
    return memcmp(&e->f, &e->f0, sizeof e->f) != 0;
}

void ced_undo(card_editor *e)
{
    e->f = e->f0;
}

/* the games a card can be given: Left and Right go to the next of these */
static const int plays[] = { 1, 10, 50, 100, 200, 300, 400, CARD_PLAYS };

void ced_change(card_editor *e, int d)
{
    card_fields *f = &e->f;
    if (e->tab == 0) {
        switch (e->row) {
        case CED_CHARA:
            do
                f->chara = (f->chara + d + CARD_CHARAS) % CARD_CHARAS;
            while (f->chara == CARD_NOT_A_PLAYER);
            break;
        case CED_RANK:
            /* a title rank (VF.NET's) is left only by going down */
            if (f->level > CARD_TOP_RANK)
                f->level = d < 0 ? CARD_TOP_RANK : f->level;
            else
                f->level = (f->level + d + CARD_TOP_RANK + 1) % (CARD_TOP_RANK + 1);
            break;
        case CED_PLAYS: {
            int n = (int)SDL_arraysize(plays), v = f->plays_left, to = -1;
            for (int i = 0; i < n && to < 0; i++)
                if (d > 0 && plays[i] > v)
                    to = plays[i];
            for (int i = n - 1; i >= 0 && to < 0; i--)
                if (d < 0 && plays[i] < v)
                    to = plays[i];
            f->plays_left = to >= 0 ? to : d > 0 ? plays[0] : plays[n - 1];
            break;
        }
        case CED_ITEMS: {
            /* every item, none, as the card had them (when that is neither) */
            static const Uint8 all[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, none[8] = { 0 };
            const Uint8 *ways[3] = { all, none, e->f0.items };
            int n = memcmp(e->f0.items, all, 8) && memcmp(e->f0.items, none, 8) ? 3 : 2, at = 0;
            for (int i = 0; i < n; i++)
                if (!memcmp(f->items, ways[i], 8))
                    at = i;
            memcpy(f->items, ways[(at + d + n) % n], 8);
            break;
        }
        }
        return;
    }
    int c = e->tab - 1, r = e->row;
    if (r < CED_HAIR)
        f->color[c][r] = (f->color[c][r] + d + CARD_COLORS + 1) % (CARD_COLORS + 1);
    else if (r == CED_HAIR)
        f->hair[c] = (f->hair[c] + d + CARD_HAIRS + 1) % (CARD_HAIRS + 1);
    else if (r == CED_EYES)
        f->eye[c] = (f->eye[c] + d + CARD_EYES + 1) % (CARD_EYES + 1);
    else
        f->equip[c][r - CED_SLOT] = (f->equip[c][r - CED_SLOT] + d + CARD_ITEMS + 1) % (CARD_ITEMS + 1);
}

void ced_row(const card_editor *e, int i, ui_row *r, char *v, size_t n, const char **about)
{
    SDL_zerop(r);
    r->kind = UI_ROW_CHOICE;
    r->value = v;
    const card_fields *f = &e->f;
    if (e->tab == 0) {
        switch (i) {
        case CED_NAME:
            r->label = "Ring name";
            r->kind = UI_ROW_TEXT;
            card_name_text(f->name, v, n);
            if (!v[0])
                SDL_strlcpy(v, "None", n);
            *about = "The name the game shows for the player, up to 20 letters, numbers and marks. A cabinet's "
                     "cards had it set on VF.NET; a card with none shows the game's own text instead. Press A or "
                     "Enter to type it.";
            break;
        case CED_CHARA:
            r->label = "Character";
            SDL_strlcpy(v, card_chara_names[f->chara], n);
            *about = "A card is one character's: with it in, the game picks that character at the select "
                     "screen. Items are numbered per character, so the worn items mean other items for another "
                     "one.";
            break;
        case CED_RANK:
            r->label = "Rank";
            card_rank_text(f->level, v, n);
            *about = "From 10th kyu to 10th dan, the ranks a card reaches without VF.NET. A new rank starts with "
                     "no progress toward the next.";
            break;
        case CED_PLAYS:
            r->label = "Games left";
            SDL_snprintf(v, n, f->plays_left ? "%d" : "None (used up)", f->plays_left);
            *about = "How many more games the card plays: each game takes one, and at none the card is used up. "
                     "A new card has 500.";
            break;
        case CED_ITEMS: {
            r->label = "Items owned";
            int k = 0;
            for (int b = 0; b < 64; b++)
                k += f->items[b / 8] >> (b & 7) & 1;
            SDL_snprintf(v, n, k == 64 ? "All" : k == 0 ? "None" : "%d of 64", k);
            *about = "The items the card has won, which the game's own item screens offer. A ready card has all "
                     "of them.";
            break;
        }
        }
        return;
    }
    int c = e->tab - 1;
    static const char *const color_labels[4] = { "Color, part 1", "Color, part 2", "Color, part 3",
                                                 "Color, part 4" };
    static const char *const slot_labels[4] = { "Item slot 1", "Item slot 2", "Item slot 3", "Item slot 4" };
    int val;
    if (i < CED_HAIR) {
        r->label = color_labels[i];
        val = f->color[c][i];
        *about = "The costume's colors, four parts: each its own color, or one of 19 others as a cabinet's "
                 "customize screen gave them.";
    } else if (i == CED_HAIR) {
        r->label = "Hair color";
        val = f->hair[c];
        *about = "The hair: its own color, or one of 2 others.";
    } else if (i == CED_EYES) {
        r->label = "Eye color";
        val = f->eye[c];
        *about = "The eyes: their own color, or one of 3 others.";
    } else {
        r->label = slot_labels[i - CED_SLOT];
        val = f->equip[c][i - CED_SLOT];
        SDL_snprintf(v, n, val ? "Item %d" : "Empty", val);
        *about = "An item the character wears, by its number in the character's list (1 to 199), or none. A "
                 "number the character has no item for shows nothing. Hold Left or Right to go faster.";
        return;
    }
    SDL_snprintf(v, n, val ? "Color %d" : "Its own", val);
}

const char *ced_note(const card_editor *e)
{
    return e->fresh ? "Save makes it a ready card." : ced_changed(e) ? "Changed: Save keeps it." : NULL;
}

void ced_name_text(const card_editor *e, char *out, size_t n)
{
    char t[44];
    card_name_text(e->f.name, t, sizeof t);
    SDL_strlcpy(out, SDL_strchr(t, '?') ? "" : t, n);     /* a name not in ASCII: anew */
}

void ced_name_set(card_editor *e, const char *t)
{
    while (*t == ' ')
        t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ')
        n--;
    memset(e->f.name, 0, sizeof e->f.name);
    memcpy(e->f.name, t, SDL_min(n, sizeof e->f.name));
}

int ced_name_char(int c, size_t len)
{
    return c >= 0x20 && c < 0x7f && len < CARD_NAME_LEN;
}

int ced_editable(const card_view *v, int file)
{
    if (file == 0)
        return 1;
    return file > 0 && (v->blank || (v->format == 4 && v->header_ok && v->body_ok && v->chara != CARD_NOT_A_PLAYER));
}
