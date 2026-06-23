/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "flash:diff"

#include "recovery/config.h"

#if defined(RK_DIFF_UPDATE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "recovery/flash.h"
#include "flash_internal.h"
#include "recovery/storage.h"
#include "recovery/ubi.h"
#include "recovery/log.h"
#include "recovery/mtd_info.h"

/* ---- Detection helpers ---- */

int is_diff_img(const char *img_path, long long offset, long long size)
{
    char tail[DIFF_TAIL_SIZE];
    int fd;

    if (size < DIFF_TAIL_SIZE)
        return 0;

    fd = open(img_path, O_RDONLY);
    if (fd < 0)
        return -1;

    if (lseek(fd, offset + size - DIFF_TAIL_SIZE, SEEK_SET) == (off_t)-1) {
        close(fd);
        return -1;
    }

    ssize_t len = 0;
    while (len < DIFF_TAIL_SIZE) {
        ssize_t r = read(fd, tail + len, DIFF_TAIL_SIZE - len);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        len += r;
    }
    close(fd);

    tail[DIFF_TAIL_SIZE - 1] = '\0';
    if (strncmp(tail, DIFF_MAGIC, strlen(DIFF_MAGIC)) == 0)
        return 1;
    return 0;
}

bool is_mtd_ubi(const char *mtd_path)
{
    char magic[5] = {0};
    int fd = open(mtd_path, O_RDONLY);
    if (fd < 0)
        return false;
    if (read(fd, magic, 4) != 4) {
        close(fd);
        return false;
    }
    close(fd);
    return (strcmp(magic, UBI_HEAD_MAGIC) == 0);
}

bool is_ubi_image(const char *img_path, long long offset)
{
    char magic[5] = {0};
    int fd = open(img_path, O_RDONLY);
    if (fd < 0)
        return false;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
        return false;
    }
    if (read(fd, magic, 4) != 4) {
        close(fd);
        return false;
    }
    close(fd);
    return (strcmp(magic, UBI_HEAD_MAGIC) == 0);
}

/*
 * Parse DIFF tail to extract old_size.
 * DIFF tail format (80 bytes): "DIFF:name:old_size:new_size:md5sum:"
 */
long long parse_diff_old_size(const char *img_path, long long offset, long long size)
{
    char tail[DIFF_TAIL_SIZE];
    int fd;

    if (size < DIFF_TAIL_SIZE)
        return -1;

    fd = open(img_path, O_RDONLY);
    if (fd < 0)
        return -1;

    if (lseek(fd, offset + size - DIFF_TAIL_SIZE, SEEK_SET) == (off_t)-1) {
        close(fd);
        return -1;
    }

    ssize_t len = 0;
    while (len < DIFF_TAIL_SIZE) {
        ssize_t r = read(fd, tail + len, DIFF_TAIL_SIZE - len);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        len += r;
    }
    close(fd);

    tail[DIFF_TAIL_SIZE - 1] = '\0';

    /* Parse: tokens are DIFF, name, old_size, new_size, md5sum */
    const char *delim = ": ";
    char *saveptr = NULL;
    char *str = tail;

    for (int j = 0; j < 5; j++, str = NULL) {
        char *token = strtok_r(str, delim, &saveptr);
        if (!token)
            return -1;
        if (j == 2)
            return strtoll(token, NULL, 10);
    }
    return -1;
}

/* ---- MTD read to file (via storage_ops->read) ---- */

int mtd_read_to_file(struct storage_ops *ops, const char *mtd_path,
                     const char *dst_file, long long size)
{
    pr_info("mtd_read %s -> %s, size=%lld\n", mtd_path, dst_file, size);

    /* read's dst_path form dumps the range straight to a file — for MTD that's
     * a single nanddump, for block one streamed copy; no partition-sized malloc
     * and no fork-per-chunk. */
    return ops->read(ops, mtd_path, NULL, dst_file, 0, size);
}

/* ---- Get source directory for temp files ---- */

