/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2003-2005 Colin Percival
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "patch"

#include "recovery/config.h"

#if defined(RK_DIFF_UPDATE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <bzlib.h>

#include "recovery/checksum.h"
#include "recovery/log.h"
#include "flash_internal.h"

/* dst is a regular file here; caller holds md5sum as 32-char ASCII hex. */
static bool patch_verify_md5(const char *dst_file, const char *md5sum_hex,
                             long long size)
{
    uint8_t digest[16];
    if (!md5_compute_file(dst_file, 0, size, digest))
        return false;
    return md5_verify_hex((const uint8_t *)md5sum_hex, digest);
}

#define TAIL_SIZE   80
#define TID_HEAD    0
#define TID_NAME    1
#define TID_OLD_SIZE 2
#define TID_NEW_SIZE 3
#define TID_MD5SUM  4
#define TOKENS      5
#define MAGIC_TAIL  "DIFF"
#define MD5SUM_LEN  32

static off_t offtin(unsigned char *buf)
{
    off_t y;

    y = buf[7] & 0x7F;
    y = y * 256; y += buf[6];
    y = y * 256; y += buf[5];
    y = y * 256; y += buf[4];
    y = y * 256; y += buf[3];
    y = y * 256; y += buf[2];
    y = y * 256; y += buf[1];
    y = y * 256; y += buf[0];

    if (buf[7] & 0x80)
        y = -y;

    return y;
}

/*
 * Apply a bsdiff patch from an RK image to produce a new file.
 *
 * @img      Path to the firmware image
 * @offset   Offset of the diff data within the image
 * @size     Size of the diff data (including DIFF tail)
 * @blk_dev  Path to the old image (block device or file)
 * @dst_file Output path for the patched file
 *
 * Returns:
 *   >0  Patched file size (success, is a diff image)
 *    0  Not a diff image (caller should do full write)
 *   <0  Error
 */
