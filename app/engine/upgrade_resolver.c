/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "upgrade:resolver"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "recovery/storage.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "upgrade_internal.h"

/* ---- Target resolvers ----
 *
 * Two modes, dispatched once at the top of build_upgrade_list:
 *
 *   OTA          (SRC_LOCAL): system has been flashed before, udev built
 *                /dev/block/by-name/<PARTNAME> softlinks. Look those up
 *                and address each partition at offset 0.
 *
 *   Full-flash   (SRC_SDCARD / SRC_USB): board may be virgin (no part
 *                table, no by-name softlinks) or being completely
 *                re-imaged from SD/USB. Write each item to the backend's
 *                bare device at the physical offset baked into the image
 *                (item.flash_offset, in sectors).
 *
 * Disk-level items (parameter = GPT, bootloader = IDBlock) bypass the
 * per-mode resolvers: their placement is a property of the medium, not of
 * how the image arrived. resolve_disk_level routes them by the image's
 * storage code to a WHOLE-device node (bare_dev / bootloader_dev) written
 * at absolute offsets. On MTD the whole-chip node must survive an mtdparts
 * cmdline (u-boot injects mtdparts for normal and BCB-recovery boots
 * alike), which requires the product kernel to set
 * CONFIG_MTD_PARTITIONED_MASTER=y. They only enter the list when
 * --upgrade= names them explicitly, so a resolution failure aborts the
 * whole upgrade before anything is written — never a silent skip.
 *
 * Each resolver fills out->dev_path and out->flash_offset; everything
 * else (img_offset, img_size, name, backend, flash fn, etc.) is set by
 * the build_upgrade_list main loop.
 */

/* Pick the only registered backend on single-storage systems.
 * Returns NULL if 0 or >1 backends are registered. */
static struct storage_ops *only_backend(struct storage_mgr *mgr)
{
    if (mgr->backend_count != 1)
        return NULL;
    for (int t = 0; t < STORAGE_TYPE_MAX; t++) {
        if (mgr->backends[t])
            return mgr->backends[t];
    }
    return NULL;
}

/*
 * Backend routing by the image's per-item storage tag — the shared trust
 * root for everything that addresses a whole device (full-flash items, and
 * disk-level GPT/IDBlock items in both modes).
 *
 *   SSFW (code != 0): route by the vendor STORAGE_*_CODE bitmask. A
 *   partial-storage upgrade falls out naturally: an SSFW packaging only
 *   one sub-image routes every item to that one disk, the other disk is
 *   never touched.
 *
 *   RKFW (code == 0): no per-item tag — only allowed on a single-backend
 *   system. Dual-storage boards must supply SSFW so the package itself
 *   names the target storage; recovery never guesses which disk a
 *   tag-less item belongs to.
 *
 * Returns NULL when no backend can be picked (reason logged; the caller
 * decides whether that skips the item or aborts the upgrade).
 */
static struct storage_ops *route_by_code(struct upgrade_context *ctx,
                                         const struct rk_image_hdr *hdr,
                                         int item_index)
{
    const struct rk_image_item *img = &hdr->item[item_index];
    uint16_t code = hdr->storage_code[item_index];
    struct storage_ops *backend;

    if (code != 0) {
        backend = storage_backend_by_code(ctx->storage, code);
        if (!backend)
            pr_info("'%s' storage code 0x%x not routable on this board\n",
                    img->name, code);
        return backend;
    }

    backend = only_backend(ctx->storage);
    if (!backend) {
        pr_err("'%s' has no storage tag (RKFW) and this is a multi-backend "
               "board — supply an SSFW update.img whose sub-images carry "
               "storage codes\n", img->name);
        return NULL;
    }
    pr_dbg("'%s' RKFW + single backend %s — defaulting\n",
           img->name, backend->name);
    return backend;
}

int resolve_for_ota(struct upgrade_context *ctx,
                    const struct rk_image_hdr *hdr,
                    int item_index,
                    struct upgrade_item *out)
{
    const struct rk_image_item *img = &hdr->item[item_index];

    if (storage_get_part_path(ctx->storage, img->name,
                                    out->dev_path,
                                    sizeof(out->dev_path)) != 0) {
        pr_info("OTA: no by-name for %s — skip (partition not present)\n",
                img->name);
        return RESOLVE_SKIP;
    }
    out->flash_offset = 0;

    /* Backend by name (storage manager consults by-name link-target +
     * /proc/mtd). OTA mode ignores hdr->storage_code[] for ordinary
     * partitions — udev/by-name is the trust root here. */
    out->backend = storage_resolve(ctx->storage, img->name);
    if (!out->backend) {
        pr_info("OTA: no backend for %s — skip\n", img->name);
        return RESOLVE_SKIP;
    }
    return RESOLVE_OK;
}

int resolve_for_full_flash(struct upgrade_context *ctx,
                           const struct rk_image_hdr *hdr,
                           int item_index,
                           struct upgrade_item *out)
{
    const struct rk_image_item *img = &hdr->item[item_index];

    struct storage_ops *backend = route_by_code(ctx, hdr, item_index);
    if (!backend)
        return RESOLVE_SKIP;

    const char *bare = backend->bare_dev;
    if (!bare) {
        pr_info("full-flash: %s backend won't host %s (bare_dev=NULL) — skip\n",
                backend->name, img->name);
        return RESOLVE_SKIP;
    }

    out->backend = backend;
    snprintf(out->dev_path, sizeof(out->dev_path), "%s", bare);
    out->whole_device = true;
    /* image item flash_offset is in sectors (SECTOR_SIZE = 512B). */
    out->flash_offset = (long long)img->flash_offset * SECTOR_SIZE;

    /* Capacity fit (target offset+size must fit the device — also catches an
     * mtdparts-shrunk MTD node) is validated for every item in
     * precheck_items, before any write. */
    return RESOLVE_OK;
}

/*
 * Disk-level items: parameter = GPT, bootloader = IDBlock. Mode-independent
 * — the image's storage code picks the backend, and the target is always a
 * whole-device node written at absolute offsets (GPT: LBA0 + disk tail,
 * both computed by flash_parameter; IDBlock: the medium's fixed geom base).
 * These items only enter the list when --upgrade= names them explicitly,
 * so any resolution failure is FATAL: silently skipping a requested
 * loader/GPT update is worse than failing the upgrade.
 */
int resolve_disk_level(struct upgrade_context *ctx,
                       const struct rk_image_hdr *hdr,
                       int item_index,
                       struct upgrade_item *out)
{
    struct storage_ops *backend = route_by_code(ctx, hdr, item_index);
    if (!backend) {
        pr_err("%s: no backend to host it — abort upgrade\n", out->name);
        return RESOLVE_FATAL;
    }

    const char *dev = (out->type == ITEM_GPT) ? backend->bare_dev
                                              : backend->bootloader_dev;
    if (!dev || !dev[0]) {
        if (out->type == ITEM_GPT)
            pr_err("%s: backend %s has no whole-device node for the GPT "
                   "(MTD: boot without mtdparts, or enable "
                   "CONFIG_MTD_PARTITIONED_MASTER=y) — abort upgrade\n",
                   out->name, backend->name);
        else
            pr_err("%s: backend %s cannot host an IDBlock (not a boot "
                   "medium) — abort upgrade\n", out->name, backend->name);
        return RESOLVE_FATAL;
    }

    out->backend = backend;
    snprintf(out->dev_path, sizeof(out->dev_path), "%s", dev);
    out->whole_device = true;
    /* GPT and IDBlock compute their own absolute offsets within dev_path. */
    out->flash_offset = 0;
    return RESOLVE_OK;
}
