/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "checksum"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery/checksum.h"
#include "recovery/storage.h"

/* ---- CRC-32 (RK custom polynomial, used by IDBlock) ---- */

static const uint32_t rk_crc32_tab[256] = {
    0x00000000, 0x04c10db7, 0x09821b6e, 0x0d4316d9,
    0x130436dc, 0x17c53b6b, 0x1a862db2, 0x1e472005,
    0x26086db8, 0x22c9600f, 0x2f8a76d6, 0x2b4b7b61,
    0x350c5b64, 0x31cd56d3, 0x3c8e400a, 0x384f4dbd,
    0x4c10db70, 0x48d1d6c7, 0x4592c01e, 0x4153cda9,
    0x5f14edac, 0x5bd5e01b, 0x5696f6c2, 0x5257fb75,
    0x6a18b6c8, 0x6ed9bb7f, 0x639aada6, 0x675ba011,
    0x791c8014, 0x7ddd8da3, 0x709e9b7a, 0x745f96cd,
    0x9821b6e0, 0x9ce0bb57, 0x91a3ad8e, 0x9562a039,
    0x8b25803c, 0x8fe48d8b, 0x82a79b52, 0x866696e5,
    0xbe29db58, 0xbae8d6ef, 0xb7abc036, 0xb36acd81,
    0xad2ded84, 0xa9ece033, 0xa4aff6ea, 0xa06efb5d,
    0xd4316d90, 0xd0f06027, 0xddb376fe, 0xd9727b49,
    0xc7355b4c, 0xc3f456fb, 0xceb74022, 0xca764d95,
    0xf2390028, 0xf6f80d9f, 0xfbbb1b46, 0xff7a16f1,
    0xe13d36f4, 0xe5fc3b43, 0xe8bf2d9a, 0xec7e202d,
    0x34826077, 0x30436dc0, 0x3d007b19, 0x39c176ae,
    0x278656ab, 0x23475b1c, 0x2e044dc5, 0x2ac54072,
    0x128a0dcf, 0x164b0078, 0x1b0816a1, 0x1fc91b16,
    0x018e3b13, 0x054f36a4, 0x080c207d, 0x0ccd2dca,
    0x7892bb07, 0x7c53b6b0, 0x7110a069, 0x75d1adde,
    0x6b968ddb, 0x6f57806c, 0x621496b5, 0x66d59b02,
    0x5e9ad6bf, 0x5a5bdb08, 0x5718cdd1, 0x53d9c066,
    0x4d9ee063, 0x495fedd4, 0x441cfb0d, 0x40ddf6ba,
    0xaca3d697, 0xa862db20, 0xa521cdf9, 0xa1e0c04e,
    0xbfa7e04b, 0xbb66edfc, 0xb625fb25, 0xb2e4f692,
    0x8aabbb2f, 0x8e6ab698, 0x8329a041, 0x87e8adf6,
    0x99af8df3, 0x9d6e8044, 0x902d969d, 0x94ec9b2a,
    0xe0b30de7, 0xe4720050, 0xe9311689, 0xedf01b3e,
    0xf3b73b3b, 0xf776368c, 0xfa352055, 0xfef42de2,
    0xc6bb605f, 0xc27a6de8, 0xcf397b31, 0xcbf87686,
    0xd5bf5683, 0xd17e5b34, 0xdc3d4ded, 0xd8fc405a,
    0x6904c0ee, 0x6dc5cd59, 0x6086db80, 0x6447d637,
    0x7a00f632, 0x7ec1fb85, 0x7382ed5c, 0x7743e0eb,
    0x4f0cad56, 0x4bcda0e1, 0x468eb638, 0x424fbb8f,
    0x5c089b8a, 0x58c9963d, 0x558a80e4, 0x514b8d53,
    0x25141b9e, 0x21d51629, 0x2c9600f0, 0x28570d47,
    0x36102d42, 0x32d120f5, 0x3f92362c, 0x3b533b9b,
    0x031c7626, 0x07dd7b91, 0x0a9e6d48, 0x0e5f60ff,
    0x101840fa, 0x14d94d4d, 0x199a5b94, 0x1d5b5623,
    0xf125760e, 0xf5e47bb9, 0xf8a76d60, 0xfc6660d7,
    0xe22140d2, 0xe6e04d65, 0xeba35bbc, 0xef62560b,
    0xd72d1bb6, 0xd3ec1601, 0xdeaf00d8, 0xda6e0d6f,
    0xc4292d6a, 0xc0e820dd, 0xcdab3604, 0xc96a3bb3,
    0xbd35ad7e, 0xb9f4a0c9, 0xb4b7b610, 0xb076bba7,
    0xae319ba2, 0xaaf09615, 0xa7b380cc, 0xa3728d7b,
    0x9b3dc0c6, 0x9ffccd71, 0x92bfdba8, 0x967ed61f,
    0x8839f61a, 0x8cf8fbad, 0x81bbed74, 0x857ae0c3,
    0x5d86a099, 0x5947ad2e, 0x5404bbf7, 0x50c5b640,
    0x4e829645, 0x4a439bf2, 0x47008d2b, 0x43c1809c,
    0x7b8ecd21, 0x7f4fc096, 0x720cd64f, 0x76cddbf8,
    0x688afbfd, 0x6c4bf64a, 0x6108e093, 0x65c9ed24,
    0x11967be9, 0x1557765e, 0x18146087, 0x1cd56d30,
    0x02924d35, 0x06534082, 0x0b10565b, 0x0fd15bec,
    0x379e1651, 0x335f1be6, 0x3e1c0d3f, 0x3add0088,
    0x249a208d, 0x205b2d3a, 0x2d183be3, 0x29d93654,
    0xc5a71679, 0xc1661bce, 0xcc250d17, 0xc8e400a0,
    0xd6a320a5, 0xd2622d12, 0xdf213bcb, 0xdbe0367c,
    0xe3af7bc1, 0xe76e7676, 0xea2d60af, 0xeeec6d18,
    0xf0ab4d1d, 0xf46a40aa, 0xf9295673, 0xfde85bc4,
    0x89b7cd09, 0x8d76c0be, 0x8035d667, 0x84f4dbd0,
    0x9ab3fbd5, 0x9e72f662, 0x9331e0bb, 0x97f0ed0c,
    0xafbfa0b1, 0xab7ead06, 0xa63dbbdf, 0xa2fcb668,
    0xbcbb966d, 0xb87a9bda, 0xb5398d03, 0xb1f880b4,
};

