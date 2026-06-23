/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * MTD device class: NAND and NOR media.
 *
 * Structured in three clearly separated sections — [shared] / [NAND] / [NOR].
 * Each medium has its own self-contained ops; there is no mtd_is_nor() in the
 * I/O path. The two media differ fundamentally and are NOT mixed:
 *
 *   NAND: OOB + bad-block table, page programming. All I/O goes through the
 *         mtd-utils binaries in LOGICAL (bad-block-skipping) space:
 *           write = map nandwrite's skipbad target blocks to good erase runs,
 *                   flash_erase those runs, then nandwrite -p
 *                   - large partition: --input-skip/--input-size stream the
 *                     bytes straight from the source image (O(1) RAM, no temp)
 *                   - small in-memory data (GPT/BCB): staged to a KB temp file
 *           read  = nanddump --bb=skipbad (skip bad blocks, ECC on)
 *         Reads and writes both use skipbad semantics, so a partition
 *         round-trips correctly even with bad blocks. nandwrite is GPL →
 *         invoked as a subprocess, not linked.
 *   NOR : byte-addressable, no OOB/BBT. Pure syscalls: pread for reads,
 *         ioctl(MEMERASE)+pwrite with a single erase-block RMW buffer for
 *         writes (O(erase_block) RAM, no temp file, no mtd-utils dependency).
 */

#define __MODULE__ "storage:mtd"

#include "recovery/config.h"

#if defined(RK_STORAGE_MTD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>

#include "recovery/storage.h"
#include "recovery/log.h"
#include "recovery/mtd_info.h"
#include "recovery/process.h"

/* ============================================================
 * Shared MTD core (used by both NAND and NOR)
 * ============================================================ */

/* flash_erase <dev> <offset_bytes> <count_blocks> (count 0 = to end).
 * Supported by both NAND and NOR drivers. */
static int mtd_region_erase(const char *dev_path, off_t block_start,
                            size_t nblocks)
{
    char off_s[32], n_s[32];
    snprintf(off_s, sizeof(off_s), "%lld", (long long)block_start);
    snprintf(n_s, sizeof(n_s), "%zu", nblocks);
    char *const argv[] = {
        "flash_erase", (char *)dev_path, off_s, n_s, NULL
    };
    if (rk_run_cmd("flash_erase", argv) != 0) {
        pr_err("flash_erase %s @%lld nblk=%zu failed\n",
               dev_path, (long long)block_start, nblocks);
        return -1;
    }
    return 0;
}

/* Stage data into a fresh tmpfile (small payloads only — IDBlock is KBs). */
static int stage_tmpfile(const void *data, size_t data_size,
                         char *out_path, size_t out_sz)
{
    snprintf(out_path, out_sz, "/tmp/rk_mtd_idb.XXXXXX");
    int tfd = mkstemp(out_path);
    if (tfd < 0) {
        pr_err("mkstemp: %s\n", strerror(errno));
        return -1;
    }
    size_t remain = data_size;
    const char *p = data;
    while (remain > 0) {
        ssize_t n = write(tfd, p, remain);
        if (n <= 0) {
            pr_err("tmpfile write: %s\n", strerror(errno));
            close(tfd);
            unlink(out_path);
            return -1;
        }
        p += n;
        remain -= n;
    }
    close(tfd);
    return 0;
}

/* ============================================================
 * NAND — mtd-utils tools, logical (bad-block-skipping) space
 * ============================================================ */

#define NAND_BBT_RESERVE_BLOCKS  4

/* nanddump a region to a file (skip bad blocks, ECC on). */
static int nand_nanddump(const char *dev_path, off_t offset, long long len,
                         const char *outfile)
{
    char off_s[32], len_s[32];
    snprintf(off_s, sizeof(off_s), "%lld", (long long)offset);
    snprintf(len_s, sizeof(len_s), "%lld", len);
    char *const argv[] = {
        "nanddump", "--bb=skipbad", "-s", off_s, "-l", len_s,
        "-f", (char *)outfile, (char *)dev_path, NULL
    };
    if (rk_run_cmd("nanddump", argv) != 0) {
        pr_err("nanddump %s off=%lld len=%lld failed\n",
               dev_path, (long long)offset, len);
        return -1;
    }
    return 0;
}

