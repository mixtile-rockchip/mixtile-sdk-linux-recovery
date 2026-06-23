/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * Internal interface shared across the updateEngine upgrade pipeline split:
 *   upgrade_resolver.c  per-item target resolution
 *   upgrade_plan.c      build the item list (what / which / where) + A/B
 *   upgrade_precheck.c  pre-write validation gate (mount + capacity)
 *   cmd_upgrade.c       upgrade_run orchestration + execute
 *
 * Not a public API — it lives in app/engine, not include/recovery, and is
 * only included by these engine sources.
 */
#ifndef RK_RECOVERY_UPGRADE_INTERNAL_H
#define RK_RECOVERY_UPGRADE_INTERNAL_H

#include "recovery/upgrade_engine.h"  /* struct upgrade_context */
#include "recovery/image.h"    /* struct rk_image_hdr */
#include "recovery/upgrade.h"  /* struct upgrade_item, MAX_PARTS, SECTOR_SIZE */

/* Resolver verdicts: SKIP leaves one item out and keeps going (partition
 * not present on this board); FATAL means continuing would silently drop
 * or misplace an explicitly requested disk-level write — abort the whole
 * upgrade before it starts. */
#define RESOLVE_OK      0
#define RESOLVE_SKIP   (-1)
#define RESOLVE_FATAL  (-2)

/* Resolver signature: takes the whole hdr + item index so full-flash can
 * also read the parallel storage_code[] array. Each resolver fills
 * out->dev_path, out->flash_offset, and out->backend. */
typedef int (*resolver_fn)(struct upgrade_context *ctx,
                           const struct rk_image_hdr *hdr,
                           int item_index,
                           struct upgrade_item *out);

/* ---- upgrade_resolver.c: per-item target resolution ---- */
int resolve_for_ota(struct upgrade_context *ctx,
                    const struct rk_image_hdr *hdr,
                    int item_index, struct upgrade_item *out);
int resolve_for_full_flash(struct upgrade_context *ctx,
                           const struct rk_image_hdr *hdr,
                           int item_index, struct upgrade_item *out);
int resolve_disk_level(struct upgrade_context *ctx,
                       const struct rk_image_hdr *hdr,
                       int item_index, struct upgrade_item *out);

/* ---- upgrade_plan.c: build the item list + A/B fallback ---- */
int  build_upgrade_list(struct upgrade_context *ctx,
                        const struct rk_image_hdr *hdr,
                        struct upgrade_item *items, int max_items);
void ab_fallback(struct upgrade_item *items, int count,
                 const struct rk_image_hdr *hdr);

/* ---- upgrade_precheck.c: pre-write validation gate ---- */
int precheck_items(struct upgrade_context *ctx,
                   struct upgrade_item *items, int count);

#endif /* RK_RECOVERY_UPGRADE_INTERNAL_H */
