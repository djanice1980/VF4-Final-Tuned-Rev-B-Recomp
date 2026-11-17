/* The machine's pace from the screen's refresh: vsync.h, docs/design.md
 * section 58. */
#include "vsync.h"

#define VS_SEED 16                      /* raw intervals that start the period, or start it again */
#define VS_ALPHA (1.0 / 8)              /* how far a take's error moves the phase */
#define VS_BETA (1.0 / 64)              /* and the period, while a run is young */
#define VS_RING 512                     /* fitted takes kept: the period is measured across them */
#define VS_SPAN_NS 500000000.0          /* once they span this long */
#define VS_NUDGE_NS 500000.0            /* and then no take's error counts for more than this (one late present) */
#define VS_MISS 4                       /* a take up to this many refreshes after the one before still fits */
#define VS_MISFITS 3                    /* takes in a row that do not fit: the phase starts again */
#define VS_MULTI 16                     /* takes in a row more than a refresh apart: the period again */
#define VS_STEADY_NS 1000000000ull      /* the refresh is steady once takes have fitted for this long */
#define VS_STALE 8                      /* refreshes with no take: nothing is being shown */
#define VS_TOLERANCE 0.0025             /* k refreshes are a frame when within this of it */
#define VS_TOLERANCE_HELD 0.005         /* and stay one within this, once followed */
#define VS_SLEW_NS 500000.0             /* the machine's phase is moved by at most this a frame */

enum { WHY_FOLLOW, WHY_UNKNOWN, WHY_STALE, WHY_RATE, WHY_FREE };

static struct {
    SDL_SpinLock lock;
    Uint64 last;                /* the latest take, as it came (0: none yet) */
    double anchor;              /* the latest refresh's take, smoothed (ns) */
    double td;                  /* one refresh, smoothed (ns); 0: not known yet */
    Uint64 index;               /* refreshes counted, at the latest take */
    Uint64 steady_since;        /* the first take of the run that has fitted since (0: none) */
    int misfits, multi;         /* takes in a row that did not fit; that fitted more than a refresh on */
    Uint64 iv[VS_SEED];         /* the latest raw intervals */
    unsigned niv;
    struct { Uint64 index; double anchor; } ring[VS_RING];     /* the run's fitted takes */
    unsigned run;               /* how many of them (the newest VS_RING are kept) */
    /* the machine's side */
    int following, k;           /* its last frame followed the screen, k refreshes to a frame */
    Uint64 slot;                /* index / k of the take that last took a frame */
    int why;
    double why_hz;
    const char *free_why;
} vs;

/* one refresh from the latest raw intervals: their median, then all of them over the refreshes
 * they span by it (a present missed makes an interval two) */
