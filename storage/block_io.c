/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "storage:block"

#include "recovery/config.h"

#if defined(RK_STORAGE_EMMC) || defined(RK_STORAGE_UFS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>     /* BLKGETSIZE64, BLKSSZGET */

#include "recovery/storage.h"
#include "recovery/device.h"
#include "recovery/log.h"

/* ===== Shared access method (all block media) ===== */

/*
 * Read from dev_path[offset..]. Destination is EITHER an in-memory buffer (buf)
 * OR a file (dst_path — spooled through a stack buffer; MD5/diff bulk reads,
 * the path that used to need a separate read_to_file op).
 */
static int block_read(struct storage_ops *ops, const char *dev_path,
                      void *buf, const char *dst_path, off_t offset, long long len)
{
    (void)ops;
    if ((!buf == !dst_path) || len <= 0 || offset < 0) {
        pr_err("block_read: invalid args (offset=%lld len=%lld)\n",
               (long long)offset, len);
        return -1;
    }

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }

    int dst_fd = -1;
    if (dst_path) {
        dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            pr_err("open %s: %s\n", dst_path, strerror(errno));
            close(fd);
            return -1;
        }
    }

    char iobuf[BLOCK_WRITE_LEN];
    char *mp = buf;
    long long remain = len;
    int ret = -1;
    while (remain > 0) {
        size_t chunk = remain > (long long)sizeof(iobuf) ?
                       sizeof(iobuf) : (size_t)remain;
        ssize_t n = read(fd, dst_path ? (void *)iobuf : (void *)mp, chunk);
        if (n <= 0) {
            pr_err("block_read %s: short %zd: %s\n", dev_path, n, strerror(errno));
            goto out;
        }
        if (dst_path) {
            ssize_t w = write(dst_fd, iobuf, (size_t)n);
            if (w != n) {
                pr_err("block_read: dst write %zd/%zd: %s\n",
                       w, n, strerror(errno));
                goto out;
            }
        } else {
            mp += n;
        }
        remain -= n;
    }
    ret = 0;

out:
    if (dst_fd >= 0) {
        if (ret == 0)
            fsync(dst_fd);
        close(dst_fd);
        if (ret != 0)
            unlink(dst_path);
    }
    close(fd);
    return ret;
}

/*
 * Write to dev_path[offset..]. Source is EITHER an in-memory buffer (buf) OR a
 * file slice (src_path[src_off..], streamed in BLOCK_WRITE_LEN chunks — the
 * large-partition path, O(1) RAM). The existing device node is opened O_RDWR
 * and written at the requested offset; no create/truncate flags are used.
 */
static int block_write(struct storage_ops *ops, const char *dev_path,
                       const void *buf, const char *src_path, off_t src_off,
                       off_t offset, long long len,
                       upgrade_notify_fn cb, void *cb_ctx)
{
    (void)ops;
    if ((!buf == !src_path) || len <= 0 || offset < 0) {
        pr_err("block_write: invalid args (offset=%lld len=%lld)\n",
               (long long)offset, len);
        return -1;
    }

    int src_fd = -1;
    if (src_path) {
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) {
            pr_err("open %s: %s\n", src_path, strerror(errno));
            return -1;
        }
        if (lseek(src_fd, src_off, SEEK_SET) == (off_t)-1) {
            pr_err("lseek src %s: %s\n", src_path, strerror(errno));
            close(src_fd);
            return -1;
        }
    }

    /* A device-write op must never carry file create/truncate semantics:
     * O_TRUNC is a silent no-op on a block device but O_CREAT would create a
     * regular file if the node is missing — the write then lands in a file
     * and the GPT/BCB silently goes nowhere (brick). Always open the existing
     * node O_RDWR and lseek; matches core/flash.c's block_write. */
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        pr_err("open %s: %s\n", dev_path, strerror(errno));
        if (src_fd >= 0)
            close(src_fd);
        return -1;
    }
    if (offset > 0 && lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek %s: %s\n", dev_path, strerror(errno));
        close(fd);
        if (src_fd >= 0)
            close(src_fd);
        return -1;
    }

    char iobuf[BLOCK_WRITE_LEN];
    const char *mp = buf;
    long long remain = len;
    long long written = 0;
    int ret = -1;
    while (remain > 0) {
        size_t chunk = remain > (long long)BLOCK_WRITE_LEN ?
                       BLOCK_WRITE_LEN : (size_t)remain;
        const char *wptr;
        if (src_path) {
            size_t got = 0;
            while (got < chunk) {
                ssize_t rn = read(src_fd, iobuf + got, chunk - got);
                if (rn <= 0) {
                    pr_err("block_write: src read short %zd: %s\n",
                           rn, strerror(errno));
                    goto out;
                }
                got += (size_t)rn;
            }
            wptr = iobuf;
        } else {
            wptr = mp;
        }
        ssize_t n = write(fd, wptr, chunk);
        if (n != (ssize_t)chunk) {
            pr_err("write %s: %zd/%zu: %s\n", dev_path, n, chunk, strerror(errno));
            goto out;
        }
        if (!src_path)
            mp += chunk;
        remain -= chunk;
        written += chunk;
        if (cb)
            cb(dev_path, (size_t)written, (size_t)len, cb_ctx);
    }
    ret = 0;