uint32_t rk_crc32(const uint8_t *data, uint32_t size, uint32_t prev)
{
    uint32_t crc = prev;
    for (uint32_t i = 0; i < size; i++)
        crc = (crc << 8) ^ rk_crc32_tab[(crc >> 24) ^ data[i]];
    return crc;
}

/* ---- CRC-16 CCITT ---- */

static void crc16_build_table(uint16_t poly, uint16_t *table)
{
    for (unsigned i = 0; i < 256; i++) {
        uint16_t data = (uint16_t)(i << 8);
        uint16_t accum = 0;
        for (unsigned j = 0; j < 8; j++) {
            if ((data ^ accum) & 0x8000)
                accum = (accum << 1) ^ poly;
            else
                accum <<= 1;
            data <<= 1;
        }
        table[i] = accum;
    }
}

uint16_t rk_crc16(const uint8_t *data, uint32_t size)
{
    uint16_t crc = 0;
    uint16_t table[256];
    crc16_build_table(0x1021, table);
    for (uint32_t i = 0; i < size; i++)
        crc = (crc << 8) ^ table[(crc >> 8) ^ data[i]];
    return crc;
}

/* ---- CRC-CCITT (for UID check) ---- */

uint16_t rk_crc_ccitt(const uint8_t *data, uint32_t size)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t ch = data[i];
        for (unsigned bit = 0x80; bit != 0; bit >>= 1) {
            if ((crc & 0x8000) != 0) {
                crc <<= 1;
                crc ^= 0x1021;
            } else {
                crc <<= 1;
            }
            if (ch & bit)
                crc ^= 0x1021;
        }
    }
    return crc;
}

/* ---- RC4 (RK fixed key, for IDBlock encryption) ---- */

void rk_rc4(uint8_t *buf, uint16_t len)
{
    uint8_t S[256], K[256], temp;
    uint16_t i, j, t, x;
    static const uint8_t key[16] = {
        124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17
    };

    j = 0;
    for (i = 0; i < 256; i++) {
        S[i] = (uint8_t)i;
        K[i] = key[j & 0x0f];
        j++;
    }

    j = 0;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + K[i]) % 256;
        temp = S[i];
        S[i] = S[j];
        S[j] = temp;
    }

    i = j = 0;
    for (x = 0; x < len; x++) {
        i = (i + 1) % 256;
        j = (j + S[i]) % 256;
        temp = S[i];
        S[i] = S[j];
        S[j] = temp;
        t = (S[i] + (S[j] % 256)) % 256;
        buf[x] ^= S[t];
    }
}

/* ---- MD5 region helpers ---- */

