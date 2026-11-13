/* The players' cards: see cards.h. */
#include "cards.h"

#include <string.h>

#define REC 20                  /* the record's first byte in the card (word 5) */

void cards_path(char *out, size_t n, const char *user_dir, const char *name)
{
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    SDL_snprintf(out, n, "%scards%s%s.card", user_dir, sep, name);
}

static int by_name(const void *a, const void *b)
{
    return SDL_strcasecmp((const char *)a, (const char *)b);
}

int cards_list(const char *user_dir, char (*names)[CARD_NAME_MAX], int max)
{
    char dir[4200];
    SDL_snprintf(dir, sizeof dir, "%scards", user_dir);
    int count = 0, n = 0;
    char **found = SDL_GlobDirectory(dir, "*.card", SDL_GLOB_CASEINSENSITIVE, &count);
    for (int i = 0; found && i < count && n < max; i++) {
        const char *f = found[i];
        size_t len = strlen(f);
        if (len <= 5 || len - 5 >= CARD_NAME_MAX)
            continue;
        char name[CARD_NAME_MAX];
        memcpy(name, f, len - 5);
        name[len - 5] = 0;
        if (!settings_card_name_ok(name))
            continue;
        SDL_strlcpy(names[n++], name, CARD_NAME_MAX);
    }
    SDL_free(found);
    SDL_qsort(names, (size_t)n, CARD_NAME_MAX, by_name);
    return n;
}

Uint16 card_crc16(const Uint8 *p, size_t n)
{
    Uint16 crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (Uint16)(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = crc & 0x8000 ? (Uint16)((crc << 1) ^ 0x8005) : (Uint16)(crc << 1);
    }
    return crc;
}

