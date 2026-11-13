/* The DIMM image as the builder installs it: the disc's file (VF41_S.BIN,
 * encrypted, as on the GD-ROM) and its key (dimm.key, from the cartridge's
 * PIC). The DIMM board decrypts the file into its memory when it boots;
 * this does the same, on every processor at once.
 *
 * The cipher is DES (FIPS 46-3) in ECB mode over the file read as
 * little-endian 64-bit words. Written from the standard's tables (the same
 * construction as tools/vf4build/des.py, where the byte order was found
 * against the decompilation's decrypted image). */
#include "dimm_image.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static const unsigned char IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const unsigned char P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                                    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};
static const unsigned char PC1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};
static const unsigned char PC2[48] = {14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
                                      26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
                                      51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const unsigned char SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const unsigned char S[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}};

/* bit i of the result (1 = most significant of nout) is bit table[i] of v (of nin) */
static uint64_t permute(uint64_t v, const unsigned char *table, int nout, int nin)
{
    uint64_t r = 0;
    for (int i = 0; i < nout; i++)
        r = (r << 1) | ((v >> (nin - table[i])) & 1);
    return r;
}

static uint64_t ipt[8][256], fpt[8][256];
static uint32_t sp[8][64];
static unsigned char rk[16][8];          /* round keys, decryption order, eight 6-bit pieces each */

static void des_setup(uint64_t key)
{
    unsigned char fp[64];
    for (int i = 0; i < 64; i++)
        fp[IP[i] - 1] = (unsigned char)(i + 1);
    for (int pos = 0; pos < 8; pos++)
        for (int b = 0; b < 256; b++) {
            ipt[pos][b] = permute((uint64_t)b << (56 - 8 * pos), IP, 64, 64);
            fpt[pos][b] = permute((uint64_t)b << (56 - 8 * pos), fp, 64, 64);
        }
    for (int i = 0; i < 8; i++)
        for (int x = 0; x < 64; x++) {
            unsigned s = S[i][(((x >> 4) & 2) | (x & 1)) * 16 + ((x >> 1) & 15)];
            sp[i][x] = (uint32_t)permute((uint64_t)s << (28 - 4 * i), P, 32, 32);
        }
    uint64_t k = permute(key, PC1, 56, 64);
    uint32_t c = (uint32_t)(k >> 28), d = (uint32_t)(k & 0xfffffff);
    for (int r = 0; r < 16; r++) {
        int s = SHIFTS[r];
        c = ((c << s) | (c >> (28 - s))) & 0xfffffff;
        d = ((d << s) | (d >> (28 - s))) & 0xfffffff;
        uint64_t k48 = permute(((uint64_t)c << 28) | d, PC2, 48, 56);
        for (int i = 0; i < 8; i++)
            rk[15 - r][i] = (unsigned char)((k48 >> (42 - 6 * i)) & 63);
    }
}

static uint64_t des_decrypt(uint64_t v)
{
    uint64_t t = 0;
    for (int i = 0; i < 8; i++)
        t |= ipt[i][(v >> (56 - 8 * i)) & 255];
    uint32_t l = (uint32_t)(t >> 32), r = (uint32_t)t;
    for (int n = 0; n < 16; n++) {
        const unsigned char *k = rk[n];
        uint64_t x = ((uint64_t)(r & 1) << 33) | ((uint64_t)r << 1) | (r >> 31);
        uint32_t f = sp[0][((x >> 28) & 63) ^ k[0]] | sp[1][((x >> 24) & 63) ^ k[1]] |
                     sp[2][((x >> 20) & 63) ^ k[2]] | sp[3][((x >> 16) & 63) ^ k[3]] |
                     sp[4][((x >> 12) & 63) ^ k[4]] | sp[5][((x >> 8) & 63) ^ k[5]] |
                     sp[6][((x >> 4) & 63) ^ k[6]] | sp[7][(x & 63) ^ k[7]];
        uint32_t nl = r;
        r = l ^ f;
        l = nl;
    }
    t = ((uint64_t)r << 32) | l;
    uint64_t o = 0;
    for (int i = 0; i < 8; i++)
        o |= fpt[i][(t >> (56 - 8 * i)) & 255];
    return o;
}

typedef struct job { uint8_t *p; size_t n; } job;

static void *work(void *arg)
{
    job *j = arg;
    for (size_t i = 0; i + 8 <= j->n; i += 8) {
        uint64_t v = 0;
        for (int b = 7; b >= 0; b--)
            v = (v << 8) | j->p[i + b];
        v = des_decrypt(v);
        for (int b = 0; b < 8; b++, v >>= 8)
            j->p[i + b] = (uint8_t)v;
    }
    return NULL;
}

static int cpus(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

int dimm_image_selftest(void)
{
    /* the standard's worked example: key 133457799BBCDFF1, 0123456789ABCDEF -> 85E813540F0AB405 */
    des_setup(0x133457799BBCDFF1ull);
    return des_decrypt(0x85E813540F0AB405ull) == 0x0123456789ABCDEFull ? 0 : -1;
}

int dimm_image_load(const char *path, const char *key_path, uint8_t **out, uint32_t *size)
{
    char hex[64] = {0};
    FILE *f = fopen(key_path, "r");
    if (!f || !fgets(hex, sizeof hex, f)) {
        fprintf(stderr, "vf4ft: cannot read the DIMM key %s: %s (the builder writes it: tools/make_game.py)\n",
                key_path, strerror(errno));
        if (f)
            fclose(f);
        return -1;
    }
    fclose(f);
    char *end;
    uint64_t key = strtoull(hex, &end, 16);
    if (end - hex != 16) {
        fprintf(stderr, "vf4ft: %s is not a DIMM key (16 hex digits)\n", key_path);
        return -1;
    }
    if (dimm_image_selftest()) {
        fprintf(stderr, "vf4ft: the DES self-test failed\n");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "vf4ft: cannot open the DIMM image %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x1000000 || n > 0x40000000 || (n & 7)) {
        fprintf(stderr, "vf4ft: %s is not the DIMM image (%lld bytes)\n", path, n);
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "vf4ft: cannot read %s\n", path);
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    des_setup(key);
    int nt = cpus();
    if (nt > 32) nt = 32;
    if (nt < 1) nt = 1;
    pthread_t th[32];
    job jobs[32];
    int running[32] = {0};
    size_t per = ((size_t)n / nt + 7) & ~(size_t)7;
    for (int i = 0; i < nt && per * i < (size_t)n; i++) {
        size_t a = per * i;
        jobs[i].p = buf + a;
        jobs[i].n = (size_t)n - a < per ? (size_t)n - a : per;
        running[i] = pthread_create(&th[i], NULL, work, &jobs[i]) == 0;
        if (!running[i])
            work(&jobs[i]);          /* no thread: do it here */
    }
    for (int i = 0; i < nt; i++)
        if (running[i])
            pthread_join(th[i], NULL);
    *out = buf;
    *size = (uint32_t)n;
    return 0;
}
