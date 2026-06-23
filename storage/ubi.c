/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "storage:ubi"

#include "recovery/config.h"

#if defined(RK_STORAGE_MTD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "recovery/ubi.h"
#include "recovery/log.h"
#include "recovery/mtd_info.h"
#include "recovery/process.h"

static int ubi_op_attach(int mtd_num)
{
    int existing = ubi_find_num_by_mtd(mtd_num);
    if (existing >= 0) {
        pr_info("mtd%d already attached as ubi%d\n", mtd_num, existing);
        return existing;
    }

    char mtd_str[8], ubi_str[8];
    snprintf(mtd_str, sizeof(mtd_str), "%d", mtd_num);
    snprintf(ubi_str, sizeof(ubi_str), "%d", mtd_num);

    char *const argv[] = {
        "/usr/sbin/ubiattach", "-m", mtd_str, "-d", ubi_str, NULL
    };
    pr_info("ubiattach mtd%d -> ubi%d\n", mtd_num, mtd_num);
    if (rk_run_cmd(argv[0], argv) != 0) {
        pr_err("ubiattach failed for mtd%d\n", mtd_num);
        return -1;
    }
    return mtd_num;
}

static int ubi_op_detach(int ubi_num)
{
    char num_str[8];
    snprintf(num_str, sizeof(num_str), "%d", ubi_num);
    char *const argv[] = {
        "/usr/sbin/ubidetach", "-d", num_str, NULL
    };
    pr_info("ubidetach ubi%d\n", ubi_num);
    rk_run_cmd(argv[0], argv);
    return 0;
}

static int ubi_op_read_volume(int ubi_num, int vol_id,
                              const char *out_file, long long *out_size)
{
    char vol_path[64];
    snprintf(vol_path, sizeof(vol_path), "/dev/ubi%d_%d", ubi_num, vol_id);

    char sysfs_path[128], size_buf[32];
    long long data_size = -1;
    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/class/ubi/ubi%d_%d/data_bytes", ubi_num, vol_id);
    int sfd = open(sysfs_path, O_RDONLY);
    if (sfd >= 0) {
        int n = read(sfd, size_buf, sizeof(size_buf) - 1);
        close(sfd);
        if (n > 0) {
            size_buf[n] = '\0';
            data_size = strtoll(size_buf, NULL, 10);
        }
    }
    pr_info("UBI volume %s data_bytes=%lld\n", vol_path, data_size);

    int fd_src = open(vol_path, O_RDONLY);
    if (fd_src < 0) {
        pr_err("open %s: %s\n", vol_path, strerror(errno));
        return -1;
    }

    int fd_dst = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0) {
        pr_err("create %s: %s\n", out_file, strerror(errno));
        close(fd_src);
        return -1;
    }

    char buf[16 * 1024];
    long long remain = (data_size > 0) ? data_size : (long long)1 << 60;
    long long total = 0;

    while (remain > 0) {
        size_t to_read = remain > (long long)sizeof(buf) ?
                         sizeof(buf) : (size_t)remain;
        ssize_t n = read(fd_src, buf, to_read);
        if (n <= 0)
            break;
        if (write(fd_dst, buf, n) != n) {
            pr_err("write %s: %s\n", out_file, strerror(errno));
            close(fd_src);
            close(fd_dst);
            unlink(out_file);
            return -1;
        }
        total += n;
        remain -= n;
    }

    close(fd_src);
    fsync(fd_dst);
    close(fd_dst);

    pr_info("read %lld bytes from %s\n", total, vol_path);
    if (out_size)
        *out_size = (data_size > 0) ? data_size : total;
    return 0;
}

static int ubi_op_update_volume(const char *ubi_dev, const char *src_file)
{
    char *const argv[] = {
        "/usr/sbin/ubiupdatevol", (char *)ubi_dev, (char *)src_file, NULL
    };
    pr_info("ubiupdatevol %s %s\n", ubi_dev, src_file);
    if (rk_run_cmd(argv[0], argv) != 0) {
        pr_err("ubiupdatevol failed: %s -> %s\n", src_file, ubi_dev);
        return -1;
    }
    return 0;
}

static int ubi_op_format(int mtd_num, const char *vol_name)
{
    char mtd_dev[32], ubi_dev[32];
    char mtd_str[8], ubi_str[8];

    snprintf(mtd_dev, sizeof(mtd_dev), "/dev/mtd%d", mtd_num);
    snprintf(ubi_dev, sizeof(ubi_dev), "/dev/ubi%d", mtd_num);
    snprintf(mtd_str, sizeof(mtd_str), "%d", mtd_num);
    snprintf(ubi_str, sizeof(ubi_str), "%d", mtd_num);

    char *const detach_argv[] = {
        "/usr/sbin/ubidetach", "-d", ubi_str, NULL
    };
    rk_run_cmd(detach_argv[0], detach_argv);

    char *const format_argv[] = {
        "/usr/sbin/ubiformat", "-y", mtd_dev, NULL
    };
    pr_info("ubiformat %s\n", mtd_dev);
    if (rk_run_cmd(format_argv[0], format_argv) != 0) {
        pr_err("ubiformat failed on %s\n", mtd_dev);
        return -1;
    }

    char *const attach_argv[] = {
        "/usr/sbin/ubiattach", "-m", mtd_str, "-d", ubi_str, NULL
    };
    if (rk_run_cmd(attach_argv[0], attach_argv) != 0) {
        pr_err("ubiattach failed for mtd%d\n", mtd_num);
        return -1;
    }

    char *const mkvol_argv[] = {
        "/usr/sbin/ubimkvol", ubi_dev, "-m", "-N", (char *)vol_name, NULL
    };
    pr_info("ubimkvol %s name=%s\n", ubi_dev, vol_name);
    if (rk_run_cmd(mkvol_argv[0], mkvol_argv) != 0) {
        pr_err("ubimkvol failed on %s\n", ubi_dev);
        return -1;
    }

    return 0;
}

static struct ubi_ops ubi_impl = {
    .attach         = ubi_op_attach,
    .detach         = ubi_op_detach,
    .read_volume    = ubi_op_read_volume,
    .update_volume  = ubi_op_update_volume,
    .format         = ubi_op_format,
};

struct ubi_ops *ubi_get_ops(void)
{
    return &ubi_impl;
}

#else  /* !RK_STORAGE_MTD */

#include "recovery/ubi.h"

struct ubi_ops *ubi_get_ops(void)
{
    return NULL;
}

#endif