int do_patch_rkimg(const char *img, long long offset, long long size,
                   const char *blk_dev, const char *dst_file)
{
    int fd_img;
    const char *split = ": ";
    char tail[TAIL_SIZE];
    ssize_t len, ret;
    ssize_t oldsize, newsize;
    char *saveptr, *str, *name, *md5sum, *token[TOKENS];
    int j;

    FILE *f = NULL, *cpf = NULL, *dpf = NULL, *epf = NULL;
    BZFILE *cpfbz2 = NULL, *dpfbz2 = NULL, *epfbz2 = NULL;
    int cbz2err, dbz2err, ebz2err;
    int fd = -1;
    ssize_t bzctrllen, bzdatalen;
    unsigned char header[32], buf[8];
    unsigned char *old = NULL, *new_ptr = NULL;
    off_t oldpos, newpos;
    off_t ctrl[3];
    off_t lenread;
    off_t i;
    struct stat dst_stat;

    /* Read the DIFF tail from end of partition data */
    if ((fd_img = open(img, O_RDONLY, 0)) < 0) {
        pr_err("open %s failed\n", img);
        return -1;
    }
    if (lseek(fd_img, offset + size - TAIL_SIZE, SEEK_SET) < 0) {
        pr_err("lseek %s to %lld failed: %s\n", img,
               offset + size - TAIL_SIZE, strerror(errno));
        close(fd_img);
        return -1;
    }
    len = 0;
    while (len != TAIL_SIZE) {
        ret = read(fd_img, tail + len, TAIL_SIZE - len);
        if (ret < 0) {
            pr_err("read %s tail failed\n", img);
            close(fd_img);
            return -1;
        }
        len += ret;
    }
    close(fd_img);

    /* Parse the tail: "DIFF:name:old_size:new_size:md5sum:" */
    tail[TAIL_SIZE - 1] = '\0';
    for (j = 0, str = tail; j < TOKENS; j++, str = NULL) {
        token[j] = strtok_r(str, split, &saveptr);
        if (token[j] == NULL)
            break;
    }
    name = token[TID_NAME];
    md5sum = token[TID_MD5SUM];

    /* Recovery from unexpected reboot: if dst already has correct md5 */
    if (stat(dst_file, &dst_stat) == 0 &&
        patch_verify_md5(dst_file, md5sum, dst_stat.st_size)) {
        pr_info("recovery from unexpected reboot: %s already patched\n",
                dst_file);
        return dst_stat.st_size;
    }

    /* Check tail magic — return 0 if not a diff image */
    if (j == 0 || strncmp(MAGIC_TAIL, token[TID_HEAD],
                          strlen(MAGIC_TAIL)) != 0) {
        pr_dbg("not a diff image\n");
        return 0;
    }

    pr_info("diff image detected, patching %s...\n", name ? name : "?");

    if (j != TOKENS ||
        (oldsize = strtol(token[TID_OLD_SIZE], &saveptr, 10)) == 0 ||
        (errno == ERANGE &&
         (oldsize == LONG_MAX || oldsize == LONG_MIN)) ||
        saveptr == token[TID_OLD_SIZE] ||
        (newsize = strtol(token[TID_NEW_SIZE], &saveptr, 10)) == 0 ||
        (errno == ERANGE &&
         (newsize == LONG_MAX || newsize == LONG_MIN)) ||
        saveptr == token[TID_NEW_SIZE] ||
        strlen(token[TID_MD5SUM]) != MD5SUM_LEN) {
        pr_err("bad DIFF tail header\n");
        return -1;
    }

    /* Open patch file and read BSDIFF40 header */
    if ((f = fopen(img, "r")) == NULL) {
        pr_err("fopen %s failed\n", img);
        return -1;
    }
    if (fseeko(f, offset, SEEK_SET)) {
        pr_err("fseeko %s failed\n", img);
        fclose(f);
        return -1;
    }
    if (fread(header, 1, 32, f) < 32) {
        pr_err("read BSDIFF header failed\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    f = NULL;

    if (memcmp(header, "BSDIFF40", 8) != 0) {
        pr_err("bad BSDIFF magic\n");
        return -1;
    }

    bzctrllen = offtin(header + 8);
    bzdatalen = offtin(header + 16);
    newsize = offtin(header + 24);
    if (bzctrllen < 0 || bzdatalen < 0 || newsize <= 0) {
        pr_err("corrupt BSDIFF header lengths\n");
        return -1;
    }

    /* Open three bzip2 streams: control, diff, extra */
    if ((cpf = fopen(img, "r")) == NULL)
        return -1;
    if (fseeko(cpf, offset + 32, SEEK_SET)) {
        pr_err("fseeko control failed\n");
        goto cleanup;
    }
    if ((cpfbz2 = BZ2_bzReadOpen(&cbz2err, cpf, 0, 0, NULL, 0)) == NULL) {
        pr_err("BZ2_bzReadOpen control failed (%d)\n", cbz2err);
        goto cleanup;
    }

    if ((dpf = fopen(img, "r")) == NULL)
        goto cleanup;
    if (fseeko(dpf, offset + 32 + bzctrllen, SEEK_SET)) {
        pr_err("fseeko diff failed\n");
        goto cleanup;
    }
    if ((dpfbz2 = BZ2_bzReadOpen(&dbz2err, dpf, 0, 0, NULL, 0)) == NULL) {
        pr_err("BZ2_bzReadOpen diff failed (%d)\n", dbz2err);
        goto cleanup;
    }

    if ((epf = fopen(img, "r")) == NULL)
        goto cleanup;
    if (fseeko(epf, offset + 32 + bzctrllen + bzdatalen, SEEK_SET)) {
        pr_err("fseeko extra failed\n");
        goto cleanup;
    }
    if ((epfbz2 = BZ2_bzReadOpen(&ebz2err, epf, 0, 0, NULL, 0)) == NULL) {
        pr_err("BZ2_bzReadOpen extra failed (%d)\n", ebz2err);
        goto cleanup;
    }

    /* mmap old file (read-only) */
    if ((fd = open(blk_dev, O_RDONLY, 0)) < 0 ||
        (old = mmap(NULL, oldsize, PROT_READ,
                    MAP_SHARED | MAP_POPULATE, fd, 0)) == MAP_FAILED) {
        pr_err("mmap old %s failed\n", blk_dev);
        old = NULL;
        goto cleanup;
    }
    close(fd);
    fd = -1;

    /* mmap new file (read-write) */
    if ((fd = open(dst_file, O_CREAT | O_TRUNC | O_RDWR, 0666)) < 0 ||
        lseek(fd, newsize - 1, SEEK_SET) != newsize - 1 ||
        write(fd, "E", 1) != 1 ||
        lseek(fd, 0, SEEK_SET) != 0 ||
        (new_ptr = mmap(NULL, newsize, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0)) == MAP_FAILED) {
        pr_err("mmap new %s failed\n", dst_file);
        new_ptr = NULL;
        goto cleanup;
    }
    close(fd);
    fd = -1;

    /* Apply the bsdiff patch */
    oldpos = 0;
    newpos = 0;
    while (newpos < newsize) {
        /* Read control triple (x, y, z) */
        for (i = 0; i <= 2; i++) {
            lenread = BZ2_bzRead(&cbz2err, cpfbz2, buf, 8);
            if (lenread < 8 ||
                (cbz2err != BZ_OK && cbz2err != BZ_STREAM_END)) {
                pr_err("corrupt patch: control read\n");
                goto cleanup;
            }
            ctrl[i] = offtin(buf);
        }

        if (newpos + ctrl[0] > newsize) {
            pr_err("corrupt patch: ctrl[0] overflow\n");
            goto cleanup;
        }

        /* Read diff block and add old data */
        lenread = BZ2_bzRead(&dbz2err, dpfbz2,
                             new_ptr + newpos, ctrl[0]);
        if (lenread < ctrl[0] ||
            (dbz2err != BZ_OK && dbz2err != BZ_STREAM_END)) {
            pr_err("corrupt patch: diff read\n");
            goto cleanup;
        }

        for (i = 0; i < ctrl[0]; i++)
            if (oldpos + i >= 0 && oldpos + i < oldsize)
                new_ptr[newpos + i] += old[oldpos + i];

        newpos += ctrl[0];
        oldpos += ctrl[0];

        if (newpos + ctrl[1] > newsize) {
            pr_err("corrupt patch: ctrl[1] overflow\n");
            goto cleanup;
        }

        /* Read extra block */
        lenread = BZ2_bzRead(&ebz2err, epfbz2,
                             new_ptr + newpos, ctrl[1]);
        if (lenread < ctrl[1] ||
            (ebz2err != BZ_OK && ebz2err != BZ_STREAM_END)) {
            pr_err("corrupt patch: extra read\n");
            goto cleanup;
        }

        newpos += ctrl[1];
        oldpos += ctrl[2];
    }

    /* Cleanup bzip2 */
    BZ2_bzReadClose(&cbz2err, cpfbz2);
    BZ2_bzReadClose(&dbz2err, dpfbz2);
    BZ2_bzReadClose(&ebz2err, epfbz2);
    fclose(cpf);
    fclose(dpf);
    fclose(epf);

    munmap(new_ptr, newsize);
    munmap(old, oldsize);
    sync();

    /* Verify MD5 */
    if (!patch_verify_md5(dst_file, md5sum, newsize)) {
        pr_err("MD5 verification failed after patch\n");
        return -1;
    }

    pr_info("diff patch applied: %s, size: %zd\n",
            name ? name : "?", (size_t)newsize);
    return newsize;

cleanup:
    if (new_ptr != NULL && new_ptr != MAP_FAILED)
        munmap(new_ptr, newsize);
    if (old != NULL && old != MAP_FAILED)
        munmap(old, oldsize);
    if (fd >= 0)
        close(fd);
    if (cpf != NULL)
        fclose(cpf);
    if (dpf != NULL)
        fclose(dpf);
    if (epf != NULL)
        fclose(epf);
    return -1;
}

#endif /* RK_DIFF_UPDATE */