out:
    fsync(fd);
    close(fd);
    if (src_fd >= 0)
        close(src_fd);
    return ret;
}

/* (rawlba, rawblksz) of a block device: size and logical block measured
 * live — UFS reports 4096, SATA/eMMC 512. No trailing reserved region.
 * A failed BLKSSZGET is a hard error: guessing the logical block here
 * would build the GPT with the wrong geometry (the original "Invalid
 * GPT" bug) — failing the upgrade is strictly safer. */
static long long block_disk_lbas(struct storage_ops *ops, const char *dev_path,
                                 uint32_t *lb_bytes)
{
    (void)ops;
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("disk_lbas open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    uint64_t bytes = 0;
    int ret = ioctl(fd, BLKGETSIZE64, &bytes);
    int ssz = 0;
    int ssz_ret = ioctl(fd, BLKSSZGET, &ssz);
    close(fd);
    if (ret != 0) {
        pr_err("BLKGETSIZE64 %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    if (ssz_ret != 0 || ssz < SECTOR_SIZE) {
        pr_err("BLKSSZGET %s: ret=%d ssz=%d (%s)\n",
               dev_path, ssz_ret, ssz, strerror(errno));
        return -1;
    }
    *lb_bytes = (uint32_t)ssz;
    return (long long)(bytes / (uint32_t)ssz);
}

/* ===== eMMC medium ===== */
#if defined(RK_STORAGE_EMMC)

static struct storage_ops emmc_ops = {
    .type               = STORAGE_EMMC,
    .name               = "emmc",
    .read               = block_read,
    .write              = block_write,
    .disk_lbas          = block_disk_lbas,
};

struct storage_ops *emmc_backend_create(void)
{
    /* eMMC is single-device: the main partition holds both the IDBlock
     * (fixed offset, read by the bootrom) and everything else — bare and
     * bootloader entry are the same node. */
    const char *p = device_get_emmc_point();
    if (!p || !p[0])
        p = "/dev/mmcblk0";
    emmc_ops.bare_dev = p;
    emmc_ops.bootloader_dev = p;
    return &emmc_ops;
}

#endif /* RK_STORAGE_EMMC */

/* ===== SCSI media: UFS and SATA ===== */
#if defined(RK_STORAGE_UFS)

static struct storage_ops ufs_ops = {
    .type               = STORAGE_UFS,
    .name               = "ufs",
    .read               = block_read,
    .write              = block_write,
    .disk_lbas          = block_disk_lbas,
    /* Main LUN holds the user partitions; the bootrom reads the IDBlock
     * from the dedicated boot LUN — which is why these two stay separate. */
    .bare_dev           = "/dev/sda",
    .bootloader_dev     = "/dev/sdb",
};

struct storage_ops *ufs_backend_create(void)
{
    return &ufs_ops;
}

/* ---- SATA ----
 * Shares the SCSI /dev/sd* node namespace with UFS but is a distinct medium:
 * 512B logical block, NOT bootable (the bootrom can't read IDBlock from a SATA
 * disk — a SATA system board pairs with Nor flash for the bootloader, so
 * bootloader_dev stays NULL; a bootloader item routed here aborts in
 * resolve_disk_level). */

static struct storage_ops sata_ops = {
    .type               = STORAGE_SATA,
    .name               = "sata",
    .read               = block_read,
    .write              = block_write,
    .disk_lbas          = block_disk_lbas,
    .bare_dev           = "/dev/sda",
    .bootloader_dev     = NULL,
};

struct storage_ops *sata_backend_create(void)
{
    return &sata_ops;
}

#endif /* RK_STORAGE_UFS */

#endif /* RK_STORAGE_EMMC || RK_STORAGE_UFS */
