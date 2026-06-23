/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

/*
 * updateEngine subcommand handlers. engine_main.c parses argv into a
 * struct engine_args and dispatches to one of these; main owns the CLI
 * lifecycle (logging, cmd_pipe, storage, reboot) and these own the
 * per-command logic. All return 0 on success, -1 on failure.
 */

#define __MODULE__ "engine:cmd"

#include <stdio.h>
#include <string.h>

#include "recovery/upgrade_engine.h"
#include "recovery/upgrade_filter.h"
#include "recovery/bootmsg.h"
#include "recovery/storage.h"
#include "recovery/log.h"
#include "cmd.h"

/*
 * Both callbacks receive the upgrade_context as user_data (set up by
 * upgrade_ctx_init, which defaults cb_ctx = ctx). State that used to be
 * module-global (status, cmd_pipe) is now owned by the context.
 */
static void status_callback(void *user_data, enum rk_upgrade_status status)
{
    struct upgrade_context *ctx = user_data;
    if (ctx)
        ctx->status = status;
    if (status == RK_UPGRADE_FINISHED)
        pr_info("upgrade finished\n");
}

static void print_callback(char *prompt, void *user_data)
{
    struct upgrade_context *ctx = user_data;
    if (ctx && ctx->cmd_pipe)
        fprintf(ctx->cmd_pipe, "ui_print %s\n", prompt);
}

static bool upgrade_token_is_positive_recovery(const char *token)
{
    if (token[0] == '!' || token[0] == '-')
        return false;
    return strcmp(token, "recovery") == 0;
}

static int append_upgrade_token(char *list, size_t list_len, const char *token)
{
    size_t used = strlen(list);
    size_t need = strlen(token) + (used ? 1 : 0);

    if (used + need >= list_len) {
        pr_err("deferred --upgrade list too long while appending %s\n", token);
        return -1;
    }

    if (used)
        strcat(list, ",");
    strcat(list, token);
    return 0;
}

/*
 * Shared upgrade_context setup from CLI args — both the direct update
 * (cmd_update) and the misc=update orchestration build the same context
 * (storage / source / callbacks / staged paths / --upgrade= whitelist).
 */
static void ctx_from_args(struct upgrade_context *ctx,
                          const struct engine_args *args,
                          struct storage_mgr *mgr, FILE *cmd_pipe)
{
    upgrade_ctx_init(ctx);
    ctx->storage = mgr;
    ctx->source_type = args->src_type;
    ctx->status_cb = status_callback;
    ctx->print_cb = print_callback;
    ctx->pipe_fd = args->pipefd;
    ctx->cmd_pipe = cmd_pipe;

    if (args->image_url)
        snprintf(ctx->image_url, sizeof(ctx->image_url), "%s", args->image_url);
    if (args->save_path)
        snprintf(ctx->save_path, sizeof(ctx->save_path), "%s", args->save_path);

    /* WHAT to upgrade: caller's explicit --upgrade= whitelist (required). */
    upgrade_filter_parse(&ctx->upgrade, args->upgrade_spec);
}

/* --- --update: direct firmware upgrade --- */
int cmd_update(struct storage_mgr *mgr, const struct engine_args *args,
               FILE *cmd_pipe)
{
    struct upgrade_context ctx;
    ctx_from_args(&ctx, args, mgr, cmd_pipe);

    if (!ctx.upgrade.all && ctx.upgrade.count == 0) {
        pr_err("--upgrade=<all|name,name,...> is required for update\n");
        return -1;
    }

    if (args->src_type == SRC_SDCARD)
        pr_info("upgrade from SD card\n");
    else if (args->src_type == SRC_USB)
        pr_info("upgrade from USB\n");
    else
        pr_info("upgrade from local/OTA\n");

    /* Version check */
    if (args->version_url) {
        if (!upgrade_check_version(args->version_url)) {
            pr_err("version check failed, abort\n");
            return -1;
        }
    }

    if (upgrade_run(&ctx) == 0)
        ctx.status = RK_UPGRADE_FINISHED;
    return (ctx.status == RK_UPGRADE_FINISHED) ? 0 : -1;
}