bool md5_compute_file(const char *path, long long offset, long long size,
                      uint8_t out[16])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (offset > 0 && lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek %s to %lld failed\n", path, offset);
        close(fd);
        return false;
    }

    MD5_CTX ctx;
    MD5_Init(&ctx);

    uint8_t buf[4096];
    long long remain = size;
    while (remain > 0) {
        size_t chunk = remain > (long long)sizeof(buf) ? sizeof(buf)
                                                       : (size_t)remain;
        ssize_t n = read(fd, buf, chunk);
        if (n <= 0) {
            pr_err("read %s short: remain=%lld\n", path, remain);
            close(fd);
            return false;
        }
        MD5_Update(&ctx, buf, (unsigned long)n);
        remain -= n;
    }
    close(fd);

    MD5_Final(out, &ctx);
    return true;
}

bool md5_compute_partition(struct storage_ops *ops, const char *dev_path,
                           long long offset, long long size, uint8_t out[16])
{
    if (!ops || !ops->read || !dev_path) {
        pr_err("md5_compute_partition: invalid args\n");
        return false;
    }

    /*
     * MTD reads fork a nanddump per call, so a chunked read over a 512MB
     * partition would fork hundreds of thousands of times. Dump the whole
     * range to a tmpfile in one read (a single nanddump via read's dst_path
     * form), MD5 the file, unlink. Block devices read cheaply in-process, so
     * they stream below in 4KB chunks with no partition-sized temp file.
     */
    if (storage_type_is_mtd(ops->type)) {
        char tmpfile[] = "/tmp/rk_md5_part.XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd < 0) {
            pr_err("md5_compute_partition: mkstemp: %s\n", strerror(errno));
            return false;
        }
        close(fd);

        bool ok = false;
        if (ops->read(ops, dev_path, NULL, tmpfile, offset, size) == 0)
            ok = md5_compute_file(tmpfile, 0, size, out);
        else
            pr_err("md5: read %s to file failed\n", dev_path);

        unlink(tmpfile);
        return ok;
    }

    MD5_CTX ctx;
    MD5_Init(&ctx);

    uint8_t buf[4096];
    long long cur = offset;
    long long remain = size;
    while (remain > 0) {
        size_t chunk = remain > (long long)sizeof(buf) ? sizeof(buf)
                                                       : (size_t)remain;
        if (ops->read(ops, dev_path, buf, NULL, (off_t)cur,
                      (long long)chunk) != 0) {
            pr_err("ops->read %s @%lld failed\n", dev_path, cur);
            return false;
        }
        MD5_Update(&ctx, buf, (unsigned long)chunk);
        cur += chunk;
        remain -= chunk;
    }
    MD5_Final(out, &ctx);
    return true;
}

bool md5_verify_hex(const uint8_t expected_hex[32], const uint8_t actual[16])
{
    for (int i = 0; i < 16; i++) {
        uint8_t hi = expected_hex[i * 2];
        uint8_t lo = expected_hex[i * 2 + 1];
        uint8_t expected = 0;

        if (hi >= '0' && hi <= '9')      expected  = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') expected  = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') expected  = (hi - 'A' + 10) << 4;
        else return false;

        if (lo >= '0' && lo <= '9')      expected |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') expected |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') expected |= (lo - 'A' + 10);
        else return false;

        if (actual[i] != expected) {
            pr_err("MD5 mismatch at byte %d\n", i);
            return false;
        }
    }
    return true;
}

bool md5_compare_files(const char *dest, long long dest_off,
                       const char *src, long long src_off, long long size)
{
    uint8_t d_digest[16], s_digest[16];

    if (!md5_compute_file(dest, dest_off, size, d_digest))
        return false;
    if (!md5_compute_file(src, src_off, size, s_digest))
        return false;

    if (memcmp(d_digest, s_digest, 16) != 0) {
        pr_err("MD5 mismatch: %s vs %s\n", dest, src);
        return false;
    }
    return true;
}

bool md5_compare_partition_file(struct storage_ops *ops, const char *dest_dev,
                                long long dest_off, const char *src_path,
                                long long src_off, long long size)
{
    uint8_t d_digest[16], s_digest[16];

    if (!md5_compute_partition(ops, dest_dev, dest_off, size, d_digest))
        return false;
    if (!md5_compute_file(src_path, src_off, size, s_digest))
        return false;

    if (memcmp(d_digest, s_digest, 16) != 0) {
        pr_err("MD5 mismatch: %s vs %s\n", dest_dev, src_path);
        return false;
    }
    return true;
}

