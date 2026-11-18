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
#define VS_BAD 6                        /* a take that does not fit counts 2, one that fits takes 1 off: at
                                         * this the phase starts again (3 in a row, or most of a dozen) */
#define VS_AGAIN 8                      /* takes fitting after that, with the period measured before: steady */
#define VS_MULTI 16                     /* takes in a row more than a refresh apart: the period again */
#define VS_RESEED 3                     /* the phase started again this often and was never steady between:
                                         * the period again */
#define VS_STEADY_NS 1000000000ull      /* the refresh is steady once takes have fitted for this long */
#define VS_STALE 8                      /* refreshes with no take: nothing is being shown */
#define VS_TOLERANCE 0.0025             /* k refreshes are a frame when within this of it */
#define VS_TOLERANCE_HELD 0.005         /* and stay one within this, once followed */
#define VS_SLEW_NS 500000.0             /* the machine's phase is moved by at most this a frame */
#define VS_RATE_SAME 0.01               /* section 61: steady rates within this of each other are one rate */
#define VS_RATE_KEEP_NS 10000000000ull  /* the highest steady rate stands this long after it was last measured */
#define VS_RATE_SETTLE_NS 5000000000ull /* and is settled once it has stood this long */
#define VS_RATE_NOW_NS 500000000ull     /* and was measured this lately */

enum { WHY_FOLLOW, WHY_UNKNOWN, WHY_STALE, WHY_RATE, WHY_FREE };

static struct {
    SDL_SpinLock lock;
    Uint64 last;                /* the latest take, as it came (0: none yet) */
    double anchor;              /* the latest refresh's take, smoothed (ns) */
    double td;                  /* one refresh, smoothed (ns); 0: not known yet */
    Uint64 index;               /* refreshes counted, at the latest take */
    Uint64 steady_since;        /* the first take of the run that has fitted since (0: none) */
    int misfits, bad, multi;    /* takes in a row that did not fit; those weighed (VS_BAD); takes in a row
                                 * that fitted more than a refresh on */
    int measured;               /* the period has been measured across a run since it was last started */
    int restarts;               /* the phase has started again this often since the refresh was last steady */
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
    /* the screen's rate for its notice (section 61) */
    double rate_hz;             /* the highest steady rate of late (0: none) */
    Uint64 rate_first, rate_last;       /* when it was first measured, and last measured again */
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
    vs.measured = 0;
    return n >= VS_SEED / 2 ? sum / n : med;
}

/* the refresh is known: takes have fitted for a second, or, with the period measured before, for
 * VS_AGAIN takes since the phase last started again */
static int steady(void)
{
    if (vs.td <= 0 || !vs.steady_since)
        return 0;
    return vs.measured ? vs.run >= VS_AGAIN : vs.last - vs.steady_since >= VS_STEADY_NS;
}

/* the phase starts again at t: a new run */
static void restart(Uint64 t)
{
    vs.anchor = (double)t;
    vs.steady_since = 0;
    vs.run = 0;
    vs.multi = 0;
}

static double test_td;      /* --refresh-first: the period the filter starts with (0: none) */

void vsync_test_first(double hz)
{
    SDL_LockSpinlock(&vs.lock);
    test_td = hz > 0 ? 1e9 / hz : 0;
    vs.td = test_td;
    SDL_UnlockSpinlock(&vs.lock);
}

