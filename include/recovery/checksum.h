/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_CHECKSUM_H
#define RK_RECOVERY_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct storage_ops;

/* ---- CRC variants (different polynomials; not interchangeable) ---- */
uint32_t rk_crc32(const uint8_t *data, uint32_t size, uint32_t prev);
uint16_t rk_crc16(const uint8_t *data, uint32_t size);
uint16_t rk_crc_ccitt(const uint8_t *data, uint32_t size);

/* ---- RC4 (RK fixed key, used for IDBlock encryption) ---- */
void rk_rc4(uint8_t *buf, uint16_t len);

/* ---- MD5 algorithm (Solar Designer public-domain impl) ---- */
typedef unsigned int MD5_u32plus;

typedef struct {
    MD5_u32plus lo, hi;
    MD5_u32plus a, b, c, d;
    unsigned char buffer[64];
    MD5_u32plus block[16];
} MD5_CTX;

void MD5_Init(MD5_CTX *ctx);
void MD5_Update(MD5_CTX *ctx, const void *data, unsigned long size);
void MD5_Final(unsigned char *result, MD5_CTX *ctx);

/* ---- MD5 region helpers ---- */

/*
 * Compute MD5 over [offset, offset+size) bytes of a regular file.
 * Returns true on success; digest written to out[16].
 */
bool md5_compute_file(const char *path, long long offset, long long size,
                      uint8_t out[16]);

/*
 * Compute MD5 over a partition region via storage ops. The backend is
 * responsible for bad-block skipping (the MTD backend's nanddump --bb=skipbad
 * does this transparently).
 */
bool md5_compute_partition(struct storage_ops *ops, const char *dev_path,
                           long long offset, long long size, uint8_t out[16]);

/*
 * Verify a computed digest against a 32-char ASCII hex representation
 * (lowercase). expected_hex need not be nul-terminated.
 */
bool md5_verify_hex(const uint8_t expected_hex[32], const uint8_t actual[16]);

/*
 * Compare two regular files by MD5 over matching regions.
 * Returns true if the digests are identical.
 */
bool md5_compare_files(const char *dest, long long dest_off,
                       const char *src, long long src_off, long long size);

/*
 * Compare MD5 of a partition region (via ops) against a regular file region.
 * Intended for post-flash verification where dest is a block/MTD device and
 * src is the source image on disk.
 */
bool md5_compare_partition_file(struct storage_ops *ops, const char *dest_dev,
                                long long dest_off, const char *src_path,
                                long long src_off, long long size);

#endif /* RK_RECOVERY_CHECKSUM_H */
