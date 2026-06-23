/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "mount"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recovery/volume.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery/mtd_info.h"
#include "recovery/ubi.h"

int ensure_path_mounted(const char *path)
{
    /* If path already accessible, nothing to do */
    if (access(path, F_OK) == 0)
        return 0;

    Volume *v = volume_for_path(path);
    if (v == NULL) {
        /* Check if the resolved path is already mounted */
        if (scan_mounted_volumes() == 0) {
            /* Try partial match — can't help without a volume entry */
        }
        pr_err("unknown volume for path [%s]\n", path);
        return -1;
    }

    if (strcmp(v->fs_type, "ramdisk") == 0)
        return 0;

    /* Check if already mounted */
    if (scan_mounted_volumes() == 0) {
        const MountedVolume *mv =
            find_mounted_volume_by_mount_point(v->mount_point);
        if (mv)
            return 0;
    }

    mkdir(v->mount_point, 0755);

    int result;

    if (strncmp(v->fs_type, "ubi", 3) == 0) {
        /*
         * UBI filesystem: extract MTD number from device path,
         * attach UBI if needed, then mount ubifs.
         */
        const char *part_name = v->device;
        const char *byname = strstr(v->device, "/dev/block/by-name/");
        if (byname)
            part_name = byname + strlen("/dev/block/by-name/");

        int mtd_num = mtd_find_num_by_name(part_name);
        if (mtd_num < 0) {
            pr_err("cannot find MTD partition \"%s\"\n", part_name);
            return -1;
        }

        char ubi_dev_path[32];
        snprintf(ubi_dev_path, sizeof(ubi_dev_path), "/dev/ubi%d_0", mtd_num);

        if (access(ubi_dev_path, F_OK) != 0) {
            pr_info("UBI device %s not found, attaching mtd%d\n",
                    ubi_dev_path, mtd_num);
            struct ubi_ops *ubi = ubi_get_ops();
            if (!ubi || !ubi->attach) {
                pr_err("UBI ops unavailable\n");
                return -1;
            }
            int attached_ubi = ubi->attach(mtd_num);
            if (attached_ubi < 0) {
                pr_err("ubiattach failed for mtd%d\n", mtd_num);
                return -1;
            }
            snprintf(ubi_dev_path, sizeof(ubi_dev_path),
                     "/dev/ubi%d_0", attached_ubi);
        }

        result = mount(ubi_dev_path, v->mount_point, "ubifs",
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, NULL);
        if (result == 0)
            return 0;

        pr_err("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
        return -1;
    }

    if (strcmp(v->fs_type, "ext4") == 0 ||
        strcmp(v->fs_type, "vfat") == 0 ||
        strcmp(v->fs_type, "ext2") == 0) {
        result = mount(v->device, v->mount_point, v->fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
        if (result == 0)
            return 0;

        if (v->device2) {
            pr_warn("failed to mount %s (%s); trying %s\n",
                    v->device, strerror(errno), v->device2);
            result = mount(v->device2, v->mount_point, v->fs_type,
                           MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
            if (result == 0)
                return 0;
        }

        pr_err("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
        return -1;
    }

    pr_err("unknown fs_type \"%s\" for %s\n", v->fs_type, v->mount_point);
    return -1;
}

int ensure_path_unmounted(const char *path)
{
    Volume *v = volume_for_path(path);
    if (v == NULL) {
        pr_err("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0)
        return -1;

    if (scan_mounted_volumes() < 0) {
        pr_err("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume *mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL)
        return 0;   /* already unmounted */

    return unmount_mounted_volume(mv);
}

int ensure_ex_path_unmounted(const char *path)
{
    if (scan_mounted_volumes() < 0) {
        pr_err("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume *mv = find_mounted_volume_by_mount_point(path);
    if (mv == NULL)
        return 0;   /* already unmounted or doesn't exist */

    return unmount_mounted_volume(mv);
}

const char *get_mounted_device_from_path(const char *path)
{
    if (scan_mounted_volumes() < 0) {
        pr_err("failed to scan mounted volumes\n");
        return NULL;
    }

    const MountedVolume *volume = find_mounted_volume_by_mount_point(path);
    if (!volume) {
        pr_err("no mounted volume at %s\n", path);
        return NULL;
    }

    return volume->device;
}