/* nandwrite `len` bytes from infile[skip..] to dev @ off (caller erases mapped good runs).
 * -p pads the final page; factory bad blocks are skipped. The
 * --input-skip/--input-size pair lets the large-partition path stream straight
 * from the update image without an app buffer or temp file. */
static int nand_nandwrite(const char *dev_path, off_t off, const char *infile,
                          long long skip, long long len)
{
    char off_s[32], skip_s[48], size_s[48];
    snprintf(off_s, sizeof(off_s), "%lld", (long long)off);
    snprintf(skip_s, sizeof(skip_s), "--input-skip=%lld", skip);
    snprintf(size_s, sizeof(size_s), "--input-size=%lld", len);
    char *const argv[] = {
        "nandwrite", "-p", skip_s, size_s, "-s", off_s,
        (char *)dev_path, (char *)infile, NULL
    };
    if (rk_run_cmd("nandwrite", argv) != 0) {
        pr_err("nandwrite %s @%lld skip=%lld size=%lld file=%s failed\n",
               dev_path, (long long)off, skip, len, infile);
        return -1;
    }
    return 0;
}

struct nand_erase_run {
    off_t  start;
    size_t blocks;
};

static int nand_block_is_bad(int fd, const char *dev_path, off_t block_start)
{
    loff_t query = (loff_t)block_start;
    int ret = ioctl(fd, MEMGETBADBLOCK, &query);
    if (ret < 0) {
        pr_err("MEMGETBADBLOCK %s @%lld: %s\n",
               dev_path, (long long)block_start, strerror(errno));
        return -1;
    }
    return ret > 0 ? 1 : 0;
}

static int nand_add_erase_run_block(struct nand_erase_run **runs,
                                    size_t *count, size_t *cap,
                                    off_t eb, off_t block_start)
{
    if (*count > 0) {
        struct nand_erase_run *last = &(*runs)[*count - 1];
        off_t next = last->start + (off_t)last->blocks * eb;
        if (next == block_start) {
            last->blocks++;
            return 0;
        }
    }

    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        struct nand_erase_run *new_runs = realloc(*runs,
                                                  new_cap * sizeof(**runs));
        if (!new_runs) {
            pr_err("nand erase map: oom for %zu runs\n", new_cap);
            return -1;
        }
        *runs = new_runs;
        *cap = new_cap;
    }

    (*runs)[*count].start = block_start;
    (*runs)[*count].blocks = 1;
    (*count)++;
    return 0;
}

static int nand_collect_write_runs(int fd, const char *dev_path,
                                   const mtd_info_t *info,
                                   off_t offset, long long len,
                                   struct nand_erase_run **out_runs,
                                   size_t *out_count)
{
    off_t eb = (off_t)info->erasesize;
    off_t pos = offset;
    long long remain = len;
    struct nand_erase_run *runs = NULL;
    size_t count = 0, cap = 0;

    while (remain > 0) {
        if ((uint64_t)pos >= (uint64_t)info->size) {
            pr_err("nand erase map %s: range exceeds device at 0x%llx\n",
                   dev_path, (long long)pos);
            free(runs);
            return -1;
        }

        off_t block_start = (pos / eb) * eb;
        int bad = nand_block_is_bad(fd, dev_path, block_start);
        if (bad < 0) {
            free(runs);
            return -1;
        }
        if (bad) {
            pr_dbg("nand erase map %s: skip bad block @%lld\n",
                   dev_path, (long long)block_start);
            pos = block_start + eb;
            continue;
        }

        if (nand_add_erase_run_block(&runs, &count, &cap, eb, block_start) != 0) {
            free(runs);
            return -1;
        }

        off_t block_end = block_start + eb;
        long long avail = (long long)(block_end - pos);
        long long consume = remain < avail ? remain : avail;
        remain -= consume;
        pos += consume;
    }

    *out_runs = runs;
    *out_count = count;
    return 0;
}

static int nand_erase_runs(const char *dev_path,
                           const struct nand_erase_run *runs,
                           size_t count)
{
    for (size_t i = 0; i < count; i++) {
        pr_dbg("nand erase run %s @%lld nblk=%zu\n",
               dev_path, (long long)runs[i].start, runs[i].blocks);
        if (mtd_region_erase(dev_path, runs[i].start, runs[i].blocks) != 0)
            return -1;
    }
    return 0;
}

