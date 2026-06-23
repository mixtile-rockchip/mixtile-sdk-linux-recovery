/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "upgrade:plan"

#include <stdio.h>
#include <string.h>

#include "recovery/flash.h"     /* flash_normal / flash_bootloader / flash_parameter, is_diff_img */
#include "recovery/bootmsg.h"   /* get_ab_suffix, slot_filter */
#include "recovery/log.h"
#include "upgrade_internal.h"

/*
 * upgrade scope = update.img items ∩ --upgrade= whitelist (upgrade_allows,
 * recovery/upgrade_filter.h) ∩ A/B slot (slot_filter, recovery/bootmsg.h).
 * No partition policy table — partition.c is gone.
 */

/* ---- Build upgrade item list ---- */

int build_upgrade_list(struct upgrade_context *ctx,
                       const struct rk_image_hdr *hdr,
                       struct upgrade_item *items, int max_items)
{
    int count = 0;
    bool is_full_flash = (ctx->source_type == SRC_SDCARD ||
                          ctx->source_type == SRC_USB);

    /*
     * GPT in the set forces full-flash resolution, even on OTA: rewriting the
     * partition table invalidates the old by-name softlinks (they point at
     * the stale layout until reboot), so address every item by its image-
     * embedded absolute offset on the whole-disk node — the same coordinate
     * system as the new GPT. parameter is explicit-only, so this only fires
     * when --upgrade= names it (SD/USB already passes it). The whole-disk
     * node is already a precondition for writing the GPT at all, so this adds
     * no new requirement.
     */
    bool gpt_in_set = image_find_item(hdr, "parameter") &&
                      upgrade_allows(&ctx->upgrade, "parameter");
    if (gpt_in_set)
        is_full_flash = true;

    /*
     * Pick the target resolver once. The two modes have different trust
     * roots (OTA trusts udev/by-name; full-flash trusts update.img's
     * embedded physical layout); the rest of the loop is uniform.
     */
    resolver_fn resolve = is_full_flash ? resolve_for_full_flash
                                        : resolve_for_ota;
    pr_info("upgrade mode: %s%s\n", is_full_flash ? "full-flash" : "OTA",
            (gpt_in_set && ctx->source_type == SRC_LOCAL)
                ? " (GPT in set → whole-disk)" : "");

    /*
     * Data-driven: iterate over what the update.img actually contains.
     * recovery does NOT maintain a partition registry — the image decides
     * the upgrade scope, the storage layer decides routing, and the policy
     * table only kicks in for partitions that need a non-default flash
     * function or behavior flag (bootloader / parameter / misc / userdata).
     */
    for (int i = 0; i < hdr->item_count && count < max_items; i++) {
        const struct rk_image_item *img_item = &hdr->item[i];
        const char *name = img_item->name;

        /* Skip "package-file" pseudo-item (metadata, not a partition) */
        if (strcmp(name, "package-file") == 0)
            continue;

        /* (1) WHAT — caller's --upgrade= whitelist (base-name match, so
         * "boot" covers boot_a/boot_b). is_full_flash always parses to
         * --upgrade=all upstream, so this naturally passes everything. */
        if (!upgrade_allows(&ctx->upgrade, name))
            continue;

        /* (2) WHICH — A/B slot: keep unsuffixed + opposite-slot copy */
        char ab = get_ab_suffix(name);
        if (slot_filter(ab, ctx->current_slot))
            continue;

        /* Build upgrade_item */
        struct upgrade_item *item = &items[count];
        memset(item, 0, sizeof(*item));

        snprintf(item->name, sizeof(item->name), "%s", name);
        item->img_offset  = image_get_offset(img_item);
        item->img_size    = image_get_size(img_item);
        item->mgr         = ctx->storage;
        item->need_update = true;

        /* Flash-method dispatch by name — HOW to write, orthogonal to
         * whether. bootloader = IDBlock (RC4+header), parameter = GPT
         * (primary+backup), everything else = normal raw/diff. When the
         * special path is trimmed out (RK_IDBLOCK_UPDATE / RK_GPT_UPDATE = 0),
         * the partition MUST NOT fall back to flash_normal — that would write
         * the IDBlock image / GPT text into a by-name node as a raw partition.
         * Skip it instead (SD/USB boot always names parameter/bootloader, so a
         * trimmed build simply doesn't refresh them — consistent with trimming
         * the capability; FATAL would needlessly kill SD/USB upgrades). */
        if (strcmp(name, "bootloader") == 0) {
#if defined(RK_IDBLOCK_UPDATE)
            item->flash = flash_bootloader;
            item->type  = ITEM_IDBLOCK;
#else
            pr_warn("bootloader named but RK_IDBLOCK_UPDATE trimmed out — "
                    "skip (not flashing IDBlock as a raw partition)\n");
            continue;
#endif
        } else if (strcmp(name, "parameter") == 0) {
#if defined(RK_GPT_UPDATE)
            item->flash = flash_parameter;
            item->type  = ITEM_GPT;
#else
            pr_warn("parameter named but RK_GPT_UPDATE trimmed out — "
                    "skip (not flashing GPT text as a raw partition)\n");
            continue;
#endif
        } else {
            item->flash = flash_normal;
            item->type  = ITEM_PARTITION;
        }

        /* (3) WHERE — resolve {dev_path, flash_offset, backend}. Disk-level
         * items (GPT/IDBlock) resolve mode-independently; normal partitions
         * use the per-mode resolver picked above. */
        int r = (item->type == ITEM_PARTITION)
                    ? resolve(ctx, hdr, i, item)
                    : resolve_disk_level(ctx, hdr, i, item);
        if (r == RESOLVE_SKIP)
            continue;
        if (r != RESOLVE_OK)
            return -1;      /* FATAL — abort before anything is written */

#if defined(RK_DIFF_UPDATE)
        /* A diff patches a partition's existing content in place — meaningless
         * once we address the whole disk by absolute offset (GPT rewrite, or a
         * virgin SD/USB with no prior content). Reject up front rather than
         * read a bogus patch base off the whole-disk node. */
        if (is_full_flash && item->type == ITEM_PARTITION &&
            is_diff_img(ctx->image_path, item->img_offset, item->img_size) == 1) {
            pr_err("'%s' is a diff image, but this is a whole-disk/GPT upgrade "
                   "— ship a full image for it; abort\n", item->name);
            return -1;
        }
#endif

        pr_info("plan: [%d] %s img_off=0x%llx size=0x%llx -> %s @flash_off=0x%llx\n",
                count, item->name, item->img_offset, item->img_size,
                item->dev_path, item->flash_offset);
        count++;
    }

    return count;
}

/* ---- A/B fallback: try opposite slot if image not found ---- */

void ab_fallback(struct upgrade_item *items, int count,
                 const struct rk_image_hdr *hdr)
{
    for (int i = 0; i < count; i++) {
        if (items[i].need_update && items[i].img_size == 0) {
            /* Try opposite slot name */
            char name[32];
            snprintf(name, sizeof(name), "%s", items[i].name);
            int len = (int)strlen(name);

            if (len >= 2 && name[len - 2] == '_') {
                char slot = name[len - 1];
                name[len - 1] = (slot == 'a') ? 'b' : 'a';

                const struct rk_image_item *alt = image_find_item(hdr, name);
                if (alt) {
                    items[i].img_offset = image_get_offset(alt);
                    items[i].img_size = image_get_size(alt);
                    pr_info("A/B fallback: %s -> %s (offset=0x%llx size=0x%llx)\n",
                            items[i].name, name,
                            items[i].img_offset, items[i].img_size);
                }
            }
        }
    }
}
