/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "fstab"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "recovery/volume.h"
#include "recovery/config.h"
#include "recovery/log.h"

static int num_volumes = 0;
static Volume *device_volumes = NULL;

int get_num_volumes(void)
{
    return num_volumes;
}

void load_volume_table(void)
{
    int alloc = 2;
    device_volumes = malloc(alloc * sizeof(Volume));
    if (!device_volumes) {
        pr_err("out of memory for volume table\n");
        return;
    }

    /* Entry 0: /tmp (ramdisk, always mounted) */
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].option = NULL;
    num_volumes = 1;

    FILE *fstab = fopen(RK_FSTAB_PATH, "r");
    if (fstab == NULL) {
        pr_err("failed to open %s (%d)\n", RK_FSTAB_PATH, errno);
        return;
    }

    char buffer[1024];
    char file_system[256];
    char mount_point[256];
    char fs_type[64];
    char option[256];
    char dump[32];
    char pass[32];
    char device[256];

    while (fgets(buffer, sizeof(buffer) - 1, fstab)) {
        int fields = sscanf(buffer, "%255s %255s %63s %255s %31s %31s",
                            file_system, mount_point, fs_type,
                            option, dump, pass);
        if (file_system[0] == '#')
            continue;

        /* Convert LABEL=xxx to /dev/block/by-name/xxx */
        char *label = strstr(file_system, "LABEL=");
        if (label)
            snprintf(device, sizeof(device), "%s/%s",
                     RK_BLOCK_BY_NAME, label + 6);
        else
            snprintf(device, sizeof(device), "%s", file_system);

        if (fields >= 4) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                device_volumes = realloc(device_volumes,
                                         alloc * sizeof(Volume));
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 = NULL;
            device_volumes[num_volumes].option = (fields >= 4) ?
                                                 strdup(option) : NULL;
            ++num_volumes;
        } else {
            pr_warn("skipping malformed fstab line: %s", buffer);
        }
    }

    fclose(fstab);

    pr_info("volume table (%d entries):\n", num_volumes);
    for (int i = 0; i < num_volumes; ++i) {
        Volume *v = &device_volumes[i];
        pr_info("  %d %s %s %s\n", i,
                v->device ? v->device : "(null)",
                v->mount_point, v->fs_type);
    }
}

Volume *volume_for_path(const char *path)
{
    for (int i = 0; i < num_volumes; ++i) {
        Volume *v = device_volumes + i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/'))
            return v;
    }
    return NULL;
}