static double seed(void)
{
    Uint64 a[VS_SEED];
    for (int i = 0; i < VS_SEED; i++) {
        Uint64 v = vs.iv[i];
        int j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
    double med = ((double)a[VS_SEED / 2 - 1] + (double)a[VS_SEED / 2]) / 2, sum = 0, n = 0;
    if (med <= 0)
        return 0;
    for (int i = 0; i < VS_SEED; i++) {
        double m = SDL_floor((double)a[i] / med + 0.5);
        if (m >= 1 && m <= VS_MISS && SDL_fabs((double)a[i] - m * med) <= med / 4) {
            sum += (double)a[i];
            n += m;
        }
    }
    return n >= VS_SEED / 2 ? sum / n : med;
}

/* the phase starts again at t: a new run */
static void restart(Uint64 t)
{
    vs.anchor = (double)t;
    vs.steady_since = 0;
    vs.run = 0;
    vs.multi = 0;
}

void vsync_reset(void)
{
    SDL_LockSpinlock(&vs.lock);
    vs.last = 0;
    vs.td = 0;
    vs.index = vs.slot = 0;
    vs.misfits = 0;
    vs.niv = 0;
    restart(0);
    SDL_UnlockSpinlock(&vs.lock);
}

int vsync_take(Uint64 t)
{
    int take = 1;
    SDL_LockSpinlock(&vs.lock);
    if (!vs.last) {
        vs.anchor = (double)t;
    } else {
        vs.iv[vs.niv++ % VS_SEED] = t - vs.last;
        if (vs.td <= 0) {
            if (vs.niv >= VS_SEED) {            /* the period from the first intervals */
                vs.td = seed();
                vs.misfits = 0;
                restart(t);
            }
        } else {
            double m = SDL_floor(((double)t - vs.anchor) / vs.td + 0.5);
            double err = (double)t - (vs.anchor + m * vs.td);
            if (m < 1 || m > VS_MISS || SDL_fabs(err) > vs.td / 4) {
                /* not where the grid says. One such (a present held up, a stall) changes nothing: the
                 * grid goes on by its period. Several in a row: the phase starts again here, and
                 * after more the period too */
                if (m >= 1) {
                    vs.anchor += m * vs.td;
                    vs.index += (Uint64)m;
                }
                if (++vs.misfits >= VS_MISFITS)
                    restart(t);
                if (vs.misfits >= VS_SEED) {
                    vs.td = seed();
                    vs.misfits = 0;
                }
            } else {
                /* the phase a little towards it. The period: while the run is young, by the error
                 * too, so that a first period a little off is put right fast; then measured across
                 * the run's takes, which a late present or a step in the phase hardly moves, and
                 * no take's error counts for more than VS_NUDGE_NS */
                unsigned old = vs.run >= VS_RING ? (vs.run + 1) % VS_RING : 0;
                int measured = vs.run > 0 && vs.anchor - vs.ring[old].anchor >= VS_SPAN_NS;
                if (measured) {
                    if (err > VS_NUDGE_NS) err = VS_NUDGE_NS;
                    if (err < -VS_NUDGE_NS) err = -VS_NUDGE_NS;
                }
                vs.anchor += m * vs.td + err * VS_ALPHA;
                vs.index += (Uint64)m;
                vs.misfits = 0;
                if (!vs.steady_since)
                    vs.steady_since = t;
                vs.ring[vs.run % VS_RING].index = vs.index;
                vs.ring[vs.run % VS_RING].anchor = vs.anchor;
                vs.run++;
                if (measured)
                    vs.td += ((vs.anchor - vs.ring[old].anchor) / (double)(vs.index - vs.ring[old].index) - vs.td) / 16;
                else
                    vs.td += err * VS_BETA / m;
                /* every take two refreshes or more on: the period was taken for a fraction of the real one */
                vs.multi = m > 1 ? vs.multi + 1 : 0;
                if (vs.multi >= VS_MULTI && vs.niv >= VS_SEED) {
                    vs.td = seed();
                    restart(t);
                }
            }
        }
    }
    vs.last = t;
    if (vs.following && vs.k > 1) {     /* several refreshes to a frame: the first of them takes it */
        Uint64 slot = vs.index / (Uint64)vs.k;
        take = slot != vs.slot;
        if (take)
            vs.slot = slot;
    }
    SDL_UnlockSpinlock(&vs.lock);
    return take;
}

Uint64 vsync_next(Uint64 now, Uint64 due, Uint64 frame_ns, Uint64 lead_ns, int fresh)
{
    Uint64 next = 0;
    double k = 1;
    SDL_LockSpinlock(&vs.lock);
    int why = WHY_FOLLOW;
    if (vs.td <= 0 || !vs.steady_since || vs.last - vs.steady_since < VS_STEADY_NS) {
        why = WHY_UNKNOWN;
    } else if ((double)((Sint64)now - (Sint64)vs.last) > VS_STALE * vs.td) {
        why = WHY_STALE;
    } else {
        k = SDL_floor((double)frame_ns / vs.td + 0.5);
        double tol = vs.following ? VS_TOLERANCE_HELD : VS_TOLERANCE;
        if (k < 1 || SDL_fabs(k * vs.td - (double)frame_ns) > tol * (double)frame_ns)
            why = WHY_RATE;
    }
    if (why == WHY_FOLLOW) {
        /* a frame every k refreshes, handed over (lead_ns after its start) halfway between the first
         * of them and the first of the next k: the period is the screen's, and what is left of the
         * phase's error goes a quarter a frame (at once when fresh) */
        double period = k * vs.td;
        double slot0 = vs.anchor - (double)(vs.index % (Uint64)k) * vs.td;
        double want = (double)due + period;
        double r = SDL_fmod(want + (double)lead_ns - period / 2 - slot0, period);
        if (r < 0)
            r += period;
        if (r > period / 2)
            r -= period;
        if (!fresh) {
            r /= 4;
            if (r > VS_SLEW_NS) r = VS_SLEW_NS;
            if (r < -VS_SLEW_NS) r = -VS_SLEW_NS;
        }
        next = (Uint64)(want - r + 0.5);
    }
    vs.following = why == WHY_FOLLOW;
    vs.k = why == WHY_FOLLOW ? (int)k : 1;
    vs.why = why;
    vs.why_hz = vs.td > 0 ? 1e9 / vs.td : 0;
    SDL_UnlockSpinlock(&vs.lock);
    return next;
}

void vsync_free(const char *why)
{
    SDL_LockSpinlock(&vs.lock);
    vs.following = 0;
    vs.k = 1;
    vs.why = WHY_FREE;
    vs.free_why = why;
    SDL_UnlockSpinlock(&vs.lock);
}

void vsync_describe(char *buf, size_t n)
{
    SDL_LockSpinlock(&vs.lock);
    int why = vs.why, k = vs.k;
    double hz = vs.why_hz;
    const char *fw = vs.free_why;
    SDL_UnlockSpinlock(&vs.lock);
    switch (why) {
    case WHY_FOLLOW:
        if (k > 1)
            SDL_snprintf(buf, n, "the screen's refresh (%.3f Hz, %d refreshes to a frame)", hz, k);
        else
            SDL_snprintf(buf, n, "the screen's refresh (%.3f Hz)", hz);
        break;
    case WHY_RATE:
        SDL_snprintf(buf, n, "the game's own timer (the screen refreshes at %.2f Hz, not 60 Hz or a multiple of it)", hz);
        break;
    case WHY_STALE:
        SDL_snprintf(buf, n, "the game's own timer (nothing is being shown)");
        break;
    case WHY_FREE:
        SDL_snprintf(buf, n, "the game's own timer (%s)", fw ? fw : "not by the screen");
        break;
    default:
        SDL_snprintf(buf, n, "the game's own timer (the screen's refresh is not steady, or not measured yet)");
        break;
    }
}