/* --- --misc=update: BCB-driven OTA orchestration --- */
static int handle_misc_update(struct storage_mgr *mgr,
                              const struct engine_args *args, FILE *cmd_pipe)
{
    struct upgrade_context ctx;
    ctx_from_args(&ctx, args, mgr, cmd_pipe);

    if (!ctx.upgrade.all && ctx.upgrade.count == 0) {
        pr_err("--upgrade=<all|name,name,...> is required for --misc=update\n");
        return -1;
    }

    int slot = get_current_slot();

    /*
     * Two upgrade environments, distinguished by slot:
     *
     * Non-AB normal system (slot == -1): the RUNNING system must
     * NOT flash uboot (live bootloader area) or rootfs (mounted
     * /). Only the recovery partition is safe to flash here;
     * everything else is deferred to recovery via BCB --upgrade=:
     *   - recovery in set  → flash recovery now (bootstrap the new
     *                        executor), defer the rest
     *   - recovery not set → flash nothing here, defer whole set
     * The freshly-booted recovery then flashes the deferred set.
     * Ported from MiscUpdate(); the old 0x3F0000/bit-18 mask is an
     * explicit name set now. NEVER direct-flash non-recovery here.
     *
     * A/B (slot 0/1): the update targets the *inactive* slot,
     * which isn't in use, so flash directly — no recovery handoff.
     */
    if (slot == -1 && args->src_type == SRC_LOCAL && args->image_url) {
        bool want_recovery = upgrade_allows(&ctx.upgrade, "recovery");

        /* What recovery flashes after reboot = requested set minus recovery.
         * This mirrors the legacy MiscUpdate bitmask path, which cleared the
         * recovery bit before writing BCB needupdate. In particular, "all"
         * must become "all,!recovery"; otherwise the new recovery will flash
         * its own partition again. */
        char remaining[512] = {0};
        if (ctx.upgrade.all &&
            append_upgrade_token(remaining, sizeof(remaining), "all") != 0)
            return -1;
        /* Append explicit names (e.g. parameter/bootloader) minus
         * recovery, preserving "all,parameter,..." so high-risk
         * partitions named alongside "all" survive to recovery. */
        for (int i = 0; i < ctx.upgrade.count; i++) {
            if (upgrade_token_is_positive_recovery(ctx.upgrade.names[i]))
                continue;
            if (append_upgrade_token(remaining, sizeof(remaining),
                                     ctx.upgrade.names[i]) != 0)
                return -1;
        }
        if (want_recovery && ctx.upgrade.all &&
            append_upgrade_token(remaining, sizeof(remaining),
                                 "!recovery") != 0)
            return -1;

        int rc = 0;
        if (want_recovery) {
            pr_info("normal system: flash recovery partition only\n");
            ctx.upgrade.all = false;
            ctx.upgrade.count = 1;
            snprintf(ctx.upgrade.names[0],
                     sizeof(ctx.upgrade.names[0]), "recovery");
            if (upgrade_run(&ctx) != 0 ||
                ctx.status != RK_UPGRADE_FINISHED) {
                pr_err("recovery partition upgrade failed\n");
                rc = -1;
            }
        } else {
            pr_info("normal system: no recovery in set, deferring all to recovery\n");
        }

        if (rc != 0)
            return -1;
        if (remaining[0]) {
            /* Hand the rest to the freshly-booted recovery. If the BCB write
             * fails, the deferred partitions would never be scheduled — report
             * failure rather than a false success. */
            const char *bcb_path = args->save_path ? args->save_path
                                                   : args->image_url;
            if (cmd_misc_write_recovery_bcb(mgr, bcb_path, remaining) != 0) {
                pr_err("BCB write failed — remaining partitions (%s) will NOT "
                       "be scheduled; abort\n", remaining);
                return -1;
            }
            pr_info("BCB written, recovery will flash: %s (reboot to continue)\n",
                    remaining);
        }
        /* Only recovery was requested → already flashed here; either way OK. */
        return 0;
    }

    /* A/B inactive-slot upgrade: flash directly. */
    if (upgrade_run(&ctx) == 0)
        ctx.status = RK_UPGRADE_FINISHED;
    return (ctx.status == RK_UPGRADE_FINISHED) ? 0 : -1;
}

/* --- --misc=<func>: slot / wipe / update / display dispatch --- */
int cmd_misc(struct storage_mgr *mgr, const struct engine_args *args,
             FILE *cmd_pipe)
{
    const char *misc_func = args->misc_func;

    if (strcmp(misc_func, "now") == 0)
        return cmd_slot_now(mgr) == 0 ? 0 : -1;
    if (strcmp(misc_func, "other") == 0)
        return cmd_slot_other(mgr) == 0 ? 0 : -1;
    if (strcmp(misc_func, "wipe_userdata") == 0) {
        pr_info("--misc=wipe_userdata\n");
        return wipe_userdata(mgr, 0) == 0 ? 0 : -1;
    }
    if (strcmp(misc_func, "update") == 0)
        return handle_misc_update(mgr, args, cmd_pipe);
    if (strcmp(misc_func, "display") == 0) {
        cmd_slot_display(mgr);
        /* display is a query: it reports success via stdout but, as before,
         * is not treated as a "finished upgrade" (so --reboot won't fire). */
        return -1;
    }

    pr_err("unknown misc command: %s\n", misc_func);
    return -1;
}

/* --- --misc_custom=<func>: custom MISC cmdline read/write/clean --- */
int cmd_misc_custom(struct storage_mgr *mgr, const char *custom_define)
{
    pr_info("--misc_custom=%s\n", custom_define);

    if (strcmp(custom_define, "read") == 0)
        return misc_custom_read(mgr) == 0 ? 0 : -1;
    if (strcmp(custom_define, "write") == 0)
        return misc_custom_write(mgr) == 0 ? 0 : -1;
    if (strcmp(custom_define, "clean") == 0)
        return misc_custom_clean(mgr) == 0 ? 0 : -1;

    pr_err("unknown misc_custom command: %s\n", custom_define);
    return -1;
}
