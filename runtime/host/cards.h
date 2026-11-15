/* The players' cards (docs/design.md section 37): card profiles, each a
 * 128-byte card in the game's user/cards folder as NAME.card, and the card
 * itself as the game's card library reads and writes it. Shared by the game
 * (its menu's Cards tab) and the launcher (its Profiles page).
 *
 * A card is 32 words as the card unit holds them (runtime/machine/hw_card.c):
 * words 0 to 4 the card's own (word 1 its number), then the 108 bytes the
 * game packs its record into (vf4lib/libcard, card_pack and card_unpack in
 * the decompilation). Final Tuned writes the record in its format 4, with
 * three CRC-16s: over the header, the body and a copy of the header. */
#ifndef VF4FT_CARDS_H
#define VF4FT_CARDS_H

#include <stddef.h>

#include <SDL3/SDL.h>

#include "settings.h"

#define CARD_SIZE 128
#define CARDS_MAX 64            /* profiles listed */

/* user_dir (ending in a separator) + "cards" + the separator + name + ".card" */
void cards_path(char *out, size_t n, const char *user_dir, const char *name);

/* the profiles in user_dir's cards folder, by name (letter case aside); at
 * most max. Files whose names are not profile names are left out */
int cards_list(const char *user_dir, char (*names)[CARD_NAME_MAX], int max);

/* the card's record, as the game reads it */
typedef struct card_view {
    int blank;              /* never used: the game makes it a player's at its first game */
    int format;             /* 4: Final Tuned's; 3: Evolution's; else older */
    int header_ok, body_ok; /* the CRCs (the header or its copy) */
    int chara;              /* the character (card_hdr_char_num): card_chara_names */
    int level;              /* the rank: card_rank_text */
    int wins, losses;       /* versus matches only (all_win, all_lose): the game counts no fight
                             * against the computer */
    int plays_left;         /* use_count */
    int items;              /* the items it has (bits of item_have) */
    char name[44];          /* the ring name as text (card_name_text) */
    Uint32 number;          /* word 1: the card's own number */
} card_view;

void card_view_of(const Uint8 *card, card_view *v);

/* the characters by their number on the card; 13 (Dural) is no player's */
#define CARD_CHARAS 16
#define CARD_NOT_A_PLAYER 13
extern const char *const card_chara_names[CARD_CHARAS];

/* the rank as text: 10th kyu (0) to 1st kyu, 1st dan (10) to 10th dan (19),
 * above that the titles VF.NET gave ("title rank N") */
#define CARD_TOP_RANK 19        /* the highest a card reaches without VF.NET */
const char *card_rank_text(int level, char *buf, size_t n);

/* the ring name (20 bytes, EUC-JP, NULs after it) as text: its ASCII as it
 * is, a character outside ASCII as "?" */
void card_name_text(const Uint8 *name20, char *out, size_t n);

/* a card's details as lines of text, for the launcher's Cards tab and the
 * game menu's: character and rank, ring name, versus record, games left, items;
 * else what the card is. file: 1 its file is there (v read from it), 0 not
 * made yet (no file), -1 the file is not a card (not 128 bytes). Returns
 * which text it is */
enum { CARD_TEXT_STATS, CARD_TEXT_NEW, CARD_TEXT_OLD, CARD_TEXT_BAD };
int card_view_text(const card_view *v, int file, char *out, size_t n);

/* what the launcher's card editor changes, in a Final Tuned card (format 4) */
#define CARD_NAME_LEN 20
#define CARD_COLORS 19          /* a part's colors: 0 its default, 1 to 19 */
#define CARD_HAIRS 2            /* 0 the default, 1 to 2 */
#define CARD_EYES 3             /* 0 the default, 1 to 3 */
#define CARD_ITEMS 199          /* a slot's item: 0 none, 1 to 199 (the character's list) */
#define CARD_PLAYS 500          /* a new card's games */
typedef struct card_fields {
    Uint8 name[CARD_NAME_LEN];  /* the ring name's bytes */
    int chara;
    int level;
    int plays_left;             /* 0 to 511 */
    Uint8 items[8];             /* item_have: a bit per item */
    int color[2][4];            /* per costume, its four parts */
    int hair[2], eye[2];
    int equip[2][4];            /* per costume, its four item slots */
} card_fields;

void card_fields_get(const Uint8 *card, card_fields *f);
/* f into the card, as the game's own changes write them (a new rank starts
 * with no progress; changed colors or items clear the costume's flag that
 * VF.NET set them), then card_seal */
void card_fields_put(Uint8 *card, const card_fields *f);

/* some of a card's fields changed (docs/design.md section 42): the game
 * menu's editor keeps one for a card until the card can take it, which for
 * a card a game is using is when the game lets it go, so the edit goes onto
 * the game's latest. A field of set is nonzero when the edit sets it (for
 * name and items: their first byte) */
typedef struct card_edit {
    card_fields to;         /* what each field it sets becomes */
    card_fields set;
} card_edit;

/* the fields that differ between from and to go into e (over what it set before) */
void card_edit_add(card_edit *e, const card_fields *from, const card_fields *to);
/* the fields e sets, into f */
void card_edit_apply(const card_edit *e, card_fields *f);

/* a card ready to play, as the launcher's New makes it: Akira, 10th kyu,
 * the ring name PLAYER, every item, 500 games, numbered number */
void card_starter(Uint8 *card, Uint32 number);

/* CRC-16, polynomial 0x8005, from 0, high bit first (the game's card_crc16
 * with its second table, which Final Tuned's format uses) */
Uint16 card_crc16(const Uint8 *p, size_t n);

/* after a change to the record: the header's copy and the three CRCs */
void card_seal(Uint8 *card);

/* a card before its first use, numbered number (as the card unit's model
 * makes one) */
void card_blank(Uint8 *card, Uint32 number);

/* a new card number for a profile (a card never used made by the launcher):
 * different from the unit model's own numbering */
Uint32 card_new_number(void);

#endif
