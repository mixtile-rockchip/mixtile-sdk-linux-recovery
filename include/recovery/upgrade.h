/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UPGRADE_H
#define RK_RECOVERY_UPGRADE_H

#include <stdbool.h>
#include <limits.h>
#include "recovery/config.h"
#include "recovery/storage.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- Upgrade item types ---- */
enum upgrade_item_type {
    ITEM_PARTITION,     /* Normal partition: /dev/block/by-name/xxx or /dev/mtdX */
    ITEM_IDBLOCK,       /* IDBlock: raw device offset write */
    ITEM_GPT,           /* GPT partition table (primary + backup) */
};

struct upgrade_item;

/* Flash function signature */
typedef int (*flash_func)(struct upgrade_item *item, const char *image_path);

/* ---- Upgrade item (runtime, per-partition) ---- */
struct upgrade_item {
    char                    name[32];
    enum upgrade_item_type  type;
    struct storage_ops     *backend;

    /* Image source */
    long long   img_offset;
    long long   img_size;

    /* Target info */
    char        dev_path[PATH_MAX];
    /*
     * Physical byte offset at which to write `img_size` bytes into dev_path.
     *
     * OTA mode (dev_path = /dev/block/by-name/<partition>): always 0 — the
     * partition softlink already points at the start of the partition.
     *
     * Full-flash mode (dev_path = bare device /dev/mtd0 / /dev/mmcblk0 /
     * /dev/sda): image item.flash_offset × SECTOR_SIZE — mk-updateimg
     * embedded the physical layout from parameter.txt so we don't need
     * by-name softlinks (which haven't been created yet on a virgin board).
     */
    long long   flash_offset;
    /* dev_path is a whole-device node (bare_dev / bootloader_dev) written at
     * absolute offsets — set by the full-flash and disk-level resolvers, false
     * for OTA by-name partitions. The mount precheck reads this instead of
     * re-deriving it by string-comparing dev_path against bare_dev. */
    bool        whole_device;
    struct storage_mgr *mgr;    /* Back-pointer to storage manager */

    /* State */
    bool    need_update;

    /* Polymorphic flash method — bound at creation time by name */
    flash_func  flash;
};

#endif /* RK_RECOVERY_UPGRADE_H */