static int write_all_fd(int fd, const void *data, size_t len)
{
    const char *p = data;
    size_t remain = len;

    while (remain > 0) {
        ssize_t n = write(fd, p, remain);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            pr_err("tmpfile write: %s\n", strerror(errno));
            return -1;
        }
        p += n;
        remain -= (size_t)n;
    }
    return 0;
}

static int nand_patch_tmpfile(const char *tmpfile,
                              const void *buf,
                              const char *src_path,
                              off_t src_off,
                              off_t dst_off,
                              long long len)
{
    int dst = open(tmpfile, O_RDWR);
    if (dst < 0) {
        pr_err("open %s: %s\n", tmpfile, strerror(errno));
        return -1;
    }
    if (lseek(dst, dst_off, SEEK_SET) == (off_t)-1) {
        pr_err("lseek %s @%lld: %s\n", tmpfile, (long long)dst_off,
               strerror(errno));
        close(dst);
        return -1;
    }

    int ret = -1;
    if (src_path) {
        int src = open(src_path, O_RDONLY);
        if (src < 0) {
            pr_err("open %s: %s\n", src_path, strerror(errno));
            goto out_dst;
        }
        if (lseek(src, src_off, SEEK_SET) == (off_t)-1) {
            pr_err("lseek %s @%lld: %s\n", src_path, (long long)src_off,
                   strerror(errno));
            close(src);
            goto out_dst;
        }

        uint8_t iobuf[64 * 1024];
        long long remain = len;
        while (remain > 0) {
            size_t chunk = remain > (long long)sizeof(iobuf) ?
                           sizeof(iobuf) : (size_t)remain;
            ssize_t n;
            do {
                n = read(src, iobuf, chunk);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) {
                pr_err("read %s: short read %zd, remain %lld\n",
                       src_path, n, remain);
                close(src);
                goto out_dst;
            }
            if (write_all_fd(dst, iobuf, (size_t)n) != 0) {
                close(src);
                goto out_dst;
            }
            remain -= n;
        }
        close(src);
    } else {
        const char *p = buf;
        long long remain = len;
        while (remain > 0) {
            size_t chunk = remain > (long long)(64 * 1024) ?
                           (64 * 1024) : (size_t)remain;
            if (write_all_fd(dst, p, chunk) != 0)
                goto out_dst;
            p += chunk;
            remain -= (long long)chunk;
        }
    }

    ret = 0;
out_dst:
    close(dst);
    return ret;
}

static int nand_collect_runs_or_fail(int fd, const char *dev_path,
                                     const mtd_info_t *info,
                                     off_t offset, long long len,
                                     struct nand_erase_run **runs,
                                     size_t *run_count)
{
    *runs = NULL;
    *run_count = 0;
    if (nand_collect_write_runs(fd, dev_path, info, offset, len,
                                runs, run_count) != 0)
        return -1;
    if (*run_count == 0) {
        pr_err("nand erase map %s: no writable good blocks\n", dev_path);
        free(*runs);
        *runs = NULL;
        return -1;
    }
    return 0;
}

static int nand_write_fast(int fd, const char *dev_path,
                           const mtd_info_t *info,
                           const void *buf,
                           const char *src_path,
                           off_t src_off,
                           off_t offset,
                           long long len)
{
    char tmpfile[64];
    const char *infile = src_path;
    long long input_skip = (long long)src_off;
    bool staged = false;

    if (!src_path) {
        if ((uint64_t)len > (uint64_t)SIZE_MAX) {
            pr_err("nand_write: memory buffer too large: %lld\n", len);
            return -1;
        }
        if (stage_tmpfile(buf, (size_t)len, tmpfile, sizeof(tmpfile)) != 0)
            return -1;
        infile = tmpfile;
        input_skip = 0;
        staged = true;
    }

    struct nand_erase_run *runs = NULL;
    size_t run_count = 0;
    int ret = -1;

    if (nand_collect_runs_or_fail(fd, dev_path, info, offset, len,
                                  &runs, &run_count) != 0)
        goto out;
    if (nand_erase_runs(dev_path, runs, run_count) != 0)
        goto out;
    if (nand_nandwrite(dev_path, offset, infile, input_skip, len) != 0)
        goto out;

    ret = 0;
out:
    free(runs);
    if (staged)
        unlink(tmpfile);
    return ret;
}