static unsigned le16(const Uint8 *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static void put_le16(Uint8 *p, unsigned v)
{
    p[0] = (Uint8)v;
    p[1] = (Uint8)(v >> 8);
}

/* the record's format, as card_unpack and card_format_get find it */
static int format_of(const Uint8 *r)
{
    int f = r[2] >> 4 & 3;
    if (f == 3)
        f = ((r[18] & 0xfe) >> 1 & 3) + 3;
    return f;
}

void card_view_of(const Uint8 *card, card_view *v)
{
    const Uint8 *r = card + REC;
    SDL_zerop(v);
    v->number = (Uint32)card[4] << 24 | (Uint32)card[5] << 16 | (Uint32)card[6] << 8 | card[7];
    v->blank = r[107] == 0xff;
    for (int i = 0; i < 107 && v->blank; i++)
        v->blank = r[i] == 0;
    if (v->blank)
        return;
    v->format = format_of(r);
    v->header_ok = card_crc16(r + 2, 22) == le16(r) || card_crc16(r + 86, 22) == le16(r + 84);
    v->body_ok = card_crc16(r + 26, 58) == le16(r + 24);
    v->chara = r[3] & 15;
    v->level = r[4];
    v->wins = (int)le16(r + 5);
    v->losses = (int)le16(r + 7);
    v->plays_left = (int)((r[18] & 1) << 8 | r[17]);
    for (int i = 0; i < 64; i++)
        v->items += r[9 + i / 8] >> (i & 7) & 1;
    card_name_text(r + 44, v->name, sizeof v->name);
}

const char *const card_chara_names[CARD_CHARAS] = {
    "Akira", "Sarah", "Lau", "Shun", "Jeffry", "Pai", "Jacky", "Kage",
    "Lion", "Wolf", "Aoi", "Lei-Fei", "Vanessa", "Dural", "Goh", "Brad",
};

static const char *ordinal(int k)
{
    if (k % 100 >= 11 && k % 100 <= 13)
        return "th";
    return k % 10 == 1 ? "st" : k % 10 == 2 ? "nd" : k % 10 == 3 ? "rd" : "th";
}

const char *card_rank_text(int level, char *buf, size_t n)
{
    if (level < 10)
        SDL_snprintf(buf, n, "%d%s kyu", 10 - level, ordinal(10 - level));
    else if (level <= CARD_TOP_RANK)
        SDL_snprintf(buf, n, "%d%s dan", level - 9, ordinal(level - 9));
    else
        SDL_snprintf(buf, n, "title rank %d", level - CARD_TOP_RANK);
    return buf;
}

void card_name_text(const Uint8 *name20, char *out, size_t n)
{
    size_t k = 0;
    for (int i = 0; i < CARD_NAME_LEN && name20[i] && k + 1 < n; i++) {
        Uint8 b = name20[i];
        if (b >= 0x80 && i + 1 < CARD_NAME_LEN && name20[i + 1] >= 0x80)
            i++;            /* EUC-JP: two bytes a character */
        out[k++] = b >= 0x20 && b < 0x7f ? (char)b : '?';
    }
    out[k] = 0;
}

int card_view_text(const card_view *v, int file, char *out, size_t n)
{
    char rank[40];
    if (file == 0) {
        SDL_strlcpy(out, "Not made yet: the game makes it at the player's first game, for the character chosen then.", n);
        return CARD_TEXT_NEW;
    }
    if (file < 0) {
        SDL_strlcpy(out, "This file is not a card (a card is 128 bytes): the player would have none.", n);
        return CARD_TEXT_BAD;
    }
    if (v->blank) {
        SDL_strlcpy(out, "A new card, never used: at its first game the game makes it the player's, for the character "
                         "chosen then.", n);
        return CARD_TEXT_NEW;
    }
    if (v->format != 4) {
        SDL_snprintf(out, n, "A card from an older version of the game (its format %d). The game brings it up to date.",
                     v->format);
        return CARD_TEXT_OLD;
    }
    if (!v->header_ok || !v->body_ok) {
        SDL_strlcpy(out, "Its checks do not hold, so the game would not read it.", n);
        return CARD_TEXT_BAD;
    }
    SDL_snprintf(out, n, "%s, %s\nRing name: %s\n%d %s, %d %s\n%d %s left\nItems: %s",
                 card_chara_names[v->chara & (CARD_CHARAS - 1)], card_rank_text(v->level, rank, sizeof rank),
                 v->name[0] ? v->name : "none", v->wins, v->wins == 1 ? "win" : "wins", v->losses,
                 v->losses == 1 ? "loss" : "losses", v->plays_left, v->plays_left == 1 ? "game" : "games",
                 v->items == 64 ? "all" : v->items == 0 ? "none" : "some");
    return CARD_TEXT_STATS;
}

/* a costume's colors: four parts of five bits, the low four in the packed
 * half (card_colors_to_packed), the fifth apart: costume 0's in byte 42's
 * top four bits (card_curse_bits), costume 1's in byte 77's top three bits
 * and byte 78's bit 2 (card_misc_bits) */
static int color_hi_bit(int costume, int part)
{
    if (costume == 0)
        return 42 * 8 + 4 + part;
    return part < 3 ? 77 * 8 + 5 + part : 78 * 8 + 2;
}

static int bit_get(const Uint8 *r, int bit)
{
    return r[bit >> 3] >> (bit & 7) & 1;
}

static void bit_put(Uint8 *r, int bit, int v)
{
    r[bit >> 3] = (Uint8)((r[bit >> 3] & ~(1 << (bit & 7))) | (v & 1) << (bit & 7));
}

void card_fields_get(const Uint8 *card, card_fields *f)
{
    const Uint8 *r = card + REC;
    SDL_zerop(f);
    memcpy(f->name, r + 44, CARD_NAME_LEN);
    f->chara = r[3] & 15;
    f->level = r[4];
    f->plays_left = (r[18] & 1) << 8 | r[17];
    memcpy(f->items, r + 9, 8);
    for (int c = 0; c < 2; c++) {
        unsigned packed = le16(r + 30 + 2 * c);
        for (int part = 0; part < 4; part++)
            f->color[c][part] = (int)(packed >> (4 * part) & 15) | bit_get(r, color_hi_bit(c, part)) << 4;
        int other = c == 0 ? r[64] >> 4 : r[64] & 15;   /* other_col: hair << 2 | eyes */
        f->hair[c] = other >> 2;
        f->eye[c] = other & 3;
        for (int i = 0; i < 4; i++)
            f->equip[c][i] = r[34 + 4 * c + i];
    }
}

void card_fields_put(Uint8 *card, const card_fields *f)
{
    Uint8 *r = card + REC;
    card_fields was;
    card_fields_get(card, &was);
    memcpy(r + 44, f->name, CARD_NAME_LEN);
    r[3] = (Uint8)((r[3] & 0xf0) | (f->chara & 15));
    if (f->level != was.level) {
        r[4] = (Uint8)f->level;
        r[28] = r[29] = 0;      /* exp and point: the new rank's progress */
    }
    r[17] = (Uint8)f->plays_left;
    r[18] = (Uint8)((r[18] & 0xfe) | (f->plays_left >> 8 & 1));
    memcpy(r + 9, f->items, 8);
    for (int c = 0; c < 2; c++) {
        /* card_misc_bits, byte 77: equip_flag0, equip_flag1, color_flag0,
         * color_flag1 (set when VF.NET wrote them, cleared by the game's own
         * item_equip and costume_colors_set) */
        if (memcmp(f->color[c], was.color[c], sizeof f->color[c])) {
            unsigned packed = 0;
            for (int part = 0; part < 4; part++) {
                packed |= (unsigned)(f->color[c][part] & 15) << (4 * part);
                bit_put(r, color_hi_bit(c, part), f->color[c][part] >> 4);
            }
            put_le16(r + 30 + 2 * c, packed);
            bit_put(r, 77 * 8 + 2 + c, 0);
        }
        if (f->hair[c] != was.hair[c] || f->eye[c] != was.eye[c]) {
            int other = (f->hair[c] & 3) << 2 | (f->eye[c] & 3);
            r[64] = (Uint8)(c == 0 ? (r[64] & 0x0f) | other << 4 : (r[64] & 0xf0) | other);
        }
        if (memcmp(f->equip[c], was.equip[c], sizeof f->equip[c])) {
            for (int i = 0; i < 4; i++)
                r[34 + 4 * c + i] = (Uint8)f->equip[c][i];
            bit_put(r, 77 * 8 + c, 0);
        }
    }
    card_seal(card);
}

void card_starter(Uint8 *card, Uint32 number)
{
    Uint8 *r = card + REC;
    card_blank(card, number);       /* card_seal below makes its last byte the header copy's */
    r[2] = 0x33;                    /* flags: a card made at a cabinet (format 3 bits) */
    r[3] = 0;                       /* Akira */
    r[4] = 0;                       /* 10th kyu */
    memset(r + 9, 0xff, 8);         /* item_have: every item */
    r[17] = CARD_PLAYS & 0xff;      /* use_count's low byte */
    r[18] = 0x02 | CARD_PLAYS >> 8; /* ftf: ex_ver 1 (Final Tuned's format 4); use_count's bit 8 */
    Uint32 play_id = SDL_rand_bits() & 0x7fff;
    put_le16(r + 26, play_id);
    memcpy(r + 44, "PLAYER", 6);
    card_seal(card);
}

void card_seal(Uint8 *card)
{
    Uint8 *r = card + REC;
    put_le16(r + 24, card_crc16(r + 26, 58));
    put_le16(r, card_crc16(r + 2, 22));
    memcpy(r + 84, r, 24);          /* the header's copy (card_pack: bytes 84 to 107) */
}

static void put_be32(Uint8 *p, Uint32 v)
{
    p[0] = (Uint8)(v >> 24);
    p[1] = (Uint8)(v >> 16);
    p[2] = (Uint8)(v >> 8);
    p[3] = (Uint8)v;
}

void card_blank(Uint8 *card, Uint32 number)
{
    /* what card_header_check wants (word 0's top four bits 1, word 3's top
     * half 0x046c) and card_raw_is_blank's 107 zeros and 0xff */
    memset(card, 0, CARD_SIZE);
    put_be32(card + 0, 0x10000000u);
    put_be32(card + 4, number);
    put_be32(card + 12, 0x046c0000u);
    card[CARD_SIZE - 1] = 0xff;
}

Uint32 card_new_number(void)
{
    Uint64 t = SDL_GetTicksNS() ^ (Uint64)SDL_GetPerformanceCounter();
    Uint32 r = SDL_rand_bits() ^ (Uint32)t ^ (Uint32)(t >> 32);
    return 0x20050000u | (r & 0xffff);
}
