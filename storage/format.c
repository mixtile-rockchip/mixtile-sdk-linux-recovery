/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "format"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include "recovery/volume.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery/storage.h"
#include "recovery/ubi.h"
#include "recovery/mtd_info.h"

/* Extract label from mount_point: "/userdata" -> "userdata" */
static const char *volume_label(const Volume *v)
{
    const char *slash = strrchr(v->mount_point, '/');
    if (slash && slash[1] != '\0')
        return slash + 1;
    return "unknown";
}

/* Run an external mkfs tool */
static int run_mkfs(const char *tool, const char *device, const char *label)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (label)
            execlp(tool, tool, "-L", label, device, NULL);
        else
            execlp(tool, tool, device, NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}

/* Flash erase for MTD device */
static int flash_erase_mtd(const char *mtd_dev)
{
    pid_t pid = fork();
    if (pid == 0) {
        execlp("flash_erase", "flash_erase", mtd_dev, "0", "0", NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}

int format_volume(const char *volume)
{
    Volume *v = volume_for_path(volume);
    if (v == NULL) {
        pr_err("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        pr_err("can't format ramdisk\n");
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        pr_err("can't give path \"%s\" to format_volume\n", volume);
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        pr_err("failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    const char *label = volume_label(v);

    /* MTD raw format (yaffs2/mtd) */
    if (strcmp(v->fs_type, "yaffs2") == 0 ||
        strcmp(v->fs_type, "mtd") == 0) {
        /* Find MTD device number from /proc/mtd */
        const char *part_name = v->device;
        if (strstr(part_name, "userdata"))
            part_name = "userdata";

        int mtd_num = mtd_find_num_by_name(part_name);
        if (mtd_num < 0) {
            pr_err("no MTD partition \"%s\"\n", part_name);
            return -1;
        }
        char mtd_dev[32];
        snprintf(mtd_dev, sizeof(mtd_dev), "/dev/mtd%d", mtd_num);
        return flash_erase_mtd(mtd_dev);
    }

    /* UBIFS format */
#if defined(RK_STORAGE_MTD)
    if (strncmp(v->fs_type, "ubi", 3) == 0) {
        const char *part_name = v->device;
        const char *byname = strstr(v->device, "/dev/block/by-name/");
        if (byname)
            part_name = byname + strlen("/dev/block/by-name/");

        int mtd_num = mtd_find_num_by_name(part_name);
        if (mtd_num < 0) {
            pr_err("no MTD/UBI partition \"%s\"\n", part_name);
            return -1;
        }
        struct ubi_ops *ubi = ubi_get_ops();
        if (!ubi) {
            pr_err("UBI support not compiled in\n");
            return -1;
        }
        return ubi->format(mtd_num, part_name);
    }
#endif /* RK_STORAGE_MTD */

    /* ext4 */
    if (strcmp(v->fs_type, "ext4") == 0) {
        if (run_mkfs("mke2fs", v->device, label) != 0) {
            pr_err("mke2fs failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

    /* ext2 */
    if (strcmp(v->fs_type, "ext2") == 0) {
        if (run_mkfs("mke2fs", v->device, label) != 0) {
            pr_err("mke2fs (ext2) failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

    /* vfat */
    if (strcmp(v->fs_type, "vfat") == 0) {
        if (run_mkfs("mkfs.vfat", v->device, NULL) != 0) {
            pr_err("mkfs.vfat failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

    pr_err("unsupported fs_type \"%s\"\n", v->fs_type);
    return -1;
}

int resize_volume(const char *volume)
{
    Volume *v = volume_for_path(volume);
    if (v == NULL) {
        pr_err("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        pr_err("can't resize ramdisk\n");
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        pr_err("failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "ext4") == 0 || strcmp(v->fs_type, "ext2") == 0) {
        /* Run e2fsck + resize2fs */
        pid_t pid = fork();
        if (pid == 0) {
            execlp("e2fsck", "e2fsck", "-fy", v->device, NULL);
            _exit(127);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
        }

        pid = fork();
        if (pid == 0) {
            execlp("resize2fs", "resize2fs", v->device, NULL);
            _exit(127);
        }
        if (pid < 0)
            return -1;
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            return 0;
        pr_err("resize2fs failed on %s\n", v->device);
        return -1;
    }

    pr_err("resize unsupported for fs_type \"%s\"\n", v->fs_type);
    return -1;
}