static int nand_write_rmw(int fd, const char *dev_path,
                          const mtd_info_t *info,
                          const void *buf,
                          const char *src_path,
                          off_t src_off,
                          off_t offset,
                          long long len)
{
    off_t eb = (off_t)info->erasesize;
    off_t block_start = (offset / eb) * eb;
    off_t head = offset - block_start;

    if (head < 0 || len > LLONG_MAX - (long long)head) {
        pr_err("nand_write: invalid RMW range offset=%lld len=%lld\n",
               (long long)offset, len);
        return -1;
    }

    long long span = (long long)head + len;
    if (span > LLONG_MAX - (long long)eb + 1) {
        pr_err("nand_write: RMW range overflow offset=%lld len=%lld\n",
               (long long)offset, len);
        return -1;
    }
    long long region = ((span + (long long)eb - 1) / (long long)eb) *
                       (long long)eb;

    struct nand_erase_run *runs = NULL;
    size_t run_count = 0;
    if (nand_collect_runs_or_fail(fd, dev_path, info, block_start, region,
                                  &runs, &run_count) != 0)
        return -1;

    char tmpfile[] = "/tmp/rk_nand_rmw.XXXXXX";
    int tfd = mkstemp(tmpfile);
    if (tfd < 0) {
        pr_err("nand_write: mkstemp: %s\n", strerror(errno));
        free(runs);
        return -1;
    }
    close(tfd);

    int ret = -1;
    if (nand_nanddump(dev_path, block_start, region, tmpfile) != 0)
        goto out;
    if (nand_patch_tmpfile(tmpfile, buf, src_path, src_off,
                           head, len) != 0)
        goto out;
    if (nand_erase_runs(dev_path, runs, run_count) != 0)
        goto out;
    if (nand_nandwrite(dev_path, block_start, tmpfile, 0, region) != 0)
        goto out;

    ret = 0;
out:
    unlink(tmpfile);
    free(runs);
    return ret;
}

/*
 * File-slice writes are partition/bulk images: keep them streaming and erase
 * the mapped good-block runs that nandwrite will touch. In-memory writes are
 * metadata/control data; if they cover only part of an eraseblock, preserve the
 * surrounding bytes with nanddump + patch + erase + nandwrite.
 */
