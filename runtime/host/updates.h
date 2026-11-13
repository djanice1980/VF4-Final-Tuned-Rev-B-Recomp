/* The game's side of updates (docs/design.md section 25): the menu's
 * "Check for updates" row. A game in a release folder's game/ asks the
 * release's tools/update.py whether a newer release is out; it downloads
 * nothing itself. "Update when I quit" leaves update/at-game-quit in the
 * release folder, and the launcher does the update (section 24): when the
 * game it started ends, or, for a game started on its own (from Steam),
 * when the game starts it with --after-game as it quits. */
#ifndef VF4FT_UPDATES_H
#define VF4FT_UPDATES_H

enum {
    GU_OFF,         /* not in a release folder (or no launcher, Python or update.py there): no row */
    GU_IDLE,        /* not checked yet */
    GU_CHECKING,
    GU_NONE,        /* this is the newest release */
    GU_FAILED,      /* the check failed: gu_error() */
    GU_AVAILABLE,   /* gu_newer() is out */
    GU_ASKED,       /* it goes in when the game quits */
};

/* finds the release folder: the parent of exe_dir (SDL_GetBasePath()), or
 * the one build.json there names (an install folder chosen elsewhere) */
void gu_init(const char *exe_dir, void (*log)(const char *line));
int gu_state(void);
const char *gu_version(void);       /* this release's */
const char *gu_newer(void);         /* the newer one (GU_AVAILABLE, GU_ASKED) */
long long gu_size(void);            /* its package's size in bytes, or 0 */
const char *gu_error(void);         /* why the check failed */
int gu_by_launcher(void);           /* the launcher started the game, and does the update when it ends */

/* the row chosen: a check, or "Update when I quit" turned on or off */
void gu_press(void);
/* every turn of the window's loop; 1 when the state changed */
int gu_poll(void);
/* the game quits: a check still going is ended; an update asked for is
 * handed to the launcher, unless the launcher started the game */
void gu_quit(void);

#endif
