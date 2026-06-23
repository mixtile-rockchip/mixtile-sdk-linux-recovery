/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_VOLUME_H
#define RK_RECOVERY_VOLUME_H

#include <stddef.h>

/* ---- Volume descriptor (parsed from fstab) ---- */
typedef struct {
    const char *mount_point;    /* e.g. "/userdata" */
    const char *fs_type;        /* "ext4", "vfat", "ubifs", "yaffs2", "ramdisk" */
    const char *device;         /* block device or MTD partition name */
    const char *device2;        /* fallback device (may be NULL) */
    const char *option;         /* mount options */
} Volume;

/* ---- Mounted volume info (from /proc/mounts) ---- */
typedef struct {
    const char *device;
    const char *mount_point;
    const char *filesystem;
    const char *flags;
} MountedVolume;

/* ---- fstab (storage/fstab.c) ---- */
void    load_volume_table(void);
Volume *volume_for_path(const char *path);
int     get_num_volumes(void);

/* ---- mount/unmount (storage/mount.c) ---- */
int ensure_path_mounted(const char *path);
int ensure_path_unmounted(const char *path);
int ensure_ex_path_unmounted(const char *path);
const char *get_mounted_device_from_path(const char *path);

/* ---- format (storage/format.c) ---- */
int format_volume(const char *volume);
int resize_volume(const char *volume);

/* ---- /proc/mounts scanning (storage/mounts.c) ---- */
int scan_mounted_volumes(void);
const MountedVolume *find_mounted_volume_by_device(const char *device);
const MountedVolume *find_mounted_volume_by_mount_point(const char *mount_point);
int unmount_mounted_volume(const MountedVolume *volume);

/* Unmount every mount except `keep_path` and kernel/virtual mounts (/, /proc,
 * /sys, /dev, /run, /tmp, /var). Best-effort: 0 if all targeted unmounts
 * succeeded, -1 if any failed (e.g. busy fd). */
int umount_all_except(const char *keep_path);

/* Longest-prefix match of `file_path` against /proc/mounts; writes the owning
 * mount point into buf. Returns 0 on match, -1 (buf="/") if none. */
int mount_point_of(const char *file_path, char *buf, size_t len);

#endif /* RK_RECOVERY_VOLUME_H */