static int nand_write_op(struct storage_ops *ops, const char *dev_path,
                         const void *buf, const char *src_path, off_t src_off,
                         off_t offset, long long len,
                         upgrade_notify_fn cb, void *cb_ctx)
{
    (void)ops;
    if ((!buf == !src_path) || len <= 0 || offset < 0) {
        pr_err("nand_write: invalid args (offset=%lld len=%lld buf=%p src=%s)\n",
               (long long)offset, len, buf, src_path ? src_path : "(null)");
        return -1;
    }

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    mtd_info_t info;
    int rc = ioctl(fd, MEMGETINFO, &info);
    if (rc < 0) {
        pr_err("MEMGETINFO %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }
    if (info.erasesize == 0 || info.writesize == 0) {
        pr_err("nand_write: invalid geometry erase=%u write=%u\n",
               info.erasesize, info.writesize);
        close(fd);
        return -1;
    }
    if ((uint64_t)offset >= (uint64_t)info.size) {
        pr_err("nand_write: offset 0x%llx exceeds device size 0x%x\n",
               (long long)offset, info.size);
        close(fd);
        return -1;
    }

    off_t eb = (off_t)info.erasesize;
    int ret;
    if (src_path) {
        if (offset % eb != 0) {
            pr_err("nand_write: bulk file write offset 0x%llx is not "
                   "eraseblock-aligned (eb=0x%llx)\n",
                   (long long)offset, (long long)eb);
            close(fd);
            return -1;
        }
        ret = nand_write_fast(fd, dev_path, &info, buf, src_path, src_off,
                              offset, len);
    } else if ((offset % eb) != 0 || (len % (long long)eb) != 0) {
        ret = nand_write_rmw(fd, dev_path, &info, buf, src_path, src_off,
                             offset, len);
    } else {
        ret = nand_write_fast(fd, dev_path, &info, buf, src_path, src_off,
                              offset, len);
    }
    close(fd);

    if (ret == 0 && cb)
        cb(dev_path, (size_t)len, (size_t)len, cb_ctx);
    return ret;
}

/*
 * Read via nanddump (skip bad blocks, ECC on). Destination is EITHER a file
 * (dst_path — one nanddump straight to it; MD5/diff bulk reads) OR an in-memory
 * buffer (small BCB/slot data — nanddump to a KB temp file, then slurp it back).
 */
static int nand_read_op(struct storage_ops *ops, const char *dev_path,
                        void *buf, const char *dst_path,
                        off_t offset, long long len)
{
    (void)ops;
    if ((!buf == !dst_path) || len <= 0 || offset < 0) {
        pr_err("nand_read: invalid args (offset=%lld len=%lld)\n",
               (long long)offset, len);
        return -1;
    }

    if (dst_path)
        return nand_nanddump(dev_path, offset, len, dst_path);

    char tmpfile[] = "/tmp/rk_nand_read.XXXXXX";
    int tfd = mkstemp(tmpfile);
    if (tfd < 0) {
        pr_err("nand_read: mkstemp: %s\n", strerror(errno));
        return -1;
    }
    close(tfd);

    int ret = -1;
    if (nand_nanddump(dev_path, offset, len, tmpfile) != 0)
        goto cleanup;

    int fd = open(tmpfile, O_RDONLY);
    if (fd < 0) {
        pr_err("nand_read: open %s: %s\n", tmpfile, strerror(errno));
        goto cleanup;
    }
    long long remain = len;
    char *p = buf;
    while (remain > 0) {
        ssize_t n = read(fd, p, (size_t)remain);
        if (n <= 0) {
            pr_err("nand_read: short read %zd, remain %lld\n", n, remain);
            close(fd);
            goto cleanup;
        }
        p += n;
        remain -= n;
    }
    close(fd);
    ret = 0;

cleanup:
    unlink(tmpfile);
    return ret;
}

/*
 * (rawlba, rawblksz) of a NAND as u-boot sees it (mtd_blk.c::mtd_blk_probe):
 * the logical block is fixed 512 (u-boot's MTD rawblksz fallback); the count
 * skips the trailing BBT reserve (NAND_BBT_SCAN_MAXBLOCKS = 4 erase blocks),
 * then keeps skipping while those blocks are bad — the first good block from
 * the end is where u-boot's lba stops, and the backup GPT must end there.
 */
static long long nand_disk_lbas(struct storage_ops *ops, const char *dev_path,
                                uint32_t *lb_bytes)
{
    (void)ops;
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("disk_lbas open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    mtd_info_t info;
    if (ioctl(fd, MEMGETINFO, &info) < 0) {
        pr_err("MEMGETINFO %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }

    uint64_t nblk = info.size / info.erasesize;
    uint64_t i;
    for (i = NAND_BBT_RESERVE_BLOCKS; i < nblk; i++) {
        loff_t bo = (loff_t)(info.size - (uint64_t)info.erasesize * (i + 1));
        /* 1 = bad (keep skipping); 0 = good; <0 = query unsupported — stop,
         * degrading to the plain 4-block reserve. */
        if (ioctl(fd, MEMGETBADBLOCK, &bo) <= 0)
            break;
        pr_dbg("nand disk_lbas: trailing bad block @0x%llx, skip\n",
               (long long)bo);
    }
    close(fd);
    if (i >= nblk) {
        pr_err("disk_lbas: %s all trailing blocks bad\n", dev_path);
        return -1;
    }
    *lb_bytes = SECTOR_SIZE;
    return (long long)((info.size - (uint64_t)info.erasesize * i) / SECTOR_SIZE);
}

static struct storage_ops nand_ops = {
    .type               = STORAGE_NAND,
    .name               = "nand",
    .read               = nand_read_op,
    .write              = nand_write_op,
    .disk_lbas          = nand_disk_lbas,
};

/* ============================================================
 * NOR — byte-addressable, read()/MEMERASE+write() syscalls.
 * Erase-block-aligned read-modify-write (no mtd-utils dependency).
 * ============================================================ */

/*
 * NOR is byte-addressable: read straight with pread — no erase-block alignment,
 * no temp file. Destination is EITHER an in-memory buffer (buf) OR a file
 * (dst_path — spooled through a stack buffer; MD5/diff bulk reads).
 */
static int nor_read_op(struct storage_ops *ops, const char *dev_path,
                       void *buf, const char *dst_path,
                       off_t offset, long long len)
{
    (void)ops;
    if ((!buf == !dst_path) || len <= 0 || offset < 0) {
        pr_err("nor_read: invalid args (offset=%lld len=%lld)\n",
               (long long)offset, len);
        return -1;
    }

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("nor_read: open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }

    int dst_fd = -1;
    if (dst_path) {
        dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            pr_err("nor_read: open %s: %s\n", dst_path, strerror(errno));
            close(fd);
            return -1;
        }
    }

    uint8_t iobuf[64 * 1024];
    char *mp = buf;
    off_t pos = offset;
    long long remain = len;
    int ret = -1;
    while (remain > 0) {
        size_t chunk = remain > (long long)sizeof(iobuf) ?
                       sizeof(iobuf) : (size_t)remain;
        /* read straight into the caller's buffer, or via iobuf when spooling */
        ssize_t n = pread(fd, dst_path ? (void *)iobuf : (void *)mp, chunk, pos);
        if (n <= 0) {
            pr_err("nor_read: pread @%lld: %zd %s\n",
                   (long long)pos, n, strerror(errno));
            goto out;
        }
        if (dst_path) {
            ssize_t w = write(dst_fd, iobuf, (size_t)n);
            if (w != n) {
                pr_err("nor_read: dst write %zd/%zd: %s\n",
                       w, n, strerror(errno));
                goto out;
            }
        } else {
            mp += n;
        }
        pos += n;
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
 * Erase-block-streaming write with read-modify-write for partial blocks, one
 * erase block at a time (O(erase_block) RAM, no temp file). Source is EITHER an
 * in-memory buffer (buf) OR a file slice (src_path, streamed from src_off). For
 * each erase block the write touches: if it is only partially covered, the
 * current content is pread first to preserve the bytes outside [offset,
 * offset+len); then the block is MEMERASE'd and the whole erase block pwritten.
 */
static int nor_write_op(struct storage_ops *ops, const char *dev_path,
                        const void *buf, const char *src_path, off_t src_off,
                        off_t offset, long long len,
                        upgrade_notify_fn cb, void *cb_ctx)
{
    (void)ops;
    if ((!buf == !src_path) || len <= 0 || offset < 0) {
        pr_err("nor_write: invalid args (offset=%lld len=%lld)\n",
               (long long)offset, len);
        return -1;
    }

    int dev = open(dev_path, O_RDWR);
    if (dev < 0) {
        pr_err("nor_write: open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    mtd_info_t info;
    if (ioctl(dev, MEMGETINFO, &info) < 0) {
        pr_err("nor_write: MEMGETINFO %s: %s\n", dev_path, strerror(errno));
        close(dev);
        return -1;
    }
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)info.size) {
        pr_err("nor_write: range end 0x%llx exceeds device size 0x%x\n",
               (long long)(offset + len), info.size);
        close(dev);
        return -1;
    }
    off_t eb = (off_t)info.erasesize;

    int src_fd = -1;
    if (src_path) {
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) {
            pr_err("nor_write: open %s: %s\n", src_path, strerror(errno));
            close(dev);
            return -1;
        }
        if (lseek(src_fd, src_off, SEEK_SET) == (off_t)-1) {
            pr_err("nor_write: lseek src %lld: %s\n",
                   (long long)src_off, strerror(errno));
            close(src_fd);
            close(dev);
            return -1;
        }
    }

    uint8_t *ebbuf = malloc((size_t)eb);
    if (!ebbuf) {
        pr_err("nor_write: oom eb=%lld\n", (long long)eb);
        if (src_fd >= 0)
            close(src_fd);
        close(dev);
        return -1;
    }

    const char *mp = buf;
    off_t end = offset + len;
    long long done = 0;
    int ret = -1;

    for (off_t bo = (offset / eb) * eb; bo < end; bo += eb) {
        off_t lo = bo > offset ? bo : offset;
        off_t hi = (bo + eb) < end ? (bo + eb) : end;
        size_t head = (size_t)(lo - bo);
        size_t copy = (size_t)(hi - lo);

        if (head != 0 || copy != (size_t)eb) {
            /* partial block: keep the bytes outside [lo, hi) */
            ssize_t rn = pread(dev, ebbuf, (size_t)eb, bo);
            if (rn < 0) {
                pr_err("nor_write: RMW pread @%lld: %s\n",
                       (long long)bo, strerror(errno));
                goto out;
            }
            if ((size_t)rn < (size_t)eb)
                memset(ebbuf + rn, 0xff, (size_t)eb - (size_t)rn);
        }

        if (src_path) {
            size_t got = 0;
            while (got < copy) {
                ssize_t n = read(src_fd, ebbuf + head + got, copy - got);
                if (n <= 0) {
                    pr_err("nor_write: src read short %zd: %s\n",
                           n, strerror(errno));
                    goto out;
                }
                got += (size_t)n;
            }
        } else {
            memcpy(ebbuf + head, mp, copy);
            mp += copy;
        }

        struct erase_info_user er = {
            .start  = (uint32_t)bo,
            .length = (uint32_t)eb,
        };
        if (ioctl(dev, MEMERASE, &er) < 0) {
            pr_err("nor_write: MEMERASE @%lld: %s\n",
                   (long long)bo, strerror(errno));
            goto out;
        }
        if (pwrite(dev, ebbuf, (size_t)eb, bo) != (ssize_t)eb) {
            pr_err("nor_write: pwrite @%lld: %s\n",
                   (long long)bo, strerror(errno));
            goto out;
        }
        done += copy;
        if (cb)
            cb(dev_path, (size_t)done, (size_t)len, cb_ctx);
    }
    ret = 0;

out:
    free(ebbuf);
    if (src_fd >= 0)
        close(src_fd);
    close(dev);
    return ret;
}

/* (rawlba, rawblksz) of a NOR: lb fixed 512 (u-boot MTD rawblksz fallback);
 * no BBT, so the count is the raw device size (mtd_blk.c else branch). */
static long long nor_disk_lbas(struct storage_ops *ops, const char *dev_path,
                               uint32_t *lb_bytes)
{
    (void)ops;
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        pr_err("disk_lbas open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    mtd_info_t info;
    int rc = ioctl(fd, MEMGETINFO, &info);
    close(fd);
    if (rc < 0) {
        pr_err("MEMGETINFO %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    *lb_bytes = SECTOR_SIZE;
    return (long long)(info.size / SECTOR_SIZE);
}

static struct storage_ops nor_ops = {
    .type               = STORAGE_NOR,
    .name               = "nor",
    .read               = nor_read_op,
    .write              = nor_write_op,
    .disk_lbas          = nor_disk_lbas,
};

/* ============================================================
 * Backend creation — resolve the constant device nodes once here
 * (a registered backend's medium exists; /proc/mtd is stable from boot).
 * ============================================================ */

/*
 * Disk-level writes (GPT / IDBlock / full-flash) need the whole-chip master
 * node — never a named partition (its partition-relative origin would
 * double-offset the absolute GPT/IDBlock bases). NULL when mtdparts split
 * the chip and the kernel didn't retain the master; the IDBlock shares the
 * same node since its geom base is an absolute chip offset.
 */
static const char *mtd_resolve_master(struct storage_ops *ops, bool want_nor,
                                      char *buf, size_t buf_sz)
{
    int num = mtd_find_master(want_nor);
    if (num < 0) {
        pr_info("%s: no whole-chip mtd master visible (mtdparts without "
                "CONFIG_MTD_PARTITIONED_MASTER=y?) — GPT/IDBlock/full-flash "
                "targets unavailable\n", ops->name);
        return NULL;
    }
    snprintf(buf, buf_sz, "/dev/mtd%d", num);
    pr_dbg("%s: whole-chip master %s\n", ops->name, buf);
    return buf;
}

struct storage_ops *nand_backend_create(void)
{
    static char master[32];
    nand_ops.bare_dev = mtd_resolve_master(&nand_ops, false,
                                           master, sizeof(master));
    nand_ops.bootloader_dev = nand_ops.bare_dev;
    return &nand_ops;
}

struct storage_ops *nor_backend_create(void)
{
    static char master[32];
    nor_ops.bare_dev = mtd_resolve_master(&nor_ops, true,
                                          master, sizeof(master));
    nor_ops.bootloader_dev = nor_ops.bare_dev;
    return &nor_ops;
}

#endif /* RK_STORAGE_MTD */
