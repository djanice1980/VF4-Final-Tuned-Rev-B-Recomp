/* The DIMM image from the disc's encrypted file and its key (dimm_image.c). */
#ifndef VF4FT_DIMM_IMAGE_H
#define VF4FT_DIMM_IMAGE_H

#include <stdint.h>

/* Reads `path` (VF41_S.BIN as on the GD-ROM) into memory and decrypts it
 * with the key in `key_path` (16 hex digits, written by the builder).
 * 0 with *out (malloc'd) and *size, or -1 after saying why on stderr. */
int dimm_image_load(const char *path, const char *key_path, uint8_t **out, uint32_t *size);

/* 0 when DES decrypts the standard's worked example right */
int dimm_image_selftest(void);

#endif
