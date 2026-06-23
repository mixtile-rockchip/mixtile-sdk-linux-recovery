/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "flash"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "recovery/flash.h"
#include "flash_internal.h"
#include "recovery/storage.h"
#include "recovery/device.h"
#include "recovery/log.h"

/* ---- Block device write ---- */

/*
 * src_path: image file; src_offset/size: which bytes to copy.
 * dest_path: target device (by-name softlink or bare /dev/mmcblk0 / /dev/sda).
 * dest_offset: where to write inside dest_path. For by-name softlinks this
 * is 0 (the softlink points at the partition's own start). For bare-device
 * full-flash mode it's image item.flash_offset × SECTOR_SIZE.
 *
 * Open dest O_WRONLY without O_CREAT|O_TRUNC — these flags would corrupt a
 * raw block device (O_TRUNC fails silently on most block devs but O_CREAT
 * would create a regular file if the device node is missing).
 */
static int block_write(const char *src_path, long long src_offset,
                       long long size, const char *dest_path,
                       long long dest_offset)
{
    pr_info("block_write src=%s @0x%llx size=0x%llx -> dest=%s @0x%llx\n",
            src_path, src_offset, size, dest_path, dest_offset);

    int fd_src = open(src_path, O_RDONLY);
    if (fd_src < 0) {
        pr_err("open %s: %s\n", src_path, strerror(errno));
        return -1;
    }

    if (lseek(fd_src, src_offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek src: %s\n", strerror(errno));
        close(fd_src);
        return -1;
    }

    int fd_dest = open(dest_path, O_WRONLY);
    if (fd_dest < 0) {
        pr_err("open %s: %s\n", dest_path, strerror(errno));
        close(fd_src);
        return -1;
    }
    if (lseek(fd_dest, dest_offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek dest to 0x%llx: %s\n",
               dest_offset, strerror(errno));
        close(fd_src);
        close(fd_dest);
        return -1;
    }

    char buf[BLOCK_WRITE_LEN];
    long long remain = size;

    while (remain > 0) {
        ssize_t chunk = remain > BLOCK_WRITE_LEN ? BLOCK_WRITE_LEN : (ssize_t)remain;
        ssize_t nread = read(fd_src, buf, chunk);
        if (nread != chunk) {
            pr_err("read error: %s\n", strerror(errno));
            close(fd_src);
            close(fd_dest);
            return -1;
        }
        if (write(fd_dest, buf, chunk) != chunk) {
            pr_err("write error: %s\n", strerror(errno));
            close(fd_src);
            close(fd_dest);
            return -1;
        }
        remain -= chunk;
    }

    fsync(fd_dest);
    close(fd_dest);
    close(fd_src);
    return 0;
}

/* ---- MTD write via storage_ops ---- */

static int mtd_write_full(struct storage_ops *ops, const char *src_path,
                          long long src_offset, long long size,
                          const char *dest_path, long long dest_offset)
{
    pr_info("mtd_write src=%s @0x%llx size=0x%llx -> %s @0x%llx\n",
            src_path, src_offset, size, dest_path, dest_offset);

    /* Stream straight from the image file — the backend's write op reads the
     * slice itself (nandwrite --input-skip on NAND, pread loop on NOR), so no
     * whole-partition buffer is allocated here. Erase is owned by write (it
     * erases exactly the covering region), so there is no separate erase. */
    return ops->write(ops, dest_path, NULL, src_path, (off_t)src_offset,
                      (off_t)dest_offset, size, NULL, NULL);
}

/* ---- Main flash dispatcher ---- */

int flash_normal(struct upgrade_item *item, const char *image_path)
{
    int ret = 0;

    pr_info("flash_normal: %s -> %s\n", item->name, item->dev_path);

    /* Graceful skip when partition is not present on this device.
     * build_upgrade_list filters these out already; double-check here. */
    if (!item->backend) {
        pr_info("flash_normal: no backend for %s, skip\n", item->name);
        return 0;
    }

    if (storage_type_is_mtd(item->backend->type)) {
        /* MTD path */
#if defined(RK_DIFF_UPDATE)
        /* Self-detect diff image via DIFF magic in 80-byte tail */
        if (is_diff_img(image_path, item->img_offset, item->img_size) == 1) {
            if (is_mtd_ubi(item->dev_path)) {
                pr_info("diff image for UBI partition %s\n", item->name);
                return flash_mtd_ubi_diff(item, image_path);
            } else {
                pr_info("diff image for raw MTD partition %s\n", item->name);
                return flash_mtd_raw_diff(item, image_path);
            }
        }
#endif
        /* Full MTD write */
        ret = mtd_write_full(item->backend, image_path,
                             item->img_offset, item->img_size,
                             item->dev_path, item->flash_offset);
    } else {
        /* Block device path (eMMC / UFS) */
        char dst_file[256];
        snprintf(dst_file, sizeof(dst_file), "%s.%s", image_path, item->name);

#if defined(RK_DIFF_UPDATE)
        if (is_diff_img(image_path, item->img_offset, item->img_size) == 1) {
            ret = flash_block_diff(item, image_path);
            if (ret == 0)
                return 0;
            if (ret < 0)
                return ret;
            /* ret > 0: not actually a diff, fall through to full write */
        }
#endif

        /* Full block write */
        ret = block_write(image_path, item->img_offset, item->img_size,
                          item->dev_path, item->flash_offset);
        if (ret != 0)
            return ret;

        /* Clean up any leftover diff scratch from a prior interrupted run. */
        sync();
        unlink(dst_file);
        sync();
    }

    return ret;
}
