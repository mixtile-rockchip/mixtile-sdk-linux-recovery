/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UPDATE_SOURCE_H
#define RK_RECOVERY_UPDATE_SOURCE_H

#include <stddef.h>
#include <stdbool.h>

enum update_src_type {
    SRC_SDCARD,     /* cmdline: "sdfwupdate", mount: /mnt/sdcard */
    SRC_USB,        /* cmdline: "usbfwupdate", mount: /mnt/udisk */
    SRC_LOCAL,      /* local path: /cache/update.img, /userdata/update.img */
};

#define MAX_IMAGE_NAMES 5

struct update_source {
    enum update_src_type type;
    const char       *cmdline_key;                  /* "sdfwupdate" / "usbfwupdate" */
    const char       *mount_paths[4];               /* Priority-ordered try list */
    const char       *image_names[MAX_IMAGE_NAMES]; /* Priority-ordered candidate names */
};

/*
 * Detect the active update source.
 *
 * Returns 0 on success and fills *out (a single source — SD/USB/LOCAL).
 * Returns -1 if no source is active. Single-source contract: multi-source
 * arbitration is not needed (no caller uses it; if SD+USB ever both need
 * priority handling we extend this).
 */
int source_detect_active(struct update_source *out);

/*
 * Get full image path for this source: try every (mount_path, image_name)
 * combination from the priority arrays, return the first that exists.
 * Also honors a vendor override `image=xxx` line in sd_boot_config.config
 * (only for SD/USB sources).
 */
int source_get_image_path(const struct update_source *src,
                          char *path, size_t len);

/* Parse config item from source's config file (unified get_cfg_Item) */
int source_get_cfg_item(const struct update_source *src, const char *key,
                        char *value, size_t len);

/* Type-based boot/update probes (replaces source_sd_* / source_usb_* pairs). */
bool source_is_boot(enum update_src_type type);
bool source_has_update(enum update_src_type type);

/*
 * Ensure the source media is mounted at one of src->mount_paths.
 *   - SRC_LOCAL: no-op (fstab handles internal partitions).
 *   - SRC_SDCARD / SRC_USB: probe /sys/block for the boot media (SD card =
 *     mmcblkN with device/type "SD"; U-disk = sdX whose sysfs path contains
 *     "/usb"), and if not already mounted, mount its first partition at
 *     mount_paths[0] using a sweep of common removable-media fs types.
 *
 * Vendor fstab on rk3588 hard-codes /dev/sda1 → /mnt/udisk, which fails
 * when SATA takes sda (the U-disk enumerates as sdb); this function lets
 * recovery work without relying on fstab.
 *
 * Returns 0 if the source is mounted afterwards (or was already), -1 if
 * no media was found or mount failed for all fs-type candidates.
 */
int source_ensure_mounted(enum update_src_type type);

#endif /* RK_RECOVERY_UPDATE_SOURCE_H */
