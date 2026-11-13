"""DES (FIPS 46-3), decryption only, written from the standard's tables.
Table-driven: the S-boxes are merged with the P permutation, and the
initial and final permutations are done a byte at a time."""

IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
      62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
      57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
      61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
     2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
       10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
       63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
       14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4]
PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
       26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
       51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
     0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
     15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
     3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
     13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
     13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
     1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
     13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
     3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
     14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
     11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
     10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
     4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
     13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
     6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
     1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
     2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _permute(v, table, nin):
    """Bit i of the output (1 = most significant) is bit table[i] of the input."""
    out = 0
    for t in table:
        out = (out << 1) | ((v >> (nin - t)) & 1)
    return out


def _byte_tables(table):
    """For a 64-bit permutation: per input byte position, the output bits each byte value gives."""
    out = []
    for pos in range(8):
        row = []
        for b in range(256):
            row.append(_permute(b << (56 - 8 * pos), table, 64))
        out.append(row)
    return out


FP = [IP.index(i) + 1 for i in range(1, 65)]
IPT = _byte_tables(IP)
FPT = _byte_tables(FP)
# S-box i's 4 bits, put through P: SP[i][six input bits]
SP = []
for i in range(8):
    row = []
    for x in range(64):
        s = S[i][(((x >> 4) & 2) | (x & 1)) * 16 + ((x >> 1) & 15)]
        row.append(_permute(s << (28 - 4 * i), P, 32))
    SP.append(row)


def subkeys(key):
    """The 16 round keys of an 8-byte key, each as eight 6-bit pieces."""
    k = _permute(int.from_bytes(key, "big"), PC1, 64)
    c, d = k >> 28, k & 0xfffffff
    out = []
    for s in SHIFTS:
        c = ((c << s) | (c >> (28 - s))) & 0xfffffff
        d = ((d << s) | (d >> (28 - s))) & 0xfffffff
        k48 = _permute((c << 28) | d, PC2, 56)
        out.append([(k48 >> (42 - 6 * i)) & 63 for i in range(8)])
    return out


def decrypt(data, keys):
    """Decrypts whole 8-byte blocks of `data` (ECB) with subkeys(key)."""
    rk = keys[::-1]
    sp0, sp1, sp2, sp3, sp4, sp5, sp6, sp7 = SP
    ip, fp = IPT, FPT
    out = bytearray(len(data))
    mv = memoryview(data)
    for o in range(0, len(data) - 7, 8):
        b = mv[o:o + 8]
        v = (ip[0][b[0]] | ip[1][b[1]] | ip[2][b[2]] | ip[3][b[3]] |
             ip[4][b[4]] | ip[5][b[5]] | ip[6][b[6]] | ip[7][b[7]])
        L, R = v >> 32, v & 0xffffffff
        for k in rk:
            x = ((R & 1) << 33) | (R << 1) | (R >> 31)
            f = (sp0[((x >> 28) & 63) ^ k[0]] | sp1[((x >> 24) & 63) ^ k[1]] |
                 sp2[((x >> 20) & 63) ^ k[2]] | sp3[((x >> 16) & 63) ^ k[3]] |
                 sp4[((x >> 12) & 63) ^ k[4]] | sp5[((x >> 8) & 63) ^ k[5]] |
                 sp6[((x >> 4) & 63) ^ k[6]] | sp7[(x & 63) ^ k[7]])
            L, R = R, L ^ f
        v = (R << 32) | L
        out[o:o + 8] = (fp[0][v >> 56] | fp[1][(v >> 48) & 255] | fp[2][(v >> 40) & 255] |
                        fp[3][(v >> 32) & 255] | fp[4][(v >> 24) & 255] | fp[5][(v >> 16) & 255] |
                        fp[6][(v >> 8) & 255] | fp[7][v & 255]).to_bytes(8, "big")
    return bytes(out)


def _selftest():
    # the classic worked example: key 133457799BBCDFF1, 0123456789ABCDEF -> 85E813540F0AB405
    k = subkeys(bytes.fromhex("133457799BBCDFF1"))
    assert decrypt(bytes.fromhex("85E813540F0AB405"), k) == bytes.fromhex("0123456789ABCDEF")


_selftest()
