/* The two-PC game's security (docs/design.md section 50): the host's join
 * code, the key exchange made with it (CPace), and the datagrams sealed
 * with the keys that come of it.
 *
 * The join code: six digits the host's game shows and the joining player
 * types. It never goes over the network. Both PCs run CPace with it
 * (draft-irtf-cfrg-cpace-21, the suite CPACE-X25519-SHA512): each makes a
 * point on the curve from the code and a random number of its own and sends
 * it, and each gets the same key from the two only when both used the same
 * code. Someone without the code learns nothing from what goes over that
 * would let them test codes away from the game, and each try of theirs is
 * one guess at the code, which the host sees.
 *
 * The sealed datagrams: XChaCha20-Poly1305 with a key for each direction,
 * the datagram's counter as the nonce. A datagram whose seal is not right,
 * or whose counter came before, is not taken.
 *
 * Monocypher (runtime/crypto, ORIGIN.md there) does the arithmetic; this
 * file is CPace's steps around it. tests/secure checks it against the
 * draft's test vectors. */
#ifndef VF4FT_SECURE_H
#define VF4FT_SECURE_H

#include <stddef.h>
#include <stdint.h>

#define SECURE_CODE_DIGITS 6
#define SECURE_SID 16           /* the joining PC's random number for a try (CPace's sid) */
#define SECURE_POINT 32         /* a point: X25519's u coordinate */
#define SECURE_ISK 64           /* CPace's key */
#define SECURE_OVER 24          /* a sealed datagram's bytes more: its counter (8) and its seal (16) */
#define SECURE_WINDOW 2048      /* counters this far below the newest taken can still come (late, in another order) */

/* n random bytes from the system (getrandom; BCryptGenRandom on Windows): 0, or -1 */
int secure_random(void *buf, size_t n);

/* a new join code: six digits, each 0 to 9 alike, and a 0. 0, or -1 (no random bytes) */
int secure_new_code(char code[SECURE_CODE_DIGITS + 1]);

/* the code as typed: its digits only (spaces and marks dropped). 0 when
 * that is six digits, else -1 */
int secure_code_from(const char *typed, char code[SECURE_CODE_DIGITS + 1]);

/* CPace's generator g from the code (prs), the channel's words (ci, both
 * PCs alike) and the try's number (sid): calculate_generator of G_X25519
 * with SHA-512 */
void cpace_generator(uint8_t g[32], const void *prs, size_t prs_n, const void *ci, size_t ci_n, const void *sid,
                     size_t sid_n);

/* the generator's last step: the first 32 bytes of the hash as a field
 * element (decodeUCoordinate, 255 bits), mapped to the curve (Elligator 2) */
void cpace_map(uint8_t g[32], const uint8_t u[32]);

/* a party's point Y = y * g (X25519); y is 32 random bytes */
void cpace_point(uint8_t Y[32], const uint8_t y[32], const uint8_t g[32]);

/* CPace's key (ISK) once the other's point has come: K = y * other, then
 * the hash of K, sid and both parties' points and words (the joining PC's
 * Ya and ada, the host's Yb and adb: the initiator-responder transcript).
 * 0, or -1 when K is the neutral element (the other's point is one of the
 * few that would give a key known beforehand: it is not taken) */
int cpace_key(uint8_t isk[SECURE_ISK], const uint8_t y[32], const uint8_t other[32], const void *sid, size_t sid_n,
              const uint8_t Ya[32], const void *ada, size_t ada_n, const uint8_t Yb[32], const void *adb,
              size_t adb_n);

/* a direction: its key, and the counter of the next datagram (out) or what
 * has come (in) */
typedef struct {
    uint8_t key[32];
    uint64_t next;
} secure_out;
typedef struct {
    uint8_t key[32];
    uint64_t top;               /* the newest counter taken */
    int any;                    /* one has been */
    uint64_t seen[SECURE_WINDOW / 64];
} secure_in;

/* both directions from CPace's key, for the host (host 1) or the joining PC
 * (host 0): what it sends and what it takes */
void secure_keys(const uint8_t isk[SECURE_ISK], int host, secure_out *out, secure_in *in);

/* both wiped (no key) */
void secure_forget(secure_out *out, secure_in *in);

/* n bytes wiped (a secret no longer needed) */
void secure_wipe(void *p, size_t n);

/* seals the datagram dg of n bytes in place: its first `clear` bytes stay as
 * they are (the seal covers them), the counter follows them, then the rest
 * encrypted, then the seal. dg has room for n + SECURE_OVER bytes. Returns
 * the sealed length */
size_t secure_seal(secure_out *out, uint8_t *dg, size_t clear, size_t n);

/* opens a sealed datagram in place: the plain datagram's length, or 0 when
 * its seal is not right or its counter came before (or is older than the
 * window). The window moves only for a datagram whose seal was right */
size_t secure_open(secure_in *in, uint8_t *dg, size_t clear, size_t n);

#endif