static void get_src_dir(const char *src_path, char *dir, size_t dir_len)
{
    snprintf(dir, dir_len, "%s", src_path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *(slash + 1) = '\0';
    else
        snprintf(dir, dir_len, "./");
}

/* ---- Block device diff flash ---- */

int flash_block_diff(struct upgrade_item *item, const char *image_path)
{
    char dst_file[256];
    struct stat dst_stat;

    snprintf(dst_file, sizeof(dst_file), "%s.%s", image_path, item->name);

    /*
     * Try bsdiff patch: do_patch_rkimg checks:
     * - If dst_file exists with correct MD5: reuse (power loss during write)
     * - If dst_file exists with wrong MD5: re-patch
     * - If no dst_file: normal patch
     *
     * Returns: >0 = patched size, 0 = not a diff, <0 = error
     */
    if (stat(dst_file, &dst_stat) == 0) {
        pr_info("dst_file %s exists for <%s>; do_patch_rkimg will reuse if md5 matches\n",
                dst_file, item->name);
    }

    int ret = do_patch_rkimg(image_path, item->img_offset, item->img_size,
                             item->dev_path, dst_file);
    if (ret == 0) {
        /* Not a diff image: caller should do full write */
        return 1;
    } else if (ret < 0) {
        pr_err("do_patch_rkimg failed for %s\n", item->name);
        return -1;
    }

    /* ret > 0: patched size, write dst_file to block device.
     * O_RDWR only (never O_CREAT|O_TRUNC): the target is a device node — if
     * by-name resolution handed us a missing path, O_CREAT would silently
     * create a regular file and the write would vanish (same footgun fixed in
     * storage/block_io.c). O_TRUNC is a no-op on a device anyway. */
    int fd_dest = open(item->dev_path, O_RDWR);
    if (fd_dest < 0) {
        pr_err("open %s: %s\n", item->dev_path, strerror(errno));
        return -1;
    }

    /* Seek to the partition's physical write offset before writing. OTA
     * by-name diffs use flash_offset 0, but seek explicitly so the path stays
     * correct for any absolute-offset target, matching core/flash.c block_write. */
    if (lseek(fd_dest, item->flash_offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek %s to 0x%llx: %s\n", item->dev_path,
               item->flash_offset, strerror(errno));
        close(fd_dest);
        return -1;
    }

    int fd_src = open(dst_file, O_RDONLY);
    if (fd_src < 0) {
        pr_err("open %s: %s\n", dst_file, strerror(errno));
        close(fd_dest);
        return -1;
    }

    char buf[BLOCK_WRITE_LEN];
    long long remain = ret;  /* patched size */
    while (remain > 0) {
        ssize_t chunk = remain > BLOCK_WRITE_LEN ? BLOCK_WRITE_LEN : (ssize_t)remain;
        ssize_t nread = read(fd_src, buf, chunk);
        if (nread != chunk) {
            pr_err("read dst_file: %s\n", strerror(errno));
            close(fd_src);
            close(fd_dest);
            return -1;
        }
        if (write(fd_dest, buf, chunk) != chunk) {
            pr_err("write block: %s\n", strerror(errno));
            close(fd_src);
            close(fd_dest);
            return -1;
        }
        remain -= chunk;
    }

    fsync(fd_dest);
    close(fd_dest);
    close(fd_src);

    /* Clean up patch scratch */
    sync();
    unlink(dst_file);
    sync();

    pr_info("block diff flash complete for %s\n", item->name);
    return 0;
}

/* ---- MTD UBI diff flash ---- */

int flash_mtd_ubi_diff(struct upgrade_item *item, const char *image_path)
{
    int ret = -1;
    int mtd_num = mtd_parse_dev_num(item->dev_path);
    if (mtd_num < 0) {
        pr_err("invalid MTD path: %s\n", item->dev_path);
        return -1;
    }

    char src_dir[PATH_MAX / 2];
    char old_file[PATH_MAX], dst_file[PATH_MAX];
    get_src_dir(image_path, src_dir, sizeof(src_dir));
    snprintf(old_file, sizeof(old_file), "%subi_old_%s.img", src_dir, item->name);
    snprintf(dst_file, sizeof(dst_file), "%s.%s", image_path, item->name);

    pr_info("=== UBI diff update start: %s (mtd%d) ===\n", item->name, mtd_num);

    struct ubi_ops *ubi = ubi_get_ops();
    if (!ubi) {
        pr_err("UBI support not compiled in\n");
        return -1;
    }

    /* 1. Attach UBI */
    int ubi_num = ubi->attach(mtd_num);
    if (ubi_num < 0) {
        pr_err("UBI attach failed for mtd%d\n", mtd_num);
        return -1;
    }

    /* 2. Read old squashfs from UBI volume 0 */
    long long old_size = 0;
    ret = ubi->read_volume(ubi_num, 0, old_file, &old_size);
    if (ret != 0) {
        pr_err("read old image from UBI volume failed\n");
        ubi->detach(ubi_num);
        return -1;
    }
    pr_info("old UBI volume data_bytes=%lld\n", old_size);

    /* 3. Apply bsdiff patch */
    ret = do_patch_rkimg(image_path, item->img_offset, item->img_size,
                         old_file, dst_file);
    unlink(old_file);

    if (ret < 0) {
        pr_err("do_patch_rkimg failed for %s\n", item->name);
        ubi->detach(ubi_num);
        return -1;
    }
    if (ret == 0) {
        /* Not a diff, fallback to full MTD write */
        pr_warn("not a diff for %s, fallback to full write\n", item->name);
        ubi->detach(ubi_num);
        return flash_normal(item, image_path);
    }

    /* 4. Write new image to UBI volume */
    char ubi_dev[64];
    snprintf(ubi_dev, sizeof(ubi_dev), "/dev/ubi%d_0", ubi_num);
    ret = ubi->update_volume(ubi_dev, dst_file);

    /* 5. Detach UBI */
    ubi->detach(ubi_num);

    if (ret != 0) {
        pr_err("UBI update volume failed for %s\n", item->name);
        return -1;
    }

    /* Clean up patch scratch */
    sync();
    unlink(dst_file);
    sync();

    pr_info("=== UBI diff update success: %s ===\n", item->name);
    return 0;
}

/* ---- Raw MTD diff flash ---- */

int flash_mtd_raw_diff(struct upgrade_item *item, const char *image_path)
{
    int ret;

    char src_dir[PATH_MAX / 2];
    char old_file[PATH_MAX], dst_file[PATH_MAX];
    get_src_dir(image_path, src_dir, sizeof(src_dir));
    snprintf(old_file, sizeof(old_file), "%smtd_old_%s.img", src_dir, item->name);
    snprintf(dst_file, sizeof(dst_file), "%s.%s", image_path, item->name);

    pr_info("=== Raw MTD diff update start: %s (%s) ===\n",
            item->name, item->dev_path);

    /* 1. Parse DIFF tail to get old_size */
    long long old_size = parse_diff_old_size(image_path,
                                             item->img_offset, item->img_size);
    if (old_size <= 0) {
        pr_err("parse old_size from DIFF tail failed for %s\n", item->name);
        return -1;
    }
    pr_info("DIFF tail: old_size = %lld\n", old_size);

    /* 2. Read old data from MTD */
    ret = mtd_read_to_file(item->backend, item->dev_path, old_file, old_size);
    if (ret != 0) {
        pr_err("read old image from %s failed\n", item->dev_path);
        return -1;
    }

    /* 3. Apply bsdiff patch */
    ret = do_patch_rkimg(image_path, item->img_offset, item->img_size,
                         old_file, dst_file);
    unlink(old_file);

    if (ret < 0) {
        pr_err("do_patch_rkimg failed for %s\n", item->name);
        return -1;
    }
    if (ret == 0) {
        pr_warn("not a diff for %s, fallback to full write\n", item->name);
        return flash_normal(item, image_path);
    }

    /* 4. Write new image via storage_ops. Keep it as a file slice so NAND
     * treats it as a bulk partition image rather than metadata needing RMW. */
    long long new_size = ret;
    pr_info("writing new image (%lld bytes) to %s\n", new_size, item->dev_path);

    /* write erases the covering region itself — no separate erase op. */
    if (item->backend->write(item->backend, item->dev_path, NULL,
                             dst_file, 0, item->flash_offset,
                             (long long)new_size, NULL, NULL) != 0) {
        pr_err("write %s failed\n", item->dev_path);
        return -1;
    }

    /* Clean up patch scratch */
    sync();
    unlink(dst_file);
    sync();

    pr_info("=== Raw MTD diff update success: %s ===\n", item->name);
    return 0;
}

#endif /* RK_DIFF_UPDATE */
