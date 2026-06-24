/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

/*
 * updateEngine upgrade orchestration. The pipeline is split across:
 *   upgrade_resolver.c  per-item target resolution (by-name / raw / disk)
 *   upgrade_plan.c      build_upgrade_list (what/which/where) + A/B fallback
 *   upgrade_precheck.c  precheck_items (mount + capacity gate)
 *   upgrade_version.c   version query/compare
 * This file wires them into upgrade_run and owns execute (the write loop).
 */

#define __MODULE__ "cmd:upgrade"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "recovery/bootmsg.h"    /* get_current_slot, set_slot_activity */
#include "recovery/checksum.h"   /* md5_compare_partition_file */
#include "recovery/download.h"   /* download_file */
#include "recovery/storage.h"    /* storage_init */
#include "recovery/log.h"
#include "upgrade_internal.h"

/* ---- Execute upgrade ---- */

static void upgrade_print(struct upgrade_context *ctx, const char *fmt, ...)
{
    if (!ctx || !ctx->print_cb)
        return;

    char prompt[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(prompt, sizeof(prompt), fmt, ap);
    va_end(ap);

    ctx->print_cb(prompt, ctx->cb_ctx);
}

static int execute_upgrade(struct upgrade_context *ctx,
                           struct upgrade_item *items, int count)
{
    for (int i = 0; i < count; i++) {
        if (!items[i].need_update || !items[i].flash)
            continue;

        pr_info("=== upgrading [%d/%d] %s ===\n", i + 1, count, items[i].name);

        /* Mount and capacity were validated for all items in precheck_items
         * before any write started. */

        upgrade_print(ctx, "[%s] upgrade start...\n", items[i].name);

        int ret = items[i].flash(&items[i], ctx->image_path);
        if (ret != 0) {
            pr_err("flash %s failed (ret=%d)\n", items[i].name, ret);
            upgrade_print(ctx, "[%s] upgrade fail\n", items[i].name);
            if (ctx->status_cb)
                ctx->status_cb(ctx->cb_ctx, RK_UPGRADE_ERR);
            return -1;
        }

        upgrade_print(ctx, "[%s] upgrade success!\n", items[i].name);

        /*
         * SD/USB: skip the post-flash readback verify. This is purely a time
         * tradeoff — readback doubles I/O (re-reading every partition back
         * over slow MTD/eMMC) and SD/USB flashing is an attended, local op.
         *
         * NOT because "the SD card is the authoritative source": readback
         * verify catches *write* errors (bad flash, write-path bugs), which
         * source integrity (already MD5-checked at image parse) does not
         * cover. The verify below reads at items[i].flash_offset, so it is
         * already correct for full-flash too — enabling it for SD/USB is just
         * dropping this skip and paying the extra read time.
         */
        bool is_sdusb_v = (ctx->source_type == SRC_SDCARD ||
                           ctx->source_type == SRC_USB);
        if (is_sdusb_v) {
            pr_dbg("skip verify in sdboot/usbboot mode (policy)\n");
            continue;
        }

        /* bootloader (RC4-encrypted IDBlock) and parameter (GPT written to
         * primary+backup, not a contiguous image blob) can't be verified by
         * a direct MD5 of the image bytes — skip by name. (Out-of-scope:
         * structural verify — RKboot magic / GPT CRC32.) */
        if (strcmp(items[i].name, "bootloader") == 0 ||
            strcmp(items[i].name, "parameter")  == 0) {
            pr_dbg("skip verify for %s (raw-write, no direct MD5 match)\n",
                   items[i].name);
            continue;
        }

        /* Post-flash verification via MD5 comparison */
        if (!md5_compare_partition_file(items[i].backend, items[i].dev_path,
                                        items[i].flash_offset, ctx->image_path,
                                        items[i].img_offset,
                                        items[i].img_size)) {
            pr_err("verify %s FAILED\n", items[i].name);
            upgrade_print(ctx, "[%s] verify fail\n", items[i].name);
            if (ctx->status_cb)
                ctx->status_cb(ctx->cb_ctx, RK_UPGRADE_ERR);
            return -1;
        }
        pr_info("verify %s OK\n", items[i].name);
    }

    return 0;
}

/* ---- Public API ---- */

void upgrade_ctx_init(struct upgrade_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->current_slot = -1;
    ctx->pipe_fd = -1;
    ctx->source_type = SRC_LOCAL;
    ctx->status = RK_UPGRADE_ERR;
    /* Default callback context is ctx itself, so callbacks installed via
     * upgrade_ctx_set_callbacks() can reach ctx->cmd_pipe / ctx->status
     * without the caller wiring cb_ctx up separately. */
    ctx->cb_ctx = ctx;
}

int upgrade_run(struct upgrade_context *ctx)
{
    pr_info("=== upgrade_run start ===\n");

    /* 1. Initialize storage */
    if (!ctx->storage) {
        ctx->storage = storage_init();
        if (!ctx->storage) {
            pr_err("storage_init failed\n");
            upgrade_print(ctx, "storage init failed\n");
            return -1;
        }
    }

    /* 2. Stage image to save_path.
     *
     * download_file() unifies three cases:
     *   - save_path empty or equal to image_url -> use in place, no-op.
     *   - image_url is a local path different from save_path -> file copy.
     *   - image_url is http(s) -> curl download (needs RK_SOURCE_URL).
     *
     * The point of staging: two-step upgrades write BCB pointing at
     * save_path, and the new recovery must find the image there after the
     * reboot — even if the USB stick that supplied image_url is gone.
     * Subsequent MD5/flash/verify reads also hit save_path, which is
     * usually on faster storage than the source medium. */
    if (!ctx->image_url[0]) {
        pr_err("no image URL specified\n");
        upgrade_print(ctx, "no image URL specified\n");
        return -1;
    }

    const char *target = ctx->save_path[0] ? ctx->save_path : ctx->image_url;
    if (download_file(ctx->image_url, target) != 0) {
        pr_err("stage image (%s -> %s) failed\n", ctx->image_url, target);
        upgrade_print(ctx, "stage image failed\n");
        return -1;
    }
    /* From here on the pipeline reads the staged local file, not image_url. */
    snprintf(ctx->image_path, sizeof(ctx->image_path), "%s", target);

    if (access(ctx->image_path, F_OK) != 0) {
        pr_err("image not found: %s\n", ctx->image_path);
        upgrade_print(ctx, "image not found: %s\n", ctx->image_path);
        return -1;
    }

    /* 3. Parse firmware image */
    struct rk_image_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (image_parse(ctx->image_path, &hdr) != 0) {
        pr_err("image_parse failed\n");
        upgrade_print(ctx, "image parse failed\n");
        return -1;
    }

    /* 4. Get current A/B slot */
    ctx->current_slot = get_current_slot();
    pr_info("current slot: %d\n", ctx->current_slot);

    /* 5. Build upgrade list */
    struct upgrade_item items[MAX_PARTS];
    memset(items, 0, sizeof(items));
    int count = build_upgrade_list(ctx, &hdr, items, MAX_PARTS);
    if (count < 0) {
        upgrade_print(ctx, "build upgrade plan failed\n");
        return -1;      /* disk-level resolution failed — nothing written */
    }
    if (count == 0) {
        pr_err("no partitions to upgrade\n");
        upgrade_print(ctx, "no partitions to upgrade\n");
        return -1;
    }

    /* 6. A/B fallback */
    bool is_sdusb = (ctx->source_type == SRC_SDCARD ||
                     ctx->source_type == SRC_USB);
    if (!is_sdusb)
        ab_fallback(items, count, &hdr);

    /* 6.5 Pre-flight: validate every target before writing a single byte. */
    if (precheck_items(ctx, items, count) != 0) {
        pr_err("pre-upgrade check failed — nothing written\n");
        upgrade_print(ctx, "pre-upgrade check failed, nothing written\n");
        if (ctx->status_cb)
            ctx->status_cb(ctx->cb_ctx, RK_UPGRADE_ERR);
        return -1;
    }

    /* 7. Notify start */
    if (ctx->status_cb)
        ctx->status_cb(ctx->cb_ctx, RK_UPGRADE_START);

    /* 8. Execute flashing */
    int ret = execute_upgrade(ctx, items, count);
    if (ret != 0)
        return -1;

    /* 9. Switch A/B slot */
    if (ctx->current_slot >= 0) {
        pr_info("switching A/B slot\n");
        set_slot_activity(ctx->storage, ctx->current_slot);
    }

    /* 10. Notify success */
    if (ctx->status_cb)
        ctx->status_cb(ctx->cb_ctx, RK_UPGRADE_FINISHED);
    upgrade_print(ctx, "Update SUCCESS.\n");

    pr_info("=== upgrade_run complete ===\n");
    return 0;
}