void vsync_reset(void)
{
    SDL_LockSpinlock(&vs.lock);
    vs.last = 0;
    vs.td = test_td;
    vs.index = vs.slot = 0;
    vs.misfits = vs.bad = vs.measured = vs.restarts = 0;
    vs.niv = 0;
    vs.rate_hz = 0;
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
                vs.misfits = vs.bad = 0;
                restart(t);
            }
        } else {
            double m = SDL_floor(((double)t - vs.anchor) / vs.td + 0.5);
            double err = (double)t - (vs.anchor + m * vs.td);
            if (m < 1 || m > VS_MISS || SDL_fabs(err) > vs.td / 4) {
                /* not where the grid says. One such (a present held up, a stall), or a few among many
                 * that fit (presents that come unevenly), change nothing: the grid goes on by its
                 * period. Three in a row, or most of a dozen: the phase starts again here. All of
                 * VS_SEED in a row: the period too. And the period too when the phase has started again
                 * VS_RESEED times with the refresh never steady between: with a period that is not the
                 * screen's, fits and misfits can take turns for good (a period of 12.5 ms and presents
                 * every 20 ms: every other one fits), so neither count in a row is ever reached */
                if (m >= 1) {
                    vs.anchor += m * vs.td;
                    vs.index += (Uint64)m;
                }
                vs.bad += 2;
                if (vs.bad >= VS_BAD) {
                    restart(t);
                    vs.bad = 0;
                    vs.restarts++;
                }
                if (++vs.misfits >= VS_SEED || (vs.restarts >= VS_RESEED && vs.niv >= VS_SEED)) {
                    vs.td = seed();
                    vs.misfits = vs.restarts = 0;
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
                if (vs.bad > 0)
                    vs.bad--;
                if (measured)
                    vs.measured = 1;
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
    if (steady()) {
        vs.restarts = 0;
        /* section 61: a steady rate higher than the one kept is the screen's from now; the same one
         * is measured again; a lower one (presents held back, or the screen slower now) takes its
         * place only once the higher has not been measured for VS_RATE_KEEP_NS */
        double hz = 1e9 / vs.td;
        if (vs.rate_hz <= 0 || hz > vs.rate_hz * (1 + VS_RATE_SAME) || t - vs.rate_last > VS_RATE_KEEP_NS) {
            vs.rate_hz = hz;
            vs.rate_first = vs.rate_last = t;
        } else if (hz >= vs.rate_hz * (1 - VS_RATE_SAME)) {
            vs.rate_hz = hz;
            vs.rate_last = t;
        }
    }
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
    if (!steady()) {
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

/* ---- the game by its own clock: each frame on a refresh of the screen ----
 *
 * The machine keeps its own 60 a second, and each frame it hands over says
 * when it is to be shown by that clock (pts). The refresh it is shown on is
 * the one before, plus the whole number of refreshes that time is on from
 * the frame before: frames go on the screen at even steps whatever small
 * difference there is in how long each took. A frame keeps that step until
 * its refresh comes more than VS_EDGE_NS before its time (it moves one
 * later: on a 60 Hz screen the frame before is shown once more) or more than
 * a refresh and VS_EDGE_NS after it (one sooner: the frame before is passed
 * over). That is once for every whole refresh the two clocks have gone
 * apart, which no pacing can do without, and nowhere else. */
#define VS_EDGE_NS 750000.0

static struct {
    unsigned frame;             /* the newest frame given a refresh (0: none) */
    Uint64 pts;
    Uint64 r;                   /* its refresh, as the grid counts them */
    struct { unsigned frame; Uint64 r; } q[4];     /* the latest few: those still waiting */
    unsigned nq;
    unsigned sooner, later;     /* steps moved one refresh sooner, one later */
    int on;                     /* the last pick went by the grid */
    double hz;
} sch;

unsigned vsync_pick(const unsigned *frame, const Uint64 *pts, int n)
{
    unsigned limit = n > 0 ? frame[n - 1] : 0;      /* no steady refresh: the newest, as it comes */
    SDL_LockSpinlock(&vs.lock);
    int on = steady();
    if (on) {
        double h = vs.td / 5 < VS_EDGE_NS ? vs.td / 5 : VS_EDGE_NS;
        limit = 0;
        for (int i = 0; i < n; i++) {
            Uint64 r = 0;
            int known = 0;
            for (unsigned j = 0; j < 4 && !known; j++)
                if (sch.q[j].frame == frame[i] && frame[i]) {
                    r = sch.q[j].r;
                    known = 1;
                }
            if (!known && frame[i] <= sch.frame)
                known = 1;                          /* an old one: due */
            if (!known) {
                /* e: how long after its time the frame's refresh comes */
                double e = 0;
                int fresh = !sch.on || !sch.frame || pts[i] <= sch.pts;
                if (!fresh) {
                    r = sch.r + (Uint64)SDL_floor((double)(pts[i] - sch.pts) / vs.td + 0.5);
                    e = vs.anchor + ((double)r - (double)vs.index) * vs.td - (double)pts[i];
                    if (SDL_fabs(e) > 8 * vs.td)
                        fresh = 1;                  /* a pause, a stall: from here again */
                }
                if (fresh) {
                    double ahead = SDL_ceil(((double)pts[i] - vs.anchor) / vs.td);
                    r = vs.index + (Uint64)(ahead > 0 ? ahead : 0);
                } else {
                    while (e < -h) {
                        r++;
                        e += vs.td;
                        sch.later++;
                    }
                    while (e >= vs.td + h && r > 0) {
                        r--;
                        e -= vs.td;
                        sch.sooner++;
                    }
                }
                sch.frame = frame[i];
                sch.pts = pts[i];
                sch.r = r;
                sch.q[sch.nq % 4].frame = frame[i];
                sch.q[sch.nq % 4].r = r;
                sch.nq++;
            }
            if (r <= vs.index)
                limit = frame[i];
        }
        sch.hz = 1e9 / vs.td;
    }
    sch.on = on;
    SDL_UnlockSpinlock(&vs.lock);
    return limit;
}

double vsync_screen_hz(Uint64 now, int *settled)
{
    SDL_LockSpinlock(&vs.lock);
    double hz = vs.rate_hz;
    Uint64 first = vs.rate_first, last = vs.rate_last;
    SDL_UnlockSpinlock(&vs.lock);
    if (settled)
        *settled = 0;
    if (hz <= 0 || (now > last && now - last > VS_RATE_KEEP_NS))
        return 0;
    if (settled)
        *settled = now >= first && now - first >= VS_RATE_SETTLE_NS && (now <= last || now - last <= VS_RATE_NOW_NS);
    return hz;
}

void vsync_rate_forget(void)
{
    SDL_LockSpinlock(&vs.lock);
    vs.rate_hz = 0;
    SDL_UnlockSpinlock(&vs.lock);
}

int vsync_pick_stats(double *hz, unsigned *sooner, unsigned *later)
{
    SDL_LockSpinlock(&vs.lock);
    int on = sch.on;
    *hz = sch.hz;
    *sooner = sch.sooner;
    *later = sch.later;
    SDL_UnlockSpinlock(&vs.lock);
    return on;
}
