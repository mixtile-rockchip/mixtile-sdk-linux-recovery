/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "mounts"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <unistd.h>

#include "recovery/volume.h"
#include "recovery/log.h"

#define PROC_MOUNTS_FILENAME "/proc/mounts"
#define MAX_MOUNTED_VOLUMES  32

typedef struct {
    MountedVolume *volumes;
    int volumes_allocd;
    int volume_count;
} MountsState;

static MountsState g_mounts_state = { NULL, 0, 0 };

static void free_volume_internals(const MountedVolume *volume, int zero)
{
    free((char *)volume->device);
    free((char *)volume->mount_point);
    free((char *)volume->filesystem);
    free((char *)volume->flags);
    if (zero)
        memset((void *)volume, 0, sizeof(*volume));
}

int scan_mounted_volumes(void)
{
    char buf[2048];
    int fd;
    ssize_t nbytes;

    if (g_mounts_state.volumes == NULL) {
        MountedVolume *volumes = malloc(MAX_MOUNTED_VOLUMES * sizeof(*volumes));
        if (volumes == NULL) {
            errno = ENOMEM;
            return -1;
        }
        g_mounts_state.volumes = volumes;
        g_mounts_state.volumes_allocd = MAX_MOUNTED_VOLUMES;
        memset(volumes, 0, MAX_MOUNTED_VOLUMES * sizeof(*volumes));
    } else {
        for (int i = 0; i < g_mounts_state.volume_count; i++)
            free_volume_internals(&g_mounts_state.volumes[i], 1);
    }
    g_mounts_state.volume_count = 0;

    fd = open(PROC_MOUNTS_FILENAME, O_RDONLY);
    if (fd < 0) {
        g_mounts_state.volume_count = 0;
        return -1;
    }
    nbytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nbytes < 0) {
        g_mounts_state.volume_count = 0;
        return -1;
    }
    buf[nbytes] = '\0';

    const char *bufp = buf;
    while (nbytes > 0) {
        char device[64];
        char mount_point[64];
        char filesystem[64];
        char flags[128];

        int matches = sscanf(bufp, "%63s %63s %63s %127s",
                             device, mount_point, filesystem, flags);
        if (matches == 4 &&
            g_mounts_state.volume_count < g_mounts_state.volumes_allocd) {
            MountedVolume *v =
                &g_mounts_state.volumes[g_mounts_state.volume_count++];
            v->device = strdup(device);
            v->mount_point = strdup(mount_point);
            v->filesystem = strdup(filesystem);
            v->flags = strdup(flags);
        }

        while (nbytes > 0 && *bufp != '\n') {
            bufp++;
            nbytes--;
        }
        if (nbytes > 0) {
            bufp++;
            nbytes--;
        }
    }

    return 0;
}

const MountedVolume *find_mounted_volume_by_device(const char *device)
{
    if (g_mounts_state.volumes != NULL) {
        for (int i = 0; i < g_mounts_state.volume_count; i++) {
            MountedVolume *v = &g_mounts_state.volumes[i];
            if (v->device != NULL && strcmp(v->device, device) == 0)
                return v;
        }
    }
    return NULL;
}

const MountedVolume *find_mounted_volume_by_mount_point(const char *mount_point)
{
    if (g_mounts_state.volumes != NULL) {
        for (int i = 0; i < g_mounts_state.volume_count; i++) {
            MountedVolume *v = &g_mounts_state.volumes[i];
            if (v->mount_point != NULL &&
                strcmp(v->mount_point, mount_point) == 0)
                return v;
        }
    }
    return NULL;
}

int unmount_mounted_volume(const MountedVolume *volume)
{
    int ret = umount(volume->mount_point);
    if (ret == 0) {
        free_volume_internals(volume, 1);
        return 0;
    }
    return ret;
}

/* Kernel/virtual/essential mounts that must never be unmounted before an
 * upgrade: exact "/" plus anything under these prefixes (/dev/pts, /sys/kernel,
 * /var/log, ...). Everything else (/oem, /userdata, /mnt/sdcard, ...) is a
 * real data/media mount and a candidate for unmount. */
static bool is_protected_mount(const char *mp)
{
    if (strcmp(mp, "/") == 0)
        return true;
    static const char *prefix[] = {
        "/proc", "/sys", "/dev", "/run", "/tmp", "/var", NULL
    };
    for (int i = 0; prefix[i]; i++) {
        size_t l = strlen(prefix[i]);
        if (strncmp(mp, prefix[i], l) == 0 && (mp[l] == '\0' || mp[l] == '/'))
            return true;
    }
    return false;
}

int umount_all_except(const char *keep_path)
{
    if (scan_mounted_volumes() != 0) {
        pr_err("scan_mounted_volumes failed: %s\n", strerror(errno));
        return -1;
    }

    int rc = 0;
    /* Reverse order so child mounts are unmounted before their parents. */
    for (int i = g_mounts_state.volume_count - 1; i >= 0; i--) {
        const char *mp = g_mounts_state.volumes[i].mount_point;
        if (!mp || is_protected_mount(mp))
            continue;
        if (keep_path && strcmp(mp, keep_path) == 0) {
            pr_info("keep mounted: %s (update source)\n", mp);
            continue;
        }
        if (umount(mp) == 0) {
            pr_info("umounted %s\n", mp);
        } else {
            pr_warn("umount %s failed: %s\n", mp, strerror(errno));
            rc = -1;
        }
    }
    return rc;
}

int mount_point_of(const char *file_path, char *buf, size_t len)
{
    if (!file_path || !buf || len == 0)
        return -1;
    if (scan_mounted_volumes() != 0) {
        snprintf(buf, len, "/");
        return -1;
    }

    size_t best = 0;
    const char *best_mp = NULL;
    for (int i = 0; i < g_mounts_state.volume_count; i++) {
        const char *mp = g_mounts_state.volumes[i].mount_point;
        if (!mp)
            continue;
        size_t l = strlen(mp);
        bool match = (strcmp(mp, "/") == 0) ||
                     (strncmp(file_path, mp, l) == 0 &&
                      (file_path[l] == '\0' || file_path[l] == '/'));
        if (match && l > best) {
            best = l;
            best_mp = mp;
        }
    }

    snprintf(buf, len, "%s", best_mp ? best_mp : "/");
    return best_mp ? 0 : -1;
}
